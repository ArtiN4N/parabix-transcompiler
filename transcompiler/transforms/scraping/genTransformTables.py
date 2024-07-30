input_path = 'transform_results.txt'

data = ""

# Open the file and read its contents
with open(input_path, 'r', encoding='utf-8') as file:
    data = file.read()

def remove_newlines_after_transform(text, prefix):
    lines = text.splitlines()  # Split the text into lines
    result_lines = []
    skip_next_newline = False

    for i, line in enumerate(lines):
        if skip_next_newline:
            # Add the current line without a newline
            result_lines.append(line)
            skip_next_newline = False
        elif i < len(lines) - 1 and lines[i + 1].startswith(prefix):
            # If the next line starts with the specified prefix, skip adding newline
            result_lines.append(line)
            skip_next_newline = True
        else:
            # Add the current line with a newline
            result_lines.append(line)

    # Join the lines with a single newline
    return '\n'.join(result_lines)

output_path = 'transform_results.txt'
with open(output_path, 'w', encoding='utf-8') as file:
    file.write(remove_newlines_after_transform(data, "Transform:"))
