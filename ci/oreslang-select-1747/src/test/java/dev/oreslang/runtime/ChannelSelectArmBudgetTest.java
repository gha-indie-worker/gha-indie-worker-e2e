package dev.oreslang.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

/**
 * A global pending-select count alone would permit 4096 * 256 retained Future
 * listeners. Quota one-event case registrations as well as select instances.
 */
@Timeout(30)
final class ChannelSelectArmBudgetTest {
    @Test
    void highFanInSelectionsRejectExcessArmsAndRefundAllWaiters() {
        var producers = new ArrayList<OresFuture<Integer>>();
        var arms = new ArrayList<ChannelRuntime.SelectCase>();
        for (int i = 0; i < ChannelRuntime.MAX_SELECT_CASES; i++) {
            var producer = new OresFuture<Integer>();
            producers.add(producer);
            arms.add(ChannelRuntime.await(producer));
        }

        var set = new ChannelRuntime.SelectSet(arms);
        int admitted = ChannelRuntime.MAX_PENDING_SELECT_ARM_REGISTRATIONS
                / ChannelRuntime.MAX_SELECT_CASES;
        var pending = new ArrayList<OresFuture<ChannelRuntime.SelectResult>>();

        try {
            for (int i = 0; i < admitted; i++) {
                var selected = set.selectAsync();
                assertFalse(selected.isDone(), "premature rejection at " + i);
                pending.add(selected);
            }
            assertEquals(admitted, producers.getFirst().pendingRuntimeWaiterCount());

            var excess = set.selectAsync();
            var failure = assertThrows(CompletionException.class, excess::join);
            assertTrue(failure.getCause() instanceof ChannelRuntime.SelectAdmissionException);
            assertEquals(admitted, producers.getFirst().pendingRuntimeWaiterCount(),
                    "rejected selection must detach all of its provisional registrations");
        } finally {
            for (var selection : pending) assertTrue(selection.cancel(false));
        }

        for (var producer : producers) {
            assertFalse(producer.isDone(), "losing producers must remain unmodified");
            assertEquals(0, producer.pendingRuntimeWaiterCount());
        }

        // Releasing every pending arm makes the entire allowance reusable.
        var retry = set.selectAsync();
        assertFalse(retry.isDone());
        assertTrue(retry.cancel(false));
        assertEquals(0, producers.getFirst().pendingRuntimeWaiterCount());
    }
}
