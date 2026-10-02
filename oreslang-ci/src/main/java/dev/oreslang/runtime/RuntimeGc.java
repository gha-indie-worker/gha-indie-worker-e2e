package dev.oreslang.runtime;

import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Oreslang fallback collector for runtime-owned resources.
 *
 * Ordinary guest values are not registered here: ownership, move semantics and
 * lexical borrows remain the primary reclamation model. This collector exists
 * for runtime capabilities whose lifetime may escape ordinary lexical lowering
 * (reflection/FFI handles, shared runtime wrappers, private memory blocks,
 * pending async runtime state, generated proxies, etc.).
 *
 * No background collector thread is created. Queue draining happens at
 * compiler/runtime safepoints and on explicit actor.gc()/process.gc() requests.
 */
public final class RuntimeGc implements AutoCloseable {
    public enum Scope { ACTOR, PROCESS }
    public enum Reason { MANUAL, SAFEPOINT, ACTOR_STOP, PROCESS_CLOSE }

    @FunctionalInterface
    public interface Cleanup {
        /** Returns an approximate number of runtime-accounted bytes released. */
        long clean();
    }

    public record GcStats(
            Scope scope,
            Reason reason,
            int trackedBefore,
            int trackedAfter,
            int reclaimedResources,
            long reclaimedBytes,
            boolean hostGcRequested) { }

    private static final int DEFAULT_SAFEPOINT_INTERVAL = 256;
    private static final int PRESSURE_TRACKED_RESOURCES = 4_096;

    private final ReferenceQueue<Object> queue = new ReferenceQueue<>();
    private final Set<TrackedReference> tracked = ConcurrentHashMap.newKeySet();
    private final ConcurrentLinkedQueue<TrackedReference> pending = new ConcurrentLinkedQueue<>();
    private final AtomicLong safepoints = new AtomicLong();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final int safepointInterval;

    public RuntimeGc() {
        this(DEFAULT_SAFEPOINT_INTERVAL);
    }

    RuntimeGc(int safepointInterval) {
        if (safepointInterval <= 0) throw new IllegalArgumentException("safepointInterval must be positive");
        this.safepointInterval = safepointInterval;
    }

    public int trackedResources() {
        return tracked.size();
    }

    public Registration trackProcess(Object referent, Cleanup cleanup) {
        return track(referent, Scope.PROCESS, null, cleanup);
    }

    public Registration trackActor(Object referent, ActorRuntime.ActorId owner, Cleanup cleanup) {
        return track(referent, Scope.ACTOR, Objects.requireNonNull(owner, "owner"), cleanup);
    }

    private Registration track(Object referent, Scope scope, ActorRuntime.ActorId owner, Cleanup cleanup) {
        Objects.requireNonNull(referent, "referent");
        Objects.requireNonNull(scope, "scope");
        Objects.requireNonNull(cleanup, "cleanup");
        if (closed.get()) throw new IllegalStateException("runtime GC is closed");

        TrackedReference reference = new TrackedReference(referent, queue, scope, owner, cleanup);
        tracked.add(reference);
        if (closed.get()) {
            tracked.remove(reference);
            reference.clean();
            reference.clear();
            throw new IllegalStateException("runtime GC is closed");
        }
        return new Registration(reference);
    }

    /**
     * Cheap periodic collector hook. It never asks the host JVM for a global GC;
     * it only drains resources that the host collector has already proven dead.
     */
    public GcStats safepoint() {
        long point = safepoints.incrementAndGet();
        if (point % safepointInterval != 0 && tracked.size() < PRESSURE_TRACKED_RESOURCES) {
            return new GcStats(
                    Scope.PROCESS,
                    Reason.SAFEPOINT,
                    tracked.size(),
                    tracked.size(),
                    0,
                    0,
                    false);
        }
        return collectReachableQueue(Scope.PROCESS, null, Reason.SAFEPOINT, false);
    }

    /**
     * Actor-local manual collection. This deliberately does not call System.gc():
     * one actor must not be able to force a process-wide stop-the-world hint.
     */
    public GcStats collectActor(ActorRuntime.ActorId owner) {
        return collectReachableQueue(
                Scope.ACTOR,
                Objects.requireNonNull(owner, "owner"),
                Reason.MANUAL,
                false);
    }

