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
#include <grep/regex_passes.h>
#ifdef ENABLE_PAPI
#include <util/papi_helper.hpp>
#endif

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;

static cl::OptionCategory wcFlags("Command Flags", "csv validator options");


static cl::opt<std::string> inputSchema(cl::Positional, cl::desc("<input schema filename>"), cl::Required, cl::cat(wcFlags));

static cl::list<std::string> inputFiles(cl::Positional, cl::desc("<input file ...>"), cl::OneOrMore, cl::cat(wcFlags));


constexpr int ScanMatchBlocks = 4;

bool strictRFC4180 = false;


std::vector<fs::path> allFiles;



using namespace pablo;
using namespace kernel;
using namespace cc;
using namespace re;

#if 0
class CSVDataLexer : public PabloKernel {
public:
    CSVDataLexer(BuilderRef kb, StreamSet * Source, StreamSet * CSVlexical)
        : PabloKernel(kb, "CSVDataLexer",
                      {Binding{"Source", Source}},
                      {Binding{"CSVlexical", CSVlexical, FixedRate(), Add1()}}) {}
protected:
    void generatePabloMethod() override;
};

enum {markLF = 0, markCR = 1, markDQ = 2, markComma = 3, markEOF = 4};

void CSVDataLexer::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::unique_ptr<cc::CC_Compiler> ccc;
    ccc = std::make_unique<cc::Parabix_CC_Compiler_Builder>(getEntryScope(), getInputStreamSet("Source"));
    PabloAST * LF = ccc->compileCC(re::makeCC(charLF, &cc::UTF8));
    PabloAST * CR = ccc->compileCC(re::makeCC(charCR, &cc::UTF8));
    PabloAST * DQ = ccc->compileCC(re::makeCC(charDQ, &cc::UTF8));
    PabloAST * Comma = ccc->compileCC(re::makeCC(charComma, &cc::UTF8));
    PabloAST * EOFbit = pb.createAtEOF(pb.createAdvance(pb.createOnes(), 1));
    Var * lexOut = getOutputStreamVar("CSVlexical");
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markLF)), LF);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markCR)), CR);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markDQ)), DQ);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markComma)), Comma);
    pb.createAssign(pb.createExtract(lexOut, pb.getInteger(markEOF)), EOFbit);
}

#endif

enum CSVDataFieldMarkers {
    FieldDataMask = 0
    , FieldSeperator = 1
};



class CSVDataParser : public PabloKernel {
public:
    CSVDataParser(BuilderRef kb, StreamSet * UTFindex, StreamSet * csvMarks, StreamSet * recordSeperators, StreamSet * fieldMarkers)
        : PabloKernel(kb, "CSVDataParser",
                      {Binding{"UTFindex", UTFindex}, Binding{"csvMarks", csvMarks, FixedRate(), LookAhead(1)}},
                      {Binding{"fieldMarkers", fieldMarkers}, Binding{"recordSeperators", recordSeperators}}) {
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
    PabloAST * recordMarks = pb.createOr(pb.createAnd(csvMarks[markLF], unquoted), csvMarks[markEOF]);
    PabloAST * fieldMarks = pb.createOr(pb.createAnd(csvMarks[markComma], unquoted), recordMarks);

    // should field / record marks be part of the data? they'd indicate a different sort of RE marker
//    PabloAST * marks = pb.createOr(dquote, pb.createOr(fieldMarks, recordMarks));



    PabloAST * fieldData = pb.createOr(pb.createNot(dquote), escaped_quote);
    // TODO: is this UTF indexing needed?
    PabloAST * utfIndex = pb.createExtract(getInputStreamVar("UTFindex"), pb.getInteger(0));
    fieldData = pb.createAnd(fieldData, utfIndex, "fdata");

    Var * fieldMarkers = getOutputStreamVar("fieldMarkers");

    pb.createAssign(pb.createExtract(fieldMarkers, pb.getInteger(FieldDataMask)), fieldData);
    pb.createAssign(pb.createExtract(fieldMarkers, pb.getInteger(FieldSeperator)), fieldMarks);

    Var * recordSeperators = getOutputStreamVar("recordSeperators");

    pb.createAssign(pb.createExtract(recordSeperators, pb.getInteger(0)), recordMarks);

}

#if 0

class StructureIdentifier final: public pablo::PabloKernel {
public:
    StructureIdentifier(BuilderRef b, StreamSet * const text, StreamSet * const breaks, StreamSet * const deletionFollowsMask);
protected:
    void generatePabloMethod() override;
private:
    std::vector<char> Quotes = {'"', '\''};
    std::vector<char> FieldBreak = {','};
    std::vector<char> RecordBreak = {'\n'};
    std::vector<char> Escape = {'\\'};
};

StructureIdentifier::StructureIdentifier (BuilderRef b, StreamSet * const text, StreamSet * const breaks, StreamSet * const deletionFollowsMask)
: PabloKernel(b, "csv_structure", // make name depend on options
    {Bind("text", text)},
    {Bind("breaks", breaks), Bind("deletionFollows", deletionFollowsMask)},
    {},
    {}) {
    assert (breaks->getNumElements() == 2);
    assert (deletionFollowsMask->getNumElements() == 1);
}

void StructureIdentifier::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    Parabix_CC_Compiler_Builder ccc(getEntryScope(), getInputStreamSet("text"));


