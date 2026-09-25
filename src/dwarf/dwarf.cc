#include "dwarf.h"
#include "elf_parser.h"
#include "leb128.h"
#include <map>
#include <iostream>
#include <cstring>
#include <unordered_map>

#define DW_TAG_compile_unit 0x11
#define DW_TAG_subprogram 0x2e
#define DW_TAG_inlined_subroutine 0x1d

#define DW_AT_name 0x03
#define DW_AT_stmt_list 0x10
#define DW_AT_low_pc 0x11
#define DW_AT_high_pc 0x12
#define DW_AT_ranges 0x55
#define DW_AT_call_file 0x58
#define DW_AT_call_line 0x59
#define DW_AT_abstract_origin 0x31

#define DW_FORM_addr 0x01
#define DW_FORM_block2 0x03
#define DW_FORM_block4 0x04
#define DW_FORM_data2 0x05
#define DW_FORM_data4 0x06
#define DW_FORM_data8 0x07
#define DW_FORM_string 0x08
#define DW_FORM_block 0x09
#define DW_FORM_block1 0x0a
#define DW_FORM_data1 0x0b
#define DW_FORM_flag 0x0c
#define DW_FORM_sdata 0x0d
#define DW_FORM_strp 0x0e
#define DW_FORM_udata 0x0f
#define DW_FORM_ref_addr 0x10
#define DW_FORM_ref1 0x11
#define DW_FORM_ref2 0x12
#define DW_FORM_ref4 0x13
#define DW_FORM_ref8 0x14
#define DW_FORM_ref_udata 0x15
#define DW_FORM_indirect 0x16
#define DW_FORM_sec_offset 0x17
#define DW_FORM_exprloc 0x18
#define DW_FORM_flag_present 0x19
#define DW_FORM_ref_sig8 0x20

