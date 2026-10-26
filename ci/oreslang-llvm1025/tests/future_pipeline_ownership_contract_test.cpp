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
            PipelineEffectProvenance::UpstreamFutureUnknown,
        FunctionEffects prior = {},
        FunctionEffects stage = {}) {
    FuturePipelinePlan value;
    value.declaration_identity = identity;
    value.pipeline_node_identity = identity + 10000;
    value.input_ownership = ownership;
    value.input_type = std::move(input);
    value.output_type = std::move(output);
    value.root_stage = root;
    value.effect_provenance = provenance;
    value.prior_effects = prior;
    value.stage_effects = stage;
    value.cumulative_effects = prior.united(stage);
    return value;
}

FuturePipelineOwnershipAdmission admission(
        const FuturePipelinePlan& value,
        bool admitted = true) {
    FuturePipelineOwnershipAdmission result;
    result.declaration_identity = value.declaration_identity;
    result.pipeline_node_identity = value.pipeline_node_identity;
    result.input_ownership = value.input_ownership;
    result.admitted = admitted;
    return result;
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

    // Flatten exactly one freshly-created checked Future.
    auto flattened = plan(
        9,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        scalar(),
        true);
    flattened.flattens_future = true;
    flattened.produced_future_ownership =
        PipelineStageFutureOwnership::OwnedStageResult;
    flattened.produced_future_provenance =
        PipelineStageFutureProvenance::FreshCheckedProducer;
    validate_future_pipeline_plan(flattened);

    auto forwarded = flattened;
    forwarded.declaration_identity = 10;
    forwarded.pipeline_node_identity = 10010;
    forwarded.produced_future_provenance =
        PipelineStageFutureProvenance::BorrowedOrForwarded;
    rejected([&] {
        validate_future_pipeline_plan(forwarded);
    }, "fresh checked stage-created Future");

    auto scalar_claims_future = plan(
        11,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        scalar(),
        true);
    scalar_claims_future.produced_future_ownership =
        PipelineStageFutureOwnership::OwnedStageResult;
    scalar_claims_future.produced_future_provenance =
        PipelineStageFutureProvenance::FreshCheckedProducer;
    rejected([&] {
        validate_future_pipeline_plan(scalar_claims_future);
    }, "scalar Future pipeline stage cannot own");

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

    // Adjacent stage types must compose exactly in this fail-closed proof layer.
    auto root = plan(
        20,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        boxed(scalar()),
        true,
        PipelineEffectProvenance::UpstreamFutureUnknown);
    auto later = plan(
        21,
        PipelineInputOwnership::OwnedPipelineValue,
        boxed(scalar()),
        scalar(),
        false,
        PipelineEffectProvenance::UpstreamFutureUnknown);
    validate_future_pipeline_chain({root, later});

    auto wrong_input = later;
    wrong_input.declaration_identity = 22;
    wrong_input.pipeline_node_identity = 10022;
    wrong_input.input_type = scalar();
    rejected([&] {
        validate_future_pipeline_chain({root, wrong_input});
    }, "adjacent stage types do not compose");

    // Unknown provenance may never become CompleteStatic in a later stage.
    auto upgraded = later;
    upgraded.declaration_identity = 23;
    upgraded.pipeline_node_identity = 10023;
    upgraded.effect_provenance =
        PipelineEffectProvenance::CompleteStatic;
    rejected([&] {
        validate_future_pipeline_chain({root, upgraded});
    }, "cannot upgrade unknown upstream effect provenance");

    // Each plan's cumulative row must be exact: no dropped and no forged extra bits.
    const FunctionEffects throws_effect{
        static_cast<std::uint32_t>(FunctionEffect::Throw)};
    const FunctionEffects io_effect{
        static_cast<std::uint32_t>(FunctionEffect::Io)};

    auto effect_root = plan(
        30,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        scalar(),
        true,
        PipelineEffectProvenance::CompleteStatic,
        throws_effect,
        none);
    validate_future_pipeline_plan(effect_root);

    auto forged_extra = effect_root;
    forged_extra.declaration_identity = 31;
    forged_extra.pipeline_node_identity = 10031;
    forged_extra.cumulative_effects =
        throws_effect.united(io_effect);
    rejected([&] {
        validate_future_pipeline_plan(forged_extra);
    }, "must equal prior plus stage effects");

    auto effect_later = plan(
        32,
        PipelineInputOwnership::OwnedPipelineValue,
        scalar(),
        scalar(),
        false,
        PipelineEffectProvenance::CompleteStatic,
        effect_root.cumulative_effects,
        io_effect);
    validate_future_pipeline_chain({effect_root, effect_later});

    auto mismatched_prior = effect_later;
    mismatched_prior.declaration_identity = 33;
    mismatched_prior.pipeline_node_identity = 10033;
    mismatched_prior.prior_effects = none;
    mismatched_prior.cumulative_effects = io_effect;
    rejected([&] {
        validate_future_pipeline_chain({effect_root, mismatched_prior});
    }, "prior effect row does not match preceding stage");

    // Executable admission checks the cumulative row, so an earlier throw
    // cannot disappear merely because the current stage itself is pure.
    rejected([&] {
        validate_executable_future_pipeline_plan(effect_later);
    }, "exceptional effect execution is not enabled");

    // Invalid/unknown effect bits never become authority.
    auto unknown_effect = plan(
        40,
        PipelineInputOwnership::BorrowedUpstreamSharedPayload,
        scalar(),
        scalar(),
        true);
    unknown_effect.prior_effects = FunctionEffects::unresolved_call();
    unknown_effect.cumulative_effects = FunctionEffects::unresolved_call();
    rejected([&] {
        validate_future_pipeline_plan(unknown_effect);
    }, "unknown effect bits");

    // Publishing requires exact declaration + AST identity + ownership mode.
    auto staged = plan(
        50,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        boxed(scalar()),
        scalar(),
        true);
    assert(!publish_future_pipeline_plan(
        staged, admission(staged, false)).has_value());

    const auto published =
        publish_future_pipeline_plan(staged, admission(staged));
    assert(published.has_value());
    assert(published->declaration_identity == staged.declaration_identity);
    assert(published->pipeline_node_identity == staged.pipeline_node_identity);

    auto wrong_node = admission(staged);
    ++wrong_node.pipeline_node_identity;
    rejected([&] {
        (void)publish_future_pipeline_plan(staged, wrong_node);
    }, "does not match staged proof identity");

    auto wrong_decl = admission(staged);
    ++wrong_decl.declaration_identity;
    rejected([&] {
        (void)publish_future_pipeline_plan(staged, wrong_decl);
    }, "does not match staged proof identity");

    auto wrong_mode = admission(staged);
    wrong_mode.input_ownership =
        PipelineInputOwnership::BorrowedUpstreamSharedPayload;
    rejected([&] {
        (void)publish_future_pipeline_plan(staged, wrong_mode);
    }, "does not match staged proof identity");

    auto unnamed = staged;
    unnamed.pipeline_node_identity = 0;
    rejected([&] {
        (void)publish_future_pipeline_plan(unnamed, admission(unnamed));
    }, "exact declaration and AST identities");

    // Compilation-unit publication is atomic. One failed ownership admission
    // publishes no earlier prefix.
    auto chain_root = plan(
        60,
        PipelineInputOwnership::ConsumedUpstreamOwnedPayload,
        scalar(),
        scalar(),
        true);
    auto chain_later = plan(
        61,
        PipelineInputOwnership::OwnedPipelineValue,
        scalar(),
        scalar(),
        false);
    std::vector<FuturePipelinePlan> chain{chain_root, chain_later};
    std::vector<FuturePipelineOwnershipAdmission> failed_admissions{
        admission(chain_root), admission(chain_later, false)};
    assert(!publish_future_pipeline_chain(
        chain, failed_admissions).has_value());

    std::vector<FuturePipelineOwnershipAdmission> good_admissions{
        admission(chain_root), admission(chain_later)};
    const auto published_chain =
        publish_future_pipeline_chain(chain, good_admissions);
    assert(published_chain.has_value());
    assert(published_chain->size() == 2);

    auto rebound = good_admissions;
    ++rebound[1].pipeline_node_identity;
    rejected([&] {
        (void)publish_future_pipeline_chain(chain, rebound);
    }, "does not match staged proof identity");

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
