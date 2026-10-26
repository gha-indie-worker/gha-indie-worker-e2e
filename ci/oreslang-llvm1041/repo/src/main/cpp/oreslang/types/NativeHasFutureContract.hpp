#pragma once

#include <string_view>

namespace oreslang {

// Compiler admission only. Do not confuse an ActorRef's HasFuture readiness
// projection with its separate get_done() lifecycle future. This table is
// closed until native source can verify user-defined trait implementations.
enum class NativeAwaitTargetKind : unsigned char {
    Unknown, Scalar, Class, ActorRef, FutureVoid, FutureI64
};
enum class NativeAwaitForm : unsigned char {
    None, DirectHasFuture, ExplicitGetReady, ExplicitGetDone, FutureValue
};
enum class NativeAwaitAbi : unsigned char {
    Unsupported, SourceActorReadyControl
};

struct NativeAwaitDescriptor {
    NativeAwaitTargetKind kind = NativeAwaitTargetKind::Unknown;
    NativeAwaitForm form = NativeAwaitForm::None;
    std::string_view nominal_type;
    std::string_view actor_identity;
    bool live_actor = false;
    bool verified_get_ready_provenance = false;
    bool future_token_unconsumed = false;
    // Bind a Future<void> to the exact actor capability that produced it.
    std::string_view future_producer_actor_identity;
};
struct NativeAwaitResolution {
    NativeAwaitAbi abi = NativeAwaitAbi::Unsupported;
    bool returns_void = false;
    bool consumes_future_token = false;
};

// A successful resolution is compiler evidence of both a known statically
// admitted receiver shape and the exact native continuation ABI. It is NOT
// a runtime poll, a host blocking await, or a trait-name string comparison.
constexpr NativeAwaitResolution resolve_native_has_future(
        const NativeAwaitDescriptor& candidate) noexcept {
    if (candidate.nominal_type.empty() || candidate.actor_identity.empty() ||
            !candidate.live_actor) return {};
    if (candidate.kind == NativeAwaitTargetKind::ActorRef &&
            (candidate.form == NativeAwaitForm::DirectHasFuture ||
             candidate.form == NativeAwaitForm::ExplicitGetReady)) {
        return {NativeAwaitAbi::SourceActorReadyControl, true, false};
    }
    if (candidate.kind == NativeAwaitTargetKind::FutureVoid &&
            candidate.form == NativeAwaitForm::FutureValue &&
            candidate.verified_get_ready_provenance &&
            candidate.future_token_unconsumed &&
            candidate.future_producer_actor_identity == candidate.actor_identity) {
        return {NativeAwaitAbi::SourceActorReadyControl, true, true};
    }
    return {};
}

} // namespace oreslang
