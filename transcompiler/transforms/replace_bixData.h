#pragma once

#include <vector>
#include <array>

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

template <std::size_t N>;
struct replace_bixData {
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