    const auto n = Quotes.size();

    std::vector<Var *> quoteMark(n);

    for (unsigned i = 0; i < n; ++i) {
        quoteMark[i] = pb.createVar("q", ccc.compileCC(re::makeByte(Quotes[i])));
    }

    PabloAST * alternatingZeroOne = pb.createRepeat(1, pb.getInteger(0b10101010, 8));
    PabloAST * alternatingOneZero = pb.createNot(alternatingZeroOne);

    PabloAST * zeroes = pb.createZeroes();

    bool hasEscapeChar = !strictRFC4180;


    std::vector<Var *> followsFilteredChar(n + (hasEscapeChar ? 1 : 0));

    auto identifyMarkAfterOddLengthRuns = [&](PabloBuilder & it, PabloAST * first, PabloAST * span, const unsigned index) -> PabloAST * {
        PabloAST * negFirst = it.createAnd(first, alternatingZeroOne);
        PabloAST * markAfterNegRun = it.createScanThru(negFirst, span);        
        PabloAST * markAfterOddLengthNegRun = it.createAnd(markAfterNegRun, alternatingOneZero);

        PabloAST * negMarked = it.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {negFirst, it.createNot(span)});
        negMarked = it.createAnd(negMarked, alternatingOneZero);

        PabloAST * posFirst = it.createAnd(first, alternatingOneZero);
        PabloAST * markAfterPosRun = it.createScanThru(posFirst, span);
        PabloAST * markAfterOddLengthPosRun = it.createAnd(markAfterPosRun, alternatingZeroOne);

