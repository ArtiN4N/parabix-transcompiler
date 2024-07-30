from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
import time

input_lowertext = "\n".join("abcdefghijklmnopqrstuvwxyz")
input_uppertext = "\n".join("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
input_text = input_lowertext + "\n" + input_uppertext

driver = webdriver.Chrome()
url = "https://util.unicode.org/UnicodeJsps/transform.jsp"

results = []
def perform_transformation(transform):
    driver.get(url)

    transform_input = driver.find_element(By.NAME, "a")
    sample_text_input = driver.find_element(By.NAME, "b")

    transform_input.clear()
    sample_text_input.clear()

    transform_input.send_keys(transform)
    sample_text_input.send_keys(input_text)

    # Click the "Show Transform" button
    show_transform_button = driver.find_element(By.XPATH, "//input[@value='Show Transform']")
    show_transform_button.click()

    # Wait for the result to be displayed
    time.sleep(3)  # Adjust the sleep time if needed

    # Retrieve the transformed text
    form_element = driver.find_element(By.NAME, "myform")
    form_text = form_element.text
    result_index = form_text.find("Result") + len("Result\n")
    result_text = form_text[result_index:].strip()

    results.append({
        "transform": transform,
        "input_text": input_text,
        "result_text": result_text
    })

# Dictionary to store results


transforms = []
inputTransformFile = "transforms.txt"

with open(inputTransformFile, 'r', encoding='utf-8') as transformfile:
    transforms = transformfile.read().splitlines()

# Perform transformations for each transform in the list
#i = 0
#for transform in transforms:
    #transformed_text = 
    #perform_transformation(transform)
    #results[transform] = transformed_text
    #print(transformed_text)
    #if (i > 0):
     #   break
    #i += 1
perform_transformation(transforms[3])

# Close the web driver
driver.quit()

def char_to_utf16_hex(char):
    if len(char) != 1:
        raise ValueError("Input must be a single character.")
    
    # Encode the character in UTF-16 and remove the BOM (Byte Order Mark)
    utf16_bytes = char.encode('utf-16')[2:]
    
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
    outputStr += "constexpr size_t outer" + simplify_transform(result['transform']) + "Size = " + str(len(result['input_text'].splitlines())) + ";\n\nconst std::array<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>, outer" + simplify_transform(result['transform']) + "Size> "
    outputStr += simplify_transform(result['transform'])
    outputStr += " = {"
    outputStr += "{\n"
    
    i = 0
    for inpLine in result['input_text'].splitlines():
        
        outLine = result['result_text'].splitlines()[i]

        outputStr += "{"
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

    with open("../data/" + simplify_transform(result['transform']) + "Data.h", "w", encoding='utf-8') as f:
        f.write(outputStr)
            

# Save the results to a file


print("All transformations are done and results are saved in transform_results.txt")