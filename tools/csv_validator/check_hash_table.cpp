#include "check_hash_table.h"

#include <boost/container/flat_set.hpp>
#include <boost/dynamic_bitset.hpp>
#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/BitVector.h>
#include "csv_validator_toolchain.h"

inline size_t ceil_udiv(const size_t n, const size_t m) {
    return (n + m - 1) / m;
}

using namespace boost;
using namespace boost::container;
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

    bool findOrAdd(const uint32_t hashKey, const char * start, const char * end, ValueType value, size_t & hashValue) {
        const auto mask = (1ULL << HashBitsPerTrie) - 1ULL;
        void * current = Root; assert (Root);
        auto key = hashKey;
        for (size_t i = 1; i < NumOfHashTrieLevels; ++i) {
           const auto k = (key & mask);
           assert (k < (1ULL << HashBitsPerTrie));
           DataNode ** node = ((DataNode **)current) + k;
           if (*node == nullptr) {
               DataNode ** newNode = mInternalAllocator.allocate<DataNode *>(1ULL << HashBitsPerTrie);
               for (unsigned i = 0; i < (1ULL << HashBitsPerTrie); ++i) {
                   newNode[i] = nullptr;
               }
               *node = (DataNode *)newNode;
           }
           key >>= HashBitsPerTrie;
           current = *node;
        }

        DataNode ** data = (DataNode **)current;

        DataNode * prior = nullptr;

        assert (start <= end);

        const size_t length = end - start;

        for (;;) {

            if (LLVM_LIKELY(*data == nullptr)) {
                DataNode * newNode = mInternalAllocator.allocate<DataNode>(1);
                char * string = mInternalAllocator.allocate<char>(length);
                std::strncpy(string, start, length);
                newNode->Start = string;
                newNode->Length = length;
                const auto hv = NodeCount++;
                hashValue = hv;
                newNode->HashValue = hv;
                newNode->DataValue = value;
                newNode->Next = prior;
                *((DataNode **)current) = newNode;
                return false;
            }

            prior = *data;

            if (prior->Length == length) {
                if (std::strncmp(prior->Start, start, length) == 0) {
                    hashValue = prior->HashValue;
                    return true;
                }
            }

            data = &(prior->Next);

        }


    }

    HashArrayMappedTrie() {
        mInternalAllocator.Reset();
        DataNode ** newRoot = mInternalAllocator.allocate<DataNode *>(1ULL << HashBitsPerTrie);
        for (unsigned i = 0; i < (1ULL << HashBitsPerTrie); ++i) {
            newRoot[i] = nullptr;
        }
        Root = newRoot;
        NodeCount = 0;
    }


private:

    SlabAllocator<void*> mInternalAllocator;
    void * Root = nullptr;
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

bool check_hash_code(HashArrayMappedTrie * const table, const uint32_t hashCode, const char * start, const char * end, const size_t lineNum, size_t & out) {
    return table->findOrAdd(hashCode, start, end, lineNum, out);
}

bool report_duplicate_key(const csv::CSVSchema & schema, const char * fileName, HashArrayMappedTrie * const table, const char * start, const char * end, const size_t keyNum, const size_t failingKeyNum, const size_t originalKeyNum) {
    // TODO: if we hard coded the columnNum into the program to report here, we'd end up compiling nearly identical kernels. Need to pass that info in
    // at runtime.
    assert ((failingKeyNum % schema.NumOfIdentifyingColumns) == (originalKeyNum % schema.NumOfIdentifyingColumns));

    auto & out = errs();

    size_t keyIndex = 0;

    const auto n = schema.Column.size();
    for (size_t i = 0; i < n; ++i) {
        const CSVSchemaColumnRule & column = schema.Column[i];
        const auto m = column.CompositeKey.size();
        if ((keyIndex + m) < keyNum) {
            const CSVSchemaCompositeKey & key = column.CompositeKey[keyNum - keyIndex];
            if (column.Warning) {
                out << "Warning";
            } else {
                out << "Error";
            }

            out << ": ";
            const auto failingLineNum = (failingKeyNum / schema.NumOfIdentifyingColumns) + 1;
            out << "duplicate key observed on record: "
                << failingLineNum
                << ", column: ";

            if (key.Fields.size() > 1) {
                out << "{";
            }
            bool notFirst = false;
            for (const auto keyColId : key.Fields) {
                if (notFirst) {
                    out << ", ";
                }
                out << schema.Column[keyColId].Name;
                out << " (" << (keyColId + 1) << ")";
            }
            if (key.Fields.size() > 1) {
                out << "}";
            }

            out << ", value: \"";
            for (auto c = start; c < end; ++c) {
                out << *c;
            }
            const auto originalLineNum = ((originalKeyNum / schema.NumOfIdentifyingColumns) + 1);
            out << "\" originally observed on " << originalLineNum << "\n";

            return column.Warning || !failFast;
        } else {
            keyIndex += m;
        }
    }
    llvm_unreachable("Unknown key?");
    return false;
}

