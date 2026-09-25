#include "elf_parser.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <iostream>

ElfParser::ElfParser(const std::string& path) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return;

    struct stat st;
    if (fstat(fd_, &st) < 0) return;
    size_ = st.st_size;
    if (size_ < sizeof(Elf64_Ehdr)) return;

    data_ = (uint8_t*)mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        return;
    }

    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)data_;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return;

    if (ehdr->e_shoff + ehdr->e_shnum * ehdr->e_shentsize > size_) return;

    if (ehdr->e_shstrndx != SHN_UNDEF && ehdr->e_shstrndx < ehdr->e_shnum) {
        Elf64_Shdr* shstrtab_shdr = (Elf64_Shdr*)(data_ + ehdr->e_shoff + ehdr->e_shstrndx * ehdr->e_shentsize);
        if (shstrtab_shdr->sh_offset + shstrtab_shdr->sh_size <= size_) {
            strtab_ = (const char*)(data_ + shstrtab_shdr->sh_offset);
        }
    }

    valid_ = true;
}

ElfParser::~ElfParser() {
    if (data_) munmap(data_, size_);
    if (fd_ >= 0) close(fd_);
}

ElfSection ElfParser::get_section(std::string_view name) const {
    ElfSection sec;
    if (!valid_ || !strtab_) return sec;

    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)data_;
    for (int i = 0; i < ehdr->e_shnum; ++i) {
        Elf64_Shdr* shdr = (Elf64_Shdr*)(data_ + ehdr->e_shoff + i * ehdr->e_shentsize);
        const char* sec_name = strtab_ + shdr->sh_name;
        if (name == sec_name) {
            sec.name = name;
            sec.data = data_ + shdr->sh_offset;
            sec.size = shdr->sh_size;
            return sec;
        }
    }
    return sec;
}

void ElfParser::calculate_base_addresses(MapEntry* entries, size_t num_entries) const {
    if (!valid_) return;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)data_;

    struct PhdrInfo {
        uintptr_t offset_aligned;
        uintptr_t end;
        uintptr_t vaddr;
    };
    std::vector<PhdrInfo> valid_headers;
    uintptr_t page_size = sysconf(_SC_PAGESIZE);

    for (int i = 0; i < ehdr->e_phnum; ++i) {
        Elf64_Phdr* phdr = (Elf64_Phdr*)(data_ + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD || (phdr->p_flags & PF_X) == 0) continue;

        PhdrInfo info;
        info.offset_aligned = phdr->p_offset & ~(page_size - 1);
        info.end = (phdr->p_offset + phdr->p_filesz + page_size - 1) & ~(page_size - 1);
        info.vaddr = phdr->p_vaddr & ~(page_size - 1);
        valid_headers.push_back(info);
    }

    std::sort(valid_headers.begin(), valid_headers.end(), [](const PhdrInfo& a, const PhdrInfo& b) {
        if (a.offset_aligned != b.offset_aligned)
            return a.offset_aligned < b.offset_aligned;
        return a.end < b.end;
    });

    std::vector<MapEntry*> sorted_entries(num_entries);
    for (size_t i = 0; i < num_entries; ++i) sorted_entries[i] = &entries[i];
    std::sort(sorted_entries.begin(), sorted_entries.end(), [](const MapEntry* a, const MapEntry* b) {
        return a->offset < b->offset;
    });

    auto hdr_it = valid_headers.begin();
    for (MapEntry* entry : sorted_entries) {
        while (hdr_it != valid_headers.end() && hdr_it->end <= entry->offset)
            ++hdr_it;
        if (hdr_it == valid_headers.end()) break;

        if (entry->offset >= hdr_it->offset_aligned) {
            uintptr_t vaddr_in_file = hdr_it->vaddr + (entry->offset - hdr_it->offset_aligned);
            entry->base_address = entry->start - vaddr_in_file;
        }
    }
}
