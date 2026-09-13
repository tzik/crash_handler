#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

void SetUpCrashHandler(const char* worker_path, const char* llvm_symbolizer_path);

#ifdef __cplusplus
}
#endif

#endif // CRASH_HANDLER_H
