#ifndef COMMON_H
#define COMMON_H

#include <pablo/pablo_kernel.h>
#include <kernel/core/kernel_builder.h>
#include "ztf-logic.h"

using namespace llvm;
using namespace kernel;

using BuilderRef = Kernel::BuilderRef;

const unsigned BITS_PER_BYTE = 8;
const unsigned SIZE_T_BITS = sizeof(size_t) * BITS_PER_BYTE;

struct ScanWordParameters {
    unsigned width;
    unsigned indexWidth;
    Type * const Ty;
    Type * const pointerTy;
    Constant * const WIDTH;
    Constant * const ix_MAXBIT;
    Constant * WORDS_PER_BLOCK;
    Constant * WORDS_PER_STRIDE;

    ScanWordParameters(BuilderRef b, unsigned stride);
};

struct LengthGroupParameters {
    LengthGroupInfo groupInfo;
    Constant * MAX_HASH_BITS;
    Constant * HASH_SHIFT_BITS;
    Constant * SUFFIX_BITS;
    Constant * SUFFIX_MASK;
    Constant * LAST_SUFFIX_BASE;
    Constant * LAST_SUFFIX_SHIFT_BITS;
    Constant * LAST_SUFFIX_MASK;
    unsigned const groupHalfLength;
    Type * halfLengthTy;
    Type * halfSymPtrTy;
    Type * symLengthTy;
    Type * symLengthPtrTy;
    Constant * HALF_LENGTH;
    Constant * LO;
    Constant * HI;
    Constant * RANGE;
    // All subtables are sized the same.
    Constant * SUBTABLE_SIZE;
    Constant * PHRASE_SUBTABLE_SIZE;
    Constant * FREQ_SUBTABLE_SIZE;
    Constant * HASH_BITS;
    Constant * EXTENDED_BITS;
    Constant * PHRASE_EXTENSION_MASK;
    Constant * HASH_MASK;
    Constant * HASH_MASK_NEW;
    Constant * ENC_BYTES;
    Constant * MAX_INDEX;
    Constant * PREFIX_BASE;
    Constant * PREFIX_LENGTH_OFFSET;
    Constant * PREFIX_LENGTH_MASK;
    Constant * LENGTH_MASK;
    Constant * EXTENSION_MASK;
    Constant * TABLE_MASK;
    Constant * EXTRA_BITS;
    Constant * EXTRA_BITS_MASK;
    Constant * TABLE_IDX_MASK;

    LengthGroupParameters(BuilderRef b, EncodingInfo encodingScheme, unsigned groupNo, unsigned numSym = 0);
};

unsigned hashTableSize(LengthGroupInfo g);
unsigned phraseHashSubTableSize(EncodingInfo encodingScheme, unsigned groupNo);
unsigned phraseHashTableSize(LengthGroupInfo g);
unsigned freqTableSize(LengthGroupInfo g);
std::string lengthRangeSuffix(EncodingInfo encodingScheme, unsigned lo, unsigned hi);
std::string lengthGroupSuffix(EncodingInfo encodingScheme, unsigned groupNo);
std::vector<llvm::Value *> initializeCompressionMasks(Kernel::BuilderRef b,
                                                    struct ScanWordParameters & sw,
                                                    llvm::Constant * sz_BLOCKS_PER_STRIDE,
                                                    unsigned maskCount,
                                                    llvm::Value * strideBlockOffset,
                                                    llvm::Value * compressMaskPtr,
                                                    llvm::BasicBlock * strideMasksReady);
std::vector<llvm::Value *> initializeCompressionMasks(Kernel::BuilderRef b,
                                                    struct ScanWordParameters & sw,
                                                    llvm::Constant * sz_BLOCKS_PER_STRIDE,
                                                    unsigned maskCount,
                                                    llvm::Value * strideBlockOffset,
                                                    llvm::Value * compressMaskPtr,
                                                    llvm::Value * phraseMaskPtr,
                                                    llvm::BasicBlock * strideMasksReady);
std::vector<llvm::Value *> initializeCompressionMasks1(Kernel::BuilderRef b,
                                                    struct ScanWordParameters & sw,
                                                    llvm::Constant * sz_BLOCKS_PER_STRIDE,
                                                    unsigned maskCount,
                                                    llvm::Value * readStrideBlockOffset,
                                                    llvm::BasicBlock * strideMasksReady);
std::vector<llvm::Value *> initializeCompressionMasks2(Kernel::BuilderRef b,
                                                    struct ScanWordParameters & sw,
                                                    llvm::Constant * sz_BLOCKS_PER_STRIDE,
                                                    unsigned maskCount,
                                                    llvm::Value * strideBlockOffset,
                                                    llvm::Value * dictMaskPtr,
                                                    llvm::BasicBlock * strideMasksReady);
void initializeDecompressionMasks(Kernel::BuilderRef b,
                                                    struct ScanWordParameters & sw,
                                                    llvm::Constant * sz_BLOCKS_PER_STRIDE,
                                                    unsigned maskCount,
                                                    llvm::Value * strideBlockOffset,
                                                    std::vector<Value *> & keyMasks,
                                                    std::vector<Value *> & hashMasks,
                                                    llvm::BasicBlock * strideMasksReady);
void initializeCodeWordMasks(Kernel::BuilderRef b,
                                                    struct ScanWordParameters & sw,
                                                    llvm::Constant * sz_BLOCKS_PER_STRIDE,
                                                    unsigned maskCount,
                                                    llvm::Value * strideBlockOffset,
                                                    std::vector<Value *> & keyMasks0,
                                                    std::vector<Value *> & keyMasks1,
                                                    std::vector<Value *> & hashMasks,
                                                    llvm::BasicBlock * strideMasksReady);
void initializeOutputMasks(Kernel::BuilderRef b,
                           ScanWordParameters & sw,
                           llvm::Constant * sz_BLOCKS_PER_STRIDE,
                           llvm::Value * strideBlockOffset,
                           llvm::Value * outputMaskPtr,
                           llvm::BasicBlock * outputMasksReady);


Value * getLastLineBreakPos(Kernel::BuilderRef b,
                           ScanWordParameters & sw,
                           llvm::Constant * sz_BLOCKWIDTH,
                           llvm::Value * sz_BLOCKS_PER_STRIDE,
                           llvm::Value * strideBlockOffset,
                           llvm::BasicBlock * outputMasksReady);

Value * getSegBoundaryPos(Kernel::BuilderRef b,
                           ScanWordParameters & sw,
                           llvm::Constant * sz_BLOCKWIDTH,
                           llvm::Constant * sz_BLOCKS_PER_STRIDE,
                           llvm::Value * strideBlockOffset,
                           llvm::BasicBlock * outputMasksReady);

bool LLVM_READONLY DeferredAttributeIsSet();
bool LLVM_READONLY DelayedAttributeIsSet();
bool LLVM_READONLY PrefixCheckIsSet();

#endif
