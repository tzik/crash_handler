#pragma once
#include <string>
#include <cstdint>
#include <limits>

struct MapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t offset = 0;
  std::string path;
  uintptr_t base_address = std::numeric_limits<uintptr_t>::max();
};
