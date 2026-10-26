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

        // Explicit rt take transfers the root Future. Send-only Box<T> may
        // therefore cross that root edge, and the next edge is pipeline-owned
        // rather than being reclassified as another borrowed observation.
        Ast.Program consumedChain = TypeChecker.check(Parser.parse("""
                fnc keep_box(Box<int> value): Box<int> { return value; }
                fnc discard(Box<int> value): int { return 1; }
                fnc main(Future<Box<int>> source): void {
                  val Future<int> pending = rt take source |> keep_box() |> discard();
                  return;
                }
                """));
        Ast.FunctionDecl consumedMain = (Ast.FunctionDecl)
                consumedChain.modules().getFirst().declarations().get(2);
        Ast.CallExpr consumedOuter = (Ast.CallExpr)
                ((Ast.BindingStmt) consumedMain.body().getFirst()).initializer();
        Ast.CallExpr consumedRoot = (Ast.CallExpr)
                consumedOuter.arguments().getFirst();
        TypeChecker.PipelinePlan consumedRootPlan =
                TypeChecker.CheckedFuturePipe.plan(consumedRoot);
        TypeChecker.PipelinePlan consumedOuterPlan =
                TypeChecker.CheckedFuturePipe.plan(consumedOuter);
        if (consumedRootPlan == null
                || consumedRootPlan.inputOwnership()
                        != TypeChecker.PipelineInputOwnership.CONSUMED_UPSTREAM_OWNED_PAYLOAD) {
            throw new AssertionError("rt take root lost consuming pipeline ownership");
        }
        if (consumedOuterPlan == null
                || consumedOuterPlan.inputOwnership()
                        != TypeChecker.PipelineInputOwnership.OWNED_PIPELINE_VALUE) {
            throw new AssertionError("later pipeline edge did not become pipeline-owned");
        }
        if (consumedRootPlan.effectProvenance()
                != TypeChecker.PipelineEffectProvenance.UPSTREAM_FUTURE_UNKNOWN
                || consumedOuterPlan.effectProvenance()
                        != TypeChecker.PipelineEffectProvenance.UPSTREAM_FUTURE_UNKNOWN) {
            throw new AssertionError("rt take incorrectly upgraded opaque Future effect provenance");
        }

        // The pipeline proof must reuse canonical rt take move tracking. A
        // source binding cannot be observed again after it was transferred.
        rejected("""
                fnc discard(Box<int> value): int { return 1; }
                fnc main(Future<Box<int>> source): void {
                  val Future<int> pending = rt take source |> discard();
                  val Future<Box<int>> reused = source;
                  return;
                }
                """, "moved");

        // Keep the first implementation fail-closed: taking an already-lifted
        // pipeline would need distinct cancellation/ownership reassociation.
        rejected("""
                fnc keep(int value): int { return value; }
                fnc twice(int value): int { return value * 2; }
                fnc main(Future<int> source): void {
                  val Future<int> pending = rt take (source |> keep()) |> twice();
                  return;
                }
                """, "taking an already-lifted pipeline is not yet supported");

        // Consuming removes the Share requirement, not the Send requirement.
        rejected("""
                fnc discard(Rc<int> value): int { return 1; }
                fnc main(Future<Rc<int>> source): void {
                  val Future<int> pending = rt take source |> discard();
                  return;
                }
                """, "owned Future pipeline input requires a Send payload");

        // Ownership transfer is independent of effect provenance. A direct
        // checked async producer remains COMPLETE_STATIC when explicitly taken.
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

        // A consumed pipeline that type-checks but fails ownership later must
        // not publish executable authority for its already-staged pipe.
        TypeChecker.TypeCheckResult consumedOwnershipPending =
                TypeChecker.checkTypesWithDiagnostics(Parser.parse("""
                        fnc keep(int value): int { return value; }
                        fnc main(Future<int> source): void {
                          val Future<int> pending = rt take source |> keep();
                          val Option<int> reused = trap await (source);
                          return;
                        }
                        """));
        Ast.FunctionDecl consumedOwnershipMain = (Ast.FunctionDecl)
                consumedOwnershipPending.program().modules().getFirst().declarations().get(1);
        Ast.CallExpr consumedOwnershipPipe = (Ast.CallExpr)
                ((Ast.BindingStmt) consumedOwnershipMain.body().getFirst()).initializer();
        if (!consumedOwnershipPending.pendingFutureStages().containsKey(consumedOwnershipPipe)) {
            throw new AssertionError("consumed pipeline was not staged before ownership admission");
        }
        try {
            TypeChecker.check(consumedOwnershipPending.program());
            throw new AssertionError("use-after-take unexpectedly passed ownership admission");
        } catch (IllegalArgumentException expected) {
            if (expected.getMessage() == null || !expected.getMessage().contains("moved")) {
                throw new AssertionError(
                        "wrong consumed ownership rejection: " + expected.getMessage(), expected);
            }
            if (TypeChecker.CheckedFuturePipe.lookup(consumedOwnershipPipe)) {
                throw new AssertionError(
                        "ownership-failed consumed pipeline published executable authority");
            }
        }

        // Two consuming pipelines cannot both take the same affine Future.
        // Failure of the second take must keep both staged plans unpublished.
        TypeChecker.TypeCheckResult repeatedTakePending =
                TypeChecker.checkTypesWithDiagnostics(Parser.parse("""
                        fnc keep(int value): int { return value; }
                        fnc main(Future<int> source): void {
                          val Future<int> first = rt take source |> keep();
                          val Future<int> second = rt take source |> keep();
                          return;
                        }
                        """));
        Ast.FunctionDecl repeatedTakeMain = (Ast.FunctionDecl)
                repeatedTakePending.program().modules().getFirst().declarations().get(1);
        Ast.CallExpr firstTakePipe = (Ast.CallExpr)
                ((Ast.BindingStmt) repeatedTakeMain.body().get(0)).initializer();
        Ast.CallExpr secondTakePipe = (Ast.CallExpr)
                ((Ast.BindingStmt) repeatedTakeMain.body().get(1)).initializer();
        if (!repeatedTakePending.pendingFutureStages().containsKey(firstTakePipe)
                || !repeatedTakePending.pendingFutureStages().containsKey(secondTakePipe)) {
            throw new AssertionError("repeated take pipelines were not both staged");
        }
        try {
            TypeChecker.check(repeatedTakePending.program());
            throw new AssertionError("second rt take of the same Future unexpectedly passed");
        } catch (IllegalArgumentException expected) {
            if (expected.getMessage() == null || !expected.getMessage().contains("moved")) {
                throw new AssertionError(
                        "wrong repeated-take rejection: " + expected.getMessage(), expected);
            }
            if (TypeChecker.CheckedFuturePipe.lookup(firstTakePipe)
                    || TypeChecker.CheckedFuturePipe.lookup(secondTakePipe)) {
                throw new AssertionError(
                        "failed repeated-take admission published a pipeline proof");
            }
        }

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

