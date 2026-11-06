#!/bin/sh
#
# Assembles foot.app.
#
# Usage: make-bundle.sh <foot.app> <foot> <footclient> <footd> <Info.plist> <foot.icns> <terminfo dir> <themes dir> <macOS themes dir>

set -eu

app=$1 foot=$2 footclient=$3 footd=$4 plist=$5 icns=$6 terminfo=$7 themes=$8 macos_themes=$9

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

# footd (persistent sessions) is started by foot, from next to it
cp "$foot" "$footclient" "$footd" "$app/Contents/MacOS/"
cp "$plist" "$app/Contents/Info.plist"
cp "$icns" "$app/Contents/Resources/foot.icns"
cp -R "$terminfo" "$app/Contents/Resources/terminfo"
cp -R "$themes" "$app/Contents/Resources/themes"
cp "$macos_themes"/* "$app/Contents/Resources/themes/"
printf 'APPL????' > "$app/Contents/PkgInfo"

# Ad-hoc signature (required on Apple Silicon); replace with a real
# identity for distribution
codesign --force --deep --sign - --timestamp=none "$app" 2>/dev/null
