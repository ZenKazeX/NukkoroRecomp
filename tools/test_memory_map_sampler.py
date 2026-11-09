import ctypes as ct
import unittest

import memory_map_sampler as sampler

MEM_RELEASE, PAGE_READWRITE = 0x8000, 0x04


class WalkTests(unittest.TestCase):
    def test_committed_private_block_is_attributed_to_its_base(self):
        k32 = sampler.k32
        k32.VirtualAlloc.restype = ct.c_void_p
        k32.VirtualAlloc.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32, ct.c_uint32]
        k32.VirtualFree.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32]
        k32.GetCurrentProcess.restype = ct.c_void_p
        reserve, commit = 64 << 20, 40 << 20  # larger than anything else in a test interpreter
        base = k32.VirtualAlloc(None, reserve, sampler.MEM_RESERVE, PAGE_READWRITE)
        self.assertTrue(base)
        try:
            self.assertTrue(k32.VirtualAlloc(base, commit, sampler.MEM_COMMIT, PAGE_READWRITE))
            result = sampler.walk(k32.GetCurrentProcess())
            top = result["largest_private"][0]
            self.assertEqual(int(top["base"], 16), base)
            self.assertEqual(top["committed"], commit)
            self.assertEqual(top["reserved"], reserve - commit)
            self.assertGreaterEqual(result["committed_by_kind"]["private"], commit)
            self.assertTrue(any(name.lower().startswith("python") for name, _, _ in result["image_modules"]))
        finally:
            k32.VirtualFree(base, 0, MEM_RELEASE)

    def test_counters_report_private_commit(self):
        sampler.k32.GetCurrentProcess.restype = ct.c_void_p
        c = sampler.counters(sampler.k32.GetCurrentProcess())
        self.assertGreater(c["private"], 0)
        self.assertGreaterEqual(c["peak_private"], c["private"])


if __name__ == "__main__":
    unittest.main()
