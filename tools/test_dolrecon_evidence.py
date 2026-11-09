"""Regression tests for dolrecon's naming-evidence gates (defects found by Astra's reviews 4 and 5, 2026-11-01): the
RTTI table (duplicate records and their conflicts, pure-virtual slots, secondary vtables and thunks), class roles (classroles.assign, returned-object provenance),
symbol spelling (names.symbol), instruction effects (ppc), typed virtual calls and vtable stores (vcalls),
construction / destructor classes (names.construction / dtor_class) and RenderWare callbacks (rwheaders).

Each test is a small synthetic case where an earlier version reported evidence that was not there, or lost it.
Run from source/tools:  python -B test_dolrecon_evidence.py
"""
import struct
import unittest

from dolrecon import boostsp, classroles, corrections, dol, names, ppc, rwheaders, staticstore, strrefs, vcalls

POOL = 0x80510000
TEXT = {POOL: "first string", POOL + 8: "second"}
BASE = 0x80001000
BL = (18 << 26) | 0x100 | 1
BLR = 0x4E800020
BCTRL = 0x4E800421


def lis(rd, v):
    return (15 << 26) | (rd << 21) | (v >> 16)


def addi(rd, ra, v):
    return (14 << 26) | (rd << 21) | (ra << 16) | (v & 0xFFFF)


def mr(ra, rs):
    return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)


class Image:
    def __init__(self, words):
        self.blob = struct.pack(f">{len(words)}I", *words)

    def read(self, address, size):
        return self.blob[address - BASE:address - BASE + size]

    def word(self, address):
        return struct.unpack(">I", self.read(address, 4))[0]

    def section_at_address(self, address):
        return self if 0 <= address - BASE < len(self.blob) else None


def resolved(words):
    refs = strrefs.pooled_refs(Image(words), [(BASE, 4 * len(words))], TEXT, (None, None))
    return {a for _, a in refs.get(BASE, [])}


class PooledStringTests(unittest.TestCase):
    def test_copied_base_is_not_its_first_string(self):
        # lis r3,pool ; mr r31,r3 ; addi r3,r31,8 ; bl : only the argument string is used
        self.assertEqual(resolved([lis(3, POOL), addi(3, 3, 0), mr(31, 3), addi(3, 31, 8), BL, BLR]), {POOL + 8})

    def test_float_store_does_not_use_the_integer_register(self):
        stfs_f3 = (52 << 26) | (3 << 21) | (1 << 16)
        self.assertEqual(resolved([lis(3, POOL), addi(3, 3, 0), stfs_f3, 0]), set())

    def test_direct_argument_is_a_use(self):
        self.assertEqual(resolved([lis(3, POOL), addi(3, 3, 0), BL, BLR]), {POOL})

    def test_base_of_a_load_is_no_use(self):
        lwz = (32 << 26) | (30 << 16) | 4
        self.assertEqual(resolved([lis(30, POOL), addi(30, 30, 0), lwz, 0]), set())

    def test_indirect_call_uses_arguments(self):
        self.assertEqual(resolved([lis(3, POOL), addi(3, 3, 0), BCTRL, BLR]), {POOL})

    def test_in_place_ori_and_self_copy(self):
        ori = (24 << 26) | (3 << 21) | (3 << 16) | (POOL & 0xFFFF)
        self.assertEqual(resolved([(15 << 26) | (3 << 21) | (POOL >> 16), ori, BL, BLR]), {POOL})
        self.assertEqual(resolved([lis(3, POOL), addi(3, 3, 0), mr(3, 3), BL, BLR]), {POOL})

    def test_string_on_one_path_only_is_no_use_at_the_join(self):
        cmpwi = (11 << 26) | (4 << 16)
        beq = (16 << 26) | (12 << 21) | (2 << 16) | 12          # beq +12: around the string load, to the call
        self.assertEqual(resolved([cmpwi, beq, lis(3, POOL), addi(3, 3, 0), BL, BLR]), set())


class CallbackEvidenceTests(unittest.TestCase):
    """rwheaders.scan_calls on instruction words (until 2026-11-01 it read assembly text per label-delimited block)."""
    PROTOS = {"RpWorldForAllClumps": {"params": [("RpWorld*", "world"), ("RpClumpCallBack", "fpCallBack"),
                                                 ("void*", "pData")]}}
    CALLBACKS = {"RpClumpCallBack": {}}
    CALLER, RW, CB = 0x80003000, 0x80003080, 0x800030C0

    def scan(self, body):
        words = body + [bl_to(self.CALLER + 4 * len(body), self.RW), BLR]
        text = bytearray(0x100)
        text[:4 * len(words)] = struct.pack(f">{len(words)}I", *words)
        text[0x80:0x84] = text[0xC0:0xC4] = struct.pack(">I", BLR)
        image = dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, self.CALLER, len(text))], 0, 0,
                             self.CALLER)
        funcs = [(self.CALLER, "caller", 4 * len(words), ".text"), (self.RW, "RpWorldForAllClumps", 4, ".text"),
                 (self.CB, "cb", 4, ".text")]
        return rwheaders.scan_calls(image, funcs, self.PROTOS, self.CALLBACKS, {})

    LOAD = [lis(4, 0x800030C0), addi(4, 4, 0x30C0)]

    def test_callback_in_argument_register_is_found(self):
        self.assertIn("cb", self.scan(self.LOAD))

    def test_indirect_call_clobbers_the_argument(self):
        self.assertNotIn("cb", self.scan(self.LOAD + [BCTRL]))

    def test_lis_ori_and_record_copy(self):
        ori = (24 << 26) | (4 << 21) | (4 << 16) | 0x30C0
        self.assertIn("cb", self.scan([lis(4, 0x800030C0), ori]))
        self.assertIn("cb", self.scan([lis(28, 0x800030C0), addi(28, 28, 0x30C0), mr(4, 28) | 1]))

    def test_lmw_overwrites_the_saved_register(self):
        lmw = (46 << 26) | (28 << 21) | (1 << 16) | 8
        self.assertNotIn("cb", self.scan([lis(28, 0x800030C0), addi(28, 28, 0x30C0), lmw, mr(4, 28)]))


TEXT0, DATA0, SDATA0 = 0x80003000, 0x80400000, 0x80500000


def rtti_image():
    """Two 'Task' records (only the second has a vtable, as in the game); Gadget {dtor, update, 0, f, 0, f, pad}."""
    text = struct.pack(">64I", *([BLR] * 64))
    data = bytearray(0x100)
    data[0:16] = b"Task\0\0\0\0Gadget\0\0"
    words = {0x20: SDATA0, 0x24: 0, 0x28: 0,                                       # Gadget's base list {Task, 0}
             0x40: SDATA0 + 8, 0x44: 0, 0x48: TEXT0, 0x4C: TEXT0 + 4,             # Task vtable (second record)
             0x60: SDATA0 + 16, 0x64: 0, 0x68: TEXT0 + 8, 0x6C: TEXT0 + 12, 0x70: 0, 0x74: TEXT0 + 16, 0x78: 0,
             0x7C: TEXT0 + 20, 0x80: 0}                                           # Gadget vtable + padding
    for off, w in words.items():
        data[off:off + 4] = struct.pack(">I", w)
    sdata = struct.pack(">6I", DATA0, 0, DATA0, 0, DATA0 + 8, DATA0 + 0x20)
    sections = [dol.Section(0, "text", ".text0", 0, TEXT0, len(text)),
                dol.Section(7, "data", ".data0", len(text), DATA0, len(data)),
                dol.Section(8, "data", ".data1", len(text) + len(data), SDATA0, len(sdata))]
    return dol.DolImage(text + bytes(data) + sdata, sections, 0, 0, TEXT0)


