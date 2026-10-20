import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Public independently authored transfer/rollback protocol model: zero private source. */
public final class TransferFreezeOracle {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static final class Transfer {
        final Object lock = new Object();
        final AtomicBoolean retired = new AtomicBoolean();
        final AtomicBoolean pinDebited = new AtomicBoolean();
        boolean freeze = true;
        boolean staged;
        boolean registered;
        boolean published;
        int pins = 1;
        int epoch = 1;

        void preallocate() {
            synchronized (lock) {
                check(freeze && !staged, "stage precondition");
                staged = true;
            }
        }
        void putThenThrow() {
            synchronized (lock) {
                check(staged && freeze, "pre-return transfer owner missing");
                registered = true;
            }
        }
        void failedRestore() {
            synchronized (lock) {
                if (!staged) freeze = false;
                check(!staged || freeze, "staged candidate released freeze early");
            }
        }
        void retire() {
            synchronized (lock) {
                if (retired.compareAndSet(false, true)) {
                    registered = false;
                }
                if (!registered && pinDebited.compareAndSet(false, true))
                    pins--;
                if (!registered && pins == 0) freeze = false;
                check(pins >= 0, "double code debit");
            }
        }
        void assertMidway() {
            synchronized (lock) {
                check(freeze && staged && registered && pins == 1,
                        "candidate registered yet old ingress unfrozen");
                check(!published && epoch == 1, "old physical route lost authority");
            }
        }
        void assertComplete() {
            synchronized (lock) {
                check(!freeze && !registered && pins == 0,
                        "transfer retirement incomplete");
                check(epoch == 1 && !published, "stale candidate route published");
            }
        }
    }

    private static void negativeControl() {
        Transfer old = new Transfer();
        old.preallocate();
        old.putThenThrow();
        // Old call order recorded stagedCandidate only AFTER spawn returned.
        old.staged = false;
        old.failedRestore();
        check(!old.freeze && old.registered && old.pins == 1,
                "old early freeze-release defect no longer reproduced");
    }

    public static void main(String[] args) throws Exception {
        long assertions = 0;
        negativeControl();
        assertions++;
        for (int i = 0; i < 50000; i++) {
            Transfer t = new Transfer();
            t.preallocate();
            t.putThenThrow();
            t.failedRestore();
            t.assertMidway();
            assertions++;
            t.retire();
            t.retire();
            t.assertComplete();
            assertions++;
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int n = 0; n < 5000; n++) {
                Transfer t = new Transfer();
                t.preallocate();
                t.putThenThrow();
                t.failedRestore();
                t.assertMidway();
                CountDownLatch gate = new CountDownLatch(1);
                Future<?>[] workers = new Future<?>[4];
                for (int j = 0; j < workers.length; j++) {
                    workers[j] = pool.submit(() -> {
                        try { gate.await(); }
                        catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(e);
                        }
                        t.retire();
                    });
                }
                gate.countDown();
                for (Future<?> worker : workers) worker.get();
                t.assertComplete();
                assertions += 2;
            }
        } finally {
            pool.shutdownNow();
        }
        System.out.println("PASS independent transfer freeze oracle checks=" + assertions
                + " four_worker_races=5000"
                + " old_unsafe_close_negative_control=detected"
                + " private_source_bytes=0");
    }
}
