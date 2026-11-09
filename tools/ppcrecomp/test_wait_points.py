"""The wait-point classifier (wait_points.classify) on synthetic code: every
class it admits, and every rule that rejects a loop."""
import struct
import unittest

if __package__:
    from . import wait_points
else:
    import wait_points

BASE = 0x80003100
BLR = 0x4E800020


def image(words):
    dol = bytearray(0x100 + 4 * len(words))
    for offset, value in ((0, 0x100), (0x48, BASE), (0x90, 4 * len(words)), (0xE0, BASE)):
        struct.pack_into(">I", dol, offset, value)
    struct.pack_into(">%dI" % len(words), dol, 0x100, *words)
    return wait_points.Image(bytes(dol))


def b(offset):
    """b with a byte offset (negative for a back edge)."""
    return 0x48000000 | (offset & 0x03FFFFFC)


def bc(bo, bi, offset):
    return 0x40000000 | (bo << 21) | (bi << 16) | (offset & 0xFFFC)


BEQ_BACK = lambda offset: bc(12, 2, offset)   # beq cr0
BNE_BACK = lambda offset: bc(4, 2, offset)    # bne cr0
LWZ_R0_R3 = 0x80030000      # lwz r0,0(r3)
CMPLWI_R0_0 = 0x28000000    # cmplwi r0,0
CMPWI_R3_0 = 0x2C030000     # cmpwi r3,0
ADDI_R3_1 = 0x38630001      # addi r3,r3,1
MFTB_R4 = 0x7C8C42E6        # mftb r4
SUBF_R5_R6_R4 = 0x7CA62050  # subf r5,r6,r4
CMPLWI_R5_100 = 0x28050064  # cmplwi r5,100
STW_R0_R4 = 0x90040000      # stw r0,0(r4)
SC = 0x44000002


class ClassifyTests(unittest.TestCase):
    def kind(self, words, head, *edges):
        return wait_points.classify(image(words), BASE + head, [BASE + e for e in edges])

    def test_memory_poll_is_a_proven_wait(self):
        # lwz r0,0(r3); cmplwi r0,0; beq head; blr - only a store elsewhere ends it.
        self.assertEqual(self.kind([LWZ_R0_R3, CMPLWI_R0_0, BEQ_BACK(-8), BLR], 0, 8), "poll")

    def test_time_base_loop_is_a_clock_wait(self):
        # r6 = start (loop-invariant); spin until mftb - start >= 100.
        words = [MFTB_R4, SUBF_R5_R6_R4, CMPLWI_R5_100, bc(12, 0, -12), BLR]  # blt head
        self.assertEqual(self.kind(words, 0, 12), "clock")

    def test_register_only_loop_is_a_spin(self):
        self.assertEqual(self.kind([CMPLWI_R0_0, BNE_BACK(-4), BLR], 0, 4), "spin")

    def test_exitless_loop_is_a_halt(self):
        self.assertEqual(self.kind([b(0)], 0, 0), "halt")

    def test_loop_carried_register_is_not_a_wait(self):
        # addi r3,r3,1 reads the r3 the previous iteration wrote: a counter.
        self.assertIsNone(self.kind([ADDI_R3_1, CMPWI_R3_0, BNE_BACK(-8), BLR], 0, 8))

    def test_calls_and_stores_need_the_runtime_check(self):
        # bl to the blr at +0xC; cmpwi r3,0; beq head.
        self.assertEqual(self.kind([0x4800000D, CMPWI_R3_0, BEQ_BACK(-8), BLR], 0, 8), "progress")
        # lwz r0; stw r0 (an idempotent copy); cmplwi; beq head.
        self.assertEqual(self.kind([LWZ_R0_R3, STW_R0_R4, CMPLWI_R0_0, BEQ_BACK(-12), BLR], 0, 12),
                         "progress")

    def test_side_effects_reject_a_loop(self):
        self.assertIsNone(self.kind([SC, CMPLWI_R0_0, BEQ_BACK(-8), BLR], 0, 8))

    def test_inner_back_edge_rejects_a_loop(self):
        # head: lwz; inner: cmplwi; bne inner; beq head
        words = [LWZ_R0_R3, CMPLWI_R0_0, BNE_BACK(-4), BEQ_BACK(-12), BLR]
        self.assertIsNone(self.kind(words, 0, 12))

    def test_unreachable_back_edge_is_not_a_loop(self):
        # head: cmplwi r0,0; b exit; b head (reached only from outside the region)
        words = [CMPLWI_R0_0, b(8), b(-8), BLR]
        self.assertIsNone(self.kind(words, 0, 8))


if __name__ == "__main__":
    unittest.main()
