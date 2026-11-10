#!/usr/bin/env python3
"""Generates the website's assets (macos/site/); they're committed.

    macos/site/make-assets.py            # themes.js, favicon.svg, the PNGs, splits.webp
    macos/site/make-assets.py --lockup   # prints the logo's SVG, for index.html

themes.js has the colors of foot's themes (themes/, macos/themes/), for
the page's theme menu. The logo is the application icon as macos/banner.sh
draws it (its pixels are read from there), and "foot" in the same pixels.
The PNGs need rsvg-convert, splits.webp needs cwebp (brew install librsvg
webp).
"""

import json
import re
import subprocess
import sys
from pathlib import Path

SITE = Path(__file__).resolve().parent
ROOT = SITE.parent.parent

ICON_TOP, ICON_BOTTOM, ICON_FOOT = "#3a3a3a", "#1e1e1e", "#fbf1c7"

# "foot", 9 pixels high, strokes of 2
LETTERS = {
    "f": [
        "..####",
        ".#####",
        ".##...",
        "######",
        "######",
        ".##...",
        ".##...",
        ".##...",
        ".##...",
    ],
    "o": [
        ".......",
        ".......",
        ".......",
        ".#####.",
        "#######",
        "##...##",
        "##...##",
        "#######",
        ".#####.",
    ],
    "t": [
        ".##...",
        ".##...",
        ".##...",
        "######",
        "######",
        ".##...",
        ".##...",
        ".#####",
        "..####",
    ],
}


def icon_map():
    """The icon's pixels, from macos/banner.sh: '#' background, 'w' the foot."""
    lines = (ROOT / "macos/banner.sh").read_text().splitlines()
    return lines[lines.index("MAP") + 1 : lines.index("TEXT")]


def wordmark():
    rows = [""] * 9
    for i, letter in enumerate("foot"):
        for y, line in enumerate(LETTERS[letter]):
            rows[y] += ("." if i else "") + line
    return rows


def path(rows, char, dx=0, dy=0):
    """An SVG path of the pixels that are char: one rectangle per run."""
    d = []
    for y, row in enumerate(rows):
        for run in re.finditer(re.escape(char) + "+", row):
            width = run.end() - run.start()
            d.append(f"M{run.start() + dx} {y + dy}h{width}v1h-{width}z")
    return "".join(d)


def icon_svg(rows, dx=0, dy=0, gradient="g"):
    # Background and foot overlap by design: no seams between them
    background = [row.replace("w", "#") for row in rows]
    return (
        f'<path fill="url(#{gradient})" d="{path(background, "#", dx, dy)}"/>'
        f'<path fill="{ICON_FOOT}" d="{path(rows, "w", dx, dy)}"/>'
    )


def gradient(name, y1, y2):
    return (
        f'<linearGradient id="{name}" gradientUnits="userSpaceOnUse" x1="0" y1="{y1}" x2="0" y2="{y2}">'
        f'<stop offset="0" stop-color="{ICON_TOP}"/><stop offset="1" stop-color="{ICON_BOTTOM}"/>'
        "</linearGradient>"
    )


