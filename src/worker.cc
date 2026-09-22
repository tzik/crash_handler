#include <fcntl.h>
#include <unistd.h>

#include <format>
#include <fstream>
#include <iostream>
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
  uintptr_t start, end, offset;
  std::string path;
  uintptr_t base_address;
};

struct FrameInfo {
  uintptr_t addr;
  const MapEntry* entry;
  uintptr_t offset_in_module;
};

using Maps = std::map<uintptr_t, MapEntry>;
using ProcessMaps = std::map<pid_t, Maps>;

std::optional<uintptr_t> GetBaseAddress(std::string_view path,
                                        uintptr_t map_start,
                                        uintptr_t map_offset) {
  auto error_or_mem_buf = llvm::MemoryBuffer::getFile(path);
  if (!error_or_mem_buf)
    return std::nullopt;

  auto exp_binary =
      llvm::object::createBinary(error_or_mem_buf.get()->getMemBufferRef());
  if (!exp_binary) {
    llvm::consumeError(exp_binary.takeError());
    return std::nullopt;
  }

  llvm::object::Binary* bin = exp_binary.get().get();
  auto* elf_obj_base = llvm::dyn_cast<llvm::object::ELFObjectFileBase>(bin);
  if (!elf_obj_base)
    return std::nullopt;

  uintptr_t page_size = sysconf(_SC_PAGESIZE);
  std::optional<uintptr_t> base_address;

  auto process_obj = [&](const auto* obj) {
    auto headers = obj->getELFFile().program_headers();
    if (!headers) {
      llvm::consumeError(headers.takeError());
      return;
    }

    for (const auto& phdr : *headers) {
      if (phdr.p_type != llvm::ELF::PT_LOAD ||
          (phdr.p_flags & llvm::ELF::PF_X) == 0)
        continue;

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
  };

  if (auto* elf_32_le =
          llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(elf_obj_base)) {
    process_obj(elf_32_le);
  } else if (auto* elf_64_le = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(
                 elf_obj_base)) {
    process_obj(elf_64_le);
  }

  return base_address;
}

Maps ReadMaps(pid_t pid) {
  Maps entries;
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
                                      const Maps& maps) {
  std::vector<FrameInfo> frames;

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
      frames.emplace_back(addr, nullptr, 0);
      continue;
    }

    uintptr_t offset_in_module = addr - entry->base_address;
    frames.emplace_back(addr, entry, offset_in_module);
  }

  return frames;
}

void PrintSymbol(const llvm::DILineInfo& sym,
                 uintptr_t addr,
                 int frame_idx,
                 std::string_view module_path,
                 uintptr_t offset,
                 std::string_view path_prefix) {
  std::string_view function = sym.FunctionName;
  if (function == "<invalid>")
    function = "??";
  std::string_view file = sym.FileName;
  if (file == "<invalid>")
    file = "??";

  if (!path_prefix.empty()) {
    if (file.starts_with(path_prefix))
      file = file.substr(path_prefix.length());
    if (module_path.starts_with(path_prefix))
      module_path = module_path.substr(path_prefix.length());
  }
  int line = sym.Line;

  std::string source_loc = std::format("{}:{}", file, line);

  std::cerr << std::format("#{} 0x{:x} in {} ({} + 0x{:x}) at {}\n", frame_idx,
                           addr, function, module_path, offset, source_loc);
}

void FetchAndPrintSymbols(llvm::symbolize::LLVMSymbolizer& symbolizer,
                          const std::vector<FrameInfo>& frames,
                          int stack_depth,
                          std::string_view path_prefix) {
  for (int i = 0; i < stack_depth; ++i) {
    const auto& frame = frames[i];
    if (!frame.entry) {
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, frame.addr);
      continue;
    }

    auto res_or_err = symbolizer.symbolizeInlinedCode(
        frame.entry->path,
        {frame.offset_in_module, llvm::object::SectionedAddress::UndefSection});

    bool printed = false;
    if (res_or_err) {
      const auto& inlining_info = res_or_err.get();
      int num_frames = inlining_info.getNumberOfFrames();
      if (num_frames > 0) {
        for (int j = 0; j < num_frames; ++j) {
          PrintSymbol(inlining_info.getFrame(j), frame.addr, i,
                      frame.entry->path, frame.offset_in_module, path_prefix);
        }
        printed = true;
      }
    } else {
      llvm::consumeError(res_or_err.takeError());
    }

    if (!printed) {
      std::cerr << std::format("#{} 0x{:x} in ?? ({} + 0x{:x}) at ??:0\n", i,
                               frame.addr, frame.entry->path,
                               frame.offset_in_module);
    }
  }
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
  std::vector<FrameInfo> frames = PopulateFrames(data, maps);

  FetchAndPrintSymbols(symbolizer, frames, data.stack_depth, path_prefix);

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
