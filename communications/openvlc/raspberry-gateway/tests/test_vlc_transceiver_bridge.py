import unittest

from vlc_pacing import advance_paced_deadline


class PacingDeadlineTest(unittest.TestCase):
    def test_first_frame_anchors_from_completion(self):
        self.assertEqual(advance_paced_deadline(0, 100, 8), 108)

    def test_small_linux_delay_does_not_accumulate(self):
        deadline = advance_paced_deadline(0, 100, 8)
        deadline = advance_paced_deadline(deadline, 109, 8)
        deadline = advance_paced_deadline(deadline, 117, 8)
        self.assertEqual(deadline, 124)

    def test_missed_slot_reanchors_without_catch_up(self):
        self.assertEqual(advance_paced_deadline(108, 116, 8), 124)

    def test_disabled_pacing_has_no_deadline(self):
        self.assertEqual(advance_paced_deadline(123, 456, 0), 0)


if __name__ == "__main__":
    unittest.main()