void deconstruct_n_tries(HashArrayMappedTrie * const tables, const size_t n) {
    for (size_t i = 0; i < n; ++i) {
        (tables + i)->~HashArrayMappedTrie();
    }
    std::free(tables);
}

std::string CheckKeyUniqueness::makeSignature(const csv::CSVSchema & schema, const size_t bitsPerHashCode) {
    std::string sig;
    sig.reserve(1024);
    raw_string_ostream out(sig);

    // When provided hash codes or data positions, we get them in sequential order.
    // Thus there is no difference between a key {1,2,3} and {3,9,17}. The only time
    // an ordering matters is when we have more than one unique composite key since
    // {1,2},{2,3} would differ from {1,3},{2,3} when creating the "string" needed
    // to match the composite layers.

    const auto n = schema.Column.size();
    std::vector<size_t> KeyIndex(n);

    for (size_t i = 0, j = 0; i < n; ++i) {
        const CSVSchemaColumnRule & column = schema.Column[i];
        if (column.IsIdentifyingColumn) {
            assert (j < schema.NumOfIdentifyingColumns);
            KeyIndex[i] = j++;
        }
    }


    for (const CSVSchemaColumnRule & column : schema.Column) {
        if (column.CompositeKey.size() > 0) {

            for (const CSVSchemaCompositeKey & key : column.CompositeKey) {
                char joiner = '{';
                assert (!key.Fields.empty());
                for (const auto k : key.Fields) {
                    out << joiner << KeyIndex[k];
                    joiner = ',';
                }
                out << '}';
            }
        }
    }

    out << bitsPerHashCode;

    out.flush();

    return sig;
}


CheckKeyUniqueness::CheckKeyUniqueness(BuilderRef b, const csv::CSVSchema & schema, StreamSet * ByteStream, StreamSet * const hashCodes, StreamSet * coordinates, Scalar * const schemaObject, Scalar * const fileName)
: CheckKeyUniqueness(b, schema, makeSignature(schema, hashCodes->getFieldWidth()), ByteStream, hashCodes, coordinates, schemaObject, fileName) {

}

