package synthetic;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

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
        private final int max;
        private final long window;
        private final long minBackoff;
        private final long maxBackoff;
        private final ArrayDeque<Long> times = new ArrayDeque<>();
        private long lastGeneration;
        private String lastDecision;
        private long now;

        RestartLimiter(int max, long window, long minBackoff, long maxBackoff) {
            this.max = max;
            this.window = window;
            this.minBackoff = minBackoff;
            this.maxBackoff = maxBackoff;
        }

        String decide(long generation) {
            if (generation < lastGeneration) throw new IllegalArgumentException("stale");
            if (generation == lastGeneration && lastDecision != null) {
                return lastDecision;
            }
            while (!times.isEmpty() && now - times.peekFirst() >= window) {
                times.removeFirst();
            }
            lastGeneration = generation;
            if (times.size() >= max) {
                return lastDecision = "ESCALATE";
            }
            int ordinal = times.size() + 1;
            long backoff = minBackoff;
            for (int i = 1; i < ordinal && backoff < maxBackoff; i++) {
                backoff = Math.min(maxBackoff, backoff * 2);
            }
            times.addLast(now);
            return lastDecision = "RESTART:" + backoff;
        }

        int occupancy() { return times.size(); }
        void tick(long delta) { now += delta; }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
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

        RestartLimiter limiter = new RestartLimiter(3, 10_000, 100, 800);
        check("RESTART:100".equals(limiter.decide(1)), "17 first bounded backoff");
        check("RESTART:100".equals(limiter.decide(1)) && limiter.occupancy() == 1,
                "18 repeated CONTROL observation is idempotent");
        limiter.tick(1);
        check("RESTART:200".equals(limiter.decide(2)), "19 exponential backoff");
        limiter.tick(1);
        check("RESTART:400".equals(limiter.decide(3)), "20 third bounded restart");
        limiter.tick(1);
        check("ESCALATE".equals(limiter.decide(4)), "21 restart storm escalates");

        System.out.println("SYNTHETIC JAVA CONTRACT: 21/21 invariants passed");
        System.out.println("JAVA_SOURCE_SHA=2b4288a03e108359c4d9ed936fd57a5f03556b66");
        System.out.println("JAVA_SOURCE_TREE=a962d1e89d2091bfc62e74d89aff42c1472c46e5");
        System.out.println("LLVM_SOURCE_SHA=910a4b4cd1822192e8ab793e760dc6763b270ec7");
        System.out.println("LLVM_SOURCE_TREE=107219a9d147c8edf61042df799d3da50cdb17ad");
        System.out.println("NOTE=source-free model; no private Oreslang source copied");
    }
}