def lockup(palette):
    """The icon, "foot" beside it, and the palette below that: regular
    colors above, bright colors below (as macos/banner.sh). palette(i)
    is the fill of color i (0-15)."""
    rows = icon_map()
    left = len(rows[0]) + 3
    parts = [f"<defs>{gradient('g', 0, len(rows))}</defs>", icon_svg(rows)]
    parts.append(f'<path class="word" d="{path(wordmark(), "#", left, 3)}"/>')
    for i in range(16):
        x, y = left + (i % 8) * 3, 15 + (i // 8) * 2
        parts.append(f'<path {palette(i)} d="M{x} {y}h3v2h-3z"/>')
    width = left + len(wordmark()[0])
    return width, len(rows), "".join(parts)


# -- Themes ---------------------------------------------------------------

KEYS = (
    ["background", "foreground"]
    + [f"regular{i}" for i in range(8)]
    + [f"bright{i}" for i in range(8)]
    + ["selection-background", "selection-foreground", "cursor"]
)


def theme_colors(section):
    """A section's colors as one string of hex colors, in KEYS' order."""
    colors = dict(section)
    if "background" not in colors or "foreground" not in colors:
        return None
    for i in range(8):
        if f"regular{i}" not in colors:
            return None
        colors.setdefault(f"bright{i}", colors[f"regular{i}"])
    # foot's defaults: the selection is inverted, the cursor the foreground
    colors.setdefault("selection-background", colors["foreground"])
    colors.setdefault("selection-foreground", colors["background"])
    # cursor=<text color> <cursor color>
    colors["cursor"] = colors.get("cursor", colors["foreground"]).split()[-1]
    values = [colors[key].lower() for key in KEYS]
    if not all(re.fullmatch("[0-9a-f]{6}", value) for value in values):
        return None
    return "".join(values)


def themes():
    result = {}
    for file in sorted([*(ROOT / "themes").iterdir(), *(ROOT / "macos/themes").iterdir()], key=lambda f: f.name):
        sections, current = {}, None
        for line in file.read_text().splitlines():
            line = line.strip()
            if match := re.fullmatch(r"\[(.+)\]", line):
                current = sections.setdefault(match[1], [])
            elif current is not None and "=" in line and not line.startswith("#"):
                key, value = line.split("=", 1)
                current.append((key.strip(), value.split("#")[0].strip()))
        theme = {}
        for kind, name in (("d", "colors-dark"), ("l", "colors-light")):
            if name in sections and (colors := theme_colors(sections[name])):
                theme[kind] = colors
        if theme:
            result[file.name] = theme
        else:
            print(f"skipped: {file.name}", file=sys.stderr)
    return result


# -- Files ----------------------------------------------------------------


def write(name, text):
    (SITE / name).write_text(text)
    print(name)


def png(name, svg, width):
    subprocess.run(["rsvg-convert", "-w", str(width), "-o", SITE / name], input=svg.encode(), check=True)
    print(name)


def main():
    if "--lockup" in sys.argv:
        width, height, body = lockup(lambda i: f'class="c{i}"')
        print(f'<svg class="lockup" viewBox="0 0 {width} {height}" role="img" aria-label="foot">{body}</svg>')
        return

    data = themes()
    write(
        "themes.js",
        "// Generated by make-assets.py from themes/ and macos/themes/. Per theme: its\n"
        "// dark (d) and light (l) colors, 21 hex colors each: background, foreground,\n"
        "// regular0-7, bright0-7, selection background and foreground, cursor.\n"
        f"window.FOOT_THEMES = {json.dumps(data, separators=(',', ':'))};\n",
    )

    rows = icon_map()
    height, width = len(rows), len(rows[0])
    top = (width - height) / 2
    write(
        "favicon.svg",
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 {-top:g} {width} {width}" shape-rendering="crispEdges">'
        f"<defs>{gradient('g', 0, height)}</defs>{icon_svg(rows)}</svg>\n",
    )

    # Home screen icon: iOS rounds the corners itself
    full = ["#" * width] * 2 + [row.replace(".", "#") for row in rows] + ["#" * width] * 2
    full = [row if "w" in row else "#" * width for row in full]
    png(
        "apple-touch-icon.png",
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {width}" shape-rendering="crispEdges">'
        f"<defs>{gradient('g', 0, width)}</defs>{icon_svg(full)}</svg>",
        180,
    )

    # Link previews (Open Graph), 1200x630, in the default colors
    night = data["grok-night"]["d"]
    color = lambda i: "#" + night[i * 6 : i * 6 + 6]
    lockup_width, _, body = lockup(lambda i: f'fill="{color(2 + i)}"')
    scale = 15
    x = (1200 - lockup_width * scale) / 2
    png(
        "og.png",
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1200 630">'
        f'<rect width="1200" height="630" fill="{color(0)}"/>'
        f'<g transform="translate({x:g} 96) scale({scale})" shape-rendering="crispEdges" fill="{color(1)}">{body}</g>'
        f'<text x="{x:g}" y="520" font-family="Menlo" font-size="34" fill="{color(1)}">'
        "The fast, lightweight terminal emulator.</text>"
        f'<text x="{x:g}" y="568" font-family="Menlo" font-size="34" fill="#949494">Native on the Mac.</text>'
        "</svg>",
        1200,
    )

    # The screenshot of .github/README.md
    for name, resize in (("splits.webp", []), ("splits-small.webp", ["-resize", "1262", "0"])):
        subprocess.run(
            ["cwebp", "-quiet", "-q", "82", "-alpha_q", "90", "-m", "6", *resize,
             ROOT / ".github/splits.png", "-o", SITE / name],
            check=True,
        )
        print(name)


if __name__ == "__main__":
    main()
