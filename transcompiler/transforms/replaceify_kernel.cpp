#pragma once

#include <vector>
#include <fcntl.h>
#include <string>
#include <iostream>

#include "replace_bixData.h"
#include "replaceify_kernel.h"

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>

#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/streamutils/pdep_kernel.h>
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
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/bixnum/bixnum.h>

#include <grep/grep_kernel.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <re/toolchain/toolchain.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

Replaceify::Replaceify (KernelBuilder & b, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output)
: PabloKernel(b, "Replaceify" + std::to_string(Basis->getNumElements()) + "x1",
// inputs
{Binding{"basis", Basis}},
// output
{Binding{"Output", Output}}), mBixData(BixData) {
}

void Replaceify::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    UTF::UTF_Compiler unicodeCompiler(getInput(0), pb);

    std::vector<unicode::BitTranslationSets> nReplaceSets;
    nReplaceSets.push_back(mBixData.matchBitXorCCs(0));
    for (unsigned i = 1; i < mBixData.maxAdd; i++) {
        nReplaceSets.push_back(mBixData.matchBitCCs(i));
    }

    std::vector<std::vector<Var *>> nReplaceVars;
    nReplaceVars.assign(mBixData.maxAdd, {});

    unsigned j = 0;
    for (auto& set : nReplaceSets) {
        for (unsigned i = 0; i < set.size(); i++) {
            Var * v = pb.createVar("nAscii" + std::to_string(j) + "_bit" + std::to_string(i), pb.createZeroes());
            nReplaceVars[j].push_back(v);
            unicodeCompiler.addTarget(v, re::makeCC(set[i], &cc::Unicode));
        }

        j++;
    }

    if (LLVM_UNLIKELY(re::AlgorithmOptionIsSet(re::DisableIfHierarchy))) {
        unicodeCompiler.compile(UTF::UTF_Compiler::IfHierarchy::None);
    } else {
        unicodeCompiler.compile();
    }

    
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    Var * outputVar = getOutputStreamVar("Output");
    std::vector<PabloAST *> output_basis(basis.size());

    for (unsigned i = 0; i < basis.size(); i++) {
        auto initSet = nReplaceVars[0];
        if (i < initSet.size()) {
            output_basis[i] = pb.createXor(basis[i], initSet[i]);
        } else {
            output_basis[i] = basis[i];
        }

        for (unsigned j = 1; j < mBixData.maxAdd; j++) {
            auto set = nReplaceVars[j];
            if (i < set.size()) {
                output_basis[i] = pb.createOr(pb.createAdvance(set[i], j), output_basis[i]);
            }
        }

        pb.createAssign(pb.createExtract(outputVar, pb.getInteger(i)), output_basis[i]);
    }
}
