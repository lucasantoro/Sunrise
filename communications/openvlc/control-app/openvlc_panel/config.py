"""Persistent configuration for the OpenVLC control panel.

Stored as JSON in the user's home (``~/.openvlc_panel.json``). Nothing here is
secret-grade storage; it is a convenience cache for a lab tool.

The on-disk format is versioned. Version 1 was a flat record with one field per
node side (``trx_a``, ``trx_b``, ``trx_a_tun_ip``, ...), which is why the panel
could only ever address two transceivers. Version 2 stores a node *list*; see
``model.py``. Version 1 files are migrated on load and rewritten on the next
save, so an existing bench keeps its settings without being reconfigured.

``Config`` also exposes the version-1 attribute names as properties backed by
the node list. That is deliberate scaffolding: it lets the UI migrate to the
list model tab by tab instead of in one unverifiable sweep. Everything under
"legacy attribute compatibility" is expected to be deleted once app.py stops
using it, and nothing new should be written against it.
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from typing import Any, Optional

from .model import (
    ROLE_RX_BRIDGE,
    ROLE_TRANSCEIVER,
    ROLE_TX_HOST,
    ROLE_TX_OPTICAL,
    DEFAULT_SSH_USER,
    IperfSettings,
    Link,
    LinkSettings,
    Node,
    NodeRegistry,
    PcSettings,
    SshTarget,
    VideoSettings,
    default_links,
    default_nodes,
)

CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".openvlc_panel.json")
CONFIG_VERSION = 2

# app.py still imports this name. SshTarget is the same record.
Device = SshTarget

# Legacy single-purpose nodes keep fixed ids so migration is idempotent.
LEGACY_IDS = {
    "rx_pi": ("rx", "RX Pi", ROLE_RX_BRIDGE, "~/raspberry-gateway"),
    "tx_pi": ("tx", "TX Pi", ROLE_TX_HOST, "~/raspberry-gateway"),
    "bbb": ("bbb", "BeagleBone TX", ROLE_TX_OPTICAL, "~/beaglebone-tx"),
}


@dataclass
class Config:
    nodes: list[Node] = field(default_factory=default_nodes)
    links: list[Link] = field(default_factory=list)
    pc: PcSettings = field(default_factory=PcSettings)
    iperf: IperfSettings = field(default_factory=IperfSettings)
    video: VideoSettings = field(default_factory=VideoSettings)
    link: LinkSettings = field(default_factory=LinkSettings)

    def __post_init__(self) -> None:
        if not self.links:
            self.links = default_links(self.nodes)

    # ---------------------------------------------------------------- lookup

    @property
    def registry(self) -> NodeRegistry:
        return NodeRegistry(self.nodes)

    def node(self, node_id: str) -> Optional[Node]:
        return self.registry.get(node_id)

    def node_or_new(self, node_id: str, **defaults: Any) -> Node:
        """Fetch a node, creating it if the id is unknown.

        Used by migration, where a version-1 file may or may not have had a
        given side configured.
        """

        found = self.node(node_id)
        if found is not None:
            return found
        created = Node(node_id=node_id, **defaults)
        self.nodes.append(created)
        return created

    def add_node(self, role: str = ROLE_TRANSCEIVER) -> Node:
        """Append a node with non-colliding id, address and tun IP."""

        reg = self.registry
        node = Node(
            node_id=reg.unique_id(),
            role=role,
            optical_addr=reg.next_optical_addr(),
            tun_ip=reg.next_tun_ip(),
            ssh=SshTarget(user=DEFAULT_SSH_USER),
        )
        node.label = f"Node {node.node_id.upper()}"
        self.nodes.append(node)
        return node

    def remove_node(self, node_id: str) -> None:
        """Drop a node and every link that referenced it."""

        self.nodes = [n for n in self.nodes if n.node_id != node_id]
        self.links = [l for l in self.links
                      if l.src_id != node_id and l.dst_id != node_id]
        if self.video.camera_node_id == node_id:
            self.video.camera_node_id = ""
        if self.video.sink_node_id == node_id:
            self.video.sink_node_id = ""

    # ------------------------------------------------------------ persistence

    def save(self) -> None:
        payload = {"version": CONFIG_VERSION}
        payload.update(asdict(self))
        with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)

    @classmethod
    def load(cls, path: str = CONFIG_PATH) -> "Config":
        if not os.path.exists(path):
            return cls()
        try:
            with open(path, encoding="utf-8") as fh:
                raw = json.load(fh)
        except (OSError, ValueError):
            return cls()
        if not isinstance(raw, dict):
            return cls()
        if int(raw.get("version", 1)) >= 2:
            return _from_v2(raw)
        return _from_v1(raw)

    # ------------------------------------------ legacy attribute compatibility
    #
    # Scaffolding for the app.py migration. Device-valued properties return the
    # live object, so the settings tab's in-place edits still work.

    def _legacy_node(self, key: str) -> Node:
        node_id, label, role, gateway = LEGACY_IDS[key]
        return self.node_or_new(node_id, label=label, role=role,
                                gateway_dir=gateway, enabled=True)

    @property
    def rx_pi(self) -> SshTarget:
        return self._legacy_node("rx_pi").ssh

    @property
    def tx_pi(self) -> SshTarget:
        return self._legacy_node("tx_pi").ssh

    @property
    def bbb(self) -> SshTarget:
        return self._legacy_node("bbb").ssh

    @property
    def trx_a(self) -> SshTarget:
        return self.node_or_new("a", label="Node A", optical_addr=7).ssh

    @property
    def trx_b(self) -> SshTarget:
        return self.node_or_new("b", label="Node B", optical_addr=8).ssh

    @property
    def trx_a_tun_ip(self) -> str:
        return self.node_or_new("a", label="Node A").tun_ip

    @trx_a_tun_ip.setter
    def trx_a_tun_ip(self, value: str) -> None:
        self.node_or_new("a", label="Node A").tun_ip = value

    @property
    def trx_b_tun_ip(self) -> str:
        return self.node_or_new("b", label="Node B").tun_ip

    @trx_b_tun_ip.setter
    def trx_b_tun_ip(self, value: str) -> None:
        self.node_or_new("b", label="Node B").tun_ip = value

    @property
    def rx_gateway_dir(self) -> str:
        return self._legacy_node("rx_pi").gateway_dir

    @rx_gateway_dir.setter
    def rx_gateway_dir(self, value: str) -> None:
        self._legacy_node("rx_pi").gateway_dir = value

    @property
    def tx_gateway_dir(self) -> str:
        return self._legacy_node("tx_pi").gateway_dir

    @tx_gateway_dir.setter
    def tx_gateway_dir(self, value: str) -> None:
        self._legacy_node("tx_pi").gateway_dir = value

    @property
    def bbb_tx_dir(self) -> str:
        return self._legacy_node("bbb").gateway_dir

    @bbb_tx_dir.setter
    def bbb_tx_dir(self, value: str) -> None:
        self._legacy_node("bbb").gateway_dir = value

    @property
    def video_camera_node(self) -> str:
        """Version 1 stored ``"a" | "b" | "legacy"``; version 2 stores a node id.

        ``"legacy"`` maps to the TX Pi, which is what that value meant.
        """

        node_id = self.video.camera_node_id
        return "legacy" if node_id == "tx" else node_id

    @video_camera_node.setter
    def video_camera_node(self, value: str) -> None:
        self.video.camera_node_id = "tx" if value == "legacy" else value

    @property
    def pc_ip(self) -> str:
        return self.pc.ip

    @pc_ip.setter
    def pc_ip(self, value: str) -> None:
        self.pc.ip = value

    @property
    def vlc_path(self) -> str:
        return self.pc.vlc_path

    @vlc_path.setter
    def vlc_path(self, value: str) -> None:
        self.pc.vlc_path = value

    @property
    def serial_baud(self) -> int:
        return self.link.serial_baud

    @serial_baud.setter
    def serial_baud(self, value: int) -> None:
        self.link.serial_baud = int(value)

    @property
    def tx_budget(self) -> int:
        return self.link.tx_budget

    @tx_budget.setter
    def tx_budget(self, value: int) -> None:
        self.link.tx_budget = int(value)

    @property
    def video_port(self) -> int:
        return self.video.port

    @video_port.setter
    def video_port(self, value: int) -> None:
        self.video.port = int(value)

    @property
    def relay_port(self) -> int:
        return self.video.relay_port

    @relay_port.setter
    def relay_port(self, value: int) -> None:
        self.video.relay_port = int(value)


def _iperf_property(name: str, cast=int):
    def getter(self: Config):
        return getattr(self.iperf, name)

    def setter(self: Config, value) -> None:
        setattr(self.iperf, name, cast(value))

    return property(getter, setter)


# iperf_* were flat scalars in version 1 and are grouped in version 2.
Config.iperf_port = _iperf_property("port")            # type: ignore[attr-defined]
Config.iperf_duration = _iperf_property("duration")    # type: ignore[attr-defined]
Config.iperf_payload = _iperf_property("payload")      # type: ignore[attr-defined]
Config.iperf_rate = _iperf_property("rate", str)       # type: ignore[attr-defined]


# ------------------------------------------------------------------ decoding

def _ssh_from(raw: Any) -> SshTarget:
    if not isinstance(raw, dict):
        return SshTarget()
    fields = SshTarget.__dataclass_fields__
    return SshTarget(**{k: v for k, v in raw.items() if k in fields})


def _dataclass_from(cls, raw: Any):
    if not isinstance(raw, dict):
        return cls()
    fields = cls.__dataclass_fields__
    return cls(**{k: v for k, v in raw.items() if k in fields})


def _node_from(raw: Any) -> Optional[Node]:
    if not isinstance(raw, dict) or not raw.get("node_id"):
        return None
    fields = Node.__dataclass_fields__
    kwargs = {k: v for k, v in raw.items() if k in fields and k != "ssh"}
    return Node(ssh=_ssh_from(raw.get("ssh")), **kwargs)


def _from_v2(raw: dict) -> Config:
    nodes = [n for n in (_node_from(item) for item in raw.get("nodes", []))
             if n is not None]
    links = [_dataclass_from(Link, item) for item in raw.get("links", [])]
    return Config(
        nodes=nodes or default_nodes(),
        links=[l for l in links if l.src_id and l.dst_id],
        pc=_dataclass_from(PcSettings, raw.get("pc")),
        iperf=_dataclass_from(IperfSettings, raw.get("iperf")),
        video=_dataclass_from(VideoSettings, raw.get("video")),
        link=_dataclass_from(LinkSettings, raw.get("link")),
    )


def _from_v1(raw: dict) -> Config:
    """Migrate the flat two-node layout onto the node list."""

    nodes: list[Node] = []

    for key, (node_id, label, role, gateway) in LEGACY_IDS.items():
        ssh = _ssh_from(raw.get(key))
        dir_key = {"rx_pi": "rx_gateway_dir", "tx_pi": "tx_gateway_dir",
                   "bbb": "bbb_tx_dir"}[key]
        nodes.append(Node(
            node_id=node_id, label=label, role=role, ssh=ssh,
            gateway_dir=str(raw.get(dir_key) or gateway),
            enabled=bool(ssh.host), tun_ip="",
        ))

    for node_id, label, addr, ssh_key, ip_key, fallback_ip in (
        ("a", "Node A", 7, "trx_a", "trx_a_tun_ip", "192.168.0.1"),
        ("b", "Node B", 8, "trx_b", "trx_b_tun_ip", "192.168.0.2"),
    ):
        ssh = _ssh_from(raw.get(ssh_key))
        nodes.append(Node(
            node_id=node_id, label=label, role=ROLE_TRANSCEIVER, ssh=ssh,
            tun_ip=str(raw.get(ip_key) or fallback_ip),
            optical_addr=addr, enabled=True,
        ))

    camera = str(raw.get("video_camera_node") or "b")
    cfg = Config(
        nodes=nodes,
        links=[],
        pc=PcSettings(
            ip=str(raw.get("pc_ip") or PcSettings.ip),
            vlc_path=str(raw.get("vlc_path") or PcSettings.vlc_path),
        ),
        iperf=IperfSettings(
            port=int(raw.get("iperf_port") or IperfSettings.port),
            rate=str(raw.get("iperf_rate") or IperfSettings.rate),
            duration=int(raw.get("iperf_duration") or IperfSettings.duration),
            payload=int(raw.get("iperf_payload") or IperfSettings.payload),
        ),
        video=VideoSettings(
            camera_node_id="tx" if camera == "legacy" else camera,
            port=int(raw.get("video_port") or VideoSettings.port),
            relay_port=int(raw.get("relay_port") or VideoSettings.relay_port),
        ),
        link=LinkSettings(
            serial_baud=int(raw.get("serial_baud") or LinkSettings.serial_baud),
            tx_budget=int(raw.get("tx_budget") or LinkSettings.tx_budget),
        ),
    )
    return cfg
