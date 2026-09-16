function(coredb_add_gbenchmark name)
  add_executable(${name} ${name}.cpp)
  # Each benchmark provides its own main() (to print detected machine
  # capabilities before running), so link benchmark's library only, not
  # benchmark_main.
  target_link_libraries(${name} PRIVATE coredb benchmark::benchmark)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endfunction()

function(coredb_add_cli_benchmark name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE coredb)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endfunction()

coredb_add_gbenchmark(bench_scan_throughput)
coredb_add_gbenchmark(bench_simd_compare)
coredb_add_gbenchmark(bench_exec_compare)

coredb_add_cli_benchmark(bench_mvcc_throughput)
coredb_add_cli_benchmark(bench_compaction)
coredb_add_cli_benchmark(bench_recovery)

# bench_exec_compare exercises LLVM ORC JIT and must expose the host
# process symbols required by LLJIT on ELF platforms.
if(COREDB_ENABLE_JIT AND UNIX AND NOT APPLE)
  set_target_properties(bench_exec_compare PROPERTIES ENABLE_EXPORTS ON)
endif()
