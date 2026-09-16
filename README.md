# CoreDB

CoreDB is an experimental columnar storage engine written in C++20.

It combines:

- MVCC with snapshot isolation
- immutable columnar segments
- an append-only delta layer
- background compaction
- write-ahead logging
- checkpoint and crash recovery
- runtime SIMD dispatch
- Volcano-style execution
- vectorized execution
- LLVM ORC JIT compilation

The execution engine compares multiple strategies on the same query shape so their behavior can be measured directly.

For more detail:

- [ARCHITECTURE.md](ARCHITECTURE.md) — system structure and data flow
- [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) — tradeoffs and implementation choices
- [BENCHMARKS.md](BENCHMARKS.md) — benchmark methodology and measured results

## Features

### Columnar storage

CoreDB stores data in immutable columnar segments.

Supported column types include:

- `int32`
- `int64`
- `double`
- dictionary-encoded strings

Each column stores:

- contiguous typed data
- a validity bitmap
- minimum and maximum metadata
- CRC32C integrity data

Segments can be serialized to disk and loaded again with integrity validation.

### MVCC

CoreDB implements snapshot isolation through:

- transaction IDs
- commit timestamps
- per-transaction snapshots
- append-only row versions
- write-write conflict detection
- transaction commit and abort state

The mutable delta layer stores inserts, updates, deletes, and tombstones before they are promoted into immutable base storage.

Concurrent writes to the same logical row conflict rather than waiting on a lock queue.

### Compaction

`Table::Compact()` merges stable delta versions into the immutable base representation.

The current implementation uses full-rewrite compaction.

A background compactor can run alongside update-heavy workloads to prevent the delta layer from growing without bound.

### Write-ahead log

CoreDB includes an append-only WAL with:

- monotonically increasing LSNs
- transaction IDs
- record types
- length-prefixed records
- CRC32C validation
- truncated-tail detection
- corruption detection

Recovery stops at the first invalid WAL boundary rather than attempting to reinterpret later bytes.

### Checkpoint and recovery

Checkpointing persists the current base segments together with the corresponding WAL position.

On restart, recovery:

1. loads checkpoint segments
2. reads WAL records after the checkpoint
3. identifies committed transactions
4. replays committed operations
5. verifies recovered segment integrity

Recovery supports configurable worker counts and partitions replay work by logical row ID.

The current checkpoint implementation assumes a quiescent checkpoint boundary.

### Execution strategies

CoreDB runs the same query through three execution models:

```sql
SELECT SUM(aggregate_column)
WHERE predicate_column > threshold;
```

The implementations are:

- Volcano-style row-at-a-time iteration
- vectorized batch execution
- LLVM JIT-compiled native execution

All three operate on the same immutable segment data and are tested for result equivalence.

### SIMD

CoreDB includes separate SIMD kernels for:

- scalar execution
- ARM NEON
- AVX2
- AVX-512

The runtime dispatch layer detects available CPU capabilities and selects the best supported implementation.

The Apple Silicon reference machine executes the NEON path.

AVX2 and AVX-512 implementations are compiled only on compatible x86-64 builds, and equivalence tests execute the instruction-set tiers supported by the host CPU.

No AVX2 or AVX-512 performance numbers in this repository were measured on the ARM64 development machine.

### LLVM JIT

The JIT engine:

1. constructs LLVM IR for a fused filter-and-sum loop
2. runs LLVM's optimization pipeline
3. compiles the optimized IR using ORC `LLJIT`
4. executes the generated native function against segment columns

The generated loop uses branchless selection so LLVM's vectorizer can transform the reduction efficiently.

See [BENCHMARKS.md](BENCHMARKS.md) for the measured effect of the optimization passes and branchless IR.

## Requirements

CoreDB requires:

- CMake 3.20+
- a C++20 compiler
- LLVM 17
- GoogleTest
- Google Benchmark

## Build

### macOS

Install dependencies:

```bash
brew install llvm@17 googletest google-benchmark
```

Configure the project:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOREDB_ENABLE_JIT=ON \
  -DCOREDB_BUILD_TESTS=ON \
  -DCOREDB_BUILD_BENCHMARKS=ON
```

If Homebrew LLVM is not discovered automatically:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOREDB_ENABLE_JIT=ON \
  -DCOREDB_BUILD_TESTS=ON \
  -DCOREDB_BUILD_BENCHMARKS=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm@17
```

Build:

```bash
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

### Ubuntu

Install LLVM 17:

```bash
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 17
```

Install test and benchmark dependencies:

```bash
sudo apt-get install -y libgtest-dev libbenchmark-dev
```

Configure:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOREDB_ENABLE_JIT=ON \
  -DCOREDB_BUILD_TESTS=ON \
  -DCOREDB_BUILD_BENCHMARKS=ON
```

Build:

```bash
cmake --build build -j"$(nproc)"
```

See:

```text
.github/workflows/ci.yml
```

for the CI configuration.

## Testing

Run the full test suite:

```bash
cd build
ctest --output-on-failure
```

Or run the test binary directly:

```bash
./build/tests/coredb_tests
```

Filter specific GoogleTest cases:

```bash
./build/tests/coredb_tests \
  --gtest_filter='*Concurren*'
```

The project currently contains **79 test cases** across:

- unit tests
- integration tests
- crash-recovery tests
- concurrency tests

Test coverage includes:

- bitmap behavior
- CRC32C
- segment serialization
- MVCC visibility
- table CRUD
- compaction
- SIMD equivalence
- JIT equivalence
- execution-strategy equivalence
- WAL truncation
- WAL corruption
- recovery idempotence
- concurrent transactions
- write-write conflicts

The project is also validated with sanitizer-specific configurations.

