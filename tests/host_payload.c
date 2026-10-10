#define _POSIX_C_SOURCE 200809L
#include "../app/payload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static LoraPayload payload;
static LoraPayloadResult best[32];
static unsigned result_count;
static void decoded(const LoraPayloadResult* r, void* ctx) {
    (void)ctx;
    unsigned i = 0;
    for(; i < result_count; ++i)
        if(best[i].detection_time == r->detection_time) break;
    if(i == result_count) {
        if(result_count == 32) return;
        ++result_count;
        best[i] = *r;
    } else if(lora_payload_better(r, &best[i]))
        best[i] = *r;
}
static void detected(const LoraDetection* d, void* ctx) {
    (void)ctx;
    lora_payload_start(&payload, d);
}
int main(int argc, char** argv) {
    if(argc != 2) return 2;
    FILE* f = fopen(argv[1], "r");
    if(!f) return 2;
    LoraDetector d;
    lora_payload_init(&payload, decoded, NULL);
    lora_detector_init(&d, 62500, detected, NULL);
    char* line = NULL;
    size_t size = 0;
    static const float phases[] = {0, -8, -4, 4, 8};
    for(unsigned phase = 0; phase < 5; ++phase) {
        rewind(f);
        lora_payload_init(&payload, decoded, NULL);
        payload.phase_bias = phases[phase];
        lora_detector_init(&d, 62500, detected, NULL);
        while(getline(&line, &size, f) >= 0) {
            if(strncmp(line, "RAW_Data:", 9)) continue;
            char* p = line + 9;
            char* end;
            for(;;) {
                long v = strtol(p, &end, 10);
                if(p == end) break;
                if(!v || v > INT32_MAX || v < -INT32_MAX) return 2;
                lora_payload_feed(&payload, v > 0, (uint32_t)labs(v));
                lora_detector_feed(&d, v > 0, (uint32_t)labs(v));
                p = end;
            }
        }
    }
    for(unsigned i = 0; i < result_count; ++i) {
        const LoraPayloadResult* r = &best[i];
        printf(
            "payload t=%llu status=%d length=%u cr=%u type=%d name=%s quality=%.2f\n",
            (unsigned long long)r->detection_time,
            r->status,
            r->length,
            r->coding_rate,
            r->packet_type,
            r->name,
            (double)r->quality);
    }
    free(line);
    fclose(f);
    return 0;
}
