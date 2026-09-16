# Design Decisions

This document explains the major tradeoffs behind CoreDB's implementation.

[ARCHITECTURE.md](ARCHITECTURE.md) describes how the system is structured. This file focuses on why particular storage, MVCC, recovery, and execution choices were made, along with the limitations those choices introduce.

## MVCC uses one shared counter for transaction IDs and commit timestamps

### Decision

`TransactionManager` assigns both:

- transaction IDs during `Begin()`
- commit timestamps during `Commit()`

from one monotonic `uint64_t` counter protected by a process-wide mutex.

### Why

Using one ordering source keeps snapshot visibility simple.

A transaction records:

```text
snapshot_ts = last_commit_ts
```

when it begins.

A committed version is visible when its commit timestamp is at or before the reader's snapshot.

This avoids needing a more complex transaction-ordering structure.

### Tradeoff

The shared mutex creates a global synchronization point.

`bench_mvcc_throughput` shows that throughput decreases as additional worker threads are added, even for the insert workload where rows themselves do not contend.

The same synchronization bottleneck also appears during parallel recovery.

A future implementation could explore:

- atomic timestamp allocation
- sharded transaction state
- reduced critical sections
- lock-free snapshot tracking

Any replacement would need to preserve the current snapshot-isolation semantics under concurrency.

See [BENCHMARKS.md](BENCHMARKS.md).

---

## Write-write conflicts abort instead of blocking

### Decision

`DeltaLayer::ClaimOrInsertBaseTombstone` performs row claiming while holding one lock.

If another active transaction already owns the relevant row version, the caller receives:

```text
WriteResult::kConflict
```

and must abort.

CoreDB does not maintain:

- lock wait queues
- a lock manager
- deadlock detection
- lock timeouts

### Why

Immediate conflict failure keeps the write path deterministic and substantially simpler than introducing blocking lock management.

The caller is responsible for deciding whether and when to retry.

### Consequence

Under high contention, transactions may abort frequently instead of waiting for the current writer to finish.

This behavior is exercised by:

```text
tests/concurrency/test_aborts.cpp
```

including the case where concurrent updates to the same row allow only one transaction to succeed.

---

## `row_id` lookup is currently linear

### Decision

CoreDB does not maintain a dedicated index over logical row IDs.

`Table::RowIdExistsInBase` scans the row-ID column of the base segments.

The delta layer also performs a linear scan when locating versions for a `row_id`.

### Why

Adding a hash table or tree is straightforward for physical lookup, but an index used by MVCC must remain consistent with:

- version visibility
- updates
- deletes
- compaction
- active snapshots

The current implementation keeps the storage model simpler by using the authoritative base and delta structures directly.

### Tradeoff

Lookup cost grows with the number of rows or delta records.

This is particularly visible in update-heavy workloads because each update may scan an increasingly large delta vector.

`bench_mvcc_throughput` therefore runs a background compactor for the update workload so the delta does not grow without bound.

A future implementation could introduce a MVCC-aware `row_id` index to reduce lookup cost.

---

## Compaction rewrites the full base

### Decision

`Table::Compact()` rebuilds one consolidated base representation from:

- existing base rows that remain live
- delta rows that have become stable
- deletion state that is safe to apply

It does not use leveled or incremental compaction.

### Why

A full rewrite keeps the merge logic straightforward and makes the interaction with active snapshots easier to reason about.

The compactor only promotes or removes versions once they are safe relative to:

```text
TransactionManager::OldestActiveSnapshot()
```

This avoids needing a more complex reclamation mechanism for multiple overlapping storage levels.

### Tradeoff

Compaction cost grows with the size of the base.

`bench_compaction` demonstrates this directly: later rounds take longer as the base accumulates more rows.

The current strategy is therefore simple and correct, but not appropriate for very large continuously growing datasets.

Future options include:

- leveled compaction
- segmented base storage
- incremental merge policies
- epoch-based reclamation

