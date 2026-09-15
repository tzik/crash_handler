
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

#include <nlohmann/json.hpp>

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

std::optional<uintptr_t> GetBaseAddress(const std::string& path, uintptr_t map_start, uintptr_t map_offset) {
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
    uintptr_t phdr_end = (phdr.p_offset + phdr.p_filesz + page_size - 1) & ~(page_size - 1);

    if (map_offset >= phdr_offset_aligned && map_offset < phdr_end) {
      // map_start corresponds to phdr_offset_aligned mapped to memory
      // The virtual address of map_start in ELF is phdr.p_vaddr & ~(page_size - 1)
      // Therefore, vaddr for map_offset = (phdr.p_vaddr & ~(page_size - 1)) + (map_offset - phdr_offset_aligned)
      uintptr_t vaddr_in_file = (phdr.p_vaddr & ~(page_size - 1)) + (map_offset - phdr_offset_aligned);
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

void PrintSymbol(const nlohmann::json& sym,
                 uintptr_t addr,
                 int frame_idx,
                 const std::string& module_path,
                 uintptr_t offset) {
  std::string function = sym.value("FunctionName", "??");
  std::string file = sym.value("FileName", "??");
  int line = sym.value("Line", 0);
  std::string source_loc = std::format("{}:{}", file, line);

  std::cerr << std::format("#{} 0x{:x} in {} ({} + 0x{:x}) at {}\n", frame_idx,
                           addr, function, module_path, offset, source_loc);
}

void PrintFrames(const nlohmann::json& j,
                 uintptr_t addr,
                 int frame_idx,
                 const std::string& module_path,
                 uintptr_t offset,
                 bool& printed) {
  if (j.is_array() && !j.empty()) {
    auto& syms = j[0]["Symbol"];
    if (!syms.is_array() || syms.empty())
      return;
    for (const auto& sym : syms)
      PrintSymbol(sym, addr, frame_idx, module_path, offset);
    printed = true;
    return;
  }

  if (j.is_object() && j.contains("Symbol") && j["Symbol"].is_array() &&
      !j["Symbol"].empty()) {
    for (const auto& sym : j["Symbol"]) {
      PrintSymbol(sym, addr, frame_idx, module_path, offset);
    }
    printed = true;
    return;
  }
}

pid_t SpawnSymbolizer(const std::string& llvm_symbolizer_path,
                      int* out_pipe_write,
                      int* out_pipe_read) {
  int pipe_to_sym[2] = {-1, -1};
  int pipe_from_sym[2] = {-1, -1};
  pid_t sym_pid = -1;

  if (pipe(pipe_to_sym) < 0 || pipe(pipe_from_sym) < 0) {
    goto cleanup;
  }

  {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    posix_spawn_file_actions_adddup2(&actions, pipe_to_sym[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_from_sym[1], STDOUT_FILENO);

    posix_spawn_file_actions_addclose(&actions, pipe_to_sym[1]);
    posix_spawn_file_actions_addclose(&actions, pipe_from_sym[0]);

    std::vector<std::string> args = {llvm_symbolizer_path,
                                     "--output-style=JSON"};
    std::vector<char*> child_argv = MakeArgV(&args);

    if (posix_spawn(&sym_pid, llvm_symbolizer_path.c_str(), &actions, nullptr,
                    child_argv.data(), environ) < 0) {
      posix_spawn_file_actions_destroy(&actions);
      goto cleanup;
    }
    posix_spawn_file_actions_destroy(&actions);
  }

  *out_pipe_write = std::exchange(pipe_to_sym[1], -1);
  *out_pipe_read = std::exchange(pipe_from_sym[0], -1);

cleanup:
  if (pipe_to_sym[0] >= -1)
    close(pipe_to_sym[0]);
  if (pipe_to_sym[1] >= -1)
    close(pipe_to_sym[1]);
  if (pipe_from_sym[0] >= -1)
    close(pipe_from_sym[0]);
  if (pipe_from_sym[1] >= -1)
    close(pipe_from_sym[1]);

  return sym_pid;
}

std::vector<FrameInfo> PopulateFrames(const TracePacket& data,
                                      const std::map<uintptr_t, MapEntry>& maps,
                                      std::string* out_query) {
  std::vector<FrameInfo> frames;
  std::ostringstream oss;
  bool dump_maps = getenv("CRASH_HANDLER_DUMP_MAPS") != nullptr;

  if (dump_maps) {
    std::cerr << "--- CRASH_HANDLER_DUMP_MAPS ---\n";
    for (const auto& [start, entry] : maps) {
      std::cerr << std::format("{:x}-{:x} offset={:x} base={:x} {}\n",
                               entry.start, entry.end, entry.offset, entry.base_address, entry.path);
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
        std::cerr << std::format("Frame #{}: addr=0x{:x} (no map entry found)\n", i, addr);
      }
      continue;
    }

    uintptr_t offset_in_module = addr - entry->base_address;

    if (dump_maps) {
      std::cerr << std::format("Frame #{}: addr=0x{:x} base=0x{:x} offset=0x{:x} path={}\n",
                               i, addr, entry->base_address, offset_in_module, entry->path);
    }

    frames.push_back({addr, entry, offset_in_module});
    oss << std::format("{} 0x{:x}\n", entry->path, offset_in_module);
  }

  *out_query = oss.str();
  return frames;
}

void FetchAndPrintSymbols(const std::vector<FrameInfo>& frames,
                          FILE* sym_out,
                          FILE* sym_in,
                          const std::string& query,
                          int stack_depth) {
  int valid_frames_count = 0;
  for (const auto& f : frames) {
    if (f.entry)
      valid_frames_count++;
  }

  if (valid_frames_count > 0) {
    fwrite(query.c_str(), 1, query.size(), sym_out);
    fflush(sym_out);
  }

  std::vector<nlohmann::json> parsed_jsons;

  for (int i = 0; i < valid_frames_count; ++i) {
    char* line_ptr = nullptr;
    size_t len = 0;
    if (getline(&line_ptr, &len, sym_in) != -1) {
      std::string line = line_ptr;
      free(line_ptr);

      try {
        auto j = nlohmann::json::parse(line);
        parsed_jsons.push_back(std::move(j));
      } catch (...) {
        parsed_jsons.push_back(nlohmann::json());
      }
    } else {
      if (line_ptr)
        free(line_ptr);
      parsed_jsons.push_back(nlohmann::json());
    }
  }

  int json_idx = 0;
  for (int i = 0; i < stack_depth; ++i) {
    const auto& frame = frames[i];
    if (!frame.entry) {
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, frame.addr);
      continue;
    }

    bool printed = false;
    if (json_idx < parsed_jsons.size()) {
      PrintFrames(parsed_jsons[json_idx], frame.addr, i, frame.entry->path,
                  frame.offset_in_module, printed);
      json_idx++;
    }

    if (!printed) {
      std::cerr << std::format("#{} 0x{:x} in ?? ({} + 0x{:x}) at ??:0\n", i,
                               frame.addr, frame.entry->path,
                               frame.offset_in_module);
    }
  }
}

void ProcessCrash(const TracePacket& data,
                  std::map<uintptr_t, MapEntry>& maps,
                  FILE* sym_out,
                  FILE* sym_in) {
  pid_t parent_pid = data.process_id;
  std::cerr << std::format("\n*** Process {} crashed with signal {} ***\n",
                           parent_pid, data.signal_number);

  if (maps.empty()) {
    maps = ReadMaps(parent_pid);
    if (maps.empty()) {
      std::cerr << "Maps empty, aborting.\n";
      _exit(1);
    }
  }

  std::string query;
  std::vector<FrameInfo> frames = PopulateFrames(data, maps, &query);

  FetchAndPrintSymbols(frames, sym_out, sym_in, query, data.stack_depth);

  std::cerr << std::flush;
  char ack = 1;
  write(STDOUT_FILENO, &ack, 1);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2)
    return 1;

  std::string llvm_symbolizer_path = argv[1];

  int sym_pipe_write = -1;
  int sym_pipe_read = -1;
  pid_t sym_pid =
      SpawnSymbolizer(llvm_symbolizer_path, &sym_pipe_write, &sym_pipe_read);

  if (sym_pid < 0)
    return 1;

  FILE* sym_out = fdopen(sym_pipe_write, "w");
  if (!sym_out) {
    close(sym_pipe_write);
    close(sym_pipe_read);
    return 1;
  }

  FILE* sym_in = fdopen(sym_pipe_read, "r");
  if (!sym_in) {
    fclose(sym_out);
    close(sym_pipe_read);
    return 1;
  }

  std::map<uintptr_t, MapEntry> maps;

  TracePacket packet;
  while (ReadFully(STDIN_FILENO, &packet, sizeof(packet))) {
    ProcessCrash(packet, maps, sym_out, sym_in);
  }

  fclose(sym_out);
  fclose(sym_in);

  int status;
  waitpid(sym_pid, &status, 0);

  return 0;
}
