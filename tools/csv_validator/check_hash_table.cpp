#include "check_hash_table.h"

#include <boost/container/flat_set.hpp>
#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include "csv_validator_toolchain.h"

inline size_t ceil_udiv(const size_t n, const size_t m) {
    return (n + m - 1) / m;
}

using namespace llvm;
using namespace csv;

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

#define CACHE_LINE_SIZE (64)

namespace kernel {


class alignas(CACHE_LINE_SIZE) HashArrayMappedTrie  {

    using ValueType = size_t;

    struct DataNode {
        const char *    Start;
        size_t          Length;
        size_t          HashValue;
        DataNode *      Next;
        ValueType       DataValue;
    };

public:

    bool findOrAdd(const uint32_t hashKey, const char * start, const char * end, ValueType value, size_t * hashValue) {

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

        const size_t length = end - start;

        for (;;) {

            if (LLVM_LIKELY(*data == nullptr)) {
                DataNode * newNode = mInternalAllocator.allocate<DataNode>(1);
                char * string = mInternalAllocator.allocate<char>(length);
                std::strncpy(string, start, length);
                newNode->Start = string;
                newNode->Length = length;
                const auto hv = NodeCount++;
                *hashValue = hv;
                newNode->HashValue = hv;
                newNode->DataValue = value;
                newNode->Next = prior;
                *((DataNode **)current) = newNode;
                return false;
            }

            prior = *data;

            if (prior->Length == length) {
                if (std::strncmp(prior->Start, start, length) == 0) {
                    *hashValue = prior->HashValue;
                    return true;
                }
            }

            data = &(prior->Next);

        }


    }

    HashArrayMappedTrie() {
        mInternalAllocator.Reset();
        Root = mInternalAllocator.allocate<void *>(1ULL << HashBitsPerTrie);
        for (unsigned i = 0; i < (1ULL << HashBitsPerTrie); ++i) {
            Root[i] = nullptr;
        }
        NodeCount = 0;
    }

private:

