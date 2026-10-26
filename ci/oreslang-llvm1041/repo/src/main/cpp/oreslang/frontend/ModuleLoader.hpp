#pragma once
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include <string>

namespace oreslang {
// Compile-time only. No source-loader or resolver is linked into guest artifacts.
// Relative module selections are statically resolved before ordinary whole-program
// name/arity/effect/ownership validation. Unsupported source never yields IR.
Program load_native_program(const std::string& source_path, std::string source,
                            SourceIdentity identity, bool experimental_owned_int,
                            bool experimental_rt_scalar_loop, bool host_target);
}
