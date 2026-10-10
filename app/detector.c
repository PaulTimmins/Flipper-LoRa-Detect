#include "detector.h"
#include <math.h>
#include <string.h>

static void clear_candidates(LoraTrack* t) {
    for(unsigned j = 0; j < 2; ++j) {
        t->nibble[j] = -1;
        t->error[j] = 2.0;
        t->low[j] = 0;
    }
}

void lora_detector_init(LoraDetector* d, uint32_t bw, LoraDetected cb, void* ctx) {
    memset(d, 0, sizeof(*d));
    d->bandwidth = bw;
    d->callback = cb;
    d->context = ctx;
}

static void emit(LoraDetector* d, unsigned i) {
    LoraTrack* t = &d->tracks[i];
    LoraDetection event = {
        .sf = (uint8_t)(i + 7),
        .bandwidth = d->bandwidth,
        .preamble_symbols = t->periods + 1,
        .sync = t->nibble[0] < 0 || t->nibble[1] < 0 ? -1 : (t->nibble[0] << 4) | t->nibble[1],
        .period_us = t->period,
        .time_us = t->first,
    };
    memset(t, 0, sizeof(*t));
    if(d->callback) d->callback(&event, d->context);
}

void lora_detector_feed(LoraDetector* d, bool high, uint32_t duration) {
    if(!duration || !d->bandwidth) return;
    uint64_t edge = d->time_us;
    d->time_us += duration;
    const float chip = 1000000.0 / d->bandwidth;
    const float tolerance = fmaxf(3 * chip, 14.0);
    for(unsigned i = 0; i < 6; ++i) {
        LoraTrack* t = &d->tracks[i];
        const float nominal = (1u << (i + 7)) * chip;
        bool wrap = !high && duration >= nominal / 8;
        if(wrap && !t->collecting) {
            float delta = (float)(edge - t->last);
            if(t->have_last && fabsf(delta - (t->periods ? t->period : nominal)) <= tolerance) {
                ++t->periods;
                t->last = edge;
                t->period = (float)(edge - t->first) / t->periods;
                clear_candidates(t);
            } else if(t->periods >= 6) {
                t->collecting = true;
            } else {
                t->first = t->last = edge;
                t->periods = 0;
                t->period = nominal;
                t->have_last = true;
                clear_candidates(t);
            }
        }
        if(t->periods >= 6) {
            /* Keep all falls, including short sync lows before the first long
             * off-period wrap. Use relative times to retain precision at uptime. */
            if(!high && edge > t->last) {
                float elapsed = (float)(edge - t->last);
                for(unsigned j = 0; j < 2; ++j) {
                    float phase = (j + 1) * t->period - elapsed;
                    if(phase < 0 || phase >= t->period) continue;
                    float shift = phase / (t->period / (1u << (i + 7)));
                    if(shift > (1u << (i + 7)) / 2.0) shift -= (1u << (i + 7));
                    if(shift < -65.5 || shift > 57.5) continue;
                    int n = (int)roundf(shift / 8);
                    float error = fabsf(shift - 8 * n);
                    if(error <= 1.5 &&
                       (error < t->error[j] ||
                        (fabsf(error - t->error[j]) < 0.001 && duration > t->low[j]))) {
                        t->error[j] = error;
                        t->nibble[j] = n & 15;
                        t->low[j] = duration;
                    }
                }
            }
            if((float)(d->time_us - t->last) > 2.7 * t->period) emit(d, i);
        } else if(t->have_last && (float)(d->time_us - t->last) > 2 * nominal) {
            memset(t, 0, sizeof(*t));
        }
    }
}

void lora_detector_finish(LoraDetector* d) {
    for(unsigned i = 0; i < 6; ++i) {
        if(d->tracks[i].periods >= 6)
            emit(d, i);
        else
            memset(&d->tracks[i], 0, sizeof(d->tracks[i]));
    }
}

const char* lora_detection_label(const LoraDetection* e) {
    if(e->sync == 0x12) {
        if(e->bandwidth == 62500 && e->sf == 7) return "MeshCore US/CA";
        if(e->bandwidth == 62500 && e->sf == 8) return "MeshCore EU/UK";
        return "MeshCore-like";
    }
    if(e->sync == 0x2b) {
        if(e->bandwidth == 250000) {
            switch(e->sf) {
            case 7:
                return "Mt ShortFast";
            case 8:
                return "Mt ShortSlow";
            case 9:
                return "Mt MediumFast";
            case 10:
                return "Mt MediumSlow";
            case 11:
                return "Mt LongFast";
            }
        }
        if(e->bandwidth == 125000 && e->sf == 11) return "Mt LongModerate";
        if(e->bandwidth == 125000 && e->sf == 12) return "Mt LongSlow";
        if(e->bandwidth == 500000) {
            if(e->sf == 7) return "Mt ShortTurbo";
            if(e->sf == 9) return "Mt MediumTurbo";
            if(e->sf == 11) return "Mt LongTurbo";
        }
        return "Meshtastic-like";
    }
    if(e->sync == 0x34) return "LoRaWAN-like";
    return "LoRa";
}
