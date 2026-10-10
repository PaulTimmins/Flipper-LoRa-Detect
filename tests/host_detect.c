#define _POSIX_C_SOURCE 200809L
#include "../app/detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void detected(const LoraDetection* e, void* ctx) {
    ++*(unsigned*)ctx;
    printf(
        "t=%.3f SF%u BW=%" PRIu32 " preamble=%" PRIu32 " sync=%02X T=%.3f %s\n",
        e->time_us / 1e6,
        e->sf,
        e->bandwidth,
        e->preamble_symbols,
        (unsigned)(uint16_t)e->sync,
        e->period_us,
        lora_detection_label(e));
}
int main(int argc, char** argv) {
    if(argc != 3) {
        fprintf(stderr, "usage: %s capture.sub bandwidth_hz\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "r");
    if(!f) {
        perror(argv[1]);
        return 2;
    }
    unsigned count = 0;
    LoraDetector d;
    lora_detector_init(&d, (uint32_t)strtoul(argv[2], NULL, 10), detected, &count);
    char* line = NULL;
    size_t size = 0;
    while(getline(&line, &size, f) >= 0) {
        if(strncmp(line, "RAW_Data:", 9)) continue;
        char* p = line + 9;
        char* end;
        for(;;) {
            long v = strtol(p, &end, 10);
            if(end == p) break;
            if(v == 0 || v > INT32_MAX || v < -INT32_MAX) {
                fclose(f);
                free(line);
                return 2;
            }
            lora_detector_feed(&d, v > 0, (uint32_t)labs(v));
            p = end;
        }
    }
    lora_detector_finish(&d);
    free(line);
    fclose(f);
    fprintf(stderr, "%u detections\n", count);
    return 0;
}
