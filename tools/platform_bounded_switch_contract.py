"""Original-image candidates for a canonical bounded u32 dispatch table.

The exact eight-word sequence compares an unsigned index, skips indices above
the literal bound, constructs a table address, scales by four, loads a target
and branches through CTR. Original table bytes are evidence, never live values.
External entries, relocation, mutation, aliases and target lifetime stay open.
"""
import hashlib
import struct
from pathlib import Path
if __package__:
    from .graphics_lift_inventory import Dol, decode, hx
else:
    from graphics_lift_inventory import Dol, decode, hx


def backed_bytes(image, address, size):
    if address < 0 or address + size > 1 << 32:
        raise ValueError('table interval wraps')
    result = bytearray()
    while size:
        section = next((s for s in image.sections if s['address'] <= address < s['address'] + s['size']), None)
        if section is None:
            raise ValueError('table interval is not entirely original file-backed storage')
        count = min(size, section['address'] + section['size'] - address)
        offset = section['file_offset'] + address - section['address']
        result.extend(image.raw[offset:offset + count])
        address += count
        size -= count
    return bytes(result)


def recognize_switch(raw, compare_site):
    image = Dol(raw)
    if not image.contains_text(compare_site, 32):
        return None
    words = [image.word(compare_site + i * 4) for i in range(8)]
    compare, branch, hi, lo, scale, load, move, invoke = words
    index = (compare >> 16) & 31
    bf = (compare >> 23) & 7
    # u32 cmplwi only: no signed/L=1/record-form reinterpretation.
    if compare >> 26 != 10 or compare & (3 << 21):
        return None
    edge = decode(branch, compare_site + 4)
    if (not edge or edge.get('mnemonic') != 'bc' or edge.get('target') is None or
        edge.get('link') or edge.get('bo') != 12 or edge.get('bi') != bf * 4 + 1):
        return None
    if compare_site <= edge['target'] < compare_site + 32:
        return None  # The rejected arm must not enter the unbounded dispatch.
    table_reg = (hi >> 21) & 31
    if hi >> 26 != 15 or (hi >> 16) & 31 or table_reg == 0 or table_reg == index:
        return None
    if lo >> 26 != 14 or (lo >> 21) & 31 != table_reg or (lo >> 16) & 31 != table_reg:
        return None
    scaled = (scale >> 16) & 31
    expected_scale = 0x5400103A | (index << 21) | (scaled << 16)
    if scale != expected_scale or scaled == table_reg:
        return None
    target_reg = (load >> 21) & 31
    if load != (0x7C00002E | (target_reg << 21) | (table_reg << 16) | (scaled << 11)):
        return None
    if move != (0x7C0903A6 | (target_reg << 21)) or invoke != 0x4E800420:
        return None
    # Scaling preserves all admitted indices (literal u16 bound <= 65535).
    maximum = compare & 65535
    low = lo & 65535
    address = ((hi & 65535) * 65536 + (low - 65536 if low & 32768 else low)) & 0xFFFFFFFF
    try:
        payload = backed_bytes(image, address, (maximum + 1) * 4)
    except ValueError:
        return None
    targets = struct.unpack('>' + 'I' * (maximum + 1), payload)
    return dict(schema='platform_bounded_switch_contract_v1', compare_site=hx(compare_site),
        inputs=dict(dol_sha256=hashlib.sha256(raw).hexdigest(),
            recognizer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            decoder_sha256=hashlib.sha256(Path(__file__).with_name('graphics_lift_inventory.py').read_bytes()).hexdigest()),
        words=[dict(site=hx(compare_site + i * 4),word=hx(w),file_offset=image.offset(compare_site + i * 4)) for i,w in enumerate(words)],
        index_gpr=index, admitted_unsigned_maximum=maximum, default_target=hx(edge['target']),
        dispatch_site=hx(compare_site + 28), original_table_address=hx(address),
        original_table_bytes=len(payload), original_table_sha256=hashlib.sha256(payload).hexdigest(),
        original_entries=[dict(index=i,address=hx(address + 4*i),raw_target=hx(v),
            effective_target=hx(v & 0xFFFFFFFC),original_text_target=image.contains_text(v & 0xFFFFFFFC,4)) for i,v in enumerate(targets)],
        scope=dict(normal_contiguous_sequence_only=True,external_entry_domination_proven=False,
            original_table_is_live_table_proven=False,table_immutability_alias_or_lifetime_proven=False,
            target_execution_or_native_retirement_proven=False,deletion_authorized=False))
