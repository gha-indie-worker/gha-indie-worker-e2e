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

/**
 * A flattened Future may receive cancellation ownership only when the checker
 * proved that this exact stage creates fresh deferred work. Forwarding an
 * existing/borrowed Future never becomes owned because a surrounding root was
 * consumed.
 */
enum class PipelineStageFutureProvenance : std::uint8_t {
    None,
    FreshCheckedProducer,
    BorrowedOrForwarded
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

inline bool same_pipeline_type(const AutoType& left, const AutoType& right) {
    if (left.kind != right.kind
            || left.dyn_bounds != right.dyn_bounds
            || left.shareable_domain != right.shareable_domain
            || left.complete_nominal_layout != right.complete_nominal_layout
            || left.negative != right.negative
            || left.parameters.size() != right.parameters.size())
        return false;
    for (std::size_t i = 0; i < left.parameters.size(); ++i)
        if (!same_pipeline_type(left.parameters[i], right.parameters[i]))
            return false;
    return true;
}

/**
 * Identity-bound staged compiler plan. Both identities are required:
 * declaration_identity binds the resolved callable and pipeline_node_identity
 * binds the exact checked value-pipe AST node. Neither is a guest capability.
 *
 * prior_effects is the exact upstream cumulative row seen by this stage:
 * producer effects for the root, previous-stage cumulative effects otherwise.
 */
struct FuturePipelinePlan {
    std::uint64_t declaration_identity = 0;
    std::uint64_t pipeline_node_identity = 0;
    AutoType input_type{};
    AutoType output_type{};
    FunctionEffects prior_effects{};
    FunctionEffects stage_effects{};
    FunctionEffects cumulative_effects{};
    PipelineEffectProvenance effect_provenance =
        PipelineEffectProvenance::UpstreamFutureUnknown;
    PipelineInputOwnership input_ownership =
        PipelineInputOwnership::BorrowedUpstreamSharedPayload;
    PipelineStageFutureOwnership produced_future_ownership =
        PipelineStageFutureOwnership::None;
    PipelineStageFutureProvenance produced_future_provenance =
        PipelineStageFutureProvenance::None;
    bool flattens_future = false;
    bool root_stage = false;
};

inline void validate_future_pipeline_plan(const FuturePipelinePlan& plan) {
    if (plan.declaration_identity == 0 || plan.pipeline_node_identity == 0)
        throw std::invalid_argument(
            "Future pipeline plan requires exact declaration and AST identities");
    if (!plan.prior_effects.valid()
            || !plan.stage_effects.valid()
            || !plan.cumulative_effects.valid())
        throw std::invalid_argument(
            "Future pipeline plan contains unknown effect bits");

    const FunctionEffects expected_effects =
        plan.prior_effects.united(plan.stage_effects);
    if (plan.cumulative_effects.bits != expected_effects.bits)
        throw std::invalid_argument(
            "Future pipeline cumulative effects must equal prior plus stage effects");

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
    if (!plan.flattens_future) {
        if (owns_stage_future
                || plan.produced_future_provenance !=
                    PipelineStageFutureProvenance::None)
            throw std::invalid_argument(
                "scalar Future pipeline stage cannot own deferred Future work");
    } else {
        if (!owns_stage_future
                || plan.produced_future_provenance !=
                    PipelineStageFutureProvenance::FreshCheckedProducer)
            throw std::invalid_argument(
                "Future flattening requires one fresh checked stage-created Future");
    }
}

/**
 * Current native executable slice remains fail-closed for source exceptional
 * effects until throw/raise/panic continuation unwinding is implemented.
 * Check the full cumulative row, not only the current stage, so an earlier
 * exceptional effect cannot disappear at a later lowering boundary.
 */
inline void validate_executable_future_pipeline_plan(
        const FuturePipelinePlan& plan) {
    validate_future_pipeline_plan(plan);
    if (has_exceptional_pipeline_effects(plan.cumulative_effects))
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
        if (!same_pipeline_type(prior.output_type, plan.input_type))
            throw std::invalid_argument(
                "Future pipeline adjacent stage types do not compose");
        if (plan.prior_effects.bits != prior.cumulative_effects.bits)
            throw std::invalid_argument(
                "Future pipeline prior effect row does not match preceding stage");
        if (prior.effect_provenance ==
                PipelineEffectProvenance::UpstreamFutureUnknown
                && plan.effect_provenance ==
                    PipelineEffectProvenance::CompleteStatic)
            throw std::invalid_argument(
                "Future pipeline cannot upgrade unknown upstream effect provenance");
    }
}

struct FuturePipelineOwnershipAdmission {
    std::uint64_t declaration_identity = 0;
    std::uint64_t pipeline_node_identity = 0;
    PipelineInputOwnership input_ownership =
        PipelineInputOwnership::BorrowedUpstreamSharedPayload;
    bool admitted = false;
};

inline void require_matching_pipeline_admission(
        const FuturePipelinePlan& staged,
        const FuturePipelineOwnershipAdmission& admission) {
    if (staged.declaration_identity != admission.declaration_identity
            || staged.pipeline_node_identity != admission.pipeline_node_identity
            || staged.input_ownership != admission.input_ownership)
        throw std::invalid_argument(
            "Future pipeline ownership admission does not match staged proof identity");
}

/**
 * A single staged proof may publish only after ownership admits the exact AST,
 * declaration and ownership mode. A bare boolean is deliberately insufficient:
 * admission for one call must never authorize another rewritten/rebound call.
 */
inline std::optional<FuturePipelinePlan> publish_future_pipeline_plan(
        const FuturePipelinePlan& staged,
        const FuturePipelineOwnershipAdmission& admission) {
    validate_future_pipeline_plan(staged);
    if (!admission.admitted) return std::nullopt;
    require_matching_pipeline_admission(staged, admission);
    return staged;
}

/**
 * Compilation-unit publication is atomic like Java/Graal CheckedFuturePipe:
 * if any staged edge fails ownership admission, no prefix receives executable
 * authorization. Identity mismatches are hard errors rather than partial
 * publication.
 */
inline std::optional<std::vector<FuturePipelinePlan>>
publish_future_pipeline_chain(
        const std::vector<FuturePipelinePlan>& staged,
        const std::vector<FuturePipelineOwnershipAdmission>& admissions) {
    validate_future_pipeline_chain(staged);
    if (staged.size() != admissions.size())
        throw std::invalid_argument(
            "Future pipeline ownership admission count does not match staged chain");

    for (const auto& admission : admissions)
        if (!admission.admitted) return std::nullopt;

    for (std::size_t i = 0; i < staged.size(); ++i)
        require_matching_pipeline_admission(staged[i], admissions[i]);

    return staged;
}

} // namespace oreslang::types
