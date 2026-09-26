#include "leb128.h"

const uint8_t* read_uleb128(const uint8_t* p, uint64_t* out) {
    uint64_t result = 0;
    int shift = 0;
    while (true) {
        uint8_t byte = *p++;
        result |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            break;
        shift += 7;
    }
    *out = result;
    return p;
}

const uint8_t* read_sleb128(const uint8_t* p, int64_t* out) {
    int64_t result = 0;
    int shift = 0;
    uint8_t byte;
    do {
        byte = *p++;
        result |= (int64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);

    if (shift < 64 && (byte & 0x40))
        result |= -(1ULL << shift);

    *out = result;
    return p;
}
