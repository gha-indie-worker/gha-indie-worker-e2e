import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Source-free state-machine oracle for retryable actor physical finalization.
 * This deliberately contains no private Oreslang source, signatures, constants,
 * implementation text, or repository-derived byte content.
 */
public final class ActorFinalizationRetryOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static final class Model {
        private boolean registryPresent = true;
        private boolean recoveryOwner;
        private boolean finalizationActive;
        private boolean physicallyFinalized;
        private boolean exitHookInvoked;
        private int exitHookCalls;
        private int boundaryAttempts;

        synchronized boolean stop(boolean injectAfterUnregisterFault) {
            if (physicallyFinalized) {
                return true;
            }
            if (!registryPresent && !recoveryOwner) {
                return false;
            }
            if (finalizationActive) {
                return false;
            }

            finalizationActive = true;
            recoveryOwner = true;
            try {
                if (!exitHookInvoked) {
                    exitHookInvoked = true;
                    exitHookCalls++;
                }

                registryPresent = false;
                boundaryAttempts++;

                if (injectAfterUnregisterFault) {
                    throw new IllegalStateException(
                            "injected after registry removal");
                }

                physicallyFinalized = true;
                recoveryOwner = false;
                return true;
            } finally {
                finalizationActive = false;
            }
        }

        synchronized void buggyFaultAfterUnregisterWithoutRecoveryOwner() {
            if (!exitHookInvoked) {
                exitHookInvoked = true;
                exitHookCalls++;
            }
            registryPresent = false;
            recoveryOwner = false;
            boundaryAttempts++;
        }

        synchronized boolean registryPresent() {
            return registryPresent;
        }

        synchronized boolean recoveryOwner() {
            return recoveryOwner;
        }

        synchronized boolean physicallyFinalized() {
            return physicallyFinalized;
        }

        synchronized int exitHookCalls() {
            return exitHookCalls;
        }

        synchronized int boundaryAttempts() {
            return boundaryAttempts;
        }
    }

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void positiveRound() {
        Model model = new Model();

        boolean failed = false;
        try {
            model.stop(true);
        } catch (IllegalStateException expected) {
            failed = true;
        }

        check(failed, "first finalization attempt must inject a fault");
        check(!model.registryPresent(),
                "fault point must be after primary registry removal");
        check(model.recoveryOwner(),
                "exact recovery owner must survive registry removal");
        check(!model.physicallyFinalized(),
                "registry absence must not forge physical finalization");
        check(model.exitHookCalls() == 1,
                "first attempt invokes exactly-once exit hook once");

        check(model.stop(false),
                "supervisor retry must resume through recovery owner");
        check(model.physicallyFinalized(),
                "successful retry must publish physical finalization");
        check(!model.recoveryOwner(),
                "successful finalization must release recovery owner");
        check(model.exitHookCalls() == 1,
                "retry must not invoke exactly-once exit hook twice");
        check(model.boundaryAttempts() == 2,
                "retry must cross post-unregister boundary exactly twice");
        check(model.stop(false),
                "repeated stop after finalization must remain idempotent");
        check(model.exitHookCalls() == 1,
                "idempotent stop must not re-invoke exit hook");
    }

    private static void negativeControl() {
        Model buggy = new Model();
        buggy.buggyFaultAfterUnregisterWithoutRecoveryOwner();

        check(!buggy.registryPresent(),
                "negative control removes primary registry owner");
        check(!buggy.recoveryOwner(),
                "negative control intentionally loses retry ownership");
        check(!buggy.physicallyFinalized(),
                "negative control has not published physical finalization");
        check(!buggy.stop(false),
                "without a recovery owner, later stop cannot resume teardown");
        check(!buggy.physicallyFinalized(),
                "negative control must remain stranded");
    }

    private static void concurrentRetryRounds(int rounds)
            throws InterruptedException, ExecutionException {
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            List<Callable<Boolean>> calls = new ArrayList<>(4);
            for (int round = 0; round < rounds; round++) {
                Model model = new Model();
                try {
                    model.stop(true);
                } catch (IllegalStateException expected) {
                    // Expected one-shot fault after registry removal.
                }

                calls.clear();
                for (int i = 0; i < 4; i++) {
                    calls.add(() -> model.stop(false));
                }

                List<Future<Boolean>> results = pool.invokeAll(calls);
                boolean anySuccess = false;
                for (Future<Boolean> result : results) {
                    anySuccess |= result.get();
                }

                check(anySuccess,
                        "at least one concurrent retry must complete finalization");
                check(model.physicallyFinalized(),
                        "concurrent retries must converge on physical finalization");
                check(!model.recoveryOwner(),
                        "concurrent convergence must release recovery owner");
                check(model.exitHookCalls() == 1,
                        "concurrent retries must preserve exactly-once exit hook");
                check(model.boundaryAttempts() == 2,
                        "only the failed attempt and one successful retry cross boundary");
            }
        } finally {
            pool.shutdownNow();
        }
    }

    public static void main(String[] args) throws Exception {
        negativeControl();
        for (int i = 0; i < 20_000; i++) {
            positiveRound();
        }
        concurrentRetryRounds(5_000);

        System.out.println(
                "PASS source-free actor finalization retry checks=" + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " negative_control=lost_post_unregister_owner_detected"
                        + " exit_hook=exactly_once"
                        + " private_source_bytes=0");
    }
}
