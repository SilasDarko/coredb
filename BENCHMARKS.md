# Benchmarks

**Every number below was produced by actually running the benchmark binary
named next to it, in this repository, on the machine described here.**
Nothing is hard-coded, extrapolated silently, or backed out from a target.
Where a number is a *projection* onto different hardware, it is labeled
"projected" and shown with its assumptions, separately from anything
measured.

## Machine and toolchain

```
$ sysctl -n machdep.cpu.brand_string        Apple M1 Pro
$ sysctl -n hw.ncpu                          8 (logical cores; no SMT on this chip)
$ sysctl -n hw.memsize                       17179869184  (16 GiB)
$ sw_vers                                    macOS 26.6.2 (build 25G83)
$ clang++ --version                          Apple clang 21.0.0 (system compiler, used for
                                              the coredb library and non-JIT binaries)
$ /opt/homebrew/opt/llvm@17/bin/clang --version   Homebrew clang 17.0.6 (LLVM 17.0.6, used by jit::QueryJit)
$ cmake --version                            4.3.2
```

Disk: ~127 GB free on the volume used for benchmark scratch data. This is
the hard constraint behind every dataset-size decision below — see
"On the 200 GB / 88 GB/s / 500K txns/sec figures" at the end of this file.

Architecture: **arm64**. There is no AVX2/AVX-512 on this machine —
those kernels are real, compiled, and covered by
`tests/unit/test_simd_equivalence.cpp`'s equivalence checks on x86_64 CI
runners, but they have never executed here and no performance number for
them appears in this document. Every number below that says "NEON" or
"scalar" is what actually ran.

Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCOREDB_ENABLE_JIT=ON`,
then `cmake --build build -j8`. All numbers below are from that Release
build; correctness (equivalence, MVCC, WAL, recovery, concurrency) is
additionally verified under AddressSanitizer+UndefinedBehaviorSanitizer and
ThreadSanitizer (see "Sanitizer verification" below), which are Debug
builds and not used for any timing number here.

Every benchmark binary prints its detected CPU capabilities
(`util::DescribeCapabilities`) as its first line of output, so a run's
output is self-describing even out of context.

---

## 1. Scan throughput — `bench_scan_throughput`

Unconditional `SUM()` over a resident `int64_t` column (`simd::Sum`,
dispatched to NEON on this machine), at sizes from 8 KiB (L1-resident) to
2 GiB (far past this chip's caches).

```
$ ./build/benchmarks/bench_scan_throughput
```

| rows | bytes | time | throughput |
|---:|---:|---:|---:|
| 1,024 | 8 KiB | 0.000 ms | 27.0 GiB/s |
| 16,384 | 128 KiB | 0.005 ms | 23.8 GiB/s |
| 262,144 | 2 MiB | 0.083 ms | 23.7 GiB/s |
| 1,048,576 | 8 MiB | 0.337 ms | 23.2 GiB/s |
| 4,194,304 | 32 MiB | 1.35 ms | 23.2 GiB/s |
| 16,777,216 | 128 MiB | 5.38 ms | 23.2 GiB/s |
| 67,108,864 | 512 MiB | 21.4 ms | 23.4 GiB/s |
| 268,435,456 | 2 GiB | 90.0 ms | **22.2 GiB/s (≈ 23.9 GB/s)** |

**Reading this:** throughput is essentially flat from 128 KiB to 2 GiB —
this is a single core, memory-bandwidth-bound scan, not a cache-residency
artifact. 8 KiB is the outlier (L1-resident, ALU/loop-overhead bound, not
comparable to the rest). This is single-threaded; it does not attempt to
saturate the chip's full memory bandwidth (which requires many cores
issuing traffic concurrently) — this kernel measures one core's achievable
scan rate, which is the number `exec::RunVectorized` and the SIMD-tier
comparison below actually depend on.

## 2. Scalar vs. NEON vs. AVX2 vs. AVX-512 — `bench_simd_compare`

```
$ ./build/benchmarks/bench_simd_compare
```

16.7M rows (128 MiB), all reported on this arm64 machine — scalar vs. NEON
only; AVX2/AVX-512 rows are printed as `SkipWithError("... not available on
this CPU")` by the same binary on an x86_64 host without those features,
and are not reported here because they didn't run here:

| kernel | time | throughput |
|---|---:|---:|
| `Sum_Scalar` | 3.09 ms | 42.3 GiB/s |
| `Sum_Neon` | 2.87 ms | **44.7 GiB/s (1.05x)** |
| `FilterGtSum_Scalar` | 50.3 ms | 5.0 GiB/s |
| `FilterGtSum_Neon` | 5.48 ms | **45.8 GiB/s (9.2x)** |

**Reading this:** for a plain unconditional sum, scalar and NEON are close
— Apple clang's auto-vectorizer already does well on the trivial reduction,
and `Sum_Neon`'s advantage is modest (the kernel originally used one
128-bit accumulator and *lost* to auto-vectorized scalar; splitting it into
two independent accumulator chains — see `src/simd/kernels_neon.cpp` — is
what got it back ahead. A single accumulator serializes on
`acc = acc + next`; two chains let the CPU overlap the additions).

