# CrashHandler

CrashHandler is a out-of-process crash handling library for C/C++ applications. It captures stack traces when a crash occurs and resolves symbols using a separate worker process.

## License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for more details.

## External Dependencies

This project relies on the following external libraries and tools:
- **abseil** (`absl::stacktrace`): Used for capturing raw stack traces.
- **libelf**: Required for reading ELF files to help with symbolization.
- **nlohmann_json**: Used for structured communication between the main process and the crash handler worker.
- **llvm-symbolizer**: External tool invoked by the worker to resolve addresses to source code locations.

### Installing Dependencies on Debian/Ubuntu
You can install the required dependencies on a Debian or Ubuntu system using the following command:
```bash
sudo apt-get update
sudo apt-get install -y libelf-dev nlohmann-json3-dev libabsl-dev pkg-config llvm cmake
```

## Usage Example

To use CrashHandler in your application, include the header and call `SetUpCrashHandler()` early in your program's execution, such as at the beginning of `main()`.

```cpp
#include "crash_handler.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <worker_path> <llvm_symbolizer_path> [strip_path_prefix]\n";
        return 1;
    }

    const char* worker_path = argv[1];           // Path to the crash_handler_worker executable
    const char* llvm_symbolizer_path = argv[2];  // Path to the llvm-symbolizer executable

    // Optional: strip a specific prefix from source file paths in the stack trace
    const char* strip_path_prefix = (argc >= 4) ? argv[3] : nullptr;

    // Initialize the crash handler
    SetUpCrashHandler(worker_path, llvm_symbolizer_path, strip_path_prefix);

    // ... your application logic ...

    return 0;
}
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

3. **Run the tests:**
   You can run the test executable to verify that the crash handler works correctly. It simulates a crash and outputs the stack trace.
   ```bash
   # Make sure you are in the build directory
   ./test_crash ./crash_handler_worker "$(which llvm-symbolizer)"
   ```
   *Note: If you want to test the `strip_path_prefix` feature, you can append a prefix string as a third argument to `test_crash`.*
