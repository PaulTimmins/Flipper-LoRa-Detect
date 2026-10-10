#!/usr/bin/env python3
"""Decode LoRa packets from a Flipper Sub-GHz RAW capture taken with a 2-FSK
async preset (the CC1101 1-bit frequency discriminator output).

Approach:
  * Find the preamble (run of rising-edge periods ~= T = 2^SF/BW).
  * Average the preamble symbols into a 1-bit template of an unshifted upchirp.
  * A LoRa symbol of value s is the same chirp cyclically shifted by s chips, so
    each symbol window is matched against every cyclic shift of the template.
  * Track timing drift with the sub-chip residual of each decision.
  * Run the standard LoRa RX chain (gray, deinterleave, Hamming, dewhiten, CRC),
    following gr-lora_sdr (github.com/tapparelj/gr-lora_sdr).
"""
import argparse
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from analyze_sub import load_sub, find_runs  # noqa: E402


def whitening_seq(n):
    out, x = [], 0xFF
    for _ in range(n):
        out.append(x)
        fb = ((x >> 7) ^ (x >> 5) ^ (x >> 4) ^ (x >> 3)) & 1
        x = ((x << 1) | fb) & 0xFF
    return out


def to_samples(durations):
    """+1/-1 per microsecond."""
    sig = np.empty(sum(abs(d) for d in durations), dtype=np.int8)
    t = 0
    for d in durations:
        sig[t:t + abs(d)] = 1 if d > 0 else -1
        t += abs(d)
    return sig


def rising_edges(sig, a, b):
    seg = sig[a:b]
    return a + np.nonzero((seg[1:] > 0) & (seg[:-1] < 0))[0] + 1


class SymbolDemod:
    def __init__(self, sig, sf, t_first, period):
        self.sig, self.sf, self.n = sig, sf, 1 << sf
        self.T = period
        self.tc = period / self.n
        self.L = int(round(period))
        self.t0 = float(t_first)
        self.template = None

    def window(self, t):
        a = int(round(t))
        return self.sig[a:a + self.L].astype(np.float32)

    def build_template(self, n_sym):
        acc = np.zeros(self.L, np.float32)
        for k in range(n_sym):
            acc += self.window(self.t0 + k * self.T)
        self.template = acc / n_sym
        self.tf = np.conj(np.fft.rfft(self.template))

    def corr(self, t):
        # c[x] = sum_t W[t] * P[(t + x) mod L]  -> best x is the cyclic shift
        w = np.fft.rfft(self.window(t))
        return np.fft.irfft(np.conj(w * self.tf), self.L)

    def symbol(self, t):
        s, resid, peak, c = self.demod(t)
        return s, resid, peak, symbol_scores(c, self.tc, self.n), False

    def demod(self, t):
        c = self.corr(t)
        x = int(np.argmax(c))
        s = int(round(x / self.tc)) % self.n
        resid = x - s * self.tc
        if resid > self.L / 2:
            resid -= self.L
        ranked = np.argsort(c)[::-1]
        return s, resid, c[x], c


def near(s, step, n, tol):
    """Nearest sync nibble for symbol s (multiple of step, mod 128 signed), or None."""
    v = s if s < n // 2 else s - n          # to signed
    q = round(v / step)
    if abs(v - q * step) > tol:
        return None
    return (q * step) % 128 // step         # nibble 0..15


