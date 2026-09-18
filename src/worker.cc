#include <fcntl.h>
#include <gelf.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cxxabi.h>
#include <dwarf.h>
#include <elfutils/libdwfl.h>

#include "trace_packet.h"
#include "util.h"

extern char** environ;

namespace {

struct MapEntry {
  uintptr_t start, end, offset;
  std::string path;
  uintptr_t base_address;
};

struct FrameInfo {
  uintptr_t addr;
  const MapEntry* entry;
  uintptr_t offset_in_module;
};

std::optional<uintptr_t> GetBaseAddress(const std::string& path,
                                        uintptr_t map_start,
                                        uintptr_t map_offset) {
  if (elf_version(EV_CURRENT) == EV_NONE)
    return std::nullopt;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return std::nullopt;

  Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
  if (!elf) {
    close(fd);
    return std::nullopt;
  }

  size_t phnum;
  if (elf_getphdrnum(elf, &phnum) != 0) {
    elf_end(elf);
    close(fd);
    return std::nullopt;
  }

  std::optional<uintptr_t> base_address;

  for (size_t i = 0; i < phnum; ++i) {
    GElf_Phdr phdr;
    if (gelf_getphdr(elf, i, &phdr) != &phdr || phdr.p_type != PT_LOAD) {
      continue;
    }
    // Find the executable PT_LOAD segment that corresponds to the mmap offset
    if ((phdr.p_flags & PF_X) == 0) {
      continue;
    }

    uintptr_t page_size = sysconf(_SC_PAGESIZE);
    uintptr_t phdr_offset_aligned = phdr.p_offset & ~(page_size - 1);
    uintptr_t phdr_end =
        (phdr.p_offset + phdr.p_filesz + page_size - 1) & ~(page_size - 1);

    if (map_offset >= phdr_offset_aligned && map_offset < phdr_end) {
      uintptr_t vaddr_in_file = (phdr.p_vaddr & ~(page_size - 1)) +
                                (map_offset - phdr_offset_aligned);
      base_address = map_start - vaddr_in_file;
      break;
    }
  }

  elf_end(elf);
  close(fd);

  return base_address;
}

std::map<uintptr_t, MapEntry> ReadMaps(pid_t pid) {
  std::map<uintptr_t, MapEntry> entries;
  std::string maps_path = std::format("/proc/{}/maps", pid);
  std::ifstream maps(maps_path);
  std::string line;

  while (std::getline(maps, line)) {
    std::istringstream iss(line);
    std::string addr, perms, offset, dev, inode, path;
    iss >> addr >> perms >> offset >> dev >> inode;
    std::getline(iss, path);
    size_t first = path.find_first_not_of(" \t");

    if (first == std::string::npos)
      continue;

    path = path.substr(first);

    if (perms.find('x') == std::string::npos)
      continue;
    if (path.empty() || path[0] != '/')
      continue;

    size_t dash = addr.find('-');
    if (dash == std::string::npos)
      continue;

    MapEntry e;
    e.start = std::stoull(addr.substr(0, dash), nullptr, 16);
    e.end = std::stoull(addr.substr(dash + 1), nullptr, 16);
    e.offset = std::stoull(offset, nullptr, 16);
    e.path = path;

    auto base_addr_opt = GetBaseAddress(path, e.start, e.offset);
    if (!base_addr_opt.has_value())
      continue;

    e.base_address = base_addr_opt.value();
    entries[e.start] = e;
  }
  return entries;
}

std::vector<FrameInfo> PopulateFrames(const TracePacket& data,
                                      const std::map<uintptr_t, MapEntry>& maps) {
  std::vector<FrameInfo> frames;
  bool dump_maps = getenv("CRASH_HANDLER_DUMP_MAPS") != nullptr;

  if (dump_maps) {
    std::cerr << "--- CRASH_HANDLER_DUMP_MAPS ---\n";
    for (const auto& [start, entry] : maps) {
      std::cerr << std::format("{:x}-{:x} offset={:x} base={:x} {}\n",
                               entry.start, entry.end, entry.offset,
                               entry.base_address, entry.path);
    }
    std::cerr << "-------------------------------\n";
  }

  for (int i = 0; i < data.stack_depth; ++i) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(data.stack[i]);

    const MapEntry* entry = nullptr;
    auto it = maps.upper_bound(addr);
    if (it != maps.begin()) {
      --it;
      if (addr >= it->second.start && addr < it->second.end) {
        entry = &it->second;
      }
    }

    if (!entry) {
      frames.push_back({addr, nullptr, 0});
      if (dump_maps) {
        std::cerr << std::format(
            "Frame #{}: addr=0x{:x} (no map entry found)\n", i, addr);
      }
      continue;
    }

    uintptr_t offset_in_module = addr - entry->base_address;

    if (dump_maps) {
      std::cerr << std::format(
          "Frame #{}: addr=0x{:x} base=0x{:x} offset=0x{:x} path={}\n", i, addr,
          entry->base_address, offset_in_module, entry->path);
    }

    frames.push_back({addr, entry, offset_in_module});
  }

