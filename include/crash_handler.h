#pragma once

#if defined(CRASH_HANDLER_IMPLEMENTATION)
#define CRASH_HANDLER_EXPORT __attribute__((visibility("default")))
#else
#define CRASH_HANDLER_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

CRASH_HANDLER_EXPORT void SetUpCrashHandler(const char* worker_path,
                                            const char* llvm_symbolizer_path);

#ifdef __cplusplus
}
#endif
