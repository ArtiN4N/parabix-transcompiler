#ifndef KERNEL_EXECUTION_LOGIC_HPP
#define KERNEL_EXECUTION_LOGIC_HPP

#include "pipeline_compiler.hpp"

namespace kernel {

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief writeRegularKernelCall
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::writeKernelCall(BuilderRef b) {

    // WARNING: any change to this must be reflected in Kernel::addDoSegmentDeclaration, Kernel::getDoSegmentFields,
    // Kernel::setDoSegmentProperties and Kernel::getDoSegmentProperties.

    // TODO: consider whether we should share internally synchronized item counts via state?

    // TODO: add MProtect to buffers and their handles.

    // TODO: send in the # of output items we want in the external buffers

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b->CreateMProtect(mKernelHandle, CBuilder::Protect::NONE);
    }

    mUpdatedNumOfStrides = b->CreateAdd(mCurrentNumOfStrides, mNumOfLinearStrides);

    Value * releaseLock = nullptr;

    if (mKernelIsInternallySynchronized) {
        updateProcessedAndProducedItemCounts(b);
        writeUpdatedItemCounts(b, ItemCountSource::ComputedAtKernelCall);
        writeTerminationSignal(b, mIsFinalInvocationPhi);
        BasicBlock * const lastPartialSegment = b->CreateBasicBlock("", mKernelTerminationCheck);
        BasicBlock * const afterSyncLock = b->CreateBasicBlock("", mKernelTerminationCheck);

        const BufferNode & bn = mBufferGraph[mKernelIndex];
        Constant * const maxStrides = b->getSize(ceiling(bn.Upper));
        releaseLock = b->CreateICmpEQ(mUpdatedNumOfStrides, maxStrides);
        releaseLock = b->CreateOr(b->CreateIsNotNull(mIsFinalInvocationPhi), releaseLock);

        b->CreateCondBr(releaseLock, lastPartialSegment, afterSyncLock);

        b->SetInsertPoint(lastPartialSegment);
        releaseSynchronizationLock(b, LockType::ItemCheck);
        b->CreateBr(afterSyncLock);

        b->SetInsertPoint(afterSyncLock);
    }

    const auto args = buildKernelCallArgumentList(b);

    #ifdef PRINT_DEBUG_MESSAGES
    const auto prefix = makeKernelName(mKernelIndex);
    debugPrint(b, "* " + prefix + "_executing = %" PRIu64, mNumOfLinearStrides);
    debugHalt(b);
    #endif
    startCycleCounter(b, CycleCounter::BEFORE_KERNEL_CALL);
    Value * const doSegment = getKernelDoSegmentFunction(b);
    FunctionType * const doSegFuncType = cast<FunctionType>(doSegment->getType()->getPointerElementType());
    Value * doSegmentRetVal = nullptr;
    if (mRethrowException) {
        BasicBlock * const invokeOk = b->CreateBasicBlock("", mKernelTerminationCheck);
        #if LLVM_VERSION_INTEGER >= LLVM_VERSION_CODE(11, 0, 0)
        auto *calleePtrType = llvm::cast<llvm::PointerType>(doSegment->getType());
        auto *calleeType = llvm::cast<llvm::FunctionType>(calleePtrType->getElementType());
        doSegmentRetVal = b->CreateInvoke(calleeType, doSegment, invokeOk, mRethrowException, args);
        #else
        doSegmentRetVal = b->CreateInvoke(doSegment, invokeOk, mRethrowException, args);
        #endif
        b->SetInsertPoint(invokeOk);
    } else {
        doSegmentRetVal = b->CreateCall(doSegFuncType, doSegment, args);
    }
    updateCycleCounter(b, CycleCounter::BEFORE_KERNEL_CALL, CycleCounter::AFTER_KERNEL_CALL);
    #ifdef PRINT_DEBUG_MESSAGES
    debugResume(b);
    #endif

    mTerminatedExplicitly = mKernelCanTerminateEarly ? doSegmentRetVal : nullptr;

