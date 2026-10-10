#!/usr/bin/env python3
"""Look for LoRa preambles in a Flipper Sub-GHz RAW (.sub) capture.

A LoRa preamble seen through the CC1101's 1-bit 2-FSK discriminator should be a
square wave whose rising-edge-to-rising-edge period equals one symbol time
T = 2^SF / BW. This scans for runs of consecutive periods close to a target T.
"""
import argparse
import sys

PROFILES = {
    # name: (bw_hz, sf, preamble_symbols)
    "meshcore-us": (62500, 7, 32),
    "meshtastic-mediumfast": (250000, 9, 16),
    "meshtastic-longfast": (250000, 11, 16),
}


def load_sub(path):
    header, durations = {}, []
    with open(path) as f:
        for line in f:
            key, _, value = line.partition(":")
            if key == "RAW_Data":
                durations.extend(int(v) for v in value.split())
            elif value:
                header[key] = value.strip()
    return header, durations


def edges(durations):
    """Return (time_us, level) for each level change; positive duration = high."""
    t, out = 0, []
    for d in durations:
        out.append((t, d > 0))
        t += abs(d)
    return out, t


def find_runs(durations, period_us, tol, min_run, min_pulse_us):
    # Merge glitches shorter than min_pulse_us into their neighbours so a noisy
    # edge doesn't split an otherwise clean period.
    merged = []
    for d in durations:
        if merged and (abs(d) < min_pulse_us or (d > 0) == (merged[-1] > 0)):
            sign = 1 if merged[-1] > 0 else -1
            merged[-1] = sign * (abs(merged[-1]) + abs(d))
        else:
            merged.append(d)

    ev, _ = edges(merged)
    rises = [t for t, lvl in ev if lvl]
    runs, start = [], None
    for i in range(1, len(rises)):
        p = rises[i] - rises[i - 1]
        if abs(p - period_us) <= tol * period_us:
            if start is None:
                start = i - 1
        else:
            if start is not None and i - 1 - start >= min_run:
                runs.append((start, i - 1))
            start = None
    if start is not None and len(rises) - 1 - start >= min_run:
        runs.append((start, len(rises) - 1))

    results = []
    for s, e in runs:
        periods = [rises[k + 1] - rises[k] for k in range(s, e)]
        results.append({
            "start_ms": rises[s] / 1000,
            "symbols": e - s,
            "mean_period_us": sum(periods) / len(periods),
            "min_period_us": min(periods),
            "max_period_us": max(periods),
            # rising-edge periods right after the preamble (sync word symbols)
            "after": [rises[k + 1] - rises[k] for k in range(e, min(e + 2, len(rises) - 1))],
        })
    return results, merged


def sync_word(after, period_us, chip_us):
    """Each sync symbol is a chirp shifted by 8*nibble chips. A shift of s chips
    moves the rising edge s chips earlier, so the period into symbol k is
    T - (s_k - s_{k-1}) * Tc, with s = 0 during the preamble."""
    if len(after) < 2:
        return None
    s1 = (period_us - after[0]) / chip_us
    s2 = s1 + (period_us - after[1]) / chip_us
    n1, n2 = round(s1 / 8), round(s2 / 8)
    if not (0 <= n1 <= 15 and 0 <= n2 <= 15):
        return None
    err = max(abs(s1 - 8 * n1), abs(s2 - 8 * n2))
    return (n1 << 4) | n2, s1, s2, err


SYNC_WORDS = {0x12: "MeshCore / private", 0x2B: "Meshtastic", 0x34: "LoRaWAN public"}


def duty_cycle(merged, start_ms, n_sym, period_us):
    """High fraction over a window starting at start_ms spanning n_sym periods."""
    t, hi = 0, 0
    w0, w1 = start_ms * 1000, start_ms * 1000 + n_sym * period_us
    for d in merged:
        a, b = t, t + abs(d)
        if d > 0:
            hi += max(0, min(b, w1) - max(a, w0))
        t = b
    return hi / (w1 - w0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--profile", default="meshcore-us", choices=PROFILES)
    ap.add_argument("--tol", type=float, default=0.05, help="period tolerance (fraction)")
    ap.add_argument("--min-run", type=int, default=6, help="min consecutive periods")
    ap.add_argument("--min-pulse", type=int, default=0, help="merge pulses shorter than this (us)")
    args = ap.parse_args()

    header, durations = load_sub(args.file)
    bw, sf, pre = PROFILES[args.profile]
    period = (2 ** sf) / bw * 1e6
    _, total = edges(durations)
    print(f"{args.file}: {header.get('Frequency')} Hz, {len(durations)} pulses, "
          f"{total/1e6:.2f} s")
    print(f"profile {args.profile}: T = {period:.0f} us, expected preamble {pre} symbols")

    runs, merged = find_runs(durations, period, args.tol, args.min_run, args.min_pulse)
    if not runs:
        print("no preamble-like runs found")
        return 1
    for r in runs:
        duty = duty_cycle(merged, r["start_ms"], r["symbols"], r["mean_period_us"])
        df = (duty - 0.5) * bw / 1000
        print(f"  t={r['start_ms']:9.1f} ms  run={r['symbols']:3d} sym  "
              f"T={r['mean_period_us']:7.1f} us ({r['min_period_us']}-{r['max_period_us']})  "
              f"duty={duty:.2f} (~{df:+.1f} kHz)")
        sw = sync_word(r["after"], r["mean_period_us"], 1e6 / bw)
        if sw:
            word, s1, s2, err = sw
            print(f"      sync word 0x{word:02X} ({SYNC_WORDS.get(word, 'unknown')}), "
                  f"symbol shifts {s1:.1f}/{s2:.1f} chips, worst error {err:.1f} chips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
