#!/usr/bin/env python3
"""Decode Meshtastic packets from a Flipper RAW capture.

Meshtastic over-the-air frame (src/mesh/RadioInterface.h, PacketHeader):
    to (4, LE) | from (4, LE) | id (4, LE) | flags | channel hash | next_hop | relay_node
    followed by the encrypted Data protobuf.
Channel traffic is AES-CTR with nonce = id (as uint64 LE) | from (uint32 LE) | 0 (uint32).
The default channel key ("AQ==") is public, and "LongFast" + that key hashes to 0x08.

Because single copies carry bit errors, each packet is list-decoded: corrections are
tried cheapest-first until the LoRa CRC matches AND the decrypted payload parses as a
well-formed Meshtastic Data protobuf.
"""
import argparse
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from analyze_sub import load_sub, find_runs  # noqa: E402
from lora_decode import crc_list_decode, decode_packet, to_samples  # noqa: E402

DEFAULT_PSK = bytes([0xD4, 0xF1, 0xBB, 0x3A, 0x20, 0x29, 0x07, 0x59,
                     0xF0, 0xBC, 0xFF, 0xAB, 0xCF, 0x4E, 0x69, 0x01])
LONGFAST_HASH = 0x08

PORTS = {
    1: "TEXT_MESSAGE", 3: "POSITION", 4: "NODEINFO", 5: "ROUTING", 6: "ADMIN",
    7: "TEXT_MESSAGE_COMPRESSED", 8: "WAYPOINT", 10: "DETECTION_SENSOR", 32: "REPLY",
    34: "PAXCOUNTER", 64: "SERIAL", 65: "STORE_FORWARD", 66: "RANGE_TEST",
    67: "TELEMETRY", 70: "TRACEROUTE", 71: "NEIGHBORINFO", 72: "ATAK_PLUGIN", 73: "MAP_REPORT",
}
# Data message fields and their protobuf wire types (meshtastic/mesh.proto)
DATA_WIRE = {1: 0, 2: 2, 3: 0, 4: 5, 5: 5, 6: 5, 7: 5, 8: 5, 9: 0}


def varint(b, i):
    v, shift = 0, 0
    while True:
        if i >= len(b) or shift > 63:
            raise ValueError("varint")
        c = b[i]
        i += 1
        v |= (c & 0x7F) << shift
        if not c & 0x80:
            return v, i
        shift += 7


def parse_proto(b, wire_map=None):
    """Return [(field, wire, value)], raising ValueError unless b is well formed."""
    out, i = [], 0
    while i < len(b):
        key, i = varint(b, i)
        field, wire = key >> 3, key & 7
        if field == 0 or (wire_map is not None and wire_map.get(field) != wire):
            raise ValueError("field")
        if wire == 0:
            v, i = varint(b, i)
        elif wire == 2:
            n, i = varint(b, i)
            if i + n > len(b):
                raise ValueError("len")
            v, i = b[i:i + n], i + n
        elif wire == 5:
            if i + 4 > len(b):
                raise ValueError("fixed32")
            v, i = struct.unpack("<I", b[i:i + 4])[0], i + 4
        elif wire == 1:
            if i + 8 > len(b):
                raise ValueError("fixed64")
            v, i = struct.unpack("<Q", b[i:i + 8])[0], i + 8
        else:
            raise ValueError("wire")
        out.append((field, wire, v))
    return out


def decrypt(frame, key=DEFAULT_PSK):
    to, frm, pid = struct.unpack("<III", frame[:12])
    nonce = struct.pack("<QI", pid, frm) + b"\0\0\0\0"
    d = Cipher(algorithms.AES(key), modes.CTR(nonce)).decryptor()
    return d.update(frame[16:]) + d.finalize()


def parse_data(plain):
    fields = parse_proto(plain, DATA_WIRE)
    port = next((v for f, _, v in fields if f == 1), None)
    if port is None or (port not in PORTS and not 256 <= port <= 511):
        raise ValueError("port")
    return port, fields


