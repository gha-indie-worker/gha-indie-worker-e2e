#pragma once

#include <cstddef>
#include <string>

namespace oreslang {

// This is a compile-time selector/ABI key, never a runtime structural
// overload resolution step. Native source identifiers cannot contain '$'.
// A single encoder must be used by the ownership verifier, code generator,
// and exported overloaded symbols to prevent signature/ABI drift.
inline std::string native_function_slot(const std::string& name, std::size_t value_arity) {
    return name + "$arity" + std::to_string(value_arity);
}

} // namespace oreslang
