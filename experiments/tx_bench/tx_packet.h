#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define TX_WAVE_CAPACITY 40000U
size_t bench_packet(uint8_t out[64], uint32_t sequence, uint32_t timestamp, bool flood);
size_t bench_waveform(
    const uint8_t* packet,
    size_t length,
    unsigned cr,
    uint8_t* out,
    size_t capacity,
    uint32_t* duration);