    SlabAllocator<char> mInternalAllocator;
    void ** Root = nullptr;
    size_t NodeCount = 0;
};


HashArrayMappedTrie * construct_n_tries(const size_t n) {
    const auto m = sizeof(HashArrayMappedTrie) * n;
    HashArrayMappedTrie * const tries = (HashArrayMappedTrie *)aligned_alloc(CACHE_LINE_SIZE, m);
    for (size_t i = 0; i < n; ++i) {
        new (tries + i) HashArrayMappedTrie();
    }
    return tries;
}

bool check_hash_code(HashArrayMappedTrie * const table, const uint32_t hashCode, const char * start, const char * end, const size_t lineNum, size_t * out) {
    return table->findOrAdd(hashCode, start, end, lineNum, out);
}

void report_duplicate_key(HashArrayMappedTrie * const table, const size_t failingLineNum, const size_t originalLineNum) {
    errs() << "duplicate key found on row " << failingLineNum << " (originally observed on " << originalLineNum << ")";
}

void deconstruct_n_tries(HashArrayMappedTrie * const tables, const size_t n) {
    for (size_t i = 0; i < n; ++i) {
        (tables + i)->~HashArrayMappedTrie();
    }
    std::free(tables);
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
        for (unsigned i = 0; i < SetOfKeyColumns.size(); ++i) {
            const auto k = SetOfKeyColumns.nth(i);
            const CSVSchemaColumnRule & col = schema.Column[*k];
            if (LLVM_UNLIKELY(col.Warning)) {
                out << 'w' << i;
            }
        }


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
    return C;
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
    addInternalScalar(b->getVoidPtrTy(), "hashTableObjects");
}

StringRef CheckKeyUniqueness::getSignature() const {
    return mSignature;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief linkExternalMethods
 ** ------------------------------------------------------------------------------------------------------------- */
void CheckKeyUniqueness::linkExternalMethods(BuilderRef b) {

    IntegerType * const sizeTy = b->getSizeTy();
    PointerType * const voidPtrTy = b->getVoidPtrTy();

    BEGIN_SCOPED_REGION
    FixedArray<Type *, 4> paramTys;
    paramTys[0] = b->getInt32Ty();
    paramTys[1] = sizeTy->getPointerTo();
    paramTys[2] = b->getInt8PtrTy()->getPointerTo();
    paramTys[3] = b->getSizeTy();
    FunctionType * fty = FunctionType::get(b->getInt1Ty(), paramTys, false);
    b->LinkFunction("check_hash_code", fty, (void*)check_hash_code);
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    FixedArray<Type *, 3> paramTys;
    paramTys[0] = voidPtrTy;
    paramTys[1] = sizeTy;
    paramTys[2] = sizeTy;
    FunctionType * fty = FunctionType::get(voidPtrTy, paramTys, false);
    b->LinkFunction("report_duplicate_key", fty, (void*)report_duplicate_key);
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    FixedArray<Type *, 1> paramTys;
    paramTys[0] = sizeTy;
    FunctionType * fty = FunctionType::get(voidPtrTy, paramTys, false);
    b->LinkFunction("construct_n_tries", fty, (void*)construct_n_tries);
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    FixedArray<Type *, 2> paramTys;
    paramTys[0] = voidPtrTy;
    paramTys[1] = sizeTy;
    FunctionType * fty = FunctionType::get(b->getVoidTy(), paramTys, false);
    b->LinkFunction("deconstruct_n_tries", fty, (void*)deconstruct_n_tries);
    END_SCOPED_REGION

}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateDoSegmentMethod
 ** ------------------------------------------------------------------------------------------------------------- */
void CheckKeyUniqueness::generateDoSegmentMethod(BuilderRef b) {

    BasicBlock * const entry = b->GetInsertBlock();
    BasicBlock * const loopStart = b->CreateBasicBlock("strideCoordinateVecLoop");
    BasicBlock * const foundDuplicate = b->CreateBasicBlock("foundDuplicate");
    BasicBlock * const continueToNext = b->CreateBasicBlock("continueToNext");
    BasicBlock * const loopEnd = b->CreateBasicBlock("strideCoordinateElemLoop");

    Constant * const sz_ZERO = b->getSize(0);
    Constant * const sz_ONE = b->getSize(1);
    IntegerType *  const sizeTy = b->getSizeTy();
    IntegerType * const i32Ty = b->getInt32Ty();

    Value * const hashCodesProcessed = b->getProcessedItemCount("HashCodes");
    Value * const baseHashCodePtr = b->getRawInputPointer("HashCodes", sz_ZERO);
    Value * const baseCoordinatePtr = b->getRawInputPointer("Coordinates", sz_ZERO);
    Value * const baseInputPtr = b->getRawInputPointer("InputStream", sz_ZERO);

    boost::container::flat_set<size_t> SetOfKeyColumns;
    SetOfKeyColumns.reserve(mStride);
    for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
        for (const auto k : key.Fields) {
            SetOfKeyColumns.insert(k);
        }
    }
    assert (mStride == SetOfKeyColumns.size());

    BitVector mustBeUnique(mStride, false);

    for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
        if (key.Fields.size() == 1) {
            const auto f = SetOfKeyColumns.find(key.Fields.front());
            const auto k = std::distance(SetOfKeyColumns.begin(), f);
            mustBeUnique.set(k);
        }
    }

    size_t compositeKeys = 0;

