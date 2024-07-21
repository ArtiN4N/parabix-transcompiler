# STEPS:
# create unordered map:

# const static std::unordered_map<codepoint_t, codepoint_t> explicit_cp_data = { {0x0041, 0x0061}, ..., {.., ..}};


# OPTION 1: create codepoint property object
#static CodePointPropertyObject property_object(slc,
#                                                    std::move(null_codepoint_set),
#                                                    std::move(reflexive_set),
#                                                    std::move(explicit_cp_data));


# OPTION 2: convert to translationmap
# unicode::TranslationMap mExplicitCodepointMap(explicit_cp_data);

# then, compute:
# std::vector<UCD::UnicodeSet> bit_xform_sets = unicode::ComputeBitTranslationSets(mExplicitCodepointMap);

import pandas as pd
import math

preamble = "const static std::unordered_map<codepoint_t, codepoint_t> explicit_cp_data = {"
postamble = "};"
final = ""

table = pd.read_excel("maps/mapping.xlsx", sheet_name="Sheet1")

fromColInd = 0
toColInd = 0

while True:
    dat = table.iloc[:, fromColInd][0]

    if str(dat).startswith("0x"):
        break
    
    fromColInd += 1

toColInd = fromColInd + 1
while True:
    dat = table.iloc[:, toColInd][0]

    if str(dat).startswith("0x"):
        break
    
    toColInd += 1

datFrom = table.iloc[:, fromColInd]
datTo = table.iloc[:, toColInd]

final += preamble
for i in range(datFrom.size):
    add = "{" + ", ".join([datFrom[i], datTo[i]]) + "}"
    final += add
    if i < datFrom.size - 1:
        final += ","
final += postamble

print(final)