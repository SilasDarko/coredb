cmake_minimum_required(VERSION 3.20)
project(CoreDB LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

option(COREDB_BUILD_TESTS "Build CoreDB unit/integration/crash/concurrency tests" ON)
option(COREDB_BUILD_BENCHMARKS "Build CoreDB benchmark binaries" ON)
option(COREDB_ENABLE_JIT "Build the LLVM JIT execution path" ON)

# Homebrew keeps versioned/keg-only formulae out of the default search path.
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH
    /opt/homebrew
    /opt/homebrew/opt/llvm@17
    /opt/homebrew/opt/llvm
    /opt/homebrew/opt/googletest
    /opt/homebrew/opt/google-benchmark
  )
endif()

# ---------------------------------------------------------------------------
# Architecture detection: drives which SIMD kernel sources get compiled.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(COREDB_ARCH_ARM64 ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(COREDB_ARCH_X86_64 ON)
endif()

add_subdirectory(src)

if(COREDB_ENABLE_JIT)
  find_package(LLVM 17 REQUIRED CONFIG)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
  target_include_directories(coredb PUBLIC ${LLVM_INCLUDE_DIRS})
  separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
  target_compile_definitions(coredb PUBLIC ${LLVM_DEFINITIONS_LIST} COREDB_ENABLE_JIT=1)
  llvm_map_components_to_libnames(LLVM_LIBS core orcjit orctargetprocess native irreader)
  target_link_libraries(coredb PUBLIC ${LLVM_LIBS})
endif()

if(COREDB_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_subdirectory(tests)
endif()

if(COREDB_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)
  add_subdirectory(benchmarks)
endif()
