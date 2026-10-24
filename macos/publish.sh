#!/bin/bash
#
# Publishes a release of foot for macOS: tags HEAD, builds the notarized
# foot.app (macos/dist.sh), uploads it to a GitHub release with the
# Sparkle appcast (foot > Check for Updates…), and updates the Homebrew
# cask (gobijan/homebrew-tap).
#
# Usage: publish.sh  (make publish)
#
# It asks for everything: first it checks what a release needs (and
# offers to push, and to wait for CI), then it suggests the version
# (<upstream version>-macos.<n>, e.g. 1.28.0-macos.1), shows the release
# notes (the subjects of the commits since the previous release) to edit
# them, and asks before it publishes anything.
#
# Environment: as macos/dist.sh (NOTARY_PROFILE, SIGN_IDENTITY). The
# notarization profile is asked for once, and remembered in the clone's
# git config (foot.notaryprofile). The updates' signing key must be in
# the keychain (Sparkle's generate_keys --account foot-macos, see
# .github/RELEASING.md).

set -euo pipefail

REPO=gobijan/foot-macos
TAP=gobijan/homebrew-tap
SPARKLE_ACCOUNT=foot-macos

fail() {
    echo "publish.sh: $*" >&2
    exit 1
}

ok() {
    echo "  ✓ $*"
}

# ask <question> <default: y or n>; true for yes
ask() {
    local answer hint='[y/N]'
    [[ $2 == y ]] && hint='[Y/n]'
    read -r -p "  $1 $hint " answer || fail "aborted"
    [[ ${answer:-$2} == [yY]* ]]
}

[[ $# -eq 0 ]] || fail "usage: publish.sh (it asks for everything)"

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

echo "==> Checking"
[[ $(git rev-parse --abbrev-ref HEAD) == macos ]] || fail "not on the 'macos' branch (git switch macos)"
if [[ -n $(git status --porcelain) ]]; then
    git status --short | sed 's/^/    /'
    fail "there are uncommitted changes; commit them, or put them aside (git stash)"
fi
ok "everything is committed"

gh auth status >/dev/null 2>&1 || fail "the GitHub CLI isn't signed in (gh auth login)"

git fetch -q origin macos
behind=$(git rev-list --count HEAD..origin/macos)
[[ $behind -eq 0 ]] || fail "origin/macos has $behind commit(s) that aren't here (git pull)"
ahead=$(git rev-list --count origin/macos..HEAD)
if [[ $ahead -gt 0 ]]; then
    git log --oneline --no-decorate origin/macos..HEAD | sed 's/^/    /'
    ask "$ahead commit(s) aren't pushed yet. Push them now?" y || fail "HEAD isn't pushed"
    git push -q origin macos
fi
ok "pushed"

previous=$(git describe --tags --abbrev=0 --match '*-macos.*' 2>/dev/null || true)
if [[ -n $previous ]]; then
    changes=$(git rev-list --count --first-parent "$previous..HEAD")
    [[ $changes -gt 0 ]] || fail "nothing to release: no commits since $previous"
    ok "$changes commit(s) since $previous"
fi

if [[ -z ${SIGN_IDENTITY:-} ]]; then
    security find-identity -v -p codesigning | grep -q '"Developer ID Application: ' ||
        fail "no Developer ID Application certificate in the keychain (.github/RELEASING.md)"
fi
ok "Developer ID certificate"

# notarytool's credentials in the keychain (xcrun notarytool
# store-credentials <profile>); the profile's name is remembered
profile=${NOTARY_PROFILE:-$(git config foot.notaryprofile || true)}
until [[ -n $profile ]] && xcrun notarytool history --keychain-profile "$profile" >/dev/null 2>&1; do
    if [[ -n $profile ]]; then
        echo "  ✗ the notarization profile '$profile' doesn't work (or there's no network)"
    fi
    echo "    The name given to xcrun notarytool store-credentials (.github/RELEASING.md)"
    read -r -p "  Notarization profile: " profile || fail "aborted"
done
git config foot.notaryprofile "$profile"
export NOTARY_PROFILE=$profile
ok "notarization profile '$profile'"

# The updates must be signed with the key whose public half is in foot.app
sparkle=build-release/sparkle-$(sed -n 's/^SPARKLE_VERSION=//p' macos/dist.sh)
public_key=$(sed -n 's/^SPARKLE_PUBLIC_KEY=\${SPARKLE_PUBLIC_KEY:-\(.*\)}$/\1/p' macos/dist.sh)
[[ -n $public_key ]] || fail "macos/dist.sh: no SPARKLE_PUBLIC_KEY"

# Before building: the first time, macOS asks whether Sparkle's tools
# may use the key in the keychain
./macos/dist.sh --fetch-sparkle
[[ $("$sparkle/bin/generate_keys" --account "$SPARKLE_ACCOUNT" -p) == "$public_key" ]] ||
    fail "the keychain's Sparkle key (--account $SPARKLE_ACCOUNT) isn't SPARKLE_PUBLIC_KEY"
"$sparkle/bin/sign_update" --account "$SPARKLE_ACCOUNT" -p macos/dist.sh >/dev/null ||
    fail "sign_update can't sign with the keychain's Sparkle key"
ok "Sparkle's key"

# CI built and tested this commit (.github/workflows/macos.yml); right
# after a push, its run takes a moment to appear
head=$(git rev-parse HEAD)
for _ in {1..10}; do
    run=$(gh run list -R "$REPO" --workflow macos.yml --commit "$head" --limit 1 \
              --json databaseId,status,conclusion,url \
              --jq '.[0] // empty | "\(.databaseId) \(.status) \(.conclusion) \(.url)"')
    [[ -z $run && $ahead -gt 0 ]] || break
    sleep 3
done
read -r run_id run_status run_conclusion run_url <<< "$run"
if [[ -z $run ]]; then
    echo "  ? CI hasn't built this commit"
elif [[ $run_status != completed ]]; then
    if ask "CI is still building this commit. Wait for it (a minute or two)?" y; then
        gh run watch -R "$REPO" "$run_id" --exit-status --interval 10 >/dev/null ||
            fail "CI failed: $run_url"
        ok "CI passed"
    else
        echo "  ? CI hasn't finished: $run_url"
    fi
elif [[ $run_conclusion == success ]]; then
    ok "CI passed"
else
    echo "  ✗ CI: $run_conclusion, $run_url"
    ask "Publish anyway?" n || fail "CI didn't pass"
fi

echo "==> Version"
upstream=$(sed -n "1,10s/^ *version: '\([0-9.]*\)',\$/\1/p" meson.build)
[[ -n $upstream ]] || fail "meson.build: no version"
# <n> starts at 1 for every upstream version
n=$(git tag -l "$upstream-macos.*" | sed 's/.*-macos\.//' | sort -n | tail -1)
suggested=$upstream-macos.$(( ${n:-0} + 1 ))
echo "  Upstream foot $upstream (meson.build); the previous release: ${previous:-none}"
while true; do
    read -r -p "  Version [$suggested]: " version || fail "aborted"
    version=${version:-$suggested}
    if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+-macos\.[0-9]+$ ]]; then
        echo "  The version must look like $suggested"
    elif git rev-parse -q --verify "refs/tags/$version" >/dev/null ||
         gh release view "$version" -R "$REPO" >/dev/null 2>&1; then
        echo "  $version exists"
    else
        break
    fi
