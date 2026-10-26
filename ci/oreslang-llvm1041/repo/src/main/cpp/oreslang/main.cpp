#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/runtime/TargetAbi.hpp"
#include "oreslang/runtime/CoreLog.hpp"
#include "oreslang/types/RealtimeAnalysis.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iterator>
#include <stdexcept>
#include <string>
#include <array>
#include <string_view>


static bool paths_alias(const char* source_path, const std::string& report_path) {
    std::error_code source_error;
    std::error_code report_error;
    const auto source = std::filesystem::weakly_canonical(
        std::filesystem::path(source_path), source_error);
    const auto report = std::filesystem::weakly_canonical(
        std::filesystem::path(report_path), report_error);
    if (source_error || report_error)
        throw std::runtime_error("cannot safely resolve rt-report/source paths");
    if (source == report) return true;
    std::error_code identity_error;
    const bool same_file = std::filesystem::equivalent(source, report, identity_error);
    if (!identity_error) return same_file;
    // libc++ may report a generic identity error when the destination does
    // not exist yet. Check that case explicitly without swallowing failures.
    std::error_code exists_error;
    const bool report_exists = std::filesystem::exists(report, exists_error);
    if (!exists_error && !report_exists) return false;
    if (identity_error == std::errc::no_such_file_or_directory) return false;
    throw std::runtime_error("cannot safely compare rt-report/source identity");
}

static std::vector<std::string> tree_shake_function_calls(
        const oreslang::Function& function,
        std::size_t remaining_budget) {
    const auto facts =
            oreslang::realtime_detail::direct_facts(function, remaining_budget);
    if (facts.call_budget_exceeded)
        throw std::runtime_error(
            "tree shaking call graph exceeds compiler edge budget");
    return facts.calls;
}

static void require_function_only_tree_shake_surface(
        const oreslang::Program& program) {
    if (!program.actors.empty())
        throw std::runtime_error(
            "tree shaking currently rejects actor declarations until actor roots are modeled");
    if (!program.classes.empty())
        throw std::runtime_error(
            "tree shaking currently rejects class declarations until class initializer roots are modeled");
    if (!program.cooperative_loops.empty())
        throw std::runtime_error(
            "tree shaking currently rejects cooperative loop declarations until continuation ABI roots are modeled");
    if (std::any_of(
            program.functions.begin(),
            program.functions.end(),
            [](const oreslang::Function& fn) {
                return static_cast<bool>(fn.scalar_while);
            }))
        throw std::runtime_error(
            "tree shaking currently rejects scalar continuation functions until continuation ABI roots are modeled");
    if (std::any_of(
            program.functions.begin(),
            program.functions.end(),
            [](const oreslang::Function& fn) {
                return fn.async_source_i64 ||
                    !fn.source_tasks.empty() ||
                    !fn.future_pipelines.empty();
            }))
        throw std::runtime_error(
            "tree shaking currently rejects Task/Future pipeline metadata edges until they participate in the semantic reachability graph");
}

