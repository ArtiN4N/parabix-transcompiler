/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/io/source_kernel.h>

#include <kernel/core/kernel_builder.h>
#include <kernel/core/streamset.h>
#include <llvm/IR/Module.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <toolchain/toolchain.h>
#include <boost/interprocess/mapped_region.hpp>
#include <llvm/Support/raw_ostream.h>
#include <unistd.h>

#ifdef __APPLE__

#else


#endif

#if !defined(__APPLE__) && _POSIX_C_SOURCE >= 200809L
#define PREAD pread64
#endif

using namespace llvm;

inline unsigned getPageSize() {
    return boost::interprocess::mapped_region::get_page_size();
}

extern "C" uint64_t file_size(const uint32_t fd) {
    struct stat st;
    if (LLVM_UNLIKELY(fstat(fd, &st) != 0)) {
        st.st_size = 0;
    }
    return st.st_size;
}

//int madvise_wrapper(void * addr, size_t length, int flags) {
//    const auto r = madvise(addr, length, flags);
//    if (r) {
//        const auto e = errno;
//        errs() << "MADV:" << strerror(e) << "\n";
//    }
//    return r;
//}

namespace kernel {

/// MMAP SOURCE KERNEL

void MMapSourceKernel::generatLinkExternalFunctions(BuilderRef b) {
    b->LinkFunction("file_size", file_size);
    b->LinkFunction("mmap", mmap);
    b->LinkFunction("madvise", madvise);
    b->LinkFunction("munmap", munmap);
}

void MMapSourceKernel::generateInitializeMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b) {

    BasicBlock * const emptyFile = b->CreateBasicBlock("emptyFile");
    BasicBlock * const nonEmptyFile = b->CreateBasicBlock("NonEmptyFile");
    BasicBlock * const exit = b->CreateBasicBlock("Exit");
    IntegerType * const sizeTy = b->getSizeTy();
    Value * const fd = b->getScalarField("fileDescriptor");
    PointerType * const codeUnitPtrTy = b->getIntNTy(codeUnitWidth)->getPointerTo();
    b->setScalarField("ancillaryBuffer", ConstantPointerNull::get(codeUnitPtrTy));
    Function * const fileSizeFn = b->getModule()->getFunction("file_size"); assert (fileSizeFn);
    FunctionType * fTy = fileSizeFn->getFunctionType();
    Value * fileSize = b->CreateZExtOrTrunc(b->CreateCall(fTy, fileSizeFn, fd), sizeTy);
    b->CreateLikelyCondBr(b->CreateIsNotNull(fileSize), nonEmptyFile, emptyFile);

    b->SetInsertPoint(nonEmptyFile);
    Value * const fileBuffer = b->CreatePointerCast(b->CreateFileSourceMMap(fd, fileSize), codeUnitPtrTy);
    b->setScalarField("buffer", fileBuffer);
    b->setBaseAddress("sourceBuffer", fileBuffer);
    Value * fileItems = fileSize;
    if (LLVM_UNLIKELY(codeUnitWidth > 8)) {
        fileItems = b->CreateUDiv(fileSize, b->getSize(codeUnitWidth / 8));
    }
    b->setScalarField("fileItems", fileItems);
    b->setCapacity("sourceBuffer", fileItems);
    b->CreateBr(exit);

    b->SetInsertPoint(emptyFile);
    ConstantInt * const STRIDE_BYTES = b->getSize(stride * codeUnitWidth);
    Value * const emptyFilePtr = b->CreatePointerCast(b->CreateAnonymousMMap(STRIDE_BYTES), codeUnitPtrTy);
    b->setScalarField("buffer", emptyFilePtr);
    b->setBaseAddress("sourceBuffer", emptyFilePtr);
    b->setScalarField("fileItems", STRIDE_BYTES);
    b->setTerminationSignal();
    b->CreateBr(exit);

    b->SetInsertPoint(exit);
}


void MMapSourceKernel::generateDoSegmentMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b) {

    BasicBlock * const dropPages = b->CreateBasicBlock("dropPages");
    BasicBlock * const checkRemaining = b->CreateBasicBlock("checkRemaining");
    BasicBlock * const setTermination = b->CreateBasicBlock("setTermination");
    BasicBlock * const exit = b->CreateBasicBlock("mmapSourceExit");

    Value * const numOfStrides = b->getNumOfStrides();
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
        b->CreateAssert(b->CreateIsNotNull(numOfStrides),
                        "Internal error: %s.numOfStrides cannot be 0", b->GetString("MMapSource"));
    }

    // TODO: could we improve overall performance by trying to "preload" the data by reading it? This would increase
    // the cost of this kernel but might allow the first kernel to read the file data be better balanced with it.

    const auto pageSize = getPageSize();
    Value * const desiredItems = b->CreateMul(numOfStrides, b->getSize(stride));
    ConstantInt * const CODE_UNIT_BYTES = b->getSize(codeUnitWidth / 8);

    Value * const consumedItems = b->getConsumedItemCount("sourceBuffer");
    Value * const consumedBytes = b->CreateMul(consumedItems, CODE_UNIT_BYTES);
    Value * const consumedPageOffset = b->CreateRoundDownRational(consumedBytes, pageSize);
    Value * const consumedBuffer = b->getRawOutputPointer("sourceBuffer", consumedPageOffset);
    Value * const readableBuffer = b->getScalarField("buffer");
    Value * const unnecessaryBytes = b->CreateSub(consumedPageOffset, b->CreatePtrToInt(readableBuffer, b->getSizeTy()));

    Module * const m = b->getModule();
    Function * MAdviseFunc = m->getFunction("madvise");
    assert (MAdviseFunc);
    FixedArray<Value *, 3> args;

    // avoid calling madvise unless an actual page table change could occur
    b->CreateLikelyCondBr(b->CreateIsNotNull(unnecessaryBytes), dropPages, checkRemaining);
    b->SetInsertPoint(dropPages);
    // instruct the OS that it can safely drop any fully consumed pages


