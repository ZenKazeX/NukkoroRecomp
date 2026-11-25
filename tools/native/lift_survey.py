#!/usr/bin/env python
"""lift_survey.py - how much of the program lift_host.py can re-express today.

Every function of the function table is offered to the lifter alone (every callee external); it counts as
convertible when the lifter refuses nothing in it. Reported: functions and instructions convertible, in total and
among the functions a coverage file marks as executed, and the refusal reasons ranked by functions blocked (the
first refusal of each function).

  lift_survey.py --dol <main.dol> --functions <functions.csv> [--jump-tables f] [--executed <coverage file>]
                 [--top 40] [--list <out file of convertible addresses>]
"""
import argparse
import collections
import re
import sys

import lift_host


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--dol', required=True)
    p.add_argument('--functions', required=True)
    p.add_argument('--jump-tables')
    p.add_argument('--executed')
    p.add_argument('--top', type=int, default=40)
    p.add_argument('--list')
    a = p.parse_args()
    sizes = lift_host.read_table(a.functions)
    executed = {int(line, 16) for line in open(a.executed) if line.strip()} if a.executed else None
    lifter = lift_host.Lifter(lift_host.Dol(a.dol), sizes, lift_host.read_jump_tables(a.jump_tables), set())
    blocked = collections.Counter()
    blocked_exec = collections.Counter()
    example = {}
    ok = ok_insns = total_insns = ok_exec = ok_exec_insns = exec_total = exec_insns = 0
    convertible = []
    for address in sorted(sizes):
        n = sizes[address] // 4
        if n == 0:
            continue
        total_insns += n
        is_exec = executed is not None and address in executed
        if is_exec:
            exec_total += 1
            exec_insns += n
        try:
            lifter.function(address)
        except lift_host.Refused as error:
            text = str(error)
            key = re.sub(r'^0x[0-9A-F]{8}( \(word [0-9A-F]{8}\))?: ', '', text)
            blocked[key] += 1
            if is_exec:
                blocked_exec[key] += 1
            example.setdefault(key, text[:34])
            continue
        ok += 1
        ok_insns += n
        convertible.append(address)
        if is_exec:
            ok_exec += 1
            ok_exec_insns += n
    total = ok + sum(blocked.values())
    print('functions: %d convertible of %d (%.1f %%); instructions %d of %d (%.1f %%)' % (
        ok, total, 100.0 * ok / total, ok_insns, total_insns, 100.0 * ok_insns / total_insns))
    if executed is not None:
        print('executed functions: %d in the table of %d marked; convertible %d (%.1f %%); instructions %d of %d (%.1f %%)' % (
            exec_total, len(executed), ok_exec, 100.0 * ok_exec / max(exec_total, 1), ok_exec_insns, exec_insns,
            100.0 * ok_exec_insns / max(exec_insns, 1)))
    print('refusals by functions blocked (all / executed):')
    for key, n in blocked.most_common(a.top):
        print('  %6d %6d  %-58s e.g. %s' % (n, blocked_exec.get(key, 0), key, example[key]))
    print('  ... %d distinct reasons in all' % len(blocked))
    if a.list:
        with open(a.list, 'w') as f:
            for address in convertible:
                f.write('%08X\n' % address)
    return 0


if __name__ == '__main__':
    sys.exit(main())
