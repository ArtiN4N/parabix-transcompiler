def sort_lines_by_length(filename):
    # Read all lines from the file
    with open(filename, 'r') as file:
        lines = file.readlines()
    
    # Sort lines by their length
    sorted_lines = sorted(lines, key=len)
    
    # Write the sorted lines back to the file
    with open(filename, 'w') as file:
        file.writelines(sorted_lines)

if __name__ == "__main__":
    filename = 'file_list.txt'  # Replace with the path to your file
    sort_lines_by_length(filename)
    print(f"Lines in {filename} have been sorted by length.")