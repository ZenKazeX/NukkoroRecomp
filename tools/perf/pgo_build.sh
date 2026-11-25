#!/usr/bin/env bash
# One-command profile-guided build of the game image (cpu/opt 2026-11-15).
#
# usage: pgo_build.sh <build tree> <game dir> <recordings dir> [extra cmake -D... for the tree]
#
#   1. configure <tree> with SHADOW_LTO=ON SHADOW_LTO_SHARDS=OFF SHADOW_PGO=GEN and build ShadowTheHedgehog
#      (the runtime and native owners become /GL objects; the generated shards stay ordinary objects, so the
#      link takes minutes instead of hours);
#   2. train the instrumented image (tools/perf/pgo_train.sh: recorded gameplay that is neither the benchmark
#      nor a gate tape) and merge the counts into <tree>/pgo/ShadowTheHedgehog.pgd;
#   3. reconfigure with SHADOW_PGO=USE and relink (the /GL objects are reused; only the link runs again).
#
# The profile decides code layout, inlining and branch order only; /fp:precise holds, so guest results are
# bit-identical (gate every image like any other build). A profile matches functions by checksum: after source
# changes, retrain (functions whose code changed fall back to the unprofiled /Os path and the linker reports them).
# The tree must already be configured for the game (generator, DOL, dolrecon dir); extra -D args pass through.
set -eu
tree=$1; game=$2; rec=$3; shift 3
mkdir -p "$tree"; tree=$(cd "$tree" && pwd)   # absolute: the training runs cd into their own directories
src=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$tree/pgo"
rm -f "$tree/pgo/ShadowTheHedgehog.pgd"
cmake -S "$src" -B "$tree" -DSHADOW_LTO=ON -DSHADOW_LTO_SHARDS=OFF -DSHADOW_PGO=GEN "$@"
cmake --build "$tree" --config Release --target ShadowTheHedgehog -- -m:3 -v:m
link_dir=$(dirname "$(cmake -LA -N "$tree" | sed -n 's/^CMAKE_LINKER:FILEPATH=//p')")
PATH="$(cygpath -u "$link_dir"):$PATH" bash "$src/tools/perf/pgo_train.sh" \
    "$tree/bin/Release/ShadowTheHedgehog.exe" "$tree/pgo/ShadowTheHedgehog.pgd" "$game" "$rec" "$tree/pgo/train"
cmake -S "$src" -B "$tree" -DSHADOW_PGO=USE
cmake --build "$tree" --config Release --target ShadowTheHedgehog -- -m:3 -v:m
