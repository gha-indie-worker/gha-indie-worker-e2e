package dev.oreslang.runtime;

import static org.junit.jupiter.api.Assertions.*;
import org.junit.jupiter.api.Test;

final class StaticSelectTicketTest {
    @Test
    void sourceTicketsOverrideDescriptorFairnessWithoutMutatingDescriptorCursor() {
        var a = new ChannelRuntime.Channel<Integer>(2);
        var b = new ChannelRuntime.Channel<Integer>(2);
        var cached = ChannelRuntime.SelectSet.of(ChannelRuntime.read(a), ChannelRuntime.read(b));
        assertTrue(a.tryWrite(1));
        assertTrue(b.tryWrite(2));
        assertEquals(0, cached.tryStaticSelect(ChannelRuntime.SelectPolicy.FAIR, 0)
                .orElseThrow().index());
        assertTrue(a.tryWrite(3));
        assertEquals(1, cached.tryStaticSelect(ChannelRuntime.SelectPolicy.FAIR, 1)
                .orElseThrow().index());
        assertTrue(b.tryWrite(4));
        assertEquals(0, cached.tryStaticSelect(ChannelRuntime.SelectPolicy.FAIR, 2)
                .orElseThrow().index());
        // Both channels are ready; if a source ticket had rotated the plan's
        // independent cursor this would pick the wrong first arm.
        assertTrue(a.tryWrite(5));
        assertEquals(0, cached.trySelect(ChannelRuntime.SelectPolicy.FAIR)
                .orElseThrow().index());
    }

    @Test
    void eachPendingInvocationKeepsOriginalTicketAndFreshCancellation() {
        var inbox = new ChannelRuntime.Channel<String>(1);
        var alt = new ChannelRuntime.Channel<String>(1);
        var cached = ChannelRuntime.SelectSet.of(ChannelRuntime.read(inbox), ChannelRuntime.read(alt));
        var pending = cached.selectStaticAsync(ChannelRuntime.SelectPolicy.FAIR, 1);
        var cancelled = cached.selectStaticAsync(ChannelRuntime.SelectPolicy.FAIR, 0);
        assertTrue(cancelled.cancel(false));
        assertEquals(1, inbox.pendingRegistrations());
        assertEquals(1, alt.pendingRegistrations());
        assertTrue(alt.tryWrite("win"));
        assertEquals(1, pending.join().index());
        assertEquals("win", pending.join().value());
        assertEquals(0, inbox.pendingRegistrations());
        assertEquals(0, alt.pendingRegistrations());
    }

    @Test
    void repeatedImmediateMissDoesNotRetainRegistrations() {
        var inbox = new ChannelRuntime.Channel<Integer>(1);
        var cached = ChannelRuntime.SelectSet.of(ChannelRuntime.read(inbox));
        for (int i = 0; i < 1024; i++) {
            assertTrue(cached.tryStaticSelect(ChannelRuntime.SelectPolicy.FAIR, i).isEmpty());
            assertEquals(0, inbox.pendingRegistrations());
        }
    }

    @Test
    void futureArmCannotShareWinnerAcrossReusedStaticTickets() {
        var producer = new OresFuture<Integer>();
        var inbox = new ChannelRuntime.Channel<Integer>(1);
        var cached = ChannelRuntime.SelectSet.of(
                ChannelRuntime.read(inbox), ChannelRuntime.await(producer));
        var pending = cached.selectStaticAsync(ChannelRuntime.SelectPolicy.FAIR, 1);
        assertEquals(1, producer.pendingRuntimeWaiterCount());
        assertTrue(pending.cancel(false));
        assertEquals(0, producer.pendingRuntimeWaiterCount());
        assertFalse(producer.isDone());
        assertTrue(inbox.tryWrite(17));
        var selected = cached.tryStaticSelect(ChannelRuntime.SelectPolicy.FAIR, 3).orElseThrow();
        assertEquals(0, selected.index());
        assertEquals(17, selected.value());
    }
}
