"""Tests for dolrecon.autodecomp_calls (run from tools/:  python -B -m unittest dolrecon.test_autodecomp_calls).

The symbolic part needs no compiler. The compiler facts (CodeWarrior never emits a sibling call from C; a wrapper compiles to a
frame plus `bl`) need the game's compiler and are skipped when it is not installed.
"""
import os
import subprocess
import tempfile
import unittest

from . import autodecomp_calls as ac
from . import match as m

NL = chr(10)
SYMS = {"fn_800091EC": (0x800091EC, 0x20, ".text", "function"), "fn_80010000": (0x80010000, 0x20, ".text", "function")}


def body(text):
    out = []
    for line in text.strip().splitlines():
        op, _, args = line.strip().partition(" ")
        out.append((op, args.strip()))
    return out


WRAPPER2 = """
stwu r1, -0x10(r1)
mflr r0
stw r0, 0x14(r1)
stmw r30, 0x8(r1)
mr r30, r3
mr r31, r5
bl fn_800091EC
mr r4, r31
addi r3, r30, 0xc
bl fn_800091EC
mr r3, r30
lmw r30, 0x8(r1)
lwz r0, 0x14(r1)
mtlr r0
addi r1, r1, 0x10
blr
"""


class SymbolicTests(unittest.TestCase):
    def source(self, text, min_args=0, **kw):
        return ac.CallLeaf("f", body(text), SYMS, lambda a, n: None, min_args=min_args, **kw).c_source()

    def test_two_calls_keep_values_across_the_first(self):
        src = self.source(WRAPPER2)
        self.assertEqual(src.count("fn_800091EC("), 2)
        self.assertIn("return a3;", src)

    def test_argument_count_is_a_search_variable(self):
        text = "stwu r1, -0x10(r1)\nmflr r0\nstw r0, 0x14(r1)\nbl fn_80010000\nlwz r0, 0x14(r1)\nmtlr r0\naddi r1, r1, 0x10\nblr"
        self.assertIn("fn_80010000()", self.source(text, 0))
        self.assertIn("fn_80010000(a3)", self.source(text, 1))
        self.assertIn("fn_80010000(a3, a4)", self.source(text, 2))

    def test_reading_a_clobbered_register_is_refused(self):
        text = "stwu r1, -0x10(r1)\nmflr r0\nstw r0, 0x14(r1)\nbl fn_80010000\nadd r3, r3, r4\nlwz r0, 0x14(r1)\nmtlr r0\naddi r1, r1, 0x10\nblr"
        with self.assertRaises(ac.Unsupported):
            self.source(text, 0)

    def test_branches_and_unknown_callees_are_refused(self):
        with self.assertRaises(ac.Unsupported):
            self.source("stwu r1, -0x10(r1)\nbl fn_missing\nblr")
        with self.assertRaises(ac.Unsupported):
            self.source("stwu r1, -0x10(r1)\nlabel .L_1\nblr")

    def test_stack_locals_become_arrays(self):
        text = NL.join(["stwu r1, -0x20(r1)", "mflr r0", "stw r0, 0x24(r1)", "addi r3, r1, 0x8", "li r0, 5", "stw r0, 0xc(r1)",
                        "bl fn_80010000", "lwz r3, 0xc(r1)", "lwz r0, 0x24(r1)", "mtlr r0", "addi r1, r1, 0x20", "blr"])
        src = self.source(text, 0)
        self.assertIn("int l8[1];", src)
        self.assertIn("int l12[", src)
        self.assertIn("fn_80010000(((int)l8)", src)

    def test_variadic_call_after_crclr(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "crclr cr1eq", "bl fn_80010000",
                        "lwz r0, 0x14(r1)", "mtlr r0", "addi r1, r1, 0x10", "blr"])
        leaf = ac.CallLeaf("f", body(text), SYMS, lambda a, n: None)
        src = leaf.c_source()
        self.assertEqual(leaf.externs["fn_80010000"], "fnv")
        self.assertIn("fn_80010000(a3)", src)
        self.assertIn("(int, ...)", ac.decl("fn_80010000", "fnv"))

    def test_virtual_call_through_ctr(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "lwz r12, 0x0(r3)", "lwz r12, 0xc(r12)",
                        "mtctr r12", "bctrl", "lwz r0, 0x14(r1)", "mtlr r0", "addi r1, r1, 0x10", "blr"])
        src = self.source(text, 1)
        self.assertIn("(int(*)(int))", src)
        self.assertIn("a3", src)

    def test_if_block_becomes_if(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "cmplwi r3, 0x0", "beq .L_1", "bl fn_80010000",
                        "label .L_1:", "lwz r0, 0x14(r1)", "mtlr r0", "addi r1, r1, 0x10", "blr"])
        src = self.source(text, 0)
        self.assertIn("if (((unsigned)a3) != ((unsigned)0)) {", src)
        self.assertIn("fn_80010000(", src)
        self.assertTrue(src.startswith("void "))        # the block's r3 is not readable after the join

    def test_if_else_return_value_in_a_hoisted_variable(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "cmpwi r3, 0x0", "beq .L_1", "li r3, 5", "b .L_2",
                        "label .L_1:", "li r3, 7", "label .L_2:", "lwz r0, 0x14(r1)", "mtlr r0", "addi r1, r1, 0x10", "blr"])
        src = self.source(text, 0)
        self.assertIn("int v0;", src)
        self.assertIn("v0 = 5;", src)
        self.assertIn("v0 = 7;", src)
        self.assertIn("return v0;", src)

    def test_backward_branch_refused(self):
        text = NL.join(["stwu r1, -0x10(r1)", "label .L_1:", "cmpwi r3, 0x0", "bne .L_1", "blr"])
        with self.assertRaises(ac.Unsupported):
            self.source(text, 0)

    def test_new_expression_pattern_needs_hoisting(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "li r3, 8", "bl fn_80010000", "mr. r0, r3", "beq .L_1",
                        "mr r4, r3", "bl fn_800091EC", "mr r0, r3", "label .L_1:", "mr r3, r0", "lwz r0, 0x14(r1)", "mtlr r0",
                        "addi r1, r1, 0x10", "blr"])
        with self.assertRaises(ac.Unsupported):
            self.source(text, 0)
        src = ac.CallLeaf("f", body(text), SYMS, lambda a, n: None, opts=("hoist",)).c_source()
        self.assertIn("int v0;", src)
        self.assertIn("return v0;", src)

    def test_indexed_and_bitfield_instructions(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "lwzx r0, r3, r4", "extrwi r4, r0, 8, 8", "stwx r4, r3, r5",
                        "bl fn_80010000", "lwz r0, 0x14(r1)", "mtlr r0", "addi r1, r1, 0x10", "blr"])
        src = self.source(text, 0)
        self.assertIn(">> 16) & 0xFF", src)
        self.assertIn("(a3 + a4)", src)

    def test_float_arguments_and_nested_calls_are_options(self):
        text = NL.join(["stwu r1, -0x10(r1)", "mflr r0", "stw r0, 0x14(r1)", "bl fn_800091EC", "lfs f1, 0x8(r3)", "li r4, 1",
                        "bl fn_80010000", "lwz r0, 0x14(r1)", "mtlr r0", "addi r1, r1, 0x10", "blr"])
        plain = ac.CallLeaf("f", body(text), SYMS, lambda a, n: None, min_args=0)
        self.assertIn("fn_80010000(", plain.c_source())
        fl = ac.CallLeaf("f", body(text), SYMS, lambda a, n: None, opts=("fargs", "nest"))
        src = fl.c_source()
        self.assertEqual(fl.externs["fn_80010000"], "fn:int,int,float")
        self.assertEqual(ac.decl("fn_80010000", "fn:int,int,float"), "extern int fn_80010000(int, int, float);")
        self.assertIn("fn_80010000(t1, 1, t2)", src)
        nested = ac.nest_single_use(NL.join(["int f(void) {", "    int t3 = fn_80010000(1);", "    return t3;", "}"]))
        self.assertIn("return fn_80010000(1);", nested)

    def test_shapes(self):
        self.assertEqual(ac.shape_of(body(WRAPPER2)), "frame-calls-2")
        self.assertEqual(ac.shape_of(body("b fn_80010000")), "tail-call")
        self.assertEqual(ac.shape_of(body("addi r3, r3, 4\nb fn_80010000")), "thunk-or-tail")
        self.assertEqual(ac.shape_of(body("li r3, 1\nblr")), "leaf-straight")


@unittest.skipUnless(os.path.exists(os.path.join(m.COMPILER_ROOT, "2.6", "mwcceppc.exe")), "CodeWarrior not installed")
class CompilerFacts(unittest.TestCase):
    def compile(self, c):
        with tempfile.TemporaryDirectory() as d:
            src, obj = os.path.join(d, "t.c"), os.path.join(d, "t.o")
            with open(src, "w") as f:
                f.write(c)
            ok, out = m.compile_source(src, "2.6", "-O4,s -use_lmw_stmw on -inline auto -str readonly", obj, [])
            self.assertTrue(ok, out)
            return subprocess.run([m.OBJCOPY.replace("objcopy", "objdump"), "-dr", obj], capture_output=True, text=True).stdout

    def test_no_sibling_call_from_c(self):
        dis = self.compile("extern int g();\nint w(int a) { return g(a); }\n")
        self.assertIn("bl", dis)
        self.assertNotIn("\tb       ", dis)      # no `b g` tail call: such bodies are C++ thunks


if __name__ == "__main__":
    unittest.main()
