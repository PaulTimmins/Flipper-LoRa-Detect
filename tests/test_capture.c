#include "../app/capture_codec.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint16_t data[32];
    const uint32_t words[] = {0, 0x80000000, 1, 0x80000001, 32767, 0x80007fff,
        32768, 0x80008000, 0x7fffffff, 0xffffffff};
    unsigned count = 0, at = 0;
    for(unsigned i = 0; i < sizeof(words)/sizeof(*words); ++i)
        assert(capture_pack(data, &count, 32, words[i]));
    for(unsigned i = 0; i < sizeof(words)/sizeof(*words); ++i) {
        uint32_t word;
        assert(capture_unpack(data, count, &at, &word) && word == words[i]);
    }
    uint32_t word;
    assert(at == count && !capture_unpack(data, count, &at, &word));
    count = 0;
    assert(!capture_pack(data, &count, 2, 32768) && count == 0);
    assert(capture_pack(data, &count, 1, 123) && count == 1);
    assert(!capture_pack(data, &count, 1, 456) && count == 1);
    data[0] = 0; at = 0;
    assert(!capture_unpack(data, 2, &at, &word) && at == 0);
    puts("PASS lossless capture packing, long gaps, level/duration boundaries, capacity and truncation");
}
