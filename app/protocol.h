#pragma once
#include "payload.h"
/* Only public/default channel PSK; never guesses private keys. */
bool lora_mt_describe(LoraPayloadResult*, bool decrypt_default);
void lora_aes128_ctr(uint8_t* data, size_t size, const uint8_t key[16], uint8_t nonce[16]);
const char* lora_traffic_type(int type);

bool lora_mc_public_describe(LoraPayloadResult*, bool enabled);
void lora_aes128_ecb_decrypt(uint8_t*, size_t, const uint8_t key[16]);

/* 0: no match; 1: matching but damaged MC reply; 2: CRC-checked match. */
unsigned lora_query_reply_state(const LoraPayloadResult*, uint32_t tag);
