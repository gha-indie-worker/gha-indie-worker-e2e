package dev.oreslang.runtime;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.jupiter.api.Assertions.*;

/**
 * Repeated public channel/Future cancellation races. Every iteration starts
 * fresh source operations; no result is permitted to vanish between selection
 * commit and the mapped Future's completion notification.
 */
final class ChannelMappedCancellationStressTest {
    private static final int ROUNDS = 1_024;

    @Test
    @Timeout(90)
    void bufferedReadCommitAndCancellationRaceNeverLoseThePayload()
            throws Exception {
        for (int round = 0; round < ROUNDS; round++) {
            final int value = round + 1;
            var channel = new ChannelRuntime.Channel<Integer>(1);
            OresFuture<Integer> receiving = channel.readAsync();
            assertEquals(1, channel.pendingRegistrations());

            CountDownLatch start = new CountDownLatch(1);
            AtomicBoolean cancellationWon = new AtomicBoolean();
            AtomicBoolean sent = new AtomicBoolean();
            AtomicReference<Throwable> error = new AtomicReference<>();
            Thread sender = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(start);
                    sent.set(channel.tryWrite(value));
                } catch (Throwable failure) {
                    error.compareAndSet(null, failure);
                }
            });
            Thread canceller = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(start);
                    cancellationWon.set(receiving.cancel(false));
                } catch (Throwable failure) {
                    error.compareAndSet(null, failure);
                }
            });
            start.countDown();
            joinChecked(sender, canceller, error, round);
            assertTrue(sent.get(), "single-slot producer must commit in round " + round);
            if (cancellationWon.get()) {
                assertTrue(receiving.isCancelled(), "read canceled in round " + round);
                assertThrows(CancellationException.class, receiving::join);
                assertEquals(value, channel.tryRead().orElseThrow(),
                        "canceled read consumed committed payload in round " + round);
            } else {
                assertEquals(value, receiving.get(5, TimeUnit.SECONDS),
                        "read committed but result was lost in round " + round);
                assertTrue(channel.tryRead().isEmpty(),
                        "committed read was delivered twice in round " + round);
            }
            assertEquals(0, channel.pendingRegistrations(),
                    "read registration leaked in round " + round);
            assertEquals(0, channel.size(), "buffer not reconciled in round " + round);
        }
    }

    @Test
    @Timeout(90)
    void rendezvousWriteCommitAndCancellationRaceNeverDropOrDuplicate()
            throws Exception {
        for (int round = 0; round < ROUNDS; round++) {
            final int value = round + 1;
            var channel = new ChannelRuntime.Channel<Integer>(0);
            OresFuture<Void> sending = channel.writeAsync(value);
            assertEquals(1, channel.pendingRegistrations());

            CountDownLatch start = new CountDownLatch(1);
            AtomicBoolean cancellationWon = new AtomicBoolean();
            AtomicReference<OresFuture<Integer>> reader = new AtomicReference<>();
            AtomicReference<Throwable> error = new AtomicReference<>();
            Thread receiver = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(start);
                    reader.set(channel.readAsync());
                } catch (Throwable failure) {
                    error.compareAndSet(null, failure);
                }
            });
            Thread canceller = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(start);
                    cancellationWon.set(sending.cancel(false));
                } catch (Throwable failure) {
                    error.compareAndSet(null, failure);
                }
            });
            start.countDown();
            joinChecked(receiver, canceller, error, round);
            OresFuture<Integer> receiving = reader.get();
            assertNotNull(receiving, "missing reader in round " + round);
            if (cancellationWon.get()) {
                assertTrue(sending.isCancelled(), "canceled writer not terminal");
                assertThrows(CancellationException.class, sending::join);
                assertFalse(receiving.isDone(),
                        "canceled writer must not rendezvous with reader in round " + round);
                assertTrue(receiving.cancel(false),
                        "unmatched reader must be cancelable in round " + round);
            } else {
                assertNull(sending.get(5, TimeUnit.SECONDS),
                        "committed write failed in round " + round);
                assertEquals(value, receiving.get(5, TimeUnit.SECONDS),
                        "rendezvous payload lost in round " + round);
                assertFalse(receiving.cancel(false),
                        "committed rendezvous must reject late read cancellation");
            }
            assertEquals(0, channel.pendingRegistrations(),
                    "rendezvous registration leaked in round " + round);
            assertEquals(0, channel.size(), "rendezvous channel buffered data");
        }
    }

    @Test
    @Timeout(90)
    void closeAndMappedReadCancellationRaceRetainsOriginalTerminalCause()
            throws Exception {
        for (int round = 0; round < ROUNDS; round++) {
            var channel = new ChannelRuntime.Channel<Integer>(0);
            OresFuture<Integer> receiving = channel.readAsync();
            assertEquals(1, channel.pendingRegistrations());

            CountDownLatch start = new CountDownLatch(1);
            AtomicBoolean cancellationWon = new AtomicBoolean();
            AtomicReference<Throwable> error = new AtomicReference<>();
            Thread closer = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(start);
                    channel.close();
                } catch (Throwable failure) {
                    error.compareAndSet(null, failure);
                }
            });
            Thread canceller = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(start);
                    cancellationWon.set(receiving.cancel(false));
                } catch (Throwable failure) {
                    error.compareAndSet(null, failure);
                }
            });
            start.countDown();
            joinChecked(closer, canceller, error, round);
            if (cancellationWon.get()) {
                assertThrows(CancellationException.class, receiving::join,
                        "cancel should remain authoritative in round " + round);
            } else {
                ExecutionException failure = assertThrows(ExecutionException.class,
                        () -> receiving.get(5, TimeUnit.SECONDS));
                assertInstanceOf(ChannelRuntime.ChannelClosedException.class,
                        failure.getCause(),
                        "close must be propagated, not overwritten by cancellation");
            }
            assertTrue(channel.isClosed());
            assertEquals(0, channel.pendingRegistrations(),
                    "closed channel still holds read registration in round " + round);
        }
    }

    private static void awaitStart(CountDownLatch start) throws InterruptedException {
        if (!start.await(5, TimeUnit.SECONDS)) {
            throw new AssertionError("concurrent race start gate timed out");
        }
    }

    private static void joinChecked(
            Thread first, Thread second, AtomicReference<Throwable> error, int round)
            throws Exception {
        first.join(5_000);
        second.join(5_000);
        assertFalse(first.isAlive(), "first race worker stalled in round " + round);
        assertFalse(second.isAlive(), "second race worker stalled in round " + round);
        Throwable failure = error.get();
        if (failure != null) {
            throw new AssertionError("race worker failed in round " + round, failure);
        }
    }
}
