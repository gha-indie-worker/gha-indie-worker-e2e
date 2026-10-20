#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "oreslang/types/FunctionEffects.hpp"

namespace oreslang {

// Deliberately restricted, typed-i64 expression AST. Not the full Java Ast.Program.
struct GenericTypeArgument {
    std::string name; // empty for positional arguments
    std::string type; // native stage 1 admits only int
    bool infer = false;
};
struct GenericParameter {
    std::string name;
    std::string default_type; // empty when required, int or earlier slot otherwise
};

struct Expression {
    enum class Kind { Integer, Variable, Negate, Add, Subtract, Multiply, Call, ArrayCall, Copy, Take, Share, Borrow, BorrowMut, Field };
    Kind kind;
    std::size_t offset = 0;
    std::int64_t integer = 0;
    std::string name;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    std::vector<std::unique_ptr<Expression>> arguments;
    std::vector<std::string> argument_names; // indexed in lexical source order
    std::vector<GenericTypeArgument> type_arguments;
    bool type_arguments_present = false;
};

struct Parameter {
    enum class Mode { Value, Borrow, BorrowMut };
    std::string name;
    std::string class_name; // empty means primitive i64
    std::string generic_type; // stage-one i64 generic slot
    Mode mode = Mode::Value;
};

// Immutable, lexical i64 slot with initializer-before-publication semantics.
struct LocalBinding {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool heap_owned = false;
    bool int_array = false; // non-Copy owned Array<int> local, no escape
    std::string class_name; // only stack-constructed, bounded multi-field i64 class
    bool new_class = false;
    // Pure scalar constructor arguments, evaluated once at call site.
    std::vector<std::unique_ptr<Expression>> constructor_arguments;
    // Restricted local stack-only explicit class move.
    bool move_class = false;
    std::string move_source;
    bool owner_mutable = false; // explicit let mut; required for exclusive class loan
    bool field_mutable = false; // actor-owned fields only
    std::size_t offset = 0;
};

struct ClassField {
    std::string name;
    std::int64_t initial = 0;
    std::unique_ptr<Expression> initializer_expression;
    bool public_member = false;
    bool int_array = false; // owned opaque handle; never exposed as an int
    bool final_field = false; // fixed field slot: writes rejected by ownership checker
    bool mutable_referent = false; // val aliases final mut; independent of slot fixedness
};

struct ClassDecl {
    std::string name;
    // Up to 16 scalar i64 or owned Array<int> fields.
    // The ordered vector defines the native LLVM struct layout.
    std::vector<ClassField> fields;
    bool explicit_constructor = false;
    std::vector<std::string> constructor_parameters;
};

// Native source value categories. Future<T> remains a no-IR boundary
// until the compiler can lower nonblocking pinned continuations.
struct SourceType {
    enum class Kind { I64, ActorRef, Future };
    Kind kind = Kind::I64;
    std::string argument;
};

struct ActorSpawnBinding {
    std::string name;
    std::string actor_type;
    SourceType declared_type;
    std::size_t offset = 0;
};
struct ActorSend {
    std::string reference;
    std::unique_ptr<Expression> payload;
    std::size_t offset = 0;
};
// Preserve the sequence of source-visible actor effects. Separating sends
// from stops changes program behavior and can hide use-after-stop errors.
struct ActorAction {
    enum class Kind { Send, Stop };
    Kind kind;
    std::size_t index = 0;
};

struct ActorFieldWrite {
    std::string field;
    std::unique_ptr<Expression> expression;
    std::size_t offset = 0;
};

struct Statement {
    enum class Kind { Call, FieldWrite, Defer, Conditional, ScopedLocal, ArrayCommitFilled };
    Kind kind;
    // ScopedLocal is a trivially-owned i64 local limited to one if/else arm.
    // Non-Copy branch-local classes are still rejected until drop glue exists.
    std::unique_ptr<LocalBinding> branch_local;
    std::unique_ptr<Expression> expression;
    std::string receiver;
    std::string field;
    // Dedicated compiler intrinsic, never lowered as a guest-callable
    // function: rt array_commit_filled(class_owner, length, v0, v1, v2, v3).
    // Exactly four Array<int> fields in class declaration order.
    std::unique_ptr<Expression> array_commit_length;
    std::vector<std::unique_ptr<Expression>> array_commit_values;
    std::size_t offset = 0;
    // A bounded synchronous if/else statement. Arms contain checked field
    // writes or direct calls; local declarations and suspension fail closed.
    std::unique_ptr<Expression> condition_lhs;
    std::unique_ptr<Expression> condition_rhs;
    std::vector<Statement> then_statements;
    std::vector<Statement> else_statements;
};

// Side-effect-free equality guard with return-only taken branch.
// All locals and ordinary statements precede guards; branch-local objects
// and suspension remain unsupported until path-sensitive cleanup is proven.
struct GuardedReturn {
    std::unique_ptr<Expression> lhs;
    std::unique_ptr<Expression> rhs;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
};

// Experimental LLVM AOT-only, single-owner scalar loop frame. The grammar
// admits only one val i64 counter, a constant upper bound and a +1 body.
// No borrowed pointer, object owner, dynamic call or cleanup crosses a yield.
struct ScalarWhileLoop {
    std::string counter;
    std::int64_t initial = 0;
    std::int64_t upper_bound = 0;
};

struct Function {
    std::string name;
    std::vector<GenericParameter> generic_parameters;
    std::unique_ptr<ScalarWhileLoop> scalar_while;
    std::vector<Parameter> parameters;
    std::vector<LocalBinding> locals;
    std::vector<Statement> statements;
    std::vector<GuardedReturn> early_returns;
    std::vector<ActorSpawnBinding> actor_spawns;
    std::vector<ActorSend> actor_sends;
    std::vector<std::string> actor_stops;
    std::vector<ActorAction> actor_actions; // source order of send/stop
    std::vector<ActorFieldWrite> actor_field_writes;
    bool returns_void = false;
    // Single-frame native i64 throw; no nested exceptions or suspension.
    std::unique_ptr<Expression> caught_throw_value;
    std::string catch_binding;
    std::vector<Statement> caught_throw_defers;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
    FunctionEffects declared_effects{};
    bool has_declared_effects = false;
};

struct ActorDecl {
    std::string name;
    std::vector<LocalBinding> fields; // bounded, actor-confined integer slots
    Function on_message; // exactly one synchronous pub on_message(int): int
    std::size_t offset = 0;
};

// First AOT-only resumable source slice. No owned borrows, calls, FFI,
// lexical cleanup or actor scheduler integration is admitted yet.
// The host supplies a zeroed, single-owner two-i64 continuation frame.
struct CooperativeScalarLoop {
    std::string name;
    std::string variable;
    std::int64_t initial = 0;
    std::size_t offset = 0;
};

struct Program {
    std::vector<CooperativeScalarLoop> cooperative_loops;
    std::string module;
    std::vector<ClassDecl> classes;
    std::vector<ActorDecl> actors;
    std::vector<Function> functions;
};

// Parse a constrained native subset: i64 expressions, call-scoped borrows and
// bounded multi-field i64 classes and trusted, synchronous actor send/stop.
// Actors remain incompatible with wasm32 and source-level await/suspension.
Program parse_program(std::string_view source, bool experimental_owned_int = false,
                      bool experimental_rt_scalar_loop = false);

} // namespace oreslang
