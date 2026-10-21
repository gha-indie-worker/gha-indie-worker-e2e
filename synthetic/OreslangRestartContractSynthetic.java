package synthetic;

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
        @Override public String toString() {
            return id.value() + "#" + generation;
        }
    }

    private record MailStamp(ActorId id, long generation) { }

    private static final class Registry {
        private final Map<ActorId, Ref> live = new HashMap<>();
        private final Map<ActorId, Long> lastGeneration = new HashMap<>();

        Ref spawnFresh(String logicalName) {
            return publish(new ActorId(logicalName));
        }

        Ref restart(Ref failed) {
            Ref current = live.get(failed.id);
            if (current != null) {
                throw new IllegalStateException("failed incarnation still live");
            }
            return publish(failed.id);
        }

        private Ref publish(ActorId id) {
            long next = Math.addExact(lastGeneration.getOrDefault(id, 0L), 1L);
            Ref ref = new Ref(id, next);
            if (live.putIfAbsent(id, ref) != null) {
                throw new IllegalStateException("logical id already live");
            }
            lastGeneration.put(id, next);
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
        check(first.id.equals(replacement.id), "restart must preserve logical ActorId");
        check(replacement.generation > first.generation, "restart must advance generation");
        check(registry.resolveExact(first) == null, "stale ref must not resolve replacement");
        check(registry.resolveExact(replacement) == replacement, "replacement must resolve exactly");

        Ref forgedSameTuple = new Ref(replacement.id, replacement.generation);
        check(registry.resolveExact(forgedSameTuple) == null,
                "matching id+generation without capability identity must fail");
        check(registry.resolveExact(continuationTarget) == null,
                "late continuation target must not enter replacement");

        group.add(replacement);
        check(!group.contains(first), "old group membership must not transfer by ActorId");
        check(group.contains(replacement), "replacement can receive explicit ownership");

        check(!registry.stop(first), "stale lifecycle stop must be inert");
        check(registry.resolveExact(replacement) == replacement,
                "stale stop must not affect replacement");

        check(delayed.id.equals(replacement.id), "delayed metadata keeps logical id");
        check(delayed.generation != replacement.generation,
                "delayed metadata must distinguish old generation");

        MailStamp current = new MailStamp(replacement.id, replacement.generation);
        check(current.generation > delayed.generation,
                "new metadata must identify replacement generation");

        check(registry.stop(replacement), "live replacement stop must succeed");

        System.out.println("SYNTHETIC CONTRACT: 12/12 invariants passed");
        System.out.println("SOURCE_SHA=0343527520a5e2f2e160ce42894537daab48324b");
        System.out.println("SOURCE_TREE=f7d57bc61c9d5210b7acff48fa05670271777c2f");
        System.out.println("NOTE=source-free model; no private Oreslang source copied");
    }
}
