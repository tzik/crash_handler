# CrashHandler

CrashHandler is a out-of-process crash handling library for C/C++ applications. It captures stack traces when a crash occurs and resolves symbols using a separate worker process.

## License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for more details.

## External Dependencies

This project relies on the following external libraries and tools:
- **LLVM**: The LLVM C++ libraries are used in-process by the worker to resolve addresses to source code locations.

### Installing Dependencies on Debian/Ubuntu
You can install the required dependencies on a Debian or Ubuntu system using the following command:
```bash
sudo apt-get update
sudo apt-get install -y llvm-dev libzstd-dev zlib1g-dev cmake
```

## Usage Example

To use CrashHandler in your application, include the header and call `SetUpCrashHandler()` early in your program's execution, such as at the beginning of `main()`.

```cpp
#include "crash_handler.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <worker_path> [path_prefix]\n";
        return 1;
    }

    const char* worker_path = argv[1];           // Path to the crash_handler_worker executable

    // Optional: strip a specific prefix from source file paths in the stack trace
    const char* path_prefix = (argc >= 3) ? argv[2] : nullptr;

    // Initialize the crash handler
    SetUpCrashHandler(worker_path, path_prefix);

    // ... your application logic ...

    return 0;
}
```

### Sample Output

When a crash occurs, CrashHandler will output a detailed stack trace similar to the following:

```
*** Process 124304 crashed with signal SIGILL ***
#0 0x64b7f750213a in inline_function() (build/test_crash + 0x213a) at src/test_crash.cc:10
#0 0x64b7f750213a in crash_function() (build/test_crash + 0x213a) at src/test_crash.cc:14
#1 0x64b7f7502145 in intermediate_function_2() (build/test_crash + 0x2145) at src/test_crash.cc:18
#2 0x64b7f750214d in intermediate_function() (build/test_crash + 0x214d) at src/test_crash.cc:22
#3 0x64b7f75022c7 in main (build/test_crash + 0x22c7) at src/test_crash.cc:0
#4 0x7600fba2a1c9 in ?? (/usr/lib/x86_64-linux-gnu/libc.so.6 + 0x2a1c9) at ??:0
#5 0x7600fba2a28a in __libc_start_main (/usr/lib/x86_64-linux-gnu/libc.so.6 + 0x2a28a) at ??:0
#6 0x64b7f75020a4 in _start (build/test_crash + 0x20a4) at ??:0
```

## CMake Integration

You can integrate CrashHandler into your existing CMake project. Use `find_package` to locate it and link against the `CrashHandler::crash_handler` target.

```cmake
find_package(CrashHandler REQUIRED)

# ... define your target ...

target_link_libraries(your_target PRIVATE CrashHandler::crash_handler)
```

## Build Instructions

You can build the project using standard CMake commands.

1. **Create a build directory and configure the project:**
   ```bash
   mkdir build
   cd build
   cmake ..
   ```

2. **Compile the project:**
   ```bash
   make
   ```
   This will build the `crash_handler` library, the `crash_handler_worker` executable, and the `test_crash` test executable.

   *Note: In order for the crash handler to report file names and line numbers in the stack trace, the application must be built with debug information. Make sure to configure CMake with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (or `Debug`) or compile with the `-g` flag.*

3. **Run the tests:**
   You can run the test executable to verify that the crash handler works correctly. It simulates a crash and outputs the stack trace.
   ```bash
   # Make sure you are in the build directory
   ./test_crash ./crash_handler_worker
   ```
   *Note: If you want to test the `path_prefix` feature, you can append a prefix string as a third argument to `test_crash`.*
