prefix = "["
postfix = "]"
seperator = ","

input = "charClasses/numericPinyinCharClasses.txt"
text = ""

with open(input, 'r', encoding='utf-8') as f:
    text = f.read()

data = text.splitlines()
datas = []
for d in data:
    datas.append(d.replace('"', "").replace('[', "").replace(']', "").split(','))

specials = ["ā","á","ǎ","à",
"ō","ó","ǒ","ò",
"ē","é","ě","è",
"ī","í","ǐ","ì",
"ū","ú","ŭ","ù",
"ǖ","ǘ","ǚ","ǜ"]

finals = []

for dat in datas:
    toRemove = []
    for line in dat:
        remove = True
        for sp in specials:
            if sp in line:
                remove = False
        if remove:
            toRemove.append(line)
    dat = [line for line in dat if line not in toRemove]
    dat = '"[' + ",".join(dat) + ']"'
    finals.append(dat)


with open("charClasses/numericPinyinCharClasses.txt", 'w', encoding='utf-8') as f:

    f.write("\n".join(finals))

    