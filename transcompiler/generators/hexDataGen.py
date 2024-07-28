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



outputE = "};"
outputM = []
name = "hexData"
#0x110000

ranges = """U+0000 - U+007F
U+0080 - U+00FF	
U+0100 - U+017F	
U+0180 - U+024F	
U+0250 - U+02AF	
U+02B0 - U+02FF	
U+0300 - U+036F	
U+0370 - U+03FF	
U+0400 - U+04FF
U+0500 - U+052F	
U+0530 - U+058F
U+0590 - U+05FF
U+0600 - U+06FF
U+0700 - U+074F
U+0750 - U+077F	
U+0780 - U+07BF
U+07C0 - U+07FF
U+0800 - U+083F
U+0840 - U+085F
U+0860 - U+086F	
U+08A0 - U+08FF	
U+0900 - U+097F
U+0980 - U+09FF
U+0A00 - U+0A7F
U+0A80 - U+0AFF
U+0B00 - U+0B7F
U+0B80 - U+0BFF
U+0C00 - U+0C7F
U+0C80 - U+0CFF
U+0D00 - U+0D7F
U+0D80 - U+0DFF
U+0E00 - U+0E7F
U+0E80 - U+0EFF
U+0F00 - U+0FFF
U+1000 - U+109F
U+10A0 - U+10FF
U+1100 - U+11FF
U+1200 - U+137F
U+1380 - U+139F	
U+13A0 - U+13FF
U+1400 - U+167F	
U+1680 - U+169F
U+16A0 - U+16FF
U+1700 - U+171F
U+1720 - U+173F
U+1740 - U+175F
U+1760 - U+177F
U+1780 - U+17FF
U+1800 - U+18AF
U+18B0 - U+18FF	
U+1900 - U+194F
U+1950 - U+197F
U+1980 - U+19DF
U+19E0 - U+19FF	
U+1A00 - U+1A1F
U+1A20 - U+1AAF
U+1AB0 - U+1AFF	
U+1B00 - U+1B7F
U+1B80 - U+1BBF
U+1BC0 - U+1BFF
U+1C00 - U+1C4F
U+1C50 - U+1C7F
U+1C80 - U+1C8F	
U+1C90 - U+1CBF	
U+1CC0 - U+1CCF	
U+1CD0 - U+1CFF	
U+1D00 - U+1D7F	
U+1D80 - U+1DBF	
U+1DC0 - U+1DFF	
U+1E00 - U+1EFF	
U+1F00 - U+1FFF	
U+2000 - U+206F	
U+2070 - U+209F	
U+20A0 - U+20CF	
U+20D0 - U+20FF	
U+2100 - U+214F	
U+2150 - U+218F	
U+2190 - U+21FF
U+2200 - U+22FF	
U+2300 - U+23FF	
U+2400 - U+243F	
U+2440 - U+245F	
U+2460 - U+24FF	
U+2500 - U+257F
U+2580 - U+259F	
U+25A0 - U+25FF	
U+2600 - U+26FF	
U+2700 - U+27BF
U+27C0 - U+27EF	
U+27F0 - U+27FF	
U+2800 - U+28FF	
U+2900 - U+297F	
U+2980 - U+29FF	
U+2A00 - U+2AFF	
U+2B00 - U+2BFF	
U+2C00 - U+2C5F
U+2C60 - U+2C7F	
U+2C80 - U+2CFF
U+2D00 - U+2D2F	
U+2D30 - U+2D7F
U+2D80 - U+2DDF	
U+2DE0 - U+2DFF	
U+2E00 - U+2E7F	
U+2E80 - U+2EFF	
U+2F00 - U+2FDF	
U+2FF0 - U+2FFF	
U+3000 - U+303F	
U+3040 - U+309F
U+30A0 - U+30FF
U+3100 - U+312F
U+3130 - U+318F	
U+3190 - U+319F
U+31A0 - U+31BF	
U+31C0 - U+31EF
U+31F0 - U+31FF	
U+3200 - U+32FF	
U+3300 - U+33FF	
U+3400 - U+34FF	
U+4DC0 - U+4DFF	
U+4E00 - U+4EFF	
U+A000 - U+A0FF	
U+A490 - U+A4CF
U+A4D0 - U+A4FF
U+A500 - U+A63F
U+A640 - U+A69F	
U+A6A0 - U+A6FF
U+A700 - U+A71F	
U+A720 - U+A7FF	
U+A800 - U+A82F	
U+A830 - U+A83F	
U+A840 - U+A87F
U+A880 - U+A8DF
U+A8E0 - U+A8FF	
U+A900 - U+A92F
U+A930 - U+A95F
U+A960 - U+A97F	
U+A980 - U+A9DF
U+A9E0 - U+A9FF	
U+AA00 - U+AA5F
U+AA60 - U+AA7F	
U+AA80 - U+AADF
U+AAE0 - U+AAFF	
U+AB00 - U+AB2F	
U+AB30 - U+AB6F	
U+AB70 - U+ABBF	
U+ABC0 - U+ABFF	
U+AC00 - U+ACFF	
U+D7B0 - U+D7FF	
U+D800 - U+DB7F	
U+DB80 - U+DBFF	
U+DC00 - U+DFFF	
U+E000 - U+F8FF	
U+F900 - U+FAFF	
U+FB00 - U+FB4F	
U+FB50 - U+FDFF	
U+FE00 - U+FE0F	
U+FE10 - U+FE1F	
U+FE20 - U+FE2F	
U+FE30 - U+FE4F	
U+FE50 - U+FE6F	
U+FE70 - U+FEFF	
U+FF00 - U+FFEF	
U+FFF0 - U+FFFF"""

lines = ranges.splitlines()

# Initialize an empty list to hold all integers
all_integers = []

# Loop through each line
for line in lines:
    # Extract the start and end points of the range
    start_str, end_str = line.split(' - ')
    # Convert the hex strings to integers
    start = int(start_str[2:], 16)  # Remove 'U+' and convert to int
    end = int(end_str[2:], 16)      # Remove 'U+' and convert to int
    max = start + 0xFF
    if end > max:
        end = max
    # Append all integers in the range to the list
    all_integers.extend(range(start, end + 1))

toUseInts = list(set(all_integers))


outputS = """#pragma once

#include <vector>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
constexpr size_t outerSize = """ + str(len(toUseInts)) + """;
constexpr size_t innerSize = 6;

const std::array<std::pair<UCD::codepoint_t, std::array<UCD::codepoint_t>, innerSize>, outerSize> asciiCodeData ="""

print(len(toUseInts))

for i in toUseInts:
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