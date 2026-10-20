import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class SharedReleaseOrderingOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class DebitOwner {
        long local = 64;
        long aggregate = 0;
        int releaseAttempts;

        synchronized void release(long bytes) {
            if (bytes > local) throw new IllegalStateException("local underflow");
            releaseAttempts++;
            if (bytes > aggregate) throw new IllegalStateException("aggregate underflow");
            aggregate -= bytes;
            local -= bytes;
        }
    }

    private static final class BrokenDebitOwner {
        long local = 64;
        long aggregate = 0;

        synchronized void release(long bytes) {
            if (bytes > local) throw new IllegalStateException("local underflow");
            local -= bytes;
            if (bytes > aggregate) throw new IllegalStateException("aggregate underflow");
            aggregate -= bytes;
        }
    }

    private static final class SyncCellModel {
        int valueSize = 64;
        long reserved = 512;
        long aggregate = 0;

        synchronized void shrink(int newSize, long newReserved) {
            long refund = reserved - newReserved;
            if (refund < 0) throw new IllegalArgumentException("not a shrink");
            if (refund > aggregate) throw new IllegalStateException("aggregate underflow");
            aggregate -= refund;
            valueSize = newSize;
            reserved = newReserved;
        }
    }

    private static final class BrokenSyncCellModel {
        int valueSize = 64;
        long reserved = 512;
        long aggregate = 0;

        synchronized void shrink(int newSize, long newReserved) {
            long refund = reserved - newReserved;
            valueSize = newSize;
            if (refund > aggregate) throw new IllegalStateException("aggregate underflow");
            aggregate -= refund;
            reserved = newReserved;
        }
    }

    private static void debitRetryRound() {
        DebitOwner owner = new DebitOwner();
        try {
            owner.release(64);
            throw new AssertionError("expected aggregate underflow");
        } catch (IllegalStateException expected) {
            check(owner.local == 64, "failed refund lost local debit");
            check(owner.aggregate == 0, "failed refund mutated aggregate");
            check(owner.releaseAttempts == 1, "release attempt mismatch");
        }
        owner.aggregate = 64;
        owner.release(64);
        check(owner.local == 0, "retry did not consume local debit");
        check(owner.aggregate == 0, "retry did not consume aggregate debit");
        check(owner.releaseAttempts == 2, "retry count mismatch");
    }

    private static void syncCellFailureRound() {
        SyncCellModel cell = new SyncCellModel();
        try {
            cell.shrink(1, 64);
            throw new AssertionError("expected aggregate underflow");
        } catch (IllegalStateException expected) {
            check(cell.valueSize == 64, "failed shrink committed replacement value");
            check(cell.reserved == 512, "failed shrink changed local reservation");
            check(cell.aggregate == 0, "failed shrink changed aggregate");
        }
        cell.aggregate = 448;
        cell.shrink(1, 64);
        check(cell.valueSize == 1, "successful shrink did not commit value");
        check(cell.reserved == 64, "successful shrink did not publish reservation");
        check(cell.aggregate == 0, "successful shrink did not refund aggregate");
    }

    private static void raceRound(int workers) throws Exception {
        DebitOwner owner = new DebitOwner();
        owner.aggregate = 64;

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> unexpected = new AtomicReference<>();
        int[] successes = new int[workers];
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            final int slot = i;
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    owner.release(64);
                    successes[slot] = 1;
                } catch (IllegalStateException expected) {
                    // all but one contender must lose exact local ownership
                } catch (Throwable problem) {
                    unexpected.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        int won = 0;
        for (int success : successes) won += success;
        check(unexpected.get() == null, "unexpected race failure " + unexpected.get());
        check(won == 1, "concurrent exact debit had " + won + " winners");
        check(owner.local == 0, "concurrent release leaked local debit");
        check(owner.aggregate == 0, "concurrent release leaked aggregate debit");
    }

    private static void negativeControls() {
        BrokenDebitOwner debit = new BrokenDebitOwner();
        try {
            debit.release(64);
        } catch (IllegalStateException expected) {
            // expected
        }
        check(debit.local == 0, "negative debit control did not lose owner");
        check(debit.aggregate == 0, "negative debit aggregate mismatch");

        BrokenSyncCellModel cell = new BrokenSyncCellModel();
        try {
            cell.shrink(1, 64);
        } catch (IllegalStateException expected) {
            // expected
        }
        check(cell.valueSize == 1, "negative SyncCell control did not commit on failure");
        check(cell.reserved == 512, "negative SyncCell reservation unexpectedly changed");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;

        for (int i = 0; i < retryRounds; i++) {
            debitRetryRound();
            syncCellFailureRound();
        }
        negativeControls();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);

        System.out.println(
                "PASS source-free shared release ordering checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=local_owner_and_state_commit_detected"
                + " private_source_bytes=0");
    }
}