class RttiTests(unittest.TestCase):
    OBJECTS = {DATA0 + 0x40, DATA0 + 0x60, DATA0 + 0x84}

    def table(self, objects=OBJECTS):
        return corrections.rtti_classes(rtti_image(), (SDATA0, SDATA0 + 24), objects)

    def test_duplicate_record_takes_the_one_with_a_vtable(self):
        t = self.table()
        self.assertEqual(t["Task"]["record"], SDATA0 + 8)
        self.assertEqual(t["Task"]["methods"], [TEXT0, TEXT0 + 4])
        self.assertEqual(t["Gadget"]["direct_bases"], [("Task", 0)])

    def test_pure_virtual_slots_are_kept_inside_the_object(self):
        self.assertEqual(self.table()["Gadget"]["methods"],
                         [TEXT0 + 8, TEXT0 + 12, None, TEXT0 + 16, None, TEXT0 + 20])

    def test_without_object_bounds_a_zero_ends_the_vtable(self):
        t = self.table(None)
        self.assertEqual(t["Gadget"]["methods"], [TEXT0 + 8, TEXT0 + 12])


def b_to(pc, target):
    return (18 << 26) | ((target - pc) & 0x3FFFFFC)


# mi_image(): text bodies (TEXT0 + offset)
A0, A1, B0, B1, D0, THUNK, VTHUNK, FWD, P1, O0, G0 = (TEXT0 + k for k in (0, 4, 8, 12, 16, 20, 28, 48, 56, 60, 64))
MI_NAMES = ["Alpha", "Beta", "Delta", "Dup", "Two", "Pure", "Gamma", "Omega"]
MI_OBJECTS = {DATA0 + k for k in (0, 0x80, 0x90, 0xA0, 0xD0, 0xE0, 0xF0, 0x100, 0x110, 0x128, 0x130, 0x148)}


def mi_image():
    """Multiple inheritance as CodeWarrior lays it out (census 2026-11-01). Records R0..R8 at SDATA0 + 8k:
      Delta : Alpha (0), Beta (8)  primary {D0, A1}, and in the SAME data object the secondary {Delta, -8, THUNK,
              B1, FWD, VTHUNK} for its Beta subobject: THUNK = addi r3,r3,-8; b D0, VTHUNK = the virtual-base form,
              FWD = addi r3,r3,+8; b B1 (a method forwarding to a member: not a thunk)
      Dup   two records, one without bases and one with {Alpha, 0}: conflicting duplicates
      Two   two primary vtables {A0} and {B0}: conflicting
      Pure  {0, P1, 0, 0}: a leading pure slot, two zero words at the end
      Omega : Beta (8), Gamma (8), unrelated at one offset; its secondary {Omega, -8, G0} has no single subobject"""
    words = [BLR] * 64
    words[5], words[6] = addi(3, 3, -8), b_to(THUNK + 4, D0)
    words[7:12] = [addi(11, 0, 0x40), 0x7D63582E, 0x7C635A14, addi(3, 3, -8), b_to(VTHUNK + 16, D0)]
    words[12], words[13] = addi(3, 3, 8), b_to(FWD + 4, B1)
    text = struct.pack(">64I", *words)
    rec = [SDATA0 + 8 * k for k in range(9)]
    data = bytearray(0x150)
    for k, n in enumerate(MI_NAMES):
        data[8 * k:8 * k + len(n)] = n.encode()
    w = {0x40: rec[0], 0x44: 0, 0x48: rec[1], 0x4C: 8, 0x50: 0,                      # Delta's base list
         0x58: rec[0], 0x5C: 0, 0x60: 0,                                               # the second Dup's
         0x68: rec[1], 0x6C: 8, 0x70: rec[7], 0x74: 8, 0x78: 0,                        # Omega's
         0x80: rec[0], 0x84: 0, 0x88: A0, 0x8C: A1,
         0x90: rec[1], 0x94: 0, 0x98: B0, 0x9C: B1,
         0xA0: rec[2], 0xA4: 0, 0xA8: D0, 0xAC: A1,
         0xB0: rec[2], 0xB4: (-8) & 0xFFFFFFFF, 0xB8: THUNK, 0xBC: B1, 0xC0: FWD, 0xC4: VTHUNK,
         0xD0: rec[3], 0xD4: 0, 0xD8: A0, 0xE0: rec[4], 0xE4: 0, 0xE8: A0,
         0xF0: rec[5], 0xF4: 0, 0xF8: A0, 0x100: rec[5], 0x104: 0, 0x108: B0,
         0x110: rec[6], 0x114: 0, 0x118: 0, 0x11C: P1, 0x120: 0, 0x124: 0,
         0x130: rec[8], 0x134: 0, 0x138: O0, 0x13C: rec[8], 0x140: (-8) & 0xFFFFFFFF, 0x144: G0}
    for off, v in w.items():
        data[off:off + 4] = struct.pack(">I", v)
    name_of_record = [0, 1, 2, 3, 3, 4, 5, 6, 7]                 # index into MI_NAMES; R3 and R4 are both Dup
    base_list = {2: DATA0 + 0x40, 4: DATA0 + 0x58, 8: DATA0 + 0x68}
    sdata = b"".join(struct.pack(">2I", DATA0 + 8 * name_of_record[k], base_list.get(k, 0)) for k in range(9))
    sections = [dol.Section(0, "text", ".text0", 0, TEXT0, len(text)),
                dol.Section(7, "data", ".data0", len(text), DATA0, len(data)),
                dol.Section(8, "data", ".data1", len(text) + len(data), SDATA0, len(sdata))]
    return dol.DolImage(text + bytes(data) + sdata, sections, 0, 0, TEXT0)