For the filtered sum, NEON's **9.2x** speedup is the real story, and it
isn't really about SIMD width — it's branch misprediction. `pred[i] > 500`
against uniform-random `[0, 1000)` data is right at 50% selectivity, the
worst case for a branch predictor. `FilterGtSum_Scalar` branches per
element; `FilterGtSum_Neon` computes a mask and does a branchless
mask-and-add (`vandq_s64`/`vsubq_s64`) — no misprediction possible. This is
the same technique the JIT loop was rewritten to use (§3).

## 3. Volcano vs. vectorized vs. JIT — `bench_exec_compare`

`SUM(agg) WHERE pred > 500` over 4,000,000 rows across 61 segments (65,536
rows/segment), uniform-random predicate over `[0, 1000)` (~50% selectivity,
so pruning cannot skip any segment — this measures per-row execution cost,
not pruning).

```
$ ./build/benchmarks/bench_exec_compare
```

| strategy | time | throughput | vs. Volcano |
|---|---:|---:|---:|
| `RunVolcano` | 33.7 ms | 119.6M rows/s | 1.0x |
| `RunVectorized` (NEON) | 1.38 ms | 2.90G rows/s | 24.4x |
| `RunJit` | 1.96 ms | 2.07G rows/s | **17.2x** |

**Reading this — the single most interesting result in this project:** the
JIT number above is *after* two changes made in response to the first
measurement, not the first thing that was tried:

1. **First version measured 33.3ms → 24.8ms (1.3x vs. Volcano).** The
   generated IR was handed straight to LLJIT with no optimization pass at
   all — no `mem2reg`, nothing. Running LLVM's real `-O3` pipeline
   (`PassBuilder::buildPerModuleDefaultPipeline`) over the IR before
   codegen brought it to 13.4ms (2.5x).
2. **Still nowhere near the SIMD kernel.** The generated loop had a real
   conditional branch (`if (pred[i] > threshold) { sum += ...; } `) — the
   same shape `FilterGtSum_Scalar` above loses badly on. LLVM's
   auto-vectorizer won't vectorize a branchy reduction like that. Rewriting
   the generated IR to be **branchless** (`select` instead of a
   conditional branch — see `src/jit/query_jit.cpp`, mirroring the exact
   masking trick `FilterGtSum_Neon` uses) let the vectorizer recognize the
   loop and emit NEON instructions automatically: 1.96ms, within 40% of the
   hand-written kernel, using *generated* code.

This is a real engineering lesson, not a benchmark quirk: **an LLVM JIT
only outperforms an interpreter if you actually run the optimizer on the
IR you generate, and it only auto-vectorizes if the IR is shaped so the
vectorizer can prove it's safe to do — writing a naive scalar loop and
expecting LLVM to save you is not enough.** Both changes are still in the
code (`src/jit/query_jit.cpp`), and `tests/unit/test_jit_equivalence.cpp`
pins the compiled function's output against the scalar reference across
sizes 0, 1, 2, 100, and 10007 rows, so neither optimization could have
silently changed the result.

## 4. MVCC insert/update throughput — `bench_mvcc_throughput`

Two workloads, each run at 1/2/4/8 worker threads for 1.5 real seconds:
`insert` (every txn creates a new row, zero contention possible) and
`update` (every txn updates one of 16 fixed keys — a deliberate
high-contention hotspot; aborted conflicts are counted and reported, not
hidden).

```
$ ./build/benchmarks/bench_mvcc_throughput
```

| workload | threads | committed | aborted | txns/sec |
|---|---:|---:|---:|---:|
| insert | 1 | 5,280,787 | 0 | **3,496,089** |
| insert | 2 | 3,143,521 | 0 | 2,020,987 |
| insert | 4 | 1,861,601 | 0 | 1,236,751 |
| insert | 8 | 1,376,423 | 0 | 914,430 |
| update | 1 | 4,244,208 | 0 | **2,819,948** |
| update | 2 | 2,784,699 | 33,162 | 1,849,609 |
| update | 4 | 1,464,286 | 67,106 | 975,206 |
| update | 8 | 503,128 | 75,555 | 334,670 |