//    args[0] = readableBuffer;
//    args[1] = consumedPageOffset;
//    args[2] = b->getInt32(MADV_DONTNEED);
//    Value * const r0 = b->CreateCall(MAdviseFunc, args);
//    assert (r0);
//    b->CallPrintInt("r0", r0);
//    b->setScalarField("buffer", consumedBuffer);
    b->CreateBr(checkRemaining);

    // determine whether or not we've exhausted the "safe" region of the file buffer
    b->SetInsertPoint(checkRemaining);
    Value * const producedItems = b->getProducedItemCount("sourceBuffer");
    Value * const nextProducedItems = b->CreateAdd(producedItems, desiredItems);
    Value * const fileItems = b->getScalarField("fileItems");
    Value * const lastPage = b->CreateICmpULE(fileItems, nextProducedItems);
    b->CreateUnlikelyCondBr(lastPage, setTermination, exit);

    // If this is the last page, create a temporary buffer of up to two pages size, copy the unconsumed data
    // and zero any bytes that are not used.
    b->SetInsertPoint(setTermination);
    b->setTerminationSignal();
    b->setProducedItemCount("sourceBuffer", fileItems);
    b->CreateBr(exit);

    b->SetInsertPoint(exit);
    PHINode * producedPhi = b->CreatePHI(b->getSizeTy(), 2);
    producedPhi->addIncoming(nextProducedItems, checkRemaining);
    producedPhi->addIncoming(fileItems, setTermination);
    Value * const producedBytes = b->CreateMul(producedPhi, CODE_UNIT_BYTES);
    Value * const length = b->CreateSub(producedBytes, consumedPageOffset);

    if (codegen::MMapPrefetchType == 0) {
        /* do nothing */
    } else if (codegen::MMapPrefetchType == 1) {
        args[0] = b->CreatePointerCast(consumedBuffer, cast<PointerType>(MAdviseFunc->getArg(0)->getType()));
        args[1] = length;
        args[2] = b->getInt32(MADV_WILLNEED);
        b->CreateCall(MAdviseFunc, args);
    } else if (codegen::MMapPrefetchType <= 7) {

        PointerType * const int8PtrTy = b->getInt8PtrTy();

        Function * const prefetchFunc = Intrinsic::getDeclaration(m, Intrinsic::prefetch, int8PtrTy); assert (prefetchFunc);

        // declare void @llvm.prefetch(ptr <address>, i32 <rw>, i32 <locality>, i32 <cache type>)

        FixedArray<Value *, 4> params;
        params[1] = b->getInt32(0);
        params[2] = b->getInt32(3);
        params[3] = b->getInt32(1);

        // address is the address to be prefetched, rw is the specifier determining if the fetch should be
        // for a read (0) or write (1), and locality is a temporal locality specifier ranging from (0) - no locality,
        // to (3) - extremely local keep in cache. The cache type specifies whether the prefetch is performed on the data (1)
        // or instruction (0) cache. The rw, locality and cache type arguments must be constant integers.

        Rational itemsPerPage{pageSize * 8, codeUnitWidth};
        assert (itemsPerPage.denominator() == 1);

        Value * const first = b->CreateRoundUpRational(producedItems, itemsPerPage);

        BasicBlock * prefetchLoop = b->CreateBasicBlock("prefetchLoop");
        BasicBlock * prefetchLoopExit = b->CreateBasicBlock("prefetchLoopExit");

        b->CreateBr(prefetchLoop);

        b->SetInsertPoint(prefetchLoop);
        PHINode * prefetchPosPhi = b->CreatePHI(b->getSizeTy(), 2);
        prefetchPosPhi->addIncoming(first, exit);

        Type * const codeUnitTy = b->getIntNTy(codeUnitWidth);

        if (codegen::MMapPrefetchType == 2 || codegen::MMapPrefetchType == 5) {
            // prefetch the first address of each page
            Value * pos = b->getRawOutputPointer("sourceBuffer", prefetchPosPhi);
            params[0] = b->CreatePointerCast(pos, int8PtrTy);

            if (codegen::MMapPrefetchType == 2) {
                b->CreateCall(prefetchFunc, params);
            }  else {
                b->CreateLoad(codeUnitTy, params[0], true);
            }


        } else {
            // prefetch all mmaped data
            size_t limit = 0;

            const size_t cl = b->getCacheAlignment();
            Rational itemsPerCacheLine(cl * 8, codeUnitWidth);
            assert (itemsPerCacheLine.denominator() == 1);
            const auto step = itemsPerCacheLine.numerator();

            if (codegen::MMapPrefetchType == 3 || codegen::MMapPrefetchType == 6) {
                limit = step * 3;
            } else {
                limit = itemsPerPage.numerator();
            }

            for (size_t offset = 0; offset < limit; offset += step) {
                Value * pos = b->getRawOutputPointer("sourceBuffer", b->CreateAdd(prefetchPosPhi, b->getSize(offset)));
                if (codegen::MMapPrefetchType < 5) {
                    params[0] = b->CreatePointerCast(pos, int8PtrTy);
                    b->CreateCall(prefetchFunc, params);
                } else {
                    b->CreateLoad(codeUnitTy, pos, true);
                }

            }

        }

        Value * const nextPrefetchPos = b->CreateAdd(prefetchPosPhi, b->getSize(itemsPerPage.numerator()));
        prefetchPosPhi->addIncoming(nextPrefetchPos, prefetchLoop);

        b->CreateCondBr(b->CreateICmpULT(nextPrefetchPos, producedPhi), prefetchLoop, prefetchLoopExit);

        b->SetInsertPoint(prefetchLoopExit);
    } else {
        llvm::report_fatal_error("Unknown mmap prefetch type code");
    }


}
void MMapSourceKernel::freeBuffer(BuilderRef b, const unsigned codeUnitWidth) {
    Value * const fileItems = b->getScalarField("fileItems");
    Constant * const CODE_UNIT_BYTES = b->getSize(codeUnitWidth / 8);
    Value * const fileSize = b->CreateMul(fileItems, CODE_UNIT_BYTES);
    Module * const m = b->getModule();
    Function * MUnmapFunc = m->getFunction("munmap");
    assert (MUnmapFunc);
    FixedArray<Value *, 2> args;
    args[0] = b->CreatePointerCast(b->getBaseAddress("sourceBuffer"), b->getVoidPtrTy());
    args[1] = fileSize;
    b->CreateCall(MUnmapFunc, args);
}

