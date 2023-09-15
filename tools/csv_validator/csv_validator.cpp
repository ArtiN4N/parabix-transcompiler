/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <kernel/core/idisa_target.h>
#include <boost/filesystem.hpp>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/adt.h>
#include <re/unicode/resolve_properties.h>
#include <unicode/utf/utf_compiler.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/core/streamset.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pablo_toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <toolchain/toolchain.h>
#include <fileselect/file_select.h>
#include "../csv/csv_util.hpp"
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <fstream>
#include <kernel/scan/scanmatchgen.h>
#include <re/parse/parser.h>
#include <re/adt/re_name.h>
#include <re/compile/re_compiler.h>
#include <grep/grep_kernel.h>
#include <re/transforms/re_memoizing_transformer.h>
#include <grep/regex_passes.h>
#include <boost/intrusive/detail/math.hpp>
#include <kernel/util/bixhash.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <random>
#include <llvm/IR/Verifier.h>
#ifdef ENABLE_PAPI
#include <util/papi_helper.hpp>
#endif

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;

static cl::OptionCategory wcFlags("Command Flags", "CSV Validator options");


static cl::opt<std::string> inputSchema(cl::Positional, cl::desc("<input schema filename>"), cl::Required, cl::cat(wcFlags));

static cl::list<std::string> inputFiles(cl::Positional, cl::desc("<input file ...>"), cl::OneOrMore, cl::cat(wcFlags));

static cl::opt<bool> noHeaderLine("no-header", cl::desc("CSV record data begins on first line"), cl::init(false), cl::cat(wcFlags));


std::vector<fs::path> allFiles;

using namespace pablo;
using namespace kernel;
using namespace cc;
using namespace re;

using boost::intrusive::detail::floor_log2;

#if 1
class CSVDataLexer : public PabloKernel {
public:
    CSVDataLexer(BuilderRef kb, StreamSet * Source, StreamSet * CSVlexical)
        : PabloKernel(kb, "CSVDataLexer",
                      {Binding{"Source", Source}},
                      {Binding{"CSVlexical", CSVlexical, FixedRate(), Add1()}}) {}
protected:
    void generatePabloMethod() override;
};

// enum {markLF = 0, markCR = 1, markDQ = 2, markComma = 3, markEOF = 4};

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

#endif

enum CSVDataFieldMarkers {
    FieldDataMask = 0
    , RecordSeparators = 1
};



class CSVDataParser : public PabloKernel {
    static std::string makeNameFromOptions() {
        std::string tmp;
        raw_string_ostream nm(tmp);
        nm << "CSVDataParser";
        if (noHeaderLine) {
            nm << "NH";
        }
        nm.flush();
        return tmp;
    }
public:
    CSVDataParser(BuilderRef kb, StreamSet * UTFindex, StreamSet * csvMarks,
                  StreamSet * fieldData, StreamSet * recordSeparators, StreamSet * allSeperators)
        : PabloKernel(kb, makeNameFromOptions(),
                      {Binding{"UTFindex", UTFindex}, Binding{"csvMarks", csvMarks, FixedRate(), LookAhead(1)}},
                      {Binding{"fieldData", fieldData}, Binding{"recordSeparators", recordSeparators}, Binding{"allSeperators", allSeperators}}) {
        addAttribute(SideEffecting());
        assert (UTFindex->getNumElements() == 1);
        assert (csvMarks->getNumElements() == 5);
    }
protected:
    void generatePabloMethod() override;
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
    //PabloAST * recordSeparators = pb.createOr(pb.createAnd(csvMarks[markLF], unquoted), csvMarks[markEOF], "recordSeparators");
    PabloAST * allSeparators = pb.createOrAnd(recordSeparators, csvMarks[markComma], unquoted, "allSeparators");
    // PabloAST * allSeparators = pb.createOr(pb.createAnd(csvMarks[markComma], unquoted), recordSeparators, "allSeparators");
    PabloAST * CRofCRLF = pb.createAnd(csvMarks[markCR], pb.createLookahead(csvMarks[markLF], 1));

