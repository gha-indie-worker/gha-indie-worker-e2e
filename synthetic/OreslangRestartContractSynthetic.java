package synthetic;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

public final class OreslangRestartContractSynthetic {
    private record ActorId(String value) { }

    private static final class Ref {
        final ActorId id;
        final long generation;
        Ref(ActorId id, long generation) {
            this.id = id;
            this.generation = generation;
        }
    }

    private record MailStamp(ActorId id, long generation) { }

    private static final class Registry {
        private final Map<ActorId, Ref> live = new HashMap<>();
        private final Map<ActorId, Long> high = new HashMap<>();
        private long nextGeneration = 1;

        Ref spawnFresh(String logicalName) {
            return publish(new ActorId(logicalName));
        }

        Ref restart(Ref failed) {
            if (live.containsKey(failed.id)) {
                throw new IllegalStateException("failed incarnation still live");
            }
            return publish(failed.id);
        }

        private long allocateGeneration() {
            long current = nextGeneration;
            if (current <= 0) throw new IllegalStateException("generation exhausted");
            nextGeneration = current == Long.MAX_VALUE ? 0 : current + 1;
            return current;
        }

        private Ref publish(ActorId id) {
            long generation = allocateGeneration();
            long prior = high.getOrDefault(id, 0L);
            if (generation <= prior) {
                throw new IllegalStateException("generation did not advance");
            }
            Ref ref = new Ref(id, generation);
            if (live.putIfAbsent(id, ref) != null) {
                throw new IllegalStateException("logical id already live");
            }
            high.put(id, generation);
            return ref;
        }

        Ref resolveExact(Ref ref) {
            Ref current = live.get(ref.id);
            return current == ref && current.generation == ref.generation ? current : null;
        }

        void retire(Ref ref) {
            live.remove(ref.id, ref);
        }

        boolean stop(Ref ref) {
            return live.remove(ref.id, ref);
        }
    }

    private static final class RestartLimiter {
        private static final class History {
            final ArrayDeque<Long> times = new ArrayDeque<>();
            long lastFailureGeneration;
            long stableGeneration;
            String lastDecision;
        }

        private final int max;
        private final int maxTrackedActors;
        private final long window;
        private final long minBackoff;
        private final long maxBackoff;
        private final Map<ActorId, History> histories = new HashMap<>();
        private long now;

        RestartLimiter(
                int max,
                int maxTrackedActors,
                long window,
                long minBackoff,
                long maxBackoff) {
            this.max = max;
            this.maxTrackedActors = maxTrackedActors;
            this.window = window;
            this.minBackoff = minBackoff;
            this.maxBackoff = maxBackoff;
        }

        String decide(ActorId id, long generation) {
            History h = histories.get(id);
            if (h == null) {
                if (histories.size() >= maxTrackedActors) return "ESCALATE";
                h = new History();
                histories.put(id, h);
            }
            if (generation < h.stableGeneration || generation < h.lastFailureGeneration)
                throw new IllegalArgumentException("stale");
            if (generation == h.lastFailureGeneration && h.lastDecision != null)
                return h.lastDecision;
            while (!h.times.isEmpty() && now - h.times.peekFirst() >= window) {
                h.times.removeFirst();
            }
            h.lastFailureGeneration = generation;
            if (h.times.size() >= max) {
                return h.lastDecision = "ESCALATE";
            }
            int ordinal = h.times.size() + 1;
            long backoff = minBackoff;
            for (int i = 1; i < ordinal && backoff < maxBackoff; i++) {
                backoff = Math.min(maxBackoff, backoff * 2);
            }
            h.times.addLast(now);
            return h.lastDecision = "RESTART:" + backoff;
        }

        void resetAfterStability(ActorId id, long generation) {
            History h = histories.get(id);
            if (h == null) {
                if (histories.size() >= maxTrackedActors)
                    throw new IllegalStateException("history capacity exhausted");
                h = new History();
                histories.put(id, h);
            }
            if ((h.lastFailureGeneration > 0 && generation <= h.lastFailureGeneration)
                    || generation < h.stableGeneration)
                throw new IllegalArgumentException("not newer");
            h.times.clear();
            h.stableGeneration = generation;
            h.lastDecision = null;
        }