Value * MMapSourceKernel::generateExpectedOutputSizeMethod(const unsigned codeUnitWidth, BuilderRef b) {
    return b->getScalarField("fileItems");
}

void MMapSourceKernel::linkExternalMethods(BuilderRef b) {
    MMapSourceKernel::generatLinkExternalFunctions(b);
}

/// READ SOURCE KERNEL

constexpr char __MAKE_CIRCULAR_BUFFER[] = "__make_circular_buffer";
constexpr char __DESTROY_CIRCULAR_BUFFER[] = "__destroy_circular_buffer";

void ReadSourceKernel::generatLinkExternalFunctions(BuilderRef b) {
    #ifdef PREAD
    b->LinkFunction("pread64", PREAD);
    #else
    b->LinkFunction("read", read);
    #endif
    if (codegen::ReadUsesCircularBuffer) {
        b->LinkFunction(__MAKE_CIRCULAR_BUFFER, make_circular_buffer);
        b->LinkFunction(__DESTROY_CIRCULAR_BUFFER, destroy_circular_buffer);
    }
}

//uint8_t * make_circular_buffer(const size_t size, const size_t hasUnderflow);
//void destroy_circular_buffer(uint8_t * base, const size_t size, const size_t hasUnderflow);

template <typename IntTy>
inline IntTy round_up_to(const IntTy x, const IntTy y) {
    assert(is_power_2(y));
    return (x + y - 1) & -y;
}

void ReadSourceKernel::generateInitializeMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b) {
    const auto codeUnitSize = codeUnitWidth / 8;
    const auto pageSize = getPageSize();
    const auto minSize = stride * 4 * codeUnitSize;
    const auto desiredSize = round_up_to(minSize, pageSize);
    ConstantInt * const bufferBytes = b->getSize(desiredSize);
    Value * buffer = nullptr;
    if (codegen::ReadUsesCircularBuffer) {
        Module * m = b->getModule();
        FixedArray<Value *, 2> makeArgs;
        makeArgs[0] = bufferBytes;
        makeArgs[1] = b->getSize(0);
        Function * makeBuffer = m->getFunction(__MAKE_CIRCULAR_BUFFER); assert (makeBuffer);
        buffer = b->CreateCall(makeBuffer, makeArgs);
    } else {
        buffer = b->CreatePageAlignedMalloc(bufferBytes);
    }
    PointerType * const codeUnitPtrTy = b->getIntNTy(codeUnitWidth)->getPointerTo();
    buffer = b->CreatePointerCast(buffer, codeUnitPtrTy);
    b->setBaseAddress("sourceBuffer", buffer);
    b->setScalarField("buffer", buffer);
    b->setScalarField("ancillaryBuffer", ConstantPointerNull::get(codeUnitPtrTy));
    ConstantInt * const bufferItems = b->getSize(desiredSize / codeUnitSize);
    b->setScalarField("effectiveCapacity", bufferItems);
    b->setCapacity("sourceBuffer", bufferItems);
}