class MultipleInheritanceTests(unittest.TestCase):
    """Astra answer 5 item 8: duplicates abstain on conflict, secondary vtables count, thunks are not their targets."""

    def setUp(self):
        self.t = corrections.rtti_classes(mi_image(), (SDATA0, SDATA0 + 72), MI_OBJECTS)

    def test_record_names(self):
        self.assertEqual([corrections.rtti_instances(mi_image(), (SDATA0, SDATA0 + 72), MI_OBJECTS)["name"][SDATA0 + 8 * k]
                          for k in range(9)], ["Alpha", "Beta", "Delta", "Dup", "Dup", "Two", "Pure", "Gamma", "Omega"])

    def test_conflicting_duplicates_abstain(self):
        self.assertNotIn("Dup", self.t)
        self.assertIn("base lists", self.t.conflicts["Dup"])
        self.assertNotIn("Two", self.t)
        self.assertIn("primary vtables", self.t.conflicts["Two"])

    def test_leading_pure_slot_and_trailing_zeros(self):
        self.assertEqual(self.t["Pure"]["methods"], [None, P1])
        self.assertEqual(self.t["Pure"]["trailing_zero_words"], 2)

    def test_secondary_vtable_inside_the_primary_object(self):
        d = self.t["Delta"]
        self.assertEqual(d["methods"], [D0, A1])
        (s,) = d["secondary"]
        self.assertEqual((s["vtable"], s["offset"], s["methods"], s["subobject"]),
                         (DATA0 + 0xB0, -8, [THUNK, B1, FWD, VTHUNK], "Beta"))

    def test_thunks_are_decoded_and_a_forwarder_is_not_one(self):
        th = self.t["Delta"]["secondary"][0]["thunks"]
        self.assertEqual(th, {0: {"adjust": -8, "vbase_field": None, "target": D0},
                              3: {"adjust": -8, "vbase_field": 0x40, "target": D0}})

    def test_thunk_must_be_its_whole_function_and_reach_an_entry(self):
        funcs = {A0: 4, A1: 4, B0: 4, B1: 4, D0: 4, THUNK: 8, VTHUNK: 20, FWD: 8, P1: 4, O0: 4, G0: 4}
        ok = corrections.rtti_classes(mi_image(), (SDATA0, SDATA0 + 72), MI_OBJECTS, funcs)
        self.assertEqual(sorted(ok["Delta"]["secondary"][0]["thunks"]), [0, 3])
        longer = corrections.rtti_classes(mi_image(), (SDATA0, SDATA0 + 72), MI_OBJECTS, funcs | {THUNK: 12})
        self.assertEqual(sorted(longer["Delta"]["secondary"][0]["thunks"]), [3])        # more than the thunk
        no_entry = corrections.rtti_classes(mi_image(), (SDATA0, SDATA0 + 72), MI_OBJECTS,
                                            {k: v for k, v in funcs.items() if k != D0})
        self.assertEqual(no_entry["Delta"]["secondary"][0]["thunks"], {})              # target is no function

    def test_holders_keep_thunk_and_target_apart(self):
        h = corrections.slot_holders(self.t)
        self.assertEqual(h[THUNK], [("thunk", "Delta", "Beta", 0)])
        self.assertEqual(sorted(h[D0]), [("override", "Delta", "Beta", 0), ("primary", "Delta", "Delta", 0),
                                         ("vtarget", "Delta", "Beta", 3)])
        self.assertEqual(corrections.body_classes(h[D0]), {"Delta"})

    def test_virtual_base_thunk_target_makes_no_class_claim(self):
        # VTHUNK adds an offset read at run time: its target may take any subobject's this, so a body reached only
        # through it is no class's (it would otherwise read as Delta's while its own tables say another subobject)
        t = {c: dict(i) for c, i in self.t.items()}
        t["Delta"]["methods"] = [A0, A1]
        h = corrections.slot_holders(t)
        self.assertEqual(sorted(h[D0]), [("override", "Delta", "Beta", 0), ("vtarget", "Delta", "Beta", 3)])
        self.assertEqual(corrections.body_classes([("vtarget", "Delta", "Beta", 3)]), set())
        self.assertEqual(corrections.class_names(t)[0][D0][0], "Delta::Beta_vf0")     # the plain thunk's claim
        self.assertEqual(sorted(h[B1]), [("direct", "Delta", "Beta", 1), ("primary", "Beta", "Beta", 1)])
        self.assertEqual(h[FWD], [("direct", "Delta", "Beta", 2)])

    def test_class_names(self):
        n, stats = corrections.class_names(self.t)
        self.assertEqual(n[B1][0], "Beta::vf1")           # Beta's body, also in Delta's Beta subobject
        self.assertEqual(n[D0][0], "Delta::vf0")
        self.assertEqual(n[A1][0], "Alpha::vf1")
        self.assertNotIn(THUNK, n)
        self.assertNotIn(VTHUNK, n)
        self.assertNotIn(G0, n)                           # Omega's subobject at 8 is Beta or Gamma
        self.assertEqual(stats["secondary_slot_without_subobject_class"], 1)
        self.assertIsNone(self.t["Omega"]["secondary"][0]["subobject"])

    def test_body_also_in_an_unrelated_secondary_slot_is_not_named(self):
        t = {c: dict(i) for c, i in self.t.items()}
        t["Delta"]["secondary"] = [dict(t["Delta"]["secondary"][0], methods=[THUNK, P1, FWD, VTHUNK])]
        self.assertNotIn(P1, corrections.class_names(t)[0])     # Pure's slot 1 and Beta's slot 1: no common class


class ClassRoleTests(unittest.TestCase):
    TABLE = {
        "Task": {"direct_bases": [], "bases": [], "methods": [0x10, 0x11]},
        "Gadget": {"direct_bases": [("Task", 0)], "bases": ["Task"], "methods": [0x20, 0x21, None, 0x23, None, 0x25]},
        "Door": {"direct_bases": [("Gadget", 0)], "bases": ["Gadget", "Task"],
                 "methods": [0x30, 0x21, 0x32, 0x23, 0x34, 0x25]},
        "Mixin": {"direct_bases": [], "bases": [], "methods": [0x40, 0x32]},
        "Side": {"direct_bases": [("Mixin", 0), ("Task", 8)], "bases": ["Mixin", "Task"], "methods": [0x50, 0x51]},
    }

    def names(self):
        out, _, skipped = classroles.assign(self.TABLE)
        return {f: r["name"] for f, r in out.items()}, skipped

    def test_inherited_body_belongs_to_the_class_that_introduced_it(self):
        names, _ = self.names()
        self.assertEqual(names[0x11], "Task__update")
        self.assertEqual(names[0x21], "Gadget__update")
        self.assertEqual(names[0x25], "Gadget__checkRemove")

    def test_body_also_in_a_slot_without_role_is_not_named(self):
        names, skipped = self.names()
        self.assertNotIn(0x32, names)
        self.assertEqual(skipped["body also in another slot or role"], 1)

    def test_base_at_a_nonzero_offset_gives_no_role(self):
        self.assertNotIn(0x51, self.names()[0])

    def test_slot_without_a_dispatcher_is_not_named(self):
        names, _ = self.names()
        self.assertNotIn(0x23, names)
        self.assertNotIn(0x34, names)

    def with_side_secondary(self, methods):
        table = {c: dict(i) for c, i in self.TABLE.items()}
        table["Side"]["secondary"] = [{"vtable": 0x99, "offset": -8, "subobject": "Task", "methods": methods,
                                       "thunks": {}}]
        out, _, skipped = classroles.assign(table)
        return {f: r["name"] for f, r in out.items()}, skipped

    def test_role_body_also_in_an_unrelated_secondary_slot_is_not_named(self):
        names, skipped = self.with_side_secondary([0x25, 0x52])     # checkRemove's body in Task's slot 0
        self.assertNotIn(0x25, names)
        self.assertEqual(skipped["body also in another slot or role"], 2)

    def test_inherited_body_in_a_secondary_vtable_keeps_its_role(self):
        names, _ = self.with_side_secondary([0x52, 0x11])           # Task's update in Side's Task subobject
        self.assertEqual(names[0x11], "Task__update")


def lwz(rd, ra, d):
    return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)


