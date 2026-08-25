"""Tests for the node model, config migration and diagnostic rules.

The COMP lines below are verbatim bench output, not synthesised: the rules are
meant to recognise what the hardware actually prints.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from openvlc_panel import diagnostics  # noqa: E402
from openvlc_panel.config import Config, _from_v1  # noqa: E402
from openvlc_panel.model import ROLE_TRANSCEIVER  # noqa: E402
from openvlc_panel.stats import parse_line  # noqa: E402

# New RX board, 2026-08-21: every frame attempted, none delivered.
LINE_NEW_BOARD = (
    "[stm32] COMP ep=1385895 bp=116 sp=116 okp=0 gpm=8 r07=0 r811=7574 "
    "gd=211 lp=0 hc=33 t0=22 t1=41 tn=33 trq=0 thr=2606 seen=250 ok=0 "
    "crc=151 sync=99 hq=48 hd=0 rp=34014 rd=12 hwo=1 ovf=1 du=9795 dm=10162 "
    "lock=975 ss=1 m=19 pe=5 ps=-3 fc=151 sc=0 jit=0.0"
)

# Healthy v2 burst path: 125 fps in, ~124 delivered.
LINE_HEALTHY = (
    "[stm32] COMP ep=1200000 bp=125 sp=125 okp=124 gd=0 hc=32 t0=32 t1=32 "
    "tn=32 thr=1923 seen=5000 ok=4985 crc=0 sync=15 hq=5000 hd=0 rp=480 rd=0 "
    "hwo=1 ovf=0 du=4500 dm=5000 ss=0 ps=0 sc=90 jit=0.0"
)

# v3 streaming, failures all before the CRC.
LINE_FRAMING_FAIL = (
    "[stm32] COMP bp=0 sp=125 okp=122 hc=32 thr=1923 seen=5748 ok=5619 "
    "crc=0 sync=129 rd=0 ovf=0 du=0"
)

# Reference decoder, failures all at the CRC.
LINE_PAYLOAD_FAIL = (
    "[stm32] COMP bp=0 sp=90 okp=83 hc=32 thr=2110 seen=621 ok=603 "
    "crc=17 sync=1 rd=0 ovf=0 du=0"
)


def _findings(line: str) -> dict[str, diagnostics.Finding]:
    _, metrics = parse_line(line)
    return {f.rule_id: f for f in diagnostics.evaluate(metrics)}


class TestDiagnostics(unittest.TestCase):
    def test_new_board_duty_is_fatal(self):
        found = _findings(LINE_NEW_BOARD)
        self.assertIn("duty-fatal", found)
        # 22 + 41 = 63, within tolerance of 2 x 32: the clock is fine.
        self.assertIn("clock is correct", found["duty-warn"].detail)
        # 1T-wide 41 vs 2T-narrow 22+32=54 -> 13 ticks of margin, not 32.
        self.assertIn("13 apart", found["duty-fatal"].detail)

    def test_new_board_reports_seen_never_ok(self):
        found = _findings(LINE_NEW_BOARD)
        self.assertIn("seen-never-ok", found)
        self.assertEqual(found["seen-never-ok"].severity, diagnostics.ERROR)

    def test_ring_pressure_needs_a_previous_line(self):
        # rd/ovf are cumulative. On a single line there is no movement to
        # judge, so the rule stays quiet and the duty finding leads.
        _, metrics = parse_line(LINE_NEW_BOARD)
        findings = diagnostics.evaluate(metrics)
        self.assertNotIn("ring-pressure", {f.rule_id for f in findings})
        self.assertEqual(findings[0].rule_id, "duty-fatal")
        self.assertEqual(diagnostics.worst_severity(findings),
                         diagnostics.ERROR)

    def test_ring_pressure_leads_once_it_is_moving(self):
        _, before = parse_line(LINE_NEW_BOARD)
        _, after = parse_line(LINE_NEW_BOARD.replace("rd=12", "rd=19"))
        findings = diagnostics.evaluate(after, before)
        self.assertEqual(findings[0].rule_id, "ring-pressure",
                         msg="a dropping ring invalidates every other counter "
                             "and must be read first")

    def test_new_board_reports_decode_saturation(self):
        # 9795 us of an 8000 us budget.
        self.assertIn("decode-saturation", _findings(LINE_NEW_BOARD))

    def test_healthy_line_has_no_errors(self):
        _, metrics = parse_line(LINE_HEALTHY)
        findings = diagnostics.evaluate(metrics)
        severities = {f.severity for f in findings}
        self.assertNotIn(diagnostics.ERROR, severities)
        self.assertNotIn(diagnostics.WARN, severities)

    def test_balanced_duty_does_not_fire(self):
        found = _findings(LINE_HEALTHY)
        self.assertNotIn("duty-warn", found)
        self.assertNotIn("duty-fatal", found)

    def test_framing_failures_classified(self):
        found = _findings(LINE_FRAMING_FAIL)
        self.assertIn("fail-sync", found)
        self.assertNotIn("fail-crc", found)

    def test_payload_failures_classified(self):
        found = _findings(LINE_PAYLOAD_FAIL)
        self.assertIn("fail-crc", found)
        self.assertNotIn("fail-sync", found)

    def test_invisible_loss_on_low_sp(self):
        # sp=90 against a 125 fps pace.
        self.assertIn("invisible-loss", _findings(LINE_PAYLOAD_FAIL))

    def test_empty_metrics_is_quiet(self):
        self.assertEqual(diagnostics.evaluate({}), [])


class TestConfigMigration(unittest.TestCase):
    V1 = {
        "rx_pi": {"host": "10.0.0.5", "user": "vlcrx", "password": "p"},
        "tx_pi": {"host": "10.0.0.6", "user": "vlctx"},
        "bbb": {"host": "192.168.7.2", "user": "debian",
                "jump_host": "10.0.0.6"},
        "trx_a": {"host": "10.0.0.11", "user": "vlctrx"},
        "trx_b": {"host": "10.0.0.12", "user": "vlctrx"},
        "trx_a_tun_ip": "192.168.0.1",
        "trx_b_tun_ip": "192.168.0.2",
        "pc_ip": "192.168.50.101",
        "iperf_port": 10001,
        "iperf_rate": "600k",
        "video_camera_node": "legacy",
        "tx_budget": 40,
        "bbb_tx_dir": "~/bbb",
    }

    def test_v1_becomes_five_nodes(self):
        cfg = _from_v1(self.V1)
        ids = [n.node_id for n in cfg.nodes]
        self.assertEqual(sorted(ids), ["a", "b", "bbb", "rx", "tx"])

    def test_v1_preserves_ssh_and_jump(self):
        cfg = _from_v1(self.V1)
        self.assertEqual(cfg.node("rx").ssh.host, "10.0.0.5")
        self.assertEqual(cfg.node("bbb").ssh.jump_host, "10.0.0.6")

    def test_v1_preserves_optical_identity(self):
        cfg = _from_v1(self.V1)
        self.assertEqual(cfg.node("a").optical_addr, 7)
        self.assertEqual(cfg.node("b").optical_addr, 8)
        self.assertEqual(cfg.node("a").tun_ip, "192.168.0.1")

    def test_v1_legacy_camera_maps_to_tx_node(self):
        cfg = _from_v1(self.V1)
        self.assertEqual(cfg.video.camera_node_id, "tx")
        # and reads back in the old vocabulary
        self.assertEqual(cfg.video_camera_node, "legacy")

    def test_v1_per_node_directory_kept(self):
        cfg = _from_v1(self.V1)
        self.assertEqual(cfg.node("bbb").gateway_dir, "~/bbb")

    def test_unconfigured_legacy_nodes_disabled(self):
        cfg = _from_v1({"trx_a": {"host": "10.0.0.11"}})
        self.assertFalse(cfg.node("rx").enabled)
        self.assertTrue(cfg.node("a").enabled)

    def test_round_trip_v2(self):
        cfg = _from_v1(self.V1)
        cfg.add_node()
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "cfg.json")
            import openvlc_panel.config as config_mod
            original = config_mod.CONFIG_PATH
            config_mod.CONFIG_PATH = path
            try:
                cfg.save()
                with open(path, encoding="utf-8") as fh:
                    raw = json.load(fh)
                self.assertEqual(raw["version"], 2)
                back = Config.load(path)
            finally:
                config_mod.CONFIG_PATH = original
        self.assertEqual([n.node_id for n in back.nodes],
                         [n.node_id for n in cfg.nodes])
        self.assertEqual(back.node("bbb").ssh.jump_host, "10.0.0.6")


class TestNodeOperations(unittest.TestCase):
    def test_add_node_does_not_collide(self):
        cfg = Config()
        third = cfg.add_node()
        self.assertEqual(third.node_id, "c")
        self.assertEqual(third.optical_addr, 9)
        self.assertEqual(third.tun_ip, "192.168.0.3")
        self.assertEqual(third.role, ROLE_TRANSCEIVER)

    def test_add_many_nodes_stay_unique(self):
        cfg = Config()
        made = [cfg.add_node() for _ in range(6)]
        addrs = [n.optical_addr for n in made]
        ips = [n.tun_ip for n in made]
        self.assertEqual(len(set(addrs)), len(addrs))
        self.assertEqual(len(set(ips)), len(ips))

    def test_remove_node_cleans_links_and_video(self):
        cfg = Config()
        cfg.video.camera_node_id = "b"
        self.assertTrue(any(l.dst_id == "b" for l in cfg.links))
        cfg.remove_node("b")
        self.assertIsNone(cfg.node("b"))
        self.assertFalse(any(l.src_id == "b" or l.dst_id == "b"
                             for l in cfg.links))
        self.assertEqual(cfg.video.camera_node_id, "")

    def test_default_links_cover_both_directions(self):
        cfg = Config()
        pairs = {(l.src_id, l.dst_id) for l in cfg.links}
        self.assertEqual(pairs, {("a", "b"), ("b", "a")})

    def test_legacy_attributes_still_work(self):
        cfg = Config()
        cfg.trx_a_tun_ip = "10.1.1.1"
        cfg.iperf_rate = "900k"
        cfg.tx_budget = 60
        self.assertEqual(cfg.node("a").tun_ip, "10.1.1.1")
        self.assertEqual(cfg.iperf.rate, "900k")
        self.assertEqual(cfg.link.tx_budget, 60)
        # Device-valued properties hand back the live object.
        cfg.trx_a.host = "10.2.2.2"
        self.assertEqual(cfg.node("a").ssh.host, "10.2.2.2")


# Two consecutive lines from the self-interference run: cumulative counters
# standing still while the link is fully down.
LINE_SELFINT_A = (
    "[stm32] COMP ep=1053451 bp=125 sp=125 okp=0 r811=81151 hc=32 t0=22 t1=43 "
    "tn=32 thr=1923 seen=71900 ok=26618 crc=2185 sync=43097 hd=0 rd=0 ovf=172 "
    "qdrop=2571 du=725"
)
LINE_SELFINT_B = (
    "[stm32] COMP ep=1049864 bp=125 sp=125 okp=0 r811=81219 hc=32 t0=22 t1=42 "
    "tn=32 thr=1923 seen=72402 ok=26618 crc=2185 sync=43599 hd=0 rd=0 ovf=172 "
    "qdrop=2571 du=729"
)


class TestCumulativeCounters(unittest.TestCase):
    """A standing total is not a fault; only a growing one is."""

    def _pair(self, a: str, b: str):
        _, ma = parse_line(a)
        _, mb = parse_line(b)
        return {f.rule_id: f for f in diagnostics.evaluate(mb, ma)}

    def test_frozen_ovf_does_not_report_ring_pressure(self):
        found = self._pair(LINE_SELFINT_A, LINE_SELFINT_B)
        self.assertNotIn("ring-pressure", found)

    def test_frozen_qdrop_does_not_report_overflow(self):
        found = self._pair(LINE_SELFINT_A, LINE_SELFINT_B)
        self.assertNotIn("tx-queue", found)

    def test_growing_ovf_does_report_ring_pressure(self):
        grown = LINE_SELFINT_B.replace("ovf=172", "ovf=180")
        found = self._pair(LINE_SELFINT_A, grown)
        self.assertIn("ring-pressure", found)
        self.assertIn("ovf +8", found["ring-pressure"].detail)

    def test_without_a_previous_line_the_rules_stay_quiet(self):
        _, m = parse_line(LINE_SELFINT_B)
        found = {f.rule_id for f in diagnostics.evaluate(m)}
        self.assertNotIn("ring-pressure", found)
        self.assertNotIn("tx-queue", found)

    def test_counter_reset_is_not_a_delta(self):
        after_reboot = LINE_SELFINT_B.replace("ovf=172", "ovf=0")
        found = self._pair(LINE_SELFINT_B, after_reboot)
        self.assertNotIn("ring-pressure", found)

    def test_self_interference_duty_is_fatal(self):
        found = self._pair(LINE_SELFINT_A, LINE_SELFINT_B)
        self.assertIn("duty-fatal", found)
        # 1T-wide 42 against 2T-narrow 22+32=54 -> 12 ticks, not 32.
        self.assertIn("12 apart", found["duty-fatal"].detail)
        self.assertIn("seen-never-ok", found)


if __name__ == "__main__":
    unittest.main(verbosity=2)
