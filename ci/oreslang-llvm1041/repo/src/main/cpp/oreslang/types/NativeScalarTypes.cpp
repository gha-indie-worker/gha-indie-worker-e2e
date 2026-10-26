#include "oreslang/types/NativeScalarTypes.hpp"
#include "oreslang/types/NativeClassLayout.hpp"
#include "oreslang/types/NativeSumTypes.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace oreslang {
namespace {
enum class Kind { Int, Bool, String, Class, Array, Sum, Void };
struct Type {
    Kind kind = Kind::Int;
    std::string name;
    bool operator==(const Type& other) const { return kind == other.kind && name == other.name; }
};
using Locals = std::unordered_map<std::string, Type>;
Type scalar(bool boolean) { return {boolean ? Kind::Bool : Kind::Int, {}}; }
Type scalar(ScalarType type) { return {type == ScalarType::String ? Kind::String : type == ScalarType::Bool ? Kind::Bool : Kind::Int, {}}; }
Type declared_field_type(const ClassField& field) {
    if (!field.sum_type.empty()) return {Kind::Sum, field.sum_type};
    if (!field.class_name.empty()) return {Kind::Class, field.class_name};
    if (!field.int_array) return scalar(field.scalar_type);
    return {Kind::Array, !field.array_record_class.empty() ? field.array_record_class :
        field.array_element_type == ScalarType::String ? "String" : "int"};
}
Type constructor_parameter_type(const ClassDecl& cls, std::size_t index) {
    const auto* field = constructor_field(cls, index);
    return field ? declared_field_type(*field) : scalar(cls.constructor_parameter_types.empty()
        ? ScalarType::I64 : cls.constructor_parameter_types.at(index));
}
Type parameter_type(const Parameter& p) {
    if (!p.sum_type.empty()) return {Kind::Sum, p.sum_type};
    return p.class_name.empty() ? scalar(p.scalar_type) : Type{Kind::Class, p.class_name};
}
Type result_type(const Function& fn) {
    if (!fn.return_sum.empty()) return {Kind::Sum, fn.return_sum};
    return fn.returns_void ? Type{Kind::Void, {}} : fn.return_class.empty()
        ? scalar(fn.return_type) : Type{Kind::Class, fn.return_class};
}
Type local_type(const LocalBinding& local) {
    if (!local.sum_type.empty()) return {Kind::Sum, local.sum_type};
    if (local.int_array && local.array_element_type != ScalarType::I64 &&
            local.array_element_type != ScalarType::String)
        throw std::runtime_error("unsupported native array element type");
    if (local.int_array) return {Kind::Array, !local.array_record_class.empty() ? local.array_record_class :
        local.array_element_type == ScalarType::String ? "String" : "int"};
    return local.class_name.empty() ? scalar(local.scalar_type) : Type{Kind::Class, local.class_name};
}
[[noreturn]] void fail(std::size_t offset, const std::string& detail) {
    throw std::runtime_error("native scalar type mismatch: " + detail + " at byte " + std::to_string(offset));
}
void require(Type actual, Type expected, std::size_t offset, const std::string& detail) {
    if (!(actual == expected)) fail(offset, detail);
}
}
void validate_native_scalar_types(const Program& program) {
    std::unordered_map<std::string, const ClassDecl*> classes;
    std::unordered_map<std::string, const Function*> functions;
    for (const auto& cls : program.classes) classes.emplace(cls.name, &cls);
    const NativeClassLayouts layouts(program);
    auto owns_arrays = [&](const std::string& name) { return classes.count(name) && layouts.at(name).owns_arrays; };
    auto immutable_record = [&](const std::string& name) -> const ClassDecl& {
        if (!classes.count(name)) fail(0, "unknown record class '" + name + "'");
        for (const auto& leaf : layouts.at(name).leaves)
            if (!leaf.field->final_field || leaf.field->int_array) fail(0, "record collections require immutable scalar/String fields");
        return *classes.at(name);
    };
    for (const auto& fn : program.functions)
        functions.emplace(native_function_slot(fn.name, fn.parameters.size()), &fn);
    auto field_type = [&](Type owner, const std::string& name, std::size_t offset) -> Type {
        if (owner.kind == Kind::Array && name == "size") return scalar(false);
        if (owner.kind != Kind::Class) fail(offset, "field needs a nominal class");
        const auto cls = classes.find(owner.name);
        if (cls == classes.end()) fail(offset, "unknown class");
        for (const auto& field : cls->second->fields)
            if (field.name == name) return declared_field_type(field);
        fail(offset, "unknown class field");
    };
    std::function<Type(const Expression&, const Locals&)> expression;
    expression = [&](const Expression& e, const Locals& locals) -> Type {
        const auto evaluate = [&]() -> Type {
        using E = Expression::Kind;
        switch (e.kind) {
            case E::SumConstruct: {
                const auto& type = native_sum(program, e.sum_type);
                const bool option = type.kind == NativeSumType::Kind::Option;
                if ((option && e.name != "Some" && e.name != "None") || (!option && e.name != "Ok" && e.name != "Err") ||
                        e.arguments.size() != (e.name == "None" ? 0u : 1u)) fail(e.offset, "sum variant/payload arity");
                if (!e.arguments.empty()) require(expression(*e.arguments[0], locals), scalar(e.name == "Err" ? type.error_type : type.value_type), e.offset, "sum payload type");
                return {Kind::Sum, type.name};
            }
            case E::SumAccess: {
                require(expression(*e.left, locals), {Kind::Sum, e.sum_type}, e.offset, "sum receiver type");
                const auto& type = native_sum(program, e.sum_type);
                const bool option = type.kind == NativeSumType::Kind::Option;
                const bool query = option ? e.name == "is_some" || e.name == "is_none" : e.name == "is_ok" || e.name == "is_err";
                if ((!query && e.name != "unwrap" && e.name != "unwrap_or") || e.arguments.size() != (e.name == "unwrap_or" ? 1u : 0u))
                    fail(e.offset, "sum member or payload arity");
                if (e.name == "unwrap_or") require(expression(*e.arguments.at(0), locals), scalar(type.value_type), e.offset, "sum fallback type");
                return query ? scalar(true) : scalar(type.value_type);
            }
            case E::Integer: return scalar(false);
            case E::Boolean: return scalar(true);
            case E::String: return scalar(ScalarType::String);
            case E::Console:
                require(expression(*e.left, locals), scalar(ScalarType::String), e.offset, "stdio requires string");
                return {Kind::Void, {}};
            case E::Variable: return locals.at(e.name);
            case E::Field: return field_type(expression(*e.left, locals), e.name, e.offset);
            case E::Copy: case E::Take: case E::Share: case E::Borrow: case E::BorrowMut:
                return expression(*e.left, locals);
            case E::Negate:
                require(expression(*e.left, locals), scalar(false), e.offset, "unary - requires int");
                return scalar(false);
            case E::LogicalNot:
                require(expression(*e.left, locals), scalar(true), e.offset, "! requires bool");
                return scalar(true);
            case E::Equal: case E::NotEqual: {
                auto left = expression(*e.left, locals), right = expression(*e.right, locals);
                if (!(left == right) || (left.kind != Kind::Int && left.kind != Kind::Bool && left.kind != Kind::String)) fail(e.offset, "equality operand types");
                return scalar(true);
            }
            case E::Less: case E::LessEqual: case E::Greater: case E::GreaterEqual:
                require(expression(*e.left, locals), scalar(false), e.offset, "ordered comparison requires int");
                require(expression(*e.right, locals), scalar(false), e.offset, "ordered comparison requires int");
                return scalar(true);
            case E::LogicalAnd: case E::LogicalOr: case E::LogicalXor:
                require(expression(*e.left, locals), scalar(true), e.offset, "logical operator requires bool");
                require(expression(*e.right, locals), scalar(true), e.offset, "logical operator requires bool");
                return scalar(true);
            case E::Conditional: {
                if (e.condition) require(expression(*e.condition, locals), scalar(true), e.offset, "conditional needs bool");
                else {
                    auto lhs = expression(*e.condition_lhs, locals), rhs = expression(*e.condition_rhs, locals);
                    require(lhs, rhs, e.offset, "conditional equality types");
                }
                auto lhs = expression(*e.when_true, locals), rhs = expression(*e.when_false, locals);
                require(lhs, rhs, e.offset, "conditional arm types");
                return lhs;
            }
            case E::Add: {
                auto left = expression(*e.left, locals), right = expression(*e.right, locals);
                require(left, right, e.offset, "addition operand types");
                if (left.kind != Kind::Int && left.kind != Kind::String) fail(e.offset, "addition requires int or string");
                return left;
            }
            case E::Subtract: case E::Multiply:
                require(expression(*e.left, locals), scalar(false), e.offset, "arithmetic requires int");
                require(expression(*e.right, locals), scalar(false), e.offset, "arithmetic requires int");
                return scalar(false);
            case E::ArrayCall: {
                const auto owner = expression(*e.left, locals);
                if (owner.kind != Kind::Array) fail(e.offset, "array receiver");
                const Type element = owner.name == "String" ? scalar(ScalarType::String) : owner.name == "int" ? scalar(false)
                    : Type{Kind::Class, immutable_record(owner.name).name};
                for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                    const bool index = (e.name == "get" || e.name == "set") && i == 0;
                    if (!index && element.kind == Kind::Class &&
                            e.arguments[i]->kind != E::RecordConstruct &&
                            e.arguments[i]->kind != E::Call && e.arguments[i]->kind != E::Conditional &&
                            !(e.arguments[i]->kind == E::ArrayCall && e.arguments[i]->name == "get"))
                        fail(e.offset, "record arrays require a new immutable record or existing array element");
                    require(expression(*e.arguments[i], locals), index ? scalar(false) : element,
                            e.arguments[i]->offset, index ? "array index requires int" : "array element type mismatch");
                }
                return e.name == "get" ? element : scalar(false);
            }
            case E::RecordConstruct: {
                const bool owning = owns_arrays(e.name);
                const auto& cls = owning ? *classes.at(e.name) : immutable_record(e.name);
                if (owning) for (const auto& field : cls.fields)
                    if (field.int_array && !field.constructor_argument)
                        fail(e.offset, "owning return constructor requires explicit array arguments");
                if (e.arguments.size() != cls.constructor_parameters.size()) fail(e.offset, "record constructor argument arity mismatch");
                for (std::size_t i = 0; i < e.arguments.size(); ++i)
                    require(expression(*e.arguments[i], locals), constructor_parameter_type(cls, i), e.offset, "record constructor argument type mismatch");
                return {Kind::Class, cls.name};
            }
            case E::Call: {
                const auto& fn = *functions.at(native_function_slot(e.name, e.arguments.size()));
                std::size_t positional = 0;
                for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                    std::size_t slot = positional++;
                    if (!e.argument_names.empty() && !e.argument_names[i].empty()) {
                        slot = 0;
                        while (slot < fn.parameters.size() && fn.parameters[slot].name != e.argument_names[i]) ++slot;
                    }
                    require(expression(*e.arguments[i], locals), parameter_type(fn.parameters.at(slot)),
                            e.arguments[i]->offset, "argument bool/int/nominal type differs from parameter");
                }
                return result_type(fn);
            }
        }
        fail(e.offset, "unsupported expression");
        };
        const Type result = evaluate();
        // Preserve the admitted operand type for lowering. Handles are i64 at
        // the ABI boundary, so codegen must never infer String equality from
        // their physical representation or from syntax alone.
        if (result.kind == Kind::Sum) e.sum_type = result.name;
        if (result.kind == Kind::String) e.resolved_type = ScalarType::String;
        else if (result.kind == Kind::Bool) e.resolved_type = ScalarType::Bool;
        else if (result.kind == Kind::Int) e.resolved_type = ScalarType::I64;
        else if (result.kind == Kind::Class) e.resolved_class = result.name;
        return result;
    };
    auto compare = [&](const Expression& lhs, const Expression& rhs, const Locals& locals) {
        const auto left = expression(lhs, locals), right = expression(rhs, locals);
        if ((left.kind != Kind::Int && left.kind != Kind::Bool) || !(left == right))
            fail(lhs.offset, "legacy equality operands must have matching int or bool types");
    };
    // Constructor slots currently have explicit int types. Bool literals can
    // initialize bool fields; integer bit patterns cannot become boolean storage.
    for (const auto& cls : program.classes) {
        for (const auto& field : cls.fields)
            if (!field.array_record_class.empty()) (void)immutable_record(field.array_record_class);
        Locals parameters;
        if (!cls.constructor_parameter_types.empty() && cls.constructor_parameter_types.size() != cls.constructor_parameters.size())
            throw std::runtime_error("native constructor type/parameter arity mismatch");
        for (std::size_t i = 0; i < cls.constructor_parameters.size(); ++i)
            parameters.emplace(cls.constructor_parameters[i], constructor_parameter_type(cls, i));
        for (const auto& field : cls.fields)
            if (field.initializer_expression)
                require(expression(*field.initializer_expression, parameters), declared_field_type(field),
                        field.initializer_expression->offset, "class field initializer type");
    }
    std::function<void(const Function&, Locals)> function;
    function = [&](const Function& fn, Locals locals) {
        if (!fn.return_class.empty() && !owns_arrays(fn.return_class)) (void)immutable_record(fn.return_class);
        for (const auto& parameter : fn.parameters) locals.emplace(parameter.name, parameter_type(parameter));
        for (const auto& binding : fn.locals) {
            if (binding.record_view) {
                if (owns_arrays(binding.class_name)) {
                    std::function<bool(const Expression&)> owning_call_tree = [&](const Expression& e) {
                        return e.kind == Expression::Kind::Call ||
                            (e.kind == Expression::Kind::Conditional && owning_call_tree(*e.when_true) && owning_call_tree(*e.when_false));
                    };
                    if (!binding.initializer || !owning_call_tree(*binding.initializer))
                        fail(binding.offset, "owning class result requires a named call binding");
                } else (void)immutable_record(binding.class_name);
            }
            if (!binding.array_record_class.empty()) (void)immutable_record(binding.array_record_class);
            if (binding.initializer) {
                const bool discarded_effect = binding.name.rfind("$effect", 0) == 0 &&
                    binding.initializer->kind == Expression::Kind::Console;
                require(expression(*binding.initializer, locals), discarded_effect ? Type{Kind::Void, {}} : local_type(binding), binding.offset, "local initializer type");
            }
            for (std::size_t i = 0; i < binding.constructor_arguments.size(); ++i) {
                const auto& cls = *classes.at(binding.class_name);
                const auto& arg = binding.constructor_arguments[i];
                require(expression(*arg, locals), constructor_parameter_type(cls, i), arg->offset, "constructor parameter type mismatch");
            }
            locals.emplace(binding.name, local_type(binding));
        }
        if (fn.scalar_while)
            require(locals.at(fn.scalar_while->counter), scalar(false), fn.offset, "loop counter requires int");
        std::function<void(const std::vector<Statement>&, Locals&)> statements;
        statements = [&](const std::vector<Statement>& items, Locals& scope) {
            for (const auto& item : items) {
                if (item.kind == Statement::Kind::ForIndex) {
                    require(expression(*item.branch_local->initializer, scope), scalar(false), item.offset, "index initializer requires int");
                    Locals body = scope; body.emplace(item.branch_local->name, scalar(false));
                    require(expression(*item.condition, body), scalar(true), item.offset, "index condition requires bool");
                    require(expression(*item.loop_step, body), scalar(false), item.offset, "index update requires int");
                    statements(item.then_statements, body);
                } else if (item.kind == Statement::Kind::Return) {
                    if (item.expression) require(expression(*item.expression, scope), result_type(fn), item.offset, "structured return type");
                    else if (!fn.returns_void) fail(item.offset, "non-void structured return requires value");
                } else if (item.kind == Statement::Kind::ForEach) {
                    const auto source = expression(*item.expression, scope);
                    if (source.kind != Kind::Array) fail(item.offset, "for-of requires an array");
                    const auto& iterator = *item.branch_local;
                    const Type element = source.name == "String" ? scalar(ScalarType::String) : source.name == "int" ? scalar(false)
                        : Type{Kind::Class, immutable_record(source.name).name};
                    require(local_type(iterator), element, item.offset, "for-of element annotation");
                    Locals body = scope; body.emplace(iterator.name, element);
                    statements(item.then_statements, body);
                } else if (item.kind == Statement::Kind::ScopedLocal) {
                    const auto& binding = *item.branch_local;
                    if (!binding.class_name.empty()) (void)immutable_record(binding.class_name);
                    require(expression(*binding.initializer, scope), local_type(binding), binding.offset, "branch local initializer");
                    scope.emplace(binding.name, local_type(binding));
                } else if (item.kind == Statement::Kind::Conditional) {
                    if (item.condition) require(expression(*item.condition, scope), scalar(true), item.offset, "condition requires bool");
                    else compare(*item.condition_lhs, *item.condition_rhs, scope);
                    Locals yes = scope, no = scope;
                    statements(item.then_statements, yes); statements(item.else_statements, no);
                } else if (item.kind == Statement::Kind::ArrayCommitFilled) {
                    require(expression(*item.array_commit_length, scope), scalar(false), item.offset, "array length requires int");
                    for (const auto& arg : item.array_commit_values)
                        require(expression(*arg, scope), scalar(false), arg->offset, "Array<int> fill requires int");
                } else if (item.kind == Statement::Kind::ScalarWrite) {
                    require(expression(*item.expression, scope), scope.at(item.receiver), item.offset, "scalar assignment type");
                } else if (!item.receiver.empty()) {
                    require(expression(*item.expression, scope), field_type(scope.at(item.receiver), item.field, item.offset),
                            item.offset, "field assignment type");
                } else if (item.expression) expression(*item.expression, scope);
            }
        };
        statements(fn.statements, locals); statements(fn.caught_throw_defers, locals);
        if (fn.caught_throw_value)
            require(expression(*fn.caught_throw_value, locals), scalar(false), fn.offset, "throw payload requires int");
        if (!fn.catch_binding.empty()) locals.emplace(fn.catch_binding, scalar(false));
        const Type result = result_type(fn);
        for (const auto& guard : fn.early_returns) {
            if (guard.condition) require(expression(*guard.condition, locals), scalar(true), guard.offset, "guard requires bool");
            else compare(*guard.lhs, *guard.rhs, locals);
            require(expression(*guard.result, locals), result, guard.offset, "guarded return type");
        }
        if (fn.result) require(expression(*fn.result, locals), result, fn.offset, "return type");
        for (const auto& send : fn.actor_sends)
            require(expression(*send.payload, locals), scalar(false), send.offset, "actor message requires int");
        for (const auto& write : fn.actor_field_writes)
            require(expression(*write.expression, locals), scalar(false), write.offset, "actor field requires int");
    };
    for (const auto& fn : program.functions) {
        if (fn.name == "main" && (fn.return_type != ScalarType::I64 || !fn.return_class.empty() || !fn.return_sum.empty())) fail(fn.offset, "OS entrypoint requires int or void");
        function(fn, {});
    }
    for (const auto& actor : program.actors) {
        Locals fields;
        for (const auto& field : actor.fields) {
            require(expression(*field.initializer, fields), scalar(false), field.offset, "actor initializer requires int");
            fields.emplace(field.name, scalar(false));
        }
        function(actor.on_message, std::move(fields));
    }
}
}
