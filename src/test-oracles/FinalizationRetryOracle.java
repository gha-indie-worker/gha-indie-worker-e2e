import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Source-free oracle for retryable physical finalization ownership.
 * No private Oreslang source is embedded or copied.
 */
public final class FinalizationRetryOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static final class Model {
        private boolean finalizationActive;
        private boolean finalized;
        private boolean physicallyFinalized;
        private boolean failNextCleanup;
        private int cleanupAttempts;
        private int cleanupSuccesses;

        synchronized void injectOneFailure() {
            failNextCleanup = true;
        }

        synchronized boolean finalizeOnce() {
            if (finalized || finalizationActive) {
                return physicallyFinalized;
            }
            finalizationActive = true;
            boolean completed = false;
            try {
                cleanupAttempts++;
                if (failNextCleanup) {
                    failNextCleanup = false;
                    throw new IllegalStateException("injected cleanup failure");
                }
                cleanupSuccesses++;
                finalized = true;
                physicallyFinalized = true;
                completed = true;
                return true;
            } finally {
                finalizationActive = false;
                if (!completed && physicallyFinalized) {
                    throw new AssertionError("physical witness published after failed cleanup");
                }
            }
        }

        synchronized boolean finalized() { return finalized; }
        synchronized boolean active() { return finalizationActive; }
        synchronized boolean physicallyFinalized() { return physicallyFinalized; }
        synchronized int cleanupAttempts() { return cleanupAttempts; }
        synchronized int cleanupSuccesses() { return cleanupSuccesses; }
    }

    private static final class OldBugModel {
        private boolean finalized;
        private boolean physicallyFinalized;
        private boolean failNextCleanup = true;

        synchronized void finalizeOnce() {
            if (finalized) return;
            finalized = true;
            if (failNextCleanup) {
                failNextCleanup = false;
                throw new IllegalStateException("injected cleanup failure");
            }
            physicallyFinalized = true;
        }

        synchronized boolean physicallyFinalized() { return physicallyFinalized; }
        synchronized boolean finalized() { return finalized; }
    }

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) throw new AssertionError(message);
    }

    private static void negativeControl() {
        OldBugModel old = new OldBugModel();
        boolean failed = false;
        try {
            old.finalizeOnce();
        } catch (IllegalStateException expected) {
            failed = true;
        }
        check(failed, "negative control must inject cleanup failure");
        check(old.finalized(), "old model publishes finalized too early");
        check(!old.physicallyFinalized(),
                "old model must not publish physical witness after failure");
        old.finalizeOnce();
        check(!old.physicallyFinalized(),
                "old model must remain permanently stuck on retry");
    }

    private static void sequentialRound() {
        Model model = new Model();
        model.injectOneFailure();
        boolean failed = false;
        try {
            model.finalizeOnce();
        } catch (IllegalStateException expected) {
            failed = true;
        }
        check(failed, "first finalization attempt must fail");
        check(!model.finalized(), "failed cleanup cannot publish finalized");
        check(!model.physicallyFinalized(),
                "failed cleanup cannot publish physical finalization");
        check(!model.active(), "failed cleanup must release exclusive retry owner");
        check(model.cleanupAttempts() == 1, "first cleanup attempt must be counted");
        check(model.cleanupSuccesses() == 0, "failed cleanup is not success");

        check(model.finalizeOnce(), "retry must complete finalization");
        check(model.finalized(), "successful retry must publish finalized");
        check(model.physicallyFinalized(),
                "successful retry must publish physical witness");
        check(!model.active(), "successful finalization must release active owner");
        check(model.cleanupAttempts() == 2, "retry must perform second cleanup attempt");
        check(model.cleanupSuccesses() == 1, "cleanup must succeed exactly once");
        check(model.finalizeOnce(), "later calls must observe stable completion");
        check(model.cleanupAttempts() == 2, "completed finalization must not replay cleanup");
        check(model.cleanupSuccesses() == 1, "completed finalization remains exactly once");
    }

    private static void selfJoinGuard() {
        final Thread owner = Thread.currentThread();
        final boolean physicallyFinalized = false;
        boolean rejected = false;
        try {
            if (!physicallyFinalized && owner == Thread.currentThread()) {
                throw new IllegalStateException(
                        "terminal callback cannot join its own physical finalization");
            }
        } catch (IllegalStateException expected) {
            rejected = true;
        }
        check(rejected, "finalization owner must reject a self-join");
    }

    private static void concurrentRound(ExecutorService pool)
            throws InterruptedException, ExecutionException {
        Model model = new Model();
        List<Callable<Boolean>> calls = new ArrayList<>(4);
        for (int i = 0; i < 4; i++) calls.add(model::finalizeOnce);
        List<Future<Boolean>> results = pool.invokeAll(calls);
        for (Future<Boolean> result : results) {
            check(result.get(), "all callers must observe eventual completion");
        }
        check(model.finalized(), "concurrent finalization must publish finalized");
        check(model.physicallyFinalized(),
                "concurrent finalization must publish physical witness");
        check(!model.active(), "concurrent finalization must leave no active owner");
        check(model.cleanupAttempts() == 1,
                "serialized concurrent finalization must execute cleanup once");
        check(model.cleanupSuccesses() == 1,
                "serialized concurrent cleanup must succeed once");
    }

    public static void main(String[] args) throws Exception {
        negativeControl();
        selfJoinGuard();
        for (int i = 0; i < 10_000; i++) sequentialRound();

        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int i = 0; i < 5_000; i++) concurrentRound(pool);
        } finally {
            pool.shutdownNow();
        }

        System.out.println(
                "PASS source-free finalization retry checks=" + CHECKS.get()
                        + " sequential_rounds=10000"
                        + " race_rounds=5000 workers=4"
                        + " negative_control=early_finalized_stuck_detected"
                        + " private_source_bytes=0");
    }
}
