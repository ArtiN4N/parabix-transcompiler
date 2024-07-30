import unicodedata

def get_script_characters(script):
    latin_characters = []

    script = "CANADIAN"

    # Iterate through Unicode code points
    for codepoint in range(0x10000):
        char = chr(codepoint)
        try:
            # Check if the character is in the Latin script
            if script in unicodedata.name(char):
                
                latin_characters.append(char)
        except ValueError:
            # Some code points do not have a character, skip them
            continue

    return ''.join(latin_characters)

def split_string(s, chunk_size):
    # Split the string into chunks of size chunk_size
    return [s[i:i + chunk_size] for i in range(0, len(s), chunk_size)]


def extract_non_latin_word(line):
    parts = [part.strip().replace(";", "").replace(":: ", "") for part in line.split('-') if part.strip()]
    if len(parts) < 2:
        raise ValueError("Input format is incorrect. Expected at least two parts.")
    return parts[0]

def perform_transformation(transform):
    script = extract_non_latin_word(transform).upper()
    
    input_text = get_script_characters(script)

    with open("../tests/scripts/" + script.replace("/", "_") + "alphabet.txt", "w", encoding='utf-8') as f:
        f.write("\n".join(input_text))

transforms = []
inputTransformFile = "transforms.txt"

with open(inputTransformFile, 'r', encoding='utf-8') as transformfile:
    transforms = transformfile.read().splitlines()

skipping = True
for transform in transforms:
    print(transform)
perform_transformation(":: CanadianAboriginal-Latin;")