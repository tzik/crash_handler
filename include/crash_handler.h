#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void SetUpCrashHandler(const char* worker_path,
                       const char* llvm_symbolizer_path);

#ifdef __cplusplus
}
#endif
