#include "csv_error_identifier.h"
#include <toolchain/toolchain.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/core/streamset.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include "csv_validator_toolchain.h"
#include <boost/intrusive/detail/math.hpp>

using boost::intrusive::detail::floor_log2;
using namespace llvm;

extern "C" void csv_error_identifier_callback(char * fileName, const size_t fieldNum, const size_t recordNum, const size_t bytePos, char * start, char * end) {
    foundError = true;
    SmallVector<char, 1024> tmp;
    raw_svector_ostream out(tmp);
    out << "Error found in " << fileName << ": Field " << fieldNum << " of Record " << recordNum
     //   << '\n'
     //   << "start: " << bytePos << " length: " << (end - start)
        << "\n\n";
    for (auto c = start; c < end; ++c) {
        out << *c;
    }
    out << " does not match supplied rule.\n";
    // TODO: this needs to report more information as to what field/rule was invalid
    // TODO: this cannot differntiate between erroneous line ends
    errs() << out.str();
}

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
CSVErrorIdentifier::CSVErrorIdentifier(BuilderRef b, StreamSet * const errorStream, StreamSet * const allSeparators, StreamSet * const ByteStream, Scalar * const fileName, Scalar * const fieldsPerRecord)
: MultiBlockKernel(b, "CSVErrorIdentifier" + std::to_string(codegen::SegmentSize),
// inputs
{Binding{"errorStream", errorStream}
,Binding{"allSeparators", allSeparators}
,Binding{"InputStream", ByteStream, FixedRate(), { Deferred() }}},
// outputs
{},
// input scalars
{Binding{"fileName", fileName}
,Binding{"fieldsPerRecord", fieldsPerRecord}},
// output scalars
{},
// kernel state
{InternalScalar{b->getSizeTy(), "allSeparatorsObserved"}}) {
    addAttribute(SideEffecting());
    addAttribute(MayFatallyTerminate());
    // TODO: have this return an output scalar for the retval code of the pipeline
    assert ((codegen::SegmentSize % b->getBitBlockWidth()) == 0);
    assert ((b->getBitBlockWidth() % (sizeof(size_t) * 8)) == 0);
    setStride(codegen::SegmentSize);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief linkExternalMethods
 ** ------------------------------------------------------------------------------------------------------------- */
void CSVErrorIdentifier::linkExternalMethods(BuilderRef b) {
    b->LinkFunction("csv_error_identifier_callback", csv_error_identifier_callback);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief generateMultiBlockLogic
 ** ------------------------------------------------------------------------------------------------------------- */
void CSVErrorIdentifier::generateMultiBlockLogic(BuilderRef b, Value * const numOfStrides) {

    BasicBlock * const entryBlock = b->GetInsertBlock();

    BasicBlock * const fullStrideStart = b->CreateBasicBlock("strideLoopStart");

    BasicBlock * const errorOrFinalPartialStrideStart = b->CreateBasicBlock("finalPartialStrideStart");

    BasicBlock * const errorOrFinalPartialStrideStartLoopStart = b->CreateBasicBlock("finalPartialStrideStartLoopStart");

    BasicBlock * const errorOrFinalPartialStrideStartLoopExit = b->CreateBasicBlock("finalPartialStrideStartLoopExit");

    BasicBlock * const findErrorIndexFound = b->CreateBasicBlock("findErrorIndexFound");

    BasicBlock * const sumSeparationsUpToEndLoop = b->CreateBasicBlock("sumSeparationsUpToEndLoop");

    BasicBlock * const sumSeparationsUpToEndLoopExit = b->CreateBasicBlock("sumSeparationsUpToEndLoopExit");

    BasicBlock * const noErrorFoundFull = b->CreateBasicBlock("noErrorFoundFull");

    BasicBlock * const foundLastSeparator = b->CreateBasicBlock("foundLastSeparator");

    BasicBlock * const checkForNextFullStride = b->CreateBasicBlock("checkForNextFullStride");

    BasicBlock * const exit = b->CreateBasicBlock("exit");

    Value * const byteStreamProcessed = b->getProcessedItemCount("InputStream");

    const auto blockWidth = b->getBitBlockWidth();
    const auto blocksPerSegment = (codegen::SegmentSize / blockWidth);

    ConstantInt * const i32_ZERO = b->getInt32(0);
    ConstantInt * const sz_ZERO = b->getSize(0);
    ConstantInt * const sz_ONE = b->getSize(1);
    ConstantInt * const sz_BITBLOCKWIDTH = b->getSize(blockWidth);
    IntegerType * const sizeTy = b->getSizeTy();
    const auto sizeWidth = sizeTy->getBitWidth();
    ConstantInt * const sz_SIZEWIDTH = b->getSize(sizeWidth);
    assert (blockWidth % sizeWidth == 0);

    const auto partialSumFieldCount = (blockWidth / sizeWidth);
    FixedVectorType * const sizeVecTy = FixedVectorType::get(sizeTy, partialSumFieldCount);

    Value * const allSeparatorsObserved = b->getScalarField("allSeparatorsObserved");

    Value * const initialLastSeparator = b->CreateUDiv(byteStreamProcessed, sz_BITBLOCKWIDTH);

    b->CreateUnlikelyCondBr(b->isFinal(), errorOrFinalPartialStrideStart, fullStrideStart);

    b->SetInsertPoint(fullStrideStart);
    PHINode * const currentStrideNumPhi = b->CreatePHI(sizeTy, 2);
    currentStrideNumPhi->addIncoming(sz_ZERO, entryBlock);
    PHINode * const allSeparatorsObservedPhi = b->CreatePHI(sizeTy, 2);
    allSeparatorsObservedPhi->addIncoming(allSeparatorsObserved, entryBlock);
    PHINode * const lastSeparatorObservedIndexPhi = b->CreatePHI(sizeTy, 2);
    lastSeparatorObservedIndexPhi->addIncoming(initialLastSeparator, entryBlock);

    // this relies on the pipeline guaranteeing there is enough space to always process a full segment of data
    // and zero-ing out any streams past their EOF position
    Value * anyError = nullptr;
    for (unsigned i = 0; i < blocksPerSegment; ++i) {
        Value * const error = b->loadInputStreamBlock("errorStream", i32_ZERO, b->getInt32(i));
        if (anyError) {
            anyError = b->CreateOr(anyError, error);
        } else {
            anyError = error;
        }
    }

    anyError = b->bitblock_any(anyError);
    b->CreateUnlikelyCondBr(anyError, errorOrFinalPartialStrideStart, noErrorFoundFull);

    // -------------------------------------
    // PARTIAL SEGMENT OR ERROR CASE
    // -------------------------------------

    b->SetInsertPoint(errorOrFinalPartialStrideStart);
    Value * const errorStreamLength = b->getAccessibleItemCount("errorStream");
    Value * const numOfBlocks = b->CreateUDiv(errorStreamLength, sz_BITBLOCKWIDTH);
    b->CreateBr(errorOrFinalPartialStrideStartLoopStart);

    b->SetInsertPoint(errorOrFinalPartialStrideStartLoopStart);
    PHINode * const strideIndexPhi = b->CreatePHI(sizeTy, 3);
    strideIndexPhi->addIncoming(sz_ZERO, errorOrFinalPartialStrideStart);
    Value * const potentialErrorBlock = b->loadInputStreamBlock("errorStream", i32_ZERO, strideIndexPhi);
    // Since we can enter this loop in the final partial segment case, check again if this has an error
    // even if we've already proven one exists.
    Value * const hasError = b->bitblock_any(potentialErrorBlock);
    Value * const nextStrideIndex = b->CreateAdd(strideIndexPhi, sz_ONE);
    Value * const noMore = b->CreateICmpEQ(nextStrideIndex, numOfBlocks);
    strideIndexPhi->addIncoming(nextStrideIndex, errorOrFinalPartialStrideStartLoopStart);
    b->CreateCondBr(b->CreateOr(noMore, hasError), errorOrFinalPartialStrideStartLoopExit, errorOrFinalPartialStrideStartLoopStart);

    b->SetInsertPoint(errorOrFinalPartialStrideStartLoopExit);
    // If we haven't found an error, we no longer care about the separators. Just exit.
    b->CreateCondBr(hasError, findErrorIndexFound, exit);


    b->SetInsertPoint(findErrorIndexFound);
    Value * const noBlocksBeforeError = b->CreateICmpEQ(strideIndexPhi, sz_ZERO);
    b->CreateCondBr(noBlocksBeforeError, sumSeparationsUpToEndLoopExit, sumSeparationsUpToEndLoop);

    // mask and count the number of separaters prior to the error mark

    b->SetInsertPoint(sumSeparationsUpToEndLoop);
    PHINode * preErrorStrideIndexPhi = b->CreatePHI(sizeTy, 2);
    preErrorStrideIndexPhi->addIncoming(sz_ZERO, findErrorIndexFound);

    PHINode * lastSeparatorBeforeErrorStrideIndexPhi = b->CreatePHI(sizeTy, 2);
    lastSeparatorBeforeErrorStrideIndexPhi->addIncoming(sz_ZERO, findErrorIndexFound);

    PHINode * separatorPartialSumPhi = b->CreatePHI(sizeVecTy, 2);
    separatorPartialSumPhi->addIncoming(ConstantVector::getNullValue(sizeVecTy), findErrorIndexFound);

    Value * allSeparators = b->loadInputStreamBlock("allSeparators", i32_ZERO, preErrorStrideIndexPhi);
    Value * lastSeparatorBeforeErrorStrideIndex = b->CreateSelect(b->bitblock_any(allSeparators), preErrorStrideIndexPhi, lastSeparatorBeforeErrorStrideIndexPhi);

    Value * const separatorPartialSum = b->simd_popcount(sizeWidth, allSeparators);
    Value * const nextSeparatorPartialSum = b->CreateAdd(separatorPartialSum, separatorPartialSumPhi);

    Value * const nextSumUpToEndIndex = b->CreateAdd(preErrorStrideIndexPhi, sz_ONE);

    preErrorStrideIndexPhi->addIncoming(nextSumUpToEndIndex, sumSeparationsUpToEndLoop);
    lastSeparatorBeforeErrorStrideIndexPhi->addIncoming(lastSeparatorBeforeErrorStrideIndex, sumSeparationsUpToEndLoop);
    separatorPartialSumPhi->addIncoming(nextSeparatorPartialSum, sumSeparationsUpToEndLoop);

    Value * const hasMore = b->CreateICmpNE(nextSumUpToEndIndex, strideIndexPhi);
    b->CreateCondBr(hasMore, sumSeparationsUpToEndLoop, sumSeparationsUpToEndLoopExit);


    b->SetInsertPoint(sumSeparationsUpToEndLoopExit);
    PHINode * const updatedSeparatorPartialSumPhi = b->CreatePHI(sizeVecTy, 2);
    updatedSeparatorPartialSumPhi->addIncoming(ConstantVector::getNullValue(sizeVecTy), findErrorIndexFound);
    updatedSeparatorPartialSumPhi->addIncoming(nextSeparatorPartialSum, sumSeparationsUpToEndLoop);

    PHINode * const updatedLastSeparatorBeforeErrorPhi = b->CreatePHI(sizeTy, 2);
    updatedLastSeparatorBeforeErrorPhi->addIncoming(sz_ZERO, findErrorIndexFound);
    updatedLastSeparatorBeforeErrorPhi->addIncoming(lastSeparatorBeforeErrorStrideIndex, sumSeparationsUpToEndLoop);

    Value * const errorVec = b->CreateBitCast(potentialErrorBlock, sizeVecTy);
    Value * firstErrorWordIndex = b->getSize(partialSumFieldCount - 1);
    for (unsigned i = partialSumFieldCount; i > 0; --i) {
        Value * idx = b->getSize(i - 1);
        Value * elem = b->CreateExtractElement(errorVec, idx);
        Value * anySet = b->CreateICmpNE(elem, sz_ZERO);
        firstErrorWordIndex = b->CreateSelect(anySet, idx, firstErrorWordIndex);
    }

    FixedArray<Value *, 6> callbackArgs;
    // char * fileName, const size_t fieldNum, char * line_start, char * line_end

    callbackArgs[0] = b->getScalarField("fileName");

    Value * const fieldsPerRecord = b->getScalarField("fieldsPerRecord");

    Value * const firstErrorWord = b->CreateExtractElement(errorVec, firstErrorWordIndex);
    Value * const firstErrorWordPos = b->CreateCountForwardZeroes(firstErrorWord, "", true);
    Value * firstErrorSeparator = b->CreateAdd(b->CreateMul(firstErrorWordIndex, sz_SIZEWIDTH), firstErrorWordPos);

    Value * const errorMask = b->CreateNot(b->bitblock_mask_from(firstErrorSeparator));    

    firstErrorSeparator = b->CreateAdd(firstErrorSeparator, b->CreateMul(strideIndexPhi, sz_BITBLOCKWIDTH));

    Value * const unmaskedFinalValue = b->loadInputStreamBlock("allSeparators", i32_ZERO, strideIndexPhi);
    Value * const maskedFinalValue = b->CreateAnd(unmaskedFinalValue, errorMask);

    Value * const maskedSepVec = b->simd_popcount(sizeWidth, maskedFinalValue);
    assert (maskedSepVec->getType() == sizeVecTy);
    Value * const reportedSepVec = b->CreateAdd(maskedSepVec, updatedSeparatorPartialSumPhi);
    Value * const partialSum = b->hsimd_partial_sum(sizeWidth, reportedSepVec);
    Value * const numOfSeparators = b->mvmd_extract(sizeWidth, partialSum, partialSumFieldCount - 1);
    Value * const totalNumOfSeparators = b->CreateAdd(allSeparatorsObserved, numOfSeparators);

    callbackArgs[1] = b->CreateAdd(b->CreateURem(totalNumOfSeparators, fieldsPerRecord), sz_ONE);
    callbackArgs[2] = b->CreateAdd(b->CreateUDiv(totalNumOfSeparators, fieldsPerRecord), sz_ONE);

    // We know the final position of the byte input to pass the user is indicated by the position in the error
    // stream but not where it starts. We need to backtrack from the error position to find the start.

    Value * const potentialStartBlock = b->loadInputStreamBlock("allSeparators", i32_ZERO, updatedLastSeparatorBeforeErrorPhi);
    Value * const startPositionInFinalValue = b->CreateOr(b->bitblock_any(maskedFinalValue), b->CreateICmpEQ(updatedLastSeparatorBeforeErrorPhi, sz_ZERO));
    Value * const priorSeparatorBlock = b->CreateBitCast(b->CreateSelect(startPositionInFinalValue, maskedFinalValue, potentialStartBlock), sizeVecTy);
    Value * priorSeparatorWordIndex = sz_ZERO;
    for (unsigned i = 1; i < partialSumFieldCount; ++i) {
        Value * idx = b->getSize(i);
        Value * elem = b->CreateExtractElement(priorSeparatorBlock, idx);
        Value * anySet = b->CreateICmpNE(elem, sz_ZERO);
        priorSeparatorWordIndex = b->CreateSelect(anySet, idx, priorSeparatorWordIndex);
    }

    Value * const priorSeparatorWord = b->CreateExtractElement(priorSeparatorBlock, priorSeparatorWordIndex);

    // If we have a field that spans the entire segment up to the error, priorSeparatorWord will be zero. However,
    // byteStreamProcessed will be set to the last separator in the previous segment so we can reuse that as our
    // start pos.

    Value * priorSeparatorPos = b->CreateMul(b->CreateAdd(priorSeparatorWordIndex, sz_ONE), sz_SIZEWIDTH);
    priorSeparatorPos = b->CreateSub(priorSeparatorPos, b->CreateCountReverseZeroes(priorSeparatorWord, "", false));
    priorSeparatorPos = b->CreateSelect(b->CreateICmpEQ(priorSeparatorWord, sz_ZERO), byteStreamProcessed, priorSeparatorPos);
    Value * const priorSeparatorBlockIdx = b->CreateSelect(startPositionInFinalValue, strideIndexPhi, updatedLastSeparatorBeforeErrorPhi);

    Value * const priorSeparator = b->CreateAdd(b->CreateMul(priorSeparatorBlockIdx, sz_BITBLOCKWIDTH), priorSeparatorPos);

    callbackArgs[3] = priorSeparator;
    callbackArgs[4] = b->getRawInputPointer("InputStream", priorSeparator);
    callbackArgs[5] = b->getRawInputPointer("InputStream", firstErrorSeparator);

    Function * callbackFn = b->getModule()->getFunction("csv_error_identifier_callback"); assert (callbackFn);

    b->CreateCall(callbackFn->getFunctionType(), callbackFn, callbackArgs);

    b->setTerminationSignal();
    b->CreateBr(exit);

    // -------------------------------------
    // FULL SEGMENT WITHOUT ERROR CASE
    // -------------------------------------

    b->SetInsertPoint(noErrorFoundFull);
    const auto m = floor_log2(blocksPerSegment + 1) + 1;
    SmallVector<Value *, 64> adders(m);

    Value * const baseIndex = b->CreateMul(currentStrideNumPhi, b->getSize(blocksPerSegment));
    Value * value = b->loadInputStreamBlock("allSeparators", i32_ZERO, baseIndex);
    adders[0] = value;

    Value * lastSeparatorIndex = b->CreateSelect(b->bitblock_any(value), baseIndex, lastSeparatorObservedIndexPhi);

    // load and half-add the subsequent blocks
    for (unsigned i = 1; i < blocksPerSegment; ++i) {
        Constant * const I = b->getSize(i);
        Value * const idx = b->CreateAdd(baseIndex, I);
        Value * value = b->loadInputStreamBlock("allSeparators", i32_ZERO, idx);

        // is it better to scan to the position we found the last marker in or the defer the sep marker stream?

        lastSeparatorIndex = b->CreateSelect(b->bitblock_any(value), idx, lastSeparatorIndex);

        const auto k = floor_log2(i);
        for (unsigned j = 0; j <= k; ++j) {
            Value * const sum_in = adders[j]; assert (sum_in);
            Value * const sum_out = b->simd_xor(sum_in, value);
            Value * const carry_out = b->simd_and(sum_in, value);
            adders[j] = sum_out;
            value = carry_out;
        }
        const auto l = floor_log2(i + 1);
        if (k < l) {
            adders[l] = value;
        }
    }

    // sum the half adders to compute the number of separators found in this full segment
    Value * partialSumVector = nullptr;
    for (unsigned i = 0; i < m; ++i) {
        Value * const count = b->simd_popcount(sizeWidth, adders[i]);
        if (i == 0) {
            partialSumVector = count;
        } else {
            partialSumVector = b->CreateAdd(partialSumVector, b->CreateShl(count, i));
        }
    }

    Value * const currentPartialSum = b->hsimd_partial_sum(sizeWidth, partialSumVector);
    Value * const currentNumOfSeparators = b->mvmd_extract(sizeWidth, currentPartialSum, partialSumFieldCount - 1);
    Value * const nextNumOfSeparators = b->CreateAdd(allSeparatorsObservedPhi, currentNumOfSeparators);

    // locate the last separator so we can correctly set the deferred position on the byte data
    // NOTE: while it's very likely there is a separator in this block, we do not know if any
    // separators were observed in the segment.

    Value * lastSeparatorBlock = b->loadInputStreamBlock("allSeparators", i32_ZERO, lastSeparatorIndex);
    Value * const lastSeparatorVec = b->CreateBitCast(lastSeparatorBlock, sizeVecTy);

    Value * lastSeparatorVecIndex = sz_ZERO;
    for (unsigned i = 1; i < partialSumFieldCount; ++i) {
        Value * idx = b->getSize(i);
        Value * elem = b->CreateExtractElement(lastSeparatorVec, idx);
        Value * anySet = b->CreateICmpNE(elem, sz_ZERO);
        lastSeparatorVecIndex = b->CreateSelect(anySet, idx, lastSeparatorVecIndex);
    }

    Value * lastSeparatorVecElem = b->CreateExtractElement(lastSeparatorVec, lastSeparatorVecIndex);
    Value * const anySet = b->CreateICmpNE(lastSeparatorVecElem, sz_ZERO);
    b->CreateLikelyCondBr(anySet, foundLastSeparator, checkForNextFullStride);

    b->SetInsertPoint(foundLastSeparator);
    Value * lastSeparator = b->CreateSub(sz_SIZEWIDTH, b->CreateCountReverseZeroes(lastSeparatorVecElem, "", true));
    Value * blockOffset = b->CreateMul(lastSeparatorIndex, sz_BITBLOCKWIDTH);
    Value * wordOffset = b->CreateMul(lastSeparatorVecIndex, sz_SIZEWIDTH);
    lastSeparator = b->CreateAdd(b->CreateOr(blockOffset, wordOffset), lastSeparator);
    b->setProcessedItemCount("InputStream", lastSeparator);
    b->setScalarField("allSeparatorsObserved", nextNumOfSeparators);
    b->CreateBr(checkForNextFullStride);

    b->SetInsertPoint(checkForNextFullStride);
    allSeparatorsObservedPhi->addIncoming(nextNumOfSeparators, checkForNextFullStride);
    Value * const nextStrideNum = b->CreateAdd(currentStrideNumPhi, sz_ONE);
    currentStrideNumPhi->addIncoming(nextStrideNum, checkForNextFullStride);
    Value * const noMoreStrides = b->CreateICmpUGE(nextStrideNum, numOfStrides);
    lastSeparatorObservedIndexPhi->addIncoming(lastSeparatorIndex, checkForNextFullStride);
    b->CreateLikelyCondBr(noMoreStrides, exit, fullStrideStart);

    b->SetInsertPoint(exit);
}

}
