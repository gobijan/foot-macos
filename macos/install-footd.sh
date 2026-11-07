#!/bin/sh
#
# Installs footd on another machine (Linux), for sessions there (Shell >
# New Window on): builds it in a container, for that machine's
# architecture, and copies it to ~/.local/bin/footd there. Nothing else
# is installed there.
#
# Usage: macos/install-footd.sh [user@]host   (make install-footd HOST=...)
#
# Needs Docker, and SSH access to the machine. Built on Debian 12 (with
# meson from its backports): it runs with glibc 2.36 and later. A footd
# that runs there keeps running (with its sessions) until they've ended;
# the new one is used after.

set -eu

host=${1:?usage: $0 [user@]host}
root=$(cd "$(dirname "$0")/.." && pwd)

arch=$(ssh -o BatchMode=yes "$host" uname -m)
case $arch in
x86_64)         platform=linux/amd64 ;;
aarch64|arm64)  platform=linux/arm64 ;;
*)
    echo "install-footd.sh: $host: unknown architecture $arch" >&2
    exit 1
    ;;
esac

out=$(mktemp -d "${TMPDIR:-/tmp}/footd.XXXXXX")
trap 'rm -f "$out/footd"; rmdir "$out"' EXIT

echo "install-footd.sh: building footd for $arch"
docker run --rm --platform "$platform" -v "$root":/src:ro -v "$out":/out debian:bookworm sh -c '
set -eu
# (Debian 12 has meson 1.0; pixman needs 1.3)
echo "deb http://deb.debian.org/debian bookworm-backports main" \
    >/etc/apt/sources.list.d/backports.list
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends \
    build-essential ninja-build pkg-config python3 bison ca-certificates git >/dev/null
apt-get install -y -qq --no-install-recommends -t bookworm-backports meson >/dev/null
meson setup /tmp/build /src --buildtype=release -Dtests=false >/tmp/build.log 2>&1 || {
    tail -30 /tmp/build.log
    exit 1
}
ninja -C /tmp/build macos/footd >/tmp/build.log 2>&1 || {
    grep -E "error|FAILED" /tmp/build.log | head -30
    exit 1
}
strip /tmp/build/macos/footd
cp /tmp/build/macos/footd /out/footd
'

echo "install-footd.sh: installing it on $host"
ssh -o BatchMode=yes "$host" 'mkdir -p ~/.local/bin'
scp -q -o BatchMode=yes "$out/footd" "$host:.local/bin/footd.new"
ssh -o BatchMode=yes "$host" 'mv ~/.local/bin/footd.new ~/.local/bin/footd && ~/.local/bin/footd --version'
