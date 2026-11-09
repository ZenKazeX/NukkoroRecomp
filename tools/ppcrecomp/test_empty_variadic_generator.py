"""Regressions for actual empty-leaf generator output and refusal boundaries."""
from pathlib import Path
import struct
import subprocess


def check(generator: Path, root: Path):
    work = root / 'empty-variadic'
    work.mkdir()
    preserve = [0x9421FF90, 0x40860024, 0xD8210028, 0xD8410030, 0xD8610038,
                0xD8810040, 0xD8A10048, 0xD8C10050, 0xD8E10058, 0xD9010060,
                0x90610008, 0x9081000C, 0x90A10010, 0x90C10014, 0x90E10018,
                0x9101001C, 0x91210020, 0x91410024, 0x38210070, 0x4E800020]
    zero = preserve[:11] + [0x38600000] + preserve[11:]
    va_list = [0x9421FF80] + preserve[1:10] + [0x39610088, 0x38010008, 0x3D800100] + \
        preserve[10:18] + [0x91810068, 0x9161006C, 0x90010070, 0x38210080, 0x4E800020]
    base = 0x80003100
    entries = [base, base + len(preserve)*4, base + (len(preserve)+len(zero))*4]
    words = preserve + zero + va_list
    caller = base + len(words)*4
    words += [0x7DC802A6]  # mflr r14, then direct calls to each empty body
    for entry in entries:
        site = base + len(words)*4
        words.append(0x48000001 | ((entry-site) & 0x03FFFFFC))
    words += [0x7DC803A6, 0x4E800020]
    raw = bytearray(0x100+len(words)*4)
    for offset, value in [(0, 0x100), (0x48, base), (0x90, len(words)*4), (0xE0, base)]:
        struct.pack_into('>I', raw, offset, value)
    struct.pack_into(f'>{len(words)}I', raw, 0x100, *words)
    image = work / 'input.dol'
    image.write_bytes(raw)
    functions = work / 'functions.csv'
    rows = 'addr_hex,name,size\n' + ''.join(f'{a:08X},leaf_{i},{len(body)*4}\n'
        for i, (a, body) in enumerate(zip(entries, (preserve, zero, va_list)))) + f'{caller:08X},caller,24\n'
    functions.write_text(rows)
    leaves = work / 'leaves.txt'
    listed = ''.join(f'0x{a:08X}\n' for a in entries)
    leaves.write_text(listed)

    def run(name, option=True, good=True, extra=()):
        out = work / name
        out.mkdir(exist_ok=True)
        sentinel = out / 'existing.txt'
        if not good:
            sentinel.write_text('must survive rejected generation')
        before = {p.name: p.read_bytes() for p in out.iterdir()}
        command = [str(generator.resolve()), '--dol', str(image), '--functions', str(functions),
                   '--out', str(out), '--shards', '1', '--no-gap-sweep', '--no-wide-entries']
        if option:
            command += ['--empty-variadic-leaves', str(leaves)]
        result = subprocess.run(command+list(extra), capture_output=True, text=True)
        assert (result.returncode == 0) == good, result.stdout+result.stderr
        if not good:
            assert before == {p.name: p.read_bytes() for p in out.iterdir()}, 'rejection changed output'
        return out

    plain = run('plain', option=False)
    leaves.write_text('# no entries\n')
    empty = run('empty')
    for p in plain.glob('recomp_*'):
        if p.suffix in ('.cpp', '.h'):
            assert p.read_bytes() == (empty / p.name).read_bytes(), 'empty selection changed code'
    leaves.write_text(listed)
    lowered = run('lowered')
    shard = (lowered / 'recomp_000.cpp').read_text()
    table = (lowered / 'recomp_table.cpp').read_text()
    header = (lowered / 'recomp_decls.h').read_text()
    for entry in entries:
        assert f'fn_{entry:08X}' not in shard+table+header, 'old body or range survived'
        assert f'void empty_variadic_{entry:08X}(Cpu& c)' in shard
        assert f'empty_variadic_{entry:08X}(c)' in shard, 'direct caller still invokes old body'
        assert f'c.try_hle(0x{entry:08X}u) || c.lift_site_cut(0x{entry:08X}u)' in shard
    assert 'kLiftSiteCount = 3;' in table and 'kRangeCount = 1;' in table
    assert 'c.gpr[3] = 0u;' in table
    for statement in ('c.gpr[11] = c.gpr[1] + 8u;', 'c.gpr[0] = c.gpr[1] - 120u;',
                      'c.gpr[12] = 0x1000000u;'):
        assert statement in table
    assert table.count('LiftSiteContinuation::Prepared,') == 3
    assert 'return LiftSiteContinuation' not in table
    assert '#if !defined(GEKKO_SHIPPING)\nconst u32 kLiftSiteWords[]' in table
    for name, bad in [('duplicate', listed+listed.splitlines()[0]+'\n'), ('extra', listed+'DEADBEEF\n'),
                      ('unaligned', f'{base+1:08X}\n'), ('invalid', 'xyz\n'),
                      ('extra-field', f'{base:08X} zero\n')]:
        leaves.write_text(bad)
        run(name, good=False)
    leaves.unlink()
    run('missing', good=False)
    leaves.write_text(listed)
    changed = bytearray(raw)
    struct.pack_into('>I', changed, 0x100+len(preserve)*4+44, 0x38600001)  # return 1, not zero
    image.write_bytes(changed)
    run('changed-word', good=False)
    image.write_bytes(raw)
    functions.write_text(rows.replace(f'{base:08X},leaf_0,80', f'{base:08X},leaf_0,84'))
    run('changed-extent', good=False)
    functions.write_text(rows+f'{base+8:08X},overlap,72\n')
    run('overlap', good=False)
    functions.write_text(rows)
    changed = bytearray(raw)
    struct.pack_into('>I', changed, 0x100+caller-base+4, 0x48000001 | ((base+8-caller-4) & 0x03FFFFFC))
    image.write_bytes(changed)
    run('interior-entry', good=False)
    image.write_bytes(raw)
    owner = work / 'owners.txt'
    owner.write_text(f'0x{base:08X}\n')
    run('native-owner', good=False, extra=['--native-owners', str(owner)])
    print('PASS: empty-leaf semantics, direct calls, shared dispatch, body/range retirement; '
          'empty-list stability and 11 invalid-input/ownership cases preserve existing output.')
