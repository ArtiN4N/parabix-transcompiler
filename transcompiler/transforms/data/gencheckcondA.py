import os

# Specify the directory containing the files
directory = './'

def process_string(s):
    # Check if 'latin' is in the string
    if 'latin' in s:
        # If 'latin' is at the start of the string
        if s.startswith('latin'):
            return s[:5] + '-' + s[5:]
        else:
            # Find the position of 'latin'
            index = s.find('latin')
            return s[:index] + '-' + s[index:]
    return s

final = ""
# Iterate over all files in the directory
for filename in os.listdir(directory):
    # Create the full file path
    old_file = os.path.join(directory, filename)
    
    # Skip directories
    if os.path.isdir(old_file):
        continue
    
    # Generate the new file name with lowercase letters
    new_filename = filename.lower()
    strs = process_string(new_filename)
    final += "check == \"" + strs + "\" || "

print(final)