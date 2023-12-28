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

#if 0

void UTF8_index::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::unique_ptr<cc::CC_Compiler> ccc;
    bool useDirectCC = getInput(0)->getType()->getArrayNumElements() == 1;
    if (useDirectCC) {
        ccc = std::make_unique<cc::Direct_CC_Compiler>(getEntryScope(), pb.createExtract(getInput(0), pb.getInteger(0)));
    } else {
        ccc = std::make_unique<cc::Parabix_CC_Compiler_Builder>(getEntryScope(), getInputStreamSet("source"));
    }

    Zeroes * const ZEROES = pb.createZeroes();
    PabloAST * const u8pfx = ccc->compileCC(makeByte(0xC0, 0xFF));


    Var * const nonFinal = pb.createVar("nonFinal", u8pfx);
    Var * const u8invalid = pb.createVar("u8invalid", ZEROES);
    Var * const valid_pfx = pb.createVar("valid_pfx", u8pfx);

    auto it = pb.createScope();
    pb.createIf(u8pfx, it);
    PabloAST * const u8pfx2 = ccc->compileCC(makeByte(0xC2, 0xDF), it);
    PabloAST * const u8pfx3 = ccc->compileCC(makeByte(0xE0, 0xEF), it);
    PabloAST * const u8pfx4 = ccc->compileCC(makeByte(0xF0, 0xF4), it);

    //
    // Two-byte sequences
    Var * const anyscope = it.createVar("anyscope", ZEROES);
    auto it2 = it.createScope();
    it.createIf(u8pfx2, it2);
    it2.createAssign(anyscope, it2.createAdvance(u8pfx2, 1));


    //
    // Three-byte sequences
    Var * const EF_invalid = it.createVar("EF_invalid", ZEROES);
    auto it3 = it.createScope();
    it.createIf(u8pfx3, it3);
    PabloAST * const u8scope32 = it3.createAdvance(u8pfx3, 1);
    it3.createAssign(nonFinal, it3.createOr(nonFinal, u8scope32));
    PabloAST * const u8scope33 = it3.createAdvance(u8pfx3, 2);
    PabloAST * const u8scope3X = it3.createOr(u8scope32, u8scope33);
    it3.createAssign(anyscope, it3.createOr(anyscope, u8scope3X));

    PabloAST * const advE0 = it3.createAdvance(ccc->compileCC(makeByte(0xE0), it3), 1, "advEO");
    PabloAST * const range80_9F = ccc->compileCC(makeByte(0x80, 0x9F), it3);
    PabloAST * const E0_invalid = it3.createAnd(advE0, range80_9F, "E0_invalid");

    PabloAST * const advED = it3.createAdvance(ccc->compileCC(makeByte(0xED), it3), 1, "advED");
    PabloAST * const rangeA0_BF = ccc->compileCC(makeByte(0xA0, 0xBF), it3);
    PabloAST * const ED_invalid = it3.createAnd(advED, rangeA0_BF, "ED_invalid");

    PabloAST * const EX_invalid = it3.createOr(E0_invalid, ED_invalid);
    it3.createAssign(EF_invalid, EX_invalid);

    //
    // Four-byte sequences
    auto it4 = it.createScope();
    it.createIf(u8pfx4, it4);
    PabloAST * const u8scope42 = it4.createAdvance(u8pfx4, 1, "u8scope42");
    PabloAST * const u8scope43 = it4.createAdvance(u8scope42, 1, "u8scope43");
    PabloAST * const u8scope44 = it4.createAdvance(u8scope43, 1, "u8scope44");
    PabloAST * const u8scope4nonfinal = it4.createOr(u8scope42, u8scope43);
    it4.createAssign(nonFinal, it4.createOr(nonFinal, u8scope4nonfinal));
    PabloAST * const u8scope4X = it4.createOr(u8scope4nonfinal, u8scope44);
    it4.createAssign(anyscope, it4.createOr(anyscope, u8scope4X));
    PabloAST * const F0_invalid = it4.createAnd(it4.createAdvance(ccc->compileCC(makeByte(0xF0), it4), 1), ccc->compileCC(makeByte(0x80, 0x8F), it4));
    PabloAST * const F4_invalid = it4.createAnd(it4.createAdvance(ccc->compileCC(makeByte(0xF4), it4), 1), ccc->compileCC(makeByte(0x90, 0xBF), it4));
    PabloAST * const FX_invalid = it4.createOr(F0_invalid, F4_invalid);
    it4.createAssign(EF_invalid, it4.createOr(EF_invalid, FX_invalid));

    //
    // Invalid cases
    PabloAST * const legalpfx = it.createOr(it.createOr(u8pfx2, u8pfx3), u8pfx4);
    //  Any scope that does not have a suffix byte, and any suffix byte that is not in
    //  a scope is a mismatch, i.e., invalid UTF-8.
    PabloAST * const u8suffix = ccc->compileCC("u8suffix", makeByte(0x80, 0xBF), it);
    PabloAST * const mismatch = it.createXor(anyscope, u8suffix);
    //
    PabloAST * const pfx_invalid = it.createXor(valid_pfx, legalpfx);
    it.createAssign(u8invalid, it.createOr(pfx_invalid, it.createOr(mismatch, EF_invalid)));
    PabloAST * const u8valid = it.createNot(u8invalid, "u8valid");
    //
    it.createAssign(nonFinal, it.createAnd(nonFinal, u8valid));

    Var * const u8index = getOutputStreamVar("u8index");
    PabloAST * u8final = pb.createInFile(pb.createNot(nonFinal));
    if (getNumOfStreamInputs() > 1) {
        u8final = pb.createOr(u8final, getInputStreamSet("u8_LB")[0]);
    }
    pb.createAssign(pb.createExtract(u8index, pb.getInteger(0)), u8final);
}


