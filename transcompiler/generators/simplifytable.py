def remove_text_after_semicolon(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            cleaned_line = line.split(';')[0]  # Split the line at ';' and take the first part
            outfile.write(cleaned_line + '\n')

def remove_spaces_around_arrow(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            cleaned_line = line.replace(' →', '→').replace('→ ', '→')
            outfile.write(cleaned_line)

def remove_unescaped_single_quotes(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            cleaned_line = ''
            skip_next = False
            for i, char in enumerate(line):
                if skip_next:
                    cleaned_line += char
                    skip_next = False
                    continue
                
                if char == "'" and (i == 0 or line[i - 1] != '\\'):
                    continue
                elif char == '\\' and i + 1 < len(line) and line[i + 1] == "'":
                    skip_next = True
                
                cleaned_line += char

            outfile.write(cleaned_line)

def remove_second_last_character(s):
    if len(s) < 2:
        # If the string is too short, return it unchanged
        return s
    return s[:-2] + s[-1]

def remove_trailing_spaces(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            cleaned_line = remove_second_last_character(line)
            outfile.write(cleaned_line)

def string_to_unicode_values(input_string):
    unicode_values = [f'U+{ord(char):04X}' for char in input_string]
    return ' '.join(unicode_values)

def checkfinalcharacter(input_file):
    with open(input_file, 'r', encoding='utf-8') as infile:
        i = 0
        for line in infile:
            if i > 2:
                break
            print(string_to_unicode_values(line))
            i += 1

import re

def remove_unfollowed_backslashes(input_file, output_file):
    # Regular expression to match a backslash not followed by 'u'
    pattern = re.compile(r'\\(?!u)')
    
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            # Remove backslashes not followed by 'u'
            cleaned_line = pattern.sub('', line)
            outfile.write(cleaned_line)

def convert_to_unicode_codepoints(s):
    return 'u\\{}'.format('u\\'.join(f'{ord(char):04X}' for char in s))

def process_file(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            line = line.rstrip('\n')  # Remove trailing newline character

            if '→' in line:
                before_arrow, after_arrow = line.split('→', 1)

                # Process characters before the first "→"
                if not before_arrow.startswith('\\'):
                    before_arrow = convert_to_unicode_codepoints(before_arrow)

                # Process characters after the first "→"
                after_arrow = convert_to_unicode_codepoints(after_arrow)
                # Split the processed after_arrow into individual codepoints and format it
                after_arrow = '{' + ','.join(f'u\\{codepoint}' for codepoint in after_arrow.split('u\\')[1:]) + '}'

                # Write the result
                outfile.write(f'{before_arrow}→{after_arrow}\n')
            else:
                # If no "→" in the line, just write it as is
                outfile.write(line + '\n')

def replaceubackwithox(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as infile, open(output_file, 'w', encoding='utf-8') as outfile:
        for line in infile:
            cleaned_line = "{" + line
            cleaned_line = cleaned_line.replace("\n", "}\n")
            outfile.write(cleaned_line)



# Usage example
input_file = 'latinasciicodes.txt'
output_file = 'latinasciicodes.txt'
with open(input_file, 'r', encoding='utf-8') as file:
    content = file.read()

# Replace all occurrences of \u with 0x
modified_content = content.replace('\\u', '0x')

# Write the modified content to the output file
with open(output_file, 'w', encoding='utf-8') as file:
    file.write(modified_content)
#replaceubackwithox(output_file, input_file)

'''
with open(input_file, 'r', encoding='utf-8') as infile:

    # Split the input text into lines
    lines = infile.read()

lines = lines.strip().split('\n')

# Initialize an empty list to store the keys
keys = []

# Loop through each line and extract the keys
for line in lines:
    # Find the first '{' and '}' to extract the key
    start = line.find('{') + 1
    end = line.find(',')
    key = "{" + line[start:end].strip() + "," + line[start:end].strip() + "}"
    keys.append(key)

# Format the keys as requested
output = "{" + ",".join(keys) + "}"

with open(output_file, 'w', encoding='utf-8') as outfile:
    outfile.write(output)'''