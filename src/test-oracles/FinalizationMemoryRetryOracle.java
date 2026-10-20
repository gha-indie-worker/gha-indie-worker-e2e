import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class FinalizationMemoryRetryOracle {
    private static long checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static final class MemorySliceModel {
        boolean closed;
        long localBytes;
        long globalBytes;

        MemorySliceModel(long bytes) {
            localBytes = bytes;
            globalBytes = bytes;
        }

        synchronized void close() {
            closed = true;
            long bytes = localBytes;
            if (bytes == 0) return;
            if (bytes > globalBytes) {
                throw new IllegalStateException("aggregate underflow");
            }
            globalBytes -= bytes;
            localBytes = 0;
        }
    }

    private static final class BrokenMemorySliceModel {
        boolean closed;
        long localBytes;
        long globalBytes;

        BrokenMemorySliceModel(long bytes) {
            localBytes = bytes;
            globalBytes = bytes;
        }

        synchronized void close() {
            if (closed) return;
            closed = true;
            long bytes = localBytes;
            localBytes = 0;
            if (bytes > globalBytes) {
                throw new IllegalStateException("aggregate underflow");
            }
            globalBytes -= bytes;
        }
    }

    private static final class FinalizationModel {
        boolean registryPresent = true;
        boolean recoveryOwner;
        boolean finalizationActive;
        boolean physicallyFinalized;
        boolean slotDebited;
        boolean exitHookInvoked;
        int exitHookCalls;
        int boundaryAttempts;
        boolean faultOnce = true;

        synchronized void stop() {
            if (physicallyFinalized) return;
            if (!registryPresent && !recoveryOwner) return;
            if (finalizationActive) return;

            finalizationActive = true;
            recoveryOwner = true;
            try {
                if (!exitHookInvoked) {
                    exitHookInvoked = true;
                    exitHookCalls++;
                }
                registryPresent = false;
                if (!slotDebited) slotDebited = true;
                boundaryAttempts++;
                if (faultOnce) {
                    faultOnce = false;
                    throw new OutOfMemoryError("post-unregister fault");
                }
                physicallyFinalized = true;
                recoveryOwner = false;
            } finally {
                finalizationActive = false;
            }
        }
    }

    private static void memoryRetryRound() {
        MemorySliceModel model = new MemorySliceModel(16);
        model.globalBytes = 0;
        try {
            model.close();
            throw new AssertionError("expected aggregate underflow");
        } catch (IllegalStateException expected) {
            check(model.closed, "guest access must remain revoked");
            check(model.localBytes == 16, "failed close lost refund authority");
            check(model.globalBytes == 0, "failed close mutated global counter");
        }
        model.globalBytes = 16;
        model.close();
        model.close();
        check(model.localBytes == 0, "retry did not consume local debit");
        check(model.globalBytes == 0, "retry did not refund global debit");
    }

    private static void brokenMemoryNegativeControl() {
        BrokenMemorySliceModel model = new BrokenMemorySliceModel(16);
        model.globalBytes = 0;
        try {
            model.close();
        } catch (IllegalStateException expected) {
            // expected
        }
        model.globalBytes = 16;
        model.close();
        check(model.closed, "negative control did not close");
        check(model.localBytes == 0, "negative control did not lose local authority");
        check(model.globalBytes == 16,
                "negative control failed to demonstrate stranded aggregate debit");
    }

    private static void concurrentFinalizationRound(int workers) throws Exception {
        FinalizationModel model = new FinalizationModel();
        try {
            model.stop();
            throw new AssertionError("expected post-unregister fault");
        } catch (OutOfMemoryError expected) {
            check(!model.registryPresent, "registry should already be absent");
            check(model.recoveryOwner, "retry owner was not retained");
            check(!model.physicallyFinalized, "fault forged physical completion");
            check(model.exitHookCalls == 1, "exit hook was not invoked exactly once");
        }

        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] threads = new Thread[workers];
        for (int i = 0; i < workers; i++) {
            threads[i] = Thread.ofPlatform().unstarted(() -> {
                try {
                    start.await();
                    model.stop();
                } catch (Throwable problem) {
                    failure.compareAndSet(null, problem);
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join();

        check(failure.get() == null, "concurrent retry raised " + failure.get());
        check(model.physicallyFinalized, "concurrent retry did not finalize");
        check(!model.recoveryOwner, "recovery owner leaked");
        check(model.exitHookCalls == 1, "concurrent retry duplicated exit hook");
        check(model.slotDebited, "actor slot was not debited");
        check(model.boundaryAttempts == 2, "more than one recovery crossed boundary");
    }

    public static void main(String[] args) throws Exception {
        final int memoryRounds = 20_000;
        final int raceRounds = 5_000;
        final int workers = 12;

        for (int i = 0; i < memoryRounds; i++) memoryRetryRound();
        brokenMemoryNegativeControl();
        for (int i = 0; i < raceRounds; i++) concurrentFinalizationRound(workers);

        System.out.println(
                "PASS source-free finalization+memory retry checks=" + checks
                + " memory_rounds=" + memoryRounds
                + " race_rounds=" + raceRounds
                + " workers=" + workers
                + " negative_control=stranded_refund_detected"
                + " private_source_bytes=0");
    }
}
