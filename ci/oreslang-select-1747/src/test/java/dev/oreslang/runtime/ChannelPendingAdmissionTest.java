package dev.oreslang.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

/**
 * A channel waiter quota is admission backpressure, not a limit on the
 * zero-capacity handoff or a substitute for the actor's processing lease.
 */
final class ChannelPendingAdmissionTest {
    @Test
    void rejectedWriterDoesNotRetainItsPayloadOrWaiter() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 2);
        var one = channel.writeAsync(1);
        var two = channel.writeAsync(2);
        assertEquals(2, channel.pendingRegistrations());

        var refused = channel.writeAsync(3);
        assertTrue(refused.isDone());
        assertThrows(RuntimeException.class, refused::join);
        assertEquals(2, channel.pendingRegistrations());
        assertFalse(one.isDone());
        assertFalse(two.isDone());

        assertEquals(1, channel.readAsync().join());
        one.join();
        assertEquals(2, channel.readAsync().join());
        two.join();
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void fullReadBudgetCannotDeadlockRendezvousWriter() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        var waitingRead = channel.readAsync();
        assertEquals(1, channel.pendingRegistrations());

        // A ready writer must still rendezvous: the quota applies to
        // retained pending operations, never to atomic immediate commits.
        var write = channel.writeAsync(42);
        write.join();
        assertEquals(42, waitingRead.join());
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void fullWriteBudgetCannotDeadlockRendezvousReader() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        var waitingWrite = channel.writeAsync(42);
        assertEquals(1, channel.pendingRegistrations());

        assertEquals(42, channel.readAsync().join());
        waitingWrite.join();
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void canceledWaiterReleasesBudgetBeforeLaterRegistration() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        var canceled = channel.readAsync();
        assertTrue(canceled.cancel(false));
        assertEquals(0, channel.pendingRegistrations());

        var active = channel.readAsync();
        assertEquals(1, channel.pendingRegistrations());
        assertTrue(channel.tryWrite(19));
        assertEquals(19, active.join());
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void refusedMultiChannelSelectionLeavesNoPhantomArms() {
        var a = new ChannelRuntime.Channel<Integer>(0, 1);
        var b = new ChannelRuntime.Channel<Integer>(0, 1);
        var hold = b.readAsync();

        var selection = ChannelRuntime.SelectSet.of(
                ChannelRuntime.read(a), ChannelRuntime.read(b)).selectAsync();
        assertThrows(RuntimeException.class, selection::join);
        assertEquals(0, a.pendingRegistrations());
        assertEquals(1, b.pendingRegistrations());

        assertTrue(b.tryWrite(10));
        assertEquals(10, hold.join());
        assertEquals(0, b.pendingRegistrations());
    }

    @Test
    void readySelectCanCommitEvenWithSaturatedOtherArm() {
        var blocked = new ChannelRuntime.Channel<Integer>(0, 1);
        var ready = new ChannelRuntime.Channel<Integer>(1, 1);
        var waiting = blocked.readAsync();
        assertTrue(ready.tryWrite(17));

        var chosen = ChannelRuntime.SelectSet.of(
                ChannelRuntime.read(blocked), ChannelRuntime.read(ready))
                .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
        assertEquals(1, chosen.join().index());
        assertEquals(1, blocked.pendingRegistrations());
        assertTrue(blocked.tryWrite(9));
        assertEquals(9, waiting.join());
    }

    @Test
    void dynamicSelectRejectsUnboundedIterableBeforeAllocatingAllWaiters() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        Iterable<ChannelRuntime.SelectCase> infinite = () -> new java.util.Iterator<>() {
            @Override public boolean hasNext() { return true; }
            @Override public ChannelRuntime.SelectCase next() {
                return ChannelRuntime.read(channel);
            }
        };
        assertThrows(IllegalArgumentException.class,
                () -> ChannelRuntime.SelectSet.from(infinite));
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void selectCollectionIsLimitedBeforeRegisteringAnyArm() {
        var channel = new ChannelRuntime.Channel<Integer>(0, 1);
        var cases = java.util.Collections.nCopies(
                ChannelRuntime.MAX_SELECT_CASES + 1,
                ChannelRuntime.read(channel));
        assertThrows(IllegalArgumentException.class,
                () -> new ChannelRuntime.SelectSet(cases));
        assertEquals(0, channel.pendingRegistrations());
    }

    @Test
    void boundsRejectInvalidPendingBudgets() {
        assertThrows(IllegalArgumentException.class,
                () -> new ChannelRuntime.Channel<Integer>(0, 0));
        assertThrows(IllegalArgumentException.class,
                () -> new ChannelRuntime.Channel<Integer>(0, -1));
        assertEquals(ChannelRuntime.DEFAULT_MAX_PENDING_REGISTRATIONS,
                new ChannelRuntime.Channel<Integer>(1).maxPendingRegistrations());
    }
}
