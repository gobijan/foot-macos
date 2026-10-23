#!/bin/sh
# Draws foot's application icon in the terminal, two pixels per character
# cell, and beside it foot's default color palette (regular colors above,
# bright colors below). The arguments are lines of text (printf %b escapes
# allowed) set above the palette.
#
#   macos/banner.sh 'foot 1.28.0' 'The fast, lightweight terminal emulator'
if [ ! -t 1 ] || [ -n "${NO_COLOR:-}" ] || [ "${TERM:-}" = dumb ]; then
  for line in "$@"; do printf '  %b\n' "$line"; done
  exit 0
fi
case "${COLORTERM:-}" in truecolor | 24bit) truecolor=1 ;; *) truecolor=0 ;; esac

{
  # Palette: the icon's colors (macos/icon/foot.svg); '#' is the
  # background, a gradient from top to bottom.
  cat <<'EOF'
# 3a3a3a 1e1e1e
w fbf1c7
COLORS
242424 616161
f62b5a ff4d51
47b413 35d450
e3c401 e9e836
24acd4 5dc5f8
f2affd feabf2
13c299 24dfc4
e6e6e6 ffffff
MAP
...####################...
.########################.
.########################.
##########################
#####ww###################
#####ww#w#################
##########w###############
####wwwww##w##############
####wwwwwww#w#############
####wwwwwwww##############
#####wwwwwww##############
######wwwwww##############
#######wwww###############
######wwwww###############
######wwww################
####wwwwww################
####wwwww#####wwwwwww#####
####wwww######wwwwwww#####
#####ww###################
.########################.
.########################.
...####################...
TEXT
EOF
  for line in "$@"; do printf '%b\n' "$line"; done
} | awk -v truecolor="$truecolor" '
function hex(s) { return (index("0123456789abcdef", substr(s, 1, 1)) - 1) * 16 + index("0123456789abcdef", substr(s, 2, 1)) - 1 }
# The escape color (after 38; or 48;) of r, g, b; the nearest of 256 without
# truecolor (grays from the grayscale ramp).
function rgb(r, g, b) {
  if (truecolor) return "2;" r ";" g ";" b
  if (r == g && g == b) return "5;" (r < 4 ? 16 : r > 243 ? 231 : 232 + int((r - 8) / 10 + 0.5))
  return "5;" 16 + 36 * int(r / 51 + 0.5) + 6 * int(g / 51 + 0.5) + int(b / 51 + 0.5)
}
function color(value) { return rgb(hex(substr(value, 1, 2)), hex(substr(value, 3, 2)), hex(substr(value, 5, 2))) }
# The background gradient at map row y.
function background(y,   t) {
  t = rows > 1 ? y / (rows - 1) : 0
  return rgb(int(top_r + (bottom_r - top_r) * t + 0.5), int(top_g + (bottom_g - top_g) * t + 0.5), int(top_b + (bottom_b - top_b) * t + 0.5))
}
# The color of pixel x (1-based) in map row y, empty if transparent.
function pixel(y, x,   c) {
  c = substr(map[y], x, 1)
  return c == "#" ? background(y) : c == "." || c == "" ? "" : palette[c]
}
$0 == "COLORS" || $0 == "MAP" || $0 == "TEXT" { section = $0; next }
section == "" && $1 == "#" {
  top_r = hex(substr($2, 1, 2)); top_g = hex(substr($2, 3, 2)); top_b = hex(substr($2, 5, 2))
  bottom_r = hex(substr($3, 1, 2)); bottom_g = hex(substr($3, 3, 2)); bottom_b = hex(substr($3, 5, 2))
  next
}
section == "" { palette[$1] = color($2); next }
section == "COLORS" { regular[colors + 0] = color($1); bright[colors++] = color($2); next }
section == "MAP" { map[rows++] = $0; next }
section == "TEXT" { side[lines++] = $0 }
END {
  # The palette: one cell per color pair, regular above, bright below.
  side[lines++] = ""
  out = ""
  for (i = 0; i < colors; i++) out = out "\033[38;" regular[i] ";48;" bright[i] "m▀▀▀"
  side[lines++] = out "\033[0m"
  first = int((rows / 2 - lines) / 2 + 0.5)
  for (y = 0; y < rows; y += 2) {
    out = "  "
    for (x = 1; x <= length(map[y]); x++) {
      top = pixel(y, x); bottom = pixel(y + 1, x)
      if (top != "" && bottom != "") out = out "\033[38;" top ";48;" bottom "m▀"
      else if (top != "") out = out "\033[0;38;" top "m▀"
      else if (bottom != "") out = out "\033[0;38;" bottom "m▄"
      else out = out "\033[0m "
    }
    printf "%s\033[0m   %s\n", out, side[y / 2 - first]
  }
}'
