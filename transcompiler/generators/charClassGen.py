prefix = "["
postfix = "]"
seperator = ","

inputFiles = ['charClasses/numericPinyinList1.txt','charClasses/numericPinyinList2.txt','charClasses/numericPinyinList3.txt','charClasses/numericPinyinList4.txt']
outputs = []

for file in inputFiles:
    input = ""
    with open(file, 'r', encoding='utf-8') as f:
        input = f.read()

    data = input.splitlines()
    outputs.append(prefix + seperator.join(data) + postfix)

    with open("charClasses/numericPinyinCharClasses.txt", 'a', encoding='utf-8') as f:
        # Write a string to the file
        f.write(prefix + seperator.join(data) + postfix + "\n")
    