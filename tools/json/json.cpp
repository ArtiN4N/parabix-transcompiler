    /*
 *  Copyright (c) 2019 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include <kernel/io/source_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/basis/s2p_kernel.h>               // for S2PKernel
#include <kernel/io/stdout_kernel.h>               // for StdOutKernel
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/collapse.h>
#include <kernel/streamutils/multiplex.h>
#include <kernel/scan/scan.h>
#include <kernel/scan/reader.h>
#include <kernel/util/linebreak_kernel.h>
#include <llvm/IR/Function.h>                      // for Function, Function...
#include <llvm/IR/Module.h>                        // for Module
#include <llvm/Support/CommandLine.h>              // for ParseCommandLineOp...
#include <llvm/Support/Debug.h>                    // for dbgs
#include <pablo/pablo_kernel.h>                    // for PabloKernel
#include <toolchain/pablo_toolchain.h>
#include <pablo/parse/pablo_source_kernel.h>
#include <pablo/parse/pablo_parser.h>
#include <pablo/parse/simple_lexer.h>
#include <pablo/parse/rd_parser.h>
#include <kernel/core/kernel_builder.h>
#include <pablo/pe_zeroes.h>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/streamset.h>
#include <kernel/streamutils/streams_merge.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/builder.hpp>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <kernel/pipeline/pipeline_builder.h>
#include "json-kernel.h"
#include "postprocess/json-simple.h"
#include "postprocess/json-detail.h"
#include "postprocess/json-parens.h"
#include "postprocess/json2csv.h"

namespace su = kernel::streamutils;

using namespace pablo;
using namespace pablo::parse;
using namespace kernel;
using namespace llvm;
using namespace codegen;

static cl::OptionCategory jsonOptions("json Options", "json options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(jsonOptions));
bool ToCSVFlag;
static cl::opt<bool, true> ToCSVOption("c", cl::location(ToCSVFlag), cl::desc("Print equivalent CSV"), cl::cat(jsonOptions));
static cl::alias ToCSVAlias("to-csv", cl::desc("Alias for -c"), cl::aliasopt(ToCSVOption));
bool ShowLinesFlag;
static cl::opt<bool, true> ShowLinesOption("s", cl::location(ShowLinesFlag), cl::desc("Display line number on error"), cl::cat(jsonOptions));
static cl::alias ShowLinesAlias("show-lines", cl::desc("Alias for -s"), cl::aliasopt(ShowLinesOption));
static cl::opt<bool> ParallelBracketMatch("parallel-bracket-match", cl::desc("Apply parallel bracket matching."), cl::cat(jsonOptions));
static cl::opt<bool> ShowSpanLocations("show-spans", cl::desc("Generate span locations debug output"), cl::cat(jsonOptions));

typedef void (*jsonFunctionType)(uint32_t fd);

jsonFunctionType json_parsing_gen(CPUDriver & driver, std::shared_ptr<PabloParser> parser, std::shared_ptr<SourceFile> jsonPabloSrc) {

    auto & b = driver.getBuilder();
    Type * const int32Ty = b->getInt32Ty();
    auto P = driver.makePipeline({Binding{int32Ty, "fd"}});

    Scalar * const fileDescriptor = P->getInputScalar("fd");

    // Source data
    StreamSet * const codeUnitStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<MMapSourceKernel>(fileDescriptor, codeUnitStream);

    StreamSet * const u8basis = P->CreateStreamSet(8);
    P->CreateKernelCall<S2PKernel>(codeUnitStream, u8basis);

    // 1. Lexical analysis on basis stream
    StreamSet * const lexStream = P->CreateStreamSet(14);
    P->CreateKernelCall<PabloSourceKernel>(
        parser,
        jsonPabloSrc,
        "ClassifyBytes",
        Bindings { // Input Stream Bindings
            Binding {"basis", u8basis}
        },
        Bindings { // Output Stream Bindings
            Binding {"lex", lexStream}
        }
    );

    // 2. Find string marker (without backslashes)
    // 3. and make string span
    StreamSet * const stringMarker = P->CreateStreamSet(1);
    StreamSet * const stringSpan = P->CreateStreamSet(1);
    P->CreateKernelCall<JSONStringMarker>(
        lexStream,
        stringMarker,
        stringSpan
    );

    // 4. Mark end of keywords (true, false, null)
    // Note: We mark the words later when we sanitize the input because
    // lookahead only works on input streams
    StreamSet * const keywordEndMarkers = P->CreateStreamSet(3);
    P->CreateKernelCall<JSONKeywordEndMarker>(
        u8basis,
        lexStream,
        stringSpan,
        keywordEndMarkers
    );

    // 5. Validate numbers
    StreamSet * const numberLex = P->CreateStreamSet(1);
    StreamSet * const numberSpan = P->CreateStreamSet(1);
    StreamSet * const numberErr = P->CreateStreamSet(1);
    P->CreateKernelCall<JSONNumberSpan>(
        u8basis,
        lexStream,
        stringSpan,
        numberLex,
        numberSpan,
        numberErr
    );

    // 6. Validate strings
    StreamSet * const utf8Err = P->CreateStreamSet(1);
    P->CreateKernelCall<PabloSourceKernel>(
        parser,
        jsonPabloSrc,
        "ValidateUTF8",
        Bindings { // Input Stream Bindings
            Binding {"basis", u8basis},
            Binding {"lex", lexStream}
        },
        Bindings { // Output Stream Bindings
            Binding {"utf8Err", utf8Err}
        }
    );

    // 7. Clean lexers (in case there's special chars inside string)
    // 8. Validate rest of the output (check for extraneous chars)
    // We also take the opportunity to create the keyword marker
    StreamSet * const firstLexers = P->CreateStreamSet(7);
    StreamSet * const extraErr = P->CreateStreamSet(1);
    StreamSet * const keywordMarker = P->CreateStreamSet(1);
    StreamSet * const combinedBrackets = P->CreateStreamSet(1);
    StreamSet * const syntaxErr = P->CreateStreamSet(1);
    P->CreateKernelCall<JSONFindKwAndExtraneousChars>(
        lexStream,
        stringSpan,
        numberSpan,
        keywordEndMarkers,
        keywordMarker,
        firstLexers,
        combinedBrackets,
        extraErr,
        syntaxErr
    );

    // 9.1 Prepare StreamSets for validation
    StreamSet * collapsedLex;
    StreamSet * Errors;
    if (ToCSVFlag) {
        StreamSet * const allLex = P->CreateStreamSet(10, 1);
        P->CreateKernelCall<StreamsMerge>(
            std::vector<StreamSet *>{firstLexers, stringMarker, keywordMarker, numberLex, stringSpan},
            allLex
        );
        collapsedLex = su::Collapse(P, allLex);
        Errors = P->CreateStreamSet(3, 1);
        P->CreateKernelCall<StreamsMerge>(
            std::vector<StreamSet *>{extraErr, utf8Err, numberErr},
            Errors
        );
    } else {
        if (ShowLinesFlag) {
            StreamSet * allLex = P->CreateStreamSet(9, 1);
            P->CreateKernelCall<StreamsMerge>(
                std::vector<StreamSet *>{firstLexers, stringMarker, keywordMarker, numberLex},
                allLex
            );
            collapsedLex = su::Collapse(P, allLex);
        } else {
            if (ParallelBracketMatch) {
                StreamSet * const multiplexedBrackets = P->CreateStreamSet(2);
                P->CreateKernelCall<PabloSourceKernel>(
                    parser,
                    jsonPabloSrc,
                    "MultiplexBrackets",
                    Bindings {
                        Binding {"brackets", firstLexers}
                    },
                    Bindings {
                        Binding {"mpx", multiplexedBrackets}
                    }
                );
                StreamSet * const selectedBrackets = P->CreateStreamSet(2);
                FilterByMask(P, combinedBrackets, multiplexedBrackets, selectedBrackets);
                StreamSet * const toPostProcess = P->CreateStreamSet(1);
                P->CreateKernelCall<PabloSourceKernel>(
                    parser,
                    jsonPabloSrc,
                    "DeleteInlineBraces",
                    Bindings {
                        Binding {"mpx", selectedBrackets, FixedRate(1), LookAhead(1)}
                    },
                    Bindings {
                        Binding {"toPostProcess", toPostProcess}
                    }
                );
                StreamSet * const spreadFinal = P->CreateStreamSet(1);
                SpreadByMask(P, combinedBrackets, toPostProcess, spreadFinal);
                collapsedLex = su::Collapse(P, spreadFinal);
            } else {
                collapsedLex = combinedBrackets;
            }
        }
        Errors = P->CreateStreamSet(4, 1);
        P->CreateKernelCall<StreamsMerge>(
            std::vector<StreamSet *>{extraErr, utf8Err, numberErr, syntaxErr},
            Errors
        );
    }

    StreamSet * const Indices = scan::ToIndices(P, collapsedLex);

    // 9.2 Prepare error StreamSets
    StreamSet * const Errs = su::Collapse(P, Errors);
    StreamSet * const ErrIndices = scan::ToIndices(P, Errs);
    StreamSet * const Codes = su::Multiplex(P, Errs);

    // 9.3 Prepare StreamSets in case we want to show lines on error
    //    If flag -c is provided, parse for CSV
    auto normalJsonFn = SCAN_CALLBACK(postproc_validateObjectsAndArrays);
    auto simpleJsonFn = SCAN_CALLBACK(postproc_parensValidate);
    auto doneJsonFn = SCAN_CALLBACK(postproc_doneCallback);

    auto normalCsv2JsonFn = SCAN_CALLBACK(json2csv_validateObjectsAndArrays);
    auto simpleCsv2JsonFn = SCAN_CALLBACK(json2csv_simpleValidateObjectsAndArrays);
    auto doneCsv2JsonFn = SCAN_CALLBACK(json2csv_doneCallback);

    auto normalErrFn = SCAN_CALLBACK(postproc_errorStreamsCallback);
    auto simpleErrFn = SCAN_CALLBACK(postproc_parensError);

    if (ShowLinesFlag) {
        auto const LineBreaks = P->CreateStreamSet(1);
        P->CreateKernelCall<UnixLinesKernelBuilder>(codeUnitStream, LineBreaks, UnterminatedLineAtEOF::Add1);
        StreamSet * const LineNumbers = scan::LineNumbers(P, collapsedLex, LineBreaks);
        StreamSet * const LineSpans = scan::LineSpans(P, LineBreaks);
        StreamSet * const Spans = scan::FilterLineSpans(P, LineNumbers, LineSpans);

        // 9.4 Validate objects and arrays
        auto fn = ToCSVFlag ? normalCsv2JsonFn : normalJsonFn;
        auto doneFn = ToCSVFlag ? doneCsv2JsonFn : doneJsonFn;
        scan::Reader(P, driver, fn, doneFn, codeUnitStream, { Indices, Spans }, { LineNumbers, Indices });
        
        // 10. Output whether or not it is valid
        scan::Reader(P, driver, normalErrFn, codeUnitStream, { ErrIndices, Spans }, { LineNumbers, Codes });
    } else {
        // 9.4 Validate objects and arrays
        auto fn = ToCSVFlag ? simpleCsv2JsonFn : simpleJsonFn;
        auto doneFn = ToCSVFlag ? doneCsv2JsonFn : doneJsonFn;
        scan::Reader(P, driver, fn, doneFn, codeUnitStream, { Indices });

        // 10. Output whether or not it is valid
        scan::Reader(P, driver, simpleErrFn, codeUnitStream, { ErrIndices });
    }
    
// for debugging
    if (ShowSpanLocations) {
        StreamSet * filteredBasis = P->CreateStreamSet(8);
        P->CreateKernelCall<PabloSourceKernel>(
            parser,
            jsonPabloSrc,
            "SpanLocations",
            Bindings { // Input Stream Bindings
                Binding {"span", collapsedLex}
            },
            Bindings { // Output Stream Bindings
                Binding {"output", filteredBasis}
            }
        );
        StreamSet * filtered = P->CreateStreamSet(1, 8);
        P->CreateKernelCall<P2SKernel>(filteredBasis, filtered);
        P->CreateKernelCall<StdOutKernel>(filtered);
    }

    return reinterpret_cast<jsonFunctionType>(P->compile());
}

int main(int argc, char ** argv) {
    codegen::ParseCommandLineOptions(argc, argv, {&jsonOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    CPUDriver pxDriver("json");
    auto em = ErrorManager::Create();
    auto parser = RecursiveParser::Create(SimpleLexer::Create(em), em);
    auto jsonSource = SourceFile::Relative("json.pablo");
    if (jsonSource == nullptr) {
        std::cerr << "pablo-parser: error loading pablo source file: json.pablo\n";
    }
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        auto jsonParsingFunction = json_parsing_gen(pxDriver, parser, jsonSource);
        jsonParsingFunction(fd);
        close(fd);
    }
    return 0;
}
