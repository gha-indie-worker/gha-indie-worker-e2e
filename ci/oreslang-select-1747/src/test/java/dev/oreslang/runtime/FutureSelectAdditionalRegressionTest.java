package dev.oreslang.runtime;

import static org.junit.jupiter.api.Assertions.assertAll;
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
final class FutureSelectAdditionalRegressionTest {








    @Test
    void winningResultIsPublishedOnlyAfterLosingWaitersAreDetached() throws Exception {
        var producer = new OresFuture<Integer>();
        var channel = new ChannelRuntime.Channel<Integer>(1);
        var selected = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(producer), ChannelRuntime.read(channel))
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        var observedAtPublication = new java.util.concurrent.atomic.AtomicInteger(-1);
        selected.whenCompleteRuntime((winner, failure) ->
                observedAtPublication.set(producer.pendingRuntimeWaiterCount()));
        assertEquals(1, producer.pendingRuntimeWaiterCount());
        assertTrue(channel.tryWrite(77));
        assertEquals(77, selected.get(5, TimeUnit.SECONDS).value());
        assertEquals(0, observedAtPublication.get(),
                "select completion must not precede loser listener detachment");
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone(), "loser producer must remain owned by its source");
    }

    @Test
    void cancelledSelectionPublishesOnlyAfterProducerWaiterDetachment() {
        var producer = new OresFuture<Integer>();
        var selected = ChannelRuntime.SelectSet.of(ChannelRuntime.await(producer))
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        var observedAtCancellation = new java.util.concurrent.atomic.AtomicInteger(-1);
        selected.whenCompleteRuntime((winner, failure) ->
                observedAtCancellation.set(producer.pendingRuntimeWaiterCount()));
        assertEquals(1, producer.pendingRuntimeWaiterCount());
        assertTrue(selected.cancel(false));
        assertEquals(0, observedAtCancellation.get(),
                "cancellation callback must observe retired producer listeners");
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone());
    }

    @Test
    void alreadySettledSharedFutureAliasesAlwaysChooseEarliestPriorityArm() {
        var producer = OresFuture.completed(71);
        var cases = new java.util.ArrayList<ChannelRuntime.SelectCase>();
        for (int i = 0; i < ChannelRuntime.MAX_SELECT_CASES; i++) {
            cases.add(ChannelRuntime.await(producer));
        }
        var selection = new ChannelRuntime.SelectSet(cases);

        for (int round = 0; round < 16; round++) {
            var winner = selection.selectAsync(ChannelRuntime.SelectPolicy.PRIORITY).join();
            assertAll("ready shared Future round " + round,
                    () -> assertEquals(0, winner.index()),
                    () -> assertEquals(71, winner.value()),
                    () -> assertEquals(0, producer.pendingRuntimeWaiterCount()));
        }
    }







    @Test
    void pendingFutureOnlyAndMixedSelectionsRespectSeparateAdmissionDomains() {
        var futureOnlySource = new OresFuture<Integer>();
        var futureOnly = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(futureOnlySource)).selectAsync();
        assertFalse(futureOnly.isDone());
        assertEquals(1, futureOnlySource.pendingRuntimeWaiterCount());
        assertTrue(futureOnly.cancel(false));
        assertEquals(0, futureOnlySource.pendingRuntimeWaiterCount());
        assertFalse(futureOnlySource.isDone());

        var channel = new ChannelRuntime.Channel<Integer>(0);
        var sharedSource = new OresFuture<Integer>();
        var mixed = ChannelRuntime.SelectSet.of(
                ChannelRuntime.await(sharedSource),
                ChannelRuntime.read(channel)).selectAsync();
        assertFalse(mixed.isDone());
        assertEquals(1, sharedSource.pendingRuntimeWaiterCount());
        assertEquals(1, channel.pendingRegistrations());
        assertTrue(mixed.cancel(false));
        assertEquals(0, sharedSource.pendingRuntimeWaiterCount());
        assertEquals(0, channel.pendingRegistrations());
        assertFalse(sharedSource.isDone());
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














}
