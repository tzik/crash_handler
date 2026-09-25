#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct DwarfInlineFrame {
    std::string function_name;
    std::string file_name;
    int line = 0;
};

class DwarfParser {
public:
    DwarfParser(const class ElfParser& elf);

    std::vector<DwarfInlineFrame> symbolize(uintptr_t offset_in_module) const;

    struct Section {
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

private:
    Section debug_info_;
    Section debug_abbrev_;
    Section debug_line_;
    Section debug_str_;
    Section debug_ranges_;

    void init(const ElfParser& elf);
};
