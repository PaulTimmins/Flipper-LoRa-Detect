/* The firmware default is -Os. On Cortex-M4, optimizing this compute-heavy
 * translation unit for speed reduced a three-packet replay from 5.91 to 5.24 s.
 * Keep standard floating-point semantics: weak-signal scoring is sensitive to
 * changes in rounding and reassociation (do not enable fast-math).
 */
#if defined(__GNUC__) && !defined(__clang__) && defined(__arm__)
#pragma GCC optimize("O2")
#endif

#include "payload.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "protocol.h"

#define N LORA_PAYLOAD_SAMPLES

void lora_payload_init(LoraPayload* p, LoraPayloadCallback callback, void* context) {
    memset(p, 0, sizeof(*p));
    p->callback = callback;
    p->context = context;
}

static bool sample_at(const LoraPayload* p, uint64_t t, float* value) {
    /* Pulse ends are ordered: binary search avoids a full ring scan per sample. */
    unsigned lo = 0, hi = p->pulse_count;
    unsigned first = (p->pulse_write + LORA_PAYLOAD_PULSES - p->pulse_count) % LORA_PAYLOAD_PULSES;
    while(lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if(p->pulses[(first + mid) % LORA_PAYLOAD_PULSES].end <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    if(lo == p->pulse_count) return false;
    const LoraPayloadPulse* pulse = &p->pulses[(first + lo) % LORA_PAYLOAD_PULSES];
    if(t < pulse->end - pulse->duration) return false;
    *value = pulse->high ? 1 : -1;
    return true;
}

bool lora_payload_supported(const LoraDetection* e) {
    return (e->sf == 7 && e->bandwidth == 62500 && e->sync == 0x12) ||
           (e->sf >= 7 && e->sf <= 11 && e->bandwidth >= 125000 && e->bandwidth <= 500000 &&
            e->sync == 0x2b && e->period_us <= 16000);
}

/* Fit symbol boundaries to stable falling preamble edges. Coarse/chattery
 * captures must retain correlation-only decoding rather than over-trust edges. */
static bool meshcore_edge_clock(
    const LoraPayload* p,
    const LoraDetection* e,
    float* period,
    float* anchor) {
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    unsigned count = 0;
    for(unsigned k = 0; k < p->pulse_count; ++k) {
        const LoraPayloadPulse* pulse = &p->pulses[k];
        if(pulse->high || pulse->duration < e->period_us * 0.25f ||
           pulse->end < pulse->duration + e->time_us)
            continue;
        float y = pulse->end - pulse->duration - e->time_us;
        if(y < e->period_us * 0.5f || y > (e->preamble_symbols - 1.5f) * e->period_us) continue;
        float x = roundf(y / e->period_us);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++count;
    }
    float denom = count * sxx - sx * sx;
    if(count < 6 || denom <= 0) return false;
    float fitted = (count * sxy - sx * sy) / denom;
    float offset = (sy - fitted * sx) / count;
    if(fabsf(fitted - e->period_us) > 0.02f * e->period_us) return false;
    float error = 0;
    for(unsigned k = 0; k < p->pulse_count; ++k) {
        const LoraPayloadPulse* pulse = &p->pulses[k];
        if(pulse->high || pulse->duration < e->period_us * 0.25f ||
           pulse->end < pulse->duration + e->time_us)
            continue;
        float y = pulse->end - pulse->duration - e->time_us;
        if(y < e->period_us * 0.5f || y > (e->preamble_symbols - 1.5f) * e->period_us) continue;
        float residual = y - (offset + fitted * roundf(y / e->period_us));
        error += residual * residual;
    }
    float limit = 0.3f * fitted / (1u << e->sf);
    if(error > count * limit * limit) return false;
    *period = fitted;
    *anchor = offset;
    return true;
}

bool lora_payload_start(LoraPayload* p, const LoraDetection* e) {
    if(!lora_payload_supported(e) || p->active) return false;
    float period = e->period_us, anchor = 0;
    p->edge_refine = e->sync == 0x2b || meshcore_edge_clock(p, e, &period, &anchor);
    memset(p->template_sum, 0, sizeof(p->template_sum));
    unsigned symbols = 0;
    /* The pulse ring includes preamble and sync. Discard the first and last
     * preamble symbols to avoid AGC settling and sync boundary contamination. */
    for(unsigned k = 1; k + 1 < e->preamble_symbols; ++k) {
        bool complete = true;
        for(unsigned j = 0; j < N; ++j) {
            uint64_t t = e->time_us + (uint64_t)lroundf(anchor + (k + (j + 0.5f) / N) * period);
            if(!sample_at(p, t, &p->re[j])) {
                complete = false;
                break;
            }
        }
        if(complete) {
            for(unsigned j = 0; j < N; ++j)
                p->template_sum[j + 1] += (int)p->re[j];
            ++symbols;
        }
    }
    if(symbols < 4) return false;
    for(unsigned j = 0; j < N; ++j)
        p->template_sum[j + 1] += p->template_sum[j];
    p->template_count = symbols;
    p->detection = *e;
    p->origin = e->time_us;
    p->period = period;
    p->symbol_start = (e->preamble_symbols - 1 + 4.25f) * p->period + anchor + p->phase_bias;
    p->drift = 0;
    p->sample_count = p->block_count = p->nibble_count = p->symbols = 0;
    p->header = p->active = true;
    memset(&p->result, 0, sizeof(p->result));
    p->result.detection_time = e->time_us;
    p->result.sf = e->sf;
    p->result.packet_type = -1;
    p->result.meshtastic = e->sync == 0x2b;
    return true;
}

static uint8_t hamming(unsigned nibble, unsigned cr) {
    unsigned a = nibble & 1, b = (nibble >> 1) & 1, c = (nibble >> 2) & 1, d = nibble >> 3;
    if(cr == 1) return (a << 4) | (b << 3) | (c << 2) | (d << 1) | (a ^ b ^ c ^ d);
    return ((a << 7) | (b << 6) | (c << 5) | (d << 4) | ((a ^ b ^ c) << 3) | ((b ^ c ^ d) << 2) |
            ((a ^ b ^ d) << 1) | (a ^ c ^ d)) >>
           (4 - cr);
}
static uint8_t header_check(const uint8_t* n) {
    static const uint8_t masks[5][3] = {{15, 0, 0}, {8, 14, 1}, {4, 9, 10}, {2, 5, 7}, {1, 2, 15}};
    unsigned check = 0;
    for(unsigned i = 0; i < 5; ++i) {
        unsigned parity = 0;
        for(unsigned j = 0; j < 3; ++j) {
            unsigned v = n[j] & masks[i][j];
            for(; v; v >>= 1)
                parity ^= v & 1;
        }
        check = (check << 1) | parity;
    }
    return check;
}
static uint16_t crc16(const uint8_t* data, unsigned length) {
    uint16_t crc = 0;
    for(unsigned j = 0; j < length; ++j) {
        crc ^= (uint16_t)data[j] << 8;
        for(unsigned k = 0; k < 8; ++k)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    return crc;
}

static bool describe_payload(LoraPayloadResult* r) {
    r->packet_type = -1;
    r->name[0] = 0;
    r->detail[0] = 0;
    if(r->meshtastic) return lora_mt_describe(r, false);
    if(r->length < 2 || (r->data[0] >> 6) != 0) return false;
    unsigned route = r->data[0] & 3;
    unsigned offset = (route == 0 || route == 3) ? 5 : 1;
    if(offset >= r->length) return false;
    unsigned path = r->data[offset++];
    unsigned bytes = (path & 63) * ((path >> 6) + 1);
    if(bytes > r->length - offset) return false;
    offset += bytes;
    r->packet_type = (r->data[0] >> 2) & 15;
    r->hops = path & 63;
    if(r->packet_type == 11) {
        unsigned len = r->length - offset;
        if(len >= 6 && (r->data[offset] & 0xf0) == 0x80) {
            r->packet_type = 12;
            snprintf(
                r->detail,
                sizeof(r->detail),
                "Tag %02X%02X%02X%02X filter %02X",
                r->data[offset + 5],
                r->data[offset + 4],
                r->data[offset + 3],
                r->data[offset + 2],
                r->data[offset + 1]);
        } else if(len >= 14 && (r->data[offset] & 0xf0) == 0x90) {
            r->node_type = r->data[offset] & 15;
            r->packet_type = r->node_type == 2 ? 13 : 14;
            snprintf(
                r->detail,
                sizeof(r->detail),
                "Tag %02X%02X%02X%02X key %02X%02X%02X%02X",
                r->data[offset + 5],
                r->data[offset + 4],
                r->data[offset + 3],
                r->data[offset + 2],
                r->data[offset + 6],
                r->data[offset + 7],
                r->data[offset + 8],
                r->data[offset + 9]);
        } else if(len) {
            snprintf(
                r->detail, sizeof(r->detail), "Control 0x%02X (%u bytes)", r->data[offset], len);
        }
    } else if(r->packet_type == 0 || r->packet_type == 1) {
        if(r->length - offset >= 4)
            snprintf(
                r->detail,
                sizeof(r->detail),
                "Encrypted %02X > %02X; subtype unknown",
                r->data[offset + 1],
                r->data[offset]);
        else
            snprintf(r->detail, sizeof(r->detail), "Truncated encrypted header");
    } else if(r->packet_type == 7) {
        if(r->length - offset >= 35)
            snprintf(
                r->detail,
                sizeof(r->detail),
                "Encrypted to %02X; subtype unknown",
                r->data[offset]);
        else
            snprintf(r->detail, sizeof(r->detail), "Truncated anonymous header");
    }
    if(r->packet_type != 4) return true;
    if(r->length - offset < 101) {
        r->packet_type = -1;
        return false;
    }
    unsigned flags = r->data[offset + 100];
    r->node_type = flags & 15;
    offset += 101;
    unsigned extras =
        ((flags & 0x10) ? 8 : 0) + ((flags & 0x20) ? 2 : 0) + ((flags & 0x40) ? 2 : 0);
    if(extras > r->length - offset) return false;
    offset += extras;
    if(flags & 0x80) {
        unsigned len = r->length - offset;
        if(len > 32) return false;
        /* The monochrome UI font is ASCII. Replace non-printable/UTF-8 bytes. */
        for(unsigned j = 0; j < len; ++j) {
            uint8_t c = r->data[offset + j];
            r->name[j] = c >= 32 && c < 127 ? (char)c : '?';
        }
        r->name[len] = 0;
    }
    return true;
}

/* A corrupt route length must not hide a readable advert name. Only salvage
 * a unique, structurally plausible layout; bytes and CRC status stay unchanged. */
bool lora_payload_describe(LoraPayloadResult* r) {
    bool described = describe_payload(r);
    if(r->meshtastic || r->status != LoraPayloadDamaged || !r->payload_complete || r->name[0] ||
       r->length < 106 || (r->data[0] >> 6) != 0 || ((r->data[0] >> 2) & 15) != 4)
        return described;
    unsigned found = 0, name_start = 0, node_type = 0;
    for(unsigned offset = 2; offset + 101 < r->length; ++offset) {
        unsigned flags = r->data[offset + 100];
        if(!(flags & 0x80) || (flags & 15) < 1 || (flags & 15) > 4) continue;
        unsigned start = offset + 101 + ((flags & 0x10) ? 8 : 0) + ((flags & 0x20) ? 2 : 0) +
                         ((flags & 0x40) ? 2 : 0);
        if(start >= r->length) continue;
        unsigned len = r->length - start, printable = 0, letters = 0;
        if(len < 3 || len > 32) continue;
        for(unsigned i = start; i < r->length; ++i) {
            unsigned c = r->data[i];
            printable += c >= 32 && c < 127;
            letters += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }
        if(printable * 4 < len * 3 || letters < 3) continue;
        ++found;
        name_start = start;
        node_type = flags & 15;
    }
    if(found != 1) return described;
    unsigned len = r->length - name_start;
    for(unsigned i = 0; i < len; ++i) {
        unsigned c = r->data[name_start + i];
        r->name[i] = c >= 32 && c < 127 ? (char)c : '#';
    }
    r->name[len] = 0;
    r->packet_type = 4;
    r->node_type = node_type;
    snprintf(r->detail, sizeof(r->detail), "Tentative name; layout inferred");
    return true;
}

/* Repeated adverts carry the same signed body but different routing prefixes.
 * Combining improves readability only: without Ed25519 verification, these
 * remain explicitly tentative. Never promote combined data to CRC-checked. */
static void combine_advert(LoraPayload* p) {
    LoraPayloadResult* r = &p->result;
    if(r->length < 103 || (r->data[0] >> 6) != 0 || ((r->data[0] >> 2) & 15) != 4) return;
    unsigned offset = 0;
    if(p->advert_copies && p->advert_copies < 16 && r->length >= p->advert_length + 2 &&
       r->detection_time - p->advert_time < 5000000) {
        offset = r->length - p->advert_length;
        unsigned different = 0, time_different = 0;
        for(unsigned j = 0; j < 32; ++j)
            different += r->data[offset + j] != p->advert_body[j];
        for(unsigned j = 32; j < 36; ++j)
            time_different += r->data[offset + j] != p->advert_body[j];
        if(different > 8 || time_different > 2) offset = 0;
    }
    if(!offset) {
        unsigned route = r->data[0] & 3;
        unsigned start = (route == 0 || route == 3) ? 5 : 1;
        unsigned path = r->data[start];
        offset = start + 1 + (path & 63) * ((path >> 6) + 1);
        if(offset + 101 > r->length || r->length - offset > 132) return;
        p->advert_length = r->length - offset;
        p->advert_copies = 0;
        memset(p->advert_metrics, 0, sizeof(p->advert_metrics));
        memcpy(p->advert_body, r->data + offset, p->advert_length);
    }
    unsigned whitening = 255;
    for(unsigned byte = 0; byte < r->length; ++byte) {
        if(byte >= offset)
            for(unsigned half = 0; half < 2; ++half) {
                unsigned mask = half ? whitening >> 4 : whitening & 15;
                unsigned row = 2 * (byte - offset) + half;
                for(unsigned n = 0; n < 16; ++n)
                    p->advert_metrics[row][n] +=
                        p->metrics[(2 * byte + half) % LORA_PAYLOAD_METRICS][n ^ mask];
            }
        unsigned fb = ((whitening >> 7) ^ (whitening >> 5) ^ (whitening >> 4) ^ (whitening >> 3)) &
                      1;
        whitening = ((whitening << 1) | fb) & 255;
    }
    ++p->advert_copies;
    p->advert_time = r->detection_time;
    for(unsigned row = 0; row < 2 * p->advert_length; ++row) {
        unsigned best = 0;
        for(unsigned n = 1; n < 16; ++n)
            if(p->advert_metrics[row][n] > p->advert_metrics[row][best]) best = n;
        if(row & 1)
            p->advert_body[row / 2] |= best << 4;
        else
            p->advert_body[row / 2] = best;
    }
    if(p->advert_copies > 1 && r->status != LoraPayloadCrcOk) {
        LoraPayloadResult combined = {0};
        combined.length = p->advert_length + 2;
        combined.data[0] = 0x11;
        memcpy(combined.data + 2, p->advert_body, p->advert_length);
        if(lora_payload_describe(&combined)) {
            r->packet_type = combined.packet_type;
            r->node_type = combined.node_type;
            memcpy(r->name, combined.name, sizeof(r->name));
            r->combined_copies = p->advert_copies;
        }
    }
}

static void finish(LoraPayload* p, LoraPayloadStatus status) {
    p->active = false;
    p->result.status = status;
    if(p->symbols) p->result.quality /= p->symbols;
    if(!p->result.payload_complete && !p->header) {
        /* Export only complete recovered bytes, never zero-filled placeholders. */
        unsigned whitening = 255;
        unsigned count = p->nibble_count / 2;
        if(count > p->result.length) count = p->result.length;
        p->result.decoded_bytes = count;
        for(unsigned i = 0; i < count; ++i) {
            p->result.data[i] = (p->nibbles[2 * i] | (p->nibbles[2 * i + 1] << 4)) ^ whitening;
            unsigned fb =
                ((whitening >> 7) ^ (whitening >> 5) ^ (whitening >> 4) ^ (whitening >> 3)) & 1;
            whitening = ((whitening << 1) | fb) & 255;
        }
    }
    if(p->result.payload_complete) {
        lora_payload_describe(&p->result);
        if(!p->result.meshtastic) combine_advert(p);
    }
    if(p->callback) p->callback(&p->result, p->context);
}

void lora_payload_end(LoraPayload* p) {
    if(p->active) finish(p, p->header ? LoraPayloadHeaderFailed : LoraPayloadDamaged);
}

static void decode_block(LoraPayload* p) {
    unsigned sf = p->header ? p->detection.sf - 2 : p->detection.sf;
    unsigned cr = p->header ? 4 : p->result.coding_rate;
    unsigned width = 4 + cr;
    uint8_t nibs[11];
    float scores[16];
    for(unsigned row = 0; row < sf; ++row) {
        float best = -1e30f;
        nibs[row] = 0;
        for(unsigned nib = 0; nib < 16; ++nib) {
            unsigned cw = hamming(nib, cr);
            float score = 0;
            for(unsigned i = 0; i < width; ++i) {
                unsigned j = (i + sf - 1 - row) % sf;
                float llr = p->llr[i][j];
                score += (cw & (1u << (width - 1 - i))) ? llr : -llr;
            }
            scores[nib] = score;
            if(score > best) {
                best = score;
                nibs[row] = nib;
            }
        }
        if(!p->header && p->nibble_count + row < p->need) {
            for(unsigned n = 0; n < 16; ++n) {
                float metric = (scores[n] - best) / 4;
                p->metrics[(p->nibble_count + row) % LORA_PAYLOAD_METRICS][n] =
                    (int8_t)fmaxf(-127, metric);
            }
        }
    }
    if(p->header) {
        p->result.length = (nibs[0] << 4) | nibs[1];
        p->result.coding_rate = nibs[2] >> 1;
        p->has_crc = nibs[2] & 1;
        if(header_check(nibs) != ((nibs[3] & 1) << 4 | nibs[4]) || p->result.length < 2 ||
           p->result.coding_rate < 1 || p->result.coding_rate > 4) {
            finish(p, LoraPayloadHeaderFailed);
            return;
        }
        p->need = 2 * p->result.length + (p->has_crc ? 4 : 0);
        p->header = false;
        p->result.crc_present = p->has_crc;
        /* SF>7 header blocks carry the first payload nibbles too. */
        for(unsigned j = 5; j < sf && p->nibble_count < p->need; ++j)
            p->nibbles[p->nibble_count++] = nibs[j];
    } else {
        for(unsigned j = 0; j < sf && p->nibble_count < p->need; ++j)
            p->nibbles[p->nibble_count++] = nibs[j];
    }
    p->block_count = 0;
    if(p->nibble_count >= p->need) {
        unsigned whitening = 255;
        for(unsigned i = 0; i < p->result.length; ++i) {
            p->result.data[i] = (p->nibbles[2 * i] | (p->nibbles[2 * i + 1] << 4)) ^ whitening;
            unsigned fb =
                ((whitening >> 7) ^ (whitening >> 5) ^ (whitening >> 4) ^ (whitening >> 3)) & 1;
            whitening = ((whitening << 1) | fb) & 255;
        }
        unsigned n = p->result.length;
        uint16_t expected = p->nibbles[2 * n] | (p->nibbles[2 * n + 1] << 4) |
                            (p->nibbles[2 * n + 2] << 8) | (p->nibbles[2 * n + 3] << 12);
        uint16_t actual = crc16(p->result.data, n - 2) ^ p->result.data[n - 1] ^
                          (p->result.data[n - 2] << 8);
        p->result.decoded_bytes = p->result.length;
        p->result.payload_complete = true;
        p->result.received_crc = expected;
        p->result.computed_crc = actual;
        finish(p, p->has_crc && actual == expected ? LoraPayloadCrcOk : LoraPayloadDamaged);
    }
}

static void symbol(LoraPayload* p) {
    if(p->cancelled && p->cancelled(p->context)) {
        p->active = false;
        return;
    }
    /* The discriminator waveform is piecewise constant. Integrating a
     * preamble-template prefix sum over its runs gives the exact cyclic
     * correlation using integers, without FFT roundoff or a quadratic loop. */
    uint16_t edges[64];
    unsigned count = 0;
    int initial = p->re[0] > 0 ? 1 : -1;
    for(unsigned i = 1; i < N; ++i) {
        if(p->re[i] != p->re[i - 1]) {
            if(count == 64) {
                finish(p, LoraPayloadDamaged);
                return;
            }
            edges[count++] = i;
        }
    }
    int32_t total = p->template_sum[N];
    for(unsigned x = 0; x < N; ++x) {
        int sign = initial;
        int32_t correlation = initial * total;
        for(unsigned j = 0; j < count; ++j) {
            unsigned start = (edges[j] + x) % N;
            int32_t sum = p->template_sum[x] - p->template_sum[start];
            if(start > x) sum += total;
            correlation -= 2 * sign * sum;
            sign = -sign;
        }
        p->re[x] = (float)correlation / p->template_count;
    }
    unsigned peak = 0;
    for(unsigned j = 1; j < N; ++j)
        if(p->re[j] > p->re[peak]) peak = j;
    p->result.quality += p->re[peak];
    ++p->symbols;
    unsigned chips = 1u << p->detection.sf;
    float exact = (float)peak * chips / N;
    float residual = (exact - roundf(exact)) * (p->period / chips);
    bool refined = false;
    float edge_exact = 0;
    if(p->edge_refine) {
        /* Use sharp discriminator wrap edges to resolve sub-chip timing.
         * Correlation provides a prior; short chatter lows are rejected. */
        float tc = p->period / chips;
        float expected_tau = p->period - roundf(exact) * tc;
        if(expected_tau <= 0) expected_tau += p->period;
        float best_prior = -1e30f;
        for(unsigned k = 0; k < p->pulse_count; ++k) {
            const LoraPayloadPulse* pulse = &p->pulses[k];
            if(pulse->high || pulse->duration < 8 || pulse->end < pulse->duration + p->origin)
                continue;
            float tau =
                (float)(pulse->end - pulse->duration - p->origin) - p->symbol_start - p->drift;
            if(tau < 0.5f * tc || tau > p->period + 0.5f * tc) continue;
            float dist = fabsf(tau - expected_tau);
            dist = fminf(dist, fabsf(p->period - dist));
            if(dist > fmaxf(8 * tc, 40)) continue;
            float prior = -(dist / tc) * (dist / tc) / 32 + fminf(pulse->duration, 200) / 50;
            if(prior > best_prior) {
                best_prior = prior;
                edge_exact = chips - tau / tc;
                if(edge_exact < 0) edge_exact += chips;
            }
        }
        if(best_prior > -1e29f) {
            p->drift += 0.5f * (roundf(edge_exact) - edge_exact) * tc;
            refined = true;
        }
    }
    if(!refined) p->drift += 0.3f * residual;
    unsigned sf = p->header ? p->detection.sf - 2 : p->detection.sf;
    float hi[11], lo[11];
    for(unsigned bit = 0; bit < sf; ++bit)
        hi[bit] = lo[bit] = -1e30f;
    unsigned bins = refined ? chips : N;
    for(unsigned j = 0; j < bins; ++j) {
        unsigned sym = refined ? j : ((j * chips + N / 2) / N) & (chips - 1);
        unsigned index = refined ? ((j * N + chips / 2) / chips) % N : j;
        float score = p->re[index];
        if(refined) {
            float dist = fabsf((float)j - edge_exact);
            dist = fminf(dist, chips - dist);
            score = fmaxf(
                (score - p->re[peak]) * chips / (8 * N) - 3, -dist * dist / (2 * 0.35f * 0.35f));
        }
        unsigned v = (sym + chips - 1) & (chips - 1);
        if(p->header) v /= 4;
        unsigned gray = v ^ (v >> 1);
        for(unsigned bit = 0; bit < sf; ++bit) {
            if(gray & (1u << (sf - bit - 1))) {
                if(score > hi[bit]) hi[bit] = score;
            } else if(score > lo[bit])
                lo[bit] = score;
        }
    }
    for(unsigned bit = 0; bit < sf; ++bit)
        p->llr[p->block_count][bit] = hi[bit] - lo[bit];
    ++p->block_count;
    if(p->block_count == (p->header ? 8u : 4u + p->result.coding_rate)) decode_block(p);
    p->sample_count = 0;
    p->symbol_start += p->period;
}

void lora_payload_feed(LoraPayload* p, bool high, uint32_t duration) {
    uint64_t begin = p->time_us;
    p->time_us += duration;
    LoraPayloadPulse* pulse = &p->pulses[p->pulse_write];
    pulse->end = p->time_us;
    pulse->duration = duration;
    pulse->high = high;
    p->pulse_write = (p->pulse_write + 1) % LORA_PAYLOAD_PULSES;
    if(p->pulse_count < LORA_PAYLOAD_PULSES) ++p->pulse_count;
    while(p->active) {
        float offset = p->symbol_start + p->drift + (p->sample_count + 0.5f) * p->period / N;
        uint64_t t = p->origin + (uint64_t)lroundf(offset);
        if(t >= p->time_us) break;
        float value = high ? 1 : -1;
        if(t < begin && !sample_at(p, t, &value)) {
            finish(p, LoraPayloadDamaged);
            break;
        }
        p->re[p->sample_count++] = value;
        if(p->sample_count == N) symbol(p);
    }
}

bool lora_payload_better(const LoraPayloadResult* candidate, const LoraPayloadResult* previous) {
    if(candidate->status != previous->status) return candidate->status > previous->status;
    if(candidate->public_mac_ok != previous->public_mac_ok) return candidate->public_mac_ok;
    if(candidate->payload_complete != previous->payload_complete)
        return candidate->payload_complete;
    if(!candidate->payload_complete && candidate->decoded_bytes != previous->decoded_bytes)
        return candidate->decoded_bytes > previous->decoded_bytes;
    if((candidate->packet_type >= 0) != (previous->packet_type >= 0))
        return candidate->packet_type >= 0;
    if((candidate->name[0] != 0) != (previous->name[0] != 0)) return candidate->name[0] != 0;
    return candidate->quality > previous->quality;
}
