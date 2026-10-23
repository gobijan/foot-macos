#!/bin/sh
#
# Assembles foot.app.
#
# Usage: make-bundle.sh <foot.app> <foot> <footclient> <Info.plist> <foot.icns> <terminfo dir>

set -eu

app=$1 foot=$2 footclient=$3 plist=$4 icns=$5 terminfo=$6

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

cp "$foot" "$footclient" "$app/Contents/MacOS/"
cp "$plist" "$app/Contents/Info.plist"
cp "$icns" "$app/Contents/Resources/foot.icns"
cp -R "$terminfo" "$app/Contents/Resources/terminfo"
printf 'APPL????' > "$app/Contents/PkgInfo"

# Ad-hoc signature (required on Apple Silicon); replace with a real
# identity for distribution
codesign --force --deep --sign - --timestamp=none "$app" 2>/dev/null
