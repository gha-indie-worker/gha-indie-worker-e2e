package dev.oreslang.runtime;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.ReentrantLock;

import static org.junit.jupiter.api.Assertions.*;

/**
 * Regression for FAIR order sampling before coordinator admission.
 *
 * This test holds the real coordinator while two select calls queue for it.
 * Their constructors therefore run before either operation can commit. With
 * the old implementation both sampled cursor zero; under lock-owned sampling
 * the second admission sees the first winner's fairness rotation.
 */
final class ChannelSelectFairAdmissionRaceTest {

    @Test
    @Timeout(30)
    void queuedConcurrentSelectionsObservePriorFairCommit() throws Exception {
        Field field = ChannelRuntime.class.getDeclaredField("COORDINATOR");
        field.setAccessible(true);
        ReentrantLock coordinator = (ReentrantLock) field.get(null);

        for (int round = 0; round < 64; round++) {
            ChannelRuntime.Channel<Integer> first = new ChannelRuntime.Channel<>(2);
            ChannelRuntime.Channel<Integer> second = new ChannelRuntime.Channel<>(2);
            assertTrue(first.tryWrite(11));
            assertTrue(first.tryWrite(12));
            assertTrue(second.tryWrite(21));
            assertTrue(second.tryWrite(22));

            ChannelRuntime.SelectSet set = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.read(first), ChannelRuntime.read(second));

            AtomicReference<OresFuture<ChannelRuntime.SelectResult>> left =
                    new AtomicReference<>();
            AtomicReference<OresFuture<ChannelRuntime.SelectResult>> right =
                    new AtomicReference<>();
            AtomicReference<Throwable> error = new AtomicReference<>();
            CountDownLatch launch = new CountDownLatch(1);

            Thread a;
            Thread b;
            boolean bothQueued;
            coordinator.lock();
            try {
                a = Thread.startVirtualThread(() -> {
                    try {
                        if (!launch.await(5, TimeUnit.SECONDS))
                            throw new AssertionError("launch timed out");
                        left.set(set.selectAsync(ChannelRuntime.SelectPolicy.FAIR));
                    } catch (Throwable failure) {
                        error.compareAndSet(null, failure);
                    }
                });
                b = Thread.startVirtualThread(() -> {
                    try {
                        if (!launch.await(5, TimeUnit.SECONDS))
                            throw new AssertionError("launch timed out");
                        right.set(set.selectAsync(ChannelRuntime.SelectPolicy.FAIR));
                    } catch (Throwable failure) {
                        error.compareAndSet(null, failure);
                    }
                });
                launch.countDown();

                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (!(coordinator.hasQueuedThread(a) && coordinator.hasQueuedThread(b))
                        && System.nanoTime() < deadline) {
                    Thread.onSpinWait();
                }
                bothQueued = coordinator.hasQueuedThread(a) && coordinator.hasQueuedThread(b);
            } finally {
                coordinator.unlock();
            }

            a.join(5_000);
            b.join(5_000);
            assertTrue(bothQueued, "both select admissions must queue in round " + round);
            assertFalse(a.isAlive(), "first worker hung in round " + round);
            assertFalse(b.isAlive(), "second worker hung in round " + round);
            assertNull(error.get(), () -> "worker failed: " + error.get());
            assertNotNull(left.get());
            assertNotNull(right.get());

            ChannelRuntime.SelectResult firstWinner = left.get().get(5, TimeUnit.SECONDS);
            ChannelRuntime.SelectResult secondWinner = right.get().get(5, TimeUnit.SECONDS);
            assertNotEquals(firstWinner.index(), secondWinner.index(),
                    "concurrent FAIR admission reused a stale cursor in round " + round);
            assertEquals(0, firstWinner.index() + secondWinner.index() - 1,
                    "exactly one first arm and one second arm must commit");
            assertEquals(12, first.tryRead().orElseThrow());
            assertEquals(22, second.tryRead().orElseThrow());
            assertEquals(0, first.pendingRegistrations());
            assertEquals(0, second.pendingRegistrations());
        }
    }
}
