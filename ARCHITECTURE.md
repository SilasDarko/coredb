# CoreDB Architecture

CoreDB is a single-process, embedded, columnar storage engine with MVCC,
crash recovery, and three interchangeable execution strategies (Volcano
iterator, vectorized batches, LLVM JIT). It is not a client/server database:
there is no network layer, no SQL parser, and no general expression
evaluator. Everything below is scoped narrowly on purpose — see
[DESIGN_DECISIONS.md](DESIGN_DECISIONS.md) for why.

```
                     ┌─────────────────────────┐
                     │        db::Database       │  Table + WAL, wired together
                     └───────────┬───────────────┘
                                 │
                 ┌───────────────┼────────────────┐
                 ▼                                 ▼
        ┌─────────────────┐              ┌──────────────────┐
        │   table::Table    │              │   wal::WalWriter   │
        │  (MVCC + delta)   │              │  (append-only log) │
        └────────┬─────────┘              └──────────────────┘
                 │  owns
     ┌───────────┼────────────┐
     ▼                        ▼
┌──────────┐          ┌─────────────────┐
│ DeltaLayer │          │ base_segments_   │  vector<shared_ptr<const Segment>>
│ (mutable)  │          │ (immutable)      │
└──────────┘          └─────────────────┘
     ▲                        ▲
     │     Table::Compact()   │
     └────────────────────────┘
          promotes/retires/rewrites

  Execution engine reads base_segments_ directly (bypassing MVCC) for the
  scan/filter/aggregate benchmarks:

  exec::RunVolcano | exec::RunVectorized | exec::RunJit
        │                 │                    │
        ▼                 ▼                    ▼
   virtual Next()   simd::FilterGtSum()   jit::QueryJit
   per row chain     (scalar/NEON/AVX2/     (LLVM ORC, IR built
                      AVX-512, runtime-      once, O3-optimized,
                      dispatched)            called per segment)
```

## Module map

| Directory | Responsibility |
|---|---|
| `util/` | CRC32C, validity bitmap, CPU feature detection |
| `storage/` | `Segment`/`Column`: immutable, columnar, on-disk-serializable, checksummed |
| `mvcc/` | `TransactionManager` (snapshot isolation) and `DeltaLayer` (append-only row versions) |
| `table/` | `Table`: ties storage + MVCC into Insert/Update/Delete/Scan and `Compact()` |
| `compaction/` | `BackgroundCompactor`: a thread that calls `Table::Compact()` on a threshold |
| `wal/` | Append-only, checksummed, length-prefixed log records |
| `recovery/` | Checkpoint manifest + REDO replay with configurable worker threads |
| `db/` | `Database`: Table + WAL wired together, runs recovery on open |
| `exec/` | Volcano, vectorized, and JIT execution over `Segment` columns |
| `simd/` | Scalar/NEON/AVX2/AVX-512 kernels + runtime dispatch |
| `jit/` | LLVM ORC JIT: builds IR, runs the optimizer, compiles to native code |

## The one query CoreDB executes

Every execution-engine benchmark and correctness test runs the same query
shape:

```
SELECT SUM(aggregate_column) WHERE predicate_column > threshold
```

over `int64` columns. This is deliberate: the point of the exercise is to
compare *execution strategies* (row-at-a-time vs. batched vs. JIT-compiled)
on identical work, not to build a query planner. A real system would put a
general expression tree and a cost-based optimizer in front of this; CoreDB
stops at the part that is actually interesting to benchmark. See
`include/coredb/exec/query.h`.

## Storage layer: `Segment` / `Column`

A `Segment` is immutable once built (`Segment::Build`) or loaded
(`Segment::LoadFromFile`) and holds, per column:

- a typed contiguous array (`int32_t[]`, `int64_t[]`, `double[]`) **or**, for
  strings, a dense `uint32_t[]` of dictionary codes plus a `Dictionary`
  (first-seen-order string interning);
- a `util::Bitmap` validity bitmap (1 bit/row);
- `min`/`max` (`storage::Value`) for segment pruning;
- a CRC32C checksum over (validity bitmap + data block + dictionary, if
  any), checked both when a file loads and again, independently, by
  `VerifyIntegrity()` after recovery.

