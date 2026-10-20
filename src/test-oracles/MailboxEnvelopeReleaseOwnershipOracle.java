import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public final class MailboxEnvelopeReleaseOwnershipOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class Envelope {
        boolean released;
        long localDebit = 64;
        long aggregate = 0;
        int releaseAttempts;

        synchronized void close() {
            if (released) return;
            releaseAttempts++;
            if (localDebit > aggregate) {
                throw new IllegalStateException("aggregate underflow");
            }
            aggregate -= localDebit;
            localDebit = 0;
            released = true;
        }

        synchronized void repairAggregate() {
            aggregate = localDebit;
        }
    }

    private static final class DrainOwner {
        Envelope pending;
        boolean slotReleased;
        int queuedSlots = 1;

        synchronized void publish(Envelope envelope) {
            if (pending != null) throw new IllegalStateException("pending owner exists");
            pending = envelope;
            slotReleased = false;
        }

        synchronized void retry() {
            if (pending == null) return;
            if (!slotReleased) {
                if (queuedSlots <= 0) throw new IllegalStateException("slot underflow");
                queuedSlots--;
                slotReleased = true;
            }
            pending.close();
            pending = null;
            slotReleased = false;
        }
    }

    private static final class SuspendedReceive {
        Envelope current;
        Envelope suspended;
        boolean sourceSuspended;
        int queuedSlots = 1;

        synchronized void dequeue(Envelope envelope) {
            current = envelope;
            if (queuedSlots <= 0) throw new IllegalStateException("slot underflow");
            queuedSlots--;
        }

        synchronized void suspendAndUnwind() {
            sourceSuspended = true;
            if (suspended != null) throw new IllegalStateException("double suspended");
            suspended = current;
            current = null;
        }

        synchronized void complete() {
            if (suspended == null) throw new IllegalStateException("missing suspended owner");
            suspended.close();
            suspended = null;
            sourceSuspended = false;
        }
    }

    private static final class BrokenDrain {
        int queuedSlots = 1;
        Envelope dequeueAndLose(Envelope envelope) {
            queuedSlots--;
            return envelope;
        }
    }

    private static void drainRetryRound() {
        Envelope envelope = new Envelope();
        DrainOwner owner = new DrainOwner();
        owner.publish(envelope);
        try {
            owner.retry();
            throw new AssertionError("expected refund failure");
        } catch (IllegalStateException expected) {
            check(owner.pending == envelope, "failed drain lost exact envelope");
            check(owner.slotReleased, "completed slot release was not retained");
            check(owner.queuedSlots == 0, "slot release did not commit once");
            check(!envelope.released, "failed refund consumed envelope");
            check(envelope.localDebit == 64, "failed refund lost local debit");
        }

        envelope.repairAggregate();
        owner.retry();
        owner.retry();

        check(owner.pending == null, "retry leaked drain owner");
        check(owner.queuedSlots == 0, "retry double-released mailbox slot");
        check(envelope.released, "retry did not retire envelope");
        check(envelope.localDebit == 0, "retry leaked local debit");
        check(envelope.aggregate == 0, "retry leaked aggregate debit");
        check(envelope.releaseAttempts == 2, "refund did not retry exactly once");
    }

    private static void suspendedRound() {
        Envelope envelope = new Envelope();
        envelope.aggregate = 64;
        SuspendedReceive receive = new SuspendedReceive();
        receive.dequeue(envelope);
        receive.suspendAndUnwind();

        check(receive.current == null, "physical current owner did not unwind");
        check(receive.suspended == envelope, "logical receive lost exact envelope");
        check(receive.sourceSuspended, "logical suspension witness missing");
        check(envelope.localDebit == 64, "suspension released transport too early");
        check(envelope.aggregate == 64, "suspension refunded aggregate too early");

        receive.complete();
        check(receive.suspended == null, "completion leaked suspended owner");
        check(!receive.sourceSuspended, "completion left suspension witness");
        check(envelope.released, "completion did not retire envelope");
        check(envelope.localDebit == 0, "completion leaked local debit");
        check(envelope.aggregate == 0, "completion leaked aggregate debit");
    }

    private static void raceRound(int workers) throws Exception {
        Envelope envelope = new Envelope();
        envelope.aggregate = 64;
        DrainOwner owner = new DrainOwner();
        owner.publish(envelope);

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> unexpected = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    owner.retry();
                } catch (Throwable problem) {
                    unexpected.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(unexpected.get() == null, "concurrent retry raised " + unexpected.get());
        check(owner.pending == null, "concurrent retry leaked owner");
        check(owner.queuedSlots == 0, "concurrent retry double-released slot");
        check(envelope.released, "concurrent retry did not release envelope");
        check(envelope.releaseAttempts == 1, "concurrent retry duplicated refund");
    }

    private static void negativeControl() {
        Envelope envelope = new Envelope();
        BrokenDrain broken = new BrokenDrain();
        Envelope local = broken.dequeueAndLose(envelope);
        try {
            local.close();
        } catch (IllegalStateException expected) {
            // Simulate stack unwinding with no durable field owner.
        }
        local = null;
        envelope.aggregate = 64;
        check(broken.queuedSlots == 0, "negative control did not consume slot");
        check(!envelope.released, "negative control unexpectedly released envelope");
        check(envelope.localDebit == 64, "negative control did not strand debit");
        check(envelope.aggregate == 64, "negative control aggregate repair mismatch");
    }

    public static void main(String[] args) throws Exception {
        int retryRounds = 20_000;
        int suspendedRounds = 20_000;
        int raceRounds = 5_000;
        int workers = 12;

        for (int i = 0; i < retryRounds; i++) drainRetryRound();
        for (int i = 0; i < suspendedRounds; i++) suspendedRound();
        negativeControl();
        for (int i = 0; i < raceRounds; i++) raceRound(workers);

        System.out.println(
                "PASS source-free mailbox envelope ownership checks=" + checks
                + " retry_rounds=" + retryRounds
                + " suspended_rounds=" + suspendedRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=dequeued_envelope_owner_loss_detected"
                + " private_source_bytes=0");
    }
}
