#include "oreslang/types/OwnershipChecker.hpp"
#include "oreslang/types/NativeClassLayout.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/types/OwnershipCfg.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {

// Select a callable by name+explicit-arity before validating loan/type contracts.
namespace {

using Mode = Parameter::Mode;
using Functions = std::unordered_map<std::string, const Function*>;

struct LocalOwnership {
    Mode mode = Mode::Value;
    bool moved = false;
    bool heap_owned = false; // Experimental region slots cannot transfer ownership yet.
    bool int_array = false; // Non-copy lexical array; no borrows or escapes.
    bool record_view = false; // readonly materialized value, not an incoming loan
    bool mutable_owner = false; // lexical let mut or exclusive borrowed parameter
    bool assignable = false; // lexical slot replacement, not referent mutation
    std::size_t shared_loans = 0;
    bool exclusive_loan = false;
    std::string class_name;
    std::unordered_set<std::string> mutable_paths;
    std::unordered_map<std::string, std::string> subobjects;
    std::unordered_set<std::string> class_fields;
    std::unordered_set<std::string> class_array_fields;
    std::unordered_set<std::string> final_fields;
    std::unordered_set<std::string> public_fields;
    // Primitive i64 class fields can be moved and later restored by a write.
    std::unordered_set<std::string> moved_fields;
};
using Locals = std::unordered_map<std::string, LocalOwnership>;

bool same_affine_state(const Locals& left, const Locals& right) {
    if (left.size() != right.size()) return false;
    for (const auto& item : left) {
        const auto found = right.find(item.first);
        if (found == right.end()) return false;
        if (item.second.moved != found->second.moved ||
                item.second.moved_fields != found->second.moved_fields ||
                item.second.shared_loans != found->second.shared_loans ||
                item.second.exclusive_loan != found->second.exclusive_loan)
            return false;
    }
    return true;
}

[[noreturn]] void reject(const Expression& expr, const std::string& reason) {
    throw std::runtime_error(reason + " at byte " + std::to_string(expr.offset));
}

LocalOwnership& local(const Expression& expr, Locals& locals) {
    auto it = locals.find(expr.name);
    if (it == locals.end())
        reject(expr, "internal ownership error: undeclared parameter '" + expr.name + "'");
    return it->second;
}

void check_value(const Expression& expr, Locals& locals, const Functions& functions);
void check_pure_guard(const Expression& expr, Locals& locals, const Functions& functions);

LocalOwnership& checked_field(const std::string& receiver, const std::string& field,
                               std::size_t offset, Locals& locals) {
    auto found = locals.find(receiver);
    if (found == locals.end() || found->second.class_name.empty() ||
            found->second.class_fields.count(field) == 0)
        throw std::runtime_error("unknown class field or receiver at byte " + std::to_string(offset));
    // This is the lexical accessibility set for the currently checked frame.
    // Only methods of this exact nominal class receive its private fields;
    // forwarding a receiver to an ordinary function does not forward privilege.
    if (!found->second.public_fields.count(field))
        throw std::runtime_error("ownership violation: private class field '" +
                                 found->second.class_name + "." + field +
                                 "' is not accessible at byte " + std::to_string(offset));
    if (found->second.moved)
        throw std::runtime_error("ownership violation: use-after-move class field at byte " +
                                 std::to_string(offset));
    return found->second;
}

// An array handle is never an i64 source value; resolve its owned place.
LocalOwnership& array_owner(const Expression& receiver, Locals& locals, bool write) {
    const auto place = native_place(receiver);
    Expression root_expression; root_expression.kind = Expression::Kind::Variable; root_expression.name = place.first;
    const Expression* root = &root_expression;
    const std::string& field = place.second;
    auto it = locals.find(root->name);
    if (it == locals.end())
        reject(receiver, "native Array<int> operation requires a live owner");
    if (it->second.moved)
        reject(receiver, "ownership violation: use-after-move Array<int> owner");
    auto& owner = it->second;
    const bool valid = field.empty() ? owner.int_array :
        !owner.class_name.empty() && owner.class_array_fields.count(field);
    if (!field.empty() && valid && !owner.public_fields.count(field))
        reject(receiver, "ownership violation: private class field is not accessible");
    if (!valid || (!field.empty() && owner.moved_fields.count(field))) {
        if (field.empty())
            reject(receiver, write
                ? "ownership violation: Array<int> mutation requires live let mut owner"
                : "native Array<int> operation requires a live array owner");
        reject(receiver, "native Array<int> operation requires an owned Array<int> place");
    }
    if (write && !field.empty() && !owner.mutable_paths.count(field)) reject(receiver, "ownership violation: array mutation crosses readonly nested field");
    if (owner.mode == Mode::Borrow && write)
        reject(receiver, "ownership violation: Array<int> cannot be accessed through a borrowed class");
    if (owner.exclusive_loan || (write && owner.shared_loans))
        reject(receiver, "ownership violation: Array<int> conflicts with active class loan");
    if (write && owner.mode == Mode::Value && !owner.mutable_owner)
        reject(receiver, "ownership violation: Array<int> mutation requires live let mut owner");
    return owner;
}

struct Loan {
    std::string name;
    bool exclusive = false;
    // Projection borrows alias their complete class owner until this
    // synchronous call returns. This intentionally forbids disjoint loans
    // until source/CFG lowering models exact per-field loan provenance.
    bool projected_scalar_field = false;
    std::string projected_class;
};

// Only named scalar storage and admitted scalar-class layouts are addressable.
// A call loan lasts through evaluation of every later argument and the call.
// Reborrows retain the caller's provenance; shared-to-exclusive escalation,
// temporary loans, escaping references and unsupported projections fail closed.
Loan acquire(const Expression& expr, Mode expected, Locals& locals) {
    const bool exclusive = expr.kind == Expression::Kind::BorrowMut;
    if (expr.kind != Expression::Kind::Borrow &&
        expr.kind != Expression::Kind::BorrowMut)
        reject(expr, "borrowed argument requires explicit rt borrow or rt mut borrow");
    if ((expected == Mode::BorrowMut) != exclusive)
        reject(expr, "borrow mode does not match borrowed parameter");
    if (!expr.left)
        reject(expr, "native borrow requires an addressable named int parameter");
    const Expression* root = expr.left.get();
    std::string projection;
    if (root->kind == Expression::Kind::Field) {
        const auto place = native_place(*root);
        projection = place.second;
        while (root->kind == Expression::Kind::Field) root = root->left.get();
        LocalOwnership& projected_owner = checked_field(place.first, projection, expr.offset, locals);
        if (exclusive && !projected_owner.mutable_paths.count(projection)) reject(expr, "ownership violation: mutable projection crosses final or readonly field");
        if (!projected_owner.subobjects.count(projection) && (projected_owner.class_array_fields.count(projection) ||
                !projected_owner.class_array_fields.empty()))
            reject(expr, "ownership violation: native projected borrow requires primitive-only class fields");
        if (projected_owner.moved_fields.count(projection)) reject(expr, "ownership violation: cannot borrow a moved class field");
    }
    if (root->kind != Expression::Kind::Variable)
        reject(expr, "native borrow requires an addressable named int parameter");
    LocalOwnership& source = local(*root, locals);
    if (source.int_array)
        reject(expr, "ownership violation: native Array<int> cannot be borrowed or escaped");
    // A borrowed parameter has a caller-held lifetime and already carries
    // provenance to addressable i64 storage. A nested call can temporarily
    // reborrow that same storage, but it cannot strengthen a shared borrow
    // into a mutable one. Its new loan ends at that nested call's return.
    if (source.heap_owned && exclusive)
        reject(expr, "ownership violation: native owned int mutable borrow needs handle-aware writeback");
    if (source.mode == Mode::Borrow && exclusive)
        reject(expr, "cannot mutably reborrow shared parameter '" + root->name + "'");
    if (exclusive && source.mode == Mode::Value && !source.mutable_owner)
        reject(expr, "cannot mutably borrow immutable owner '" + root->name + "'");
    if (source.moved)
        reject(expr, (source.class_name.empty()
            ? "borrow of explicitly moved int '" : "ownership violation: borrow of moved class '")
            + root->name + "'");
    if (!source.class_name.empty() && !source.moved_fields.empty())
        reject(expr, "ownership violation: cannot borrow a partially moved class '" +
            root->name + "'");
    if (source.exclusive_loan || (exclusive && source.shared_loans != 0))
        reject(expr, "conflicting active native borrow of '" + root->name + "'");
    if (exclusive) source.exclusive_loan = true;
    else ++source.shared_loans;
    const auto child = source.subobjects.find(projection);
    return {root->name, exclusive, !projection.empty() && child == source.subobjects.end(),
        child == source.subobjects.end() ? std::string{} : child->second};
}

void release(const Loan& loan, Locals& locals) {
    auto& source = locals.at(loan.name);
    if (loan.exclusive) source.exclusive_loan = false;
    else --source.shared_loans;
}

void check_call(const Expression& expr, Locals& locals, const Functions& functions) {
    const auto found = functions.find(native_function_slot(expr.name, expr.arguments.size()));
    if (found == functions.end())
        reject(expr, "internal ownership error: unresolved function");
    const auto& parameters = found->second->parameters;
    if (parameters.size() != expr.arguments.size())
        reject(expr, "internal ownership error: call arity mismatch");

    // Preserve the full set of argument loans until the call has completed.
    // Nested calls observe outstanding outer-call loans. Evaluation is
    // strictly left-to-right, matching the LLVM emitter.
    if (!expr.argument_names.empty() &&
            expr.argument_names.size() != expr.arguments.size())
        reject(expr, "invalid native named call argument layout");

    std::vector<Loan> loans;
    std::vector<bool> assigned(parameters.size(), false);
    std::size_t positional = 0;
    bool named_seen = false;
    for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
        const Expression& argument = *expr.arguments[i];
        const std::string label = expr.argument_names.empty()
                ? std::string{} : expr.argument_names[i];
        std::size_t formal = positional;
        if (!label.empty()) {
            named_seen = true;
            formal = 0;
            while (formal < parameters.size() &&
                   parameters[formal].name != label) ++formal;
            if (formal == parameters.size())
                reject(argument, "ownership violation: unknown named parameter '" + label + "'");
        } else {
            if (named_seen)
                reject(argument, "ownership violation: positional argument after named argument");
            ++positional;
        }
        if (formal >= parameters.size() || assigned[formal])
            reject(argument, "ownership violation: duplicate named parameter binding");
        assigned[formal] = true;

        // Acquire loans in *lexical source order* while using the target
        // parameter's ownership mode. A later argument must observe earlier
        // mutable/shared loans even if its formal slot is numerically lower.
        const Parameter& parameter = parameters[formal];
        if (parameter.mode == Mode::Value) {
            if (!parameter.class_name.empty())
                reject(argument, "ownership violation: by-value class parameters require move/drop lowering");
            check_value(argument, locals, functions);
        } else {
            // Acquire first so malformed loans preserve precise diagnostics
            // for temporaries and missing explicit 'rt borrow' markers.
            Loan loan = acquire(argument, parameter.mode, locals);
            const auto& source = locals.at(loan.name);
            // A class-field projection supplies a borrowed i64 pointer,
            // not a borrowed class pointer. All such field loans currently
            // freeze the whole owner, preventing sibling-alias escape.
            const std::string actual_class =
                loan.projected_scalar_field ? std::string{} : !loan.projected_class.empty() ? loan.projected_class : source.class_name;
            if (actual_class != parameter.class_name)
                reject(argument, "ownership violation: borrowed argument type mismatch");
            loans.push_back(std::move(loan));
        }
    }
    for (auto it = loans.rbegin(); it != loans.rend(); ++it)
        release(*it, locals);
}