static void tree_shake_functions(oreslang::Program& program, const std::vector<std::string>& requested_roots) {
    std::unordered_map<std::string, std::size_t> definitions;
    for (std::size_t i = 0; i < program.functions.size(); ++i) {
        const auto& function = program.functions[i];
        const std::string key =
            oreslang::realtime_detail::function_key(
                function.name, function.parameters.size());
        if (!definitions.emplace(key, i).second)
            throw std::runtime_error(
                "tree shaking requires unique native function name/arity identities");
    }

    std::vector<std::string> pending;
    std::unordered_set<std::string> reachable;
    if (!requested_roots.empty()) {
        for (const auto& requested : requested_roots) {
            std::vector<std::string> matches;
            for (const auto& function : program.functions) {
                if (function.name == requested)
                    matches.push_back(oreslang::realtime_detail::function_key(
                        function.name, function.parameters.size()));
            }
            if (matches.empty())
                throw std::runtime_error("unknown tree-shake root: " + requested);
            if (matches.size() != 1)
                throw std::runtime_error("ambiguous tree-shake root: " + requested);
            if (reachable.insert(matches.front()).second) pending.push_back(matches.front());
        }
    } else {
        for (const auto& function : program.functions) {
            if (function.exported || function.name == "main") {
                const std::string key =
                    oreslang::realtime_detail::function_key(
                        function.name, function.parameters.size());
                if (reachable.insert(key).second) pending.push_back(key);
            }
            if (function.exported) {
                // LLVM names overloaded families according to family
                // cardinality. Preserve every sibling of a public overload in
                // conservative mode so removing a private sibling cannot
                // silently rename the surviving exported ABI symbol.
                for (const auto& sibling : program.functions) {
                    if (sibling.name != function.name) continue;
                    const std::string sibling_key =
                        oreslang::realtime_detail::function_key(
                            sibling.name, sibling.parameters.size());
                    if (reachable.insert(sibling_key).second)
                        pending.push_back(sibling_key);
                }
            }
        }
    }
    if (pending.empty())
        throw std::runtime_error(
            "tree shaking requires an explicit root, main, or at least one pub function");

    std::size_t scanned_edges = 0;
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto definition = definitions.find(pending[cursor]);
        if (definition == definitions.end())
            throw std::runtime_error("tree shaking found an unresolved function root");
        if (scanned_edges >= oreslang::realtime_detail::kMaxRealtimeCallEdges)
            throw std::runtime_error(
                "tree shaking call graph exceeds compiler edge budget");
        const auto calls = tree_shake_function_calls(
                program.functions[definition->second],
                oreslang::realtime_detail::kMaxRealtimeCallEdges - scanned_edges);
        scanned_edges += calls.size();
        for (const auto& call : calls) {
            if (!definitions.count(call))
                throw std::runtime_error(
                    "tree shaking found an unresolved direct call: " + call);
            if (reachable.insert(call).second) pending.push_back(call);
        }
    }

    program.functions.erase(
        std::remove_if(program.functions.begin(), program.functions.end(),
            [&](const oreslang::Function& function) {
                return !reachable.count(oreslang::realtime_detail::function_key(
                    function.name, function.parameters.size()));
            }),
        program.functions.end());
}



