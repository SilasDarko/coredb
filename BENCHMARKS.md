# Benchmarks

This document describes how CoreDB's performance measurements were collected, what each benchmark exercises, and the limitations of the current results.

The results below were collected from the benchmark binaries in this repository on the machine described here. Each benchmark reports the workload, execution environment, and measured result needed to reproduce the run.

## Machine and toolchain

```text
Apple M1 Pro
8 logical cores
16 GiB memory
macOS 26.6.2 (build 25G83)

System compiler:
Apple clang 21.0.0

LLVM used by the JIT:
Homebrew LLVM 17.0.6

CMake:
4.3.2
```

Disk space available on the benchmark volume was approximately 127 GB.

Architecture:

```text
arm64
```

This machine supports ARM NEON but not AVX2 or AVX-512.

The AVX2 and AVX-512 implementations are compiled on supported x86-64 builds, while SIMD equivalence tests execute only the instruction-set tiers available on the host CPU. No AVX2 or AVX-512 performance numbers in this document were measured on the M1 Pro.

The Release build used for performance measurements was produced with:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOREDB_ENABLE_JIT=ON

cmake --build build -j8
```

Every benchmark binary prints the detected CPU capabilities using:

```text
util::DescribeCapabilities
```

so benchmark output remains self-describing when copied outside the repository.

---

## 1. Scan throughput

Benchmark:

```text
bench_scan_throughput
```

Run with:

```bash
./build/benchmarks/bench_scan_throughput
```

This benchmark performs an unconditional `SUM()` over a resident `int64_t` column using `simd::Sum`.

On this machine, runtime dispatch selects the NEON implementation.

The workload ranges from 8 KiB to 2 GiB so the benchmark includes both cache-resident and memory-bandwidth-bound cases.

| Rows | Data size | Time | Throughput |
|---:|---:|---:|---:|
| 1,024 | 8 KiB | 0.000 ms | 27.0 GiB/s |
| 16,384 | 128 KiB | 0.005 ms | 23.8 GiB/s |
| 262,144 | 2 MiB | 0.083 ms | 23.7 GiB/s |
| 1,048,576 | 8 MiB | 0.337 ms | 23.2 GiB/s |
| 4,194,304 | 32 MiB | 1.35 ms | 23.2 GiB/s |
| 16,777,216 | 128 MiB | 5.38 ms | 23.2 GiB/s |
| 67,108,864 | 512 MiB | 21.4 ms | 23.4 GiB/s |
| 268,435,456 | 2 GiB | 90.0 ms | **22.2 GiB/s (~23.9 GB/s)** |

### Interpretation

Throughput remains close to 23 GiB/s from 128 KiB through 2 GiB.

That stability suggests the larger runs are measuring sustained single-core scan throughput rather than benefiting primarily from cache residency.

The 8 KiB result is different because the working set fits easily in L1 cache and loop overhead becomes a larger part of the measurement.

This benchmark is single-threaded. It measures the throughput available to the current scan kernel on one core rather than the aggregate memory bandwidth of the full processor.

---

## 2. Scalar vs. NEON

Benchmark:

```text
bench_simd_compare
```

Run with:

```bash
./build/benchmarks/bench_simd_compare
```

The benchmark operates on 16.7 million rows, or approximately 128 MiB of `int64_t` data.

Because the benchmark machine is ARM64, the measured comparison is between scalar and NEON implementations.

| Kernel | Time | Throughput | Relative |
|---|---:|---:|---:|
| `Sum_Scalar` | 3.09 ms | 42.3 GiB/s | 1.00x |
| `Sum_Neon` | 2.87 ms | 44.7 GiB/s | **1.05x** |
| `FilterGtSum_Scalar` | 50.3 ms | 5.0 GiB/s | 1.00x |
| `FilterGtSum_Neon` | 5.48 ms | 45.8 GiB/s | **9.2x** |

### Unconditional sum

The scalar and NEON unconditional-sum implementations are close.

Apple Clang already auto-vectorizes the simple scalar reduction effectively, so the handwritten NEON version has only a modest advantage.

The NEON implementation originally used one accumulator chain:

```text
acc = acc + next
```

That creates a dependency between consecutive vector additions.

The final implementation uses two independent accumulators so the processor can overlap more work before the partial sums are combined.

See:

```text
src/simd/kernels_neon.cpp
```

### Filtered sum

The filtered workload shows a much larger difference.

The predicate is approximately:

```text
pred[i] > 500
```

against uniformly distributed values in:

```text
[0, 1000)
```

This produces roughly 50% selectivity.

The scalar implementation performs a conditional branch for each element. At approximately 50% selectivity, the branch outcome is difficult to predict consistently.

The NEON implementation instead computes a comparison mask and uses branchless masked accumulation.

The measured result was:

```text
9.2x faster than the scalar implementation
```

for this workload.

The same branchless approach is also used by the LLVM JIT path.

---

## 3. Volcano vs. vectorized vs. LLVM JIT

Benchmark:

```text
bench_exec_compare
```

Run with:

```bash
./build/benchmarks/bench_exec_compare
```

The benchmark evaluates:

```sql
SELECT SUM(agg)
WHERE pred > 500;
```

over 4,000,000 rows distributed across 61 segments.

Each segment contains approximately:

```text
65,536 rows
```

Predicate values are uniformly distributed over:

```text
[0, 1000)
```

so selectivity is approximately 50%.

No segment can be pruned under this workload, which isolates per-row execution behavior rather than segment-pruning effectiveness.

| Strategy | Time | Throughput | vs. Volcano |
|---|---:|---:|---:|
| `RunVolcano` | 33.7 ms | 119.6M rows/s | 1.0x |
| `RunVectorized` (NEON) | 1.38 ms | 2.90G rows/s | **24.4x** |
| `RunJit` | 1.96 ms | 2.07G rows/s | **17.2x** |

### JIT optimization path

The JIT result changed substantially during implementation.

The first version generated LLVM IR and handed it directly to LLJIT without running an optimization pipeline.

Measured result:

```text
33.3 ms → 24.8 ms
1.3x vs. Volcano
```

The generated IR was then passed through LLVM's `-O3` pipeline using:

```text
PassBuilder::buildPerModuleDefaultPipeline
```

That reduced execution time to:

```text
13.4 ms
2.5x vs. Volcano
```

The generated loop still contained a conditional branch:

```text
if (pred[i] > threshold) {
    sum += value[i];
}
```

The loop was then rewritten to use branchless LLVM `select` operations.

Conceptually:

```text
selected = pred[i] > threshold ? value[i] : 0
sum += selected
```

This representation allowed LLVM's optimizer and loop vectorizer to emit SIMD instructions for the generated code.

The final measured result was:

```text
1.96 ms
17.2x vs. Volcano
```

The result shows that JIT performance depends heavily on both optimization passes and the structure of generated IR. Generating native code alone does not guarantee efficient execution.

Correctness is verified by:

```text
tests/unit/test_jit_equivalence.cpp
```

which compares JIT output against the scalar reference across multiple input sizes.

---

## 4. MVCC insert and update throughput

Benchmark:

```text
bench_mvcc_throughput
```

Run with:

```bash
./build/benchmarks/bench_mvcc_throughput
```

Two workloads are measured for 1.5 seconds each across 1, 2, 4, and 8 worker threads.

### Insert workload

Each transaction creates a new row.

There is no logical row contention between workers.

| Threads | Committed | Aborted | Txns/sec |
|---:|---:|---:|---:|
| 1 | 5,280,787 | 0 | **3,496,089** |
| 2 | 3,143,521 | 0 | 2,020,987 |
| 4 | 1,861,601 | 0 | 1,236,751 |
| 8 | 1,376,423 | 0 | 914,430 |

The peak measured result was approximately:

```text
3.5M single-threaded MVCC inserts/sec
```

### Update workload

Each transaction updates one of 16 fixed keys.

This intentionally creates a high-contention workload.

| Threads | Committed | Aborted | Txns/sec |
|---:|---:|---:|---:|
| 1 | 4,244,208 | 0 | **2,819,948** |
| 2 | 2,784,699 | 33,162 | 1,849,609 |
| 4 | 1,464,286 | 67,106 | 975,206 |
| 8 | 503,128 | 75,555 | 334,670 |

The peak measured result was approximately:

```text
2.8M single-threaded MVCC updates/sec
```

### Thread scaling

Throughput decreases as the thread count rises in both workloads.

The insert workload has no row-level contention, so its negative scaling points to contention in shared transaction-management state rather than contention over individual records.

`mvcc::TransactionManager::Begin()` and `TransactionManager::Commit()` both acquire one process-wide `std::mutex`.

Every transaction therefore serializes through that shared lock twice regardless of which row it accesses.

Additional worker threads increase lock contention and scheduling overhead rather than increasing throughput.

See:

```text
include/coredb/mvcc/transaction.h
```

and [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md).

A sharded or lock-free timestamp-allocation design is left as future work because changing the timestamp mechanism also requires additional concurrency and correctness validation.

### Compaction policy during the benchmark

The insert workload does not run the background compactor.

`Table::Insert` appends a new delta record and does not search for an existing row version, so compaction adds unnecessary work to this specific workload.

The update workload does run:

```text
compaction::BackgroundCompactor
```

with:

```text
threshold = 2000 rows
poll interval = 5 ms
```

`Update` currently performs a linear scan of the delta layer when locating the relevant row version.

Without compaction, the delta vector grows continuously and update latency increases throughout the run.

The benchmark uses background compaction so the update workload reaches a more stable operating regime.

---

## 5. Compaction

Benchmark:

```text
bench_compaction
```

Run with:

```bash
./build/benchmarks/bench_compaction
```

The benchmark repeatedly:

1. inserts a batch
2. deletes approximately 10% of the accumulated rows
3. runs `Table::Compact()`

Each batch size is tested for five rounds.

| Batch size | Round | Delta before | Base rows after | Duration | Rows/sec |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 0 | 1,000 | 900 | 0.69 ms | 1,447,266 |
| 1,000 | 4 | 1,302 | 3,687 | 1.18 ms | 1,107,221 |
| 10,000 | 0 | 10,000 | 9,000 | 4.13 ms | 2,422,799 |
| 10,000 | 4 | 13,094 | 36,856 | 6.15 ms | 2,128,082 |
| 50,000 | 0 | 50,000 | 45,000 | 12.6 ms | 3,960,331 |
| 50,000 | 4 | 65,511 | 184,280 | **34.2 ms** | 1,917,774 |

### Interpretation

Compaction time increases as the base grows.

This matches the implementation because:

```text
Table::Compact()
```

rewrites the complete base representation rather than performing incremental or leveled compaction.

The cost therefore scales approximately with:

```text
total base rows
```

rather than only with the current delta size.

For the 50,000-row batch workload, the base grows from approximately 45,000 rows to 184,000 rows over five rounds, while measured compaction time rises from 12.6 ms to 34.2 ms.

This is an expected consequence of the current full-rewrite design.

---

## 6. Recovery scalability

Benchmark:

```text
bench_recovery
```

Usage:

```bash
./build/benchmarks/bench_recovery --size-mb=N
```

The benchmark generates a WAL of approximately the requested size, then replays it into a fresh table using:

```text
1
2
4
8
16
32
```

configured worker threads.

Rows contain one fixed-length string column.

The benchmark determines the row count from the actual WAL file size rather than relying on an estimated number of bytes per row.

No checkpoint is used for these runs.

### 8 MB run

Run with:

```bash
./build/benchmarks/bench_recovery --size-mb=8
```

Approximately:

```text
30,000 rows
```

| Workers | Time | MB/s | Rows/s | Speedup |
|---:|---:|---:|---:|---:|
| 1 | 49.2 ms | 173.3 | 609,877 | 1.00x |
| 2 | 48.7 ms | 174.9 | 615,521 | 1.01x |
| 4 | 66.2 ms | 128.8 | 453,179 | 0.74x |
| 8 | 83.3 ms | 102.3 | 359,937 | 0.59x |
| 16 | 91.9 ms | 92.8 | 326,356 | 0.54x |
| 32 | 114.4 ms | 74.5 | 262,132 | 0.43x |

### 512 MB run

Run with:

```bash
./build/benchmarks/bench_recovery --size-mb=512
```

Approximately:

```text
1,802,000 rows
```

| Workers | Time | MB/s | Rows/s | Speedup |
|---:|---:|---:|---:|---:|
| 1 | 3.93 s | 130.3 | 458,467 | 1.00x |
| 2 | 3.73 s | 137.1 | 482,491 | **1.05x** |
| 4 | 4.60 s | 111.4 | 391,856 | 0.85x |
| 8 | 6.57 s | 78.0 | 274,470 | 0.60x |
| 16 | 7.08 s | 72.4 | 254,660 | 0.56x |
| 32 | 8.37 s | 61.2 | 215,267 | 0.47x |

Checksum verification completed successfully after every replay:

```text
checksum_ok=yes
```

### Interpretation

Additional recovery workers do not improve throughput on the current implementation.

The best 512 MB result was:

```text
2 workers
137.1 MB/s
1.05x relative to one worker
```

That small difference is close enough to single-worker performance that it should not be interpreted as meaningful scaling.

Performance decreases consistently at four or more workers.

The data partitioning itself is performed by:

```text
row_id % num_workers
```

but each replayed operation still calls:

```text
Table::Begin()
Table::Commit()
```

Those operations serialize through the same global transaction-manager mutex observed in the MVCC throughput benchmark.

The benchmark therefore identifies the same synchronization bottleneck through two separate workloads:

- normal transactional execution
- recovery replay

A recovery-specific bulk-apply path could avoid per-operation transaction bookkeeping because recovery runs before concurrent readers are admitted.

That optimization is not part of the current implementation.

---

## Sanitizer verification

The project was additionally validated with sanitizer-specific test configurations.

These runs are correctness checks rather than performance measurements, and their timings are not used anywhere in the benchmark results above.

### AddressSanitizer + UndefinedBehaviorSanitizer

Configuration:

```text
-fsanitize=address,undefined
Debug build
COREDB_ENABLE_JIT=OFF
```

Result:

```text
72 / 72 tests passed
```

The JIT-specific tests are excluded from this sanitizer configuration.

This run exposed a real robustness issue in the WAL reader: a corrupted record-length header could request a very large allocation before the truncation check executed.

The reader was changed to validate record lengths before allocating based on the declared size.

See [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md).

### ThreadSanitizer

Configuration:

```text
-fsanitize=thread
Debug build
```

Concurrency-relevant tests executed:

```text
20 / 20 passed
```

ThreadSanitizer reported no data races in the tested concurrency, MVCC, compaction, and table operations.

Both sanitizer configurations are represented in:

```text
.github/workflows/ci.yml
```

## Benchmark limitations

These measurements were collected on one Apple M1 Pro laptop.

They should therefore be read as measurements of this implementation on this hardware, not as universal performance characteristics.

Important limitations include:

- scan benchmarks are single-threaded
- the machine has 8 logical cores
- ARM64 measurements use NEON rather than AVX2 or AVX-512
- recovery benchmarks are bounded by the available local disk capacity
- the current transaction manager contains a global mutex
- the current compaction strategy rewrites the full base
- the delta layer does not contain a row-id index
- execution benchmarks use one fixed query shape
- benchmark data is synthetic

The benchmark binaries are parameterized where practical so the same implementation can be tested on different hardware without changing the storage or execution code.

## Future work

### 1. Reduce transaction-manager contention

The highest-impact concurrency limitation is the process-wide mutex used by the transaction manager.

A future implementation could investigate:

- atomic timestamp allocation
- sharded transaction state
- reduced critical sections
- lock-free snapshot tracking

Any replacement would require concurrency testing to preserve the current snapshot-isolation guarantees.

### 2. Add recovery-specific bulk apply

Recovery currently routes each operation through normal transaction bookkeeping.

Because recovery happens before concurrent readers are admitted, a dedicated bulk-apply path could reconstruct committed state without acquiring the transaction-manager mutex for every individual operation.

### 3. Add a row-id index

`Update` and `Delete` currently locate row versions through a linear delta scan.

A hash index or tree keyed by `row_id` could reduce lookup cost as the delta grows.

The index would need to remain consistent with MVCC visibility and compaction.

### 4. Incremental or leveled compaction

The current full-rewrite strategy has cost proportional to the entire base.

A future implementation could use incremental or leveled compaction to reduce write amplification and avoid rebuilding all base rows every cycle.

### 5. Multi-core scans

The current scan benchmark uses one execution thread.

Partitioning segments across worker threads would allow CoreDB to measure aggregate scan throughput and determine where the system begins to saturate the machine's memory subsystem.

### 6. Benchmark on additional architectures

The project already contains separate:

- scalar
- NEON
- AVX2
- AVX-512

execution paths.

Running the same benchmark suite on x86-64 systems with AVX2 and AVX-512 support would allow direct comparison of those implementations under real hardware rather than relying only on compilation and correctness checks.
