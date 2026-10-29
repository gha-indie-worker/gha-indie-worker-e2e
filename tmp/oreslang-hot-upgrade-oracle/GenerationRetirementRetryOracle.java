import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public final class GenerationRetirementRetryOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) throw new AssertionError(message);
    }

    private static final class Manager {
        private final Map<Long, Generation> generations = new LinkedHashMap<>();
        private boolean closed;
        private Generation active;

        synchronized Generation add(long id) {
            if (closed) throw new IllegalStateException("manager closed");
            Generation generation = new Generation(this, id);
            generations.put(id, generation);
            active = generation;
            return generation;
        }

        void retire(long id) {
            final Generation generation;
            synchronized (this) {
                generation = generations.get(id);
                if (generation == null) return;
                if (generation.pins() != 0) throw new IllegalStateException("pinned");
                if (active == generation) active = null;
            }
            generation.close(false);
        }

        void close() {
            final List<Generation> snapshot;
            synchronized (this) {
                if (!closed) {
                    for (Generation generation : generations.values()) {
                        if (generation.pins() != 0) throw new IllegalStateException("pinned");
                    }
                    closed = true;
                    active = null;
                } else if (generations.isEmpty()) {
                    return;
                }
                snapshot = List.copyOf(generations.values());
            }

            Throwable failure = null;
            for (Generation generation : snapshot) {
                try {
                    generation.close(true);
                } catch (RuntimeException | Error problem) {
                    if (failure == null) failure = problem;
                    else if (failure != problem) failure.addSuppressed(problem);
                }
            }
            if (failure instanceof RuntimeException runtime) throw runtime;
            if (failure instanceof Error error) throw error;
        }

        synchronized void onClosed(Generation generation) {
            generations.remove(generation.id, generation);
            if (active == generation) active = null;
        }

        synchronized int live() {
            return generations.size();
        }

        synchronized boolean isActive(Generation generation) {
            return active == generation;
        }
    }

    private static final class Generation {
        private final Manager owner;
        private final long id;
        private boolean sealed;
        private boolean contextClosed;
        private boolean closeComplete;
        private boolean closeActive;
        private int pins;
        private int failAfterSeal;
        private int failAfterContext;
        private final AtomicInteger physicalCloseCalls = new AtomicInteger();

        Generation(Manager owner, long id) {
            this.owner = owner;
            this.id = id;
        }

        synchronized int pins() {
            return pins;
        }

        synchronized void pin() {
            if (sealed || owner.closed || owner.generations.get(id) != this) {
                throw new IllegalStateException("not routable");
            }
            pins++;
        }

        synchronized void unpin() {
            if (pins <= 0) throw new IllegalStateException("underflow");
            pins--;
        }

        void close(boolean force) {
            final boolean closeContext;
            synchronized (this) {
                if (closeComplete) return;
                if (closeActive) throw new IllegalStateException("close active");
                if (!force && pins != 0) throw new IllegalStateException("pinned");
                sealed = true;
                closeActive = true;
                closeContext = !contextClosed;
            }

            try {
                synchronized (this) {
                    if (failAfterSeal > 0) {
                        failAfterSeal--;
                        throw new IllegalStateException("after seal");
                    }
                }

                if (closeContext) {
                    physicalCloseCalls.incrementAndGet();
                    synchronized (this) {
                        contextClosed = true;
                    }
                }

                synchronized (this) {
                    if (failAfterContext > 0) {
                        failAfterContext--;
                        throw new IllegalStateException("after context");
                    }
                }

                owner.onClosed(this);
                synchronized (this) {
                    closeComplete = true;
                }
            } finally {
                synchronized (this) {
                    closeActive = false;
                }
            }
        }
    }

    private static void retireAfterSealRecovery() {
        Manager manager = new Manager();
        Generation generation = manager.add(1);
        generation.failAfterSeal = 1;

        try {
            manager.retire(1);
            throw new AssertionError("expected retire failure");
        } catch (IllegalStateException expected) {
            CHECKS.incrementAndGet();
        }

        check(generation.sealed, "failed retire must remain sealed");
        check(!manager.isActive(generation), "failed retire must stay unrouted");
        check(manager.live() == 1, "failed retire must retain registry owner");
        check(!generation.contextClosed, "pre-context fault cannot forge context close");

        manager.retire(1);
        check(manager.live() == 0, "retry must evict after physical close");
        check(generation.contextClosed, "retry must close context");
        check(generation.physicalCloseCalls.get() == 1,
                "context close must execute exactly once");
    }

    private static void managerPostContextRecovery() {
        Manager manager = new Manager();
        Generation retained = manager.add(1);
        Generation sibling = manager.add(2);
        retained.failAfterContext = 1;

        try {
            manager.close();
            throw new AssertionError("expected manager close failure");
        } catch (IllegalStateException expected) {
            CHECKS.incrementAndGet();
        }

        check(manager.closed, "manager admission must remain sealed");
        check(retained.contextClosed, "context-close receipt must survive later fault");
        check(retained.physicalCloseCalls.get() == 1,
                "context close must not replay after receipt");
        check(sibling.contextClosed, "independent sibling close must continue");
        check(manager.live() == 1, "failed deregistration owner must remain registered");

        manager.close();

        check(manager.live() == 0, "manager retry must finish retained generation");
        check(retained.physicalCloseCalls.get() == 1,
                "retry must skip already-completed physical context close");
    }

    private static void concurrentRetireRetries() throws Exception {
        final int rounds = 5_000;
        final int workers = 4;

        for (int round = 0; round < rounds; round++) {
            Manager manager = new Manager();
            Generation generation = manager.add(1);
            generation.failAfterSeal = 1;
            try {
                manager.retire(1);
                throw new AssertionError("expected first retire failure");
            } catch (IllegalStateException expected) {
                CHECKS.incrementAndGet();
            }

            CountDownLatch start = new CountDownLatch(1);
            CountDownLatch done = new CountDownLatch(workers);
            AtomicReference<Throwable> unexpected = new AtomicReference<>();
            Thread[] threads = new Thread[workers];

            for (int i = 0; i < workers; i++) {
                threads[i] = Thread.ofPlatform().unstarted(() -> {
                    try {
                        start.await();
                        try {
                            manager.retire(1);
                        } catch (IllegalStateException inProgress) {
                            // Another retry may own closeActive temporarily.
                        }
                    } catch (Throwable problem) {
                        unexpected.compareAndSet(null, problem);
                    } finally {
                        done.countDown();
                    }
                });
                threads[i].start();
            }

            start.countDown();
            check(done.await(10, TimeUnit.SECONDS), "retire retry race stalled");
            check(unexpected.get() == null,
                    "retire retry race failed: " + unexpected.get());

            if (manager.live() != 0) manager.retire(1);
            check(manager.live() == 0, "retire retries must converge");
            check(generation.contextClosed, "context must close");
            check(generation.physicalCloseCalls.get() == 1,
                    "physical context close may happen once");

            for (Thread thread : threads) {
                thread.join(1000);
                check(!thread.isAlive(), "retire worker remained alive");
            }
        }
    }

    private static void oldRemoveBeforeCloseNegativeControl() {
        Manager manager = new Manager();
        Generation generation = manager.add(1);
        generation.failAfterSeal = 1;

        synchronized (manager) {
            manager.generations.remove(1L);
            manager.active = null;
        }
        boolean failed = false;
        try {
            generation.close(false);
        } catch (IllegalStateException expected) {
            failed = true;
            CHECKS.incrementAndGet();
        }
        check(failed && manager.live() == 0 && !generation.contextClosed,
                "old remove-before-close ordering must lose retry registry");
    }

    public static void main(String[] args) throws Exception {
        retireAfterSealRecovery();
        managerPostContextRecovery();
        concurrentRetireRetries();
        oldRemoveBeforeCloseNegativeControl();
        System.out.println(
                "PASS source-free generation retirement retry oracle checks="
                        + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " remove_before_close_negative_control=detected"
                        + " private_source_bytes=0");
    }
}
