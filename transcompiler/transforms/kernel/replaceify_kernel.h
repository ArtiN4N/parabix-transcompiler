#pragma once

#include <vector>
#include <array>
#include <fcntl.h>
#include <string>
#include <iostream>

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

struct replace_bixData {
    template <std::size_t N>
    replace_bixData(std::array<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>, N>);
    std::vector<re::CC *> insertionBixNumCCs();
    unicode::BitTranslationSets matchBitXorCCs(unsigned);
    unicode::BitTranslationSets matchBitCCs(unsigned);
    unsigned bitsNeeded;
    unsigned maxAdd;
private:
    std::unordered_map<codepoint_t, unsigned> mInsertLength;
    std::vector<unicode::TranslationMap> mCharMap;
};

template <std::size_t N>
replace_bixData::replace_bixData(std::array<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>, N> data) {
    maxAdd = 0;
    for (auto& pair : data) {
        mInsertLength.emplace(pair.first, pair.second.size());
        if (pair.second.size() > maxAdd) {
            
            for (auto i = 0; i < (pair.second.size() - maxAdd); i++) {
                mCharMap.push_back({});
            }
            maxAdd++;  
        }

        unsigned int i = 0;
        for (auto& target : pair.second) {
            mCharMap[i].emplace(pair.first, target);
            i++;
        }
    }

    unsigned n = maxAdd;

    

    bitsNeeded = 0;
    while (n) {
        bitsNeeded++;
        n >>= 1;
    }
}

std::vector<re::CC *> replace_bixData::insertionBixNumCCs() {
    unicode::BitTranslationSets BixNumCCs;

    for (unsigned i = 0; i < bitsNeeded; i++) {
        BixNumCCs.push_back(UCD::UnicodeSet());
    }

    for (auto& p : mInsertLength) {
        auto insert_amt = p.second - 1;

        unsigned bitAmt = 1;
        for (unsigned i = 0; i < bitsNeeded; i++) {
            if ((insert_amt & bitAmt) == bitAmt) {
                BixNumCCs[i].insert(p.first);
            }
            bitAmt <<= 1;
        }
    }

    std::vector<re::CC *> ret;
    for (unsigned i = 0; i < bitsNeeded; i++) {
        ret.push_back(re::makeCC(BixNumCCs[i], &cc::Unicode));
    }
    

    return ret;
}

unicode::BitTranslationSets replace_bixData::matchBitXorCCs(unsigned i) {
    return unicode::ComputeBitTranslationSets(mCharMap[i]);
}

unicode::BitTranslationSets replace_bixData::matchBitCCs(unsigned i) {
    return unicode::ComputeBitTranslationSets(mCharMap[i], unicode::XlateMode::LiteralBit);
}

class Replaceify : public pablo::PabloKernel {
public:
    Replaceify(KernelBuilder & b, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output)
    : pablo::PabloKernel(b, "Replaceify",
                        {Binding{"Basis", Basis}},
                            {Binding{"Output", Output}}), mBixData(BixData) {}

protected:
    void generatePabloMethod() override;
private:
    replace_bixData & mBixData;
};

void Replaceify::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    UTF::UTF_Compiler unicodeCompiler(getInput(0), pb);

    std::cout << "called replaceify" << std::endl;
    std::cout << mBixData.maxAdd << ", " << mBixData.bitsNeeded << std::endl;


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

    
    std::vector<PabloAST *> basis = getInputStreamSet("Basis");
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

void ReplaceByBixData(PipelineBuilder & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output);

inline void ReplaceByBixData(const std::unique_ptr<PipelineBuilder> & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output) {
    return ReplaceByBixData(*P.get(), BixData, Basis, Output);
}

inline void ReplaceByBixData(const std::unique_ptr<ProgramBuilder> & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output) {
    return ReplaceByBixData(*P.get(), BixData, Basis, Output);
}

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

void ReplaceByBixData(PipelineBuilder & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output) {
    auto insert_ccs = BixData.insertionBixNumCCs();

    StreamSet * Insertion_BixNum = P.CreateStreamSet(insert_ccs.size());
    P.CreateKernelCall<CharClassesKernel>(insert_ccs, Basis, Insertion_BixNum);
    SHOW_BIXNUM(Insertion_BixNum);

    StreamSet * SpreadMask = InsertionSpreadMask(P, Insertion_BixNum, InsertPosition::After);
    SHOW_STREAM(SpreadMask);

    StreamSet * ExpandedBasis = P.CreateStreamSet(21, 1);
    SpreadByMask(P, SpreadMask, Basis, ExpandedBasis, 0, false, kernel::StreamExpandOptimization::None, 64, GammaDistribution(5.0f, 0.1f));
    SHOW_BIXNUM(ExpandedBasis);

    std::cout << "calling replaceify" << std::endl;
    P.CreateKernelCall<Replaceify>(BixData, ExpandedBasis, Output);
    SHOW_BIXNUM(Output);
}