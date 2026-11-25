#!/usr/bin/env python
"""hot_shards.py - a shard layout that gathers the profiled translated functions in the first N shards.

Why: with SHADOW_LTO_SHARDS=OFF the profile-guided link sees the runtime only; all 48 shards under /GL made the
optimized link run for 6 h without finishing (docs/agents/CPU_OPT_2026-11-15_RESULT.md). The functions a baseline
profile sampled are a small part of the generated code; placed in a few shards of their own, those shards can be /GL
objects (CMake SHADOW_LTO_HOT_SHARDS=N) while the rest stay ordinary objects.

The generator carries a recomp_layout.csv found in its output directory (plan_shards), so the layout this writes is
taken as it is: put it there before the first generator run of a tree. Which shard a function is in changes no
generated statement, only the file it is written to.

  hot_shards.py --layout <existing recomp_layout.csv> --profile <address<TAB>samples<TAB>...<TAB>size tsv>
                --hot-shards N --out <recomp_layout.csv>
"""
import argparse
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--layout', required=True)
    p.add_argument('--profile', required=True)
    p.add_argument('--hot-shards', type=int, required=True)
    p.add_argument('--out', required=True)
    a = p.parse_args()
    lines = open(a.layout).read().split('\n')
    header = lines[0]
    if not header.startswith('ppcrecomp-shards-v1,'):
        raise SystemExit('hot_shards: not a shard layout: ' + a.layout)
    shards = int(header.split(',')[1])
    n = a.hot_shards
    if not 0 < n < shards:
        raise SystemExit('hot_shards: --hot-shards must be between 1 and %d' % (shards - 1))
    order, place = [], {}
    for line in lines[1:]:
        if line.strip():
            address, shard = line.split(',')
            order.append(int(address, 16))
            place[order[-1]] = int(shard)
    hot = {}
    for line in open(a.profile):
        if line.startswith('#') or not line.strip():
            continue
        f = line.rstrip('\n').split('\t')
        address = int(f[0], 16)
        if address in place:
            hot[address] = (int(f[1]), int(f[-1]))
    # the hot functions over the first N shards, largest first into the lightest shard (by guest size)
    load = [0] * n
    for address in sorted(hot, key=lambda x: -hot[x][1]):
        k = load.index(min(load))
        place[address] = k
        load[k] += hot[address][1]
    # everything else out of the first N shards, into the shard after them with the fewest rows
    count = [0] * shards
    for address in order:
        if address not in hot and place[address] >= n:
            count[place[address]] += 1
    moved = 0
    for address in order:
        if address not in hot and place[address] < n:
            k = min(range(n, shards), key=lambda s: count[s])
            place[address] = k
            count[k] += 1
            moved += 1
    with open(a.out, 'w', newline='\n') as f:
        f.write(header + '\n')
        for address in order:
            f.write('0x%08X,%d\n' % (address, place[address]))
    print('hot_shards: %d profiled functions (%d bytes of guest code, %d samples) in shards 0..%d (%s bytes each); %d other rows moved out' % (
        len(hot), sum(v[1] for v in hot.values()), sum(v[0] for v in hot.values()), n - 1, ', '.join(str(x) for x in load), moved))
    return 0


if __name__ == '__main__':
    sys.exit(main())