    PabloAST * formattingQuotes = pb.createXor(dquote, escaped_quote);
    PabloAST * nonText = pb.createOr(formattingQuotes, CRofCRLF);
    PabloAST * utfIndex = pb.createExtract(getInputStreamVar("UTFindex"), pb.getInteger(0));
    PabloAST * fieldData = pb.createAnd(utfIndex, pb.createNot(nonText));
    if (!noHeaderLine) {
        // PabloAST * afterHeader = pb.createMatchStar(pb.createAdvance(recordSeparators, 1), pb.createOnes(), "afterHeader");
        // pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {afterHeader} );
        PabloAST * afterHeader2 = pb.createSpanAfterFirst(recordSeparators, "afterHeader");
        // pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {afterHeader2} );
        fieldData = pb.createAnd(fieldData, afterHeader2);
        allSeparators = pb.createAnd(allSeparators, afterHeader2);
        recordSeparators = pb.createAnd(recordSeparators, afterHeader2);
    }
    pb.createAssign(pb.createExtract(getOutputStreamVar("fieldData"), pb.getInteger(0)), fieldData);
    pb.createAssign(pb.createExtract(getOutputStreamVar("recordSeparators"), pb.getInteger(0)), recordSeparators);
    pb.createAssign(pb.createExtract(getOutputStreamVar("allSeperators"), pb.getInteger(0)), allSeparators);

}

typedef void (*CSVValidatorFunctionType)(uint32_t fd, const char * fileName);

struct CSVSchemaDefinition {
    std::vector<RE *> Validator;
    std::vector<unsigned> FieldValidatorIndex;
    std::vector<unsigned> UIDFields;
};



class CSVSchemaValidatorKernel : public pablo::PabloKernel {
public:
    CSVSchemaValidatorKernel(BuilderRef b, const CSVSchemaDefinition & schema, StreamSet * basisBits, StreamSet * fieldData, StreamSet * recordSeperators, StreamSet * allSeperators, StreamSet * invalid, StreamSet * keyRuns = nullptr);
    llvm::StringRef getSignature() const override;
    bool hasSignature() const override { return true; }

protected:
    CSVSchemaValidatorKernel(BuilderRef b, const CSVSchemaDefinition & schema, std::string signature, StreamSet * basisBits, StreamSet * fieldData, StreamSet * recordSeperators, StreamSet * allSeperators, StreamSet * invalid, StreamSet * keyRuns);
    void generatePabloMethod() override;
private:
    static std::string makeSignature(const std::vector<RE *> & fields);
private:
    const CSVSchemaDefinition &         mSchema;
    std::string                         mSignature;

};

static size_t NumOfFields = 1;

const static std::string FieldSepName = ":";

CSVSchemaDefinition parseSchemaFile() {
    CSVSchemaDefinition Def;

    std::ifstream schemaFile(inputSchema);
    if (LLVM_LIKELY(schemaFile.is_open())) {

        RE_MemoizingTransformer memo("memoizer");

        std::string line;

        boost::container::flat_map<RE *, unsigned> M;

        Name * fs = makeName(FieldSepName);
        fs->setDefinition(makeCC(charComma));

        FixedArray<RE *, 3> parsedLine;
        parsedLine[0] = makeAlt({makeStart(), fs});

        parsedLine[2] = fs; // makeAlt({fs, makeEnd()});

        while(std::getline(schemaFile, line)) {
            parsedLine[1] = RE_Parser::parse(line);
            assert (parsedLine[1]);
            RE * const original = makeSeq(parsedLine.begin(), parsedLine.end());
            RE * const field = memo.transformRE(toUTF8(regular_expression_passes(original)));
            assert (field);

            auto f = M.find(field);
            unsigned index = 0;
            if (f == M.end()) {
                index = M.size();
                M.emplace(field, index);
            } else {
                index = f->second;
            }
            Def.FieldValidatorIndex.push_back(index);
        }
        schemaFile.close();
        NumOfFields = Def.FieldValidatorIndex.size();
        Def.Validator.resize(M.size());
        for (auto p : M) {
            Def.Validator[p.second] = p.first;
        }

        Def.UIDFields.push_back(0);
        Def.UIDFields.push_back(1);

    } else {
        report_fatal_error("Cannot open schema: " + inputSchema);
    }
    return Def;
}

