#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t sf;
    uint32_t bandwidth;
    uint32_t preamble_symbols;
    int16_t sync; /* -1 if either nibble could not be read */
    float period_us;
    uint64_t time_us;
} LoraDetection;
typedef void (*LoraDetected)(const LoraDetection* event, void* context);

typedef struct {
    uint64_t first, last;
    uint32_t periods;
    float period;
    bool have_last, collecting;
    int nibble[2];
    float error[2];
    uint32_t low[2];
} LoraTrack;
typedef struct {
    uint32_t bandwidth;
    uint64_t time_us;
    LoraTrack tracks[6];
    LoraDetected callback;
    void* context;
} LoraDetector;

/* Reset after a retune or ANY lost samples; never bridge a capture discontinuity. */
void lora_detector_init(LoraDetector*, uint32_t bandwidth, LoraDetected, void* context);
/* A completed high/low pulse, with the same sign convention as Flipper RAW. */
void lora_detector_feed(LoraDetector*, bool high, uint32_t duration_us);
/* End of capture: report a locked preamble even if sync is incomplete. */
void lora_detector_finish(LoraDetector*);
const char* lora_detection_label(const LoraDetection*);