class EdgeDemod(SymbolDemod):
    """Template demod refined by the chirp's wrap edge.

    When the instantaneous frequency wraps from +BW/2 to -BW/2 the discriminator
    output falls sharply, so the falling edge at the wrap is far more precise than
    the slow threshold crossing. For a symbol of value s starting at t_k the wrap
    happens at t_k + (N - s) * Tc. Windows must start on true symbol boundaries,
    which the preamble's wrap edges provide."""

    SIGMA_CHIPS = 0.35

    def __init__(self, sig, sf, t_first, period, falls, lows):
        super().__init__(sig, sf, t_first, period)
        self.falls, self.lows = falls, lows

    def symbol(self, t):
        s, resid, peak, c = self.demod(t)
        n, tc = self.n, self.tc
        ar = np.arange(n)
        # template log-likelihood floor: ~0.5 nats per chip of misalignment
        tmpl = symbol_scores(c, tc, n)
        tmpl_ll = (tmpl - tmpl.max()) / (8 * tc)
        tau_c = ((n - s) % n or n) * tc
        lo = np.searchsorted(self.falls, t + 0.5 * tc)
        hi = np.searchsorted(self.falls, t + self.T + 0.5 * tc)
        comps = []
        for i in range(lo, hi):
            tau = self.falls[i] - t
            dist = min(abs(tau - tau_c), self.T - abs(tau - tau_c))
            # a wrap is followed by a solid low, not a few-us chatter pulse
            if self.lows[i] < 8 or dist > max(8 * tc, 40):
                continue
            exact = (n - tau / tc) % n
            prior = -(dist / tc) ** 2 / (2 * 4.0 ** 2) + min(self.lows[i], 200) / 50
            comps.append((prior, exact))
        if not comps:
            return s, resid, peak, tmpl_ll, False
        comps.sort(reverse=True)
        top = comps[0][0]
        ll = np.log(0.05) + tmpl_ll
        for prior, exact in comps:
            d = (ar - exact + n / 2) % n - n / 2
            ll = np.logaddexp(ll, (prior - top) - d ** 2 / (2 * self.SIGMA_CHIPS ** 2))
        exact = comps[0][1]
        s_f = int(round(exact)) % n
        resid = (s_f - exact) * tc
        return s_f, resid, peak, ll, True


def falling_edges_with_low(sig, a, b):
    seg = sig[a:b]
    f = np.nonzero((seg[1:] < 0) & (seg[:-1] > 0))[0] + 1
    r = np.nonzero((seg[1:] > 0) & (seg[:-1] < 0))[0] + 1
    nxt = np.searchsorted(r, f)
    lows = np.where(nxt < len(r), r[np.minimum(nxt, len(r) - 1)] - f, len(seg) - f)
    return a + f, lows


def gray(v):
    return v ^ (v >> 1)


def deinterleave(symbols, sf_app, cw_len):
    inter = [[(v >> (sf_app - 1 - j)) & 1 for j in range(sf_app)] for v in symbols]
    de = [[0] * cw_len for _ in range(sf_app)]
    for i in range(cw_len):
        for j in range(sf_app):
            de[(i - j - 1) % sf_app][i] = inter[i][j]
    return [int("".join(map(str, row)), 2) for row in de]


def hamming_dec(cw, cr):
    n = cr + 4
    b = [(cw >> (n - 1 - i)) & 1 for i in range(n)]
    nib = [b[3], b[2], b[1], b[0]]
    ok = True
    if cr in (3, 4):
        s0 = b[0] ^ b[1] ^ b[2] ^ b[4]
        s1 = b[1] ^ b[2] ^ b[3] ^ b[5]
        s2 = b[0] ^ b[1] ^ b[3] ^ b[6]
        syn = s0 | (s1 << 1) | (s2 << 2)
        if cr == 4 and sum(b) % 2 == 0:
            ok = syn == 0  # even errors: don't correct
        else:
            flip = {5: 3, 7: 2, 3: 1, 6: 0}.get(syn)
            if flip is not None:
                nib[flip] ^= 1
    elif cr == 2:
        s0 = b[0] ^ b[1] ^ b[2] ^ b[4]
        s1 = b[1] ^ b[2] ^ b[3] ^ b[5]
        ok = not (s0 | s1)
    elif cr == 1:
        ok = sum(b) % 2 == 0
    return (nib[0] << 3) | (nib[1] << 2) | (nib[2] << 1) | nib[3], ok


