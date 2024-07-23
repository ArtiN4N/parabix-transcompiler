'''prefix = "["
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
    dat = '"' + "","".join(dat) + '"'
    finals.append(dat)


with open("charClasses/numericPinyinCharClasses.txt", 'w', encoding='utf-8') as f:

    f.write("\n".join(finals))'''


t1 = '"uāi","uái","uǎi","uài","uāng","uáng","uǎng","uàng","iāo","iáo","iǎo","iào","iāng","iáng","iǎng","iàng","ōu","óu","ǒu","òu","uō","uó","uǒ","uò","iōng","ióng","iǒng","iòng","ēi","éi","ěi","èi","ēng","éng","ěng","èng","ēr","ér","ěr","èr","iē","ié","iě","iè","uē","ué","uě","uè","uī","uí","uǐ","uì","īng","íng","ǐng","ìng","ūn","ún","ŭn","ùn","īu","íu","ǐu","ìu","ǖn","ǘn","ǚn","ǜn"'
t2 = '"āi","ái","ǎi","ài","āo","áo","ǎo","ào","āng","áng","ǎng","àng","uān","uán","uǎn","uàn","iān","ián","iǎn","iàn","ōng","óng","ǒng","òng","ēn","én","ěn","èn","īn","ín","ǐn","ìn"'
t3 = '"ān","án","ǎn","àn","uā","uá","uǎ","uà","iā","iá","iǎ","ià","ō","ó","ǒ","ò","ē","é","ě","è"'
t4 = '"ā","á","ǎ","à","ī","í","ǐ","ì","ū","ú","ŭ","ù","ǖ","ǘ","ǚ","ǜ"'

print("[" + "".join(list(set(t1+t2+t3+t4))) + "]")