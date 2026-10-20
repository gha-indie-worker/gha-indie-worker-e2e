import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class SharedCleanupRetryOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class RetryableOwner {
        boolean sealed;
        boolean cleanupComplete;
        long localDebit = 64;
        long aggregate = 64;
        boolean failFirst = true;
        int releaseAttempts;

        synchronized void close() {
            if (cleanupComplete) return;
            sealed = true;
            if (localDebit != 0) {
                releaseAttempts++;
                if (failFirst) {
                    failFirst = false;
                    aggregate = 0;
                }
                if (localDebit > aggregate) {
                    throw new IllegalStateException("aggregate underflow");
                }
                aggregate -= localDebit;
                localDebit = 0;
            }
            cleanupComplete = true;
        }

        synchronized void repairAggregate() {
            aggregate = localDebit;
        }
    }

    private static final class BrokenOwner {
        boolean sealed;
        long localDebit = 64;
        long aggregate = 0;

        synchronized void close() {
            if (sealed) return;
            sealed = true;
            long bytes = localDebit;
            localDebit = 0;
            if (bytes > aggregate) {
                throw new IllegalStateException("aggregate underflow");
            }
            aggregate -= bytes;
        }
    }

    private static void retryRound() {
        RetryableOwner owner = new RetryableOwner();
        try {
            owner.close();
            throw new AssertionError("expected underflow");
        } catch (IllegalStateException expected) {
            check(owner.sealed, "cleanup did not seal observation");
            check(!owner.cleanupComplete, "underflow forged cleanup completion");
            check(owner.localDebit == 64, "failed cleanup lost exact debit");
            check(owner.releaseAttempts == 1, "first release attempt mismatch");
        }

        owner.repairAggregate();
        owner.close();
        owner.close();

        check(owner.cleanupComplete, "retry did not complete cleanup");
        check(owner.localDebit == 0, "retry did not consume local debit");
        check(owner.aggregate == 0, "retry did not refund aggregate debit");
        check(owner.releaseAttempts == 2, "release did not retry exactly once");
    }

    private static void raceRound(int workers) throws Exception {
        RetryableOwner owner = new RetryableOwner();
        try {
            owner.close();
        } catch (IllegalStateException expected) {
            // expected
        }
        owner.repairAggregate();

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    owner.close();
                } catch (Throwable problem) {
                    failure.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(failure.get() == null, "concurrent retry raised " + failure.get());
        check(owner.cleanupComplete, "concurrent retry did not complete cleanup");
        check(owner.localDebit == 0, "concurrent retry leaked local debit");
        check(owner.aggregate == 0, "concurrent retry leaked aggregate debit");
        check(owner.releaseAttempts == 2, "concurrent retry duplicated release");
    }

    private static void negativeControl() {
        BrokenOwner owner = new BrokenOwner();
        try {
            owner.close();
        } catch (IllegalStateException expected) {
            // expected
        }
        owner.aggregate = 64;
        owner.close();
        check(owner.sealed, "negative control did not seal");
        check(owner.localDebit == 0, "negative control did not lose debit owner");
        check(owner.aggregate == 64, "negative control failed to strand aggregate debit");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;

        for (int i = 0; i < retryRounds; i++) retryRound();
        negativeControl();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);

        System.out.println(
                "PASS source-free shared cleanup retry checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=sealed_owner_lost_detected"
                + " private_source_bytes=0");
    }
}
