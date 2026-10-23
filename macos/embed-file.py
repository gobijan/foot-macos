#!/usr/bin/env python3
#
# Embeds a text file in a C header, as a string constant.
#
# Usage: embed-file.py <input> <output> <symbol> [--macos-defaults]
#
# With --macos-defaults, foot.ini's documented defaults are adjusted to
# those of the macOS port (foot.ini itself is left as upstream has it).

import sys

MACOS_DEFAULTS = [
    ('# login-shell=no\n', '# login-shell=yes\n'),
    ('# font=monospace:size=8\n', '# font=monospace:size=13\n'),
    ("# shell=$SHELL (if set, otherwise user's default shell from /etc/passwd)\n",
     "# shell=$SHELL (if set, otherwise user's default shell from /etc/passwd)\n"
     "# macos-option-as-alt=none  (none, left, right or both)\n"),
    ('# launch=xdg-open ${url}\n', '# launch=open ${url}\n'),
]

def main():
    path, output, symbol = sys.argv[1:4]

    with open(path, encoding='utf-8') as f:
        text = f.read()

    if '--macos-defaults' in sys.argv[4:]:
        for old, new in MACOS_DEFAULTS:
            if old not in text:
                sys.exit(f'{path}: expected default not found: {old.strip()}')
            text = text.replace(old, new, 1)

        # The notification command
        text = '\n'.join(
            '# command=osascript -e "on run argv" -e "display notification '
            '(item 2 of argv) with title (item 1 of argv)" -e "end run" '
            '${title} ${body}'
            if line.startswith('# command=notify-send') else line
            for line in text.split('\n'))

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
