def codepoint_to_hex_string(codepoint):
    return f"0x{codepoint:04X}"

def codepoint_to_utf16_string(codepoint):
    if codepoint < 0x10000:
        return f"\\u{codepoint:04X}"
    else:
        # Calculate surrogate pair
        codepoint -= 0x10000
        high_surrogate = 0xD800 + (codepoint >> 10)
        low_surrogate = 0xDC00 + (codepoint & 0x3FF)
        return f"\\u{high_surrogate:04X}\\u{low_surrogate:04X}"

def char_to_hex_string(character):
    codepoint = ord(character)
    return f"0x{codepoint:04X}"


outputS = """#pragma once

#include <vector>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
const std::vector<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>> asciiCodeData = {"""
outputE = "};"
outputM = []
name = "hexData"
#0x110000
for i in range(0xFFFF):
    mTextS = "{"
    mTextE = "}"
    mTextInp = codepoint_to_hex_string(i)
    mTextOut = []
    for char in codepoint_to_utf16_string(i):
        mTextOut.append(char_to_hex_string(char))

    

    outputM.append(mTextS + mTextInp + "," + "{" + ','.join(mTextOut) + "}" + mTextE)

outputFile = name + ".h"
with open(outputFile, 'w', encoding='utf-8') as outfile:
    outfile.write(outputS + ','.join(outputM) + outputE)