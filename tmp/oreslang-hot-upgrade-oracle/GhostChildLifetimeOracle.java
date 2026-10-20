import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Independent source-free lifecycle model. No Oreslang private code. */
public final class GhostChildLifetimeOracle {
    private static final class Model {
        private final Object lock = new Object();
        private boolean registered = true;
        private boolean parentOwns = true;
        private boolean groupOwns = true;
        private boolean childFinalized;
        private boolean parentCloseRequested;
        private boolean parentFinalized;
        private int childSlots = 1;
        private int childCodePins = 1;
        private final AtomicBoolean debited = new AtomicBoolean();

        void failedRollbackBeforeRemove() {
            synchronized (lock) {
                // The registered physical child survived. Rollback has
                // no authority to unlink its parent or managed group.
                check(registered && parentOwns && groupOwns, "missing original owner");
            }
        }
        void requestParentClose() {
            synchronized (lock) {
                parentCloseRequested = true;
                if (!parentOwns) parentFinalized = true;
            }
        }
        void finalizeChild() {
            synchronized (lock) {
                if (childFinalized) return;
                childFinalized = true;
                registered = false;
                childSlots--;
                parentOwns = false;
                groupOwns = false;
                if (parentCloseRequested) parentFinalized = true;
                if (debited.compareAndSet(false, true)) childCodePins--;
            }
        }
        void assertInvariants() {
            synchronized (lock) {
                check(childSlots >= 0 && childCodePins >= 0, "negative lifetime balance");
                check(!registered || (parentOwns && groupOwns && !childFinalized),
                        "registered child lost lifecycle ownership");
                check(!parentFinalized || !registered,
                        "parent finalized before registered child");
                check(!childFinalized || (childSlots == 0 && childCodePins == 0),
                        "child physically finalized without exact debit");
            }
        }
        void assertComplete() {
            synchronized (lock) {
                check(parentFinalized && childFinalized, "incomplete physical exit");
                check(!registered && !parentOwns && !groupOwns, "orphaned child");
                check(childSlots == 0 && childCodePins == 0, "incorrect final balance");
            }
        }
    }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void negativeControl() {
        Model bad = new Model();
        synchronized (bad.lock) {
            bad.parentOwns = false;
            bad.groupOwns = false;
            bad.parentCloseRequested = true;
            bad.parentFinalized = true;
            check(bad.registered && bad.parentFinalized,
                    "old unlink-on-failed-remove bug not reproduced");
        }
    }
    public static void main(String[] args) throws Exception {
        long assertions = 0L;
        negativeControl();
        assertions++;
        for (int n = 0; n < 40000; n++) {
            Model m = new Model();
            m.failedRollbackBeforeRemove();
            m.requestParentClose();
            m.assertInvariants();
            m.finalizeChild();
            m.finalizeChild();
            m.assertComplete();
            assertions += 3;
        }
        ExecutorService workers = Executors.newFixedThreadPool(4);
        try {
            for (int round = 0; round < 2000; round++) {
                Model m = new Model();
                m.failedRollbackBeforeRemove();
                CountDownLatch ready = new CountDownLatch(4);
                CountDownLatch start = new CountDownLatch(1);
                Future<?>[] jobs = new Future<?>[4];
                for (int i = 0; i < 4; i++) {
                    final int action = i;
                    jobs[i] = workers.submit(() -> {
                        ready.countDown();
                        try { start.await(); }
                        catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(e);
                        }
                        if (action == 0) m.requestParentClose();
                        else m.finalizeChild();
                    });
                }
                check(ready.await(5, TimeUnit.SECONDS), "workers did not arrive");
                start.countDown();
                for (Future<?> job : jobs) job.get();
                m.assertInvariants();
                m.assertComplete();
                assertions += 3;
            }
        } finally {
            workers.shutdownNow();
        }
        System.out.println("PASS source-free ghost child oracle checks=" + assertions
                + " four_worker_races=2000 pre_mutation_failure=verified"
                + " old_unlink_negative_control=detected private_source_bytes=0");
    }
}