def stw(rs, ra, d):
    return (36 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


MTCTR, VT = 0x7D8903A6, DATA0 + 0x40


def vcall(base, slot):
    return [lwz(12, base, 0x18), lwz(12, 12, 8 + 4 * slot), MTCTR, BCTRL]


def vcall_image():
    """ctor stores A's vtable at this+0x18; m1 calls this->slot 2; m2 walks a list through +0xC (the Task walk's
    shape), so its call is not on this after the first iteration."""
    ctor = [lis(4, VT), addi(4, 4, VT & 0xFFFF), stw(4, 3, 0x18), BLR]
    m1 = [mr(31, 3)] + vcall(31, 2) + [BLR]
    loop = vcall(31, 1) + [lwz(31, 31, 0xC), (11 << 26) | (31 << 16)]
    m2 = [mr(31, 3)] + loop + [(16 << 26) | (4 << 21) | (2 << 16) | ((-4 * len(loop)) & 0xFFFC), BLR]
    code = {TEXT0: ctor, TEXT0 + 0x40: m1, TEXT0 + 0x80: m2, TEXT0 + 0xC0: [BLR]}
    text = bytearray(struct.pack(">64I", *([0] * 64)))
    for a, ws in code.items():
        text[a - TEXT0:a - TEXT0 + 4 * len(ws)] = struct.pack(f">{len(ws)}I", *ws)
    sections = [dol.Section(0, "text", ".text0", 0, TEXT0, len(text)),
                dol.Section(7, "data", ".data0", len(text), DATA0, 0x100)]
    funcs = [(a, f"f{a:X}", 4 * len(ws), ".text") for a, ws in code.items()]
    table = {"A": {"vtable": VT, "methods": [TEXT0 + 0xC0, TEXT0 + 0x80, TEXT0 + 0x40], "direct_bases": [],
                   "bases": []}}
    return dol.DolImage(bytes(text) + bytes(0x100), sections, 0, 0, TEXT0), funcs, table


class VirtualCallTests(unittest.TestCase):
    def test_constructor_gives_the_vptr_offset(self):
        vptr, ctors, _, _ = vcalls.survey(*vcall_image())
        self.assertEqual(vptr["A"], 0x18)
        self.assertEqual(ctors["A"], {TEXT0})

    def test_call_on_this_is_typed(self):
        _, _, calls, _ = vcalls.survey(*vcall_image())
        self.assertEqual([(c["caller"], c["this"], c["slot"], c["base"]) for c in calls], [(TEXT0 + 0x40, "A", 2, "A")])

    def test_register_reassigned_in_a_loop_is_not_this(self):
        _, _, calls, field_calls = vcalls.survey(*vcall_image())
        self.assertNotIn(TEXT0 + 0x80, {c["caller"] for c in calls})


def stores_of(words, op_new=None):
    text = bytearray(4 * 64)
    text[:4 * len(words)] = struct.pack(f">{len(words)}I", *words)
    image = dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text))], 0, 0, TEXT0)
    return [(kind, off) for _, vt, kind, off, _ in
            vcalls.vtable_stores(vcalls.Function(image, TEXT0, 4 * len(words)), {VT}, op_new) if vt]


def bl_to(pc, target):
    return (18 << 26) | ((target - pc) & 0x3FFFFFC) | 1


class ConstructionTests(unittest.TestCase):
    """N2 named 42 functions C__construct that only build a C on the stack (__dynamic_cast's std::bad_cast); N7 named
    431 derived deleting destructors after their base (until 2026-11-01)."""
    LOAD = [lis(4, VT), addi(4, 4, VT & 0xFFFF)]
    ANC = {"Base": {"Base"}, "Derived": {"Derived", "Base"}, "Other": {"Other"}}

    def test_store_kinds(self):
        self.assertEqual(stores_of(self.LOAD + [stw(4, 1, 8), BLR]), [("stack", 8)])
        self.assertEqual(stores_of(self.LOAD + [mr(31, 3), stw(4, 31, 0x18), BLR]), [("this", 0x18)])
        mr_dot = mr(31, 3) | 1                       # mr. r31, r3: a copy that also sets cr0
        new = TEXT0 + 0x100
        code = [bl_to(TEXT0, new), mr_dot] + self.LOAD + [stw(4, 31, 0), BLR]
        self.assertEqual(stores_of(code, new), [("new", 0)])

    def test_stack_temporary_gives_no_construction(self):
        rows = [(0, VT, "stack", 8, "stack")]
        self.assertEqual(names.construction(rows, {VT: "Base"}, {"Base": 0}, self.ANC), (None, None))

    def test_store_at_the_class_vptr_offset_is_its_constructor(self):
        rows = [(0, VT, "this", 4, "this")]
        self.assertEqual(names.construction(rows, {VT: "Base"}, {"Base": 4}, self.ANC), ("ctor", "Base"))
        # a virtual base's vtable at another offset than its own constructors use names nothing
        self.assertEqual(names.construction(rows, {VT: "Base"}, {"Base": 0}, self.ANC), (None, None))

    def test_new_object_is_a_construction(self):
        rows = [(0, VT, "new", 0, 0x100)]
        self.assertEqual(names.construction(rows, {VT: "Base"}, {"Base": 0}, self.ANC)[1], "Base")
        # a vtable stored at another offset of the new object is a member subobject, not the object's class
        self.assertEqual(names.construction(rows, {VT: "Base"}, {"Base": 8}, self.ANC), (None, None))

    def test_final_class_does_not_depend_on_store_order(self):
        cls, off = {VT: "Base", VT + 0x40: "Derived"}, {"Base": 4, "Derived": 4}
        for order in ((VT, VT + 0x40), (VT + 0x40, VT)):
            rows = [(i, v, "this", 4, "this") for i, v in enumerate(order)]
            self.assertEqual(names.construction(rows, cls, off, self.ANC), ("ctor", "Derived"))

    def test_unrelated_or_overwritten_vptr_gives_nothing(self):
        cls = {VT: "Base", VT + 0x40: "Other"}
        rows = [(0, VT, "this", 4, "this"), (1, VT + 0x40, "this", 4, "this")]
        self.assertEqual(names.construction(rows, cls, {"Base": 4, "Other": 4}, self.ANC), (None, None))
        rows = [(0, VT, "this", 4, "this"), (1, None, "this", 4, "this")]
        self.assertEqual(names.construction(rows, cls, {"Base": 4}, self.ANC), (None, None))

    def test_two_objects_of_different_classes_give_nothing(self):
        cls = {VT: "Base", VT + 0x40: "Other"}
        rows = [(0, VT, "new", 0, 0x100), (1, VT + 0x40, "new", 0, 0x200)]
        self.assertEqual(names.construction(rows, cls, {"Base": 0, "Other": 0}, self.ANC), (None, None))

    def test_destructor_belongs_to_the_class_holding_it(self):
        self.assertEqual(names.dtor_class("Base", {"Derived"}, self.ANC), "Derived")
        self.assertEqual(names.dtor_class("Base", {"Base", "Derived"}, self.ANC), "Base")
        self.assertEqual(names.dtor_class("Base", {"Other"}, self.ANC), "Base")
        self.assertEqual(names.dtor_class("Base", set(), self.ANC), "Base")