done

echo "==> Release notes"
# Kept until the release is published, for another attempt
notes=build-release/release-notes-$version.md
editor=${VISUAL:-${EDITOR:-vi}}
mkdir -p build-release
if [[ -f $notes ]]; then
    echo "  (as edited before, $notes)"
elif [[ -n $previous ]]; then
    git log --first-parent --format='* %s' "$previous..HEAD" > "$notes"
else
    : > "$notes"
fi
while true; do
    if [[ -s $notes ]]; then
        sed 's/^/    /' "$notes"
        echo
        ask "Edit them (in $editor)?" n || break
    fi
    $editor "$notes"
done

echo "==> Publishing"
echo "  foot $version, from $(git log -1 --format='%h %s')"
echo "  Installed copies update to it (Sparkle, Homebrew)."
ask "Publish it?" n || { echo "Nothing was published."; exit 0; }

echo "==> Tagging $version"
git tag -a "$version" -m "foot $version"
tag_pushed=no
trap '[[ $tag_pushed == yes ]] || git tag -d "$version" >/dev/null' EXIT

# foot --version is the tag (git describe)
./macos/dist.sh "$version"

dist=build-release/dist
zip=$dist/foot-$version.zip

signature=$("$sparkle/bin/sign_update" --account "$SPARKLE_ACCOUNT" "$zip") ||
    fail "sign_update failed"

echo "==> Appcast"
plist=$dist/foot.app/Contents/Info.plist
python3 -I - "$version" "$(plutil -extract CFBundleVersion raw -o - "$plist")" \
    "$(plutil -extract LSMinimumSystemVersion raw -o - "$plist")" "$signature" \
    "$notes" "$REPO" > "$dist/appcast.xml" <<'EOF'
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
tag_pushed=yes
gh release create "$version" "$zip" "$dist/appcast.xml" -R "$REPO" \
    --verify-tag --title "foot $version" --notes-file "$notes"

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
rm "$notes"

echo "==> Published: https://github.com/$REPO/releases/tag/$version"
