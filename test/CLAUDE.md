# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run Tests

```bash
# Build all test executables (from project root, after cmake .)
make tests

# Run all tests
make test

# Run with verbose output
ctest --output-on-failure -V

# Run via Docker
docker-build-v2/build.sh --compile linux -t tests --verbose

# Run a single test binary directly
./build/test_Float3
```

The `check` target runs ctest and depends on `engine-headless`:
```bash
make check
```

## Framework

**Catch2** (amalgamated single-header version) in `lib/catch2/`. Custom main in `lib/catch2/catch_main.cpp` with leak detection enabled via `CATCH_AMALGAMATED_CUSTOM_MAIN`.

## Test Organization

```
engine/System/       # Core system tests (math, threading, I/O, serialization)
engine/Sim/Misc/     # Simulation tests (QuadField, Ellipsoid)
lib/luasocket/       # Lua socket restriction tests
other/               # Mutex benchmarks, memory pool tests
unitsync/            # UnitSync API tests
validation/          # Integration tests (shell scripts that run full game simulation)
tools/CompileFailTest/  # Negative test framework (tests that must NOT compile)
headercheck/         # Header isolation tests (cmake -DHEADERCHECK=ON)
```

## Adding a New Test

1. Create test source in the appropriate subdirectory under `engine/`, `other/`, etc.
2. In `test/CMakeLists.txt`, add a block using the `add_spring_test` macro:
```cmake
set(test_name MyTest)
set(test_src
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/System/testMyTest.cpp"
    ${test_Common_sources}
)
set(test_libs "")
set(test_flags "-DNOT_USING_CREG -DNOT_USING_STREFLOP -DBUILDING_AI")
add_spring_test(${test_name} "${test_src}" "${test_libs}" "${test_flags}")
```
3. The macro creates executable `test_<name>` and registers it with ctest as `test<name>`.

## Common Compile Flags

| Flag | Purpose |
|------|---------|
| `-DUNIT_TEST` | Always set for all tests (global) |
| `-DSYNCCHECK` | Always set for all tests (global) |
| `-DNOT_USING_CREG` | Disables CREG serialization system |
| `-DNOT_USING_STREFLOP` | Disables streflop math library |
| `-DBUILDING_AI` | AI-specific code paths |
| `-DTHREADPOOL` | Enables ThreadPool (globally removed, re-enabled per-test) |
| `-DUNITSYNC` | UnitSync library paths |

## Patterns

### Basic test file
```cpp
#include <catch_amalgamated.hpp>
#include "System/Log/ILog.h"

TEST_CASE("MyFeature") {
    CHECK(1 + 1 == 2);
    SECTION("sub-case") {
        CHECK(true);
    }
}
```

### Tests that need timing
```cpp
#include "System/Misc/SpringTime.h"
TEST_CASE("TimingTest") {
    InitSpringTime ist;  // RAII - must be instantiated before using spring_time
    // ...
}
```

### Thread-safe assertions
Catch2 is NOT thread-safe. Multi-threaded tests must guard assertions:
```cpp
static spring::mutex m;
#define SAFE_CHECK(expr) { std::lock_guard lk(m); CHECK(expr); }
```

### Compile-fail tests
Tests that verify code correctly fails to compile. Source uses `#ifdef FAIL` guards:
```cpp
#ifdef FAIL
#ifdef TEST1
    int x = someStronglyTypedEnum;  // must not compile
#endif
#endif
```
Registered in CMakeLists.txt via:
```cmake
spring_test_compile_fail(testName_fail1 ${test_src} "-DTEST1")
```

## Test Helpers (mock/stub files)

- `engine/System/NullGlobalConfig.cpp` — provides default `globalConfig` without full engine init
- `engine/System/Nullerrorhandler.cpp` — stubs `ErrorMessageBox()` to prevent GUI popups