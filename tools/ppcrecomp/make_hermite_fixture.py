"""Generate unchanged original Hermite routines and their compressed-track caller.

This is a reference fixture for numerical, generated-caller and component cost
comparisons. It does not enable production lowerings or prove all consumers.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

DOL_SHA256 = 'fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af'
ENTRIES = (0x804162AC, 0x80416580, 0x8041669C, 0x80416728)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('generator', 'dol', 'functions', 'out'):
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    if hashlib.sha256(args.dol.read_bytes()).hexdigest() != DOL_SHA256:
        raise ValueError('Hermite fixture requires the pinned original DOL')
    args.out.mkdir(parents=True, exist_ok=True)
    only = args.out/'only.txt'
    only.write_text(''.join(f'0x{entry:08X}\n' for entry in ENTRIES))
    subprocess.run([str(args.generator.resolve()), '--dol', str(args.dol.resolve()),
                    '--functions', str(args.functions.resolve()), '--out', str(args.out),
                    '--only', str(only), '--shards', '1', '--no-gap-sweep'], check=True)
    table = (args.out/'recomp_table.cpp').read_text()
    for entry in ENTRIES:
        if f'{{0x{entry:08X}u,' not in table:
            raise ValueError(f'original Hermite fixture lost entry {entry:08X}')
    print('Generated original Hermite callbacks and compressed-track caller; production unchanged.')

if __name__ == '__main__':
    main()
