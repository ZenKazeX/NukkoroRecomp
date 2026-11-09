"""Boost 1.32 shared_ptr / shared_count code as CodeWarrior emits it, recognised by whole bodies (Astra answer 5
item 16: summaries of the shared-pointer copy / assignment the class-role contracts pass through).

Each body is pinned word for word; an external call is written as the role (or runtime name) it must reach, every
other word - internal branches included - as itself. The four shared_count / sp_counted_base primitives are
non-template code: exactly one function may have each body (else that role, and every role calling it, abstains).
A shared_ptr<T> member is emitted once per instantiation and unit: every function with its body is one.

Read from the binary 2026-11-01 (0x8000945C, 0x800097E0, 0x80009E4C, 0x80009408, 0x8007259C, 0x800A8420,
0x800093B8); this = r3, r = r4, a shared_ptr is {T* px, shared_count pn}, a shared_count {sp_counted_base* pi},
an sp_counted_base {vptr, use_count +4, weak_count +8}:
  release              if (--use_count == 0) { this->dispose() (vptr +0xC); if (--weak_count == 0) this->destruct()
                       (vptr +0x10) }   - the dispatcher classroles' sp_counted_base contracts read
  shared_count_copy    pi = r.pi; if (pi) ++pi->use_count
  shared_count_assign  t = r.pi; if (t != pi) { if (t) ++t->use_count; if (pi) release(pi); pi = t } return this
  shared_count_dtor    if (this) { if (pi) release(pi); if (flag > 0) operator delete(this) } return this
  shared_ptr_copy      px = r.px; shared_count_copy(&pn, &r.pn); return this      the payload is shared, the count +1
  shared_ptr_assign    px = r.px; shared_count_assign(&pn, &r.pn); return this
  shared_ptr_dtor      if (this) { shared_count_dtor(&pn, -1); if (flag > 0) operator delete(this) } return this

recognise(image, funcs, names) -> {function: role}; NAMES / EVIDENCE give each role's name and meaning.
"""
from __future__ import annotations

import collections

from . import ppc

RUNTIME = {"__dl__FPv"}          # operator delete, proven by matching (M0)

BODIES = {
    "release": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0x93E1000C, 0x7C7F1B78, 0x80830004, 0x3404FFFF, 0x90030004,
                0x40820038, 0x81830000, 0x818C000C, 0x7D8903A6, 0x4E800421, 0x807F0008, 0x3403FFFF, 0x901F0008,
                0x40820018, 0x7FE3FB78, 0x819F0000, 0x818C0010, 0x7D8903A6, 0x4E800421, 0x80010014, 0x83E1000C,
                0x7C0803A6, 0x38210010, 0x4E800020],
    "shared_count_copy": [0x80040000, 0x90030000, 0x80A30000, 0x28050000, 0x4D820020, 0x80850004, 0x38040001,
                          0x90050004, 0x4E800020],
    "shared_count_assign": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0xBFC10008, 0x7C7E1B78, 0x83E40000, 0x80030000,
                            0x7C1F0040, 0x4182002C, 0x281F0000, 0x41820010, 0x807F0004, 0x38030001, 0x901F0004,
                            0x807E0000, 0x28030000, 0x41820008, "release", 0x93FE0000, 0x7FC3F378, 0xBBC10008,
                            0x80010014, 0x7C0803A6, 0x38210010, 0x4E800020],
    "shared_count_dtor": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0xBFC10008, 0x7C7E1B79, 0x7C9F2378, 0x41820024,
                          0x807E0000, 0x28030000, 0x41820008, "release", 0x7FE00735, 0x4081000C, 0x7FC3F378,
                          "__dl__FPv", 0x7FC3F378, 0xBBC10008, 0x80010014, 0x7C0803A6, 0x38210010, 0x4E800020],
    "shared_ptr_copy": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0x80040000, 0x38840004, 0x93E1000C, 0x7C7F1B78,
                        0x387F0004, 0x901F0000, "shared_count_copy", 0x80010014, 0x7FE3FB78, 0x83E1000C,
                        0x7C0803A6, 0x38210010, 0x4E800020],
    "shared_ptr_assign": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0x80040000, 0x38840004, 0x93E1000C, 0x7C7F1B78,
                          0x387F0004, 0x901F0000, "shared_count_assign", 0x80010014, 0x7FE3FB78, 0x83E1000C,
                          0x7C0803A6, 0x38210010, 0x4E800020],
    "shared_ptr_dtor": [0x9421FFF0, 0x7C0802A6, 0x90010014, 0xBFC10008, 0x7C7E1B79, 0x7C9F2378, 0x41820020,
                        0x387E0004, 0x3880FFFF, "shared_count_dtor", 0x7FE00735, 0x4081000C, 0x7FC3F378,
                        "__dl__FPv", 0x7FC3F378, 0xBBC10008, 0x80010014, 0x7C0803A6, 0x38210010, 0x4E800020],
}
PRIMITIVES = ("release", "shared_count_copy", "shared_count_assign", "shared_count_dtor")
FAMILIES = ("shared_ptr_copy", "shared_ptr_assign", "shared_ptr_dtor")

NAMES = {                                   # role -> (class, member)
    "release": ("boost::detail::sp_counted_base", "release"),
    "shared_count_copy": ("boost::detail::shared_count", "copy_ctor"),
    "shared_count_assign": ("boost::detail::shared_count", "assign"),
    "shared_count_dtor": ("boost::detail::shared_count", "dtor"),
    "shared_ptr_copy": ("boost::shared_ptr", "copy_ctor"),
    "shared_ptr_assign": ("boost::shared_ptr", "assign"),
    "shared_ptr_dtor": ("boost::shared_ptr", "dtor"),
}
EVIDENCE = {
    "release": "--use_count, dispose (vptr +0xC) at 0, then --weak_count, destruct (vptr +0x10) at 0",
    "shared_count_copy": "pi = r.pi; ++pi->use_count when not null",
    "shared_count_assign": "takes r.pi when it differs: ++ its use_count, releases the old pi",
    "shared_count_dtor": "releases pi when not null; operator delete under the flag",
    "shared_ptr_copy": "px = r.px, then shared_count's copy of r.pn",
    "shared_ptr_assign": "px = r.px, then shared_count's assignment of r.pn",
    "shared_ptr_dtor": "shared_count's destructor on pn (flag -1); operator delete under the flag",
}


def matches(image, start, body, roles) -> bool:
    """True when the function at start is body word for word, its external calls (`bl` to a role name) reaching a
    function that has that role."""
    for k, want in enumerate(body):
        w = image.word(start + 4 * k)
        if isinstance(want, str):
            b = ppc.branch(w, start + 4 * k)
            if b is None or b.kind != "b" or not b.link or roles.get(b.target) != want:
                return False
        elif w != want:
            return False
    return True


def recognise(image, funcs, names) -> dict:
    """{function: role} over funcs [(address, size)]; names {address: symbol} supplies the RUNTIME roles."""
    roles = {a: n for a, n in names.items() if n in RUNTIME}
    by_size = collections.defaultdict(list)
    for a, size in funcs:
        by_size[size].append(a)
    for role in PRIMITIVES + FAMILIES:
        body = BODIES[role]
        hits = [a for a in by_size.get(4 * len(body), []) if matches(image, a, body, roles)]
        if role in PRIMITIVES and len(hits) != 1:
            continue                    # a second copy of non-template code: not the layout read above
        for a in hits:
            roles[a] = role
    return {a: r for a, r in roles.items() if r not in RUNTIME}
