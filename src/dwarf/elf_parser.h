#pragma once
#include <elf.h>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>
#include "../map_entry.h"

struct ElfSection {
    std::string_view name;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

class ElfParser {
public:
    ElfParser(const std::string& path);
    ~ElfParser();

    bool is_valid() const { return valid_; }
    void calculate_base_addresses(MapEntry* entries, size_t num_entries) const;
    ElfSection get_section(std::string_view name) const;

private:
    int fd_ = -1;
    size_t size_ = 0;
    uint8_t* data_ = nullptr;
    bool valid_ = false;
    const char* strtab_ = nullptr;
};
