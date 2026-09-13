#include "crash_handler.h"
#include <signal.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <string.h>
#include <stdio.h>
#include "absl/debugging/stacktrace.h"

// Define a simple structure to pass to the worker
struct CrashData {
    int signal_number;
    int stack_depth;
    void* stack[128];
};

static int worker_stdin_fd = -1;
static int worker_stdout_fd = -1;

extern char **environ;

static void CrashSignalHandler(int signo, siginfo_t* info, void* context) {
    CrashData data;
    data.signal_number = signo;

    // Skip this frame and the signal handler frame
    int sizes[128];
    data.stack_depth = absl::GetStackTraceWithContext(data.stack, 128, 1, nullptr, sizes);

    // Write data to worker via its stdin
    // Assuming writes < PIPE_BUF are atomic and complete
    if (worker_stdin_fd != -1) {
        ssize_t written = 0;
        const char* p = (const char*)&data;
        size_t to_write = sizeof(data);
        while (to_write > 0) {
            ssize_t res = write(worker_stdin_fd, p, to_write);
            if (res > 0) {
                p += res;
                to_write -= res;
            } else if (res < 0) {
                // error writing, break
                break;
            }
        }

        // Wait for confirmation byte
        if (worker_stdout_fd != -1) {
            char ack = 0;
            read(worker_stdout_fd, &ack, 1);
        }
    }

    // Reset signal handler to default
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, nullptr);

    // Re-raise signal
    raise(signo);
}

void SetUpCrashHandler(const char* worker_path, const char* llvm_symbolizer_path) {
    int pipe_to_worker[2];
    int pipe_from_worker[2];

    if (pipe(pipe_to_worker) != 0 || pipe(pipe_from_worker) != 0) {
        return; // failed to create pipes
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Worker's stdin is the read end of pipe_to_worker
    posix_spawn_file_actions_adddup2(&actions, pipe_to_worker[0], STDIN_FILENO);
    // Worker's stdout is the write end of pipe_from_worker
    posix_spawn_file_actions_adddup2(&actions, pipe_from_worker[1], STDOUT_FILENO);

    // Close other ends in the worker
    posix_spawn_file_actions_addclose(&actions, pipe_to_worker[1]);
    posix_spawn_file_actions_addclose(&actions, pipe_from_worker[0]);

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", getpid());

    // Worker takes: argv[0] = path, argv[1] = pid, argv[2] = llvm_symbolizer_path
    char* const argv[] = {
        (char*)worker_path,
        pid_str,
        (char*)llvm_symbolizer_path,
        nullptr
    };

    pid_t worker_pid;
    if (posix_spawn(&worker_pid, worker_path, &actions, nullptr, argv, environ) == 0) {
        worker_stdin_fd = pipe_to_worker[1];
        worker_stdout_fd = pipe_from_worker[0];

        // Close unused ends in the parent
        close(pipe_to_worker[0]);
        close(pipe_from_worker[1]);

        // Set up signal handlers
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = CrashSignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigemptyset(&sa.sa_mask);

        int signals[] = {SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGBUS, SIGTRAP};
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
