#include "coredb/mvcc/delta.h"
#include "coredb/mvcc/transaction.h"

#include <gtest/gtest.h>

using namespace coredb::mvcc;

TEST(MvccVisibility, OwnUncommittedWriteIsVisibleToSelfOnly) {
  TransactionManager txm;
  Transaction writer = txm.Begin();
  Transaction other = txm.Begin();  // concurrent, also hasn't seen writer's insert

  DeltaRecord rec{/*row_id=*/1, writer.txn_id, /*deleted_txn=*/0, {}, false};

  EXPECT_TRUE(IsVisible(rec, writer, txm));
  EXPECT_FALSE(IsVisible(rec, other, txm));
}

TEST(MvccVisibility, CommittedWriteVisibleOnlyToLaterSnapshots) {
  TransactionManager txm;
  Transaction early_reader = txm.Begin();  // snapshot taken before the write commits

  Transaction writer = txm.Begin();
  DeltaRecord rec{1, writer.txn_id, 0, {}, false};
  txm.Commit(writer);

  Transaction late_reader = txm.Begin();  // snapshot taken after the commit

  EXPECT_FALSE(IsVisible(rec, early_reader, txm)) << "predates the commit; must not see it";
  EXPECT_TRUE(IsVisible(rec, late_reader, txm)) << "begins after the commit; must see it";
}

TEST(MvccVisibility, AbortedWriteIsNeverVisibleToOthers) {
  TransactionManager txm;
  Transaction writer = txm.Begin();
  DeltaRecord rec{1, writer.txn_id, 0, {}, false};
  txm.Abort(writer);

  Transaction reader = txm.Begin();
  EXPECT_FALSE(IsVisible(rec, reader, txm));
}

TEST(MvccVisibility, DeleteHidesRowOnlyFromSnapshotsAfterTheDelete) {
  TransactionManager txm;
  Transaction creator = txm.Begin();
  DeltaRecord rec{1, creator.txn_id, 0, {}, false};
  txm.Commit(creator);

  Transaction reader_before_delete = txm.Begin();

  Transaction deleter = txm.Begin();
  rec.deleted_txn = deleter.txn_id;
  txm.Commit(deleter);

  Transaction reader_after_delete = txm.Begin();

  EXPECT_TRUE(IsVisible(rec, reader_before_delete, txm))
      << "snapshot predates the delete's commit, so the old value must still be visible";
  EXPECT_FALSE(IsVisible(rec, reader_after_delete, txm));
}

TEST(MvccVisibility, OldestActiveSnapshotTracksRunningReaders) {
  TransactionManager txm;
  Transaction t1 = txm.Begin();
  Transaction t2 = txm.Begin();
  EXPECT_EQ(txm.OldestActiveSnapshot(), t1.snapshot_ts);

  txm.Commit(t1);
  EXPECT_EQ(txm.OldestActiveSnapshot(), t2.snapshot_ts);

  txm.Commit(t2);
  // No active readers left: watermark advances to the latest commit.
  EXPECT_GE(txm.OldestActiveSnapshot(), t2.commit_ts);
}
