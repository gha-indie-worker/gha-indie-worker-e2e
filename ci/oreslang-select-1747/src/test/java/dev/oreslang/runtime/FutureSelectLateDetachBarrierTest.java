package dev.oreslang.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

/** Deterministically holds the exact losing-listener detachment boundary. */
@Timeout(20)
final class FutureSelectLateDetachBarrierTest {
    private static void await(CountDownLatch latch) {
        try {
            if (!latch.await(8, TimeUnit.SECONDS)) {
                throw new AssertionError("timed out at select attachment boundary");
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError("attachment interrupted", interrupted);
        }
    }

    private static final class HeldDetach implements AutoCloseable {
        private final CountDownLatch attached = new CountDownLatch(1);
        private final CountDownLatch finishAttachment = new CountDownLatch(1);
        private final CountDownLatch aboutToDetach = new CountDownLatch(1);
        private final CountDownLatch finishDetachment = new CountDownLatch(1);
        private final AtomicReference<Throwable> error = new AtomicReference<>();
        private final AtomicReference<OresFuture<ChannelRuntime.SelectResult>> selected =
                new AtomicReference<>();
        private final Thread worker;

        HeldDetach(ChannelRuntime.SelectSet set) {
            worker = Thread.startVirtualThread(() -> {
                try {
                    set.selectAsyncForTesting(ChannelRuntime.SelectPolicy.PRIORITY,
                            selected::set,
                            () -> {
                                attached.countDown();
                                await(finishAttachment);
                            },
                            () -> {
                                aboutToDetach.countDown();
                                await(finishDetachment);
                            });
                } catch (Throwable failure) {
                    error.set(failure);
                }
            });
        }

        OresFuture<ChannelRuntime.SelectResult> afterAttachment() {
            await(attached);
            return selected.get();
        }

        void enterDetachment() {
            finishAttachment.countDown();
            await(aboutToDetach);
        }

        @Override
        public void close() throws Exception {
            finishAttachment.countDown();
            finishDetachment.countDown();
            worker.join(10_000);
            assertFalse(worker.isAlive(), "select attachment thread did not finish");
            if (error.get() != null) {
                throw new AssertionError("select attachment failed", error.get());
            }
        }
    }

    @Test
    void committedChannelWinnerCannotPublishWhileLoserIsAboutToDetach()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        OresFuture<ChannelRuntime.SelectResult> selected;
        try (var held = new HeldDetach(ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel)))) {
            selected = held.afterAttachment();
            assertEquals(1, producer.pendingRuntimeWaiterCount());
            assertTrue(channel.tryWrite(62));
            held.enterDetachment();
            assertEquals(1, producer.pendingRuntimeWaiterCount(),
                    "loser should remain linked until actual detachment");
            assertFalse(selected.isDone(),
                    "committed channel transfer published before waiter detach");
            assertFalse(selected.cancel(false),
                    "a committed winner must reject late cancellation");
        }
        var result = selected.get(5, TimeUnit.SECONDS);
        assertEquals(1, result.index());
        assertEquals(62, result.value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
        assertFalse(producer.isDone());
    }

    @Test
    void channelCloseFailureCannotPublishWhileLoserIsAboutToDetach()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(0);
        var cause = new IllegalStateException("closed");
        OresFuture<ChannelRuntime.SelectResult> selected;
        try (var held = new HeldDetach(ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel)))) {
            selected = held.afterAttachment();
            channel.close(cause);
            held.enterDetachment();
            assertEquals(1, producer.pendingRuntimeWaiterCount());
            assertFalse(selected.isDone(), "close failure escaped detach barrier");
            assertFalse(selected.cancel(false));
        }
        var failure = assertThrows(CompletionException.class, selected::join);
        assertTrue(failure.getCause() instanceof ChannelRuntime.ChannelClosedException);
        assertEquals(cause, failure.getCause().getCause());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void cancellationRevokesAdmissionEvenWhenLateDetachHasNotFinished()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        OresFuture<ChannelRuntime.SelectResult> selected;
        try (var held = new HeldDetach(ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel)))) {
            selected = held.afterAttachment();
            assertTrue(selected.cancel(false));
            held.enterDetachment();
            assertTrue(selected.isCancelled());
            assertEquals(1, producer.pendingRuntimeWaiterCount());
            assertEquals(0, channel.pendingRegistrations());
            assertTrue(channel.tryWrite(81));
            assertFalse(producer.isDone());
        }
        assertThrows(CancellationException.class, selected::join);
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(81, channel.tryRead().orElseThrow());
    }
    @Test
    void throwingTestDetachObserverStillReleasesWaiterBeforePublishing()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var observed = new AtomicReference<OresFuture<ChannelRuntime.SelectResult>>();
        var failure = new IllegalStateException("detachment observer failed");
        var thrown = assertThrows(IllegalStateException.class,
                () -> ChannelRuntime.SelectSet.of(
                        ChannelRuntime.await(producer), ChannelRuntime.read(channel))
                        .selectAsyncForTesting(ChannelRuntime.SelectPolicy.PRIORITY,
                                observed::set,
                                () -> {
                                    assertTrue(channel.tryWrite(93));
                                    assertFalse(observed.get().isDone());
                                },
                                () -> { throw failure; }));
        assertEquals(failure, thrown);
        assertEquals(93, observed.get().get(5, TimeUnit.SECONDS).value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
        assertFalse(producer.isDone());
    }

}
