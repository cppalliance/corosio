# Benchmark: Callbacks vs Coroutines

Compares dispatch overhead for composed async operations.

## Design Principles

- **Both must be fully optimized** — recycle and preallocate whenever possible
- **Measure inherent abstraction cost** — not implementation inefficiencies
- Coroutine preallocation is legitimate, not cheating

## Key Constraint

- **Callbacks cannot preallocate** — handler type varies per call site (templated)
- **Coroutines can preallocate** — `std::coroutine_handle<void>` is type-erased (fixed size)
- Callbacks can only recycle via `op_cache`

## Why This Matters

- I/O always allocates operation state
- Continuations almost always perform another async operation
- Recycling is essential for both models

## Performance Considerations for I/O Frameworks

For I/O frameworks, the virtual call or function indirection cost is insignificant compared to the actual I/O operation, making the non-template class approach more viable.

**Key insight:** When choosing between template-based and type-erased designs, consider that:
- I/O operations (network reads/writes, file I/O) typically take microseconds to milliseconds
- A virtual function call or function pointer indirection takes nanoseconds
- The overhead ratio is 1000:1 or greater, making indirection costs negligible

This means type-erased designs (non-template classes with virtual interfaces) are often acceptable for I/O frameworks, providing cleaner APIs and container compatibility with minimal performance impact.

## Build Directory Policy

**All build artifacts MUST go to `build*/` directories.** Never create executables, object files, or other artifacts in the source directory. The `.gitignore` is configured to ignore `build*/` directories and common artifact patterns, but the primary defense is always using proper build directories.

## Building with MSVC (Visual Studio)

### Using CMake (Recommended)

```powershell
# Configure and build
cmake -B build-vs2026 -G "Visual Studio 18 2026" -A x64
cmake --build build-vs2026 --config Release

# Run the benchmark
.\build-vs2026\Release\bench.exe
```

### Direct Compilation (Alternative)

```powershell
# Ensure build directory exists
New-Item -ItemType Directory -Force -Path build-msvc

# Build with MSVC
cl /std:c++20 /EHsc /O2 /Ob2 /GL /W4 /permissive- bench.cpp /Fe:build-msvc\bench.exe /link /LTCG

# Run
.\build-msvc\bench.exe
```

## Building with Clang

### Direct Compilation

```powershell
# Ensure build directory exists
New-Item -ItemType Directory -Force -Path build-clang

# Build with Clang
clang++ -std=c++20 -Wall -Wextra -Wpedantic -O3 -march=native -DNDEBUG bench.cpp -o build-clang\bench.exe

# Run
.\build-clang\bench.exe
```

## Building with GCC (Windows/MSYS2)

### Quick Build Command

**Use `bash -c` wrapper** to avoid PowerShell parsing issues and ensure proper MSYS2 environment:

```bash
# Ensure build directory exists
mkdir -p build-gcc

# Build with full optimizations (including LTO)
bash -c "g++ -std=c++20 -fcoroutines -Wall -Wextra -Wpedantic -O3 -march=native -flto -funroll-loops -DNDEBUG -ffast-math -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -pthread bench.cpp -o build-gcc/bench.exe -flto -s -Wl,--as-needed"

# Run the benchmark
bash -c "./build-gcc/bench.exe"
```

### Key Learnings

1. **GCC is available via MSYS2** (`C:/msys64/ucrt64/bin/g++.exe`)
   - Version: GCC 15.2.0 (Rev8, Built by MSYS2 project)
   - Already in PATH when MSYS2 is installed

2. **PowerShell issues to avoid:**
   - ❌ Don't use `&&` (use `;` instead)
   - ❌ Don't use backslashes in paths with special characters
   - ❌ Complex command lines with many flags cause parsing errors
   - ✅ Use `bash -c "..."` wrapper for complex commands

3. **CMake configuration challenges:**
   - CMake auto-detects MSVC on Windows, not GCC
   - Specifying `-DCMAKE_CXX_COMPILER=g++` doesn't always work
   - Generators (MinGW Makefiles, Unix Makefiles) may not be available
   - **Solution:** Direct `g++` compilation bypasses CMake entirely

4. **Executable format:**
   - GCC builds create Unix-style executables (not native Windows PE)
   - Must run through `bash -c` or MSYS2 environment
   - Cannot run directly in PowerShell (`The specified executable is not a valid application`)

5. **Optimization flags** (from CMakeLists.txt):
   - Compile: `-std=c++20 -fcoroutines -Wall -Wextra -Wpedantic -O3 -march=native -flto -funroll-loops -DNDEBUG -ffast-math -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -pthread`
   - Link: `-flto -s -Wl,--as-needed`

6. **Build directory:**
   - **Always use `build-gcc/` directory** for output
   - Never create executables in the source directory
   - CMakeLists.txt validates builds are in `build*` directory

### Recommended Workflow

1. **Ensure build directory exists:**
   ```bash
   mkdir -p build-gcc
   ```

2. **Build directly with g++** (faster, fewer issues than CMake):
   ```bash
   bash -c "g++ -std=c++20 -fcoroutines -Wall -Wextra -Wpedantic -O3 -march=native -flto -funroll-loops -DNDEBUG -ffast-math -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -pthread bench.cpp -o build-gcc/bench.exe -flto -s -Wl,--as-needed"
   ```

3. **Run through bash:**
   ```bash
   bash -c "./build-gcc/bench.exe"
   ```

### Why Direct Compilation Works Better

- ✅ No CMake configuration overhead
- ✅ No generator compatibility issues
- ✅ Direct control over compiler flags
- ✅ Faster iteration (single command)
- ✅ Avoids PowerShell command parsing issues
- ✅ Works reliably in MSYS2/bash environment

## Running All Benchmarks

To run benchmarks with all available compilers:

```powershell
# MSVC
cmake --build build-vs2026 --config Release
.\build-vs2026\Release\bench.exe

# Clang
New-Item -ItemType Directory -Force -Path build-clang
clang++ -std=c++20 -Wall -Wextra -Wpedantic -O3 -march=native -DNDEBUG bench.cpp -o build-clang\bench.exe
.\build-clang\bench.exe

# GCC (via MSYS2)
bash -c "mkdir -p build-gcc"
bash -c "g++ -std=c++20 -fcoroutines -Wall -Wextra -Wpedantic -O3 -march=native -flto -funroll-loops -DNDEBUG -ffast-math -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -pthread bench.cpp -o build-gcc/bench.exe -flto -s -Wl,--as-needed"
bash -c "./build-gcc/bench.exe"
```

**Remember:** All executables must be in `build*/` directories, never in the source directory.