def x31(xo, d=5, a=6, b=7, rc=0):
    return (31 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc


class PpcEffectTests(unittest.TestCase):
    """dolrecon.ppc: GPR writes per instruction family (Astra answer 5: vcalls.writes() had the indexed update forms,
    paired-single update forms and string loads wrong)."""

    def test_update_forms_write_the_base(self):
        for xo in (183, 247, 439, 695, 759, 567, 631):        # stwux stbux sthux stfsux stfdux lfsux lfdux
            self.assertEqual(ppc.gpr_writes(x31(xo)), (6,), xo)
        for xo in (55, 119, 311, 375):                        # lwzux lbzux lhzux lhaux
            self.assertEqual(set(ppc.gpr_writes(x31(xo))), {5, 6}, xo)
        self.assertEqual(ppc.gpr_writes((4 << 26) | (1 << 21) | (6 << 16) | (38 << 1)), (6,))      # psq_lux
        self.assertEqual(ppc.gpr_writes((37 << 26) | (5 << 21) | (6 << 16)), (6,))                  # stwu

    def test_plain_stores_and_moves(self):
        for xo in (151, 215, 407, 663, 727, 983, 467, 0, 32):  # stwx stbx sthx stfsx stfdx stfiwx mtspr cmp cmpl
            self.assertEqual(ppc.gpr_writes(x31(xo)), (), xo)
        self.assertEqual(ppc.gpr_writes(x31(339)), (5,))      # mfspr writes rD
        self.assertEqual(ppc.gpr_writes((47 << 26) | (28 << 21) | (1 << 16)), ())     # stmw
        self.assertEqual(ppc.gpr_writes((46 << 26) | (28 << 21) | (1 << 16)), (28, 29, 30, 31))   # lmw

    def test_string_loads(self):
        self.assertEqual(ppc.gpr_writes(x31(597, d=30, b=12)), (30, 31, 0))           # lswi 12 bytes wraps
        self.assertEqual(len(ppc.gpr_writes(x31(597, d=0, b=0))), 8)                 # NB 0 = 32 bytes
        self.assertIsNone(ppc.gpr_writes(x31(533)))                                  # lswx: count in XER

    def test_record_and_overflow_forms(self):
        self.assertEqual(ppc.gpr_writes(x31(266 | 0x200, rc=1)), (5,))                # addo.
        self.assertEqual(ppc.gpr_writes(x31(444, rc=1)), (6,))                        # or. writes rA
        self.assertTrue(ppc.is_mr(x31(444, d=3, a=31, b=3, rc=1))[0])                 # mr. is a copy
        self.assertIsNone(ppc.gpr_writes(1 << 26))                                    # undecoded

    def test_branches(self):
        self.assertEqual(ppc.branch(0x48000012, 0x80003000).target, 0x10)            # ba: absolute
        self.assertEqual(ppc.branch(0x4BFFFFFC, 0x80003000).target, 0x80002FFC)      # b -4
        bcl = ppc.branch((16 << 26) | (20 << 21) | 5, 0x80003000)
        self.assertTrue(bcl.link and bcl.always and bcl.target == 0x80003004)
        self.assertEqual(ppc.branch((16 << 26) | (12 << 21) | 0xFFF8 | 2, 0).target, 0xFFFFFFF8)   # bca


def calls_of(words, entries=()):
    """Slots of the typed calls survey() proves in a method body made of words (class A, vptr +0x18)."""
    text = bytearray(4 * 64)
    text[:4 * len(words)] = struct.pack(f">{len(words)}I", *words)
    ctor = [lis(4, VT), addi(4, 4, VT & 0xFFFF), stw(4, 3, 0x18), BLR]
    text[0x80:0x80 + 16] = struct.pack(">4I", *ctor)
    image = dol.DolImage(bytes(text) + bytes(0x100), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text)),
                         dol.Section(7, "data", ".data0", len(text), DATA0, 0x100)], 0, 0, TEXT0)
    funcs = [(TEXT0, "m", 4 * len(words), ".text"), (TEXT0 + 0x80, "ctor", 16, ".text")]
    table = {"A": {"vtable": VT, "methods": [TEXT0, TEXT0 + 0x80, TEXT0 + 0x80], "direct_bases": [], "bases": []}}
    return [c["slot"] for c in vcalls.survey(image, funcs, table, {TEXT0: set(entries)})[2]]


NOP = 0x60000000


class VirtualCallProofTests(unittest.TestCase):
    """vcall_at() was a pattern window (Astra answer 5): a call is proven by def-use inside one block, with r3 still
    the object whose vtable was loaded."""
    LOADS = [lwz(12, 3, 0x18), lwz(12, 12, 0x10)]

    def test_plain_call(self):
        self.assertEqual(calls_of(self.LOADS + [MTCTR, BCTRL, BLR]), [2])

    def test_r12_overwritten_before_mtctr(self):
        self.assertEqual(calls_of(self.LOADS + [addi(12, 0, 0), MTCTR, BCTRL, BLR]), [])

    def test_receiver_not_this_at_the_call(self):
        self.assertEqual(calls_of(self.LOADS + [lwz(3, 3, 4), MTCTR, BCTRL, BLR]), [])

    def test_negative_slot_displacement(self):
        self.assertEqual(calls_of([lwz(12, 3, 0x18), lwz(12, 12, -8), MTCTR, BCTRL, BLR]), [])

    def test_bcl_clobbers_this(self):
        bcl = (16 << 26) | (20 << 21) | 4 | 1                  # bcl 20,0,+4: a call
        self.assertEqual(calls_of([bcl] + self.LOADS + [MTCTR, BCTRL, BLR]), [])

    def test_jump_table_targets_are_edges(self):
        # lis/addi r4 = table; lwzx r0,r4,r5; mtctr r0; bctr - the table holds two addresses of this function
        lwzx = (31 << 26) | (0 << 21) | (4 << 16) | (5 << 11) | (23 << 1)
        words = [lis(4, DATA0), addi(4, 4, DATA0 & 0xFFFF), lwzx, 0x7C0903A6, 0x4E800420, NOP, BLR, NOP, BLR]
        text = bytearray(4 * 64)
        text[:4 * len(words)] = struct.pack(f">{len(words)}I", *words)
        data = struct.pack(">3I", TEXT0 + 0x14, TEXT0 + 0x1C, 0)
        image = dol.DolImage(bytes(text) + data + bytes(0x100 - len(data)),
                             [dol.Section(0, "text", ".text0", 0, TEXT0, len(text)),
                              dol.Section(7, "data", ".data0", len(text), DATA0, 0x100)], 0, 0, TEXT0)
        blocks = vcalls.Function(image, TEXT0, 4 * len(words)).blocks()
        self.assertEqual(blocks[TEXT0][1], [TEXT0 + 0x14, TEXT0 + 0x1C])

    def test_interior_entry_knows_nothing(self):
        # the loads start at an address another function branches to: r3 there need not be this
        self.assertEqual(calls_of([NOP] + self.LOADS + [MTCTR, BCTRL, BLR], entries=[TEXT0 + 4]), [])


