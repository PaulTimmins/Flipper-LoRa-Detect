#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LORA_NAME_MAX 16
/* Private material stays on the SD card, never in logs or shipped assets. */
typedef struct {
    uint8_t device_uid[12];
    uint32_t mt_node;
    uint8_t mc_private[64], mc_public[32];
    uint8_t mt_private[32], mt_public[32];
} LoraIdentity;
bool lora_name_valid(const char* name);
unsigned lora_mc_message_limit(const char* name);
void lora_identity_generate(LoraIdentity* identity);
