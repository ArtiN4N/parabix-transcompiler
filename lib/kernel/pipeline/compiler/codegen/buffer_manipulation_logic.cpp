#include "../pipeline_compiler.hpp"

using namespace IDISA;

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief allocateLocalZeroExtensionSpace
 ** ------------------------------------------------------------------------------------------------------------- */
Value * PipelineCompiler::allocateLocalZeroExtensionSpace(BuilderRef b, BasicBlock * const insertBefore) const {
    #ifndef DISABLE_ZERO_EXTEND
    const auto strideSize = mKernel->getStride();
    const auto blockWidth = b->getBitBlockWidth();
    Value * requiredSpace = nullptr;

    Constant * const ZERO = b->getSize(0);
    Constant * const ONE = b->getSize(1);
    Value * const numOfStrides = b->CreateUMax(mNumOfLinearStrides, ONE);

    for (const auto e : make_iterator_range(in_edges(mKernelId, mBufferGraph))) {

        const BufferPort & br = mBufferGraph[e];
        if (br.isZeroExtended()) {

            assert (HasZeroExtendedStream);

            const auto streamSet = source(e, mBufferGraph);
            const BufferNode & bn = mBufferGraph[streamSet];
            const Binding & input = br.Binding;

            const auto itemWidth = getItemWidth(input.getType());
            Constant * const strideFactor = b->getSize(itemWidth * strideSize / 8);
            Value * requiredBytes = b->CreateMul(numOfStrides, strideFactor); assert (requiredBytes);
            if (br.LookAhead) {
                const auto lh = (br.LookAhead * itemWidth);
                requiredBytes = b->CreateAdd(requiredBytes, b->getSize(lh));
            }
            if (LLVM_LIKELY(itemWidth < blockWidth)) {
                Constant * const factor = b->getSize(blockWidth / itemWidth);
                assert ((blockWidth % itemWidth) == 0);
                requiredBytes = b->CreateRoundUp(requiredBytes, factor);
            }
            requiredBytes = b->CreateMul(requiredBytes, bn.Buffer->getStreamSetCount(b));

            const auto fieldWidth = input.getFieldWidth();
            if (fieldWidth < 8) {
                requiredBytes = b->CreateUDiv(requiredBytes, b->getSize(8 / fieldWidth));
            } else if (fieldWidth > 8) {
                requiredBytes = b->CreateMul(requiredBytes, b->getSize(fieldWidth / 8));
            }
            const auto name = makeBufferName(mKernelId, br.Port);
            requiredBytes = b->CreateSelect(mIsInputZeroExtended[br.Port], requiredBytes, ZERO, "zeroExtendRequiredBytes");
            requiredSpace = b->CreateUMax(requiredSpace, requiredBytes);
        }
    }
    assert (requiredSpace);    
    const auto prefix = makeKernelName(mKernelId);
    BasicBlock * const entry = b->GetInsertBlock();
    BasicBlock * const expandZeroExtension =
        b->CreateBasicBlock(prefix + "_expandZeroExtensionBuffer", insertBefore);
    BasicBlock * const hasSufficientZeroExtendSpace =
        b->CreateBasicBlock(prefix + "_hasSufficientZeroExtendSpace", insertBefore);

    auto zeSpaceRef = b->getScalarFieldPtr(ZERO_EXTENDED_SPACE);
    Value * const currentSpace = b->CreateLoad(zeSpaceRef.second, zeSpaceRef.first);

    auto zeBufferRef = b->getScalarFieldPtr(ZERO_EXTENDED_BUFFER);
    Value * const currentBuffer = b->CreateLoad(zeBufferRef.second, zeBufferRef.first);

    requiredSpace = b->CreateRoundUp(requiredSpace, b->getSize(b->getCacheAlignment()));

    Value * const largeEnough = b->CreateICmpUGE(currentSpace, requiredSpace);
    b->CreateLikelyCondBr(largeEnough, hasSufficientZeroExtendSpace, expandZeroExtension);

    b->SetInsertPoint(expandZeroExtension);
    assert (b->getCacheAlignment() >= (b->getBitBlockWidth() / 8));
    b->CreateFree(currentBuffer);
    Value * const newBuffer = b->CreatePageAlignedMalloc(requiredSpace);
    b->CreateMemZero(newBuffer, requiredSpace, b->getCacheAlignment());
    b->CreateStore(requiredSpace, zeSpaceRef.first);
    b->CreateStore(newBuffer, zeBufferRef.first);
    b->CreateBr(hasSufficientZeroExtendSpace);

    b->SetInsertPoint(hasSufficientZeroExtendSpace);
    PHINode * const zeroBuffer = b->CreatePHI(b->getVoidPtrTy(), 2);
    zeroBuffer->addIncoming(currentBuffer, entry);
    zeroBuffer->addIncoming(newBuffer, expandZeroExtension);
    return zeroBuffer;
    #else
    return nullptr;
    #endif
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief getZeroExtendedInputVirtualBaseAddresses
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::getZeroExtendedInputVirtualBaseAddresses(BuilderRef b,
                                                                const Vec<Value *> & baseAddresses,
                                                                Value * const zeroExtensionSpace,
                                                                Vec<Value *> & zeroExtendedVirtualBaseAddress) const {
    #ifndef DISABLE_ZERO_EXTEND
    for (const auto e : make_iterator_range(in_edges(mKernelId, mBufferGraph))) {
        const BufferPort & rt = mBufferGraph[e];
        assert (rt.Port.Type == PortType::Input);
        Value * const zeroExtended = mIsInputZeroExtended[rt.Port];
        if (zeroExtended) {
            PHINode * processed = nullptr;
            if (mCurrentProcessedDeferredItemCountPhi[rt.Port]) {
                processed = mCurrentProcessedDeferredItemCountPhi[rt.Port];
            } else {
                processed = mCurrentProcessedItemCountPhi[rt.Port];
            }
            const BufferNode & bn = mBufferGraph[source(e, mBufferGraph)];
            const Binding & binding = rt.Binding;
            const StreamSetBuffer * const buffer = bn.Buffer;

            Constant * const LOG_2_BLOCK_WIDTH = b->getSize(floor_log2(b->getBitBlockWidth()));
            Constant * const ZERO = b->getSize(0);
            PointerType * const bufferType = buffer->getPointerType();
            Value * const blockIndex = b->CreateLShr(processed, LOG_2_BLOCK_WIDTH);

            // allocateLocalZeroExtensionSpace guarantees this will be large enough to satisfy the kernel
            ExternalBuffer tmp(0, b, binding.getType(), true, buffer->getAddressSpace());
            Value * zeroExtension = b->CreatePointerCast(zeroExtensionSpace, bufferType);
            Value * addr = tmp.getStreamBlockPtr(b, zeroExtension, ZERO, b->CreateNeg(blockIndex));
            addr = b->CreatePointerCast(addr, bufferType);
            const auto i = rt.Port.Number;
            assert (addr->getType() == baseAddresses[i]->getType());

            addr = b->CreateSelect(zeroExtended, addr, baseAddresses[i], "zeroExtendAddr");
            zeroExtendedVirtualBaseAddress[i] = addr;
        }
    }
    #endif
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief zeroInputAfterFinalItemCount
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::zeroInputAfterFinalItemCount(BuilderRef b, const Vec<Value *> & accessibleItems, Vec<Value *> & inputBaseAddresses) {
    #ifndef DISABLE_INPUT_ZEROING

    Constant * const sz_ZERO = b->getSize(0);
    Constant * const sz_ONE = b->getSize(1);

    for (const auto e : make_iterator_range(in_edges(mKernelId, mBufferGraph))) {

        const auto streamSet = source(e, mBufferGraph);

        const BufferNode & bn = mBufferGraph[streamSet];
        const StreamSetBuffer * const buffer = bn.Buffer;
        const BufferPort & port = mBufferGraph[e];
        const auto inputPort = port.Port;
        assert (inputPort.Type == PortType::Input);
        const Binding & input = port.Binding;
        const ProcessingRate & rate = input.getRate();

        // TODO: have an "unsafe" override attribute for unowned ones? this isn't needed for
        // nested pipelines but could replace the source output.

        const auto alwaysTruncate = bn.isUnowned() || bn.isTruncated() || bn.isConstant();

        if (LLVM_UNLIKELY(rate.isGreedy() && !alwaysTruncate)) {
            continue;
        }

        //if (LLVM_LIKELY(rate.isFixed() || rate.isPartialSum() || bn.isTruncated() || bn.isConstant())) {

            const auto itemWidth = getItemWidth(buffer->getBaseType());

            if (LLVM_UNLIKELY(itemWidth == 0)) {
                continue;
            }

            // TODO: for fixed rate inputs, so long as the actual number of items is aa even
            // multiple of the stride*rate, we can ignore masking.

            // TODO: if we can prove that this will be the last kernel invocation that will ever touch this stream)
            // and is not an input to the pipeline (which we cannot prove will have space after the last item), we
            // can avoid copying the buffer and instead just mask out the surpressed items.


            AllocaInst * bufferStorage = nullptr;
            PointerType * const int8PtrTy = b->getInt8PtrTy();
            Constant * const nullPtr = ConstantPointerNull::get(int8PtrTy);
            if (mNumOfTruncatedInputBuffers < mTruncatedInputBuffer.size()) {
                bufferStorage = mTruncatedInputBuffer[mNumOfTruncatedInputBuffers];
            } else { // create a stack entry for this buffer at the start of the pipeline
                bufferStorage = b->CreateAllocaAtEntryPoint(int8PtrTy);
                Instruction * const nextNode = bufferStorage->getNextNode(); assert (nextNode);
                new StoreInst(ConstantPointerNull::get(int8PtrTy), bufferStorage, nextNode);
                mTruncatedInputBuffer.push_back(bufferStorage);
            }
            ++mNumOfTruncatedInputBuffers;

            const auto prefix = makeBufferName(mKernelId, inputPort);

            Constant * const ITEM_WIDTH = b->getSize(itemWidth);

            PointerType * const bufferType = buffer->getPointerType();

            BasicBlock * const maskedInput = b->CreateBasicBlock(prefix + "_maskInput", mKernelCheckOutputSpace);
            BasicBlock * const selectedInput = b->CreateBasicBlock(prefix + "_selectInput", mKernelCheckOutputSpace);

            BasicBlock * const entryBlock = b->GetInsertBlock();

            Value * const selected = accessibleItems[inputPort.Number];
            Value * const totalNumOfItems = getAccessibleInputItems(b, port);



            if (LLVM_UNLIKELY(alwaysTruncate)) {
                b->CreateBr(maskedInput);
            } else {
                Value * const tooMany = b->CreateICmpULT(selected, totalNumOfItems);
                Value * computeMask = tooMany;
                if (mIsInputZeroExtended[inputPort]) {
                    computeMask = b->CreateAnd(tooMany, b->CreateNot(mIsInputZeroExtended[inputPort]));
                }
                b->CreateStore(nullPtr, bufferStorage);
                b->CreateUnlikelyCondBr(computeMask, maskedInput, selectedInput);
            }

            b->SetInsertPoint(maskedInput);

            // if this is a deferred fixed rate stream, we cannot be sure how many
            // blocks will have to be provided to the kernel in order to mask out
            // the truncated input stream.


            // Generate a name to describe this masking function.
            SmallVector<char, 32> tmp;
            raw_svector_ostream name(tmp);

            name << "__maskInput" << itemWidth;

            const auto unaligned = input.hasAttribute(AttrId::AllowsUnalignedAccess);
            if (unaligned) {
                name << "U";
            }

            Module * const m = b->getModule();

            Function * maskInput = m->getFunction(name.str());

            if (maskInput == nullptr) {

                IntegerType * const sizeTy = b->getSizeTy();

                const auto blockWidth = b->getBitBlockWidth();
                const auto log2BlockWidth = floor_log2(blockWidth);
                Constant * const BLOCK_MASK = b->getSize(blockWidth - 1);
                Constant * const LOG_2_BLOCK_WIDTH = b->getSize(log2BlockWidth);

                const auto ip = b->saveIP();

                FixedArray<Type *, 6> params;
                params[0] = int8PtrTy; // input buffer
                params[1] = sizeTy; // bytes per stride
                params[2] = sizeTy; // start
                params[3] = sizeTy; // end
                params[4] = sizeTy; // numOfStreams
                params[5] = int8PtrTy->getPointerTo(); // masked buffer storage ptr

                LLVMContext & C = m->getContext();

                FunctionType * const funcTy = FunctionType::get(int8PtrTy, params, false);
                maskInput = Function::Create(funcTy, Function::InternalLinkage, name.str(), m);
                if (LLVM_UNLIKELY(CheckAssertions)) {
                    #if LLVM_VERSION_INTEGER < LLVM_VERSION_CODE(15, 0, 0)
                    maskInput->setHasUWTable();
                    #else
                    maskInput->setUWTableKind(UWTableKind::Default);
                    #endif
                }
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
                Value * const itemsPerSegment = nextArg();
                itemsPerSegment->setName("itemsPerSegment");
                Value * const start = nextArg();
                start->setName("start");
                Value * const end = nextArg();
                end->setName("end");
                Value * const numOfStreams = nextArg();
                numOfStreams->setName("numOfStreams");
                Value * const bufferStorage = nextArg();
                bufferStorage->setName("bufferStorage");
                assert (arg == maskInput->arg_end());


                Type * const singleElementStreamSetTy = ArrayType::get(FixedVectorType::get(IntegerType::get(C, itemWidth), static_cast<unsigned>(0)), 1);
                ExternalBuffer tmp(0, b, singleElementStreamSetTy, true, 0);
                PointerType * const bufferPtrTy = tmp.getPointerType();

                Value * const inputAddress = b->CreatePointerCast(inputBuffer, bufferPtrTy);
                Value * const initial = b->CreateMul(b->CreateLShr(start, LOG_2_BLOCK_WIDTH), numOfStreams);
                Value * const initialPtr = tmp.getStreamBlockPtr(b, inputAddress, sz_ZERO, initial);
                Value * const initialPtrInt = b->CreatePtrToInt(initialPtr, intPtrTy);


                Value * const requiredItemsPerStream = b->CreateAdd(end, itemsPerSegment);
                Value * const requiredBlocksPerStream = b->CreateLShr(requiredItemsPerStream, LOG_2_BLOCK_WIDTH);
                Value * const requiredBlocks = b->CreateMul(requiredBlocksPerStream, numOfStreams);
                Value * const requiredPtr = tmp.getStreamBlockPtr(b, inputAddress, sz_ZERO, requiredBlocks);
                Value * const requiredPtrInt = b->CreatePtrToInt(requiredPtr, intPtrTy);

                Value * const mallocBytes = b->CreateSub(requiredPtrInt, initialPtrInt);
                const auto blockSize = b->getBitBlockWidth() / 8;
                const auto alignment = unaligned ? 1 : blockSize;

                Value * const maskedBuffer = b->CreateAlignedMalloc(mallocBytes, blockSize);
                b->CreateMemZero(maskedBuffer, mallocBytes, blockSize);
                b->CreateStore(maskedBuffer, bufferStorage);
                Value * const mallocedAddress = b->CreatePointerCast(maskedBuffer, bufferPtrTy);
                Value * const total = b->CreateLShr(end, LOG_2_BLOCK_WIDTH);
                Value * const fullCopyEnd = b->CreateMul(total, numOfStreams);
                Value * const fullCopyEndPtr = tmp.getStreamBlockPtr(b, inputAddress, sz_ZERO, fullCopyEnd);
                Value * const fullCopyEndPtrInt = b->CreatePtrToInt(fullCopyEndPtr, intPtrTy);
                Value * const fullBytesToCopy = b->CreateSub(fullCopyEndPtrInt, initialPtrInt);

                b->CreateMemCpy(mallocedAddress, initialPtr, fullBytesToCopy, alignment);
                Value * const outputVBA = tmp.getStreamBlockPtr(b, mallocedAddress, sz_ZERO, b->CreateNeg(initial));
                Value * const maskedAddress = b->CreatePointerCast(outputVBA, bufferPtrTy);
                assert (maskedAddress->getType() == inputAddress->getType());

                Value * packIndex = nullptr;
                Value * maskOffset = b->CreateAnd(end, BLOCK_MASK);
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
                    b->CreateMemCpy(outputPtr, inputPtr, bytesToCopy, alignment);
                    inputPtr = partialCopyInputEndPtr;
                    outputPtr = tmp.getStreamPackPtr(b, maskedAddress, streamIndex, fullCopyEnd, packIndex);
                }
                assert (inputPtr->getType() == outputPtr->getType());
                Value * const val = b->CreateAlignedLoad(mask->getType(), inputPtr, alignment);
                Value * const maskedVal = b->CreateAnd(val, mask);
                b->CreateAlignedStore(maskedVal, outputPtr, alignment);

                Value * const nextIndex = b->CreateAdd(streamIndex, sz_ONE);
                Value * const notDone = b->CreateICmpNE(nextIndex, numOfStreams);
                streamIndex->addIncoming(nextIndex, maskedInputLoop);

                b->CreateCondBr(notDone, maskedInputLoop, maskedInputExit);

                b->SetInsertPoint(maskedInputExit);
                b->CreateRet(b->CreatePointerCast(maskedAddress, int8PtrTy));

                b->restoreIP(ip);
            }

            FixedArray<Value *, 6> args;
            args[0] = b->CreatePointerCast(inputBaseAddresses[inputPort.Number], int8PtrTy);
            const auto itemsPerSegment = ceiling(mKernel->getStride() * rate.getUpperBound());
            assert (itemsPerSegment >= 1 || rate.isGreedy());
            args[1] = b->getSize(std::max(itemsPerSegment, b->getBitBlockWidth()));
            if (port.isDeferred()) {
                args[2] = mCurrentProcessedDeferredItemCountPhi[inputPort];
            } else {
                args[2] = mCurrentProcessedItemCountPhi[inputPort];
            }
            args[3] = b->CreateAdd(mCurrentProcessedItemCountPhi[inputPort], selected);
            args[4] = buffer->getStreamSetCount(b);
            args[5] = bufferStorage;

            #ifdef PRINT_DEBUG_MESSAGES
            debugPrint(b, prefix + " truncating item count from %" PRIu64 " to %" PRIu64,
                      totalNumOfItems, selected);
            #endif

            Value * const maskedAddress = b->CreatePointerCast(b->CreateCall(maskInput->getFunctionType(), maskInput, args), bufferType);
            BasicBlock * const maskedInputLoopExit = b->GetInsertBlock();
            b->CreateBr(selectedInput);

            b->SetInsertPoint(selectedInput);
            PHINode * const phi = b->CreatePHI(bufferType, 2);
            if (!alwaysTruncate) {
                phi->addIncoming(inputBaseAddresses[inputPort.Number], entryBlock);
            }
            phi->addIncoming(maskedAddress, maskedInputLoopExit);
            inputBaseAddresses[inputPort.Number] = phi;

        //}
    }
    #endif
}

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief freeZeroedInputBuffers
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::freeZeroedInputBuffers(BuilderRef b) {
    // free any truncated input buffers
    for (unsigned i = 0; i < mNumOfTruncatedInputBuffers; ++i) {
        b->CreateFree(b->CreateLoad(b->getInt8PtrTy(), mTruncatedInputBuffer[i]));
        b->CreateStore(ConstantPointerNull::get(b->getInt8PtrTy()), mTruncatedInputBuffer[i]);
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
    Constant * const sz_ZERO = b->getSize(0);
    Constant * const ONE = b->getSize(1);
    Constant * const BLOCK_MASK = b->getSize(blockWidth - 1);

    for (const auto e : make_iterator_range(out_edges(mKernelId, mBufferGraph))) {
        const auto streamSet = target(e, mBufferGraph);
        const BufferNode & bn = mBufferGraph[streamSet];

        // If this stream is either controlled by this kernel or is an external
        // stream, any clearing of data is the responsibility of the owner.
        // Simply ignore any external buffers for the purpose of zeroing out
        // unnecessary data.
        if (LLVM_UNLIKELY(bn.isUnowned() || bn.isTruncated() || bn.isConstant())) {
            continue;
        }

        const StreamSetBuffer * const buffer = bn.Buffer;
        const BufferPort & rt = mBufferGraph[e];
        assert (rt.Port.Type == PortType::Output);
        const auto port = rt.Port;

        const auto itemWidth = getItemWidth(buffer->getBaseType());

        const auto prefix = makeBufferName(mKernelId, port);

        Value * produced = nullptr;
        if (LLVM_UNLIKELY(bn.OutputItemCountId != streamSet)) {
            produced = mLocallyAvailableItems[bn.OutputItemCountId];
        } else {
            produced = mProducedAtTermination[port];
        }

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
        BasicBlock * const maskLoop = b->CreateBasicBlock(prefix + "_zeroUnwrittenLoop", mKernelLoopExit);
        BasicBlock * const maskExit = b->CreateBasicBlock(prefix + "_zeroUnwrittenExit", mKernelLoopExit);
        Value * const numOfStreams = buffer->getStreamSetCount(b);
        Value * const baseAddress = buffer->getBaseAddress(b);

        DataLayout DL(b->getModule());
        Type * const intPtrTy = DL.getIntPtrType(baseAddress->getType());

        #ifdef PRINT_DEBUG_MESSAGES
        Value * const epoch = buffer->getStreamPackPtr(b, baseAddress, sz_ZERO, sz_ZERO, sz_ZERO);
        Value * const epochInt = b->CreatePtrToInt(epoch, intPtrTy);
        #endif
        BasicBlock * const entry = b->GetInsertBlock();
        b->CreateCondBr(b->CreateICmpNE(maskOffset, sz_ZERO), maskLoop, maskExit);

        b->SetInsertPoint(maskLoop);
        PHINode * const streamIndexPhi = b->CreatePHI(b->getSizeTy(), 2, "streamIndex");
        streamIndexPhi->addIncoming(sz_ZERO, entry);
        Value * inputPtr = nullptr;
        if (itemWidth > 1) {
            inputPtr = buffer->getStreamPackPtr(b, baseAddress, streamIndexPhi, blockIndex, packIndex);
        } else {
            inputPtr = buffer->getStreamBlockPtr(b, baseAddress, streamIndexPhi, blockIndex);
        }
        #ifdef PRINT_DEBUG_MESSAGES
        Value * const ptrInt = b->CreatePtrToInt(inputPtr, intPtrTy);
        debugPrint(b, prefix + "_zeroUnwritten_partialPtr = 0x%" PRIx64, ptrInt);
        #endif
        Value * const value = b->CreateBlockAlignedLoad(mask->getType(), inputPtr);
        Value * const maskedValue = b->CreateAnd(value, mask);

        Value * outputPtr = inputPtr;
        if (LLVM_UNLIKELY(bn.isTruncated())) {
            if (itemWidth > 1) {
                outputPtr = buffer->getStreamPackPtr(b, baseAddress, streamIndexPhi, blockIndex, packIndex);
            } else {
                outputPtr = buffer->getStreamBlockPtr(b, baseAddress, streamIndexPhi, blockIndex);
            }

        }
        b->CreateBlockAlignedStore(maskedValue, outputPtr);
        if (itemWidth > 1) {
            // Since packs are laid out sequentially in memory, it will hopefully be cheaper to zero them out here
            // because they may be within the same cache line.
            Value * const nextPackIndex = b->CreateAdd(packIndex, ONE);
            Value * const start = buffer->getStreamPackPtr(b, baseAddress, streamIndexPhi, blockIndex, nextPackIndex);
            Value * const startInt = b->CreatePtrToInt(start, intPtrTy);
            Value * const end = buffer->getStreamPackPtr(b, baseAddress, streamIndexPhi, blockIndex, ITEM_WIDTH);
            Value * const endInt = b->CreatePtrToInt(end, intPtrTy);
            Value * const remainingPackBytes = b->CreateSub(endInt, startInt);
            b->CreateMemZero(start, remainingPackBytes, blockWidth / 8);
        }
        BasicBlock * const maskLoopExit = b->GetInsertBlock();
        Value * const nextStreamIndex = b->CreateAdd(streamIndexPhi, ONE);
        streamIndexPhi->addIncoming(nextStreamIndex, maskLoopExit);
        Value * const notDone = b->CreateICmpNE(nextStreamIndex, numOfStreams);
        b->CreateCondBr(notDone, maskLoop, maskExit);

        b->SetInsertPoint(maskExit);
        // Zero out any blocks we could potentially touch
        Rational strideLength{0};
        for (const auto e : make_iterator_range(out_edges(streamSet, mBufferGraph))) {
            const BufferPort & rd = mBufferGraph[e];
            const Binding & input = rd.Binding;
            Rational R{rd.Maximum};
            if (LLVM_UNLIKELY(input.hasLookahead())) {
                R += input.getLookahead();
            }
            strideLength = std::max(strideLength, R);
        }

        const auto blocksToZero = ceiling(Rational{strideLength.numerator(), blockWidth * strideLength.denominator()});


        if (blocksToZero > 1) {
            Value * const nextBlockIndex = b->CreateAdd(blockIndex, ONE);
            Value * const nextOffset = buffer->modByCapacity(b, nextBlockIndex);
            Value * const startPtr = buffer->StreamSetBuffer::getStreamBlockPtr(b, baseAddress, sz_ZERO, nextOffset);
            Value * const startPtrInt = b->CreatePtrToInt(startPtr, intPtrTy);
            Value * const endOffset = b->CreateRoundUp(nextOffset, b->getSize(blocksToZero));
            Value * const endPtr = buffer->StreamSetBuffer::getStreamBlockPtr(b, baseAddress, sz_ZERO, endOffset);
            Value * const endPtrInt = b->CreatePtrToInt(endPtr, intPtrTy);
            Value * const remainingBytes = b->CreateSub(endPtrInt, startPtrInt);
//            #ifdef PRINT_DEBUG_MESSAGES
//            debugPrint(b, prefix + "_zeroUnwritten_bufferStart = %" PRIu64, b->CreateSub(startPtrInt, epochInt));
//            debugPrint(b, prefix + "_zeroUnwritten_remainingBufferBytes = %" PRIu64, remainingBytes);
//            #endif
            b->CreateMemZero(startPtr, remainingBytes, blockWidth / 8);
        }
    }
    #endif
}

}
