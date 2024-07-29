#pragma once

#include <vector>
#include <array>

#include <pablo/pablo_kernel.h>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>

#include <re/toolchain/toolchain.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>


#include <kernel/core/kernel.h>
#include <llvm/IR/Value.h>
#include <string>
#include <kernel/pipeline/driver/driver.h>
#include <kernel/pipeline/pipeline_builder.h>


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
    unicode::TranslationMap mCharMap[5];
};

template <std::size_t N>
replace_bixData::replace_bixData(std::array<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>, N> data) {
    maxAdd = 0;
    for (auto& pair : data) {
        mInsertLength.emplace(pair.first, pair.second.size());
        if (pair.second.size() > maxAdd) {
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

void ReplaceByBixData(PipelineBuilder & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output);

inline void ReplaceByBixData(const std::unique_ptr<PipelineBuilder> & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output) {
    return ReplaceByBixData(*P.get(), BixData, Basis, Output);
}

inline void ReplaceByBixData(const std::unique_ptr<ProgramBuilder> & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output) {
    return ReplaceByBixData(*P.get(), BixData, Basis, Output);
}