void ReadSourceKernel::generateDoSegmentMethod(const unsigned codeUnitWidth, const unsigned stride, BuilderRef b) {

    Value * const numOfStrides = b->getNumOfStrides();
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
        b->CreateAssert(b->CreateIsNotNull(numOfStrides),
                        "Internal error: %s.numOfStrides cannot be 0", b->GetString("ReadSource"));
    }

    Value * const segmentItems = b->CreateMul(numOfStrides, b->getSize(stride));
    ConstantInt * const codeUnitBytes = b->getSize(codeUnitWidth / 8);
    Type * codeUnitTy = b->getIntNTy(codeUnitWidth);
    Value * const segmentBytes = b->CreateMul(segmentItems, codeUnitBytes);

    BasicBlock * const entryBB = b->GetInsertBlock();
    BasicBlock * const expandAndCopyBack = b->CreateBasicBlock("ExpandAndCopyBack");
    BasicBlock * const afterCopyBackOrExpand = b->CreateBasicBlock("AfterCopyBackOrExpand");
    BasicBlock * const readData = b->CreateBasicBlock("ReadData");
    BasicBlock * const readIncomplete = b->CreateBasicBlock("readIncomplete");
    BasicBlock * const setTermination = b->CreateBasicBlock("SetTermination");
    BasicBlock * const readExit = b->CreateBasicBlock("ReadExit");


    // Can we append to our existing buffer without impacting any subsequent kernel?
    Value * const produced = b->getProducedItemCount("sourceBuffer");
    Value * const itemsPending = b->CreateAdd(produced, segmentItems);
    Value * const baseBuffer = b->getScalarField("buffer");
    Value * const fd = b->getScalarField("fileDescriptor");

    Value * const effectiveCapacity = b->getScalarField("effectiveCapacity");

    IntegerType * const sizeTy = b->getSizeTy();

    if (codegen::ReadUsesCircularBuffer) {

        Value * const consumedItems = b->getConsumedItemCount("sourceBuffer");
        Value * const required = b->CreateSub(itemsPending, consumedItems);

        Value * const permitted = b->CreateICmpULT(required, effectiveCapacity);
        b->CreateLikelyCondBr(permitted, afterCopyBackOrExpand, expandAndCopyBack);

        // Otherwise, allocate a buffer with twice the capacity and copy the unconsumed data back into it
        b->SetInsertPoint(expandAndCopyBack);

        Value * const expandedCapacity = b->CreateRoundUp(required, effectiveCapacity);
        Value * const expandedBytes = b->CreateMul(expandedCapacity, codeUnitBytes);

        Module * m = b->getModule();
        FixedArray<Value *, 2> makeArgs;
        makeArgs[0] = expandedBytes;
        makeArgs[1] = b->getSize(0);
        Function * makeBuffer = m->getFunction(__MAKE_CIRCULAR_BUFFER); assert (makeBuffer);
        Value * expandedBuffer = b->CreateCall(makeBuffer, makeArgs);

        Value * const priorBuffer = b->getScalarField("ancillaryBuffer");
        Value * const priorCapacity = b->getScalarField("ancillaryCapacity");
        Function * destroyBuffer = m->getFunction(__DESTROY_CIRCULAR_BUFFER); assert (makeBuffer);
        FixedArray<Value *, 3> destroyArgs;
        destroyArgs[0] = b->CreatePointerCast(priorBuffer, b->getInt8PtrTy());
        destroyArgs[1] = b->CreateMul(priorCapacity, codeUnitBytes);
        destroyArgs[2] = b->getSize(0);
        b->CreateCall(destroyBuffer, destroyArgs);

        b->setScalarField("ancillaryBuffer", baseBuffer);
        b->setScalarField("ancillaryCapacity", effectiveCapacity);

        Value * newBufferOffset = b->CreateURem(consumedItems, expandedCapacity);
        Value * const toCopyPtr = b->CreateInBoundsGEP(codeUnitTy, expandedBuffer, newBufferOffset);

        Value * const oldBufferOffset = b->CreateURem(consumedItems, effectiveCapacity);
        Value * const unreadDataPtr = b->CreateInBoundsGEP(codeUnitTy, baseBuffer, oldBufferOffset);

        Value * const unreadItems = b->CreateSub(produced, consumedItems);
        Value * const remainingBytes = b->CreateMul(unreadItems, codeUnitBytes);

        b->CreateMemCpy(toCopyPtr, unreadDataPtr, remainingBytes, 1);

        b->setScalarField("buffer", expandedBuffer);
        b->setScalarField("effectiveCapacity", expandedCapacity);

        b->CreateBr(afterCopyBackOrExpand);

        b->SetInsertPoint(afterCopyBackOrExpand);
        PHINode * const bufferPhi = b->CreatePHI(unreadDataPtr->getType(), 2);
        bufferPhi->addIncoming(baseBuffer, entryBB);
        bufferPhi->addIncoming(expandedBuffer, expandAndCopyBack);
        PHINode * const capacityPhi = b->CreatePHI(sizeTy, 2);
        capacityPhi->addIncoming(effectiveCapacity, entryBB);
        capacityPhi->addIncoming(expandedCapacity, expandAndCopyBack);

        Value * consumedOffset = b->CreateSub(b->CreateURem(consumedItems, capacityPhi), consumedItems);
        Value * const newBaseAddress = b->CreateInBoundsGEP(codeUnitTy, bufferPhi, consumedOffset);
        b->setBaseAddress("sourceBuffer", newBaseAddress);

        b->CreateBr(readData);


    } else {

        BasicBlock * const moveData = b->CreateBasicBlock("MoveData", expandAndCopyBack);

        Value * const permitted = b->CreateICmpULT(itemsPending, effectiveCapacity);
        b->CreateLikelyCondBr(permitted, readData, moveData);

        // No. If we can copy the unconsumed data back to the start of the buffer *and* write a full
        // segment of data without overwriting the currently unconsumed data, do so since it won't
        // affect any potential consumer that could be using the "stale" output base pointer.
        b->SetInsertPoint(moveData);

        // Determine how much data has been consumed and how much needs to be copied back, noting
        // that our "unproduced" data must be block aligned.
        Value * capacity = b->getCapacity("sourceBuffer");

        const auto blockSize = b->getBitBlockWidth() / 8;
        Value * const consumedItems = b->getConsumedItemCount("sourceBuffer");
        ConstantInt * const BLOCK_WIDTH = b->getSize(b->getBitBlockWidth());
        Constant * const ALIGNMENT_MASK = ConstantExpr::getNeg(BLOCK_WIDTH);
        Value * const consumed = b->CreateAnd(consumedItems, ALIGNMENT_MASK);

        Value * const unreadItems = b->CreateSub(produced, consumed);
        Value * const unreadData = b->getRawOutputPointer("sourceBuffer", consumed);
        Value * const potentialItems = b->CreateAdd(unreadItems, segmentItems);

        Value * const toWrite = b->CreateGEP(codeUnitTy, baseBuffer, potentialItems);
        Value * const canCopy = b->CreateICmpULT(toWrite, unreadData);

        Value * const remainingBytes = b->CreateMul(unreadItems, codeUnitBytes);

        BasicBlock * const copyBack = b->CreateBasicBlock("CopyBack", expandAndCopyBack);

        // Have we consumed enough data that we can safely copy back the unconsumed data and still
        // leave enough space for one segment without needing a temporary buffer?
        b->CreateLikelyCondBr(canCopy, copyBack, expandAndCopyBack);

        // If so, just copy the data ...
        b->SetInsertPoint(copyBack);
        b->CreateMemCpy(baseBuffer, unreadData, remainingBytes, blockSize);

        // Since our consumed count cannot exceed the effective capacity, in order for (consumed % capacity)
        // to be less than (effective capacity % capacity), we must have fully read all the data past the
        // effective capacity of the buffer. Thus we can set the effective capacity to the buffer capacity.
        // If, however, (consumed % capacity) >= (effective capacity % capacity), then we still have some
        // unconsumed data at the end of the buffer. Here, we can set the reclaimed capacity position to
        // (consumed % capacity).

        Value * const consumedModCap = b->CreateURem(consumed, capacity);
        Value * const effectiveCapacityModCap = b->CreateURem(effectiveCapacity, capacity);
        Value * const reclaimCapacity = b->CreateICmpULT(consumedModCap, effectiveCapacityModCap);
        Value * const reclaimedCapacity = b->CreateSelect(reclaimCapacity, capacity, consumedModCap);

        Value * const updatedEffectiveCapacity = b->CreateAdd(consumed, reclaimedCapacity);
        b->setScalarField("effectiveCapacity", updatedEffectiveCapacity);
        BasicBlock * const copyBackExit = b->GetInsertBlock();
        b->CreateBr(afterCopyBackOrExpand);

        // Otherwise, allocate a buffer with twice the capacity and copy the unconsumed data back into it
        b->SetInsertPoint(expandAndCopyBack);
        Value * const expandedCapacity = b->CreateShl(capacity, 1);
        Value * const expandedBytes = b->CreateMul(expandedCapacity, codeUnitBytes);

        Value * expandedBuffer = b->CreatePageAlignedMalloc(expandedBytes);
        b->CreateMemCpy(expandedBuffer, unreadData, remainingBytes, blockSize);

        // Free the prior buffer if it exists
        Value * const ancillaryBuffer = b->getScalarField("ancillaryBuffer");
        b->setScalarField("ancillaryBuffer", baseBuffer);
        b->CreateFree(ancillaryBuffer);

        expandedBuffer = b->CreatePointerCast(expandedBuffer, unreadData->getType());

        b->setScalarField("buffer", expandedBuffer);
        b->setCapacity("sourceBuffer", expandedCapacity);
        Value * const expandedEffectiveCapacity = b->CreateAdd(consumed, expandedCapacity);
        b->setScalarField("effectiveCapacity", expandedEffectiveCapacity);
        BasicBlock * const expandAndCopyBackExit = b->GetInsertBlock();
        b->CreateBr(afterCopyBackOrExpand);

        b->SetInsertPoint(afterCopyBackOrExpand);
        PHINode * const newBaseBuffer = b->CreatePHI(baseBuffer->getType(), 2);
        newBaseBuffer->addIncoming(baseBuffer, copyBackExit);
        newBaseBuffer->addIncoming(expandedBuffer, expandAndCopyBackExit);
        Value * const newBaseAddress = b->CreateGEP(codeUnitTy, newBaseBuffer, b->CreateNeg(consumed));
        b->setBaseAddress("sourceBuffer", newBaseAddress);
        b->CreateBr(readData);

    }

    // Regardless of whether we're simply appending data or had to allocate a new buffer, read a new page
    // of data into the input source buffer. This may involve multiple read calls.
    b->SetInsertPoint(readData);
    PHINode * const bytesToRead = b->CreatePHI(sizeTy, 2);
    if (!codegen::ReadUsesCircularBuffer) {
        bytesToRead->addIncoming(segmentBytes, entryBB);
    }
    bytesToRead->addIncoming(segmentBytes, afterCopyBackOrExpand);
    PHINode * const producedSoFar = b->CreatePHI(sizeTy, 2);
    if (!codegen::ReadUsesCircularBuffer) {
        producedSoFar->addIncoming(produced, entryBB);
    }
    producedSoFar->addIncoming(produced, afterCopyBackOrExpand);

    Value * const sourceBuffer = b->getRawOutputPointer("sourceBuffer", producedSoFar);
    #ifdef PREAD
    FixedArray<Value *, 4> args;
    args[0] = fd;
    args[1] = sourceBuffer;
    args[2] = bytesToRead;
    args[3] = producedSoFar;
    Function *  const preadFunc = b->getModule()->getFunction("pread64");
    #else
    FixedArray<Value *, 3> args;
    args[0] = fd;
    args[1] = sourceBuffer;
    args[2] = bytesToRead;
    Function *  const preadFunc = b->getModule()->getFunction("read");
    #endif
    Value * const bytesRead = b->CreateCall(preadFunc, args);

    // There are 4 possibile results from read:
    // bytesRead == -1: an error occurred
    // bytesRead == 0: EOF, no bytes read
    // 0 < bytesRead < bytesToRead:  some data read (more may be available)
    // bytesRead == bytesToRead, the full amount requested was read.
    b->CreateUnlikelyCondBr(b->CreateICmpNE(bytesToRead, bytesRead), readIncomplete, readExit);

    b->SetInsertPoint(readIncomplete);
    // Keep reading until a the full stride is read, or there is no more data.
    Value * moreToRead = b->CreateSub(bytesToRead, bytesRead);
    Value * readSoFar = b->CreateSub(segmentBytes, moreToRead);
    Value * const itemsRead = b->CreateUDiv(readSoFar, codeUnitBytes);
    Value * const itemsBuffered = b->CreateAdd(produced, itemsRead);
    bytesToRead->addIncoming(moreToRead, readIncomplete);
    producedSoFar->addIncoming(itemsBuffered, readIncomplete);
    b->CreateCondBr(b->CreateICmpSGT(bytesRead, b->getSize(0)), readData, setTermination);

    // ... set the termination signal.
    b->SetInsertPoint(setTermination);
    Value * const bytesToZero = b->CreateMul(b->CreateSub(itemsPending, itemsBuffered), codeUnitBytes);
    b->CreateMemZero(b->getRawOutputPointer("sourceBuffer", itemsBuffered), bytesToZero);
    b->setScalarField("fileItems", itemsBuffered);
    b->setTerminationSignal();
    b->setProducedItemCount("sourceBuffer", itemsBuffered);
    b->CreateBr(readExit);

    b->SetInsertPoint(readExit);
}

