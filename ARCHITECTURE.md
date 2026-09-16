# CoreDB Architecture

CoreDB is a single-process, embedded, columnar storage engine with MVCC, crash recovery, and three interchangeable execution strategies: Volcano iteration, vectorized batches, and LLVM JIT compilation.

It is intentionally not a client/server database. There is no network layer, SQL parser, or general-purpose expression evaluator. The implementation focuses on storage, transactional visibility, recovery, execution strategy, SIMD, and JIT compilation. See [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for the reasoning behind those boundaries.

```text
                     ┌─────────────────────────┐
                     │      db::Database       │
                     │   Table + WAL wiring    │
                     └───────────┬─────────────┘
                                 │
                 ┌───────────────┼────────────────┐
                 ▼                                ▼
        ┌─────────────────┐              ┌──────────────────┐
        │  table::Table   │              │  wal::WalWriter  │
        │ (MVCC + delta)  │              │ append-only log  │
        └────────┬────────┘              └──────────────────┘
                 │ owns
     ┌───────────┼────────────┐
     ▼                        ▼
┌──────────────┐       ┌────────────────────┐
│  DeltaLayer  │       │   base_segments_   │
│  (mutable)   │       │ vector<shared_ptr  │
└──────────────┘       │ <const Segment>>   │
     ▲                 └────────────────────┘
     │                        ▲
     │     Table::Compact()   │
     └────────────────────────┘
          promotes / retires /
              rewrites

Execution benchmarks read base segments directly:

exec::RunVolcano | exec::RunVectorized | exec::RunJit
        │                 │                    │
        ▼                 ▼                    ▼
   virtual Next()   simd::FilterGtSum()   jit::QueryJit
   per-row chain    scalar / NEON /       LLVM ORC,
                    AVX2 / AVX-512        O3 optimized
```

## Module map

| Directory | Responsibility |
|---|---|
| `util/` | CRC32C, validity bitmap, CPU capability detection |
| `storage/` | Immutable columnar `Segment` / `Column` storage, serialization, checksums |
| `mvcc/` | `TransactionManager` and append-only `DeltaLayer` |
| `table/` | `Table`: Insert, Update, Delete, Scan, and `Compact()` |
| `compaction/` | `BackgroundCompactor` |
| `wal/` | Append-only, checksummed, length-prefixed WAL records |
| `recovery/` | Checkpoint loading and REDO replay |
| `db/` | `Database`: table and WAL lifecycle |
| `exec/` | Volcano, vectorized, and JIT execution |
| `simd/` | Scalar, NEON, AVX2, and AVX-512 kernels with runtime dispatch |
| `jit/` | LLVM ORC JIT compilation and optimization |

## The query CoreDB executes

The execution-engine benchmarks and correctness tests use one query shape:

```sql
SELECT SUM(aggregate_column)
WHERE predicate_column > threshold;
```

The query operates over `int64` columns.

This narrow scope is deliberate. CoreDB compares row-at-a-time, vectorized, and JIT-compiled execution on identical work rather than implementing a general-purpose planner or expression system.

A larger database would place expression trees, planning, and cost-based optimization in front of this execution layer. CoreDB deliberately stops at execution-engine comparison.

See:

```text
include/coredb/exec/query.h
```

## Storage layer: `Segment` and `Column`

A `Segment` is immutable after being built with `Segment::Build` or loaded with `Segment::LoadFromFile`.

Each column contains:

- a contiguous typed array such as `int32_t[]`, `int64_t[]`, or `double[]`
- for strings, a dense `uint32_t[]` of dictionary codes and a `Dictionary`
- a `util::Bitmap` validity bitmap with one bit per row
- `min` and `max` statistics
- a CRC32C checksum

For string columns, the dictionary uses first-seen-order interning.

The checksum covers:

- validity bitmap
- column data
- dictionary contents when present

Integrity is checked while loading a segment and can also be rechecked independently through `VerifyIntegrity()` after recovery.

### Hidden row identifier

Every table contains a hidden leading column:

```text
__row_id__
```

It is stored as `int64_t` and assigned by `Table`.

The hidden row identifier is not part of the user-facing schema. It gives `Update` and `Delete` a stable logical identity for a row regardless of whether the newest version currently lives in an immutable base segment or the mutable delta layer.

See:

```text
table::kRowIdColumnIndex
```

### Segment pruning

CoreDB stores per-segment minimum and maximum values.

For the supported predicate:

```text
column > threshold
```

a segment can be skipped when its recorded maximum value is less than or equal to the threshold.

Both `RunVolcano` and `RunVectorized` perform this check before scanning row data.

## MVCC: transactions, visibility, and the delta layer

`mvcc::TransactionManager` assigns transaction identifiers and commit timestamps from one shared monotonic counter.

When a transaction begins:

```text
snapshot_ts = last_commit_ts
```

When a transaction commits, it receives a new `commit_ts`.

A reader at `snapshot_ts` can see writes committed at or before that timestamp.

Visibility can therefore be evaluated with timestamp comparisons rather than traversing a transaction dependency graph.

CoreDB implements snapshot isolation. It does not implement serializable isolation or transaction-ID wraparound handling.

See [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for the tradeoffs.

## Delta layer

`mvcc::DeltaLayer` is an append-only:

```text
vector<DeltaRecord>
```

protected by a `shared_mutex`.

Each record stores:

- `row_id`
- `created_txn`
- `deleted_txn`
- the complete user-schema row

An empty row is used for tombstones.

### `created_txn == 0`

A `created_txn` value of `0` is reserved as a sentinel.

It means the record represents a tombstone for a pre-existing base row rather than a row originally created by a tracked transaction.

This distinction is required because rows already present in immutable base segments do not have transaction metadata recording their original creation.

## Updates and deletes

`Table::Update` and `Table::Delete` both call:

```text
DeltaLayer::ClaimOrInsertBaseTombstone
```

Under one lock, the function either:

1. claims the newest live delta-resident version of the requested `row_id`, or
2. inserts a tombstone claiming a base row that has never previously been modified in the delta layer

This provides write-write conflict detection.

If two transactions concurrently attempt to modify the same untouched base row, only one can successfully claim it. The other receives:

```text
WriteResult::kConflict
```

and must abort.

CoreDB does not queue writers or wait for row locks. It aborts on write-write conflict.

## Materializing a snapshot

`Table::MaterializeVisibleRows` merges immutable base data with the delta layer for a particular reader.

The function takes a snapshot of the delta records and determines visibility relative to the reader's transaction snapshot.

A base row is shadowed only when the delta contains a visible change for that specific reader.

That can be:

- a visible replacement version, or
- a visible tombstone

A delta record that exists physically but is not visible to the current reader does not suppress the base row.

This reader-relative shadowing is necessary for active snapshots to remain correct while newer writes and compaction occur.

The behavior is covered by:

```text
ActiveSnapshotStillSeesRowThroughACompactionCycle
```

## Compaction

CoreDB uses full-rewrite compaction rather than leveled compaction.

`Table::Compact()` performs the following steps.

### 1. Determine the compaction watermark

It reads:

```text
TransactionManager::OldestActiveSnapshot()
```

This produces the oldest snapshot timestamp still needed by an active reader.

Versions older than this watermark can be classified as stable when their transaction state is also resolved.

### 2. Classify delta records

For each delta record, compaction determines:

- whether its creation transaction committed
- whether its creation `commit_ts` is at or below the watermark
- whether its deletion transaction committed
- whether the deletion `commit_ts` is at or below the watermark
- whether either transaction aborted

Aborted records can be removed once they are no longer needed.

### 3. Rewrite the base

Compaction creates one new base segment from:

- existing base rows that have not been stably deleted
- delta rows that have become stably visible

The delta layer is reduced to records that are still required for active snapshots or unresolved transactions.

This is a full merge rather than incremental or leveled LSM compaction.

Its cost is therefore approximately:

```text
O(total base rows)
```

`bench_compaction` measures how that cost changes as the base grows.

## Background compaction

`compaction::BackgroundCompactor` runs in a separate thread.

When:

```text
delta_size() >= threshold
```

it calls:

```text
Table::Compact()
```

This matters because update and delete conflict detection currently performs a linear scan over the delta layer when locating versions for a `row_id`.

Without compaction, update-heavy workloads cause the delta to grow continuously, increasing the cost of subsequent updates.

`bench_mvcc_throughput` therefore enables background compaction for the update workload.

The pure-insert workload does not require it because inserts do not search for an existing row version before appending a new record.

## WAL

`wal::WalWriter` appends length-prefixed and checksummed records.

The record layout is:

```text
[u32 length]
[u32 crc32c]
[lsn]
[txn_id]
[type]
[payload]
```

`wal::ReadAll` parses records sequentially and stops when it encounters the first invalid boundary.

It distinguishes three cases:

### Clean EOF

No additional record exists.

This is not treated as an error.

### Truncated tail

A record header declares a length that extends past the end of the file.

This represents the expected shape of a crash during record append.

### Corrupt record

The complete record bytes exist, but the stored checksum does not match the computed checksum.

CoreDB stops at the first invalid record rather than attempting to skip forward and reinterpret later bytes as new records.

See [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for the reasoning behind this policy.

## Recovery

`recovery::RecoveryManager::Recover` restores the database in several phases.

### 1. Load checkpoint segments

If a checkpoint manifest exists, its base segments are loaded directly into:

```text
Table::base_segments_
```

This bypasses the normal transactional insert path.

### 2. Read committed WAL operations

The recovery process reads WAL records after:

```text
checkpoint.last_lsn
```

It keeps operations only for transactions that contain a valid `COMMIT` record.

Operations belonging to:

- aborted transactions
- transactions without a commit record
- transactions interrupted by a crash

are excluded from REDO.

This is REDO with implicit UNDO-by-omission rather than a complete ARIES implementation.

### 3. Parallel REDO

Committed operations are partitioned by:

```text
row_id % num_workers
```

Each partition is replayed on one worker thread while preserving operation order for a given row.

Within CoreDB's current data model, there are no cross-row constraints, secondary-index side effects, or transaction-level invariants that require different rows to be replayed together.

That allows recovery to partition work by `row_id` while preserving the ordering dependencies that exist within each row.

Each replayed operation is applied through a new:

```text
Begin → apply → Commit
```

sequence.

Recovery therefore reconstructs the committed final database state before the database becomes available to readers. It does not attempt to reproduce the original transaction timestamps or historical MVCC version graph.

See [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for the recovery tradeoffs.

### 4. Verify recovered segments

After replay completes, CoreDB runs:

```text
Segment::VerifyIntegrity()
```

over the resulting base segments.

This acts as a separate post-recovery integrity pass in addition to checksum verification performed when checkpoint segments are loaded.

## Recovery benchmark

`bench_recovery` supports configurable worker counts including:

```text
1
2
4
8
16
32
```

and configurable WAL sizes.

The benchmark measures actual recovery throughput and scaling behavior rather than assuming that additional workers improve performance.

See [BENCHMARKS.md](BENCHMARKS.md) for the measured results and the synchronization bottleneck identified by the benchmark.

## Execution engine

CoreDB provides three execution strategies for the same query.

All three:

- operate on the same immutable segment data
- use the same predicate and aggregate columns
- use the same segment-pruning rules
- return equivalent results

The difference is the inner execution model.

## Volcano execution

Implemented in:

```text
src/exec/volcano.cpp
```

The execution chain uses a row-at-a-time iterator model:

```text
SegmentScanOp
      ↓
FilterOp
```

Each row moves through virtual `Next()` calls.

This provides the row-at-a-time iterator baseline used for comparison with vectorized and JIT execution.

## Vectorized execution

Implemented in:

```text
src/exec/vectorized.cpp
```

Rows are processed in batches of:

```text
kDefaultBatchSize = 2048
```

Each batch is passed to:

```text
simd::FilterGtSum
```

The SIMD dispatch layer selects the best implementation supported by the current machine.

Possible implementations are:

- scalar
- ARM NEON
- AVX2
- AVX-512

## LLVM JIT execution

Implemented in:

```text
src/jit/query_jit.cpp
src/exec/jit_exec.cpp
```

The JIT path builds LLVM IR for a fused filter-and-sum loop.

The IR is generated once, optimized with LLVM's `-O3` pipeline, and compiled to native machine code using ORC `LLJIT`.

The optimization pipeline uses:

```text
PassBuilder::buildPerModuleDefaultPipeline
```

The loop is written using branchless selection rather than a conditional branch for each row.

Conceptually:

```text
predicate > threshold
        ↓
select(value, 0)
        ↓
accumulate
```

This structure gives LLVM's optimizer and auto-vectorizer a form that can be transformed efficiently into SIMD operations.

The benchmarked effect of the optimizer and branchless structure is documented in [BENCHMARKS.md](BENCHMARKS.md).

## SIMD kernels

CoreDB provides two SIMD-enabled operations:

```text
simd::Sum
simd::FilterGtSum
```

Each has several implementations.

### Scalar

```text
_Scalar
```

Always compiled and always available.

### ARM NEON

```text
_Neon
```

Compiled when targeting ARM64.

This is the hardware SIMD implementation used on Apple Silicon.

### AVX2

```text
_Avx2
```

Compiled for supported x86-64 builds.

The implementation uses function-level target attributes rather than requiring a global `-mavx2` compiler option.

### AVX-512

```text
_Avx512
```

Compiled for supported x86-64 builds and guarded by runtime capability detection.

The implementation uses target attributes including:

```text
avx512f
avx512bw
avx512dq
```

## Runtime SIMD dispatch

The public SIMD entry points are implemented through:

```text
src/simd/dispatch.cpp
```

At startup:

```text
util::DetectCapabilities()
```

detects the available instruction-set capabilities.

The dispatch layer then selects the best compatible kernel.

On ARM64, this can select NEON.

On supported x86-64 systems, it can select AVX2 or AVX-512.

Unsupported instruction paths are never executed.

## SIMD correctness

Tests under:

```text
tests/unit/test_simd_equivalence.cpp
```

compare every SIMD implementation available on the current machine against the scalar reference implementation.

The test sizes include values that do not divide evenly by the SIMD vector width so that scalar tail handling is exercised as well.

## Branchless filtering

Both the SIMD kernels and the JIT loop avoid a conditional branch for every input element.

The SIMD implementation creates a comparison mask and masks the aggregate contribution.

Conceptually:

```text
mask = predicate > threshold
value_to_add = value & mask
sum += value_to_add
```

The JIT path performs the equivalent operation through LLVM `select`.

This is especially useful for predicates whose outcomes are difficult for the CPU's branch predictor to predict consistently.

The measured scalar-versus-NEON and JIT-versus-Volcano results are documented in [BENCHMARKS.md](BENCHMARKS.md).

## Execution and MVCC boundary

The raw execution-engine benchmarks operate directly on immutable base segments.

They intentionally do not include:

- MVCC snapshot materialization
- delta merging
- transaction visibility checks
- compaction work

This isolates the cost of the execution strategies themselves.

Transactional scans through `Table` follow the MVCC visibility rules described earlier.

Keeping these paths separate makes it possible to benchmark:

```text
Volcano vs. vectorized vs. JIT
```

without mixing execution-engine performance with transactional bookkeeping.

## System boundary

CoreDB currently focuses on:

- immutable columnar segments
- MVCC snapshot isolation
- append-only delta versions
- full-rewrite compaction
- write-ahead logging
- checkpoint recovery
- parallel REDO
- SIMD execution
- LLVM JIT compilation
- correctness and crash-recovery testing
- reproducible systems benchmarks

It intentionally does not implement:

- SQL parsing
- a cost-based query optimizer
- general expression trees
- secondary indexes
- distributed execution
- replication
- serializable isolation
- cross-row constraints
- client/server networking

These boundaries keep the implementation focused on the storage, recovery, concurrency, and execution mechanisms that the project is designed to explore.

For additional implementation tradeoffs, see [DESIGN_DECISIONS.md](DESIGN_DECISIONS.md).

For measured performance and benchmark methodology, see [BENCHMARKS.md](BENCHMARKS.md).
