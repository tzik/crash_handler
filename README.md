# CrashHandler

CrashHandler is a out-of-process crash handling library for C/C++ applications. It captures stack traces when a crash occurs and resolves symbols using a separate worker process.

## License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for more details.

## External Dependencies

This project relies on the following external libraries and tools:
- **libelf**: Required for reading ELF files to help with symbolization.
- **libdw**: Used by the worker to resolve addresses to source code locations directly from DWARF debug information.

### Installing Dependencies on Debian/Ubuntu
You can install the required dependencies on a Debian or Ubuntu system using the following command:
```bash
sudo apt-get update
sudo apt-get install -y libelf-dev libdw-dev pkg-config cmake
```

## Usage Example

To use CrashHandler in your application, include the header and call `SetUpCrashHandler()` early in your program's execution, such as at the beginning of `main()`.

```cpp
#include "crash_handler.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <worker_path> [strip_path_prefix]\n";
        return 1;
    }

    const char* worker_path = argv[1];           // Path to the crash_handler_worker executable

    // Optional: strip a specific prefix from source file paths in the stack trace
    const char* strip_path_prefix = (argc >= 3) ? argv[2] : nullptr;

    // Initialize the crash handler
    SetUpCrashHandler(worker_path, strip_path_prefix);

    // ... your application logic ...

    return 0;
}
```

### Sample Output

When a crash occurs, CrashHandler will output a detailed stack trace similar to the following:

```
*** Process 315720 crashed with signal 4 ***
#0 0x56233aaa05b1 in inline_function() (/app/build/test_crash + 0x25b1) at /app/src/test_crash.cc:10
#0 0x56233aaa05b1 in crash_function() (/app/build/test_crash + 0x25b1) at /app/src/test_crash.cc:14
#1 0x56233aaa05bf in intermediate_function_2() (/app/build/test_crash + 0x25bf) at /app/src/test_crash.cc:18
#2 0x56233aaa05cf in intermediate_function() (/app/build/test_crash + 0x25cf) at /app/src/test_crash.cc:22
#3 0x56233aaa06b7 in main() (/app/build/test_crash + 0x26b7) at /app/src/test_crash.cc:46
#4 0x7fd9636ff1c9 in __libc_start_call_main() (/usr/lib/x86_64-linux-gnu/libc.so.6 + 0x2a1c9) at ../sysdeps/nptl/libc_start_call_main.h:58
#5 0x7fd9636ff28a in __libc_start_main_impl() (/usr/lib/x86_64-linux-gnu/libc.so.6 + 0x2a28a) at ../csu/libc-start.c:360
#6 0x56233aaa04e4 in _start() (/app/build/test_crash + 0x24e4) at ??:0
#7 0xffffffffffffffff (unknown)
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
   *Note: If you want to test the `strip_path_prefix` feature, you can append a prefix string as a second argument to `test_crash`.*
