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
