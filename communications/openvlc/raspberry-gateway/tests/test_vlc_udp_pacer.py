import unittest
from unittest import mock

from vlc_udp_pacer import DatagramPacer, FixedPacketizer


class FixedPacketizerTests(unittest.TestCase):
    def test_coalesces_partial_datagrams(self):
        packetizer = FixedPacketizer(752)
        self.assertEqual(list(packetizer.feed(b"a" * 188)), [])
        self.assertEqual(list(packetizer.feed(b"b" * 564)), [b"a" * 188 + b"b" * 564])
        self.assertEqual(packetizer.buffer, b"")

    def test_splits_large_datagrams_and_keeps_remainder(self):
        packetizer = FixedPacketizer(376)
        self.assertEqual(list(packetizer.feed(b"x" * 940)), [b"x" * 376, b"x" * 376])
        self.assertEqual(packetizer.buffer, b"x" * 188)

    def test_rejects_non_ts_packet_size(self):
        with self.assertRaises(ValueError):
            FixedPacketizer(500)


class DatagramPacerTests(unittest.TestCase):
    @mock.patch("vlc_udp_pacer.time.sleep")
    @mock.patch("vlc_udp_pacer.time.monotonic_ns")
    def test_late_wakeup_does_not_schedule_catchup_burst(self, monotonic, sleep):
        monotonic.side_effect = [1_000, 20_000]
        pacer = DatagramPacer(rate_bps=8_000, packet_size=1)
        pacer.next_send_ns = 5_000
        pacer.wait()
        sleep.assert_called_once()
        self.assertEqual(pacer.next_send_ns, 1_020_000)


if __name__ == "__main__":
    unittest.main()
