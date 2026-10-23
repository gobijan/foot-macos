#!/usr/bin/env python3
#
# Embeds a text file in a C header, as a string constant.
#
# Usage: embed-file.py <input> <output> <symbol>

import sys

def main():
    path, output, symbol = sys.argv[1:4]

    with open(path, encoding='utf-8') as f:
        text = f.read()

    with open(output, 'w', encoding='utf-8') as f:
        f.write('#pragma once\n\n')
        f.write(f'/* Generated from {path.split("/")[-1]} */\n')
        f.write('#pragma clang diagnostic push\n')
        f.write('#pragma clang diagnostic ignored "-Woverlength-strings"\n')
        f.write(f'static const char {symbol}[] =\n')
        for line in text.splitlines(keepends=True):
            escaped = (line
                       .replace('\\', '\\\\')
                       .replace('"', '\\"')
                       .replace('\n', '\\n'))
            f.write(f'    "{escaped}"\n')
        f.write('    ;\n')
        f.write('#pragma clang diagnostic pop\n')

if __name__ == '__main__':
    main()
