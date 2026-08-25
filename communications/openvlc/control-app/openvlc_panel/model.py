"""Data model for the OpenVLC control panel.

The panel used to hardcode exactly two transceiver nodes (``trx_a``/``trx_b``)
plus a fixed legacy chain, with parallel scalar fields for each one
(``trx_a_tun_ip``, ``trx_b_tun_ip``, ``video_camera_node="a"|"b"|"legacy"``).
Every feature that touched nodes then had to branch on that pair, which is why
adding a third node meant editing the UI rather than the configuration.

Here a node is a record in a list and a link is an ordered pair of node ids.
Nothing in the model knows how many nodes exist.

Optical addressing follows the firmware: each node owns ONE optical address and
a receiver discards frames carrying its own address as source. A destination is
therefore a property of a *link*, not of a node -- which is exactly the part the
old two-node layout got wrong by storing ``src``/``dst`` per side.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator, Optional

# Roles decide which controls a node offers, so the UI can be built from the
# node list instead of from hardcoded tabs.
ROLE_TRANSCEIVER = "transceiver"   # openvlc-transceiver: comparator RX + TX
ROLE_RX_BRIDGE = "rx-bridge"       # legacy receive-only Pi (vlc_rx_bridge)
ROLE_TX_HOST = "tx-host"           # legacy Pi that routes/feeds a BeagleBone
ROLE_TX_OPTICAL = "tx-optical"     # legacy BeagleBone optical transmitter

ROLES = (ROLE_TRANSCEIVER, ROLE_RX_BRIDGE, ROLE_TX_HOST, ROLE_TX_OPTICAL)

#: Login on a stock Raspberry Pi OS image. The bench hosts are named nodeA,
#: nodeB, ... and reached as pi@nodeB, so this is the default that saves
#: typing rather than a placeholder to be replaced.
DEFAULT_SSH_USER = "pi"


@dataclass
class SshTarget:
    """An SSH-reachable machine.

    ``jump_*`` routes through another host, which the BeagleBone needs when it
    is reachable only over the TX Pi's USB gadget network.
    """

    host: str = ""
    port: int = 22
    user: str = ""
    password: str = ""
    jump_host: str = ""
    jump_port: int = 22
    jump_user: str = ""
    jump_password: str = ""

    @property
    def configured(self) -> bool:
        return bool(self.host)


@dataclass
class Node:
    """One lab node.

    ``node_id`` is the stable key used by links, video routing and saved
    settings; the label is display-only and may be renamed freely.
    """

    node_id: str = ""
    label: str = ""
    role: str = ROLE_TRANSCEIVER
    ssh: SshTarget = field(default_factory=SshTarget)
    enabled: bool = True

    # Network presence on the optical LAN.
    tun_dev: str = "tun0"
    tun_ip: str = ""
    tun_prefix: int = 24

    # Optical MAC-equivalent. One per node; a link picks the destination.
    optical_addr: int = 0

    # Remote layout and service name.
    gateway_dir: str = "~/raspberry-gateway"
    service: str = "openvlc-transceiver"

    @property
    def configured(self) -> bool:
        return self.enabled and self.ssh.configured

    @property
    def tun_cidr(self) -> str:
        return f"{self.tun_ip}/{self.tun_prefix}" if self.tun_ip else ""

    def display(self) -> str:
        return self.label or self.node_id or self.ssh.host or "(unnamed)"


@dataclass
class Link:
    """A directed optical hop, ``src_id`` -> ``dst_id``.

    Kept explicit rather than inferred so a bench with more than two nodes can
    describe which pairs actually see each other. Two nodes pointed at each
    other are simply two links.
    """

    src_id: str = ""
    dst_id: str = ""
    label: str = ""

    def display(self) -> str:
        return self.label or f"{self.src_id} -> {self.dst_id}"


@dataclass
class PcSettings:
    """The lab PC running this panel."""

    ip: str = "192.168.50.101"
    vlc_path: str = r"C:\Program Files\VideoLAN\VLC\vlc.exe"


@dataclass
class IperfSettings:
    """Defaults for the performance tab.

    ``protocol`` selects udp/tcp; the two need different flags and the old code
    only ever built UDP command lines.
    """

    protocol: str = "udp"
    port: int = 10001
    rate: str = "600k"
    duration: int = 20
    payload: int = 800
    mss: int = 860          # TCP only: tun MTU 900 - 40 bytes of IPv4+TCP
    parallel: int = 1
    bidirectional: bool = False


@dataclass
class VideoSettings:
    """Video relay settings.

    ``camera_node_id``/``sink_node_id`` are node ids, not the old
    ``"a" | "b" | "legacy"`` enum, so any node can be either end.
    """

    # Defaults to node "b" to match the version-1 behaviour exactly; a fresh
    # config that pointed the camera at nothing would silently break the video
    # tab for anyone upgrading.
    camera_node_id: str = "b"
    sink_node_id: str = ""
    port: int = 5000
    relay_port: int = 5001


@dataclass
class LinkSettings:
    """Physical-layer settings shared by the nodes."""

    serial_baud: int = 2_000_000
    tx_budget: int = 50     # optical budget; must match the RX profile
    mtu: int = 900


def default_nodes() -> list[Node]:
    """Two transceiver nodes matching ``docs/two_transceiver_test.md``.

    Node A is optical address 7 on 192.168.0.1, node B is 8 on 192.168.0.2.
    Hosts are left empty: an unconfigured node is hidden rather than guessed.
    """

    return [
        Node(node_id="a", label="Node A", role=ROLE_TRANSCEIVER,
             ssh=SshTarget(user=DEFAULT_SSH_USER), tun_ip="192.168.0.1",
             optical_addr=7),
        Node(node_id="b", label="Node B", role=ROLE_TRANSCEIVER,
             ssh=SshTarget(user=DEFAULT_SSH_USER), tun_ip="192.168.0.2",
             optical_addr=8),
    ]


def default_links(nodes: list[Node]) -> list[Link]:
    """Every ordered pair of enabled transceivers.

    Correct for the two-node bench and a reasonable starting point beyond it;
    a bench where not all nodes see each other edits the list.
    """

    ids = [n.node_id for n in nodes if n.role == ROLE_TRANSCEIVER]
    return [Link(src_id=a, dst_id=b)
            for a in ids for b in ids if a != b]


class NodeRegistry:
    """Lookup helpers over the node list.

    Every call tolerates unknown ids and returns ``None`` rather than raising,
    because ids come from saved settings and combo boxes that can go stale when
    a node is renamed or removed.
    """

    def __init__(self, nodes: list[Node]):
        self._nodes = nodes

    def __iter__(self) -> Iterator[Node]:
        return iter(self._nodes)

    def __len__(self) -> int:
        return len(self._nodes)

    def get(self, node_id: str) -> Optional[Node]:
        for node in self._nodes:
            if node.node_id == node_id:
                return node
        return None

    def configured(self) -> list[Node]:
        return [n for n in self._nodes if n.configured]

    def by_role(self, role: str, only_configured: bool = True) -> list[Node]:
        pool = self.configured() if only_configured else list(self._nodes)
        return [n for n in pool if n.role == role]

    def transceivers(self, only_configured: bool = True) -> list[Node]:
        return self.by_role(ROLE_TRANSCEIVER, only_configured)

    def unique_id(self, prefix: str = "node") -> str:
        """An id not already taken, for adding a node from the UI."""

        taken = {n.node_id for n in self._nodes}
        for letter in "abcdefghijklmnopqrstuvwxyz":
            if letter not in taken:
                return letter
        index = 1
        while f"{prefix}{index}" in taken:
            index += 1
        return f"{prefix}{index}"

    def next_optical_addr(self) -> int:
        """Lowest free optical address, starting at 7 to match the bench docs."""

        taken = {n.optical_addr for n in self._nodes}
        addr = 7
        while addr in taken:
            addr += 1
        return addr

    def next_tun_ip(self, subnet: str = "192.168.0.") -> str:
        """Lowest free host address in the optical subnet."""

        taken = {n.tun_ip for n in self._nodes}
        for host in range(1, 255):
            candidate = f"{subnet}{host}"
            if candidate not in taken:
                return candidate
        return ""
