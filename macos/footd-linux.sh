#!/bin/sh
#
# Builds footd for Linux, for foot.app to install on the machines it
# reaches over SSH (Shell > New Window on; see footd.c): x86_64 and
# aarch64, linked statically (musl, in Alpine containers), so they run
# on any Linux. make-bundle.sh puts them in foot.app/Contents/Resources.
#
# Usage: macos/footd-linux.sh DIR   (DIR/footd-linux-x86_64, DIR/footd-linux-aarch64)
#
# Needs Docker; without it, nothing is built (and foot.app can't install
# footd on Linux). Built again only when the sources have changed.

set -eu

out=${1:?usage: $0 DIR}
root=$(cd "$(dirname "$0")/.." && pwd)

if ! docker info >/dev/null 2>&1; then
    echo "footd-linux.sh: Docker isn't running: foot.app won't have footd for Linux" >&2
    exit 0
fi

mkdir -p "$out"
out=$(cd "$out" && pwd)

# What footd is built from (the sources, and this script)
sources="*.c *.h *meson.build meson_options.txt subprojects/*.wrap macos/footd-linux.sh"
# shellcheck disable=SC2086
key=$(cd "$root" && { git ls-files -s -- $sources; git diff HEAD -- $sources; } | shasum | cut -c1-16)

for arch in x86_64 aarch64; do
    bin=$out/footd-linux-$arch
    if [ -f "$bin" ] && [ "$(cat "$bin.key" 2>/dev/null)" = "$key" ]; then
        continue
    fi

    case $arch in
    x86_64)  platform=linux/amd64 ;;
    aarch64) platform=linux/arm64 ;;
    esac

    echo "footd-linux.sh: building footd for Linux $arch"
    rm -f "$bin" "$bin.key"
    docker run --rm --platform "$platform" -e ARCH="$arch" \
        -v "$root":/src:ro -v "$out":/out alpine:3.22 sh -c '
set -eu
apk add --no-cache build-base meson ninja pkgconf python3 bison linux-headers git >/dev/null
git config --global --add safe.directory /src
meson setup /tmp/build /src --buildtype=release -Dtests=false \
    -Ddefault_library=static -Dprefer_static=true -Dc_link_args=-static >/tmp/build.log 2>&1 || {
    tail -30 /tmp/build.log
    exit 1
}
ninja -C /tmp/build macos/footd >/tmp/build.log 2>&1 || {
    grep -E "error|FAILED" /tmp/build.log | head -30
    exit 1
}
strip -o "/out/footd-linux-$ARCH" /tmp/build/macos/footd
'
    echo "$key" >"$bin.key"
done