std::string makeCSVSchemaDefinitionName(const CSVSchemaDefinition & schema) {
    std::string tmp;
    tmp.reserve(1024);
    raw_string_ostream out(tmp);
    out.write_hex(schema.FieldValidatorIndex[0]);
    for (unsigned i = 1; i < schema.FieldValidatorIndex.size(); ++i) {
        out << ':';
        out.write_hex(schema.FieldValidatorIndex[i]);
    }
    for (const RE * re : schema.Validator) {
        out << '\0' << Printer_RE::PrintRE(re);
    }
    char joiner = 'u';
    for (const auto k : schema.UIDFields) {
        out << joiner; out.write_hex(k);
        joiner = ',';
    }
    out.flush();
    return tmp;
}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, const CSVSchemaDefinition & schema, StreamSet * basisBits, StreamSet * fieldData, StreamSet * recordSeperators, StreamSet * allSeperators, StreamSet * invalid, StreamSet * keyRuns)
: CSVSchemaValidatorKernel(b, schema, makeCSVSchemaDefinitionName(schema), basisBits, fieldData, recordSeperators, allSeperators, invalid, keyRuns) {

}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, const CSVSchemaDefinition & schema, std::string signature, StreamSet * basisBits, StreamSet * fieldData, StreamSet * recordSeperators, StreamSet * allSeperators, StreamSet * invalid, StreamSet * keyRuns)
: PabloKernel(b, "csvv" + getStringHash(signature),
    {Binding{"basisBits", basisBits}, Binding{"fieldData", fieldData}, Binding{"allSeperators", allSeperators}, Binding{"recordSeperators", recordSeperators}},
    {Binding{"invalid", invalid}})
, mSchema(schema)
, mSignature(std::move(signature)) {
    if (schema.UIDFields.size() > 0) {
        mOutputStreamSets.emplace_back("hashKeyRuns", keyRuns);
    }
}


StringRef CSVSchemaValidatorKernel::getSignature() const {
    return mSignature;
}

void CSVSchemaValidatorKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());

    Var * const recordSeperators = pb.createExtract(getInputStreamVar("recordSeperators"), pb.getInteger(0));

    RE_Compiler re_compiler(getEntryScope(), recordSeperators, &cc::UTF8);

    auto basisBits = getInputStreamSet("basisBits");

    Integer * const pb_ZERO = pb.getInteger(0);

    PabloAST * const fieldDataMask = pb.createExtract(getInputStreamVar("fieldData"), pb_ZERO);

    re_compiler.setIndexing(&cc::UTF8, fieldDataMask);

    re_compiler.addAlphabet(&cc::UTF8, basisBits);

    Var * const allSeperators = pb.createExtract(getInputStreamVar("allSeperators"), pb_ZERO);
    assert (allSeperators->getType() == getStreamTy());

    re_compiler.addPrecompiled(FieldSepName, RE_Compiler::ExternalStream(RE_Compiler::Marker(allSeperators, 1), std::make_pair<int,int>(1, 1)));

    // TODO: if the number of validators equals the number of fields, just scan sequentially? We expect everything to be valid but if the
    // total schema length is "long", we won't necessarily be starting a new record every block. Can we "break up" the validation checks to
    // test if we should scan through a chunk of them based on the current position?

    using Mark = RE_Compiler::Marker;

    const auto & validators = mSchema.Validator;
    std::vector<PabloAST *> matches(validators.size());
    for (unsigned i = 0; i < validators.size(); ++i) {
        Mark match = re_compiler.compileRE(validators[i], Mark{fieldDataMask, 0}, 1);
        assert (match.offset() == 1);
        matches[i] = match.stream();
    }

    RE_Compiler::Marker startMatch = re_compiler.compileRE(makeStart());
    PabloAST * const allStarts = startMatch.stream();



    const auto & fields = mSchema.FieldValidatorIndex;

    PabloAST * currentPos = allStarts;
    PabloAST * allSeparatorsMatches = nullptr;

    PabloAST * const nonSeparators = pb.createInFile(pb.createNot(allSeperators), "nonSeperators");

    PabloAST * const fieldSeparators = pb.createXor(allSeperators, recordSeperators, "fieldSeparators");

    const auto n = fields.size();

    // I expect to see at most one UID (since databases only really support a single primary/composite key)
    // but since the logic here doesn't depend on it, I permit it for multiple independent keys.

    std::vector<bool> usedInSchemaUID(n, false);
    for (const auto k : mSchema.UIDFields) {
        usedInSchemaUID[k] = true;
    }

    FixedArray<PabloAST *, 2> args;
    args[0] = nullptr;

    // TODO: this will fail on a blank line; should probably ignore them

    for (unsigned i = 0; i < n; ++i) {
        PabloAST * const fieldStart = currentPos;
        currentPos = pb.createAdvanceThenScanThru(fieldStart, nonSeparators, "currentPos" + std::to_string(i + 1));
        currentPos = pb.createAnd(currentPos, matches[fields[i]], "field" + std::to_string(fields[i]) + "." + std::to_string(i + 1));

        if (i < (n - 1)) {
            currentPos = pb.createAnd(currentPos, fieldSeparators, "matchedFieldSep" + std::to_string(i + 1));
        } else {
            currentPos = pb.createAnd(currentPos, recordSeperators, "matchedRecSep");
        }
        PabloAST * const fieldEnd = currentPos;

        if (LLVM_UNLIKELY(usedInSchemaUID[i])) {

            if (args[0] == nullptr) {
                args[0] = fieldStart;
                args[1] = fieldEnd;
            } else {
                args[0] = pb.createOr(args[0], fieldStart);
                args[1] = pb.createOr(args[1], fieldEnd);
            }

        }

        if (allSeparatorsMatches) {
            allSeparatorsMatches = pb.createOr(allSeparatorsMatches, currentPos);
        } else {
            allSeparatorsMatches = currentPos;
        }
    }

    PabloAST * result = pb.createXor(allSeparatorsMatches, allSeperators, "result");
    pb.createAssign(pb.createExtract(getOutputStreamVar("invalid"), pb_ZERO), result);

    if (mSchema.UIDFields.size() > 0) {
        // TODO: compute the run or store independent start/end markers? Work balance probably favours the latter.
        PabloAST * const run = pb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, args, "run");
        PabloAST * const masked = pb.createAnd(run, nonSeparators);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyRuns"), pb_ZERO), masked);
    }


}

static bool foundError = false;

extern "C" void csv_error_identifier_callback(char * fileName, const size_t fieldNum, char * start, char * end) {
    foundError = true;
    SmallVector<char, 1024> tmp;
    raw_svector_ostream out(tmp);
    assert (NumOfFields > 0);
    const auto n = std::max<size_t>(NumOfFields, 1);
    out << "Error found in " << fileName << ": Field " << ((fieldNum % n) + 1) << " of Line " << ((fieldNum / n) + 1)
        << '\n';
    for (auto c = start; c < end; ++c) {
        out << *c;
    }
    out << " does not match supplied rule.";
    // TODO: this needs to report more information as to what field/rule was invalid
    // TODO: this cannot differntiate between erroneous line ends
    errs() << out.str();
}

class CSVErrorIdentifier : public MultiBlockKernel {
public:
    CSVErrorIdentifier(BuilderRef b, StreamSet * const errorStream, StreamSet * const allSeparators, StreamSet * const ByteStream, Scalar * const fileName);
    void linkExternalMethods(BuilderRef b) override;
private:
    void generateMultiBlockLogic(BuilderRef iBuilder, llvm::Value * const numOfStrides) override;
};

/** ------------------------------------------------------------------------------------------------------------- *
 * @brief constructor
 ** ------------------------------------------------------------------------------------------------------------- */
