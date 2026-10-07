# STDCORELIB

Portable C++ Application Infrastructure.

## Introduction

Most components exist because their implementation differs between Windows and other platforms: console color, UTF-8 output on a Windows console, starting a child process, and loading a shared object. The remaining components are containers and utilities that projects otherwise reimplement repeatedly.

Components are header-only if possible and compiled otherwise. The library depends only on the standard library. The test suite additionally requires Boost.Test.

## Requirements

- A C++17 compiler.
- CMake 3.16 or later.
- Windows, Linux or macOS. MSVC, clang-cl, GCC and Clang are all built and tested.

## Build & Install

```bash
git clone https://github.com/stdware/stdcorelib.git
cd stdcorelib
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/path/to/install
cmake --build build --config Release
cmake --install build --config Release
```

## Integration

Install the library, make its installation prefix discoverable by CMake, and consume its exported target:

```cmake
find_package(stdcorelib CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE stdcorelib::stdcorelib)
```

Or as a subdirectory:

```cmake
add_subdirectory(stdcorelib)
target_link_libraries(myapp PRIVATE stdcorelib::stdcorelib)
```

## Components

| Component | |
| --- | --- |
| Command line | Declaration of the command line syntax, and access to the parsed values |
| Processes and libraries | Child processes and shared libraries |
| Text | Strings, formatting, console output, UTF conversion |
| Containers and views | `array_view`, `vlarray`, `linked_map`, `any` |
| Type identity | Type names without RTTI, and registries based on them |
| Logging | Named categories with per-level switches and filter rules |
| JSON and CBOR | One document tree with both encodings |
| Platform and system | Program and machine information, the Windows registry |
| Utilities | Flags, scope guards, version numbers |

The documentation of each component, including an example, is in its header. The `stdcorelib_docs` target, enabled with `-DSTDC_BUILD_DOCS=ON`, generates the same documentation as HTML.

## Credits

Code derived from the following projects is cited at the place of use:

- [CPython](https://github.com/python/cpython)
- [qtbase](https://github.com/qt/qtbase)
- [xmake](https://github.com/xmake-io/xmake)
- [LLVM](https://github.com/llvm/llvm-project)

The test suites use:

- [Boost.Test](https://www.boost.org/users/history/test.html)

The documentation is built and styled with:

- [Doxygen](https://www.doxygen.nl/)
- [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css)

## License

MIT. See [LICENSE](LICENSE).
