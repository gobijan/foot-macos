#!/bin/sh
#
# footd on Linux: macos/meson.build builds only footd there (the session
# daemon, for machines foot reaches over SSH). This builds it in a
# Debian container and runs its tests (meson test sessions footd),
# optimized, and with AddressSanitizer and UBSan.
#
# Usage: macos/linux.sh [PLATFORM]   (make linux [PLATFORM=linux/amd64])
#
# Needs Docker. The platform defaults to the Mac's (linux/arm64 on Apple
# Silicon). linux/amd64 builds there, but runs under Rosetta, which
# doesn't run signal handlers during epoll_pwait(): footd never notices
# a program exiting, and the footd test fails. Test x86_64 natively.

set -eu

platform=${1:-}
root=$(cd "$(dirname "$0")/.." && pwd)

docker run --rm ${platform:+--platform "$platform"} -v "$root":/src:ro debian:trixie sh -c '
set -eu
echo "linux.sh: installing the build tools ($(uname -m))"
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends \
    build-essential meson ninja-build pkg-config python3 bison >/dev/null
useradd -m build

for variant in release sanitize; do
    case $variant in
    release)  options="--buildtype=release" ;;
    sanitize) options="--buildtype=debug -Db_sanitize=address,undefined -Db_lundef=false" ;;
    esac

    echo "linux.sh: $variant"
    meson setup /tmp/$variant /src $options >/tmp/$variant.log 2>&1 || {
        tail -30 /tmp/$variant.log
        exit 1
    }
    ninja -C /tmp/$variant >/tmp/$variant.log 2>&1 || {
        grep -E "error|FAILED" /tmp/$variant.log | head -30
        exit 1
    }
    chown -R build /tmp/$variant
    su build -c "cd /tmp/$variant && UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        meson test --no-rebuild sessions footd --print-errorlogs"
done
'