#endif

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
    PabloAST * CRofCRLF = pb.createAnd(csvMarks[markCR], pb.createLookahead(csvMarks[markLF], 1), "CRofCRLF");
    PabloAST * recordSeparators = pb.createOrAnd(csvMarks[markEOF], csvMarks[markLF], unquoted, "recordSeparators");

    // track and "remove" empty lines by advacing recordSeparators and adding the results to nonText?
    PabloAST * allSeparators = pb.createOrAnd(recordSeparators, csvMarks[markComma], unquoted, "allSeparators");
    PabloAST * formattingQuotes = pb.createXor(dquote, escaped_quote, "formattingQuotes");
    PabloAST * nonText = pb.createOr3(CRofCRLF, formattingQuotes, quote_escape, "nonText");
    PabloAST * recordSeparatorsAndNonText = pb.createOr(recordSeparators, nonText, "recordSeparatorsAndNonText");

    PabloAST * start = pb.createScanThru(recordSeparators, recordSeparatorsAndNonText);
    if (noHeaderLine) {
        start = pb.createOr(pb.createNot(pb.createAdvance(pb.createOnes(), 1)), start);
    } else {
        PabloAST * const afterHeader = pb.createSpanAfterFirst(recordSeparators, "afterHeader");
        allSeparators = pb.createAnd(allSeparators, afterHeader, "allSeparators'");
        recordSeparatorsAndNonText = pb.createOr(recordSeparatorsAndNonText, pb.createNot(afterHeader), "recordSeparatorsAndNonText'");
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

    Value * const markersProcessed = b->getProcessedItemCount("markers");

    StreamSet * const coordinates = b->getOutputStreamSet("Coordinates");
    IntegerType * const coordinateTy = b->getIntNTy(coordinates->getFieldWidth());
    Value * const alreadyProduced = b->getProducedItemCount("Coordinates");
    Value * const coordinatePtr = b->getRawOutputPointer("Coordinates", sz_ZERO);
    b->CreateBr(stridePrologue);

    b->SetInsertPoint(stridePrologue);
    PHINode * const strideNumPhi = b->CreatePHI(sizeTy, 2);
    strideNumPhi->addIncoming(sz_ZERO, entryBlock);
    PHINode * const currentProcessedPhi = b->CreatePHI(sizeTy, 2);
    currentProcessedPhi->addIncoming(markersProcessed, entryBlock);
    PHINode * const outerCoordinatePhi = b->CreatePHI(sizeTy, 2);
    outerCoordinatePhi->addIncoming(alreadyProduced, entryBlock);

    Value * const currentMarks = b->loadInputStreamBlock("markers", sz_ZERO, strideNumPhi);
    FixedVectorType * const sizeVecTy = FixedVectorType::get(sizeTy, vecsPerStride);

    Value * a = b->simd_popcount(b->getBitBlockWidth(), currentMarks);
    a = b->CreateBitCast(a, b->getIntNTy(b->getBitBlockWidth()));
    a = b->CreateTrunc(a, sizeTy);

    Value * expected = b->CreateAdd(a, outerCoordinatePhi);

    Value * const currentVec = b->CreateBitCast(currentMarks, sizeVecTy);
    b->CreateLikelyCondBr(b->bitblock_any(currentMarks), strideCoordinateVecLoop, strideCoordinateVecDone);

    b->SetInsertPoint(strideCoordinateVecLoop);
    PHINode * const elemIdx = b->CreatePHI(sizeTy, 2, "elemIdx");
    elemIdx->addIncoming(sz_ZERO, stridePrologue);
    PHINode * const incomingOuterCoordinatePhi = b->CreatePHI(sizeTy, 2);
    incomingOuterCoordinatePhi->addIncoming(outerCoordinatePhi, stridePrologue);
    Value * const elem = b->CreateExtractElement(currentVec, elemIdx);
    b->CreateCondBr(b->CreateICmpNE(elem, sz_ZERO), strideCoordinateElemLoop, strideCoordinateElemDone);

    b->SetInsertPoint(strideCoordinateElemLoop);
    PHINode * const remaining = b->CreatePHI(sizeTy, 2);
    remaining->addIncoming(elem, strideCoordinateVecLoop);
    PHINode * const innerCoordinatePhi = b->CreatePHI(sizeTy, 2);
    innerCoordinatePhi->addIncoming(incomingOuterCoordinatePhi, strideCoordinateVecLoop);

    Value * elempos = b->CreateCountForwardZeroes(remaining);
    Value * position = b->CreateAdd(elempos, b->CreateMul(elemIdx, sz_BITS));
    position = b->CreateAdd(position, currentProcessedPhi);
    b->CreateStore(position, b->CreateGEP(coordinateTy, coordinatePtr, innerCoordinatePhi));

    Value * const nextCoord = b->CreateAdd(innerCoordinatePhi, sz_ONE);
    innerCoordinatePhi->addIncoming(nextCoord, strideCoordinateElemLoop);

    Value * const nextRemaining = b->CreateXor(remaining, b->CreateShl(sz_ONE, elempos));
    remaining->addIncoming(nextRemaining, strideCoordinateElemLoop);
    b->CreateCondBr(b->CreateICmpNE(nextRemaining, sz_ZERO), strideCoordinateElemLoop, strideCoordinateElemDone);

    b->SetInsertPoint(strideCoordinateElemDone);
    PHINode * const nextCoordinatePhi = b->CreatePHI(sizeTy, 2);
    nextCoordinatePhi->addIncoming(incomingOuterCoordinatePhi, strideCoordinateVecLoop);
    nextCoordinatePhi->addIncoming(nextCoord, strideCoordinateElemLoop);


    incomingOuterCoordinatePhi->addIncoming(nextCoordinatePhi, strideCoordinateElemDone);
    Value * const nextElemIdx = b->CreateAdd(elemIdx, sz_ONE);
    elemIdx->addIncoming(nextElemIdx, strideCoordinateElemDone);
    Value * const moreVecs = b->CreateICmpNE(nextElemIdx, b->getSize(vecsPerStride));
    b->CreateCondBr(moreVecs, strideCoordinateVecLoop, strideCoordinateVecDone);

    b->SetInsertPoint(strideCoordinateVecDone);
    PHINode * const nextOuterCoordinatePhi = b->CreatePHI(sizeTy, 2);
    nextOuterCoordinatePhi->addIncoming(outerCoordinatePhi, stridePrologue);
    nextOuterCoordinatePhi->addIncoming(nextCoordinatePhi, strideCoordinateElemDone);
    outerCoordinatePhi->addIncoming(nextOuterCoordinatePhi, strideCoordinateVecDone);

    Value * nextProcessed = b->CreateAdd(currentProcessedPhi, b->getSize(mStride));
    currentProcessedPhi->addIncoming(nextProcessed, strideCoordinateVecDone);

    Value * const nextStrideNum = b->CreateAdd(strideNumPhi, sz_ONE);
    strideNumPhi->addIncoming(nextStrideNum, strideCoordinateVecDone);
    b->CreateCondBr(b->CreateICmpULT(nextStrideNum, numOfStrides), stridePrologue, strideCoordinatesDone);

    b->SetInsertPoint(strideCoordinatesDone);
}

}
