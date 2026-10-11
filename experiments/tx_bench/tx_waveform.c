#include "tx_packet.h"
#include <string.h>

typedef struct {
    uint8_t* out;
    size_t capacity, count;
    uint32_t phase, run, duration;
    bool level, failed;
} Wave;
static void chirp(Wave* w, unsigned shift, bool down, unsigned length) {
    const uint32_t denominator = 4096000000U;
    for(unsigned t = 0; t < length; ++t) {
        bool level = w->phase < denominator / 2;
        if(level != w->level && w->run) {
            if(w->count >= w->capacity || w->run < 2 || w->run > 255) {
                w->failed = true;
                return;
            }
            w->out[w->count++] = w->run;
            w->run = 0;
        }
        w->level = level;
        ++w->run;
        ++w->duration;
        unsigned fraction = (2 * t + 1 + shift * 32) % 4096;
        if(down) fraction = 4096 - fraction;
        uint32_t advance = 40960000U + 62500U * fraction;
        w->phase = w->phase >= denominator - advance ? w->phase - (denominator - advance) :
                                                       w->phase + advance;
    }
}
static void block(Wave* w, const uint8_t* n, unsigned bits, unsigned cr, bool reduced) {
    uint8_t cw[7][8];
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
        chirp(w, (value * (reduced ? 4 : 1) + 1) % 128, false, 2048);
    }
}
size_t bench_waveform(
    const uint8_t* packet,
    size_t length,
    unsigned cr,
    uint8_t* out,
    size_t capacity,
    uint32_t* duration) {
    if(length < 2 || length > 64 || (cr != 1 && cr != 4)) return 0;
    Wave w = {.out = out, .capacity = capacity, .phase = 1024000000U, .level = true};
    for(unsigned i = 0; i < 32; ++i)
        chirp(&w, 0, false, 2048);
    chirp(&w, 8, false, 2048);
    chirp(&w, 16, false, 2048);
    chirp(&w, 0, true, 2048);
    chirp(&w, 0, true, 2048);
    chirp(&w, 0, true, 512);
    uint8_t h[5] = {(uint8_t)(length >> 4), (uint8_t)(length & 15), (uint8_t)(cr * 2 + 1), 0, 0};
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
    block(&w, h, 5, 4, true);
    uint16_t crc = 0;
    for(size_t i = 0; i < length - 2; ++i) {
        crc ^= (uint16_t)packet[i] << 8;
        for(unsigned j = 0; j < 8; ++j)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    crc ^= packet[length - 1] | ((uint16_t)packet[length - 2] << 8);
    uint8_t nibbles[140] = {0}, whitening = 255;
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
    for(size_t i = 0; i < 2 * (length + 2); i += 7)
        block(&w, nibbles + i, 7, cr, false);
    if(w.run == 1) {
        ++w.run;
        ++w.duration;
    }
    if(w.failed || w.count >= capacity || w.run > 255) return 0;
    out[w.count++] = w.run;
    *duration = w.duration;
    return w.count;
}
