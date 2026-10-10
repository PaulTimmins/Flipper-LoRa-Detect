#pragma once
#include <stdint.h>

typedef struct {
    const char* name;
    uint32_t frequency;
    uint32_t bandwidth;
    uint8_t preset;
} LoraProfile;

#define LORA_PRESET(mdmcfg4, deviatn)                                              \
    {0x02, 0x0D, 0x0B, 0x06, 0x08, 0x32,    0x07, 0x04,    0x14, 0x00, 0x13, 0x02, \
     0x12, 0x04, 0x11, 0xF8, 0x10, mdmcfg4, 0x15, deviatn, 0x18, 0x18, 0x19, 0x14, \
     0x1D, 0x91, 0x1C, 0x00, 0x1B, 0x07,    0x20, 0xFB,    0x22, 0x10, 0x21, 0x56, \
     0x00, 0x00, 0xC0, 0x00, 0x00, 0x00,    0x00, 0x00,    0x00, 0x00}
/* Mutable arrays because the SDK preset API takes uint8_t*, but does not modify it. */
static uint8_t lora_presets[][46] = {
    LORA_PRESET(0xAB, 0x42),
    LORA_PRESET(0x9B, 0x52),
    LORA_PRESET(0x5B, 0x62),
    LORA_PRESET(0x1B, 0x72),
};
static const LoraProfile lora_profiles[] = {
    {"MeshCore US/CA", 910525000, 62500, 0},
    {"Mt LongFast", 906875000, 250000, 2},
    {"Mt MediumFast", 913125000, 250000, 2},
    {"Mt MediumSlow", 914875000, 250000, 2},
    {"Mt ShortFast", 918875000, 250000, 2},
    {"Mt ShortSlow", 920625000, 250000, 2},
    {"Mt LongTurbo", 908750000, 500000, 3},
    {"Mt MediumTurbo", 925250000, 500000, 3},
    {"Mt ShortTurbo", 926750000, 500000, 3},
    {"Mt LongModerate", 902687500, 125000, 1},
    {"Mt LongSlow", 905312500, 125000, 1},
};
#define LORA_PROFILE_COUNT (sizeof(lora_profiles) / sizeof(lora_profiles[0]))
