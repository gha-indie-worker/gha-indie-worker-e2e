import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Independent public protocol model, not private Oreslang production source. */
public final class RetiredStopRetryOracle {
    private static void check(boolean ok, String why) {
        if (!ok) throw new AssertionError(why);
    }

    private static final class Supervisor {
        final Object lock = new Object();
        final AtomicBoolean transientStopFailure = new AtomicBoolean(true);
        boolean publicationCompleted;
        boolean oldPhysicalRunning = true;
        boolean oldPinRegistered = true;
        boolean retired = false;
        boolean quarantined = true;
        long routeEpoch = 1L;
        int codePins = 1;
        int stopAttempts;

        void publish() {
            synchronized (lock) {
                check(!publicationCompleted, "double commit");
                publicationCompleted = true;
                routeEpoch = 2;
            }
        }

        void stopOld() {
            synchronized (lock) {
                check(publicationCompleted, "cannot stop active source");
                stopAttempts++;
                if (transientStopFailure.compareAndSet(true, false)) {
                    throw new IllegalStateException("injected stop before physical mutation");
                }
                oldPhysicalRunning = false;
            }
        }

        int passiveReap() {
            synchronized (lock) {
                if (!oldPhysicalRunning && oldPinRegistered) {
                    oldPinRegistered = false;
                    codePins--;
                    retired = true;
                    quarantined = false;
                    return 1;
                }
                return 0;
            }
        }

        int retryStopAndReap() {
            synchronized (lock) {
                check(publicationCompleted, "cannot reconcile staging");
                if (oldPinRegistered && oldPhysicalRunning) stopOld();
                return passiveReap();
            }
        }

        void assertQuarantined() {
            synchronized (lock) {
                check(publicationCompleted && routeEpoch == 2
                        && oldPhysicalRunning && codePins == 1
                        && oldPinRegistered && quarantined,
                        "postcommit error reopened old route or debited code");
            }
        }

        void assertComplete() {
            synchronized (lock) {
                check(publicationCompleted && routeEpoch == 2
                        && !oldPhysicalRunning && !oldPinRegistered
                        && retired && !quarantined && codePins == 0,
                        "retirement retry did not settle exactly once");
            }
        }
    }

    private static void negativeControl() {
        Supervisor s = new Supervisor();
        s.publish();
        try {
            s.stopOld();
            throw new AssertionError("injected stop unexpectedly succeeded");
        } catch (IllegalStateException expected) {
            s.assertQuarantined();
        }
        for (int i = 0; i < 10; i++)
            check(s.passiveReap() == 0, "passive reaper forged finalization");
        check(s.oldPhysicalRunning && s.codePins == 1,
                "passive-only reaper unexpectedly made progress");
    }

    public static void main(String[] args) throws Exception {
        long checks = 0L;
        negativeControl();
        checks++;
        for (int i = 0; i < 50000; i++) {
            Supervisor s = new Supervisor();
            s.publish();
            try {
                s.stopOld();
                throw new AssertionError("failure expected");
            } catch (IllegalStateException expected) {
                s.assertQuarantined();
                checks++;
            }
            check(s.passiveReap() == 0, "passive finalization unproven");
            check(s.retryStopAndReap() == 1, "first retry did not retire");
            check(s.retryStopAndReap() == 0, "double-retirement");
            s.assertComplete();
            checks += 3;
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int n = 0; n < 5000; n++) {
                Supervisor s = new Supervisor();
                s.publish();
                try { s.stopOld(); }
                catch (IllegalStateException expected) { s.assertQuarantined(); }
                CountDownLatch start = new CountDownLatch(1);
                Future<?>[] races = new Future<?>[4];
                AtomicInteger reaped = new AtomicInteger();
                for (int worker = 0; worker < 4; worker++) {
                    races[worker] = pool.submit(() -> {
                        try { start.await(); }
                        catch (InterruptedException failure) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(failure);
                        }
                        reaped.addAndGet(s.retryStopAndReap());
                    });
                }
                start.countDown();
                for (Future<?> race : races) race.get();
                check(reaped.get() == 1, "concurrent retry double-debited code");
                s.assertComplete();
                checks += 2;
            }
        } finally { pool.shutdownNow(); }
        System.out.println("PASS source-free retired stop retry checks=" + checks
                + " four_worker_races=5000"
                + " passive_reap_negative_control=detected"
                + " private_source_bytes=0");
    }
}
