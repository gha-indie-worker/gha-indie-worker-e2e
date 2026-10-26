#pragma once
#include "oreslang/types/NativeClassLayout.hpp"
namespace oreslang {
inline std::unique_ptr<Expression> clone_native_expression(const Expression* source) {
    if (!source) return nullptr;
    auto target = std::make_unique<Expression>();
    target->kind = source->kind; target->offset = source->offset;
    target->integer = source->integer; target->boolean = source->boolean;
    target->resolved_type = source->resolved_type; target->resolved_class = source->resolved_class;
    target->sum_type = source->sum_type;
    target->name = source->name; target->argument_names = source->argument_names;
    target->type_arguments = source->type_arguments; target->type_arguments_present = source->type_arguments_present;
    target->left = clone_native_expression(source->left.get()); target->right = clone_native_expression(source->right.get());
    target->condition = clone_native_expression(source->condition.get());
    target->condition_lhs = clone_native_expression(source->condition_lhs.get()); target->condition_rhs = clone_native_expression(source->condition_rhs.get());
    target->when_true = clone_native_expression(source->when_true.get()); target->when_false = clone_native_expression(source->when_false.get());
    for (const auto& arg : source->arguments) target->arguments.push_back(clone_native_expression(arg.get()));
    return target;
}
inline ClassDecl native_flat_class(const ClassDecl& source, const NativeClassLayouts& layouts) {
    ClassDecl flat;
    flat.name = source.name; flat.exported = source.exported; flat.methods = source.methods;
    flat.explicit_constructor = source.explicit_constructor; flat.constructor_exported = source.constructor_exported;
    flat.constructor_parameters = source.constructor_parameters; flat.constructor_parameter_types = source.constructor_parameter_types;
    for (const auto& leaf : layouts.at(source.name).leaves) {
        ClassField field;
        const auto& original = *leaf.field;
        field.sum_type = leaf.field->sum_type;
        field.name = leaf.path; field.initial = original.initial;
        field.public_member = original.public_member; field.scalar_type = original.scalar_type;
        field.int_array = original.int_array; field.final_field = original.final_field;
        field.mutable_referent = original.mutable_referent; field.array_element_type = original.array_element_type;
        field.array_record_class = original.array_record_class;
        field.constructor_argument = leaf.access.front().second->constructor_argument;
        if (leaf.access.size() == 1) field.initializer_expression = clone_native_expression(original.initializer_expression.get());
        else if (!field.int_array) {
            field.initializer_expression = std::make_unique<Expression>();
            field.initializer_expression->kind = Expression::Kind::Variable;
            field.initializer_expression->name = leaf.path;
            field.initializer_expression->resolved_type = original.scalar_type;
        }
        flat.fields.push_back(std::move(field));
    }
    return flat;
}
}
