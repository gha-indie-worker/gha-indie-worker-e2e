import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class ActorGroupCloseRetryOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class GroupCloseModel {
        boolean closed;
        boolean closeActive;
        boolean closeComplete;
        boolean ownerRetained;
        boolean observersInstalled;
        boolean workerAlive = true;
        boolean mailmanOwned = true;
        boolean cancellationFaultOnce = true;
        int cancelAttempts;
        int completedTransitions;

        synchronized void close() {
            if (closeComplete) return;
            if (closeActive) throw new IllegalStateException("reentrant close");
            closeActive = true;
            try {
                if (!closed) {
                    ownerRetained = true;
                    closed = true;
                }
                if (!ownerRetained) {
                    throw new IllegalStateException("lost close owner");
                }
                observersInstalled = true;
                if (workerAlive) {
                    cancelAttempts++;
                    if (cancellationFaultOnce) {
                        cancellationFaultOnce = false;
                        throw new IllegalStateException("live cancellation failed");
                    }
                    workerAlive = false;
                }
                mailmanOwned = false;
                closeComplete = true;
                ownerRetained = false;
                completedTransitions++;
            } finally {
                closeActive = false;
            }
        }
    }

    private static final class BrokenGroupCloseModel {
        boolean closed;
        boolean workerAlive = true;
        boolean cancellationFaultOnce = true;

        synchronized void close() {
            if (closed) return;
            closed = true;
            if (workerAlive && cancellationFaultOnce) {
                cancellationFaultOnce = false;
                throw new IllegalStateException("live cancellation failed");
            }
            workerAlive = false;
        }
    }

    private static void retryRound() {
        GroupCloseModel model = new GroupCloseModel();
        try {
            model.close();
            throw new AssertionError("expected cancellation failure");
        } catch (IllegalStateException expected) {
            check(model.closed, "admission seal must remain visible");
            check(!model.closeActive, "failed close leaked active owner");
            check(!model.closeComplete, "failed cancellation forged completion");
            check(model.ownerRetained, "failed close discarded durable owner");
            check(model.observersInstalled, "observer ownership was not retained");
            check(model.workerAlive, "negative cancellation unexpectedly killed worker");
            check(model.mailmanOwned, "later cleanup owner was discarded");
        }

        model.close();
        model.close();
        check(model.closeComplete, "retry did not complete close");
        check(!model.ownerRetained, "completed close leaked owner");
        check(!model.workerAlive, "retry did not cancel worker");
        check(!model.mailmanOwned, "retry did not release mailman owner");
        check(model.cancelAttempts == 2, "worker cancellation did not retry exactly once");
        check(model.completedTransitions == 1, "physical close witness duplicated");
    }

    private static void raceRound(int workers) throws Exception {
        GroupCloseModel model = new GroupCloseModel();
        try {
            model.close();
        } catch (IllegalStateException expected) {
            // expected first cancellation failure
        }

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    model.close();
                } catch (Throwable problem) {
                    failure.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(failure.get() == null, "concurrent retry raised " + failure.get());
        check(model.closeComplete, "concurrent retries did not finish close");
        check(model.completedTransitions == 1, "concurrent retries duplicated witness");
        check(model.cancelAttempts == 2, "concurrent retries duplicated cancellation");
        check(!model.ownerRetained, "concurrent retries leaked durable owner");
    }

    private static void brokenNegativeControl() {
        BrokenGroupCloseModel model = new BrokenGroupCloseModel();
        try {
            model.close();
        } catch (IllegalStateException expected) {
            // expected
        }
        model.close();
        check(model.closed, "negative control did not seal");
        check(model.workerAlive,
                "negative control failed to demonstrate stranded live worker");
    }

    public static void main(String[] args) throws Exception {
        final int retryRounds = 20_000;
        final int raceRounds = 5_000;
        final int workers = 12;
        for (int i = 0; i < retryRounds; i++) retryRound();
        brokenNegativeControl();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);
        System.out.println(
                "PASS source-free ActorGroup close retry checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=stranded_live_worker_detected"
                + " private_source_bytes=0");
    }
}