---

## Insert and update workloads use different compaction policies

### Decision

The insert benchmark does not run background compaction.

The update benchmark does.

### Why

`Table::Insert` appends a new delta record and does not need to search for an existing logical row.

Running full-rewrite compaction during a pure insert workload therefore adds work without reducing the cost of the insert operation.

Updates behave differently.

`Table::Update` must locate the current version of a `row_id`, and the current delta implementation performs a linear scan.

Without compaction, an update-heavy workload causes the delta vector to grow continuously, increasing the cost of later updates.

The benchmark policies reflect those different execution paths rather than forcing the same compaction behavior onto both workloads.

---

## Checkpoints require a quiescent point

### Decision

`recovery::WriteCheckpoint` first runs:

```text
Table::Compact()
```

then persists the resulting base segments and records the current WAL LSN as:

```text
last_lsn
```

The checkpoint mechanism assumes there are no transactions mid-flight while the checkpoint is created.

### Why

If a transaction were active during checkpoint creation, some of its WAL records could appear before `last_lsn` while its changes were not yet represented in the compacted base.

Recovery would then skip those WAL records because they are at or before the checkpoint LSN.

A quiescent checkpoint avoids that ambiguity.

### Alternative

A fuzzy checkpoint design could allow checkpointing concurrently with active transactions.

That would require the recovery process to track additional state describing which pages or logical rows may still need to be reconstructed from the WAL.

CoreDB does not currently implement that machinery.

---

## Recovery reconstructs final committed state, not historical transaction identity

### Decision

`RecoveryManager` identifies WAL operations belonging to transactions with valid `COMMIT` records.

Each surviving operation is then replayed through a new:

```text
Begin → apply → Commit
```

sequence.

The original transaction boundaries, transaction IDs, and commit timestamps are not recreated.

### Why

CoreDB's recovery contract is to restore the committed database state before the database becomes available to readers.

The system does not expose historical transaction identifiers or commit timestamps as persistent user-visible state.

Operations from transactions that:

- aborted
- never committed
- were interrupted by a crash

are excluded from REDO entirely.

This reconstructs the committed logical state while avoiding the need to reproduce the original MVCC history.

### Tradeoff

Recovered MVCC metadata is not identical to the metadata that existed before the crash.

If CoreDB later needed historical transaction identity, temporal queries, or externally visible commit timestamps, this recovery model would need to change.

---

## Parallel REDO is partitioned by `row_id`

### Decision

Committed recovery operations are assigned to workers using:

```text
row_id % num_workers
```

Each worker processes its own bucket in original LSN order.

### Why

Within the current CoreDB data model:

- there are no secondary-index side effects
- there are no cross-row constraints
- there are no foreign-key relationships
- operations on one row do not depend on the state of another row

The important ordering constraint is therefore that operations for the same `row_id` remain ordered.

Partitioning by `row_id` guarantees that all operations for a given row are assigned to the same worker.

### Limitation

The partitioning strategy is logically parallel, but the current apply path still calls:

```text
Table::Begin()
Table::Commit()
```

for each operation.

Those methods serialize through the global `TransactionManager` mutex.

As a result, `bench_recovery` shows that additional recovery workers do not produce useful scaling on the current implementation.

A recovery-specific bulk-apply path could avoid normal per-operation transaction bookkeeping because recovery runs before concurrent readers are admitted.

---

## WAL corruption handling stops at the first invalid record

### Decision

`wal::ReadAll` stops when it encounters:

- a truncated record
- an invalid length
- a checksum failure

It does not attempt to skip damaged bytes and continue parsing later records.

### Why

The WAL is a sequential length-prefixed stream.

Once a record boundary can no longer be trusted, the reader cannot safely determine where the next valid record begins.

For example, corruption in a length field could cause all subsequent bytes to be interpreted at incorrect offsets.

