#include "../app/detector.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

static unsigned count;
static LoraDetection last;
static void detected(const LoraDetection* e, void* context) {
    (void)context;
    ++count;
    last = *e;
}
static void preamble(LoraDetector* d, unsigned sf, uint32_t bw, unsigned symbols) {
    unsigned period = (unsigned)round((1u << sf) * 1e6 / bw);
    for(unsigned i = 0; i < symbols; ++i) {
        lora_detector_feed(d, true, period / 2);
        lora_detector_feed(d, false, period - period / 2);
    }
}
static void advance_to(LoraDetector* d, uint64_t time, bool high) {
    assert(time > d->time_us);
    lora_detector_feed(d, high, (uint32_t)(time - d->time_us));
}
int main(void) {
    LoraDetector d;
    /* All supported SF/BW pairs, no aliases at double or half the period. */
    const uint32_t bws[] = {62500, 125000, 250000, 500000};
    for(unsigned b = 0; b < 4; ++b) {
        for(unsigned sf = 7; sf <= 12; ++sf) {
            count = 0;
            lora_detector_init(&d, bws[b], detected, NULL);
            preamble(&d, sf, bws[b], 16);
            lora_detector_finish(&d);
            assert(count == 1 && last.sf == sf && last.preamble_symbols == 16);
            assert(last.sync == -1);
            lora_detector_finish(&d);
            assert(count == 1);
        }
    }
    /* Signed sync2 wraps just AFTER g, not g+T (the captured LongFast case). */
    for(unsigned sf = 7; sf <= 12; ++sf) {
        count = 0;
        lora_detector_init(&d, 250000, detected, NULL);
        preamble(&d, sf, 250000, 16);
        uint64_t period = (1u << sf) * 4;
        uint64_t g = d.tracks[sf - 7].last + period;
        advance_to(&d, g - 64, true);
        lora_detector_feed(&d, false, 32);
        advance_to(&d, g + 160, true);
        lora_detector_feed(&d, false, (uint32_t)(period / 2));
        advance_to(&d, g + 3 * period, true);
        assert(count == 1 && last.sf == sf && last.sync == 0x2b);
    }
    /* Positive sync nibbles near the end of each symbol. */
    count = 0;
    lora_detector_init(&d, 62500, detected, NULL);
    preamble(&d, 7, 62500, 32);
    uint64_t g = d.tracks[0].last + 2048;
    advance_to(&d, g - 128, true);
    lora_detector_feed(&d, false, 1000);
    advance_to(&d, g + 2048 - 256, true);
    lora_detector_feed(&d, false, 1000);
    advance_to(&d, g + 6144, true);
    assert(count == 1 && last.sync == 0x12 && last.preamble_symbols == 32);
    /* Retune/loss cannot stitch two sub-threshold runs together. */
    count = 0;
    lora_detector_init(&d, 62500, detected, NULL);
    preamble(&d, 7, 62500, 5);
    lora_detector_init(&d, 62500, detected, NULL);
    preamble(&d, 7, 62500, 5);
    lora_detector_finish(&d);
    assert(count == 0);
    /* Deterministic noise, plus a clock beyond the 32-bit microsecond rollover. */
    uint32_t rng = 7;
    for(unsigned i = 0; i < 100000; ++i) {
        rng = rng * 1664525u + 1013904223u;
        lora_detector_feed(&d, (i & 1) != 0, 16 + rng % 400);
    }
    lora_detector_finish(&d);
    assert(count == 0);
    lora_detector_init(&d, 62500, detected, NULL);
    d.time_us = UINT64_C(5000000000);
    preamble(&d, 7, 62500, 32);
    lora_detector_finish(&d);
    assert(count == 1 && last.time_us > UINT32_MAX && last.preamble_symbols == 32);
    puts("PASS synthetic SF/BW, sync, reset, noise, and long-uptime tests");
    return 0;
}
