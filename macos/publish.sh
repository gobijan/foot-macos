#!/bin/bash
#
# Publishes a release of foot for macOS: tags HEAD, builds the notarized
# foot.app (macos/dist.sh), uploads it to a GitHub release with the
# Sparkle appcast (foot > Check for Updates…), and updates the Homebrew
# cask (gobijan/homebrew-tap).
#
# Usage: publish.sh <version> [notes.md]
#
#   version  <upstream version>-macos.<n>, e.g. 1.28.0-macos.1
#   notes    the release notes, in Markdown; by default, the subjects of
#            the commits since the previous release
#
# Environment: as macos/dist.sh (NOTARY_PROFILE, SIGN_IDENTITY). The
# updates' signing key must be in the keychain (Sparkle's generate_keys
# --account foot-macos, see .github/RELEASING.md).

set -euo pipefail

REPO=gobijan/foot-macos
TAP=gobijan/homebrew-tap
SPARKLE_ACCOUNT=foot-macos

fail() {
    echo "publish.sh: $*" >&2
    exit 1
}

[[ $# -ge 1 && $# -le 2 ]] || fail "usage: publish.sh <version> [notes.md]"
version=$1
notes_file=${2:-}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+-macos\.[0-9]+$ ]] ||
    fail "the version must look like 1.28.0-macos.1"
[[ -z $notes_file || -f $notes_file ]] || fail "$notes_file: no such file"

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

echo "==> Checking"
[[ $(git rev-parse --abbrev-ref HEAD) == macos ]] || fail "not on the 'macos' branch"
[[ -z $(git status --porcelain) ]] || fail "there are uncommitted changes"
git fetch -q origin macos
[[ $(git rev-parse HEAD) == "$(git rev-parse origin/macos)" ]] ||
    fail "HEAD isn't origin/macos; push first"
! git rev-parse -q --verify "refs/tags/$version" >/dev/null || fail "the tag $version exists"
! gh release view "$version" -R "$REPO" >/dev/null 2>&1 || fail "the release $version exists"

# The updates must be signed with the key whose public half is in foot.app
sparkle=build-release/sparkle-$(sed -n 's/^SPARKLE_VERSION=//p' macos/dist.sh)
public_key=$(sed -n 's/^SPARKLE_PUBLIC_KEY=\${SPARKLE_PUBLIC_KEY:-\(.*\)}$/\1/p' macos/dist.sh)
[[ -n $public_key ]] || fail "macos/dist.sh: no SPARKLE_PUBLIC_KEY"

check_key() {
    [[ $("$sparkle/bin/generate_keys" --account "$SPARKLE_ACCOUNT" -p) == "$public_key" ]] ||
        fail "the keychain's Sparkle key (--account $SPARKLE_ACCOUNT) isn't SPARKLE_PUBLIC_KEY"
}

# Before building, if Sparkle's tools are there already (macos/dist.sh
# fetches them)
[[ ! -x $sparkle/bin/generate_keys ]] || check_key

if [[ -z $notes_file ]]; then
    previous=$(git describe --tags --abbrev=0 --match '*-macos.*' 2>/dev/null) ||
        fail "no previous release; write the release notes, and pass them"
    notes_file=$(mktemp)
    git log --first-parent --format='* %s' "$previous..HEAD" > "$notes_file"
fi

echo "==> Tagging $version"
git tag -a "$version" -m "foot $version"
pushed=no
trap '[[ $pushed == yes ]] || git tag -d "$version" >/dev/null' EXIT

# foot --version is the tag (git describe)
./macos/dist.sh "$version"

dist=build-release/dist
zip=$dist/foot-$version.zip

check_key
signature=$("$sparkle/bin/sign_update" --account "$SPARKLE_ACCOUNT" "$zip")

echo "==> Appcast"
plist=$dist/foot.app/Contents/Info.plist
python3 -I - "$version" "$(plutil -extract CFBundleVersion raw -o - "$plist")" \
    "$(plutil -extract LSMinimumSystemVersion raw -o - "$plist")" "$signature" \
    "$notes_file" "$REPO" > "$dist/appcast.xml" <<'EOF'
import email.utils, html, re, sys

version, build, min_system, signature, notes_file, repo = sys.argv[1:]

# The release notes, as simple HTML: list items and paragraphs
notes, items = [], []
for line in open(notes_file, encoding='utf-8').read().splitlines():
    text = html.escape(line.strip())
    text = re.sub(r'`([^`]+)`', r'<code>\1</code>', text)
    text = re.sub(r'\[([^]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', text)
    if line.lstrip().startswith(('* ', '- ')):
        items.append(f'<li>{text[2:]}</li>')
        continue
    if items:
        notes.append('<ul>' + ''.join(items) + '</ul>')
        items = []
    if text:
        notes.append(f'<p>{text.lstrip("#").strip()}</p>')
if items:
    notes.append('<ul>' + ''.join(items) + '</ul>')

url = f'https://github.com/{repo}/releases/download/{version}/foot-{version}.zip'
print(f'''<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>foot for macOS</title>
    <item>
      <title>foot {version}</title>
      <pubDate>{email.utils.formatdate(usegmt=True)}</pubDate>
      <sparkle:version>{build}</sparkle:version>
      <sparkle:shortVersionString>{version}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>{min_system}</sparkle:minimumSystemVersion>
      <sparkle:fullReleaseNotesLink>https://github.com/{repo}/releases/tag/{version}</sparkle:fullReleaseNotesLink>
      <description><![CDATA[{''.join(notes)}]]></description>
      <enclosure url="{url}" {signature} type="application/octet-stream"/>
    </item>
  </channel>
</rss>''')
EOF

echo "==> GitHub release"
git push -q origin "refs/tags/$version"
pushed=yes
gh release create "$version" "$zip" "$dist/appcast.xml" -R "$REPO" \
    --verify-tag --title "foot $version" --notes-file "$notes_file"

echo "==> Homebrew cask"
tap=build-release/homebrew-tap
if [[ -d $tap/.git ]]; then
    git -C "$tap" pull -q --ff-only
elif gh repo view "$TAP" >/dev/null 2>&1; then
    gh repo clone "$TAP" "$tap" -- -q
else
    # The first release: create the tap
    rm -rf "$tap"
    mkdir -p "$tap"
    git -C "$tap" init -q -b main
    cp macos/homebrew/README.md "$tap/"
    git -C "$tap" add README.md
    gh repo create "$TAP" --public --description "Homebrew casks: foot for macOS"
    git -C "$tap" remote add origin "git@github.com:$TAP.git"
fi
mkdir -p "$tap/Casks"
sha256=$(shasum -a 256 "$zip" | cut -d' ' -f1)
sed -e "s/@version@/$version/" -e "s/@sha256@/$sha256/" macos/homebrew/foot.rb.in > "$tap/Casks/foot.rb"
git -C "$tap" add Casks/foot.rb
git -C "$tap" commit -q -m "foot $version"
git -C "$tap" push -q -u origin main

echo "==> Published: https://github.com/$REPO/releases/tag/$version"
