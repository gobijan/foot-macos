#!/bin/sh
#
# Installs footd on another machine (Linux) by hand, as foot.app does
# itself when a window is opened there (Shell > New Window on): copies
# it to ~/.local/bin/footd there. Nothing else is installed there.
#
# Usage: macos/install-footd.sh [user@]host   (make install-footd HOST=...)
#
# Needs Docker (footd is built by footd-linux.sh: linked statically, it
# runs on any Linux), and SSH access to the machine. A footd that runs
# there keeps running (with its sessions) until they've ended; the new
# one is used after.

set -eu

host=${1:?usage: $0 [user@]host}
root=$(cd "$(dirname "$0")/.." && pwd)
dir=$root/build-release/footd-linux

arch=$(ssh -o BatchMode=yes "$host" uname -m)
case $arch in
x86_64|amd64)   arch=x86_64 ;;
aarch64|arm64)  arch=aarch64 ;;
*)
    echo "install-footd.sh: $host: unknown architecture $arch" >&2
    exit 1
    ;;
esac

docker info >/dev/null 2>&1 || { echo "install-footd.sh: Docker must run" >&2; exit 1; }
"$root/macos/footd-linux.sh" "$dir"

echo "install-footd.sh: installing it on $host"
ssh -o BatchMode=yes "$host" 'umask 022; d="$HOME/.local/bin"; mkdir -p "$d" &&
    cat >"$d/footd.new" && chmod 755 "$d/footd.new" &&
    mv -f "$d/footd.new" "$d/footd" && "$d/footd" --version' <"$dir/footd-linux-$arch"