CSVErrorIdentifier::CSVErrorIdentifier(BuilderRef b, StreamSet * const errorStream, StreamSet * const allSeparators, StreamSet * const ByteStream, Scalar * const fileName)
: MultiBlockKernel(b, "CSVErrorIdentifier" + std::to_string(codegen::SegmentSize),
// inputs
{Binding{"errorStream", errorStream}
,Binding{"allSeparators", allSeparators}
,Binding{"InputStream", ByteStream, FixedRate(), { Deferred() }}},
// outputs
{},
// input scalars
{Binding{"fileName", fileName}},
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

    FixedArray<Value *, 4> callbackArgs;
    // char * fileName, const size_t fieldNum, char * line_start, char * line_end

    callbackArgs[0] = b->getScalarField("fileName");

    Value * const firstErrorWord = b->CreateExtractElement(errorVec, firstErrorWordIndex);
    Value * const firstErrorWordPos = b->CreateCountForwardZeroes(firstErrorWord, "", true);
    Value * const firstErrorSeparator = b->CreateAdd(b->CreateMul(firstErrorWordIndex, sz_SIZEWIDTH), firstErrorWordPos);
    Value * const errorMask = b->CreateNot(b->bitblock_mask_from(firstErrorSeparator));

    Value * const unmaskedFinalValue = b->loadInputStreamBlock("allSeparators", i32_ZERO, strideIndexPhi);
    Value * const maskedFinalValue = b->CreateAnd(unmaskedFinalValue, errorMask);

    Value * const maskedSepVec = b->simd_popcount(sizeWidth, maskedFinalValue);
    assert (maskedSepVec->getType() == sizeVecTy);
    Value * const reportedSepVec = b->CreateAdd(maskedSepVec, updatedSeparatorPartialSumPhi);
    Value * const partialSum = b->hsimd_partial_sum(sizeWidth, reportedSepVec);
    Value * const numOfSeparators = b->mvmd_extract(sizeWidth, partialSum, partialSumFieldCount - 1);
    callbackArgs[1] = b->CreateAdd(allSeparatorsObserved, numOfSeparators);

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
    callbackArgs[2] = b->getRawInputPointer("InputStream", priorSeparator);
    callbackArgs[3] = b->getRawInputPointer("InputStream", firstErrorSeparator);

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

class BixHash2 final: public pablo::PabloKernel {
public:
    BixHash2(BuilderRef b,
            StreamSet * basis, StreamSet * allSeparators, StreamSet * run,
             StreamSet * hashes, StreamSet * selector_span, StreamSet * run_separators, unsigned steps=4, unsigned seed = 179321)
    : PabloKernel(b, "BixHash2_" + std::to_string(hashes->getNumElements()) + "_" + std::to_string(steps) + "_" + std::to_string(seed),
                  {Binding{"basis", basis}, Binding{"run", run}},
                  {Binding{"hashes", hashes}, Binding{"selector_span", selector_span}, Binding{"keyFields", run_separators}}),
    mHashBits(hashes->getNumElements()), mHashSteps(steps), mSeed(seed) {}


protected:
    void generatePabloMethod() override;
private:
    const unsigned mHashBits;
    const unsigned mHashSteps;
    const unsigned mSeed;
};




void BixHash2::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());

    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    PabloAST * run = getInputStreamSet("run")[0];
    std::vector<PabloAST *> hash(mHashBits);
    // For every byte we create an in-place hash, in which each bit
    // of the byte is xor'd with one other bit.
    std::vector<unsigned> bitmix(mHashBits);
    const auto m = basis.size(); assert (m > 0);
    for (unsigned i = 0; i < mHashBits; ++i) {
        bitmix[i] = i % m;
    }

    std::mt19937 rng(mSeed);

    if (mHashBits > 1) {
        // Avoid XOR-ing a value with itself. Since we may have significantly more hashbits than
        // basis bits, we want to minimize the work needed to find such a solution.
        for (unsigned j = 0; j < mHashBits; j += m) {
            const auto l = j + std::min<unsigned>(mHashBits - j, m);
 retry_shuffle:
            std::shuffle (bitmix.begin() + j, bitmix.begin() + l, rng);
            for (unsigned i = j; i < l; i++) {
                if ((i % m) == bitmix[i]) {
                    goto retry_shuffle;
                }
            }
        }
    }

    // By masking out values in our runs here, we clear out the value
    // in the separator(s) between runs.

    for (unsigned i = 0; i < mHashBits; i++) {
        hash[i] = pb.createAndXor(run, basis[i % m], basis[bitmix[i]]);
    }

    // In each step, the select stream will mark positions that are
    // to receive bits from prior locations in the symbol.   The
    // select stream must ensure that no bits from outside the symbol
    // are included in the calculated hash value.
    PabloAST * select = run;
    for (unsigned j = 0; j < mHashSteps; j++) {
        const auto shft = 1U << j;
        // Select bits from prior positions.
        std::shuffle (bitmix.begin(), bitmix.end(), rng);
        for (unsigned i = 0; i < mHashBits; i++) {
            PabloAST * priorBits = pb.createAdvance(hash[bitmix[i]], shft);
            // Mix in bits from prior positions.
            hash[i] = pb.createXorAnd(hash[i], priorBits, select);
        }
        select = pb.createAnd(select, pb.createAdvance(select, shft));
    }

    // if the value is still in the select span, we did not include it in
    // the hash'ed value.
    PabloAST * const selectors = pb.createAnd(run, pb.createNot(select));

    Var * hashVar = getOutputStreamVar("hashes");
    for (unsigned i = 0; i < mHashBits; i++) {
        pb.createAssign(pb.createExtract(hashVar, pb.getInteger(i)), hash[i]);
    }

    pb.createAssign(pb.createExtract(getOutputStreamVar("selector_span"), pb.getInteger(0)), selectors);

    // The "run" goes up to but not including the separator marker. Move it up to the separator.
    // TODO: pass allSeparators into here and try to avoid the unnecessary advance?

    PabloAST * advRun = pb.createAdvance(run, 1);
