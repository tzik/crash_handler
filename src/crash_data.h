#pragma once

#include <sys/types.h>

struct CrashData {
  pid_t process_id;
  int signal_number;
  int stack_depth;
  void* stack[128];
};
