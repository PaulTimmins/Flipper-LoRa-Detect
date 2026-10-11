/* Reuse this repository's portable crypto inside the isolated FAP. The receive
 * app sources stay unchanged. Unused receive entry points are linker-stripped. */
#include "../../app/protocol.c"
#include "../../app/mc_public.c"
#include "tx_packet.h"

size_t bench_packet(uint8_t out[64], uint32_t sequence, uint32_t timestamp, bool flood) {
    uint8_t plain[48] = {0};
    for(unsigned i = 0; i < 4; ++i)
        plain[i] = timestamp >> (8 * i);
    int n = snprintf(
        (char*)plain + 5, sizeof(plain) - 5, "Flipper bench: TEST %lu", (unsigned long)sequence);
    if(n < 0 || n >= (int)sizeof(plain) - 5) return 0;
    size_t len = ((size_t)n + 5 + 15) & ~(size_t)15;
    out[0] = flood ? 0x15 : 0x16;
    out[1] = 0;
    out[2] = 0x11;
    for(size_t off = 0; off < len; off += 16) {
        /* CTR's first keystream block is AES(key, counter). With a zero input
         * and plaintext as the counter this gives exactly one ECB block. */
        uint8_t block[16];
        memcpy(block, plain + off, 16);
        memset(out + 5 + off, 0, 16);
        lora_aes128_ctr(out + 5 + off, 16, public_key, block);
    }
    uint8_t buffer[112], digest[32];
    memset(buffer, 0x36, 64);
    for(unsigned i = 0; i < 16; ++i)
        buffer[i] ^= public_key[i];
    memcpy(buffer + 64, out + 5, len);
    sha256(buffer, 64 + len, digest);
    memset(buffer, 0x5c, 64);
    for(unsigned i = 0; i < 16; ++i)
        buffer[i] ^= public_key[i];
    memcpy(buffer + 64, digest, 32);
    sha256(buffer, 96, digest);
    memcpy(out + 3, digest, 2);
    return len + 5;
}
