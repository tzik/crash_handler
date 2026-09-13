#include "crash_handler.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "absl/debugging/stacktrace.h"
#include "crash_data.h"

extern char** environ;

namespace {

int worker_stdin_fd = -1;
int worker_stdout_fd = -1;

struct sigaction old_handlers[NSIG];

void WriteFully(int fd, const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  size_t to_write = size;
  while (to_write > 0) {
    ssize_t res = write(fd, p, to_write);
    if (res < 0)
      break;
    if (res == 0)
      break;
    p += res;
    to_write -= res;
  }
}

std::vector<char*> MakeArgV(std::vector<std::string>* args) {
  std::vector<char*> argv;
  argv.reserve(args->size() + 1);
  for (auto& arg : *args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  return argv;
}

void CrashSignalHandler(int signo, siginfo_t* info, void* context) {
  CrashData data;
  data.process_id = getpid();
  data.signal_number = signo;

  data.stack_depth =
      absl::GetStackTraceWithContext(data.stack, 128, 1, context, nullptr);

  WriteFully(worker_stdin_fd, &data, sizeof(data));

  char ack = 0;
  read(worker_stdout_fd, &ack, 1);

  if (old_handlers[signo].sa_flags & SA_SIGINFO) {
    if (old_handlers[signo].sa_sigaction) {
      old_handlers[signo].sa_sigaction(signo, info, context);
      return;
    }
  } else {
    if (old_handlers[signo].sa_handler == SIG_IGN) {
      return;
    }
    if (old_handlers[signo].sa_handler &&
        old_handlers[signo].sa_handler != SIG_DFL) {
      old_handlers[signo].sa_handler(signo);
      return;
    }
  }

  struct sigaction sa = {};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(signo, &sa, nullptr);
  raise(signo);
}

}  // namespace

void SetUpCrashHandler(const char* worker_path,
                       const char* llvm_symbolizer_path) {
  int pipe_to_worker[2];
  if (pipe2(pipe_to_worker, O_CLOEXEC) < 0) {
    return;
  }

  int pipe_from_worker[2];
  if (pipe2(pipe_from_worker, O_CLOEXEC) < 0) {
    close(pipe_to_worker[0]);
    close(pipe_to_worker[1]);
    return;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

  posix_spawn_file_actions_adddup2(&actions, pipe_to_worker[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipe_from_worker[1],
                                   STDOUT_FILENO);

  posix_spawn_file_actions_addclose(&actions, pipe_to_worker[1]);
  posix_spawn_file_actions_addclose(&actions, pipe_from_worker[0]);

  std::vector<std::string> args = {worker_path, llvm_symbolizer_path};
  std::vector<char*> argv = MakeArgV(&args);

  if (posix_spawn(nullptr, worker_path, &actions, nullptr, argv.data(),
                  environ) < 0) {
    close(pipe_to_worker[0]);
    close(pipe_to_worker[1]);
    close(pipe_from_worker[0]);
    close(pipe_from_worker[1]);
    posix_spawn_file_actions_destroy(&actions);
    return;
  }

  worker_stdin_fd = pipe_to_worker[1];
  worker_stdout_fd = pipe_from_worker[0];

  close(pipe_to_worker[0]);
  close(pipe_from_worker[1]);

  struct sigaction sa = {};
  sa.sa_sigaction = CrashSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);

  int signals[] = {SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGBUS, SIGTRAP};
  for (int sig : signals) {
    sigaction(sig, &sa, &old_handlers[sig]);
  }

  posix_spawn_file_actions_destroy(&actions);
}