    if (mKernelIsInternallySynchronized) {

        BasicBlock * const lastPartialSegment = b->CreateBasicBlock("", mKernelTerminationCheck);
        BasicBlock * const afterSyncLock = b->CreateBasicBlock("", mKernelTerminationCheck);

        b->CreateCondBr(releaseLock, lastPartialSegment, afterSyncLock);

        b->SetInsertPoint(lastPartialSegment);
        startCycleCounter(b, CycleCounter::BEFORE_SYNCHRONIZATION);
        acquireSynchronizationLock(b, LockType::Segment, CycleCounter::BEFORE_SYNCHRONIZATION);
        b->CreateBr(afterSyncLock);

        b->SetInsertPoint(afterSyncLock);
    } else {
        updateProcessedAndProducedItemCounts(b);
    }

    readReturnedOutputVirtualBaseAddresses(b);

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b->CreateMProtect(mKernelHandle, CBuilder::Protect::WRITE);
    }

}

#if 0

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief buildKernelCallArgumentList
 ** ------------------------------------------------------------------------------------------------------------- */
ArgVec PipelineCompiler::buildKernelCallArgumentList(BuilderRef b) {


    const auto numOfInputs = getNumOfStreamInputs(mKernelIndex);
    const auto numOfOutputs = getNumOfStreamOutputs(mKernelIndex);

    ArgVec args;
    args.reserve(4 + (numOfInputs + numOfOutputs) * 4);
    if (LLVM_LIKELY(mKernel->isStateful())) {
        args.push_back(mKernelHandle); assert (mKernelHandle);
    }
    if (LLVM_UNLIKELY(mKernel->hasThreadLocal())) {
        args.push_back(b->CreateLoad(getThreadLocalHandlePtr(b, mKernelIndex)));
    }
    args.push_back(mNumOfLinearStrides); assert (mNumOfLinearStrides);
    if (LLVM_LIKELY(!mKernel->requiresExplicitPartialFinalStride())) {
        args.push_back(b->CreateIsNotNull(mIsFinalInvocationPhi));
    }
    // If a kernel is internally synchronized, pass the segno to
    // allow the kernel to initialize its current "position"
    if (mKernelIsInternallySynchronized) {
        const auto prefix = makeKernelName(mKernelIndex);
        Value * const internalSegNoPtr = b->getScalarFieldPtr(prefix + CURRENT_LOGICAL_SEGMENT_NUMBER);
        Value * const segNo = b->CreateLoad(internalSegNoPtr);
        b->CreateStore(b->CreateAdd(segNo, b->getSize(1)), internalSegNoPtr);
        args.push_back(segNo);
    }
    if (mFixedRateFactorPhi) {
        args.push_back(mFixedRateFactorPhi);
    }

    RelationshipType prior_in{};

    for (const auto e : make_iterator_range(in_edges(mKernelIndex, mBufferGraph))) {
        const BufferRateData & rt = mBufferGraph[e];
        assert (rt.Port.Type == PortType::Input);
        assert (prior_in < rt.Port);
        prior_in = rt.Port;

        if (LLVM_LIKELY(rt.Port.Reason == ReasonType::Explicit)) {

            // calculate the deferred processed item count
            PHINode * processed = nullptr;
            bool deferred = false;

            const auto i = rt.Port.Number;
            if (mAlreadyProcessedDeferredPhi[i]) {
                processed = mAlreadyProcessedDeferredPhi[i];
                deferred = true;
            } else {
                processed = mAlreadyProcessedPhi[i];
            }

            const Binding & input = rt.Binding;
            #ifndef NDEBUG
            const auto buffer = source(e, mBufferGraph);
            const BufferNode & bn = mBufferGraph[buffer];
            assert ("input buffer type mismatch?" && (input.getType() == bn.Buffer->getBaseType()));
            #endif
            args.push_back(mInputEpochPhi[i]);

            mReturnedProcessedItemCountPtr[i] = addItemCountArg(b, input, deferred, processed, args);

            if (LLVM_UNLIKELY(requiresItemCount(input))) {
                // calculate how many linear items are from the *deferred* position
                Value * inputItems = mLinearInputItemsPhi[i];
                if (deferred) {
                    Value * diff = b->CreateSub(mAlreadyProcessedPhi[i], mAlreadyProcessedDeferredPhi[i]);
                    inputItems = b->CreateAdd(inputItems, diff);
                }
                args.push_back(inputItems); assert (inputItems);
            }
        }
    }

    RelationshipType prior_out{};
    for (const auto e : make_iterator_range(out_edges(mKernelIndex, mBufferGraph))) {
        const BufferRateData & rt = mBufferGraph[e];
        assert (rt.Port.Reason == ReasonType::Explicit);
        assert (rt.Port.Type == PortType::Output);
        assert (prior_out < rt.Port);
        prior_out = rt.Port;
        const auto i = rt.Port.Number;

        PHINode * const produced = mAlreadyProducedPhi[i];
        const auto buffer = target(e, mBufferGraph);
        const BufferNode & bn = mBufferGraph[buffer];
        const Binding & output = rt.Binding;

        assert ("output buffer type mismatch?" && (output.getType() == bn.Buffer->getBaseType()));

        if (LLVM_UNLIKELY(bn.Type == BufferType::ManagedByKernel)) {
            mReturnedOutputVirtualBaseAddressPtr[i] = addVirtualBaseAddressArg(b, bn.Buffer, args);
        } else {
            args.push_back(getVirtualBaseAddress(b, output, bn.Buffer, produced, nullptr));
        }
        mReturnedProducedItemCountPtr[i] = addItemCountArg(b, output, mKernelCanTerminateEarly, produced, args);
        // TODO:  consider whether we should pass a requested amount to source streams?
        if (requiresItemCount(output)) {
            args.push_back(mLinearOutputItemsPhi[i]);  assert (mLinearOutputItemsPhi[i]);
        }
        if (LLVM_UNLIKELY(bn.Type == BufferType::ManagedByKernel)) {
            args.push_back(mConsumedItemCount[i]); assert (mConsumedItemCount[i]);
        }
    }

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableMProtect))) {
        b->CreateMProtect(mKernelHandle, CBuilder::Protect::NONE);
    }

    return args;
}

