#!/bin/bash
#
# A release build with profile guided optimization (and LTO): builds foot
# with instrumentation, in <build-dir>-pgo, has it process the benchmark's
# workloads (macos/bench/tbench.c), and then builds <build-dir> with the
# resulting profile. Windows open on screen, for a few seconds: a real
# profile needs real rendering, so this doesn't work on CI.
#
# Usage: pgo.sh <build-dir> [meson options]
#
# The profile is only valid for the sources it was made from. Functions
# changed afterwards are just not optimized for it, so rebuilding
# <build-dir> without running this again is fine for development.

set -euo pipefail

fail() {
    echo "pgo.sh: $*" >&2
    exit 1
}

[[ $# -ge 1 ]] || fail "usage: pgo.sh <build-dir> [meson options]"

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

build=$1
shift
pgo=$build-pgo

echo "==> Building with instrumentation ($pgo)"
[[ -f $pgo/build.ninja ]] || meson setup "$pgo" "$@" -Db_lto=true -Db_pgo=generate
ninja -C "$pgo"

echo "==> Profiling"
rm -f "$pgo"/default_*.profraw
xcrun cc -O2 -Wall -o "$pgo/tbench" macos/bench/tbench.c
: > "$pgo/empty.ini"
for size in 160x50 400x88; do
    rm -f "$pgo/tbench.txt"
    LLVM_PROFILE_FILE="$root/$pgo/default_%m.profraw" \
        "$pgo/macos/foot" --config="$pgo/empty.ini" -o font=Menlo:size=14 \
        --window-size-chars="$size" "$root/$pgo/tbench" run "$root/$pgo/tbench.txt" 1 \
        < /dev/null
    [[ $(tail -1 "$pgo/tbench.txt") == done ]] || fail "$size: the workloads didn't finish"
done
ls "$pgo"/default_*.profraw > /dev/null
mkdir -p "$build"
xcrun llvm-profdata merge --output="$build/default.profdata" "$pgo"/default_*.profraw

echo "==> Building with the profile ($build)"
[[ -f $build/build.ninja ]] || meson setup "$build" "$@"
meson configure "$build" -Db_lto=true -Db_pgo=use
# ninja doesn't know that objects depend on the profile
ninja -C "$build" clean
ninja -C "$build"