void check_value(const Expression& expr, Locals& locals, const Functions& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
        case Expression::Kind::Boolean:
        case Expression::Kind::String:
            return;
        case Expression::Kind::Console:
            check_value(*expr.left, locals, functions);
            return;
        case Expression::Kind::Variable: {
            LocalOwnership& source = local(expr, locals);
            if (source.moved)
                reject(expr, "use of explicitly moved int '" + expr.name + "'");
            if (source.exclusive_loan)
                reject(expr, "read conflicts with active mutable borrow of '" + expr.name + "'");
            if (source.int_array)
                reject(expr, "ownership violation: native Array<int> is non-Copy and cannot escape");
            if (!source.class_name.empty())
                reject(expr, "ownership violation: class is non-Copy; use an explicit borrow");
            return;
        }
        case Expression::Kind::Field: {
            if (expr.name == "size" && expr.left &&
                    expr.left->kind == Expression::Kind::Field) {
                array_owner(*expr.left, locals, false);
                return;
            }
            if (expr.left && expr.left->kind == Expression::Kind::Variable) {
                const auto found = locals.find(expr.left->name);
                if (found != locals.end() && found->second.int_array) {
                    if (found->second.moved)
                        reject(expr, "ownership violation: use-after-move Array<int> owner");
                    if (expr.name != "size")
                        reject(expr, "ownership violation: only Array<int>.size is readable");
                    return;
                }
            }
            const auto place = native_place(expr);
            const auto& source = checked_field(place.first, place.second, expr.offset, locals);
            if (source.class_array_fields.count(place.second)) reject(expr, "ownership violation: Array<int> field is non-Copy and cannot escape");
            if (source.subobjects.count(place.second)) reject(expr, "ownership violation: nested class field is non-Copy; use an explicit borrow");
            if (source.moved_fields.count(place.second)) reject(expr, "ownership violation: use-after-move class field");
            if (source.exclusive_loan)
                reject(expr, "ownership violation: field read conflicts with active mutable borrow");
            return;
        }
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
        case Expression::Kind::Negate:
        case Expression::Kind::LogicalNot:
            check_value(*expr.left, locals, functions);
            return;
        case Expression::Kind::Take: {
            // The bounded class subset has only trivially destructible i64
            // fields. Lowering a field take as a load is valid only because
            // checker state invalidates that place until an explicit write.
            check_value(*expr.left, locals, functions);
            if (expr.left->kind == Expression::Kind::Variable) {
                LocalOwnership& source = local(*expr.left, locals);
                if (source.mode != Mode::Value)
                    reject(expr, "cannot rt take through a borrowed parameter");
                // Experimental native owned-i64 values transfer the scalar,
                // drop their old heap slot immediately and disarm the
                // generated exit-time drop flag. The emitted native backend
                // must keep that conditional flag in sync with this move.
                if (source.exclusive_loan || source.shared_loans != 0)
                    reject(expr, "cannot move int while borrow is active");
                source.moved = true;
            } else if (expr.left->kind == Expression::Kind::Field) {
                const Expression& field = *expr.left;
                // Nested array.size and other projections are not direct
                // owned class field places for destructive rt take.
                if (!field.left || field.left->kind != Expression::Kind::Variable)
                    reject(expr, "native rt take requires a direct named class field");
                LocalOwnership& source = checked_field(
                    field.left->name, field.name, field.offset, locals);
                if (source.final_fields.count(field.name))
                    reject(expr, "ownership violation: cannot take final class field");
                if (source.mode != Mode::Value)
                    reject(expr, "ownership violation: cannot take a class field through a borrowed parameter");
                if (source.exclusive_loan || source.shared_loans != 0)
                    reject(expr, "ownership violation: cannot take a class field with active loans");
                source.moved_fields.insert(field.name);
            }
            return;
        }
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
        case Expression::Kind::Equal:
        case Expression::Kind::NotEqual:
        case Expression::Kind::Less:
        case Expression::Kind::LessEqual:
        case Expression::Kind::Greater:
        case Expression::Kind::GreaterEqual:
        case Expression::Kind::LogicalXor:
            check_value(*expr.left, locals, functions);
            check_value(*expr.right, locals, functions);
            return;
        case Expression::Kind::LogicalAnd:
        case Expression::Kind::LogicalOr: {
            check_value(*expr.left, locals, functions);
            Locals skipped = locals;
            Locals evaluated = locals;
            check_value(*expr.right, evaluated, functions);
            if (!same_affine_state(skipped, evaluated))
                reject(expr, "ownership violation: short-circuit CFG predecessor ownership mismatch");
            locals = std::move(skipped);
            return;
        }
        case Expression::Kind::Conditional: {
            if (expr.condition)
                check_value(*expr.condition, locals, functions);
            else {
                check_pure_guard(*expr.condition_lhs, locals, functions);
                check_pure_guard(*expr.condition_rhs, locals, functions);
            }
            Locals when_true = locals;
            Locals when_false = locals;
            check_value(*expr.when_true, when_true, functions);
            check_value(*expr.when_false, when_false, functions);
            if (!same_affine_state(when_true, when_false))
                reject(expr, "ownership violation: ternary CFG predecessor ownership mismatch");
            locals = std::move(when_true);
            return;
        }
        case Expression::Kind::ArrayCall: {
            if (!expr.left)
                reject(expr, "native Array<int> operation requires a named owner");
            array_owner(*expr.left, locals, false);
            if (expr.name != "get" || expr.arguments.size() != 1)
                reject(expr, "native Array<int> expression supports only get(index)");
            check_value(*expr.arguments[0], locals, functions);
            return;
        }
        case Expression::Kind::SumConstruct:
        case Expression::Kind::SumAccess:
        case Expression::Kind::RecordConstruct:
            if (expr.left) check_value(*expr.left, locals, functions);
            for (const auto& arg : expr.arguments) check_value(*arg, locals, functions);
            return;
        case Expression::Kind::Call:
            if (functions.at(native_function_slot(expr.name, expr.arguments.size()))->returns_void)
                reject(expr, "void call cannot be used as an int expression");
            check_call(expr, locals, functions);
            return;
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
            reject(expr, "borrowed reference may only be passed to a borrowed parameter");
    }
    reject(expr, "invalid native ownership expression kind");
}