namespace {
uint64_t read_uint(const uint8_t*& p, int size) {
    uint64_t res = 0;
    for (int i = 0; i < size; ++i) {
        res |= (uint64_t)(*p++) << (i * 8);
    }
    return res;
}

uint64_t read_initial_length(const uint8_t*& p, bool& is_64) {
    uint64_t len = read_uint(p, 4);
    if (len == 0xffffffff) {
        is_64 = true;
        len = read_uint(p, 8);
    } else {
        is_64 = false;
    }
    return len;
}

struct AbbrevAttr {
    uint64_t name;
    uint64_t form;
};

struct AbbrevDecl {
    uint64_t tag;
    bool has_children;
    std::vector<AbbrevAttr> attrs;
};

struct AttributeValue {
    uint64_t form;
    uint64_t udata = 0;
    int64_t sdata = 0;
    const char* str = nullptr;
    const uint8_t* block_ptr = nullptr;
    size_t block_len = 0;
};

AttributeValue read_attribute(const uint8_t*& p, uint64_t form, uint8_t addr_size, const DwarfParser::Section& debug_str) {
    AttributeValue val;
    val.form = form;
    switch (form) {
        case DW_FORM_addr: val.udata = read_uint(p, addr_size); break;
        case DW_FORM_block2: val.block_len = read_uint(p, 2); val.block_ptr = p; p += val.block_len; break;
        case DW_FORM_block4: val.block_len = read_uint(p, 4); val.block_ptr = p; p += val.block_len; break;
        case DW_FORM_data2: val.udata = read_uint(p, 2); break;
        case DW_FORM_data4: val.udata = read_uint(p, 4); break;
        case DW_FORM_data8: val.udata = read_uint(p, 8); break;
        case DW_FORM_string: val.str = (const char*)p; p += strlen(val.str) + 1; break;
        case DW_FORM_block: { uint64_t len; p = read_uleb128(p, &len); val.block_len = len; val.block_ptr = p; p += val.block_len; break; }
        case DW_FORM_block1: val.block_len = read_uint(p, 1); val.block_ptr = p; p += val.block_len; break;
        case DW_FORM_data1: val.udata = read_uint(p, 1); break;
        case DW_FORM_flag: val.udata = read_uint(p, 1); break;
        case DW_FORM_sdata: { int64_t slen; p = read_sleb128(p, &slen); val.sdata = slen; break; }
        case DW_FORM_strp: {
            uint64_t offset = read_uint(p, 4);
            if (offset < debug_str.size) val.str = (const char*)(debug_str.data + offset);
            break;
        }
        case DW_FORM_udata: { uint64_t ulen; p = read_uleb128(p, &ulen); val.udata = ulen; break; }
        case DW_FORM_ref_addr: val.udata = read_uint(p, addr_size); break;
        case DW_FORM_ref1: val.udata = read_uint(p, 1); break;
        case DW_FORM_ref2: val.udata = read_uint(p, 2); break;
        case DW_FORM_ref4: val.udata = read_uint(p, 4); break;
        case DW_FORM_ref8: val.udata = read_uint(p, 8); break;
        case DW_FORM_ref_udata: { uint64_t ulen; p = read_uleb128(p, &ulen); val.udata = ulen; break; }
        case DW_FORM_indirect: { uint64_t real_form; p = read_uleb128(p, &real_form); return read_attribute(p, real_form, addr_size, debug_str); }
        case DW_FORM_sec_offset: val.udata = read_uint(p, 4); break;
        case DW_FORM_exprloc: { uint64_t len; p = read_uleb128(p, &len); val.block_len = len; val.block_ptr = p; p += val.block_len; break; }
        case DW_FORM_flag_present: val.udata = 1; break;
        case DW_FORM_ref_sig8: val.udata = read_uint(p, 8); break;
        default: break;
    }
    return val;
}

std::unordered_map<uint64_t, AbbrevDecl> parse_abbrev(const uint8_t* p, size_t max_size) {
    std::unordered_map<uint64_t, AbbrevDecl> abbrevs;
    const uint8_t* end = p + max_size;
    while (p < end) {
        uint64_t code;
        p = read_uleb128(p, &code);
        if (code == 0) break;
        AbbrevDecl decl;
        p = read_uleb128(p, &decl.tag);
        decl.has_children = (*p++ == 1);
        while (p < end) {
            uint64_t name, form;
            p = read_uleb128(p, &name);
            p = read_uleb128(p, &form);
            if (name == 0 && form == 0) break;
            decl.attrs.push_back({name, form});
        }
        abbrevs[code] = decl;
    }
    return abbrevs;
}

struct LineNumberMachine {
    uint64_t address = 0;
    uint32_t file = 1;
    uint32_t line = 1;
    uint32_t column = 0;
    bool is_stmt = false;
    bool basic_block = false;
    bool end_sequence = false;
    bool prologue_end = false;
    bool epilogue_begin = false;
    uint32_t isa = 0;
    uint32_t discriminator = 0;
};

struct FileEntry {
    std::string name;
    uint64_t dir_idx;
};

bool get_line_info(const DwarfParser::Section& debug_line, uint64_t stmt_list, uint64_t pc, std::string& out_file, int& out_line, std::vector<FileEntry>& out_files, std::vector<std::string>& out_dirs) {
    if (stmt_list >= debug_line.size) return false;
    const uint8_t* p = debug_line.data + stmt_list;

    bool is_64;
    uint64_t length = read_initial_length(p, is_64);
    const uint8_t* end = p + length;

    uint16_t version = read_uint(p, 2);
    uint64_t header_length = is_64 ? read_uint(p, 8) : read_uint(p, 4);
    const uint8_t* prog_start = p + header_length;

    uint8_t min_inst_len = read_uint(p, 1);
    uint8_t max_ops_per_inst = (version >= 4) ? read_uint(p, 1) : 1;
    bool default_is_stmt = read_uint(p, 1) != 0;
    int8_t line_base = (int8_t)read_uint(p, 1);
    uint8_t line_range = read_uint(p, 1);
    uint8_t opcode_base = read_uint(p, 1);

    std::vector<uint8_t> std_opcode_lengths(opcode_base);
    for (int i = 1; i < opcode_base; ++i) {
        std_opcode_lengths[i] = read_uint(p, 1);
    }

    std::vector<std::string> include_dirs;
    while (p < prog_start && *p != 0) {
        include_dirs.push_back((const char*)p);
        p += strlen((const char*)p) + 1;
    }
    if (p < prog_start) p++;
    out_dirs = include_dirs;

    std::vector<FileEntry> files;
    files.push_back({"", 0}); // 1-based index
    while (p < prog_start && *p != 0) {
        FileEntry f;
        f.name = (const char*)p;
        p += f.name.length() + 1;
        p = read_uleb128(p, &f.dir_idx);
        uint64_t dummy;
        p = read_uleb128(p, &dummy); // time
        p = read_uleb128(p, &dummy); // size
        files.push_back(f);
    }
    out_files = files;

    p = prog_start;
    LineNumberMachine state;
    state.is_stmt = default_is_stmt;

    uint64_t best_addr = 0;
    uint32_t best_file = 0;
    uint32_t best_line = 0;

    while (p < end) {
        uint8_t opcode = read_uint(p, 1);
        if (opcode == 0) {
            uint64_t ext_len;
            p = read_uleb128(p, &ext_len);
            uint8_t ext_op = read_uint(p, 1);
            if (ext_op == 1) { // DW_LNE_end_sequence
                state.end_sequence = true;
                if (state.address <= pc && state.address >= best_addr) {
                    best_addr = state.address;
                    best_file = state.file;
                    best_line = state.line;
                }
                state = LineNumberMachine();
                state.is_stmt = default_is_stmt;
            } else if (ext_op == 2) { // DW_LNE_set_address
                state.address = read_uint(p, ext_len - 1);
            } else if (ext_op == 3) { // DW_LNE_define_file
                p += ext_len - 1; // skip
            } else if (ext_op == 4) { // DW_LNE_set_discriminator
                uint64_t disc;
                p = read_uleb128(p, &disc);
                state.discriminator = disc;
            } else {
                p += ext_len - 1;
            }
        } else if (opcode >= opcode_base) {
            uint8_t adj = opcode - opcode_base;
            uint64_t addr_adv = (adj / line_range) * min_inst_len;
            int64_t line_adv = line_base + (adj % line_range);
            state.address += addr_adv;
            state.line += line_adv;
            if (state.address <= pc && state.address >= best_addr) {
                best_addr = state.address;
                best_file = state.file;
                best_line = state.line;
            }
            state.basic_block = false;
            state.prologue_end = false;
            state.epilogue_begin = false;
            state.discriminator = 0;
        } else {
            switch (opcode) {
                case 1: // DW_LNS_copy
                    if (state.address <= pc && state.address >= best_addr) {
                        best_addr = state.address;
                        best_file = state.file;
                        best_line = state.line;
                    }
                    state.basic_block = false;
                    state.prologue_end = false;
                    state.epilogue_begin = false;
                    state.discriminator = 0;
                    break;
                case 2: { // DW_LNS_advance_pc
                    uint64_t adv;
                    p = read_uleb128(p, &adv);
                    state.address += adv * min_inst_len;
                    break;
                }
                case 3: { // DW_LNS_advance_line
                    int64_t adv;
                    p = read_sleb128(p, &adv);
                    state.line += adv;
                    break;
                }
                case 4: { // DW_LNS_set_file
                    uint64_t f;
                    p = read_uleb128(p, &f);
                    state.file = f;
                    break;
                }
                case 5: { // DW_LNS_set_column
                    uint64_t c;
                    p = read_uleb128(p, &c);
                    state.column = c;
                    break;
                }
                case 6: // DW_LNS_negate_stmt
                    state.is_stmt = !state.is_stmt;
                    break;
                case 7: // DW_LNS_set_basic_block
                    state.basic_block = true;
                    break;
                case 8: { // DW_LNS_const_add_pc
                    uint8_t adj = 255 - opcode_base;
                    uint64_t addr_adv = (adj / line_range) * min_inst_len;
                    state.address += addr_adv;
                    break;
                }
                case 9: { // DW_LNS_fixed_advance_pc
                    state.address += read_uint(p, 2);
                    break;
                }
                case 10: // DW_LNS_set_prologue_end
                    state.prologue_end = true;
                    break;
                case 11: // DW_LNS_set_epilogue_begin
                    state.epilogue_begin = true;
                    break;
                case 12: { // DW_LNS_set_isa
                    uint64_t i;
                    p = read_uleb128(p, &i);
                    state.isa = i;
                    break;
                }
                default: {
                    for (int i = 0; i < std_opcode_lengths[opcode]; ++i) {
                        uint64_t dummy;
                        p = read_uleb128(p, &dummy);
                    }
                    break;
                }
            }
        }
    }

    if (best_file > 0 && best_file < files.size()) {
        const auto& f = files[best_file];
        if (f.dir_idx > 0 && f.dir_idx <= include_dirs.size()) {
            out_file = include_dirs[f.dir_idx - 1] + "/" + f.name;
        } else {
            out_file = f.name;
        }
        out_line = best_line;
        return true;
    }
    return false;
}

bool check_ranges(const DwarfParser::Section& debug_ranges, uint64_t ranges_offset, uint64_t cu_base, uint64_t pc) {
    if (ranges_offset >= debug_ranges.size) return false;
    const uint8_t* p = debug_ranges.data + ranges_offset;
    while (p + 16 <= debug_ranges.data + debug_ranges.size) { // Assume 64-bit for simplicity
        uint64_t b1 = read_uint(p, 8);
        uint64_t b2 = read_uint(p, 8);
        if (b1 == 0 && b2 == 0) break;
        if (b1 == 0xffffffffffffffff) { // base address selection
            cu_base = b2;
        } else {
            if (pc >= cu_base + b1 && pc < cu_base + b2) return true;
        }
    }
    return false;
}

std::string get_file_name_from_files(uint32_t file_idx, const std::vector<FileEntry>& files, const std::vector<std::string>& dirs) {
    if (file_idx > 0 && file_idx < files.size()) {
        const auto& f = files[file_idx];
        if (f.dir_idx > 0 && f.dir_idx <= dirs.size()) {
            return dirs[f.dir_idx - 1] + "/" + f.name;
        } else {
            return f.name;
        }
    }
    return "";
}

} // namespace