Every table has a hidden leading column, `__row_id__` (int64), assigned by
`Table` and never exposed through the user-facing schema. It is what lets
`Update`/`Delete` address a specific logical row no matter which layer
(delta or base) currently holds its latest version — see
`table::kRowIdColumnIndex`.

Segment pruning (`Segment::CanSkip`) is a pure min/max check: a segment can
be skipped for a predicate only when the literal provably falls outside
`[min, max]`. Both `RunVolcano` and `RunVectorized` apply it before touching
row data.

## MVCC: transactions, visibility, the delta layer

`mvcc::TransactionManager` hands out transaction ids and commit
timestamps from **one shared monotonic counter** — `Begin()` records
`snapshot_ts = last_commit_ts`, `Commit()` assigns `commit_ts` from the same
counter. A reader at `snapshot_ts` sees exactly the writes committed at or
before that value; visibility (`mvcc::IsVisible`) is therefore a plain
integer comparison, not a full transaction-graph walk. See
DESIGN_DECISIONS.md for why this is a deliberate simplification (no MVCC
wraparound handling, no serializable isolation — this is snapshot
isolation).

`mvcc::DeltaLayer` is an append-only `vector<DeltaRecord>` guarded by a
`shared_mutex`. Each record carries:

- `row_id` — the logical row it belongs to;
- `created_txn` / `deleted_txn` — 0 means "not applicable"; `created_txn ==
  0` is a sentinel meaning "this is a tombstone for a pre-existing base
  row", since base rows were never "created" by a tracked transaction;
- `row` — the full user-schema row (empty for tombstones).

`Table::Update`/`Table::Delete` both go through one method,
`DeltaLayer::ClaimOrInsertBaseTombstone`, which — under a single lock —
either claims the newest live delta-resident version of `row_id`, or, if
`row_id` has never been touched by delta before, inserts a tombstone
claiming the base row. Two concurrent transactions racing to touch the same
untouched row can therefore never both succeed: the loser sees
`WriteResult::kConflict` and must abort. There is no lock-waiting/queueing —
**CoreDB aborts on write-write conflict** rather than blocking.

`Table::MaterializeVisibleRows` merges base + delta for one reader: it walks
the delta snapshot once, and a base row is only shadowed when the delta
actually has something *that specific reader can see* for its `row_id`
(either a visible non-tombstone record, or a tombstone whose delete is
visible). This reader-relative shadowing is what makes active-snapshot
protection actually correct — see the `ActiveSnapshotStillSeesRowThroughACompactionCycle`
test and the compaction section below.

## Compaction: full-rewrite, not leveled

`Table::Compact()`:

1. Computes `oldest_active = TransactionManager::OldestActiveSnapshot()` —
   the compaction watermark below which every reader has already stopped
   caring, so it's always safe to advance.
2. Classifies every delta record: is its creation "stable" (committed with
   `commit_ts <= oldest_active`)? Is its deletion (if any) also stable? Was
   its creator/deleter aborted (safe to drop unconditionally)?
3. Rebuilds **one** new base segment from (all existing base rows minus
   stably-deleted row_ids) plus (delta rows that are now stably live), and
   trims the delta layer to just what's still undecided.

This is a full merge every cycle, not incremental/leveled LSM compaction —
`Table::Compact()` is O(total base rows), and `bench_compaction` shows that
cost growing linearly with base size as batches accumulate. That trade-off
is deliberate at this project's scale; see DESIGN_DECISIONS.md.

`compaction::BackgroundCompactor` is a thread that calls `Table::Compact()`
whenever `delta_size() >= threshold`. It exists because `Update`/`Delete`'s
claim logic does a **linear scan** of the delta layer looking for `row_id`
(no index) — letting delta grow unboundedly under an update-heavy workload
makes every subsequent update slower. `bench_mvcc_throughput`'s update
workload runs one; its insert workload deliberately doesn't (inserts never
scan delta, so compacting a pure-insert workload is pure overhead — see that
benchmark's comments).

## WAL and recovery

