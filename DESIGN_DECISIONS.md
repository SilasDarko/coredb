# Design Decisions

This file is the "why", not the "what" — ARCHITECTURE.md covers structure.
Each entry names a trade-off CoreDB made deliberately, the alternative it
gave up, and why that was the right call *at this project's scope*. Several
of these were only discovered by writing benchmarks and sanitizer-checking
the result, not planned up front — the entry says so where that's the case.

## MVCC: a single shared counter for txn_id and commit_ts

**Decision:** `TransactionManager` hands out both transaction ids (at
`Begin`) and commit timestamps (at `Commit`) from the same monotonic
`uint64_t` counter, under one mutex.

**Alternative given up:** Real engines (Postgres, etc.) separate XID
allocation from commit ordering and handle XID wraparound, because a
32-bit XID space wraps on a busy server. CoreDB's `uint64_t` counter won't
realistically wrap in this project's lifetime, so that whole subsystem
(vacuum-driven wraparound prevention) doesn't need to exist.

**Cost measured, not assumed:** `bench_mvcc_throughput` shows both
`Begin()` and `Commit()` serializing on this one mutex — insert throughput
*drops* as thread count rises (single-thread beats 8-thread). This is the
most concrete, measured limitation in the whole project: a lock-free or
sharded timestamp oracle would fix it, and it's the first thing worth doing
if this became a real system. See BENCHMARKS.md.

## Write-write conflicts abort; they never block or queue

**Decision:** `DeltaLayer::ClaimOrInsertBaseTombstone` runs entirely under
one lock; if a different, still-active transaction already holds a claim on
`row_id`, the caller gets `kConflict` and must abort. There is no waiting,
no deadlock detection, no lock manager.

**Why:** A lock manager (wait-for graphs, deadlock detection, timeouts) is
a project in itself. Immediate-abort is strictly simpler, is what several
real optimistic-concurrency-control systems do under contention, and is
fully correct — it just means CoreDB pushes retry logic to the caller
instead of doing it internally. `tests/concurrency/test_aborts.cpp` covers
exactly this (`RacingUpdatesToSameRowExactlyOneWins`).

## No row_id index — Update/Delete are O(rows)

**Decision:** Finding whether a given `row_id` exists in the base layer
(`Table::RowIdExistsInBase`) is a linear scan across every base segment's
row_id column. `DeltaLayer`'s claim scan is a linear walk of the whole
delta vector.

**Why it's still here:** A B-tree or hash index over row_id is the obvious
next thing to build, but it interacts with MVCC (an index entry needs
transaction-aware visibility too, or it becomes another race), and getting
that right is real complexity. At this project's data scale, `O(rows)` is
honest and correct; the cost is visible and explained, not hidden.

**How it actually bit us:** the first version of `bench_mvcc_throughput`
ran its update-hotspot workload with *no* compactor. Delta grew unboundedly
for 1.5 seconds of pure updates, and because every `Update()` re-scans the
whole delta vector for `row_id`, per-op cost grew with total ops issued —
throughput visibly collapsed over the run (not a clean steady-state
number). The fix wasn't indexing row_id (out of scope) — it was running
`compaction::BackgroundCompactor` alongside the writers, which is how the
system is meant to be operated. See BENCHMARKS.md for the before/after.

## Compaction does a full rewrite every cycle

**Decision:** `Table::Compact()` rebuilds *one* consolidated base segment
from (existing base rows minus stably-deleted ones) plus (newly-stable
delta rows), every time it runs. It never does incremental/leveled merges
and never splits the base into multiple segments by size.

**Alternative given up:** A real LSM engine merges in levels (L0, L1, L2,
...) specifically so a compaction cycle never has to touch data that's
already settled. CoreDB's approach is O(total base rows) per compaction.

**Why:** it's dramatically simpler to implement, verify, and explain, and
it composes correctly with active-snapshot protection without needing
epoch-based reclamation. `bench_compaction` shows the cost this trade-off
has: compaction duration grows with base size as rounds accumulate. It also
explains why `bench_mvcc_throughput`'s *insert* workload deliberately runs
*without* a background compactor — inserts never need it (Insert doesn't
scan delta), and running a full-rewrite compactor against a purely-growing,
never-shrinking base is pure overhead. Discovering that (compacting made
the insert benchmark *slower*) is what led to splitting the two workloads'
compaction policy in the benchmark.

## Checkpoints assume a quiescent point

**Decision:** `recovery::WriteCheckpoint` calls `Table::Compact()` then
saves whatever base segments result, recording the WAL's current LSN as
`last_lsn`. This is only correct if no transaction is mid-flight when the
checkpoint runs — an in-progress transaction's already-logged operations
would sit at an LSN below `last_lsn` but not be reflected in the saved
segments (compaction only promotes *stable* rows), and recovery skips
everything at or below `last_lsn`.

**Alternative given up:** ARIES-style fuzzy checkpoints, which can be taken
concurrently with an active workload.