void ReadSourceKernel::freeBuffer(const unsigned codeUnitWidth, BuilderRef b) {



    if (codegen::ReadUsesCircularBuffer) {
        Module * m = b->getModule();
        ConstantInt * const codeUnitBytes = b->getSize(codeUnitWidth / 8);
        Value * const buffer = b->getScalarField("buffer");
        Value * const capacity = b->getScalarField("effectiveCapacity");
        Function * destroyBuffer = m->getFunction(__DESTROY_CIRCULAR_BUFFER);
        FixedArray<Value *, 3> destroyArgs;
        destroyArgs[0] = b->CreatePointerCast(buffer, b->getInt8PtrTy());
        destroyArgs[1] = b->CreateMul(capacity, codeUnitBytes);
        destroyArgs[2] = b->getSize(0);
        b->CreateCall(destroyBuffer, destroyArgs);
        Value * const priorBuffer = b->getScalarField("ancillaryBuffer");
        Value * const priorCapacity = b->getScalarField("ancillaryCapacity");
        destroyArgs[0] = b->CreatePointerCast(priorBuffer, b->getInt8PtrTy());
        destroyArgs[1] = b->CreateMul(priorCapacity, codeUnitBytes);
        b->CreateCall(destroyBuffer, destroyArgs);
    } else {
        b->CreateFree(b->getScalarField("ancillaryBuffer"));
        b->CreateFree(b->getScalarField("buffer"));
    }



}

