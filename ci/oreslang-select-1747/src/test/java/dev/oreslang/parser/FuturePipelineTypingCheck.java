package dev.oreslang.parser;

import dev.oreslang.ast.Ast;
import dev.oreslang.types.TypeChecker;

/** Frontend boundaries only; no guest runtime or scheduler is executed here. */
public final class FuturePipelineTypingCheck {
    private FuturePipelineTypingCheck() { }

    public static void main(String[] args) {
        check();
        System.out.println("restricted pipeline typing boundary checks passed (frontend only).");
    }

    static void check() {
        Ast.Program checked = TypeChecker.check(Parser.parse("""
                fnc step(int value): int { return value + 1; }
                fnc main(): int { return 2 |> step; }
                """));
        Ast.FunctionDecl main = (Ast.FunctionDecl) checked.modules().get(0).declarations().get(1);
        Ast.CallExpr call = (Ast.CallExpr) ((Ast.ReturnStmt) main.body().get(0)).value();
        if (call.origin() != Ast.CallOrigin.VALUE_PIPE_ARGUMENT)
            throw new AssertionError("typing erased pipeline provenance");

        TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                fnc observe(Future<int> pending): int { return 1; }
                fnc main(): int { return observe(source()); }
                """));

        // A restricted, pure scalar chain is checked as Future<int> and
        // produces immutable identity-bound lowering proofs at both stages.
        Ast.Program lifted = TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                fnc save(int value): int { return value + 1; }
                fnc twice(int value): int { return value * 2; }
                fnc main(): void {
                  val Future<int> pending = source() |> save() |> twice();
                  val Option<int> result = trap await (pending);
                  return;
                }
                """));
        Ast.FunctionDecl liftedMain = (Ast.FunctionDecl)
                lifted.modules().get(0).declarations().get(3);
        Ast.BindingStmt pending = (Ast.BindingStmt) liftedMain.body().get(0);
        Ast.CallExpr second = (Ast.CallExpr) pending.initializer();
        Ast.CallExpr first = (Ast.CallExpr) second.arguments().getFirst();
        if (!TypeChecker.CheckedFuturePipe.lookup(first)
                || !TypeChecker.CheckedFuturePipe.lookup(second)) {
            throw new AssertionError("checked Future chain lost a stage proof");
        }
        if (TypeChecker.CheckedFuturePipe.target(first)
                != lifted.modules().get(0).declarations().get(1)
                || TypeChecker.CheckedFuturePipe.target(second)
                != lifted.modules().get(0).declarations().get(2)) {
            throw new AssertionError("checked Future stage bound to the wrong declaration identity");
        }
        // Type analysis may stage a plan internally, but it is not executable
        // authority until the ownership pass succeeds.
        TypeChecker.TypeCheckResult typeOnly = TypeChecker.checkTypesWithDiagnostics(
                Parser.parse("""
                        async fnc source(): int { return 1; }
                        fnc keep(int value): int { return value; }
                        fnc main(): void {
                          val Future<int> pending = source() |> keep();
                          return;
                        }
                        """));
        Ast.FunctionDecl typeOnlyMain = (Ast.FunctionDecl)
                typeOnly.program().modules().getFirst().declarations().get(2);
        Ast.CallExpr typeOnlyPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) typeOnlyMain.body().getFirst()).initializer();
        if (TypeChecker.CheckedFuturePipe.lookup(typeOnlyPipe)) {
            throw new AssertionError(
                    "type-only analysis published executable Future-pipeline authority");
        }
        if (!typeOnly.pendingFutureStages().containsKey(typeOnlyPipe)) {
            throw new AssertionError("type-only analysis lost its staged pipeline plan");
        }
        Ast.Program fullyAdmitted = TypeChecker.check(typeOnly.program());
        Ast.FunctionDecl fullyAdmittedMain = (Ast.FunctionDecl)
                fullyAdmitted.modules().getFirst().declarations().get(2);
        Ast.CallExpr fullyAdmittedPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) fullyAdmittedMain.body().getFirst()).initializer();
        if (!TypeChecker.CheckedFuturePipe.lookup(fullyAdmittedPipe)) {
            throw new AssertionError(
                    "full ownership admission did not publish the staged pipeline plan");
        }

        // Ownership rejection after successful typing must also leave the
        // staged pipeline unpublished.
        TypeChecker.TypeCheckResult ownershipPending =
                TypeChecker.checkTypesWithDiagnostics(Parser.parse("""
                        define class Payload as
                          pub val int value = 1;
                        end
                        fnc consume(Payload value): void { return; }
                        async fnc source(): int { return 1; }
                        fnc keep(int value): int { return value; }
                        fnc main(): void {
                          val Future<int> pending = source() |> keep();
                          let Payload payload = new Payload();
                          consume(payload);
                          stdio.println(payload.value);
                          return;
                        }
                        """));
        Ast.FunctionDecl ownershipMain = (Ast.FunctionDecl)
                ownershipPending.program().modules().getFirst().declarations().get(4);
        Ast.CallExpr ownershipPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) ownershipMain.body().getFirst()).initializer();
        try {
            TypeChecker.check(ownershipPending.program());
            throw new AssertionError("ownership-invalid program unexpectedly admitted");
        } catch (IllegalArgumentException expected) {
            if (!expected.getMessage().contains("moved")) {
                throw new AssertionError(
                        "wrong ownership rejection: " + expected.getMessage(), expected);
            }
            if (TypeChecker.CheckedFuturePipe.lookup(ownershipPipe)) {
                throw new AssertionError(
                        "ownership-failed compilation published Future-pipeline authority");
            }
        }

        // A later bad declaration must invalidate the entire compilation
        // rather than leak a proof for an earlier successfully visited pipe.
        Ast.Program invalidWholeProgram = Parser.parse("""
                async fnc source(): int { return 21; }
                fnc save(int value): int { return value + 1; }
                fnc main(): void {
                  val Future<int> pending = source() |> save();
                  return;
                }
                fnc invalid(): bool { return 123; }
                """);
        Ast.FunctionDecl partlyCheckedMain = (Ast.FunctionDecl)
                invalidWholeProgram.modules().getFirst().declarations().get(2);
        Ast.CallExpr unsafeEarlierPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) partlyCheckedMain.body().getFirst()).initializer();
        try {
            TypeChecker.checkTypes(invalidWholeProgram);
            throw new AssertionError("later invalid declaration unexpectedly type-checked");
        } catch (IllegalArgumentException expected) {
            if (TypeChecker.CheckedFuturePipe.lookup(unsafeEarlierPipe)) {
                throw new AssertionError(
                        "failed compilation published a partially checked Future proof");
            }
        }

        // Host code must not have a public proof-minting capability,
        // even if it constructs a matching function declaration and CallExpr.
        try {
            TypeChecker.CheckedFuturePipe.class.getMethod(
                    "record", Ast.CallExpr.class, TypeChecker.PipelinePlan.class);
            throw new AssertionError("pipeline proof minting is publicly accessible");
        } catch (NoSuchMethodException expected) {
            // The compiler is the only permitted issuer.
        }

        if (TypeChecker.CheckedFuturePipe.lookup(
                second.rewrite(second.callee(), second.typeArguments(),
                        second.typeArgumentsPresent(), second.arguments()))) {
            throw new AssertionError("rewritten Future stage inherited stale lowering proof");
        }

        Ast.Program typedAB = TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                fnc render(int value): String { return "ok"; }
                fnc main(): void {
                  val Future<String> pending = source() |> render();
                  return;
                }
                """));
        Ast.FunctionDecl typedABMain = (Ast.FunctionDecl)
                typedAB.modules().getFirst().declarations().get(2);
        Ast.CallExpr typedABPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) typedABMain.body().getFirst()).initializer();
        TypeChecker.PipelinePlan typedABPlan =
                TypeChecker.CheckedFuturePipe.plan(typedABPipe);
        if (typedABPlan == null
                || !typedABPlan.outputTypeRef().name().equals("String")
                || !typedABPlan.stageEffects().isEmpty()
                || !typedABPlan.cumulativeEffects().isEmpty()
                || typedABPlan.effectProvenance()
                        != TypeChecker.PipelineEffectProvenance.COMPLETE_STATIC
                || typedABPlan.inputOwnership()
                        != TypeChecker.PipelineInputOwnership.BORROWED_UPSTREAM_SHARED_PAYLOAD
                || typedABPlan.producedFutureOwnership()
                        != TypeChecker.PipelineStageFutureOwnership.NONE) {
            throw new AssertionError("typed A -> B pipeline plan metadata is incomplete");
        }

        Ast.Program unknownUpstreamEffects = TypeChecker.check(Parser.parse("""
                fnc keep(int value): int { return value; }
                fnc main(Future<int> source): void {
                  val Future<int> pending = source |> keep();
                  return;
                }
                """));
        Ast.FunctionDecl unknownEffectsMain = (Ast.FunctionDecl)
                unknownUpstreamEffects.modules().getFirst().declarations().get(1);
        Ast.CallExpr unknownEffectsPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) unknownEffectsMain.body().getFirst()).initializer();
        TypeChecker.PipelinePlan unknownEffectsPlan =
                TypeChecker.CheckedFuturePipe.plan(unknownEffectsPipe);
        if (unknownEffectsPlan == null
                || unknownEffectsPlan.effectProvenance()
                        != TypeChecker.PipelineEffectProvenance.UPSTREAM_FUTURE_UNKNOWN) {
            throw new AssertionError(
                    "Future binding incorrectly acquired complete upstream effect provenance");
        }

        Ast.Program forwardedFutureEffects = TypeChecker.check(Parser.parse("""
                fnc pass(Future<int> pending): Future<int> { return pending; }
                fnc keep(int value): int { return value; }
                fnc main(Future<int> opaque): void {
                  val Future<int> pending = pass(opaque) |> keep();
                  return;
                }
                """));
        Ast.FunctionDecl forwardedMain = (Ast.FunctionDecl)
                forwardedFutureEffects.modules().getFirst().declarations().get(2);
        Ast.CallExpr forwardedPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) forwardedMain.body().getFirst()).initializer();
        TypeChecker.PipelinePlan forwardedPlan =
                TypeChecker.CheckedFuturePipe.plan(forwardedPipe);
        if (forwardedPlan == null
                || forwardedPlan.effectProvenance()
                        != TypeChecker.PipelineEffectProvenance.UPSTREAM_FUTURE_UNKNOWN) {
            throw new AssertionError(
                    "sync Future forwarding incorrectly acquired complete effect provenance");
        }

        rejected("""
                fnc discard(Box<int> value): int { return 1; }
                fnc main(Future<Box<int>> source): void {
                  val Future<int> pending = source |> discard();
                  return;
                }
                """, "borrowed Future pipeline input requires a Send + Share payload");


        Ast.Program consumedMoveOnly = TypeChecker.check(Parser.parse("""
                define class Payload as
                  pub val int value = 1;
                end
                fnc pass(Payload value): Payload { return value; }
                fnc discard(Payload value): int { return 1; }
                fnc main(Future<Payload> source): void {
                  val Future<int> pending = rt take source |> pass() |> discard();
                  return;
                }
                """));
        Ast.FunctionDecl consumedMoveOnlyMain = (Ast.FunctionDecl)
                consumedMoveOnly.modules().getFirst().declarations().get(3);
        Ast.CallExpr consumedOuter = (Ast.CallExpr)
                ((Ast.BindingStmt) consumedMoveOnlyMain.body().getFirst()).initializer();
        Ast.CallExpr consumedInner = (Ast.CallExpr) consumedOuter.arguments().getFirst();
        TypeChecker.PipelinePlan consumedRootPlan =
                TypeChecker.CheckedFuturePipe.plan(consumedInner);
        TypeChecker.PipelinePlan consumedOuterPlan =
                TypeChecker.CheckedFuturePipe.plan(consumedOuter);
        if (consumedRootPlan == null
                || consumedRootPlan.inputOwnership()
                        != TypeChecker.PipelineInputOwnership.CONSUMED_UPSTREAM_OWNED_PAYLOAD
                || consumedOuterPlan == null
                || consumedOuterPlan.inputOwnership()
                        != TypeChecker.PipelineInputOwnership.OWNED_PIPELINE_VALUE) {
            throw new AssertionError(
                    "consuming Future pipeline did not preserve root/internal ownership states");
        }
        if (consumedRootPlan.effectProvenance()
                != TypeChecker.PipelineEffectProvenance.UPSTREAM_FUTURE_UNKNOWN
                || consumedOuterPlan.effectProvenance()
                != TypeChecker.PipelineEffectProvenance.UPSTREAM_FUTURE_UNKNOWN) {
            throw new AssertionError(
                    "rt take incorrectly upgraded opaque Future effect provenance");
        }

        Ast.Program consumedDirectProducer = TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                fnc keep(int value): int { return value; }
                fnc main(): void {
                  val Future<int> pending = rt take source() |> keep();
                  return;
                }
                """));
        Ast.FunctionDecl consumedDirectMain = (Ast.FunctionDecl)
                consumedDirectProducer.modules().getFirst().declarations().get(2);
        Ast.CallExpr consumedDirectPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) consumedDirectMain.body().getFirst()).initializer();
        TypeChecker.PipelinePlan consumedDirectPlan =
                TypeChecker.CheckedFuturePipe.plan(consumedDirectPipe);
        if (consumedDirectPlan == null
                || consumedDirectPlan.inputOwnership()
                        != TypeChecker.PipelineInputOwnership.CONSUMED_UPSTREAM_OWNED_PAYLOAD
                || consumedDirectPlan.effectProvenance()
                        != TypeChecker.PipelineEffectProvenance.COMPLETE_STATIC) {
            throw new AssertionError(
                    "consumed direct async producer lost ownership/effect provenance");
        }

        rejected("""
                fnc keep(int value): int { return value; }
                fnc main(Future<int> source): void {
                  val Future<int> pending = rt take source |> keep();
                  val Option<int> again = trap await (source);
                  return;
                }
                """, "moved");

        rejected("""
                fnc discard(Rc<int> value): int { return 1; }
                fnc main(Future<Rc<int>> source): void {
                  val Future<int> pending = rt take source |> discard();
                  return;
                }
                """, "owned Future pipeline input requires a Send payload");

        rejected("""
                async fnc source(): int { return 21; }
                fnc keep(int value): int { return value; }
                fnc main(): void {
                  val Future<int> pending =
                      rt take (source() |> keep()) |> keep();
                  return;
                }
                """, "rt take must consume a Future binding or direct producer before Future pipeline lifting");

        Ast.Program syncFutureStage = TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                async fnc delayed(int value): int {
                  rt cooperate;
                  return value + 1;
                }
                fnc bridge(int value): Future<int> { return delayed(value); }
                fnc main(): void {
                  val Future<int> pending = source() |> bridge();
                  return;
                }
                """));
        Ast.FunctionDecl syncFutureMain = (Ast.FunctionDecl)
                syncFutureStage.modules().getFirst().declarations().get(3);
        Ast.CallExpr syncFuturePipe = (Ast.CallExpr)
                ((Ast.BindingStmt) syncFutureMain.body().getFirst()).initializer();
        TypeChecker.PipelinePlan syncFuturePlan =
                TypeChecker.CheckedFuturePipe.plan(syncFuturePipe);
        if (syncFuturePlan == null
                || !syncFuturePlan.flattensFuture()
                || syncFuturePlan.asyncStage()
                || syncFuturePlan.producedFutureOwnership()
                        != TypeChecker.PipelineStageFutureOwnership.OWNED_STAGE_RESULT
                || !syncFuturePlan.outputTypeRef().name().equals("int")) {
            throw new AssertionError("sync Future-returning stage lost flatten/ownership plan");
        }

        rejected("""
                async fnc source(): int { return 1; }
                async fnc fresh(int value): int { return value; }
                fnc pass(Future<int> pending): Future<int> { return pending; }
                fnc bridge(int value): Future<int> {
                  return pass(fresh(value));
                }
                fnc main(): void {
                  val Future<int> nested = source() |> bridge();
                  return;
                }
                """, "borrowed/shared Future results cannot acquire cancellation ownership");

        Ast.Program becomesAsync = TypeChecker.check(Parser.parse("""
                async fnc start_async(int value): int {
                  rt cooperate;
                  return value + 1;
                }
                fnc twice(int value): int { return value * 2; }
                fnc main(): void {
                  val Future<int> pending = 21 |> start_async() |> twice();
                  val int result = await pending;
                  return;
                }
                """));
        Ast.FunctionDecl becomesAsyncMain = (Ast.FunctionDecl)
                becomesAsync.modules().getFirst().declarations().get(2);
        Ast.CallExpr liftedAfterAsync = (Ast.CallExpr)
                ((Ast.BindingStmt) becomesAsyncMain.body().getFirst()).initializer();
        if (!TypeChecker.CheckedFuturePipe.lookup(liftedAfterAsync)) {
            throw new AssertionError("pipeline did not lift after scalar prefix became async");
        }
        Ast.CallExpr eagerAsyncPrefix = (Ast.CallExpr)
                liftedAfterAsync.arguments().getFirst();
        if (TypeChecker.CheckedFuturePipe.lookup(eagerAsyncPrefix)) {
            throw new AssertionError("scalar prefix was incorrectly converted into a deferred stage");
        }

        Ast.Program asyncLifted = TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                async fnc delayed_identity(int value): int {
                  rt cooperate;
                  return value;
                }
                async fnc save(int value): int {
                  return (await delayed_identity(value)) + 1;
                }
                fnc main(): void {
                  val Future<int> pending = source() |> save();
                  val int result = await pending;
                  return;
                }
                """));
        Ast.FunctionDecl asyncMain = (Ast.FunctionDecl)
                asyncLifted.modules().getFirst().declarations().get(3);
        Ast.CallExpr asyncPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) asyncMain.body().getFirst()).initializer();
        if (!TypeChecker.CheckedFuturePipe.lookup(asyncPipe)
                || !TypeChecker.CheckedFuturePipe.target(asyncPipe).async()) {
            throw new AssertionError("checked async Future stage lost its lowering proof");
        }

        rejected("""
                async fnc source(): int { return 21; }
                async fnc save(int value): int {
                  stdio.println(value);
                  return value + 1;
                }
                fnc main(): void {
                  val Future<int> pending = source() |> save();
                  return;
                }
                """, "pure unary sync/async stage body");

        rejected("""
                async fnc source(): int { return 21; }
                fnc save(int value, int extra): int { return value + extra; }
                fnc main(): void {
                  val Future<int> pending = source() |> save(5);
                  return;
                }
                """, "named unary stage with no extra arguments");

        rejected("""
                async fnc source(): int { return 21; }
                fnc save(int value): int {
                  let int other = value;
                  return other;
                }
                fnc main(): void {
                  val Future<int> pending = source() |> save();
                  return;
                }
                """, "pure unary sync/async stage body");

        // Scalar call-shaped producers do not incur speculative Future
        // type checking or acquire a checked Future pipeline proof.
        TypeChecker.check(Parser.parse("""
                fnc source(): int { return 21; }
                fnc save(int value): int { return value + 1; }
                fnc main(): int { return source() |> save(); }
                """));

        TypeChecker.check(Parser.parse("""
                async fnc source(): int { return 21; }
                fnc main(): void {
                  val Option<int> result = trap await (source());
                  return;
                }
                """));

        rejected("""
                async fnc source(): int { return 21; }
                fnc main(): void {
                  val Option<int> result = await trap (source());
                  return;
                }
                """, "await requires Future<T> or ActorRef<T>", "name=Option");
    }

    private static void rejected(String source, String... diagnosticFragments) {
        Ast.Program parsed = Parser.parse(source); // parser rejection is not a typing pass
        try {
            TypeChecker.check(parsed);
        } catch (IllegalArgumentException expected) {
            for (String fragment : diagnosticFragments) {
                if (expected.getMessage() == null || !expected.getMessage().contains(fragment))
                    throw new AssertionError("wrong typing diagnostic: " + expected.getMessage(), expected);
            }
            return;
        }
        throw new AssertionError("unsupported source was admitted: " + source);
    }
}

