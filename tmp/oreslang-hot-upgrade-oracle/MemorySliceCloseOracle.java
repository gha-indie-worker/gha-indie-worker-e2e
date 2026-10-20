import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

public final class MemorySliceCloseOracle {
    private static final AtomicLong CHECKS = new AtomicLong();

    private static final class InjectedFault extends Error {
        InjectedFault(String message) {
            super(message);
        }
    }

    private static final class Block {
        private byte[] memory;
        private boolean closed;

        Block(int bytes) {
            memory = new byte[bytes];
            Arrays.fill(memory, (byte) 0x5a);
        }

        synchronized void invalidate() {
            closed = true;
            if (memory == null) {
                return;
            }
            Arrays.fill(memory, (byte) 0);
            memory = null;
        }

        synchronized boolean closed() {
            return closed;
        }

        synchronized boolean detached() {
            return memory == null;
        }
    }

    private static final class SafeSlice {
        private final List<Block> blocks = new ArrayList<>();
        private boolean sealed;
        private boolean cleanupComplete;
        private boolean failAfterSealOnce;
        private long usedBytes;
        private long runtimeBytes;
        private int aggregateReleaseCount;

        SafeSlice(boolean failAfterSealOnce) {
            this.failAfterSealOnce = failAfterSealOnce;
        }

        synchronized Block allocate(int bytes) {
            if (sealed) {
                throw new IllegalStateException("sealed");
            }
            usedBytes += bytes;
            runtimeBytes += bytes;
            Block block = new Block(bytes);
            blocks.add(block);
            return block;
        }

        synchronized void close() {
            sealed = true;
            if (cleanupComplete) {
                return;
            }
            if (failAfterSealOnce) {
                failAfterSealOnce = false;
                throw new InjectedFault("after seal");
            }
            for (Block block : blocks) {
                block.invalidate();
            }
            blocks.clear();

            long bytes = usedBytes;
            if (bytes != 0L) {
                if (runtimeBytes < bytes) {
                    throw new IllegalStateException("runtime accounting underflow");
                }
                runtimeBytes -= bytes;
                aggregateReleaseCount++;
                usedBytes = 0L;
            }
            cleanupComplete = true;
        }
    }

    private static final class OldSealOnlySlice {
        private final List<Block> blocks = new ArrayList<>();
        private boolean sealed;
        private boolean failAfterSealOnce;
        private long usedBytes;
        private long runtimeBytes;

        OldSealOnlySlice(boolean failAfterSealOnce) {
            this.failAfterSealOnce = failAfterSealOnce;
        }

        synchronized Block allocate(int bytes) {
            if (sealed) {
                throw new IllegalStateException("sealed");
            }
            usedBytes += bytes;
            runtimeBytes += bytes;
            Block block = new Block(bytes);
            blocks.add(block);
            return block;
        }

        synchronized void close() {
            boolean firstClose = !sealed;
            sealed = true;
            if (firstClose) {
                if (failAfterSealOnce) {
                    failAfterSealOnce = false;
                    throw new InjectedFault("after seal");
                }
                for (Block block : blocks) {
                    block.invalidate();
                }
                blocks.clear();
            }
            long bytes = usedBytes;
            if (bytes != 0L) {
                runtimeBytes -= bytes;
                usedBytes = 0L;
            }
        }
    }

    private static void check(boolean condition, String message) {
        CHECKS.incrementAndGet();
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void deterministicRetry() {
        SafeSlice slice = new SafeSlice(true);
        Block block = slice.allocate(64);
        byte[] original = block.memory;

        try {
            slice.close();
            throw new AssertionError("expected injected close failure");
        } catch (InjectedFault expected) {
            CHECKS.incrementAndGet();
        }

        check(slice.sealed, "first failure must retain sealed admission");
        check(!slice.cleanupComplete, "seal is not cleanup completion");
        check(slice.usedBytes == 64L, "failed cleanup must retain local debit");
        check(slice.runtimeBytes == 64L, "failed cleanup must retain runtime debit");
        check(!block.closed(), "fault occurs before block invalidation");

        slice.close();

        check(slice.cleanupComplete, "retry must publish cleanup completion");
        check(slice.usedBytes == 0L, "retry must clear local debit");
        check(slice.runtimeBytes == 0L, "retry must refund runtime debit");
        check(slice.aggregateReleaseCount == 1, "aggregate debit must release exactly once");
        check(block.closed(), "retry must close direct block");
        check(block.detached(), "retry must detach direct block");
        for (byte value : original) {
            check(value == 0, "retry must zero private backing bytes");
        }
    }

    private static void negativeControl() {
        OldSealOnlySlice old = new OldSealOnlySlice(true);
        Block block = old.allocate(64);
        byte[] original = block.memory;

        try {
            old.close();
            throw new AssertionError("expected old close failure");
        } catch (InjectedFault expected) {
            CHECKS.incrementAndGet();
        }
        old.close();

        boolean leakedBacking = !block.closed()
                && !block.detached()
                && Arrays.stream(toInts(original)).anyMatch(value -> value != 0);
        check(old.runtimeBytes == 0L,
                "old control should falsely refund runtime accounting");
        check(leakedBacking,
                "old seal-only retry must expose detached-cleanup bug");
    }

    private static int[] toInts(byte[] bytes) {
        int[] values = new int[bytes.length];
        for (int i = 0; i < bytes.length; i++) {
            values[i] = bytes[i];
        }
        return values;
    }

    private static void concurrentRetryRaces() throws Exception {
        final int rounds = 5_000;
        final int workers = 4;
        ExecutorService pool = Executors.newFixedThreadPool(workers);
        try {
            for (int round = 0; round < rounds; round++) {
                SafeSlice slice = new SafeSlice(true);
                Block block = slice.allocate(32);
                byte[] original = block.memory;
                try {
                    slice.close();
                    throw new AssertionError("expected injected close failure");
                } catch (InjectedFault expected) {
                    CHECKS.incrementAndGet();
                }

                CountDownLatch start = new CountDownLatch(1);
                CountDownLatch done = new CountDownLatch(workers);
                for (int worker = 0; worker < workers; worker++) {
                    pool.execute(() -> {
                        try {
                            start.await();
                            slice.close();
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(interrupted);
                        } finally {
                            done.countDown();
                        }
                    });
                }
                start.countDown();
                check(done.await(10, TimeUnit.SECONDS),
                        "concurrent cleanup retry stalled");
                check(slice.cleanupComplete,
                        "concurrent retries must converge on cleanup completion");
                check(slice.aggregateReleaseCount == 1,
                        "concurrent retries must release aggregate debit once");
                check(slice.usedBytes == 0L && slice.runtimeBytes == 0L,
                        "concurrent retries must settle all debit ownership");
                check(block.closed() && block.detached(),
                        "concurrent retries must detach the direct block");
                for (byte value : original) {
                    check(value == 0, "concurrent retry must zero private bytes");
                }
            }
        } finally {
            pool.shutdown();
            check(pool.awaitTermination(30, TimeUnit.SECONDS),
                    "oracle worker pool did not terminate");
        }
    }

    public static void main(String[] args) throws Exception {
        deterministicRetry();
        negativeControl();
        concurrentRetryRaces();
        System.out.println(
                "PASS source-free private memory finalization oracle checks="
                        + CHECKS.get()
                        + " race_rounds=5000 workers=4"
                        + " old_seal_only_negative_control=detected"
                        + " private_source_bytes=0");
    }
}