#else


/** ------------------------------------------------------------------------------------------------------------- *
 * @brief buildKernelCallArgumentList
 ** ------------------------------------------------------------------------------------------------------------- */
ArgVec PipelineCompiler::buildKernelCallArgumentList(BuilderRef b) {

    // WARNING: any change to this must be reflected in Kernel::addDoSegmentDeclaration, Kernel::getDoSegmentFields,
    // Kernel::setDoSegmentProperties and Kernel::getDoSegmentProperties.

    const auto numOfInputs = in_degree(mKernelIndex, mBufferGraph);
    const auto numOfOutputs = out_degree(mKernelIndex, mBufferGraph);

    ArgVec args;

    #ifndef NDEBUG
    Value * const doSegment = getKernelDoSegmentFunction(b);
    FunctionType * const doSegmentFunctionType = cast<FunctionType>(doSegment->getType()->getPointerElementType());
    #endif

    auto addNextArg = [&](Value * arg) {

        #ifndef NDEBUG
        assert ("null argument" && arg);

        const auto n = doSegmentFunctionType->getNumParams();
        if (LLVM_UNLIKELY(args.size() >= n)) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);
            out << mKernel->getName() << ": "
                   "was given too many arguments";
            throw std::runtime_error(out.str().str());
        }

        Type * const argTy = doSegmentFunctionType->getParamType(args.size());
        if (LLVM_UNLIKELY(argTy != arg->getType())) {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);

            Function * const func = mKernel->getDoSegmentFunction(b, true);

            out << mKernel->getName() << ": "
                "invalid argument type for ";

            auto argItr = func->arg_begin();
            std::advance(argItr, args.size());
            out << argItr->getName();

            out << " (#" << args.size()
                << "): expected ";
            argTy->print(out);
            out << " but got ";
            arg->getType()->print(out);
            throw std::runtime_error(out.str().str());
        }
        #endif

        args.push_back(arg);
    };

    auto addItemCountArg = [&](const Binding & binding,
                               const bool forceAddressability,
                               Value * const itemCount) {

        const ProcessingRate & rate = binding.getRate();
        Value * ptr = nullptr;
        if (LLVM_UNLIKELY(rate.isRelative())) {
            return ptr;
        }
        if (forceAddressability || isAddressable(binding)) {
            if (LLVM_UNLIKELY(mNumOfAddressableItemCount == mAddressableItemCountPtr.size())) {
                auto aic = b->CreateAllocaAtEntryPoint(b->getSizeTy());
                mAddressableItemCountPtr.push_back(aic);
            }
            ptr = mAddressableItemCountPtr[mNumOfAddressableItemCount++];
            b->CreateStore(itemCount, ptr);
            addNextArg(ptr);
        } else if (isCountable(binding)) {
            addNextArg(itemCount);
        }
        return ptr;
    };

    auto addVirtualBaseAddressArg = [&](const StreamSetBuffer * buffer) {
        PointerType * const voidPtrTy = b->getVoidPtrTy();
        if (LLVM_UNLIKELY(mNumOfVirtualBaseAddresses == mVirtualBaseAddressPtr.size())) {
            auto vba = b->CreateAllocaAtEntryPoint(voidPtrTy);
            mVirtualBaseAddressPtr.push_back(vba);
        }
        Value * ptr = mVirtualBaseAddressPtr[mNumOfVirtualBaseAddresses++];
        ptr = b->CreatePointerCast(ptr, buffer->getPointerType()->getPointerTo());
        b->CreateStore(buffer->getBaseAddress(b.get()), ptr);
        addNextArg(b->CreatePointerCast(ptr, voidPtrTy->getPointerTo()));
        return ptr;
    };

    args.reserve(4 + (numOfInputs + numOfOutputs) * 4);
    if (LLVM_LIKELY(mKernelHandle != nullptr)) {
        addNextArg(mKernelHandle);
    }
    if (LLVM_UNLIKELY(mKernel->hasThreadLocal())) {
        addNextArg(b->CreateLoad(getThreadLocalHandlePtr(b, mKernelIndex)));
    }

    // If a kernel is internally synchronized, pass the segno to
    // allow the kernel to initialize its current "position"
    #ifdef PRINT_DEBUG_MESSAGES
    const auto prefix = makeKernelName(mKernelIndex);
    #endif
    #ifdef PRINT_DEBUG_MESSAGES
    debugPrint(b, "* " + prefix + "_isFinal = %" PRIu64, mIsFinalInvocationPhi);
    #endif
    const auto greedy = mKernel->isGreedy();
    if (mKernelIsInternallySynchronized || greedy) {
        if (mKernelIsInternallySynchronized) {
            addNextArg(mSegNo);
        }
        addNextArg(b->CreateIsNotNull(mIsFinalInvocationPhi));
    } else {
        addNextArg(mNumOfLinearStrides);
        #ifdef PRINT_DEBUG_MESSAGES
        debugPrint(b, "* " + prefix + "_executing = %" PRIu64, mNumOfLinearStrides);
        #endif
        if (LLVM_LIKELY(mKernel->hasFixedRateInput())) {
            addNextArg(mFixedRateFactorPhi);
        }
    }

    PointerType * const voidPtrTy = b->getVoidPtrTy();

    RelationshipType prior_in{};

    for (const auto e : make_iterator_range(in_edges(mKernelIndex, mBufferGraph))) {
        const BufferRateData & rt = mBufferGraph[e];
        assert (rt.Port.Type == PortType::Input);
        assert (prior_in < rt.Port);
        prior_in = rt.Port;

        if (LLVM_LIKELY(rt.Port.Reason == ReasonType::Explicit)) {

            PHINode * processed = nullptr;
            bool deferred = false;

            const auto i = rt.Port.Number;
            if (mAlreadyProcessedDeferredPhi[i]) {
                processed = mAlreadyProcessedDeferredPhi[i];
                deferred = true;
            } else {
                processed = mAlreadyProcessedPhi[i];
            }

            assert (processed);

            Value * addr = mInputEpochPhi[i];

            addNextArg(b->CreatePointerCast(addr, voidPtrTy));

            mReturnedProcessedItemCountPtr[i] = addItemCountArg(rt.Binding, deferred, processed);

            if (LLVM_UNLIKELY(requiresItemCount(rt.Binding))) {
                // calculate how many linear items are from the *deferred* position
                Value * inputItems = mLinearInputItemsPhi[i];
                if (deferred) {
                    Value * diff = b->CreateSub(mAlreadyProcessedPhi[i], mAlreadyProcessedDeferredPhi[i]);
                    inputItems = b->CreateAdd(inputItems, diff);
                }
                addNextArg(inputItems);
            }

        }
    }

    RelationshipType prior_out{};
    for (const auto e : make_iterator_range(out_edges(mKernelIndex, mBufferGraph))) {
        const BufferRateData & rt = mBufferGraph[e];
        assert (rt.Port.Reason == ReasonType::Explicit);
        assert (rt.Port.Type == PortType::Output);
        assert (prior_out < rt.Port);
        prior_out = rt.Port;
        const auto i = rt.Port.Number;

        PHINode * const produced = mAlreadyProducedPhi[i];
        const auto buffer = target(e, mBufferGraph);
        const BufferNode & bn = mBufferGraph[buffer];
        const Binding & output = rt.Binding;

        if (LLVM_UNLIKELY(bn.Type == BufferType::ManagedByKernel)) {
            mReturnedOutputVirtualBaseAddressPtr[i] = addVirtualBaseAddressArg(bn.Buffer);
        } else {
            Value * vba = getVirtualBaseAddress(b, output, bn.Buffer, produced, nullptr);
            addNextArg(b->CreatePointerCast(vba, voidPtrTy));
        }
        mReturnedProducedItemCountPtr[i] = addItemCountArg(output, mKernelCanTerminateEarly, produced);

        if (requiresItemCount(output)) {
            addNextArg(mLinearOutputItemsPhi[i]);  assert (mLinearOutputItemsPhi[i]);
        }
        if (LLVM_UNLIKELY(bn.Type == BufferType::ManagedByKernel)) {
            addNextArg(mConsumedItemCount[i]); assert (mConsumedItemCount[i]);
        }

    }

    assert (args.size() == doSegmentFunctionType->getNumParams());

    return args;
}