  return frames;
}

void PrintSymbol(uintptr_t addr,
                 int frame_idx,
                 const std::string& module_path,
                 uintptr_t offset,
                 const std::string& strip_path_prefix,
                 std::string function,
                 std::string file,
                 int line) {
  std::string display_module_path = module_path;

  if (!strip_path_prefix.empty()) {
    if (file.starts_with(strip_path_prefix)) {
      file = file.substr(strip_path_prefix.length());
    }
    if (display_module_path.starts_with(strip_path_prefix)) {
      display_module_path =
          display_module_path.substr(strip_path_prefix.length());
    }
  }

  if (function.empty())
    function = "??";
  if (file.empty())
    file = "??";
  if (display_module_path.empty())
    display_module_path = "??";

  std::string source_loc = std::format("{}:{}", file, line);

  std::cerr << std::format("#{} 0x{:x} in {} ({} + 0x{:x}) at {}\n", frame_idx,
                           addr, function, display_module_path, offset,
                           source_loc);
}

std::string FormatFuncName(const char* func_name) {
  std::string demangled = func_name ? func_name : "??";
  if (func_name) {
    int status;
    char* demangled_name = abi::__cxa_demangle(func_name, nullptr, nullptr, &status);
    if (status == 0 && demangled_name) {
      demangled = demangled_name;
      free(demangled_name);
    }
  }

  size_t paren = demangled.find('(');
  if (paren != std::string::npos) {
    demangled = demangled.substr(0, paren) + "()";
  } else if (demangled != "??") {
    demangled += "()";
  }
  return demangled;
}