        PabloAST * posMarked = it.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {posFirst, it.createNot(span)});
        posMarked = it.createAnd(posMarked, alternatingZeroOne);

        // Although not permited by RFC 4180, suppose both ' and " are valid quote characters.  If we mark
        // all of the open/close quotes and escape characters for deletion, we run into a problem. Ideally,
        // we want:

        //     TEXT = ''''     TEXT='"'''     TEXT=' "'' '      TEXT='""',"''"
        //            ^ ^           ^ ^ ^          ^  ^  ^           ^  ^ ^  ^

        // But we don't have enough information to decide that here. The best we can do is try and decide
        // which characters we may want to keep, filter out the false matches, and do a lookahead later.
        // Thus we end up with something like:

        //     TEXT = ''''     TEXT='"'''     TEXT=' "'' '      TEXT='""',"''"
        //             ^ ^           ^*^ ^          ^ *^  ^           ^ *^ ^ *^


        Var * var = pb.createVar("followsFiltered", zeroes);
        followsFilteredChar[index] = var;
        it.createAssign(var, it.createOr(negMarked, posMarked));

        return it.createOr(markAfterOddLengthNegRun, markAfterOddLengthPosRun);
    };

    auto compileCC =[&](const std::vector<char> & CCs) -> PabloAST * {
        assert (!CCs.empty());
        CC * cc = re::makeByte(CCs[0]);
        for (unsigned i = 1; i < n; ++i) {
            cc->insert(CCs[i]);
        }
        return ccc.compileCC(cc);
    };

    if (!Escape.empty()) {
        PabloAST * escapeMark = compileCC(Escape);

        auto it = pb.createScope();
        pb.createIf(escapeMark, it);

        // Look through runs of ///...// to see if there are an odd number of them. If so, the char after
        // the last one is escaped out.

        PabloAST * first = it.createAnd(it.createNot(it.createAdvance(escapeMark, 1)), escapeMark);
        PabloAST * nonEscapedChars = it.createNot(identifyMarkAfterOddLengthRuns(it, first, escapeMark, n));

        for (unsigned i = 0; i < n; ++i) {
            PabloAST * q = it.createAnd(quoteMark[i], nonEscapedChars);
            it.createAssign(quoteMark[i], q);
        }

    }

    std::vector<Var *> followsQuote(n);
    std::vector<Var *> advancedQuote(n);
    for (unsigned i = 0; i < n; ++i) {
        followsQuote[i] = pb.createVar("followsQuote", zeroes);
        advancedQuote[i] = pb.createVar("advQuote", zeroes);
    }

    Var * allQuoteFollows = pb.createVar("allFollows", zeroes);

    for (unsigned i = 0; i < n; ++i) {
        // similar to above, we want to filter out adjacent quote marks. Unlike the / escapes, we will end up being
        // one position after the quote we're interested in. So if we have:

        // TEXT     = """ """""
        // QM       = ...1.....1

        // Ideal QM = 1.......1.

        // This is problematic since what we consider the actual quote mark as opposed to the position after mark
        // is dependent on whether we're starting or closing a mark span. We fix that in a subsequent stage
        // by and only focus on finding the non-escaped quote marks

        auto quoteScope = pb.createScope();
        PabloAST * q = quoteMark[i];
        pb.createIf(q, quoteScope);
        PabloAST * advQ = quoteScope.createAdvance(q, 1);
        quoteScope.createAssign(advancedQuote[i], advQ);

        PabloAST * first = quoteScope.createAnd(quoteScope.createNot(advQ), q);
        quoteScope.createAssign(followsQuote[i], identifyMarkAfterOddLengthRuns(quoteScope, first, q, i));
        quoteScope.createAssign(allQuoteFollows, quoteScope.createOr(first, followsQuote[i]));
    }

    // Although not permited by RFC 4180, suppose both ' and " are valid quote characters. We may have
    // TEXT pattern such as '"""' , which should be considered a field of three double quotes. A more
    // problematic example would be "'",'"''' .

    // To consider "mixed" runs like that, we need to iterate sequentially through spans to decide what
    // type of span the "outermost" one is and remove any non-matching follows that precede the next
    // non-escaped matching one.

    // TODO: check if we have any mixed types. even if the validator supports multiple quote mark types
    // its unlikely that a document mixes them this pathelogical fashion.

    // ----------------------------------------------------------------------------------------------

    std::vector<Var *> followStarts(n);
    std::vector<Var *> followEnds(n);
    for (unsigned i = 0; i < n; ++i) {
        followStarts[i] = pb.createVar("starts", zeroes);
        followEnds[i] = pb.createVar("ends", zeroes);
    }

    // get the first unprocessed follows? better than doing the insert first trick and scanning through to "here"?
    PabloAST * start = pb.createNot(pb.createAdvance(pb.createNot(zeroes), 1));
    PabloAST * first = pb.createScanTo(start, allQuoteFollows);

    Var * anyOpener = pb.createVar("anyOpener", first);

    // TODO: once identified, we probably no longer need to distinguish between quote type

    auto filterScope = pb.createScope();
    pb.createWhile(anyOpener, filterScope);
    // ----------------------------------------------------------------------------------------------
    Var * next = filterScope.createVar("next", zeroes);
    for (unsigned i = 0; i < n; ++i) {
        auto check = filterScope.createScope();
        PabloAST * s = filterScope.createAnd(first, followsQuote[i]);
        pb.createIf(s, check);
        // ----------------------------------------------------------------------------------------------

        // NOTE: to avoid the potential clean up problem of "'","'" we only store the start and end
        // positions rather than the full span mask here. We later clean those out without worrying
        // about them crossing over multiple fields.

        PabloAST * t = check.createAdvanceThenScanTo(s, followsQuote[i]);
        check.createAssign(followStarts[i], check.createOr(followStarts[i], s));
        check.createAssign(followEnds[i], check.createOr(followEnds[i], t));

        check.createAssign(next, t);
        // ----------------------------------------------------------------------------------------------
    }
    PabloAST * nextOpener = filterScope.createAdvanceThenScanTo(next, allQuoteFollows);
    filterScope.createAssign(anyOpener, nextOpener);
    // ----------------------------------------------------------------------------------------------

    Var * unquoted = pb.createVar("unquotedText", pb.createNot(zeroes));

    for (unsigned i = 0; i < n; ++i) {
        auto check = pb.createScope();
        pb.createIf(followStarts[i], check);
        // ----------------------------------------------------------------------------------------------
        PabloAST * spans = check.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, {followStarts[i], followEnds[i]});
        PabloAST * unfiltered = check.createNot(spans);
        // remove any deletion follows from streams that are enclosed by a different type of quotes
        for (unsigned j = 0; j < i; ++j) {
            Var * f = followsFilteredChar[j];
            check.createAssign(f, check.createAnd(f, unfiltered));
        }
        for (unsigned j = i + 1; j < n; ++j) {
            Var * f = followsFilteredChar[j];
            check.createAssign(f, check.createAnd(f, unfiltered));
        }
        check.createAssign(unquoted, check.createAnd(unquoted, unfiltered));
        // ----------------------------------------------------------------------------------------------
    }

    Var * breaks = getOutput(0);
    PabloAST * fieldBreakChars = compileCC(FieldBreak);
    pb.createAssign(pb.createExtract(breaks, 0), pb.createAnd(fieldBreakChars, unquoted));

    PabloAST * recordBreakChars = compileCC(RecordBreak);
    pb.createAssign(pb.createExtract(breaks, 1), pb.createAnd(recordBreakChars, unquoted));

    Var * f = followsFilteredChar[0];

    for (unsigned i = 1; i < followsFilteredChar.size(); ++i) {
        pb.createAssign(f, pb.createOr(f, followsFilteredChar[i]));
    }

    Var * deletion = getOutput(1);
    pb.createAssign(pb.createExtract(deletion, 0), f);
}