def header_check(n):
    b = lambda x, k: (x >> k) & 1  # noqa: E731
    c4 = b(n[0], 3) ^ b(n[0], 2) ^ b(n[0], 1) ^ b(n[0], 0)
    c3 = b(n[0], 3) ^ b(n[1], 3) ^ b(n[1], 2) ^ b(n[1], 1) ^ b(n[2], 0)
    c2 = b(n[0], 2) ^ b(n[1], 3) ^ b(n[1], 0) ^ b(n[2], 3) ^ b(n[2], 1)
    c1 = b(n[0], 1) ^ b(n[1], 2) ^ b(n[1], 0) ^ b(n[2], 2) ^ b(n[2], 1) ^ b(n[2], 0)
    c0 = b(n[0], 0) ^ b(n[1], 1) ^ b(n[2], 3) ^ b(n[2], 2) ^ b(n[2], 1) ^ b(n[2], 0)
    return (c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0


def crc16(data):
    crc = 0
    for byte in data:
        for i in range(8):
            if ((crc & 0x8000) >> 8) ^ (byte & 0x80):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
            byte = (byte << 1) & 0xFF
    return crc


def lora_crc_ok(payload, crc_bytes):
    if len(payload) < 2:
        return False
    c = crc16(payload[:-2]) ^ payload[-1] ^ (payload[-2] << 8)
    return c == crc_bytes[0] | (crc_bytes[1] << 8)


def hamming_enc(nib, cr):
    """Codeword (MSB first, cr+4 bits) as built by gr-lora_sdr hamming_enc."""
    d = [(nib >> (3 - k)) & 1 for k in range(4)]  # d[0] = MSB
    if cr == 1:
        return d[3] << 4 | d[2] << 3 | d[1] << 2 | d[0] << 1 | (d[0] ^ d[1] ^ d[2] ^ d[3])
    p0 = d[3] ^ d[2] ^ d[1]
    p1 = d[2] ^ d[1] ^ d[0]
    p2 = d[3] ^ d[2] ^ d[0]
    p3 = d[3] ^ d[1] ^ d[0]
    cw = d[3] << 7 | d[2] << 6 | d[1] << 5 | d[0] << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3
    return cw >> (4 - cr)


def symbol_scores(c, tc, n):
    """Best correlation within each chip bin -> score per symbol value."""
    idx = np.round(np.arange(len(c)) / tc).astype(int) % n
    sc = np.full(n, -np.inf)
    np.maximum.at(sc, idx, c)
    return sc


_BITMAPS = {}


def bit_llrs(scores, sf, header):
    """Max-log LLR for each bit (MSB first) of the gray-mapped value."""
    n = 1 << sf
    sf_app = sf - 2 if header else sf
    key = (sf, header)
    if key not in _BITMAPS:
        s = np.arange(n)
        v = ((s - 1) % n) // (4 if header else 1)
        g = v ^ (v >> 1)
        _BITMAPS[key] = [((g >> (sf_app - 1 - j)) & 1).astype(bool) for j in range(sf_app)]
    return [scores[m].max() - scores[~m].max() for m in _BITMAPS[key]]


def soft_block(llr_syms, sf_app, cw_len, cr):
    """Deinterleave per-symbol LLRs and ML-decode each codeword.
    Returns [(nibble, margin, runner_up_nibble, metric_per_nibble)] per codeword."""
    de = [[0.0] * cw_len for _ in range(sf_app)]
    for i in range(cw_len):
        for j in range(sf_app):
            de[(i - j - 1) % sf_app][i] = llr_syms[i][j]
    out = []
    for row in de:
        metrics = []
        for nib in range(16):
            cw = hamming_enc(nib, cr)
            m = sum(l if (cw >> (cw_len - 1 - k)) & 1 else -l for k, l in enumerate(row))
            metrics.append((m, nib))
        full = np.array([m for m, _ in metrics])
        metrics.sort(reverse=True)
        out.append((metrics[0][1], metrics[0][0] - metrics[1][0], metrics[1][1], full))
    return out


def assemble(nibs, length, has_crc):
    w = whitening_seq(length)
    data = [((nibs[2 * i + 1] ^ (w[i] >> 4)) << 4) | (nibs[2 * i] ^ (w[i] & 0x0F))
            for i in range(length)]
    crc_bytes = [(nibs[2 * length + 2 * i + 1] << 4) | nibs[2 * length + 2 * i]
                 for i in range(2)] if has_crc else []
    return data, crc_bytes


def decode_packet(sig, run, sf, bw, verbose=False, max_flips=12):
    T = run["mean_period_us"]
    start_us = int(run["start_ms"] * 1000)
    n_pre = run["symbols"]
    # refine period with a straight-line fit through the preamble rising edges
    # (drop the run's last period: a small sync shift can still fall within tolerance)
    re_ = rising_edges(sig, start_us - 10, start_us + int((n_pre - 0.5) * T))
    keep = [0] + [i for i in range(1, len(re_)) if abs(re_[i] - re_[i - 1] - T) < 0.02 * T]
    re_ = re_[keep]
    k = np.round((re_ - re_[0]) / T)
    T, first = np.polyfit(k, re_, 1)

    n = 1 << sf
    tol = max(1, n // 512)
    best = None
    # Wrap-anchored mode: preamble wraps are falling edges followed by a long low.
    falls, lows = falling_edges_with_low(sig, max(0, start_us - int(T)), min(len(sig), start_us + int(
        (n_pre + 300) * T)))
    pre = [(f, l) for f, l in zip(falls, lows)
           if f < start_us + (n_pre - 0.5) * T and l > 0.25 * T]
    if len(pre) >= 6:
        pf = np.array([f for f, _ in pre], float)
        # index wraps from the preamble start, seed the offset with a median (robust to
        # stray noise edges), then fit
        kk = np.round((pf - pf[np.argmin(np.abs(pf - start_us - T))]) / T)
        b0, Tw = np.median(pf - kk * T), T
        # drop outliers (e.g. the first sync symbol's wrap, a few chips early) and refit
        for _ in range(3):
            keep = np.abs(pf - (Tw * kk + b0)) < max(3 * Tw / n, 6)
            if keep.sum() < 6:
                break
            Tw, b0 = np.polyfit(kk[keep], pf[keep], 1)
        pf, kk = pf[keep], kk[keep]
        b0 += Tw * kk[0]
        kk -= kk[0]
        resid_w = pf - (Tw * kk + b0)
        # only worth it when wraps are sharp (high data-rate preset); coarse captures
        # (stock 4.8 kBaud, ~26 us grid) do better with the plain template demod
        if keep.sum() >= 6 and np.std(resid_w) < 0.3 * Tw / n:
            d = EdgeDemod(sig, sf, b0, Tw, falls, lows)
            # edge precision from this packet's own preamble (coarse captures are ~0.5+ chip)
            d.SIGMA_CHIPS = max(0.3, 1.5 * np.std(resid_w) / (Tw / n))
            d.build_template(int(kk[-1]) + 1)
            n_pre_w = int(kk[-1]) + 1
            for j0 in range(n_pre_w - 1, n_pre_w + 3):
                p, a, b = (d.symbol(d.t0 + (j0 + i) * Tw) for i in (-1, 0, 1))
                if near(p[0], 8, n, tol) == 0 and near(a[0], 8, n, tol) not in (None, 0) \
                        and near(b[0], 8, n, tol) is not None:
                    best, T = (0, d, j0), Tw
                    break

    # The symbol boundary sits somewhere inside the rising-edge period, and the
    # first preamble symbols may be lost to AGC settling. Search window phase and
    # the slot of the first sync symbol together, scoring how confidently the
    # two sync slots demodulate.
    def try_phase(phase_chip):
        d = SymbolDemod(sig, sf, first - phase_chip * T / n, T)
        d.build_template(n_pre)
        found = None
        for j0 in range(n_pre - 2, n_pre + 3):
            p, a, b = (d.demod(d.t0 + (j0 + i) * T) for i in (-1, 0, 1))
            # sync symbols are 8 * nibble (as a signed 7-bit value, so 0xB -> 88 -> -40),
            # preceded by a preamble symbol (0). Allow a little slack at high SF.
            if near(p[0], 8, n, tol) != 0 or near(a[0], 8, n, tol) in (None, 0) \
                    or near(b[0], 8, n, tol) is None:
                continue
            hdr0 = d.t0 + (j0 + 4.25) * T
            score = a[2] + b[2] + sum(d.demod(hdr0 + i * T)[2] for i in range(8))
            if found is None or score > found[0]:
                found = (score, d, j0)
        return found

    # coarse search over the whole symbol, then refine around the best phase
    step = max(2, n // 64)
    for ph in (range(0, n, step) if best is None else ()):
        f = try_phase(ph)
        if f and (best is None or f[0] > best[0]):
            best, best_ph = f, ph
    if best is not None and best[0] != 0 and step > 2:
        fine = max(1, step // 8)
        for ph in range(best_ph - step + fine, best_ph + step, fine):
            f = try_phase(ph % n)
            if f and f[0] > best[0]:
                best = f
    if best is None:
        return {"sync": None, "length": 0, "cr": 0, "crc": 0, "header_ok": False,
                "T": T, "header_syms": []}
    _, d, j0 = best

    sync = [near(d.symbol(d.t0 + j * T)[0], 8, n, tol) for j in (j0, j0 + 1)]
    mode = "wrap" if isinstance(d, EdgeDemod) else "template"
    edges_used = []
    # header starts after the 2 sync symbols and 2.25 downchirps
    t = d.t0 + (j0 + 2 + 2.25) * T

    raw, confid = [], []
    drift = 0.0

    def next_symbol():
        nonlocal t, drift
        s_, resid, peak, scores, used = d.symbol(t + drift)
        drift += (0.5 if used else 0.3) * resid  # simple timing loop
        raw.append(s_)
        confid.append(peak)
        edges_used.append(used)
        t += T
        return scores

    # --- header: 8 symbols, reduced rate, CR 4/8
    hdr = soft_block([bit_llrs(next_symbol(), sf, True) for _ in range(8)], sf - 2, 8, 4)
    nibs = [x[0] for x in hdr]
    length = (nibs[0] << 4) | nibs[1]
    has_crc = nibs[2] & 1
    cr = nibs[2] >> 1
    chk = ((nibs[3] & 1) << 4) | nibs[4]
    hdr_ok = chk == header_check(nibs) and length > 0 and 1 <= cr <= 4
    out = {
        "sync": sync, "length": length, "cr": cr, "crc": has_crc,
        "header_ok": hdr_ok, "T": T, "header_syms": raw[:8], "mode": mode,
    }
    if not hdr_ok:
        return out

    decided = hdr[5:]
    need = 2 * length + (4 if has_crc else 0)
    while len(decided) < need:
        llrs = [bit_llrs(next_symbol(), sf, False) for _ in range(4 + cr)]
        decided += soft_block(llrs, sf, 4 + cr, cr)
    decided = decided[:need]
    nibs = [x[0] for x in decided]
    data, crc_bytes = assemble(nibs, length, has_crc)
    crc_ok = lora_crc_ok(data, crc_bytes) if has_crc else None

    # List decoding: if the CRC fails, try runner-up nibbles on the least
    # confident codewords.
    flips = 0
    if crc_ok is False:
        weakest = sorted(range(need), key=lambda i: decided[i][1])[:max_flips]
        for mask in range(1, 1 << len(weakest)):
            trial = nibs[:]
            for b, i in enumerate(weakest):
                if mask >> b & 1:
                    trial[i] = decided[i][2]
            dt, cb = assemble(trial, length, has_crc)
            if lora_crc_ok(dt, cb):
                data, crc_ok, flips = dt, True, bin(mask).count("1")
                break

    out.update(
        payload=bytes(data), crc_ok=crc_ok, flips=flips,
        nib_metrics=[x[3] for x in decided],
        weak=sum(1 for x in decided if x[1] < 1e-6),
        n_symbols=len(raw), symbols=raw,
        edge_frac=sum(edges_used) / max(1, len(edges_used)),
    )
    return out


def crc_list_decode(r, validator=None, max_checks=300000, max_cands=48, per_pos=3):
    """Best-first search over nibble corrections, cheapest (by soft metric)
    first, for a payload whose LoRa CRC matches and that `validator(bytes)`
    accepts. The LoRa CRC is affine in the payload bits, so each candidate
    correction's effect on the CRC syndrome is precomputed and the search only
    XORs 16-bit values. Returns (payload, n_corrections, checks) or None."""
    import heapq
    L, nm = r["length"], r["nib_metrics"]
    P = 2 * L + 4
    best = [int(np.argmax(m)) for m in nm[:P]]
    data, crc_b = assemble(best, L, True)
    syndrome = (crc16(data[:-2]) ^ data[-1] ^ (data[-2] << 8)) ^ (crc_b[0] | crc_b[1] << 8)

    # CRC effect of flipping each bit of each payload byte
    bit_delta = []
    for k in range(L):
        row = []
        for bit in range(8):
            e = [0] * L
            e[k] = 1 << bit
            row.append(crc16(e[:-2]) ^ e[-1] ^ (e[-2] << 8))
        bit_delta.append(row)

    def delta(i, x):
        if i >= 2 * L:  # received CRC nibble
            return x << (4 * (i - 2 * L))
        k, shift = i // 2, 4 * (i % 2)
        d = 0
        for bit in range(4):
            if x >> bit & 1:
                d ^= bit_delta[k][bit + shift]
        return d

    cands = []
    for i in range(P):
        m = nm[i]
        order = np.argsort(m)[::-1]
        for v in order[1:per_pos + 1]:
            cands.append((float(m[best[i]] - m[v]), i, int(v)))
    cands.sort()
    cands = cands[:max_cands]
    deltas = [delta(i, best[i] ^ v) for _, i, v in cands]

    def materialize(idx):
        nibs = best[:]
        for j in idx:
            nibs[cands[j][1]] = cands[j][2]
        return bytes(assemble(nibs, L, True)[0])

    checks = 0
    if syndrome == 0 and (validator is None or validator(bytes(data))):
        return bytes(data), 0, 0
    # heap of (penalty, index tuple, syndrome so far); children extend or advance last index
    heap = [(cands[0][0], (0,), deltas[0])] if cands else []
    while heap and checks < max_checks:
        pen, idx, syn = heapq.heappop(heap)
        checks += 1
        positions = [cands[j][1] for j in idx]
        if len(set(positions)) == len(positions) and syn == syndrome:
            payload = materialize(idx)
            if validator is None or validator(payload):
                return payload, len(idx), checks
        last = idx[-1]
        if last + 1 < len(cands):
            heapq.heappush(heap, (pen + cands[last + 1][0], idx + (last + 1,), syn ^ deltas[last + 1]))
            heapq.heappush(heap, (pen - cands[last][0] + cands[last + 1][0],
                                  idx[:-1] + (last + 1,), syn ^ deltas[last] ^ deltas[last + 1]))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--sf", type=int, default=7)
    ap.add_argument("--bw", type=int, default=62500)
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()

    _, durations = load_sub(args.file)
    sig = to_samples(durations)
    period = (1 << args.sf) / args.bw * 1e6
    runs, _ = find_runs(durations, period, 0.05, 6, 0)
    for run in runs:
        r = decode_packet(sig, run, args.sf, args.bw, args.v)
        sync = "none" if not r["sync"] else f"0x{r['sync'][0]:X}{r['sync'][1]:X}"
        print(f"t={run['start_ms']:.1f} ms  T={r['T']:.2f} us  sync={sync}  "
              f"len={r['length']} cr=4/{r['cr'] + 4} crc={r['crc']} header_ok={r['header_ok']}")
        if args.v:
            print("   header symbols:", r["header_syms"])
        if "payload" in r:
            print(f"   crc_ok={r['crc_ok']}  flips={r['flips']}  symbols={r['n_symbols']}")
            print("   " + r["payload"].hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
