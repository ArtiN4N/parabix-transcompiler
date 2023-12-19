#include "csv_validator_util.h"

#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/adt.h>
#include <re/unicode/resolve_properties.h>
#include <unicode/utf/utf_compiler.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>

#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pe_ones.h>
#include <pablo/pablo_toolchain.h>

#include <kernel/core/streamset.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include "../csv/csv_util.hpp"

#include <boost/intrusive/detail/math.hpp>

#include <llvm/Support/raw_os_ostream.h>

#include "csv_validator_toolchain.h"

using namespace pablo;
using namespace kernel;
using namespace cc;
using namespace re;
using namespace llvm;

using boost::intrusive::detail::floor_log2;

namespace kernel {

// enum {markLF = 0, markCR = 1, markDQ = 2, markComma = 3, markEOF = 4};


std::string CSVDataParser::makeNameFromOptions() {
    std::string tmp;
    raw_string_ostream nm(tmp);
    nm << "CSVDataParser";
    if (noHeaderLine) {
        nm << "NH";
    }
    nm.flush();
    return tmp;
}

void CSVDataLexer::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::unique_ptr<cc::CC_Compiler> ccc;
    ccc = std::make_unique<cc::Parabix_CC_Compiler_Builder>(getEntryScope(), getInputStreamSet("Source"));
    PabloAST * LF = ccc->compileCC(re::makeCC(charLF, &cc::UTF8));
    PabloAST * CR = ccc->compileCC(re::makeCC(charCR, &cc::UTF8));
    PabloAST * DQ = ccc->compileCC(re::makeCC(charDQ, &cc::UTF8));
    PabloAST * Comma = ccc->compileCC(re::makeCC(charComma, &cc::UTF8));
    PabloAST * EOFbit = pb.createAtEOF(pb.createOnes()); // pb.createAdvance(pb.createOnes(), 1));
    Var * lexOut = getOutputStreamVar("CSVlexical");
    // TODO: multiplex these?
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markLF)), LF);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markCR)), CR);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markDQ)), DQ);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markComma)), Comma);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markEOF)), EOFbit);
}

enum CSVDataFieldMarkers {
    FieldDataMask = 0
    , RecordSeparators = 1
};

void CSVDataParser::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> csvMarks = getInputStreamSet("csvMarks");
    PabloAST * dquote = csvMarks[markDQ];
    PabloAST * dquote_odd = pb.createEveryNth(dquote, pb.getInteger(2));
    PabloAST * dquote_even = pb.createXor(dquote, dquote_odd);
    PabloAST * quote_escape = pb.createAnd(dquote_even, pb.createLookahead(dquote, 1));
    PabloAST * escaped_quote = pb.createAdvance(quote_escape, 1);
    PabloAST * start_dquote = pb.createXor(dquote_odd, escaped_quote);
    PabloAST * end_dquote = pb.createXor(dquote_even, quote_escape);
    PabloAST * quoted_data = pb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, {start_dquote, end_dquote});
    PabloAST * unquoted = pb.createNot(quoted_data);
    PabloAST * recordSeparators = pb.createOrAnd(csvMarks[markEOF], csvMarks[markLF], unquoted, "recordSeparators");
    PabloAST * allSeparators = pb.createOrAnd(recordSeparators, csvMarks[markComma], unquoted, "allSeparators");
    PabloAST * CRofCRLF = pb.createAnd3(csvMarks[markCR], pb.createLookahead(csvMarks[markLF], 1), unquoted, "CRofCRLF");
    PabloAST * formattingQuotes = pb.createXor(dquote, escaped_quote, "formattingQuotes");
    PabloAST * nonText = pb.createOr3(CRofCRLF, formattingQuotes, quote_escape);

    PabloAST * recordSeparatorsAndNonText = pb.createOr(recordSeparators, nonText);
    PabloAST * start = pb.createAdvanceThenScanThru(recordSeparators, nonText);
    if (noHeaderLine) {
        start = pb.createNot(pb.createAdvance(pb.createNot(start), 1));
    } else {
        PabloAST * const afterHeader = pb.createSpanAfterFirst(recordSeparators, "afterHeader");
        allSeparators = pb.createAnd(allSeparators, afterHeader);
        recordSeparatorsAndNonText = pb.createOr(recordSeparatorsAndNonText, pb.createNot(afterHeader), "recordSeparatorsAndNonText");
        start = pb.createAnd(start, afterHeader);
    }

    Var * fd = getOutputStreamVar("fieldData");

    pb.createAssign(pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::RecordSeparatorsAndNonText)), recordSeparatorsAndNonText);
    pb.createAssign(pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::StartPositions)), start);

    pb.createAssign(pb.createExtract(getOutputStreamVar("allSeperators"), pb.getInteger(0)), allSeparators);

}

