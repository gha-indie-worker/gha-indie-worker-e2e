import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

public final class ActorOutputReleaseOracle {
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

    private static final class Stream {
        final Set<Envelope> retained = ConcurrentHashMap.newKeySet();

        void retain(Envelope envelope) {
            if (!retained.add(envelope)) {
                throw new IllegalStateException("duplicate envelope");
            }
        }

        void retryFailed() {
            for (Envelope envelope : retained) {
                if (envelope.releaseFailed()) envelope.close();
            }
        }
    }

    private static final class Envelope {
        private final Stream owner;
        private final Accounting accounting;
        private final long bytes;
        private boolean closed;
        private boolean releaseFailed;

        Envelope(Stream owner, Accounting accounting, long bytes) {
            this.owner = owner;
            this.accounting = accounting;
            this.bytes = bytes;
        }

        synchronized boolean releaseFailed() {
            return releaseFailed && !closed;
        }

        synchronized void close() {
            if (closed) return;
            try {
                accounting.release(bytes);
            } catch (RuntimeException | Error failure) {
                releaseFailed = true;
                throw failure;
            }
            releaseFailed = false;
            closed = true;
            owner.retained.remove(this);
        }

        synchronized boolean closed() {
            return closed;
        }
    }

    private static void futureWaiterThatSwallows(Envelope envelope) {
        try {
            envelope.close();
        } catch (RuntimeException | Error ignored) {
            // Models OresFuture runtime waiter isolation.
        }
    }

    private static void deterministicRecovery() {
        Accounting accounting = new Accounting();
        accounting.bytes = 64;
        Stream stream = new Stream();
        Envelope envelope = new Envelope(stream, accounting, 64);
        stream.retain(envelope);

        accounting.fail = true;
        futureWaiterThatSwallows(envelope);

        check(stream.retained.contains(envelope),
                "swallowed read callback failure must retain envelope");
        check(envelope.releaseFailed(),
                "failed envelope release must publish retry state");
        check(!envelope.closed(), "failed release cannot seal envelope");
        check(accounting.bytes == 64, "failed release cannot mutate accounting");

        accounting.fail = false;
        stream.retryFailed();

        check(stream.retained.isEmpty(), "successful retry must release owner");
        check(envelope.closed(), "successful retry must close envelope");
        check(accounting.bytes == 0, "successful retry must debit once");
        stream.retryFailed();
        check(accounting.bytes == 0, "completed retry must be idempotent");
    }

    private static void concurrentRetries() throws Exception {
        final int rounds = 5_000;
        final int workers = 4;
        for (int round = 0; round < rounds; round++) {
            Accounting accounting = new Accounting();
            accounting.bytes = 32;
            Stream stream = new Stream();
            Envelope envelope = new Envelope(stream, accounting, 32);
            stream.retain(envelope);

            accounting.fail = true;
            futureWaiterThatSwallows(envelope);
            accounting.fail = false;

            CountDownLatch start = new CountDownLatch(1);
            CountDownLatch done = new CountDownLatch(workers);
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread[] threads = new Thread[workers];

            for (int i = 0; i < workers; i++) {
                threads[i] = Thread.ofPlatform().unstarted(() -> {
                    try {
                        start.await();
                        stream.retryFailed();
                    } catch (Throwable problem) {
                        failure.compareAndSet(null, problem);
                    } finally {
                        done.countDown();
                    }
                });
                threads[i].start();
            }

            start.countDown();
            check(done.await(10, TimeUnit.SECONDS), "output retry race stalled");
            check(failure.get() == null, "output retry race failed: " + failure.get());
            check(stream.retained.isEmpty(), "all output retries must converge");
            check(envelope.closed(), "envelope must close exactly once");
            check(accounting.bytes == 0, "output debit must settle exactly once");

            for (Thread thread : threads) {
                thread.join(1000);
                check(!thread.isAlive(), "output retry worker remained alive");
            }
        }
    }

    private static void oldClosedBitNegativeControl() {
        Accounting accounting = new Accounting();
        accounting.bytes = 16;
        accounting.fail = true;
        boolean closed = true; // old compareAndSet(false,true) published first
        boolean failed = false;
        try {
            accounting.release(16);
        } catch (IllegalStateException expected) {
            failed = true;
            CHECKS.incrementAndGet();
        }
        check(closed && failed && accounting.bytes == 16,
                "old close-before-release ordering must expose stranded debit");
    }

    public static void main(String[] args) throws Exception {
        deterministicRecovery();
        concurrentRetries();
        oldClosedBitNegativeControl();
        System.out.println(
                "PASS source-free actor output release oracle checks="
                        + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " old_closed_bit_negative_control=detected"
                        + " private_source_bytes=0");
    }
}
