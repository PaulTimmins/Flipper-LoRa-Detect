#include "../app/report.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    LoraPayloadResult r = {0};
    r.status = LoraPayloadDamaged;
    r.length = 120;
    r.payload_complete = r.crc_present = true;
    r.combined_copies = 3;
    strcpy(r.name, "TestNode\nFake entry");
    char out[512];
    size_t n = lora_report_format(
        out, sizeof(out), &r, "2026-10-10 15:30:00", 910525000, -73, "Advertisement");
    assert(n == strlen(out));
    assert(strstr(out, "2026-10-10 15:30:00\nAdvertisement\n"));
    assert(strstr(out, "TestNode?Fake entry"));
    assert(strstr(out, "Signal: -73 dBm"));
    assert(strstr(out, "910.5250 MHz"));
    assert(strstr(out, "UNVERIFIED (CRC failed)"));
    r.status = LoraPayloadCrcOk;
    r.name[0] = 0;
    assert(lora_report_format(out, sizeof(out), &r, "Unknown time", 0, 1000, "Unknown"));
    assert(strstr(out, "Node: Not recovered"));
    assert(strstr(out, "Signal: Unavailable"));
    assert(strstr(out, "CRC OK; signature NOT checked"));
    r.status = LoraPayloadHeaderFailed;
    assert(lora_report_format(out, sizeof(out), &r, "Unknown", 0, 1000, "Unknown"));
    assert(strstr(out, "Header not recovered; payload unknown"));
    assert(!lora_report_format(out, 10, &r, "Unknown", 0, 1000, "Unknown"));
    assert(!out[0]);
    r.status = LoraPayloadDamaged;
    r.payload_complete = false;
    assert(lora_report_format(out, sizeof(out), &r, "Unknown", 0, 1000, "Unknown"));
    assert(strstr(out, "Incomplete payload; type unknown"));
    r.payload_complete = true;
    r.crc_present = false;
    assert(lora_report_format(out, sizeof(out), &r, "Unknown", 0, 1000, "Unknown"));
    assert(strstr(out, "no payload CRC"));
    for(unsigned len = 0; len <= 255; ++len) {
        r.decoded_bytes = len;
        for(unsigned i = 0; i < len; ++i)
            r.data[i] = i;
        unsigned bytes = 0;
        for(unsigned offset = 0; offset < len; offset += 32) {
            char hex[65];
            size_t n = lora_report_hex(hex, sizeof(hex), &r, offset);
            assert(n && !(n % 2) && n == strlen(hex));
            for(unsigned i = 0; i < n / 2; ++i) {
                char expected[3];
                snprintf(expected, sizeof(expected), "%02X", r.data[offset + i]);
                assert(!memcmp(hex + 2 * i, expected, 2));
            }
            bytes += n / 2;
            assert(!lora_report_hex(hex, n, &r, offset) && !hex[0]);
        }
        assert(bytes == len);
        assert(!lora_report_hex(out, sizeof(out), &r, len));
        assert(!lora_report_hex(NULL, 0, &r, 0));
    }
    puts("PASS phone report timestamps, signal, confidence, missing fields and bounds");
}
