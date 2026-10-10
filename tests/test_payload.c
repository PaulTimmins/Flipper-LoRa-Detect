#include "../app/payload.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    LoraPayloadResult r = {0};
    r.length = 111;
    r.data[0] = 0x11;
    r.data[102] = 0x82;
    memcpy(r.data + 103, "TestNode", 8);
    assert(lora_payload_describe(&r));
    assert(r.packet_type == 4 && r.node_type == 2 && !strcmp(r.name, "TestNode"));
    r.data[1] = 0xff; /* path extends beyond the packet */
    assert(!lora_payload_describe(&r) && r.packet_type == -1 && !r.name[0]);
    r.data[1] = 0;
    r.data[102] = 0x92; /* position indicated but truncated */
    r.length = 107;
    assert(!lora_payload_describe(&r) && !r.name[0]);
    r.length = 111;
    r.data[102] = 0x82;
    r.data[103] = 1;
    assert(lora_payload_describe(&r) && r.name[0] == '?');
    r.data[0] = 0x13; /* transport-direct routing prefix */
    assert(!lora_payload_describe(&r));
    r.data[0] = 0xd1; /* unsupported version */
    assert(!lora_payload_describe(&r));
    uint32_t rng = 7;
    for(unsigned n = 0; n < 10000; ++n) {
        r.length = n % 256;
        for(unsigned j = 0; j < r.length; ++j) {
            rng = 1664525u * rng + 1013904223u;
            r.data[j] = rng >> 24;
        }
        lora_payload_describe(&r);
        assert(memchr(r.name, 0, sizeof(r.name)));
    }
    LoraPayload* p = malloc(sizeof(*p));
    assert(p);
    lora_payload_init(p, NULL, NULL);
    LoraDetection e = {.sf = 11, .bandwidth = 250000, .sync = 0x2b};
    assert(!lora_payload_start(p, &e));
    e.sf = 7;
    e.bandwidth = 62500;
    e.sync = 0x12;
    assert(!lora_payload_start(p, &e)); /* no preamble history */
    p->active = true;
    lora_payload_init(p, NULL, NULL);
    assert(!p->active && !p->pulse_count && !p->advert_copies);
    free(p);
    puts("PASS payload parser bounds, malformed frames, unsupported PHY, and reset");
}
