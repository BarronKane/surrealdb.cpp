# SurrealDB C++ SDK

A C++23 wrapper for the SurrealDB C library, supporting both traditional headers and C++20 modules.

## Requirements

- CMake 3.31.11 or higher
- C++23 compatible compiler (MSVC 19.x, GCC 14+, Clang 18+)
- For C++20 modules: Visual Studio 18 2026 or Ninja generator

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTING` | ON | Build tests |
| `BUILD_EXAMPLES` | ON | Build examples |
| `BUILD_SHARED` | ON | Build shared library |
| `SURREALDB_USE_MODULES` | OFF | Enable C++20 modules support |
| `SURREALDB_RELEASE` | OFF | Build Rust library in release mode |
| `GIT_SUBMODULE` | ON | Automatically initialize and update git submodules |

## Building Examples

### Example without Modules (Traditional Headers)

This example works with any CMake generator:

```bash
# Configure
cmake -S . -B build -DSURREALDB_USE_MODULES=OFF

# Build
cmake --build build --target surrealdb_cpp_no_modules_example

# Run (Linux/macOS)
./build/bin/surrealdb_cpp_no_modules_example

# Run (Windows)
.\build\bin\surrealdb_cpp_no_modules_example.exe
```

### Example with Modules (C++20 Modules)

C++20 modules require a compatible generator. Use **Visual Studio 17 2022** or **Ninja**.

#### Using Visual Studio 17 2022 (Windows)

```bash
# Configure
cmake -S . -B build -G "Visual Studio 18 2026" -DSURREALDB_USE_MODULES=ON

# Build
cmake --build build --target surrealdb_cpp_modules_example --config Debug

# Run
.\build\bin\Debug\surrealdb_cpp_modules_example.exe
```

#### Using Ninja

```bash
# Configure
cmake -S . -B build -G Ninja -DSURREALDB_USE_MODULES=ON

# Build
cmake --build build --target surrealdb_cpp_modules_example

# Run (Linux/macOS)
./build/bin/surrealdb_cpp_modules_example

# Run (Windows)
.\build\bin\surrealdb_cpp_modules_example.exe
```

## Usage in Your Project

### Traditional Headers

```cpp
#include <surrealdb/surrealdb.hpp>

int main() {
    return hello_surreal();
}
```

### C++20 Modules

```cpp
import surrealdb;

int main() {
    return hello_surreal();
}
```

### CMake Integration

```cmake
find_package(surrealdb.cpp CONFIG REQUIRED)

# Optional: Enable modules
set(SURREALDB_USE_MODULES ON)

target_link_libraries(your_app PRIVATE surrealdb::surrealdb_cpp)
```

## Notes

- The `SURREALDB_USE_MODULES` preprocessor define controls whether to use `import surrealdb;` or `#include <surrealdb/surrealdb.hpp>`
- NMake Makefiles generator does **not** support C++20 modules
- Both examples produce the same output: `Surrealdb server version: X.X.X`