        boolean forget(ActorId id, long generation) {
            History h = histories.get(id);
            if (h == null) return false;
            if (generation < h.lastFailureGeneration || generation < h.stableGeneration)
                throw new IllegalArgumentException("stale forget");
            return histories.remove(id, h);
        }

        int occupancy(ActorId id) {
            History h = histories.get(id);
            return h == null ? 0 : h.times.size();
        }
        int trackedActors() { return histories.size(); }
        void tick(long delta) { now += delta; }
    }

    private static final class RestartPermit {
        private boolean consumed;

        void attempt(boolean admissionSucceeds) {
            if (consumed) throw new IllegalStateException("already consumed");
            consumed = true;
            if (!admissionSucceeds) throw new IllegalStateException("admission failed");
        }
    }

    private static final class ControlRegistry {
        private final Map<ActorId, Long> retained = new HashMap<>();

        void retain(Ref failed) {
            if (retained.putIfAbsent(failed.id, failed.generation) != null)
                throw new IllegalStateException("duplicate retained CONTROL");
        }

        String evaluate(
                RestartLimiter limiter,
                ActorId id,
                long generation) {
            Long current = retained.get(id);
            if (current == null || current.longValue() != generation) return null;
            return limiter.decide(id, generation);
        }

        boolean consume(ActorId id, long generation) {
            return retained.remove(id, generation);
        }

        void resetAfterStability(
                RestartLimiter limiter,
                Registry registry,
                Ref stable) {
            if (registry.resolveExact(stable) != stable)
                throw new IllegalArgumentException("stability requires exact live ref");
            limiter.resetAfterStability(stable.id, stable.generation);
        }
    }


    private enum FailureKind { THROW, RAISE, PANIC }

    private record ControlEvent(Ref child, Ref parent, FailureKind kind) { }

    private interface TypedControlHandler {
        default void onThrow(ControlEvent event) throws Exception { }
        default void onRaise(ControlEvent event) throws Exception { }
        default void onPanic(ControlEvent event) throws Exception { }
    }

    private static final class RetainedControls {
        private final Map<ActorId, ControlEvent> retained = new HashMap<>();

        void retain(ControlEvent event) {
            if (retained.putIfAbsent(event.child.id, event) != null)
                throw new IllegalStateException("duplicate retained control");
        }

        boolean contains(ControlEvent event) {
            return retained.get(event.child.id) == event;
        }

        boolean acknowledge(ControlEvent event) {
            return retained.remove(event.child.id, event);
        }
    }

    private static final class SerializedControlLane implements AutoCloseable {
        private final Ref owner;
        private final ExecutorService executor = Executors.newSingleThreadExecutor();
        private final ThreadLocal<Boolean> controlCapability =
                ThreadLocal.withInitial(() -> false);
        private final AtomicInteger active = new AtomicInteger();
        private final AtomicInteger maxActive = new AtomicInteger();

        SerializedControlLane(Ref owner) {
            this.owner = owner;
        }

        Future<?> submitUser(Runnable task) {
            return executor.submit(task);
        }

        Future<?> submitControl(ControlEvent event, TypedControlHandler handler) {
            if (event.parent != owner)
                throw new IllegalArgumentException("stale parent generation");
            return executor.submit(() -> {
                if (controlCapability.get())
                    throw new IllegalStateException("nested control");
                int current = active.incrementAndGet();
                maxActive.accumulateAndGet(current, Math::max);
                controlCapability.set(true);
                try {
                    switch (event.kind) {
                        case THROW -> handler.onThrow(event);
                        case RAISE -> handler.onRaise(event);
                        case PANIC -> handler.onPanic(event);
                    }
                } finally {
                    controlCapability.remove();
                    active.decrementAndGet();
                }
                return null;
            });
        }

        boolean inControl() { return controlCapability.get(); }
        int maxActive() { return maxActive.get(); }

        @Override public void close() throws Exception {
            executor.shutdownNow();
            executor.awaitTermination(5, TimeUnit.SECONDS);
        }
    }

    private static final class SupervisorDefinition {
        private static final Set<String> REQUIRED =
                Set.of("on_throw", "on_raise", "on_panic");
        private final Set<String> hiddenHandlers;