See [BENCHMARKS.md](BENCHMARKS.md) for the AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer results.

## Benchmarks

Build the Release configuration first.

Then run:

```bash
./build/benchmarks/bench_scan_throughput
```

```bash
./build/benchmarks/bench_simd_compare
```

```bash
./build/benchmarks/bench_exec_compare
```

```bash
./build/benchmarks/bench_mvcc_throughput
```

```bash
./build/benchmarks/bench_compaction
```

```bash
./build/benchmarks/bench_recovery --size-mb=512
```

The recovery workload is configurable:

```bash
./build/benchmarks/bench_recovery --size-mb=8
./build/benchmarks/bench_recovery --size-mb=512
./build/benchmarks/bench_recovery --size-mb=1024
```

subject to available disk space.

Benchmark binaries print the detected machine capabilities so their output can be interpreted in the context of the hardware that produced it.

The custom CLI benchmarks also write machine-readable results under:

```text
results/
```

This directory is ignored by Git.

## Reference benchmark results

The reference measurements were collected on:

```text
Apple M1 Pro
8 logical cores
16 GiB RAM
ARM64
```

Selected results from that machine include:

| Benchmark | Result |
|---|---:|
| Single-core 2 GiB scan | **22.2 GiB/s (~23.9 GB/s)** |
| NEON filtered sum vs scalar | **9.2x** |
| Vectorized execution vs Volcano | **24.4x** |
| LLVM JIT vs Volcano | **17.2x** |
| Single-threaded MVCC insert throughput | **~3.5M txns/sec** |
| Single-threaded MVCC update throughput | **~2.8M txns/sec** |
| 512 MB recovery, 1 worker | **130.3 MB/s** |
| 512 MB recovery, 2 workers | **137.1 MB/s** |

These figures are specific to the benchmark workloads and machine used.

The benchmark suite also exposed negative scaling under additional transaction and recovery workers because both paths currently contend on the same global transaction-manager mutex.

See [BENCHMARKS.md](BENCHMARKS.md) for:

- exact workloads
- full result tables
- machine configuration
- interpretation
- limitations
- reproduction instructions

## Execution comparison

The execution benchmark evaluates:

```sql
SELECT SUM(agg)
WHERE pred > 500;
```

over 4 million rows.

On the reference M1 Pro:

```text
Volcano      33.7 ms   119.6M rows/s
Vectorized    1.38 ms    2.90G rows/s
LLVM JIT      1.96 ms    2.07G rows/s
```

The vectorized implementation uses the runtime-selected SIMD kernel.

The JIT implementation generates and optimizes native code dynamically.

All execution paths are checked for result equivalence.

## Sanitizer verification

CoreDB is also tested under sanitizer builds.

### AddressSanitizer + UndefinedBehaviorSanitizer

Configuration:

```text
-fsanitize=address,undefined
```

with JIT disabled.

Result:

```text
72 / 72 tests passed
```

### ThreadSanitizer

Configuration:

```text
-fsanitize=thread
```

Concurrency-relevant result:

```text
20 / 20 tests passed
```

No data races were reported in the tested MVCC, compaction, table, and concurrency paths.

See [BENCHMARKS.md](BENCHMARKS.md) and [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for additional details.

## Repository layout

```text
coredb/
├── CMakeLists.txt
├── README.md
├── ARCHITECTURE.md
├── DESIGN_DECISIONS.md
├── BENCHMARKS.md
│
├── .github/
│   └── workflows/
│       └── ci.yml
│
├── include/
│   └── coredb/
│       ├── compaction/
│       ├── db/
│       ├── exec/
│       ├── jit/
│       ├── mvcc/
│       ├── recovery/
│       ├── simd/
│       ├── storage/
│       ├── table/
│       ├── util/
│       └── wal/
│
├── src/
│   ├── compaction/
│   ├── db/
│   ├── exec/
│   ├── jit/
│   ├── mvcc/
│   ├── recovery/
│   ├── simd/
│   ├── storage/
│   ├── table/
│   ├── util/
│   └── wal/
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── crash_recovery/
│   └── concurrency/
│
└── benchmarks/
    ├── bench_common.h
    ├── bench_scan_throughput.cpp
    ├── bench_simd_compare.cpp
    ├── bench_exec_compare.cpp
    ├── bench_mvcc_throughput.cpp
    ├── bench_compaction.cpp
    └── bench_recovery.cpp
```

## Current boundaries

CoreDB intentionally does not implement:

- SQL parsing
- a general expression evaluator
- a cost-based query optimizer
- multi-table joins
- secondary indexes
- distributed execution
- replication
- a client/server protocol
- serializable isolation
- fuzzy checkpoints

The execution engine currently supports one query shape:

```sql
SELECT SUM(aggregate_column)
WHERE predicate_column > threshold;
```

This keeps the project focused on comparing execution strategies while also exercising the storage, MVCC, recovery, SIMD, and JIT layers underneath them.

Known limitations include:

- linear `row_id` lookup
- a global transaction-manager mutex
- full-rewrite compaction
- quiescent checkpointing
- single-threaded scan execution
- no secondary-index maintenance

These tradeoffs and their measured effects are documented in [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) and [BENCHMARKS.md](BENCHMARKS.md).

## Design goals

CoreDB focuses on a narrow set of systems concerns:

- correctness under MVCC
- inspectable storage state
- deterministic recovery
- explicit concurrency behavior
- portable SIMD dispatch
- execution-strategy comparison
- reproducible performance measurements
- failure-path testing

The result is intentionally smaller than a general-purpose database, but the storage, recovery, and execution layers are implemented deeply enough to make their tradeoffs measurable and testable.