void FetchAndPrintSymbols(const std::vector<FrameInfo>& frames,
                          int stack_depth,
                          const std::string& strip_path_prefix) {
  static char* debuginfo_path = nullptr;
  static const Dwfl_Callbacks callbacks = {
      .find_elf = dwfl_build_id_find_elf,
      .find_debuginfo = dwfl_standard_find_debuginfo,
      .section_address = dwfl_offline_section_address,
      .debuginfo_path = &debuginfo_path,
  };

  Dwfl* dwfl = dwfl_begin(&callbacks);
  if (!dwfl) {
    for (int i = 0; i < stack_depth; ++i) {
      const auto& frame = frames[i];
      if (!frame.entry) {
        std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, frame.addr);
      } else {
        PrintSymbol(frame.addr, i, frame.entry->path, frame.offset_in_module,
                    strip_path_prefix, "??", "??", 0);
      }
    }
    return;
  }

  std::map<std::string, Dwfl_Module*> loaded_modules;

  // First pass: Report all modules
  for (int i = 0; i < stack_depth; ++i) {
    const auto& frame = frames[i];
    if (!frame.entry) continue;

    const std::string& path = frame.entry->path;
    if (loaded_modules.find(path) == loaded_modules.end()) {
      Dwfl_Module* mod = dwfl_report_offline(dwfl, path.c_str(), path.c_str(), -1);
      loaded_modules[path] = mod;
    }
  }

  dwfl_report_end(dwfl, nullptr, nullptr);

  // Second pass: resolve symbols
  for (int i = 0; i < stack_depth; ++i) {
    const auto& frame = frames[i];
    if (!frame.entry) {
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, frame.addr);
      continue;
    }

    const std::string& path = frame.entry->path;
    Dwfl_Module* mod = loaded_modules[path];
    bool printed = false;

    if (mod) {
      Dwarf_Addr mod_start, mod_end;
      dwfl_module_info(mod, nullptr, &mod_start, &mod_end, nullptr, nullptr, nullptr, nullptr);

      uintptr_t lookup_addr = mod_start + frame.offset_in_module;

      Dwarf_Addr bias;
      Dwarf_Die* cu = dwfl_module_addrdie(mod, lookup_addr, &bias);

      std::vector<std::pair<std::string, std::pair<std::string, int>>> stack;

      Dwfl_Line* line = dwfl_module_getsrc(mod, lookup_addr);
      std::string innermost_file = "??";
      int innermost_line = 0;
      if (line) {
        Dwarf_Addr addr2;
        int lineno, colno;
        const char* file = dwfl_lineinfo(line, &addr2, &lineno, &colno, nullptr, nullptr);
        if (file) {
          innermost_file = file;
          innermost_line = lineno;
        }
      }

      std::string current_file = innermost_file;
      int current_line = innermost_line;
      bool has_subprogram = false;

      if (cu) {
        Dwarf_Die* scopes;
        int n = dwarf_getscopes(cu, lookup_addr - bias, &scopes);
        if (n > 0) {
          for (int j = 0; j < n; ++j) {
            Dwarf_Die* scope = &scopes[j];
            int tag = dwarf_tag(scope);
            if (tag == DW_TAG_inlined_subroutine || tag == DW_TAG_subprogram) {
              if (tag == DW_TAG_subprogram) has_subprogram = true;

              const char* func_name = dwarf_diename(scope);
              if (!func_name) {
                Dwarf_Attribute attr;
                if (dwarf_attr(scope, DW_AT_linkage_name, &attr) || dwarf_attr(scope, DW_AT_MIPS_linkage_name, &attr)) {
                  func_name = dwarf_formstring(&attr);
                }
              }

              stack.push_back({FormatFuncName(func_name), {current_file, current_line}});

              if (tag == DW_TAG_inlined_subroutine) {
                Dwarf_Attribute attr;
                Dwarf_Word call_file_idx = 0, call_line = 0;
                Dwarf_Files* files;
                size_t nfiles;
                dwarf_getsrcfiles(cu, &files, &nfiles);

                if (dwarf_attr(scope, DW_AT_call_file, &attr)) {
                  dwarf_formudata(&attr, &call_file_idx);
                  const char* c_file = dwarf_filesrc(files, call_file_idx, nullptr, nullptr);
                  if (c_file) current_file = c_file;
                }
                if (dwarf_attr(scope, DW_AT_call_line, &attr)) {
                  dwarf_formudata(&attr, &call_line);
                  current_line = call_line;
                }
              }
            }
          }
          free(scopes);
        }
      }

      if (!has_subprogram) {
        const char* func = dwfl_module_addrname(mod, lookup_addr);
        stack.push_back({FormatFuncName(func), {current_file, current_line}});
      }

      if (!stack.empty()) {
        for (const auto& item : stack) {
          PrintSymbol(frame.addr, i, frame.entry->path, frame.offset_in_module,
                      strip_path_prefix, item.first, item.second.first,
                      item.second.second);
        }
        printed = true;
      }
    }

    if (!printed) {
      PrintSymbol(frame.addr, i, frame.entry->path, frame.offset_in_module,
                  strip_path_prefix, "??", "??", 0);
    }
  }

  dwfl_end(dwfl);
}

void ProcessCrash(const TracePacket& data,
                  std::map<uintptr_t, MapEntry>& maps,
                  const std::string& strip_path_prefix) {
  pid_t parent_pid = data.process_id;
  std::cerr << std::format("\n*** Process {} crashed with signal {} ***\n",
                           parent_pid, data.signal_number);

  if (maps.empty())
    maps = ReadMaps(parent_pid);

  std::vector<FrameInfo> frames = PopulateFrames(data, maps);

  FetchAndPrintSymbols(frames, data.stack_depth, strip_path_prefix);

  std::cerr << std::flush;
  char ack = 1;
  write(STDOUT_FILENO, &ack, 1);
}

}  // namespace

int main(int argc, char** argv) {
  std::string strip_path_prefix = "";
  if (argc >= 2) {
    strip_path_prefix = argv[1];
    if (!strip_path_prefix.empty() && !strip_path_prefix.ends_with("/")) {
      strip_path_prefix += "/";
    }
  }

  std::map<uintptr_t, MapEntry> maps;

  TracePacket packet;
  while (ReadFully(STDIN_FILENO, &packet, sizeof(packet))) {
    ProcessCrash(packet, maps, strip_path_prefix);
  }

  return 0;
}