CheckKeyUniqueness::CheckKeyUniqueness(BuilderRef b, const csv::CSVSchema & schema, std::string && signature, StreamSet * ByteStream, StreamSet * const hashCodes, StreamSet * coordinates, Scalar * const schemaObject, Scalar * const fileName)
: SegmentOrientedKernel(b, "cht" + getStringHash(signature),
// inputs
{Binding{"InputStream", ByteStream, GreedyRate(), Deferred()}
, Binding{"HashCodes", hashCodes, FixedRate(1)}
, Binding{"Coordinates", coordinates, FixedRate(2)}},
// outputs
{},
// input scalars
{Binding{"schemaObject", schemaObject}
,Binding{"fileName", fileName}},
// output scalars
{},
// kernel state
{InternalScalar{b->getVoidPtrTy(), "hashTableObjects"}})
, mSchema(schema)
, mSignature(std::move(signature)) {
    setStride(schema.NumOfIdentifyingColumns);
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

    IntegerType * const sizeTy = b->getSizeTy();
    PointerType * const voidPtrTy = b->getVoidPtrTy();
    IntegerType * const boolTy = b->getInt1Ty();
    PointerType * const int8PtrTy = b->getInt8PtrTy();

    BEGIN_SCOPED_REGION
    FixedArray<Type *, 6> paramTys;
    paramTys[0] = voidPtrTy;
    paramTys[1] = b->getInt32Ty();
    paramTys[2] = int8PtrTy;
    paramTys[3] = int8PtrTy;
    paramTys[4] = sizeTy;
    paramTys[5] = sizeTy->getPointerTo();
    FunctionType * fty = FunctionType::get(boolTy, paramTys, false);
    b->LinkFunction("check_hash_code", fty, (void*)check_hash_code);
    END_SCOPED_REGION

    BEGIN_SCOPED_REGION
    FixedArray<Type *, 8> paramTys;
    paramTys[0] = voidPtrTy;
    paramTys[1] = int8PtrTy;
    paramTys[2] = voidPtrTy;
    paramTys[3] = int8PtrTy;
    paramTys[4] = int8PtrTy;
    paramTys[5] = sizeTy;
    paramTys[6] = sizeTy;
    paramTys[7] = sizeTy;
    FunctionType * fty = FunctionType::get(boolTy, paramTys, false);
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
    BasicBlock * const fatalError = b->CreateBasicBlock("fatalError");
    BasicBlock * const acceptedAllDuplicates = b->CreateBasicBlock("acceptedAllErrors");
    BasicBlock * const loopEnd = b->CreateBasicBlock("strideCoordinateElemLoop");


    Constant * const sz_ZERO = b->getSize(0);
    Constant * const sz_ONE = b->getSize(1);
    Constant * const sz_TWO = b->getSize(2);
    Constant * const sz_STRIDE = b->getSize(mStride);
    IntegerType *  const sizeTy = b->getSizeTy();
    IntegerType * const i32Ty = b->getInt32Ty();

    StreamSet * const hashCodes = b->getInputStreamSet("HashCodes");
    IntegerType * const hashCodesTy = b->getIntNTy(hashCodes->getFieldWidth());
    Value * const hashCodesProcessed = b->getProcessedItemCount("HashCodes");
    Value * const baseHashCodePtr = b->getRawInputPointer("HashCodes", sz_ZERO);

    StreamSet * const coordinates = b->getInputStreamSet("Coordinates");
    IntegerType * const coordinatesTy = b->getIntNTy(coordinates->getFieldWidth());
    Value * const baseCoordinatePtr = b->getRawInputPointer("Coordinates", sz_ZERO);

    Value * const baseInputPtr = b->getRawInputPointer("InputStream", sz_ZERO);

    const auto n = mSchema.Column.size();

    std::vector<size_t> KeyIndex(n, -1U);
    size_t totalNumOfKeys = 0;

    for (size_t i = 0, j = 0; i < n; ++i) {
        const CSVSchemaColumnRule & column = mSchema.Column[i];
        if (column.IsIdentifyingColumn) {
            KeyIndex[i] = j++;
        }
        totalNumOfKeys += column.CompositeKey.size();
    }

    boost::dynamic_bitset<size_t> mustBeUnique(mStride, false);

    for (size_t i = 0; i < n; ++i) {
        const CSVSchemaColumnRule & column = mSchema.Column[i];
        for (const CSVSchemaCompositeKey & key : column.CompositeKey) {
            const auto & fields = key.Fields;
            if (fields.size() == 1) {
                const auto k = KeyIndex[fields[0]];
                assert (k < mStride);
                mustBeUnique.set(k);
            }
        }
    }

    boost::dynamic_bitset<size_t> isCompositeKey(totalNumOfKeys, false);

    for (size_t i = 0, j = mStride; i < n; ++i) {
        const CSVSchemaColumnRule & column = mSchema.Column[i];
        for (const CSVSchemaCompositeKey & key : column.CompositeKey) {
            const auto & fields = key.Fields;
            if (fields.size() > 1) {
                bool notCompositeKey = false;
                for (auto v : fields) {
                    const auto k = KeyIndex[v];
                    if (LLVM_UNLIKELY(k < mStride && mustBeUnique.test(k))) {
                        notCompositeKey = true;
                    }
                }
                if (!notCompositeKey) {
                    isCompositeKey.set(j);
                }
            }
            ++j;
        }
    }

    const auto compositeKeys = isCompositeKey.count();

    assert (mustBeUnique.any() || compositeKeys > 0);

    const auto totalKeys = mStride + compositeKeys;

    ArrayType * const priorLineNumArrayTy = ArrayType::get(sizeTy, totalKeys);
    Value * const priorLineNumArray = b->CreateAllocaAtEntryPoint(priorLineNumArrayTy);
    Value * const outValueArray = b->CreateAllocaAtEntryPoint(priorLineNumArrayTy);

    Value * const unprocessedHashCodes = b->getAccessibleItemCount("HashCodes");
    Value * const totalHashCodeProcessed = b->CreateAdd(hashCodesProcessed, unprocessedHashCodes);

    Value * const initial = b->getProcessedItemCount("InputStream");

    b->CreateLikelyCondBr(b->CreateICmpNE(unprocessedHashCodes, sz_ZERO), loopStart, loopEnd);

    b->SetInsertPoint(loopStart);
    PHINode * const hashCodesProcessedPhi = b->CreatePHI(sizeTy, 2);
    hashCodesProcessedPhi->addIncoming(hashCodesProcessed, entry);

    FixedArray<Value *, 6> args;

    Module * const m = b->getModule();

    Function * checkHashCodeFn = m->getFunction("check_hash_code");
    assert (checkHashCodeFn->getFunctionType()->getNumParams() == 6);

    Value * endPosition = nullptr;

    const auto sizeTyBitWidth = sizeTy->getBitWidth();

    IntegerType * int8Ty = b->getInt8Ty();
    Type * const voidPtrTy = b->getVoidPtrTy();

    Value * baseHashTableObjects = b->getScalarField("hashTableObjects");
    assert (baseHashTableObjects->getType() == voidPtrTy);
    baseHashTableObjects = b->CreatePointerCast(baseHashTableObjects, voidPtrTy->getPointerTo());

    FixedArray<Value *, 2> priorLineNumIndex;
    priorLineNumIndex[0] = sz_ZERO;

    SmallVector<Value *, 8> fieldResult((totalKeys + sizeTyBitWidth - 1) / sizeTyBitWidth, nullptr);

    size_t fieldResultCount = 0;

    auto addFieldResult = [&](Value * result) {
        Value * retVal = b->CreateZExt(result, sizeTy);
        const auto shift = fieldResultCount & (sizeTyBitWidth - 1);
        if (shift) {
            retVal = b->CreateShl(retVal, shift);
        }
        const auto idx = fieldResultCount / sizeTyBitWidth;
        assert (idx < fieldResult.size());
        Value *& r = fieldResult[idx];
        if (r == nullptr) {
            r = retVal;
        } else {
            r = b->CreateOr(r, retVal);
        }
        ++fieldResultCount;
    };

    // // bool check_hash_code(HashArrayMappedTrie * const table, const uint32_t hashCode, const char * start, const char * end, const size_t lineNum, size_t & out);

    for (unsigned i = 0; i < mStride; ++i) {
        ConstantInt * const sz_Offset = b->getSize(i * sizeof(HashArrayMappedTrie));
        args[0] = b->CreatePointerCast(b->CreateGEP(voidPtrTy, baseHashTableObjects, sz_Offset), voidPtrTy);
        Value * const hashIndex = b->CreateAdd(hashCodesProcessedPhi, b->getSize(i));
        Value * const hashCodePtr = b->CreateGEP(hashCodesTy, baseHashCodePtr, hashIndex);
        args[1] = b->CreateZExt(b->CreateLoad(hashCodesTy, hashCodePtr), i32Ty);
        Value * const startCoordIndex = b->CreateMul(hashIndex, sz_TWO);
        Value * const startPtr = b->CreateGEP(coordinatesTy, baseCoordinatePtr, startCoordIndex);
        Value * const startPosition = b->CreateLoad(coordinatesTy, startPtr);
        args[2] = b->CreateGEP(int8Ty, baseInputPtr, startPosition);
        Value * const endCoordIndex = b->CreateAdd(startCoordIndex, sz_ONE);
        Value * const endPtr = b->CreateGEP(coordinatesTy, baseCoordinatePtr, endCoordIndex);
        endPosition = b->CreateLoad(coordinatesTy, endPtr);
        args[3] = b->CreateGEP(int8Ty, baseInputPtr, endPosition);
        args[4] = hashCodesProcessedPhi;
        priorLineNumIndex[1] = b->getSize(i);
        args[5] = b->CreateGEP(priorLineNumArrayTy, priorLineNumArray, priorLineNumIndex);
        assert (checkHashCodeFn->getFunctionType()->getNumParams() == args.size());
        Value * const result = b->CreateCall(checkHashCodeFn->getFunctionType(), checkHashCodeFn, args);
        if (mustBeUnique.test(i)) {
            addFieldResult(result);
        }
    }

    assert (endPosition);

    if (compositeKeys > 0) {

        ConstantInt * const i32_5381 = b->getInt32(5381);
        ConstantInt * const i32_33 = b->getInt32(33);

        Value * startBasePtr = b->CreatePointerCast(outValueArray, b->getInt8PtrTy());

        for (size_t i = 0, k = mStride; i < n; ++i) {
            const CSVSchemaColumnRule & column = mSchema.Column[i];
            for (const CSVSchemaCompositeKey & key : column.CompositeKey) {
                const auto & fields = key.Fields;
                const auto m = fields.size();
                if (isCompositeKey.test(k)) { // composite key

                    Value * hashCode = i32_5381; // using djb2 as combiner

                    for (unsigned j = 0; j < m; ++j) {

                        const auto k = KeyIndex[fields[j]];
                        assert (k < mStride);
                        Value * const inPtr = b->CreateGEP(sizeTy, priorLineNumArray, b->getSize(k));
                        Value * const outPtr = b->CreateGEP(sizeTy, outValueArray, b->getSize(j));
                        b->CreateStore(b->CreateLoad(sizeTy, inPtr), outPtr);

                        Value * const hashCodePtr = b->CreateGEP(hashCodesTy, baseHashCodePtr, b->CreateAdd(hashCodesProcessedPhi, b->getSize(k)));
                        Value * const hashVal = b->CreateZExt(b->CreateLoad(hashCodesTy, hashCodePtr), i32Ty);

                        hashCode = b->CreateAdd(b->CreateMul(hashCode, i32_33), hashVal); // using djb2 as combiner

                    }

                    ConstantInt * const sz_Offset = b->getSize(k * sizeof(HashArrayMappedTrie));
                    args[0] = b->CreatePointerCast(b->CreateGEP(voidPtrTy, baseHashTableObjects, sz_Offset), voidPtrTy);
                    args[1] = hashCode;
                    args[2] = startBasePtr;
                    args[3] = b->CreateGEP(int8Ty, startBasePtr, b->getSize(m * sizeof(size_t)));
                    args[4] = hashCodesProcessedPhi;
                    priorLineNumIndex[1] = b->getSize(k);
                    args[5] = b->CreateGEP(priorLineNumArrayTy, priorLineNumArray, priorLineNumIndex);
                    assert (checkHashCodeFn->getFunctionType()->getNumParams() == args.size());
                    Value * const result = b->CreateCall(checkHashCodeFn->getFunctionType(), checkHashCodeFn, args);
                    addFieldResult(result);
                }

                ++k;

            }
        }
    }


    assert (fieldResultCount > 0);

    Value * anyFail = fieldResult[0]; assert (anyFail);
    for (unsigned i = 1; i < fieldResult.size(); ++i) {
        if (fieldResult[i]) {
            anyFail = b->CreateOr(anyFail, fieldResult[i]);
        }
    }
    anyFail = b->CreateICmpNE(anyFail, sz_ZERO);

    b->CreateUnlikelyCondBr(anyFail, foundDuplicate, acceptedAllDuplicates);

    b->SetInsertPoint(foundDuplicate);

    FixedArray<Value *, 8> reportArgs;
    reportArgs[0] = b->getScalarField("schemaObject");
    reportArgs[1] = b->getScalarField("fileName");

    for (unsigned currentFieldError = 0; currentFieldError < fieldResultCount; ++currentFieldError) {
        BasicBlock * const reportError = b->CreateBasicBlock("reportError", fatalError);

        BasicBlock * nextCheck = nullptr;
        if (currentFieldError == (fieldResultCount - 1)) {
            nextCheck = acceptedAllDuplicates;
        } else {
            nextCheck = b->CreateBasicBlock("continueToNext", fatalError);
        }

        Value * fieldResultValue = fieldResult[currentFieldError / sizeTyBitWidth];
        Value * fieldMask = b->getSize(1ULL << (currentFieldError & (sizeTyBitWidth - 1)));
        Value * cond = b->CreateICmpNE(b->CreateAnd(fieldResultValue, fieldMask), sz_ZERO);

        b->CreateUnlikelyCondBr(cond, reportError, nextCheck);

        b->SetInsertPoint(reportError);
        Function * reportDuplicateKeyFn = m->getFunction("report_duplicate_key");

        assert (reportDuplicateKeyFn->getFunctionType()->getNumParams() == reportArgs.size());
        Value * const sz_Offset = b->getSize(sizeof(HashArrayMappedTrie) * currentFieldError);
        reportArgs[2] = b->CreatePointerCast(b->CreateGEP(voidPtrTy, baseHashTableObjects, sz_Offset), voidPtrTy);


        if (currentFieldError < mStride) {
            Value * const hashIndex = b->CreateAdd(hashCodesProcessedPhi, b->getSize(currentFieldError));
            Value * const startCoordIndex = b->CreateMul(hashIndex, sz_TWO);
            Value * const startPtr = b->CreateGEP(coordinatesTy, baseCoordinatePtr, startCoordIndex);
            Value * const startPosition = b->CreateLoad(coordinatesTy, startPtr);
            Value * const endCoordIndex = b->CreateAdd(startCoordIndex, sz_ONE);
            Value * const endPtr = b->CreateGEP(coordinatesTy, baseCoordinatePtr, endCoordIndex);
            Value * const endPosition = b->CreateLoad(coordinatesTy, endPtr);
            reportArgs[3] = b->CreateGEP(int8Ty, baseInputPtr, startPosition);
            reportArgs[4] = b->CreateGEP(int8Ty, baseInputPtr, endPosition);
        } else {
            // TODO: need to construct the key string from original data
            reportArgs[3] = ConstantPointerNull::getNullValue(b->getInt8PtrTy());
            reportArgs[4] = ConstantPointerNull::getNullValue(b->getInt8PtrTy());
        }
        ConstantInt * const keyNum = b->getSize(currentFieldError);
        reportArgs[5] = keyNum;
        reportArgs[6] = hashCodesProcessedPhi;
        priorLineNumIndex[1] = keyNum;
        reportArgs[7] = b->CreateLoad(sizeTy, b->CreateGEP(priorLineNumArrayTy, priorLineNumArray, priorLineNumIndex));
        Value * const continueToNext = b->CreateCall(reportDuplicateKeyFn->getFunctionType(), reportDuplicateKeyFn, reportArgs);
        b->CreateCondBr(continueToNext, nextCheck, fatalError);

        b->SetInsertPoint(nextCheck);
    }

    b->SetInsertPoint(fatalError);
    b->setFatalTerminationSignal();
    b->CreateBr(loopEnd);

    b->SetInsertPoint(acceptedAllDuplicates);
    Value * const nextHashCodesProcessed = b->CreateAdd(hashCodesProcessedPhi, sz_STRIDE);
    hashCodesProcessedPhi->addIncoming(nextHashCodesProcessed, acceptedAllDuplicates);
    Value * const notDone = b->CreateICmpULT(nextHashCodesProcessed, totalHashCodeProcessed);
    b->CreateCondBr(notDone, loopStart, loopEnd);

    b->SetInsertPoint(loopEnd);
    PHINode * const finalPos = b->CreatePHI(sizeTy, 3);
    finalPos->addIncoming(initial, entry);
    finalPos->addIncoming(endPosition, fatalError);
    finalPos->addIncoming(endPosition, acceptedAllDuplicates);
    b->setProcessedItemCount("InputStream", finalPos);
}

