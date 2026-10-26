#include "oreslang/types/FuturePipelineContract.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using oreslang::FunctionEffect;
using oreslang::FunctionEffects;
using namespace oreslang::types;

namespace {

AutoType scalar() {
    AutoType result;
    result.kind = AutoTypeKind::Scalar;
    return result;
}

AutoType boxed(AutoType value) {
    AutoType result;
    result.kind = AutoTypeKind::Box;
    result.parameters.push_back(std::move(value));
    return result;
}

AutoType rc(AutoType value) {
    AutoType result;
    result.kind = AutoTypeKind::Rc;
    result.parameters.push_back(std::move(value));
    return result;
}

FuturePipelinePlan plan(
        std::uint64_t identity,
        PipelineInputOwnership ownership,
        AutoType input,
        AutoType output,
        bool root,
        PipelineEffectProvenance provenance =
            PipelineEffectProvenance::UpstreamFutureUnknown) {
    FuturePipelinePlan value;
    value.declaration_identity = identity;
    value.input_ownership = ownership;
    value.input_type = std::move(input);
    value.output_type = std::move(output);
    value.root_stage = root;
    value.effect_provenance = provenance;
    return value;
}

template <class Fn>
void rejected(Fn&& fn, const char* expected) {
    try {
        fn();
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()).find(expected) != std::string::npos);
        return;
    }
    assert(false && "expected Future pipeline contract rejection");
}

} // namespace