Stopping at the first invalid record avoids treating arbitrary later bytes as valid WAL entries.

### Truncated tail

A partially written final record is treated separately from a checksum mismatch.

A truncated tail is consistent with a crash during record append.

Records before the truncated boundary remain valid and recoverable.

---

## WAL record lengths are validated before allocation

### Decision

The WAL reader verifies that a declared record length is plausible relative to the remaining file size before allocating a buffer for the record.

### Why

A corrupted length header could otherwise contain an extremely large integer.

Allocating directly from that value could cause excessive memory allocation even though the WAL file itself contains only a small amount of remaining data.

The reader therefore validates the declared size first.

### How this was discovered

A corruption test became unexpectedly slow under AddressSanitizer because an invalid length field triggered an excessive allocation attempt.

The implementation was changed so record bounds are checked before allocation.

The behavior is covered by:

```text
tests/crash_recovery/test_wal_corruption.cpp
```

---

## Segment checksums cover row data, validity, and string dictionaries

### Decision

The per-column CRC32C currently covers:

- the validity bitmap
- the typed data block
- the string dictionary where applicable

The stored `min` and `max` metadata are not currently included in the checksum.

### Why

The checksum was initially focused on the data required to reconstruct column values.

However, `min` and `max` values also influence segment pruning.

Because pruning may skip a segment based on these values, corruption of this metadata could affect query correctness rather than only performance.

### Consequence

The current checksum does not provide integrity protection for pruning metadata.

A future format revision should include `min` and `max` in the checksummed representation, or recompute them from verified column data when a segment is loaded.

Until then, the format assumes pruning metadata itself has not been corrupted independently of the checksummed column contents.

---

## Segment pruning remains conservative for valid metadata

### Decision

For the supported predicate:

```text
column > threshold
```

CoreDB skips a segment only when:

```text
segment_max <= threshold
```

### Why

If the maximum value in a valid segment is at or below the threshold, no row in that segment can satisfy the predicate.

If the metadata is absent, CoreDB does not prune the segment.

This biases the implementation toward scanning additional data rather than skipping a segment without enough information.

The integrity limitation around corrupted `min` / `max` metadata is documented separately above.

---

## Execution benchmarks bypass MVCC

### Decision

The Volcano, vectorized, SIMD, and JIT execution benchmarks operate directly on immutable base segments.

They do not include:

- snapshot materialization
- delta merging
- visibility checks
- transaction bookkeeping
- compaction

### Why

The purpose of those benchmarks is to compare execution strategies on the same raw query workload.

Including MVCC work would mix storage-engine costs with execution-engine costs and make the comparison harder to interpret.

Transactional scans through `Table` still use the normal MVCC path.

The benchmark separation therefore measures:

```text
Volcano vs. vectorized vs. JIT
```

independently from transactional overhead.

---

## The execution engine supports one query shape

### Decision

The execution engine implements:

```sql
SELECT SUM(aggregate_column)
WHERE predicate_column > threshold;
```

over `int64` columns.

### Why

The project focuses on comparing execution strategies rather than building a full query-processing stack.

Supporting one fixed query shape makes it possible to compare:

- row-at-a-time iteration
- vectorized execution
- explicit SIMD
- LLVM-generated native code

on identical work.

### Alternative

A general engine would require additional layers such as:

- expression trees
- type coercion
- query planning
- cost estimation
- multiple operators
- joins
- general aggregation

Those components are outside the current storage-and-execution focus.

---

## SIMD dispatch happens at runtime

### Decision

CoreDB contains separate implementations for:

- scalar execution
- ARM NEON
- AVX2
- AVX-512

The dispatch layer selects the best supported implementation based on runtime CPU capability detection.

### Why

This allows one codebase to support multiple processor architectures without requiring every machine to implement the same instruction set.

Unsupported paths are never executed.

On Apple Silicon, the dispatch layer selects NEON.

