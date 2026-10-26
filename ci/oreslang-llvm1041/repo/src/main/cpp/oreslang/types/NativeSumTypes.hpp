#pragma once
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace oreslang {
inline std::string native_sum_name(NativeSumType::Kind kind, ScalarType value, ScalarType error = ScalarType::I64) {
    auto name = [](ScalarType t) { return t == ScalarType::String ? "String" : t == ScalarType::Bool ? "bool" : "int"; };
    return std::string(kind == NativeSumType::Kind::Option ? "Option<" : "Result<") + name(value) +
        (kind == NativeSumType::Kind::Result ? std::string(",") + name(error) : "") + ">";
}
inline const NativeSumType& native_sum(const Program& program, const std::string& name) {
    for (const auto& type : program.sum_types) if (type.name == name) return type;
    throw std::runtime_error("unknown native sum type '" + name + "'");
}
// Context resolves constructors; the nominal type checker still independently
// validates every payload and use. This pass never turns arbitrary i64 into a sum.
inline void resolve_native_sums(Program& program) {
    struct Hint {
        ScalarType scalar = ScalarType::I64;
        std::string sum{}, cls{};
        bool array = false;
        ScalarType element = ScalarType::I64;
        std::string element_class{};
    };
    using Env = std::unordered_map<std::string, Hint>;
    std::unordered_map<std::string, const ClassDecl*> classes;
    std::unordered_map<std::string, const Function*> functions;
    for (const auto& cls : program.classes) classes.emplace(cls.name, &cls);
    for (const auto& fn : program.functions) functions.emplace(native_function_slot(fn.name, fn.parameters.size()), &fn);
    auto field_hint = [](const ClassField& f) { return Hint{f.scalar_type, f.sum_type, f.class_name, f.int_array, f.array_element_type, f.array_record_class}; };
    auto local_hint = [](const LocalBinding& b) { return Hint{b.scalar_type, b.sum_type, b.class_name, b.int_array, b.array_element_type, b.array_record_class}; };
    auto add_type = [&](NativeSumType::Kind kind, ScalarType value, ScalarType error = ScalarType::I64) {
        const auto name = native_sum_name(kind, value, error);
        for (const auto& type : program.sum_types) if (type.name == name) return name;
        program.sum_types.push_back({kind, name, value, error}); return name;
    };
    std::function<Hint(Expression&, const Env&, const std::string&)> expression;
    expression = [&](Expression& e, const Env& env, const std::string& expected) -> Hint {
        using E = Expression::Kind;
        if (e.kind == E::SumConstruct) {
            std::string selected = expected;
            if (selected.empty()) {
                if (e.name != "Some" || e.arguments.size() != 1) throw std::runtime_error("native sum constructor requires a declared Option/Result context");
                const auto value = expression(*e.arguments[0], env, {});
                if (!value.sum.empty() || !value.cls.empty() || value.array) throw std::runtime_error("native sum compound payload requires ownership/lifetime ABI");
                selected = add_type(NativeSumType::Kind::Option, value.scalar);
            }
            const NativeSumType type = native_sum(program, selected);
            const bool option = type.kind == NativeSumType::Kind::Option;
            if ((option && e.name != "Some" && e.name != "None") || (!option && e.name != "Ok" && e.name != "Err"))
                throw std::runtime_error("native sum constructor variant does not match declared sum type");
            if (e.arguments.size() != (e.name == "None" ? 0u : 1u)) throw std::runtime_error("native sum constructor payload arity mismatch");
            for (auto& arg : e.arguments) expression(*arg, env, {});
            e.sum_type = selected; return {ScalarType::I64, selected};
        }
        if (e.kind == E::Variable) return env.count(e.name) ? env.at(e.name) : Hint{};
        if (e.kind == E::String) return {ScalarType::String};
        if (e.kind == E::Boolean) return {ScalarType::Bool};
        if (e.kind == E::Integer) return {};
        if (e.kind == E::Field) {
            const auto owner = expression(*e.left, env, {});
            if (!owner.cls.empty() && classes.count(owner.cls))
                for (const auto& field : classes.at(owner.cls)->fields) if (field.name == e.name) return field_hint(field);
            return {};
        }
        if (e.kind == E::Call) {
            const auto found = functions.find(native_function_slot(e.name, e.arguments.size()));
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                std::size_t slot = i;
                if (found != functions.end() && !e.argument_names.empty() && !e.argument_names[i].empty()) {
                    slot = 0; while (slot < found->second->parameters.size() && found->second->parameters[slot].name != e.argument_names[i]) ++slot;
                }
                const auto context = found != functions.end() && slot < found->second->parameters.size() ? found->second->parameters[slot].sum_type : std::string{};
                expression(*e.arguments[i], env, context);
            }
            if (found == functions.end()) return {};
            return {found->second->return_type, found->second->return_sum, found->second->return_class};
        }
        if (e.kind == E::RecordConstruct) {
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                const ClassField* field = classes.count(e.name) ? constructor_field(*classes.at(e.name), i) : nullptr;
                expression(*e.arguments[i], env, field ? field->sum_type : std::string{});
            }
            return {ScalarType::I64, {}, e.name};
        }
        if (e.kind == E::ArrayCall || e.kind == E::SumAccess) {
            const auto owner = expression(*e.left, env, {});
            for (auto& arg : e.arguments) expression(*arg, env, {});
            if (!owner.sum.empty()) {
                const NativeSumType type = native_sum(program, owner.sum);
                const bool option = type.kind == NativeSumType::Kind::Option;
                const bool query = option ? e.name == "is_some" || e.name == "is_none" : e.name == "is_ok" || e.name == "is_err";
                const bool unwrap = e.name == "unwrap";
                const bool fallback = e.name == "unwrap_or";
                if ((!query && !unwrap && !fallback) || e.arguments.size() != (fallback ? 1u : 0u))
                    throw std::runtime_error("unsupported native sum method or arity '" + e.name + "'");
                e.kind = E::SumAccess; e.sum_type = owner.sum;
                e.resolved_type = query ? ScalarType::Bool : type.value_type;
                return {e.resolved_type};
            }
            return {owner.element, {}, owner.element_class};
        }
        if (e.kind == E::Conditional) {
            if (e.condition) expression(*e.condition, env, {});
            if (e.condition_lhs) expression(*e.condition_lhs, env, {});
            if (e.condition_rhs) expression(*e.condition_rhs, env, {});
            const auto yes = expression(*e.when_true, env, expected);
            expression(*e.when_false, env, expected.empty() ? yes.sum : expected);
            return yes;
        }
        Hint left;
        if (e.left) left = expression(*e.left, env, {});
        if (e.right) expression(*e.right, env, {});
        for (auto& arg : e.arguments) expression(*arg, env, {});
        if (e.kind == E::LogicalNot || e.kind == E::Equal || e.kind == E::NotEqual || e.kind == E::Less || e.kind == E::LessEqual ||
                e.kind == E::Greater || e.kind == E::GreaterEqual || e.kind == E::LogicalAnd || e.kind == E::LogicalOr || e.kind == E::LogicalXor)
            return {ScalarType::Bool};
        return left;
    };
    for (auto& cls : program.classes) {
        Env env;
        for (std::size_t i = 0; i < cls.constructor_parameters.size(); ++i) {
            const auto* field = constructor_field(cls, i);
            env.emplace(cls.constructor_parameters[i], field ? field_hint(*field) : Hint{cls.constructor_parameter_types.empty() ? ScalarType::I64 : cls.constructor_parameter_types[i]});
        }
        for (auto& field : cls.fields) if (field.initializer_expression) expression(*field.initializer_expression, env, field.sum_type);
    }
    auto function = [&](Function& fn) {
        Env env;
        for (const auto& p : fn.parameters) env.emplace(p.name, Hint{p.scalar_type, p.sum_type, p.class_name});
        for (auto& binding : fn.locals) {
            if (binding.initializer) expression(*binding.initializer, env, binding.sum_type);
            for (std::size_t i = 0; i < binding.constructor_arguments.size(); ++i) {
                const auto* field = classes.count(binding.class_name) ? constructor_field(*classes.at(binding.class_name), i) : nullptr;
                expression(*binding.constructor_arguments[i], env, field ? field->sum_type : std::string{});
            }
            env.emplace(binding.name, local_hint(binding));
        }
        std::function<void(std::vector<Statement>&, Env&)> statements;
        statements = [&](std::vector<Statement>& items, Env& scope) {
            for (auto& item : items) {
                if (item.branch_local && item.branch_local->initializer) expression(*item.branch_local->initializer, scope, item.branch_local->sum_type);
                if (item.kind == Statement::Kind::ForEach || item.kind == Statement::Kind::ForIndex) {
                    if (item.expression) expression(*item.expression, scope, {});
                    Env body = scope; body.emplace(item.branch_local->name, local_hint(*item.branch_local));
                    if (item.condition) expression(*item.condition, body, {});
                    if (item.loop_step) expression(*item.loop_step, body, {});
                    statements(item.then_statements, body); continue;
                }
                if (item.condition) expression(*item.condition, scope, {});
                if (item.condition_lhs) expression(*item.condition_lhs, scope, {});
                if (item.condition_rhs) expression(*item.condition_rhs, scope, {});
                std::string context = item.kind == Statement::Kind::Return ? fn.return_sum : std::string{};
                if (item.kind == Statement::Kind::ScalarWrite && scope.count(item.receiver))
                    context = scope.at(item.receiver).sum;
                if (!item.receiver.empty() && scope.count(item.receiver) && classes.count(scope.at(item.receiver).cls))
                    for (const auto& field : classes.at(scope.at(item.receiver).cls)->fields) if (field.name == item.field) context = field.sum_type;
                if (item.expression) expression(*item.expression, scope, context);
                if (item.array_commit_length) expression(*item.array_commit_length, scope, {});
                for (auto& value : item.array_commit_values) expression(*value, scope, {});
                Env yes = scope, no = scope;
                statements(item.then_statements, yes); statements(item.else_statements, no);
                if (item.kind == Statement::Kind::ScopedLocal) scope.emplace(item.branch_local->name, local_hint(*item.branch_local));
            }
        };
        statements(fn.statements, env); statements(fn.caught_throw_defers, env);
        for (auto& guard : fn.early_returns) {
            if (guard.condition) expression(*guard.condition, env, {});
            if (guard.lhs) expression(*guard.lhs, env, {});
            if (guard.rhs) expression(*guard.rhs, env, {});
            expression(*guard.result, env, fn.return_sum);
        }
        if (fn.result) expression(*fn.result, env, fn.return_sum);
    };
    for (auto& fn : program.functions) function(fn);
    for (auto& actor : program.actors) function(actor.on_message);
}
}