    /**
     * Process-level manual collection. requestHostGc is a best-effort JVM hint,
     * never a correctness primitive and never required for ordinary Ores values.
     */
    public GcStats collectProcess(boolean requestHostGc) {
        return collectReachableQueue(Scope.PROCESS, null, Reason.MANUAL, requestHostGc);
    }

    /**
     * Actor termination is deterministic: actor-scoped runtime resources are
     * invalid after the actor dies, so clean them even if a host reference leaked.
     */
    public GcStats releaseActor(ActorRuntime.ActorId owner) {
        Objects.requireNonNull(owner, "owner");
        drainReferenceQueue();
        int before = tracked.size();
        int resources = 0;
        long bytes = 0;
        for (TrackedReference reference : List.copyOf(tracked)) {
            if (reference.scope == Scope.ACTOR && owner.equals(reference.owner)) {
                long released = cleanAndRemove(reference);
                if (released >= 0) {
                    resources++;
                    bytes = saturatingAdd(bytes, released);
                }
            }
        }
        return new GcStats(Scope.ACTOR, Reason.ACTOR_STOP, before, tracked.size(), resources, bytes, false);
    }

    private GcStats collectReachableQueue(
            Scope scope,
            ActorRuntime.ActorId owner,
            Reason reason,
            boolean requestHostGc) {
        if (closed.get()) {
            return new GcStats(scope, reason, 0, 0, 0, 0, false);
        }

        int before = tracked.size();
        if (requestHostGc) {
            // A hint only. We do not wait for, assume, or depend on completion.
            System.gc();
        }
        drainReferenceQueue();

        int resources = 0;
        long bytes = 0;
        int candidates = pending.size();
        for (int i = 0; i < candidates; i++) {
            TrackedReference reference = pending.poll();
            if (reference == null) break;
            if (matches(scope, owner, reference)) {
                long released = cleanAndRemove(reference);
                if (released >= 0) {
                    resources++;
                    bytes = saturatingAdd(bytes, released);
                }
            } else {
                pending.offer(reference);
            }
        }
        return new GcStats(scope, reason, before, tracked.size(), resources, bytes, requestHostGc);
    }

    private boolean matches(Scope scope, ActorRuntime.ActorId owner, TrackedReference reference) {
        if (scope == Scope.PROCESS) return true;
        return reference.scope == Scope.ACTOR && owner.equals(reference.owner);
    }

    private void drainReferenceQueue() {
        for (;;) {
            TrackedReference reference = (TrackedReference) queue.poll();
            if (reference == null) return;
            pending.offer(reference);
        }
    }

    private long cleanAndRemove(TrackedReference reference) {
        if (!tracked.remove(reference)) {
            pending.remove(reference);
            reference.clear();
            return -1;
        }
        pending.remove(reference);
        long bytes = reference.clean();
        reference.clear();
        return bytes;
    }

    private static long saturatingAdd(long left, long right) {
        if (right <= 0) return left;
        long result = left + right;
        return result < left ? Long.MAX_VALUE : result;
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) return;
        drainReferenceQueue();
        List<TrackedReference> snapshot = new ArrayList<>(tracked);
        for (TrackedReference reference : snapshot) cleanAndRemove(reference);
        pending.clear();
    }

    public final class Registration implements AutoCloseable {
        private final TrackedReference reference;
        private final AtomicBoolean closedRegistration = new AtomicBoolean();

        private Registration(TrackedReference reference) {
            this.reference = reference;
        }

        public boolean cleaned() {
            return reference.cleaned.get();
        }

        public long cleanNow() {
            if (!closedRegistration.compareAndSet(false, true)) return 0L;
            long released = cleanAndRemove(reference);
            return Math.max(0L, released);
        }

        @Override
        public void close() {
            cleanNow();
        }
    }

    private static final class TrackedReference extends PhantomReference<Object> {
        private final Scope scope;
        private final ActorRuntime.ActorId owner;
        private final Cleanup cleanup;
        private final AtomicBoolean cleaned = new AtomicBoolean();

        private TrackedReference(
                Object referent,
                ReferenceQueue<Object> queue,
                Scope scope,
                ActorRuntime.ActorId owner,
                Cleanup cleanup) {
            super(referent, queue);
            this.scope = scope;
            this.owner = owner;
            this.cleanup = cleanup;
        }

        private long clean() {
            if (!cleaned.compareAndSet(false, true)) return 0L;
            long released = cleanup.clean();
            return Math.max(0L, released);
        }
    }
}