On supported x86-64 hosts, AVX2 or AVX-512 may be selected depending on available features.

### Correctness

SIMD implementations are tested against the scalar reference for the tiers available on the current host.

The tests include input sizes that exercise scalar tail handling when the row count is not a multiple of the vector width.

See:

```text
tests/unit/test_simd_equivalence.cpp
```

---

## The JIT runs LLVM optimization passes before compilation

### Decision

Generated LLVM IR is passed through an `-O3` optimization pipeline before being handed to ORC `LLJIT`.

CoreDB uses:

```text
PassBuilder::buildPerModuleDefaultPipeline
```

### Why

Generating native machine code does not by itself produce an optimized execution path.

The initial implementation passed relatively direct scalar IR to LLJIT and produced only a small improvement over Volcano execution.

Running the standard optimization pipeline substantially improved the generated code.

The benchmark history is documented in [BENCHMARKS.md](BENCHMARKS.md).

---

## The JIT filter loop is branchless

### Decision

The generated filter-and-sum loop uses LLVM `select` instead of a per-row conditional branch.

Conceptually:

```text
selected = predicate > threshold ? value : 0
sum += selected
```

### Why

The benchmark predicate has approximately 50% selectivity.

A branch with a nearly unpredictable outcome performs poorly because the CPU cannot consistently predict which path will be taken.

The branchless form also gives LLVM's loop vectorizer a simpler representation to transform into SIMD instructions.

This change reduced the measured JIT runtime substantially and allowed the generated code to approach the handwritten vectorized path.

See [BENCHMARKS.md](BENCHMARKS.md).

---

## Background compaction uses polling

### Decision

`BackgroundCompactor` periodically checks:

```text
delta_size() >= threshold
```

and triggers compaction when the threshold is reached.

### Why

A polling implementation keeps the compactor independent from every individual write operation.

The current design avoids adding notification or scheduling state to the transaction path.

### Tradeoff

Polling introduces a delay between crossing the threshold and starting compaction.

For the current workloads, that delay is acceptable.

A larger system could instead use:

- condition variables
- work queues
- adaptive thresholds
- write-rate-aware scheduling

---

## Recovery verifies segments after replay

### Decision

After recovery completes, CoreDB runs:

```text
Segment::VerifyIntegrity()
```

on recovered base segments.

### Why

Checkpoint loading already validates segment checksums, but recovery also mutates the logical database state by applying WAL records.

A final integrity pass provides an explicit verification boundary before the recovered database is considered ready.

This check is separate from WAL checksum validation.

---

## Snapshot isolation is the transaction boundary

### Decision

CoreDB implements snapshot isolation rather than serializable isolation.

A transaction reads from a snapshot determined at `Begin()` and commits its writes if no write-write conflict prevents it.

### Why

Snapshot isolation provides a meaningful MVCC model without introducing:

- predicate locking
- serializable snapshot isolation
- read-write dependency tracking
- full conflict-graph validation

### Limitation

Snapshot isolation does not prevent every anomaly that serializable execution would prevent.

CoreDB's transaction guarantees should therefore be understood specifically as snapshot isolation with write-write conflict detection.

---

## Design priorities

The implementation generally favors:

- explicit invariants
- deterministic behavior
- conservative recovery
- inspectable storage state
- independently testable execution paths
- correctness before optimistic parallelism
- measured performance rather than assumed scaling

Those priorities explain several of the current tradeoffs:

- global transaction synchronization instead of a more complex timestamp oracle
- full-rewrite compaction instead of a leveled LSM
- quiescent checkpoints instead of fuzzy checkpoints
- recovery by committed-state reconstruction instead of full ARIES
- one query shape instead of a complete planner
- direct runtime SIMD dispatch instead of architecture-specific builds

The resulting implementation is intentionally narrow, but the boundaries are explicit and the performance consequences of those choices are measured in [BENCHMARKS.md](BENCHMARKS.md).