//    PabloAST * fieldStarts = pb.createAnd(run, pb.createNot(advRun));
    PabloAST * fieldEnds = pb.createAnd(advRun, pb.createNot(run));

   Var * keyFieldVar = getOutputStreamVar("keyFields");

//   pb.createAssign(pb.createExtract(keyFieldVar, pb.getInteger(0)), fieldStarts);
   pb.createAssign(pb.createExtract(keyFieldVar, pb.getInteger(0)), fieldEnds);

}


class IdentifyLastSelector final: public pablo::PabloKernel {
public:
    IdentifyLastSelector(BuilderRef b, StreamSet * selector_span, StreamSet * selectors)
    : PabloKernel(b, "IdentifyLastSelector",
                  {Binding{"selector_span", selector_span, FixedRate(), LookAhead(1)}},
                  {Binding{"selectors", selectors}}) {}
protected:
    void generatePabloMethod() override;
};


void IdentifyLastSelector::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    // TODO: we shouldn't need this kernel to obtain the last mark of each run of a selector_span
    Integer * pb_ZERO = pb.getInteger(0);
    PabloAST * span = pb.createExtract(getInputStreamVar("selector_span"), pb_ZERO);
    PabloAST * la = pb.createLookahead(span, 1);
    PabloAST * selectors = pb.createAnd(span, pb.createNot(la));
    pb.createAssign(pb.createExtract(getOutputStreamVar("selectors"), pb_ZERO), selectors);
}


class PullBack final: public pablo::PabloKernel {
public:
    PullBack(BuilderRef b, StreamSet * input, StreamSet * output, const unsigned ammount = 1)
    : PabloKernel(b, "PullBack" + std::to_string(ammount),
                  {Binding{"input", input, FixedRate(), LookAhead(ammount)}},
                  {Binding{"output", output}}) {}
protected:
    void generatePabloMethod() override;

};


void PullBack::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    // TODO: we shouldn't need this kernel to obtain the last mark of each run of a selector_span
    Integer * pb_ZERO = pb.getInteger(0);
    PabloAST * span = pb.createExtract(getInputStreamVar("input"), pb_ZERO);
    const auto k = getInputStreamSetBinding(0).getLookahead();
    PabloAST * la = pb.createLookahead(span, k);
    pb.createAssign(pb.createExtract(getOutputStreamVar("output"), pb_ZERO), la);
}


#if 0

