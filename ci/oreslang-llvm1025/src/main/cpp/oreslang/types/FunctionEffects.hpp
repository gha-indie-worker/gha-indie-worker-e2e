#ifndef ORESLANG_FUNCTION_EFFECTS_HPP
#define ORESLANG_FUNCTION_EFFECTS_HPP

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace oreslang {
enum class FunctionEffect : std::uint32_t {
    StateRead = 1u << 0,
    StateWrite = 1u << 1,
    Suspend = 1u << 2,
    Io = 1u << 3,
    Alloc = 1u << 4,
    Block = 1u << 5,
    Ffi = 1u << 6,
    Throw = 1u << 7,
    Raise = 1u << 8,
    Panic = 1u << 9
};

struct FunctionEffects {
    // Bits are public for the compiler-owned ABI. Never grant admission to
    // arbitrary or future/unknown bits: those require an explicit review.
    static constexpr std::uint32_t KnownBits =
        static_cast<std::uint32_t>(FunctionEffect::StateRead) |
        static_cast<std::uint32_t>(FunctionEffect::StateWrite) |
        static_cast<std::uint32_t>(FunctionEffect::Suspend) |
        static_cast<std::uint32_t>(FunctionEffect::Io) |
        static_cast<std::uint32_t>(FunctionEffect::Alloc) |
        static_cast<std::uint32_t>(FunctionEffect::Block) |
        static_cast<std::uint32_t>(FunctionEffect::Ffi) |
        static_cast<std::uint32_t>(FunctionEffect::Throw) |
        static_cast<std::uint32_t>(FunctionEffect::Raise) |
        static_cast<std::uint32_t>(FunctionEffect::Panic);

    std::uint32_t bits = 0;
    constexpr bool valid() const noexcept {
        return (bits & ~KnownBits) == 0;
    }
    constexpr bool has(FunctionEffect effect) const noexcept {
        const auto bit = static_cast<std::uint32_t>(effect);
        // A casted composite/zero/unknown flag is not a single effect.
        return valid() && bit != 0 && (bit & (bit - 1u)) == 0 &&
               (bit & KnownBits) != 0 && (bits & bit) != 0;
    }
    constexpr bool permits(FunctionEffects inferred) const noexcept {
        // Do not silently approve unrecognized effects, even if both sets
        // happen to contain the same forged bit.
        return valid() && inferred.valid() &&
               (bits & inferred.bits) == inferred.bits;
    }
    constexpr FunctionEffects united(FunctionEffects rhs) const {
        return {bits | rhs.bits};
    }
    // Native counterpart of Java Call.unresolved(...). This sentinel is
    // deliberately NOT an authorized/known effect bit: both the inferred row
    // and a forged declared row must fail permits() closed. A compiler must
    // emit it for calls whose effect-bearing target cannot be proven.
    static constexpr std::uint32_t UnresolvedCallBit = 1u << 31;
    static constexpr FunctionEffects unresolved_call() noexcept {
        return {UnresolvedCallBit};
    }

    static FunctionEffect parse(std::string_view name) {
        if (name == "state.read") return FunctionEffect::StateRead;
        if (name == "state.write") return FunctionEffect::StateWrite;
        if (name == "suspend") return FunctionEffect::Suspend;
        if (name == "io") return FunctionEffect::Io;
        if (name == "alloc") return FunctionEffect::Alloc;
        if (name == "block") return FunctionEffect::Block;
        if (name == "ffi") return FunctionEffect::Ffi;
        if (name == "throw") return FunctionEffect::Throw;
        if (name == "raise") return FunctionEffect::Raise;
        if (name == "panic") return FunctionEffect::Panic;
        throw std::invalid_argument("unknown Oreslang function effect");
    }
};
} // namespace oreslang
#endif
