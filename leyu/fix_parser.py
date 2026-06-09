import re

def fix_file(filename):
    with open(filename, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Remove literal \n at start of lines, \nvoid, \nbool, and at EOF
    content = re.sub(r'(?m)^\\n', '', content)
    content = content.replace('\\nvoid ', 'void ')
    content = content.replace('\\nbool ', 'bool ')
    content = re.sub(r'\\n$', '', content)
    
    with open(filename, 'w', encoding='utf-8') as f:
        f.write(content)

print("Fixing literal \\n in files...")
fix_file('d:/leyu/main/mqtt_msg_builder.c')
fix_file('d:/leyu/main/mqtt_msg_parser.c')
print("Done.")