// Guard conditions must not mutate ownership or execute calls with hidden
// effects. A taken return has its own ownership snapshot and cannot poison
// the distinct fallthrough path. This is *not* general branch-lifetime proof.
void check_pure_guard(const Expression& expr, Locals& locals, const Functions& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
        case Expression::Kind::Boolean:
        case Expression::Kind::String:
        case Expression::Kind::Variable:
        case Expression::Kind::Field:
            check_value(expr, locals, functions);
            return;
        case Expression::Kind::Negate:
        case Expression::Kind::LogicalNot:
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
            check_pure_guard(*expr.left, locals, functions);
            return;
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
        case Expression::Kind::Equal:
        case Expression::Kind::NotEqual:
        case Expression::Kind::Less:
        case Expression::Kind::LessEqual:
        case Expression::Kind::Greater:
        case Expression::Kind::GreaterEqual:
        case Expression::Kind::LogicalAnd:
        case Expression::Kind::LogicalOr:
        case Expression::Kind::LogicalXor:
            check_pure_guard(*expr.left, locals, functions);
            check_pure_guard(*expr.right, locals, functions);
            return;
        case Expression::Kind::Conditional:
            if (expr.condition) check_pure_guard(*expr.condition, locals, functions);
            else {
                check_pure_guard(*expr.condition_lhs, locals, functions);
                check_pure_guard(*expr.condition_rhs, locals, functions);
            }
            check_pure_guard(*expr.when_true, locals, functions);
            check_pure_guard(*expr.when_false, locals, functions);
            return;
        case Expression::Kind::Take:
        case Expression::Kind::Call:
        case Expression::Kind::SumConstruct:
        case Expression::Kind::SumAccess:
        case Expression::Kind::RecordConstruct:
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
        case Expression::Kind::ArrayCall:
        case Expression::Kind::Console:
            reject(expr, "native guarded condition must be pure and cannot move, call or borrow");
    }
    reject(expr, "unsupported guarded condition");
}

// Only primitive stack int reads are safe in the initial caught-throw CFG.
// A syntactically pure Array<int>.size or heap-owned int read still invokes
// native runtime functions and therefore lacks a nonthrowing proof.
void check_caught_throw_scalar_read(const Expression& expr, Locals& locals,
                                   const Functions& functions) {
    switch (expr.kind) {
        case Expression::Kind::String:
        case Expression::Kind::Console:
            reject(expr, "native caught throw cannot own string region effects");
        case Expression::Kind::Integer:
        case Expression::Kind::Boolean:
            return;
        case Expression::Kind::Variable: {
            const auto& place = local(expr, locals);
            if (place.heap_owned || place.int_array || !place.class_name.empty())
                reject(expr, "native caught throw scalar read requires stack i64");
            check_value(expr, locals, functions);
            return;
        }
        case Expression::Kind::Field: {
            if (!expr.left || expr.left->kind != Expression::Kind::Variable)
                reject(expr, "native caught throw scalar read requires direct named field");
            const auto& place = local(*expr.left, locals);
            if (place.heap_owned || place.int_array ||
                    place.class_array_fields.count(expr.name))
                reject(expr, "native caught throw scalar read requires stack i64");
            check_value(expr, locals, functions);
            return;
        }
        case Expression::Kind::Negate:
        case Expression::Kind::LogicalNot:
            check_caught_throw_scalar_read(*expr.left, locals, functions);
            return;
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
        case Expression::Kind::Equal:
        case Expression::Kind::NotEqual:
        case Expression::Kind::Less:
        case Expression::Kind::LessEqual:
        case Expression::Kind::Greater:
        case Expression::Kind::GreaterEqual:
        case Expression::Kind::LogicalAnd:
        case Expression::Kind::LogicalOr:
        case Expression::Kind::LogicalXor:
            check_caught_throw_scalar_read(*expr.left, locals, functions);
            check_caught_throw_scalar_read(*expr.right, locals, functions);
            return;
        case Expression::Kind::Conditional:
            if (expr.condition)
                check_caught_throw_scalar_read(*expr.condition, locals, functions);
            else {
                check_caught_throw_scalar_read(*expr.condition_lhs, locals, functions);
                check_caught_throw_scalar_read(*expr.condition_rhs, locals, functions);
            }
            check_caught_throw_scalar_read(*expr.when_true, locals, functions);
            check_caught_throw_scalar_read(*expr.when_false, locals, functions);
            return;
        default:
            reject(expr, "native caught throw scalar read cannot call, move or borrow");
    }
}

} // namespace

