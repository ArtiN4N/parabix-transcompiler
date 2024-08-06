import re

wave = 1
startText = "const static std::unordered_map<codepoint_t, codepoint_t> lasciiwave" + str(wave) + " = {"
content = ""
endText = "};"

inputFile = "fulllasciichart.txt"
outputFile = "wave" + str(wave) + "lascii.txt"

with open(inputFile, 'r', encoding='utf-8') as infile, open(outputFile, 'w', encoding='utf-8') as outfile:
    for line in infile:
        matches = re.search(r'\{([0-9A-Fx,]+)\}', line)

        nested_items = []
        if matches:
            # Get the matched string and split it by comma
            nested_items = matches.group(1).split(',')
            # Convert the list of strings to a list of integers
            nested_items = [item.strip() for item in nested_items]

        lineWaves = len(nested_items)

        first_characters = []

        comma_position = line.find(',')
        if comma_position != -1:
            first_character = line[1:comma_position]  # Exclude the '{' at the start
            first_characters.append(first_character.strip())

        print(first_characters)
        print(nested_items)
        if (lineWaves - wave >= 0):
            curWaveItem = nested_items[-1 * wave]
            content = content + "{" + first_characters[0] + "," + curWaveItem + "},"


    outfile.write(startText + content + endText)