#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace oreslang {

// This is a compile-time selector/ABI key, never a runtime structural
// overload resolution step. Native source identifiers cannot contain '$'.
// A single encoder must be used by the ownership verifier, code generator,
// and exported overloaded symbols to prevent signature/ABI drift.
inline std::string native_function_slot(const std::string& name, std::size_t value_arity) {
    return name + "$arity" + std::to_string(value_arity);
}

/** Subtract the compiler-injected receiver from source-visible method arity. */
inline std::size_t native_source_value_arity(
        std::size_t lowered_arity, bool has_hidden_receiver) {
    if (!has_hidden_receiver) return lowered_arity;
    if (lowered_arity == 0)
        throw std::invalid_argument("native method lowered without hidden receiver");
    return lowered_arity - 1;
}

} // namespace oreslang
