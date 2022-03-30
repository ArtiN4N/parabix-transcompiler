#ifndef RE_ANALYSIS_H
#define RE_ANALYSIS_H

#include <utility>
namespace re { class RE; class Name; class CC; class Capture; class Reference;}
namespace cc { class Alphabet;}

namespace re {

// Does the RE match the empty string, considering that ^ and $ each
// do match an empty string.
bool matchesEmptyString(const RE * re);

//  Determine the set of all codepoints cp such that the given RE
//  matches a string consisting of a single Unicode character whose
//  codepoint value is cp.
const CC * matchableCodepoints(const RE * re);

bool isUnicodeUnitLength(const RE * re);

std::pair<int, int> getLengthRange(const RE * re, const cc::Alphabet * indexingAlphabet);

bool isFixedLength(const RE * re);

int minMatchLength(const RE * re);

/* Validate that the given RE can be compiled in UTF-8 mode
   without variable advances. */
bool validateFixedUTF8(const RE * r);

bool isTypeForLocal(const RE * re);
    
bool hasAssertion(const RE * re);
    
bool byteTestsWithinLimit(RE * re, unsigned limit);
    
bool hasTriCCwithinLimit(RE * r, unsigned byteCClimit, RE * & prefixRE, RE * & suffixRE);

bool hasStartAnchor(const RE * r);

bool hasEndAnchor(const RE * r);
    
bool DefiniteLengthBackReferencesOnly(const RE * re);
    
std::pair<RE *, unsigned> makeWordOnlySubExpression(RE * r, bool UnicodeIndexing);

}

#endif // RE_ANALYSIS_H
