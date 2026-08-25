import unittest

from extract_rx_failure_trace import analyze_trace, parse_trace, quantize_interval


class FailureTraceTests(unittest.TestCase):
    def test_parse_complete_trace(self):
        trace = parse_trace(
            """
prefix RXFAIL_BEGIN v=1 id=7 tick_hz=64000000 edges=5 runs=4 status=-4
prefix RXFAIL_DATA id=7 off=0 hex=0028001800280018
prefix RXFAIL_END id=7 runs=4 clipped=0
"""
        )
        self.assertEqual(trace.metadata["id"], 7)
        self.assertEqual(trace.intervals, [40, 24, 40, 24])

    def test_rejects_gap_in_offsets(self):
        with self.assertRaisesRegex(ValueError, "non-contiguous"):
            parse_trace(
                """
RXFAIL_BEGIN v=1 id=1 tick_hz=64000000 edges=2 runs=1 status=-4
RXFAIL_DATA id=1 off=1 hex=0028
RXFAIL_END id=1 runs=1 clipped=0
"""
            )

    def test_reports_truncated_prefix(self):
        with self.assertRaisesRegex(
            ValueError, r"first available RXFAIL_DATA offset is 4224"
        ):
            parse_trace(
                """
RXFAIL_DATA id=1 off=4224 hex=00280018
RXFAIL_END id=1 runs=11834 clipped=0
"""
            )

    def test_quantizer_matches_nominal_cells(self):
        self.assertEqual(quantize_interval(40, 0, 40, 24, 32)[:3], (1, 40, 0))
        self.assertEqual(quantize_interval(56, 1, 40, 24, 32)[:3], (2, 56, 0))

    def test_analysis_builds_histogram(self):
        trace = parse_trace(
            """
RXFAIL_BEGIN v=1 id=2 tick_hz=64000000 edges=5 runs=4 status=-4 t0=40 t1=24 tn=32
RXFAIL_DATA id=2 off=0 hex=0028001800280038
RXFAIL_END id=2 runs=4 clipped=0
"""
        )
        summary, rows = analyze_trace(trace)
        self.assertEqual(summary["cell_histogram"], {"1": 3, "2": 1})
        self.assertEqual(len(rows), 4)


if __name__ == "__main__":
    unittest.main()
