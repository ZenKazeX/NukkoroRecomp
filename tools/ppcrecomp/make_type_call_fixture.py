"""Compile an unchanged original caller with the pending native type-query lowering.

This fixture deliberately does not activate production sites or certify object
metadata lifetime. The independent cast/unwind oracles remain separate.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('generator', 'dol', 'catalog', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument("--reference", action="store_true", help="original reference-cast caller and its continuations")
    parser.add_argument("--functions", type=Path, help="full original function table, required for reference caller")
    args = parser.parse_args()
    if args.reference and args.functions is None:
        parser.error("--reference requires --functions")
    catalog = json.loads(args.catalog.read_text())
    digest = hashlib.sha256(args.dol.read_bytes()).hexdigest()
    if digest != catalog['dol_sha256'] or digest != 'fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af':
        raise ValueError('fixture needs the verified original image and matching catalog')
    entry, end, site = ((0x8007CE74, 0x8007CEB8, 0x8007CE9C) if args.reference else
                        (0x80077494, 0x800774D8, 0x800774B0))
    call = next(c for c in catalog['call_arguments']['calls'] if c['site'] == site)
    assert len(call['contexts']) == 1
    context = call['contexts'][0]
    assert (context['entry'], context['end_exclusive']) == (entry, end)
    arguments = call['arguments']
    target = arguments['r5']
    assert arguments['r4'] == (0 if args.reference else 24) and arguments['r7'] == int(args.reference)
    identity = next(d['identity'] for d in catalog['descriptors'] if d['address'] == target)
    query = next(i for i, q in enumerate(catalog['compiled_queries']['plans']) if q['target_identity'] == identity)
    args.out.mkdir(parents=True, exist_ok=True)
    functions = args.out / 'functions.csv'
    if not args.reference:
        with functions.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.writer(stream)
            writer.writerow(('addr_hex', 'addr', 'name', 'size', 'ins', 'tags', 'strings', 'callees'))
            writer.writerow((hex(entry), entry, 'original_type_flag_caller', end-entry, (end-entry)//4, '', '', ''))
    sites = args.out / 'type-sites.txt'
    sites.write_text(f'site 0x{site:08X} 0x{call["word"]:08X} type_query 0x{query:X} 0x{target:08X}\n')
    command = [str(args.generator.resolve()), '--dol', str(args.dol.resolve()),
               '--functions', str(args.functions.resolve() if args.reference else functions),
               '--out', str(args.out), '--shards', '1', '--no-gap-sweep', '--no-wide-entries',
               '--lift-sites', str(sites)]
    if args.reference:
        only = args.out / 'only.txt'
        only.write_text(f'0x{entry:08X}\n')
        command += ['--only', str(only)]
    subprocess.run(command, check=True)
    # Do not allow a mistaken fixture dependency to make the old cast available.
    generated = '\n'.join(p.read_text() for p in args.out.glob('recomp_*.cpp'))
    assert 'fn_803A1AFC' not in generated
    assert 'call_type_query' in generated
    constants = {'Caller': entry, 'Site': site, 'Cast': 0x803A1AFC, 'Throw': 0x803A2660,
                 'ThrowReturn': 0x803A1D48, 'Target': target, 'Source': arguments['r6'],
                 'Query': query, 'Sda': 0x805EC500}
    (args.out / 'type_call_fixture.h').write_text('#pragma once\n' + ''.join(
        f'inline constexpr unsigned k{name} = 0x{value:08X}u;\n' for name, value in constants.items()))
    print('Generated unchanged original ' + ('reference-cast' if args.reference else 'flag') +
          ' caller; old cast absent from fixture code.')


if __name__ == '__main__':
    main()