    for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
        if (key.Fields.size() > 1) {
            for (auto v : key.Fields) {
                const auto f = SetOfKeyColumns.find(v);
                const auto k = std::distance(SetOfKeyColumns.begin(), f);
                if (LLVM_UNLIKELY(mustBeUnique.test(k))) {
                    goto has_unique_key;
                }
            }
            compositeKeys++;
has_unique_key:
            continue;
        }
    }

    const auto totalKeys = mStride + compositeKeys;

    ArrayType * const hashValueArrayTy = ArrayType::get(sizeTy, totalKeys);
    Value * const hashValueArray = b->CreateAllocaAtEntryPoint(hashValueArrayTy);
    Value * const outValueArray = b->CreateAllocaAtEntryPoint(hashValueArrayTy);

    Value * const numOfUnprocessedHashCodes = b->getAccessibleItemCount("HashCodes");
    Value * const totalHashCodeProcessed = b->CreateAdd(hashCodesProcessed, numOfUnprocessedHashCodes);

    Value * const initial = b->getProcessedItemCount("InputStream");

    b->CreateLikelyCondBr(b->CreateICmpNE(numOfUnprocessedHashCodes, sz_ZERO), loopStart, loopEnd);

    b->SetInsertPoint(loopStart);
    PHINode * const hashCodesProcessedPhi = b->CreatePHI(sizeTy, 2);
    hashCodesProcessedPhi->addIncoming(hashCodesProcessed, entry);

    FixedArray<Value *, 6> args;

    args[4] = b->CreateExactUDiv(hashCodesProcessedPhi, b->getSize(mStride));

    Module * const m = b->getModule();

    Function * checkHashCodeFn = m->getFunction("check_hash_code");

    size_t currentKey = 0;

    Value * endOfLastSymbol = nullptr;

    const auto bw = sizeTy->getBitWidth();

    SmallVector<Value *, 2> resultArray(ceil_udiv(totalKeys, bw), nullptr);

    auto updateResultArray = [&](Value * result, const size_t k) {

        result = b->CreateZExt(result, sizeTy);
        const auto index = ceil_udiv(k, bw);
        const auto offset = k & (bw - 1);
        if (offset > 0) {
            result = b->CreateShl(result, b->getSize(offset));
            result = b->CreateOr(result, resultArray[index]);
        } else {
            assert (resultArray[index] == nullptr);
        }
        resultArray[index] = result;
    };

    Value * const baseHashTableObjects = b->CreatePointerCast(b->getScalarField("hashTableObjects"), b->getInt8PtrTy());

    for (unsigned i = 0; i < mStride; ++i) {

        ConstantInt * const sz_Offset = b->getSize(i * sizeof(HashArrayMappedTrie));
        args[0] = b->CreateGEP(baseHashTableObjects, sz_Offset);
        Value * const hashCodePtr = b->CreateGEP(baseHashCodePtr, b->CreateAdd(hashCodesProcessedPhi, b->getSize(i)));
        args[1] = b->CreateZExt(b->CreateLoad(hashCodePtr), i32Ty);
        Value * const startPtr = b->CreateGEP(baseCoordinatePtr, b->CreateAdd(hashCodesProcessedPhi, b->getSize(i * 2)));
        args[2] = b->CreateGEP(baseInputPtr, b->CreateLoad(startPtr));
        Value * const endPtr = b->CreateGEP(baseCoordinatePtr, b->CreateAdd(hashCodesProcessedPhi, b->getSize(i * 2 + 1)));
        endOfLastSymbol = b->CreateLoad(endPtr);
        args[3] = b->CreateGEP(baseInputPtr, endOfLastSymbol);

        args[5] = b->CreateGEP(hashValueArray, b->getSize(i));

        Value * result = b->CreateCall(checkHashCodeFn->getFunctionType(), checkHashCodeFn, args);

        if (LLVM_LIKELY(mustBeUnique.test(i))) {
            updateResultArray(result, i);
        }

    }

    assert (endOfLastSymbol);

    if (compositeKeys > 0) {

        ConstantInt * const i32_5381 = b->getInt32(5381);
        ConstantInt * const i32_33 = b->getInt32(33);

        Value * startBasePtr = b->CreatePointerCast(outValueArray, b->getInt8PtrTy());

        size_t compositeKeyIndex = mStride;

        for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
            const auto n = key.Fields.size();
            if (n > 1) {

                for (auto v : key.Fields) {
                    const auto f = SetOfKeyColumns.find(v);
                    const auto k = std::distance(SetOfKeyColumns.begin(), f);
                    if (LLVM_UNLIKELY(mustBeUnique.test(k))) {
                        goto skip_composite_key;
                    }
                }

                BEGIN_SCOPED_REGION

                auto itr = key.Fields.begin();

                Value * hashCode = i32_5381; // using djb2 as combiner

                for (unsigned j = 0; j < n; ++j) {

                    const auto f = SetOfKeyColumns.find(*itr++);
                    const auto k = std::distance(SetOfKeyColumns.begin(), f);
                    Value * const inPtr = b->CreateGEP(hashValueArray, b->getSize(k));
                    Value * const outPtr = b->CreateGEP(outValueArray, b->getSize(j));
                    b->CreateStore(b->CreateLoad(inPtr), outPtr);

                    Value * const hashCodePtr = b->CreateGEP(baseHashCodePtr, b->CreateAdd(hashCodesProcessedPhi, b->getSize(k)));
                    Value * const hashVal = b->CreateZExt(b->CreateLoad(hashCodePtr), i32Ty);

                    hashCode = b->CreateAdd(b->CreateMul(hashCode, i32_33), hashVal); // using djb2 as combiner

                }

                ConstantInt * const sz_Offset = b->getSize(compositeKeyIndex * sizeof(HashArrayMappedTrie));
                args[0] = b->CreateGEP(baseHashTableObjects, sz_Offset);

                args[1] = hashCode;
                args[2] = startBasePtr;
                args[3] = b->CreateGEP(startBasePtr, b->getSize(n * sizeof(size_t)));

                args[5] = b->CreateGEP(hashValueArray, b->getSize(compositeKeyIndex));

                Value *  result = b->CreateCall(checkHashCodeFn->getFunctionType(), checkHashCodeFn, args);

                updateResultArray(result, compositeKeyIndex++);

                END_SCOPED_REGION
skip_composite_key:
                continue;

            }
        }

    }

    Value * allResults = resultArray[0];
    for (unsigned i = 1; i < resultArray.size(); ++i) {
        allResults = b->CreateOr(allResults, resultArray[i]);
    }

    b->CreateLikelyCondBr(b->CreateICmpEQ(allResults, sz_ZERO), continueToNext, foundDuplicate);

    b->SetInsertPoint(foundDuplicate);
    Value * earliestResult = resultArray[0];
    Value * earliestResultOffset = sz_ZERO;
    for (unsigned i = 1; i < resultArray.size(); ++i) {
        Value * const alreadyFound = b->CreateICmpNE(earliestResult, sz_ZERO);
        earliestResult = b->CreateSelect(alreadyFound, earliestResult, resultArray[i]);
        earliestResultOffset = b->CreateSelect(alreadyFound, earliestResultOffset, b->getSize(i * bw));
    }
    Value * failingKey = b->CreateCountReverseZeroes(earliestResult);
    if (resultArray.size() > 1) {
        failingKey = b->CreateAdd(earliestResultOffset, failingKey);
    }

    FixedArray<Value *, 3> reportArgs;
    Value * const sz_Offset = b->CreateMul(b->getSize(sizeof(HashArrayMappedTrie)), failingKey);
    reportArgs[0] = b->CreateGEP(baseHashTableObjects, sz_Offset);
    reportArgs[1] = args[4];
    reportArgs[2] = b->CreateLoad(b->CreateGEP(hashValueArray, failingKey));
    Function * reportDuplicateKeyFn = m->getFunction("report_duplicate_key");
    b->CreateCall(reportDuplicateKeyFn, reportArgs);

    // TODO: to make warnings work, have this construct a constant array to indicate
    // that if all key columns have a warning flag, then do not set the termination signal.

    // Have tables contain a friendly name for the key type / fields?

    b->setFatalTerminationSignal();
    b->CreateBr(continueToNext);

    b->SetInsertPoint(continueToNext);


    Value * const nextHashCodesProcessed = b->CreateAdd(hashCodesProcessedPhi, b->getSize(mStride));
    hashCodesProcessedPhi->addIncoming(nextHashCodesProcessed, entry);
    Value * const notDone = b->CreateICmpULT(nextHashCodesProcessed, totalHashCodeProcessed);
    b->CreateCondBr(notDone, loopStart, loopEnd);

    b->SetInsertPoint(loopEnd);
    PHINode * const finalPos = b->CreatePHI(sizeTy, 2);
    finalPos->addIncoming(initial, entry);
    finalPos->addIncoming(endOfLastSymbol, continueToNext);
    b->setProcessedItemCount("InputStream", finalPos);
}

