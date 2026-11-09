"""Checks of tools/platform/closure_proof.py against addresses whose answer is known from the DOL (docs/agents/axfx-effects.md, Step K).

Run from the source root:  python -B -m unittest discover -s tools/platform -p test_closure_proof.py
(needs ../game/sys/main.dol; skipped without it). The positives are the cases the proof relies on; the negatives make the tool fail
when it stops finding something it must find - in particular 0x80464D30, which a census had called "never entered" and which the
registry init enters at boot, must keep coming back with LIVE places.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
os.chdir(ROOT)

try:
    import dolxref
    HAVE_DOL = os.path.exists(str(dolxref.DOL))
except Exception:  # pragma: no cover
    HAVE_DOL = False


@unittest.skipUnless(HAVE_DOL, "needs the game's main.dol")
class ClosureProofTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from closure_proof import Closure
        cls.c = Closure()

    def sites(self, res, key):
        return {s[0]: s[1] for s in res[key]}

    def test_rx_pipeline_execute_closes(self):
        # five bl sites: two lowered call sites, three in the RwIm3D owners; nothing else names it
        res = self.c.query(0x8049C6C4)
        self.assertEqual(self.sites(res, "bl"),
                         {0x80456F3C: "LOWERED", 0x80460BC8: "LOWERED",
                          0x8049B930: "OWNED", 0x8049BAFC: "OWNED", 0x8049BC90: "OWNED"})
        self.assertEqual(res["branch"], [])
        self.assertEqual(res["mat"], [])
        self.assertEqual(res["data"], [])
        self.assertEqual(self.c.live_places(res), [])

    def test_registry_init_callback_is_live(self):
        # 0x80464D30 is the all-in-one node's init method: a data word (def+0x10) whose table consumer
        # (the definition accessor 0x80464D6C) is live. It must never come back closed.
        res = self.c.query(0x80464D30)
        self.assertEqual(res["bl"], [])
        self.assertEqual([a for a, _ in res["data"]], [0x8056E99C])
        live = self.c.live_places(res)
        self.assertTrue(live, "0x80464D30 must have LIVE places")
        self.assertIn(0x80464D6C, [fa for _addr, cons in res["data"] for (fa, st) in cons if st == "LIVE"])

    def test_node_body_is_a_single_data_word(self):
        res = self.c.query(0x80464AAC)
        self.assertEqual((res["bl"], res["branch"], res["mat"]), ([], [], []))
        self.assertEqual([a for a, _ in res["data"]], [0x8056E990])

    def test_materialisation_is_found(self):
        # pTank render callback: one lis/addi pair, in the node definition init 0x8044960C
        res = self.c.query(0x80449D54)
        self.assertEqual(self.sites(res, "mat"), {0x80449624: "LIVE"})
        self.assertEqual(res["bl"], [])

    def test_bl_sites_are_counted(self):
        # the texture bind has 16 bl sites, none materialised, none a data word
        res = self.c.query(0x8049AC88)
        self.assertEqual(len(res["bl"]), 16)
        self.assertEqual((res["mat"], res["data"]), ([], []))

    def test_plain_branch_is_found(self):
        # reached only by a tail branch (`b`) from 0x800477A4
        res = self.c.query(0x80045FDC)
        self.assertEqual(res["bl"], [])
        self.assertEqual([s[0] for s in res["branch"]], [0x800477A4])

    def test_unreferenced_address_has_nothing(self):
        res = self.c.query(0x80003101)
        self.assertEqual((res["bl"], res["branch"], res["mat"], res["data"]), ([], [], [], []))
        self.assertEqual(self.c.live_places(res), [])

    def test_pruned_set_changes_the_class(self):
        # the same site becomes PRUNED when its function is in the pruned set (how the final build is classified)
        from closure_proof import Closure
        c2 = Closure(pruned={0x8044960C})
        res = c2.query(0x80449D54)
        self.assertEqual(self.sites(res, "mat"), {0x80449624: "PRUNED"})
        self.assertEqual(c2.live_places(res), [])


if __name__ == "__main__":
    unittest.main()
