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
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>
#include "trace_packet.h"
#include "util.h"

extern char** environ;

namespace {

struct MapEntry {
  uintptr_t start, end, offset;
  std::string path;
  uintptr_t load_bias;
};

uintptr_t GetLoadBias(const std::string& path) {
  if (elf_version(EV_CURRENT) == EV_NONE)
    return 0;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return 0;

  Elf* elf = elf_begin(fd, ELF_C_READ, nullptr);
  if (!elf) {
    close(fd);
    return 0;
  }

  size_t phnum;
  if (elf_getphdrnum(elf, &phnum) != 0) {
    elf_end(elf);
    close(fd);
    return 0;
  }

  uintptr_t min_vaddr = std::numeric_limits<uintptr_t>::max();
  for (size_t i = 0; i < phnum; ++i) {
    GElf_Phdr phdr;
    if (gelf_getphdr(elf, i, &phdr) != &phdr)
      continue;
    if (phdr.p_type != PT_LOAD)
      continue;
    if (phdr.p_vaddr < min_vaddr) {
      min_vaddr = phdr.p_vaddr;
    }
  }

  elf_end(elf);
  close(fd);

  if (min_vaddr == std::numeric_limits<uintptr_t>::max())
    return 0;
  return min_vaddr;
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
    e.load_bias = GetLoadBias(path);
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
    for (const auto& sym : syms) {
      PrintSymbol(sym, addr, frame_idx, module_path, offset);
    }
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2)
    return 1;

  std::string llvm_symbolizer_path = argv[1];

  TracePacket data;
  ssize_t to_read = sizeof(data);
  char* p = reinterpret_cast<char*>(&data);

  while (to_read > 0) {
    ssize_t res = read(STDIN_FILENO, p, to_read);
    if (res <= 0)
      break;
    p += res;
    to_read -= res;
  }

  if (to_read != 0)
    return 1;

  pid_t parent_pid = data.process_id;
  std::cerr << std::format("\n*** Process {} crashed with signal {} ***\n",
                           parent_pid, data.signal_number);

  auto maps = ReadMaps(parent_pid);

  int pipe_to_sym[2];
  if (pipe(pipe_to_sym) < 0)
    return 1;

  int pipe_from_sym[2];
  if (pipe(pipe_from_sym) < 0) {
    close(pipe_to_sym[0]);
    close(pipe_to_sym[1]);
    return 1;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

  posix_spawn_file_actions_adddup2(&actions, pipe_to_sym[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipe_from_sym[1], STDOUT_FILENO);

  posix_spawn_file_actions_addclose(&actions, pipe_to_sym[1]);
  posix_spawn_file_actions_addclose(&actions, pipe_from_sym[0]);

  std::vector<std::string> args = {llvm_symbolizer_path, "--output-style=JSON"};
  std::vector<char*> child_argv = MakeArgV(&args);

  pid_t sym_pid;
  if (posix_spawn(&sym_pid, llvm_symbolizer_path.c_str(), &actions, nullptr,
                  child_argv.data(), environ) < 0) {
    close(pipe_to_sym[0]);
    close(pipe_to_sym[1]);
    close(pipe_from_sym[0]);
    close(pipe_from_sym[1]);
    posix_spawn_file_actions_destroy(&actions);
    return 1;
  }

  close(pipe_to_sym[0]);
  close(pipe_from_sym[1]);

  struct FrameInfo {
    uintptr_t addr;
    const MapEntry* entry;
    uintptr_t offset_in_module;
  };
  std::vector<FrameInfo> frames;

  std::string query = "";
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
      continue;
    }

    uintptr_t offset_in_module =
        addr - entry->start + entry->offset + entry->load_bias;

    frames.push_back({addr, entry, offset_in_module});
    query += std::format("{} 0x{:x}\n", entry->path, offset_in_module);
  }

  write(pipe_to_sym[1], query.c_str(), query.size());
  close(pipe_to_sym[1]);

  std::string result = "";
  char buf[4096];
  ssize_t n;
  while ((n = read(pipe_from_sym[0], buf, sizeof(buf) - 1)) > 0) {
    buf[n] = '\0';
    result += buf;
  }
  close(pipe_from_sym[0]);

  int status;
  waitpid(sym_pid, &status, 0);
  posix_spawn_file_actions_destroy(&actions);

  // Parse result lines
  std::vector<std::string> json_lines;
  std::istringstream iss(result);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty()) {
      json_lines.push_back(line);
    }
  }

  int json_idx = 0;
  for (int i = 0; i < data.stack_depth; ++i) {
    const auto& frame = frames[i];
    if (!frame.entry) {
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, frame.addr);
      continue;
    }

    bool printed = false;
    if (json_idx < json_lines.size()) {
      try {
        auto j = nlohmann::json::parse(json_lines[json_idx]);
        PrintFrames(j, frame.addr, i, frame.entry->path, frame.offset_in_module,
                    printed);
      } catch (...) {
      }
      json_idx++;
    }

    if (!printed) {
      std::cerr << std::format("#{} 0x{:x} in ?? ({} + 0x{:x}) at ??:0\n", i,
                               frame.addr, frame.entry->path,
                               frame.offset_in_module);
    }
  }

  char ack = 1;
  write(STDOUT_FILENO, &ack, 1);

  return 0;
}
