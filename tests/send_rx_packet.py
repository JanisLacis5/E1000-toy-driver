#!/usr/bin/env python3
"""
AI GENERATED TEST:
Send Ethernet frames directly to a test NIC; no ARP or replies required
"""

import argparse
import ipaddress
import math
from pathlib import Path
import socket
import struct
import time


def mac_address(value):
    try:
        parts = value.split(":")
        if len(parts) != 6 or any(len(part) != 2 for part in parts):
            raise ValueError
        return bytes(int(part, 16) for part in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected six hex bytes, e.g. 52:54:00:12:34:57") from exc


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


def build_frame(src_mac, dst_mac, src_ip, dst_ip, protocol, payload, sequence):
    src = socket.inet_aton(src_ip)
    dst = socket.inet_aton(dst_ip)
    if protocol == "icmp":
        number = 1
        segment = struct.pack("!BBHHH", 8, 0, 0, 0x4a52, sequence) + payload
        segment = segment[:2] + struct.pack("!H", checksum(segment)) + segment[4:]
    else:
        number = 17 if protocol == "udp" else 6
        if protocol == "udp":
            segment = struct.pack("!HHHH", 40000, 9000, 8 + len(payload), 0) + payload
            offset = 6
        else:
            # A standalone SYN; no handshake or retransmission machinery.
            segment = struct.pack("!HHIIBBHHH", 40000, 9000, sequence, 0,
                                  5 << 4, 0x02, 65535, 0, 0) + payload
            offset = 16
        pseudo = src + dst + struct.pack("!BBH", 0, number, len(segment))
        value = checksum(pseudo + segment)
        if protocol == "udp" and value == 0:
            value = 0xffff
        segment = segment[:offset] + struct.pack("!H", value) + segment[offset + 2:]

    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(segment),
                         sequence, 0, 64, number, 0, src, dst)
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    frame = dst_mac + src_mac + b"\x08\x00" + header + segment
    # Ethernet minimum length excluding FCS. The NIC/backend supplies FCS.
    return frame.ljust(60, b"\0")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True, help="sender interface (e.g. host tap-test)")
    parser.add_argument("--dst-mac", required=True, type=mac_address, help="test NIC's MAC")
    parser.add_argument("--src-mac", type=mac_address, help="override sender MAC")
    parser.add_argument("--src-ip", type=ipaddress.IPv4Address, default="192.0.2.1")
    parser.add_argument("--dst-ip", type=ipaddress.IPv4Address, default="192.0.2.2")
    parser.add_argument("--protocol", choices=("icmp", "udp", "tcp"), default="icmp")
    parser.add_argument("--payload", default="janis RX test", help="UTF-8 payload")
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--interval", type=float, default=0.25, help="seconds between frames")
    parser.add_argument("--dry-run", action="store_true", help="print frame hex without sending")
    args = parser.parse_args()
    if args.count < 1 or not math.isfinite(args.interval) or args.interval < 0:
        parser.error("count must be positive and interval must be finite and nonnegative")
    payload = args.payload.encode()
    transport_size = 20 if args.protocol == "tcp" else 8
    if 20 + transport_size + len(payload) > 1500:
        parser.error("IPv4 packet exceeds 1500 bytes; fragmentation is not implemented")
    try:
        socket.if_nametoindex(args.interface)
        src_mac = args.src_mac or mac_address(
            (Path("/sys/class/net") / args.interface / "address").read_text().strip())
        sock = None
        if not args.dry_run:
            sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800))
            sock.bind((args.interface, 0))
        try:
            for index in range(args.count):
                frame = build_frame(src_mac, args.dst_mac, str(args.src_ip), str(args.dst_ip),
                                    args.protocol, payload, index & 0xffff)
                if args.dry_run:
                    print(frame.hex())
                else:
                    sent = sock.send(frame)
                    if sent != len(frame):
                        raise OSError(f"short send: {sent}/{len(frame)} bytes")
                    print(f"Sent {args.protocol} frame {index + 1}: {sent} bytes via {args.interface}")
                if index + 1 < args.count and not args.dry_run:
                    time.sleep(args.interval)
        finally:
            if sock is not None:
                sock.close()
    except PermissionError:
        parser.exit(1, "Permission denied: sending requires root or CAP_NET_RAW.\n")
    except (OSError, argparse.ArgumentTypeError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
