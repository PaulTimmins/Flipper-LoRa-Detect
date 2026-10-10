#pragma once
#include "detector.h"
#include <stddef.h>

#define LORA_PAYLOAD_SAMPLES 1024
#define LORA_PAYLOAD_PULSES  256
#define LORA_PAYLOAD_NIBBLES 514
#define LORA_PAYLOAD_METRICS 268

typedef enum {
    LoraPayloadHeaderFailed,
    LoraPayloadDamaged,
    LoraPayloadCrcOk,
} LoraPayloadStatus;
typedef struct {
    uint64_t detection_time;
    LoraPayloadStatus status;
    uint8_t length, coding_rate;
    float quality;
    int8_t packet_type;
    uint8_t hops, node_type, combined_copies;
    char name[33];
    uint8_t data[255];
} LoraPayloadResult;
typedef void (*LoraPayloadCallback)(const LoraPayloadResult*, void*);
typedef struct {
    uint64_t end;
    uint32_t duration;
    bool high;
} LoraPayloadPulse;
typedef struct {
    LoraPayloadPulse pulses[LORA_PAYLOAD_PULSES];
    unsigned pulse_write, pulse_count;
    uint64_t time_us, origin;
    LoraDetection detection;
    bool active, header, has_crc;
    unsigned sample_count, block_count, nibble_count, need, symbols;
    float symbol_start, period, drift, phase_bias;
    int32_t template_sum[LORA_PAYLOAD_SAMPLES + 1];
    unsigned template_count;
    float re[LORA_PAYLOAD_SAMPLES];
    float llr[8][7];
    uint8_t nibbles[LORA_PAYLOAD_NIBBLES];
    int8_t metrics[LORA_PAYLOAD_METRICS][16];
    int16_t advert_metrics[264][16];
    uint8_t advert_body[132];
    unsigned advert_length, advert_copies;
    uint64_t advert_time;
    LoraPayloadResult result;
    bool (*cancelled)(void*); /* optional, checked at each symbol for prompt shutdown */
    LoraPayloadCallback callback;
    void* context;
} LoraPayload;

void lora_payload_init(LoraPayload*, LoraPayloadCallback, void*);
/* Start a best-effort SF7/62.5 kHz MeshCore decode; other PHYs remain detections. */
bool lora_payload_start(LoraPayload*, const LoraDetection*);
void lora_payload_feed(LoraPayload*, bool high, uint32_t duration);
/* Strict bounds-checked protocol parsing, also usable by desktop tests. */
bool lora_payload_describe(LoraPayloadResult*);

/* Prefer independently CRC-checked, structurally valid, then better-correlated results. */
bool lora_payload_better(const LoraPayloadResult* candidate, const LoraPayloadResult* previous);
