#include "check_hash_table.h"

#include <boost/container/flat_set.hpp>
#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace csv;

namespace kernel {


class HashArrayMappedTrie {

    using ValueType = size_t;

    struct DataNode {
        const char *    Start;
        size_t          Length;
        ValueType       Value;
        DataNode *      Next;
    };

public:

    std::pair<bool, ValueType> findOrAdd(const uint32_t hashKey, const char * start, const char * end, ValueType value) {

        const auto mask = (1ULL << HashBitsPerTrie) - 1ULL;
        void ** current = Root;
        auto key = hashKey;
        for (size_t i = 1; i < NumOfHashTrieLevels; ++i) {
           void ** node = current + (key & mask);
           if (*node == nullptr) {
               void ** newNode = mInternalAllocator.allocate<void *>(1ULL << HashBitsPerTrie);
               for (unsigned i = 0; i < (1ULL << HashBitsPerTrie); ++i) {
                   newNode[i] = nullptr;
               }
               *node = newNode;
           }
           key >>= HashBitsPerTrie;
           current = node;
        }



        DataNode ** data = (DataNode **)current;

        DataNode * prior = nullptr;

        assert (*end == '\n');

        const size_t length = end - start;

        for (;;) {

            if (LLVM_LIKELY(*data == nullptr)) {
                DataNode * newNode = mInternalAllocator.allocate<DataNode>(1);
                char * string = mInternalAllocator.allocate<char>(length);
                std::strncpy(string, start, length);
                newNode->Start = string;
                newNode->Length = length;
                newNode->Value = value;
                newNode->Next = prior;
                *((DataNode **)current) = newNode;
                return std::make_pair(true, value);
            }

            prior = *data;

            if (prior->Length == length) {
                if (std::strncmp(prior->Start, start, length) == 0) {
                    return std::make_pair(false, prior->Value);
                }
            }

            data = &(prior->Next);

        }


    }

    HashArrayMappedTrie() = default;

    void reset() {
        mInternalAllocator.Reset();
        Root = mInternalAllocator.allocate<void *>(1ULL << HashBitsPerTrie);
        for (unsigned i = 0; i < (1ULL << HashBitsPerTrie); ++i) {
            Root[i] = nullptr;
        }
    }

private:

