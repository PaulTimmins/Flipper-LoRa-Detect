#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LoraTxMcText,
    LoraTxMcQuery,
    LoraTxMtText,
    LoraTxMtQuery,
    LoraTxMcCompose,
    LoraTxMtCompose
} LoraTxAction;
/* Waveform storage borrows capture/decoder RAM only while their worker is idle. */
typedef struct {
    uint8_t *first, *second;
    size_t first_size, second_size;
    uint32_t samples, position;
    unsigned quantum;
    bool (*cancelled)(void*);
    void* context;
} LoraTxWave;
size_t lora_mc_test_packet(uint8_t out[64], uint32_t sequence, uint32_t timestamp, bool flood);
size_t lora_tx_packet(
    uint8_t out[64],
    LoraTxAction action,
    uint32_t sequence,
    uint32_t timestamp,
    uint32_t node,
    bool flood);
bool lora_tx_waveform(LoraTxWave*, const uint8_t*, size_t, bool meshtastic);
/* A zero duration means end of waveform. Callable from the radio DMA refill. */
uint32_t lora_tx_next(LoraTxWave*, bool* level);

#define LORA_MC_MESSAGE_MAX 34
#define LORA_MT_MESSAGE_MAX 44
size_t lora_mc_message_packet(uint8_t out[64], const char* text, uint32_t timestamp, bool flood);
size_t lora_tx_message(
    uint8_t out[64],
    bool mt,
    const char* text,
    uint32_t sequence,
    uint32_t timestamp,
    uint32_t node,
    bool flood);