Value * ReadSourceKernel::generateExpectedOutputSizeMethod(const unsigned codeUnitWidth, BuilderRef b) {
    Value * const fd = b->getScalarField("fileDescriptor");
    Function * const fileSizeFn = b->getModule()->getFunction("file_size"); assert (fileSizeFn);
    FunctionType * fTy = fileSizeFn->getFunctionType();
    return b->CreateZExtOrTrunc(b->CreateCall(fTy, fileSizeFn, fd), b->getSizeTy());
}

void ReadSourceKernel::linkExternalMethods(BuilderRef b) {
    ReadSourceKernel::generatLinkExternalFunctions(b);
}

/// Hybrid MMap/Read source kernel

void FDSourceKernel::generateFinalizeMethod(BuilderRef b) {
    BasicBlock * finalizeRead = b->CreateBasicBlock("finalizeRead");
    BasicBlock * finalizeMMap = b->CreateBasicBlock("finalizeMMap");
    BasicBlock * finalizeDone = b->CreateBasicBlock("finalizeDone");
    Value * const useMMap = b->CreateIsNotNull(b->getScalarField("useMMap"));
    b->CreateCondBr(useMMap, finalizeMMap, finalizeRead);
    b->SetInsertPoint(finalizeMMap);
    MMapSourceKernel::freeBuffer(b, mCodeUnitWidth);
    b->CreateBr(finalizeDone);
    b->SetInsertPoint(finalizeRead);
    ReadSourceKernel::freeBuffer(mCodeUnitWidth, b);
    b->CreateBr(finalizeDone);
    b->SetInsertPoint(finalizeDone);
}