class CheckKeyUniqueness : public SegmentOrientedKernel {
public:
    CheckKeyUniqueness(BuilderRef b, StreamSet * ByteStream, StreamSet * const HashValSequence,
                       StreamSet * allSeparators, StreamSet * selectors,
                       Scalar * const errorCallbackObject, const unsigned fieldsPerKey);
private:
    void generateDoSegmentMethod(BuilderRef b) override;
private:
    const unsigned FieldsPerKey;
};

CheckKeyUniqueness::CheckKeyUniqueness(BuilderRef b, StreamSet * ByteStream, StreamSet * const HashValSequence,
                                       StreamSet * allSeparators, StreamSet * selectors,
                                       Scalar * const errorCallbackObject, const unsigned fieldsPerKey)
: SegmentOrientedKernel(b, "CheckKeyUniqueness" + std::to_string(fieldsPerKey),
// inputs
{Binding{"InputStream", ByteStream, GreedyRate(), Deferred()}},
// outputs
{},
// input scalars
{Binding{"callbackObject", errorCallbackObject}},
// output scalars
{},
// kernel state
{})
, FieldsPerKey(fieldsPerKey) {
    setStride(1);
    addAttribute(SideEffecting());
}

void CheckKeyUniqueness::generateDoSegmentMethod(BuilderRef b) {


}

#endif

CSVValidatorFunctionType wcPipelineGen(CPUDriver & pxDriver) {

    auto & b = pxDriver.getBuilder();

    Type * const int32Ty = b->getInt32Ty();

    auto P = pxDriver.makePipeline({Binding{int32Ty, "fd"}, Binding{b->getInt8PtrTy(), "fileName"}});

    P->setUniqueName("csv_validator");

    Scalar * const fileDescriptor = P->getInputScalar("fd");

    StreamSet * const ByteStream = P->CreateStreamSet(1, 8);

    P->CreateKernelCall<MMapSourceKernel>(fileDescriptor, ByteStream);

    auto BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);


    StreamSet * csvCCs = P->CreateStreamSet(5);
    P->CreateKernelCall<CSVlexer>(BasisBits, csvCCs);
    StreamSet * fieldData = P->CreateStreamSet(1);
    StreamSet * recordSeparators = P->CreateStreamSet(1);
    StreamSet * allSeparators = P->CreateStreamSet(1);

    StreamSet * u8index = P->CreateStreamSet();
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);

    P->CreateKernelCall<CSVDataParser>(u8index, csvCCs, fieldData, recordSeparators, allSeparators);

   const auto schemaFile = parseSchemaFile();

    StreamSet * errors = P->CreateStreamSet(1);

    const auto uniqueFields = schemaFile.UIDFields.size();

    if (uniqueFields == 0) {

        P->CreateKernelFamilyCall<CSVSchemaValidatorKernel>(schemaFile, BasisBits, fieldData, recordSeparators, allSeparators, errors);

    } else {
        StreamSet * keyRuns = P->CreateStreamSet(1);



        // If we use a bixhash like technique, we could possibly chunk the field data into N-byte phases and use
        // a loop to combine the data. But how can we prevent the data from one record from being combined with
        // another? We could scan through and iterate over each record individually?

        P->CreateKernelFamilyCall<CSVSchemaValidatorKernel>(schemaFile, BasisBits, fieldData, recordSeparators, allSeparators, errors, keyRuns);

//        P->CreateKernelCall<DebugDisplayKernel>("allSeparators", allSeparators);

//        P->CreateKernelCall<DebugDisplayKernel>("keyRuns", keyRuns);

        constexpr unsigned HASH_BITS = 16;

        StreamSet * hashes = P->CreateStreamSet(HASH_BITS);

        StreamSet * selector_span = P->CreateStreamSet(1);

        StreamSet * key_separators = P->CreateStreamSet(1);

        P->CreateKernelCall<BixHash2>(BasisBits, allSeparators, keyRuns, hashes, selector_span, key_separators, 5);

        StreamSet * compressed_separators = P->CreateStreamSet(1);

        P->CreateKernelCall<FieldCompressKernel>(Select(allSeparators, {0}), SelectOperationList{Select(key_separators, {0})}, compressed_separators, 64);

        StreamSet * compressed_separators2 = P->CreateStreamSet(1);

        P->CreateKernelCall<StreamCompressKernel>(allSeparators, compressed_separators, compressed_separators2, 64);

        StreamSet * const compressed_separators3 = P->CreateStreamSet(1);

        // TODO: merge PullBack with the decompression logic?

        // Doesn't work if the very first field on the first line is a key. Needs fake start

        P->CreateKernelCall<PullBack>(compressed_separators2, compressed_separators3);

        StreamSet * const selected_starts = P->CreateStreamSet(1);

      //  SpreadByMask(E, lineStarts, MatchesByLine, MatchedLineStarts);

        P->CreateKernelCall<StreamExpandKernel>(allSeparators, compressed_separators3, selected_starts, P->CreateConstant(b->getSize(0)), false);

        StreamSet * const selected_starts2 = P->CreateStreamSet(1);

        P->CreateKernelCall<FieldDepositKernel>(allSeparators, selected_starts, selected_starts2);


         P->CreateKernelCall<DebugDisplayKernel>("allSeparators", allSeparators);

         P->CreateKernelCall<DebugDisplayKernel>("key_separators", key_separators);

        P->CreateKernelCall<DebugDisplayKernel>("selected_starts2", selected_starts2);


        StreamSet * selectors = P->CreateStreamSet(1);




        // TODO: remove the need for the IdentifyLastSelector kernel

        P->CreateKernelCall<IdentifyLastSelector>(selector_span, selectors);

        StreamSet * const compressed = P->CreateStreamSet(HASH_BITS);




        P->CreateKernelCall<FieldCompressKernel>(Select(selectors, {0}), SelectOperationList{Select(hashes, streamutils::Range(0, HASH_BITS))}, compressed, 64);

        StreamSet * const outputs = P->CreateStreamSet(HASH_BITS);

        P->CreateKernelCall<StreamCompressKernel>(selectors, compressed, outputs, 64);

        StreamSet * const hashVals = P->CreateStreamSet(1, HASH_BITS);

        P->CreateKernelCall<P2S16Kernel>(outputs, hashVals);

        //


    }


    Scalar * const fileName = P->getInputScalar("fileName");

    // TODO: using the scan match here like this won't really let us determine what field is wrong in the case an error occurs
    // since the only information we get is whether a match fails.

    P->CreateKernelCall<CSVErrorIdentifier>(errors, allSeparators, ByteStream, fileName);

    return reinterpret_cast<CSVValidatorFunctionType>(P->compile());
}