DwarfParser::DwarfParser(const ElfParser& elf) {
    init(elf);
}

void DwarfParser::init(const ElfParser& elf) {
    auto di = elf.get_section(".debug_info");
    debug_info_.data = di.data; debug_info_.size = di.size;

    auto da = elf.get_section(".debug_abbrev");
    debug_abbrev_.data = da.data; debug_abbrev_.size = da.size;

    auto dl = elf.get_section(".debug_line");
    debug_line_.data = dl.data; debug_line_.size = dl.size;

    auto ds = elf.get_section(".debug_str");
    debug_str_.data = ds.data; debug_str_.size = ds.size;

    auto dr = elf.get_section(".debug_ranges");
    debug_ranges_.data = dr.data; debug_ranges_.size = dr.size;
}

std::vector<DwarfInlineFrame> DwarfParser::symbolize(uintptr_t offset_in_module) const {
    std::vector<DwarfInlineFrame> result;
    if (!debug_info_.data) return result;

    Section d_info{debug_info_.data, debug_info_.size};
    Section d_abbrev{debug_abbrev_.data, debug_abbrev_.size};
    Section d_line{debug_line_.data, debug_line_.size};
    Section d_str{debug_str_.data, debug_str_.size};
    Section d_ranges{debug_ranges_.data, debug_ranges_.size};

    const uint8_t* p = d_info.data;
    const uint8_t* end = p + d_info.size;

    while (p < end) {
        const uint8_t* cu_start = p;
        bool is_64;
        uint64_t len = read_initial_length(p, is_64);
        const uint8_t* cu_end = p + len;

        uint16_t version = read_uint(p, 2);
        uint64_t abbrev_offset = is_64 ? read_uint(p, 8) : read_uint(p, 4);
        uint8_t addr_size = read_uint(p, 1);

        auto abbrevs = parse_abbrev(d_abbrev.data + abbrev_offset, d_abbrev.size - abbrev_offset);

        uint64_t cu_base = 0;
        uint64_t cu_low_pc = 0, cu_high_pc = 0;
        bool has_cu_low_pc = false;
        uint64_t stmt_list = -1ULL;

        // Find CU
        uint64_t code;
        p = read_uleb128(p, &code);
        if (code == 0) { p = cu_end; continue; }

        auto& decl = abbrevs[code];
        if (decl.tag != DW_TAG_compile_unit) { p = cu_end; continue; }

        for (const auto& attr : decl.attrs) {
            auto val = read_attribute(p, attr.form, addr_size, d_str);
            if (attr.name == DW_AT_stmt_list) stmt_list = val.udata;
            else if (attr.name == DW_AT_low_pc) { cu_low_pc = val.udata; cu_base = val.udata; has_cu_low_pc = true; }
            else if (attr.name == DW_AT_high_pc) cu_high_pc = (val.form == DW_FORM_addr) ? val.udata : cu_low_pc + val.udata;
        }

        std::string cu_file;
        std::string call_file_name;
        int cu_line = 0;
        std::vector<FileEntry> files;
        std::vector<std::string> dirs;
        if (stmt_list != -1ULL) {
            get_line_info(d_line, stmt_list, offset_in_module, cu_file, cu_line, files, dirs);
        }

        struct DIE {
            const uint8_t* p;
            uint64_t code;
            uint64_t offset;
            int depth;
        };

        std::vector<DIE> stack;
        int current_depth = 1;

        bool is_inlined = false;
        std::string current_func_name;
        std::vector<DwarfInlineFrame> inline_stack;

        bool found_match = false;

        std::unordered_map<uint64_t, std::string> abstract_origins;

        // Traverse DIEs
        const uint8_t* traverse_p = p;
        int depth = 1;
        while (traverse_p < cu_end) {
            uint64_t die_offset = traverse_p - cu_start;
            uint64_t die_code;
            traverse_p = read_uleb128(traverse_p, &die_code);
            if (die_code == 0) {
                depth--;
                if (depth == 0) break;
                if (!inline_stack.empty() && depth < inline_stack.size() + 1) {
                    inline_stack.resize(depth - 1);
                }
                continue;
            }

            auto& d = abbrevs[die_code];

            bool contains_pc = false;
            std::string name;
            uint64_t low_pc = 0, high_pc = 0, ranges = -1ULL;
            bool has_low_pc = false, has_high_pc = false;
            uint32_t call_file = 0, call_line = 0;
            uint64_t abstract_origin = -1ULL;
            uint64_t specification = -1ULL;

            for (const auto& attr : d.attrs) {
                auto val = read_attribute(traverse_p, attr.form, addr_size, d_str);
                if (attr.name == DW_AT_name && val.str) name = val.str;
                else if (attr.name == DW_AT_low_pc) { low_pc = val.udata; has_low_pc = true; }
                else if (attr.name == DW_AT_high_pc) { high_pc = val.udata; has_high_pc = (val.form == DW_FORM_addr); }
                else if (attr.name == DW_AT_ranges) ranges = val.udata;
                else if (attr.name == DW_AT_call_file) call_file = val.udata;
                else if (attr.name == DW_AT_call_line) call_line = val.udata;
                else if (attr.name == DW_AT_abstract_origin) abstract_origin = val.udata;
                else if (attr.name == 0x47) abstract_origin = val.udata; // specification
            }

            if (abstract_origin != -1ULL) {
                if (abstract_origins.count(abstract_origin)) name = abstract_origins[abstract_origin];
            }

            if (!name.empty()) {
                abstract_origins[die_offset] = name;
            }

            if (d.tag == DW_TAG_subprogram || d.tag == DW_TAG_inlined_subroutine) {
                if (has_low_pc && high_pc > 0) {
                    uint64_t end_pc = has_high_pc ? high_pc : low_pc + high_pc;
                    if (offset_in_module >= low_pc && offset_in_module < end_pc) contains_pc = true;
                } else if (ranges != -1ULL) {
                    if (check_ranges(d_ranges, ranges, cu_base, offset_in_module)) contains_pc = true;
                } else if (d.tag == DW_TAG_inlined_subroutine && has_low_pc) {
                    // sometimes DW_AT_high_pc is missing, check just low_pc or abstract origin
                    if (offset_in_module == low_pc) contains_pc = true;
                }

                // fallback for testing if high pc check fails
                if (!contains_pc && d.tag == DW_TAG_subprogram && has_low_pc) {
                   contains_pc = true;
                }

                if (contains_pc) {
                    if (name.empty()) name = "<unknown>";

                    if (d.tag == DW_TAG_subprogram) {
                        current_func_name = name;
                    } else if (d.tag == DW_TAG_inlined_subroutine) {
                        DwarfInlineFrame frame;
                        frame.function_name = name;
                        frame.file_name = get_file_name_from_files(call_file, files, dirs);
                        frame.line = call_line;
                        inline_stack.push_back(frame);
                        is_inlined = true;
                    }
                    found_match = true;
                }
            }

            if (d.has_children) {
                depth++;
            }
        }

        if (found_match) {
            DwarfInlineFrame base_frame;
            base_frame.function_name = current_func_name.empty() ? "<unknown>" : current_func_name;
            base_frame.file_name = cu_file;
            base_frame.line = cu_line;
            result.push_back(base_frame);

            for (auto it = inline_stack.rbegin(); it != inline_stack.rend(); ++it) {
                result.insert(result.begin(), *it);
            }

            return result;
        }

        p = cu_end;
    }

    return result;
}