void FDSourceKernel::generateInitializeMethod(BuilderRef b) {
    BasicBlock * initializeRead = b->CreateBasicBlock("initializeRead");
    BasicBlock * checkFileSize = b->CreateBasicBlock("checkFileSize");
    BasicBlock * initializeMMap = b->CreateBasicBlock("initializeMMap");
    BasicBlock * initializeDone = b->CreateBasicBlock("initializeDone");

    // The source will use MMapSource or readSoure kernel logic depending on the useMMap
    // parameter, possibly overridden.

    Value * const useMMap = b->getScalarField("useMMap");
    // if the fileDescriptor is 0, the file is stdin, use readSource kernel logic.
    Value * const fd = b->getScalarField("fileDescriptor");
    Value * const notStdIn = b->CreateICmpNE(fd, b->getInt32(STDIN_FILENO));
    Value * const tryMMap = b->CreateAnd(b->CreateIsNotNull(useMMap), notStdIn);
    b->CreateCondBr(tryMMap, checkFileSize, initializeRead);

    b->SetInsertPoint(checkFileSize);
    // If the fileSize is 0, we may have a virtual file such as /proc/cpuinfo
    Function * const fileSizeFn = b->getModule()->getFunction("file_size");
    assert (fileSizeFn);
    FunctionType * fTy = fileSizeFn->getFunctionType();
    Value * const fileSize = b->CreateCall(fTy, fileSizeFn, fd);
    Value * const emptyFile = b->CreateIsNull(fileSize);
    b->CreateUnlikelyCondBr(emptyFile, initializeRead, initializeMMap);

    b->SetInsertPoint(initializeMMap);
    MMapSourceKernel::generateInitializeMethod(mCodeUnitWidth, mStride, b);
    b->CreateBr(initializeDone);

    b->SetInsertPoint(initializeRead);
    // Ensure that readSource logic is used throughout.
    b->setScalarField("useMMap", ConstantInt::getNullValue(useMMap->getType()));
    ReadSourceKernel::generateInitializeMethod(mCodeUnitWidth, mStride,b);
    b->CreateBr(initializeDone);

    b->SetInsertPoint(initializeDone);
}

void FDSourceKernel::generateDoSegmentMethod(BuilderRef b) {
    BasicBlock * DoSegmentRead = b->CreateBasicBlock("DoSegmentRead");
    BasicBlock * DoSegmentMMap = b->CreateBasicBlock("DoSegmentMMap");
    BasicBlock * DoSegmentDone = b->CreateBasicBlock("DoSegmentDone");
    Value * const useMMap = b->CreateIsNotNull(b->getScalarField("useMMap"));

    b->CreateCondBr(useMMap, DoSegmentMMap, DoSegmentRead);
    b->SetInsertPoint(DoSegmentMMap);
    MMapSourceKernel::generateDoSegmentMethod(mCodeUnitWidth, mStride, b);
    b->CreateBr(DoSegmentDone);
    b->SetInsertPoint(DoSegmentRead);
    ReadSourceKernel::generateDoSegmentMethod(mCodeUnitWidth, mStride, b);
    b->CreateBr(DoSegmentDone);
    b->SetInsertPoint(DoSegmentDone);
}

Value * FDSourceKernel::generateExpectedOutputSizeMethod(BuilderRef b) {
    BasicBlock * finalizeRead = b->CreateBasicBlock("finalizeRead");
    BasicBlock * finalizeMMap = b->CreateBasicBlock("finalizeMMap");
    BasicBlock * finalizeDone = b->CreateBasicBlock("finalizeDone");
    Value * const useMMap = b->CreateIsNotNull(b->getScalarField("useMMap"));
    b->CreateCondBr(useMMap, finalizeMMap, finalizeRead);
    b->SetInsertPoint(finalizeMMap);
    Value * mmapVal = MMapSourceKernel::generateExpectedOutputSizeMethod(mCodeUnitWidth, b);
    b->CreateBr(finalizeDone);
    b->SetInsertPoint(finalizeRead);
    Value * readVal = ReadSourceKernel::generateExpectedOutputSizeMethod(mCodeUnitWidth, b);
    b->CreateBr(finalizeDone);
    b->SetInsertPoint(finalizeDone);
    PHINode * const resultPhi = b->CreatePHI(b->getSizeTy(), 2);
    resultPhi->addIncoming(mmapVal, finalizeMMap);
    resultPhi->addIncoming(readVal, finalizeRead);
    return resultPhi;
}


void FDSourceKernel::linkExternalMethods(BuilderRef b) {
    MMapSourceKernel::generatLinkExternalFunctions(b);
    ReadSourceKernel::generatLinkExternalFunctions(b);
}

/// MEMORY SOURCE KERNEL

void MemorySourceKernel::generateInitializeMethod(BuilderRef b) {
    Value * const fileSource = b->getScalarField("fileSource");
    b->setBaseAddress("sourceBuffer", fileSource);
    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
        b->CreateAssert(fileSource, getName() + " fileSource cannot be null");
    }
    Value * const fileItems = b->getScalarField("fileItems");
    b->setCapacity("sourceBuffer", fileItems);
}

void MemorySourceKernel::generateDoSegmentMethod(BuilderRef b) {

    Value * const numOfStrides = b->getNumOfStrides();

    if (LLVM_UNLIKELY(codegen::DebugOptionIsSet(codegen::EnableAsserts))) {
        b->CreateAssert(b->CreateIsNotNull(numOfStrides),
                        "Internal error: %s.numOfStrides cannot be 0", b->GetString(getName()));
    }

    Value * const segmentItems = b->CreateMul(numOfStrides, b->getSize(getStride()));
    BasicBlock * const createTemporary = b->CreateBasicBlock("createTemporary");
    BasicBlock * const exit = b->CreateBasicBlock("exit");

    Value * const fileItems = b->getScalarField("fileItems");
    Value * const producedItems = b->getProducedItemCount("sourceBuffer");
    Value * const nextProducedItems = b->CreateAdd(producedItems, segmentItems);
    Value * const lastPage = b->CreateICmpULE(fileItems, nextProducedItems);
    b->CreateUnlikelyCondBr(lastPage, createTemporary, exit);

    b->SetInsertPoint(createTemporary);
    b->setTerminationSignal();
    b->setProducedItemCount("sourceBuffer", fileItems);
    b->CreateBr(exit);

    b->SetInsertPoint(exit);
}