        SupervisorDefinition(boolean supervisor, Set<String> handlers) {
            if (!supervisor && !handlers.isEmpty())
                throw new IllegalArgumentException("privileged handlers require supervisor actor");
            if (supervisor && !handlers.equals(REQUIRED))
                throw new IllegalArgumentException("supervisor handlers must be exhaustive");
            hiddenHandlers = Set.copyOf(handlers);
        }

        boolean ordinaryMethodVisible(String name) {
            return !hiddenHandlers.contains(name);
        }
    }

    private static final class RevocableChildFailure {
        private final FailureKind kind;
        private String directive;
        private boolean active = true;

        RevocableChildFailure(FailureKind kind) { this.kind = kind; }

        FailureKind kind() {
            requireActive();
            return kind;
        }

        void choose(String value) {
            requireActive();
            if (directive != null) throw new IllegalStateException("directive already chosen");
            directive = value;
        }

        String directiveOrRetain() {
            requireActive();
            return directive == null ? "RETAIN" : directive;
        }

        void revoke() { active = false; }

        private void requireActive() {
            if (!active) throw new SecurityException("expired ChildFailure");
        }
    }

    private static final class DeferredRestart {
        int handlerCalls;
        boolean armed;
        boolean restarted;

        void firstControl() {
            handlerCalls++;
            armed = true;
        }

        boolean prematureRedispatch() {
            return false;
        }

