out = "["
postfix = "]"
seperator = ","

file_contents = ""

file = 'charClasses/numericPinyinList1.txt'

# Open the file in read mode
with open(file, 'r', encoding='utf-8') as f:
    # Read the entire file content into a string
    file_contents = f.read()

data = file_contents.splitlines()
print(data)

retData = []

# first, use an algorithm to find the order in which pinyin finals must be parsed
# then, use python to expand simple letters to all accented versions
# use python to turn this into one big character class
# then, match on set 1, then set2 AND ~set1, set3 AND ~set2 AND ~set1, etc.
# note that when parsing the regex into CC, must use CASE_INSENSITIVE_MODE_FLAG

accents = {
    "a": ["ā","á","ǎ","à"],
    "o": ["ō","ó","ǒ","ò"],
    "e": ["ē","é","ě","è"],
    "i": ["ī","í","ǐ","ì"],
    "u": ["ū","ú","ŭ","ù"],
    "ü": ["ǖ","ǘ","ǚ","ǜ"]
}

for final in data:
    for key, value in accents.items():
        if key in final:
            retData.append(final)
            for val in value:
                retData.append(final.replace(key, val))
            break


with open(file, 'w', encoding='utf-8') as f:
    # Write a string to the file
    f.write("\n".join(retData))
