#include "pipeline_compiler.hpp"

#include <llvm/Support/ErrorHandling.h>

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief beginKernel
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::setActiveKernel(BuilderRef b, const unsigned index) {
    assert (index >= FirstKernel && index <= LastKernel);
    mKernelIndex = index;
    mKernel = getKernel(index);
    mKernelHandle = nullptr;
    if (LLVM_LIKELY(mKernel->isStateful())) {
        Value * handle = b->getScalarField(makeKernelName(index));
        if (LLVM_UNLIKELY(mKernel->externallyInitialized())) {
            handle = b->CreatePointerCast(handle, mKernel->getSharedStateType()->getPointerTo());
        }
        mKernelHandle = handle;
    }
    if (LLVM_UNLIKELY(mCheckAssertions)) {
        SmallVector<char, 256> tmp;
        raw_svector_ostream out(tmp);
        out << mKernelIndex << "." << mKernel->getName();
        mKernelAssertionName = b->GetString(out.str());
    }
}


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief zeroInputAfterFinalItemCount
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::zeroInputAfterFinalItemCount(BuilderRef b, const Vec<Value *> & accessibleItems, Vec<Value *> & inputBaseAddresses) {
    #ifndef DISABLE_INPUT_ZEROING

    Constant * const sz_ZERO = b->getSize(0);
    Constant * const sz_ONE = b->getSize(1);

    for (const auto e : make_iterator_range(in_edges(mKernelIndex, mBufferGraph))) {

        const auto streamSet = source(e, mBufferGraph);

        const BufferNode & bn = mBufferGraph[streamSet];
        const StreamSetBuffer * const buffer = bn.Buffer;
        const BufferRateData & port = mBufferGraph[e];
        const auto inputPort = port.Port;
        assert (inputPort.Type == PortType::Input);
        const Binding & input = port.Binding;
        const ProcessingRate & rate = input.getRate();

        inputBaseAddresses[inputPort.Number] = mInputEpoch[inputPort.Number];

        if (LLVM_LIKELY(rate.isFixed())) {

            // TODO: support popcount/partialsum

            // TODO: for fixed rate inputs, so long as the actual number of items is aa even
            // multiple of the stride*rate, we can ignore masking.

            // TODO: if we can prove that this will be the last kernel invocation that will ever touch this stream)
            // and is not an input to the pipeline (which we cannot prove will have space after the last item), we
            // can avoid copying the buffer and instead just mask out the surpressed items.


            // create a stack entry for this buffer at the start of the pipeline
            PointerType * const int8PtrTy = b->getInt8PtrTy();
            AllocaInst * const bufferStorage = b->CreateAllocaAtEntryPoint(int8PtrTy);
            Instruction * const nextNode = bufferStorage->getNextNode(); assert (nextNode);
            new StoreInst(ConstantPointerNull::get(int8PtrTy), bufferStorage, nextNode);
            mTruncatedInputBuffer.push_back(bufferStorage);

            const auto prefix = makeBufferName(mKernelIndex, inputPort);
            const auto itemWidth = getItemWidth(buffer->getBaseType());

            if (LLVM_UNLIKELY(itemWidth == 0)) {
                continue;
            }

            Constant * const ITEM_WIDTH = b->getSize(itemWidth);

            PointerType * const bufferType = buffer->getPointerType();

            BasicBlock * const maskedInput = b->CreateBasicBlock(prefix + "_maskInput", mKernelLoopCall);
            BasicBlock * const selectedInput = b->CreateBasicBlock(prefix + "_selectInput", mKernelLoopCall);

            Value * selected = accessibleItems[inputPort.Number];
            Value * totalNumOfItems = mAccessibleInputItems[inputPort.Number];
            Value * const tooMany = b->CreateICmpULT(selected, totalNumOfItems);
            Value * computeMask = tooMany;
            if (mIsInputZeroExtended[inputPort.Number]) {
                computeMask = b->CreateAnd(tooMany, b->CreateNot(mIsInputZeroExtended[inputPort.Number]));
            }

            BasicBlock * const entryBlock = b->GetInsertBlock();
            b->CreateUnlikelyCondBr(computeMask, maskedInput, selectedInput);

            b->SetInsertPoint(maskedInput);

            // if this is a deferred fixed rate stream, we cannot be sure how many
            // blocks will have to be provided to the kernel in order to mask out
            // the truncated input stream.


            // Generate a name to describe this masking function.
            SmallVector<char, 32> tmp;
            raw_svector_ostream name(tmp);

            name << "__maskInput" << itemWidth;

            Module * const m = b->getModule();

            Function * maskInput = m->getFunction(name.str());

            if (maskInput == nullptr) {

                IntegerType * const sizeTy = b->getSizeTy();

                const auto blockWidth = b->getBitBlockWidth();
                const auto log2BlockWidth = floor_log2(blockWidth);
                Constant * const BLOCK_MASK = b->getSize(blockWidth - 1);
                Constant * const LOG_2_BLOCK_WIDTH = b->getSize(log2BlockWidth);

                const auto ip = b->saveIP();

                FixedArray<Type *, 7> params;
                params[0] = int8PtrTy; // input buffer
                params[1] = sizeTy; // bytes per stride
                params[2] = sizeTy; // processed
                params[3] = sizeTy; // processed (deferred)
                params[4] = sizeTy; // accessible
                params[5] = sizeTy; // numOfStreams
                params[6] = int8PtrTy->getPointerTo(); // masked buffer storage ptr

                LLVMContext & C = m->getContext();

                FunctionType * const funcTy = FunctionType::get(int8PtrTy, params, false);
                maskInput = Function::Create(funcTy, Function::InternalLinkage, name.str(), m);
                b->SetInsertPoint(BasicBlock::Create(C, "entry", maskInput));

                auto arg = maskInput->arg_begin();
                auto nextArg = [&]() {
                    assert (arg != maskInput->arg_end());
                    Value * const v = &*arg;
                    std::advance(arg, 1);
                    return v;
                };


                DataLayout DL(b->getModule());
                Type * const intPtrTy = DL.getIntPtrType(int8PtrTy);

                Value * const inputBuffer = nextArg();
                inputBuffer->setName("inputBuffer");
                Value * const padding = nextArg();
                padding->setName("padding");
                Value * const processed = nextArg();
                processed->setName("processed");
                Value * const consumed = nextArg();
                consumed->setName("consumed");
                Value * const accessible = nextArg();
                accessible->setName("accessible");
                Value * const numOfStreams = nextArg();
                numOfStreams->setName("numOfStreams");
                Value * const bufferStorage = nextArg();
                bufferStorage->setName("bufferStorage");
                assert (arg == maskInput->arg_end());


                Type * const singleElementStreamSetTy = ArrayType::get(FixedVectorType::get(IntegerType::get(C, itemWidth), static_cast<unsigned>(0)), 1);
                ExternalBuffer tmp(0, b, singleElementStreamSetTy, true, 0);
                PointerType * const bufferPtrTy = tmp.getPointerType();

                Value * const inputAddress = b->CreatePointerCast(inputBuffer, bufferPtrTy);
                Value * const initial = b->CreateMul(b->CreateLShr(consumed, LOG_2_BLOCK_WIDTH), numOfStreams);
                Value * const initialPtr = tmp.getStreamBlockPtr(b, inputAddress, sz_ZERO, initial);
                Value * const initialPtrInt = b->CreatePtrToInt(initialPtr, intPtrTy);

                Value * const total = b->CreateLShr(b->CreateAdd(processed, accessible), LOG_2_BLOCK_WIDTH);

                if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
                    b->CreateAssert(b->CreateICmpUGE(padding, sz_ONE), "maskInput padding cannot be zero");
                }

                Value * const required = b->CreateMul(b->CreateAdd(total, padding), numOfStreams);
                Value * const requiredPtr = tmp.getStreamBlockPtr(b, inputAddress, sz_ZERO, required);
                Value * const requiredPtrInt = b->CreatePtrToInt(requiredPtr, intPtrTy);
                Value * const requiredBytes = b->CreateSub(requiredPtrInt, initialPtrInt);

                const auto blockSize = b->getBitBlockWidth() / 8;

                Value * const maskedBuffer = b->CreateAlignedMalloc(requiredBytes, blockSize);
                b->CreateMemZero(maskedBuffer, requiredBytes, blockSize);
                // TODO: look into checking whether the OS supports aligned realloc.
                b->CreateFree(b->CreateLoad(bufferStorage));
                b->CreateStore(maskedBuffer, bufferStorage);
                Value * const mallocedAddress = b->CreatePointerCast(maskedBuffer, bufferPtrTy);
                Value * const fullCopyEnd = b->CreateMul(total, numOfStreams);
                Value * const fullCopyEndPtr = tmp.getStreamBlockPtr(b, inputAddress, sz_ZERO, fullCopyEnd);
                Value * const fullCopyEndPtrInt = b->CreatePtrToInt(fullCopyEndPtr, intPtrTy);
                Value * const fullBytesToCopy = b->CreateSub(fullCopyEndPtrInt, initialPtrInt);

                b->CreateMemCpy(mallocedAddress, initialPtr, fullBytesToCopy, blockSize);

                // Value * const base = b->CreateMul(b->CreateLShr(processed, LOG_2_BLOCK_WIDTH), numOfStreams);
                Value * const outputVBA = tmp.getStreamBlockPtr(b, mallocedAddress, sz_ZERO, b->CreateNeg(initial));
                Value * const maskedAddress = b->CreatePointerCast(outputVBA, bufferPtrTy);
                assert (maskedAddress->getType() == inputAddress->getType());

                Value * packIndex = nullptr;
                Value * maskOffset = b->CreateAnd(accessible, BLOCK_MASK);
                if (itemWidth > 1) {
                    Value * const position = b->CreateMul(maskOffset, ITEM_WIDTH);
                    packIndex = b->CreateLShr(position, LOG_2_BLOCK_WIDTH);
                    maskOffset = b->CreateAnd(position, BLOCK_MASK);
                }
                Value * const mask = b->CreateNot(b->bitblock_mask_from(maskOffset));
                BasicBlock * const loopEntryBlock = b->GetInsertBlock();

                BasicBlock * const maskedInputLoop = BasicBlock::Create(C, "maskInputLoop", maskInput);
                BasicBlock * const maskedInputExit = BasicBlock::Create(C, "maskInputExit", maskInput);
                b->CreateBr(maskedInputLoop);

                b->SetInsertPoint(maskedInputLoop);
                PHINode * const streamIndex = b->CreatePHI(b->getSizeTy(), 2);
                streamIndex->addIncoming(sz_ZERO, loopEntryBlock);

                Value * inputPtr = tmp.getStreamBlockPtr(b, inputAddress, streamIndex, fullCopyEnd);
                Value * outputPtr = tmp.getStreamBlockPtr(b, maskedAddress, streamIndex, fullCopyEnd);
                assert (inputPtr->getType() == outputPtr->getType());
                if (itemWidth > 1) {
                    Value * const partialCopyInputEndPtr = tmp.getStreamPackPtr(b, inputAddress, streamIndex, fullCopyEnd, packIndex);
                    Value * const partialCopyInputEndPtrInt = b->CreatePtrToInt(partialCopyInputEndPtr, intPtrTy);
                    Value * const partialCopyInputStartPtrInt = b->CreatePtrToInt(inputPtr, intPtrTy);
                    Value * const bytesToCopy = b->CreateSub(partialCopyInputEndPtrInt, partialCopyInputStartPtrInt);

                    b->CreateMemCpy(outputPtr, inputPtr, bytesToCopy, blockSize);
                    inputPtr = partialCopyInputEndPtr;
                    Value * const afterCopyOutputPtr = tmp.getStreamPackPtr(b, maskedAddress, streamIndex, fullCopyEnd, packIndex);
                    outputPtr = afterCopyOutputPtr;
                }
                assert (inputPtr->getType() == outputPtr->getType());
                Value * const val = b->CreateBlockAlignedLoad(inputPtr);
                Value * const maskedVal = b->CreateAnd(val, mask);
                b->CreateBlockAlignedStore(maskedVal, outputPtr);

                Value * const nextIndex = b->CreateAdd(streamIndex, sz_ONE);
                Value * const notDone = b->CreateICmpNE(nextIndex, numOfStreams);
                streamIndex->addIncoming(nextIndex, maskedInputLoop);

                b->CreateCondBr(notDone, maskedInputLoop, maskedInputExit);

                b->SetInsertPoint(maskedInputExit);
                b->CreateRet(b->CreatePointerCast(maskedAddress, int8PtrTy));

                b->restoreIP(ip);
            }

            FixedArray<Value *, 7> args;
            args[0] = b->CreatePointerCast(mInputEpoch[inputPort.Number], int8PtrTy);
            args[1] = b->getSize(ceiling(rate.getRate() * Rational{mKernel->getStride(), b->getBitBlockWidth()}));
            args[2] = mAlreadyProcessedPhi[inputPort.Number];
            if (input.isDeferred()) {
                args[3] = mAlreadyProcessedDeferredPhi[inputPort.Number];
            } else {
                args[3] = mAlreadyProcessedPhi[inputPort.Number];
            }

            args[4] = accessibleItems[inputPort.Number];
            args[5] = buffer->getStreamSetCount(b);
            args[6] = bufferStorage;


            #ifdef PRINT_DEBUG_MESSAGES
            debugPrint(b, prefix + " truncating item count from %" PRIu64 " to %" PRIu64,
                      totalNumOfItems, accessibleItems[inputPort.Number]);
            #endif

            Value * const maskedAddress = b->CreatePointerCast(b->CreateCall(maskInput, args), bufferType);
            BasicBlock * const maskedInputLoopExit = b->GetInsertBlock();
            b->CreateBr(selectedInput);

            b->SetInsertPoint(selectedInput);
            PHINode * const phi = b->CreatePHI(bufferType, 2);
            phi->addIncoming(inputBaseAddresses[inputPort.Number], entryBlock);
            phi->addIncoming(maskedAddress, maskedInputLoopExit);
            inputBaseAddresses[inputPort.Number] = phi;

        }
    }
    #endif
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief prepareLocalZeroExtendSpace
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::prepareLocalZeroExtendSpace(BuilderRef b) {
    if (mHasZeroExtendedStream) {
        const auto numOfInputs = getNumOfStreamInputs(mKernelIndex);
        mZeroExtendBufferPhi = nullptr;
        const auto strideSize = mKernel->getStride();
        const auto blockWidth = b->getBitBlockWidth();
        Value * requiredSpace = nullptr;

        Constant * const ZERO = b->getSize(0);
        Constant * const ONE = b->getSize(1);
        Value * const numOfStrides = b->CreateUMax(mNumOfLinearStrides, ONE);

        for (unsigned i = 0; i < numOfInputs; ++i) {
            if (mIsInputZeroExtended[i]) {
                const auto bufferVertex = getInputBufferVertex(i);
                const BufferNode & bn = mBufferGraph[bufferVertex];
                const Binding & input = getInputBinding(i);

                const auto itemWidth = getItemWidth(input.getType());
                Constant * const strideFactor = b->getSize(itemWidth * strideSize / 8);
                Value * requiredBytes = b->CreateMul(numOfStrides, strideFactor); assert (requiredBytes);
                if (bn.LookAhead) {
                    const auto lh = (bn.LookAhead * itemWidth);
                    requiredBytes = b->CreateAdd(requiredBytes, b->getSize(lh));
                }
                if (LLVM_LIKELY(itemWidth < blockWidth)) {
                    Constant * const factor = b->getSize(blockWidth / itemWidth);
                    requiredBytes = b->CreateRoundUp(requiredBytes, factor);
                }
                requiredBytes = b->CreateMul(requiredBytes, bn.Buffer->getStreamSetCount(b));

                const auto fieldWidth = input.getFieldWidth();
                if (fieldWidth < 8) {
                    requiredBytes = b->CreateUDiv(requiredBytes, b->getSize(8 / fieldWidth));
                } else if (fieldWidth > 8) {
                    requiredBytes = b->CreateMul(requiredBytes, b->getSize(fieldWidth / 8));
                }
                requiredBytes = b->CreateSelect(mIsInputZeroExtended[i], requiredBytes, ZERO);
                requiredSpace = b->CreateUMax(requiredSpace, requiredBytes);
            }
        }
        if (requiredSpace) {
            const auto prefix = makeKernelName(mKernelIndex);
            BasicBlock * const entry = b->GetInsertBlock();
            BasicBlock * const expandZeroExtension =
                b->CreateBasicBlock(prefix + "_expandZeroExtensionBuffer", mKernelLoopCall);
            BasicBlock * const executeKernel =
                b->CreateBasicBlock(prefix + "_executeKernelAfterZeroExtension", mKernelLoopCall);
            Value * const currentSpace = b->CreateLoad(mZeroExtendSpace);
            Value * const currentBuffer = b->CreateLoad(mZeroExtendBuffer);
            requiredSpace = b->CreateRoundUp(requiredSpace, b->getSize(b->getCacheAlignment()));

            Value * const largeEnough = b->CreateICmpUGE(currentSpace, requiredSpace);
            b->CreateLikelyCondBr(largeEnough, executeKernel, expandZeroExtension);

            b->SetInsertPoint(expandZeroExtension);
            assert (b->getCacheAlignment() >= (b->getBitBlockWidth() / 8));
            b->CreateFree(currentBuffer);
            Value * const newBuffer = b->CreateCacheAlignedMalloc(requiredSpace);
            b->CreateMemZero(newBuffer, requiredSpace, b->getCacheAlignment());
            b->CreateStore(requiredSpace, mZeroExtendSpace);
            b->CreateStore(newBuffer, mZeroExtendBuffer);
            b->CreateBr(executeKernel);

            b->SetInsertPoint(executeKernel);
            PHINode * const zeroBuffer = b->CreatePHI(b->getVoidPtrTy(), 2);
            zeroBuffer->addIncoming(currentBuffer, entry);
            zeroBuffer->addIncoming(newBuffer, expandZeroExtension);
            mZeroExtendBufferPhi = zeroBuffer;

        }
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief clearUnwrittenOutputData
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::clearUnwrittenOutputData(BuilderRef b) {
    #ifndef DISABLE_OUTPUT_ZEROING
    const auto blockWidth = b->getBitBlockWidth();
    const auto log2BlockWidth = floor_log2(blockWidth);
    Constant * const LOG_2_BLOCK_WIDTH = b->getSize(log2BlockWidth);
    Constant * const ZERO = b->getSize(0);
    Constant * const ONE = b->getSize(1);
    Constant * const BLOCK_MASK = b->getSize(blockWidth - 1);

    const auto numOfOutputs = getNumOfStreamOutputs(mKernelIndex);
    for (unsigned i = 0; i < numOfOutputs; ++i) {
        const StreamSetBuffer * const buffer = getOutputBuffer(i);
        if (LLVM_UNLIKELY(isa<ExternalBuffer>(buffer))) {
            // If this stream is either controlled by this kernel or is an external
            // stream, any clearing of data is the responsibility of the owner.
            // Simply ignore any external buffers for the purpose of zeroing out
            // unnecessary data.
            continue;
        }
        const auto itemWidth = getItemWidth(buffer->getBaseType());

        const auto prefix = makeBufferName(mKernelIndex, StreamSetPort{PortType::Output, i});
        Value * const produced = mFinalProducedPhi[i];
        Value * const blockIndex = b->CreateLShr(produced, LOG_2_BLOCK_WIDTH);
        Constant * const ITEM_WIDTH = b->getSize(itemWidth);
        Value * packIndex = nullptr;
        Value * maskOffset = b->CreateAnd(produced, BLOCK_MASK);

        if (itemWidth > 1) {
            Value * const position = b->CreateMul(maskOffset, ITEM_WIDTH);
            packIndex = b->CreateLShr(position, LOG_2_BLOCK_WIDTH);
            maskOffset = b->CreateAnd(position, BLOCK_MASK);
        }

        Value * const mask = b->CreateNot(b->bitblock_mask_from(maskOffset));
        BasicBlock * const maskLoop = b->CreateBasicBlock(prefix + "_zeroFillLoop", mKernelInsufficientIOExit);
        BasicBlock * const maskExit = b->CreateBasicBlock(prefix + "_zeroFillExit", mKernelInsufficientIOExit);
        Value * const numOfStreams = buffer->getStreamSetCount(b);
        Value * const baseAddress = buffer->getBaseAddress(b);
        #ifdef PRINT_DEBUG_MESSAGES
        Value * const epoch = buffer->getStreamPackPtr(b, baseAddress, ZERO, ZERO, ZERO);
        #endif
        BasicBlock * const entry = b->GetInsertBlock();
        b->CreateBr(maskLoop);

        b->SetInsertPoint(maskLoop);
        PHINode * const streamIndex = b->CreatePHI(b->getSizeTy(), 2);
        streamIndex->addIncoming(ZERO, entry);
        Value * ptr = nullptr;
        if (itemWidth > 1) {
            ptr = buffer->getStreamPackPtr(b, baseAddress, streamIndex, blockIndex, packIndex);
        } else {
            ptr = buffer->getStreamBlockPtr(b, baseAddress, streamIndex, blockIndex);
        }
        Value * const value = b->CreateBlockAlignedLoad(ptr);
        Value * const maskedValue = b->CreateAnd(value, mask);
        b->CreateBlockAlignedStore(maskedValue, ptr);

        DataLayout DL(b->getModule());
        Type * const intPtrTy = DL.getIntPtrType(ptr->getType());
        #ifdef PRINT_DEBUG_MESSAGES
        Value * const epochInt = b->CreatePtrToInt(epoch, intPtrTy);
        #endif
        if (itemWidth > 1) {
            // Since packs are laid out sequentially in memory, it will hopefully be cheaper to zero them out here
            // because they may be within the same cache line.
            Value * const nextPackIndex = b->CreateAdd(packIndex, ONE);
            Value * const start = buffer->getStreamPackPtr(b, baseAddress, streamIndex, blockIndex, nextPackIndex);
            Value * const startInt = b->CreatePtrToInt(start, intPtrTy);
            Value * const end = buffer->getStreamPackPtr(b, baseAddress, streamIndex, blockIndex, ITEM_WIDTH);
            Value * const endInt = b->CreatePtrToInt(end, intPtrTy);
            Value * const remainingPackBytes = b->CreateSub(endInt, startInt);
            #ifdef PRINT_DEBUG_MESSAGES
            debugPrint(b, prefix + "_zeroFill_packStart = %" PRIu64, b->CreateSub(startInt, epochInt));
            debugPrint(b, prefix + "_zeroFill_remainingPackBytes = %" PRIu64, remainingPackBytes);
            #endif
            b->CreateMemZero(start, remainingPackBytes, blockWidth / 8);
        }
        BasicBlock * const maskLoopExit = b->GetInsertBlock();
        Value * const nextStreamIndex = b->CreateAdd(streamIndex, ONE);
        streamIndex->addIncoming(nextStreamIndex, maskLoopExit);
        Value * const notDone = b->CreateICmpNE(nextStreamIndex, numOfStreams);
        b->CreateCondBr(notDone, maskLoop, maskExit);

        b->SetInsertPoint(maskExit);
        // Zero out any blocks we could potentially touch

        Rational strideLength{0};
        const auto bufferVertex = getOutputBufferVertex(i);
        for (const auto e : make_iterator_range(out_edges(bufferVertex, mBufferGraph))) {
            const BufferRateData & rd = mBufferGraph[e];
            const Binding & input = rd.Binding;

            Rational R{rd.Maximum};
            if (LLVM_UNLIKELY(input.hasLookahead())) {
                R += input.getLookahead();
            }
            strideLength = std::max(strideLength, R);
        }

        const auto blocksToZero = ceiling(strideLength * Rational{1, blockWidth});

        if (blocksToZero > 1) {
            Value * const nextBlockIndex = b->CreateAdd(blockIndex, ONE);
            Value * const nextOffset = buffer->modByCapacity(b, nextBlockIndex);
            Value * const startPtr = buffer->StreamSetBuffer::getStreamBlockPtr(b, baseAddress, ZERO, nextOffset);
            Value * const startPtrInt = b->CreatePtrToInt(startPtr, intPtrTy);
            Constant * const BLOCKS_TO_ZERO = b->getSize(blocksToZero);
            Value * const endOffset = b->CreateRoundUp(nextOffset, BLOCKS_TO_ZERO);
            Value * const endPtr = buffer->StreamSetBuffer::getStreamBlockPtr(b, baseAddress, ZERO, endOffset);
            Value * const endPtrInt = b->CreatePtrToInt(endPtr, intPtrTy);
            Value * const remainingBytes = b->CreateSub(endPtrInt, startPtrInt);
            #ifdef PRINT_DEBUG_MESSAGES
            debugPrint(b, prefix + "_zeroFill_bufferStart = %" PRIu64, b->CreateSub(startPtrInt, epochInt));
            debugPrint(b, prefix + "_zeroFill_remainingBufferBytes = %" PRIu64, remainingBytes);
            #endif
            b->CreateMemZero(startPtr, remainingBytes, blockWidth / 8);
        }
    }
    #endif
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief computeFullyProcessedItemCounts
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::computeFullyProcessedItemCounts(BuilderRef b) {
    const auto numOfInputs = getNumOfStreamInputs(mKernelIndex);
    for (unsigned i = 0; i < numOfInputs; ++i) {
        const Binding & input = getInputBinding(i);
        Value * processed = nullptr;
        if (mUpdatedProcessedDeferredPhi[i]) {
            processed = mUpdatedProcessedDeferredPhi[i];
        } else {
            processed = mUpdatedProcessedPhi[i];
        }
        processed = truncateBlockSize(b, input, processed);
        mFullyProcessedItemCount[i] = processed;
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief computeFullyProducedItemCounts
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::computeFullyProducedItemCounts(BuilderRef b) {

    // TODO: we only need to consider the blocksize attribute if it's possible this
    // stream could be read before being fully written. This might occur if one of
    // it's consumers has a non-Fixed rate that does not have a matching BlockSize
    // attribute.

    const auto numOfOutputs = getNumOfStreamOutputs(mKernelIndex);
    for (unsigned i = 0; i < numOfOutputs; ++i) {
        const Binding & output = getOutputBinding(i);
        Value * produced = mUpdatedProducedPhi[i];
        if (LLVM_UNLIKELY(output.hasAttribute(AttrId::Delayed))) {
            const auto & D = output.findAttribute(AttrId::Delayed);
            Value * const delayed = b->CreateSaturatingSub(produced, b->getSize(D.amount()));
            Value * const terminated = b->CreateIsNotNull(mTerminatedAtLoopExitPhi);
            produced = b->CreateSelect(terminated, produced, delayed);
        }
        produced = truncateBlockSize(b, output, produced);
        mFullyProducedItemCount[i]->addIncoming(produced, mKernelLoopExitPhiCatch);
    }
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getTotalItemCount
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getLocallyAvailableItemCount(BuilderRef /* b */, const unsigned inputPort) const {
    const auto bufferVertex = getInputBufferVertex(inputPort);
    return mLocallyAvailableItems[getBufferIndex(bufferVertex)];
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief addLookahead
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::addLookahead(BuilderRef b, const unsigned inputPort, Value * const itemCount) const {
    Constant * const lookAhead = getLookahead(b, inputPort);
    if (LLVM_LIKELY(lookAhead == nullptr)) {
        return itemCount;
    }
    return b->CreateAdd(itemCount, lookAhead);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief subtractLookahead
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::subtractLookahead(BuilderRef b, const unsigned inputPort, Value * const itemCount) {
    Constant * const lookAhead = getLookahead(b, inputPort);
    if (LLVM_LIKELY(lookAhead == nullptr)) {
        return itemCount;
    }
    Value * const closed = isClosed(b, inputPort);
    if (LLVM_UNLIKELY(mCheckAssertions)) {
        const Binding & binding = getInputBinding(inputPort);
        b->CreateAssert(b->CreateOr(b->CreateICmpUGE(itemCount, lookAhead), closed),
                        "%s.%s: look ahead exceeds item count",
                        mKernelAssertionName,
                        b->GetString(binding.getName()));
    }
    Value * const reducedItemCount = b->CreateSub(itemCount, lookAhead);
    return b->CreateSelect(closed, itemCount, reducedItemCount);
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getLookahead
 ** ------------------------------------------------------------------------------------------------------------- */
Constant * PipelineCompiler::getLookahead(BuilderRef b, const unsigned inputPort) const {
    const Binding & input = getInputBinding(inputPort);
    if (LLVM_UNLIKELY(input.hasLookahead())) {
        return b->getSize(input.getLookahead());
    }
    return nullptr;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief maskBlockSize
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::truncateBlockSize(BuilderRef b, const Binding & binding, Value * itemCount) const {
    // TODO: if we determine all of the inputs of a stream have a blocksize attribute, or the output has one,
    // we can skip masking it on input



    if (LLVM_UNLIKELY(binding.hasAttribute(AttrId::BlockSize))) {
        // If the input rate has a block size attribute then --- for the purpose of determining how many
        // items have been consumed --- we consider a stream set to be fully processed when an entire
        // stride has been processed.
        Constant * const BLOCK_WIDTH = b->getSize(b->getBitBlockWidth());
        Value * const maskedItemCount = b->CreateAnd(itemCount, ConstantExpr::getNeg(BLOCK_WIDTH));
        Value * const terminated = hasKernelTerminated(b, mKernelIndex);
        itemCount = b->CreateSelect(terminated, itemCount, maskedItemCount);
    }
    return itemCount;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInitializationFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getKernelInitializeFunction(BuilderRef b) const {    
    Function * const init = mKernel->getInitializeFunction(b);
    assert (!mKernel->hasFamilyName());
    return init;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInitializationThreadLocalFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getKernelInitializeThreadLocalFunction(BuilderRef b) const {
    Function * const init = mKernel->getInitializeThreadLocalFunction(b);
    if (mKernel->hasFamilyName()) {
        return getFamilyFunctionFromKernelState(b, init->getType(), INITIALIZE_THREAD_LOCAL_FUNCTION_POINTER_SUFFIX);
    }
    return init;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getDoSegmentFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getKernelDoSegmentFunction(BuilderRef b) const {
    Function * const doSegment = mKernel->getDoSegmentFunction(b);
    if (mKernel->hasFamilyName()) {
        return getFamilyFunctionFromKernelState(b, doSegment->getType(), DO_SEGMENT_FUNCTION_POINTER_SUFFIX);
    }
    return doSegment;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getInitializationThreadLocalFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getKernelFinalizeThreadLocalFunction(BuilderRef b) const {
    Function * const finalize = mKernel->getFinalizeThreadLocalFunction(b);
    if (mKernel->hasFamilyName()) {
        return getFamilyFunctionFromKernelState(b, finalize->getType(), FINALIZE_THREAD_LOCAL_FUNCTION_POINTER_SUFFIX);
    }
    return finalize;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getFinalizeFunction
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getKernelFinalizeFunction(BuilderRef b) const {
    Function * const term = mKernel->getFinalizeFunction(b);
    if (mKernel->hasFamilyName()) {
        return getFamilyFunctionFromKernelState(b, term->getType(), FINALIZE_FUNCTION_POINTER_SUFFIX);
    }
    return term;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getThreadLocalHandlePtr
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::getThreadLocalHandlePtr(BuilderRef b, const unsigned kernelIndex) const {
    const Kernel * const kernel = getKernel(kernelIndex);
    assert ("getThreadLocalHandlePtr should not have been called" && kernel->hasThreadLocal());
    const auto prefix = makeKernelName(kernelIndex);
    Value * handle = getScalarFieldPtr(b.get(), prefix + KERNEL_THREAD_LOCAL_SUFFIX);
    if (LLVM_UNLIKELY(kernel->externallyInitialized())) {
        PointerType * const localStateTy = kernel->getThreadLocalStateType()->getPointerTo();
        handle = b->CreatePointerCast(handle, localStateTy->getPointerTo());
    }
    return handle;
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief reset
 ** ------------------------------------------------------------------------------------------------------------- */
namespace {
template <typename Vec>
inline void reset(Vec & vec, const size_t n) {
    vec.resize(n);
    std::fill_n(vec.begin(), n, nullptr);
}
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief resetMemoizedFields
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::resetMemoizedFields() {       
    const auto numOfInputs = in_degree(mKernelIndex, mBufferGraph);
    reset(mIsInputZeroExtended, numOfInputs);
    reset(mInitiallyProcessedItemCount, numOfInputs);
    reset(mInitiallyProcessedDeferredItemCount, numOfInputs);
    reset(mAlreadyProcessedPhi, numOfInputs);
    reset(mAlreadyProcessedDeferredPhi, numOfInputs);
    reset(mInputEpoch, numOfInputs);
    reset(mInputEpochPhi, numOfInputs);
    reset(mFirstInputStrideLength, numOfInputs);
    reset(mAccessibleInputItems, numOfInputs);
    reset(mLinearInputItemsPhi, numOfInputs);
    reset(mReturnedProcessedItemCountPtr, numOfInputs);
    reset(mProcessedItemCount, numOfInputs);
    reset(mProcessedDeferredItemCount, numOfInputs);
    reset(mFinalProcessedPhi, numOfInputs);
    reset(mUpdatedProcessedPhi, numOfInputs);
    reset(mUpdatedProcessedDeferredPhi, numOfInputs);
    reset(mFullyProcessedItemCount, numOfInputs);
    const auto numOfOutputs = out_degree(mKernelIndex, mBufferGraph);
    reset(mInitiallyProducedItemCount, numOfOutputs);
    reset(mInitiallyProducedDeferredItemCount, numOfOutputs);
    reset(mAlreadyProducedPhi, numOfOutputs);
    reset(mAlreadyProducedDeferredPhi, numOfOutputs);
    reset(mFirstOutputStrideLength, numOfOutputs);
    reset(mWritableOutputItems, numOfOutputs);
    reset(mConsumedItemCount, numOfOutputs);
    reset(mLinearOutputItemsPhi, numOfOutputs);    
    reset(mReturnedOutputVirtualBaseAddressPtr, numOfOutputs);
    reset(mReturnedProducedItemCountPtr, numOfOutputs);
    reset(mProducedItemCount, numOfOutputs);
    reset(mProducedDeferredItemCount, numOfOutputs);
    reset(mFinalProducedPhi, numOfOutputs);
    reset(mUpdatedProducedPhi, numOfOutputs);
    reset(mUpdatedProducedDeferredPhi, numOfOutputs);
    reset(mFullyProducedItemCount, numOfOutputs);
    mNumOfAddressableItemCount = 0;
    mNumOfVirtualBaseAddresses = 0;
    mHasClosedInputStream = nullptr;
}

}
