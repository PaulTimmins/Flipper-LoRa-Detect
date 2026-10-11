#include "transmit.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

static void put32(uint8_t* out, uint32_t n) {
    for(unsigned i = 0; i < 4; ++i)
        out[i] = n >> (8 * i);
}
static size_t make_packet(
    uint8_t out[64],
    LoraTxAction action,
    uint32_t sequence,
    uint32_t timestamp,
    uint32_t node,
    bool flood,
    const char* message) {
    if(action == LoraTxMcText) return lora_mc_test_packet(out, sequence, timestamp, flood);
    memset(out, 0, 64);
    if(action == LoraTxMcQuery) {
        /* CONTROL, direct empty path; full key response; repeater type mask. */
        out[0] = 0x2e;
        out[2] = 0x80;
        out[3] = 1 << 2;
        put32(out + 4, sequence);
        return 12; /* includes optional since=0 */
    }
    if(action != LoraTxMtText && action != LoraTxMtQuery) return 0;
    if(node < 4 || node == UINT32_MAX || !sequence) return 0;
    memset(out, 255, 4);
    put32(out + 4, node);
    put32(out + 8, sequence);
    /* Queries always zero-hop. Messages optionally flood up to three hops. */
    out[12] = (action == LoraTxMtText && flood) ? (3 | (3 << 5)) : 0;
    out[13] = 0x08; /* default LongFast channel hash */
    uint8_t* data = out + 16;
    data[0] = 8;
    data[1] = action == LoraTxMtQuery ? 4 : 1;
    data[2] = 18;
    size_t len;
    if(action == LoraTxMtText) {
        size_t n;
        if(message) {
            n = strlen(message);
            if(!n || n > LORA_MT_MESSAGE_MAX) return 0;
            memcpy(data + 4, message, n);
        } else {
            int count = snprintf((char*)data + 4, 44, "Flipper TEST %lu", (unsigned long)sequence);
            if(count < 0 || count >= 44) return 0;
            n = count;
        }
        data[3] = n;
        len = 4 + n;
    } else {
        /* User protobuf: identifiable test name, short name; unlicensed client. */
        static const uint8_t user[] = {0x12, 12,  'F', 'l', 'i',  'p', 'p', 'e', 'r', ' ',
                                       't',  'e', 's', 't', 0x1a, 4,   'F', 'L', 'I', 'P'};
        data[3] = sizeof(user);
        memcpy(data + 4, user, sizeof(user));
        len = 4 + sizeof(user);
        data[len++] = 24;
        data[len++] = 1; /* Data.want_response */
    }
    static const uint8_t key[16] = {
        0xd4,
        0xf1,
        0xbb,
        0x3a,
        0x20,
        0x29,
        0x07,
        0x59,
        0xf0,
        0xbc,
        0xff,
        0xab,
        0xcf,
        0x4e,
        0x69,
        0x01};
    uint8_t nonce[16] = {0};
    put32(nonce, sequence);
    put32(nonce + 8, node);
    lora_aes128_ctr(data, len, key, nonce);
    return len + 16;
}

size_t lora_tx_packet(
    uint8_t out[64],
    LoraTxAction action,
    uint32_t sequence,
    uint32_t timestamp,
    uint32_t node,
    bool flood,
    uint8_t mc_hop_bytes) {
    if(mc_hop_bytes < 1 || mc_hop_bytes > 3) return 0;
    size_t length = make_packet(out, action, sequence, timestamp, node, flood, NULL);
    if(length && (action == LoraTxMcText || action == LoraTxMcQuery))
        out[1] = (mc_hop_bytes - 1) << 6; /* Width with zero initial hops. */
    return length;
}
size_t lora_tx_message(
    uint8_t out[64],
    bool mt,
    const char* text,
    uint32_t sequence,
    uint32_t timestamp,
    uint32_t node,
    bool flood,
    uint8_t mc_hop_bytes) {
    if(mc_hop_bytes < 1 || mc_hop_bytes > 3) return 0;
    if(!text || !text[0]) return 0;
    if(!mt) {
        size_t length = lora_mc_message_packet(out, text, timestamp, flood);
        if(length) out[1] = (mc_hop_bytes - 1) << 6;
        return length;
    }
    return make_packet(out, LoraTxMtText, sequence, timestamp, node, flood, text);
}
