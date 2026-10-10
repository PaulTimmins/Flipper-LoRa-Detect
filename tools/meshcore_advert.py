#!/usr/bin/env python3
"""Recover MeshCore adverts from a Flipper RAW capture by soft-combining every
copy heard (the original plus repeater retransmissions) and checking the
advert's Ed25519 signature.

A flood advert's payload is identical in every copy. Only the path (one hash
per repeater) differs, so the copies line up at the end of the packet.
"""
import argparse
import datetime
import itertools
import struct
import sys

import numpy as np
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from analyze_sub import load_sub, find_runs  # noqa: E402
from lora_decode import crc16, decode_packet, to_samples, whitening_seq  # noqa: E402

ADV_TYPES = {0: "none", 1: "chat", 2: "repeater", 3: "room", 4: "sensor"}
ROUTES = {0: "transport-flood", 1: "flood", 2: "direct", 3: "transport-direct"}


def body_metrics(r, off, blen):
    """Per-nibble metrics (16 values) of the dewhitened body starting at byte off."""
    w = whitening_seq(r["length"])
    out = []
    for j in range(2 * blen):
        b = off + j // 2
        mask = (w[b] & 0x0F) if j % 2 == 0 else (w[b] >> 4)
        out.append(r["nib_metrics"][2 * off + j][np.arange(16) ^ mask])
    return out


def to_bytes(nibs):
    return bytes((nibs[2 * i + 1] << 4) | nibs[2 * i] for i in range(len(nibs) // 2))


def verify_advert(body):
    if len(body) < 101:
        return False
    pub, ts, sig, app = body[:32], body[32:36], body[36:100], body[100:]
    try:
        Ed25519PublicKey.from_public_bytes(pub).verify(sig, pub + ts + app)
        return True
    except (InvalidSignature, ValueError):
        return False


def parse_advert(body):
    pub, ts = body[:32], struct.unpack("<I", body[32:36])[0]
    app = body[100:]
    flags = app[0] if app else 0
    info = {
        "pubkey": pub.hex(),
        "timestamp": datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).isoformat(),
        "type": ADV_TYPES.get(flags & 0x0F, flags & 0x0F),
    }
    i = 1
    if flags & 0x10:
        lat, lon = struct.unpack("<ii", app[i:i + 8])
        info["lat"], info["lon"] = lat / 1e6, lon / 1e6
        i += 8
    if flags & 0x20:
        i += 2
    if flags & 0x40:
        i += 2
    if flags & 0x80:
        info["name"] = app[i:].decode("utf-8", "replace")
    return info


def combine(copies, max_flips):
    """Return (body, verified, flips, offsets) for the best alignment."""
    shortest = min(r["length"] for r in copies)
    best = None
    # body length: try a few path lengths on the shortest copy (header + path_len >= 2)
    for blen in range(shortest - 2, max(shortest - 2 - 16, 100), -1):
        offs = [r["length"] - blen for r in copies]
        if min(offs) < 2:
            continue
        summed = sum(np.array(body_metrics(r, o, blen)) for r, o in zip(copies, offs))
        score = summed.max(axis=1).sum()
        if best is None or score > best[0]:
            best = (score, blen, offs, summed)
    _, blen, offs, summed = best

    order = np.argsort(summed, axis=1)
    nibs = list(order[:, -1])
    runner = list(order[:, -2])
    margin = np.take_along_axis(summed, order[:, -1:], 1)[:, 0] - \
        np.take_along_axis(summed, order[:, -2:-1], 1)[:, 0]

    body = to_bytes(nibs)
    if verify_advert(body):
        return body, True, 0, offs
    weakest = list(np.argsort(margin)[:max_flips])
    for mask in range(1, 1 << len(weakest)):
        trial = nibs[:]
        for b, i in enumerate(weakest):
            if mask >> b & 1:
                trial[i] = runner[i]
        t = to_bytes(trial)
        if verify_advert(t):
            return t, True, bin(mask).count("1"), offs
    return body, False, 0, offs


def copy_prefix(r, off, body, depth=2):
    """Find this copy's header + path. Only structurally valid path_len values are
    tried, and a candidate counts as confirmed when it reproduces one of the
    copy's likely CRC values. Otherwise fall back to the hard decision."""
    L, nm = r["length"], r["nib_metrics"]
    w = whitening_seq(L)
    top = lambda m: list(np.argsort(m)[::-1][:depth])  # noqa: E731
    crcs = {(c[1] << 4 | c[0]) | ((c[3] << 4 | c[2]) << 8)
            for c in itertools.product(*[top(nm[2 * L + i]) for i in range(4)])}
    hdr_cands = []
    for j in range(2 * off):
        mask = (w[j // 2] & 0x0F) if j % 2 == 0 else (w[j // 2] >> 4)
        hdr_cands.append(top(nm[j][np.arange(16) ^ mask]))
    confirmed = None
    for pre_n in itertools.product(*hdr_cands[:2]):
        hdr = (pre_n[1] << 4) | pre_n[0]
        for pl in range(256):
            if (pl & 63) * ((pl >> 6) + 1) != off - 2:
                continue
            for pn in itertools.product(*hdr_cands[4:]):
                pre = bytes([hdr, pl]) + to_bytes(list(pn))
                d = list(pre + body)
                if crc16(d[:-2]) ^ d[-1] ^ (d[-2] << 8) in crcs:
                    confirmed = pre
                    break
            if confirmed:
                break
        if confirmed:
            break
    data = confirmed or r["payload"][:off]
    size, count = (data[1] >> 6) + 1, data[1] & 63
    path = [data[2 + k * size: 2 + (k + 1) * size].hex() for k in range(count)] \
        if 2 + count * size == off else None
    return {"route": ROUTES.get(data[0] & 3), "path": path, "prefix": data.hex(),
            "confirmed": confirmed is not None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--sf", type=int, default=7)
    ap.add_argument("--bw", type=int, default=62500)
    ap.add_argument("--max-flips", type=int, default=14)
    args = ap.parse_args()

    _, durations = load_sub(args.file)
    sig = to_samples(durations)
    runs, _ = find_runs(durations, (1 << args.sf) / args.bw * 1e6, 0.05, 6, 0)
    copies = []
    for run in runs:
        r = decode_packet(sig, run, args.sf, args.bw)
        if r.get("payload") is not None and (r["payload"][0] >> 2) & 0x0F == 4:
            r["t_ms"] = run["start_ms"]
            copies.append(r)
    print(f"{len(runs)} LoRa packets, {len(copies)} look like adverts")
    if not copies:
        return 1

    body, ok, flips, offs = combine(copies, args.max_flips)
    print(f"combined {len(copies)} copies: signature {'VALID' if ok else 'not verified'}"
          + (f" (after {flips} soft corrections)" if flips else ""))
    for k, v in parse_advert(body).items():
        print(f"  {k:9s} {v}")
    for r, off in zip(copies, offs):
        c = copy_prefix(r, off, body)
        hops = "unreadable" if c["path"] is None else \
            (f"{len(c['path'])} hop(s) via {', '.join(c['path'])}" if c["path"] else "0 hops (originator)")
        print(f"  heard at {r['t_ms']:8.1f} ms: {c['route']}, {hops} "
              f"[{'CRC confirmed' if c['confirmed'] else 'unconfirmed, CRC damaged'}]")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
