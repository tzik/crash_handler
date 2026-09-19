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

void FetchAndPrintSymbols(const TracePacket& data,
                          const std::string& strip_path_prefix) {
  static char* debuginfo_path = nullptr;
  static const Dwfl_Callbacks callbacks = {
      .find_elf = dwfl_linux_proc_find_elf,
      .find_debuginfo = dwfl_standard_find_debuginfo,
      .section_address = dwfl_offline_section_address,
      .debuginfo_path = &debuginfo_path,
  };

  Dwfl* dwfl = dwfl_begin(&callbacks);
  if (!dwfl) {
    for (int i = 0; i < data.stack_depth; ++i) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(data.stack[i]);
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, addr);
    }
    return;
  }

  int report_err = dwfl_linux_proc_report(dwfl, data.process_id);
  if (report_err != 0) {
    std::cerr << "dwfl_linux_proc_report failed: " << (dwfl_errmsg(-1) ? dwfl_errmsg(-1) : "unknown error") << "\n";
    for (int i = 0; i < data.stack_depth; ++i) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(data.stack[i]);
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, addr);
    }
    dwfl_end(dwfl);
    return;
  }
  dwfl_report_end(dwfl, nullptr, nullptr);

  for (int i = 0; i < data.stack_depth; ++i) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(data.stack[i]);
    Dwfl_Module* mod = dwfl_addrmodule(dwfl, addr);

    if (!mod) {
      std::cerr << std::format("#{} 0x{:x} (unknown)\n", i, addr);
      continue;
    }


    bool printed = false;

    Dwarf_Addr mod_start, mod_end;
    const char* mod_name = dwfl_module_info(mod, nullptr, &mod_start, &mod_end, nullptr, nullptr, nullptr, nullptr);
    const char* mod_file = dwfl_module_info(mod, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    // Some logic to get the correct path
    std::string path = mod_file ? mod_file : (mod_name ? mod_name : "??");
    uintptr_t offset_in_module = addr - mod_start;

    Dwarf_Addr bias;
    Dwarf_Die* cu = dwfl_module_addrdie(mod, addr, &bias);

    std::vector<std::pair<std::string, std::pair<std::string, int>>> stack;

    Dwfl_Line* line = dwfl_module_getsrc(mod, addr);
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


    if (innermost_line == 0 && cu) {
      Dwarf_Die* scopes;
      if (dwarf_getscopes(cu, addr - bias, &scopes) > 0) {
        Dwarf_Die* scope = &scopes[0];
        Dwarf_Attribute attr;
        Dwarf_Word decl_file_idx = 0, decl_line = 0;
        if (dwarf_attr(scope, DW_AT_decl_file, &attr)) {
          dwarf_formudata(&attr, &decl_file_idx);
          Dwarf_Files* files;
          size_t nfiles;
          if (dwarf_getsrcfiles(cu, &files, &nfiles) == 0) {
            const char* decl_file = dwarf_filesrc(files, decl_file_idx, nullptr, nullptr);
            if (decl_file) {
              innermost_file = decl_file;
            }
          }
        }
        if (dwarf_attr(scope, DW_AT_decl_line, &attr)) {
          dwarf_formudata(&attr, &decl_line);
          innermost_line = decl_line;
        }
        free(scopes);
      }
    }

    std::string current_file = innermost_file;

    int current_line = innermost_line;
    bool has_subprogram = false;

    if (cu) {
      Dwarf_Die* scopes;
      int n = dwarf_getscopes(cu, addr - bias, &scopes);
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
      const char* func = dwfl_module_addrname(mod, addr);
      stack.push_back({FormatFuncName(func), {current_file, current_line}});
    }

    if (!stack.empty()) {
      for (const auto& item : stack) {
        PrintSymbol(addr, i, path, offset_in_module,
                    strip_path_prefix, item.first, item.second.first,
                    item.second.second);
      }
      printed = true;
    }

    if (!printed) {
      PrintSymbol(addr, i, path, offset_in_module,
                  strip_path_prefix, "??", "??", 0);
    }
  }

  dwfl_end(dwfl);
}

void ProcessCrash(const TracePacket& data,
                  const std::string& strip_path_prefix) {
  pid_t parent_pid = data.process_id;
  std::cerr << std::format("\n*** Process {} crashed with signal {} ***\n",
                           parent_pid, data.signal_number);

  FetchAndPrintSymbols(data, strip_path_prefix);

  std::cerr << std::flush;
  char ack = 1;
  if (write(STDOUT_FILENO, &ack, 1) < 0) {}
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

  TracePacket packet;
  while (ReadFully(STDIN_FILENO, &packet, sizeof(packet))) {
    ProcessCrash(packet, strip_path_prefix);
  }

  return 0;
}
