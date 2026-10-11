#include "report.h"
#include <stdio.h>

size_t lora_report_format(
    char* out,
    size_t capacity,
    const LoraPayloadResult* r,
    const char* detected_at,
    uint32_t frequency,
    int rssi,
    const char* traffic_type) {
    char signal[24], name[33];
    if(rssi == 1000)
        snprintf(signal, sizeof(signal), "Unavailable");
    else
        snprintf(signal, sizeof(signal), "%d dBm", rssi);
    unsigned i = 0;
    for(; i < sizeof(name) - 1 && r->name[i]; ++i) {
        unsigned char c = (unsigned char)r->name[i];
        name[i] = c >= 32 && c <= 126 ? (char)c : '?';
    }
    name[i] = 0;
    int n = snprintf(
        out,
        capacity,
        "%s\n%s\nNode: %s\nSignal: %s\nFrequency: %lu.%04lu MHz\n"
        "Details: %s\nStatus: %s\nLength: %u bytes | Combined copies: %u\n\n",
        detected_at,
        traffic_type,
        name[0] ? name : "Not recovered",
        signal,
        (unsigned long)(frequency / 1000000),
        (unsigned long)((frequency % 1000000) / 100),
        r->detail[0] ? r->detail : "None",
        r->status == LoraPayloadCrcOk ?
            "CRC OK; signature NOT checked" :
        r->status == LoraPayloadDamaged ?
            (!r->payload_complete ? "Incomplete payload; type unknown" :
             r->crc_present       ? "UNVERIFIED (CRC failed); name/type tentative" :
                                    "UNVERIFIED (no payload CRC); name/type tentative") :
            "Header not recovered; payload unknown",
        r->length,
        r->combined_copies);
    if(n < 0 || (size_t)n >= capacity) {
        if(capacity) out[0] = 0;
        return 0;
    }
    return (size_t)n;
}

size_t lora_report_hex(char* out, size_t capacity, const LoraPayloadResult* r, size_t offset) {
    if(capacity) out[0] = 0;
    if(offset >= r->decoded_bytes) return 0;
    size_t n = r->decoded_bytes - offset;
    if(n > 32) n = 32;
    if(capacity < 2 * n + 1) return 0;
    static const char digits[] = "0123456789ABCDEF";
    for(size_t i = 0; i < n; ++i) {
        uint8_t b = r->data[offset + i];
        out[2 * i] = digits[b >> 4];
        out[2 * i + 1] = digits[b & 15];
    }
    out[2 * n] = 0;
    return 2 * n;
}