class ReturnedConstructionTests(unittest.TestCase):
    """classroles' type proofs: a factory must RETURN the object a constructor of the class built (Astra answer 5)."""
    NEW, CTOR, BAD = TEXT0 + 0x80, TEXT0 + 0x90, TEXT0 + 0xA0

    def built(self, body):
        code = {TEXT0: body, self.NEW: [BLR],
                self.CTOR: [lis(4, VT), addi(4, 4, VT & 0xFFFF), stw(4, 3, 0), BLR],
                self.BAD: [lis(4, VT), addi(4, 4, VT & 0xFFFF), stw(4, 3, 0), addi(3, 0, 0), BLR]}
        text = bytearray(4 * 64)
        for a, ws in code.items():
            text[a - TEXT0:a - TEXT0 + 4 * len(ws)] = struct.pack(f">{len(ws)}I", *ws)
        image = dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text))], 0, 0, TEXT0)
        size = {a: 4 * len(ws) for a, ws in code.items()}
        ctors = {self.CTOR: {"Base"}, self.BAD: {"Base"}}
        return vcalls.returned_construction(image, TEXT0, size, ctors, self.NEW, None, {})

    def call(self, i, target):
        return bl_to(TEXT0 + 4 * i, target)

    def test_new_then_constructor_returned(self):
        self.assertEqual(self.built([self.call(0, self.NEW), self.call(1, self.CTOR), BLR]), {"Base"})

    def test_constructed_temporary_not_returned(self):
        self.assertEqual(self.built([self.call(0, self.NEW), self.call(1, self.CTOR), addi(3, 0, 0), BLR]), set())

    def test_constructor_that_does_not_return_this(self):
        self.assertEqual(self.built([self.call(0, self.NEW), self.call(1, self.BAD), BLR]), set())

    MR_DOT_R0_R3 = mr(0, 3) | 1                                         # mr. r0, r3

    def test_null_checked_factory(self):
        # p = new; if (p) p = Ctor(p); return p   (CodeWarrior's factory, 0x8007DD5C)
        body = [self.call(0, self.NEW), self.MR_DOT_R0_R3, (16 << 26) | (12 << 21) | (2 << 16) | 12,   # beq +12
                self.call(3, self.CTOR), mr(0, 3), mr(3, 0), BLR]
        self.assertEqual(self.built(body), {"Base"})

    def test_constructor_on_one_branch_only(self):
        # the same object returned, but one non-null path skips the constructor (Astra answer 6)
        cmp_r5 = (11 << 26) | (5 << 16)                                 # cmpwi r5, 0: unrelated to the object
        body = [self.call(0, self.NEW), mr(0, 3), cmp_r5, (16 << 26) | (12 << 21) | (2 << 16) | 12,
                self.call(4, self.CTOR), mr(0, 3), mr(3, 0), BLR]
        self.assertEqual(self.built(body), set())


EXTSH_R31 = (31 << 26) | (31 << 21) | (0 << 16) | (922 << 1) | 1          # extsh. r0, r31


def bc(bo, bi, disp):
    return (16 << 26) | (bo << 21) | (bi << 16) | (disp & 0xFFFC)


class ObjectCodeTests(unittest.TestCase):
    """vcalls.object_code for N6 / N7 / sizes (Astra answer 5, item 1: the linear scan proved neither the delete's
    branch condition nor which vtable a slot holds at the end)."""
    NEW, DEL, CTOR = TEXT0 + 0xC0, TEXT0 + 0xD0, TEXT0 + 0xE0

    def facts(self, body):
        text = bytearray(4 * 64)
        text[:4 * len(body)] = struct.pack(f">{len(body)}I", *body)
        for a in (self.NEW, self.DEL, self.CTOR):
            text[a - TEXT0:a - TEXT0 + 4] = struct.pack(">I", BLR)
        image = dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text))], 0, 0, TEXT0)
        return vcalls.object_code(vcalls.Function(image, TEXT0, 4 * len(body)), {VT, VT + 0x40}, self.NEW, self.DEL)

    def call(self, i, target):
        return bl_to(TEXT0 + 4 * i, target)

    STORE = [lis(4, VT), addi(4, 4, VT & 0xFFFF), stw(4, 30, 0)]
    HEAD = [mr(30, 3), mr(31, 4)] + STORE                 # this in r30, the flag in r31

    def dtor(self, test, branch):
        n = len(self.HEAD)
        return self.HEAD + test + [branch, mr(3, 30), self.call(n + len(test) + 2, self.DEL), BLR]

    def test_delete_under_the_flag(self):
        f = self.facts(self.dtor([EXTSH_R31], bc(4, 1, 12)))            # ble over the delete
        self.assertTrue(f["deletes"] and f["flag_tested"] and not f["plain_delete"])

    def test_delete_on_the_wrong_side_of_the_test(self):
        f = self.facts(self.dtor([EXTSH_R31], bc(12, 1, 12)))           # bgt over it: deletes when flag <= 0
        self.assertFalse(f["deletes"])
        self.assertTrue(f["plain_delete"])

    def test_delete_when_the_flag_is_only_nonzero(self):
        f = self.facts(self.dtor([EXTSH_R31], bc(12, 2, 12)))           # beq over it: -1 deletes too (Astra 6)
        self.assertFalse(f["deletes"])
        self.assertTrue(f["plain_delete"])

    def test_conditional_return_is_a_return(self):
        beqlr = 0x4D820020                                              # returns before the vtable store
        f = self.facts([mr(30, 3), (11 << 26) | (3 << 16), beqlr] + self.STORE + [BLR])
        self.assertEqual(len(f["finals"]), 2)
        self.assertNotEqual(f["finals"][0].get(0), f["finals"][1].get(0))

    def test_cr0_overwritten_before_the_branch(self):
        cmpwi_r5 = (11 << 26) | (5 << 16)
        f = self.facts(self.dtor([EXTSH_R31, cmpwi_r5], bc(4, 1, 12)))
        self.assertFalse(f["deletes"])

    def test_unconditional_delete(self):
        n = len(self.HEAD)
        f = self.facts(self.HEAD + [EXTSH_R31, mr(3, 30), self.call(n + 2, self.DEL), BLR])
        self.assertTrue(f["plain_delete"] and not f["deletes"])

    def finals(self, body):
        f = self.facts(body)
        rows = [r for r in f["stores"] if r[2] == "this"]
        return names.final_classes(rows, f["finals"], {VT: "Base", VT + 0x40: "Derived"})

    def test_last_store_names_the_constructor(self):
        unlisted = [lis(5, 0x80400100), addi(5, 5, 0x100), stw(5, 3, 0)]
        derived = [lis(4, VT + 0x40), addi(4, 4, (VT + 0x40) & 0xFFFF), stw(4, 3, 0)]
        self.assertEqual(self.finals(unlisted + derived + [BLR]), ["Derived"])   # unlisted base first: fine
        self.assertIsNone(self.finals(derived + unlisted + [BLR]))               # unlisted derived last

    def test_paths_storing_different_classes(self):
        base = [lis(4, VT), addi(4, 4, VT & 0xFFFF), stw(4, 3, 0)]
        derived = [lis(4, VT + 0x40), addi(4, 4, (VT + 0x40) & 0xFFFF), stw(4, 3, 0)]
        cmpwi = (11 << 26) | (5 << 16)
        body = [cmpwi, bc(12, 2, 4 * (len(base) + 2))] + base + [(18 << 26) | (4 * (len(derived) + 1))] + derived + [BLR]
        self.assertIsNone(self.finals(body))

    def test_allocation_size_and_constructor_call(self):
        body = [addi(3, 0, 0x58), self.call(1, self.NEW), mr(31, 3) | 1, bc(12, 2, 16), self.call(4, TEXT0 + 0xF0),
                mr(3, 31), self.call(6, self.CTOR), BLR]
        f = self.facts(body)
        site = TEXT0 + 4
        self.assertEqual(f["allocs"], {site: 0x58})
        self.assertIn(self.CTOR, f["alloc_calls"][site])


