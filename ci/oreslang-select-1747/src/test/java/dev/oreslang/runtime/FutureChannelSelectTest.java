package dev.oreslang.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

/** Future completion and channel transfer must share one atomic select winner. */
final class FutureChannelSelectTest {
    @Test
    void pendingFutureWakesHeterogeneousSelectWithoutConsumingLosingChannel() {
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var producer = new OresFuture<Integer>();
        var set = ChannelRuntime.SelectSet.of(
                ChannelRuntime.read(channel), ChannelRuntime.await(producer));

        var selected = set.selectAsync(ChannelRuntime.SelectPolicy.FAIR);
        assertFalse(selected.isDone());
        assertEquals(1, producer.pendingRuntimeWaiterCount());

        assertTrue(producer.completeFromRuntime(42));
        var result = selected.join();
        assertEquals(1, result.index());
        assertEquals(ChannelRuntime.SelectOperation.FUTURE, result.operation());
        assertEquals(42, result.value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());

        assertTrue(channel.tryWrite(7));
        assertEquals(7, channel.tryRead().orElseThrow());
    }

    @Test
    void channelWinnerDetachesFutureWaiterAndDoesNotCancelSharedProducer() {
        var channel = new ChannelRuntime.Channel<String>(1);
        var producer = new OresFuture<String>();
        var selected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer),
                ChannelRuntime.read(channel)).selectAsync();

        assertEquals(1, producer.pendingRuntimeWaiterCount());
        assertTrue(channel.tryWrite("mail"));

        assertEquals(1, selected.join().index());
        assertEquals("mail", selected.join().value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone());

        assertTrue(producer.completeFromRuntime("later"));
        assertEquals("mail", selected.join().value());
    }

    @Test
    void cancellingWhileFutureAttachmentIsPausedEventuallyDetachesLateWaiter()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var attached = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var selectedRef = new java.util.concurrent.atomic.AtomicReference<
                OresFuture<ChannelRuntime.SelectResult>>();
        var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
        var selection = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel));

        Thread worker = Thread.startVirtualThread(() -> {
            try {
                selection.selectAsyncForTesting(
                        ChannelRuntime.SelectPolicy.PRIORITY,
                        selectedRef::set,
                        () -> {
                            attached.countDown();
                            awaitStart(release);
                        });
            } catch (Throwable failure) {
                workerFailure.set(failure);
            }
        });
        try {
            assertTrue(attached.await(5, TimeUnit.SECONDS),
                    "listener was never attached");
            var selected = selectedRef.get();
            assertEquals(1, producer.pendingRuntimeWaiterCount());
            assertTrue(selected.cancel(false));
            assertTrue(selected.isCancelled());
            assertEquals(0, channel.pendingRegistrations());
            // Cancellation must not consume a channel message or cancel the
            // independently-owned producer, even while cleanup is in flight.
            assertTrue(channel.tryWrite(31));
            assertFalse(producer.isDone());
        } finally {
            release.countDown();
            worker.join(5_000);
        }
        assertFalse(worker.isAlive(), "listener attachment failed to drain");
        if (workerFailure.get() != null) {
            throw new AssertionError("listener attachment failed", workerFailure.get());
        }
        // Cancellation may become observable before in-flight attachments
        // return; late handles must be detached before that thread exits.
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
        assertEquals(31, channel.tryRead().orElseThrow());
    }

    @Test
    void committedChannelWinnerIsNotPublishedBeforeLateWaiterDetaches()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var attached = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var selectedRef = new java.util.concurrent.atomic.AtomicReference<
                OresFuture<ChannelRuntime.SelectResult>>();
        var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
        var selection = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel));

        Thread worker = Thread.startVirtualThread(() -> {
            try {
                selection.selectAsyncForTesting(
                        ChannelRuntime.SelectPolicy.PRIORITY,
                        selectedRef::set,
                        () -> {
                            attached.countDown();
                            awaitStart(release);
                        });
            } catch (Throwable failure) {
                workerFailure.set(failure);
            }
        });
        try {
            assertTrue(attached.await(5, TimeUnit.SECONDS),
                    "listener was never attached");
            var selected = selectedRef.get();
            assertEquals(1, producer.pendingRuntimeWaiterCount());
            assertTrue(channel.tryWrite(42));
            assertFalse(selected.isDone(),
                    "winner published before late waiter was detached");
            assertFalse(producer.isDone());
        } finally {
            release.countDown();
            worker.join(5_000);
        }
        assertFalse(worker.isAlive(), "listener attachment failed to drain");
        if (workerFailure.get() != null) {
            throw new AssertionError("listener attachment failed", workerFailure.get());
        }
        var winner = selectedRef.get().get(5, TimeUnit.SECONDS);
        assertEquals(1, winner.index());
        assertEquals(42, winner.value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void producerSettlementDuringPausedAttachmentRejectsLateCancellation()
            throws Exception {
        var producer = new OresFuture<Integer>();
        var attached = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var selectedRef = new java.util.concurrent.atomic.AtomicReference<
                OresFuture<ChannelRuntime.SelectResult>>();
        var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
        var selection = ChannelRuntime.SelectSet.of(ChannelRuntime.await(producer));
        Thread worker = Thread.startVirtualThread(() -> {
            try {
                selection.selectAsyncForTesting(
                        ChannelRuntime.SelectPolicy.PRIORITY,
                        selectedRef::set,
                        () -> {
                            attached.countDown();
                            awaitStart(release);
                        });
            } catch (Throwable failure) {
                workerFailure.set(failure);
            }
        });
        try {
            assertTrue(attached.await(5, TimeUnit.SECONDS));
            assertTrue(producer.completeFromRuntime(77));
            assertFalse(selectedRef.get().isDone(),
                    "terminal result overtook listener attachment");
            assertFalse(selectedRef.get().cancel(false),
                    "committed producer winner must reject cancellation");
        } finally {
            release.countDown();
            worker.join(5_000);
        }
        assertFalse(worker.isAlive(), "listener attachment failed to drain");
        if (workerFailure.get() != null) {
            throw new AssertionError("listener attachment failed", workerFailure.get());
        }
        var winner = selectedRef.get().get(5, TimeUnit.SECONDS);
        assertEquals(0, winner.index());
        assertEquals(77, winner.value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
    }


    @Test
    void cancellationBeforeRegistrationNeverAttachesOrConsumes() {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var selected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel))
                .selectAsyncForTesting(ChannelRuntime.SelectPolicy.PRIORITY,
                        future -> assertTrue(future.cancel(false)),
                        () -> { throw new AssertionError("cancelled registration ran"); });
        assertTrue(selected.isCancelled());
        assertThrows(CancellationException.class, selected::join);
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
        assertFalse(producer.isDone());
        assertTrue(channel.tryWrite(19));
        assertEquals(19, channel.tryRead().orElseThrow());
    }

    @Test
    void hundredCancellationVersusProducerFailureRacesLeaveNoWaiters()
            throws Exception {
        for (int round = 0; round < 100; round++) {
            var producer = new OresFuture<Integer>();
            var channel = new ChannelRuntime.Channel<Integer>(0);
            var failure = new IllegalStateException("round " + round);
            var selected = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.await(producer), ChannelRuntime.await(producer),
                    ChannelRuntime.read(channel))
                    .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
            assertEquals(2, producer.pendingRuntimeWaiterCount());
            var go = new CountDownLatch(1);
            var errors = new java.util.concurrent.atomic.AtomicReference<Throwable>();
            Thread canceller = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(go);
                    selected.cancel(false);
                } catch (Throwable ex) {
                    errors.compareAndSet(null, ex);
                }
            });
            Thread settler = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(go);
                    if (!producer.failFromRuntime(failure)) {
                        throw new AssertionError("duplicate producer settlement");
                    }
                } catch (Throwable ex) {
                    errors.compareAndSet(null, ex);
                }
            });
            go.countDown();
            canceller.join(5_000);
            settler.join(5_000);
            assertFalse(canceller.isAlive(), "cancel stalled: " + round);
            assertFalse(settler.isAlive(), "settlement stalled: " + round);
            if (errors.get() != null) {
                throw new AssertionError("race failed in round " + round, errors.get());
            }
            assertTrue(selected.isDone(), "pending selection in round " + round);
            if (selected.isCancelled()) {
                assertThrows(CancellationException.class, selected::join);
            } else {
                var result = assertThrows(CompletionException.class, selected::join);
                assertEquals(failure, result.getCause());
            }
            assertEquals(0, producer.pendingRuntimeWaiterCount(),
                    "retained Future listener: " + round);
            assertEquals(0, channel.pendingRegistrations(),
                    "retained channel credit: " + round);
        }
    }

    @Test
    void cancelledSelectionDetachesAllFutureWaitersWithoutCancellingSources() {
        var first = new OresFuture<Integer>();
        var second = new OresFuture<Integer>();
        var selected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(first), ChannelRuntime.await(second)).selectAsync();

        assertEquals(1, first.pendingRuntimeWaiterCount());
        assertEquals(1, second.pendingRuntimeWaiterCount());
        assertTrue(selected.cancel(false));
        assertTrue(selected.isCancelled());
        assertThrows(CancellationException.class, selected::join);
        assertEquals(0, first.pendingRuntimeWaiterCount());
        assertEquals(0, second.pendingRuntimeWaiterCount());
        assertFalse(first.isDone());
        assertFalse(second.isDone());
    }

    @Test
    void priorityAndFairnessApplyAcrossAlreadyReadyChannelsAndFutures() {
        var channel = new ChannelRuntime.Channel<Integer>(2);
        assertTrue(channel.tryWrite(11));
        var future = OresFuture.completed(22);
        var set = ChannelRuntime.SelectSet.of(
                ChannelRuntime.read(channel), ChannelRuntime.await(future));

        var first = set.selectAsync(ChannelRuntime.SelectPolicy.FAIR).join();
        assertEquals(0, first.index());
        assertEquals(11, first.value());

        assertTrue(channel.tryWrite(33));
        var second = set.selectAsync(ChannelRuntime.SelectPolicy.FAIR).join();
        assertEquals(1, second.index());
        assertEquals(22, second.value());
        assertEquals(33, channel.tryRead().orElseThrow());
    }

    @Test
    @SuppressWarnings("unchecked")
    void concurrentReusableFairSelectCapturesCursorAtAdmission() throws Exception {
        var first = OresFuture.completed(10);
        var second = OresFuture.completed(20);
        var set = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(first), ChannelRuntime.await(second));

        // Deterministically suspend a selection between construction and
        // admission, the same window that overlapping host threads can hit.
        // This private registration is intentionally exercised by reflection
        // rather than exposing an unsafe test-only runtime entrypoint.
        var registrationType = Class.forName(
                "dev.oreslang.runtime.ChannelRuntime$SelectRegistration");
        var constructor = registrationType.getDeclaredConstructor(
                ChannelRuntime.SelectSet.class, ChannelRuntime.SelectPolicy.class);
        constructor.setAccessible(true);
        Object deferred = constructor.newInstance(
                set, ChannelRuntime.SelectPolicy.FAIR);

        assertEquals(0, set.trySelect(ChannelRuntime.SelectPolicy.FAIR)
                .orElseThrow().index());

        var start = registrationType.getDeclaredMethod("start", boolean.class);
        start.setAccessible(true);
        start.invoke(deferred, true);
        var futureField = registrationType.getDeclaredField("future");
        futureField.setAccessible(true);
        var deferredResult = (OresFuture<ChannelRuntime.SelectResult>)
                futureField.get(deferred);
        assertEquals(1, deferredResult.get(5, TimeUnit.SECONDS).index());
        assertEquals(20, deferredResult.join().value());
        assertEquals(0, set.trySelect(ChannelRuntime.SelectPolicy.FAIR)
                .orElseThrow().index());
    }

    @Test
    void settledFailedFutureIsSelectableButPreservesItsFailure() {
        var failure = new IllegalStateException("upstream failed");
        var future = OresFuture.<Integer>failed(failure);
        var channel = new ChannelRuntime.Channel<Integer>(0);
        var selected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(future), ChannelRuntime.read(channel))
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        var thrown = assertThrows(CompletionException.class, selected::join);
        assertEquals(failure, thrown.getCause());
        assertFalse(channel.tryRead().isPresent());
    }

    @Test
    void concurrentChannelWriteAndFutureSettlementChooseExactlyOneWinner() throws Exception {
        for (int round = 0; round < 64; round++) {
            var channel = new ChannelRuntime.Channel<Integer>(1);
            var producer = new OresFuture<Integer>();
            var selected = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.read(channel), ChannelRuntime.await(producer))
                    .selectAsync(ChannelRuntime.SelectPolicy.FAIR);
            var go = new CountDownLatch(1);
            var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
            Thread writer = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(go);
                    assertTrue(channel.tryWrite(71));
                } catch (Throwable failure) {
                    workerFailure.compareAndSet(null, failure);
                }
            });
            Thread resolver = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(go);
                    assertTrue(producer.completeFromRuntime(82));
                } catch (Throwable failure) {
                    workerFailure.compareAndSet(null, failure);
                }
            });

            go.countDown();
            writer.join(5_000);
            resolver.join(5_000);
            assertFalse(writer.isAlive(), "writer stalled in round " + round);
            assertFalse(resolver.isAlive(), "resolver stalled in round " + round);
            if (workerFailure.get() != null) {
                throw new AssertionError("select race worker failed in round " + round,
                        workerFailure.get());
            }
            var result = selected.get(5, TimeUnit.SECONDS);
            if (result.index() == 0) {
                assertEquals(71, result.value());
                assertTrue(channel.tryRead().isEmpty());
                assertEquals(82, producer.join());
            } else {
                assertEquals(1, result.index());
                assertEquals(82, result.value());
                assertEquals(71, channel.tryRead().orElseThrow());
            }
            assertEquals(0, producer.pendingRuntimeWaiterCount());
        }
    }

    private static void awaitStart(CountDownLatch gate) {
        try {
            if (!gate.await(5, TimeUnit.SECONDS)) {
                throw new AssertionError("concurrent select test gate timed out");
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError("concurrent select test interrupted", interrupted);
        }
    }

    @Test
    void tooManySelectableDescriptorsFailWithoutConsumingAnyPayload() {
        var channel = new ChannelRuntime.Channel<Integer>(1);
        assertTrue(channel.tryWrite(91));
        var cases = new java.util.ArrayList<ChannelRuntime.SelectCase>();
        for (int i = 0; i < 257; i++) cases.add(ChannelRuntime.read(channel));
        assertThrows(ChannelRuntime.SelectAdmissionException.class,
                () -> new ChannelRuntime.SelectSet(cases));
        assertEquals(91, channel.tryRead().orElseThrow());
    }

    @Test
    void perChannelWaiterQuotaStillAppliesToMixedFutureSelections() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        var first = ChannelRuntime.SelectSet.of(ChannelRuntime.read(channel))
                .selectAsync(ChannelRuntime.SelectPolicy.FAIR);
        var sharedProducer = new OresFuture<Integer>();

        // Global pending-selection quota is not exhausted, but this channel's
        // own one-waiter budget is. Reject the entire heterogeneous selection.
        var rejected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(sharedProducer),
                ChannelRuntime.read(channel))
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        var failure = assertThrows(CompletionException.class, rejected::join);
        assertTrue(failure.getCause()
                instanceof ChannelRuntime.ChannelBackpressureException);
        assertEquals(0, sharedProducer.pendingRuntimeWaiterCount());
        assertFalse(sharedProducer.isDone());
        assertFalse(first.isDone());

        // Failed arbitration may neither steal the queued read nor retain a
        // Future subscriber. The earlier registration is still deliverable.
        assertEquals(ChannelRuntime.SelectOperation.WRITE,
                ChannelRuntime.SelectSet.of(ChannelRuntime.write(channel, 31))
                        .selectAsync().join().operation());
        assertEquals(31, first.join().value());
        assertTrue(sharedProducer.completeFromRuntime(99));
    }

    @Test
    void readyFutureWinsBeforePerChannelQuotaIsEvaluated() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        var waiting = ChannelRuntime.SelectSet.of(ChannelRuntime.read(channel))
                .selectAsync(ChannelRuntime.SelectPolicy.FAIR);
        try {
            var selected = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.await(OresFuture.completed(7)),
                    ChannelRuntime.read(channel))
                    .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY).join();
            assertEquals(0, selected.index());
            assertEquals(7, selected.value());
            assertFalse(waiting.isDone());
        } finally {
            waiting.cancel(false);
        }
    }

    @Test
    void fullPendingQuotaCannotSuppressAnImmediatelyReadyFutureOrRead() {
        var pending = new java.util.ArrayList<OresFuture<ChannelRuntime.SelectResult>>();
        try {
            for (int i = 0; i < 4096; i++) {
                var channel = new ChannelRuntime.Channel<Integer>(0);
                pending.add(ChannelRuntime.SelectSet.of(ChannelRuntime.read(channel))
                        .selectAsync(ChannelRuntime.SelectPolicy.FAIR));
            }
            var full = new ChannelRuntime.Channel<Integer>(0);
            var rejected = ChannelRuntime.SelectSet.of(ChannelRuntime.read(full)).selectAsync();
            var rejectedFailure = assertThrows(CompletionException.class, rejected::join);
            assertTrue(rejectedFailure.getCause()
                    instanceof ChannelRuntime.SelectAdmissionException);

            var readyFuture = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.await(OresFuture.completed(99))).selectAsync();
            assertEquals(99, readyFuture.join().value());

            var buffered = new ChannelRuntime.Channel<Integer>(1);
            assertTrue(buffered.tryWrite(100));
            var readyRead = ChannelRuntime.SelectSet.of(ChannelRuntime.read(buffered)).selectAsync();
            assertEquals(100, readyRead.join().value());
        } finally {
            for (var waiter : pending) waiter.cancel(false);
        }
    }

    @Test
    void selectableSourcesReduceToOneAtomicNextEventWithoutConsumingLosers() {
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var future = OresFuture.completed(55);
        ChannelRuntime.Selectable<Integer> nextChannelEvent =
                () -> ChannelRuntime.read(channel);
        ChannelRuntime.Selectable<Integer> futureEvent =
                () -> ChannelRuntime.await(future);
        var sources = java.util.List.of(nextChannelEvent, futureEvent);

        var first = ChannelRuntime.SelectSet.fromSelectables(sources)
                .selectAsync(ChannelRuntime.SelectPolicy.FAIR).join();
        assertEquals(1, first.index());
        assertEquals(55, first.value());

        assertTrue(channel.tryWrite(77));
        var second = ChannelRuntime.SelectSet.fromSelectables(sources)
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY).join();
        assertEquals(0, second.index());
        assertEquals(77, second.value());
    }

    @Test
    void selectableCannotForgeDefaultOrNullOperation() {
        ChannelRuntime.Selectable<Integer> forgedDefault = ChannelRuntime::defaultCase;
        ChannelRuntime.Selectable<Integer> nullOperation = () -> null;
        assertThrows(IllegalArgumentException.class, () ->
                ChannelRuntime.SelectSet.fromSelectables(java.util.List.of(forgedDefault)));
        assertThrows(NullPointerException.class, () ->
                ChannelRuntime.SelectSet.fromSelectables(java.util.List.of(nullOperation)));
    }

    @Test
    void concurrentCancellationAndFutureSettlementDoNotRetainLosingWaiters()
            throws Exception {
        for (int round = 0; round < 64; round++) {
            var producer = new OresFuture<Integer>();
            var channel = new ChannelRuntime.Channel<Integer>(1);
            var selected = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.await(producer), ChannelRuntime.read(channel))
                    .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
            var gate = new CountDownLatch(1);
            var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
            Thread canceller = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(gate);
                    selected.cancel(false);
                } catch (Throwable error) {
                    workerFailure.compareAndSet(null, error);
                }
            });
            Thread settler = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(gate);
                    assertTrue(producer.completeFromRuntime(19));
                } catch (Throwable error) {
                    workerFailure.compareAndSet(null, error);
                }
            });
            gate.countDown();
            canceller.join(5_000);
            settler.join(5_000);
            assertFalse(canceller.isAlive(), "cancel stalled in round " + round);
            assertFalse(settler.isAlive(), "settlement stalled in round " + round);
            if (workerFailure.get() != null) {
                throw new AssertionError("select race worker failed in round " + round,
                        workerFailure.get());
            }
            if (selected.isCancelled()) {
                assertThrows(CancellationException.class, selected::join);
            } else {
                var winner = selected.get(5, TimeUnit.SECONDS);
                assertEquals(ChannelRuntime.SelectOperation.FUTURE, winner.operation());
                assertEquals(19, winner.value());
            }
            assertEquals(0, producer.pendingRuntimeWaiterCount());
            assertEquals(19, producer.get(5, TimeUnit.SECONDS));
            assertTrue(channel.tryWrite(73));
            assertEquals(73, channel.tryRead().orElseThrow());
        }
    }

    @Test
    void maximumFanInPreservesHighIndexWinnerAndRefundsEveryLosingWaiter() {
        // Probe the largest supported arm set. The channel is deliberately
        // last, so arbitration visits every pending Future before its winner.
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var cases = new java.util.ArrayList<ChannelRuntime.SelectCase>();
        for (int i = 0; i < ChannelRuntime.MAX_SELECT_CASES - 1; i++) {
            cases.add(ChannelRuntime.await(producer));
        }
        cases.add(ChannelRuntime.read(channel));
        var selection = new ChannelRuntime.SelectSet(cases);
        var pending = selection.selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        assertEquals(ChannelRuntime.MAX_SELECT_CASES - 1,
                producer.pendingRuntimeWaiterCount());
        assertTrue(channel.tryWrite(42));
        var channelWinner = pending.join();
        assertEquals(ChannelRuntime.MAX_SELECT_CASES - 1, channelWinner.index());
        assertEquals(42, channelWinner.value());
        assertTrue(channel.tryRead().isEmpty());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone());

        // A reused select sees its first newly completed Future and never
        // consumes a channel value merely because that channel was registered.
        var waitingAgain = selection.selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        assertTrue(producer.completeFromRuntime(99));
        var futureWinner = waitingAgain.join();
        assertEquals(0, futureWinner.index());
        assertEquals(99, futureWinner.value());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertTrue(channel.tryWrite(73));
        assertEquals(73, channel.tryRead().orElseThrow());
    }

    @Test
    void closingChannelAndSettlingFutureRaceWithoutCancellingLosingSource()
            throws Exception {
        for (int round = 0; round < 64; round++) {
            var channel = new ChannelRuntime.Channel<Integer>(0);
            var producer = new OresFuture<Integer>();
            var closeCause = new IllegalStateException("channel shutdown");
            var selected = ChannelRuntime.SelectSet.of(
                    ChannelRuntime.read(channel), ChannelRuntime.await(producer))
                    .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
            var gate = new CountDownLatch(1);
            var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
            Thread closer = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(gate);
                    channel.close(closeCause);
                } catch (Throwable failure) {
                    workerFailure.compareAndSet(null, failure);
                }
            });
            Thread settler = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(gate);
                    assertTrue(producer.completeFromRuntime(202));
                } catch (Throwable failure) {
                    workerFailure.compareAndSet(null, failure);
                }
            });
            gate.countDown();
            closer.join(5_000);
            settler.join(5_000);
            assertFalse(closer.isAlive(), "channel close stalled in round " + round);
            assertFalse(settler.isAlive(), "Future settlement stalled in round " + round);
            if (workerFailure.get() != null) {
                throw new AssertionError("close/settle race failed in round " + round,
                        workerFailure.get());
            }
            try {
                var winner = selected.get(5, TimeUnit.SECONDS);
                assertEquals(ChannelRuntime.SelectOperation.FUTURE, winner.operation());
                assertEquals(1, winner.index());
                assertEquals(202, winner.value());
            } catch (java.util.concurrent.ExecutionException failed) {
                assertTrue(failed.getCause()
                        instanceof ChannelRuntime.ChannelClosedException);
                assertEquals(closeCause, failed.getCause().getCause());
            }
            assertTrue(channel.isClosed());
            assertEquals(0, channel.pendingRegistrations());
            assertEquals(0, producer.pendingRuntimeWaiterCount());
            assertEquals(202, producer.get(5, TimeUnit.SECONDS));
        }
    }

    @Test
    void settlementDuringManyFutureListenerAttachmentsLeavesNoWaiters()
            throws Exception {
        for (int round = 0; round < 32; round++) {
            var producer = new OresFuture<Integer>();
            var cases = new java.util.ArrayList<ChannelRuntime.SelectCase>();
            for (int i = 0; i < 128; i++) {
                cases.add(ChannelRuntime.await(producer));
            }
            var gate = new CountDownLatch(1);
            var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
            Thread settler = Thread.startVirtualThread(() -> {
                try {
                    awaitStart(gate);
                    assertTrue(producer.completeFromRuntime(17));
                } catch (Throwable failure) {
                    workerFailure.compareAndSet(null, failure);
                }
            });
            gate.countDown();
            var selected = new ChannelRuntime.SelectSet(cases)
                    .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
            settler.join(5_000);
            assertFalse(settler.isAlive(), "settlement stalled in round " + round);
            if (workerFailure.get() != null) {
                throw new AssertionError("attachment race failed in round " + round,
                        workerFailure.get());
            }
            var winner = selected.get(5, TimeUnit.SECONDS);
            assertEquals(ChannelRuntime.SelectOperation.FUTURE, winner.operation());
            assertEquals(0, winner.index());
            assertEquals(17, winner.value());
            assertEquals(0, producer.pendingRuntimeWaiterCount());
        }
    }

    @Test
    void failedClosedArmAdvancesFairCursorPastPermanentFailure() {
        var closed = new ChannelRuntime.Channel<Integer>(0);
        closed.close();
        var pending = new OresFuture<Integer>();
        var set = ChannelRuntime.SelectSet.of(
                ChannelRuntime.read(closed), ChannelRuntime.await(pending));
        var failed = assertThrows(CompletionException.class,
                () -> set.selectAsync(ChannelRuntime.SelectPolicy.FAIR).join());
        assertTrue(failed.getCause() instanceof ChannelRuntime.ChannelClosedException);

        assertTrue(pending.completeFromRuntime(23));
        var winner = set.selectAsync(ChannelRuntime.SelectPolicy.FAIR).join();
        assertEquals(ChannelRuntime.SelectOperation.FUTURE, winner.operation());
        assertEquals(1, winner.index());
        assertEquals(23, winner.value());
        assertEquals(0, pending.pendingRuntimeWaiterCount());
    }

    @Test
    void internalBufferedDrainUnblocksPendingWriterWithoutStealingLosingFuture()
            throws Exception {
        var channel = new ChannelRuntime.Channel<Integer>(1);
        assertTrue(channel.tryWrite(10));
        var producer = new OresFuture<Integer>();
        var selected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.write(channel, 20),
                ChannelRuntime.await(producer))
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        assertFalse(selected.isDone());
        assertEquals(1, channel.pendingRegistrations());
        assertEquals(1, producer.pendingRuntimeWaiterCount());

        assertTrue(channel.drainOne(value -> {
            assertEquals(10, value);
            // Completion callbacks are not run inside the mailbox consumer.
            assertFalse(selected.isDone());
        }));
        var winner = selected.get(5, TimeUnit.SECONDS);
        assertEquals(0, winner.index());
        assertEquals(ChannelRuntime.SelectOperation.WRITE, winner.operation());
        assertEquals(0, channel.pendingRegistrations());
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone());
        assertEquals(20, channel.tryRead().orElseThrow());
    }

    @Test
    void throwingInternalDrainConsumerStillCompletesAlreadyAdmittedWriter()
            throws Exception {
        var channel = new ChannelRuntime.Channel<Integer>(1);
        assertTrue(channel.tryWrite(41));
        var admitted = channel.writeAsync(42);
        assertFalse(admitted.isDone());

        var failure = new IllegalStateException("mailbox consumer failure");
        var caught = assertThrows(IllegalStateException.class,
                () -> channel.drainOne(value -> {
                    assertEquals(41, value);
                    throw failure;
                }));
        assertEquals(failure, caught);
        assertEquals(null, admitted.get(5, TimeUnit.SECONDS));
        assertEquals(0, channel.pendingRegistrations());
        assertEquals(42, channel.tryRead().orElseThrow());
    }

    @Test
    void immediateProbeOnPendingFutureRegistersNoWaiter() {
        var future = new OresFuture<Integer>();
        var set = ChannelRuntime.SelectSet.of(ChannelRuntime.await(future));

        assertTrue(set.trySelect().isEmpty());
        assertEquals(0, future.pendingRuntimeWaiterCount());

        assertTrue(future.completeFromRuntime(4));
        assertEquals(4, set.trySelect().orElseThrow().value());
        assertEquals(0, future.pendingRuntimeWaiterCount());
    }
}
