#pragma once
#include <cstdint>
#include <cstddef>

const uint8_t* read_uleb128(const uint8_t* p, uint64_t* out);
const uint8_t* read_sleb128(const uint8_t* p, int64_t* out);