**Reading this:** single-threaded throughput is high — **3.5M txns/sec**
insert, **2.8M txns/sec** update — and *drops* as thread count increases in
both workloads. This is the clearest, most concrete finding in this
project: `mvcc::TransactionManager::Begin()` and `::Commit()` both take one
process-wide `std::mutex` (see `include/coredb/mvcc/transaction.h`).
Every transaction, regardless of what row it touches, serializes on that
one lock twice. More threads means more contention on that lock and more
context-switch overhead, not more parallelism. See DESIGN_DECISIONS.md for
what a fix would look like (a lock-free or sharded timestamp allocator);
it was not attempted here because verifying a lock-free MVCC timestamp
scheme's correctness under the remaining time budget for this project was
judged riskier than reporting the bottleneck honestly. Note this same root
cause reappears independently in §6 (recovery).

The `insert` workload runs with **no** background compactor —
`Table::Insert` never scans the delta layer, so compacting it is pure
overhead (confirmed by measurement: running a compactor alongside this
workload measured ~700K txns/sec single-threaded, roughly 5x worse than
without one). The `update` workload **does** run
`compaction::BackgroundCompactor` (threshold 2000 rows, 5ms poll) — without
it, `Update`'s linear delta scan (no row_id index; see
DESIGN_DECISIONS.md) makes every subsequent update on the same 16-key
hotspot scan an ever-growing vector, and measured throughput visibly
collapsed over the run instead of reaching a steady state. Both policy
choices, and why they differ, are in `benchmarks/bench_mvcc_throughput.cpp`.

## 5. Compaction — `bench_compaction`

Repeated rounds of (insert a batch, delete ~10% of everything inserted so
far, compact), at three batch sizes, 5 rounds each:

```
$ ./build/benchmarks/bench_compaction
```

| batch size | round | delta before | base rows after | duration | rows/sec |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 0 | 1,000 | 900 | 0.69 ms | 1,447,266 |
| 1,000 | 4 | 1,302 | 3,687 | 1.18 ms | 1,107,221 |
| 10,000 | 0 | 10,000 | 9,000 | 4.13 ms | 2,422,799 |
| 10,000 | 4 | 13,094 | 36,856 | 6.15 ms | 2,128,082 |
| 50,000 | 0 | 50,000 | 45,000 | 12.6 ms | 3,960,331 |
| 50,000 | 4 | 65,511 | 184,280 | **34.2 ms** | 1,917,774 |

**Reading this:** compaction duration climbs with base size within a batch
size (round 0 → round 4), exactly as the architecture predicts —
`Table::Compact()` rewrites the *entire* base segment every cycle (see
ARCHITECTURE.md/DESIGN_DECISIONS.md), so cost is O(total base rows), not
O(delta size). At 50,000-row batches, base grows from 45K to 184K rows
over 5 rounds and per-round compaction time nearly triples. This is the
expected, documented cost of the "full rewrite, not leveled" design choice
— not a bug.

## 6. Recovery scalability — `bench_recovery`

`bench_recovery --size-mb=N` generates a WAL of the requested size (rows
are a single fixed-length string column; row count is derived by checking
actual file size, not estimated), then replays it from scratch at
1/2/4/8/16/32 worker threads, each a fresh `Table`, no checkpoint.

### 8 MB (30,000 rows) — quick/CI-sized run

```
$ ./build/benchmarks/bench_recovery --size-mb=8
```

| workers | time | MB/s | rows/s | speedup |
|---:|---:|---:|---:|---:|
| 1 | 49.2 ms | 173.3 | 609,877 | 1.00x |
| 2 | 48.7 ms | 174.9 | 615,521 | 1.01x |
| 4 | 66.2 ms | 128.8 | 453,179 | 0.74x |
| 8 | 83.3 ms | 102.3 | 359,937 | 0.59x |
| 16 | 91.9 ms | 92.8 | 326,356 | 0.54x |
| 32 | 114.4 ms | 74.5 | 262,132 | 0.43x |

### 512 MB (1,802,000 rows)

```
$ ./build/benchmarks/bench_recovery --size-mb=512
```

| workers | time | MB/s | rows/s | speedup |
|---:|---:|---:|---:|---:|
| 1 | 3.93 s | 130.3 | 458,467 | 1.00x |
| 2 | 3.73 s | 137.1 | 482,491 | **1.05x** |
| 4 | 4.60 s | 111.4 | 391,856 | 0.85x |
| 8 | 6.57 s | 78.0 | 274,470 | 0.60x |
| 16 | 7.08 s | 72.4 | 254,660 | 0.56x |
| 32 | 8.37 s | 61.2 | 215,267 | 0.47x |

Checksum verification (`Segment::VerifyIntegrity`, run after every replay)
passed on every configuration in both runs: `checksum_ok=yes`.

