#pragma once

#include "oreslang/types/AutoTraitRules.hpp"
#include "oreslang/types/FunctionEffects.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace oreslang::types {

/**
 * Exact LLVM-side counterpart of the Java/Graal checked Future-pipeline
 * ownership edge. This is compiler proof metadata, not a runtime capability.
 */
enum class PipelineInputOwnership : std::uint8_t {
    BorrowedUpstreamSharedPayload,
    ConsumedUpstreamOwnedPayload,
    OwnedPipelineValue
};

enum class PipelineEffectProvenance : std::uint8_t {
    CompleteStatic,
    UpstreamFutureUnknown
};

enum class PipelineStageFutureOwnership : std::uint8_t {
    None,
    OwnedStageResult
};

struct PipelineEffectSummary {
    FunctionEffects effects{};
    PipelineEffectProvenance provenance =
        PipelineEffectProvenance::UpstreamFutureUnknown;

    PipelineEffectSummary extend(FunctionEffects stage) const {
        if (!effects.valid() || !stage.valid())
            throw std::invalid_argument(
                "Future pipeline effect summary contains unknown effect bits");
        return {effects.united(stage), provenance};
    }
};

/**
 * Effect provenance is established by the producer proof only. Ownership
 * transfer is deliberately absent from this API so rt take cannot launder an
 * opaque/forwarded Future into CompleteStatic.
 */
inline PipelineEffectSummary pipeline_root_effects(
        FunctionEffects upstream,
        bool exact_static_producer) {
    if (!upstream.valid())
        throw std::invalid_argument(
            "Future pipeline upstream effect summary contains unknown effect bits");
    return {
        upstream,
        exact_static_producer
            ? PipelineEffectProvenance::CompleteStatic
            : PipelineEffectProvenance::UpstreamFutureUnknown
    };
}

inline PipelineInputOwnership pipeline_root_ownership(bool explicit_take) noexcept {
    return explicit_take
        ? PipelineInputOwnership::ConsumedUpstreamOwnedPayload
        : PipelineInputOwnership::BorrowedUpstreamSharedPayload;
}

inline constexpr PipelineInputOwnership pipeline_internal_ownership() noexcept {
    return PipelineInputOwnership::OwnedPipelineValue;
}

inline bool has_exceptional_pipeline_effects(FunctionEffects effects) noexcept {
    return effects.has(FunctionEffect::Throw)
        || effects.has(FunctionEffect::Raise)
        || effects.has(FunctionEffect::Panic);
}

inline void require_pipeline_input_traits(
        PipelineInputOwnership ownership,
        const AutoType& type) {
    const Capabilities caps = infer_auto_traits(type);
    if (ownership == PipelineInputOwnership::BorrowedUpstreamSharedPayload) {
        if (!caps.send || !caps.share)
            throw std::invalid_argument(
                "borrowed Future pipeline input requires Send + Share");
        return;
    }
    if (!caps.send)
        throw std::invalid_argument(
            "owned Future pipeline input requires Send");
}

inline void require_pipeline_output_traits(const AutoType& type) {
    if (!infer_auto_traits(type).send)
        throw std::invalid_argument(
            "Future pipeline output requires Send");
}

/**
 * Identity-bound staged compiler plan. declaration_identity must be the exact
 * resolved callable identity, never a name-only lookup token.
 */
struct FuturePipelinePlan {
    std::uint64_t declaration_identity = 0;
    AutoType input_type{};
    AutoType output_type{};
    FunctionEffects stage_effects{};
    FunctionEffects cumulative_effects{};
    PipelineEffectProvenance effect_provenance =
        PipelineEffectProvenance::UpstreamFutureUnknown;
    PipelineInputOwnership input_ownership =
        PipelineInputOwnership::BorrowedUpstreamSharedPayload;
    PipelineStageFutureOwnership produced_future_ownership =
        PipelineStageFutureOwnership::None;
    bool flattens_future = false;
    bool root_stage = false;
};

inline void validate_future_pipeline_plan(const FuturePipelinePlan& plan) {
    if (plan.declaration_identity == 0)
        throw std::invalid_argument(
            "Future pipeline plan requires exact declaration identity");
    if (!plan.stage_effects.valid() || !plan.cumulative_effects.valid())
        throw std::invalid_argument(
            "Future pipeline plan contains unknown effect bits");
    if (!plan.cumulative_effects.permits(plan.stage_effects))
        throw std::invalid_argument(
            "Future pipeline cumulative effects dropped stage effects");

    if (plan.root_stage) {
        if (plan.input_ownership == PipelineInputOwnership::OwnedPipelineValue)
            throw std::invalid_argument(
                "Future pipeline root cannot be an internal owned edge");
    } else if (plan.input_ownership != PipelineInputOwnership::OwnedPipelineValue) {
        throw std::invalid_argument(
            "Future pipeline non-root edge must be pipeline-owned");
    }

    require_pipeline_input_traits(plan.input_ownership, plan.input_type);
    require_pipeline_output_traits(plan.output_type);

    const bool owns_stage_future =
        plan.produced_future_ownership ==
            PipelineStageFutureOwnership::OwnedStageResult;
    if (plan.flattens_future != owns_stage_future)
        throw std::invalid_argument(
            "Future pipeline flattening must own exactly one stage-created Future");
}

/**
 * Current native executable slice remains fail-closed for source exceptional
 * effects until throw/raise/panic continuation unwinding is implemented.
 */
inline void validate_executable_future_pipeline_plan(
        const FuturePipelinePlan& plan) {
    validate_future_pipeline_plan(plan);
    if (has_exceptional_pipeline_effects(plan.stage_effects))
        throw std::invalid_argument(
            "Future pipeline exceptional effect execution is not enabled");
}

inline void validate_future_pipeline_chain(
        const std::vector<FuturePipelinePlan>& plans) {
    if (plans.empty())
        throw std::invalid_argument("Future pipeline chain is empty");
    if (plans.size() > 128)
        throw std::invalid_argument("Future pipeline exceeds 128 stages");

    for (std::size_t i = 0; i < plans.size(); ++i) {
        const auto& plan = plans[i];
        validate_future_pipeline_plan(plan);
        if (i == 0) {
            if (!plan.root_stage)
                throw std::invalid_argument(
                    "Future pipeline first plan must describe the root edge");
            continue;
        }

        if (plan.root_stage)
            throw std::invalid_argument(
                "Future pipeline later edge cannot become a new root");
        const auto& prior = plans[i - 1];
        if (!plan.cumulative_effects.permits(prior.cumulative_effects))
            throw std::invalid_argument(
                "Future pipeline cumulative effects are not monotonic");
        if (prior.effect_provenance ==
                PipelineEffectProvenance::UpstreamFutureUnknown
                && plan.effect_provenance ==
                    PipelineEffectProvenance::CompleteStatic)
            throw std::invalid_argument(
                "Future pipeline cannot upgrade unknown upstream effect provenance");
    }
}

/**
 * Type checking may stage a plan, but executable authority is published only
 * after the ownership checker admits the same exact AST/declaration proof.
 */
inline std::optional<FuturePipelinePlan> publish_future_pipeline_plan(
        const FuturePipelinePlan& staged,
        bool ownership_admitted) {
    validate_future_pipeline_plan(staged);
    if (!ownership_admitted) return std::nullopt;
    return staged;
}

} // namespace oreslang::types
