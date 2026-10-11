#include "transmit.h"
#include <string.h>
#if defined(__arm__)
#pragma GCC optimize("O2")
#endif

typedef struct {
    LoraTxWave* wave;
    uint64_t phase, denominator;
    unsigned sf, period, bandwidth, chip;
    bool failed;
} Encoder;
static uint8_t* byte_at(LoraTxWave* w, size_t n) {
    if(n < w->first_size) return w->first + n;
    n -= w->first_size;
    if(n < w->second_size) return w->second + n;
    return w->extra + (n - w->second_size);
}
static void chirp(Encoder* e, unsigned shift, bool down, unsigned length) {
    LoraTxWave* w = e->wave;
    if(e->failed) return;
    if(w->cancelled && w->cancelled(w->context)) {
        e->failed = true;
        return;
    }
    if(w->samples + length / w->quantum > (w->first_size + w->second_size + w->extra_size) * 8) {
        e->failed = true;
        return;
    }
    for(unsigned t = 0; t < length; t += w->quantum) {
        uint8_t* b = byte_at(w, w->samples >> 3);
        unsigned bit = w->samples & 7;
        if(!bit) *b = 0;
        if(e->phase < e->denominator / 2) *b |= 1 << bit;
        ++w->samples;
        unsigned fraction = (2 * t + w->quantum + shift * 2 * e->chip) % (2 * e->period);
        if(down) fraction = 2 * e->period - fraction;
        /* Centering LongFast halves the edge rate. Both conjugate chirps are
         * emitted; coherent LoRa demodulation separates their energy. */
        int64_t low = e->sf == 11 ? -125000 : 10000;
        int64_t advance =
            (int64_t)w->quantum * (low * 2 * e->period + (int64_t)e->bandwidth * fraction);
        e->phase = (e->phase + e->denominator + advance) % e->denominator;
    }
}
static void block(Encoder* e, const uint8_t* n, unsigned bits, unsigned cr, bool reduced) {
    uint8_t cw[11][8];
    for(unsigned j = 0; j < bits; ++j) {
        uint8_t* d = cw[j];
        for(unsigned k = 0; k < 4; ++k)
            d[k] = (n[j] >> k) & 1;
        d[4] = cr == 1 ? d[0] ^ d[1] ^ d[2] ^ d[3] : d[0] ^ d[1] ^ d[2];
        d[5] = d[1] ^ d[2] ^ d[3];
        d[6] = d[0] ^ d[1] ^ d[3];
        d[7] = d[0] ^ d[2] ^ d[3];
    }
    for(unsigned i = 0; i < 4 + cr; ++i) {
        unsigned gray = 0;
        for(unsigned j = 0; j < bits; ++j)
            gray = 2 * gray + cw[(i + bits - 1 - j) % bits][i];
        unsigned value = gray;
        for(unsigned shifted = gray >> 1; shifted; shifted >>= 1)
            value ^= shifted;
        chirp(e, (value * (reduced ? 4 : 1) + 1) % (1U << e->sf), false, e->period);
    }
}
bool lora_tx_waveform(LoraTxWave* w, const uint8_t* packet, size_t length, bool mt) {
    w->samples = w->position = 0;
    w->quantum = mt ? 2 : 1;
    if(length < 2 || length > 64 || !w->first) return false;
    Encoder e = {
        .wave = w,
        .sf = mt ? 11 : 7,
        .period = mt ? 8192 : 2048,
        .bandwidth = mt ? 250000 : 62500,
        .chip = mt ? 4 : 16};
    e.denominator = 2ULL * e.period * 1000000;
    e.phase = e.denominator / 4;
    for(unsigned i = 0; i < (mt ? 16U : 32U); ++i)
        chirp(&e, 0, false, e.period);
    chirp(&e, mt ? 16 : 8, false, e.period);
    chirp(&e, mt ? 88 : 16, false, e.period);
    chirp(&e, 0, true, e.period);
    chirp(&e, 0, true, e.period);
    chirp(&e, 0, true, e.period / 4);
    uint8_t h[11] = {(uint8_t)(length >> 4), (uint8_t)(length & 15), 3, 0, 0};
    const uint8_t masks[5][3] = {{15, 0, 0}, {8, 14, 1}, {4, 9, 10}, {2, 5, 7}, {1, 2, 15}};
    unsigned check = 0;
    for(unsigned i = 0; i < 5; ++i) {
        unsigned parity = 0;
        for(unsigned j = 0; j < 3; ++j)
            for(unsigned k = 0; k < 4; ++k)
                parity ^= (h[j] & masks[i][j]) >> k & 1;
        check = check * 2 + parity;
    }
    h[3] = check >> 4;
    h[4] = check & 15;
    uint16_t crc = 0;
    for(size_t i = 0; i < length - 2; ++i) {
        crc ^= (uint16_t)packet[i] << 8;
        for(unsigned j = 0; j < 8; ++j)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    crc ^= packet[length - 1] | ((uint16_t)packet[length - 2] << 8);
    uint8_t nibbles[144] = {0}, whitening = 255;
    for(size_t i = 0; i < length + 2; ++i) {
        uint8_t byte;
        if(i < length) {
            byte = packet[i] ^ whitening;
            whitening =
                (whitening << 1) |
                (((whitening >> 7) ^ (whitening >> 5) ^ (whitening >> 4) ^ (whitening >> 3)) & 1);
        } else
            byte = crc >> (8 * (i - length));
        nibbles[2 * i] = byte & 15;
        nibbles[2 * i + 1] = byte >> 4;
    }
    unsigned carry = e.sf - 7;
    memcpy(h + 5, nibbles, carry);
    block(&e, h, e.sf - 2, 4, true);
    for(size_t i = carry; i < 2 * (length + 2); i += e.sf)
        block(&e, nibbles + i, e.sf, 1, false);
    if(e.failed) {
        w->samples = 0;
        return false;
    }
    /* Validate HAL minimum duration including chirp/symbol boundaries. */
    bool level;
    uint32_t run;
    while((run = lora_tx_next(w, &level))) {
        if(run < 2) {
            if(w->position == w->samples &&
               w->samples < (w->first_size + w->second_size + w->extra_size) * 8) {
                uint8_t* b = byte_at(w, w->samples >> 3);
                unsigned bit = w->samples & 7;
                if(!bit) *b = 0;
                if(level) *b |= 1 << bit;
                ++w->samples;
                break;
            } else {
                w->samples = w->position = 0;
                return false;
            }
        }
    }
    w->position = 0;
    return true;
}
uint32_t lora_tx_next(LoraTxWave* w, bool* level) {
    uint32_t start = w->position, pos = start;
    if(pos >= w->samples) return 0;
    uint8_t value = *byte_at(w, pos >> 3);
    *level = (value >> (pos & 7)) & 1;
    for(;;) {
        unsigned shift = pos & 7, available = 8 - shift;
        unsigned edges = (*level ? (unsigned)(uint8_t)~value : value) >> shift;
        unsigned run = edges ? (unsigned)__builtin_ctz(edges) : available;
        if(run > w->samples - pos) run = w->samples - pos;
        pos += run;
        if(run < available || pos == w->samples) break;
        value = *byte_at(w, pos >> 3);
    }
    w->position = pos;
    return (pos - start) * w->quantum;
}