    SlabAllocator<char> mInternalAllocator;
    void ** Root;
};

static HashArrayMappedTrie SingleFieldSet;
static HashArrayMappedTrie MultiFieldSet;

bool check_singleton_keyset(const size_t numOfFields, const uint32_t * const hashCode, const char ** pointers, const size_t lineNum) {
    assert (numOfFields == 1);
    return SingleFieldSet.findOrAdd(hashCode[0], pointers[0], pointers[1], lineNum).first;
}

bool check_composite_keyset(const size_t numOfFields, const uint32_t * const hashCode, const char ** pointers, const size_t lineNum) {

    // when using the composite keyset, we store each field independently rather than the full composite key
    // and then use the ids of those keys as our "string" for the composite key.

    SmallVector<size_t, 8> subKeys(numOfFields);
    uint32_t combinedHashCode = 5381;
    for (size_t i = 0; i < numOfFields; ++i) {
        const auto h = hashCode[i];
        auto f = MultiFieldSet.findOrAdd(h, pointers[i * 2], pointers[i * 2 + 1], lineNum);
        combinedHashCode = (combinedHashCode * 33) + h; // using djb2 as combiner
        subKeys[i] = f.second;
    }
    const auto s = (const char *)(subKeys.data());
    return SingleFieldSet.findOrAdd(combinedHashCode, s, s + numOfFields * sizeof(uint32_t), lineNum).first;
}

CheckKeyUniqueness::Config CheckKeyUniqueness::makeCreateHashTableConfig(const csv::CSVSchema & schema, const size_t bitsPerHashCode) {
    std::string sig;
    sig.reserve(1024);
    raw_string_ostream out(sig);

    // When provided hash codes or data positions, we get them in sequential order.
    // Thus there is no difference between a key {1,2,3} and {3,9,17}. The only time
    // an ordering matters is when we have more than one unique composite key since
    // {1,2},{2,3} would differ from {1,3},{2,3} when creating the "string" needed
    // to match the composite layers.

    size_t fieldsPerKey = 0;

    if (LLVM_LIKELY(schema.CompositeKey.size() == 1)) {
        fieldsPerKey = schema.CompositeKey.size();
    } else {
        boost::container::flat_set<size_t> SetOfKeyColumns;
        for (const CSVSchemaCompositeKey & key : schema.CompositeKey) {
            for (const auto k : key.Fields) {
                SetOfKeyColumns.insert(k);
            }
        }
        fieldsPerKey = SetOfKeyColumns.size();

        for (const CSVSchemaCompositeKey & key : schema.CompositeKey) {
            char joiner = '{';
            assert (!key.Fields.empty());
            for (const auto k : key.Fields) {
                auto f = SetOfKeyColumns.find(k);
                assert (f != SetOfKeyColumns.end());
                out << joiner << std::distance(SetOfKeyColumns.begin(), f);
                joiner = ',';
            }
            out << '}';
        }
    }

    out << bitsPerHashCode << 'x' << fieldsPerKey;

    out.flush();

    Config C;
    C.FieldsPerKey = fieldsPerKey;
    C.Signature = sig;
}


CheckKeyUniqueness::CheckKeyUniqueness(BuilderRef b, const csv::CSVSchema & schema, StreamSet * ByteStream, StreamSet * const hashCodes, StreamSet * coordinates)
: CheckKeyUniqueness(b, schema, makeCreateHashTableConfig(schema, hashCodes->getFieldWidth()), ByteStream, hashCodes, coordinates) {

}

CheckKeyUniqueness::CheckKeyUniqueness(BuilderRef b, const csv::CSVSchema & schema, Config && config, StreamSet * ByteStream, StreamSet * const hashCodes, StreamSet * coordinates)
: SegmentOrientedKernel(b, "cht" + getStringHash(config.Signature),
// inputs
{Binding{"InputStream", ByteStream, GreedyRate(), Deferred()}
, Binding{"HashCodes", hashCodes, FixedRate(1)}
, Binding{"Coordinates", coordinates, FixedRate(2)}},
// outputs
{},
// input scalars
{},
// output scalars
{},
// kernel state
{})
, mSchema(schema)
, mSignature(std::move(config.Signature)) {
    setStride(config.FieldsPerKey);
    addAttribute(SideEffecting());
    addAttribute(MayFatallyTerminate());
}

StringRef CheckKeyUniqueness::getSignature() const {
    return mSignature;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief linkExternalMethods
 ** ------------------------------------------------------------------------------------------------------------- */
void CheckKeyUniqueness::linkExternalMethods(BuilderRef b) {
    // bool check_unique_keyset(UniqueKeySets & sets, const size_t hashCode, const size_t numOfFields, const char ** keys)

    FixedArray<Type *, 4> paramTys;
    paramTys[0] = b->getInt32Ty();
    paramTys[1] = b->getSizeTy()->getPointerTo();
    paramTys[2] = b->getInt8PtrTy()->getPointerTo();
    paramTys[3] = b->getSizeTy();
    FunctionType * fty = FunctionType::get(b->getInt1Ty(), paramTys, false);
    if (FieldsPerKey == 1) {
        b->LinkFunction("checkSingleKey", fty, (void*)check_singleton_keyset);
    } else {
        b->LinkFunction("checkCompositeKey", fty, (void*)check_composite_keyset);
    }
}

#warning at init, malloc the HashArrayMappedTries

void CheckKeyUniqueness::generateDoSegmentMethod(BuilderRef b) {

    BasicBlock * const entry = b->GetInsertBlock();
    BasicBlock * const loopStart = b->CreateBasicBlock("strideCoordinateVecLoop");
    BasicBlock * const foundDuplicate = b->CreateBasicBlock("foundDuplicate");
    BasicBlock * const continueToNext = b->CreateBasicBlock("continueToNext");
    BasicBlock * const loopEnd = b->CreateBasicBlock("strideCoordinateElemLoop");

    Constant * const sz_ZERO = b->getSize(0);
    Constant * const sz_ONE = b->getSize(1);
    IntegerType * sizeTy = b->getSizeTy();

    Value * const hashCodesProcessed = b->getProcessedItemCount("HashCodes");
    Value * const baseHashCodePtr = b->getRawInputPointer("HashCodes", sz_ZERO);
    Value * const baseCoordinatePtr = b->getRawInputPointer("Coordinates", sz_ZERO);
    Value * const baseInputPtr = b->getRawInputPointer("InputStream", sz_ZERO);

    boost::container::flat_set<size_t> SetOfKeyColumns;
    for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
        for (const auto k : key.Fields) {
            SetOfKeyColumns.insert(k);
        }
    }
    assert (mStride == SetOfKeyColumns.size());


    ArrayType * const hashCodeArrayTy = ArrayType::get(b->getInt32Ty(), mStride);
    Value * const hashCodeArray = b->CreateAllocaAtEntryPoint(hashCodeArrayTy);

    ArrayType * const coordTy = ArrayType::get(b->getInt8PtrTy(), mStride * 2);
    Value * const coordArray = b->CreateAllocaAtEntryPoint(coordTy);


    Value * const numOfUnprocessedHashCodes = b->getAccessibleItemCount("HashCodes");
    Value * const totalHashCodeProcessed = b->CreateAdd(hashCodesProcessed, numOfUnprocessedHashCodes);

    Value * const initial = b->getProcessedItemCount("InputStream");

    b->CreateLikelyCondBr(b->CreateICmpNE(numOfUnprocessedHashCodes, sz_ZERO), loopStart, loopEnd);

    b->SetInsertPoint(loopStart);
    PHINode * const hashCodesProcessedPhi = b->CreatePHI(sizeTy, 2);
    hashCodesProcessedPhi->addIncoming(hashCodesProcessed, entry);

    Value * pos = nullptr;
    FixedArray<Value *, 2> offset;
    offset[0] = sz_ZERO;
    Value * const baseCoordinate = b->CreateShl(hashCodesProcessedPhi, sz_ONE);
    for (unsigned i = 0; i < (mStride * 2); ++i) {
        Constant * sz_I = b->getSize(i);
        pos = b->CreateLoad(b->CreateGEP(baseCoordinatePtr, b->CreateAdd(baseCoordinate, sz_I)));
        assert (pos->getType() == sizeTy);
        Value * const ptr = b->CreateGEP(baseInputPtr, pos);
        offset[1] = sz_I;
        b->CreateStore(ptr, b->CreateGEP(coordArray, offset));
    }

    ConstantInt * sz_STEP = b->getSize(mStride);

    FixedArray<Value *, 4> args;
    args[0] = sz_STEP;
    args[1] = b->CreateGEP(baseHashCodePtr, hashCodesProcessedPhi);
    args[2] = b->CreateBitCast(coordArray, b->getInt8PtrTy()->getPointerTo());
    args[3] = b->CreateExactUDiv(hashCodesProcessedPhi, sz_STEP);

    Function * callbackFn = nullptr;
    if (mStride == 1) {
        callbackFn = b->getModule()->getFunction("checkSingleKey");
    } else {
        callbackFn = b->getModule()->getFunction("checkCompositeKey");
    }
    Value * const retVal = b->CreateCall(callbackFn->getFunctionType(), callbackFn, args);
    b->CreateUnlikelyCondBr(b->CreateIsNull(retVal), foundDuplicate, continueToNext);

    b->SetInsertPoint(foundDuplicate);
    b->setFatalTerminationSignal();
    b->CreateBr(continueToNext);

    b->SetInsertPoint(continueToNext);


    Value * const nextHashCodesProcessed = b->CreateAdd(hashCodesProcessedPhi, sz_STEP);
    hashCodesProcessedPhi->addIncoming(nextHashCodesProcessed, entry);
    Value * const notDone = b->CreateICmpULT(nextHashCodesProcessed, totalHashCodeProcessed);
    b->CreateCondBr(notDone, loopStart, loopEnd);

    b->SetInsertPoint(loopEnd);
    PHINode * const finalPos = b->CreatePHI(sizeTy, 2);
    finalPos->addIncoming(initial, entry);
    finalPos->addIncoming(pos, continueToNext);
    b->setProcessedItemCount("InputStream", finalPos);
}


}
