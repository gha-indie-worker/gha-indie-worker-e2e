#pragma once
#include "oreslang/frontend/Parser.hpp"
namespace oreslang {
// Preserve typed, implicit receiver loans when binding direct instance calls.
// This is not dynamic dispatch, escaping bound methods or suspension lowering.
void resolve_native_methods(Program& program);
}
