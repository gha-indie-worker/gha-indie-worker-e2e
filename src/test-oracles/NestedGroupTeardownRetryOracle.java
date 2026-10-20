import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class NestedGroupTeardownRetryOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class EventBusModel {
        boolean sealed;
        boolean cleanupComplete;
        boolean ambiguous;
        boolean safeFaultOnce = true;
        int cleanupAttempts;

        synchronized void close() {
            if (cleanupComplete) return;
            if (ambiguous) throw new IllegalStateException("ambiguous bus cleanup");
            sealed = true;
            cleanupAttempts++;
            if (safeFaultOnce) {
                safeFaultOnce = false;
                throw new OutOfMemoryError("after seal before cleanup");
            }
            cleanupComplete = true;
        }
    }

    private static final class GroupModel {
        final EventBusModel bus = new EventBusModel();
        boolean sealed;
        boolean registryPresent = true;
        boolean ownerRetained;
        boolean closeComplete;
        boolean active;
        int witnessTransitions;

        synchronized void close() {
            if (closeComplete) return;
            if (active) throw new IllegalStateException("reentrant close");
            active = true;
            try {
                if (!sealed) {
                    ownerRetained = true;
                    sealed = true;
                }
                bus.close();
                registryPresent = false;
                closeComplete = true;
                ownerRetained = false;
                witnessTransitions++;
            } finally {
                active = false;
            }
        }
    }

    private static final class BrokenGroupModel {
        final EventBusModel bus = new EventBusModel();
        boolean sealed;
        boolean registryPresent = true;

        synchronized void close() {
            if (sealed) return;
            sealed = true;
            try {
                bus.close();
            } finally {
                registryPresent = false;
            }
        }
    }

    private static void retryRound() {
        GroupModel group = new GroupModel();
        try {
            group.close();
            throw new AssertionError("expected nested bus fault");
        } catch (OutOfMemoryError expected) {
            check(group.sealed, "group admission not sealed");
            check(group.bus.sealed, "bus admission not sealed");
            check(!group.bus.cleanupComplete, "bus fault forged cleanup completion");
            check(group.registryPresent, "group registry owner was dropped");
            check(group.ownerRetained, "durable group owner was dropped");
            check(!group.closeComplete, "nested fault forged group completion");
            check(!group.active, "failed close leaked active owner");
        }
        group.close();
        group.close();
        check(group.bus.cleanupComplete, "nested cleanup did not retry");
        check(!group.registryPresent, "registry owner not released after success");
        check(!group.ownerRetained, "durable owner leaked after success");
        check(group.closeComplete, "group retry did not complete");
        check(group.witnessTransitions == 1, "group witness duplicated");
        check(group.bus.cleanupAttempts == 2, "nested cleanup retried incorrectly");
    }

    private static void raceRound(int workers) throws Exception {
        GroupModel group = new GroupModel();
        try {
            group.close();
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
                    group.close();
                } catch (Throwable problem) {
                    failure.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(failure.get() == null, "race raised " + failure.get());
        check(group.closeComplete, "race did not complete group");
        check(group.bus.cleanupComplete, "race did not complete nested bus");
        check(group.witnessTransitions == 1, "race duplicated group witness");
        check(!group.registryPresent, "race leaked registry owner");
    }

    private static void negativeControl() {
        BrokenGroupModel group = new BrokenGroupModel();
        try {
            group.close();
        } catch (OutOfMemoryError expected) {
            // expected
        }
        group.close();
        check(group.sealed, "negative control did not seal group");
        check(!group.registryPresent,
                "negative control did not lose the registry recovery owner");
        check(!group.bus.cleanupComplete,
                "negative control failed to strand nested cleanup");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;
        for (int i = 0; i < retryRounds; i++) retryRound();
        negativeControl();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);
        System.out.println(
                "PASS source-free nested group teardown retry checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=lost_nested_registry_owner_detected"
                + " private_source_bytes=0");
    }
}