class SymbolNameTests(unittest.TestCase):
    """names.ident() dropped RenderWare's leading underscore (_rpClumpOpen -> rpClumpOpen, 97 names) and collapsed
    the class separator (Task__update -> Task_update) until names.symbol() (2026-11-01)."""

    def test_identifier_is_kept(self):
        for name in ("_rpClumpOpen", "Task__update", "Player__Npc__Sonic__MotionImpl__update"):
            self.assertEqual(names.symbol(name), name)

    def test_other_text_keeps_its_separators(self):
        self.assertEqual(names.symbol("ELEC BAR__create"), "ELEC_BAR__create")
        self.assertEqual(names.symbol("9LIVES__create"), "_9LIVES__create")

    def test_long_names_keep_their_role(self):
        long = "boost__detail__sp_counted_base_impl_" + "SonicteamUSA__System__Camera__CameraManagerUnit_" * 3 + "_dispose"
        n = names.symbol(long.replace("__dispose", "") + "__dispose")
        self.assertTrue(n.endswith("__dispose"))
        self.assertLessEqual(len(n), 96)
        self.assertTrue(names.symbol("Class with spaces " * 10 + "__update").endswith("__update"))


def boost_image(layout, delete=TEXT0 + 0x1F00):
    """Bodies of dolrecon.boostsp laid out at {address: (role body name, {call role: target})}; returns
    (image, funcs) with operator delete at `delete`."""
    text = bytearray(0x2000)
    funcs = [(delete, 4)]
    text[delete - TEXT0:delete - TEXT0 + 4] = struct.pack(">I", BLR)
    for a, (body, targets) in layout.items():
        for k, w in enumerate(boostsp.BODIES[body]):
            if isinstance(w, str):
                w = bl_to(a + 4 * k, delete if w == "__dl__FPv" else targets[w])
            text[a - TEXT0 + 4 * k:a - TEXT0 + 4 * k + 4] = struct.pack(">I", w)
        funcs.append((a, 4 * len(boostsp.BODIES[body])))
    return dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text))], 0, 0, TEXT0), funcs


class BoostSharedPtrTests(unittest.TestCase):
    REL, COPY, ASSIGN, DT = TEXT0 + 0x100, TEXT0 + 0x200, TEXT0 + 0x300, TEXT0 + 0x400
    LAYOUT = {REL: ("release", {}), COPY: ("shared_count_copy", {}),
              ASSIGN: ("shared_count_assign", {"release": REL}),
              DT: ("shared_count_dtor", {"release": REL}),
              TEXT0 + 0x500: ("shared_ptr_copy", {"shared_count_copy": COPY}),
              TEXT0 + 0x600: ("shared_ptr_copy", {"shared_count_copy": COPY}),
              TEXT0 + 0x700: ("shared_ptr_assign", {"shared_count_assign": ASSIGN}),
              TEXT0 + 0x800: ("shared_ptr_dtor", {"shared_count_dtor": DT})}

    def roles(self, layout, patch=None):
        image, funcs = boost_image(layout)
        if patch:
            raw = bytearray(image.raw)
            for a, w in patch.items():
                raw[a - TEXT0:a - TEXT0 + 4] = struct.pack(">I", w)
            image = dol.DolImage(bytes(raw), image.sections, 0, 0, TEXT0)
        return boostsp.recognise(image, funcs, {TEXT0 + 0x1F00: "__dl__FPv"})

    def test_every_role_is_recognised(self):
        r = self.roles(self.LAYOUT)
        self.assertEqual(r[self.REL], "release")
        self.assertEqual(r[TEXT0 + 0x500], "shared_ptr_copy")
        self.assertEqual(r[TEXT0 + 0x600], "shared_ptr_copy")         # one per instantiation
        self.assertEqual(r[TEXT0 + 0x700], "shared_ptr_assign")
        self.assertEqual(r[TEXT0 + 0x800], "shared_ptr_dtor")

    def test_second_primitive_copy_abstains_with_its_callers(self):
        layout = self.LAYOUT | {TEXT0 + 0x900: ("shared_count_copy", {})}
        r = self.roles(layout)
        self.assertNotIn(self.COPY, r)
        self.assertNotIn(TEXT0 + 0x500, r)
        self.assertEqual(r[TEXT0 + 0x700], "shared_ptr_assign")       # other roles unaffected

    def test_copy_body_calling_the_assignment_is_an_assignment(self):
        # the two bodies differ only in the callee: the role follows the shared_count function reached
        layout = self.LAYOUT | {TEXT0 + 0x500: ("shared_ptr_copy", {"shared_count_copy": self.ASSIGN})}
        self.assertEqual(self.roles(layout)[TEXT0 + 0x500], "shared_ptr_assign")

    def test_copy_body_calling_an_unknown_function_is_nothing(self):
        layout = self.LAYOUT | {TEXT0 + 0x500: ("shared_ptr_copy", {"shared_count_copy": TEXT0 + 0x1F00})}
        self.assertNotIn(TEXT0 + 0x500, self.roles(layout))

    def test_copy_taking_the_payload_from_another_offset_is_rejected(self):
        # lwz r0, 0(r4) -> lwz r0, 8(r4): px no longer r.px
        r = self.roles(self.LAYOUT, patch={TEXT0 + 0x500 + 12: 0x80040008})
        self.assertNotIn(TEXT0 + 0x500, r)
        self.assertEqual(r[TEXT0 + 0x600], "shared_ptr_copy")


REGISTER, DTOR, OTHER = TEXT0 + 0x80, TEXT0 + 0x90, TEXT0 + 0xA0


def lbz(rd, ra, d):
    return (34 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)


def stb(rs, ra, d):
    return (38 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


CMPLWI_R0 = 10 << 26                                                    # cmplwi r0, 0
EXTSB_R0 = (31 << 26) | (954 << 1) | 1                                  # extsb. r0, r0


def bne(i, j):
    """bne from word i to word j (BO 4: branch if EQ is clear)."""
    return bc(4, 2, 4 * (j - i))


def registered(words):
    """staticstore.exit_destructors over one registering function made of words (__register_global_object at
    REGISTER): {destructor: [(site offset, object)]}."""
    text = bytearray(4 * 64)
    text[:4 * len(words)] = struct.pack(f">{len(words)}I", *words)
    image = dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text))], 0, 0, TEXT0)
    found, _ = staticstore.exit_destructors(image, REGISTER, {TEXT0}, {TEXT0: 4 * len(words)}, {})
    return {d: sorted((pc - TEXT0, obj) for _, pc, obj, _ in s) for d, s in found.items()}


def runtime_image(change=None):
    """The three runtime bodies of staticstore.RUNTIME_BODIES at TEXT0 + 0x100 / 0x200 / 0x300, with the _ctors
    address 0x8040AC60 and chain offset 0x5E70 filled in; change {(name, word index): word} edits one."""
    ctors, chain = 0x8040AC60, 0x5E70
    text, address, size = bytearray(0x400), {}, {}
    for k, (name, body) in enumerate(staticstore.RUNTIME_BODIES.items()):
        a = TEXT0 + 0x100 * (k + 1)
        address[name], size[a] = a, 4 * len(body)
        for i, w in enumerate(body):
            if isinstance(w, tuple):
                low = {"ha": ((ctors + 0x8000) >> 16) & 0xFFFF, "lo": ctors & 0xFFFF, "chain": chain}[w[0]]
                w = (w[1] << 16) | low
            w = (change or {}).get((name, i), w)
            text[a - TEXT0 + 4 * i:a - TEXT0 + 4 * i + 4] = struct.pack(">I", w)
    image = dol.DolImage(bytes(text), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text))], 0, 0, TEXT0)
    return staticstore.runtime_proven(image, address, size, ctors)