`wal::WalWriter` appends length-prefixed, checksummed records
(`[u32 len][u32 crc32c][lsn][txn_id][type][payload]`); `wal::ReadAll` parses
them back and stops at the first sign of damage — see
`DESIGN_DECISIONS.md` for why "stop, don't skip" is the policy — distinguishing:

- **clean EOF** (nothing more to read, not an error),
- **truncated tail** (a record's declared length runs past EOF — the
  classic "crashed mid-`fwrite`" shape), and
- **corrupt record** (full-length bytes present, checksum doesn't match).

`recovery::RecoveryManager::Recover`:

1. Loads a checkpoint's segments (if a manifest exists) directly into
   `Table::base_segments_`, bypassing the normal insert path entirely.
2. Reads the WAL, keeps only records with `lsn > checkpoint.last_lsn` whose
   transaction has a `COMMIT` record (an aborted or never-resolved — i.e.
   crashed mid-transaction — txn's operations are dropped: this is REDO
   with implicit UNDO-by-omission, not full ARIES).
3. Shards the surviving operations by `row_id % num_workers` and replays
   each shard on its own thread, each operation as its own
   Begin→apply→Commit. Sharding by `row_id` is safe because a given row's
   operations only ever depend on earlier operations *on that same row* —
   see DESIGN_DECISIONS.md for why this does not preserve original
   transaction boundaries, and why that's fine for durability.
4. Re-verifies every resulting base segment's checksum
   (`Segment::VerifyIntegrity`) as an explicit post-recovery integrity pass,
   independent of the checksum check `LoadFromFile` already did.

`bench_recovery` measures this at 1/2/4/8/16/32 workers; see BENCHMARKS.md
for what that scaling curve actually looks like on this machine and why.

## Execution engine: Volcano vs. vectorized vs. JIT

All three (`exec::RunVolcano`, `exec::RunVectorized`, `exec::RunJit`) prune
segments the same way and read the same raw `int64_t*` column arrays; they
differ only in the inner loop:

- **Volcano** (`src/exec/volcano.cpp`): a textbook iterator-model chain —
  `SegmentScanOp` → `FilterOp`, each a virtual `Next()` call per row. This
  is the deliberately slow baseline.
- **Vectorized** (`src/exec/vectorized.cpp`): batches of `kDefaultBatchSize`
  (2048) rows per call into `simd::FilterGtSum`, which dispatches to the
  best kernel tier this process detected at startup.
- **JIT** (`src/jit/query_jit.cpp`, `src/exec/jit_exec.cpp`): builds LLVM IR
  for the fused filter+sum loop **once**, runs LLVM's real `-O3` pipeline
  over it (`PassBuilder::buildPerModuleDefaultPipeline`), and JIT-compiles
  it via ORC's `LLJIT`. The loop body is written *branchless* (`select`
  instead of a conditional branch) specifically so LLVM's auto-vectorizer
  can turn it into SIMD instructions — see BENCHMARKS.md for the measured
  effect that one change had, and `src/jit/query_jit.cpp`'s comments for
  the reasoning.

## SIMD kernels

`simd::Sum` and `simd::FilterGtSum` each have four implementations —
`_Scalar` (always compiled and runnable), `_Neon` (compiled only when
targeting arm64), `_Avx2`/`_Avx512` (compiled only when targeting x86_64,
gated further by `__attribute__((target("avx2")))` /
`("avx512f,avx512bw,avx512dq")` so the file doesn't require a global
`-mavx2` build flag) — behind one dispatch function
(`src/simd/dispatch.cpp`) that picks the best tier `util::DetectCapabilities()`
found at process startup. `tests/unit/test_simd_equivalence.cpp` checks
every tier this process can actually run against the scalar reference,
across sizes chosen to exercise each kernel's scalar tail-loop.

Every SIMD kernel and the JIT loop use the **same branchless-predication
trick**: compute a 0/all-ones (or `select`) mask from the comparison and
mask-add, rather than branching per element. This is not an aesthetic
choice — see BENCHMARKS.md's `FilterGtSum` scalar-vs-NEON numbers for why
it matters on data with an unpredictable branch.
