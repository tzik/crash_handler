#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_PROCMAP_QUERY
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include "trace_packet.h"
#include "util.h"

namespace {

struct MapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t offset = 0;
  std::string path;
  uintptr_t base_address = std::numeric_limits<uintptr_t>::max();
};

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
using ProcessMaps = std::map<pid_t, Maps>;
using Frames = std::vector<FrameInfo>;
using Symbols = std::vector<SymbolInfo>;

void GetBaseAddress(std::string_view path,
                    std::vector<MapEntry>* entries) {
  auto error_or_mem_buf = llvm::MemoryBuffer::getFile(path);
  if (!error_or_mem_buf)
    return;

  auto exp_binary =
      llvm::object::createBinary(error_or_mem_buf.get()->getMemBufferRef());
  if (!exp_binary) {
    llvm::consumeError(exp_binary.takeError());
    return;
  }

  llvm::object::Binary* bin = exp_binary.get().get();
  auto* elf_obj_base = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(bin);
  if (!elf_obj_base)
    return;

  uintptr_t page_size = sysconf(_SC_PAGESIZE);

  auto process_obj = [&](const auto* obj) {
    auto headers = obj->getELFFile().program_headers();
    if (!headers) {
      llvm::consumeError(headers.takeError());
      return;
    }

    for (auto& entry : *entries) {
      for (const auto& phdr : *headers) {
        if (phdr.p_type != llvm::ELF::PT_LOAD ||
            (phdr.p_flags & llvm::ELF::PF_X) == 0)
          continue;

        uintptr_t phdr_offset_aligned = phdr.p_offset & ~(page_size - 1);
        uintptr_t phdr_end =
            (phdr.p_offset + phdr.p_filesz + page_size - 1) & ~(page_size - 1);

        if (entry.offset >= phdr_offset_aligned && entry.offset < phdr_end) {
          uintptr_t vaddr_in_file = (phdr.p_vaddr & ~(page_size - 1)) +
                                    (entry.offset - phdr_offset_aligned);
          entry.base_address = entry.start - vaddr_in_file;
          break;
        }
      }
    }
  };

  if (auto* elf_32_le =
          llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(elf_obj_base)) {
    process_obj(elf_32_le);
  } else if (auto* elf_64_le = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(
                 elf_obj_base)) {
    process_obj(elf_64_le);
  }
}

#ifdef HAVE_PROCMAP_QUERY
bool ReadMapsIoctl(pid_t pid, std::map<std::string, std::vector<MapEntry>>* entries_by_path) {
  std::string maps_path = std::format("/proc/{}/maps", pid);
  unique_fd fd(open(maps_path.c_str(), O_RDONLY));
  if (!fd.is_valid()) return false;

  struct procmap_query q = {};
  char name_buf[4096];
  bool success = true;

  q.size = sizeof(q);
  q.query_flags = PROCMAP_QUERY_COVERING_OR_NEXT_VMA;
  q.query_addr = 0;
  q.vma_name_size = sizeof(name_buf);
  q.vma_name_addr = reinterpret_cast<uintptr_t>(name_buf);

  while (true) {
    int ret = ioctl(fd.get(), PROCMAP_QUERY, &q);
    if (ret < 0) {
      if (errno == ENOTTY || errno == EINVAL) {
        success = false;
      }
      break;
    }

    if ((q.vma_flags & PROCMAP_QUERY_VMA_EXECUTABLE) && q.vma_name_size > 0 && name_buf[0] == '/') {
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

  if (!success) {
    entries_by_path->clear();
  }
  return success;
}
#endif

void ReadMapsText(pid_t pid, std::map<std::string, std::vector<MapEntry>>* entries_by_path) {
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
  std::map<std::string, std::vector<MapEntry>> entries_by_path;

#ifdef HAVE_PROCMAP_QUERY
  if (!ReadMapsIoctl(pid, &entries_by_path)) {
    ReadMapsText(pid, &entries_by_path);
  }
#else
  ReadMapsText(pid, &entries_by_path);
#endif

  for (auto& [path, group] : entries_by_path) {
    GetBaseAddress(path, &group);
    for (auto& e : group) {
      if (e.base_address != std::numeric_limits<uintptr_t>::max()) {
        entries[e.start] = std::move(e);
      }
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
      if (addr >= it->second.start && addr < it->second.end) {
        entry = &it->second;
      }
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

Symbols Symbolize(llvm::symbolize::LLVMSymbolizer& symbolizer,
                  const Frames& frames) {
  Symbols symbols;
  symbols.reserve(2 * frames.size());

  for (const auto& frame : frames) {
    symbols.emplace_back(&frame);

    if (!frame.entry)
      continue;

    auto res_or_err = symbolizer.symbolizeInlinedCode(
        frame.entry->path,
        {frame.offset_in_module, llvm::object::SectionedAddress::UndefSection});

    if (!res_or_err) {
      llvm::consumeError(res_or_err.takeError());
      continue;
    }

    const auto& inlining_info = res_or_err.get();
    int num_frames = inlining_info.getNumberOfFrames();
    if (num_frames)
      symbols.pop_back();

    for (int j = 0; j < num_frames; ++j) {
      const llvm::DILineInfo& sym = inlining_info.getFrame(j);
      std::string_view function = sym.FunctionName;
      std::string_view file = sym.FileName;
      int line = sym.Line;
      symbols.emplace_back(&frame, std::string(function), std::string(file),
                           line);
    }
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

void ProcessTracePacket(llvm::symbolize::LLVMSymbolizer& symbolizer,
                        const TracePacket& data,
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
  Symbols symbols = Symbolize(symbolizer, frames);
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

  llvm::symbolize::LLVMSymbolizer symbolizer;
  ProcessMaps process_maps;

  TracePacket packet;
  while (ReadFully(STDIN_FILENO, &packet, sizeof(packet)))
    ProcessTracePacket(symbolizer, packet, process_maps, path_prefix);

  return 0;
}
