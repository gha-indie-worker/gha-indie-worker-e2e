package dev.oreslang.runtime;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.jupiter.api.Assertions.*;

/**
 * The public channel read/write Future must share the source select's
 * cancellation decision, not cancel successfully after a committed delivery.
 */
final class ChannelMappedSelectionCancellationTest {

    /** Keep production mapSelection private; exercise it through test reflection. */
    @SuppressWarnings("unchecked")
    private static <T> OresFuture<T> mapSelection(
            OresFuture<ChannelRuntime.SelectResult> source,
            java.util.function.Function<ChannelRuntime.SelectResult, T> mapper) {
        try {
            var method = ChannelRuntime.class.getDeclaredMethod(
                    "mapSelection", OresFuture.class, java.util.function.Function.class);
            method.setAccessible(true);
            return (OresFuture<T>) method.invoke(null, source, mapper);
        } catch (ReflectiveOperationException reflectionFailure) {
            throw new AssertionError("private channel adapter is unavailable", reflectionFailure);
        }
    }

    @Test
    @Timeout(15)
    void committedSelectCannotBeCanceledBeforeItsMappedValueIsPublished()
            throws Exception {
        OresFuture<ChannelRuntime.SelectResult> source = new OresFuture<>();
        CountDownLatch sourceClaimed = new CountDownLatch(1);
        CountDownLatch releaseNotification = new CountDownLatch(1);
        AtomicReference<Throwable> producerFailure = new AtomicReference<>();

        // Registered first: hold the source's notification drain AFTER the
        // terminal CAS but BEFORE mapSelection's own observer is notified.
        source.whenCompleteRuntime((value, error) -> {
            sourceClaimed.countDown();
            try {
                if (!releaseNotification.await(5, TimeUnit.SECONDS)) {
                    throw new AssertionError("source notification gate timed out");
                }
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new AssertionError(interrupted);
            }
        });
        OresFuture<Integer> mapped = mapSelection(
                source, selected -> (Integer) selected.value());

        Thread producer = Thread.startVirtualThread(() -> {
            try {
                assertTrue(source.completeFromRuntime(new ChannelRuntime.SelectResult(
                        0, ChannelRuntime.SelectOperation.READ, 73)));
            } catch (Throwable failure) {
                producerFailure.set(failure);
            }
        });
        try {
            assertTrue(sourceClaimed.await(5, TimeUnit.SECONDS));
            assertTrue(source.isDone(), "selection has committed");
            assertFalse(mapped.isDone(), "mapped notification has not yet run");
            assertFalse(mapped.cancel(false),
                    "a committed selection must reject late mapped cancellation");
            assertFalse(mapped.isCancelled());
        } finally {
            releaseNotification.countDown();
            producer.join(5_000);
        }
        assertFalse(producer.isAlive(), "completion thread stuck");
        assertNull(producerFailure.get(), () -> "producer failed: " + producerFailure.get());
        assertEquals(73, mapped.get(5, TimeUnit.SECONDS));
    }

    @Test
    void cancelingPendingMappedReadCancelsUnderlyingSelection() {
        OresFuture<ChannelRuntime.SelectResult> source = new OresFuture<>();
        OresFuture<Integer> mapped = mapSelection(
                source, selected -> (Integer) selected.value());

        assertTrue(mapped.cancel(false),
                "pending mapped read should revoke its source selection");
        assertTrue(mapped.isCancelled());
        assertTrue(source.isCancelled());
        assertThrows(CancellationException.class, mapped::join);
        assertThrows(CancellationException.class, source::join);
        assertEquals(0, source.pendingRuntimeWaiterCount());
    }

    @Test
    void pendingReadAndWriteCancellationReleaseTheirChannelRegistrations() {
        ChannelRuntime.Channel<Integer> rendezvous = new ChannelRuntime.Channel<>(0);
        OresFuture<Integer> reading = rendezvous.readAsync();
        assertEquals(1, rendezvous.pendingRegistrations());
        assertTrue(reading.cancel(false));
        assertEquals(0, rendezvous.pendingRegistrations(),
                "canceled pending read must not retain a read arm");

        OresFuture<Void> writing = rendezvous.writeAsync(97);
        assertEquals(1, rendezvous.pendingRegistrations());
        assertTrue(writing.cancel(false));
        assertEquals(0, rendezvous.pendingRegistrations(),
                "canceled pending write must not retain a write arm");
        assertTrue(rendezvous.tryRead().isEmpty());
        assertFalse(rendezvous.tryWrite(98),
                "canceled read cannot rendezvous with a later producer");
    }

    @Test
    void committedBufferedReadAndWriteRefuseCancellationAndPreserveValue()
            throws Exception {
        ChannelRuntime.Channel<Integer> buffered = new ChannelRuntime.Channel<>(1);
        OresFuture<Void> sent = buffered.writeAsync(57);
        assertFalse(sent.cancel(false), "completed write cannot be canceled");
        assertNull(sent.get(5, TimeUnit.SECONDS));
        assertEquals(1, buffered.size(), "committed write must remain in buffer");

        OresFuture<Integer> read = buffered.readAsync();
        assertFalse(read.cancel(false), "completed read cannot be canceled");
        assertEquals(57, read.get(5, TimeUnit.SECONDS));
        assertEquals(0, buffered.size());
    }

    @Test
    @Timeout(15)
    void committedSelectionFailureCannotBeHiddenByLateCancellation()
            throws Exception {
        OresFuture<ChannelRuntime.SelectResult> source = new OresFuture<>();
        CountDownLatch sourceClaimed = new CountDownLatch(1);
        CountDownLatch releaseNotification = new CountDownLatch(1);
        AtomicReference<Throwable> producerFailure = new AtomicReference<>();
        source.whenCompleteRuntime((value, error) -> {
            sourceClaimed.countDown();
            try {
                if (!releaseNotification.await(5, TimeUnit.SECONDS)) {
                    throw new AssertionError("source notification gate timed out");
                }
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new AssertionError(interrupted);
            }
        });
        OresFuture<Integer> mapped = mapSelection(
                source, selected -> (Integer) selected.value());
        Thread producer = Thread.startVirtualThread(() -> {
            try {
                assertTrue(source.failFromRuntime(
                        new ChannelRuntime.ChannelClosedException("closed")));
            } catch (Throwable failure) {
                producerFailure.set(failure);
            }
        });
        try {
            assertTrue(sourceClaimed.await(5, TimeUnit.SECONDS));
            assertTrue(source.isDone());
            assertFalse(mapped.isDone());
            assertFalse(mapped.cancel(false),
                    "already-failed selection must not become mapped cancellation");
        } finally {
            releaseNotification.countDown();
            producer.join(5_000);
        }
        assertFalse(producer.isAlive());
        assertNull(producerFailure.get(), () -> "producer failed: " + producerFailure.get());
        ExecutionException failure = assertThrows(ExecutionException.class,
                () -> mapped.get(5, TimeUnit.SECONDS));
        assertInstanceOf(ChannelRuntime.ChannelClosedException.class,
                failure.getCause());
        assertEquals("closed", failure.getCause().getMessage());
    }
}
