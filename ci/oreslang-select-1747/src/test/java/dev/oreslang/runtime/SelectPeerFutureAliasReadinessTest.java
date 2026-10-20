package dev.oreslang.runtime;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;

import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.jupiter.api.Assertions.*;

/**
 * Selection peer readiness must treat duplicate Future aliases as a single
 * readiness observation within one priority/fairness arbitration pass.
 */
final class SelectPeerFutureAliasReadinessTest {
    @Test
    @Timeout(60)
    void producerSettlementRacingWithRendezvousNeverSelectsLaterFutureAlias()
            throws Exception {
        final int aliases = 64;
        for (int round = 0; round < 128; round++) {
            final int value = round + 100;
            OresFuture<Integer> producer = new OresFuture<>();
            ChannelRuntime.Channel<Integer> rendezvous =
                    new ChannelRuntime.Channel<>(0);
            var cases = new ArrayList<ChannelRuntime.SelectCase>();
            for (int i = 0; i < aliases; i++) {
                cases.add(ChannelRuntime.await(producer));
            }
            cases.add(ChannelRuntime.read(rendezvous));

            OresFuture<ChannelRuntime.SelectResult> selection =
                    new ChannelRuntime.SelectSet(cases)
                            .selectAsync(ChannelRuntime.SelectPolicy.PRIORITY);
            assertFalse(selection.isDone());
            CountDownLatch start = new CountDownLatch(1);
            AtomicReference<OresFuture<Void>> sent = new AtomicReference<>();
            AtomicReference<Throwable> failed = new AtomicReference<>();

            Thread writer = Thread.startVirtualThread(() -> {
                try {
                    if (!start.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("writer start timed out");
                    }
                    sent.set(rendezvous.writeAsync(value));
                } catch (Throwable ex) {
                    failed.compareAndSet(null, ex);
                }
            });
            Thread settler = Thread.startVirtualThread(() -> {
                try {
                    if (!start.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("settler start timed out");
                    }
                    assertTrue(producer.completeFromRuntime(-value));
                } catch (Throwable ex) {
                    failed.compareAndSet(null, ex);
                }
            });

            start.countDown();
            writer.join(5_000);
            settler.join(5_000);
            assertFalse(writer.isAlive(), "writer stalled in round " + round);
            assertFalse(settler.isAlive(), "settler stalled in round " + round);
            assertNull(failed.get(), () -> "race failed: " + failed.get());
            OresFuture<Void> writeResult = sent.get();
            assertNotNull(writeResult);

            ChannelRuntime.SelectResult winner = selection.get(5, TimeUnit.SECONDS);
            if (winner.operation() == ChannelRuntime.SelectOperation.FUTURE) {
                // If any alias of a producer wins PRIORITY, only its first
                // lexical occurrence may be selected.
                assertEquals(0, winner.index(),
                        "a later Future alias stole priority in round " + round);
                assertEquals(-value, winner.value());
                assertFalse(writeResult.isDone(),
                        "unmatched writer must remain pending in round " + round);
                assertTrue(writeResult.cancel(false),
                        "unmatched writer could not be revoked in round " + round);
            } else {
                assertEquals(ChannelRuntime.SelectOperation.READ, winner.operation());
                assertEquals(aliases, winner.index());
                assertEquals(value, winner.value());
                assertNull(writeResult.get(5, TimeUnit.SECONDS));
                assertFalse(writeResult.cancel(false),
                        "committed writer must not be cancelable in round " + round);
            }
            assertEquals(-value, producer.get(5, TimeUnit.SECONDS));
            assertEquals(0, producer.pendingRuntimeWaiterCount(),
                    "losing Future aliases leaked in round " + round);
            assertEquals(0, rendezvous.pendingRegistrations(),
                    "rendezvous credits leaked in round " + round);
            assertEquals(0, rendezvous.size());
        }
    }
}
