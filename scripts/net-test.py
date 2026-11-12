#!/usr/bin/env python3
"""Check that myos joins a network and answers on it, as another machine on
the LAN sees it.

    scripts/net-test.py <iso> <log> [card]

QEMU's network card (`card`: e1000e, e1000, ...) is plugged into this script
through a socket (-netdev socket: each Ethernet frame goes over TCP after
its length, 4 bytes, big endian), so the script is the rest of the network:
it plays a DHCP server that hands out 192.168.77.10, then, once the kernel
says it has that address, checks that it

  - answers an ARP request for it
  - answers pings (ICMP echo requests), with the same data
  - says "port unreachable" (ICMP) to a UDP datagram for a closed port
  - resets (TCP RST) a connection attempt to a closed port

UEFI with $OVMF set, legacy BIOS otherwise. Exits 0 on a pass; on a failure
prints why and the kernel's serial log.
"""

import os
import re
import select
import socket
import struct
import subprocess
import sys
import time

PEER_MAC = bytes.fromhex("020000000001")
PEER_IP = bytes([192, 168, 77, 1])
GUEST_IP = bytes([192, 168, 77, 10])
BROADCAST = b"\xff" * 6
LEASE_S = 3600
TIMEOUT_S = float(os.environ.get("NET_TIMEOUT", "90"))


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_packet(proto, src, dst, payload):
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
    return h[:10] + struct.pack("!H", checksum(h)) + h[12:] + payload


def pseudo(src, dst, proto, length):
    return src + dst + struct.pack("!BBH", 0, proto, length)


def udp_packet(src, dst, sport, dport, data):
    u = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
    c = checksum(pseudo(src, dst, 17, len(u)) + u) or 0xFFFF
    return ip_packet(17, src, dst, u[:6] + struct.pack("!H", c) + u[8:])


def eth(dst, ethertype, payload):
    return dst + PEER_MAC + struct.pack("!H", ethertype) + payload


class Link:
    """The TCP connection QEMU's socket netdev makes to us."""

    def __init__(self, conn):
        self.conn = conn
        self.buf = b""

    def send(self, frame):
        self.conn.sendall(struct.pack("!I", len(frame)) + frame)

    def recv(self, timeout):
        """The next frame, or None after `timeout` seconds."""
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack("!I", self.buf[:4])[0]
                if len(self.buf) >= 4 + n:
                    frame, self.buf = self.buf[4 : 4 + n], self.buf[4 + n :]
                    return frame
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.conn], [], [], left)
            if r:
                data = self.conn.recv(65536)
                if not data:
                    raise EOFError("QEMU closed the connection")
                self.buf += data


