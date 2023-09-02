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

typedef void (*CSVValidatorFunctionType)(uint32_t fd, const char * fileName);

struct CSVSchemaDefinition {
    std::vector<RE *> Validator;
    std::vector<unsigned> FieldValidatorIndex;
};

class CSVSchemaValidatorKernel : public pablo::PabloKernel {
public:
    CSVSchemaValidatorKernel(BuilderRef b, CSVSchemaDefinition schema, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid);
    llvm::StringRef getSignature() const override;
    bool hasSignature() const override { return true; }

protected:
    CSVSchemaValidatorKernel(BuilderRef b, CSVSchemaDefinition && schema, std::string signature, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid);
    void generatePabloMethod() override;
private:
    static std::string makeSignature(const std::vector<RE *> & fields);
private:
    CSVSchemaDefinition                 mSchema;
    std::string                         mSignature;

};

CSVSchemaDefinition parseSchemaFile() {
    CSVSchemaDefinition Def;

    std::ifstream schemaFile(inputSchema);
    if (LLVM_LIKELY(schemaFile.is_open())) {

        RE_MemoizingTransformer memo("memoizer");

        std::string line;

        boost::container::flat_map<RE *, unsigned> M;

        while(std::getline(schemaFile, line)) {
            RE * const original = RE_Parser::parse(line);
            assert (original);
            RE * const field = regular_expression_passes(memo.transformRE(original));
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

        Def.Validator.resize(M.size());
        for (auto p : M) {
            Def.Validator[p.second] = p.first;
        }

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
    out.flush();
    return tmp;
}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, CSVSchemaDefinition schema, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid)
: CSVSchemaValidatorKernel(b, std::move(schema), makeCSVSchemaDefinitionName(schema), basisBits, recordSeperators, fieldMarkers, invalid) {

}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, CSVSchemaDefinition && schema, std::string signature, StreamSet * basisBits, StreamSet * recordSeperators, StreamSet * fieldMarkers, StreamSet * invalid)
: PabloKernel(b, "csvv" + getStringHash(signature),
    {Binding{"basisBits", basisBits}, Binding{"recordSeperators", recordSeperators}, Binding{"fieldMarkers", fieldMarkers}},
    {Binding{"invalid", invalid}})
, mSchema(schema)
, mSignature(std::move(signature)) {
    addAttribute(InfrequentlyUsed());
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

 //   re_compiler.addPrecompiled(FieldSepName, RE_Compiler::ExternalStream(RE_Compiler::Marker(fieldSeps, 0), std::make_pair<int,int>(1, 1)));

    // TODO: if the number of validators equals the number of fields, just scan sequentially? We expect everything to be valid but if the
    // total schema length is "long", we won't necessarily be starting a new record every block. Can we "break up" the validation checks to
    // test if we should scan through a chunk of them based on the current position?

    const auto & validators = mSchema.Validator;
    std::vector<PabloAST *> matches(validators.size());
    for (unsigned i = 0; i < validators.size(); ++i) {
        RE_Compiler::Marker match = re_compiler.compileRE(validators[i]);
        matches[i] = match.stream();
    }

    PabloAST * const mask = pb.createNot(pb.createOr(fieldSeps, recordSeperators), "mask");

    RE_Compiler::Marker startMatch = re_compiler.compileRE(makeStart());
    PabloAST * const allStarts = startMatch.stream();

    const auto & fields = mSchema.FieldValidatorIndex;

    PabloAST * currentPos = allStarts;
    for (unsigned i = 0; i < fields.size(); ++i) {
        currentPos = pb.createAdvanceThenScanThru(currentPos, mask);
        currentPos = pb.createAnd(currentPos, matches[fields[i]]);
    }
    PabloAST * result = pb.createXor(currentPos, recordSeperators, "result");

    Var * const output = pb.createExtract(getOutputStreamVar("invalid"), pb.getInteger(0));
    pb.createAssign(output, result);

}

extern "C" void accumulate_match_wrapper(char * fileName, const size_t lineNum, char * line_start, char * line_end) {
    std::string tmp;
    tmp.reserve(256);
    raw_string_ostream out(tmp);
    out << "Error found in " << fileName << ':' << lineNum;
    // TODO: this needs to report more information as to what field/rule was invalid
    report_fatal_error(out.str());
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
