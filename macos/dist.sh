#!/bin/bash
#
# Builds foot.app for distribution: universal (Apple silicon and Intel),
# with Sparkle (foot > Check for Updates…), signed with a Developer ID
# and the hardened runtime, notarized and stapled; and zipped.
#
# Usage: dist.sh <version>  (e.g. 1.28.0-macos.1)
#        dist.sh --fetch-sparkle  (only fetches Sparkle, and its tools)
#
# Environment:
#   NOTARY_PROFILE  notarytool credentials in the keychain (see
#                   `xcrun notarytool store-credentials`); default: the
#                   profile make publish remembered (git config
#                   foot.notaryprofile)
#   SIGN_IDENTITY   the signing identity; default: the keychain's
#                   Developer ID Application identity
#
# Output, in build-release/dist/: foot.app, and foot-<version>.zip.

set -euo pipefail

SPARKLE_VERSION=2.10.0
SPARKLE_SHA256=c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c

# The appcast (published with each release by macos/publish.sh), and the
# public key of the updates' signatures; the private key is in the
# keychain of whoever publishes releases (Sparkle's generate_keys
# --account foot-macos)
SPARKLE_FEED=https://github.com/gobijan/foot-macos/releases/latest/download/appcast.xml
SPARKLE_PUBLIC_KEY=${SPARKLE_PUBLIC_KEY:-hSItVWtGBebUpG7NtVCJT+7njBWaIRC9yhaibwX9Cgg=}

fail() {
    echo "dist.sh: $*" >&2
    exit 1
}

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

sparkle=build-release/sparkle-$SPARKLE_VERSION
fetch_sparkle() {
    [[ ! -d $sparkle/Sparkle.framework ]] || return 0
    echo "==> Fetching Sparkle $SPARKLE_VERSION"
    rm -rf "$sparkle"
    mkdir -p "$sparkle"
    curl -fsSL -o "$sparkle.tar.xz" \
        "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"
    echo "$SPARKLE_SHA256  $sparkle.tar.xz" | shasum -a 256 -c - >/dev/null ||
        fail "Sparkle-$SPARKLE_VERSION.tar.xz: checksum mismatch"
    tar -xJf "$sparkle.tar.xz" -C "$sparkle"
    rm "$sparkle.tar.xz"
}

[[ $# -eq 1 ]] || fail "usage: dist.sh <version>"
if [[ $1 == --fetch-sparkle ]]; then
    fetch_sparkle
    exit
fi

version=$1
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || fail "invalid version: $version"
NOTARY_PROFILE=${NOTARY_PROFILE:-$(git config foot.notaryprofile || true)}
[[ -n $NOTARY_PROFILE ]] || fail "NOTARY_PROFILE isn't set (xcrun notarytool store-credentials)"
[[ -n $SPARKLE_PUBLIC_KEY ]] || fail "SPARKLE_PUBLIC_KEY isn't set"

if [[ -z ${SIGN_IDENTITY:-} ]]; then
    identities=$(security find-identity -v -p codesigning | grep -c '"Developer ID Application: ' || true)
    [[ $identities -eq 1 ]] || fail "found $identities Developer ID Application identities; set SIGN_IDENTITY"
    SIGN_IDENTITY=$(security find-identity -v -p codesigning |
                    sed -n 's/.*"\(Developer ID Application: .*\)"$/\1/p')
fi

meson_options=(--native-file macos/native.ini -Dgrapheme-clustering=enabled --buildtype=release)

echo "==> Building (Apple silicon)"
[[ -f build-release/build.ninja ]] || meson setup build-release "${meson_options[@]}"
ninja -C build-release

echo "==> Building (Intel)"
[[ -f build-x86_64/build.ninja ]] ||
    meson setup build-x86_64 "${meson_options[@]}" --cross-file macos/x86_64.ini
ninja -C build-x86_64

fetch_sparkle

echo "==> Assembling foot.app"
dist=build-release/dist
app=$dist/foot.app
rm -rf "$dist"
mkdir -p "$dist"
ditto build-release/macos/foot.app "$app"

for exe in foot footclient; do
    lipo -create build-release/macos/$exe build-x86_64/macos/$exe -output "$app/Contents/MacOS/$exe"
done

# CFBundleVersion: a number that grows with every release (Sparkle
# compares it); the commit count of the 'macos' branch
plist=$app/Contents/Info.plist
plutil -replace CFBundleShortVersionString -string "$version" "$plist"
plutil -replace CFBundleVersion -string "$(git rev-list --count HEAD)" "$plist"
plutil -replace SUFeedURL -string "$SPARKLE_FEED" "$plist"
plutil -replace SUPublicEDKey -string "$SPARKLE_PUBLIC_KEY" "$plist"

# The XPC services are for sandboxed applications only
mkdir -p "$app/Contents/Frameworks"
ditto "$sparkle/Sparkle.framework" "$app/Contents/Frameworks/Sparkle.framework"
rm -rf "$app/Contents/Frameworks/Sparkle.framework/Versions/B/XPCServices" \
       "$app/Contents/Frameworks/Sparkle.framework/XPCServices"

echo "==> Signing ($SIGN_IDENTITY)"
sign() {
    codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" "$@"
}
framework=$app/Contents/Frameworks/Sparkle.framework
sign "$framework/Versions/B/Autoupdate"
sign "$framework/Versions/B/Updater.app"
sign "$framework"
sign "$app/Contents/MacOS/footclient"
# The privacy permissions of the programs running in foot
sign --entitlements macos/foot.entitlements "$app"
codesign --verify --deep --strict "$app"

echo "==> Notarizing"
zip=$dist/foot-$version.zip
ditto -c -k --keepParent "$app" "$zip"
xcrun notarytool submit "$zip" --keychain-profile "$NOTARY_PROFILE" --wait --timeout 30m \
    --output-format plist > "$dist/notary.plist"
status=$(plutil -extract status raw -o - "$dist/notary.plist")
if [[ $status != Accepted ]]; then
    id=$(plutil -extract id raw -o - "$dist/notary.plist")
    xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" >&2 || true
    fail "notarization: $status"
fi

# The ticket is stapled to the application, which is zipped again
xcrun stapler staple "$app"
rm "$zip"
ditto -c -k --keepParent "$app" "$zip"
spctl --assess --type execute "$app"

echo "==> $zip"
shasum -a 256 "$zip"