class Peer:
    def __init__(self, link):
        self.link = link
        self.guest_mac = None
        self.bound = False

    def dhcp_reply(self, frame):
        """Answer a DHCPDISCOVER or DHCPREQUEST in `frame` (if it is one)."""
        ip = frame[14:]
        if len(ip) < 28 or ip[9] != 17 or struct.unpack("!H", ip[22:24])[0] != 67:
            return
        m = ip[28:]
        if len(m) < 240 or m[0] != 1 or m[236:240] != b"\x63\x82\x53\x63":
            return
        xid, chaddr = m[4:8], m[28:34]
        opts, i = {}, 240
        while i < len(m) and m[i] != 255:
            if m[i] == 0:
                i += 1
                continue
            opts[m[i]] = m[i + 2 : i + 2 + m[i + 1]]
            i += 2 + m[i + 1]
        kind = opts.get(53, b"\0")[0]
        if kind == 1:
            reply = 2  # offer
        elif kind == 3 and opts.get(50, GUEST_IP) == GUEST_IP:
            reply = 5  # ack
        else:
            return
        self.guest_mac = chaddr
        o = bytes([53, 1, reply, 54, 4]) + PEER_IP + bytes([1, 4, 255, 255, 255, 0, 3, 4]) + PEER_IP
        o += bytes([6, 4]) + PEER_IP + bytes([51, 4]) + struct.pack("!I", LEASE_S) + b"\xff"
        bootp = bytes([2, 1, 6, 0]) + xid + b"\0" * 8 + GUEST_IP + PEER_IP + b"\0" * 4 + chaddr + b"\0" * 202
        bootp += b"\x63\x82\x53\x63" + o
        self.link.send(eth(BROADCAST, 0x0800, udp_packet(PEER_IP, b"\xff" * 4, 67, 68, bootp)))
        if reply == 5:
            self.bound = True

    def handle_background(self, frame):
        """DHCP, and the guest's ARP requests for us."""
        if len(frame) < 14:
            return
        ethertype = struct.unpack("!H", frame[12:14])[0]
        if ethertype == 0x0800:
            self.dhcp_reply(frame)
        elif ethertype == 0x0806:
            a = frame[14:42]
            if struct.unpack("!H", a[6:8])[0] == 1 and a[24:28] == PEER_IP:
                reply = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + PEER_MAC + PEER_IP + a[8:14] + a[14:18]
                self.link.send(eth(a[8:14], 0x0806, reply))

    def wait_for(self, match, timeout, what):
        """The first frame from the guest that `match` accepts; others are
        handled in the background."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            f = self.link.recv(deadline - time.monotonic())
            if f is None:
                break
            if match(f):
                return f
            self.handle_background(f)
        raise AssertionError("no " + what)

    def ip_from_guest(self, proto):
        def match(f):
            return len(f) >= 34 and f[12:14] == b"\x08\x00" and f[23] == proto and f[26:30] == GUEST_IP
        return match


def run_checks(peer, log):
    deadline = time.monotonic() + TIMEOUT_S
    # DHCP, until the kernel says it has the address.
    while not (peer.bound and "192.168.77.10/24 from DHCP server 192.168.77.1" in log()):
        if time.monotonic() > deadline:
            raise AssertionError("the kernel never said it got 192.168.77.10 from DHCP")
        f = peer.link.recv(0.2)
        if f:
            peer.handle_background(f)
    gmac = peer.guest_mac

    # ARP.
    req = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1) + PEER_MAC + PEER_IP + b"\0" * 6 + GUEST_IP
    peer.link.send(eth(BROADCAST, 0x0806, req))
    f = peer.wait_for(lambda f: f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02", 5, "ARP reply")
    if f[22:28] != gmac or f[28:32] != GUEST_IP or f[0:6] != PEER_MAC:
        raise AssertionError("a wrong ARP reply: " + f[:42].hex())
    print("  ARP: 192.168.77.10 is at " + ":".join("%02x" % b for b in gmac))

    # Ping.
    for seq in range(1, 4):
        data = bytes((seq * 7 + i) & 0xFF for i in range(56))
        icmp = struct.pack("!BBHHH", 8, 0, 0, 0x4D59, seq) + data
        icmp = icmp[:2] + struct.pack("!H", checksum(icmp)) + icmp[4:]
        t0 = time.monotonic()
        peer.link.send(eth(gmac, 0x0800, ip_packet(1, PEER_IP, GUEST_IP, icmp)))
        f = peer.wait_for(peer.ip_from_guest(1), 5, "echo reply to icmp_seq=%d" % seq)
        ip = f[14:]
        hlen = (ip[0] & 15) * 4
        reply = ip[hlen : struct.unpack("!H", ip[2:4])[0]]
        if checksum(ip[:hlen]) != 0 or checksum(reply) != 0:
            raise AssertionError("a bad checksum on the echo reply")
        if reply[0] != 0 or reply[4:8] != struct.pack("!HH", 0x4D59, seq) or reply[8:] != data:
            raise AssertionError("a wrong echo reply: " + reply.hex())
        if ip[16:20] != PEER_IP or f[0:6] != PEER_MAC:
            raise AssertionError("the echo reply went to the wrong place")
        print("  ping: reply to icmp_seq=%d in %.3f ms" % (seq, (time.monotonic() - t0) * 1000))

    # A closed UDP port.
    peer.link.send(eth(gmac, 0x0800, udp_packet(PEER_IP, GUEST_IP, 40000, 9, b"anyone there?")))
    f = peer.wait_for(peer.ip_from_guest(1), 5, "ICMP port unreachable for UDP port 9")
    icmp = f[34:]
    if icmp[0] != 3 or icmp[1] != 3 or icmp[8 + 20 + 2 : 8 + 20 + 4] != b"\x00\x09":
        raise AssertionError("not a port unreachable for port 9: " + icmp[:40].hex())
    print("  UDP to closed port 9: ICMP port unreachable")

    # A closed TCP port.
    seq = 0x12345678
    syn = struct.pack("!HHIIBBHHH", 40001, 80, seq, 0, 5 << 4, 0x02, 65535, 0, 0)
    syn = syn[:16] + struct.pack("!H", checksum(pseudo(PEER_IP, GUEST_IP, 6, len(syn)) + syn)) + syn[18:]
    peer.link.send(eth(gmac, 0x0800, ip_packet(6, PEER_IP, GUEST_IP, syn)))
    f = peer.wait_for(peer.ip_from_guest(6), 5, "TCP reset from port 80")
    tcp = f[34:]
    sport, dport, _, ack, _, flags = struct.unpack("!HHIIBB", tcp[:14])
    if (sport, dport) != (80, 40001) or flags & 0x14 != 0x14 or ack != seq + 1:
        raise AssertionError("not a RST+ACK for the SYN: " + tcp[:20].hex())
    print("  TCP to closed port 80: reset")


def main():
    iso, logpath = sys.argv[1], sys.argv[2]
    card = sys.argv[3] if len(sys.argv) > 3 else "e1000e"
    os.makedirs(os.path.dirname(logpath) or ".", exist_ok=True)
    open(logpath, "w").close()

    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]
    args = ["qemu-system-x86_64", "-M", "q35", "-m", "512M", "-cpu", "max", "-smp", "2", "-vga", "std",
            "-display", "none", "-serial", "file:" + logpath, "-no-reboot", "-cdrom", iso,
            "-netdev", "socket,id=lan,connect=127.0.0.1:%d" % port, "-device", "%s,netdev=lan" % card]
    ovmf = os.environ.get("OVMF")
    if ovmf:
        args += ["-drive", "if=pflash,unit=0,format=raw,readonly=on,file=" + ovmf]
    qemu = subprocess.Popen(args, stderr=subprocess.PIPE)

    def log():
        with open(logpath, errors="replace") as f:
            return re.sub(r"\x1b\[[0-9;=]*[a-zA-Z]", "", f.read())

    label = "[net %s%s]" % (card, " uefi" if ovmf else " bios")
    t0 = time.monotonic()
    ok = False
    try:
        server.settimeout(30)
        conn, _ = server.accept()
        run_checks(Peer(Link(conn)), log)
        ok = not re.search(r"KERNEL PANIC|CPU EXCEPTION|unhandled interrupt", log())
        if not ok:
            print("FAIL %s the kernel crashed:" % label)
    except (AssertionError, EOFError, OSError) as e:
        print("FAIL %s %s:" % (label, e))
    finally:
        qemu.kill()
        _, err = qemu.communicate()
    if ok:
        print("PASS %s in %ds" % (label, time.monotonic() - t0))
        return 0
    for line in log().splitlines():
        print("    " + line)
    if err.strip():
        print("  QEMU stderr:\n    " + err.decode(errors="replace").strip().replace("\n", "\n    "))
    return 1


if __name__ == "__main__":
    sys.exit(main())
