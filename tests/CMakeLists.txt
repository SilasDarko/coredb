set(COREDB_TEST_SOURCES
  unit/test_bitmap.cpp
  unit/test_crc32c.cpp
  unit/test_segment.cpp
  unit/test_mvcc_visibility.cpp
  unit/test_table_crud.cpp
  unit/test_compaction.cpp
  integration/test_end_to_end.cpp
  crash_recovery/test_wal_truncation.cpp
  crash_recovery/test_wal_corruption.cpp
  crash_recovery/test_recovery_idempotence.cpp
  concurrency/test_concurrent_txns.cpp
  concurrency/test_aborts.cpp
  unit/test_simd_equivalence.cpp
  unit/test_exec_equivalence.cpp
)

if(COREDB_ENABLE_JIT)
  list(APPEND COREDB_TEST_SOURCES unit/test_jit_equivalence.cpp)
endif()

add_executable(coredb_tests ${COREDB_TEST_SOURCES})
target_link_libraries(coredb_tests PRIVATE coredb GTest::gtest GTest::gtest_main)
target_include_directories(coredb_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(coredb_tests)
