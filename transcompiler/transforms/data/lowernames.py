import os

# Specify the directory containing the files
directory = "./"

# Iterate over all files in the directory
for filename in os.listdir(directory):
    # Create the full file path
    old_file = os.path.join(directory, filename)
    
    # Skip directories
    if os.path.isdir(old_file):
        continue
    
    print("hi")
    # Generate the new file name with lowercase letters
    new_filename = filename.lower()
    name, ext = os.path.splitext(new_filename)
    
    new_filename = name[:-1] + ext
    new_file = os.path.join(directory, new_filename)
    
    # Create the full new file path
    #new_file = os.path.join(directory, new_filename)
    
    # Rename the file
    os.rename(old_file, new_file)