import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public final class TaskRetirementRetryOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) throw new AssertionError(message);
    }

    private static final class RetirementOwner {
        private final AtomicInteger attempts;
        private final int failThrough;
        private final CountDownLatch physicalExit = new CountDownLatch(1);
        private boolean exitObserved;
        private boolean releaseComplete;

        RetirementOwner(AtomicInteger attempts, int failThrough) {
            this.attempts = attempts;
            this.failThrough = failThrough;
        }

        void onPhysicalExit() {
            synchronized (this) {
                if (!exitObserved) {
                    exitObserved = true;
                    physicalExit.countDown();
                }
            }
            try {
                retry();
            } catch (IllegalStateException ignored) {
                // Exact owner remains retained for host retry.
            }
        }

        void awaitExitAndRetry() {
            boolean interrupted = false;
            try {
                for (;;) {
                    try {
                        physicalExit.await();
                        break;
                    } catch (InterruptedException ignored) {
                        interrupted = true;
                    }
                }
            } finally {
                if (interrupted) Thread.currentThread().interrupt();
            }
            retry();
        }

        private synchronized void retry() {
            if (releaseComplete) return;
            if (!exitObserved) throw new IllegalStateException("pre-exit retirement");
            int attempt = attempts.incrementAndGet();
            if (attempt <= failThrough) {
                throw new IllegalStateException("injected retirement failure " + attempt);
            }
            releaseComplete = true;
        }

        synchronized boolean complete() {
            return releaseComplete;
        }
    }

    private static final class RetryableScope {
        private final Object gate = new Object();
        private final RetirementOwner owner;
        private boolean joining;
        private boolean closeComplete;
        private Throwable closeFailure;

        RetryableScope(RetirementOwner owner) {
            this.owner = owner;
        }

        void close() {
            synchronized (gate) {
                if (closeComplete) return;
                if (joining) {
                    boolean interrupted = false;
                    while (joining) {
                        try {
                            gate.wait();
                        } catch (InterruptedException ignored) {
                            interrupted = true;
                        }
                    }
                    if (interrupted) Thread.currentThread().interrupt();
                    if (closeFailure != null) rethrow(closeFailure);
                    return;
                }
                closeFailure = null;
                joining = true;
            }

            Throwable failure = null;
            try {
                owner.awaitExitAndRetry();
            } catch (RuntimeException | Error problem) {
                failure = problem;
            }

            synchronized (gate) {
                closeFailure = failure;
                joining = false;
                if (failure == null) closeComplete = true;
                gate.notifyAll();
            }
            if (failure != null) rethrow(failure);
        }

        boolean physicallyQuiescent() {
            return owner.complete();
        }

        private static void rethrow(Throwable failure) {
            if (failure instanceof RuntimeException runtime) throw runtime;
            if (failure instanceof Error error) throw error;
            throw new AssertionError(failure);
        }
    }

    private static void deterministicTwoStageRetry() {
        AtomicInteger attempts = new AtomicInteger();
        RetirementOwner owner = new RetirementOwner(attempts, 2);
        RetryableScope scope = new RetryableScope(owner);

        owner.onPhysicalExit();
        check(attempts.get() == 1, "scheduler exit must own first retirement attempt");
        check(!scope.physicallyQuiescent(), "failed scheduler retirement cannot quiesce");

        try {
            scope.close();
            throw new AssertionError("first close must report retained retirement failure");
        } catch (IllegalStateException expected) {
            CHECKS.incrementAndGet();
        }
        check(attempts.get() == 2, "first host close must retry exact owner once");
        check(!scope.physicallyQuiescent(), "failed close must retain owner");

        scope.close();
        check(attempts.get() == 3, "later close must reacquire and retry owner");
        check(scope.physicallyQuiescent(), "successful release must publish quiescence");
        scope.close();
        check(attempts.get() == 3, "successful close must be idempotent");
    }

    private static void concurrentCloseRaces() throws Exception {
        final int rounds = 5_000;
        final int workers = 4;
        for (int round = 0; round < rounds; round++) {
            AtomicInteger attempts = new AtomicInteger();
            RetirementOwner owner = new RetirementOwner(attempts, 1);
            RetryableScope scope = new RetryableScope(owner);
            owner.onPhysicalExit();
            check(attempts.get() == 1, "physical callback must fail once");

            CountDownLatch start = new CountDownLatch(1);
            CountDownLatch done = new CountDownLatch(workers);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread[] threads = new Thread[workers];
            for (int i = 0; i < workers; i++) {
                threads[i] = Thread.ofPlatform().unstarted(() -> {
                    try {
                        start.await();
                        scope.close();
                    } catch (Throwable problem) {
                        failure.compareAndSet(null, problem);
                    } finally {
                        done.countDown();
                    }
                });
                threads[i].start();
            }
            start.countDown();
            check(done.await(10, TimeUnit.SECONDS), "concurrent close race stalled");
            check(failure.get() == null, "concurrent close race failed: " + failure.get());
            check(scope.physicallyQuiescent(), "concurrent closers must converge on quiescence");
            check(attempts.get() == 2, "retirement callback may succeed exactly once after failure");
            for (Thread thread : threads) {
                thread.join(1000);
                check(!thread.isAlive(), "close worker remained live");
            }
        }
    }

    private static void oldOneShotNegativeControl() {
        AtomicInteger attempts = new AtomicInteger();
        boolean oneShotFenceFailed = false;
        attempts.incrementAndGet();
        try {
            throw new IllegalStateException("first retirement failure terminalized old fence");
        } catch (IllegalStateException expected) {
            oneShotFenceFailed = true;
            CHECKS.incrementAndGet();
        }
        check(oneShotFenceFailed && attempts.get() == 1,
                "negative control must model lost retry authority");
    }

    public static void main(String[] args) throws Exception {
        deterministicTwoStageRetry();
        concurrentCloseRaces();
        oldOneShotNegativeControl();
        System.out.println(
                "PASS source-free TaskScope retirement retry oracle checks="
                        + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " old_terminal_fence_negative_control=detected"
                        + " private_source_bytes=0");
    }
}