class PullBackOne final: public pablo::PabloKernel {
public:
    PullBackOne(BuilderRef b, StreamSet * const input, StreamSet * const output);
protected:
    void generatePabloMethod() override;
};

PullBackOne::PullBackOne (BuilderRef b, StreamSet * const input, StreamSet * const output)
: PabloKernel(b, "csv_structure", // make name depend on options
    {Bind("input", input, LookAhead(1))},
    {Bind("output", output)},
    {},
    {}) {
    assert (input->getNumElements() == 1);
    assert (output->getNumElements() == 1);
    addAttribute(SideEffecting());
}

void PullBackOne::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    pb.createAssign(pb.createExtract(getOutput(0), 0), pb.createLookahead(getInput(0), 1));
}

#endif

typedef void (*CSVValidatorFunctionType)(uint32_t fd, const char * fileName);

#if 0

WordCountFunctionType wcPipelineGen(CPUDriver & pxDriver) {

    auto & iBuilder = pxDriver.getBuilder();

    Type * const int32Ty = iBuilder->getInt32Ty();

    auto P = pxDriver.makePipeline({Binding{int32Ty, "fd"}});

    Scalar * const fileDescriptor = P->getInputScalar("fd");

    StreamSet * const ByteStream = P->CreateStreamSet(1, 8);

    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);

    auto BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);



    auto breaks = P->CreateStreamSet(2, 1);
    auto delFollows = P->CreateStreamSet(1, 1);

    P->CreateKernelCall<StructureIdentifier>(BasisBits, breaks, delFollows);


    auto deletions = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<PullBackOne>(delFollows, deletions);

    // Do we need to delete all the data from the basis bits and derived streams? It might be better
    // if we can treat them as skippable characters in the RE matchers. It would be easy to have optional
    // quote starts but if we don't enforce that a field is either quoted or unquoted entirely (e.g.,
    // treat "123"45"67" as illegal, as per RFC 4180), then this becomes far more difficult. Further,
    // if we allow explicit escaped chars using / instead of only considering "", then we'd have
    // to fall back to deleting all of them.

    return reinterpret_cast<WordCountFunctionType>(P->compile());
}

#endif

class CSVSchemaValidatorKernel : public pablo::PabloKernel {
public:
    CSVSchemaValidatorKernel(BuilderRef b, RE * schema, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid);
    llvm::StringRef getSignature() const override;
    bool hasSignature() const override { return true; }

protected:
    CSVSchemaValidatorKernel(BuilderRef b, RE * schema, std::string signature, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid);
    void generatePabloMethod() override;
private:
    static std::string makeSignature(const std::vector<RE *> & fields);
private:
    RE * const                          mSchema;
    std::string                         mSignature;

};

const static std::string FieldSepName = ":";