void validate_i64_ownership(Program& program) {
    // A re-validation attempt must revoke every stale lowering authorization
    // before checking anything. Failed ownership admission therefore cannot
    // leave executable Future-pipeline authority from an earlier pass.
    for (auto& function : program.functions)
        for (auto& pipeline : function.future_pipelines)
            pipeline.ownership_admitted = false;
    Functions functions;
    for (const auto& function : program.functions)
        functions.emplace(native_function_slot(function.name, function.parameters.size()), &function);
    std::unordered_map<std::string, std::unordered_set<std::string>> class_fields;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_arrays;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_finals;
    const NativeClassLayouts layouts(program);
    for (const auto& cls : program.classes) {
        std::unordered_set<std::string> fields, arrays, finals;
        for (const auto& leaf : layouts.at(cls.name).leaves) {
            fields.insert(leaf.path);
            if (leaf.field->int_array) arrays.insert(leaf.path);
            if (leaf.field->final_field) finals.insert(leaf.path);
        }
        for (const auto& child : layouts.at(cls.name).subobjects) { fields.insert(child.first); finals.insert(child.first); }
        class_fields.emplace(cls.name, std::move(fields));
        class_arrays.emplace(cls.name, std::move(arrays));
        class_finals.emplace(cls.name, std::move(finals));
    }
    // The compiler's actor-owned temporary path is narrower than general
    // expression lowering. Check source name resolution and purity *before*
    // emitting any LLVM IR; a syntactically pure but undeclared variable
    // must never reach codegen's value slot lookup.
    for (const auto& actor : program.actors) {
        if (actor.on_message.locals.empty()) continue;
        const auto& method = actor.on_message;
        if (method.parameters.size() != 1)
            throw std::runtime_error(
                "ownership violation: native actor owned locals require one message argument");
        std::unordered_set<std::string> visible;
        visible.insert(method.parameters.front().name);
        for (const auto& field : actor.fields)
            if (!visible.insert(field.name).second)
                throw std::runtime_error(
                    "ownership violation: duplicate actor field/message name");

        std::function<void(const Expression&)> pure;
        pure = [&](const Expression& expr) {
            using K = Expression::Kind;
            if (expr.kind == K::Integer || expr.kind == K::Boolean) return;
            if (expr.kind == K::Variable) {
                if (!visible.count(expr.name))
                    throw std::runtime_error(
                        "ownership violation: actor handler unknown or forward local '" +
                        expr.name + "'");
                return;
            }
            if (expr.kind == K::Negate || expr.kind == K::LogicalNot) {
                if (!expr.left)
                    throw std::runtime_error("ownership violation: missing unary operand");
                pure(*expr.left);
                return;
            }
            if (expr.kind == K::Add || expr.kind == K::Subtract ||
                    expr.kind == K::Multiply || expr.kind == K::Equal ||
                    expr.kind == K::NotEqual || expr.kind == K::Less ||
                    expr.kind == K::LessEqual || expr.kind == K::Greater ||
                    expr.kind == K::GreaterEqual || expr.kind == K::LogicalAnd ||
                    expr.kind == K::LogicalOr || expr.kind == K::LogicalXor) {
                if (!expr.left || !expr.right)
                    throw std::runtime_error("ownership violation: missing binary operand");
                pure(*expr.left);
                pure(*expr.right);
                return;
            }
            if (expr.kind == K::Conditional && expr.when_true && expr.when_false &&
                    (expr.condition || (expr.condition_lhs && expr.condition_rhs))) {
                if (expr.condition) pure(*expr.condition);
                if (expr.condition_lhs) pure(*expr.condition_lhs);
                if (expr.condition_rhs) pure(*expr.condition_rhs);
                pure(*expr.when_true);
                pure(*expr.when_false);
                return;
            }
            throw std::runtime_error(
                "ownership violation: actor owned temporary may not call, borrow, move, or escape");
        };
        for (const auto& local : method.locals) {
            if (!local.heap_owned || local.owner_mutable || !local.initializer)
                throw std::runtime_error(
                    "ownership violation: actor scoped local requires immutable owned initializer");
            if (visible.count(local.name))
                throw std::runtime_error(
                    "ownership violation: actor temporary shadows live owner '" +
                    local.name + "'");
            pure(*local.initializer);
            visible.insert(local.name);
        }
        for (const auto& write : method.actor_field_writes)
            pure(*write.expression);
        if (method.result) pure(*method.result);
    }

    for (const auto& function : program.functions) {
        Locals locals;
        for (const auto& param : function.parameters) {
            if (!param.class_name.empty()) {
                if (!class_fields.count(param.class_name))
                    throw std::runtime_error("unknown parameter class '" + param.class_name + "'");
                if (param.mode == Mode::Value)
                    throw std::runtime_error("ownership violation: by-value class parameter '" +
                        param.name + "' requires exclusive borrow or proven move/drop lowering");
            }
            LocalOwnership entry;
            entry.mode = param.mode;
            // By-value scalar parameters own data that may be lent exclusively;
            // their identifiers remain read-only. Borrowed parameters retain
            // caller loans. Const locals do not gain referent mutability.
            entry.mutable_owner = param.mode == Mode::Value;
            // Parameter bindings are val slots even for exclusive loans.
            // Referent capability does not grant identifier reassignment.
            entry.class_name = param.class_name;
            if (!param.class_name.empty()) {
                entry.mutable_paths = layouts.mutable_paths(param.class_name);
                entry.subobjects = layouts.at(param.class_name).subobjects;
                entry.class_fields = class_fields.at(param.class_name);
                entry.class_array_fields = class_arrays.at(param.class_name);
                entry.final_fields = class_finals.at(param.class_name);
                entry.public_fields = layouts.accessible(param.class_name, function.method_owner);
            }
            locals.emplace(param.name, std::move(entry));
        }
        for (const auto& pipeline : function.future_pipelines) {
            if (pipeline.plans.empty() ||
                    pipeline.plans.front().input_ownership !=
                        types::pipeline_root_ownership(pipeline.root_consumed))
                throw std::runtime_error(
                    "ownership violation: Future pipeline root ownership proof "
                    "does not match source rt take shape");
            for (std::size_t i = 1; i < pipeline.plans.size(); ++i)
                if (pipeline.plans[i].input_ownership !=
                        types::PipelineInputOwnership::OwnedPipelineValue)
                    throw std::runtime_error(
                        "ownership violation: Future pipeline internal edge "
                        "lost pipeline-owned authority");
            types::validate_future_pipeline_chain(pipeline.plans);
        }
        for (const auto& binding : function.locals) {
            LocalOwnership entry;
            entry.heap_owned = binding.heap_owned;
            entry.int_array = binding.int_array;
            entry.mutable_owner = binding.owner_mutable;
            entry.assignable = binding.assignable;
            entry.class_name = binding.class_name;
            if (binding.int_array) {
                if (!binding.owner_mutable || binding.heap_owned || binding.initializer)
                    throw std::runtime_error("invalid native Array<int> owner");
            } else if (binding.name.rfind("$effect", 0) == 0 && binding.initializer &&
                    binding.initializer->kind == Expression::Kind::ArrayCall) {
                const auto& call = *binding.initializer;
                array_owner(*call.left, locals, true);
                for (const auto& arg : call.arguments) check_value(*arg, locals, functions);
            } else if (!binding.task_await_name.empty()) {
                if (binding.scalar_type != ScalarType::I64 ||
                        binding.initializer)
                    throw std::runtime_error(
                        "ownership violation: invalid Task<int> await result binding");
                const bool task_exists = std::any_of(
                    function.source_tasks.begin(), function.source_tasks.end(),
                    [&](const SourceTaskI64Binding& task) {
                        return task.name == binding.task_await_name &&
                            task.awaited;
                    });
                if (!task_exists)
                    throw std::runtime_error(
                        "ownership violation: Task<int> await lost producer provenance");
                // Capture-free producer transfers no caller owner or borrow.
                // The awaited scalar becomes an ordinary owned stack value.
            } else if (!binding.future_await_name.empty()) {
                if (binding.scalar_type != ScalarType::I64 ||
                        binding.initializer)
                    throw std::runtime_error(
                        "ownership violation: invalid Future<int> await result binding");
                const auto future = std::find_if(
                    function.future_pipelines.begin(),
                    function.future_pipelines.end(),
                    [&](const SourceFuturePipelineI64Binding& pipeline) {
                        return pipeline.name == binding.future_await_name &&
                            pipeline.awaited;
                    });
                if (future == function.future_pipelines.end())
                    throw std::runtime_error(
                        "ownership violation: Future<int> await lost pipeline provenance");
                types::validate_future_pipeline_chain(future->plans);
            } else if (binding.record_view) {
                check_value(*binding.initializer, locals, functions);
                const bool owning = !class_arrays.at(binding.class_name).empty();
                if (owning && !binding.owner_mutable)
                    throw std::runtime_error("owning class result requires val or let mut owner");
                entry.record_view = !owning;
                entry.mode = owning ? Mode::Value : Mode::Borrow;
                entry.mutable_owner = owning && binding.owner_mutable;
                entry.mutable_paths = layouts.mutable_paths(binding.class_name);
                entry.subobjects = layouts.at(binding.class_name).subobjects;
                entry.class_fields = class_fields.at(binding.class_name);
                entry.class_array_fields = class_arrays.at(binding.class_name);
                entry.final_fields = class_finals.at(binding.class_name);
                entry.public_fields = layouts.accessible(binding.class_name, function.method_owner);
            } else if (binding.new_class) {
                // Pure syntax alone is insufficient: experimental owned-int
                // operands carry fallible region reads. Do not let one unwind
                // an incompletely constructed object before drop glue exists.
                std::function<void(const Expression&)> validate_scalar;
                validate_scalar = [&](const Expression& expr) {
                    using K = Expression::Kind;
                    if (expr.kind == K::Integer || expr.kind == K::Boolean || expr.kind == K::String) return;
                    if (expr.kind == K::SumConstruct) {
                        for (const auto& arg : expr.arguments) validate_scalar(*arg);
                        return;
                    }
                    if (expr.kind == K::Field) {
                        check_value(expr, locals, functions);
                        return;
                    }
                    if (expr.kind == K::Variable) {
                        const auto it = locals.find(expr.name);
                        if (it == locals.end() || it->second.heap_owned ||
                                it->second.int_array || !it->second.class_name.empty())
                            reject(expr, "native scalar constructor arguments require plain stack scalar values");
                        check_value(expr, locals, functions);
                        return;
                    }
                    if ((expr.kind == K::Negate || expr.kind == K::LogicalNot) && expr.left) {
                        validate_scalar(*expr.left); return;
                    }
                    if ((expr.kind == K::Add || expr.kind == K::Subtract ||
                            expr.kind == K::Multiply || expr.kind == K::Equal ||
                            expr.kind == K::NotEqual || expr.kind == K::Less ||
                            expr.kind == K::LessEqual || expr.kind == K::Greater ||
                            expr.kind == K::GreaterEqual || expr.kind == K::LogicalAnd ||
                            expr.kind == K::LogicalOr || expr.kind == K::LogicalXor) &&
                            expr.left && expr.right) {
                        validate_scalar(*expr.left);
                        validate_scalar(*expr.right);
                        return;
                    }
                    if (expr.kind == K::Conditional && expr.when_true && expr.when_false &&
                            (expr.condition || (expr.condition_lhs && expr.condition_rhs))) {
                        if (expr.condition) validate_scalar(*expr.condition);
                        if (expr.condition_lhs) validate_scalar(*expr.condition_lhs);
                        if (expr.condition_rhs) validate_scalar(*expr.condition_rhs);
                        validate_scalar(*expr.when_true);
                        validate_scalar(*expr.when_false);
                        return;
                    }
                    reject(expr, "native scalar constructor argument requires pure integer expression");
                };
                const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding.class_name; });
                for (std::size_t i = 0; i < binding.constructor_arguments.size(); ++i) {
                    const auto& arg = binding.constructor_arguments[i];
                    const auto* field = constructor_field(*cls, i);
                    if (field && !field->class_name.empty()) {
                        const Expression& source = arg->kind == Expression::Kind::Take && arg->left ? *arg->left : *arg;
                        if (source.kind != Expression::Kind::Variable) reject(*arg, "nested constructor requires a unique named class owner");
                        auto& owner = local(source, locals);
                        if (owner.class_name != field->class_name || owner.mode != Mode::Value || owner.moved ||
                                owner.exclusive_loan || owner.shared_loans || !owner.moved_fields.empty())
                            reject(*arg, "nested constructor requires a live unique complete class owner");
                        owner.moved = true; continue;
                    }
                    if (!field || !field->int_array) { validate_scalar(*arg); continue; }
                    const Expression* source = arg.get();
                    if (source->kind == Expression::Kind::Take && source->left) source = source->left.get();
                    if (source->kind != Expression::Kind::Variable)
                        reject(*arg, "collection constructor requires a unique named lexical array");
                    auto& owner = local(*source, locals);
                    if (!owner.int_array || owner.mode != Mode::Value || owner.moved ||
                            owner.exclusive_loan || owner.shared_loans)
                        reject(*arg, "collection constructor requires a live unique array owner");
                    owner.moved = true;
                }
                entry.mutable_paths = layouts.mutable_paths(binding.class_name);
                entry.subobjects = layouts.at(binding.class_name).subobjects;
                entry.class_fields = class_fields.at(binding.class_name);
                entry.class_array_fields = class_arrays.at(binding.class_name);
                entry.final_fields = class_finals.at(binding.class_name);
                entry.public_fields = layouts.accessible(binding.class_name, function.method_owner);
                if (!entry.class_array_fields.empty() && !binding.owner_mutable)
                    throw std::runtime_error("ownership violation: class with Array<int> fields requires let mut owner");
            } else if (binding.move_class) {
                // Transfer the whole class's unique array-owned places, not
                // individual fields. A source with moved fields or live
                // borrows is rejected below before any IR is generated.
                if (!class_arrays.at(binding.class_name).empty() &&
                        !binding.owner_mutable)
                    throw std::runtime_error("ownership violation: moved Array<int> class requires let mut owner");
                const auto found = locals.find(binding.move_source);
                if (found == locals.end() || found->second.class_name != binding.class_name)
                    throw std::runtime_error("ownership violation: class move type mismatch at byte " +
                        std::to_string(binding.offset));
                auto& source = found->second;
                if (source.mode != Mode::Value || source.moved ||
                        source.exclusive_loan || source.shared_loans != 0 ||
                        !source.moved_fields.empty())
                    throw std::runtime_error("ownership violation: class move requires live unborrowed owner at byte " +
                        std::to_string(binding.offset));
                entry.mutable_paths = source.mutable_paths;
                entry.subobjects = source.subobjects;
                entry.class_fields = source.class_fields;
                entry.class_array_fields = source.class_array_fields;
                entry.final_fields = source.final_fields;
                entry.public_fields = source.public_fields;
                source.moved = true;
            } else {
                check_value(*binding.initializer, locals, functions);
            }
            locals.emplace(binding.name, std::move(entry));
        }
        std::vector<const Statement*> deferred_effects;
        // Provenance for an array field is tracked as a single compile-time
        // shadow handle in this first native slice. Branch-specific moves
        // require phi ownership slots, so reject the entire mixed case.
        const bool conditional_effects = std::any_of(function.statements.begin(),
            function.statements.end(), [](const Statement& stmt) {
                return stmt.kind == Statement::Kind::Conditional;
            });
        std::vector<Locals> structured_exits;
        std::unordered_set<std::string> root_names;
        for (const auto& item : locals) root_names.insert(item.first);
        std::unordered_map<std::string, unsigned> iteration_loans;
        std::function<void(const Statement&, Locals&)> check_effect;
        auto scoped_entry = [&](const LocalBinding& binding) {
            LocalOwnership entry;
            entry.mutable_owner = binding.owner_mutable;
            entry.assignable = binding.assignable;
            entry.class_name = binding.class_name;
            if (!binding.class_name.empty()) {
                if (!class_fields.count(binding.class_name))
                    throw std::runtime_error("native scoped value has unknown class");
                entry.mode = Mode::Borrow; entry.record_view = true;
                entry.mutable_paths = layouts.mutable_paths(binding.class_name);
                entry.subobjects = layouts.at(binding.class_name).subobjects;
                entry.class_fields = class_fields.at(binding.class_name);
                entry.class_array_fields = class_arrays.at(binding.class_name);
                entry.final_fields = class_finals.at(binding.class_name);
                entry.public_fields = layouts.accessible(binding.class_name, function.method_owner);
            }
            return entry;
        };
        auto scoped_effects = [&](const std::vector<Statement>& body, Locals& state) {
            for (const auto& action : body) check_effect(action, state);
            for (const auto& action : body) if (action.kind == Statement::Kind::ScopedLocal)
                state.erase(action.branch_local->name);
        };
        check_effect = [&](const Statement& stmt, Locals& state) {
            if (stmt.kind == Statement::Kind::ScalarWrite) {
                const auto found = state.find(stmt.receiver);
                if (found == state.end()) throw std::runtime_error("assignment requires a visible scalar slot");
                auto writable = [&]() {
                    const auto& target = state.at(stmt.receiver);
                    return target.assignable && !target.heap_owned && !target.int_array && target.class_name.empty() &&
                        target.shared_loans == 0 && !target.exclusive_loan;
                };
                if (!writable()) throw std::runtime_error("assignment requires an unloaned mutable scalar slot");
                check_value(*stmt.expression, state, functions);
                if (!writable()) throw std::runtime_error("assignment cannot publish through an active loan");
                state.at(stmt.receiver).moved = false;
                return;
            }
            if (stmt.kind == Statement::Kind::Conditional) {
                if (stmt.condition) check_value(*stmt.condition, state, functions);
                else { check_pure_guard(*stmt.condition_lhs, state, functions); check_pure_guard(*stmt.condition_rhs, state, functions); }
                Locals yes = state, no = state;
                scoped_effects(stmt.then_statements, yes); scoped_effects(stmt.else_statements, no);
                if (!same_affine_state(yes, no))
                    throw std::runtime_error("ownership violation: conditional CFG predecessor ownership mismatch");
                state = std::move(yes); return;
            }
            if (stmt.kind == Statement::Kind::Return) {
                const Locals before = state;
                if (stmt.expression) check_value(*stmt.expression, state, functions);
                if (!same_affine_state(before, state))
                    throw std::runtime_error("structured return ownership transfer requires terminal-edge analysis");
                Locals exit;
                for (const auto& name : root_names) exit.emplace(name, state.at(name));
                for (const auto& loan : iteration_loans) if (exit.count(loan.first)) {
                    if (exit.at(loan.first).shared_loans < loan.second) throw std::runtime_error("internal loop loan accounting");
                    exit.at(loan.first).shared_loans -= loan.second;
                }
                structured_exits.push_back(std::move(exit)); return;
            }
            if (stmt.kind == Statement::Kind::ForIndex) {
                check_value(*stmt.branch_local->initializer, state, functions);
                Locals entrance = state;
                auto iterator = scoped_entry(*stmt.branch_local); iterator.mutable_owner = true;
                if (!entrance.emplace(stmt.branch_local->name, iterator).second)
                    throw std::runtime_error("native index loop shadows live owner");
                Locals body = entrance;
                check_value(*stmt.condition, body, functions);
                if (!same_affine_state(entrance, body)) throw std::runtime_error("index condition must preserve ownership");
                scoped_effects(stmt.then_statements, body);
                check_value(*stmt.loop_step, body, functions);
                if (!same_affine_state(entrance, body))
                    throw std::runtime_error("ownership violation: index back-edge must preserve outer owners and loans");
                return;
            }
            if (stmt.kind == Statement::Kind::ForEach) {
                auto& owner = array_owner(*stmt.expression, state, false);
                ++owner.shared_loans;
                const auto& source = *stmt.expression;
                const std::string source_name = native_place(source).first;
                ++iteration_loans[source_name];
                const Locals entrance = state;
                Locals body = entrance;
                const auto& iterator = *stmt.branch_local;
                if (!body.emplace(iterator.name, scoped_entry(iterator)).second)
                    throw std::runtime_error("native for-of element shadows live owner");
                scoped_effects(stmt.then_statements, body);
                body.erase(iterator.name);
                if (!same_affine_state(entrance, body))
                    throw std::runtime_error("ownership violation: for-of back-edge must preserve outer owners and loans");
                // No mutation/move can retire this owner while the read loan is
                // active. Repeated iterations start in the same affine state.
                --owner.shared_loans;
                --iteration_loans[source_name];
                return;
            }
            if (stmt.kind == Statement::Kind::ScopedLocal) {
                const LocalBinding& local = *stmt.branch_local;
                if (state.count(local.name))
                    throw std::runtime_error("ownership violation: branch local shadows live owner at byte " +
                        std::to_string(local.offset));
                check_value(*local.initializer, state, functions);
                state.emplace(local.name, scoped_entry(local));
                return;
            }
            if (stmt.kind == Statement::Kind::ArrayCommitFilled) {
                const auto found = state.find(stmt.receiver);
                if (found == state.end() || found->second.class_name.empty())
                    throw std::runtime_error("ownership violation: filled array commit requires named class owner");
                auto& recv = found->second;
                if (recv.class_array_fields.size() != 4)
                    throw std::runtime_error("ownership violation: filled array commit requires exactly four owned Array<int> fields");
                // This source-reachable intrinsic writes all four array slots.
                // Owning the whole class does not grant access to private slots.
                for (const auto& field : recv.class_array_fields)
                    if (!recv.public_fields.count(field))
                        throw std::runtime_error("ownership violation: private class field '" +
                            recv.class_name + "." + field +
                            "' is not accessible to filled array commit");
                if (recv.mode != Mode::Value || !recv.mutable_owner ||
                        recv.moved || !recv.moved_fields.empty() ||
                        recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: filled array commit requires live exclusive mutable whole class");
                if (conditional_effects)
                    throw std::runtime_error("ownership violation: filled array commit requires straight-line ownership CFG");
                // Evaluate every input as a pure scalar before allocation.
                // Calls, borrows and moves cannot be hidden in length/fills.
                check_pure_guard(*stmt.array_commit_length, state, functions);
                if (stmt.array_commit_values.size() != 4)
                    throw std::runtime_error("invalid compiler-owned filled array commit arity");
                for (const auto& v : stmt.array_commit_values)
                    check_pure_guard(*v, state, functions);
                return;
            }
            if (stmt.kind == Statement::Kind::FieldWrite) {
                auto& recv = checked_field(stmt.receiver, stmt.field, stmt.offset, state);
                if (recv.final_fields.count(stmt.field))
                    throw std::runtime_error("ownership violation: cannot reassign final class field");
                if (recv.class_array_fields.count(stmt.field)) {
                    const Expression& move = *stmt.expression;
                    if (move.kind != Expression::Kind::Take || !move.left ||
                            move.left->kind != Expression::Kind::Variable)
                        reject(move, "ownership violation: Array<int> field requires direct rt take of lexical owner");
                    if (conditional_effects)
                        reject(move, "native Array<int> field move requires straight-line ownership CFG");
                    if (recv.mode != Mode::Value || !recv.mutable_owner ||
                            recv.exclusive_loan || recv.shared_loans != 0)
                        reject(move, "ownership violation: Array<int> destination requires exclusive let mut class");
                    LocalOwnership& src = local(*move.left, state);
                    if (!src.int_array || src.mode != Mode::Value ||
                            !src.mutable_owner || src.moved ||
                            src.exclusive_loan || src.shared_loans != 0)
                        reject(move, "ownership violation: Array<int> field transfer requires live unique lexical source");
                    // The old field is released only during the verified
                    // infallible native handle swap; the source becomes dead.
                    src.moved = true;
                    return;
                }
                if (recv.mode == Mode::Borrow)
                    throw std::runtime_error("ownership violation: cannot mutate through shared class borrow");
                if (recv.mode == Mode::Value && !recv.mutable_owner)
                    throw std::runtime_error("ownership violation: class mutation requires let mut owner");
                if (recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: class field write conflicts with active borrow");
                check_value(*stmt.expression, state, functions);
                recv.moved_fields.erase(stmt.field);
            } else if (stmt.kind == Statement::Kind::Call) {
                const Expression& call = *stmt.expression;
                if (call.kind == Expression::Kind::ArrayCall) {
                    if (!call.left)
                        reject(call, "native Array<int> mutation requires a named owner");
                    array_owner(*call.left, state, true);
                    const std::size_t arity = call.name == "add" ? 1 :
                                              call.name == "set" ? 2 : 0;
                    if (!arity || call.arguments.size() != arity)
                        reject(call, "native Array<int> effect supports only add(value) or set(index,value)");
                    for (const auto& arg : call.arguments) check_value(*arg, state, functions);
                } else if (call.kind == Expression::Kind::Call) {
                    check_call(call, state, functions);
                } else if (call.kind == Expression::Kind::Console) {
                    check_value(call, state, functions);
                } else {
                    throw std::runtime_error("invalid native call statement");
                }
            } else {
                throw std::runtime_error("ownership violation: unsupported conditional arm effect");
            }
        };
        for (const auto& stmt : function.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                if (!structured_exits.empty()) throw std::runtime_error("defer after structured return requires path-specific registration");
                deferred_effects.push_back(&stmt);
                continue;
            }
            if (stmt.kind == Statement::Kind::Conditional) {
                // The bool condition executes once before the branch. Its
                // ownership state is therefore common to both predecessors.
                if (stmt.condition)
                    check_value(*stmt.condition, locals, functions);
                else {
                    check_pure_guard(*stmt.condition_lhs, locals, functions);
                    check_pure_guard(*stmt.condition_rhs, locals, functions);
                }
                Locals then_state = locals;
                Locals else_state = locals;
                scoped_effects(stmt.then_statements, then_state);
                scoped_effects(stmt.else_statements, else_state);
                // Scoped i64 locals have complete lexical lifetime in their
                // own arm; they may not influence the predecessor join or
                // be observable on later exit cleanup paths.
                for (const auto& arm : stmt.then_statements)
                    if (arm.kind == Statement::Kind::ScopedLocal)
                        then_state.erase(arm.branch_local->name);
                for (const auto& arm : stmt.else_statements)
                    if (arm.kind == Statement::Kind::ScopedLocal)
                        else_state.erase(arm.branch_local->name);
                if (!same_affine_state(then_state, else_state))
                    throw std::runtime_error(
                        "ownership violation: conditional CFG predecessor ownership mismatch at byte " +
                        std::to_string(stmt.offset));
                locals = std::move(then_state);
                continue;
            }
            check_effect(stmt, locals);
            if (stmt.kind == Statement::Kind::ScopedLocal)
                root_names.insert(stmt.branch_local->name);
        }
        // Actor sends transfer scalar payloads in lexical send/stop order.
        // Do not let the backend load moved locals or leak raw borrow pointers.
        for (const auto& action : function.actor_actions) {
            if (action.kind == ActorAction::Kind::Send) {
                const auto& send = function.actor_sends.at(action.index);
                check_value(*send.payload, locals, functions);
            }
        }
        auto check_deferred_exit = [&](Locals& path) {
            for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it) {
                const Statement& cleanup = **it;
                if (cleanup.receiver.empty()) {
                    check_call(*cleanup.expression, path, functions);
                    continue;
                }
                auto& recv = checked_field(cleanup.receiver, cleanup.field,
                                           cleanup.offset, path);
                if (recv.final_fields.count(cleanup.field))
                    throw std::runtime_error("ownership violation: cannot reassign final class field");
                if (recv.class_array_fields.count(cleanup.field))
                    throw std::runtime_error("ownership violation: cannot reassign owned Array<int> class field");
                if (recv.mode == Mode::Borrow)
                    throw std::runtime_error("ownership violation: cannot mutate through shared class borrow");
                if (recv.mode == Mode::Value && !recv.mutable_owner)
                    throw std::runtime_error("ownership violation: class mutation requires let mut owner");
                if (recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: class field write conflicts with active borrow");
                if (function.caught_throw_value)
                    check_caught_throw_scalar_read(*cleanup.expression, path, functions);
                else
                    check_value(*cleanup.expression, path, functions);
                recv.moved_fields.erase(cleanup.field);
            }
        };
        if (function.caught_throw_value) {
            // Type-check payload before unwind; each scoped field cleanup
            // is verified at its LIFO execution point.
            check_caught_throw_scalar_read(*function.caught_throw_value, locals, functions);
            for (auto it = function.caught_throw_defers.rbegin();
                 it != function.caught_throw_defers.rend(); ++it) {
                const auto& effect = *it;
                auto& recv = checked_field(effect.receiver, effect.field,
                                           effect.offset, locals);
                if (recv.final_fields.count(effect.field))
                    throw std::runtime_error("ownership violation: cannot reassign final class field");
                if (recv.class_array_fields.count(effect.field))
                    throw std::runtime_error("ownership violation: cannot reassign owned Array<int> class field");
                if (recv.mode == Mode::Borrow || (recv.mode == Mode::Value && !recv.mutable_owner))
                    throw std::runtime_error("ownership violation: caught throw defer requires mutable receiver");
                if (recv.exclusive_loan || recv.shared_loans)
                    throw std::runtime_error("ownership violation: caught throw defer conflicts with active borrow");
                check_caught_throw_scalar_read(*effect.expression, locals, functions);
                recv.moved_fields.erase(effect.field);
            }
            if (!locals.emplace(function.catch_binding, LocalOwnership{}).second)
                throw std::runtime_error("native caught throw binding shadows live local");
        }
        const Locals cfg_entry = locals;
        const bool owning_return = class_arrays.count(function.return_class) &&
            !class_arrays.at(function.return_class).empty();
        // Leaf records snapshot into root storage. Owning aggregates instead
        // move their array owners into a caller-provided region before cleanup.
        std::function<void(const Expression&, Locals&)> check_return;
        check_return = [&](const Expression& expr, Locals& state) {
            if (!function.return_class.empty() && expr.kind == Expression::Kind::Variable) {
                auto& owner = local(expr, state);
                if (owner.class_name != function.return_class || owner.moved || owner.exclusive_loan ||
                        (owner.mode != Mode::Value && (!owner.record_view || owning_return)) || owner.shared_loans ||
                        !owner.moved_fields.empty() || (!owning_return && (!owner.class_array_fields.empty() ||
                        owner.final_fields.size() != owner.class_fields.size())))
                    reject(expr, "record return requires a live transferable owner or immutable leaf without active loans");
                if (owner.mode == Mode::Value) owner.moved = true;
                return;
            }
            if (!function.return_class.empty() && expr.kind == Expression::Kind::RecordConstruct) {
                const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                    [&](const ClassDecl& c) { return c.name == expr.name; });
                if (cls == program.classes.end() || cls->name != function.return_class ||
                        expr.arguments.size() != cls->constructor_parameters.size())
                    reject(expr, "record return type/arity mismatch");
                for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                    const auto* field = constructor_field(*cls, i);
                    const auto& arg = *expr.arguments[i];
                    if (field && !field->class_name.empty()) {
                        const Expression& source = arg.kind == Expression::Kind::Take && arg.left ? *arg.left : arg;
                        if (source.kind != Expression::Kind::Variable) reject(arg, "nested return constructor requires named class owner");
                        auto& owner = local(source, state);
                        if (owner.class_name != field->class_name || owner.mode != Mode::Value || owner.moved ||
                                owner.exclusive_loan || owner.shared_loans || !owner.moved_fields.empty())
                            reject(arg, "nested return constructor requires live unique complete owner");
                        owner.moved = true; continue;
                    }
                    if (!field || !field->int_array) { check_value(arg, state, functions); continue; }
                    const auto& source = arg.kind == Expression::Kind::Take && arg.left ? *arg.left : arg;
                    if (source.kind != Expression::Kind::Variable)
                        reject(arg, "owning return constructor requires a named lexical array");
                    auto& owner = local(source, state);
                    if (!owner.int_array || owner.mode != Mode::Value || owner.moved || owner.shared_loans || owner.exclusive_loan)
                        reject(arg, "owning return constructor requires a live unique array");
                    owner.moved = true;
                }
                return;
            }
            if (!function.return_class.empty() && expr.kind == Expression::Kind::Conditional) {
                if (expr.condition) check_value(*expr.condition, state, functions);
                else { check_value(*expr.condition_lhs, state, functions); check_value(*expr.condition_rhs, state, functions); }
                Locals yes = state, no = state;
                check_return(*expr.when_true, yes); check_return(*expr.when_false, no);
                if (!same_affine_state(yes, no)) reject(expr, "record return ternary ownership mismatch");
                state = std::move(yes);
                return;
            }
            check_value(expr, state, functions);
        };
        // Analyze each taken return independently. Even an explicit scalar
        // rt take in one return branch does not move that value in the
        // fallthrough path, which has a different live ownership state.
        for (const auto& guard : function.early_returns) {
            if (guard.condition)
                check_value(*guard.condition, locals, functions);
            else {
                check_pure_guard(*guard.lhs, locals, functions);
                check_pure_guard(*guard.rhs, locals, functions);
            }
            Locals taken = locals;
            check_return(*guard.result, taken);
            check_deferred_exit(taken);
        }
        if (function.result) {
            if (function.caught_throw_value)
                check_caught_throw_scalar_read(*function.result, locals, functions);
            else
                check_return(*function.result, locals);
        }
        for (auto& exit : structured_exits) check_deferred_exit(exit);
        check_deferred_exit(locals);
        // Build the real guarded-return edge graph and run the reusable
        // field-sensitive fixed-point verifier. This currently models the
        // admitted return-only source CFG. The lattice also handles general
        // diamonds and loops (tested separately), but does not admit new
        // branch-local constructors, exceptional edges or suspension yet.
        ownership::Graph cfg;
        for (const auto& item : cfg_entry) {
            const auto& name = item.first;
            const auto& owner = item.second;
            ownership::PlaceState state;
            state.possible = owner.moved ? ownership::kMoved : ownership::kOwned;
            state.shared_loans = static_cast<unsigned>(owner.shared_loans);
            state.exclusive_loan = owner.exclusive_loan;
            if (owner.class_name.empty()) {
                cfg.entry.emplace(name, state);
            } else {
                // A fieldless nominal object still owns identity/liveness.
                // Track an unspellable proof slot so whole-object reads and
                // moves retain the same CFG checks as nonempty classes.
                if (owner.class_fields.empty())
                    cfg.entry.emplace(name + ".$identity", state);
                for (const auto& field : owner.class_fields) {
                    if (owner.subobjects.count(field)) continue;
                    auto field_state = state;
                    if (owner.moved_fields.count(field))
                        field_state.possible = ownership::kMoved;
                    cfg.entry.emplace(name + "." + field, field_state);
                }
            }
        }
        using OpKind = ownership::Operation::Kind;
        using Ops = std::vector<ownership::Operation>;
        std::function<void(const Expression&, Ops&)> add_effects;
        add_effects = [&](const Expression& expr, Ops& operations) {
            if (expr.kind == Expression::Kind::Variable) {
                const auto found = cfg_entry.find(expr.name);
                const bool object = found != cfg_entry.end() && !found->second.class_name.empty();
                operations.push_back({object ? OpKind::ReadObject : OpKind::Read, expr.name});
                return;
            }
            if (expr.kind == Expression::Kind::ArrayCall) {
                const std::string place = native_place_key(*expr.left);
                operations.push_back({OpKind::Read, place});
                for (const auto& arg : expr.arguments)
                    add_effects(*arg, operations);
                return;
            }
            if (expr.kind == Expression::Kind::Field) {
                if (expr.left && expr.left->kind == Expression::Kind::Field && expr.name == "size") {
                    operations.push_back({OpKind::Read, native_place_key(*expr.left)}); return;
                }
                const auto place = native_place(expr);
                const auto owner = cfg_entry.find(place.first);
                if (owner != cfg_entry.end() && owner->second.int_array) operations.push_back({OpKind::Read, place.first});
                else operations.push_back({owner != cfg_entry.end() && owner->second.subobjects.count(place.second)
                    ? OpKind::ReadObject : OpKind::Read, native_place_key(expr)});
                return;
            }
            if (expr.kind == Expression::Kind::Take && expr.left) {
                if (expr.left->kind == Expression::Kind::Variable) {
                    operations.push_back({OpKind::Move, expr.left->name});
                    return;
                }
                if (expr.left->kind == Expression::Kind::Field) {
                    if (!expr.left->left ||
                            expr.left->left->kind != Expression::Kind::Variable)
                        reject(expr, "native CFG requires named class field take");
                    operations.push_back({OpKind::Move,
                        expr.left->left->name + "." + expr.left->name});
                    return;
                }
            }
            if (expr.left) add_effects(*expr.left, operations);
            if (expr.right) add_effects(*expr.right, operations);
            if (expr.condition) add_effects(*expr.condition, operations);
            if (expr.condition_lhs) add_effects(*expr.condition_lhs, operations);
            if (expr.condition_rhs) add_effects(*expr.condition_rhs, operations);
            if (expr.when_true) add_effects(*expr.when_true, operations);
            if (expr.when_false) add_effects(*expr.when_false, operations);
            for (const auto& argument : expr.arguments)
                add_effects(*argument, operations);
        };
        auto add_cleanup_effects = [&](const Statement& cleanup, Ops& operations) {
            if (!cleanup.receiver.empty())
                operations.push_back({OpKind::Read, cleanup.receiver + "." + cleanup.field});
            add_effects(*cleanup.expression, operations);
        };
        auto add_return_effects = [&](const Expression& expr, Ops& operations) {
            if (!function.return_class.empty() && expr.kind == Expression::Kind::Variable &&
                    cfg_entry.at(expr.name).mode == Mode::Value)
                operations.push_back({OpKind::MoveObject, expr.name});
            else if (!function.return_class.empty() && expr.kind == Expression::Kind::RecordConstruct) {
                const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                    [&](const ClassDecl& c) { return c.name == expr.name; });
                for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                    const auto* field = constructor_field(*cls, i);
                    const auto& arg = *expr.arguments[i];
                    if (field && !field->class_name.empty()) {
                        const auto& source = arg.kind == Expression::Kind::Take ? *arg.left : arg;
                        operations.push_back({OpKind::MoveObject, source.name});
                    } else
                    if (field && field->int_array) {
                        const auto& source = arg.kind == Expression::Kind::Take ? *arg.left : arg;
                        operations.push_back({OpKind::Move, source.name});
                    } else add_effects(arg, operations);
                }
            } else add_effects(expr, operations);
        };
        const std::size_t guard_count = function.early_returns.size();
        cfg.blocks.resize(guard_count * 2 + 1);
        // Record returns can move an owner in either lazy arm. Preserve those
        // terminal edges instead of flattening both arms into sequential reads.
        std::function<void(const Expression*, std::size_t)> append_return_path;
        append_return_path = [&](const Expression* expr, std::size_t block) {
            if (expr && !function.return_class.empty() && expr->kind == Expression::Kind::Conditional) {
                if (expr->condition) add_effects(*expr->condition, cfg.blocks[block].operations);
                else {
                    add_effects(*expr->condition_lhs, cfg.blocks[block].operations);
                    add_effects(*expr->condition_rhs, cfg.blocks[block].operations);
                }
                const std::size_t yes = cfg.blocks.size(), no = yes + 1;
                cfg.blocks.resize(no + 1);
                cfg.blocks[block].successors = {yes, no};
                append_return_path(expr->when_true.get(), yes);
                append_return_path(expr->when_false.get(), no);
                return;
            }
            if (expr) add_return_effects(*expr, cfg.blocks[block].operations);
            for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it)
                add_cleanup_effects(**it, cfg.blocks[block].operations);
        };
        for (std::size_t i = 0; i < guard_count; ++i) {
            const auto& guard = function.early_returns[i];
            auto& condition = cfg.blocks[2 * i];
            if (guard.condition) {
                add_effects(*guard.condition, condition.operations);
            } else {
                add_effects(*guard.lhs, condition.operations);
                add_effects(*guard.rhs, condition.operations);
            }
            condition.successors = {2 * i + 1, 2 * i + 2};
            append_return_path(guard.result.get(), 2 * i + 1);
        }
        append_return_path(function.result.get(), guard_count * 2);
        // Every reachable terminal CFG path must end with an explicit
        // conditional drop of the experimental heap-owned places. The
        // emitter backs each DropIfOwned with a runtime live flag; scalar
        // stack locals and trivially destructible class fields need none.
        for (auto& block : cfg.blocks)
            if (block.successors.empty())
                for (const auto& entry : cfg_entry)
                    if (entry.second.heap_owned)
                        block.operations.push_back({OpKind::DropIfOwned, entry.first});
        ownership::verify(cfg);

    }

    // Publication is compilation-unit atomic. First validate every ownership
    // admission without mutating any pipeline authority. Only after all chains
    // succeed do we mark them executable for LLVM lowering.
    for (const auto& function : program.functions) {
        for (const auto& pipeline : function.future_pipelines) {
            std::vector<types::FuturePipelineOwnershipAdmission> admissions;
            admissions.reserve(pipeline.plans.size());
            for (const auto& plan : pipeline.plans) {
                types::FuturePipelineOwnershipAdmission admission;
                admission.declaration_identity = plan.declaration_identity;
                admission.pipeline_node_identity = plan.pipeline_node_identity;
                admission.input_ownership = plan.input_ownership;
                admission.admitted = true;
                admissions.push_back(admission);
            }
            if (!types::publish_future_pipeline_chain(
                    pipeline.plans, admissions).has_value())
                throw std::runtime_error(
                    "ownership violation: Future pipeline proof publication "
                    "failed closed");
        }
    }
    for (auto& function : program.functions)
        for (auto& pipeline : function.future_pipelines)
            pipeline.ownership_admitted = true;
}

} // namespace oreslang
