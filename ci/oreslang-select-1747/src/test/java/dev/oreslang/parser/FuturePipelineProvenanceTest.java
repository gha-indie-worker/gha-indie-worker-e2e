package dev.oreslang.parser;

import org.junit.jupiter.api.Test;

final class FuturePipelineProvenanceTest {
    @Test void callArgumentsAndTypesAreImmutable() { FuturePipelineProvenanceCheck.callArgumentsAndTypesAreImmutable(); }
    @Test void rewritesPreserveOriginWithoutCopyingProofs() { FuturePipelineProvenanceCheck.rewritesPreserveOriginWithoutCopyingProofs(); }
    @Test void typingPreservesProvenanceAndCurrentFutureBoundaries() { FuturePipelineTypingCheck.check(); }
    @Test void directCallsStayDirect() { FuturePipelineProvenanceCheck.directCallsStayDirect(); }
    @Test void chainedStagesKeepSeparateOrigins() { FuturePipelineProvenanceCheck.chainedStagesKeepSeparateOrigins(); }
    @Test void explicitTypeArgumentsAndArgumentOrderSurvive() { FuturePipelineProvenanceCheck.explicitTypeArgumentsAndArgumentOrderSurvive(); }
    @Test void sumCombinatorsKeepReceiverOrigins() { FuturePipelineProvenanceCheck.sumCombinatorsKeepReceiverOrigins(); }
    @Test void actorPipeStaysDistinct() { FuturePipelineProvenanceCheck.actorPipeStaysDistinct(); }
    @Test void typedLambdaStageKeepsOrigin() { FuturePipelineProvenanceCheck.typedLambdaStageKeepsOrigin(); }
    @Test void trapAwaitOrderSurvives() { FuturePipelineProvenanceCheck.trapAwaitOrderSurvives(); }
    @Test void stageLimitStillApplies() { FuturePipelineProvenanceCheck.stageLimitStillApplies(); }
    @Test void malformedOriginsAreRejected() { FuturePipelineProvenanceCheck.malformedOriginsAreRejected(); }
}
