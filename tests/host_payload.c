#define _POSIX_C_SOURCE 200809L
#include "../app/payload.h"
#include "../app/protocol.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static LoraPayload payload;
static LoraPayloadResult best[32];
static unsigned result_count, edge_starts, correlation_starts;
static void decoded(const LoraPayloadResult* r, void* ctx) {
    (void)ctx;
    if(r->payload_complete)
        assert(r->decoded_bytes == r->length);
    else
        assert(r->packet_type == -1);
    if(r->status == LoraPayloadCrcOk)
        assert(r->payload_complete && r->crc_present && r->received_crc == r->computed_crc);
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
    if(lora_payload_start(&payload, d)) {
        if(payload.edge_refine)
            ++edge_starts;
        else
            ++correlation_starts;
    }
}
int main(int argc, char** argv) {
    if(argc < 2 || argc > 3) return 2;
    unsigned bw = argc == 3 ? (unsigned)atoi(argv[2]) : 62500;
    FILE* f = fopen(argv[1], "r");
    if(!f) return 2;
    LoraDetector d;
    lora_payload_init(&payload, decoded, NULL);
    lora_detector_init(&d, bw, detected, NULL);
    char* line = NULL;
    size_t size = 0;
    static const float phases[] = {0, -8, -4, 4, 8};
    for(unsigned phase = 0; phase < 5; ++phase) {
        rewind(f);
        lora_payload_init(&payload, decoded, NULL);
        payload.phase_bias = phases[phase] * 62500.0f / bw;
        lora_detector_init(&d, bw, detected, NULL);
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
        LoraPayloadResult* r = &best[i];
        if(r->meshtastic && r->payload_complete) lora_mt_describe(r, true);
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
    printf("timing edge=%u correlation=%u\n", edge_starts, correlation_starts);
    free(line);
    fclose(f);
    return 0;
}
