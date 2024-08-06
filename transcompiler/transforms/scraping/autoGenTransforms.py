from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
import time
import unicodedata

def get_latin_characters():
    latin_characters = []

    # Iterate through Unicode code points
    for codepoint in range(0x10000):
        char = chr(codepoint)
        try:
            # Check if the character is in the Latin script
            if 'LATIN' in unicodedata.name(char):
                latin_characters.append(char)
        except ValueError:
            # Some code points do not have a character, skip them
            continue

    return ''.join(latin_characters)

latin_characters = get_latin_characters()
#print(latin_characters)

input_lowertext = "\n".join("abcdefghijklmnopqrstuvwxyz")
input_uppertext = "\n".join("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
input_text = latin_characters#input_lowertext + "\n" + input_uppertext

driver = webdriver.Chrome()
url = "https://util.unicode.org/UnicodeJsps/transform.jsp"

def split_string(s, chunk_size):
    # Split the string into chunks of size chunk_size
    return [s[i:i + chunk_size] for i in range(0, len(s), chunk_size)]


#print(len(input_text))
results = []
def perform_transformation(transform):
    chunks = split_string(input_text, 460)  # Adjust chunk_size as needed
    res = []
    k = 0
    for chunk in chunks:
        chunk = "\n".join(chunk)
        driver.get(url)

        transform_input = driver.find_element(By.NAME, "a")
        sample_text_input = driver.find_element(By.NAME, "b")

        transform_input.clear()
        sample_text_input.clear()

        transform_input.send_keys(transform)
        sample_text_input.send_keys(chunk)

        # Click the "Show Transform" button
        show_transform_button = driver.find_element(By.XPATH, "//input[@value='Show Transform']")
        show_transform_button.click()

        # Wait for the result to be displayed
        time.sleep(3)  # Adjust the sleep time if needed

        # Retrieve the transformed text
        form_element = driver.find_element(By.NAME, "myform")
        form_text = form_element.text
        result_index = form_text.find("Result") + len("Result\n")
        res.append(form_text[result_index:].strip())

    combine = "\n".join(res)
    #combine = res[0] + res[1]
    #print(combine[l-1])
    #print(combine[l])
    #print(combine[l+1])
    #print(combine[l+2])
    results.append({
        "transform": transform,
        "input_text": input_text,
        "result_text": combine
    })

# Dictionary to store results


transforms = []
inputTransformFile = "transforms.txt"

with open(inputTransformFile, 'r', encoding='utf-8') as transformfile:
    transforms = transformfile.read().splitlines()

# Perform transformations for each transform in the list
#i = 0
skipping = True
for transform in transforms:
    #transformed_text = 
    if transform != ":: Latin-Ethiopic/ALALOC;" and skipping:
        continue
    skipping = False
    perform_transformation(transform)
    #results[transform] = transformed_text
    #print(transformed_text)
    #if (i > 0):
     #   break
    #i += 1
#perform_transformation(transforms[3])

# Close the web driver
driver.quit()

def char_to_utf16_hex(char):
    if len(char) != 1:
        raise ValueError("Input must be a single character.")
    
    utf16_bytes = char.encode('utf-16-be')
    
    # Convert each byte to its hexadecimal representation and join them
    utf16_hex = ''.join(f"{byte:02x}" for byte in utf16_bytes)
    
    # Prefix with 0x
    utf16_hex = '0x' + utf16_hex
    
    return utf16_hex

def simplify_transform(text):
    # Remove leading ':: ' and trailing ';'
    simplified_text = text.strip(':: ;')
    
    # Remove hyphens
    simplified_text = simplified_text.replace('-', '')
    
    # Append 'Data' to the end
    simplified_text += 'Data'
    
    return simplified_text

for result in results:
    outputStr = """#pragma once

#include <vector>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>


"""
    outputStr += "constexpr size_t outer" + simplify_transform(result['transform']) + "Size = " + str(len(result['input_text'])) + ";\n\nconst std::array<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>, outer" + simplify_transform(result['transform']) + "Size> "
    outputStr += simplify_transform(result['transform'])
    outputStr += " = {"
    outputStr += "{\n"
    
    i = 0
    #print(result['result_text'])
    #print(result['input_text'])
    for inpLine in result['input_text']:
        
        outLine = result['result_text'].splitlines()[i]

        outputStr += "{"
        #if len(inpLine) != 1:
            #print(inpLine)
        outputStr += char_to_utf16_hex(inpLine)
        outputStr += ", {"
        j = 0
        for char in outLine:
            outputStr += char_to_utf16_hex(char)
            if (j != len(outLine) - 1):
                outputStr += ","
            j += 1
        outputStr += "}"
        outputStr += "}"
        if (i != len(result['input_text']) - 1):
            outputStr += ","
        outputStr += "\n"

        i += 1
    outputStr += "}"
    outputStr += "};\n"

    with open("../data/" + simplify_transform(result['transform']).replace("/", "") + ".h", "w", encoding='utf-8') as f:
        f.write(outputStr)
            

# Save the results to a file


print("All transformations are done and results are saved in transform_results.txt")