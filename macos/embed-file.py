#!/usr/bin/env python3
#
# Embeds a text file in a C header, as a string constant.
#
# Usage: embed-file.py <input> <output> <symbol> [--macos-defaults]
#
# With --macos-defaults, foot.ini's documented defaults are adjusted to
# those of the macOS port (foot.ini itself is left as upstream has it).

import re
import sys

MACOS_DEFAULTS = [
    ('# login-shell=no\n', '# login-shell=yes\n'),
    ('# pad=0x0 center-when-maximized-and-fullscreen\n', '# pad=8x8 center-when-maximized-and-fullscreen\n'),
    ('# font=monospace:size=8\n', '# font=monospace:size=13\n'),
    ("# shell=$SHELL (if set, otherwise user's default shell from /etc/passwd)\n",
     "# shell=$SHELL (if set, otherwise user's default shell from /etc/passwd)\n"
     "# macos-option-as-alt=none  (none, left, right or both)\n"
     "# macos-font-smoothing=yes  (thickens glyphs, like macOS' own text rendering)\n"
     "# macos-font-smoothing-strength=1.0  (0.0 - 1.0)\n"
     "# macos-follow-appearance=yes  (switches between [colors-dark] and [colors-light] with the system's appearance)\n"
     "# macos-confirm-close=yes  (asks before closing a window, or quitting, with something other than the shell running)\n"
     "# macos-quake-hotkey=Mod1+grave  (the quake terminal's hotkey: Option+`, the key below Esc; none: no hotkey)\n"
     "# macos-quake-position=top  (top, bottom, left, right or center)\n"
     "# macos-quake-size=50  (height, or WIDTHxHEIGHT, in percent of the screen; without a width: twice as wide as high)\n"
     "# macos-quake-alpha=<the color theme's alpha>\n"
     "# macos-quake-dim=0.6  (how much the rest of the screen is darkened, while it has the keyboard focus; 0.0 - 1.0)\n"
     "# macos-quake-auto-hide=yes  (hides it when it loses the keyboard focus)\n"
     "# macos-quake-animation-duration=0.3,0.2  (seconds, showing and hiding; one value: both; 0: no animation)\n"
     "# macos-quake-area=screen  (or X,Y,WIDTH,HEIGHT, in points: where it's placed instead of the screen)\n"),
    ('# initial-color-theme=dark\n',
     '# include=/Applications/foot.app/Contents/Resources/themes/<theme>  (e.g. catppuccin-mocha)\n'
     '# initial-color-theme=dark\n'),
    ('# launch=xdg-open ${url}\n', '# launch=open ${url}\n'),
]

# The default colors, Grok Night and Grok Day (macos/default-colors.c)
GROK_NIGHT = [
    ('background', '0a0a0a'), ('foreground', 'e1e1e1'), ('cursor', '0a0a0a bb9af7'),
    ('regular0', '111111'), ('regular1', 'f7768e'), ('regular2', '9ece6a'), ('regular3', 'e0af68'),
    ('regular4', '7aa2f7'), ('regular5', 'bb9af7'), ('regular6', '1abc9c'), ('regular7', 'c8c8c8'),
    ('bright0', '6c6c6c'), ('bright1', 'f7768e'), ('bright2', '8bc34a'), ('bright3', 'ff9e64'),
    ('bright4', '7aa2f7'), ('bright5', '9d7cd8'), ('bright6', '3a95ab'), ('bright7', 'e1e1e1'),
    ('selection-foreground', 'e1e1e1'), ('selection-background', '323237'),
]
GROK_DAY = [
    ('background', 'f5f5f5'), ('foreground', '262626'), ('cursor', 'f5f5f5 7d4bc6'),
    ('regular0', '262626'), ('regular1', 'cd3048'), ('regular2', '378e23'), ('regular3', 'a27612'),
    ('regular4', '2f64d2'), ('regular5', '7d4bc6'), ('regular6', '0a8e70'), ('regular7', '444444'),
    ('bright0', '767676'), ('bright1', 'cd3048'), ('bright2', '4caf50'), ('bright3', 'c3691e'),
    ('bright4', '2f64d2'), ('bright5', '6c3eb2'), ('bright6', '0f87a2'), ('bright7', '262626'),
    ('selection-foreground', '262626'), ('selection-background', 'd4d4d8'),
]

def default_colors(path, text):
    start = text.index('[colors-dark]\n')
    end = text.index('[colors-light]\n')
    dark = text[start:end]
    for key, value in GROK_NIGHT:
        # The value, e.g. 'f62b5a' or '<inverse foreground/background>'
        dark, n = re.subn(rf'^# {key}=(<[^>]*>|\S+)', f'# {key}={value}', dark,
                          count=1, flags=re.MULTILINE)
        if n != 1:
            sys.exit(f'{path}: expected default not found: {key}')

    old = '# Same builtin defaults as [color], except for:\n# dim-blend-towards=white\n'
    if text.find(old, end) < 0:
        sys.exit(f'{path}: expected default not found: {old.strip()}')
    light = ('# Same builtin defaults as [colors-dark], except for:\n'
             '# dim-blend-towards=white\n'
             + ''.join(f'# {key}={value}\n' for key, value in GROK_DAY))

    return text[:start] + dark + text[end:].replace(old, light, 1)

def main():
    path, output, symbol = sys.argv[1:4]

    with open(path, encoding='utf-8') as f:
        text = f.read()

    if '--macos-defaults' in sys.argv[4:]:
        for old, new in MACOS_DEFAULTS:
            if old not in text:
                sys.exit(f'{path}: expected default not found: {old.strip()}')
            text = text.replace(old, new, 1)

        text = default_colors(path, text)

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