**Why:** correctly implementing a fuzzy checkpoint needs recovery to
understand "this row might be stale as of the checkpoint, re-verify against
the WAL tail," which is real complexity for a project at this scope.
Quiescent-point checkpoints are simple and fully correct under that one
assumption, which every test and benchmark here honors.

## REDO replay doesn't preserve original transaction boundaries

**Decision:** `RecoveryManager` replays each surviving WAL operation as its
own single-operation transaction (`Begin` → one op → `Commit`), rather than
grouping operations back into their original transaction.

**Why this is still correct:** durability only requires that a committed
transaction's *effects* survive a crash, not that recovery re-enacts the
exact same transaction shape. Only operations belonging to transactions
that have a `COMMIT` record in the log are replayed at all — an aborted or
crashed-mid-transaction transaction's operations are dropped in their
entirety (implicit UNDO by omission), so there's no scenario where a
partially-replayed original transaction leaks through. What's lost is
fidelity of `txn_id`/`commit_ts` values post-recovery, which nothing in
this system's contract promises to preserve.

**Why this is what makes replay parallelizable:** collapsing each op to its
own transaction is exactly what makes row_id-sharded parallel replay valid
— see the next entry.

## Parallel REDO is sharded by `row_id`, not by anything else

**Decision:** `RecoveryManager` buckets surviving operations by
`row_id % num_workers`; each worker thread replays its bucket sequentially,
in original LSN order.

**Why this is safe:** the only correctness constraint on replay order is
"a row's operations must apply in the order they were logged" (an insert
before its own update, etc.) — operations on *different* row_ids have no
ordering dependency in this table's model. Sharding by row_id preserves
per-row order (a row's ops all land in the same bucket, appended in scan
order) while letting unrelated rows replay fully in parallel.

**What the benchmark actually found:** `bench_recovery` shows *no* speedup
from extra worker threads on this machine — 2 workers barely help and 4+
actively regress (see BENCHMARKS.md for numbers). Root cause: each replayed
op still calls `Table::Begin()`/`Commit()`, which serialize on
`TransactionManager`'s single mutex — the same bottleneck
`bench_mvcc_throughput` found independently. Sharding the *data* correctly
doesn't help when the *apply path* itself has a global lock. This is
flagged, not hidden: fixing it means giving `RecoveryManager` a bulk-apply
path that bypasses per-op transaction bookkeeping (safe during recovery,
since there are no concurrent readers yet), which is the natural next step
if this project continued.

## WAL/segment corruption policy: stop, don't skip

**Decision:** Both `wal::ReadAll` and `Segment::LoadFromFile` stop at the
first truncated or corrupt record/checksum and discard everything after it,
rather than trying to skip past the damage and recover what they can
further on.

**Why:** once one record's integrity can't be verified, nothing after it in
a sequentially-dependent log can be trusted either (a corrupt length header
could make the reader mis-parse every subsequent record as garbage without
any of them *individually* failing a checksum). "Trust nothing after the
first bad record" is the conservative, provably-safe policy.

**Hardening found by fuzzing the length header in a test:** the first
version allocated a buffer of the *declared* record length before checking
it against the file's actual remaining size — a single corrupted 4-byte
length header could trigger a multi-gigabyte allocation attempt. Fixed by
checking the declared length against the file size (already known, cheap)
before allocating. Caught by `tests/crash_recovery/test_wal_corruption.cpp`
running abnormally slowly under AddressSanitizer (46s for one test, down to
under 1ms after the fix) — a case where a sanitizer surfaced a real
robustness bug via a performance anomaly, not a memory-safety error per se.

## Checksums cover data + validity + dictionary, not min/max

**Decision:** `Segment`'s per-column CRC32C covers the validity bitmap, the
typed data block, and (for strings) the dictionary — not the stored
`min`/`max` bounds.

**Why:** min/max are pruning hints; if one were corrupted, the worst case is
a segment that should have been prunable isn't (a correctness-preserving,
performance-only miss — `CanSkip` returning `false` when it "should" return
`true`), never a false skip that silently drops matching rows, because
`CanSkip` requires `min`/`max` to be present at all before it will ever
return `true`, and the check is otherwise pure comparison against real row
data that *is* checksummed. Covering `min`/`max` too is a reasonable
follow-up but wasn't necessary for correctness.

## Benchmark integrity: nothing here reports a number it didn't measure

Every figure in BENCHMARKS.md comes from actually running the benchmark
binary in this repository, on the machine BENCHMARKS.md names, on the date
it names. Where a number came out worse than expected (JIT initially
*slower* than a hand-written SIMD kernel; NEON initially *slower* than
auto-vectorized scalar code for plain `Sum`; MVCC throughput *degrading*
under more threads), the fix — or the decision not to chase a fix — is
described above and in BENCHMARKS.md rather than the inconvenient number
being quietly dropped.
