#include "crash_handler.h"

#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <unwind.h>
#include <utility>
#include <vector>

#include "trace_packet.h"
#include "util.h"

extern char** environ;

namespace {

int worker_stdin_fd = -1;
int worker_stdout_fd = -1;

struct SignalHandlerPair {
  int signo;
  struct sigaction old_handler;
};

SignalHandlerPair old_handlers[] = {{SIGSEGV}, {SIGILL}, {SIGFPE}, {SIGABRT},
                                    {SIGTERM}, {SIGBUS}, {SIGTRAP}};

struct UnwindState {
  void** stack;
  int size;
  int depth;
  int skip;
};

_Unwind_Reason_Code UnwindCallback(struct _Unwind_Context* context, void* arg) {
  UnwindState* state = static_cast<UnwindState*>(arg);
  if (state->skip > 0) {
    state->skip--;
    return _URC_NO_REASON;
  }
  if (state->depth >= state->size)
    return _URC_END_OF_STACK;
  uintptr_t ip = _Unwind_GetIP(context);
  if (state->depth)
    ip -= 1;
  state->stack[state->depth++] = reinterpret_cast<void*>(ip);
  return _URC_NO_REASON;
}

__attribute__((noinline)) int GetStackTraceUnwind(void** stack, int size) {
  UnwindState state = {stack, size, 0, 3};
  _Unwind_Backtrace(UnwindCallback, &state);
  return state.depth;
}

__attribute__((noinline)) void CrashSignalHandler(int signo,
                                                  siginfo_t* info,
                                                  void* context) {
  TracePacket data;
  data.process_id = getpid();
  data.signal_number = signo;

  data.stack_depth = GetStackTraceUnwind(data.stack, array_size(data.stack));

  WriteFully(worker_stdin_fd, &data, sizeof(data));

  char ack = 0;
  read(worker_stdout_fd, &ack, 1);

  struct sigaction* old_sa = nullptr;
  for (auto& pair : old_handlers) {
    if (pair.signo == signo) {
      old_sa = &pair.old_handler;
      break;
    }
  }

  if (old_sa) {
    sigaction(signo, old_sa, nullptr);
  } else {
    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, nullptr);
  }

  raise(signo);
}

bool SpawnWorker(const char* worker_path,
                 const char* llvm_symbolizer_path,
                 const char* strip_path_prefix) {
  int pipe_to_worker[2] = {-1, -1};
  int pipe_from_worker[2] = {-1, -1};
  std::vector<char*> argv;
  bool success = false;

  if (pipe2(pipe_to_worker, O_CLOEXEC) < 0 ||
      pipe2(pipe_from_worker, O_CLOEXEC) < 0) {
    goto cleanup;
  }

  {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    posix_spawn_file_actions_adddup2(&actions, pipe_to_worker[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_from_worker[1],
                                     STDOUT_FILENO);

    posix_spawn_file_actions_addclose(&actions, pipe_to_worker[1]);
    posix_spawn_file_actions_addclose(&actions, pipe_from_worker[0]);

    std::vector<std::string> args = {worker_path, llvm_symbolizer_path};
    if (strip_path_prefix && strip_path_prefix[0] != '\0') {
      args.push_back(strip_path_prefix);
    }
    argv = MakeArgV(&args);

    if (posix_spawn(nullptr, worker_path, &actions, nullptr, argv.data(),
                    environ) < 0) {
      posix_spawn_file_actions_destroy(&actions);
      goto cleanup;
    }

    posix_spawn_file_actions_destroy(&actions);
  }

  worker_stdin_fd = std::exchange(pipe_to_worker[1], -1);
  worker_stdout_fd = std::exchange(pipe_from_worker[0], -1);

  success = true;

cleanup:
  if (pipe_to_worker[0] >= 0)
    close(pipe_to_worker[0]);
  if (pipe_to_worker[1] >= 0)
    close(pipe_to_worker[1]);
  if (pipe_from_worker[0] >= 0)
    close(pipe_from_worker[0]);
  if (pipe_from_worker[1] >= 0)
    close(pipe_from_worker[1]);

  return success;
}

void InstallSignalHandlers() {
  struct sigaction sa = {};
  sa.sa_sigaction = CrashSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);

  for (auto& pair : old_handlers)
    sigaction(pair.signo, &sa, &pair.old_handler);
}

}  // namespace

void SetUpCrashHandler(const char* worker_path,
                       const char* llvm_symbolizer_path,
                       const char* strip_path_prefix) {
  if (SpawnWorker(worker_path, llvm_symbolizer_path, strip_path_prefix))
    InstallSignalHandlers();
}
