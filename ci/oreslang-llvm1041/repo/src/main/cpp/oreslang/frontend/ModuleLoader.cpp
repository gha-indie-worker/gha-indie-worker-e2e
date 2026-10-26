#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/frontend/ProjectRootImport.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t max_bytes = 8u * 1024u * 1024u;
constexpr std::size_t max_units = 128, max_depth = 32;
struct Unit {
    fs::path path;
    SourceIdentity identity;
    Program program;
    std::string prefix;
    std::unordered_map<std::string, std::size_t> dependencies;
    std::unordered_map<std::string, std::pair<std::size_t, std::string>> class_imports;
    // Declaration identities stay immutable while ASTs are moved/qualified.
    std::unordered_map<std::string, std::unordered_map<std::size_t, bool>> functions;
    std::unordered_set<std::string> classes;
    std::unordered_map<std::string, bool> class_exports;
    std::unordered_map<std::string, bool> constructor_exports;
};
class Loader {
    std::vector<std::unique_ptr<Unit>> units;
    std::size_t bytes = 0, read_bytes = 0, edges = 0;
    std::unordered_map<std::string, std::size_t> paths;
    bool owned, scalar_loop, host;
public:
    Loader(bool owned, bool scalar_loop, bool host) : owned(owned), scalar_loop(scalar_loop), host(host) {}
    Program run(const std::string& path, std::string text, SourceIdentity identity) {
        read_bytes = text.size();
        visit(fs::canonical(path), std::move(text), identity, 0);
        if (units.front()->program.imports.empty()) {
            auto root = std::move(units.front()->program);
            resolve_and_validate_program(root);
            return root;
        }
        Program linked;
        linked.module = units.front()->program.module;
        linked.namespace_name = units.front()->program.namespace_name;
        for (std::size_t index = 0; index < units.size(); ++index) {
            auto& unit = *units[index];
            if (!unit.program.type_only_declarations.empty() ||
                    !unit.program.type_constraint_descriptors.empty() ||
                    !unit.program.namespace_name.empty()) {
                fail(unit,
                    "native linked type-only declarations/namespaces require checked cross-unit type identity lowering");
            }
            if (!unit.program.contracts.empty() || !unit.program.conforms.empty())
                fail(unit, "native linked contract conformance requires checked cross-unit contract identity lowering");
            // An init hook has observable ordering/once-only behavior in Java.
            // Never silently treat it as an ordinary never-called function.
            if (unit.functions.count("init")) fail(unit, "native module init hook requires initialization lowering");
            if (!unit.program.cooperative_loops.empty())
                fail(unit, "native linked suspend functions require module continuation lowering");
            if (index && !unit.program.actors.empty())
                fail(unit, "native imported actor declarations require typed module actor placement lowering");
            for (const auto& type : unit.program.sum_types) {
                bool found = false; for (const auto& existing : linked.sum_types) if (existing.name == type.name) found = true;
                if (!found) linked.sum_types.push_back(type);
            }
            for (auto& fn : unit.program.functions) {
                qualify_function(unit, fn);
                fn.name = unit.prefix + fn.name;
                fn.native_internal = index && !fn.exported;
                linked.functions.push_back(std::move(fn));
            }
            for (auto& cls : unit.program.classes) {
                binding_name(unit, cls.name);
                for (const auto& name : cls.constructor_parameters) binding_name(unit, name);
                for (auto& field : cls.fields) {
                    if (!field.class_name.empty()) field.class_name = class_type(unit, field.class_name);
                    if (!field.array_record_class.empty()) field.array_record_class = class_type(unit, field.array_record_class);
                    expression(unit, field.initializer_expression.get());
                }
                cls.name = unit.prefix + cls.name;
                linked.classes.push_back(std::move(cls));
            }
            // Root actors may call statically imported scalar helpers. Their
            // state/capability identities remain in the ordinary actor ABI.
            for (auto& actor : unit.program.actors) {
                for (auto& field : actor.fields) {
                    binding_name(unit, field.name);
                    expression(unit, field.initializer.get());
                }
                qualify_function(unit, actor.on_message);
                linked.actors.push_back(std::move(actor));
            }
        }
        resolve_and_validate_program(linked);
        return linked;
    }
private:
    [[noreturn]] static void fail(const Unit& unit, const std::string& reason) {
        throw std::runtime_error(unit.path.string() + ": " + reason);
    }
    static bool same_file(SourceIdentity left, SourceIdentity right) {
        return left.valid && right.valid && left.device == right.device && left.inode == right.inode;
    }
    std::size_t visit(fs::path path, std::string text, SourceIdentity identity, std::size_t depth) {
        if (depth > max_depth) throw std::runtime_error("native import graph exceeds 32 edges of depth");
        if (units.size() >= max_units) throw std::runtime_error("native import graph exceeds 128 source files");
        if (text.size() > max_bytes - bytes) throw std::runtime_error("native import graph exceeds 8 MiB aggregate source limit");
        bytes += text.size();
        require_valid_source_utf8(text);
        if (text.find("// ores:") != std::string::npos)
            throw std::runtime_error(path.string() + ": native target admission unsupported: ores environment headers require compiler verification");
        const auto index = units.size();
        auto unit = std::make_unique<Unit>();
        unit->path = std::move(path); unit->identity = identity;
        unit->prefix = index ? "__ores_module_" + std::to_string(index) + "." : "";
        try { unit->program = parse_unresolved_program(text, owned, scalar_loop); }
        catch (const std::exception& error) {
            // Preserve historical diagnostics for a standalone entry unit.
            if (!index) throw;
            fail(*unit, error.what());
        }
        for (const auto& fn : unit->program.functions) {
            auto& arities = unit->functions[fn.name];
            arities.emplace(fn.parameters.size(), fn.exported);
        }
        for (const auto& cls : unit->program.classes) {
            unit->classes.insert(cls.name); unit->class_exports.emplace(cls.name, cls.exported);
            unit->constructor_exports.emplace(cls.name, !cls.explicit_constructor || cls.constructor_exported);
        }
        paths.emplace(unit->path.string(), index);
        units.push_back(std::move(unit));
        auto& current = *units[index];
        if (!current.program.imports.empty() && (!host || scalar_loop))
            fail(current, "native module imports currently require the ordinary host target");
        for (const auto& imported : current.program.imports) {
            if (++edges > 4096) fail(current, "native import graph exceeds 4096 edges");
            const auto& raw = imported.path;
            fs::path dependency;
            if (raw.rfind("+/", 0) == 0) {
                try {
                    dependency = resolve_project_root_import(
                        current.path, raw,
                        [](const fs::path& manifest) {
                            auto bytes = read_regular_source(
                                manifest.string().c_str(), 64u * 1024u,
                                "native package manifest exceeds 64 KiB");
                            require_valid_source_utf8(bytes);
                            return bytes;
                        });
                } catch (const std::exception& error) {
                    fail(current, error.what());
                }
            } else {
                if (raw.rfind("./", 0) != 0 && raw.rfind("../", 0) != 0)
                    fail(current, "native imports require relative ./ or ../ or project-root +/ source paths; package/std/host resolution unsupported");
                dependency = fs::path(raw);
                if (dependency.extension().empty()) dependency += ".ores";
                if (dependency.extension() != ".ores")
                    fail(current, "native module source must use .ores extension");
                dependency = current.path.parent_path() / dependency;
                std::error_code error;
                dependency = fs::canonical(dependency, error);
                if (error) fail(current, "cannot resolve native import '" + raw + "'");
            }
            const auto known = paths.find(dependency.string());
            std::size_t selected = known == paths.end() ? units.size() : known->second;
            if (selected == index) fail(current, "source file cannot import itself");
            if (selected == units.size()) {
                SourceIdentity child_identity;
                auto child_text = read_regular_source(dependency.string().c_str(), max_bytes - read_bytes,
                    "native import graph exceeds 8 MiB source read budget", &child_identity);
                read_bytes += child_text.size();
                // Compare descriptor identities too: hard-link aliases must not
                // bypass self-edge rejection or create duplicate module identity.
                for (std::size_t i = 0; i < units.size(); ++i)
                    if (same_file(units[i]->identity, child_identity)) { selected = i; break; }
                if (selected == index) fail(current, "source file cannot import itself through a filesystem alias");
                if (selected == units.size()) selected = visit(dependency, std::move(child_text), child_identity, depth + 1);
                else paths.emplace(dependency.string(), selected);
            }
            if (imported.local_name == current.program.module)
                fail(current, "native imported module binding conflicts with containing module");
            const auto& target = *units[selected];
            if (imported.selected_class) {
                const auto cls = target.class_exports.find(imported.selected_module);
                if (cls == target.class_exports.end() || !cls->second)
                    fail(current, "unknown or private imported class '" + imported.selected_module + "'");
                current.class_imports.emplace(imported.local_name, std::make_pair(selected, imported.selected_module));
            } else if (target.program.module != imported.selected_module)
                fail(current, "imported file does not declare selected module '" + imported.selected_module + "'");
            if (!imported.selected_class) current.dependencies.emplace(imported.local_name, selected);
        }
        return index;
    }
    static void binding_name(const Unit& unit, const std::string& name) {
        if (unit.dependencies.count(name) || unit.class_imports.count(name))
            fail(unit, "native module import binding cannot be shadowed: '" + name + "'");
    }
    void expression(const Unit& unit, Expression* expr) {
        if (!expr) return;
        expression(unit, expr->left.get()); expression(unit, expr->right.get());
        expression(unit, expr->condition.get()); expression(unit, expr->condition_lhs.get()); expression(unit, expr->condition_rhs.get());
        expression(unit, expr->when_true.get()); expression(unit, expr->when_false.get());
        for (auto& arg : expr->arguments) expression(unit, arg.get());
        if (expr->kind == Expression::Kind::RecordConstruct) {
            expr->name = class_type(unit, expr->name, true);
            return;
        }
        if (expr->kind != Expression::Kind::Call) return;
        const auto dot = expr->name.find('.');
        const Unit* target = &unit;
        std::string member = expr->name;
        bool external = false;
        if (dot != std::string::npos) {
            const auto binding = unit.dependencies.find(expr->name.substr(0, dot));
            if (binding == unit.dependencies.end()) fail(unit, "unknown native module binding");
            target = units[binding->second].get(); member = expr->name.substr(dot + 1); external = true;
        }
        const auto function = target->functions.find(member);
        if (function == target->functions.end() || !function->second.count(expr->arguments.size()))
            fail(unit, "unknown function or wrong call arity '" + expr->name + "'");
        if (external && !function->second.at(expr->arguments.size()))
            fail(unit, "imported function is private: '" + expr->name + "'");
        expr->name = target->prefix + member;
    }
    std::string class_type(const Unit& unit, const std::string& name, bool constructing = false) {
        const auto imported = unit.class_imports.find(name);
        if (imported != unit.class_imports.end()) {
            const auto& target = *units[imported->second.first];
            if (constructing && !target.constructor_exports.at(imported->second.second))
                fail(unit, "private imported constructor is not accessible");
            return target.prefix + imported->second.second;
        }
        if (unit.classes.count(name)) return unit.prefix + name;
        const auto dot = name.find('.');
        if (dot == std::string::npos) return name;
        const auto dependency = unit.dependencies.find(name.substr(0, dot));
        if (dependency == unit.dependencies.end()) fail(unit, "unknown module in class type");
        const auto& target = *units[dependency->second];
        auto member = target.class_exports.find(name.substr(dot + 1));
        if (member == target.class_exports.end() || !member->second)
            fail(unit, "unknown or private imported class type '" + name + "'");
        if (constructing && !target.constructor_exports.at(member->first))
            fail(unit, "private imported constructor is not accessible");
        return target.prefix + member->first;
    }
    void local(const Unit& unit, LocalBinding& binding) {
        binding_name(unit, binding.name);
        if (!binding.class_name.empty()) binding.class_name = class_type(unit, binding.class_name, binding.new_class);
        if (!binding.array_record_class.empty()) binding.array_record_class = class_type(unit, binding.array_record_class);
        expression(unit, binding.initializer.get());
        for (auto& arg : binding.constructor_arguments) expression(unit, arg.get());
    }
    void statement(const Unit& unit, Statement& stmt) {
        if (stmt.branch_local) local(unit, *stmt.branch_local);
        expression(unit, stmt.expression.get());
        expression(unit, stmt.condition.get()); expression(unit, stmt.condition_lhs.get()); expression(unit, stmt.condition_rhs.get());
        expression(unit, stmt.loop_step.get());
        expression(unit, stmt.array_commit_length.get());
        for (auto& value : stmt.array_commit_values) expression(unit, value.get());
        for (auto& arm : stmt.then_statements) statement(unit, arm);
        for (auto& arm : stmt.else_statements) statement(unit, arm);
    }
    void qualify_function(const Unit& unit, Function& fn) {
        binding_name(unit, fn.name);
        if (!fn.return_class.empty()) fn.return_class = class_type(unit, fn.return_class);
        if (!fn.method_owner.empty()) fn.method_owner = class_type(unit, fn.method_owner);
        for (auto& parameter : fn.parameters) {
            binding_name(unit, parameter.name);
            if (!parameter.class_name.empty()) parameter.class_name = class_type(unit, parameter.class_name);
        }
        for (auto& binding : fn.locals) local(unit, binding);
        for (auto& stmt : fn.statements) statement(unit, stmt);
        for (auto& stmt : fn.caught_throw_defers) statement(unit, stmt);
        for (auto& guard : fn.early_returns) {
            expression(unit, guard.condition.get()); expression(unit, guard.lhs.get()); expression(unit, guard.rhs.get()); expression(unit, guard.result.get());
        }
        for (auto& send : fn.actor_sends) expression(unit, send.payload.get());
        for (auto& write : fn.actor_field_writes) expression(unit, write.expression.get());
        for (const auto& spawn : fn.actor_spawns) binding_name(unit, spawn.name);
        if (!fn.catch_binding.empty()) binding_name(unit, fn.catch_binding);
        expression(unit, fn.caught_throw_value.get()); expression(unit, fn.result.get());
    }
};
} // namespace
Program load_native_program(const std::string& path, std::string source, SourceIdentity identity,
                            bool owned, bool scalar_loop, bool host) {
    return Loader(owned, scalar_loop, host).run(path, std::move(source), identity);
}
} // namespace oreslang
