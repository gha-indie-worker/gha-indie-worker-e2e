package dev.oreslang.compiler;

import java.util.List;
import java.util.Map;
import java.util.Set;

import static dev.oreslang.compiler.RealtimeRegionFlow.*;

/** JDK-only adversarial suite for explicit event-IR actor/region contracts. */
public final class RealtimeRegionFlowSelfTest {
    private RealtimeRegionFlowSelfTest() { }

    private static final Region A = new Region("A", Kind.ACTOR_LOCAL, "actorA", null, null, 1, 1024);
    private static final Region B = new Region("B", Kind.ACTOR_LOCAL, "actorB", null, null, 1, 1024);
    private static final Region S = new Region("S", Kind.SCOPED, "actorA", "A", null, 1, 128);
    private static final Region T = new Region("T", Kind.SCOPED, "actorA", "S", null, 1, 64);
    private static final Region G = new Region("G", Kind.SHARED_IMMUTABLE, null, null, "team", 1, 1024);
    private static final Budget BUDGET = new Budget(2048, 100, 128);
    private static final Map<String, Set<String>> GRANTS = Map.of(
            "actorA", Set.of("team"), "actorB", Set.of("team"));
    private static int checked;

    private static Allocate alloc(String id, String region, long bytes,
                                  boolean frozen, boolean primitive) {
        return new Allocate(new Value(id, region, bytes, frozen, primitive));
    }

    private static Plan plan(List<Step> steps) {
        return new Plan(List.of(A, B, S, T, G), GRANTS, steps);
    }

    private static Report pass(String label, List<Step> steps) {
        checked++;
        try {
            return RealtimeRegionFlow.verify(plan(steps), BUDGET);
        } catch (IllegalArgumentException failure) {
            throw new AssertionError("safe model rejected: " + label, failure);
        }
    }

    private static void deny(String label, List<Step> steps) {
        denyPlan(label, plan(steps), BUDGET);
    }

    private static void denyPlan(String label, Plan plan, Budget budget) {
        checked++;
        try {
            RealtimeRegionFlow.verify(plan, budget);
        } catch (IllegalArgumentException failure) {
            if (failure.getMessage() != null && failure.getMessage().startsWith("RT-REGION:")) return;
            throw new AssertionError("uncategorized rejection for " + label, failure);
        }
        throw new AssertionError("unsafe model accepted: " + label);
    }

