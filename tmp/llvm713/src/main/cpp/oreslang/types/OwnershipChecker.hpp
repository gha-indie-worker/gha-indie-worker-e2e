#pragma once

#include "oreslang/frontend/Parser.hpp"

namespace oreslang {

// Enforces explicit ownership operations in the LLVM i64-only source subset.
// Admits only call-scoped i64 borrows/reborrows. General references, pointer
// projections, actor hops and suspension still fail closed before codegen.
void validate_i64_ownership(const Program& program);

} // namespace oreslang