void MemorySourceKernel::generateFinalizeMethod(BuilderRef b) {
    b->CreateFree(b->getScalarField("ancillaryBuffer"));
}

Value * MemorySourceKernel::generateExpectedOutputSizeMethod(BuilderRef b) {
    return b->getScalarField("fileItems");
}

std::string makeSourceName(StringRef prefix, const unsigned fieldWidth, const unsigned numOfStreams = 1U) {
    std::string tmp;
    tmp.reserve(64);
    llvm::raw_string_ostream out(tmp);
    out << prefix << codegen::SegmentSize << '@' << fieldWidth;
    if (numOfStreams != 1) {
        out << ':' << numOfStreams;
    }
    out.flush();
    return tmp;
}

MMapSourceKernel::MMapSourceKernel(BuilderRef b, Scalar * const fd, StreamSet * const outputStream)
: SegmentOrientedKernel(b, makeSourceName("mmap_source" + std::to_string(codegen::MMapPrefetchType), outputStream->getFieldWidth())
// input streams
,{}
// output streams
,{Binding{"sourceBuffer", outputStream, FixedRate(), { ManagedBuffer(), Linear() }}}
// input scalars
,{Binding{"fileDescriptor", fd}}
// output scalars
,{Binding{b->getSizeTy(), "fileItems"}}
// internal scalars
,{})
, mCodeUnitWidth(outputStream->getFieldWidth()) {
    PointerType * const codeUnitPtrTy = b->getIntNTy(mCodeUnitWidth)->getPointerTo();
    addInternalScalar(codeUnitPtrTy, "buffer");
    addInternalScalar(codeUnitPtrTy, "ancillaryBuffer");
    addAttribute(MustExplicitlyTerminate());
    addAttribute(SideEffecting());
    setStride(codegen::SegmentSize);
}

ReadSourceKernel::ReadSourceKernel(BuilderRef b, Scalar * const fd, StreamSet * const outputStream)
: SegmentOrientedKernel(b, makeSourceName(std::string("read_source") + (codegen::ReadUsesCircularBuffer ? 'C' : 'L'), outputStream->getFieldWidth())
// input streams
,{}
// output streams
,{Binding{"sourceBuffer", outputStream, FixedRate(), { ManagedBuffer(), Linear() }}}
// input scalars
,{Binding{"fileDescriptor", fd}}
// output scalars
,{Binding{b->getSizeTy(), "fileItems"}}
// internal scalars
,{})
, mCodeUnitWidth(outputStream->getFieldWidth()) {
    PointerType * const codeUnitPtrTy = b->getIntNTy(mCodeUnitWidth)->getPointerTo();
    addInternalScalar(codeUnitPtrTy, "buffer");
    addInternalScalar(codeUnitPtrTy, "ancillaryBuffer");
    IntegerType * const sizeTy = b->getSizeTy();
    addInternalScalar(sizeTy, "effectiveCapacity");
    if (codegen::ReadUsesCircularBuffer) {
        addInternalScalar(sizeTy, "ancillaryCapacity");
    }
    addAttribute(MustExplicitlyTerminate());
    addAttribute(SideEffecting());
    setStride(codegen::SegmentSize);
}



FDSourceKernel::FDSourceKernel(BuilderRef b, Scalar * const useMMap, Scalar * const fd, StreamSet * const outputStream)
: SegmentOrientedKernel(b, makeSourceName("FD_source" + std::to_string(codegen::MMapPrefetchType) + (codegen::ReadUsesCircularBuffer ? 'C' : 'L'), outputStream->getFieldWidth())
// input streams
,{}
// output stream
,{Binding{"sourceBuffer", outputStream, FixedRate(), { ManagedBuffer(), Linear() }}}
// input scalar
,{Binding{"useMMap", useMMap}
, Binding{"fileDescriptor", fd}}
// output scalar
,{Binding{b->getSizeTy(), "fileItems"}}
// internal scalars
,{})
, mCodeUnitWidth(outputStream->getFieldWidth()) {
    PointerType * const codeUnitPtrTy = b->getIntNTy(mCodeUnitWidth)->getPointerTo();
    addInternalScalar(codeUnitPtrTy, "buffer");
    addInternalScalar(codeUnitPtrTy, "ancillaryBuffer");
    IntegerType * const sizeTy = b->getSizeTy();
    addInternalScalar(sizeTy, "effectiveCapacity");
    if (codegen::ReadUsesCircularBuffer) {
        addInternalScalar(sizeTy, "ancillaryCapacity");
    }
    addAttribute(MustExplicitlyTerminate());
    addAttribute(SideEffecting());
    setStride(codegen::SegmentSize);
}

MemorySourceKernel::MemorySourceKernel(BuilderRef b, Scalar * fileSource, Scalar * fileItems, StreamSet * const outputStream)
: SegmentOrientedKernel(b, makeSourceName("memory_source", outputStream->getFieldWidth(), outputStream->getNumElements()),
// input streams
{},
// output stream
{Binding{"sourceBuffer", outputStream, FixedRate(), { ManagedBuffer(), Linear() }}},
// input scalar
{Binding{"fileSource", fileSource}, Binding{"fileItems", fileItems}},
{},
// internal scalar
{}) {
    addAttribute(MustExplicitlyTerminate());
    addInternalScalar(fileSource->getType(), "ancillaryBuffer");
    setStride(codegen::SegmentSize);
}

}
