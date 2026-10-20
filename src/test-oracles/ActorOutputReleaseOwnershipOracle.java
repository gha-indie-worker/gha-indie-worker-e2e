import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class ActorOutputReleaseOwnershipOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class Envelope {
        boolean closed;
        long localDebit = 64;
        long aggregate = 0;
        int releaseAttempts;

        synchronized void close() {
            if (closed) return;
            releaseAttempts++;
            if (localDebit > aggregate) {
                throw new IllegalStateException("aggregate underflow");
            }
            aggregate -= localDebit;
            localDebit = 0;
            closed = true;
        }

        synchronized void repair() {
            aggregate = localDebit;
        }
    }

    private static final class DrainOwner {
        Envelope pending;
        boolean complete;

        synchronized void drain(Envelope envelope) {
            if (complete) return;
            if (pending != null) {
                pending.close();
                pending = null;
            }
            if (envelope != null) {
                pending = envelope;
                pending.close();
                pending = null;
            }
            complete = true;
        }
    }

    private static final class ReadOwner {
        Envelope envelope;
        boolean resultFailed;

        synchronized void accept(Envelope value) {
            envelope = value;
            try {
                value.close();
                envelope = null;
            } catch (IllegalStateException expected) {
                resultFailed = true;
            }
        }

        synchronized void retryCleanup() {
            if (envelope == null) return;
            envelope.close();
            envelope = null;
        }
    }

    private static final class BrokenEnvelope {
        boolean closed;
        long localDebit = 64;
        long aggregate = 0;

        synchronized void close() {
            if (closed) return;
            closed = true;
            localDebit = 0;
            if (64 > aggregate) {
                throw new IllegalStateException("aggregate underflow");
            }
            aggregate -= 64;
        }
    }

    private static void drainRetryRound() {
        Envelope envelope = new Envelope();
        DrainOwner owner = new DrainOwner();
        try {
            owner.drain(envelope);
            throw new AssertionError("expected drain underflow");
        } catch (IllegalStateException expected) {
            check(owner.pending == envelope, "drain lost dequeued envelope owner");
            check(!owner.complete, "failed drain forged cleanup completion");
            check(!envelope.closed, "failed release consumed envelope owner");
            check(envelope.localDebit == 64, "failed release lost local debit");
        }

        envelope.repair();
        owner.drain(null);
        owner.drain(null);
        check(owner.pending == null, "successful retry leaked pending envelope");
        check(owner.complete, "successful retry did not complete cleanup");
        check(envelope.closed, "successful retry did not consume envelope");
        check(envelope.localDebit == 0, "successful retry leaked local debit");
        check(envelope.aggregate == 0, "successful retry leaked aggregate debit");
        check(envelope.releaseAttempts == 2, "release did not retry exactly once");
    }

    private static void readRetryRound() {
        Envelope envelope = new Envelope();
        ReadOwner owner = new ReadOwner();
        owner.accept(envelope);
        check(owner.resultFailed, "consumer did not surface failed refund");
        check(owner.envelope == envelope, "consumer lost exact dequeued owner");
        check(!envelope.closed, "consumer failure consumed envelope");

        envelope.repair();
        owner.retryCleanup();
        owner.retryCleanup();
        check(owner.envelope == null, "consumer cleanup owner leaked");
        check(envelope.closed, "consumer cleanup retry did not close envelope");
        check(envelope.localDebit == 0, "consumer cleanup leaked local debit");
        check(envelope.aggregate == 0, "consumer cleanup leaked aggregate debit");
    }

    private static void concurrentEnvelopeRound(int workers) throws Exception {
        Envelope envelope = new Envelope();
        envelope.repair();
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> unexpected = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    envelope.close();
                } catch (Throwable problem) {
                    unexpected.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(unexpected.get() == null, "concurrent close raised " + unexpected.get());
        check(envelope.closed, "concurrent close did not consume envelope");
        check(envelope.localDebit == 0, "concurrent close leaked local debit");
        check(envelope.aggregate == 0, "concurrent close leaked aggregate debit");
        check(envelope.releaseAttempts == 1, "concurrent close duplicated release");
    }

    private static void negativeControl() {
        BrokenEnvelope broken = new BrokenEnvelope();
        try {
            broken.close();
        } catch (IllegalStateException expected) {
            // expected
        }
        broken.aggregate = 64;
        broken.close();
        check(broken.closed, "negative control did not seal envelope");
        check(broken.localDebit == 0, "negative control did not lose local owner");
        check(broken.aggregate == 64, "negative control failed to strand aggregate debit");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;

        for (int i = 0; i < retryRounds; i++) {
            drainRetryRound();
            readRetryRound();
        }
        negativeControl();
        for (int i = 0; i < raceRounds; i++) {
            concurrentEnvelopeRound(workers);
        }

        System.out.println(
                "PASS source-free actor output release ownership checks=" + checks
                + " retry_rounds=" + retryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=dequeued_owner_loss_detected"
                + " private_source_bytes=0");
    }
}