void wc(CSVValidatorFunctionType fn_ptr, const fs::path & fileName) {
    struct stat sb;
    const auto fn = fileName.c_str();
    const int fd = open(fn, O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        if (errno == EACCES) {
            std::cerr << "csv_validator: " << fileName << ": Permission denied.\n";
        }
        else if (errno == ENOENT) {
            std::cerr << "csv_validator: " << fileName << ": No such file.\n";
        }
        else {
            std::cerr << "csv_validator: " << fileName << ": Failed.\n";
        }
        return;
    }
    if (stat(fileName.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) {
        std::cerr << "csv_validator: " << fileName << ": Is a directory.\n";
    } else {
        foundError = false;
        fn_ptr(fd, fn);
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&wcFlags, pablo_toolchain_flags(), codegen::codegen_flags()});
    if (argv::RecursiveFlag || argv::DereferenceRecursiveFlag) {
        argv::DirectoriesFlag = argv::Recurse;
    }
    CPUDriver pxDriver("csvv");

    allFiles = argv::getFullFileList(pxDriver, inputFiles);

    auto wordCountFunctionPtr = wcPipelineGen(pxDriver);

    #ifdef REPORT_PAPI_TESTS
    papi::PapiCounter<4> jitExecution{{PAPI_L3_TCM, PAPI_L3_TCA, PAPI_TOT_INS, PAPI_TOT_CYC}};
    jitExecution.start();
    #endif

    for (auto & file : allFiles) {
        wc(wordCountFunctionPtr, file);
    }

    #ifdef REPORT_PAPI_TESTS
    jitExecution.stop();
    jitExecution.write(std::cerr);
    #endif

    return 0;
}
