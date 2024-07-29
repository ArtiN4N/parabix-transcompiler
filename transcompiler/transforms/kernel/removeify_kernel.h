#pragma once

#include <vector>
#include <array>
#include <fcntl.h>
#include <string>
#include <iostream>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyAliases.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>


#include <kernel/streamutils/deletion.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>

#include <pablo/codegenstate.h>

#include <grep/grep_kernel.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <re/transforms/re_simplifier.h>
#include <re/toolchain/toolchain.h>
#include <re/unicode/resolve_properties.h>
#include <re/parse/parser.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>


using namespace kernel;
using namespace llvm;
using namespace pablo;

// from csv2json.cpp
class Invert : public PabloKernel {
public:
    Invert(KernelBuilder & b, StreamSet * mask, StreamSet * inverted)
        : PabloKernel(b, "Invert",
                      {Binding{"mask", mask}},
                      {Binding{"inverted", inverted}}) {}
protected:
    void generatePabloMethod() override;
};

void Invert::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    PabloAST * mask = getInputStreamSet("mask")[0];
    PabloAST * inverted = pb.createInFile(pb.createNot(mask));
    Var * outVar = getOutputStreamVar("inverted");
    pb.createAssign(pb.createExtract(outVar, pb.getInteger(0)), inverted);
}

void doRemoveTransform(const std::unique_ptr<ProgramBuilder> & P, std::string toRemoveStr, StreamSet * Basis, StreamSet * Output);

void doRemoveTransform(const std::unique_ptr<ProgramBuilder> & P, std::string toRemoveStr, StreamSet * Basis, StreamSet * Output) {
    re::RE * toRemoveRegex = re::simplifyRE(re::RE_Parser::parse(toRemoveStr));
    toRemoveRegex = UCD::linkAndResolve(toRemoveRegex);
    toRemoveRegex = UCD::externalizeProperties(toRemoveRegex);
    re::CC * toRemoveClass = dyn_cast<re::CC>(toRemoveRegex);

    StreamSet * toRemoveMarker = P->CreateStreamSet(1);
    std::vector<re::CC *> toRemoveMarker_CC = {toRemoveClass};
    P->CreateKernelCall<CharacterClassKernelBuilder>(toRemoveMarker_CC, Basis, toRemoveMarker);

    StreamSet * toKeepMarker = P->CreateStreamSet(1);
    P->CreateKernelCall<Invert>(toRemoveMarker, toKeepMarker);

    FilterByMask(P, toKeepMarker, Basis, Output);
}