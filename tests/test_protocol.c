#include "../app/protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "public_chat_vectors.h"
static const uint8_t public_key[16] =
    {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
static LoraPayloadResult frame(const uint8_t* plain, size_t n) {
    LoraPayloadResult r = {0};
    r.meshtastic = true;
    r.status = LoraPayloadCrcOk;
    r.length = 16 + n;
    r.data[0] = 0xff;
    r.data[4] = 0x12;
    r.data[8] = 7;
    r.data[13] = 8;
    memcpy(r.data + 16, plain, n);
    uint8_t nonce[16] = {7, 0, 0, 0, 0, 0, 0, 0, 0x12};
    lora_aes128_ctr(r.data + 16, n, public_key, nonce);
    return r;
}
int main(void) {
    LoraPayloadResult query_reply = {
        .status = LoraPayloadDamaged, .packet_type = 13, .decoded_bytes = 40};
    query_reply.data[0] = 0x2e;
    query_reply.data[2] = 0x92;
    query_reply.data[4] = 12;
    assert(lora_query_reply_state(&query_reply, 12) == 1);
    assert(lora_query_reply_state(&query_reply, 13) == 0);
    query_reply.status = LoraPayloadCrcOk;
    assert(lora_query_reply_state(&query_reply, 12) == 2);
    query_reply.data[1] = 1;
    assert(lora_query_reply_state(&query_reply, 12) == 0);
    query_reply.meshtastic = true;
    query_reply.request_id = 12;
    assert(lora_query_reply_state(&query_reply, 12) == 2);
    query_reply.request_id = 0;
    assert(lora_query_reply_state(&query_reply, 12) == 0);

    /* NIST SP 800-38A AES-128 CTR first two blocks. */
    const uint8_t key[16] = {
        0x2b,
        0x7e,
        0x15,
        0x16,
        0x28,
        0xae,
        0xd2,
        0xa6,
        0xab,
        0xf7,
        0x15,
        0x88,
        0x09,
        0xcf,
        0x4f,
        0x3c};
    uint8_t nonce[16] = {
        0xf0,
        0xf1,
        0xf2,
        0xf3,
        0xf4,
        0xf5,
        0xf6,
        0xf7,
        0xf8,
        0xf9,
        0xfa,
        0xfb,
        0xfc,
        0xfd,
        0xfe,
        0xff};
    uint8_t b[32] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e,
                     0x11, 0x73, 0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03,
                     0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51};
    const uint8_t expected[32] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68,
                                  0x64, 0x99, 0x0d, 0xb6, 0xce, 0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70,
                                  0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff};
    lora_aes128_ctr(b, sizeof(b), key, nonce);
    assert(!memcmp(b, expected, sizeof(b)));
    uint8_t query[] = {8, 70, 18, 0, 24, 1};
    LoraPayloadResult r = frame(query, sizeof(query));
    assert(lora_mt_describe(&r, false) && r.packet_type == 32);
    assert(lora_mt_describe(&r, true) && r.packet_type == 40);
    r.status = LoraPayloadDamaged;
    assert(lora_mt_describe(&r, true) && r.packet_type == 32);
    uint8_t reply[] = {8, 70, 18, 0, 53, 7, 0, 0, 0};
    r = frame(reply, sizeof(reply));
    assert(lora_mt_describe(&r, true) && r.packet_type == 41);
    uint8_t neighbors[] = {8, 71, 18, 0};
    r = frame(neighbors, sizeof(neighbors));
    assert(lora_mt_describe(&r, true) && r.packet_type == 37);
    uint8_t route[] = {8, 70, 18, 16, 10, 8, 1, 0,  0, 0, 2, 0, 0,
                       0, 26, 4,  3,  0,  0, 0, 53, 7, 0, 0, 0};
    r = frame(route, sizeof(route));
    assert(
        lora_mt_describe(&r, true) && r.packet_type == 41 && strstr(r.detail, "route 2 / back 1"));
    uint8_t bad_route[] = {8, 70, 18, 3, 10, 1, 0};
    r = frame(bad_route, sizeof(bad_route));
    assert(lora_mt_describe(&r, true) && r.packet_type == 32);
    uint8_t two_neighbors[] = {8, 71, 18, 4, 34, 0, 34, 0};
    r = frame(two_neighbors, sizeof(two_neighbors));
    assert(lora_mt_describe(&r, true) && r.packet_type == 37 && strstr(r.detail, "neighbors 2"));
    uint8_t heartbeat[] = {8, 65, 18, 7, 8, 2, 34, 3, 8, 0xb0, 4};
    r = frame(heartbeat, sizeof(heartbeat));
    assert(
        lora_mt_describe(&r, true) && r.packet_type == 42 && strstr(r.detail, "Router heartbeat"));
    uint8_t ack[] = {8, 5, 18, 2, 24, 0, 53, 7, 0, 0, 0};
    r = frame(ack, sizeof(ack));
    assert(lora_mt_describe(&r, true) && r.packet_type == 43);
    ack[5] = 3;
    r = frame(ack, sizeof(ack));
    assert(lora_mt_describe(&r, true) && r.packet_type == 44 && strstr(r.detail, "result 3"));
    uint8_t other[] = {8, 100, 18, 0};
    r = frame(other, sizeof(other));
    assert(lora_mt_describe(&r, true) && r.packet_type == 45 && strstr(r.detail, "port 100"));
    uint8_t bad_ack[] = {8, 5, 18, 2, 24, 0x80};
    r = frame(bad_ack, sizeof(bad_ack));
    assert(lora_mt_describe(&r, true) && r.packet_type == 32);
    uint8_t node[] = {8, 4, 18, 6, 18, 4, 'T', 'e', 's', 't'};
    r = frame(node, sizeof(node));
    assert(lora_mt_describe(&r, true) && r.packet_type == 34 && !strcmp(r.name, "Test"));
    uint8_t malformed[] = {8, 70, 18, 255};
    r = frame(malformed, sizeof(malformed));
    assert(lora_mt_describe(&r, true) && r.packet_type == 32);
    memset(&r, 0, sizeof(r));
    r.length = 8;
    r.data[0] = 0x2e;
    r.data[2] = 0x80;
    r.data[3] = 4;
    assert(lora_payload_describe(&r) && r.packet_type == 12);
    r.data[2] = 0x92;
    r.length = 16;
    assert(lora_payload_describe(&r) && r.packet_type == 13 && r.node_type == 2);
    r.length = 9;
    assert(lora_payload_describe(&r) && r.packet_type == 11); /* truncated reply */
    memset(&r, 0, sizeof(r));
    r.length = r.decoded_bytes = sizeof(mc_short);
    r.payload_complete = true;
    r.status = LoraPayloadDamaged;
    memcpy(r.data, mc_short, sizeof(mc_short));
    assert(!lora_mc_public_describe(&r, false) && !r.text[0]);
    assert(lora_mc_public_describe(&r, true) && r.packet_type == 16 && r.public_mac_ok);
    assert(!strcmp(r.text, "Alice: Hello Public") && !strcmp(r.name, "Alice"));
    assert(r.status == LoraPayloadDamaged && !memcmp(r.data, mc_short, sizeof(mc_short)));
    r.data[r.length - 1] ^= 1;
    assert(!lora_mc_public_describe(&r, true) && !r.text[0] && !r.public_mac_ok);
    memcpy(r.data, mc_short, sizeof(mc_short));
    r.data[2] = 0x12;
    assert(!lora_mc_public_describe(&r, true));
    memcpy(r.data, mc_short, sizeof(mc_short));
    r.data[1] = 0xff;
    assert(!lora_mc_public_describe(&r, true));
    memcpy(r.data, mc_short, sizeof(mc_short));
    --r.length;
    assert(!lora_mc_public_describe(&r, true));
    r.length = sizeof(mc_short);
    r.payload_complete = false;
    assert(!lora_mc_public_describe(&r, true));
    r.payload_complete = true;
    r.length = r.decoded_bytes = sizeof(mc_long);
    memcpy(r.data, mc_long, sizeof(mc_long));
    assert(lora_mc_public_describe(&r, true) && strlen(r.text) == 235 && !strcmp(r.name, "Bob"));
    uint8_t text_message[] = {8, 1, 18, 5, 'H', 'e', 'l', 'l', 'o'};
    r = frame(text_message, sizeof(text_message));
    assert(lora_mt_describe(&r, true) && !strcmp(r.text, "Hello"));
    /* NIST SP 800-38A AES-128 ECB vector, independent of protocol fixtures. */
    uint8_t ecb[16] = {
        0x3a,
        0xd7,
        0x7b,
        0xb4,
        0x0d,
        0x7a,
        0x36,
        0x60,
        0xa8,
        0x9e,
        0xca,
        0xf3,
        0x24,
        0x66,
        0xef,
        0x97};
    const uint8_t clear[16] = {
        0x6b,
        0xc1,
        0xbe,
        0xe2,
        0x2e,
        0x40,
        0x9f,
        0x96,
        0xe9,
        0x3d,
        0x7e,
        0x11,
        0x73,
        0x93,
        0x17,
        0x2a};
    lora_aes128_ecb_decrypt(ecb, 16, key);
    assert(!memcmp(ecb, clear, 16));
    uint32_t rng = 8;
    for(unsigned i = 0; i < 10000; ++i) {
        r.length = i % 256;
        r.status = LoraPayloadCrcOk;
        for(unsigned j = 0; j < r.length; ++j) {
            rng = rng * 1664525u + 1013904223u;
            r.data[j] = rng >> 24;
        }
        lora_mt_describe(&r, true);
        r.meshtastic = false;
        r.payload_complete = true;
        lora_mc_public_describe(&r, true);
        assert(memchr(r.name, 0, sizeof(r.name)) && memchr(r.detail, 0, sizeof(r.detail)));
    }
    puts("PASS AES vectors, crypto toggle, MT query/reply/node, MC discovery and parser bounds");
}
