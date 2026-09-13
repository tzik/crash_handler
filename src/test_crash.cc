#include "crash_handler.h"
#include <assert.h>
#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((always_inline)) inline void inline_function() {
  __builtin_trap();
}

__attribute__((noinline)) void crash_function() {
  inline_function();
}

__attribute__((noinline)) void intermediate_function_2() {
  crash_function();
}

__attribute__((noinline)) void intermediate_function() {
  intermediate_function_2();
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0]
              << " <worker_path> <llvm_symbolizer_path>\n";
    return 1;
  }

  // fork and crash in child to test it
  pid_t pid = fork();
  if (pid == 0) {
    SetUpCrashHandler(argv[1], argv[2]);
    intermediate_function();
    exit(1);
  } else {
    int status;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
      std::cout << "Child crashed with signal " << WTERMSIG(status) << "\n";
      return 0;
    }
    return 1;  // test failed if child didn't crash with signal
  }
}