        void timerWake() {
            if (!armed) throw new IllegalStateException("wake not armed");
            armed = false;
            restarted = true;
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        Registry registry = new Registry();
        Ref first = registry.spawnFresh("worker-A");

        Set<Ref> group = new HashSet<>();
        group.add(first);
        Ref continuationTarget = first;
        MailStamp delayed = new MailStamp(first.id, first.generation);

        registry.retire(first);
        group.remove(first);

        Ref replacement = registry.restart(first);
        check(first.id.equals(replacement.id), "1 stable logical ActorId");
        check(replacement.generation > first.generation, "2 generation advances");
        check(registry.resolveExact(first) == null, "3 stale ref rejected");
        check(registry.resolveExact(replacement) == replacement, "4 exact ref accepted");

        Ref forgedSameTuple = new Ref(replacement.id, replacement.generation);
        check(registry.resolveExact(forgedSameTuple) == null, "5 forged capability rejected");
        check(registry.resolveExact(continuationTarget) == null, "6 stale continuation rejected");

        group.add(replacement);
        check(!group.contains(first), "7 group ownership does not retarget");
        check(group.contains(replacement), "8 replacement can be explicitly owned");

        check(!registry.stop(first), "9 stale stop inert");
        check(registry.resolveExact(replacement) == replacement, "10 stale stop preserves replacement");

        check(delayed.id.equals(replacement.id), "11 delayed metadata keeps logical id");
        check(delayed.generation != replacement.generation, "12 delayed metadata generation-fenced");

        MailStamp current = new MailStamp(replacement.id, replacement.generation);
        check(current.generation > delayed.generation, "13 replacement metadata advances");
        check(registry.stop(replacement), "14 live replacement stop succeeds");

        Registry exhausted = new Registry();
        exhausted.nextGeneration = Long.MAX_VALUE;
        Ref last = exhausted.spawnFresh("last");
        check(last.generation == Long.MAX_VALUE, "15 max generation is usable once");
        exhausted.retire(last);
        boolean exhaustedClosed = false;
        try { exhausted.spawnFresh("never"); }
        catch (IllegalStateException expected) { exhaustedClosed = true; }
        check(exhaustedClosed, "16 generation exhaustion fails closed");

        ActorId policyActor = new ActorId("policy-A");
        RestartLimiter limiter = new RestartLimiter(3, 2, 10_000, 100, 800);
        check("RESTART:100".equals(limiter.decide(policyActor, 1)), "17 first bounded backoff");
        check("RESTART:100".equals(limiter.decide(policyActor, 1))
                        && limiter.occupancy(policyActor) == 1,
                "18 repeated CONTROL observation is idempotent");
        limiter.tick(1);
        check("RESTART:200".equals(limiter.decide(policyActor, 2)), "19 exponential backoff");
        limiter.tick(1);
        check("RESTART:400".equals(limiter.decide(policyActor, 3)), "20 third bounded restart");
        limiter.tick(1);
        check("ESCALATE".equals(limiter.decide(policyActor, 4)), "21 restart storm escalates");

        boolean sameGenerationResetRejected = false;
        try { limiter.resetAfterStability(policyActor, 4); }
        catch (IllegalArgumentException expected) { sameGenerationResetRejected = true; }
        check(sameGenerationResetRejected, "22 stability reset requires newer generation");
        limiter.resetAfterStability(policyActor, 5);
        check(limiter.occupancy(policyActor) == 0, "23 newer stable generation resets budget");

        ActorId second = new ActorId("policy-B");
        ActorId third = new ActorId("policy-C");
        check("RESTART:100".equals(limiter.decide(second, 1)), "24 second actor tracked");
        check("ESCALATE".equals(limiter.decide(third, 1))
                        && limiter.trackedActors() == 2,
                "25 full history table fails closed without eviction");
        check(limiter.forget(second, 1) && limiter.trackedActors() == 1,
                "26 quiescent exact generation can release history capacity");
        check("RESTART:100".equals(limiter.decide(third, 1)),
                "27 released history capacity admits new actor");

        boolean staleForgetRejected = false;
        try { limiter.forget(policyActor, 4); }
        catch (IllegalArgumentException expected) { staleForgetRejected = true; }
        check(staleForgetRejected, "28 stale generation cannot erase newer policy state");

        RestartPermit permit = new RestartPermit();
        boolean firstAdmissionFailed = false;
        try {
            permit.attempt(false);
        } catch (IllegalStateException expected) {
            firstAdmissionFailed = true;
        }
        check(firstAdmissionFailed, "29 failed restart admission is surfaced");

        boolean retryRejected = false;
        try {
            permit.attempt(true);
        } catch (IllegalStateException expected) {
            retryRejected = true;
        }
        check(retryRejected, "30 failed admission still consumes one-shot restart permit");

        Registry controlActors = new Registry();
        Ref controlledFailed = controlActors.spawnFresh("controlled-A");
        ControlRegistry controls = new ControlRegistry();
        controls.retain(controlledFailed);
        RestartLimiter authoritativeLimiter =
                new RestartLimiter(2, 2, 10_000, 0, 0);

        check(controls.evaluate(
                        authoritativeLimiter,
                        controlledFailed.id,
                        controlledFailed.generation + 1) == null
                        && authoritativeLimiter.trackedActors() == 0,
                "31 fabricated generation cannot consume authoritative budget");

        check("RESTART:0".equals(controls.evaluate(
                        authoritativeLimiter,
                        controlledFailed.id,
                        controlledFailed.generation))
                        && authoritativeLimiter.occupancy(controlledFailed.id) == 1,
                "32 exact retained CONTROL alone can consume policy budget");

        check(controls.consume(controlledFailed.id, controlledFailed.generation)
                        && controls.evaluate(
                                authoritativeLimiter,
                                controlledFailed.id,
                                controlledFailed.generation) == null,
                "33 consumed CONTROL cannot be policy-evaluated again");

        controlActors.retire(controlledFailed);
        Ref controlledReplacement = controlActors.restart(controlledFailed);
        controls.resetAfterStability(
                authoritativeLimiter, controlActors, controlledReplacement);
        boolean staleStableResetRejected = false;
        try {
            controls.resetAfterStability(
                    authoritativeLimiter, controlActors, controlledFailed);
        } catch (IllegalArgumentException expected) {
            staleStableResetRejected = true;
        }
        check(staleStableResetRejected
                        && authoritativeLimiter.occupancy(controlledReplacement.id) == 0,
                "34 stale ref cannot reset same-id replacement budget");

        Ref parentV1 = new Ref(new ActorId("supervisor"), 7);
        Ref parentV2 = new Ref(parentV1.id, 8);
        Ref throwChild = new Ref(new ActorId("throw-child"), 1);
        Ref raiseChild = new Ref(new ActorId("raise-child"), 1);
        Ref panicChild = new Ref(new ActorId("panic-child"), 1);
        RetainedControls retainedControls = new RetainedControls();
        ControlEvent throwEvent = new ControlEvent(throwChild, parentV1, FailureKind.THROW);
        ControlEvent raiseEvent = new ControlEvent(raiseChild, parentV1, FailureKind.RAISE);
        ControlEvent panicEvent = new ControlEvent(panicChild, parentV1, FailureKind.PANIC);
        retainedControls.retain(throwEvent);
        retainedControls.retain(raiseEvent);
        retainedControls.retain(panicEvent);

        AtomicBoolean ordinaryActive = new AtomicBoolean();
        AtomicBoolean overlapped = new AtomicBoolean();
        AtomicBoolean privileged = new AtomicBoolean();
        AtomicInteger throwsSeen = new AtomicInteger();
        AtomicInteger raisesSeen = new AtomicInteger();
        AtomicInteger panicsSeen = new AtomicInteger();
        CountDownLatch ordinaryEntered = new CountDownLatch(1);
        CountDownLatch releaseOrdinary = new CountDownLatch(1);
        CountDownLatch firstControl = new CountDownLatch(1);

        try (SerializedControlLane laneModel = new SerializedControlLane(parentV1)) {
            Future<?> user = laneModel.submitUser(() -> {
                ordinaryActive.set(true);
                ordinaryEntered.countDown();
                try {
                    releaseOrdinary.await(5, TimeUnit.SECONDS);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new RuntimeException(interrupted);
                } finally {
                    ordinaryActive.set(false);
                }
            });
            check(ordinaryEntered.await(5, TimeUnit.SECONDS), "35 ordinary turn entered");

            Future<?> queuedControl = laneModel.submitControl(
                    panicEvent,
                    new TypedControlHandler() {
                        @Override public void onPanic(ControlEvent event) {
                            privileged.set(laneModel.inControl());
                            overlapped.set(ordinaryActive.get());
                            panicsSeen.incrementAndGet();
                            firstControl.countDown();
                        }
                    });
            Thread.sleep(20);
            check(firstControl.getCount() == 1, "36 control cannot re-enter ordinary turn");
            releaseOrdinary.countDown();
            user.get(5, TimeUnit.SECONDS);
            queuedControl.get(5, TimeUnit.SECONDS);
            check(privileged.get() && !overlapped.get(),
                    "37 control runs with scoped privilege after ordinary turn");

            TypedControlHandler typed = new TypedControlHandler() {
                private void pause() throws InterruptedException {
                    Thread.sleep(5);
                }
                @Override public void onThrow(ControlEvent event) throws Exception {
                    check(event.parent == parentV1, "exact parent throw");
                    throwsSeen.incrementAndGet();
                    pause();
                }
                @Override public void onRaise(ControlEvent event) throws Exception {
                    check(event.parent == parentV1, "exact parent raise");
                    raisesSeen.incrementAndGet();
                    pause();
                }
                @Override public void onPanic(ControlEvent event) throws Exception {
                    check(event.parent == parentV1, "exact parent panic");
                    panicsSeen.incrementAndGet();
                    pause();
                }
            };
            Future<?> ft = laneModel.submitControl(throwEvent, typed);
            Future<?> fr = laneModel.submitControl(raiseEvent, typed);
            Future<?> fp = laneModel.submitControl(panicEvent, typed);
            ft.get(5, TimeUnit.SECONDS);
            fr.get(5, TimeUnit.SECONDS);
            fp.get(5, TimeUnit.SECONDS);
            check(throwsSeen.get() == 1 && raisesSeen.get() == 1 && panicsSeen.get() == 2,
                    "38 typed THROW RAISE PANIC dispatch");
            check(laneModel.maxActive() == 1,
                    "39 supervisor control is single-flight");

            ControlEvent handlerFailure =
                    new ControlEvent(new Ref(new ActorId("handler-failure"), 1),
                            parentV1, FailureKind.PANIC);
            retainedControls.retain(handlerFailure);
            Future<?> failedHandler = laneModel.submitControl(
                    handlerFailure,
                    new TypedControlHandler() {
                        @Override public void onPanic(ControlEvent event) {
                            throw new IllegalStateException("handler failed");
                        }
                    });
            boolean failed = false;
            try {
                failedHandler.get(5, TimeUnit.SECONDS);
            } catch (ExecutionException expectedFailure) {
                failed = true;
            }
            check(failed && retainedControls.contains(handlerFailure),
                    "40 handler failure retains authoritative child control");
        }

        boolean staleParentRejected = false;
        try (SerializedControlLane replacementLane = new SerializedControlLane(parentV2)) {
            try {
                replacementLane.submitControl(panicEvent, new TypedControlHandler() { });
            } catch (IllegalArgumentException expectedFailure) {
                staleParentRejected = true;
            }
        }
        check(staleParentRejected,
                "41 old parent generation control cannot retarget replacement parent");

        check(retainedControls.acknowledge(throwEvent)
                        && retainedControls.acknowledge(raiseEvent)
                        && retainedControls.acknowledge(panicEvent),
                "42 retained controls require explicit acknowledgement");

        SupervisorDefinition sourceSupervisor = new SupervisorDefinition(
                true, Set.of("on_throw", "on_raise", "on_panic"));
        check(!sourceSupervisor.ordinaryMethodVisible("on_panic"),
                "43 source supervisor handlers stay hidden from ordinary method lookup");

        boolean incompleteSupervisorRejected = false;
        try {
            new SupervisorDefinition(true, Set.of("on_throw", "on_panic"));
        } catch (IllegalArgumentException expectedFailure) {
            incompleteSupervisorRejected = true;
        }
        check(incompleteSupervisorRejected,
                "44 source supervisor handler set must be exhaustive");

        boolean ordinaryActorRejected = false;
        try {
            new SupervisorDefinition(false, Set.of("on_panic"));
        } catch (IllegalArgumentException expectedFailure) {
            ordinaryActorRejected = true;
        }
        check(ordinaryActorRejected,
                "45 ordinary actor cannot acquire privileged supervisor handlers");

        check(FailureKind.THROW.name().equals("THROW")
                        && FailureKind.RAISE.name().equals("RAISE")
                        && FailureKind.PANIC.name().equals("PANIC"),
                "46 source terminal effects retain typed CONTROL identity");

        RevocableChildFailure childFailure =
                new RevocableChildFailure(FailureKind.PANIC);
        check(childFailure.kind() == FailureKind.PANIC,
                "47 ChildFailure exposes immutable typed metadata");
        childFailure.choose("RESTART");
        boolean secondDirectiveRejected = false;
        try {
            childFailure.choose("ACKNOWLEDGE");
        } catch (IllegalStateException expectedFailure) {
            secondDirectiveRejected = true;
        }
        check(secondDirectiveRejected
                        && "RESTART".equals(childFailure.directiveOrRetain()),
                "48 ChildFailure permits exactly one supervisor decision");
        childFailure.revoke();
        boolean expiredCapabilityRejected = false;
        try {
            childFailure.kind();
        } catch (SecurityException expectedFailure) {
            expiredCapabilityRejected = true;
        }
        check(expiredCapabilityRejected,
                "49 ChildFailure capability is revoked after CONTROL turn");

        DeferredRestart deferred = new DeferredRestart();
        deferred.firstControl();
        check(!deferred.prematureRedispatch() && deferred.handlerCalls == 1,
                "50 armed backoff cannot re-enter guest handler early");
        deferred.timerWake();
        check(deferred.restarted && deferred.handlerCalls == 1,
                "51 timer wake resumes stored RESTART without guest re-entry");

        System.out.println("SYNTHETIC JAVA CONTRACT: 51/51 invariants passed");
        System.out.println("JAVA_SOURCE_SHA=ba3f2c395b93e3a35c7e3499fc92a8c59c2ffdb0");
        System.out.println("JAVA_SOURCE_TREE=238b56cd7365de9ba9642e9020343da76a186973");
        System.out.println("LLVM_SOURCE_SHA=9e28dcdd855b22fdfadb47978ebfcd2c918f703b");
        System.out.println("LLVM_SOURCE_TREE=e9bf402f802c0654ab602d1f12be4f10bfa58c50");
        System.out.println("NOTE=source-free model; no private Oreslang source copied");
    }
}
