import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public final class ReliablePendingReleaseOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) throw new AssertionError(message);
    }

    private static final class Accounting {
        long bytes;
        boolean fail;

        synchronized void release(long amount) {
            if (fail || amount > bytes) {
                throw new IllegalStateException("accounting underflow");
            }
            bytes -= amount;
        }
    }

    private static final class Event {
        private final Accounting accounting;
        private final long bytes;
        private int refs = 1;

        Event(Accounting accounting, long bytes) {
            this.accounting = accounting;
            this.bytes = bytes;
        }

        synchronized void retain() {
            if (refs <= 0) throw new IllegalStateException("released");
            refs++;
        }

        synchronized void release() {
            if (refs <= 0) throw new IllegalStateException("underflow");
            if (refs == 1) {
                accounting.release(bytes);
                refs = 0;
            } else {
                refs--;
            }
        }

        synchronized int refs() {
            return refs;
        }
    }

    private static final class Owner {
        private final Event event;
        private final Set<Owner> owners;
        private final AtomicInteger reliableOutstanding;
        private boolean retained;
        private boolean failed;
        private boolean settled;

        Owner(Event event, Set<Owner> owners, AtomicInteger reliableOutstanding) {
            this.event = event;
            this.owners = owners;
            this.reliableOutstanding = reliableOutstanding;
        }

        synchronized void markRetained() {
            retained = true;
        }

        synchronized void admitted() {
            if (settled) return;
            settled = true;
            owners.remove(this);
        }

        synchronized void failAndRelease() {
            if (settled) return;
            failed = true;
            if (!retained) {
                settled = true;
                owners.remove(this);
                return;
            }
            event.release();
            reliableOutstanding.decrementAndGet();
            settled = true;
            owners.remove(this);
        }

        synchronized void retry() {
            if (settled) return;
            if (!failed) throw new IllegalStateException("pending after close");
            failAndRelease();
        }
    }

    private static void futureCallbackThatSwallows(Owner owner) {
        try {
            owner.failAndRelease();
        } catch (RuntimeException | Error ignored) {
            // Models OresFuture runtime waiter isolation.
        }
    }

    private static void deterministicRecovery() {
        Accounting accounting = new Accounting();
        accounting.bytes = 64;
        Set<Owner> owners = ConcurrentHashMap.newKeySet();
        AtomicInteger reliableOutstanding = new AtomicInteger(1);
        Event event = new Event(accounting, 64);
        Owner owner = new Owner(event, owners, reliableOutstanding);

        owners.add(owner);
        event.retain();
        owner.markRetained();
        event.release(); // publisher temporary hold
        check(event.refs() == 1, "pending write must own final Event ref");

        accounting.fail = true;
        futureCallbackThatSwallows(owner);

        check(owners.contains(owner),
                "swallowed Future callback failure must retain exact owner");
        check(event.refs() == 1,
                "failed callback must retain final Event ref");
        check(reliableOutstanding.get() == 1,
                "failed payload release cannot consume reliable credit");

        accounting.fail = false;
        owner.retry();

        check(owners.isEmpty(), "successful retry must remove exact owner");
        check(event.refs() == 0, "successful retry must release final Event ref");
        check(reliableOutstanding.get() == 0,
                "successful retry must release reliable credit exactly once");
        check(accounting.bytes == 0, "successful retry must settle accounting");
        owner.retry();
        check(reliableOutstanding.get() == 0,
                "completed owner retry must be idempotent");
    }

    private static void concurrentCloseRetry() throws Exception {
        final int rounds = 5_000;
        final int workers = 4;
        for (int round = 0; round < rounds; round++) {
            Accounting accounting = new Accounting();
            accounting.bytes = 32;
            Set<Owner> owners = ConcurrentHashMap.newKeySet();
            AtomicInteger reliableOutstanding = new AtomicInteger(1);
            Event event = new Event(accounting, 32);
            Owner owner = new Owner(event, owners, reliableOutstanding);
            owners.add(owner);
            event.retain();
            owner.markRetained();
            event.release();

            accounting.fail = true;
            futureCallbackThatSwallows(owner);
            accounting.fail = false;

            CountDownLatch start = new CountDownLatch(1);
            CountDownLatch done = new CountDownLatch(workers);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread[] threads = new Thread[workers];

            for (int i = 0; i < workers; i++) {
                threads[i] = Thread.ofPlatform().unstarted(() -> {
                    try {
                        start.await();
                        for (Owner retained : owners) retained.retry();
                    } catch (Throwable problem) {
                        failure.compareAndSet(null, problem);
                    } finally {
                        done.countDown();
                    }
                });
                threads[i].start();
            }

            start.countDown();
            check(done.await(10, TimeUnit.SECONDS), "retry race stalled");
            check(failure.get() == null, "retry race failed: " + failure.get());
            check(owners.isEmpty(), "all close retries must converge");
            check(event.refs() == 0, "Event final ref must release exactly once");
            check(reliableOutstanding.get() == 0,
                    "reliable credit must release exactly once");
            check(accounting.bytes == 0, "accounting must settle exactly once");

            for (Thread thread : threads) {
                thread.join(1000);
                check(!thread.isAlive(), "retry worker remained alive");
            }
        }
    }

    private static void oldWaiterOnlyNegativeControl() {
        Accounting accounting = new Accounting();
        accounting.bytes = 16;
        accounting.fail = true;
        Event event = new Event(accounting, 16);
        event.retain();
        event.release();
        boolean swallowed = false;
        try {
            event.release();
        } catch (IllegalStateException expected) {
            swallowed = true;
            CHECKS.incrementAndGet();
        }
        check(swallowed && event.refs() == 1 && accounting.bytes == 16,
                "old waiter-only path must expose stranded retained delivery");
    }

    public static void main(String[] args) throws Exception {
        deterministicRecovery();
        concurrentCloseRetry();
        oldWaiterOnlyNegativeControl();
        System.out.println(
                "PASS source-free RELIABLE pending release oracle checks="
                        + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " swallowed_waiter_negative_control=detected"
                        + " private_source_bytes=0");
    }
}
