/*
 *  Copyright (c) 2015 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include <re/re_toolchain.h>
#include <cc/cc_compiler.h>            // for CC_Compiler
#include <llvm/Support/CommandLine.h>  // for clEnumVal, clEnumValEnd, Optio...
#include <re/re_compiler.h>            // for RE_Compiler
#include <re/re_nullable.h>            // for RE_Nullable
#include <re/re_star_normal.h>         // for RE_Star_Normal
#include <re/re_simplifier.h>          // for RE_Simplifier
#include <re/printer_re.h>
#include <iostream>

using namespace pablo;
using namespace llvm;

namespace re {

static cl::OptionCategory RegexOptions("Regex Toolchain Options",
                                              "These options control the regular expression transformation and compilation.");
const cl::OptionCategory * re_toolchain_flags() {
    return &RegexOptions;
}

static cl::bits<RE_PrintFlags> 
    PrintOptions(cl::values(clEnumVal(ShowREs, "Print parsed or generated regular expressions"),
                            clEnumVal(ShowAllREs, "Print all regular expression passes"),
                            clEnumVal(ShowStrippedREs, "Print REs with nullable prefixes/suffixes removed"),
                            clEnumVal(ShowSimplifiedREs, "Print final simplified REs")
                            ), cl::cat(RegexOptions));

static cl::bits<RE_AlgorithmFlags>
    AlgorithmOptions(cl::values(clEnumVal(DisableLog2BoundedRepetition, "disable log2 optimizations for bounded repetition of bytes"),
                              clEnumVal(DisableIfHierarchy, "disable nested if hierarchy for generated Unicode classes (not recommended)"), 
                              clEnumVal(DisableMatchStar, "disable MatchStar optimization"), 
                              clEnumVal(DisableUnicodeMatchStar, "disable Unicode MatchStar optimization"),
                              clEnumVal(DisableUnicodeLineBreak, "disable Unicode line breaks - use LF only")
                              ),
                   cl::cat(RegexOptions));

bool AlgorithmOptionIsSet(RE_AlgorithmFlags flag) {
    return AlgorithmOptions.isSet(flag);
}

int IfInsertionGap;
static cl::opt<int, true> 
    IfInsertionGapOption("if-insertion-gap",  cl::location(IfInsertionGap), cl::init(3),
                         cl::desc("minimum number of nonempty elements between inserted if short-circuit tests"), 
                         cl::cat(RegexOptions));



RE * regular_expression_passes(RE * re_ast)  {
    if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowREs)) {
        std::cerr << "Parser:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }

    //Optimization passes to simplify the AST.
    re_ast = re::RE_Nullable::removeNullablePrefix(re_ast);
    if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowStrippedREs)) {
        std::cerr << "RemoveNullablePrefix:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }
    re_ast = re::RE_Nullable::removeNullableSuffix(re_ast);
    if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowStrippedREs)) {
        std::cerr << "RemoveNullableSuffix:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }
    re_ast = re::RE_Nullable::removeNullableAssertion(re_ast);
    if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowStrippedREs)) {
        std::cerr << "RemoveNullableAssertion:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }
    //re_ast = re::RE_Nullable::removeNullableAfterAssertion(re_ast);
    //if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowStrippedREs)) {
    //    std::cerr << "RemoveNullableAfterAssertion" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    //}
    
    re_ast = re::RE_Simplifier::simplify(re_ast);
    if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowSimplifiedREs)) {
        //Print to the terminal the AST that was generated by the simplifier.
        std::cerr << "Simplifier:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }

    re_ast = re::RE_Star_Normal::star_normal(re_ast);
    if (PrintOptions.isSet(ShowAllREs) || PrintOptions.isSet(ShowSimplifiedREs)) {
        //Print to the terminal the AST that was transformed to the star normal form.
        std::cerr << "Star_Normal_Form:" << std::endl << Printer_RE::PrintRE(re_ast) << std::endl;
    }    
    return re_ast;
}
    
void re2pablo_compiler(PabloKernel * kernel, RE * re_ast) {
    Var * const basis = kernel->getInputStreamVar("basis");
    cc::CC_Compiler cc_compiler(kernel, basis);
    re::RE_Compiler re_compiler(kernel, cc_compiler);
    re_compiler.compileUnicodeNames(re_ast);
    re_compiler.compile(re_ast);
}

}
