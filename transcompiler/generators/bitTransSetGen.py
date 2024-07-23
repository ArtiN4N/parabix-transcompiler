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

# HOW TO USE:
# python bitTransSetGen.py <filename>,<filedirection> <start_cp>,<end_cp>,<cp_gap>,<cp_dir> ...
# "<>", and "..." are not to be actually typed

# file must be xlsx, located in ./maps/ directory
# file must be formatted so that the the codepoints that are being mapped from, appear alone in a column formatted in hex like so: "0x0000"
# they must have four digits following the "0x"
# the codepoints that are being mapped to, must be formatted in the same way, but come in any column after the codepoints being mapped from
# a filedirection of -1 will flip the from and to codepoints

# start_cp is the start of a range of codepoints
# end_cp is the end of said range
# cp_gap is how much must be added to a codepoint to complete the transformation
# a cp_dir value of -1 will flip the from and to codepoints
# The "..." signifies that you can input as many pairs of "<start_cp>,<end_cp>,<cp_gap>,<cp_dir>" as you like


import pandas as pd
import sys

if len(sys.argv) < 2:
    print("Usage: python script.py <filename> ...")
    sys.exit(1)

(filename, mapDir) = sys.argv[1].split(",")
mapDir = int(mapDir)

rangeMaps = []
class rangeMap:
    def __init__(self, start_cp, end_cp, gap, dir):
        self.start_cp = int(start_cp)
        self.end_cp = int(end_cp)
        self.gap = int(gap)
        self.dir = int(dir)
        self.num = self.end_cp - self.start_cp

    def getPair(self, i):
        if (i > self.num):
            return ("error", "error")
        
        cp = self.start_cp + i
        ret = (f"0x{cp:04X}", f"0x{cp+self.gap:04X}")
        if self.dir == -1:
            ret = (ret[1], ret[0])
        return ret


for i in range(len(sys.argv) - 2):
    arg = sys.argv[i + 2]
    splits = arg.split(",")
    rangeMaps.append(rangeMap(splits[0], splits[1], splits[2], splits[3]))

preamble = "const static std::unordered_map<codepoint_t, codepoint_t> explicit_cp_data = {"
postamble = "};"
final = ""

table = pd.read_excel("maps/" + filename + ".xlsx", sheet_name="Sheet1")

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
for i in range(len(rangeMaps)):
    map = rangeMaps[i]
    for j in range(map.num):
        (mapTo, mapFrom) = map.getPair(j)
        add = "{" + mapTo + ", " + mapFrom + "}, "
        final += add

for i in range(datFrom.size):
    add = "{" + ", ".join([datFrom[i], datTo[i]]) + "}"
    if (mapDir == -1):
        add = "{" + ", ".join([datTo[i], datFrom[i]]) + "}"
    final += add
    if i < datFrom.size - 1:
        final += ", "
final += postamble

print(final)