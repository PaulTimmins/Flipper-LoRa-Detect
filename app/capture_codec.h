#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Common pulses fit in 15 duration bits plus level. Zero escapes a full
 * 32-bit pulse, preserving long gaps and zero-duration values losslessly. */
static inline bool
    capture_pack(uint16_t* data, unsigned* count, unsigned capacity, uint32_t word) {
    uint32_t duration = word & 0x7fffffffU;
    unsigned need = duration && duration <= 32767 ? 1 : 3;
    if(*count > capacity || capacity - *count < need) return false;
    if(need == 1) {
        data[(*count)++] = (word >> 16 & 0x8000) | duration;
    } else {
        data[(*count)++] = 0;
        data[(*count)++] = word;
        data[(*count)++] = word >> 16;
    }
    return true;
}
static inline bool
    capture_unpack(const uint16_t* data, unsigned count, unsigned* at, uint32_t* word) {
    if(*at >= count) return false;
    uint16_t value = data[*at];
    if(value) {
        *word = ((uint32_t)(value & 0x8000) << 16) | (value & 0x7fff);
        ++*at;
    } else {
        if(count - *at < 3) return false;
        *word = data[*at + 1] | ((uint32_t)data[*at + 2] << 16);
        *at += 3;
    }
    return true;
}