int main(int argc, char** argv) {
    bool experimental_owned_int = false;
    bool experimental_rt_scalar_loop = false;
    bool wasm32_target = false;
    bool cortex_m_target = false;
    bool target_seen = false;
    bool native_hard_rt_profile = false;
    bool profile_seen = false;
    bool rt_check_only = false;
    bool rt_entry_seen = false;
    bool rt_report_seen = false;
    bool constraint_descriptors_only = false;
    std::string rt_entry = "main/0";
    std::string rt_report_path;
    bool tree_shake = false;
    std::vector<std::string> tree_shake_roots;
    const char* path = nullptr;
    // Host-only severity policy: last CLI flag overrides the environment.
    const char* envLevel = std::getenv("ORES_CORE_LOG_LEVEL");
    std::string coreLogLevel = envLevel ? envLevel : "off";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg.rfind("--profile=", 0) == 0) {
            if (profile_seen) { std::cerr << "duplicate profile option\n"; return 2; }
            profile_seen = true;
            const std::string profile = arg.substr(std::string_view("--profile=").size());
            if (profile != "native-hard-rt") {
                std::cerr << "unsupported native compiler profile: " << profile << '\n';
                return 2;
            }
            native_hard_rt_profile = true;
        } else if (arg == "--rt-check") {
            if (rt_check_only) { std::cerr << "duplicate rt-check option\n"; return 2; }
            rt_check_only = true;
            native_hard_rt_profile = true;
        } else if (arg.rfind("--rt-entry=", 0) == 0) {
            if (rt_entry_seen) { std::cerr << "duplicate rt-entry option\n"; return 2; }
            rt_entry_seen = true;
            rt_entry = arg.substr(std::string_view("--rt-entry=").size());
            if (rt_entry.empty()) { std::cerr << "rt-entry cannot be empty\n"; return 2; }
        } else if (arg.rfind("--rt-report=", 0) == 0) {
            if (rt_report_seen) { std::cerr << "duplicate rt-report option\n"; return 2; }
            rt_report_seen = true;
            rt_report_path = arg.substr(std::string_view("--rt-report=").size());
            if (rt_report_path.empty()) { std::cerr << "rt-report path cannot be empty\n"; return 2; }
        } else if (arg == "--constraint-descriptors") {
            if (constraint_descriptors_only) {
                std::cerr << "duplicate constraint-descriptors option\n";
                return 2;
            }
            constraint_descriptors_only = true;
        } else if (arg.rfind("--core-log-level=", 0) == 0) {
            coreLogLevel = arg.substr(std::string_view("--core-log-level=").size());
        } else if (arg == "--no-core-log") {
            coreLogLevel = "off";
        } else if (arg == "--experimental-rt-scalar-loop") {
            if (experimental_rt_scalar_loop) {
                std::cerr << "duplicate rt scalar loop option\n"; return 2;
            }
            experimental_rt_scalar_loop = true;
        } else if (arg == "--experimental-owned-int") {
            if (experimental_owned_int) { std::cerr << "duplicate owned-int option\n"; return 2; }
            experimental_owned_int = true;
        } else if (arg == "--tree-shake") {
            if (tree_shake) { std::cerr << "duplicate tree-shake option\n"; return 2; }
            tree_shake = true;
        } else if (arg.rfind("--tree-shake-root=", 0) == 0) {
            const std::string root = arg.substr(18);
            if (root.empty()) { std::cerr << "empty tree-shake root\n"; return 2; }
            tree_shake_roots.push_back(root);
        } else if (arg.rfind("--target=", 0) == 0) {
            if (target_seen) { std::cerr << "duplicate target option\n"; return 2; }
            target_seen = true;
            const std::string target = arg.substr(9);
            if (target == "host") {
                // Historical default: host triple and ABI remain inferred downstream.
            } else if (target == "thumbv7m-none-eabi") {
                cortex_m_target = oreslang::TargetAbi::parse(target).experimental_cortexm_ir();
            } else if (target == "wasm32" || target == "wasm32-unknown-unknown") {
                wasm32_target = oreslang::TargetAbi::parse("wasm32-unknown-unknown").experimental_wasm32_ir();
            } else {
                try {
                    (void)oreslang::TargetAbi::parse(target);
                    std::cerr << "target has no certified IR/ABI lowering: " << target << '\n';
                } catch (const std::invalid_argument&) {
                    std::cerr << "unknown target triple: " << target << '\n';
                }
                return 2;
            }
        } else if (arg.rfind("--cpu=", 0) == 0) {
            std::cerr << "explicit CPU feature selection not implemented: " << arg << '\n';
            return 2;
        } else if (!arg.empty() && arg[0] != '-' && path == nullptr) path = argv[i];
        else { std::cerr << "unrecognized or duplicate option: " << arg << '\n'; return 2; }
    }
    if (constraint_descriptors_only &&
            (tree_shake || !tree_shake_roots.empty() || native_hard_rt_profile ||
             rt_check_only || rt_entry_seen || rt_report_seen ||
             experimental_owned_int || experimental_rt_scalar_loop ||
             target_seen)) {
        std::cerr << "constraint-descriptors is a metadata-only mode and cannot be combined with execution/codegen options\n";
        return 2;
    }
    if (!tree_shake && !tree_shake_roots.empty()) {
        std::cerr << "tree-shake-root requires --tree-shake\n";
        return 2;
    }
    if (rt_entry_seen && !native_hard_rt_profile && !rt_report_seen) {
        std::cerr << "rt-entry requires --rt-check, --profile=native-hard-rt, or --rt-report\n";
        return 2;
    }
    oreslang::CoreLog::Level admittedLevel = oreslang::CoreLog::Level::Off;
    if (!oreslang::CoreLog::parse_level(coreLogLevel, admittedLevel)) {
        std::cerr << "invalid core log level (off|error|warn|info|debug|trace)\n";
        return 2;
    }
    // Default/off does not allocate or initialize 8192 recorder slots.
    // Optional diagnostics must not make ordinary compilation fail on OOM.
    std::unique_ptr<oreslang::CoreLog> coreLog;
    if (admittedLevel != oreslang::CoreLog::Level::Off)
        coreLog.reset(new (std::nothrow) oreslang::CoreLog(admittedLevel));
    if (path == nullptr) {
        std::cerr << "usage: oreslang-llvmc [--constraint-descriptors] [--tree-shake] [--tree-shake-root=name ...] [--profile=native-hard-rt] [--rt-check] [--rt-entry=name/arity] [--rt-report=path] [--target=host|wasm32|wasm32-unknown-unknown|thumbv7m-none-eabi] [--experimental-owned-int] [--experimental-rt-scalar-loop] [--core-log-level=off|error|warn|info|debug|trace] [--no-core-log] <source.ores>\n"
                  << "Supported: int functions/parameters, return arithmetic, and same-module calls.\n";
        return 2;
    }
    try {
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        const std::size_t kMaxSourceBytes = cortex_m_target ? 128u * 1024u : 8u * 1024u * 1024u;
        SourceIdentity source_identity;
        const auto text = read_regular_source(path, kMaxSourceBytes, cortex_m_target
            ? "cortex-m source exceeds 128 KiB limit"
            : "native source exceeds 8 MiB admission limit", &source_identity);
        require_valid_source_utf8(text);
        // Target/capability directives are lexical comments in the native
        // subset. Reject rather than silently discard unverified portability
        // constraints before parsing or emitting LLVM IR.
        if (text.find("// ores:") != std::string::npos) {
            throw std::runtime_error("native target admission unsupported: ores environment headers require compiler verification");
        }
        if (experimental_rt_scalar_loop && (wasm32_target || experimental_owned_int))
            throw std::runtime_error("scalar-loop frame requires native host and no owned-int experiment");
        if (cortex_m_target && text.size() > 128u * 1024u)
            throw std::runtime_error("cortex-m source exceeds 128 KiB limit");
        if (native_hard_rt_profile && (wasm32_target || cortex_m_target))
            throw std::runtime_error("native-hard-rt profile currently requires the host target");
        if (native_hard_rt_profile && experimental_owned_int)
            throw std::runtime_error("native-hard-rt profile rejects experimental owned heap lowering");

        auto ast = oreslang::load_native_program(path, text, source_identity, experimental_owned_int,
                                                 experimental_rt_scalar_loop, !wasm32_target && !cortex_m_target);

        if (constraint_descriptors_only) {
            for (const auto& descriptor : ast.type_constraint_descriptors) {
                std::cout << "descriptor\t"
                          << descriptor.schema_id << '\t'
                          << descriptor.qualified_type_name << '\t'
                          << descriptor.representation << '\t'
                          << (descriptor.validation_required ? "true" : "false") << '\t'
                          << descriptor.fingerprint << '\t'
                          << descriptor.canonical << '\n';
            }
            std::cout.flush();
            return 0;
        }

        if (rt_report_seen && path != nullptr && paths_alias(path, rt_report_path))
            throw std::runtime_error("rt-report path must not overwrite source input");

        if (native_hard_rt_profile || rt_report_seen) {
            const auto report = oreslang::analyze_native_hard_rt(ast, rt_entry);
            const std::string report_json = report.to_json();
            if (rt_report_seen) {
                std::ofstream output(rt_report_path, std::ios::out | std::ios::trunc);
                if (!output) throw std::runtime_error("cannot open rt-report output");
                output.exceptions(std::ios::badbit | std::ios::failbit);
                output << report_json;
                output.flush();
            }
            if (rt_check_only) {
                std::cout << report_json;
                std::cout.flush();
                if (coreLog) coreLog->dump(stderr);
                return report.source_admitted ? 0 : 1;
            }
            if (native_hard_rt_profile && !report.source_admitted) {
                throw std::runtime_error(
                    "native-hard-rt admission failed [" + report.diagnostic +
                    "]: " + report.reason);
            }
        }

        if (cortex_m_target) {
            // Deliberately small freestanding ABI: i64 scalar functions only.
            // Never lower native actors, heap regions or unverified host calls
            // merely because they happen to compile into some LLVM IR.
            if (experimental_owned_int || experimental_rt_scalar_loop ||
                !ast.module.empty() || !ast.classes.empty() ||
                !ast.cooperative_loops.empty() || ast.functions.empty())
                throw std::runtime_error("cortex-m target supports only freestanding scalar functions");
            constexpr std::size_t max_functions = 64;
            constexpr std::size_t max_parameters = 8;
            constexpr std::size_t max_expression_nodes = 4096;
            constexpr std::size_t max_expression_depth = 32;
            constexpr std::size_t max_call_depth = 16;
            if (ast.functions.size() > max_functions)
                throw std::runtime_error("cortex-m function count exceeds 64");
            std::size_t expression_nodes = 0;
            std::unordered_map<std::string, std::vector<std::string>> edges;
            std::function<bool(const oreslang::Expression&, std::size_t,
                               std::vector<std::string>&)> scalar =
                [&](const oreslang::Expression& expr, std::size_t depth,
                    std::vector<std::string>& calls) -> bool {
                if (depth > max_expression_depth || ++expression_nodes > max_expression_nodes)
                    throw std::runtime_error("cortex-m expression budget exceeded");

                using Kind = oreslang::Expression::Kind;
                switch (expr.kind) {
                    case Kind::Integer: case Kind::Variable: return true;
                    case Kind::Negate:
                        return expr.left && scalar(*expr.left, depth + 1, calls);
                    case Kind::Add: case Kind::Subtract: case Kind::Multiply:
                        return expr.left && expr.right &&
                               scalar(*expr.left, depth + 1, calls) &&
                               scalar(*expr.right, depth + 1, calls);
                    case Kind::Call:
                        if (expr.arguments.size() > max_parameters) return false;
                        calls.push_back(expr.name);
                        for (const auto& arg : expr.arguments)
                            if (!arg || !scalar(*arg, depth + 1, calls)) return false;
                        return true;
                    default: return false;
                }
            };
            if (!ast.actors.empty()) {
                if (ast.actors.size() > 8 || ast.functions.size() != 1 ||
                    ast.functions.front().name != "main")
                    throw std::runtime_error("cortex-m actors require 1..8 actor types and only main");
                const auto& start = ast.functions.front();
                if (start.returns_void || !start.result ||
                    start.result->kind != oreslang::Expression::Kind::Integer ||
                    !start.locals.empty() || !start.statements.empty() ||
                    !start.early_returns.empty() || start.has_declared_effects ||
                    start.actor_spawns.empty() || start.actor_spawns.size() > 8 ||
                    start.actor_sends.size() > 32 ||
                    start.actor_stops.size() != start.actor_spawns.size() ||
                    !start.parameters.empty())
                    throw std::runtime_error("cortex-m actor bootstrap requires bounded source main");
                if (start.actor_actions.size() !=
                        start.actor_sends.size() + start.actor_stops.size())
                    throw std::runtime_error("cortex-m actor action plan inconsistent");
                for (const auto& actor : ast.actors) {
                    if (actor.fields.empty() || actor.fields.size() > 8 ||
                        actor.on_message.actor_field_writes.size() > 16 ||
                        !actor.on_message.result)
                        throw std::runtime_error("cortex-m actor field or handler budget exceeded");
                    for (const auto& field : actor.fields) {
                        if (!field.initializer ||
                            field.initializer->kind != oreslang::Expression::Kind::Integer)
                            throw std::runtime_error("cortex-m actors need literal i64 field initialization");
                    }
                    std::vector<std::string> calls;
                    if (!scalar(*actor.on_message.result, 0, calls) || !calls.empty())
                        throw std::runtime_error("cortex-m actor return must be scalar without guest calls");
                    for (const auto& write : actor.on_message.actor_field_writes) {
                        calls.clear();
                        if (!write.expression || !scalar(*write.expression, 0, calls) ||
                            !calls.empty())
                            throw std::runtime_error("cortex-m actor writes must be scalar without guest calls");
                    }
                }
                for (const auto& send : start.actor_sends) {
                    if (!send.payload ||
                        send.payload->kind != oreslang::Expression::Kind::Integer)
                        throw std::runtime_error("cortex-m actor send must be a literal i64");
                }
                // All bootstrap effects are statically ordered. Prove no
                // mailbox can overflow before lowering irreversible void sends.
                // The runtime's 16-entry cap is an ABI contract, not a hint.
                std::unordered_map<std::string, std::size_t> pending;
                std::unordered_set<std::string> stopped;
                for (const auto& spawn : start.actor_spawns) {
                    if (!pending.emplace(spawn.name, 0).second)
                        throw std::runtime_error("cortex-m duplicate actor ref");
                }
                for (const auto& action : start.actor_actions) {
                    std::string ref;
                    if (action.kind == oreslang::ActorAction::Kind::Send) {
                        if (action.index >= start.actor_sends.size())
                            throw std::runtime_error("cortex-m invalid actor send index");
                        ref = start.actor_sends[action.index].reference;
                    } else {
                        if (action.index >= start.actor_stops.size())
                            throw std::runtime_error("cortex-m invalid actor stop index");
                        ref = start.actor_stops[action.index];
                    }
                    const auto it = pending.find(ref);
                    if (it == pending.end() || stopped.count(ref))
                        throw std::runtime_error("cortex-m invalid or stopped actor capability");
                    if (action.kind == oreslang::ActorAction::Kind::Send) {
                        if (++it->second > 16)
                            throw std::runtime_error("cortex-m static mailbox capacity exceeded (16)");
                    } else {
                        stopped.insert(ref);
                    }
                }
                if (stopped.size() != pending.size())
                    throw std::runtime_error("cortex-m bootstrap must stop all actors");
                // The trusted C runtime owns static actors, mailboxes, and
                // orderly stop/drain; this does not admit guest awaits.
            }
            for (const auto& fn : ast.functions) {
                if ((!ast.actors.empty() && fn.name != "main") ||
                    (ast.actors.empty() && fn.name == "main") ||
                    fn.returns_void || !fn.result ||
                    !fn.locals.empty() || !fn.statements.empty() ||
                    !fn.early_returns.empty() ||
                    (ast.actors.empty() && (!fn.actor_spawns.empty() ||
                        !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                        !fn.actor_actions.empty() || !fn.actor_field_writes.empty())) ||
                    fn.scalar_while || fn.caught_throw_value ||
                    !fn.caught_throw_defers.empty() || fn.has_declared_effects)
                    throw std::runtime_error("cortex-m target rejects main, effects or non-scalar functions");
                if (fn.parameters.size() > max_parameters)
                    throw std::runtime_error("cortex-m parameter budget exceeded");
                for (const auto& param : fn.parameters)
                    if (!param.class_name.empty() ||
                        param.mode != oreslang::Parameter::Mode::Value)
                        throw std::runtime_error("cortex-m target requires value int parameters");
                std::vector<std::string> calls;
                if (!scalar(*fn.result, 0, calls))
                    throw std::runtime_error("cortex-m target requires bounded scalar expressions");
                if (!edges.emplace(fn.name, std::move(calls)).second)
                    throw std::runtime_error("cortex-m target rejects overloaded function names");
            }
            // Source-level cycles are forbidden on tiny stacks. The call-graph
            // path limit caps nesting even when individual ASTs are shallow.
            std::unordered_set<std::string> active;
            std::unordered_map<std::string, std::size_t> longest;
            std::function<std::size_t(const std::string&)> walk =
                [&](const std::string& name) -> std::size_t {
                const auto known = longest.find(name);
                if (known != longest.end()) return known->second;
                if (!active.insert(name).second)
                    throw std::runtime_error("cortex-m recursive call graph rejected");
                auto node = edges.find(name);
                if (node == edges.end())
                    throw std::runtime_error("cortex-m unresolved function call");
                std::size_t depth = 1;
                for (const auto& dependency : node->second) {
                    const auto child = walk(dependency);
                    if (child >= max_call_depth)
                        throw std::runtime_error("cortex-m call depth budget exceeded");
                    depth = std::max(depth, child + 1);
                }
                active.erase(name);
                longest.emplace(name, depth);
                return depth;
            };
            for (const auto& fn : ast.functions) (void)walk(fn.name);
        }
        if (tree_shake) {
            // Tree shaking is an optimization, not conditional compilation.
            // Validate/codegen the complete admitted source once before pruning
            // so unsupported target features cannot hide in unreachable code.
            (void)oreslang::emit_verified_llvm_ir(ast, wasm32_target, cortex_m_target);

            // The shipped pass is deliberately function-only. Richer Program
            // owners have hidden semantic/ABI roots that are not represented
            // by the direct-call graph yet. Fail closed rather than deleting
            // a helper that is reachable only from an actor handler, class
            // initializer, or host-driven continuation.
            require_function_only_tree_shake_surface(ast);
            tree_shake_functions(ast, tree_shake_roots);
        }
        std::cout << oreslang::emit_verified_llvm_ir(ast, wasm32_target, cortex_m_target);
        std::cout.flush();
        if (coreLog) coreLog->dump(stderr);
        return 0;
    } catch (const std::exception& error) {
        // No guest payload, exception message or source path enters CoreLog.
        if (coreLog)
            coreLog->record(oreslang::CoreLog::Level::Error,
                            oreslang::CoreLog::EventId::CompileFailed, 1);
        std::cerr << "oreslang-llvmc: " << error.what() << '\n';
        if (coreLog) coreLog->dump(stderr);
        return 1;
    }
}
