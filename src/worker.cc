#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_PROCMAP_QUERY
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trace_packet.h"
#include "util.h"
#include "map_entry.h"
#include "dwarf/elf_parser.h"
#include "dwarf/dwarf.h"

namespace {

struct FrameInfo {
  int frame_index = 0;
  uintptr_t addr = 0;
  const MapEntry* entry = nullptr;
  uintptr_t offset_in_module = 0;
};

struct SymbolInfo {
  const FrameInfo* frame = nullptr;
  std::string function;
  std::string file;
  int line = 0;
};

using Maps = std::map<uintptr_t, MapEntry>;
using ProcessMaps = std::unordered_map<pid_t, Maps>;
using Frames = std::vector<FrameInfo>;
using Symbols = std::vector<SymbolInfo>;
using BaseAddressQueries =
    std::unordered_map<std::string, std::vector<MapEntry>>;

void GetBaseAddress(std::string_view path, std::vector<MapEntry>* entries) {
  std::string path_str(path);
  ElfParser parser(path_str);
  if (parser.is_valid()) {
    parser.calculate_base_addresses(entries->data(), entries->size());
  }
}

#ifdef HAVE_PROCMAP_QUERY
bool ReadMapsIoctl(pid_t pid, BaseAddressQueries* entries_by_path) {
  std::string maps_path = std::format("/proc/{}/maps", pid);
  unique_fd fd(open(maps_path.c_str(), O_RDONLY));
  if (!fd.is_valid())
    return false;

  struct procmap_query q = {};
  char name_buf[4096];

  q.size = sizeof(q);
  q.query_flags = PROCMAP_QUERY_COVERING_OR_NEXT_VMA;
  q.query_addr = 0;
  q.vma_name_size = sizeof(name_buf);
  q.vma_name_addr = reinterpret_cast<uintptr_t>(name_buf);

  int ret;
  while ((ret = ioctl(fd.get(), PROCMAP_QUERY, &q)) == 0) {
    if ((q.vma_flags & PROCMAP_QUERY_VMA_EXECUTABLE) && q.vma_name_size > 0 &&
        name_buf[0] == '/') {
      MapEntry e;
      e.start = q.vma_start;
      e.end = q.vma_end;
      e.offset = q.vma_offset;
      e.path = std::string(name_buf, q.vma_name_size - 1);
      (*entries_by_path)[e.path].push_back(std::move(e));
    }

    q.query_addr = q.vma_end;
    q.vma_name_size = sizeof(name_buf);
  }

  if (ret < 0 && (errno == ENOTTY || errno == EINVAL)) {
    entries_by_path->clear();
    return false;
  }

  return true;
}
#endif

void ReadMapsText(pid_t pid, BaseAddressQueries* entries_by_path) {
  std::string maps_path = std::format("/proc/{}/maps", pid);
  std::ifstream maps(maps_path);
  std::string line;
  std::string addr, perms, offset, dev, inode, path;
  while (std::getline(maps, line)) {
    std::istringstream iss(line);
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

    (*entries_by_path)[e.path].push_back(std::move(e));
  }
}

Maps ReadMaps(pid_t pid) {
  Maps entries;
  BaseAddressQueries entries_by_path;

#ifdef HAVE_PROCMAP_QUERY
  if (!ReadMapsIoctl(pid, &entries_by_path))
    ReadMapsText(pid, &entries_by_path);
#else
  ReadMapsText(pid, &entries_by_path);
#endif

  for (auto& [path, group] : entries_by_path) {
    GetBaseAddress(path, &group);
    for (auto& e : group) {
      if (e.base_address != std::numeric_limits<uintptr_t>::max())
        entries[e.start] = std::move(e);
    }
  }

  return entries;
}

Frames PopulateFrames(const TracePacket& data, const Maps& maps) {
  Frames frames;
  size_t n = std::min<size_t>(data.stack_depth, std::size(data.stack));
  frames.reserve(n);

  for (int i = 0; i < n; ++i) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(data.stack[i]);

    const MapEntry* entry = nullptr;
    auto it = maps.upper_bound(addr);
    if (it != maps.begin()) {
      --it;
      if (addr >= it->second.start && addr < it->second.end)
        entry = &it->second;
    }

    if (!entry) {
      frames.emplace_back(i, addr, nullptr, 0);
      continue;
    }

    uintptr_t offset_in_module = addr - entry->base_address;
    frames.emplace_back(i, addr, entry, offset_in_module);
  }