void CheckKeyUniqueness::generateInitializeMethod(BuilderRef b) {
    Function * constructNTriesFn = b->getModule()->getFunction("construct_n_tries");
    size_t compositeKeys = 0;
    for (const CSVSchemaColumnRule & column : mSchema.Column) {
        for (const CSVSchemaCompositeKey & key : column.CompositeKey) {
            if (key.Fields.size() > 1) {
                compositeKeys++;
            }
        }
    }
    FunctionType * fty = constructNTriesFn->getFunctionType();
    Value * const hashTableObjects = b->CreateCall(fty, constructNTriesFn, { b->getSize(mStride + compositeKeys)});
    b->setScalarField("hashTableObjects", b->CreatePointerCast(hashTableObjects, b->getVoidPtrTy()));
}

void CheckKeyUniqueness::generateFinalizeMethod(BuilderRef b) {
    Function * deconstructNTriesFn = b->getModule()->getFunction("deconstruct_n_tries");
    size_t compositeKeys = 0;
    for (const CSVSchemaColumnRule & column : mSchema.Column) {
        for (const CSVSchemaCompositeKey & key : column.CompositeKey) {
            if (key.Fields.size() > 1) {
                compositeKeys++;
            }
        }
    }
    FunctionType * fty = deconstructNTriesFn->getFunctionType();
    FixedArray<Value *, 2> args;
    args[0] = b->CreatePointerCast(b->getScalarField("hashTableObjects"), b->getVoidPtrTy());
    args[1] = b->getSize(mStride + compositeKeys);
    b->CreateCall(fty, deconstructNTriesFn, args);
}

}
