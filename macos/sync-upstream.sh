#!/bin/sh
#
# Merges upstream foot into the current branch (normally 'macos'), and
# builds and tests the result.
#
#   macos/sync-upstream.sh [--no-build]
#
# Conflict resolutions are recorded with git rerere, so a conflict only
# ever has to be resolved once. See macos/UPSTREAM.md for how to
# resolve conflicts.

set -eu

upstream_url=https://codeberg.org/dnkl/foot.git
upstream_ref=upstream/master
build=yes

for arg in "$@"; do
    case "$arg" in
        --no-build) build=no ;;
        -h|--help) sed -n '3,10p' "$0" | cut -c3-; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

cd "$(git rev-parse --show-toplevel)"

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "error: uncommitted changes; commit or stash them first" >&2
    exit 1
fi

git remote get-url upstream >/dev/null 2>&1 || git remote add upstream "$upstream_url"
git config rerere.enabled true
git config rerere.autoupdate true

git fetch --quiet --tags upstream master

base=$(git merge-base HEAD "$upstream_ref")
count=$(git rev-list --count "HEAD..$upstream_ref")

if [ "$count" -eq 0 ]; then
    echo "Already up to date with $upstream_ref ($(git describe --tags --always "$upstream_ref"))"
    exit 0
fi

echo "Merging $count upstream commit(s), up to $(git describe --tags --always "$upstream_ref"):"
git log --oneline --no-decorate "HEAD..$upstream_ref" | sed 's/^/  /'
echo

# Upstream files that are kept, but not built on macOS (their job is
# done by macos/). Changes there may need to be mirrored in macos/.
platform_files='wayland.c wayland.h shm.c quirks.c quirks.h shm-formats.h'
changed=$(git diff --name-only "$base" "$upstream_ref" -- $platform_files)
if [ -n "$changed" ]; then
    echo "Upstream changed platform code; check whether macos/ needs the same:"
    for f in $changed; do
        echo "  $f:"
        git log --oneline --no-decorate "$base..$upstream_ref" -- "$f" | sed 's/^/    /'
    done
    echo
fi

if [ -n "$(git diff --name-only "$base" "$upstream_ref" -- meson.build)" ]; then
    echo "Upstream changed meson.build; mirror new sources/targets in macos/meson.build:"
    git diff "$base" "$upstream_ref" -- meson.build | grep -E '^[-+][^-+]' | sed 's/^/  /' | head -40
    echo
fi

# Keep 'master' identical to upstream (unless it's checked out)
if [ "$(git rev-parse --abbrev-ref HEAD)" != master ]; then
    git branch --quiet --force master "$upstream_ref"
fi

if ! git merge --no-edit -m "Merge upstream foot $(git describe --tags --always "$upstream_ref")" "$upstream_ref"; then
    echo
    echo "Conflicts (see macos/UPSTREAM.md for how to resolve them):"
    git diff --name-only --diff-filter=U | sed 's/^/  /'
    echo
    echo "When resolved: git add <files> && git commit, then re-run with --no-build"
    echo "skipped, or build and test (ninja -C build && meson test -C build config)."
    exit 1
fi

if [ "$build" = yes ]; then
    [ -d build ] || meson setup build --native-file macos/native.ini
    ninja -C build
    meson test -C build config --print-errorlogs
fi

echo
echo "Done. Try it (./build/macos/foot), then: git push"
