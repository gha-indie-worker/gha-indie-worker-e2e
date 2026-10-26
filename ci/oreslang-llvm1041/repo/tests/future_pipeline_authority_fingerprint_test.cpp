#include "oreslang/frontend/Parser.hpp"

#include <cassert>
#include <cstdint>

using namespace oreslang;

int main() {
    SourceFuturePipelineI64Binding pipeline;
    pipeline.name = "pending";
    pipeline.producer_function = "source";
    pipeline.stages = {"plus_one", "twice"};
    pipeline.root_consumed = true;

    types::FuturePipelinePlan first;
    first.declaration_identity = 11;
    first.pipeline_node_identity = 101;
    first.input_ownership =
        types::PipelineInputOwnership::ConsumedUpstreamOwnedPayload;
    first.effect_provenance =
        types::PipelineEffectProvenance::CompleteStatic;
    first.root_stage = true;

    types::FuturePipelinePlan second;
    second.declaration_identity = 12;
    second.pipeline_node_identity = 102;
    second.input_ownership =
        types::PipelineInputOwnership::OwnedPipelineValue;
    second.effect_provenance =
        types::PipelineEffectProvenance::CompleteStatic;

    pipeline.plans = {first, second};
    const std::uint64_t authorized =
        future_pipeline_authority_fingerprint(pipeline);
    assert(authorized != 0);

    auto rewritten_stage = pipeline;
    rewritten_stage.stages[1] = "other";
    assert(future_pipeline_authority_fingerprint(rewritten_stage) != authorized);

    auto rebound_decl = pipeline;
    ++rebound_decl.plans[0].declaration_identity;
    assert(future_pipeline_authority_fingerprint(rebound_decl) != authorized);

    auto rewritten_node = pipeline;
    ++rewritten_node.plans[1].pipeline_node_identity;
    assert(future_pipeline_authority_fingerprint(rewritten_node) != authorized);

    auto changed_mode = pipeline;
    changed_mode.plans[0].input_ownership =
        types::PipelineInputOwnership::BorrowedUpstreamSharedPayload;
    assert(future_pipeline_authority_fingerprint(changed_mode) != authorized);

    auto changed_effects = pipeline;
    changed_effects.plans[1].cumulative_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::Io);
    assert(future_pipeline_authority_fingerprint(changed_effects) != authorized);

    auto changed_input_type = pipeline;
    changed_input_type.plans[0].input_type.kind =
        types::AutoTypeKind::Rc;
    assert(future_pipeline_authority_fingerprint(changed_input_type) != authorized);

    auto changed_output_type = pipeline;
    changed_output_type.plans[1].output_type.kind =
        types::AutoTypeKind::Box;
    assert(future_pipeline_authority_fingerprint(changed_output_type) != authorized);

    auto changed_flattening = pipeline;
    changed_flattening.plans[0].flattens_future = true;
    assert(future_pipeline_authority_fingerprint(changed_flattening) != authorized);

    auto changed_future_provenance = pipeline;
    changed_future_provenance.plans[0].produced_future_provenance =
        types::PipelineStageFutureProvenance::FreshCheckedProducer;
    assert(future_pipeline_authority_fingerprint(changed_future_provenance) != authorized);

    auto changed_await = pipeline;
    changed_await.awaited = true;
    assert(future_pipeline_authority_fingerprint(changed_await) != authorized);

    auto changed_offset = pipeline;
    ++changed_offset.offset;
    assert(future_pipeline_authority_fingerprint(changed_offset) != authorized);

    auto changed_root = pipeline;
    changed_root.root_consumed = false;
    assert(future_pipeline_authority_fingerprint(changed_root) != authorized);

    auto changed_producer = pipeline;
    changed_producer.producer_function = "other_source";
    assert(future_pipeline_authority_fingerprint(changed_producer) != authorized);

    return 0;
}
