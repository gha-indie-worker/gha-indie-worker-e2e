#pragma once
#include "oreslang/frontend/Parser.hpp"
namespace oreslang {
// Source bool/int distinction is checked even when both use an i64 ABI slot.
void validate_native_scalar_types(const Program& program);
}
