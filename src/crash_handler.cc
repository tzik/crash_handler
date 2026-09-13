#include "crash_handler.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "absl/debugging/stacktrace.h"
#include "crash_data.h"

extern char** environ;

namespace {

int worker_stdin_fd = -1;
int worker_stdout_fd = -1;

void WriteFully(int fd, const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  size_t to_write = size;
  while (to_write > 0) {
    ssize_t res = write(fd, p, to_write);
    if (res > 0) {
      p += res;
      to_write -= res;
    } else if (res < 0) {
      break;
    }
  }
}

void CrashSignalHandler(int signo, siginfo_t* info, void* context) {
  CrashData data;
  data.process_id = getpid();
  data.signal_number = signo;

  // Skip this frame and the signal handler frame
  data.stack_depth =
      absl::GetStackTraceWithContext(data.stack, 128, 1, context, nullptr);

  // Write data to worker via its stdin
  WriteFully(worker_stdin_fd, &data, sizeof(data));

  // Wait for confirmation byte
  char ack = 0;
  read(worker_stdout_fd, &ack, 1);

  // Reset signal handler to default
  struct sigaction sa = {};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(signo, &sa, nullptr);

  // Re-raise signal
  raise(signo);
}

}  // namespace

void SetUpCrashHandler(const char* worker_path,
                       const char* llvm_symbolizer_path) {
  int pipe_to_worker[2];
  int pipe_from_worker[2];

  if (pipe2(pipe_to_worker, O_CLOEXEC) != 0) {
    return;
  }
  if (pipe2(pipe_from_worker, O_CLOEXEC) != 0) {
    close(pipe_to_worker[0]);
    close(pipe_to_worker[1]);
    return;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

  // Worker's stdin is the read end of pipe_to_worker
  posix_spawn_file_actions_adddup2(&actions, pipe_to_worker[0], STDIN_FILENO);
  // Worker's stdout is the write end of pipe_from_worker
  posix_spawn_file_actions_adddup2(&actions, pipe_from_worker[1],
                                   STDOUT_FILENO);

  // Close other ends in the worker
  posix_spawn_file_actions_addclose(&actions, pipe_to_worker[1]);
  posix_spawn_file_actions_addclose(&actions, pipe_from_worker[0]);

  // Worker takes: argv[0] = path, argv[1] = llvm_symbolizer_path
  char* const argv[] = {(char*)worker_path, (char*)llvm_symbolizer_path,
                        nullptr};

  if (posix_spawn(nullptr, worker_path, &actions, nullptr, argv, environ) ==
      0) {
    worker_stdin_fd = pipe_to_worker[1];
    worker_stdout_fd = pipe_from_worker[0];

    // Close unused ends in the parent
    close(pipe_to_worker[0]);
    close(pipe_from_worker[1]);

    // Set up signal handlers
    struct sigaction sa = {};
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    int signals[] = {SIGSEGV, SIGILL, SIGFPE, SIGABRT,
                     SIGTERM, SIGBUS, SIGTRAP};
    for (int sig : signals) {
      sigaction(sig, &sa, nullptr);
    }
  } else {
    // Failed to spawn
    close(pipe_to_worker[0]);
    close(pipe_to_worker[1]);
    close(pipe_from_worker[0]);
    close(pipe_from_worker[1]);
  }

  posix_spawn_file_actions_destroy(&actions);
}
