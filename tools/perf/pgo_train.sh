#!/usr/bin/env bash
# Profile-guided optimization training for the game image (CMake SHADOW_LTO=ON SHADOW_PGO=GEN, then USE).
#
# usage: pgo_train.sh <instrumented exe> <pgd> <game dir> <recordings dir> <work dir> [budget_ms]
#
# Runs the instrumented exe headless on recorded gameplay tapes and merges the counts into <pgd>. The training
# set is deliberately NOT the benchmark or gate tapes (platform/ab-westopolis host-a, recordings/final-haunt):
#   lethal-highway-sfx                    Lethal Highway (stg0202), a player recording
#   user-westopolis-720p-20261109-092727  a separate Westopolis play by the user (not the benchmark tape)
# The instrumented runtime (pgort140.dll) must be on PATH: it lives next to the toolset's link.exe.
set -u
case $1 in /*|?:*) ;; *) echo "pgo_train.sh: give the exe as an absolute path"; exit 1;; esac
export MSYS2_ARG_CONV_EXCL='*'   # Git bash would rewrite pgomgr's /merge into a path
exe=$1; pgd=$2; game=$3; rec=$4; work=$5; budget=${6:-150000}
mkdir -p "$work"
run() {  # <name> <stage> <tape dir> <files dir> <card dir>
    local d=$work/$1; rm -rf "$d"; mkdir -p "$d"
    cp "$4/display.cfg" "$4/pad.cfg" "$d/"
    cp "$5/card.raw" "$d/card.raw"; [ -f "$5/SRAM.pc.raw" ] && cp "$5/SRAM.pc.raw" "$d/"
    ( cd "$d" && "$exe" --game-data "$game" --boot-stage "$2" --headless --no-audio --memory-card card.raw \
        --display-cfg display.cfg --pad-cfg pad.cfg --state-file unused.gekkostate --snapshot-key none --no-xinput \
        --budget-ms "$budget" --input-tape-play "$(cygpath -m "$3/input.tape")" --native-output 1280x720 \
        --width 1280 --height 720 --json-report session.json > stdout.log 2> stderr.log )
    local rc=$?
    echo "$1 exit $rc"
    [ $rc -eq 0 ] || { echo "TRAIN-FAIL $1"; exit 2; }
}
lh=$rec/lethal-highway-sfx
run lethal-highway stg0202 "$lh" "$lh/initial" "$lh/initial"
uw=$rec/user-westopolis-720p-20261109-092727
run user-westopolis stg0100 "$uw" "$uw/initial" "$lh/initial"
pgc_dir=$(dirname "$exe"); base=$(basename "$pgd" .pgd)   # the runtime writes <name>!N.pgc next to the exe
ls "$pgc_dir"/"$base"\!*.pgc || { echo "NO-PGC: the instrumented runs wrote no counts"; exit 2; }
for f in "$pgc_dir"/"$base"\!*.pgc; do pgomgr /merge "$(cygpath -w "$f")" "$(cygpath -w "$pgd")" > /dev/null || { echo "MERGE-FAIL $f"; exit 3; }; rm -f "$f"; done
pgomgr /summary "$(cygpath -w "$pgd")" | head -20
