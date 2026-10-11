#pragma once
#include <stddef.h>
#include <stdint.h>
#include "payload.h"

/* Returns zero if the supplied output buffer cannot hold a complete entry. */
size_t lora_report_format(
    char* out,
    size_t capacity,
    const LoraPayloadResult* result,
    const char* detected_at,
    uint32_t frequency,
    int rssi,
    const char* traffic_type);

/* Up to 32 recovered bytes per chunk, uppercase hex, no separators.
 * Returns zero at end or for insufficient output capacity. */
size_t lora_report_hex(char* out, size_t capacity, const LoraPayloadResult* result, size_t offset);