void CheckKeyUniqueness::generateInitializeMethod(BuilderRef b) {
    Function * constructNTriesFn = b->getModule()->getFunction("construct_n_tries");
    size_t compositeKeys = 0;
    for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
        if (key.Fields.size() > 1) {
            compositeKeys++;
        }
    }
    FunctionType * fty = constructNTriesFn->getFunctionType();
    Value * const hashTableObjects = b->CreateCall(fty, constructNTriesFn, { b->getSize(mStride + compositeKeys)});
    b->setScalarField("hashTableObjects", b->CreatePointerCast(hashTableObjects, b->getVoidPtrTy()));
}

void CheckKeyUniqueness::generateFinalizeMethod(BuilderRef b) {
    Function * deconstructNTriesFn = b->getModule()->getFunction("deconstruct_n_tries");
    size_t compositeKeys = 0;
    for (const CSVSchemaCompositeKey & key : mSchema.CompositeKey) {
        if (key.Fields.size() > 1) {
            compositeKeys++;
        }
    }
    FunctionType * fty = deconstructNTriesFn->getFunctionType();
    FixedArray<Value *, 2> args;
    args[0] = b->CreatePointerCast(b->getScalarField("hashTableObjects"), b->getVoidPtrTy());
    args[1] = b->getSize(mStride + compositeKeys);
    b->CreateCall(fty, deconstructNTriesFn, args);
}

}