**Reading this — more parallel workers makes recovery slower, not
faster, at every size tested.** The row_id-sharded partitioning itself is
correct (see DESIGN_DECISIONS.md for why), but each replayed operation
still goes through `Table::Begin()`/`Commit()`, which serialize on
`TransactionManager`'s single mutex — **the exact same bottleneck §4 found
independently.** Sharding the *data* correctly doesn't help when the
*apply path* has a global lock regardless of which shard called it. Two
workers occasionally edges out one (thread-count noise, not real
parallelism — 1.01x and 1.05x are within run-to-run variance); 4 and above
are consistently worse. This is reported as measured, not smoothed over;
DESIGN_DECISIONS.md names the fix (a bulk-apply path for recovery that
bypasses per-op transaction bookkeeping, which is safe during recovery
specifically because there are no concurrent readers yet).

## Sanitizer verification

Not a performance benchmark, but part of what makes the numbers above
trustworthy: the full test suite (all 79 test cases,
`COREDB_ENABLE_JIT=OFF` where noted) passes clean under:

- **AddressSanitizer + UndefinedBehaviorSanitizer** (`-fsanitize=address,undefined`,
  Debug build): 72/72 tests pass (JIT tests excluded from this configuration).
  One real bug was found and fixed this way — see DESIGN_DECISIONS.md,
  "Hardening found by fuzzing the length header in a test."
- **ThreadSanitizer** (`-fsanitize=thread`, Debug build): all
  concurrency/MVCC/compaction/table-CRUD tests (20 cases) pass with zero
  reported data races.

Both are wired into CI (`.github/workflows/ci.yml`, `sanitizers` job) and
were run locally on this machine before being trusted in CI.

## On the 200 GB / 88 GB/s / 500K txns/sec / 32-REDO-thread figures

This machine cannot produce those numbers, for reasons that are physical,
not implementation quality:

- **200 GB recovery workload:** this machine has ~127 GB free disk and
  16 GB RAM. A 200 GB WAL does not fit. `bench_recovery`'s `--size-mb` flag
  is exactly how you'd run that test on hardware where it does fit — the
  recovery code path itself does not change with dataset size (see
  `RecoveryConfig`/`RecoveryManager`).
- **88 GB/s scan throughput:** measured single-core throughput here is
  ~23 GB/s (§1). This machine's *aggregate* memory bandwidth (many cores
  issuing traffic concurrently, LPDDR5) is higher than that, but CoreDB's
  scan path is currently single-threaded per segment — reaching 88 GB/s
  would need both a multi-core parallel scan (not yet implemented — see
  "Future work" below) *and* server-class memory bandwidth well beyond a
  laptop's, most plausibly demonstrated on an x86_64 host with AVX-512 and
  many memory channels.
- **32 REDO threads:** this machine has 8 logical cores. §6 shows 16/32
  configured workers running as oversubscribed threads on 8 cores, which
  is a real, valid thing to measure (and did), but "32 REDO threads"
  as a *scaling* claim needs a machine with enough cores for that number to
  mean something, which this one doesn't have.
- **500K txns/sec MVCC-delta:** measured single-threaded throughput
  (§4) is **3.5M txns/sec insert / 2.8M txns/sec update — both already
  above 500K** without needing a bigger machine. What does *not* hold up
  at any thread count on this hardware is *multi-threaded scaling* past
  that number; see §4's discussion of the `TransactionManager` mutex.

**What would be needed to responsibly attempt the full-scale figures:** an
x86_64 host with AVX-512, many cores (32+ for the REDO-thread claim to be
meaningful), high memory bandwidth (multi-channel DDR5 or similar), and
enough NVMe storage for a 200 GB WAL plus checkpoint segments —
run this exact benchmark suite there. No code changes would be needed
for the recovery or SIMD-tier benchmarks (`--size-mb` and runtime CPU
detection already parameterize both); the MVCC throughput ceiling would
still need the `TransactionManager` mutex contention fix described in
DESIGN_DECISIONS.md before multi-threaded numbers would scale, regardless
of hardware.

## Future work (identified by these benchmarks, not attempted here)

1. Replace `TransactionManager`'s global mutex with a lock-free or sharded
   timestamp allocator — the single highest-leverage change, since it's
   the root cause behind both §4's negative thread scaling and §6's.
2. A bulk-apply path for `RecoveryManager` that skips per-operation
   `Begin`/`Commit` bookkeeping (safe specifically during recovery, where
   there are no concurrent readers).
3. A row_id index (hash or B-tree, MVCC-aware) to remove `Update`/`Delete`'s
   linear delta scan.
4. Incremental/leveled compaction, to avoid full-base-rewrite cost (§5).
5. A multi-core parallel scan path, needed before an 88 GB/s-class scan
   number would be meaningful on any hardware.