RE * parseSchemaFile() {
    RE * parsedSchema = nullptr;
    std::vector<RE *> fields;
    std::ifstream schemaFile(inputSchema);
    if (LLVM_LIKELY(schemaFile.is_open())) {
        std::string line;
        Name * fs = nullptr;
        while(std::getline(schemaFile, line)) {
            RE_Parser::parse(line);
            RE * delim = nullptr;
            if (fs == nullptr) {
                fs = makeName(FieldSepName);
                fs->setDefinition(makeAny()); // ExternalPropertyName::Create(FieldSepName.c_str()));
                delim = makeStart();
            } else {
                delim = fs;
            }
            assert (delim);
            fields.push_back(delim);
            RE * const field = RE_Parser::parse(line);
            assert (field);
            fields.push_back(field);
        }
        schemaFile.close();
        fields.push_back(makeEnd());
        for (auto r : fields) {
            assert (r);
        }
        parsedSchema = makeSeq(fields.begin(), fields.end());
        // TODO: ideally we want to "reuse" the equations for fields with the same validation RE. Instead of making
        // a single big RE here, make separate ones. Can we automatically contract a sequence of the same RE into a range?
        assert (parsedSchema);
        parsedSchema = regular_expression_passes(parsedSchema);
    } else {
        report_fatal_error("Cannot open schema: " + inputSchema);
    }
    return parsedSchema;
}


CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, RE * schema, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid)
: CSVSchemaValidatorKernel(b, schema, Printer_RE::PrintRE(schema), basisBits, recordSeperators, fieldMarkers, invalid) {

}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, RE * schema, std::string signature, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid)
: PabloKernel(b, "csvv" + getStringHash(signature),
    {Binding{"basisBits", basisBits}, Binding{"recordSeperators", recordSeperators}, Binding{"fieldMarkers", fieldMarkers}},
    {Binding{"invalid", invalid}})
, mSchema(schema)
, mSignature(std::move(signature)) {
    addAttribute(InfrequentlyUsed());
    assert (mSchema);
}


StringRef CSVSchemaValidatorKernel::getSignature() const {
    return mSignature;
}

void CSVSchemaValidatorKernel::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());

    Var * recordSeperators = pb.createExtract(getInputStreamVar("recordSeperators"), pb.getInteger(0));

    RE_Compiler re_compiler(getEntryScope(), recordSeperators, &cc::UTF8);

    re_compiler.addAlphabet(&cc::UTF8, getInputStreamSet("basisBits"));

    Var * const fieldMarkers = getInputStreamVar("fieldMarkers");

    re_compiler.setIndexing(&cc::UTF8, pb.createExtract(fieldMarkers, pb.getInteger(FieldDataMask)));

    Var * const fieldSeps = pb.createExtract(fieldMarkers, pb.getInteger(FieldSeperator));
    assert (fieldSeps->getType() == getStreamTy());

    re_compiler.addPrecompiled(FieldSepName, RE_Compiler::ExternalStream(RE_Compiler::Marker(fieldSeps, 0), std::make_pair<int,int>(1, 1)));

    RE_Compiler::Marker matches = re_compiler.compileRE(mSchema);
    errs() << "matches.offset=" << matches.offset() << "\n";
    Var * const output = pb.createExtract(getOutputStreamVar("invalid"), pb.getInteger(0));
    PabloAST * result = pb.createXor(matches.stream(), recordSeperators, "result");
    pb.createAssign(output, result);

}

extern "C" void accumulate_match_wrapper(char * fileName, const size_t lineNum, char * line_start, char * line_end) {
    std::string tmp;
    tmp.reserve(256);
    raw_string_ostream out(tmp);
    out << "Error found in " << fileName << ':' << lineNum;
    // TODO: this needs to report more information as to what field/rule was invalid
    throw std::runtime_error(out.str());
}

extern "C" void finalize_match_wrapper(char * fileName, char * buffer_end) {
    errs() << "No errors found in " << fileName << "\n";
}

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
    StreamSet * recordSeperators = P->CreateStreamSet(1);
    StreamSet * fieldMarkers = P->CreateStreamSet(2);

    StreamSet * u8index = P->CreateStreamSet();
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);

    P->CreateKernelCall<CSVDataParser>(u8index, csvCCs, recordSeperators, fieldMarkers);

    StreamSet * invalid = P->CreateStreamSet(1);

    P->CreateKernelFamilyCall<CSVSchemaValidatorKernel>(parseSchemaFile(), BasisBits, recordSeperators, fieldMarkers, invalid);

    Scalar * const fileName = P->getInputScalar("fileName");

    // TODO: using the scan match here like this won't really let us determine what field is wrong in the case an error occurs
    // since the only information we get is whether a match fails.

    Kernel * const scanMatchK = P->CreateKernelCall<ScanMatchKernel>(invalid, recordSeperators, ByteStream, fileName, ScanMatchBlocks);
    scanMatchK->link("accumulate_match_wrapper", accumulate_match_wrapper);
    scanMatchK->link("finalize_match_wrapper", finalize_match_wrapper);

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