  return frames;
}

#include <string.h>
Symbols Symbolize(const Frames& frames) {
  Symbols symbols;
  symbols.reserve(2 * frames.size());

  std::unordered_map<std::string, DwarfParser*> dwarf_parsers;

  for (const auto& frame : frames) {
    symbols.emplace_back(&frame);

    if (!frame.entry)
      continue;

    auto& path = frame.entry->path;
    if (dwarf_parsers.find(path) == dwarf_parsers.end()) {
        ElfParser elf(path);
        dwarf_parsers[path] = new DwarfParser(elf);
    }

    DwarfParser* parser = dwarf_parsers[path];
    auto inline_frames = parser->symbolize(frame.offset_in_module);

    if (!inline_frames.empty()) {
        symbols.pop_back(); // Remove the empty symbol we just added
    } else {
        continue; // Keep the empty symbol
    }

    for (const auto& inlined_frame : inline_frames) {
      symbols.emplace_back(&frame, inlined_frame.function_name, inlined_frame.file_name,
                           inlined_frame.line);
    }
  }

  for (auto& pair : dwarf_parsers) {
      delete pair.second;
  }

  return symbols;
}

void PrintSymbol(std::ostream& out,
                 const SymbolInfo& symbol,
                 std::string_view path_prefix) {
  const FrameInfo& frame = *symbol.frame;
  const MapEntry& entry = *frame.entry;
  out << "#" << frame.frame_index << " 0x" << std::hex << frame.addr;
  if (!symbol.function.empty() && symbol.function != "<invalid>")
    out << " in " << symbol.function;
  if (entry.path.empty() || entry.path == "<invalid>") {
    out << " (unknown)";
  } else {
    std::string_view module_path = entry.path;
    if (module_path.starts_with(path_prefix))
      module_path = module_path.substr(path_prefix.length());
    out << " (" << module_path << " + 0x" << std::hex << frame.offset_in_module
        << ")";
  }
  if (!symbol.file.empty() && symbol.file != "<invalid>") {
    std::string_view file = symbol.file;
    if (file.starts_with(path_prefix))
      file = file.substr(path_prefix.length());
    out << " at " << file << ":" << std::dec << symbol.line;
  }
  out << "\n";
}

void ProcessTracePacket(const TracePacket& data,
                        ProcessMaps& process_maps,
                        std::string_view path_prefix) {
  pid_t pid = data.process_id;
  std::cerr << "\n*** Process " << pid << " crashed";
  if (const char* name = sigabbrev_np(data.signal_number))
    std::cerr << " with signal SIG" << name;
  std::cerr << " ***\n";

  auto [found, inserted] = process_maps.emplace(pid, Maps{});
  auto& maps = found->second;
  if (inserted)
    maps = ReadMaps(pid);
  Frames frames = PopulateFrames(data, maps);
  Symbols symbols = Symbolize(frames);
  for (auto&& symbol : symbols)
    PrintSymbol(std::cerr, symbol, path_prefix);

  std::cerr << std::flush;
  char ack = 1;
  if (write(STDOUT_FILENO, &ack, 1) != 1)
    exit(1);
}

}  // namespace

int main(int argc, char** argv) {
  std::string path_prefix;
  if (argc >= 2) {
    path_prefix = argv[1];
    if (!path_prefix.empty() && !path_prefix.ends_with("/"))
      path_prefix += "/";
  }

  ProcessMaps process_maps;

  TracePacket packet;
  while (ReadFully(STDIN_FILENO, &packet, sizeof(packet)))
    ProcessTracePacket(packet, process_maps, path_prefix);

  return 0;
}