int main() {
    // Borrowed roots create an aliasing observation and therefore need Share.
    validate_future_pipeline_plan(plan(
        1,
        PipelineInputOwnership::BorrowedUpstreamSharedPayload,
        scalar(),
        scalar(),
        true));

    rejected([&] {
        validate_future_pipeline_plan(plan(
            2,
            PipelineInputOwnership::BorrowedUpstreamSharedPayload,
            boxed(scalar()),
            scalar(),
            true));
    }, "Send + Share");

    // rt take removes only the Share requirement. Box<T> remains Send.
    validate_future_pipeline_plan(plan(
        3,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        boxed(scalar()),
        scalar(),
        true));

    // Consuming cannot launder a non-Send payload.
    rejected([&] {
        validate_future_pipeline_plan(plan(
            4,
            PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
            rc(scalar()),
            scalar(),
            true));
    }, "requires Send");

    // Later pipeline-owned move-only values are Send-only, never root-borrowed.
    validate_future_pipeline_plan(plan(
        5,
        PipelineInputOwnership::OwnedPipelineValue,
        boxed(scalar()),
        scalar(),
        false));

    rejected([&] {
        validate_future_pipeline_plan(plan(
            6,
            PipelineInputOwnership::BorrowedUpstreamSharedPayload,
            scalar(),
            scalar(),
            false));
    }, "non-root edge must be pipeline-owned");

    rejected([&] {
        validate_future_pipeline_plan(plan(
            7,
            PipelineInputOwnership::OwnedPipelineValue,
            scalar(),
            scalar(),
            true));
    }, "root cannot be an internal owned edge");

    // Output crosses the tracked Task/Future boundary and must be Send.
    rejected([&] {
        validate_future_pipeline_plan(plan(
            8,
            PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
            scalar(),
            rc(scalar()),
            true));
    }, "output requires Send");

    // Flatten exactly one fresh stage-created Future and own its cancellation.
    auto flattened = plan(
        9,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        scalar(),
        true);
    flattened.flattens_future = true;
    flattened.produced_future_ownership =
        PipelineStageFutureOwnership::OwnedStageResult;
    validate_future_pipeline_plan(flattened);

    auto forged_flatten = flattened;
    forged_flatten.declaration_identity = 10;
    forged_flatten.produced_future_ownership =
        PipelineStageFutureOwnership::None;
    rejected([&] {
        validate_future_pipeline_plan(forged_flatten);
    }, "flattening must own exactly one");

    // Unknown upstream effect provenance survives both rt take and later edges.
    const FunctionEffects none{};
    const auto opaque = pipeline_root_effects(none, false);
    assert(opaque.provenance ==
        PipelineEffectProvenance::UpstreamFutureUnknown);
    assert(pipeline_root_ownership(true) ==
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload);
    const auto opaque_after_stage = opaque.extend(none);
    assert(opaque_after_stage.provenance ==
        PipelineEffectProvenance::UpstreamFutureUnknown);

    // A direct exact producer is complete because of producer identity,
    // independently of whether its Future is borrowed or consumed.
    const auto direct = pipeline_root_effects(none, true);
    assert(direct.provenance ==
        PipelineEffectProvenance::CompleteStatic);
    assert(pipeline_root_ownership(false) ==
        PipelineInputOwnership::BorrowedUpstreamSharedPayload);
    assert(pipeline_root_ownership(true) ==
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload);

    // Unknown provenance may never become CompleteStatic in a later stage.
    auto root = plan(
        11,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        scalar(),
        true,
        PipelineEffectProvenance::UpstreamFutureUnknown);
    auto later = plan(
        12,
        PipelineInputOwnership::OwnedPipelineValue,
        scalar(),
        scalar(),
        false,
        PipelineEffectProvenance::CompleteStatic);
    rejected([&] {
        validate_future_pipeline_chain({root, later});
    }, "cannot upgrade unknown upstream effect provenance");

    later.effect_provenance =
        PipelineEffectProvenance::UpstreamFutureUnknown;
    validate_future_pipeline_chain({root, later});

    // Cumulative checked effects are monotonic and cannot be stripped.
    root.stage_effects = {
        static_cast<std::uint32_t>(FunctionEffect::Throw)};
    root.cumulative_effects = root.stage_effects;
    later.stage_effects = {};
    later.cumulative_effects = {};
    rejected([&] {
        validate_future_pipeline_chain({root, later});
    }, "cumulative effects are not monotonic");

    later.cumulative_effects = root.cumulative_effects;
    validate_future_pipeline_chain({root, later});

    // Execution remains fail-closed for exceptional effects even though the
    // plan retains them for future lowering.
    rejected([&] {
        validate_executable_future_pipeline_plan(root);
    }, "exceptional effect execution is not enabled");

    // Invalid/unknown effect bits never become authority.
    auto unknown_effect = plan(
        13,
        PipelineInputOwnership::BorrowedUpstreamSharedPayload,
        scalar(),
        scalar(),
        true);
    unknown_effect.stage_effects = FunctionEffects::unresolved_call();
    unknown_effect.cumulative_effects = FunctionEffects::unresolved_call();
    rejected([&] {
        validate_future_pipeline_plan(unknown_effect);
    }, "unknown effect bits");

    // Type checking can stage a proof, but ownership failure publishes none.
    auto staged = plan(
        14,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        boxed(scalar()),
        scalar(),
        true);
    assert(!publish_future_pipeline_plan(staged, false).has_value());
    const auto published = publish_future_pipeline_plan(staged, true);
    assert(published.has_value());
    assert(published->declaration_identity == 14);

    auto unnamed = staged;
    unnamed.declaration_identity = 0;
    rejected([&] {
        (void)publish_future_pipeline_plan(unnamed, true);
    }, "exact declaration identity");

    // The same hard stage-count bound as Java/Graal fails before traversal.
    std::vector<FuturePipelinePlan> too_many;
    too_many.reserve(129);
    too_many.push_back(plan(
        100,
        PipelineInputOwnership::BorrowedUpstreamSharedPayload,
        scalar(),
        scalar(),
        true));
    for (std::uint64_t i = 1; i < 129; ++i) {
        too_many.push_back(plan(
            100 + i,
            PipelineInputOwnership::OwnedPipelineValue,
            scalar(),
            scalar(),
            false));
    }
    rejected([&] {
        validate_future_pipeline_chain(too_many);
    }, "exceeds 128 stages");

    return 0;
}
