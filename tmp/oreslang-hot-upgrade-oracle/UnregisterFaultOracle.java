import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Independent exact-slot retirement model. No Oreslang private source. */
public final class UnregisterFaultOracle {
    private static final class Actor {
        final int id;
        final AtomicBoolean debited = new AtomicBoolean();
        final AtomicBoolean finalized = new AtomicBoolean();
        Actor(int id) { this.id = id; }
    }
    private static final class Registry extends ConcurrentHashMap<Integer, Actor> {
        private static final long serialVersionUID = 1L;
        final AtomicBoolean before = new AtomicBoolean();
        final AtomicBoolean after = new AtomicBoolean();
        @Override public boolean remove(Object key, Object value) {
            if (before.compareAndSet(true, false))
                throw new IllegalStateException("before removal");
            boolean removed = super.remove(key, value);
            if (removed && after.compareAndSet(true, false))
                throw new IllegalStateException("after removal");
            return removed;
        }
    }
    private static final class Model {
        final Actor actor;
        final Registry registry = new Registry();
        final AtomicInteger slots = new AtomicInteger(1);
        Model(int id, boolean before, boolean after) {
            actor = new Actor(id);
            registry.put(id, actor);
            registry.before.set(before);
            registry.after.set(after);
        }
        synchronized void finish() {
            if (actor.finalized.get()) return;
            boolean absent;
            try {
                absent = registry.remove(actor.id, actor);
            } catch (RuntimeException failure) {
                if (registry.get(actor.id) != null) throw failure;
                absent = true;
            }
            if (!absent && registry.get(actor.id) != null)
                throw new AssertionError("live registry cannot certify retirement");
            if (actor.debited.compareAndSet(false, true)) {
                if (slots.decrementAndGet() != 0)
                    throw new AssertionError("actor slot refunded more than once");
            }
            actor.finalized.set(true);
        }
        void complete() {
            check(registry.isEmpty(), "ghost actor");
            check(slots.get() == 0, "unrefunded actor slot");
            check(actor.finalized.get(), "false physical completion");
            check(actor.debited.get(), "missing once-only debit");
        }
    }
    static void check(boolean yes, String description) {
        if (!yes) throw new AssertionError(description);
    }
    private static void oldNegativeControl() {
        Registry broken = new Registry();
        Actor actor = new Actor(1);
        broken.put(1, actor);
        broken.after.set(true);
        int[] count = {1};
        try {
            if (broken.remove(1, actor)) count[0]--;
        } catch (IllegalStateException injected) {
            check(broken.isEmpty() && count[0] == 1,
                    "former post-removal error must strand old slot");
        }
    }
    public static void main(String[] args) throws Exception {
        long checks = 0;
        oldNegativeControl(); checks++;
        for (int i = 0; i < 25000; i++) {
            Model after = new Model(i, false, true);
            after.finish(); after.finish(); after.complete(); checks += 4;
            Model before = new Model(i, true, false);
            try {
                before.finish();
                throw new AssertionError("before-mutation failure not injected");
            } catch (IllegalStateException expected) {
                check(before.slots.get() == 1
                        && before.registry.get(i) == before.actor
                        && !before.actor.finalized.get(), "early completion");
            }
            before.finish(); before.complete(); checks += 5;
        }
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            for (int i = 0; i < 5000; i++) {
                Model model = new Model(i, false, true);
                CountDownLatch gate = new CountDownLatch(1);
                Future<?>[] jobs = new Future<?>[4];
                for (int j = 0; j < jobs.length; j++) {
                    jobs[j] = pool.submit(() -> {
                        try { gate.await(); }
                        catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(e);
                        }
                        model.finish();
                    });
                }
                gate.countDown();
                for (Future<?> job : jobs) job.get();
                model.complete(); checks += 4;
            }
        } finally { pool.shutdownNow(); }
        System.out.println("PASS independent physical unregister oracle checks="
                + checks + " four_worker_races=5000"
                + " before_and_after_mutation=verified"
                + " old_failure_negative_control=detected private_source_bytes=0");
    }
}