#endif

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief updateProcessedAndProducedItemCounts
 ** ------------------------------------------------------------------------------------------------------------- */
void PipelineCompiler::updateProcessedAndProducedItemCounts(BuilderRef b) {

    const auto numOfInputs = getNumOfStreamInputs(mKernelIndex);
    const auto numOfOutputs = getNumOfStreamOutputs(mKernelIndex);

    // calculate or read the item counts (assuming this kernel did not terminate)
    for (unsigned i = 0; i < numOfInputs; ++i) {
        Value * processed = nullptr;
        const Binding & input = getInputBinding(i);
        const ProcessingRate & rate = input.getRate();
        if (LLVM_LIKELY(rate.isFixed() || rate.isPartialSum() || rate.isGreedy())) {
            processed = b->CreateAdd(mAlreadyProcessedPhi[i], mLinearInputItemsPhi[i]);
            if (mAlreadyProcessedDeferredPhi[i]) {
                assert (mReturnedProcessedItemCountPtr[i]);
                mProcessedDeferredItemCount[i] = b->CreateLoad(mReturnedProcessedItemCountPtr[i]);
                #ifdef PRINT_DEBUG_MESSAGES
                const auto prefix = makeBufferName(mKernelIndex, StreamSetPort{PortType::Input, i});
                debugPrint(b, prefix + "_processed_deferred'", mProcessedDeferredItemCount[i]);
                #endif
                if (LLVM_UNLIKELY(mCheckAssertions)) {
                    Value * const deferred = mProcessedDeferredItemCount[i];
                    Value * const isDeferred = b->CreateICmpULE(deferred, processed);
                    Value * const isFinal = mIsFinalInvocationPhi;
                    // TODO: workaround now for ScanMatch; if it ends with a match on a
                    // block-aligned boundary the start of the next match seems to be one
                    // after? Revise the logic to only perform a 0-item final block on
                    // kernels that may produce Add'ed data? Define the final/non-final
                    // contract first.
                    Value * const isDeferredOrFinal = b->CreateOr(isDeferred, b->CreateIsNotNull(isFinal));
                    b->CreateAssert(isDeferredOrFinal,
                                    "%s.%s: deferred processed item count (%" PRIu64 ") "
                                    "exceeds non-deferred (%" PRIu64 ")",
                                    mKernelAssertionName,
                                    b->GetString(input.getName()),
                                    deferred, processed);
                }
            }
        } else if (rate.isBounded() || rate.isUnknown()) {
            assert (mReturnedProcessedItemCountPtr[i]);
            processed = b->CreateLoad(mReturnedProcessedItemCountPtr[i]);
        } else {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);
            out << "Kernel " << mKernel->getName() << ":" << input.getName()
                << " has an " << "input" << " rate that is not properly handled by the PipelineKernel";
            report_fatal_error(out.str());
        }
        mProcessedItemCount[i] = processed; assert (processed);
        #ifdef PRINT_DEBUG_MESSAGES
        const auto prefix = makeBufferName(mKernelIndex, StreamSetPort{PortType::Input, i});
        debugPrint(b, prefix + "_processed' = %" PRIu64, mProcessedItemCount[i]);
        #endif
    }

    for (unsigned i = 0; i < numOfOutputs; ++i) {
        const Binding & output = getOutputBinding(i);
        const ProcessingRate & rate = output.getRate();
        Value * produced = nullptr;
        if (LLVM_LIKELY(rate.isFixed() || rate.isPartialSum())) {
            produced = b->CreateAdd(mAlreadyProducedPhi[i], mLinearOutputItemsPhi[i]);
            if (mAlreadyProducedDeferredPhi[i]) {
                assert (mReturnedProducedItemCountPtr[i]);
                mProducedDeferredItemCount[i] = b->CreateLoad(mReturnedProducedItemCountPtr[i]);
                #ifdef PRINT_DEBUG_MESSAGES
                const auto prefix = makeBufferName(mKernelIndex, StreamSetPort{PortType::Input, i});
                debugPrint(b, prefix + "_processed_deferred'", mProcessedDeferredItemCount[i]);
                #endif
                if (LLVM_UNLIKELY(mCheckAssertions)) {
                    Value * const deferred = mProducedDeferredItemCount[i];
                    Value * const isDeferred = b->CreateICmpULE(deferred, produced);
                    Value * const isFinal = mIsFinalInvocationPhi;
                    // TODO: workaround now for ScanMatch; if it ends with a match on a
                    // block-aligned boundary the start of the next match seems to be one
                    // after? Revise the logic to only perform a 0-item final block on
                    // kernels that may produce Add'ed data? Define the final/non-final
                    // contract first.
                    Value * const isDeferredOrFinal = b->CreateOr(isDeferred, b->CreateIsNotNull(isFinal));
                    b->CreateAssert(isDeferredOrFinal,
                                    "%s.%s: deferred processed item count (%" PRIu64 ") "
                                    "exceeds non-deferred (%" PRIu64 ")",
                                    mKernelAssertionName,
                                    b->GetString(output.getName()),
                                    deferred, produced);
                }
            }
        } else if (rate.isBounded() || rate.isUnknown()) {
            assert (mReturnedProducedItemCountPtr[i]);
            produced = b->CreateLoad(mReturnedProducedItemCountPtr[i]);
        } else {
            SmallVector<char, 256> tmp;
            raw_svector_ostream out(tmp);
            out << "Kernel " << mKernel->getName() << ":" << output.getName()
                << " has an " << "output" << " rate that is not properly handled by the PipelineKernel";
            report_fatal_error(out.str());
        }
        #ifdef PRINT_DEBUG_MESSAGES
        const auto prefix = makeBufferName(mKernelIndex, StreamSetPort{PortType::Output, i});
        debugPrint(b, prefix + "_produced' = %" PRIu64, produced);
        #endif
        mProducedItemCount[i] = produced;
    }

}

}


#endif // KERNEL_EXECUTION_LOGIC_HPP
