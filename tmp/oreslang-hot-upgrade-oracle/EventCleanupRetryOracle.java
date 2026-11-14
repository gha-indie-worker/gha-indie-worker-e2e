import java.util.ArrayDeque;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public final class EventCleanupRetryOracle {
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

    private static final class Reservation {
        private final AtomicReference<Accounting> owner;
        private final long bytes;

        Reservation(Accounting accounting, long bytes) {
            this.owner = new AtomicReference<>(accounting);
            this.bytes = bytes;
        }

        synchronized void close() {
            Accounting accounting = owner.get();
            if (accounting == null) return;
            accounting.release(bytes);
            owner.set(null);
        }

        boolean owned() {
            return owner.get() != null;
        }
    }

    private static final class Event {
        private final Reservation reservation;
        private int refs = 1;

        Event(Reservation reservation) {
            this.reservation = reservation;
        }

        synchronized void retain() {
            if (refs <= 0) throw new IllegalStateException("released");
            refs++;
        }

        synchronized void release() {
            if (refs <= 0) throw new IllegalStateException("underflow");
            if (refs == 1) {
                reservation.close();
                refs = 0;
            } else {
                refs--;
            }
        }

        synchronized int refs() {
            return refs;
        }
    }

    private static final class Subscription {
        private final ArrayDeque<Event> buffered = new ArrayDeque<>();
        private Event closePending;
        private boolean sealed;
        private boolean cleanupComplete;

        synchronized void enqueue(Event event) {
            if (sealed) throw new IllegalStateException("closed");
            event.retain();
            buffered.addLast(event);
        }

        synchronized void close() {
            if (cleanupComplete) return;
            sealed = true;

            releasePending();
            while (!buffered.isEmpty()) {
                closePending = buffered.removeFirst();
                releasePending();
            }
            cleanupComplete = true;
        }

        private void releasePending() {
            Event pending = closePending;
            if (pending == null) return;
            pending.release();
            closePending = null;
        }
    }

    private static void deterministicRecovery() {
        Accounting accounting = new Accounting();
        accounting.bytes = 64;
        Reservation reservation = new Reservation(accounting, 64);
        Event event = new Event(reservation);
        Subscription subscription = new Subscription();

        subscription.enqueue(event);
        event.release(); // publisher temporary hold
        check(event.refs() == 1, "subscription must own the final event ref");

        accounting.fail = true;
        try {
            subscription.close();
            throw new AssertionError("expected accounting release failure");
        } catch (IllegalStateException expected) {
            CHECKS.incrementAndGet();
        }

        check(subscription.sealed, "subscription admission must stay sealed");
        check(!subscription.cleanupComplete, "seal is not drain completion");
        check(subscription.closePending == event,
                "dequeued event must remain exact retry owner");
        check(event.refs() == 1,
                "failed final debit must retain the event ref");
        check(reservation.owned(),
                "failed debit must retain reservation owner");
        check(accounting.bytes == 64,
                "failed debit cannot mutate aggregate accounting");

        accounting.fail = false;
        subscription.close();

        check(subscription.cleanupComplete, "retry must complete drain");
        check(subscription.closePending == null, "retry owner must clear");
        check(event.refs() == 0, "successful retry must consume final ref");
        check(!reservation.owned(), "successful debit must clear reservation owner");
        check(accounting.bytes == 0, "successful retry must debit exactly once");

        subscription.close();
        check(accounting.bytes == 0, "completed close must be idempotent");
    }

    private static void concurrentRetryRaces() throws Exception {
        final int rounds = 5_000;
        final int workers = 4;

        for (int round = 0; round < rounds; round++) {
            Accounting accounting = new Accounting();
            accounting.bytes = 32;
            Reservation reservation = new Reservation(accounting, 32);
            Event event = new Event(reservation);
            Subscription subscription = new Subscription();
            subscription.enqueue(event);
            event.release();

            accounting.fail = true;
            try {
                subscription.close();
                throw new AssertionError("expected first release failure");
            } catch (IllegalStateException expected) {
                CHECKS.incrementAndGet();
            }
            accounting.fail = false;

            CountDownLatch start = new CountDownLatch(1);
            CountDownLatch done = new CountDownLatch(workers);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread[] threads = new Thread[workers];

            for (int i = 0; i < workers; i++) {
                threads[i] = Thread.ofPlatform().unstarted(() -> {
                    try {
                        start.await();
                        subscription.close();
                    } catch (Throwable problem) {
                        failure.compareAndSet(null, problem);
                    } finally {
                        done.countDown();
                    }
                });
                threads[i].start();
            }

            start.countDown();
            check(done.await(10, TimeUnit.SECONDS), "concurrent retry stalled");
            check(failure.get() == null, "concurrent retry failed: " + failure.get());
            check(subscription.cleanupComplete, "concurrent retries must converge");
            check(subscription.closePending == null, "pending owner must clear");
            check(event.refs() == 0, "final event ref must release exactly once");
            check(!reservation.owned(), "reservation owner must clear exactly once");
            check(accounting.bytes == 0, "aggregate debit must settle exactly once");

            for (Thread thread : threads) {
                thread.join(1000);
                check(!thread.isAlive(), "retry worker remained live");
            }
        }
    }

    private static void oldOrderingNegativeControl() {
        Accounting accounting = new Accounting();
        accounting.bytes = 64;
        accounting.fail = true;
        AtomicReference<Accounting> oldOwner = new AtomicReference<>(accounting);
        Accounting lost = oldOwner.getAndSet(null);
        boolean failed = false;
        try {
            lost.release(64);
        } catch (IllegalStateException expected) {
            failed = true;
            CHECKS.incrementAndGet();
        }
        check(failed && oldOwner.get() == null,
                "old getAndSet ordering must demonstrate lost retry owner");
    }

    public static void main(String[] args) throws Exception {
        deterministicRecovery();
        concurrentRetryRaces();
        oldOrderingNegativeControl();
        System.out.println(
                "PASS source-free event cleanup retry oracle checks="
                        + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " old_owner_loss_negative_control=detected"
                        + " private_source_bytes=0");
    }
}
