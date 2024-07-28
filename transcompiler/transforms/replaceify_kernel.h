#pragma once

#include <vector>

#include "replace_bixData.h"

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>

#include <pablo/codegenstate.h>
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/bixnum/bixnum.h>

#include <re/toolchain/toolchain.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>

class Replaceify : public pablo::PabloKernel {
public:
    Replaceify(KernelBuilder & b, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output);
protected:
    void generatePabloMethod() override;
    replace_bixData & mBixData;
};

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