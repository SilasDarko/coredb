# CoreDB

An experimental columnar storage engine in C++20: MVCC with snapshot
isolation, an LSM-style delta/compaction storage layer, a write-ahead log
with crash recovery, and three interchangeable execution strategies
(row-at-a-time Volcano iterator, vectorized SIMD batches, LLVM JIT-compiled
native code) benchmarked head-to-head on identical work.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit together,
[DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for why they're built the way
they are (including the trade-offs and limitations found while building
and benchmarking it), and [BENCHMARKS.md](BENCHMARKS.md) for every
performance number this project claims, each reproducible with the command
printed next to it.

## What's actually implemented

- **Storage:** immutable columnar segments (`int32`/`int64`/`double`/
  dictionary-encoded `string`), validity bitmaps, per-column min/max,
  CRC32C checksums, binary serialization with corruption detection.
- **MVCC:** transaction ids, snapshot-isolation visibility, insert/update/
  delete, write-write conflict abort, an append-only delta layer, and
  active-snapshot-safe compaction that promotes/retires/rewrites.
- **WAL + recovery:** append-only checksummed log, truncated-tail and
  corrupt-record detection, checkpoint manifests, REDO replay with a
  configurable number of parallel worker threads, post-recovery checksum
  verification.
- **Execution:** the same `SUM(x) WHERE y > threshold` query run three
  ways — Volcano iterator, vectorized SIMD batches, LLVM ORC JIT (with a
  real `-O3` optimization pass and a branchless loop shape so LLVM's
  auto-vectorizer actually kicks in) — plus segment pruning via min/max.
- **SIMD:** scalar, ARM NEON, x86 AVX2, and x86 AVX-512 kernels behind
  runtime CPU-feature dispatch. AVX2/AVX-512 are real, compiled, tested
  code (`tests/unit/test_simd_equivalence.cpp`), gated at build time by
  architecture and at call time by runtime detection — they've never
  executed on the arm64 machine this was built on, and no performance
  number is claimed for them here.

## Build

Requires CMake ≥ 3.20, a C++20 compiler, LLVM 17 (dev package/config
files), GoogleTest, and Google Benchmark.

```bash
# macOS
brew install llvm@17 googletest google-benchmark

# Ubuntu (see .github/workflows/ci.yml for the exact CI steps)
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 17
sudo apt-get install -y libgtest-dev libbenchmark-dev
```

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOREDB_ENABLE_JIT=ON \
  -DCOREDB_BUILD_TESTS=ON \
  -DCOREDB_BUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc || sysctl -n hw.ncpu)"
```

On macOS, if LLVM isn't found, pass
`-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm@17`.

## Test

```bash
cd build && ctest --output-on-failure
# or directly:
./build/tests/coredb_tests
./build/tests/coredb_tests --gtest_filter='*Concurren*'   # just one suite
```

79 test cases across unit, integration, crash-recovery, and concurrency
suites (see the repository tree below for the breakdown). Additionally
verified under AddressSanitizer+UndefinedBehaviorSanitizer and
ThreadSanitizer — see BENCHMARKS.md, "Sanitizer verification", for exact
commands and results.

## Benchmark

```bash
./build/benchmarks/bench_scan_throughput
./build/benchmarks/bench_simd_compare
./build/benchmarks/bench_exec_compare
./build/benchmarks/bench_mvcc_throughput
./build/benchmarks/bench_compaction
./build/benchmarks/bench_recovery --size-mb=512   # default 32; pass a larger
                                                    # value for a bigger run
                                                    # (disk-space permitting)
```

Every benchmark prints the detected CPU/architecture/core-count/memory
banner as its first lines of output, so a run is self-describing even
pasted out of context. The three CLI benchmarks (`bench_mvcc_throughput`,
`bench_compaction`, `bench_recovery`) also append machine-readable JSON
lines to `results/*.jsonl` (git-ignored).

See [BENCHMARKS.md](BENCHMARKS.md) for the numbers this produced on the
reference machine it was developed on, and for exactly what it would take
to run this at the larger scale a bigger machine could support.

## Repository layout

```
CoreDB/
├── CMakeLists.txt
├── ARCHITECTURE.md
├── DESIGN_DECISIONS.md
├── BENCHMARKS.md
├── README.md
├── .github/workflows/ci.yml
├── include/coredb/          # public headers, mirrors src/
│   ├── util/                # crc32c, bitmap, cpu_features
│   ├── storage/              # types, segment, dictionary
│   ├── mvcc/                 # transaction, delta
│   ├── table/                 # table (ties storage + mvcc together)
│   ├── compaction/            # background compactor
│   ├── wal/                   # record, wal_writer, wal_reader
│   ├── recovery/              # checkpoint, recovery_manager
│   ├── db/                    # database (table + wal wired together)
│   ├── exec/                  # query.h (Volcano/vectorized/JIT entry points)
│   ├── simd/                  # kernels.h
│   └── jit/                   # query_jit.h
├── src/                      # implementation, same module layout as include/
├── tests/
│   ├── unit/                  # bitmap, crc32c, segment, mvcc visibility,
│   │                           # table CRUD, compaction, SIMD/JIT equivalence,
│   │                           # exec-strategy equivalence
│   ├── integration/            # full lifecycle: insert/update/delete/
│   │                           # compact/checkpoint/recover
│   ├── crash_recovery/          # WAL truncation, WAL corruption, recovery
│   │                           # idempotence
│   └── concurrency/            # concurrent transactions, aborts/conflicts
└── benchmarks/
    ├── bench_scan_throughput.cpp    (Google Benchmark)
    ├── bench_simd_compare.cpp       (Google Benchmark)
    ├── bench_exec_compare.cpp       (Google Benchmark)
    ├── bench_mvcc_throughput.cpp    (custom CLI)
    ├── bench_compaction.cpp         (custom CLI)
    └── bench_recovery.cpp           (custom CLI, --size-mb=N)
```

## What this project does not do

No SQL, no query planner, no general expression evaluator, no network
protocol or client/server split, no multi-table joins, no secondary
indexes. The execution engine runs exactly one query shape
(`SUM(x) WHERE y > threshold`) by design — see ARCHITECTURE.md for why that
scope was chosen (comparing execution *strategies* on identical work, not
building a planner) — and several real limitations (no row_id index, a
single global transaction-manager mutex, full-rewrite-only compaction) are
documented, not hidden, in DESIGN_DECISIONS.md, along with what measurably
happened because of them.
