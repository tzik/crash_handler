#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include "crash_handler.h"

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
              << " <worker_path> <llvm_symbolizer_path> [strip_path_prefix]\n";
    return 1;
  }

  const char* strip_path_prefix = nullptr;
  if (argc >= 4) {
    strip_path_prefix = argv[3];
  }

  SetUpCrashHandler(argv[1], argv[2], strip_path_prefix);

  pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "fork failed\n";
    return 1;
  }

  if (pid == 0) {
    intermediate_function();
    exit(1);
  }

  int status;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status)) {
    std::cout << "Child crashed with signal " << WTERMSIG(status) << "\n";
  }

  pid_t pid2 = fork();
  if (pid2 < 0) {
    std::cerr << "fork failed\n";
    return 1;
  }

  if (pid2 == 0) {
    intermediate_function();
    exit(1);
  }

  waitpid(pid2, &status, 0);
  if (WIFSIGNALED(status)) {
    std::cout << "Child 2 crashed with signal " << WTERMSIG(status) << "\n";
  }

  return 0;
}
