#include "protocol.h"
#include <stdio.h>
#include <string.h>

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static uint8_t xt(uint8_t v) {
    return (uint8_t)((v << 1) ^ ((v & 128) ? 0x1b : 0));
}
static void aes_keys(uint8_t rk[176], const uint8_t key[16]) {
    uint8_t rc = 1;
    memcpy(rk, key, 16);
    for(unsigned i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if(!(i % 16)) {
            uint8_t first = t[0];
            t[0] = sbox[t[1]] ^ rc;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rc = xt(rc);
        }
        for(unsigned j = 0; j < 4; ++j)
            rk[i + j] = rk[i + j - 16] ^ t[j];
    }
}
void lora_aes128_ctr(uint8_t* data, size_t size, const uint8_t key[16], uint8_t nonce[16]) {
    uint8_t rk[176];
    aes_keys(rk, key);
    for(size_t off = 0; off < size; off += 16) {
        uint8_t state[16], shifted[16];
        for(unsigned i = 0; i < 16; ++i)
            state[i] = nonce[i] ^ rk[i];
        for(unsigned round = 1; round <= 10; ++round) {
            for(unsigned col = 0; col < 4; ++col)
                for(unsigned row = 0; row < 4; ++row)
                    shifted[4 * col + row] = sbox[state[4 * ((col + row) % 4) + row]];
            memcpy(state, shifted, 16);
            if(round < 10)
                for(unsigned col = 0; col < 4; ++col) {
                    uint8_t* v = state + 4 * col;
                    uint8_t sum = v[0] ^ v[1] ^ v[2] ^ v[3], first = v[0];
                    v[0] ^= sum ^ xt(v[0] ^ v[1]);
                    v[1] ^= sum ^ xt(v[1] ^ v[2]);
                    v[2] ^= sum ^ xt(v[2] ^ v[3]);
                    v[3] ^= sum ^ xt(v[3] ^ first);
                }
            for(unsigned i = 0; i < 16; ++i)
                state[i] ^= rk[16 * round + i];
        }
        for(unsigned i = 0; i < 16 && off + i < size; ++i)
            data[off + i] ^= state[i];
        for(int i = 15; i >= 0; --i)
            if(++nonce[i]) break;
    }
}
static uint8_t multiply(uint8_t a, uint8_t b) {
    uint8_t v = 0;
    while(b) {
        if(b & 1) v ^= a;
        a = xt(a);
        b >>= 1;
    }
    return v;
}
void lora_aes128_ecb_decrypt(uint8_t* data, size_t size, const uint8_t key[16]) {
    uint8_t rk[176], inverse[256];
    aes_keys(rk, key);
    for(unsigned i = 0; i < 256; ++i)
        inverse[sbox[i]] = i;
    for(size_t off = 0; off + 16 <= size; off += 16) {
        uint8_t* state = data + off;
        uint8_t shifted[16];
        for(unsigned i = 0; i < 16; ++i)
            state[i] ^= rk[160 + i];
        for(int round = 9; round >= 0; --round) {
            for(unsigned col = 0; col < 4; ++col)
                for(unsigned row = 0; row < 4; ++row)
                    shifted[4 * col + row] = inverse[state[4 * ((col + 4 - row) % 4) + row]];
            for(unsigned i = 0; i < 16; ++i)
                state[i] = shifted[i] ^ rk[16 * round + i];
            if(round)
                for(unsigned col = 0; col < 4; ++col) {
                    uint8_t* v = state + 4 * col;
                    uint8_t a = v[0], b = v[1], c = v[2], d = v[3];
                    v[0] = multiply(a, 14) ^ multiply(b, 11) ^ multiply(c, 13) ^ multiply(d, 9);
                    v[1] = multiply(a, 9) ^ multiply(b, 14) ^ multiply(c, 11) ^ multiply(d, 13);
                    v[2] = multiply(a, 13) ^ multiply(b, 9) ^ multiply(c, 14) ^ multiply(d, 11);
                    v[3] = multiply(a, 11) ^ multiply(b, 13) ^ multiply(c, 9) ^ multiply(d, 14);
                }
        }
    }
}
static void chat_text(LoraPayloadResult* r, const uint8_t* text, size_t n) {
    if(n >= sizeof(r->text)) n = sizeof(r->text) - 1;
    for(size_t i = 0; i < n; ++i)
        r->text[i] = text[i] >= 32 && text[i] != 127 ? text[i] : ' ';
    r->text[n] = 0;
    for(size_t i = 0; i < sizeof(r->detail) - 1; ++i) {
        uint8_t c = r->text[i];
        r->detail[i] = c < 127 ? c : '?';
        if(!c) break;
    }
    r->detail[sizeof(r->detail) - 1] = 0;
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static bool varint(const uint8_t* b, size_t len, size_t* pos, uint64_t* value) {
    *value = 0;
    for(unsigned n = 0; n < 10; ++n) {
        if(*pos >= len) return false;
        uint8_t c = b[(*pos)++];
        if(n == 9 && c > 1) return false;
        *value |= (uint64_t)(c & 127) << (7 * n);
        if(!(c & 128)) return true;
    }
    return false;
}
/* Consume one protobuf field with strict length/overflow checks. */
static bool field(
    const uint8_t* b,
    size_t len,
    size_t* pos,
    unsigned* id,
    unsigned* wire,
    uint64_t* value,
    const uint8_t** bytes,
    size_t* size) {
    uint64_t key;
    if(!varint(b, len, pos, &key) || key < 8 || key > UINT32_MAX) return false;
    *id = (unsigned)(key >> 3);
    *wire = key & 7;
    *bytes = NULL;
    *size = 0;
    if(*wire == 0) return varint(b, len, pos, value);
    if(*wire == 2) {
        if(!varint(b, len, pos, value) || *value > len - *pos) return false;
        *size = (size_t)*value;
    } else if(*wire == 5)
        *size = 4;
    else if(*wire == 1)
        *size = 8;
    else
        return false;
    if(*size > len - *pos) return false;
    *bytes = b + *pos;
    *pos += *size;
    if(*wire == 5) *value = le32(*bytes);
    return true;
}
static void node_name(LoraPayloadResult* r, const uint8_t* b, size_t len) {
    size_t pos = 0;
    char name[33] = "";
    while(pos < len) {
        unsigned id, wire;
        uint64_t v;
        const uint8_t* bytes;
        size_t n;
        if(!field(b, len, &pos, &id, &wire, &v, &bytes, &n)) return;
        if(id == 2 && wire == 2) {
            unsigned count = n < 32 ? n : 32;
            for(unsigned i = 0; i < count; ++i)
                name[i] = bytes[i] >= 32 && bytes[i] < 127 ? (char)bytes[i] : '?';
            name[count] = 0;
        }
    }
    memcpy(r->name, name, sizeof(name));
}
static bool
    route_counts(const uint8_t* b, size_t len, bool neighbors, unsigned* forward, unsigned* back) {
    size_t pos = 0;
    *forward = *back = 0;
    while(pos < len) {
        unsigned id, wire;
        uint64_t v;
        const uint8_t* bytes;
        size_t n;
        if(!field(b, len, &pos, &id, &wire, &v, &bytes, &n)) return false;
        if(neighbors && id == 4) {
            if(wire != 2) return false;
            ++*forward;
        } else if(!neighbors && (id == 1 || id == 3)) {
            if(!((wire == 2 && n % 4 == 0) || wire == 5)) return false;
            if(id == 1)
                *forward += n / 4;
            else
                *back += n / 4;
        }
    }
    return true;
}

static bool
    message_enum(const uint8_t* b, size_t len, unsigned wanted, uint32_t* value, bool* found) {
    size_t pos = 0;
    *found = false;
    while(pos < len) {
        unsigned id, wire;
        uint64_t v;
        const uint8_t* bytes;
        size_t n;
        if(!field(b, len, &pos, &id, &wire, &v, &bytes, &n)) return false;
        if(id == wanted) {
            if(wire != 0 || v > UINT32_MAX || *found) return false;
            *value = (uint32_t)v;
            *found = true;
        }
    }
    return true;
}

bool lora_mt_describe(LoraPayloadResult* r, bool decrypt_default) {
    r->request_id = 0;
    r->packet_type = -1;
    r->name[0] = r->detail[0] = r->text[0] = 0;
    if(r->length < 16) return false;
    r->packet_type = 32;
    uint32_t from = le32(r->data + 4), to = le32(r->data);
    snprintf(
        r->detail, sizeof(r->detail), "!%08lX > !%08lX", (unsigned long)from, (unsigned long)to);
    /* Only try the public LongFast PSK/hash on CRC-checked packets. */
    if(!decrypt_default || r->status != LoraPayloadCrcOk || r->data[13] != 0x08) return true;
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
    uint8_t plain[239], nonce[16] = {0};
    size_t len = r->length - 16;
    memcpy(nonce, r->data + 8, 4);
    memcpy(nonce + 8, r->data + 4, 4);
    memcpy(plain, r->data + 16, len);
    lora_aes128_ctr(plain, len, key, nonce);
    size_t pos = 0, body_len = 0;
    const uint8_t* body = NULL;
    uint64_t port = 0;
    bool has_port = false, reply = false, want = false;
    while(pos < len) {
        unsigned id, wire;
        uint64_t v;
        const uint8_t* bytes;
        size_t n;
        if(!field(plain, len, &pos, &id, &wire, &v, &bytes, &n)) return true;
        if(id == 1) {
            if(wire != 0 || has_port || v > 511) return true;
            port = v;
            has_port = true;
        }
        if(id == 2) {
            if(wire != 2 || body) return true;
            body = bytes;
            body_len = n;
        }
        if(id == 3) {
            if(wire != 0 || v > 1) return true;
            want = v;
        }
        if(id == 6) {
            if(wire != 5) return true;
            reply = v != 0;
            r->request_id = (uint32_t)v;
        }
    }
    if(!has_port || !body) return true;
    if(port == 70 || port == 71) {
        unsigned forward, back;
        if(!route_counts(body, body_len, port == 71, &forward, &back)) return true;
        if(port == 70)
            snprintf(
                r->detail,
                sizeof(r->detail),
                "!%08lX route %u / back %u",
                (unsigned long)from,
                forward,
                back);
        else
            snprintf(
                r->detail, sizeof(r->detail), "!%08lX neighbors %u", (unsigned long)from, forward);
    }
    switch(port) {
    case 1:
        chat_text(r, body, body_len);
        r->packet_type = 33;
        break;
    case 4:
        r->packet_type = 34;
        node_name(r, body, body_len);
        break;
    case 5: {
        uint32_t reason = 0;
        bool found;
        if(!message_enum(body, body_len, 3, &reason, &found)) return true;
        r->packet_type = found ? (reason == 0 ? 43 : 44) : 35;
        if(found)
            snprintf(
                r->detail,
                sizeof(r->detail),
                "!%08lX routing result %lu",
                (unsigned long)from,
                (unsigned long)reason);
        break;
    }
    case 65: {
        uint32_t kind = 0;
        bool found;
        if(!message_enum(body, body_len, 1, &kind, &found)) return true;
        r->packet_type = 42;
        const char* name = kind == 2  ? "Router heartbeat" :
                           kind == 3  ? "Router ping" :
                           kind == 4  ? "Router pong" :
                           kind == 5  ? "Router busy" :
                           kind == 6  ? "History reply" :
                           kind == 7  ? "Stats reply" :
                           kind == 65 ? "History request" :
                           kind == 66 ? "Stats request" :
                           kind == 67 ? "Client ping" :
                           kind == 68 ? "Client pong" :
                                        NULL;
        if(found && name)
            snprintf(r->detail, sizeof(r->detail), "%s", name);
        else
            snprintf(r->detail, sizeof(r->detail), "Store/forward type %lu", (unsigned long)kind);
        break;
    }
    case 70:
        r->packet_type = reply ? 41 : want ? 40 : 36;
        break;
    case 71:
        r->packet_type = 37;
        break;
    case 67:
        r->packet_type = 38;
        break;
    case 3:
        r->packet_type = 39;
        break;
    default:
        r->packet_type = 45;
        snprintf(
            r->detail,
            sizeof(r->detail),
            "Decoded port %lu (%u bytes)",
            (unsigned long)port,
            (unsigned)body_len);
        break;
    }
    return true;
}
const char* lora_traffic_type(int type) {
    if(type == 16) return "MC Public chat";
    static const char* mc[] = {
        "Encrypted request",
        "Encrypted reply",
        "Direct message",
        "ACK",
        "Advertisement",
        "Channel message",
        "Channel data",
        "Anon request",
        "Path",
        "Trace",
        "Multipart",
        "Control",
        "Discovery query",
        "Repeater reply",
        "Discovery reply"};
    static const char* mt[] = {
        "MT encrypted",
        "MT text",
        "MT node info",
        "MT routing",
        "MT traceroute",
        "MT neighbors",
        "MT telemetry",
        "MT position",
        "MT trace query",
        "MT trace reply",
        "MT store/forward",
        "MT ACK",
        "MT NAK",
        "MT app data"};
    if(type >= 0 && type < (int)(sizeof(mc) / sizeof(mc[0]))) return mc[type];
    if(type >= 32 && type < 32 + (int)(sizeof(mt) / sizeof(mt[0]))) return mt[type - 32];
    return "Unknown traffic";
}

unsigned lora_query_reply_state(const LoraPayloadResult* r, uint32_t tag) {
    if(!tag || r->status == LoraPayloadHeaderFailed) return 0;
    uint32_t received = r->request_id;
    if(!r->meshtastic) {
        if(r->packet_type != 13 || r->decoded_bytes < 16 || r->data[0] != 0x2e || r->data[1])
            return 0;
        received = le32(r->data + 4);
    }
    if(received != tag) return 0;
    return r->status == LoraPayloadCrcOk ? 2 : 1;
}