def validator(frame):
    if len(frame) < 17:
        return False
    if frame[13] != LONGFAST_HASH:
        return False
    try:
        parse_data(decrypt(frame))
        return True
    except ValueError:
        return False


def describe(port, fields):
    payload = next((v for f, _, v in fields if f == 2), b"")
    if port == 1:
        return repr(payload.decode("utf-8", "replace"))
    if port == 4:  # User: id, long_name, short_name, macaddr, hw_model ...
        try:
            u = {f: v for f, _, v in parse_proto(payload)}
            names = [u.get(k, b"").decode("utf-8", "replace") for k in (1, 2, 3)]
            return f"user id={names[0]} long_name={names[1]!r} short_name={names[2]!r}"
        except ValueError:
            pass
    if port == 70:  # RouteDiscovery: route (packed fixed32), snr_towards (packed int32), ...
        try:
            f = {}
            for fld, _, v in parse_proto(payload):
                f.setdefault(fld, b"")
                f[fld] += v if isinstance(v, bytes) else b""
            route = [f"!{x:08x}" for x in struct.unpack(f"<{len(f.get(1, b'')) // 4}I", f.get(1, b""))]
            back = [f"!{x:08x}" for x in struct.unpack(f"<{len(f.get(3, b'')) // 4}I", f.get(3, b""))]
            snr = [((x ^ 0x80) - 0x80) / 4 for x in f.get(2, b"")]  # small values: 1-byte varints
            out = f"route={route} snr_towards_dB={snr}"
            return out + (f" route_back={back}" if back else "")
        except (ValueError, struct.error):
            pass
    try:
        inner = parse_proto(payload)
        return "fields " + ", ".join(f"{f}={v.hex() if isinstance(v, bytes) else v}" for f, _, v in inner)
    except ValueError:
        return payload.hex()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--sf", type=int, default=11)
    ap.add_argument("--bw", type=int, default=250000)
    ap.add_argument("--max-checks", type=int, default=300000)
    args = ap.parse_args()

    _, durations = load_sub(args.file)
    sig = to_samples(durations)
    runs, _ = find_runs(durations, (1 << args.sf) / args.bw * 1e6, 0.05, 6, 0)
    print(f"{len(runs)} LoRa preambles")
    for run in runs:
        r = decode_packet(sig, run, args.sf, args.bw)
        sync = "none" if not r["sync"] else f"0x{r['sync'][0]:X}{r['sync'][1]:X}"
        head = f"t={run['start_ms'] / 1000:7.2f}s sync={sync}"
        if not r["header_ok"] or "nib_metrics" not in r:
            print(f"{head}  header not decoded")
            continue
        hard = r["payload"]
        to, frm, pid = struct.unpack("<III", hard[:12])
        if hard[13] == 0:
            # PKI direct message: can't decrypt, and no structure to validate beyond CRC
            got = crc_list_decode(r, None, max_checks=args.max_checks)
            frame = got[0] if got else hard
            to, frm, pid = struct.unpack("<III", frame[:12])
            print(f"{head}  len={r['length']}  PKI DM !{frm:08x} -> !{to:08x} id={pid:08x} "
                  f"[{'CRC ok, ' + str(got[1]) + ' corrections' if got else 'CRC not recovered'}, encrypted]")
            continue
        got = crc_list_decode(r, validator, max_checks=args.max_checks)
        if not got:
            print(f"{head}  len={r['length']}  from~!{frm:08x} chan~0x{hard[13]:02x}  not recovered")
            continue
        frame, ncorr, checks = got
        to, frm, pid = struct.unpack("<III", frame[:12])
        flags = frame[12]
        port, fields = parse_data(decrypt(frame))
        dest = "broadcast" if to == 0xFFFFFFFF else f"!{to:08x}"
        print(f"{head}  len={r['length']}  !{frm:08x} -> {dest} id={pid:08x} "
              f"hops {flags & 7}/{flags >> 5} relay=0x{frame[15]:02x} "
              f"[CRC+protobuf ok, {ncorr} corrections, {checks} tries]")
        print(f"      {PORTS.get(port, port)}: {describe(port, fields)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
