#include "transmit.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>

/* MeshCore Public: AES-128 ECB and truncated HMAC-SHA256 over ciphertext.
 * The 16-byte PSK is zero-extended to 32 bytes for HMAC (same HMAC key block).
 * See MeshCore src/Utils.cpp and src/helpers/BaseChatMesh.cpp. */
static const uint8_t public_key[16] =
    {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
static const uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
static uint32_t rotr(uint32_t v, unsigned n) {
    return (v >> n) | (v << (32 - n));
}
static void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19};
    size_t padded = (len + 9 + 63) & ~(size_t)63;
    for(size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for(unsigned i = 0; i < 16; ++i) {
            uint32_t v = 0;
            for(unsigned j = 0; j < 4; ++j) {
                size_t pos = off + 4 * i + j;
                uint8_t b = pos < len ? data[pos] : pos == len ? 0x80 : 0;
                if(pos >= padded - 8)
                    b = (uint8_t)(((uint64_t)len * 8) >> ((padded - 1 - pos) * 8));
                v = (v << 8) | b;
            }
            w[i] = v;
        }
        for(unsigned i = 16; i < 64; ++i) {
            uint32_t a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + (rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3)) + w[i - 7] +
                   (rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10));
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for(unsigned i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                          k256[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    for(unsigned i = 0; i < 32; ++i)
        out[i] = h[i / 4] >> (24 - 8 * (i % 4));
}
bool lora_mc_public_describe(LoraPayloadResult* r, bool enabled) {
    r->public_mac_ok = false;
    r->text[0] = 0;
    if(!enabled || r->meshtastic || !r->payload_complete || r->length < 21 ||
       (r->data[0] >> 6) != 0 || ((r->data[0] >> 2) & 15) != 5)
        return false;
    unsigned route = r->data[0] & 3;
    unsigned offset = (route == 0 || route == 3) ? 5 : 1;
    unsigned path = r->data[offset++];
    offset += (path & 63) * ((path >> 6) + 1);
    if(offset + 3 >= r->length || r->data[offset] != 0x11) return false;
    unsigned len = r->length - offset - 3;
    if(!len || len % 16 || len > 240) return false;
    const uint8_t* cipher = r->data + offset + 3;
    uint8_t buffer[304], digest[32];
    memset(buffer, 0x36, 64);
    for(unsigned i = 0; i < 16; ++i)
        buffer[i] ^= public_key[i];
    memcpy(buffer + 64, cipher, len);
    sha256(buffer, 64 + len, digest);
    memset(buffer, 0x5c, 64);
    for(unsigned i = 0; i < 16; ++i)
        buffer[i] ^= public_key[i];
    memcpy(buffer + 64, digest, 32);
    sha256(buffer, 96, digest);
    if((digest[0] ^ r->data[offset + 1]) | (digest[1] ^ r->data[offset + 2])) return false;
    memcpy(buffer, cipher, len);
    lora_aes128_ecb_decrypt(buffer, len, public_key);
    if((buffer[4] >> 2) != 0) return false;
    unsigned text_len = 0;
    while(text_len < len - 5 && buffer[5 + text_len])
        ++text_len;
    if(!text_len) return false;
    for(unsigned i = 0; i < text_len; ++i) {
        uint8_t c = buffer[5 + i];
        r->text[i] = c >= 32 && c != 127 ? c : ' ';
    }
    r->text[text_len] = 0;
    r->name[0] = 0;
    const char* colon = strchr(r->text, ':');
    if(colon && colon - r->text <= 32) {
        unsigned n = colon - r->text;
        for(unsigned i = 0; i < n; ++i)
            r->name[i] = (uint8_t)r->text[i] < 127 ? r->text[i] : '?';
        r->name[n] = 0;
    }
    for(unsigned i = 0; i < sizeof(r->detail) - 1; ++i) {
        uint8_t c = r->text[i];
        r->detail[i] = c < 127 ? c : '?';
        if(!c) break;
    }
    r->detail[sizeof(r->detail) - 1] = 0;
    r->public_mac_ok = true;
    r->packet_type = 16;
    return true;
}

static size_t mc_text_packet(uint8_t out[64], const char* text, uint32_t timestamp, bool flood) {
    size_t n = strlen(text);
    if(!n || n > 43) return 0;
    uint8_t plain[48] = {0};
    for(unsigned i = 0; i < 4; ++i)
        plain[i] = timestamp >> (8 * i);
    memcpy(plain + 5, text, n);
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

size_t lora_mc_test_packet(uint8_t out[64], uint32_t sequence, uint32_t timestamp, bool flood) {
    char text[44];
    int n = snprintf(text, sizeof(text), "Flipper bench: TEST %lu", (unsigned long)sequence);
    if(n < 0 || n >= (int)sizeof(text)) return 0;
    return mc_text_packet(out, text, timestamp, flood);
}
size_t lora_mc_message_packet(uint8_t out[64], const char* text, uint32_t timestamp, bool flood) {
    if(!text || !text[0] || strlen(text) > 34) return 0;
    char named[44];
    snprintf(named, sizeof(named), "Flipper: %s", text);
    return mc_text_packet(out, named, timestamp, flood);
}

size_t lora_mc_named_packet(
    uint8_t out[64],
    const char* name,
    const char* text,
    uint32_t timestamp,
    bool flood) {
    unsigned limit = lora_mc_message_limit(name);
    if(!limit || !text || !text[0] || strlen(text) > limit) return 0;
    char named[44];
    snprintf(named, sizeof(named), "%s: %s", name, text);
    return mc_text_packet(out, named, timestamp, flood);
}
