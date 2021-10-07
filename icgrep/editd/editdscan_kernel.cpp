/*
 *  Copyright (c) 2015 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */


#include "editdscan_kernel.h"
#include <kernels/kernel_builder.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace kernel {

void editdScanKernel::generateDoBlockMethod(const std::unique_ptr<kernel::KernelBuilder> & idb) {
    auto savePoint = idb->saveIP();
    Function * scanWordFunction = generateScanWordRoutine(idb);
    idb->restoreIP(savePoint);

    const unsigned fieldCount = idb->getBitBlockWidth() / mScanwordBitWidth;
    Type * T = idb->getIntNTy(mScanwordBitWidth);
    VectorType * scanwordVectorType =  FixedVectorType::get(T, fieldCount);
    Value * blockNo = idb->getScalarField("BlockNo");
    Value * scanwordPos = idb->CreateMul(blockNo, ConstantInt::get(blockNo->getType(), idb->getBitBlockWidth()));
    
    std::vector<Value * > matchWordVectors;
    for(unsigned d = 0; d <= mEditDistance; d++) {
        Value * matches = idb->loadInputStreamBlock("matchResults", idb->getInt32(d));
        matchWordVectors.push_back(idb->CreateBitCast(matches, scanwordVectorType));
    }
    
    for(unsigned i = 0; i < fieldCount; ++i) {
        for(unsigned d = 0; d <= mEditDistance; d++) {
            Value * matchWord = idb->CreateExtractElement(matchWordVectors[d], ConstantInt::get(T, i));
            idb->CreateCall(scanWordFunction, {matchWord, idb->getInt32(d), scanwordPos});
        }
        scanwordPos = idb->CreateAdd(scanwordPos, ConstantInt::get(T, mScanwordBitWidth));
    }

    idb->setScalarField("BlockNo", idb->CreateAdd(blockNo, idb->getSize(1)));
}

Function * editdScanKernel::generateScanWordRoutine(const std::unique_ptr<KernelBuilder> &iBuilder) const {

    IntegerType * T = iBuilder->getIntNTy(mScanwordBitWidth);
    Module * const m = iBuilder->getModule();

    FunctionType * scanFuncTy = FunctionType::get(iBuilder->getVoidTy(), { T, iBuilder->getInt32Ty(), T}, false);
    Function * scanFunc = m->getFunction("scan_word");
    if (scanFunc == nullptr) {
        scanFunc = Function::Create(scanFuncTy, Function::InternalLinkage, "scan_word", m);
        scanFunc->setCallingConv(CallingConv::C);
    }

    Function::arg_iterator args = scanFunc->arg_begin();

    Value * matchWord = &*(args++);
    matchWord->setName("matchWord");
    Value * dist = &*(args++);
    dist->setName("dist");
    Value * basePos = &*(args++);
    basePos->setName("basePos");


    FunctionType * matchProcessorTy = FunctionType::get(iBuilder->getVoidTy(), { T, iBuilder->getInt32Ty()}, false);
    Function * matchProcessor = m->getFunction("wrapped_report_pos");
    if (matchProcessor == nullptr) {
        matchProcessor = Function::Create(matchProcessorTy, Function::InternalLinkage, "wrapped_report_pos", m);
        matchProcessor->setCallingConv(CallingConv::C);
    }

    BasicBlock * entryBlock = BasicBlock::Create(iBuilder->getContext(), "entry", scanFunc);
    BasicBlock * matchesCondBlock = BasicBlock::Create(iBuilder->getContext(), "matchesCond", scanFunc);
    BasicBlock * matchesLoopBlock = BasicBlock::Create(iBuilder->getContext(), "matchesLoop", scanFunc);
    BasicBlock * matchesDoneBlock = BasicBlock::Create(iBuilder->getContext(), "matchesDone", scanFunc);

    iBuilder->SetInsertPoint(entryBlock);
    iBuilder->CreateBr(matchesCondBlock);

    iBuilder->SetInsertPoint(matchesCondBlock);
    PHINode * matches_phi = iBuilder->CreatePHI(T, 2, "matches");
    matches_phi->addIncoming(matchWord, entryBlock);
    Value * have_matches_cond = iBuilder->CreateICmpUGT(matches_phi, ConstantInt::get(T, 0));
    iBuilder->CreateCondBr(have_matches_cond, matchesLoopBlock, matchesDoneBlock);

    iBuilder->SetInsertPoint(matchesLoopBlock);
    Value * match_pos = iBuilder->CreateAdd(iBuilder->CreateCountForwardZeroes(matches_phi), basePos);
    Value * matches_new = iBuilder->CreateAnd(matches_phi, iBuilder->CreateSub(matches_phi, ConstantInt::get(T, 1)));
    matches_phi->addIncoming(matches_new, matchesLoopBlock);
    iBuilder->CreateCall(matchProcessor, std::vector<Value *>({match_pos, dist}));
    iBuilder->CreateBr(matchesCondBlock);

    iBuilder->SetInsertPoint(matchesDoneBlock);
    iBuilder -> CreateRetVoid();

    return scanFunc;

}

editdScanKernel::editdScanKernel(const std::unique_ptr<kernel::KernelBuilder> & iBuilder, unsigned dist) :
BlockOrientedKernel("scanMatch",
              {Binding{iBuilder->getStreamSetTy(dist + 1), "matchResults"}},
              {}, {}, {}, {Binding{iBuilder->getSizeTy(), "BlockNo"}}),
mEditDistance(dist),
mScanwordBitWidth(iBuilder->getSizeTy()->getBitWidth()) {

}

}
