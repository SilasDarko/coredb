set(COREDB_SOURCES
  util/cpu_features.cpp
  util/crc32c.cpp
  storage/types.cpp
  storage/segment.cpp
  mvcc/transaction.cpp
  mvcc/delta.cpp
  table/table.cpp
  compaction/compactor.cpp
  wal/record.cpp
  wal/wal_writer.cpp
  wal/wal_reader.cpp
  recovery/checkpoint.cpp
  recovery/recovery_manager.cpp
  db/database.cpp
  simd/kernels_scalar.cpp
  simd/kernels_neon.cpp
  simd/kernels_avx2.cpp
  simd/kernels_avx512.cpp
  simd/dispatch.cpp
  exec/volcano.cpp
  exec/vectorized.cpp
)

if(COREDB_ENABLE_JIT)
  list(APPEND COREDB_SOURCES jit/query_jit.cpp exec/jit_exec.cpp)
endif()

add_library(coredb STATIC ${COREDB_SOURCES})
target_include_directories(coredb PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_features(coredb PUBLIC cxx_std_20)

if(COREDB_ARCH_ARM64)
  target_compile_definitions(coredb PUBLIC COREDB_ARCH_ARM64=1)
elseif(COREDB_ARCH_X86_64)
  target_compile_definitions(coredb PUBLIC COREDB_ARCH_X86_64=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(coredb PUBLIC Threads::Threads)
