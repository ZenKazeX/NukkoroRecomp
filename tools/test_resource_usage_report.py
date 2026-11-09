import copy
import unittest
from resource_usage_report import HEAPS, summarize


class ResourceReportTests(unittest.TestCase):
    def setUp(self):
        self.session = {"stop": {"reason": 0, "failed": False},
                        "host": {"process_cpu_seconds": 3, "wall_seconds": 10},
                        "host_stats": {"memory": {"working_set_mib": 10,
                            "peak_working_set_mib": 12, "private_mib": 7, "peak_private_mib": 9}}}
        self.row = dict(ns=10, frame=1, local_usage=400, non_local_usage=200, mapped_bytes=50,
                        upload_ring_waits=0, upload_dedicated=0)
        for heap in HEAPS:
            self.row.update({f"{heap}_block_bytes": 100, f"{heap}_allocation_bytes": 90})

    def test_scopes_and_same_sample_slack(self):
        later = dict(self.row, ns=20, frame=2, default_block_bytes=80, default_allocation_bytes=0)
        result = summarize(self.session, [self.row, later])
        self.assertEqual(result["gpu"]["heaps"]["default"]["sampled_max_unused_block_bytes"], 80)
        self.assertEqual(result["gpu"]["sampled_max_local_usage_bytes"], 400)
        self.assertEqual(result["cpu"]["mean_logical_cores"], .3)
        self.assertEqual(result["ram_mib"]["peak_working_set_mib"], 12)

    def test_invalid_traces_are_not_measurements(self):
        for rows in ([], [dict(self.row, mapped_bytes=-1)],
                     [dict(self.row, default_allocation_bytes=101)],
                     [self.row, dict(self.row, ns=9)],
                     [self.row, dict(self.row, frame=0)]):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                summarize(self.session, rows)
        missing = dict(self.row); del missing["local_usage"]
        with self.assertRaises(KeyError): summarize(self.session, [missing])

    def test_invalid_session_is_not_a_resource_gain(self):
        for field, value in (("process_cpu_seconds", float("nan")),
                             ("process_cpu_seconds", -1), ("wall_seconds", 0)):
            session = copy.deepcopy(self.session); session["host"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                summarize(session, [self.row])
        session = copy.deepcopy(self.session); session["stop"]["failed"] = True
        with self.assertRaises(ValueError): summarize(session, [self.row])


if __name__ == "__main__":
    unittest.main()