class StaticStorageTests(unittest.TestCase):
    """N14 / N15 (dolrecon.staticstore): the runtime as whole bodies, exit-time destructors, singleton getters."""

    def test_runtime_bodies(self):
        self.assertTrue(runtime_image())

    def test_runtime_with_a_changed_chain_store_fails(self):
        self.assertFalse(runtime_image({("__register_global_object", 4): 0x908D5E70}))     # stw r4 to the chain

    def test_runtime_with_two_chain_offsets_fails(self):
        self.assertFalse(runtime_image({("__destroy_global_chain", 11): 0x806D5E74}))

    def test_runtime_building_another_ctors_address_fails(self):
        self.assertFalse(runtime_image({("__init_cpp", 5): 0x3803AC64}))

    def test_runtime_with_another_instruction_fails(self):
        self.assertFalse(runtime_image({("__init_cpp", 7): NOP}))                         # was b +4

    def test_constant_destructor_argument(self):
        words = [lis(4, DTOR), addi(4, 4, DTOR & 0xFFFF), bl_to(TEXT0 + 8, REGISTER), BLR]
        self.assertEqual(registered(words), {DTOR: [(8, None)]})

    def test_registered_object_is_recorded(self):
        words = [lis(3, DATA0), addi(3, 3, 0x10), lis(4, DTOR), addi(4, 4, DTOR & 0xFFFF),
                 bl_to(TEXT0 + 16, REGISTER), BLR]
        self.assertEqual(registered(words), {DTOR: [(16, DATA0 + 0x10)]})

    def test_call_in_between_clobbers_the_argument(self):
        words = [lis(4, DTOR), addi(4, 4, DTOR & 0xFFFF), bl_to(TEXT0 + 8, OTHER), bl_to(TEXT0 + 12, REGISTER), BLR]
        self.assertEqual(registered(words), {})

    OBJ, CACHE, GUARD, OTHER_OBJ, CTOR, WRITER = (DATA0 + 0x10, DATA0 + 0x20, DATA0 + 0x28, DATA0 + 0x30,
                                                   TEXT0 + 0xB0, TEXT0 + 0x100)

    def getter_words(self, publish_before=False, guard=None, unguarded_return=False):
        """CodeWarrior's function-local static getter (fn_80009208 in PAL): cache test, guard test, construct,
        register, publish the guard, store the cache, return the cache."""
        o, c, g = self.OBJ, self.CACHE, guard or self.GUARD
        head = [lis(6, c), lwz(0, 6, c & 0xFFFF), CMPLWI_R0, bne(3, 21),
                lis(6, self.GUARD), lbz(0, 6, self.GUARD & 0xFFFF), EXTSB_R0, bne(7, 21 if unguarded_return else 17)]
        build = [lis(3, o), addi(3, 3, o & 0xFFFF), None, lis(4, DTOR), addi(4, 4, DTOR & 0xFFFF), None]
        publish = [addi(0, 0, 1), lis(6, g), stb(0, 6, g & 0xFFFF)]
        tail = [lis(5, o), addi(0, 5, o & 0xFFFF), lis(6, c), stw(0, 6, c & 0xFFFF),
                lis(6, c), lwz(3, 6, c & 0xFFFF), BLR]
        w = head + (publish + build if publish_before else build + publish) + tail
        ctor_at, reg_at = (13, 16) if publish_before else (10, 13)
        w[ctor_at] = bl_to(TEXT0 + 4 * ctor_at, self.CTOR)
        w[reg_at] = bl_to(TEXT0 + 4 * reg_at, REGISTER)
        return w

    def image_of(self, words, writer=()):
        text = bytearray(4 * 128)
        text[:4 * len(words)] = struct.pack(f">{len(words)}I", *words)
        text[0xB0:0xB4] = struct.pack(">I", BLR)                       # CTOR: returns this
        text[0x100:0x100 + 4 * len(writer)] = struct.pack(f">{len(writer)}I", *writer)
        return dol.DolImage(bytes(text) + bytes(0x40), [dol.Section(0, "text", ".text0", 0, TEXT0, len(text)),
                            dol.Section(7, "data", ".data0", len(text), DATA0, 0x40)], 0, 0, TEXT0)

    def getter(self, words, receivers=(CTOR,)):
        return staticstore.singleton(self.image_of(words), TEXT0, 4 * len(words), (), REGISTER, {}, set(receivers))

    def test_guarded_getter(self):
        self.assertEqual(self.getter(self.getter_words()), (self.OBJ, DTOR, self.CACHE, frozenset({self.CTOR})))

    def test_unguarded_registration_is_no_singleton(self):
        o = self.OBJ
        words = [lis(3, o), addi(3, 3, o & 0xFFFF), bl_to(TEXT0 + 8, self.CTOR),
                 lis(4, DTOR), addi(4, 4, DTOR & 0xFFFF), bl_to(TEXT0 + 20, REGISTER),
                 lis(3, o), addi(3, 3, o & 0xFFFF), BLR]
        self.assertIsNone(self.getter(words))

    def test_guard_published_before_construction(self):
        self.assertIsNone(self.getter(self.getter_words(publish_before=True)))

    def test_wrong_guard_address(self):
        self.assertIsNone(self.getter(self.getter_words(guard=self.GUARD + 4)))

    def test_cache_returned_where_it_may_still_be_zero(self):
        self.assertIsNone(self.getter(self.getter_words(unguarded_return=True)))

    def test_constructor_not_returning_this(self):
        self.assertIsNone(self.getter(self.getter_words(), receivers=()))

    def cache_ok(self, writer, cache=None, in_bss=False):
        cache = cache or self.CACHE
        image = self.image_of(self.getter_words(), writer)
        funcs = [TEXT0, self.WRITER]
        size = {TEXT0: 4 * 24, self.WRITER: 4 * max(len(writer), 1)}
        index = staticstore.store_index(image, funcs, size)
        return staticstore.cache_holds_only(image, index, size, {}, cache, self.OBJ, {}, in_bss)

    def test_cache_written_only_with_the_object(self):
        self.assertTrue(self.cache_ok([BLR]))

    def test_cache_written_elsewhere_with_another_value(self):
        o, c = self.OTHER_OBJ, self.CACHE
        self.assertFalse(self.cache_ok([lis(5, o), addi(0, 5, o & 0xFFFF), lis(6, c), stw(0, 6, c & 0xFFFF), BLR]))

    def test_lis_ori_writer_is_seen(self):
        c = 0x80408000                                           # low half >= 0x8000: lis @h differs from @ha
        ori = (24 << 26) | (6 << 21) | (6 << 16) | 0x8000
        writer = [addi(0, 0, 5), (15 << 26) | (6 << 21) | 0x8040, ori, stw(0, 6, 0), BLR]
        self.assertFalse(self.cache_ok(writer, cache=c, in_bss=True))
        self.assertTrue(self.cache_ok([BLR], cache=c, in_bss=True))

    def test_indexed_store_near_the_cache_is_unprovable(self):
        c = self.CACHE
        stwx = (31 << 26) | (6 << 16) | (7 << 11) | (151 << 1)
        self.assertFalse(self.cache_ok([lis(6, c), addi(6, 6, c & 0xFFFF), stwx, BLR]))

    def test_unmapped_cache_outside_bss_is_unprovable(self):
        self.assertFalse(self.cache_ok([BLR], cache=0x80500000, in_bss=False))


if __name__ == "__main__":
    unittest.main()