void IdentifyLastSelector::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    // TODO: we shouldn't need this kernel to obtain the last mark of each run of a selector_span
    // if we can push them ahead to the end of the run but that may add an N advances where N is
    // the hash code bit width.
    Integer * pb_ZERO = pb.getInteger(0);
    PabloAST * span = pb.createExtract(getInputStreamVar("selector_span"), pb_ZERO);
    PabloAST * la = pb.createLookahead(span, 1);
    PabloAST * selectors = pb.createAnd(span, pb.createNot(la));
    pb.createAssign(pb.createExtract(getOutputStreamVar("selectors"), pb_ZERO), selectors);
}


ExtractCoordinateSequence::ExtractCoordinateSequence(BuilderRef b,
                                               StreamSet * const Matches,
                                               StreamSet * const Coordinates, unsigned strideBlocks)
: MultiBlockKernel(b, "ExtractCoordinateSequence" + std::to_string(strideBlocks),
// inputs
{Binding{"markers", Matches}},
// outputs
{Bind("Coordinates", Coordinates, PopcountOf("markers"))},
// input scalars
{},
// output scalars
{},
// kernel state
{}) {
     assert (Matches->getNumElements() == 1);
     assert (Coordinates->getNumElements() == 1);
}

void ExtractCoordinateSequence::generateMultiBlockLogic(BuilderRef b, Value * const numOfStrides) {

    Constant * const sz_ZERO = b->getSize(0);
    Constant * const sz_ONE = b->getSize(1);
    IntegerType * sizeTy = b->getSizeTy();
    const auto sizeTyWidth = sizeTy->getBitWidth();

    Constant * const sz_BITS = b->getSize(sizeTyWidth);
    Constant * const sz_MAXBIT = b->getSize(sizeTyWidth - 1);

    Type * const blockTy = b->getBitBlockType();


    assert ((mStride % sizeTyWidth ) == 0);

    const auto vecsPerStride = mStride / sizeTyWidth;

    BasicBlock * const entryBlock = b->GetInsertBlock();

    // we expect that every block will have at least one marker

    BasicBlock * const stridePrologue = b->CreateBasicBlock("stridePrologue");
    BasicBlock * const strideCoordinateVecLoop = b->CreateBasicBlock("strideCoordinateVecLoop");
    BasicBlock * const strideCoordinateElemLoop = b->CreateBasicBlock("strideCoordinateElemLoop");
    BasicBlock * const strideCoordinateElemDone = b->CreateBasicBlock("strideCoordinateElemDone");
    BasicBlock * const strideCoordinateVecDone = b->CreateBasicBlock("strideCoordinateVecDone");
    BasicBlock * const strideCoordinatesDone = b->CreateBasicBlock("strideCoordinatesDone");

    Value * const processedMarkers = b->getProcessedItemCount("markers");
    StreamSet * const coordinates = b->getOutputStreamSet("Coordinates");
    IntegerType * const coordinateTy = b->getIntNTy(coordinates->getFieldWidth());
    Value * const coordinatePtr = b->getRawOutputPointer("Coordinates", b->getProducedItemCount("Coordinates"));
    b->CreateBr(stridePrologue);

    b->SetInsertPoint(stridePrologue);
    PHINode * const strideNumPhi = b->CreatePHI(sizeTy, 2);
    strideNumPhi->addIncoming(sz_ZERO, entryBlock);
    PHINode * const currentProcessed = b->CreatePHI(sizeTy, 2);
    currentProcessed->addIncoming(processedMarkers, entryBlock);
    PHINode * const outerCoordinatePtrPhi = b->CreatePHI(coordinatePtr->getType(), 2);
    outerCoordinatePtrPhi->addIncoming(coordinatePtr, entryBlock);

    Value * const currentMarks = b->loadInputStreamBlock("markers", sz_ZERO, strideNumPhi);
    FixedVectorType * const sizeVecTy = FixedVectorType::get(sizeTy, vecsPerStride);
    Value * currentVec = b->CreateBitCast(currentMarks, sizeVecTy);

    b->CreateLikelyCondBr(b->bitblock_any(currentMarks), strideCoordinateVecLoop, strideCoordinateVecDone);

    b->SetInsertPoint(strideCoordinateVecLoop);
    PHINode * const elemIdx = b->CreatePHI(sizeTy, 2, "elemIdx");
    elemIdx->addIncoming(sz_ZERO, stridePrologue);
    PHINode * const incomingOuterCoordinatePtrPhi = b->CreatePHI(coordinatePtr->getType(), 2);
    incomingOuterCoordinatePtrPhi->addIncoming(outerCoordinatePtrPhi, stridePrologue);
    Value * const elem = b->CreateExtractElement(currentVec, elemIdx);
    b->CreateCondBr(b->CreateICmpNE(elem, sz_ZERO), strideCoordinateElemLoop, strideCoordinateElemDone);

    b->SetInsertPoint(strideCoordinateElemLoop);
    PHINode * const remaining = b->CreatePHI(sizeTy, 2);
    remaining->addIncoming(elem, strideCoordinateVecLoop);
    PHINode * const innerCoordinatePtrPhi = b->CreatePHI(coordinatePtr->getType(), 2);
    innerCoordinatePtrPhi->addIncoming(incomingOuterCoordinatePtrPhi, strideCoordinateVecLoop);

    Value * pos = b->CreateCountForwardZeroes(remaining);
    pos = b->CreateAdd(pos, b->CreateMul(elemIdx, sz_BITS));
    pos = b->CreateAdd(pos, currentProcessed);
    b->CreateStore(pos, innerCoordinatePtrPhi);

    Value * const nextCoordPtr = b->CreateGEP(coordinateTy, innerCoordinatePtrPhi, sz_ONE);
    innerCoordinatePtrPhi->addIncoming(nextCoordPtr, strideCoordinateElemLoop);
    Value * const nextRemaining = b->CreateResetLowestBit(remaining);
    remaining->addIncoming(nextRemaining, strideCoordinateElemLoop);
    b->CreateCondBr(b->CreateICmpNE(nextRemaining, sz_ZERO), strideCoordinateElemLoop, strideCoordinateElemDone);

    b->SetInsertPoint(strideCoordinateElemDone);
    PHINode * const nextCoordinatePtrPhi = b->CreatePHI(coordinatePtr->getType(), 2);
    nextCoordinatePtrPhi->addIncoming(outerCoordinatePtrPhi, strideCoordinateVecLoop);
    nextCoordinatePtrPhi->addIncoming(nextCoordPtr, strideCoordinateElemLoop);
    incomingOuterCoordinatePtrPhi->addIncoming(nextCoordinatePtrPhi, strideCoordinateElemDone);
    Value * const nextElemIdx = b->CreateAdd(elemIdx, sz_ONE);
    elemIdx->addIncoming(nextElemIdx, strideCoordinateElemDone);
    Value * const moreVecs = b->CreateICmpNE(nextElemIdx, b->getSize(vecsPerStride));
    b->CreateCondBr(moreVecs, strideCoordinateVecLoop, strideCoordinateVecDone);

    b->SetInsertPoint(strideCoordinateVecDone);
    PHINode * const nextOuterCoordinatePtrPhi = b->CreatePHI(coordinatePtr->getType(), 2);
    nextOuterCoordinatePtrPhi->addIncoming(outerCoordinatePtrPhi, stridePrologue);
    nextOuterCoordinatePtrPhi->addIncoming(nextCoordinatePtrPhi, strideCoordinateElemDone);
    Value * const nextStrideNum = b->CreateAdd(strideNumPhi, sz_ONE);
    strideNumPhi->addIncoming(nextStrideNum, strideCoordinateVecDone);
    currentProcessed->addIncoming(b->CreateAdd(currentProcessed, b->getSize(mStride)), strideCoordinateVecDone);
    outerCoordinatePtrPhi->addIncoming(nextOuterCoordinatePtrPhi, strideCoordinateVecDone);

    b->CreateCondBr(b->CreateICmpULT(nextStrideNum, numOfStrides), stridePrologue, strideCoordinatesDone);

    b->SetInsertPoint(strideCoordinatesDone);
}

}
