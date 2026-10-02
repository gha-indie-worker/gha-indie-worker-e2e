package dev.oreslang.runtime;

import org.junit.jupiter.api.Test;

import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.jupiter.api.Assertions.*;

final class RuntimeGcTest {
    @Test
    void actorTerminationCleansActorScopedResourcesDeterministically() {
        RuntimeGc gc = new RuntimeGc(1);
        ActorRuntime.ActorId owner = ActorRuntime.ActorId.create();
        Object stillReachable = new Object();
        AtomicInteger cleaned = new AtomicInteger();

        gc.trackActor(stillReachable, owner, () -> {
            cleaned.incrementAndGet();
            return 128L;
        });

        RuntimeGc.GcStats stats = gc.releaseActor(owner);

        assertEquals(RuntimeGc.Scope.ACTOR, stats.scope());
        assertEquals(RuntimeGc.Reason.ACTOR_STOP, stats.reason());
        assertEquals(1, stats.reclaimedResources());
        assertEquals(128L, stats.reclaimedBytes());
        assertEquals(1, cleaned.get());
        assertEquals(0, gc.trackedResources());
        assertNotNull(stillReachable, "actor stop invalidates runtime resources even if a host reference leaked");
    }

    @Test
    void liveProcessResourceIsNotCollectedByManualQueueDrain() {
        RuntimeGc gc = new RuntimeGc(1);
        Object live = new Object();
        AtomicInteger cleaned = new AtomicInteger();
        RuntimeGc.Registration registration = gc.trackProcess(live, () -> {
            cleaned.incrementAndGet();
            return 64L;
        });

        RuntimeGc.GcStats stats = gc.collectProcess(false);

        assertEquals(0, stats.reclaimedResources());
        assertEquals(0, cleaned.get());
        assertFalse(registration.cleaned());
        assertEquals(1, gc.trackedResources());

        registration.close();
        assertEquals(1, cleaned.get());
        assertEquals(0, gc.trackedResources());
    }

    @Test
    void explicitCleanupIsIdempotent() {
        RuntimeGc gc = new RuntimeGc(1);
        AtomicInteger cleaned = new AtomicInteger();
        RuntimeGc.Registration registration = gc.trackProcess(new Object(), () -> {
            cleaned.incrementAndGet();
            return 7L;
        });

        assertEquals(7L, registration.cleanNow());
        assertEquals(0L, registration.cleanNow());
        registration.close();

        assertEquals(1, cleaned.get());
        assertTrue(registration.cleaned());
        assertEquals(0, gc.trackedResources());
    }

    @Test
    void actorGcNeverRequestsHostWideGc() {
        RuntimeGc gc = new RuntimeGc(1);
        RuntimeGc.GcStats stats = gc.collectActor(ActorRuntime.ActorId.create());

        assertEquals(RuntimeGc.Scope.ACTOR, stats.scope());
        assertEquals(RuntimeGc.Reason.MANUAL, stats.reason());
        assertFalse(stats.hostGcRequested());
    }

    @Test
    void safepointCollectorOnlyDrainsAlreadyDeadRuntimeResources() {
        RuntimeGc gc = new RuntimeGc(1);
        RuntimeGc.GcStats stats = gc.safepoint();

        assertEquals(RuntimeGc.Reason.SAFEPOINT, stats.reason());
        assertFalse(stats.hostGcRequested());
    }
}