    public static void main(String[] args) {
        Report nested = pass("parent refs stay alive", List.of(
                alloc("parent", "A", 16, false, false),
                alloc("child", "S", 8, false, false),
                alloc("leaf", "T", 4, false, false),
                new StoreReference("child", "parent"),
                new StoreReference("leaf", "child"),
                new ReleaseScope("T"),
                new ReleaseScope("S")));
        if (nested.checkedSteps() != 7 || nested.allocatedBytes() != 28) {
            throw new AssertionError("wrong positive resource bounds");
        }
        Report sharing = pass("explicit immutable sharing and scalar transfer", List.of(
                alloc("number", "A", 4, true, true),
                alloc("snapshot", "G", 20, true, false),
                new ShareToActor("snapshot", "actorA", "actorB"),
                new CopyToActor("number", "actorB"),
                new Call("pureHelper", Assurance.PROVEN, 10, false, false,
                        false, false, false, false),
                new Suspend()));
        if (sharing.copiedBytes() != 14) throw new AssertionError("copy accounting");

        deny("short scope escapes to parent", List.of(
                alloc("holder", "A", 8, false, false),
                alloc("referent", "S", 8, false, false),
                new StoreReference("holder", "referent")));
        deny("sibling/child lifetime inversion", List.of(
                alloc("holder", "S", 8, false, false),
                alloc("referent", "T", 8, false, false),
                new StoreReference("holder", "referent")));
        deny("raw cross-actor reference", List.of(
                alloc("holder", "A", 8, false, false),
                alloc("private", "B", 8, false, false),
                new StoreReference("holder", "private")));
        deny("actor-private frozen is not authorized shared", List.of(
                alloc("private", "A", 8, true, false),
                new ShareToActor("private", "actorA", "actorB")));
        denyPlan("missing share grant", new Plan(List.of(A, B, G),
                Map.of("actorA", Set.of("team")), List.of(
                alloc("snapshot", "G", 12, true, false),
                new ShareToActor("snapshot", "actorA", "actorB"))), BUDGET);
        deny("mutable shared object", List.of(alloc("mutable", "G", 8, false, false)));
        deny("shared referencing actor heap", List.of(
                alloc("shared", "G", 8, true, false),
                alloc("private", "A", 8, false, false),
                new StoreReference("shared", "private")));
        deny("immutable region can't point across unrelated domains", List.of(
                alloc("shared", "G", 8, true, false),
                alloc("private", "B", 8, false, false),
                new StoreReference("shared", "private")));
        deny("mutable and immutable borrow conflict", List.of(
                alloc("owner", "A", 8, false, false),
                new Borrow("reader", "owner", false),
                new Borrow("writer", "owner", true)));
        deny("live borrow across await", List.of(
                alloc("owner", "S", 8, false, false),
                new Borrow("reader", "owner", false),
                new Suspend()));
        deny("release with live loan", List.of(
                alloc("owner", "S", 8, false, false),
                new Borrow("loan", "owner", false),
                new ReleaseScope("S")));
        deny("use after scoped lifetime ended", List.of(
                alloc("owner", "S", 8, false, false),
                new ReleaseScope("T"),
                new ReleaseScope("S"),
                new Borrow("late", "owner", false)));
        deny("release parent before child", List.of(new ReleaseScope("S")));
        deny("write while shared loan active", List.of(
                alloc("owner", "A", 8, false, false),
                new Borrow("reader", "owner", false),
                new Write("owner")));
        deny("mutate immutable value", List.of(
                alloc("owner", "G", 8, true, false),
                new Write("owner")));
        deny("copy unknown object representation", List.of(
                alloc("owner", "A", 8, false, false),
                new CopyToActor("owner", "actorB")));
        deny("region capacity overflow", List.of(
                alloc("oversized", "S", 129, true, true)));
        denyPlan("allocation total over budget",
                plan(List.of(alloc("x", "A", 100, false, false))),
                new Budget(99, 100, 128));
        denyPlan("copy bound exceeded", plan(List.of(
                alloc("x", "A", 32, true, true),
                new CopyToActor("x", "actorB"))), new Budget(2048, 31, 128));
        deny("unknown effects", List.of(
                new Call("external", Assurance.UNKNOWN, 0, false, false,
                        false, false, false, false)));
        deny("measured cannot substitute for proof", List.of(
                new Call("measured", Assurance.MEASURED, 0, false, false,
                        false, false, false, false)));
        deny("managed-heap access", List.of(
                new Call("vm", Assurance.PROVEN, 0, true, false,
                        false, false, false, false)));
        deny("unqualified FFI", List.of(
                new Call("foreign", Assurance.CONTRACTED, 0, false, false,
                        false, true, false, false)));
        deny("blocking and suspension", List.of(
                new Call("io", Assurance.CONTRACTED, 0, false, true,
                        true, false, false, false)));
        deny("dynamic classloading", List.of(
                new Call("reflective", Assurance.PROVEN, 0, false, false,
                        false, false, true, false)));
        deny("unchecked exceptional path", List.of(
                new Call("throws", Assurance.CONTRACTED, 0, false, false,
                        false, false, false, true)));
        denyPlan("unqualified managed heap allocation",
                new Plan(List.of(new Region("heap", Kind.MANAGED_HEAP,
                        null, null, null, 0, 100)), Map.of(),
                        List.of(alloc("x", "heap", 8, false, false))), BUDGET);
        denyPlan("cyclic region ancestry",
                new Plan(List.of(new Region("X", Kind.SCOPED, "actorA", "Y", null, 1, 100),
                        new Region("Y", Kind.SCOPED, "actorA", "X", null, 1, 100)),
                        Map.of(), List.of()), BUDGET);
        denyPlan("duplicate region identity",
                new Plan(List.of(A, A), Map.of(), List.of()), BUDGET);
        denyPlan("unbounded steps",
                plan(List.of(new Suspend(), new Suspend())),
                new Budget(1000, 1000, 1));

        if (checked < 30) throw new AssertionError("insufficient audit coverage");
        System.out.println("PASS: " + checked + " actor-region provenance and resource cases");
    }
}
