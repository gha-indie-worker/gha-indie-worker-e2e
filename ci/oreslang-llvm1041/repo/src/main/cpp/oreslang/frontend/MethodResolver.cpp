#include "oreslang/frontend/MethodResolver.hpp"
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace oreslang {
void resolve_native_methods(Program& program) {
    std::unordered_map<std::string, const ClassDecl*> classes;
    for (const auto& cls : program.classes) classes.emplace(cls.name, &cls);
    for (auto& fn : program.functions) {
        if (!fn.method_owner.empty()) {
            if (!classes.count(fn.method_owner) || fn.parameters.empty()
                    || fn.parameters.front().name != "self"
                    || fn.parameters.front().class_name != fn.method_owner)
                throw std::runtime_error("invalid compiler-owned method receiver");
            fn.native_internal = true; // source visibility is not a raw FFI export
        }
        std::unordered_map<std::string, std::string> owners;
        std::unordered_map<std::string, const LocalBinding*> arrays;
        for (const auto& parameter : fn.parameters)
            if (!parameter.class_name.empty()) owners.emplace(parameter.name, parameter.class_name);
        for (const auto& binding : fn.locals)
            if (!binding.class_name.empty()) owners.emplace(binding.name, binding.class_name);
        for (const auto& binding : fn.locals) if (binding.int_array) arrays.emplace(binding.name, &binding);
        std::function<std::string(const Expression&)> nominal = [&](const Expression& expr) -> std::string {
            if (expr.kind == Expression::Kind::Variable) return owners.count(expr.name) ? owners.at(expr.name) : std::string{};
            if (expr.kind != Expression::Kind::Field || !expr.left) return {};
            const auto parent = nominal(*expr.left);
            if (parent.empty() || !classes.count(parent)) return {};
            for (const auto& field : classes.at(parent)->fields) if (field.name == expr.name) return field.class_name;
            return {};
        };
        std::function<void(Expression*)> expression = [&](Expression* expr) {
            if (!expr) return;
            expression(expr->left.get()); expression(expr->right.get());
            expression(expr->condition.get()); expression(expr->condition_lhs.get()); expression(expr->condition_rhs.get());
            expression(expr->when_true.get()); expression(expr->when_false.get());
            for (auto& arg : expr->arguments) expression(arg.get());
            if (expr->kind != Expression::Kind::ArrayCall || !expr->left) return;
            const auto owner = nominal(*expr->left);
            if (owner.empty()) return; // array receiver validates separately
            auto cls = classes.find(owner);
            if (cls == classes.end()) throw std::runtime_error("unknown instance method class");
            const ClassMethod* method = nullptr;
            for (const auto& candidate : cls->second->methods)
                if (candidate.name == expr->name && candidate.arity == expr->arguments.size()) method = &candidate;
            if (!method) throw std::runtime_error("unknown method or wrong explicit call arity '" + expr->name + "'");
            if (!method->public_member && fn.method_owner != cls->first)
                throw std::runtime_error("private instance method is not accessible");
            auto loan = std::make_unique<Expression>();
            loan->kind = method->mutable_receiver ? Expression::Kind::BorrowMut : Expression::Kind::Borrow;
            loan->offset = expr->offset; loan->left = std::move(expr->left);
            expr->kind = Expression::Kind::Call;
            expr->name = cls->first + "." + method->name;
            if (!expr->argument_names.empty()) expr->argument_names.insert(expr->argument_names.begin(), "");
            expr->arguments.insert(expr->arguments.begin(), std::move(loan));
        };
        std::function<void(Statement&)> statement = [&](Statement& stmt) {
            if (stmt.kind == Statement::Kind::ForEach) {
                auto& iterator = *stmt.branch_local;
                const auto& source = *stmt.expression;
                if (source.kind == Expression::Kind::Variable && arrays.count(source.name)) {
                    const auto& array = *arrays.at(source.name);
                    iterator.class_name = array.array_record_class;
                    iterator.scalar_type = array.array_element_type;
                } else if (source.kind == Expression::Kind::Field && source.left &&
                        !nominal(*source.left).empty()) {
                    const auto& cls = *classes.at(nominal(*source.left));
                    const ClassField* array = nullptr;
                    for (const auto& field : cls.fields) if (field.name == source.name && field.int_array) array = &field;
                    if (!array) throw std::runtime_error("native for-of requires a typed array field");
                    iterator.class_name = array->array_record_class;
                    iterator.scalar_type = array->array_element_type;
                } else throw std::runtime_error("native for-of requires a named lexical array or class array field");
                iterator.record_view = !iterator.class_name.empty();
                expression(stmt.expression.get());
                const auto saved = owners;
                if (!iterator.class_name.empty()) owners.emplace(iterator.name, iterator.class_name);
                for (auto& child : stmt.then_statements) statement(child);
                owners = saved;
                return;
            }
            expression(stmt.expression.get()); expression(stmt.condition.get()); expression(stmt.condition_lhs.get()); expression(stmt.condition_rhs.get());
            expression(stmt.loop_step.get());
            expression(stmt.array_commit_length.get());
            for (auto& item : stmt.array_commit_values) expression(item.get());
            if (stmt.branch_local) {
                expression(stmt.branch_local->initializer.get());
                if (!stmt.branch_local->class_name.empty()) owners.emplace(stmt.branch_local->name, stmt.branch_local->class_name);
            }
            const auto saved = owners;
            for (auto& arm : stmt.then_statements) statement(arm);
            owners = saved;
            for (auto& arm : stmt.else_statements) statement(arm);
            owners = saved;
        };
        for (auto& local : fn.locals) {
            expression(local.initializer.get());
            for (auto& arg : local.constructor_arguments) expression(arg.get());
        }
        for (auto& item : fn.statements) statement(item);
        for (auto& item : fn.caught_throw_defers) statement(item);
        for (auto& guard : fn.early_returns) { expression(guard.condition.get()); expression(guard.lhs.get()); expression(guard.rhs.get()); expression(guard.result.get()); }
        for (auto& send : fn.actor_sends) expression(send.payload.get());
        for (auto& write : fn.actor_field_writes) expression(write.expression.get());
        expression(fn.caught_throw_value.get()); expression(fn.result.get());
    }
}
}
