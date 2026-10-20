import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class RuntimeCloseAggregateProofOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class RuntimeModel {
        boolean closed;
        boolean resourcesInvalidated;
        boolean actorsTerminated = true;
        boolean closeComplete;
        boolean faultOnce = true;
        long sharedAggregate = 64;
        int resetCount;

        synchronized void close() {
            if (closeComplete) return;
            closed = true;
            resourcesInvalidated = true;

            if (faultOnce) {
                faultOnce = false;
                throw new OutOfMemoryError("pre-proof fault");
            }
            if (!actorsTerminated) {
                throw new IllegalStateException("physical actors still running");
            }

            sharedAggregate = 0;
            resetCount++;
            closeComplete = true;
        }
    }

    private static final class BrokenRuntimeModel {
        boolean closed;
        boolean closeComplete;
        boolean faultOnce = true;
        long sharedAggregate = 64;

        synchronized void close() {
            if (closeComplete) return;
            closed = true;
            sharedAggregate = 0;
            if (faultOnce) {
                faultOnce = false;
                throw new OutOfMemoryError("pre-proof fault");
            }
            closeComplete = true;
        }
    }

    private static void retryRound() {
        RuntimeModel runtime = new RuntimeModel();
        try {
            runtime.close();
            throw new AssertionError("expected pre-proof fault");
        } catch (OutOfMemoryError expected) {
            check(runtime.closed, "runtime close did not seal admission");
            check(runtime.resourcesInvalidated, "resource invalidation did not occur");
            check(!runtime.closeComplete, "fault forged runtime close completion");
            check(runtime.sharedAggregate == 64,
                    "aggregate reset occurred before physical proof");
            check(runtime.resetCount == 0, "reset count advanced before proof");
        }

        runtime.close();
        runtime.close();

        check(runtime.closeComplete, "retry did not complete close");
        check(runtime.sharedAggregate == 0, "successful close did not reset aggregate");
        check(runtime.resetCount == 1, "aggregate reset was not exactly once");
    }

    private static void raceRound(int workers) throws Exception {
        RuntimeModel runtime = new RuntimeModel();
        try {
            runtime.close();
        } catch (OutOfMemoryError expected) {
            // expected
        }

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    runtime.close();
                } catch (Throwable problem) {
                    failure.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(failure.get() == null, "concurrent close retry raised " + failure.get());
        check(runtime.closeComplete, "concurrent close retry did not finish");
        check(runtime.sharedAggregate == 0, "concurrent close retry did not reset aggregate");
        check(runtime.resetCount == 1, "concurrent close retry duplicated reset");
    }

    private static void negativeControl() {
        BrokenRuntimeModel runtime = new BrokenRuntimeModel();
        try {
            runtime.close();
        } catch (OutOfMemoryError expected) {
            // expected
        }
        check(runtime.closed, "negative control did not seal runtime");
        check(!runtime.closeComplete, "negative control unexpectedly completed");
        check(runtime.sharedAggregate == 0,
                "negative control failed to expose premature aggregate reset");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;

        for (int i = 0; i < retryRounds; i++) retryRound();
        negativeControl();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);

        System.out.println(
                "PASS source-free runtime close aggregate proof checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=premature_zero_detected"
                + " private_source_bytes=0");
    }
}
