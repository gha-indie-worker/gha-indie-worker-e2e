#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/NativeClassLayout.hpp"
#include "oreslang/types/OwnershipChecker.hpp"
#include "oreslang/frontend/MethodResolver.hpp"
#include "oreslang/types/NativeScalarTypes.hpp"
#include "oreslang/types/NativeSumTypes.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <functional>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {

enum class Kind { Identifier, Integer, String, Template, Symbol, End };
struct Token {
    Kind kind;
    std::string_view text;
    std::size_t offset;
};

bool exceptional_control(std::string_view word) {
    return word == "throw" || word == "raise" || word == "panic" ||
           word == "recover" || word == "defer" || word == "try" ||
           word == "catch" || word == "finally" || word == "trap";
}

bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
bool identifier_part(char ch) { return identifier_start(ch) || digit(ch); }

inline constexpr std::size_t kMaxNativeTemplateNesting = 32;
inline constexpr std::size_t kMaxNativeTemplateBraceNesting = 256;
inline constexpr std::size_t kMaxNativeTemplateInterpolations = 1024;

[[noreturn]] void template_error(std::string_view detail, std::size_t offset) {
    throw std::runtime_error("native template " + std::string(detail) +
                             " at byte " + std::to_string(offset));
}

std::size_t skip_template_quoted(std::string_view source, std::size_t at,
                                 char quote, std::size_t origin) {
    while (at < source.size()) {
        const char ch = source[at++];
        if (ch == '\\') {
            if (at == source.size()) template_error("has unterminated quoted escape", origin);
            ++at;
        } else if (ch == quote) {
            return at;
        }
    }
    template_error("has unterminated quoted string", origin);
}

std::size_t skip_template_block_comment(std::string_view source, std::size_t at,
                                        std::size_t origin) {
    std::size_t depth = 1;
    while (at < source.size()) {
        if (source.substr(at, 2) == "/*") {
            ++depth;
            at += 2;
        } else if (source.substr(at, 2) == "*/") {
            at += 2;
            if (--depth == 0) return at;
        } else {
            ++at;
        }
    }
    template_error("has unterminated block comment", origin);
}

std::size_t template_close(std::string_view source, std::size_t from,
                           std::size_t nesting, std::size_t origin);

std::size_t template_interpolation_close(std::string_view source, std::size_t from,
                                         std::size_t nesting, std::size_t origin) {
    if (nesting > kMaxNativeTemplateNesting)
        template_error("interpolation nesting limit exceeded (32)", origin);
    std::size_t braces = 1;
    std::size_t at = from;
    while (at < source.size()) {
        const char ch = source[at];
        if (ch == '"' || ch == '\'') {
            at = skip_template_quoted(source, at + 1, ch, origin);
        } else if (static_cast<unsigned char>(ch) == 0x60) {
            at = template_close(source, at + 1, nesting + 1, origin) + 1;
        } else if (source.substr(at, 2) == "//") {
            at += 2;
            while (at < source.size() && source[at] != '\n') ++at;
        } else if (source.substr(at, 2) == "/*") {
            at = skip_template_block_comment(source, at + 2, origin);
        } else if (ch == '{') {
            if (++braces > kMaxNativeTemplateBraceNesting)
                template_error("interpolation brace nesting limit exceeded (256)", origin);
            ++at;
        } else if (ch == '}') {
            if (--braces == 0) return at;
            ++at;
        } else {
            ++at;
        }
    }
    template_error("has unterminated interpolation", origin);
}

std::size_t template_close(std::string_view source, std::size_t from,
                           std::size_t nesting, std::size_t origin) {
    if (nesting > kMaxNativeTemplateNesting)
        template_error("interpolation nesting limit exceeded (32)", origin);
    std::size_t at = from;
    while (at < source.size()) {
        const char ch = source[at];
        if (ch == '\\') {
            if (at + 1 >= source.size()) template_error("has unterminated escape", origin);
            at += 2;
        } else if (static_cast<unsigned char>(ch) == 0x24 &&
                   at + 1 < source.size() && source[at + 1] == '{') {
            at = template_interpolation_close(source, at + 2, nesting + 1, origin) + 1;
        } else if (static_cast<unsigned char>(ch) == 0x60) {
            return at;
        } else {
            ++at;
        }
    }
    template_error("literal is unterminated", origin);
}

bool reserved(std::string_view name) {
    // Keep these reserved even while their grammar is unsupported.
    static constexpr std::string_view words[] = {
        "define", "class", "enum", "pattern", "module", "contract", "conforms", "namespace",
        "import", "from", "as", "extends", "implements", "try", "catch",
        "throw", "raise", "panic", "recover",
        "finally", "end", "fi", "if", "do", "else", "then", "new",
        "spawn", "stop", "done", "await", "rt", "async", "suspends", "suspend", "critical", "generator",
        "nlex", "trap", "actor", "isoactor", "def", "fnc", "routine",
        "while", "for", "of", "loop", "block", "break", "continue", "yield",
        "super", "elseif", "elif", "switch", "match", "matches", "is",
        "default", "eq", "neq", "nb", "select", "readch", "writech",
        "cb", "shared", "untrusted", "type", "types", "typeof",
        "interface", "trait", "struct", "impl", "abstract", "void",
        "static", "pub", "private", "return", "defer", "throw", "raise",
        "panic", "recover", "val", "const",
        "let", "mut", "borrow", "self", "true", "false", "null", "obj", "arr",
        "gpu", "cpu", "quantum", "over", "structural", "int", "bool", "pure",
        "Some", "None", "Ok", "Err", "using", "requires", "serial", "critical-read", "critical-write", "nb-select"
    };
    return std::find(std::begin(words), std::end(words), name) != std::end(words);
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        for (;;) {
            if (at_ == source_.size()) return {Kind::End, {}, at_};
            const unsigned char ch = static_cast<unsigned char>(source_[at_]);
            if (std::isspace(ch)) { ++at_; continue; }
            if (source_.substr(at_, 2) == "//") {
                const auto end = source_.find('\n', at_ + 2);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "/*") {
                const auto start = at_;
                at_ += 2;
                std::size_t depth = 1;
                while (depth != 0) {
                    if (at_ == source_.size()) {
                        throw std::runtime_error("unterminated block comment at byte "
                                                 + std::to_string(start));
                    }
                    if (source_.substr(at_, 2) == "/*") { at_ += 2; ++depth; }
                    else if (source_.substr(at_, 2) == "*/") { at_ += 2; --depth; }
                    else ++at_;
                }
                continue;
            }
            break;
        }
        const std::size_t start = at_;
        const char ch = source_[at_];
        if (identifier_start(ch)) {
            ++at_;
            while (at_ < source_.size() && identifier_part(source_[at_])) ++at_;
            // Keep 'critical-read' / 'critical-write' as ordinary
            // IDENT '-' IDENT tokens. The parser recognizes them *only*
            // in callable modifier positions: arbitrary expressions must
            // never lose subtraction because of declaration syntax.
            // Existing nb-select recognition is intentionally unchanged
            // until native select lowering supports that grammar.
            const auto first = source_.substr(start, at_ - start);
            if (first == "nb" && source_.substr(at_, 7) == "-select") {
                const auto end = at_ + 7;
                if (end == source_.size() ||
                        (source_[end] != '-' && !identifier_part(source_[end]))) {
                    at_ = end;
                }
            }
            return {Kind::Identifier, source_.substr(start, at_ - start), start};
        }
        if (ch == '"' || ch == '\'') {
            const char quote = ch;
            ++at_;
            const auto content = at_;
            while (at_ < source_.size() && source_[at_] != quote) {
                if (source_[at_] == '\\') {
                    ++at_;
                    if (at_ == source_.size()) break;
                }
                ++at_;
            }
            if (at_ == source_.size()) throw std::runtime_error("unterminated native string at byte " + std::to_string(start));
            const auto text = source_.substr(content, at_ - content);
            ++at_;
            return {Kind::String, text, start};
        }
        if (static_cast<unsigned char>(ch) == 0x60) {
            const auto close = template_close(source_, at_ + 1, 0, start);
            const auto text = source_.substr(at_ + 1, close - at_ - 1);
            at_ = close + 1;
            return {Kind::Template, text, start};
        }
        if (digit(ch)) {
            ++at_;
            while (at_ < source_.size() && (digit(source_[at_]) || source_[at_] == '_')) {
                ++at_;
            }
            return {Kind::Integer, source_.substr(start, at_ - start), start};
        }
        // Contiguous comparison/logical operators are single tokens, matching
        // the Java/Graal precedence grammar.
        for (std::string_view op : {"->", "::", "|>", "==", "!=", "<=", ">=", "&&", "||", "^^"}) {
            if (source_.substr(at_, 2) == op) {
                at_ += 2;
                return {Kind::Symbol, source_.substr(start, 2), start};
            }
        }
        if (ch == '@' && source_.substr(start, 15) == "@DefaultArgVals") {
            throw std::runtime_error("native @DefaultArgVals declaration requires "
                                     "checked named defaults, ownership and ABI lowering at byte "
                                     + std::to_string(start));
        }
        // Admit '@' as a TOKEN only so existing parameter-local @Default guards
        // can issue their specific fail-closed diagnostic instead of a generic
        // unsupported-token error. No annotation execution is enabled.
        if (std::string_view("(){},:;+-*/%=.<>@?!").find(ch) != std::string_view::npos) {
            ++at_;
            return {Kind::Symbol, source_.substr(start, 1), start};
        }
        throw std::runtime_error("unsupported Oreslang token at byte "
                                 + std::to_string(start));
    }

private:
    std::string_view source_;
    std::size_t at_ = 0;
};

std::unique_ptr<Expression> make_expression(Expression::Kind kind,
                                             std::size_t offset) {
    auto result = std::make_unique<Expression>();
    result->kind = kind;
    result->offset = offset;
    return result;
}

inline constexpr std::size_t kMaxNativeExpressionDepth = 64;

inline void require_expression_depth(std::size_t depth, std::size_t offset) {
    if (depth > kMaxNativeExpressionDepth) {
        throw std::runtime_error(
            "native expression nesting limit exceeded (64) at byte " +
            std::to_string(offset));
    }
}

// The first native terminal-try slice has no exceptional CFG. Restrict
// protected return/guard expressions to primitive values or an addressable,
// ownership-checked class field read: no nested calls or ownership transfers.
bool nonthrowing_protected_read(const Expression& expr) {
    if (expr.kind == Expression::Kind::Integer ||
            expr.kind == Expression::Kind::Boolean ||
            expr.kind == Expression::Kind::Variable) return true;
    return expr.kind == Expression::Kind::Field && expr.left &&
           expr.left->kind == Expression::Kind::Variable;
}

// Arithmetic over checked scalar reads is nonthrowing. Never admit
// general calls, array mutations, ownership transfers, or suspension.
bool native_pure_scalar_tree(const Expression& expr, std::size_t depth = 1) {
    require_expression_depth(depth, expr.offset);
    if (nonthrowing_protected_read(expr)) return true;
    if (expr.kind == Expression::Kind::Negate ||
            expr.kind == Expression::Kind::LogicalNot)
        return expr.left && native_pure_scalar_tree(*expr.left, depth + 1);
    if (expr.kind == Expression::Kind::Add ||
            expr.kind == Expression::Kind::Subtract ||
            expr.kind == Expression::Kind::Multiply ||
            expr.kind == Expression::Kind::Equal ||
            expr.kind == Expression::Kind::NotEqual ||
            expr.kind == Expression::Kind::Less ||
            expr.kind == Expression::Kind::LessEqual ||
            expr.kind == Expression::Kind::Greater ||
            expr.kind == Expression::Kind::GreaterEqual ||
            expr.kind == Expression::Kind::LogicalAnd ||
            expr.kind == Expression::Kind::LogicalOr ||
            expr.kind == Expression::Kind::LogicalXor)
        return expr.left && expr.right &&
            native_pure_scalar_tree(*expr.left, depth + 1) &&
            native_pure_scalar_tree(*expr.right, depth + 1);
    if (expr.kind == Expression::Kind::Conditional)
        return (expr.condition ||
                (expr.condition_lhs && expr.condition_rhs)) &&
            expr.when_true && expr.when_false &&
            (!expr.condition || native_pure_scalar_tree(*expr.condition, depth + 1)) &&
            (!expr.condition_lhs || native_pure_scalar_tree(*expr.condition_lhs, depth + 1)) &&
            (!expr.condition_rhs || native_pure_scalar_tree(*expr.condition_rhs, depth + 1)) &&
            native_pure_scalar_tree(*expr.when_true, depth + 1) &&
            native_pure_scalar_tree(*expr.when_false, depth + 1);
    return false;
}

bool native_capture_free_task_tree(
        const Expression& expr, std::size_t depth = 1) {
    require_expression_depth(depth, expr.offset);
    using K = Expression::Kind;
    if (expr.kind == K::Integer || expr.kind == K::Boolean) return true;
    if ((expr.kind == K::Negate || expr.kind == K::LogicalNot) && expr.left)
        return native_capture_free_task_tree(*expr.left, depth + 1);
    if ((expr.kind == K::Add || expr.kind == K::Subtract ||
            expr.kind == K::Multiply || expr.kind == K::Equal ||
            expr.kind == K::NotEqual || expr.kind == K::Less ||
            expr.kind == K::LessEqual || expr.kind == K::Greater ||
            expr.kind == K::GreaterEqual || expr.kind == K::LogicalAnd ||
            expr.kind == K::LogicalOr || expr.kind == K::LogicalXor) &&
            expr.left && expr.right)
        return native_capture_free_task_tree(*expr.left, depth + 1) &&
            native_capture_free_task_tree(*expr.right, depth + 1);
    if (expr.kind == K::Conditional && expr.when_true && expr.when_false &&
            (expr.condition || (expr.condition_lhs && expr.condition_rhs)))
        return (!expr.condition ||
                    native_capture_free_task_tree(*expr.condition, depth + 1)) &&
            (!expr.condition_lhs ||
                    native_capture_free_task_tree(*expr.condition_lhs, depth + 1)) &&
            (!expr.condition_rhs ||
                    native_capture_free_task_tree(*expr.condition_rhs, depth + 1)) &&
            native_capture_free_task_tree(*expr.when_true, depth + 1) &&
            native_capture_free_task_tree(*expr.when_false, depth + 1);
    return false;
}

struct NativeCallSignature {
    std::vector<std::string> parameters;
    std::vector<GenericParameter> generics;
    std::vector<std::string> parameter_generic_types;
    std::vector<ScalarType> parameter_scalar_types;
    std::vector<bool> parameter_is_scalar;
    ScalarType return_type = ScalarType::I64;
    bool returns_void = false;
    bool async_source_i64 = false;
};
using NativeSignatures = std::unordered_map<
    std::string, std::unordered_map<std::size_t, NativeCallSignature>>;

// This proof is deliberately narrower than the general native scalar guard:
// an unpublished constructor can read only integer literals and its own
// explicitly typed scalar parameters, never arbitrary fields or operations.
bool native_constructor_scalar_tree(const Expression& expr,
                                    std::size_t depth = 1) {
    require_expression_depth(depth, expr.offset);
    using Kind = Expression::Kind;
    if (expr.kind == Kind::Integer || expr.kind == Kind::Boolean || expr.kind == Kind::String ||
            expr.kind == Kind::Variable) return true;
    if (expr.kind == Kind::SumConstruct)
        return std::all_of(expr.arguments.begin(), expr.arguments.end(), [&](const auto& arg) {
            return native_constructor_scalar_tree(*arg, depth + 1);
        });
    if (expr.kind == Kind::Field)
        return expr.left && expr.left->kind == Kind::Variable;
    if (expr.kind == Kind::Negate || expr.kind == Kind::LogicalNot)
        return expr.left && native_constructor_scalar_tree(*expr.left, depth + 1);
    if (expr.kind == Kind::Add || expr.kind == Kind::Subtract ||
            expr.kind == Kind::Multiply || expr.kind == Kind::Equal ||
            expr.kind == Kind::NotEqual || expr.kind == Kind::Less ||
            expr.kind == Kind::LessEqual || expr.kind == Kind::Greater ||
            expr.kind == Kind::GreaterEqual || expr.kind == Kind::LogicalAnd ||
            expr.kind == Kind::LogicalOr || expr.kind == Kind::LogicalXor)
        return expr.left && expr.right &&
            native_constructor_scalar_tree(*expr.left, depth + 1) &&
            native_constructor_scalar_tree(*expr.right, depth + 1);
    if (expr.kind == Kind::Conditional)
        return (expr.condition || (expr.condition_lhs && expr.condition_rhs)) &&
            expr.when_true && expr.when_false &&
            (!expr.condition || native_constructor_scalar_tree(*expr.condition, depth + 1)) &&
            (!expr.condition_lhs || native_constructor_scalar_tree(*expr.condition_lhs, depth + 1)) &&
            (!expr.condition_rhs || native_constructor_scalar_tree(*expr.condition_rhs, depth + 1)) &&
            native_constructor_scalar_tree(*expr.when_true, depth + 1) &&
            native_constructor_scalar_tree(*expr.when_false, depth + 1);
    return false;
}

// A runtime callable's code/signature must never be forged as a nominal
// object or actor identity in the constrained native frontend.
bool callable_nominal_name(std::string_view name) {
    return name == "Callable" || name == "Fnc" || name == "Function"
            || name == "Routine" || name == "Method";
}

// Entry is a compiler-owned nominal product in the Java/Graal frontend.
// Until the native backend has a real two-slot representation, projection
// ownership, value equality, transport traits and ABI tests, it must not be
// forgeable as an ordinary user class/actor nominal.
bool entry_builtin_name(std::string_view name) {
    return name == "Entry";
}

[[noreturn]] void reject_native_entry_boundary(std::size_t offset) {
    throw std::runtime_error(
        "native Entry<K,V> requires nominal product lowering and ownership parity"
        " at byte " + std::to_string(offset));
}

class Parser {
public:
    explicit Parser(std::string_view source, bool experimental_owned_int,
                    bool experimental_rt_scalar_loop)
        : source_(source), lexer_(source), token_(lexer_.next()),
          experimental_owned_int_(experimental_owned_int),
          experimental_rt_scalar_loop_(experimental_rt_scalar_loop) {}

    Program parse() {
        Program program;
        while (token_.text == "import") {
            advance();
            if (token_.text == "contract")
                fail("native contract imports require a validated cross-unit linker");
            ModuleImport imported;
            imported.offset = token_.offset;
            imported.selected_class = accept("class");
            if (!imported.selected_class) require("module");
            imported.selected_module = identifier();
            imported.local_name = accept("as") ? identifier() : imported.selected_module;
            if (!module_names_.insert(imported.local_name).second)
                fail("duplicate imported module binding '" + imported.local_name + "'");
            require("from");
            if (token_.kind != Kind::String) fail("native module import requires a quoted source path");
            imported.path = std::string(token_.text);
            if (imported.path.empty() || imported.path.size() > 4096)
                fail("native import path must contain 1..4096 bytes");
            for (unsigned char ch : imported.path)
                if (ch < 32 || ch == 127 || ch == '\\')
                    fail("native import path escapes/control characters unsupported");
            advance();
            require(";");
            program.imports.push_back(std::move(imported));
        }
        if (accept("namespace")) {
            program.namespace_name = identifier();
            require(";");
        }
        bool wrapped_module = false;
        if (token_.text == "import")
            fail("native contract imports require a validated cross-unit linker");
        while (token_.text == "define" || token_.text == "declare" ||
                (token_.text == "pub" &&
                 (peek_next().text == "define" || peek_next().text == "declare"))) {
            if (at_type_only_introducer()) {
                type_only_declaration(program);
                continue;
            }
            bool exported = accept("pub");
            require("define");
            if (accept("pub")) {
                if (exported) fail("duplicate pub modifier");
                exported = true;
            }
            if (token_.text == "mtd") {
                program.functions.push_back(extension_method(exported));
                continue;
            }
            if (token_.text == "type" || token_.text == "trait" ||
                    token_.text == "interface") {
                type_only_declaration(program, true, exported);
                continue;
            }
            if (token_.text == "model")
                fail("MODEL_BACKEND_UNSUPPORTED: native model graph requires verified operation provenance, configuration and codegen parity");
            if (accept("enum")) {
                if (token_.text != "MaxActorThreads")
                    fail("native user-defined tagged enum requires ADT layout and constructor lowering; expected 'MaxActorThreads'");
                if (exported) fail("built-in MaxActorThreads enum cannot be public in the native subset");
                built_in_actor_threads_enum();
                continue;
            }
            if (accept("contract")) {
                program.contracts.push_back(contract());
                continue;
            }
            if (exported && token_.text != "class") fail("native public module wrapper is not supported in the i64 subset");
            if (accept("serial")) {
                require("actor");
                fail("native serial actor requires shared HTTP/message lane lowering");
            }
            if (accept("module")) {
                if (!defined_type_aliases_.empty() || !forwards_.empty())
                    fail("native module wrapper cannot import file-root forward aliases");
                program.module = identifier();
                if (accept("conforms")) {
                    do program.conforms.push_back(identifier()); while (accept(","));
                }
                accept("as");
                wrapped_module = true;
            } else {
                if (accept("class")) { auto cls = class_declaration(); cls.exported = exported; program.classes.push_back(std::move(cls)); }
                else if (accept("actor")) program.actors.push_back(actor_declaration());
                else if (accept_transform_pipe_modifier()) {
                    require("actor");
                    fail("native transform-pipe actor requires typed pipeline role and transport lowering");
                } else fail("expected 'module', 'class' or 'actor'");
            }
            break;
        }
        while (token_.kind != Kind::End
                && !(wrapped_module && token_.text == "end")) {
            if (token_.text == "import")
                fail("native contract imports require a validated cross-unit linker");
            if (at_type_only_introducer()) {
                type_only_declaration(program);
                continue;
            }
            bool public_class = token_.text == "pub" && peek_next().text == "define";
            if (public_class) advance();
            if (accept("@")) {
                const MaxActorThreads cap = actor_max_threads_attribute();
                require("define");
                require("actor");
                auto actor = actor_declaration();
                actor.max_actor_threads = cap;
                program.actors.push_back(std::move(actor));
            } else if (accept("define")) {
                if (accept("pub")) {
                    if (public_class) fail("duplicate pub modifier");
                    public_class = true;
                }
                if (token_.text == "mtd") {
                    program.functions.push_back(extension_method(public_class));
                    continue;
                }
                if (token_.text == "type" || token_.text == "trait" ||
                        token_.text == "interface") {
                    type_only_declaration(program, true, public_class);
                    continue;
                }
                if (token_.text == "model")
                    fail("MODEL_BACKEND_UNSUPPORTED: native model graph requires verified operation provenance, configuration and codegen parity");
                if (public_class && token_.text != "class") fail("native pub define currently supports only class declarations");
                if (accept("serial")) {
                    require("actor");
                    fail("native serial actor requires shared HTTP/message lane lowering");
                }
                if (accept("enum")) {
                    if (token_.text != "MaxActorThreads")
                        fail("native user-defined tagged enum requires ADT layout and constructor lowering; expected 'MaxActorThreads'");
                    built_in_actor_threads_enum();
                }
                else if (accept("class")) { auto cls = class_declaration(); cls.exported = public_class; program.classes.push_back(std::move(cls)); }
                else if (accept("actor")) program.actors.push_back(actor_declaration());
                else if (accept_transform_pipe_modifier()) {
                    require("actor");
                    fail("native transform-pipe actor requires typed pipeline role and transport lowering");
                } else fail("expected 'class', 'actor', or built-in MaxActorThreads enum");
            } else if (token_.text == "suspend") {
                program.cooperative_loops.push_back(cooperative_scalar_loop());
            } else {
                program.functions.push_back(function());
            }
        }
        for (auto& method : methods_) program.functions.push_back(std::move(method));
        if (wrapped_module) require("end");
        if (token_.kind != Kind::End) fail("unexpected trailing source");
        validate_type_only_declarations(program);
        if (!program.namespace_name.empty() &&
                (!program.functions.empty() || !program.classes.empty() ||
                 !program.actors.empty() || !program.contracts.empty() ||
                 !program.cooperative_loops.empty())) {
            fail("NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED: native namespace v1 is metadata-only");
        }
        compile_type_constraint_descriptors(program);
        lower_extension_calls(program);
        resolve_alias_uses(program);
        if (program.functions.empty() && program.actors.empty() && program.classes.empty() &&
                program.cooperative_loops.empty() && program.type_only_declarations.empty())
            fail("expected at least one function, actor or type-only declaration");
        if (!program.cooperative_loops.empty() &&
                (program.cooperative_loops.size() != 1 || !program.functions.empty() ||
                 !program.actors.empty() || !program.classes.empty() || !program.module.empty())) {
            fail("first AOT continuation slice requires exactly one standalone suspend function");
        }
        program.sum_types = std::move(sum_types_);
        return program;
    }

    static void validate_complete(const Program& program) { validate(program); }

private:
    std::vector<NativeSumType> sum_types_;
    bool is_sum_name(const std::string& name) const {
        return name.rfind("Option<", 0) == 0 || name.rfind("Result<", 0) == 0;
    }
    static bool sum_variant(std::string_view name) { return name == "Some" || name == "None" || name == "Ok" || name == "Err"; }

    bool at_type_only_introducer() const {
        return token_.text == "declare" ||
               (token_.text == "pub" && peek_next().text == "declare");
    }

    // Aliases are compile-time spellings for the existing scalar ABI, never
    // new native structs. Resolve through a bounded chain; a missing target or
    // trait/interface name must fail instead of becoming a forged class.
    ScalarType resolve_scalar_alias(const std::string& name) const {
        std::unordered_set<std::string> seen;
        std::string current = name;
        for (std::size_t depth = 0; depth < 129; ++depth) {
            if (current == "int") return ScalarType::I64;
            if (current == "bool") return ScalarType::Bool;
            if (!seen.insert(current).second)
                throw std::runtime_error("TYPE_ALIAS_CYCLE: cyclic scalar alias '" + current + "'");
            auto alias = scalar_alias_targets_.find(current);
            if (alias == scalar_alias_targets_.end()) {
                if (type_only_nominals_.count(current))
                    throw std::runtime_error("TYPE_ALIAS_KIND_MISMATCH: alias '" + name
                                             + "' names a trait/interface '" + current + "'");
                throw std::runtime_error("TYPE_ALIAS_UNKNOWN_TARGET: alias '" + name
                                         + "' targets unknown scalar type '" + current + "'");
            }
            current = alias->second;
        }
        throw std::runtime_error("TYPE_ALIAS_DEPTH_BUDGET: maximum alias depth is 128");
    }

    std::string type_only_scalar(bool allow_void) {
        if (token_.text == "int" || token_.text == "bool" ||
                (allow_void && token_.text == "void")) {
            std::string type(token_.text);
            advance();
            return type;
        }
        // Type aliases are complete declarations. Canonicalize their
        // underlying scalar ABI only after whole-catalog validation.
        if (token_.kind == Kind::Identifier && !reserved(token_.text) &&
                scalar_alias_targets_.count(std::string(token_.text))) {
            return identifier();
        }
        fail("TYPE_ONLY_UNSUPPORTED_TYPE: native v1 admits only int/bool scalar"
             " signatures and void results; nominal/generic lowering is not verified");
    }

    std::string type_alias_target() {
        if (accept("int") || accept("i64")) return "int";
        if (accept("bool")) return "bool";
        if (token_.kind == Kind::Identifier && !reserved(token_.text))
            return identifier();
        fail("TYPE_ONLY_UNSUPPORTED_TYPE: alias target must name int/i64, bool or another alias");
    }

    std::int64_t constraint_i64() {
        const bool negative = accept("-");
        if (token_.kind != Kind::Integer)
            fail("CONSTRAINT_I64_REQUIRED: constraint operand must be signed i64");
        std::string digits(token_.text);
        if (digits.front() == '_' || digits.back() == '_' ||
                digits.find("__") != std::string::npos)
            fail("CONSTRAINT_I64_MALFORMED: malformed integer separator");
        digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
        std::uint64_t magnitude = 0;
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), magnitude);
        const std::uint64_t negative_limit = (std::uint64_t{1} << 63);
        const std::uint64_t positive_limit =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const std::uint64_t limit = negative ? negative_limit : positive_limit;
        if (parsed.ec != std::errc{} ||
                parsed.ptr != digits.data() + digits.size() ||
                magnitude > limit)
            fail("CONSTRAINT_I64_OVERFLOW: constraint operand exceeds signed i64");
        advance();
        if (!negative) return static_cast<std::int64_t>(magnitude);
        if (magnitude == negative_limit)
            return std::numeric_limits<std::int64_t>::min();
        return -static_cast<std::int64_t>(magnitude);
    }

    void type_alias_constraints(TypeOnlyDecl& decl) {
        if (!accept("with")) return;
        if (decl.alias_target != "int")
            fail("CONSTRAINT_REPRESENTATION_UNSUPPORTED: descriptor v1 requires direct int/i64 alias");

        bool have_min = false;
        bool have_max = false;
        std::int64_t minimum = 0;
        std::int64_t maximum = 0;
        for (;;) {
            if (accept("min")) {
                if (have_min) fail("CONSTRAINT_DUPLICATE_MIN");
                minimum = constraint_i64();
                have_min = true;
            } else if (accept("max")) {
                if (have_max) fail("CONSTRAINT_DUPLICATE_MAX");
                maximum = constraint_i64();
                have_max = true;
            } else {
                fail("CONSTRAINT_PREDICATE_UNSUPPORTED: with admits min/max inclusive i64 only");
            }
            if (!accept("and")) break;
            if (token_.text == ";")
                fail("CONSTRAINT_PREDICATE_REQUIRED: expected constraint after and");
        }
        if (have_min && have_max && minimum > maximum)
            fail("CONSTRAINT_EMPTY_RANGE: min exceeds max");

        if (have_min) {
            TypeOnlyDecl::Constraint constraint;
            constraint.kind = TypeOnlyDecl::Constraint::Kind::MinInclusive;
            constraint.value = minimum;
            decl.constraints.push_back(constraint);
        }
        if (have_max) {
            TypeOnlyDecl::Constraint constraint;
            constraint.kind = TypeOnlyDecl::Constraint::Kind::MaxInclusive;
            constraint.value = maximum;
            decl.constraints.push_back(constraint);
        }
    }

    static std::string sha256_hex(const std::string& canonical) {
        llvm::SHA256 hash;
        hash.update(llvm::StringRef(canonical));
        const auto digest = hash.final();
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (std::uint8_t value : digest) {
            result.push_back(digits[(value >> 4) & 0x0f]);
            result.push_back(digits[value & 0x0f]);
        }
        return result;
    }

    void compile_type_constraint_descriptors(Program& program) const {
        program.type_constraint_descriptors.clear();
        for (const auto& decl : program.type_only_declarations) {
            if (decl.kind != TypeOnlyDecl::Kind::Alias || decl.constraints.empty())
                continue;
            TypeConstraintDescriptor descriptor;
            descriptor.schema_id = "ores.type-constraint.v1";
            descriptor.representation = "i64";
            const std::string local_name = decl.module_name.empty()
                ? decl.name : decl.module_name + "." + decl.name;
            descriptor.qualified_type_name = program.namespace_name.empty()
                ? local_name : program.namespace_name + "." + local_name;
            descriptor.canonical = descriptor.schema_id +
                "|type=" + descriptor.qualified_type_name + "|repr=i64";
            for (const auto& constraint : decl.constraints) {
                if (constraint.kind == TypeOnlyDecl::Constraint::Kind::MinInclusive) {
                    descriptor.canonical += "|min_inclusive_i64=" +
                        std::to_string(constraint.value) +
                        "|error=ORES_CONSTRAINT_MIN_INCLUSIVE_I64";
                } else {
                    descriptor.canonical += "|max_inclusive_i64=" +
                        std::to_string(constraint.value) +
                        "|error=ORES_CONSTRAINT_MAX_INCLUSIVE_I64";
                }
            }
            descriptor.fingerprint = sha256_hex(descriptor.canonical);
            program.type_constraint_descriptors.push_back(std::move(descriptor));
        }
    }

    void type_only_declaration(Program& program, bool already_defined = false,
                               bool before_public = false) {
        bool exported = before_public;
        if (already_defined) {
            if (accept("pub")) {
                if (exported) fail("TYPE_ONLY_DUPLICATE_VISIBILITY: duplicate pub");
                exported = true;
            }
        } else {
            const bool prefix_public = accept("pub");
            require("declare");
            const bool postfix_public = accept("pub");
            if ((prefix_public && postfix_public) || token_.text == "pub")
                fail("TYPE_ONLY_DUPLICATE_VISIBILITY: duplicate pub");
            exported = prefix_public || postfix_public;
        }

        TypeOnlyDecl decl;
        decl.module_name = program.module;
        decl.exported = exported;
        if (accept("type")) decl.kind = TypeOnlyDecl::Kind::Alias;
        else if (accept("trait")) decl.kind = TypeOnlyDecl::Kind::Trait;
        else if (accept("interface")) decl.kind = TypeOnlyDecl::Kind::Interface;
        else fail("TYPE_ONLY_INVALID_KIND: declare admits type, trait or interface; "
                  "define model is a runtime type and value");

        decl.name = identifier();
        if (decl.name == "String" || decl.name == "string" || callable_nominal_name(decl.name) || entry_builtin_name(decl.name))
            fail("TYPE_DECL_RESERVED_BUILTIN: compiler-owned nominal type name '" +
                 decl.name + "' cannot be redefined");
        if (decl.kind == TypeOnlyDecl::Kind::Alias) {
            if (token_.text == "<")
                fail("TYPE_ONLY_GENERIC_UNSUPPORTED: native aliases are non-generic in descriptor v1");
            // 'declare type' and 'define type' share this exact grammar.
            // Incomplete 'declare type Code;' never creates an alias.
            if (token_.text != "=")
                fail("TYPE_ONLY_INCOMPLETE_DEFINITION: declare type and define type"
                     " both require '= <type>'");
            require("=");
            decl.alias_target = type_alias_target();
            type_alias_constraints(decl);
            require(";");
            scalar_alias_targets_.emplace(decl.name, decl.alias_target);
            if (!decl.constraints.empty()) constrained_aliases_.insert(decl.name);
        } else {
            type_only_nominals_.insert(decl.name);
            if (accept("extends")) {
                do {
                    if (decl.parents.size() >= 32)
                        fail("TYPE_ONLY_PARENT_BUDGET: maximum 32 parent types");
                    decl.parents.push_back(identifier());
                } while (accept(","));
            }
            const bool braces = accept("{");
            if (!braces) require("as");
            std::unordered_set<std::string> member_names;
            while (token_.kind != Kind::End &&
                    token_.text != (braces ? "}" : "end")) {
                if (decl.methods.size() >= 128)
                    fail("TYPE_ONLY_MEMBER_BUDGET: maximum 128 method signatures");
                require("fnc");
                TypeOnlyDecl::Method method;
                method.name = identifier();
                require("(");
                std::unordered_set<std::string> formal_names;
                if (!accept(")")) {
                    do {
                        if (method.parameter_types.size() >= 32)
                            fail("TYPE_ONLY_PARAM_BUDGET: maximum 32 method parameters");
                        std::string type;
                        if (token_.kind == Kind::Identifier && !reserved(token_.text) &&
                                peek_next().text == ":") {
                            const std::string name = identifier();
                            if (!formal_names.insert(name).second)
                                fail("TYPE_ONLY_DUPLICATE_PARAM: duplicate method parameter '" + name + "'");
                            require(":");
                            type = type_only_scalar(false);
                        } else {
                            type = type_only_scalar(false);
                            const std::string name = identifier();
                            if (!formal_names.insert(name).second)
                                fail("TYPE_ONLY_DUPLICATE_PARAM: duplicate method parameter '" + name + "'");
                        }
                        method.parameter_types.push_back(std::move(type));
                    } while (accept(","));
                    require(")");
                }
                // Method overload identity is name + arity, not return type.
                const std::string key = method.name + "/" +
                    std::to_string(method.parameter_types.size());
                if (!member_names.insert(key).second)
                    fail("TYPE_ONLY_DUPLICATE_MEMBER: duplicate method slot '" + key + "'");
                if (!accept(":")) {
                    // '=' and '>' are separate lexer tokens: require adjacency.
                    if (token_.text != "=" || peek_next().text != ">" ||
                            peek_next().offset != token_.offset + 1)
                        fail("TYPE_ONLY_BAD_ARROW: signature requires contiguous => or :");
                    require("=");
                    require(">");
                }
                method.return_type = type_only_scalar(true);
                require(";");
                decl.methods.push_back(std::move(method));
            }
            require(braces ? "}" : "end");
        }
        if (program.type_only_declarations.size() >= 1024)
            fail("TYPE_ONLY_DECL_BUDGET: maximum 1024 type-only declarations");
        program.type_only_declarations.push_back(std::move(decl));
    }

    struct ForwardType {
        bool exported = false;
    };
    struct ScalarAlias {
        std::string target;
        bool exported = false;
    };

    void parse_type_forward() {
        const bool exported = accept("pub");
        if (!accept("type")) {
            if (token_.text == "interface" || token_.text == "trait") {
                fail("native trait/interface forward requires a verified type-level linker; unsupported");
            }
            fail("native declare supports only scalar type aliases; class/actor/enum/model unsupported");
        }
        const std::string name = identifier();
        if (name == "String" || name == "string" || name == "int" ||
                name == "bool" || reserved(name) || callable_nominal_name(name) ||
                entry_builtin_name(name))
            fail("native forward type cannot shadow builtin or reserved types");
        if (token_.text == "<")
            fail("native generic type alias forward requires a verified monomorphization ABI");
        require(";");
        if (!forwards_.emplace(name, ForwardType{exported}).second) {
            fail("duplicate native forward type declaration '" + name + "'");
        }
        // Forward signatures are permitted after a matching definition in
        // the same scope, but never change the effective export authority.
        const auto existing = defined_type_aliases_.find(name);
        if (existing != defined_type_aliases_.end() && existing->second.exported != exported)
            fail("native forward type visibility mismatch for '" + name + "'");
    }

    void parse_scalar_type_alias(bool exported) {
        if (token_.text == "int" || token_.text == "bool")
            fail("native type alias cannot shadow a builtin or reserved language name");
        const std::string name = identifier();
        if (token_.text == "<")
            fail("native generic type alias requires a verified monomorphization ABI");
        if (name == "int" || name == "bool" || name == "string" ||
                name == "String" || reserved(name) ||
                callable_nominal_name(name) || entry_builtin_name(name))
            fail("native type alias cannot shadow a builtin or reserved language name");
        require("=");
        std::string target;
        if (accept("int")) target = "int";
        else if (accept("bool")) target = "bool";
        else if (accept("string") || accept("String")) {
            if (token_.text == "with" || token_.text == "as")
                fail("native refined string aliases require Zorde UTF-8/charset checks "
                     "and proof-preserving LLVM lowering; unsupported");
            fail("native string type alias ABI is not yet admitted");
        } else {
            // Do not eagerly copy the aliased layout: a forward chain may
            // resolve after this declaration. The second pass checks cycles,
            // missing definitions and the ultimate primitive scalar kind.
            const std::string candidate(token_.text);
            if (defined_type_aliases_.count(candidate) || forwards_.count(candidate))
                target = identifier();
            else fail("native type aliases currently require a declared int/bool scalar target");
        }
        if (token_.text == "with" || token_.text == "as")
            fail("native refined aliases require checked Zorde exact-int constraint ABI "
                 "and proof-preserving lowering; cannot erase with/as to int");
        require(";");
        if (defined_type_aliases_.count(name))
            fail("duplicate native type alias definition '" + name + "'");
        const auto forward = forwards_.find(name);
        if (forward != forwards_.end() && forward->second.exported != exported)
            fail("native forward type visibility mismatch for '" + name + "'");
        defined_type_aliases_.emplace(name, ScalarAlias{std::move(target), exported});
    }

    bool accept_scalar_alias(std::string_view expected) {
        const auto it = defined_type_aliases_.find(std::string(token_.text));
        if (it == defined_type_aliases_.end() || it->second.target != expected) return false;
        advance();
        return true;
    }

    bool accept_pending_scalar_alias(std::string& name) {
        // Generic parameters, classes and type-only traits/interfaces are not
        // silently erased to scalar aliases.
        const std::string candidate(token_.text);
        if (!scalar_alias_targets_.count(candidate))
            return false;
        name = identifier();
        return true;
    }

    void resolve_scalar_type_aliases(Program& program) {
        std::unordered_set<std::string> active;
        std::function<std::string(const std::string&, std::size_t)> resolve =
            [&](const std::string& name, std::size_t depth) -> std::string {
                if (depth > 64)
                    throw std::runtime_error("native type alias resolution depth exceeds 64");
                auto found = defined_type_aliases_.find(name);
                if (found == defined_type_aliases_.end())
                    throw std::runtime_error("unresolved native scalar type alias '" + name + "'");
                const std::string target = found->second.target;
                if (target == "int" || target == "bool") return target;
                if (!active.insert(name).second)
                    throw std::runtime_error("native type alias cycle involving '" + name + "'");
                // All edges are explicit same-scope forward or alias declarations.
                const std::string result = resolve(target, depth + 1);
                active.erase(name);
                found->second.target = result;
                return result;
            };
        for (auto& entry : defined_type_aliases_)
            (void)resolve(entry.first, 0);

        for (auto& fn : program.functions) {
            for (auto& param : fn.parameters) {
                if (param.pending_type_alias.empty()) continue;
                const std::string type = resolve(param.pending_type_alias, 0);
                if (type == "bool" && param.mode != Parameter::Mode::Value)
                    throw std::runtime_error("native bool borrow ABI is not yet admitted; pass bool by value");
                param.scalar_type = type == "bool" ? ScalarType::Bool : ScalarType::I64;
                param.pending_type_alias.clear();
            }
            if (!fn.pending_return_type_alias.empty()) {
                const std::string type = resolve(fn.pending_return_type_alias, 0);
                fn.return_type = type == "bool" ? ScalarType::Bool : ScalarType::I64;
                fn.pending_return_type_alias.clear();
            }
        }
    }

    Token peek_next() const {
        Lexer next_lexer = lexer_;
        return next_lexer.next();
    }

    void built_in_actor_threads_enum() {
        if (actor_threads_enum_seen_)
            fail("duplicate MaxActorThreads enum definition");
        require("MaxActorThreads");
        require("as");
        constexpr std::string_view tiers[] = {"LOW", "MEDIUM", "HIGH"};
        constexpr std::string_view numbers[] = {"1", "2", "3"};
        for (std::size_t i = 0; i < 3; ++i) {
            require(tiers[i]);
            require("int");
            require("=");
            if (token_.kind != Kind::Integer || token_.text != numbers[i])
                fail("MaxActorThreads backing value must be the canonical integer");
            advance();
            accept(";");
        }
        require("end");
        actor_threads_enum_seen_ = true;
    }

    MaxActorThreads actor_max_threads_attribute() {
        require("ActorMaxThreads");
        require("(");
        require("MaxActorThreads");
        require(".");
        MaxActorThreads level = MaxActorThreads::LOW;
        if (accept("LOW")) level = MaxActorThreads::LOW;
        else if (accept("MEDIUM")) level = MaxActorThreads::MEDIUM;
        else if (accept("HIGH")) level = MaxActorThreads::HIGH;
        else fail("@ActorMaxThreads requires MaxActorThreads.LOW/MEDIUM/HIGH");
        require(")");
        if (level != MaxActorThreads::LOW)
            fail("@ActorMaxThreads(MEDIUM/HIGH) requires compiler-proven per-turn actor state isolation; source parallel actors are not enabled");
        return level;
    }

    ModuleContract contract() {
        ModuleContract result;
        result.name = nominal_declaration_name();
        require("as");
        const bool braced = accept("{");
        std::unordered_set<std::string> seen;
        while (token_.kind != Kind::End
               && token_.text != (braced ? "}" : "end")) {
            ContractMethod method;
            if (!accept("fnc")) {
                fail("native i64 contracts only admit fnc signatures");
            }
            method.name = identifier();
            if (!seen.insert(method.name).second) {
                fail("duplicate native module contract method");
            }
            require("(");
            std::unordered_set<std::string> parameter_names;
            if (!accept(")")) {
                do {
                    Parameter::Mode mode = Parameter::Mode::Value;
                    if (accept("borrow")) {
                        mode = accept("mut") ? Parameter::Mode::BorrowMut
                                             : Parameter::Mode::Borrow;
                    }
                    require("int");
                    if (!parameter_names.insert(identifier()).second)
                        fail("duplicate native module contract parameter");
                    method.parameter_modes.push_back(mode);
                } while (accept(","));
                require(")");
            }
            const auto arrow_next = peek_next();
            if (token_.text != "=" || arrow_next.text != ">"
                    || arrow_next.offset != token_.offset + 1)
                fail("contract signatures require contiguous '=>'");
            require("=");
            require(">");
            require("int");
            require(";");
            result.methods.push_back(std::move(method));
        }
        require(braced ? "}" : "end");
        if (result.methods.empty()) fail("empty native module contract is unsupported");
        return result;
    }

    ActorDecl actor_declaration() {
        ActorDecl actor;
        actor.offset = token_.offset;
        actor.name = nominal_declaration_name();
        if (callable_nominal_name(actor.name))
            fail("callable signatures cannot be declared as nominal actor types");
        if (entry_builtin_name(actor.name))
            fail("builtin Entry is compiler-owned and cannot be declared as a nominal actor type");
        require("as");
        const bool braced = accept("{");
        if (token_.text == "state") {
            fail("native actor state { ... } requires sealed handle_state lowering; legacy scalar actor fields are not this contract");
        }
        std::unordered_set<std::string> names;
        while (token_.text == "let" || token_.text == "const" || token_.text == "val") {
            if (actor.fields.size() == 16) fail("native actor field limit exceeded (16)");
            LocalBinding field;
            field.offset = token_.offset;
            field.field_mutable = token_.text == "let";
            advance();
            field.name = identifier();
            if (!names.insert(field.name).second) fail("duplicate actor field '" + field.name + "'");
            require(":");
            require("int");
            require("=");
            field.initializer = expression();
            require(";");
            actor.fields.push_back(std::move(field));
        }
        if (token_.text == "final") {
            fail("native final actor fields require typed field initialization and ownership lowering");
        }
        if (actor.fields.empty()) fail("actor requires at least one scalar field");
        Function method;
        method.offset = token_.offset;
        method.exported = accept("pub");
        if (!method.exported) fail("native actor requires pub on_message(int)");
        // Match the Java frontend's declaration spelling contract. The native
        // actor subset still lowers only on_message(int) and cannot admit
        // arbitrary methods, static fnc members or suspension frames.
        if (token_.text == "suspend" || token_.text == "suspends") {
            fail("native suspends actor method requires continuation lowering");
        }
        // Actor method modifiers have their own admission boundary. Do not
        // allow the fixed-int actor subset to parse these as method names or
        // quietly route a read declaration through the legacy write lane.
        if (at_unsupported_critical_mode()) {
            fail("native reader/writer critical declaration requires compiler-checked RW gate and scheduler-owned logical permits");
        }
        if (accept("static")) {
            if (accept("method")) fail("static method is invalid; use static fnc");
            fail("native actor static fnc requires separate member lowering");
        }
        if (!accept("fnc")) accept("method"); // optional method marker
        if (at_unsupported_critical_mode()) {
            fail("native reader/writer critical declaration requires compiler-checked RW gate and scheduler-owned logical permits");
        }
        method.name = identifier();
        if (method.name != "on_message") fail("native actor supports only on_message");
        require("(");
        if (token_.text == "@") fail("native @Default actor method formal requires checked annotation ABI lowering");
        require("int");
        Parameter param;
        param.name = identifier();
        method.parameters.push_back(std::move(param));
        require(")");
        require(":");
        require("int");
        require("{");
        // The first compiler-lowered per-message owned value slice is
        // deliberately synchronous and immutable. Handler temporaries are
        // allocated in the existing actor heap and dropped before returning;
        // borrow/take/await need separate lifetime-aware lowering.
        while (token_.text != "return") {
            if (token_.kind == Kind::End || token_.text == "}") fail("actor handler requires return");
            if (token_.text == "const") {
                if (!method.actor_field_writes.empty())
                    fail("native actor owned temporaries must precede field writes");
                if (!experimental_owned_int_)
                    fail("experimental owned int requires --experimental-owned-int");
                LocalBinding local;
                local.offset = token_.offset;
                advance();
                local.name = identifier();
                if (local.name == method.parameters.front().name ||
                        names.count(local.name))
                    fail("actor temporary shadows a field or parameter");
                if (method.locals.size() >= 8)
                    fail("native actor temporary owner limit exceeded (8)");
                require(":");
                require("owned");
                require("int");
                require("=");
                local.initializer = expression();
                if (!native_constructor_scalar_tree(*local.initializer))
                    fail("native actor owned temporary requires pure scalar initializer");
                require(";");
                if (!names.insert(local.name).second)
                    fail("duplicate actor temporary owner");
                local.heap_owned = true;
                method.locals.push_back(std::move(local));
                continue;
            }
            ActorFieldWrite write;
            write.offset = token_.offset;
            write.field = identifier();
            require("=");
            write.expression = expression();
            require(";");
            method.actor_field_writes.push_back(std::move(write));
        }
        require("return");
        method.result = expression();
        if (!method.locals.empty()) {
            // This slice has one drop epilogue. No taken/moved or borrowed
            // reference can escape the handler in its scalar return value.
            if (!native_constructor_scalar_tree(*method.result))
                fail("native actor owned temporary return requires pure scalar expression");
            for (const auto& write : method.actor_field_writes)
                if (!native_constructor_scalar_tree(*write.expression))
                    fail("native actor owned temporary field write requires pure scalar expression");
        }
        require(";");
        require("}");
        actor.on_message = std::move(method);
        if (braced) require("}");
        else require("end");
        return actor;
    }

    ClassDecl class_declaration() {
        ClassDecl result;
        result.name = nominal_declaration_name();
        if (result.name == "Option" || result.name == "Result") fail("native sum type names are compiler-owned");
        if (callable_nominal_name(result.name))
            fail("callable signatures cannot be declared as nominal class types");
        if (entry_builtin_name(result.name))
            fail("builtin Entry is compiler-owned and cannot be declared as a nominal class type");
        require("as");
        std::unordered_set<std::string> seen;
        while (token_.text != "end") {
            if (token_.kind == Kind::End) fail("expected 'end'");
            const bool publicly_visible = accept("pub");
            if (!publicly_visible) accept("private");
            // Formatter may insert 'mtd', but method identity and receiver
            // erasure must remain unchanged. Never consume it as a class field.
            const bool method_marker = accept("mtd");
            const bool method_ahead = token_.kind == Kind::Identifier && token_.text != "constructor" && (!reserved(token_.text) || token_.text == "pattern")
                && [&] { Lexer look = lexer_; const auto next = look.next().text; return next == "(" || next == "<"; }();
            if (method_marker && !method_ahead)
                fail("native mtd marker must precede a method signature");
            if (method_ahead) {
                auto method = function(result.name, publicly_visible);
                ClassMethod signature;
                signature.name = method.name.substr(result.name.size() + 1);
                signature.arity = native_source_value_arity(method.parameters.size(), true);
                signature.public_member = publicly_visible;
                signature.mutable_receiver = method.parameters.front().mode == Parameter::Mode::BorrowMut;
                for (const auto& prior : result.methods)
                    if (prior.name == signature.name && prior.arity == signature.arity)
                        fail("duplicate native method name/arity");
                result.methods.push_back(std::move(signature));
                methods_.push_back(std::move(method));
                continue;
            }
            if (accept("constructor")) {
                if (result.explicit_constructor)
                    fail("native class may declare only one scalar constructor");
                result.explicit_constructor = true;
                result.constructor_exported = publicly_visible;
                require("(");
                std::unordered_set<std::string> params;
                if (!accept(")")) {
                    do {
                        if (result.constructor_parameters.size() == 16)
                            fail("native scalar constructor parameter limit exceeded (16)");
                        if (token_.text == "@")
                            fail("native @Default constructor formal requires checked constant and constructor ABI lowering");
                        require("int");
                        std::string name = identifier();
                        if (!params.insert(name).second)
                            fail("duplicate native constructor parameter '" + name + "'");
                        result.constructor_parameters.push_back(std::move(name));
                        result.constructor_parameter_types.push_back(ScalarType::I64);
                    } while (accept(","));
                    require(")");
                }
                require("{");
                if (accept("return")) require(";");
                if (token_.text != "}")
                    fail("native scalar constructor body must be empty or contain only 'return;'");
                require("}");
                continue;
            }
            if (result.explicit_constructor)
                fail("native scalar constructor must be the last class member");
            if (result.fields.size() == 16)
                fail("native class field limit exceeded (16)");
            const bool legacy_let = accept("let");
            ClassField field;
            field.public_member = publicly_visible;
            bool literal_const = false;
            bool val_alias = false;
            if (!legacy_let) {
                field.final_field = accept("final");
                if (field.final_field) literal_const = accept("const");
                else if (accept("const")) literal_const = field.final_field = true;
                else field.final_field = val_alias = accept("val");
            }
            const bool explicit_mut = accept("mut");
            if (val_alias && explicit_mut) fail("duplicate mut class field qualifier");
            const bool mutable_referent = val_alias || explicit_mut;
            field.mutable_referent = mutable_referent;
            if (mutable_referent && accept("Array")) {
                // 'mut Array<int>' is canonical: mut is the referent
                // capability, independent of whether the slot is final.
                require("<");
                if (accept("String") || accept("string")) field.array_element_type = ScalarType::String;
                else if (!accept("int")) field.array_record_class = type_identifier();
                require(">");
                field.int_array = true;
                field.name = identifier();
                if (!seen.insert(field.name).second)
                    fail("duplicate class field '" + field.name + "'");
                if (accept(";")) {
                    if (!field.final_field) fail("declaration-only collection field requires val or final");
                    field.constructor_argument = true;
                    result.fields.push_back(std::move(field));
                    continue;
                }
                if (!accept(":")) require("=");
                require("new"); require("Array");
                require("<");
                if (!field.array_record_class.empty()) {
                    if (type_identifier() != field.array_record_class) fail("array field constructor type mismatch");
                } else if (field.array_element_type == ScalarType::String) {
                    if (!accept("String")) require("string");
                } else require("int");
                require(">");
                require("(");
                const std::size_t initializer_end = token_.offset + token_.text.size();
                require(")");
                class_field_terminator(initializer_end);
            } else {
                if (token_.text == "Array") fail("native Array<int> field requires mut referent capability");
                if (token_.text != "Option" && token_.text != "Result" && token_.text != "int" && token_.text != "bool" && token_.text != "String" && token_.text != "string") {
                    if (!field.final_field || literal_const) fail("expected val or final for native nested class field");
                    field.class_name = type_identifier(); field.name = identifier();
                    if (!seen.insert(field.name).second) fail("duplicate class field '" + field.name + "'");
                    require(";"); field.constructor_argument = true;
                    result.fields.push_back(std::move(field)); continue;
                }
                if (token_.text == "Option" || token_.text == "Result") field.sum_type = type_identifier();
                field.scalar_type = accept("bool") ? ScalarType::Bool :
                    (accept("String") || accept("string")) ? ScalarType::String : ScalarType::I64;
                if (field.scalar_type == ScalarType::I64 && field.sum_type.empty()) require("int");
                field.name = identifier();
                if (!seen.insert(field.name).second)
                    fail("duplicate class field '" + field.name + "'");
                if (accept(";")) {
                    if (literal_const)
                        fail("const native field requires a literal initializer");
                    field.constructor_argument = true;
                    result.fields.push_back(std::move(field));
                    continue;
                }
                if (!accept(":") && !accept("="))
                    fail(field.final_field
                        ? "final class field requires declaration-site initializer"
                        : "class field requires declaration-site initializer");
                const std::size_t literal_end = token_.offset + token_.text.size();
                if (literal_const && token_.kind != Kind::Integer &&
                        !(field.scalar_type == ScalarType::Bool && (token_.text == "true" || token_.text == "false")))
                    fail("class field requires i64 literal initializer");
                auto initializer = expression();
                if (initializer->kind == Expression::Kind::Negate && initializer->left && initializer->left->kind == Expression::Kind::Integer) {
                    initializer->integer = -initializer->left->integer; initializer->left.reset(); initializer->kind = Expression::Kind::Integer;
                }
                if (literal_const && initializer->kind != Expression::Kind::Integer && initializer->kind != Expression::Kind::Boolean)
                    fail("class field requires i64 literal initializer");
                if (initializer->kind == Expression::Kind::Integer || initializer->kind == Expression::Kind::Boolean) {
                    field.initial = initializer->kind == Expression::Kind::Boolean ? (initializer->boolean ? 1 : 0) : initializer->integer;
                    class_field_terminator(literal_end);
                } else {
                    // General expression boundaries require explicit ';' in
                    // this limited native constructor slice.
                    require(";");
                }
                if (!native_constructor_scalar_tree(*initializer))
                    fail("native class field initializer requires pure scalar constructor parameters or integers");
                field.initializer_expression = std::move(initializer);
            }
            result.fields.push_back(std::move(field));
        }
        // Immutable declaration-only fields form a positional constructor.
        // Initializers use parameter slots, never uninitialized field storage.
        for (auto& field : result.fields) {
            if (!field.constructor_argument) continue;
            if (result.explicit_constructor)
                fail("declaration-only fields with explicit constructor require checked field-assignment lowering");
            result.constructor_parameters.push_back(field.name);
            result.constructor_parameter_types.push_back(field.scalar_type);
            field.initializer_expression = std::make_unique<Expression>();
            field.initializer_expression->kind = Expression::Kind::Variable;
            field.initializer_expression->name = field.name;
        }
        // Fieldless classes still have nominal identity and checked receiver loans.
        for (const auto& method : result.methods)
            if (seen.count(method.name)) fail("native field/method name collision requires explicit callable member resolution");
        std::unordered_set<std::string> allowed(
            result.constructor_parameters.begin(), result.constructor_parameters.end());
        for (const auto& field : result.fields) {
            if (!field.sum_type.empty() && result.explicit_constructor) fail("native sum field constructor requires checked nominal parameters");
            if (!field.class_name.empty() && result.explicit_constructor)
                fail("native nested owned class constructor requires checked parameter ownership");
            if (field.int_array && result.explicit_constructor)
                fail("native owned Array<int> constructor requires checked failure cleanup");
            if (!field.initializer_expression) continue;
            const bool literal_sum = !field.sum_type.empty() && field.initializer_expression->kind == Expression::Kind::SumConstruct &&
                std::all_of(field.initializer_expression->arguments.begin(), field.initializer_expression->arguments.end(), [](const auto& arg) {
                    return arg->kind == Expression::Kind::String || arg->kind == Expression::Kind::Integer || arg->kind == Expression::Kind::Boolean;
                });
            if (!result.explicit_constructor && !field.constructor_argument &&
                    !literal_sum && field.initializer_expression->kind != Expression::Kind::Integer &&
                    field.initializer_expression->kind != Expression::Kind::Boolean &&
                    field.initializer_expression->kind != Expression::Kind::String)
                fail("native nonliteral class field initializer requires explicit scalar constructor");
            validate_expression(*field.initializer_expression, allowed, {});
        }
        require("end");
        return result;
    }

    void class_field_terminator(std::size_t initializer_end) {
        if (accept(";")) return;
        if (token_.kind == Kind::End || token_.offset < initializer_end)
            fail("class field requires ';' or a line break");
        // Unlike expression statements, native class fields may omit ';'
        // only at a real lexical line boundary. A same-line next member is
        // never silently admitted by whitespace or comments alone.
        const auto gap = source_.substr(initializer_end, token_.offset - initializer_end);
        if (gap.find('\n') == std::string_view::npos &&
                gap.find('\r') == std::string_view::npos)
            fail("class field requires ';' or a line break");
    }

    std::int64_t integer_literal() {
        if (token_.kind != Kind::Integer) fail("expected integer literal");
        std::string digits(token_.text);
        if (digits.front() == '_' || digits.back() == '_' ||
                digits.find("__") != std::string::npos) fail("malformed integer separator");
        digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
        std::int64_t value = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
            fail("integer literal overflows signed 64-bit result");
        advance();
        return value;
    }

    CooperativeScalarLoop cooperative_scalar_loop() {
        // Explicitly narrow syntax avoids accidentally accepting unpinned
        // loans, exception cleanup, arbitrary calls or nonresumable CFG.
        CooperativeScalarLoop loop;
        loop.offset = token_.offset;
        require("suspend");
        if (token_.text != "fnc") {
            fail("native suspends declaration requires suspension-aware borrow checking and LLVM continuation lowering");
        }
        require("fnc");
        loop.name = identifier();
        require("("); require(")");
        require(":"); require("void");
        require("{");
        require("val");
        loop.variable = identifier();
        require(":"); require("int"); require("=");
        loop.initial = integer_literal();
        require(";");
        require("while"); require("("); require("true"); require(")");
        require("{");
        if (identifier() != loop.variable)
            fail("AOT continuation requires assignment to its sole frame-owned scalar");
        require("=");
        if (identifier() != loop.variable)
            fail("AOT continuation update must read its frame-owned scalar");
        require("+");
        if (integer_literal() != 1)
            fail("first AOT continuation supports only increment by one");
        require(";");
        require("}");
        require("}");
        return loop;
    }

    static std::string extension_function_name(const std::string& receiver,
                                               const std::string& method) {
        // Dollar is not a guest identifier character, so source cannot forge
        // or directly call this compiler-owned extension lowering slot.
        return "$mtd$" + receiver + "$" + method;
    }

    Function extension_method(bool exported) {
        extension_offset_ = token_.offset;
        require("mtd");
        extension_method_name_ = identifier();
        if (extension_method_name_ == "init" || extension_method_name_ == "main")
            fail("EXTENSION_RESERVED_NAME: extension method cannot use a reserved lifecycle or entrypoint name");
        require("on");
        if (token_.text == "int" || token_.text == "bool")
            fail("EXTENSION_RECEIVER_REQUIRES_NAMED_ALIAS: primitive receivers are not extension identities");
        extension_receiver_alias_ = identifier();
        if (!scalar_alias_targets_.count(extension_receiver_alias_)) {
            if (type_only_nominals_.count(extension_receiver_alias_))
                fail("EXTENSION_RECEIVER_KIND: extension receiver must be a scalar type alias");
            fail("EXTENSION_RECEIVER_UNKNOWN: extension receiver must name a declared scalar type alias");
        }
        extension_header_active_ = true;
        Function value = function();
        extension_header_active_ = false;
        value.exported = false;
        const std::size_t explicit_arity = value.parameters.empty()
            ? 0 : value.parameters.size() - 1;
        auto& slots = extension_methods_[extension_receiver_alias_][extension_method_name_];
        if (!slots.insert(explicit_arity).second)
            fail("EXTENSION_DUPLICATE_SLOT: duplicate receiver/method/arity");
        if (exported) extension_public_slots_.insert(
            extension_receiver_alias_ + "$" + extension_method_name_ + "$" +
            std::to_string(explicit_arity));
        return value;
    }

    Function function(std::string owner = {}, bool visible = false) {
        struct Restore { std::string& target; std::string prior; ~Restore() { target = std::move(prior); } } restore{method_scope_, method_scope_};
        method_scope_ = owner;
        struct RestoreReceivers { std::unordered_set<std::string>& target; std::unordered_set<std::string> prior; ~RestoreReceivers() { target = std::move(prior); } } restore_receivers{class_receivers_, std::move(class_receivers_)};
        class_receivers_.clear();
        Function value;
        value.method_owner = owner;
        const bool alias_extension = extension_header_active_;
        value.offset = alias_extension ? extension_offset_ : token_.offset;
        if (alias_extension) {
            if (!owner.empty())
                fail("extension method cannot also be a class method");
            value.exported = false;
            value.name = extension_function_name(
                extension_receiver_alias_, extension_method_name_);
        } else {
            value.exported = owner.empty() ? accept("pub") : visible;
        const bool source_async = owner.empty() && accept("async");
        value.async_source_i64 = source_async;
        if (token_.text == "pure") {
            fail("native pure modifier awaits transitive observable-effect verification");
        }
        if (exceptional_control(token_.text))
            fail("native exception/control effect unsupported");
        if (token_.text == "gpu" || token_.text == "cpu" || token_.text == "quantum") {
            fail("native execution domain lowering unsupported");
        }
        if (token_.text == "suspends" || token_.text == "suspend") {
            fail("native suspends declaration requires suspension-aware borrow checking and LLVM continuation lowering");
        }
        // Check compound READ/WRITE modes before handling legacy critical,
        // otherwise declaration parsing could report WRITE-only semantics.
        if (at_unsupported_critical_mode()) {
            fail("native reader/writer critical declaration requires compiler-checked RW gate and scheduler-owned logical permits");
        }
        if (token_.text == "critical") {
            fail("native critical declaration requires compiler lowering and scheduler-owned logical critical tickets");
        }
        if (token_.text == "actor" || token_.text == "async" ||
            token_.text == "generator" || token_.text == "isoactor") {
            fail("native actor/async declaration requires actor frames, suspension-aware borrow checking and LLVM lowering");
        }
        if (source_async) {
            if (!accept("fnc"))
                fail("native first async Future<int> producer requires async fnc");
        } else if (owner.empty() && !accept("fnc") && !accept("routine")) {
            fail("expected fnc or routine");
        }
        if (at_unsupported_critical_mode()) {
            fail("native reader/writer critical declaration requires compiler-checked RW gate and scheduler-owned logical permits");
        }
        value.name = owner.empty() ? identifier() : owner + "." + identifier();
        if (!owner.empty() && token_.text == "<") fail("native generic methods require typed receiver monomorphization");
        if (accept("<")) {
            if (accept(">")) fail("native generic declaration cannot be empty");
            do {
                GenericParameter slot;
                slot.name = identifier();
                if (slot.name == "_" || slot.name.rfind("$", 0) == 0)
                    fail("invalid generic parameter name");
                for (const auto& prior : value.generic_parameters)
                    if (prior.name == slot.name)
                        fail("duplicate generic parameter '" + slot.name + "'");
                if (accept("=")) {
                    if (accept("int")) {
                        slot.default_type = "int";
                    } else if (token_.kind == Kind::Identifier &&
                               scalar_alias_targets_.count(std::string(token_.text))) {
                        const std::string alias = identifier();
                        if (resolve_scalar_alias(alias) != ScalarType::I64)
                            fail("native scalar generic default requires int ABI");
                        slot.default_type = "int";
                    } else {
                        const std::string dependency = identifier();
                        const bool earlier = std::any_of(value.generic_parameters.begin(),
                            value.generic_parameters.end(),
                            [&](const GenericParameter& p) { return p.name == dependency; });
                        if (!earlier) fail("forward or self-referential generic default");
                        slot.default_type = dependency;
                    }
                }
                value.generic_parameters.push_back(std::move(slot));
            } while (accept(","));
            require(">");
        }
        if (value.name == "main" && !value.generic_parameters.empty())
            fail("native entrypoint main cannot be generic");
        }
        require("(");
        if (alias_extension) {
            Parameter self;
            self.name = "self";
            self.pending_type_alias = extension_receiver_alias_;
            self.mode = Parameter::Mode::Value;
            value.parameters.push_back(std::move(self));
        }
        if (!owner.empty()) {
            Parameter receiver;
            receiver.name = "self"; receiver.class_name = owner; receiver.mode = Parameter::Mode::Borrow;
            if (accept("mut")) {
                require("self");
                receiver.mode = Parameter::Mode::BorrowMut;
                if (accept(",")) {
                    if (token_.text == ")")
                        fail("native explicit self receiver must be followed by a parameter after ','");
                } else if (token_.text == ")" && peek_next().text == "(") {
                    // Keep legacy method(mut self)(params) syntax.
                    require(")"); require("(");
                } else if (token_.text != ")") {
                    fail("native explicitly typed mutable self receiver unsupported");
                }
            } else if (accept("self")) {
                // The receiver already occupies the first hidden ABI parameter.
                // A declaration-level self marker consumes no source argument.
                if (accept(",")) {
                    if (token_.text == ")")
                        fail("native explicit self receiver must be followed by a parameter after ','");
                } else if (token_.text != ")") {
                    fail("native explicitly typed read self receiver unsupported");
                }
            }
            value.parameters.push_back(std::move(receiver));
        }
        if (!accept(")")) {
            do {
                if (token_.text == "@")
                    fail("native @Default parameter annotation requires checked literal, Option and ownership ABI lowering");
                Parameter parameter;
                if (accept("borrow")) {
                    parameter.mode = accept("mut") ? Parameter::Mode::BorrowMut
                                                    : Parameter::Mode::Borrow;
                }
                if (accept("int")) {
                    parameter.scalar_type = ScalarType::I64;
                } else if (accept("bool")) {
                    parameter.scalar_type = ScalarType::Bool;
                } else if (accept("string") || accept("String")) {
                    parameter.scalar_type = ScalarType::String;
                } else if (accept_pending_scalar_alias(parameter.pending_type_alias)) {
                    // Deferred scalar aliases are admitted only after full resolution.
                } else if (token_.kind == Kind::Identifier &&
                           type_only_nominals_.count(std::string(token_.text))) {
                    fail("TYPE_ALIAS_KIND_MISMATCH: trait/interface cannot be a scalar parameter");
                } else {
                    const std::size_t type_offset = token_.offset;
                    const std::string type = type_identifier();
                    if (entry_builtin_name(type)) reject_native_entry_boundary(type_offset);
                    const bool generic = std::any_of(value.generic_parameters.begin(),
                        value.generic_parameters.end(),
                        [&](const GenericParameter& p) { return p.name == type; });
                    if (generic) parameter.generic_type = type;
                    else if (is_sum_name(type)) parameter.sum_type = type;
                    else parameter.class_name = type;
                }
                if (token_.text == "?")
                    fail("native optional type '?' requires versioned Option/default ABI lowering");
                if (alias_extension && token_.text == "self")
                    fail("EXTENSION_SELF_SHADOW: explicit extension parameter cannot be named self");
                parameter.name = identifier();
                if (token_.text == "?")
                    fail("native optional parameter presence '?' requires versioned default/presence ABI lowering");
                value.parameters.push_back(std::move(parameter));
            } while (accept(","));
            require(")");
        }
        for (const auto& parameter : value.parameters)
            if (!parameter.class_name.empty()) class_receivers_.insert(parameter.name);
        const bool arrow_return_type = !accept(":");
        if (arrow_return_type) require("->");
        value.returns_void = accept("void");
        if (!value.returns_void) {
            if (accept("int")) {
                value.return_type = ScalarType::I64;
            } else if (accept("bool")) {
                value.return_type = ScalarType::Bool;
            } else if (accept("string") || accept("String")) {
                value.return_type = ScalarType::String;
            } else if (accept_pending_scalar_alias(value.pending_return_type_alias)) {
                // Deferred scalar aliases are admitted only after full resolution.
            } else if (token_.kind == Kind::Identifier &&
                       type_only_nominals_.count(std::string(token_.text))) {
                fail("TYPE_ALIAS_KIND_MISMATCH: trait/interface cannot be a scalar return type");
            } else {
                const std::size_t type_offset = token_.offset;
                const std::string type = type_identifier();
                if (entry_builtin_name(type)) reject_native_entry_boundary(type_offset);
                const bool generic = std::any_of(value.generic_parameters.begin(),
                    value.generic_parameters.end(),
                    [&](const GenericParameter& p) { return p.name == type; });
                if (is_sum_name(type)) value.return_sum = type;
                else if (!generic) value.return_class = type;
                value.return_type = ScalarType::I64;
            }
        }
        if (token_.text == "?")
            fail("native optional return type '?' requires versioned Option/default ABI lowering");
        if (accept("using")) {
            value.has_declared_effects = true;
            do {
                std::string name = identifier();
                if (accept(".")) name += "." + identifier();
                FunctionEffect effect;
                try { effect = FunctionEffects::parse(name); }
                catch (const std::invalid_argument&) {
                    fail("unknown function effect '" + name + "'");
                }
                const auto bit = static_cast<std::uint32_t>(effect);
                if (value.declared_effects.bits & bit)
                    fail("duplicate function effect '" + name + "'");
                value.declared_effects.bits |= bit;
            } while (accept(","));
        }
        if (!owner.empty() && value.has_declared_effects)
            fail("native method using effects require receiver effect summaries");
        if (accept("requires")) {
            // Never accept an uncertified realtime property as a mere annotation.
            fail("native requires clause awaits verified WCET/allocation/blocking analysis");
        }
        // End-delimited class methods use the same Function / hidden-receiver
        // representation as braced methods. Until general structured-body
        // lowering is shared, permit only an empty/return-only void body or
        // one scalar terminal return; reject every other statement.
        // This is not an alternate arity or method ABI.
        if (token_.text == "::") {
            if (owner.empty()) fail("native '::' body requires a class method");
            if (!arrow_return_type)
                fail("native '::' method requires '->' return syntax");
            require("::");
            if (value.returns_void) {
                if (accept("return")) {
                    if (!accept(";") && token_.text != "end")
                        fail("native end-delimited void method requires terminal return");
                }
            } else {
                require("return");
                value.result = expression();
                if (!accept(";") && token_.text != "end")
                    fail("native end-delimited method requires terminal return");
            }
            if (token_.text != "end")
                fail("native end-delimited method supports only one terminal return");
            require("end");
            return value;
        }
        require("{");
        auto array_effect_ahead = [&] {
            return peek_next().text == "." && std::any_of(value.locals.begin(), value.locals.end(),
                [&](const LocalBinding& local) { return local.int_array && local.name == token_.text; });
        };
        auto source_task_local = [&]() {
            if (!(token_.text == "let" && peek_next().text == "Task"))
                return false;
            if (!owner.empty() || value.name != "main" ||
                    !value.parameters.empty())
                fail("native Task<int> v1 requires zero-argument top-level main");
            if (!value.source_tasks.empty())
                fail("native Task<int> v1 supports exactly one source Task");
            if (!value.locals.empty() || !value.actor_spawns.empty() ||
                    !value.ready_future_name.empty())
                fail("native Task<int> v1 must be the first lexical binding");

            SourceTaskI64Binding task;
            task.offset = token_.offset;
            require("let");
            require("Task");
            require("<");
            require("int");
            require(">");
            task.name = identifier();
            require("=");
            require("task");
            require("{");
            require("return");
            task.result = expression();
            require(";");
            require("}");
            require(";");
            if (!native_capture_free_task_tree(*task.result))
                fail("native Task<int> v1 producer must be capture-free and scalar");
            value.source_tasks.push_back(std::move(task));
            return true;
        };
        auto source_future_pipeline_local = [&]() {
            if (!((token_.text == "let" || token_.text == "val" ||
                    token_.text == "const") && peek_next().text == "Future"))
                return false;
            Lexer look = lexer_;
            const auto future = look.next();
            const auto open = look.next();
            const auto payload = look.next();
            if (future.text != "Future" || open.text != "<" ||
                    payload.text != "int")
                return false;
            if (!owner.empty() || value.name != "main" ||
                    !value.parameters.empty())
                fail("native Future<int> pipeline v1 requires zero-argument top-level main");
            if (!value.future_pipelines.empty() || !value.source_tasks.empty())
                fail("native Future<int> pipeline v1 supports exactly one live Future");
            if (!value.locals.empty() || !value.actor_spawns.empty() ||
                    !value.ready_future_name.empty())
                fail("native Future<int> pipeline v1 must be the first lexical binding");

            SourceFuturePipelineI64Binding pipeline;
            pipeline.offset = token_.offset;
            advance();
            require("Future");
            require("<");
            require("int");
            require(">");
            pipeline.name = identifier();
            require("=");

            if (accept("rt")) {
                require("take");
                pipeline.root_consumed = true;
                if (token_.text == "(")
                    fail("taking an already-lifted pipeline is not yet supported");
            }

            pipeline.producer_function = identifier();
            require("(");
            require(")");
            if (token_.text != "|>")
                fail("native Future<int> pipeline v1 requires at least one |> stage");
            while (accept("|>")) {
                if (pipeline.stages.size() >= 128)
                    fail("Future pipeline exceeds 128 stages");
                pipeline.stages.push_back(identifier());
                if (accept("(")) {
                    if (token_.text != ")")
                        fail("native Future<int> pipeline stage requires no extra arguments");
                    require(")");
                }
            }
            require(";");
            value.future_pipelines.push_back(std::move(pipeline));
            return true;
        };
        auto ready_future_local = [&]() {
            if (!((token_.text == "const" || token_.text == "val") &&
                    peek_next().text == "Future")) return false;
            if (!value.ready_future_name.empty())
                fail("native source supports only one Future<void> readiness token");
            advance();
            require("Future");
            require("<");
            if (!accept("void"))
                fail("Future<T> requires native continuation lowering");
            require(">");
            value.ready_future_name = identifier();
            require("=");
            value.ready_future_actor = identifier();
            require(".");
            require("get_ready");
            require("(");
            require(")");
            const bool bound = std::any_of(value.actor_spawns.begin(),
                value.actor_spawns.end(), [&](const ActorSpawnBinding& spawn) {
                    return spawn.name == value.ready_future_actor;
                });
            if (!bound)
                fail("Future<void> requires previously spawned ActorRef.get_ready");
            require(";");
            return true;
        };
        while (token_.text == "const" || token_.text == "val" || token_.text == "let" || token_.text == "stdio" || array_effect_ahead()) {
            // Typed Future<void> is admitted ONLY as a linear lexical alias
            // for one already-spawned ActorRef.get_ready() lifecycle promise.
            // No scalar/object borrow crosses this suspension, and general
            // Future<T> / async lowering must remain fail-closed.
            if (source_task_local()) continue;
            if (source_future_pipeline_local()) continue;
            if (ready_future_local()) continue;
            LocalBinding binding;
            binding.offset = token_.offset;
            Lexer sum_look = lexer_; auto sum_first = sum_look.next();
            if (token_.text == "let" && sum_first.text == "mut") sum_first = sum_look.next();
            if ((token_.text == "val" || token_.text == "const" || token_.text == "let") &&
                    (sum_first.text == "int" || sum_first.text == "bool" || sum_first.text == "String" || sum_first.text == "string")) {
                const auto mode = token_.text; advance(); if (mode == "let") accept("mut");
                binding.owner_mutable = mode != "const"; binding.assignable = mode == "let";
                binding.scalar_type = accept("bool") ? ScalarType::Bool : (accept("String") || accept("string")) ? ScalarType::String : ScalarType::I64;
                if (binding.scalar_type == ScalarType::I64) require("int");
                binding.name = identifier();
                require("=");
                if (accept("await")) {
                    if (binding.scalar_type != ScalarType::I64)
                        fail("native scalar await requires an int result binding");
                    if (!value.locals.empty())
                        fail("native first async slice requires await to immediately consume its Future/Task");
                    const std::string awaited_name = identifier();
                    auto task = std::find_if(
                        value.source_tasks.begin(), value.source_tasks.end(),
                        [&](const SourceTaskI64Binding& item) {
                            return item.name == awaited_name;
                        });
                    if (task != value.source_tasks.end()) {
                        if (binding.name == awaited_name)
                            fail("Task<int> await result cannot shadow its Task binding");
                        if (task->awaited)
                            fail("source Task<int> must be awaited exactly once");
                        task->awaited = true;
                        binding.task_await_name = awaited_name;
                    } else {
                        auto pipeline = std::find_if(
                            value.future_pipelines.begin(),
                            value.future_pipelines.end(),
                            [&](const SourceFuturePipelineI64Binding& item) {
                                return item.name == awaited_name;
                            });
                        if (pipeline == value.future_pipelines.end())
                            fail("await requires the live source Future<int> pipeline binding");
                        if (binding.name == awaited_name)
                            fail("Future<int> await result cannot shadow its Future binding");
                        if (pipeline->awaited)
                            fail("source Future<int> pipeline must be awaited exactly once");
                        pipeline->awaited = true;
                        binding.future_await_name = awaited_name;
                    }
                } else {
                    binding.initializer = expression();
                }
            } else if ((token_.text == "val" || token_.text == "const" || token_.text == "let") &&
                    (sum_first.text == "Option" || sum_first.text == "Result")) {
                const auto mode = token_.text; advance(); binding.assignable = mode == "let"; binding.owner_mutable = mode == "val" || mode == "let"; if (mode == "let") accept("mut");
                binding.sum_type = type_identifier(); binding.name = identifier(); require("="); binding.initializer = expression();
            } else if (array_effect_ahead()) {
                binding.name = "$effect" + std::to_string(value.locals.size());
                binding.initializer = primary();
                if (binding.initializer->kind != Expression::Kind::ArrayCall ||
                        (binding.initializer->name != "add" && binding.initializer->name != "set"))
                    fail("native array effect supports only add(value) or set(index,value)");
            } else if (token_.text == "stdio") {
                // ANF: discard an effect's integer status in an unspellable
                // compiler-owned slot, preserving declaration/effect order.
                binding.name = "$effect" + std::to_string(value.locals.size());
                binding.initializer = primary();
            } else if (token_.text == "val" && peek_next().text == "Array") {
                advance(); require("Array"); require("<");
                array_element(binding);
                require(">"); binding.name = identifier(); require("=");
                require("new"); require("Array"); require("<");
                if (!binding.array_record_class.empty()) {
                    if (type_identifier() != binding.array_record_class) fail("array constructor element type mismatch");
                } else if (binding.array_element_type == ScalarType::String) {
                    if (!accept("String")) require("string");
                } else require("int");
                require(">"); require("("); require(")");
                binding.int_array = binding.owner_mutable = true;
            } else if ((token_.text == "val" || token_.text == "const") &&
                    [&] { Lexer look = lexer_; const auto type = look.next();
                        return type.text == "string" || type.text == "String" ||
                            (type.kind == Kind::Identifier && look.next().kind == Kind::Identifier); }()) {
                const bool mutable_value = accept("val");
                if (!mutable_value) require("const");
                binding.owner_mutable = mutable_value;
                const std::string type = type_identifier();
                binding.name = identifier();
                require("=");
                if (type == "string" || type == "String") {
                    binding.scalar_type = ScalarType::String;
                    binding.initializer = expression();
                } else {
                    binding.class_name = type;
                    if (!accept("new")) {
                        binding.initializer = expression();
                        if (binding.initializer->kind != Expression::Kind::Call &&
                                binding.initializer->kind != Expression::Kind::Conditional &&
                                binding.initializer->kind != Expression::Kind::ArrayCall)
                            fail("native class initializer requires new or immutable record value");
                        binding.record_view = true;
                    } else {
                    if (type_identifier() != type) fail("class constructor type mismatch");
                    require("(");
                    if (!accept(")")) {
                        do binding.constructor_arguments.push_back(expression()); while (accept(","));
                        require(")");
                    }
                    binding.new_class = true;
                    }
                }
            } else if (accept("let")) {
                // Preserve the Java/Graal distinction between immutable
                // let Class and exclusively borrowable let mut Class.
                binding.owner_mutable = accept("mut");
                // Class construction is stack-only. No reference to an
                // uninitialized, moved, or temporary class can escape.
                if (token_.text == "bool") {
                    advance(); binding.scalar_type = ScalarType::Bool;
                    binding.name = identifier(); require("=");
                    binding.initializer = expression();
                } else if (token_.text == "Array") {
                    // Native slice #265: only a lexical, exclusively owned Array<int>.
                    // No implicit copies, fields, captures, cross-call transfers, or
                    // suspension can enter the current synchronous array ABI.
                    if (!binding.owner_mutable)
                        fail("native Array<int> requires let mut owner");
                    require("Array");
                    require("<");
                    array_element(binding);
                    require(">");
                    binding.name = identifier();
                    require("=");
                    require("new");
                    require("Array");
                    require("<");
                    if (!binding.array_record_class.empty()) {
                        if (type_identifier() != binding.array_record_class) fail("array constructor element type mismatch");
                    } else if (binding.array_element_type == ScalarType::String) {
                        if (!accept("String")) require("string");
                    } else require("int");
                    require(">");
                    require("(");
                    require(")");
                    binding.int_array = true;
                } else {
                binding.class_name = type_identifier();
                binding.name = identifier();
                require("=");
                if (accept("new")) {
                    const std::string ctor = type_identifier();
                    if (ctor != binding.class_name) fail("class constructor type mismatch");
                    require("(");
                    if (!accept(")")) {
                        do binding.constructor_arguments.push_back(expression());
                        while (accept(","));
                        require(")");
                    }
                    binding.new_class = true;
                } else {
                    require("rt");
                    require("take");
                    const bool parenthesized = accept("(");
                    binding.move_source = identifier();
                    if (parenthesized) require(")");
                    binding.move_class = true;
                }
                }
            } else {
                // A val owns mutable scalar storage; const permits only shared
                // loans. Preserve the distinction for native ownership checks.
                binding.owner_mutable = token_.text == "val";
                binding.assignable = token_.text == "let";
                binding.owner_mutable = binding.owner_mutable || binding.assignable;
                advance();
                binding.name = identifier();
                // Distinguish actor capabilities from scalar data before IR.
                // Unsupported generic futures never become fake blocking calls.
                if (token_.text != ":" && token_.text != "=") require(":");
                const bool explicit_type = accept(":");
                SourceType declared_type;
                bool typed_actor_ref = false;
                if (explicit_type) {
                    if (accept("ActorRef")) {
                        require("<");
                        declared_type.kind = SourceType::Kind::ActorRef;
                        declared_type.argument = identifier();
                        require(">");
                        typed_actor_ref = true;
                    } else if (accept("Future")) {
                        require("<");
                        declared_type.kind = SourceType::Kind::Future;
                        if (accept("int")) declared_type.argument = "int";
                        else declared_type.argument = identifier();
                        require(">");
                        fail("Future<T> requires native continuation lowering");
                    } else {
                        if (token_.text == "Entry")
                            reject_native_entry_boundary(token_.offset);
                        if (token_.text == "owned" && !experimental_owned_int_)
                            fail("experimental owned int requires --experimental-owned-int");
                        binding.heap_owned = experimental_owned_int_ && accept("owned");
                        if (accept("int")) {
                            binding.scalar_type = ScalarType::I64;
                        } else if (accept("bool")) {
                            if (binding.heap_owned)
                                fail("native owned bool region lowering is not admitted");
                            binding.scalar_type = ScalarType::Bool;
                        } else if (accept("string") || accept("String")) {
                            if (binding.heap_owned) fail("owned string qualifier is unsupported");
                            binding.scalar_type = ScalarType::String;
                        } else if (token_.kind == Kind::Identifier &&
                                   scalar_alias_targets_.count(std::string(token_.text))) {
                            binding.pending_type_alias = identifier();
                        } else if (token_.kind == Kind::Identifier &&
                                   type_only_nominals_.count(std::string(token_.text))) {
                            fail("TYPE_ALIAS_KIND_MISMATCH: trait/interface cannot be a scalar local");
                        } else {
                            fail("native scalar local requires int, bool or checked scalar alias");
                        }
                    }
                }
                require("=");
                if (accept("spawn")) {
                    if (explicit_type && !typed_actor_ref)
                        fail("spawn initializer requires ActorRef<T> type or inference");
                    ActorSpawnBinding spawned;
                    spawned.name = binding.name;
                    spawned.offset = binding.offset;
                    spawned.actor_type = identifier();
                    spawned.declared_type.kind = SourceType::Kind::ActorRef;
                    spawned.declared_type.argument =
                        typed_actor_ref ? declared_type.argument : spawned.actor_type;
                    if (spawned.declared_type.argument != spawned.actor_type)
                        fail("ActorRef<T> type mismatch with spawned actor");
                    require("(");
                    require(")");
                    require(";");
                    value.actor_spawns.push_back(std::move(spawned));
                    continue;
                }
                if (typed_actor_ref) fail("ActorRef<T> requires spawn initializer");
                if (!explicit_type) fail("scalar local requires an explicit ': int' or ': bool' type");
                binding.initializer = expression();
            }
            require(";");
            if (!binding.class_name.empty()) class_receivers_.insert(binding.name);
            value.locals.push_back(std::move(binding));
        }
        for (const auto& task : value.source_tasks)
            if (!task.awaited)
                fail("source Task<int> must be awaited exactly once");

        // Restrict the first general-source CFG slice to synchronous
        // if/else arms containing field writes or direct function calls.
        // They may change owned scalar state only when both predecessors
        // agree at the join; checked below by the ownership verifier.
        std::function<std::vector<Statement>(bool, bool)> branch_arm;
        std::function<Statement()> foreach_statement;
        auto scoped_local = [&]() {
            Statement statement; statement.kind = Statement::Kind::ScopedLocal;
            statement.offset = token_.offset;
            const bool mutable_slot = token_.text == "let";
            const bool mutable_referent = token_.text != "const";
            advance(); if (mutable_slot) accept("mut");
            if (token_.text == "owned") fail("branch-local owned value requires destructor/drop lowering");
            if (token_.text == "Array")
                fail("ordered owning local requires scoped region/drop lowering");
            auto local = std::make_unique<LocalBinding>(); local->offset = statement.offset;
            local->assignable = mutable_slot; local->owner_mutable = mutable_referent;
            auto type_name = [&]() -> std::string {
                for (const auto* scalar : {"int", "bool", "string", "String"})
                    if (accept(scalar)) return scalar;
                return type_identifier();
            };
            const std::string first = type_name(); std::string type;
            if (accept(":")) { local->name = first; type = type_name(); }
            else { type = first; local->name = identifier(); }
            if (type == "owned") fail("branch-local owned value requires destructor/drop lowering");
            if (type == "Array") fail("ordered owning local requires scoped region/drop lowering");
            if (type == "int") local->scalar_type = ScalarType::I64;
            else if (type == "bool") local->scalar_type = ScalarType::Bool;
            else if (type == "String" || type == "string") local->scalar_type = ScalarType::String;
            else if (scalar_alias_targets_.count(type)) local->pending_type_alias = type;
            else if (type_only_nominals_.count(type))
                fail("TYPE_ALIAS_KIND_MISMATCH: trait/interface cannot be a scalar branch local");
            else if (is_sum_name(type)) local->sum_type = type;
            else {
                if (mutable_slot) fail("native branch-local class owner requires destructor/drop lowering");
                local->class_name = type; local->record_view = true;
            }
            require("="); local->initializer = expression(); require(";");
            if (!local->class_name.empty()) class_receivers_.insert(local->name);
            statement.branch_local = std::move(local); return statement;
        };
        std::size_t statement_depth = 0;
        foreach_statement = [&]() {
            Lexer look = lexer_;
            const auto open = look.next(), mode = look.next(), name = look.next(), of = look.next();
            if (open.text == "(" && mode.text == "let" && name.text == "int") {
                Statement loop; loop.kind = Statement::Kind::ForIndex; loop.offset = token_.offset;
                require("for"); require("("); require("let"); require("int");
                loop.branch_local = std::make_unique<LocalBinding>();
                loop.branch_local->name = identifier(); loop.branch_local->offset = loop.offset;
                loop.branch_local->owner_mutable = true; loop.branch_local->assignable = true;
                require("="); loop.branch_local->initializer = expression(); require(";");
                loop.condition = expression(); require(";");
                if (identifier() != loop.branch_local->name) fail("native index loop must update its own counter");
                require("="); loop.loop_step = expression(); require(")"); require("{");
                loop.then_statements = branch_arm(false, false); require("}"); return loop;
            }
            if (open.text != "(" || (mode.text != "const" && mode.text != "val") ||
                    name.kind != Kind::Identifier || of.text != "of")
                fail("native realtime source loop requires verified LLVM continuation lowering");
            Statement loop;
            loop.kind = Statement::Kind::ForEach;
            loop.offset = token_.offset;
            require("for"); require("(");
            if (!accept("const")) require("val");
            loop.branch_local = std::make_unique<LocalBinding>();
            loop.branch_local->offset = token_.offset;
            loop.branch_local->name = identifier();
            require("of"); loop.expression = expression(); require(")"); require("{");
            const auto receivers = class_receivers_;
            class_receivers_.insert(loop.branch_local->name);
            loop.then_statements = branch_arm(false, false);
            class_receivers_ = receivers;
            require("}"); return loop;
        };
        branch_arm = [&](bool finally_scope, bool then_scope) {
            if (++statement_depth > 16) fail("native structured statement depth exceeds 16");
            struct Depth { std::size_t& n; ~Depth() { --n; } } depth{statement_depth};
            struct Receivers { std::unordered_set<std::string>& target; std::unordered_set<std::string> saved;
                ~Receivers() { target = std::move(saved); } } receivers{class_receivers_, class_receivers_};
            std::vector<Statement> actions;
            while (token_.text != "}" && (!then_scope || (token_.text != "fi" && token_.text != "else" && token_.text != "elif"))) {
                if (token_.kind == Kind::End || actions.size() == 16)
                    fail("native conditional arm limit exceeded (16)");
                if (token_.text == "for" && !finally_scope) {
                    actions.push_back(foreach_statement()); continue;
                }
                if (token_.text == "while" || token_.text == "for" ||
                        token_.text == "loop") {
                    fail("native realtime source loop requires verified LLVM continuation lowering");
                }
                if (token_.text == "rt") {
                    advance();
                    if (token_.text == "cooperate") {
                        throw std::runtime_error(
                            "native rt cooperate requires verified heap-owned continuation lowering"
                            " at byte " + std::to_string(token_.offset));
                    }
                    fail("unsupported native runtime operation in conditional arm");
                }
                Statement statement;
                statement.offset = token_.offset;
                if (token_.text == "if" && !finally_scope) {
                    advance(); statement.kind = Statement::Kind::Conditional;
                    statement.condition = expression();
                    if (accept(";")) {
                        require("then"); statement.then_statements = branch_arm(false, true);
                        if (accept("else")) statement.else_statements = branch_arm(false, true);
                    } else {
                        require("{"); statement.then_statements = branch_arm(false, false); require("}");
                        if (accept("else")) { require("{"); statement.else_statements = branch_arm(false, false); require("}"); }
                    }
                    require("fi"); actions.push_back(std::move(statement)); continue;
                }
                if (finally_scope && (token_.text == "return" ||
                        token_.text == "break" || token_.text == "continue"))
                    fail("return/break/continue cannot escape finally; finally is cleanup-only");
                if (finally_scope && (token_.text == "defer" || token_.text == "try"))
                    fail("native finally nested defer/try requires scoped cleanup lowering");
                if (accept("return")) {
                    if (!value.return_class.empty()) fail("structured class return requires ownership-aware exit lowering");
                    statement.kind = Statement::Kind::Return;
                    if (!value.returns_void) statement.expression = expression();
                    require(";"); actions.push_back(std::move(statement));
                    if (token_.text != "}" && (!then_scope || (token_.text != "fi" && token_.text != "else" && token_.text != "elif")))
                        fail("native statement follows terminal return");
                    continue;
                }
                if (token_.text == "const" || token_.text == "val" || token_.text == "let") {
                    actions.push_back(scoped_local());
                    continue;
                }
                auto target = primary();
                if (target->kind == Expression::Kind::Variable && accept("=")) {
                    statement.kind = Statement::Kind::ScalarWrite;
                    statement.receiver = target->name; statement.expression = expression();
                } else if (target->kind == Expression::Kind::Field && accept("=")) {
                    if (!target->left || target->left->kind != Expression::Kind::Variable)
                        fail("native conditional requires named class field");
                    statement.kind = Statement::Kind::FieldWrite;
                    statement.receiver = target->left->name;
                    statement.field = target->name;
                    statement.expression = expression();
                } else if (target->kind == Expression::Kind::Call ||
                           target->kind == Expression::Kind::ArrayCall || target->kind == Expression::Kind::Console) {
                    statement.kind = Statement::Kind::Call;
                    statement.expression = std::move(target);
                } else {
                    fail("native conditional arm requires field assignment or direct call");
                }
                require(";");
                actions.push_back(std::move(statement));
            }
            return actions;
        };
        bool return_guards_started = false;
        bool terminal_try_finally = false;
        std::size_t branch_count = 0;
        while (token_.text != "return") {
            if (token_.text == "while" && experimental_rt_scalar_loop_) {
                if (value.scalar_while || value.locals.size() != 1 ||
                        !value.statements.empty() || !value.early_returns.empty() ||
                        !value.actor_spawns.empty() || !value.actor_actions.empty() ||
                        value.has_declared_effects || !value.parameters.empty() ||
                        value.returns_void)
                    fail("experimental scalar loop requires one local and no effects");
                const LocalBinding& local = value.locals.front();
                if (!local.owner_mutable || local.heap_owned || local.int_array ||
                        !local.class_name.empty() || !local.initializer ||
                        local.initializer->kind != Expression::Kind::Integer)
                    fail("experimental scalar loop requires val scalar integer initializer");
                auto loop = std::make_unique<ScalarWhileLoop>();
                loop->counter = local.name;
                loop->initial = local.initializer->integer;
                require("while");
                require("(");
                if (identifier() != loop->counter)
                    fail("experimental scalar loop condition must read its counter");
                require("<");
                loop->upper_bound = integer_literal();
                require(")");
                require("{");
                if (identifier() != loop->counter)
                    fail("experimental scalar loop must update its counter");
                require("=");
                if (identifier() != loop->counter)
                    fail("experimental scalar loop cannot read another owner");
                require("+");
                if (integer_literal() != 1)
                    fail("experimental scalar loop increment must be exactly one");
                require(";");
                require("}");
                if (loop->initial < 0 || loop->upper_bound < loop->initial ||
                        loop->upper_bound > 1000000000LL)
                    fail("experimental scalar loop requires bounded nonnegative i64 range");
                value.scalar_while = std::move(loop);
                if (token_.text != "return")
                    fail("experimental scalar loop must be followed by a single return");
                continue;
            }
            if (token_.text == "for") {
                if (terminal_try_finally || return_guards_started)
                    fail("native for-of cannot enter protected/return-guard scope");
                value.statements.push_back(foreach_statement()); continue;
            }
            if (token_.text == "while" || token_.text == "for" ||
                    token_.text == "loop") {
                fail("native realtime source loop requires verified LLVM continuation lowering");
            }
            if (token_.text == "try") {
                if (terminal_try_finally || return_guards_started)
                    fail("native terminal try/finally must be a single return-only scope");
                terminal_try_finally = true;
                require("try");
                require("{");
                if (token_.text == "defer" || token_.text == "throw") {
                    // First typed-i64 exceptional CFG: one unconditional throw,
                    // LIFO scoped field cleanups, and a local catch. No await,
                    // exception-producing calls or nested try are admitted.
                    if (value.returns_void || !value.statements.empty() ||
                            !value.actor_actions.empty())
                        fail("native caught throw requires a single terminal int try scope");
                    // No partially inferred effect set can masquerade as a
                    // complete exception/cleanup summary at this boundary.
                    if (value.has_declared_effects)
                        throw std::runtime_error(
                            "native caught throw effect-qualified function unsupported at byte " +
                            std::to_string(token_.offset));
                    while (accept("defer")) {
                        if (value.caught_throw_defers.size() == 8)
                            fail("native caught throw defer limit exceeded (8)");
                        Statement deferred;
                        deferred.kind = Statement::Kind::FieldWrite;
                        deferred.offset = token_.offset;
                        auto target = primary();
                        if (target->kind != Expression::Kind::Field ||
                                !target->left ||
                                target->left->kind != Expression::Kind::Variable)
                            fail("native caught throw defer requires named class field write");
                        deferred.receiver = target->left->name;
                        deferred.field = target->name;
                        require("=");
                        deferred.expression = expression();
                        if (!native_pure_scalar_tree(*deferred.expression))
                            fail("native caught throw defer requires nonthrowing scalar/field expression");
                        require(";");
                        value.caught_throw_defers.push_back(std::move(deferred));
                    }
                    require("throw");
                    value.caught_throw_value = expression();
                    if (!native_pure_scalar_tree(*value.caught_throw_value))
                        fail("native caught throw requires nonthrowing int payload");
                    require(";");
                    require("}");
                    require("catch");
                    require("(");
                    value.catch_binding = identifier();
                    for (const auto& param : value.parameters)
                        if (param.name == value.catch_binding)
                            fail("native caught throw binding shadows parameter");
                    for (const auto& local : value.locals)
                        if (local.name == value.catch_binding)
                            fail("native caught throw binding shadows local owner");
                    require(")");
                    require("{");
                    require("return");
                    value.result = expression();
                    if (!native_pure_scalar_tree(*value.result))
                        fail("native caught throw catch return requires nonthrowing scalar/field read");
                    require(";");
                    require("}");
                    require("finally");
                    require("{");
                    auto cleanup = branch_arm(true, false);
                    require("}");
                    for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) {
                        if (it->kind != Statement::Kind::FieldWrite ||
                                !native_pure_scalar_tree(*it->expression))
                            fail("native caught throw finally requires nonthrowing field write");
                        it->kind = Statement::Kind::Defer;
                        value.statements.push_back(std::move(*it));
                    }
                    require("}");
                    return value;
                }
                continue;
            }
            // Instance void methods may have empty bodies or fall through,
            // just as in the Java reference frontend. The native entrypoint
            // also retains its existing implicit-success fallthrough. Non-void
            // functions continue to require an explicit final return.
            if (token_.text == "}" && value.returns_void &&
                    ((!owner.empty()) || (value.name == "main" && value.parameters.empty()))) {
                require("}");
                return value;
            }
            if (token_.kind == Kind::End || token_.text == "}") {
                if (return_guards_started) fail("expected 'return'");
                fail("missing final return");
            }
            // The initial native finally slice admits only guarded returns
            // and a terminal return in the protected region. No potentially
            // throwing calls, nested effects or suspension can be skipped.
            if (terminal_try_finally && token_.text != "if")
                fail("native terminal try supports only return guards and final return");
            if (token_.text == "if") {
                const auto offset = token_.offset;
                require("if");
                auto condition = expression();
                if (terminal_try_finally && !native_pure_scalar_tree(*condition))
                    fail("native protected try guard requires a pure nonthrowing bool expression");
                const bool then_form = accept(";");
                if (then_form) require("then"); else require("{");
                if (accept("return")) {
                    if (value.returns_void)
                        fail("guarded early return requires a scalar result");
                    if (value.early_returns.size() == 8)
                        fail("native guarded return limit exceeded (8)");
                    GuardedReturn guard;
                    guard.offset = offset;
                    guard.condition = std::move(condition);
                    guard.result = expression();
                    if (terminal_try_finally && !nonthrowing_protected_read(*guard.result))
                        fail("native protected try return requires nonthrowing scalar/field read");
                    require(";");
                    if (!then_form) require("}");
                    require("fi");
                    if (then_form && value.return_class.empty() && !terminal_try_finally) {
                        if (return_guards_started)
                            fail("native structured statements cannot follow guarded returns");
                        if (++branch_count > 128) fail("native structured return branch limit exceeded (128)");
                        Statement conditional; conditional.kind = Statement::Kind::Conditional; conditional.offset = offset;
                        conditional.condition = std::move(guard.condition);
                        Statement terminal; terminal.kind = Statement::Kind::Return; terminal.offset = offset;
                        terminal.expression = std::move(guard.result); conditional.then_statements.push_back(std::move(terminal));
                        value.statements.push_back(std::move(conditional));
                    } else {
                        value.early_returns.push_back(std::move(guard));
                        return_guards_started = true;
                    }
                } else {
                    if (terminal_try_finally)
                        fail("native terminal try supports only guarded return branches");
                    if (return_guards_started)
                        fail("native conditional statements cannot follow return guards");
                    if (++branch_count > 8)
                        fail("native conditional CFG limit exceeded (8)");
                    Statement conditional;
                    conditional.kind = Statement::Kind::Conditional;
                    conditional.offset = offset;
                    conditional.condition = std::move(condition);
                    conditional.then_statements = branch_arm(false, then_form);
                    if (!then_form) require("}");
                    if (accept("else")) {
                        if (!then_form) require("{");
                        conditional.else_statements = branch_arm(false, then_form);
                        if (!then_form) require("}");
                    } else if (!then_form) require("else");
                    require("fi");
                    value.statements.push_back(std::move(conditional));
                }
                continue;
            }
            if (return_guards_started)
                fail("native statements cannot follow guarded returns");
            if (ready_future_local()) continue;
            if (token_.text == "const" || token_.text == "val" || token_.text == "let") {
                value.statements.push_back(scoped_local()); continue;
            }
            if (token_.text == "await") {
                const std::size_t await_offset = token_.offset;
                advance();
                if (!value.ready_await_actor.empty() || !value.done_await_actor.empty())
                    fail("native source supports only one direct ActorRef lifecycle await");
                const std::string awaited = identifier();
                if (!value.ready_future_name.empty()) {
                    if (awaited != value.ready_future_name || token_.text != ";")
                        fail("native Future<void> await requires its declared readiness token");
                    if (!value.actor_actions.empty() || !value.statements.empty())
                        fail("await get_ready must precede post-await actor effects");
                    value.ready_await_actor = value.ready_future_actor;
                    value.ready_await_offset = await_offset;
                    value.ready_await_form = NativeAwaitForm::FutureValue;
                    require(";");
                    continue;
                }
                // ActorRef<T> implements HasFuture: `await actor;` is
                // equivalent to `await actor.get_ready();`, never get_done().
                // get_done remains an explicit physical-quiescence await.
                if (accept(";")) {
                    if (!value.actor_actions.empty() || !value.statements.empty())
                        fail("await get_ready must precede post-await actor effects");
                    value.ready_await_actor = awaited;
                    value.ready_await_offset = await_offset;
                    value.ready_await_form = NativeAwaitForm::DirectHasFuture;
                    continue;
                }
                if (token_.text != ".")
                    fail("native await requires HasFuture ActorRef or explicit get_ready()/get_done()");
                require(".");
                const std::string lifecycle = identifier();
                require("(");
                require(")");
                require(";");
                if (lifecycle == "get_ready") {
                    if (!value.actor_actions.empty() || !value.statements.empty())
                        fail("await get_ready must precede post-await actor effects");
                    value.ready_await_actor = awaited;
                    value.ready_await_offset = await_offset;
                    value.ready_await_form = NativeAwaitForm::ExplicitGetReady;
                } else if (lifecycle == "get_done") {
                    value.done_await_actor = awaited;
                    value.done_await_offset = await_offset;
                } else {
                    fail("native async/channel/select boundary unsupported: only ActorRef.get_ready/get_done is lowered");
                }
                continue;
            }
            if (token_.text != "defer" && exceptional_control(token_.text))
                fail("native exception/control effect unsupported");
            if (accept("defer")) {
                Statement deferred;
                deferred.kind = Statement::Kind::Defer;
                deferred.offset = token_.offset;
                auto target = primary();
                if (target->kind == Expression::Kind::Field && accept("=")) {
                    if (!target->left || target->left->kind != Expression::Kind::Variable)
                        fail("native deferred field assignment requires named class receiver");
                    deferred.receiver = target->left->name;
                    deferred.field = target->name;
                    deferred.expression = expression();
                } else if (target->kind == Expression::Kind::Call) {
                    deferred.expression = std::move(target);
                } else {
                    fail("native defer requires a direct function call or class field assignment");
                }
                require(";");
                value.statements.push_back(std::move(deferred));
                continue;
            }
            if (accept("stop")) {
                if (!value.done_await_actor.empty())
                    fail("actor effects after await get_done are unsupported");
                value.actor_stops.push_back(identifier());
                require(";");
                value.actor_actions.push_back({ActorAction::Kind::Stop,
                                               value.actor_stops.size() - 1});
                continue;
            }
            if (token_.text == "rt") {
                const auto intrinsic_offset = token_.offset;
                advance();
                if (accept("array_commit_filled")) {
                    // Restricted, compiler-only transaction over four
                    // owned array fields in declaration order.
                    Statement effect;
                    effect.kind = Statement::Kind::ArrayCommitFilled;
                    effect.offset = intrinsic_offset;
                    require("(");
                    effect.receiver = identifier();
                    require(",");
                    effect.array_commit_length = expression();
                    for (std::size_t i = 0; i < 4; ++i) {
                        require(",");
                        effect.array_commit_values.push_back(expression());
                    }
                    require(")");
                    require(";");
                    value.statements.push_back(std::move(effect));
                    continue;
                }
                if (token_.text == "cooperate") {
                    throw std::runtime_error(
                            "native rt cooperate requires verified heap-owned continuation lowering"
                            " at byte " + std::to_string(token_.offset));
                }
                fail("native rt statement unsupported until suspension lowering");
            }
            Statement statement;
            statement.offset = token_.offset;
            auto action = primary();
            if (action->kind == Expression::Kind::Field && action->name == "send"
                    && accept("(")) {
                if (!value.done_await_actor.empty())
                    fail("actor effects after await get_done are unsupported");
                // Nested class receivers cannot alias unrelated ActorRef names.
                if (!action->left || action->left->kind != Expression::Kind::Variable)
                    fail("native actor send requires direct named ActorRef receiver");
                ActorSend send;
                send.offset = statement.offset;
                send.reference = action->left->name;
                send.payload = expression();
                require(")");
                require(";");
                value.actor_sends.push_back(std::move(send));
                value.actor_actions.push_back({ActorAction::Kind::Send,
                                               value.actor_sends.size() - 1});
                continue;
            }
            if (action->kind == Expression::Kind::Variable && accept("=")) {
                statement.kind = Statement::Kind::ScalarWrite;
                statement.receiver = action->name; statement.expression = expression();
            } else if (action->kind == Expression::Kind::Field && accept("=")) {
                // No nested assignment may resolve through a flattened field.
                if (!action->left || action->left->kind != Expression::Kind::Variable)
                    fail("native field assignment requires direct named class receiver");
                statement.kind = Statement::Kind::FieldWrite;
                statement.receiver = action->left->name;
                statement.field = action->name;
                statement.expression = expression();
            } else if (action->kind == Expression::Kind::Call ||
                       action->kind == Expression::Kind::ArrayCall ||
                       action->kind == Expression::Kind::Console) {
                statement.kind = Statement::Kind::Call;
                statement.expression = std::move(action);
            } else {
                fail("expected class field assignment or function call statement");
            }
            require(";");
            value.statements.push_back(std::move(statement));
        }
        require("return");
        if (!value.returns_void) {
            value.result = expression();
            if (value.scalar_while &&
                    (value.result->kind != Expression::Kind::Variable ||
                     value.result->name != value.scalar_while->counter))
                fail("experimental scalar loop must return the counter");
            if (terminal_try_finally && !nonthrowing_protected_read(*value.result))
                fail("native protected try return requires nonthrowing scalar/field read");
        }
        if (!accept(";") && token_.text != "}") fail("expected ';' or closing return brace");
        if (terminal_try_finally) {
            require("}");
            // Java/Graal syntax requires catch. This native subset has no
            // throw-capable operations inside try; catch must be empty and
            // remains unreachable until typed exceptional CFG is implemented.
            require("catch");
            require("(");
            identifier();
            require(")");
            require("{");
            // Native try cannot throw: allow the Java-required catch return
            // only as a literal sentinel. It is unreachable by construction,
            // never lowered as a fake catch handler.
            if (accept("return")) {
                if (value.returns_void)
                    fail("native nonthrowing catch return requires int result");
                if (token_.kind != Kind::Integer)
                    fail("native nonthrowing catch requires only an integer literal return");
                integer_literal();
                require(";");
            }
            if (token_.text != "}")
                fail("native nonthrowing catch requires only an optional integer literal return");
            require("}");
            require("finally");
            require("{");
            auto cleanup = branch_arm(true, false);
            require("}");
            // The protected region always exits via an evaluated scalar
            // return. Lower its finally effects as scope-exit actions
            // registered in reverse order: finally runs in source order,
            // before any previously registered outer-scope defers.
            for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) {
                if (it->kind != Statement::Kind::Call &&
                        it->kind != Statement::Kind::FieldWrite)
                    fail("native finally permits only direct calls and class field writes");
                it->kind = Statement::Kind::Defer;
                value.statements.push_back(std::move(*it));
            }
        }
        require("}");
        return value;
    }

    std::unique_ptr<Expression> expression() { return conditional(); }

    std::unique_ptr<Expression> conditional() {
        if (++conditional_depth_ > kMaxNativeExpressionDepth) {
            --conditional_depth_;
            fail("native conditional expression nesting limit exceeded (64)");
        }
        struct ConditionalDepthGuard {
            std::size_t& depth;
            ~ConditionalDepthGuard() { --depth; }
        } depth_guard{conditional_depth_};

        auto condition = logical_or();
        if (!accept("?")) return condition;

        auto result = make_expression(Expression::Kind::Conditional, condition->offset);
        result->condition = std::move(condition);
        result->when_true = conditional();
        require(":");
        result->when_false = conditional();
        return result;
    }

    std::unique_ptr<Expression> logical_or() {
        auto left = logical_xor();
        while (token_.text == "||") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::LogicalOr, offset);
            expr->left = std::move(left);
            expr->right = logical_xor();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> logical_xor() {
        auto left = logical_and();
        while (token_.text == "^^") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::LogicalXor, offset);
            expr->left = std::move(left);
            expr->right = logical_and();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> logical_and() {
        auto left = equality();
        while (token_.text == "&&") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::LogicalAnd, offset);
            expr->left = std::move(left);
            expr->right = equality();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> equality() {
        auto left = comparison();
        while (token_.text == "==" || token_.text == "eq" ||
                token_.text == "!=" || token_.text == "neq") {
            const auto offset = token_.offset;
            const bool equal = token_.text == "==" || token_.text == "eq";
            advance();
            auto expr = make_expression(
                equal ? Expression::Kind::Equal : Expression::Kind::NotEqual, offset);
            expr->left = std::move(left);
            expr->right = comparison();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> comparison() {
        auto left = additive();
        while (token_.text == "<" || token_.text == "<=" ||
                token_.text == ">" || token_.text == ">=") {
            const auto offset = token_.offset;
            const auto op = token_.text;
            advance();
            auto kind = op == "<" ? Expression::Kind::Less
                : op == "<=" ? Expression::Kind::LessEqual
                : op == ">" ? Expression::Kind::Greater
                : Expression::Kind::GreaterEqual;
            auto expr = make_expression(kind, offset);
            expr->left = std::move(left);
            expr->right = additive();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> additive() {
        auto left = product();
        while (token_.text == "+" || token_.text == "-") {
            const auto offset = token_.offset;
            const auto op = token_.text;
            advance();
            auto expr = make_expression(op == "+" ? Expression::Kind::Add
                                                 : Expression::Kind::Subtract, offset);
            expr->left = std::move(left);
            expr->right = product();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> product() {
        auto left = unary();
        while (token_.text == "*" || token_.text == "/" || token_.text == "%") {
            // Java defines int/int division as a FLOAT result; this native
            // source compiler lowers expressions only to signed i64. Reject
            // at the operator before either lowering or emitting partial IR.
            if (token_.text == "/")
                fail("native floating division unsupported: int/int yields float in Java; "
                     "LLVM float typing and zero-divisor semantics are not implemented");
            if (token_.text == "%")
                fail("native integer remainder unsupported: zero-divisor and overflow contracts "
                     "are not implemented");
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Multiply, offset);
            expr->left = std::move(left);
            expr->right = unary();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> unary() {
        if (++expression_depth_ > kMaxNativeExpressionDepth) {
            --expression_depth_;
            fail("native expression nesting limit exceeded (64)");
        }
        struct DepthGuard {
            std::size_t& depth;
            ~DepthGuard() { --depth; }
        } depth_guard{expression_depth_};

        if (token_.text == "trap") {
            // Reserved for Option-valued ordinary-error capture. Reject it
            // before IR generation: accepting a normal value here would erase
            // the guest failure channel and violate panic/raise precedence.
            fail("native trap expression unsupported: typed throw effects and cleanup lowering required");
        }
        if (token_.text == "rt") {
            const auto offset = token_.offset;
            advance();

            bool mutable_borrow = false;
            std::string operation(token_.text);
            if (operation == "mut") {
                advance();
                if (token_.text != "borrow")
                    fail("'rt mut' only supports borrowing; use 'rt mut borrow value'");
                operation = "borrow";
                mutable_borrow = true;
            }
            if (operation == "cooperate") {
                throw std::runtime_error(
                            "native rt cooperate requires verified heap-owned continuation lowering"
                            " at byte " + std::to_string(token_.offset));
            }
            if (operation == "ref" || operation == "deref" ||
                operation == "unref" || operation == "raw") {
                fail("pointer-style rt operation is not valid Oreslang source; "
                     "use rt copy/share/take/borrow or rt mut borrow");
            }
            if (operation != "borrow" && operation != "copy" &&
                operation != "take" && operation != "share") {
                fail("unsupported native rt operation; suspension/ownership lowering is unavailable");
            }
            advance();
            if (!mutable_borrow && operation == "borrow" &&
                    token_.text == "mut") {
                fail("legacy 'rt borrow mut value' is not accepted; use 'rt mut borrow value'");
            }
            if (mutable_borrow && token_.text == "mut")
                fail("duplicate mutable borrow qualifier");
            auto result = make_expression(
                operation == "copy" ? Expression::Kind::Copy :
                operation == "take" ? Expression::Kind::Take :
                operation == "share" ? Expression::Kind::Share :
                mutable_borrow ? Expression::Kind::BorrowMut : Expression::Kind::Borrow,
                offset);
            if (accept("(")) {
                result->left = expression();
                require(")");
            } else {
                result->left = unary();
            }
            return result;
        }
        if (token_.text == "+") {
            // Unary plus is an int-only identity, not a truthy or String
            // coercion. Reuse typed addition with a side-effect-free zero
            // operand: the existing scalar checker then rejects +bool and
            // +String before any LLVM IR is published. Nested plus remains
            // subject to the same expression-depth budget as unary minus.
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Add, offset);
            expr->left = make_expression(Expression::Kind::Integer, offset);
            expr->left->integer = 0;
            expr->right = unary();
            return expr;
        }
        if (token_.text == "-") {
            const auto offset = token_.offset;
            advance();
            // Exact i64 minimum has positive magnitude 2^63, so it cannot
            // first pass the positive signed-literal parser. Keep the
            // exception local to a directly negated, valid decimal token.
            // Padded leading-zero spellings are valid only when separated
            // by a digit on both sides of each underscore.
            if (token_.kind == Kind::Integer) {
                std::string digits(token_.text);
                if (!digits.empty() && digits.back() != '_' &&
                    digits.find("__") == std::string::npos) {
                    digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                    // Zero-padded decimal spellings denote the same magnitude.
                    // Removing the padding only inside the direct-negation
                    // exception preserves all positive i64 overflow checks.
                    digits.erase(0, digits.find_first_not_of('0'));
                    if (digits == "9223372036854775808") {
                        auto expr = make_expression(Expression::Kind::Integer, offset);
                        expr->integer = std::numeric_limits<std::int64_t>::min();
                        advance();
                        return expr;
                    }
                }
            }
            auto expr = make_expression(Expression::Kind::Negate, offset);
            expr->left = unary();
            return expr;
        }
        if (token_.text == "!") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::LogicalNot, offset);
            expr->left = unary();
            return expr;
        }
        return primary();
    }

    std::unique_ptr<Expression> primary() {
        const auto offset = token_.offset;
        if (accept("new")) {
            auto expr = make_expression(Expression::Kind::RecordConstruct, offset);
            expr->name = type_identifier();
            require("(");
            if (!accept(")")) {
                do expr->arguments.push_back(expression()); while (accept(","));
                require(")");
            }
            return expr;
        }
        if (token_.kind == Kind::String) {
            auto expr = make_expression(Expression::Kind::String, offset);
            expr->name = string_literal(token_.text);
            advance();
            return expr;
        }
        if (token_.kind == Kind::Template) {
            const std::string_view raw = token_.text;
            advance();
            return template_literal(raw, offset);
        }
        if (accept("stdio")) {
            require(".");
            auto expr = make_expression(Expression::Kind::Console, offset);
            expr->name = identifier();
            if (expr->name != "print" && expr->name != "println") fail("unsupported native stdio operation");
            // Java stdio.print/println concatenate arguments without an
            // inserted separator and append at most one newline. Lower all
            // string arguments to the existing checked string-concat AST so
            // evaluation order, type admission, effects and region ownership
            // are identical to ordinary string concatenation.
            require("(");
            expr->left = expression();
            std::size_t argc = 1;
            while (accept(",")) {
                if (++argc > 32) fail("native stdio argument count exceeds 32");
                auto joined = make_expression(Expression::Kind::Add, offset);
                joined->left = std::move(expr->left);
                joined->right = expression();
                expr->left = std::move(joined);
            }
            require(")");
            return expr;
        }
        if (accept("(")) {
            // Java/Graal's () is a zero-slot Tuple, not grouping.
            // Native scalar lowering must reject all tuple arities before IR.
            if (token_.text == ")") {
                fail("native tuple literal unsupported: fixed product layout and ownership lowering are not implemented");
            }
            auto inner = expression();
            if (token_.text == ",") {
                fail("native tuple literal unsupported: fixed product layout and ownership lowering are not implemented");
            }
            require(")");
            return inner;
        }
        if (token_.kind == Kind::Integer) {
            auto expr = make_expression(Expression::Kind::Integer, offset);
            expr->integer = integer_literal();
            return expr;
        }
        if (token_.text == "true" || token_.text == "false") {
            auto expr = make_expression(Expression::Kind::Boolean, offset);
            expr->boolean = token_.text == "true";
            advance();
            return expr;
        }
        // Explicit brace tuples and nominal Entry values have no LLVM native
        // product ABI. Do not lower them as scalars, and do not steal ordinary
        // calls to a user function named tuple(...).
        if (token_.text == "tuple") {
            Lexer lookahead = lexer_;
            if (lookahead.next().text == "{") {
                fail("native tuple literal unsupported: fixed product layout and ownership lowering are not implemented");
            }
        }
        if (token_.text == "Entry") {
            Lexer lookahead = lexer_;
            const Token next = lookahead.next();
            // '<' in native expressions is currently parsed as generic-call
            // syntax, so Entry<...> must fail before scalar lowering.
            if (next.text == "." || next.text == "<") {
                fail("native Entry<K,V> unsupported: nominal key/value product layout and ownership lowering are not implemented");
            }
        }
        // 'task' is contextual in Oreslang v2. Preserve ordinary identifiers
        // named task, but fail closed when it opens a task block.
        if (token_.text == "task") {
            Lexer lookahead = lexer_;
            if (lookahead.next().text == "{")
                fail("native execution task lowering unsupported: scheduler-owned Task/Future continuation required; no implicit inline execution");
        }
        // Opaque ActorGroup and join permits are runtime-affine capabilities.
        // Never silently admit a native scalar name or emit partial IR.
        if (token_.text == "ActorGroup" ||
            token_.text == "ActorGroupJoinCapability")
            fail("native ActorGroup source lowering unsupported: "
                 "typed membership capabilities and lifecycle authority required");
        const bool extension_self = extension_header_active_ && token_.text == "self";
        if (token_.kind != Kind::Identifier || (reserved(token_.text) && token_.text != "pattern" &&
                !sum_variant(token_.text) &&
                !(token_.text == "self" && (!method_scope_.empty() || extension_self)))) {
            fail("expected int expression");
        }
        auto expr = make_expression(Expression::Kind::Variable, offset);
        if (token_.text == "self" && (!method_scope_.empty() || extension_self)) {
            expr->name = "self";
            advance();
        }
        else if (sum_variant(token_.text)) { expr->name = token_.text; advance(); }
        else expr->name = identifier();
        if (module_names_.count(expr->name) && accept(".")) expr->name += "." + identifier();
        if (generic_call_type_arguments_ahead()) {
            require("<");
            expr->type_arguments_present = true;
            if (!accept(">")) {
                bool seen_named = false;
                do {
                    GenericTypeArgument argument;
                    if (token_.kind == Kind::Identifier && token_.text != "int") {
                        Lexer lookahead = lexer_;
                        if (lookahead.next().text == "=") {
                            argument.name = identifier();
                            require("=");
                            seen_named = true;
                        }
                    }
                    if (argument.name.empty() && seen_named)
                        fail("positional generic argument after named argument");
                    if (accept("_")) argument.infer = true;
                    else if (accept("int")) argument.type = "int";
                    else if (token_.kind == Kind::Identifier &&
                             scalar_alias_targets_.count(std::string(token_.text))) {
                        std::string alias = identifier();
                        if (resolve_scalar_alias(alias) != ScalarType::I64)
                            fail("native scalar generic type requires int ABI");
                        argument.type = "int";
                    } else fail("native scalar generic type requires int or bare '_' (non-scalar generics unsupported)");
                    expr->type_arguments.push_back(std::move(argument));
                } while (accept(","));
                require(">");
            }
            if (token_.text != "(")
                fail("generic type arguments require a call");
        }
        if (accept("(")) {
            expr->kind = Expression::Kind::Call;
            bool named_seen = false;
            std::unordered_set<std::string> labels;
            if (!accept(")")) {
                do {
                    if (named_argument_ahead()) {
                        std::string label = identifier();
                        require(":");
                        if (!labels.insert(label).second) {
                            fail("duplicate named argument '" + label + "'");
                        }
                        named_seen = true;
                        expr->argument_names.push_back(std::move(label));
                    } else {
                        if (named_seen) fail("positional argument cannot follow named argument");
                        expr->argument_names.emplace_back();
                    }
                    expr->arguments.push_back(expression());
                } while (accept(","));
                require(")");
            }
        }
        if (sum_variant(expr->name)) {
            if (expr->type_arguments_present) fail("native sum explicit constructor type arguments unsupported; use a declared sum type");
            const std::size_t arity = expr->name == "None" ? 0 : 1;
            if (expr->arguments.size() != arity || (!expr->argument_names.empty() && std::any_of(expr->argument_names.begin(), expr->argument_names.end(), [](const std::string& s) { return !s.empty(); })))
                fail("native sum constructor requires its exact positional payload arity");
            expr->kind = Expression::Kind::SumConstruct;
        }
        // Class-owned array expressions are intentionally limited to one
        // field selection followed by a native array method or .size.
        while (accept(".")) {
            auto field = make_expression(Expression::Kind::Field, offset);
            field->left = std::move(expr);
            field->name = identifier();
            // Keep ActorRef.send(...) for the outer actor action parser.
            // Eager ArrayCall parsing would erase the send/stop ordering.
            const bool named_class_receiver = field->left
                && field->left->kind == Expression::Kind::Variable
                && class_receivers_.count(field->left->name);
            const bool actor_send = field->name == "send" && !named_class_receiver;
            if (!actor_send && accept("(")) {
                field->kind = Expression::Kind::ArrayCall;
                if (!accept(")")) {
                    do {
                        if (named_argument_ahead()) {
                            std::string label = identifier();
                            require(":");
                            field->argument_names.push_back(std::move(label));
                        } else {
                            field->argument_names.emplace_back();
                        }
                        field->arguments.push_back(expression());
                    } while (accept(","));
                    require(")");
                }
            }
            expr = std::move(field);
        }
        return expr;
    }

    bool generic_call_type_arguments_ahead() const {
        if (token_.text != "<") return false;
        Lexer probe = lexer_; // already positioned immediately after '<'
        std::size_t budget = 0;
        while (++budget <= 64) {
            const Token next = probe.next();
            if (next.kind == Kind::End) return false;
            if (next.text == ">") return probe.next().text == "(";
            if (next.text == "," || next.text == "=" || next.text == "_" ||
                    next.text == "int" || next.kind == Kind::Identifier)
                continue;
            return false;
        }
        return false;
    }

    bool named_argument_ahead() const {
        if (token_.kind != Kind::Identifier || reserved(token_.text)) return false;
        Lexer lookahead = lexer_;
        return lookahead.next().text == ":";
    }

    bool accept(std::string_view text) {
        if (token_.text != text) return false;
        advance();
        return true;
    }

    // Consult a copy of the scanner rather than merging arithmetic tokens.
    // The original scanner position and error offset remain unchanged.
    bool at_unsupported_critical_mode() const {
        if (token_.text != "critical") return false;
        Lexer probe = lexer_;
        const Token dash = probe.next();
        if (dash.text != "-" || dash.offset != token_.offset + token_.text.size())
            return false;
        const Token mode = probe.next();
        return (mode.text == "read" || mode.text == "write")
            && mode.offset == dash.offset + 1;
    }

    // Parse the role only after 'define'. A plain transform-pipe expression
    // must still lex as identifier, minus, identifier.
    bool accept_transform_pipe_modifier() {
        if (token_.text != "transform") return false;
        const auto start = token_.offset;
        advance();
        if (token_.text != "-" || token_.offset != start + 9)
            fail("expected contiguous transform-pipe actor modifier");
        advance();
        if (token_.text != "pipe" || token_.offset != start + 10)
            fail("expected contiguous transform-pipe actor modifier");
        advance();
        return true;
    }

    void require(std::string_view text) {
        if (!accept(text)) fail("expected '" + std::string(text) + "'");
    }

    void array_element(LocalBinding& binding) {
        if (accept("String") || accept("string")) binding.array_element_type = ScalarType::String;
        else if (!accept("int")) binding.array_record_class = type_identifier();
    }

    std::string type_identifier() {
        auto name = identifier();
        if ((name == "Option" || name == "Result") && accept("<")) {
            NativeSumType type; type.kind = name == "Option" ? NativeSumType::Kind::Option : NativeSumType::Kind::Result;
            auto payload = [&]() -> ScalarType {
                if (accept("int")) return ScalarType::I64;
                if (accept("bool")) return ScalarType::Bool;
                if (accept("String") || accept("string")) return ScalarType::String;
                fail("native sum class/compound payload requires ownership and lifetime lowering");
            };
            auto scalar_name = [](ScalarType t) { return t == ScalarType::String ? "String" : t == ScalarType::Bool ? "bool" : "int"; };
            type.value_type = payload(); name += "<" + std::string(scalar_name(type.value_type));
            if (type.kind == NativeSumType::Kind::Result) { require(","); type.error_type = payload(); name += "," + std::string(scalar_name(type.error_type)); }
            require(">"); type.name = name + ">";
            if (std::none_of(sum_types_.begin(), sum_types_.end(), [&](const NativeSumType& t) { return t.name == type.name; })) sum_types_.push_back(type);
            return type.name;
        }
        if (module_names_.count(name) && accept(".")) name += "." + identifier();
        return name;
    }

    std::string nominal_declaration_name() {
        // '_' is reserved for wildcard/inference-hole use, never a
        // stable runtime nominal identity or module contract name.
        std::string name = identifier();
        if (name == "pattern") fail("contextual pattern keyword cannot declare a native nominal type");
        if (name == "_") fail("wildcard '_' cannot be declared as a nominal type name");
        return name;
    }

    std::string identifier() {
        // SelectPlan and SelectCase are source-level linear ownership
        // capabilities, never ordinary scalar/class identifiers. Reject
        // their native use explicitly; generic parser errors are not a
        // trustworthy compiler admission boundary.
        if (token_.text == "SelectPlan" || token_.text == "SelectCase")
            fail("native SelectPlan/SelectCase ownership unsupported: "
                 "linear case tickets, cancellation and scheduler lowering required");
        if (token_.text == "ActorGroup" ||
            token_.text == "ActorGroupJoinCapability")
            fail("native ActorGroup source lowering unsupported: "
                 "typed membership capabilities and lifecycle authority required");
        if (token_.kind != Kind::Identifier || (reserved(token_.text) && token_.text != "pattern")) {
            fail("expected non-keyword identifier");
        }
        std::string result(token_.text);
        advance();
        return result;
    }

    static std::string string_literal(std::string_view raw) {
        std::string result;
        auto hex = [](char ch) -> unsigned {
            if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
            if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
            if (ch >= 'A' && ch <= 'F') return static_cast<unsigned>(ch - 'A' + 10);
            throw std::runtime_error("invalid native string Unicode escape");
        };
        auto unicode = [&](std::size_t& i) {
            if (raw.size() - i < 4) throw std::runtime_error("incomplete native string Unicode escape");
            unsigned value = 0;
            for (unsigned n = 0; n != 4; ++n) value = value * 16 + hex(raw[i++]);
            return value;
        };
        for (std::size_t i = 0; i < raw.size();) {
            char ch = raw[i++];
            if (ch != '\\') { result += ch; continue; }
            if (i == raw.size()) throw std::runtime_error("incomplete native string escape");
            ch = raw[i++];
            if (ch == 'n') result += '\n';
            else if (ch == 'r') result += '\r';
            else if (ch == 't') result += '\t';
            else if (ch != 'u') result += ch;
            else {
                unsigned code = unicode(i);
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (raw.size() - i < 6 || raw[i] != '\\' || raw[i + 1] != 'u')
                        throw std::runtime_error("unpaired native string Unicode surrogate");
                    i += 2;
                    const unsigned low = unicode(i);
                    if (low < 0xdc00 || low > 0xdfff) throw std::runtime_error("unpaired native string Unicode surrogate");
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff)
                    throw std::runtime_error("unpaired native string Unicode surrogate");
                if (code <= 0x7f) result += static_cast<char>(code);
                else if (code <= 0x7ff) {
                    result += static_cast<char>(0xc0 | (code >> 6)); result += static_cast<char>(0x80 | (code & 63));
                } else if (code <= 0xffff) {
                    result += static_cast<char>(0xe0 | (code >> 12)); result += static_cast<char>(0x80 | ((code >> 6) & 63)); result += static_cast<char>(0x80 | (code & 63));
                } else {
                    result += static_cast<char>(0xf0 | (code >> 18)); result += static_cast<char>(0x80 | ((code >> 12) & 63));
                    result += static_cast<char>(0x80 | ((code >> 6) & 63)); result += static_cast<char>(0x80 | (code & 63));
                }
            }
        }
        return result;
    }

    static bool blank_template_expression(std::string_view source) {
        return std::all_of(source.begin(), source.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
    }

    static void validate_template_escape(std::string_view raw, std::size_t& at,
                                         std::size_t origin) {
        if (at == raw.size()) template_error("has unterminated escape", origin);
        const char escaped = raw[at++];
        if (escaped == 'n' || escaped == 'r' || escaped == 't' ||
                escaped == '\\' || escaped == '"' || escaped == '\'' ||
                escaped == '$' || static_cast<unsigned char>(escaped) == 0x60) return;
        if (escaped != 'u') template_error("contains invalid escape", origin);
        if (raw.size() - at < 4) template_error("has incomplete Unicode escape", origin);
        for (unsigned n = 0; n != 4; ++n) {
            const char ch = raw[at++];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                  (ch >= 'A' && ch <= 'F')))
                template_error("contains invalid Unicode escape", origin);
        }
    }

    std::unique_ptr<Expression> template_expression(std::string_view source,
                                                    std::size_t base_offset) {
        Parser nested(source, experimental_owned_int_, experimental_rt_scalar_loop_);
        auto expr = nested.expression();
        if (nested.token_.kind != Kind::End)
            nested.fail("expected end of template interpolation expression");
        std::function<void(Expression&)> rebase = [&](Expression& node) {
            node.offset += base_offset;
            if (node.left) rebase(*node.left);
            if (node.right) rebase(*node.right);
            if (node.condition) rebase(*node.condition);
            if (node.condition_lhs) rebase(*node.condition_lhs);
            if (node.condition_rhs) rebase(*node.condition_rhs);
            if (node.when_true) rebase(*node.when_true);
            if (node.when_false) rebase(*node.when_false);
            for (auto& arg : node.arguments) rebase(*arg);
        };
        rebase(*expr);
        return expr;
    }

    std::unique_ptr<Expression> template_literal(std::string_view raw,
                                                 std::size_t origin) {
        std::vector<std::unique_ptr<Expression>> pieces;
        std::string literal;
        std::size_t interpolations = 0;
        std::size_t at = 0;
        auto flush_literal = [&] {
            auto part = make_expression(Expression::Kind::String, origin);
            part->name = string_literal(literal);
            pieces.push_back(std::move(part));
            literal.clear();
        };

        while (at < raw.size()) {
            const char ch = raw[at++];
            if (ch == '\\') {
                const std::size_t escape = at;
                validate_template_escape(raw, at, origin);
                literal.append(raw.substr(escape - 1, at - escape + 1));
                continue;
            }
            if (static_cast<unsigned char>(ch) == 0x24 &&
                    at < raw.size() && raw[at] == '{') {
                if (++interpolations > kMaxNativeTemplateInterpolations)
                    template_error("interpolation count limit exceeded (1024)", origin);
                flush_literal();
                const std::size_t expression_start = at + 1;
                const std::size_t close = template_interpolation_close(
                    raw, expression_start, 0, origin);
                const auto source = raw.substr(expression_start, close - expression_start);
                if (source.empty() || blank_template_expression(source))
                    template_error("contains empty interpolation", origin + 1 + expression_start);
                pieces.push_back(template_expression(source, origin + 1 + expression_start));
                at = close + 1;
                continue;
            }
            literal += ch;
        }
        flush_literal();

        auto result = std::move(pieces.front());
        for (std::size_t i = 1; i < pieces.size(); ++i) {
            auto concat = make_expression(Expression::Kind::Add, pieces[i]->offset);
            concat->left = std::move(result);
            concat->right = std::move(pieces[i]);
            result = std::move(concat);
        }
        return result;
    }

    static void validate_expression(const Expression& expr,
                                    const std::unordered_set<std::string>& parameters,
                                    const NativeSignatures& functions,
                                    std::size_t depth = 1) {
        require_expression_depth(depth, expr.offset);
        switch (expr.kind) {
            case Expression::Kind::Integer:
            case Expression::Kind::Boolean:
            case Expression::Kind::String:
                return;
            case Expression::Kind::Console:
                validate_expression(*expr.left, parameters, functions, depth + 1);
                return;
            case Expression::Kind::Variable:
                if (parameters.count(expr.name) == 0) {
                    throw std::runtime_error("unknown parameter '" + expr.name
                                             + "' at byte " + std::to_string(expr.offset));
                }
                return;
            case Expression::Kind::Field:
                validate_expression(*expr.left, parameters, functions, depth + 1);
                return;
            case Expression::Kind::ArrayCall:
                validate_expression(*expr.left, parameters, functions, depth + 1);
                for (const auto& arg : expr.arguments)
                    validate_expression(*arg, parameters, functions, depth + 1);
                return;
            case Expression::Kind::SumConstruct:
            case Expression::Kind::SumAccess:
            case Expression::Kind::RecordConstruct:
                if (expr.left) validate_expression(*expr.left, parameters, functions, depth + 1);
                for (const auto& arg : expr.arguments)
                    validate_expression(*arg, parameters, functions, depth + 1);
                return;
            case Expression::Kind::Call: {
                const auto found = functions.find(expr.name);
                if (found == functions.end() ||
                    found->second.count(expr.arguments.size()) == 0) {
                    throw std::runtime_error("unknown function or wrong call arity '"
                                             + expr.name + "' at byte "
                                             + std::to_string(expr.offset));
                }
                if (!expr.argument_names.empty() &&
                    expr.argument_names.size() != expr.arguments.size())
                    throw std::runtime_error("invalid named call argument layout");
                const auto& signature = found->second.at(expr.arguments.size());
                if (signature.async_source_i64)
                    throw std::runtime_error(
                        "async source call requires checked Future<int> pipeline lowering at byte " +
                        std::to_string(expr.offset));
                const auto& formals = signature.parameters;
                if (expr.type_arguments_present && signature.generics.empty())
                    throw std::runtime_error("call-site type arguments require generic function");
                if (!signature.generics.empty()) {
                    std::unordered_set<std::string> inferred;
                    std::unordered_set<std::string> assigned;
                    std::unordered_set<std::string> holes;
                    std::size_t positional_type = 0;
                    bool saw_named_type = false;
                    if (expr.type_arguments.size() > signature.generics.size())
                        throw std::runtime_error("too many generic type arguments");
                    for (const auto& arg : expr.type_arguments) {
                        std::size_t slot = positional_type;
                        if (!arg.name.empty()) {
                            saw_named_type = true;
                            auto it = std::find_if(signature.generics.begin(), signature.generics.end(),
                                [&](const GenericParameter& candidate) { return candidate.name == arg.name; });
                            if (it == signature.generics.end())
                                throw std::runtime_error("unknown named generic argument '" + arg.name + "'");
                            slot = static_cast<std::size_t>(it - signature.generics.begin());
                        } else {
                            if (saw_named_type)
                                throw std::runtime_error("positional generic argument after named argument");
                            ++positional_type;
                        }
                        const std::string& name = signature.generics.at(slot).name;
                        if (!assigned.insert(name).second)
                            throw std::runtime_error("duplicate generic argument '" + name + "'");
                        if (arg.infer) holes.insert(name);
                        else if (arg.type == "int") inferred.insert(name);
                        else throw std::runtime_error("unsupported native generic type specialization");
                    }
                    // Every native scalar value argument is i64. Only slots that
                    // actually appear in a scalar formal may be inferred.
                    for (const auto& type : signature.parameter_generic_types)
                        if (!type.empty()) inferred.insert(type);
                    for (const auto& slot : signature.generics) {
                        if (inferred.count(slot.name)) continue;
                        if (!holes.count(slot.name) && !slot.default_type.empty()) {
                            if (slot.default_type == "int" || inferred.count(slot.default_type)) {
                                inferred.insert(slot.name);
                                continue;
                            }
                            throw std::runtime_error("cannot resolve dependent default for generic '" + slot.name + "'");
                        }
                        throw std::runtime_error("cannot infer generic parameter '" + slot.name + "'");
                    }
                }
                std::vector<bool> bound(formals.size(), false);
                bool saw_named = false;
                std::size_t positional = 0;
                for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                    const std::string label = expr.argument_names.empty()
                        ? std::string{} : expr.argument_names[i];
                    std::size_t slot = positional;
                    if (!label.empty()) {
                        saw_named = true;
                        auto it = std::find(formals.begin(), formals.end(), label);
                        if (it == formals.end())
                            throw std::runtime_error("unknown named argument '" + label
                                                     + "' for " + expr.name);
                        slot = static_cast<std::size_t>(it - formals.begin());
                    } else {
                        if (saw_named)
                            throw std::runtime_error("positional argument after named argument");
                        ++positional;
                    }
                    if (slot >= formals.size() || bound[slot])
                        throw std::runtime_error("duplicate binding of parameter '"
                                                 + formals[slot] + "'");
                    bound[slot] = true;
                    // Check nested calls in lexical order, not formal-slot order.
                    validate_expression(*expr.arguments[i], parameters, functions, depth + 1);
                }
                return;
            }
            case Expression::Kind::Negate:
            case Expression::Kind::LogicalNot:
            case Expression::Kind::Copy:
            case Expression::Kind::Take:
            case Expression::Kind::Share:
            case Expression::Kind::Borrow:
            case Expression::Kind::BorrowMut:
                validate_expression(*expr.left, parameters, functions, depth + 1);
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
                validate_expression(*expr.left, parameters, functions, depth + 1);
                validate_expression(*expr.right, parameters, functions, depth + 1);
                return;
            case Expression::Kind::Conditional:
                if (expr.condition)
                    validate_expression(*expr.condition, parameters, functions, depth + 1);
                else {
                    validate_expression(*expr.condition_lhs, parameters, functions, depth + 1);
                    validate_expression(*expr.condition_rhs, parameters, functions, depth + 1);
                }
                validate_expression(*expr.when_true, parameters, functions, depth + 1);
                validate_expression(*expr.when_false, parameters, functions, depth + 1);
                return;
        }
        throw std::runtime_error("internal invalid expression kind");
    }

    static const char* scalar_type_name(ScalarType type) {
        return type == ScalarType::String ? "string" : type == ScalarType::Bool ? "bool" : "int";
    }

    static ScalarType validate_scalar_expression(
            const Expression& expr,
            const std::unordered_map<std::string, ScalarType>& variables,
            const NativeSignatures& functions,
            std::size_t depth = 1) {
        require_expression_depth(depth, expr.offset);
        auto require_type = [&](ScalarType actual, ScalarType expected, std::string_view where) {
            if (actual != expected)
                throw std::runtime_error(std::string(where) + " requires " +
                    scalar_type_name(expected) + ", got " + scalar_type_name(actual) +
                    " at byte " + std::to_string(expr.offset));
        };
        switch (expr.kind) {
            case Expression::Kind::SumConstruct:
            case Expression::Kind::SumAccess:
                if (expr.left) (void)validate_scalar_expression(*expr.left, variables, functions, depth + 1);
                for (const auto& arg : expr.arguments) (void)validate_scalar_expression(*arg, variables, functions, depth + 1);
                return expr.kind == Expression::Kind::SumAccess ? expr.resolved_type : ScalarType::I64;

            case Expression::Kind::Integer:
                return ScalarType::I64;
            case Expression::Kind::Boolean:
                return ScalarType::Bool;
            case Expression::Kind::String:
                return expr.resolved_type = ScalarType::String;
            case Expression::Kind::Console:
                require_type(validate_scalar_expression(*expr.left, variables, functions, depth + 1), ScalarType::String, "stdio");
                return ScalarType::I64;
            case Expression::Kind::Variable: {
                const auto found = variables.find(expr.name);
                // Non-scalar class/array/actor names are rejected by ownership
                // if used as scalar values; treat their placeholder as i64 here
                // only so field/borrow syntax can reach that stricter pass.
                return found == variables.end() ? ScalarType::I64 : found->second;
            }
            case Expression::Kind::Field: {
                if (expr.left) {
                    auto found = variables.find(native_place_key(expr));
                    if (found != variables.end()) return found->second;
                }
                return ScalarType::I64;
            }
            case Expression::Kind::ArrayCall: {
                std::string receiver;
                if (expr.left) receiver = native_place_key(*expr.left);
                const auto element_slot = variables.find(receiver + ".$element");
                const ScalarType element = element_slot == variables.end() ? ScalarType::I64 : element_slot->second;
                const std::size_t expected = expr.name == "get" || expr.name == "add"
                    ? 1 : expr.name == "set" ? 2 : 0;
                if (expected == 0 || expr.arguments.size() != expected)
                    throw std::runtime_error("native Array<int> effect supports only add(value) or set(index,value); scalar expression supports only get(index) at byte " +
                        std::to_string(expr.offset));
                for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                    const auto& argument = expr.arguments[i];
                    const ScalarType actual = validate_scalar_expression(
                        *argument, variables, functions, depth + 1);
                    const bool index = (expr.name == "get" || expr.name == "set") && i == 0;
                    if (actual != (index ? ScalarType::I64 : element))
                        throw std::runtime_error(std::string(element == ScalarType::I64
                            ? "native Array<int> arguments require int, got "
                            : "native Array<String> argument type mismatch, got ") +
                            std::string(scalar_type_name(actual)) + " at byte " +
                            std::to_string(argument->offset));
                }
                return expr.resolved_type = expr.name == "get" ? element : ScalarType::I64;
            }
            case Expression::Kind::RecordConstruct:
                for (const auto& arg : expr.arguments)
                    validate_scalar_expression(*arg, variables, functions, depth + 1);
                return ScalarType::I64; // nominal record validation follows, never a guest integer
            case Expression::Kind::Call: {
                const auto family = functions.find(expr.name);
                if (family == functions.end() ||
                        !family->second.count(expr.arguments.size()))
                    throw std::runtime_error("unknown function or wrong call arity '" +
                        expr.name + "' at byte " + std::to_string(expr.offset));
                const auto& signature = family->second.at(expr.arguments.size());
                if (signature.async_source_i64)
                    throw std::runtime_error(
                        "async source call requires checked Future<int> pipeline lowering at byte " +
                        std::to_string(expr.offset));
                std::vector<bool> bound(signature.parameters.size(), false);
                std::size_t positional = 0;
                bool named_seen = false;
                for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
                    const std::string label = expr.argument_names.empty()
                        ? std::string{} : expr.argument_names[i];
                    std::size_t slot = positional;
                    if (!label.empty()) {
                        named_seen = true;
                        auto it = std::find(signature.parameters.begin(),
                            signature.parameters.end(), label);
                        if (it == signature.parameters.end())
                            throw std::runtime_error("unknown named argument '" + label + "'");
                        slot = static_cast<std::size_t>(it - signature.parameters.begin());
                    } else {
                        if (named_seen)
                            throw std::runtime_error("positional argument after named argument");
                        ++positional;
                    }
                    if (slot >= signature.parameters.size() || bound[slot])
                        throw std::runtime_error("duplicate native scalar argument binding");
                    bound[slot] = true;
                    const ScalarType actual = validate_scalar_expression(
                        *expr.arguments[i], variables, functions, depth + 1);
                    if (signature.parameter_is_scalar[slot] &&
                            actual != signature.parameter_scalar_types[slot])
                        throw std::runtime_error("argument '" + signature.parameters[slot] +
                            "' requires " + scalar_type_name(signature.parameter_scalar_types[slot]) +
                            ", got " + scalar_type_name(actual) + " at byte " +
                            std::to_string(expr.arguments[i]->offset));
                }
                // Ownership/expression-context admission separately rejects
                // a void call where a scalar value is required. Returning the
                // placeholder here lets statement-position void calls reuse the
                // exact same typed argument binder.
                return signature.returns_void ? ScalarType::I64 : signature.return_type;
            }
            case Expression::Kind::Negate: {
                ScalarType operand = validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
                require_type(operand, ScalarType::I64, "unary '-'");
                return ScalarType::I64;
            }
            case Expression::Kind::LogicalNot: {
                ScalarType operand = validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
                require_type(operand, ScalarType::Bool, "logical '!'");
                return ScalarType::Bool;
            }
            case Expression::Kind::Copy:
            case Expression::Kind::Take:
            case Expression::Kind::Share:
            case Expression::Kind::Borrow:
            case Expression::Kind::BorrowMut:
                return validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
            case Expression::Kind::Add: {
                auto left = validate_scalar_expression(*expr.left, variables, functions, depth + 1);
                auto right = validate_scalar_expression(*expr.right, variables, functions, depth + 1);
                if (left == ScalarType::String) require_type(right, ScalarType::String, "string concatenation");
                else {
                    require_type(left, ScalarType::I64, "arithmetic");
                    require_type(right, ScalarType::I64, "arithmetic");
                }
                return expr.resolved_type = left;
            }
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply: {
                ScalarType left = validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
                ScalarType right = validate_scalar_expression(
                    *expr.right, variables, functions, depth + 1);
                require_type(left, ScalarType::I64, "arithmetic");
                require_type(right, ScalarType::I64, "arithmetic");
                return ScalarType::I64;
            }
            case Expression::Kind::Less:
            case Expression::Kind::LessEqual:
            case Expression::Kind::Greater:
            case Expression::Kind::GreaterEqual: {
                ScalarType left = validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
                ScalarType right = validate_scalar_expression(
                    *expr.right, variables, functions, depth + 1);
                require_type(left, ScalarType::I64, "ordered comparison");
                require_type(right, ScalarType::I64, "ordered comparison");
                return ScalarType::Bool;
            }
            case Expression::Kind::Equal:
            case Expression::Kind::NotEqual: {
                ScalarType left = validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
                ScalarType right = validate_scalar_expression(
                    *expr.right, variables, functions, depth + 1);
                if (left != right)
                    throw std::runtime_error("equality operands require the same scalar type at byte " +
                        std::to_string(expr.offset));
                return ScalarType::Bool;
            }
            case Expression::Kind::LogicalAnd:
            case Expression::Kind::LogicalOr:
            case Expression::Kind::LogicalXor: {
                ScalarType left = validate_scalar_expression(
                    *expr.left, variables, functions, depth + 1);
                ScalarType right = validate_scalar_expression(
                    *expr.right, variables, functions, depth + 1);
                require_type(left, ScalarType::Bool, "logical operator");
                require_type(right, ScalarType::Bool, "logical operator");
                return ScalarType::Bool;
            }
            case Expression::Kind::Conditional: {
                ScalarType condition = expr.condition
                    ? validate_scalar_expression(*expr.condition, variables, functions, depth + 1)
                    : ScalarType::Bool;
                require_type(condition, ScalarType::Bool, "ternary condition");
                ScalarType when_true = validate_scalar_expression(
                    *expr.when_true, variables, functions, depth + 1);
                ScalarType when_false = validate_scalar_expression(
                    *expr.when_false, variables, functions, depth + 1);
                if (when_true != when_false)
                    throw std::runtime_error("ternary arms require a common scalar type at byte " +
                        std::to_string(expr.offset));
                return when_true;
            }
        }
        throw std::runtime_error("internal invalid native scalar expression kind");
    }

    static bool contains_callable_invocation(
            const Expression* expr, std::size_t depth = 1) {
        if (!expr) return false;
        require_expression_depth(depth, expr->offset);
        if (expr->kind == Expression::Kind::SumConstruct || expr->kind == Expression::Kind::SumAccess || expr->kind == Expression::Kind::RecordConstruct || expr->kind == Expression::Kind::Call ||
            expr->kind == Expression::Kind::ArrayCall) return true;
        if (contains_callable_invocation(expr->left.get(), depth + 1) ||
            contains_callable_invocation(expr->right.get(), depth + 1) ||
            contains_callable_invocation(expr->condition.get(), depth + 1) ||
            contains_callable_invocation(expr->condition_lhs.get(), depth + 1) ||
            contains_callable_invocation(expr->condition_rhs.get(), depth + 1) ||
            contains_callable_invocation(expr->when_true.get(), depth + 1) ||
            contains_callable_invocation(expr->when_false.get(), depth + 1)) return true;
        for (const auto& item : expr->arguments)
            if (contains_callable_invocation(item.get(), depth + 1)) return true;
        return false;
    }

    static void validate_declared_effects(const Function& fn, const Program& program) {
        if (!fn.has_declared_effects) return;
        if (!fn.actor_spawns.empty() || !fn.ready_await_actor.empty()
                || !fn.done_await_actor.empty()
                || !fn.actor_sends.empty() || !fn.actor_stops.empty()
                || !fn.actor_field_writes.empty()) {
            throw std::runtime_error(
                "using effects on actor actions requires trusted actor effect summaries");
        }
        for (const auto& local : fn.locals) {
            bool allocates_class_array = false;
            if (local.new_class) {
                const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                    [&](const ClassDecl& item) { return item.name == local.class_name; });
                if (cls != program.classes.end())
                    allocates_class_array = std::any_of(cls->fields.begin(), cls->fields.end(),
                        [](const ClassField& field) { return field.int_array; });
            }
            if ((local.heap_owned || local.int_array || allocates_class_array) &&
                    !fn.declared_effects.has(FunctionEffect::Alloc))
                throw std::runtime_error(
                    "owned allocation violates using signature without alloc");
            if (contains_callable_invocation(local.initializer.get()))
                throw std::runtime_error(
                    "using effects require transitive call summaries");
        }
        std::function<void(const Statement&)> validate_statement_effects;
        validate_statement_effects = [&](const Statement& stmt) {
            if (stmt.kind == Statement::Kind::ArrayCommitFilled &&
                    !fn.declared_effects.has(FunctionEffect::Alloc))
                throw std::runtime_error("filled array commit violates using signature without alloc");
            if (contains_callable_invocation(stmt.expression.get()) ||
                    contains_callable_invocation(stmt.condition.get()) ||
                    contains_callable_invocation(stmt.condition_lhs.get()) ||
                    contains_callable_invocation(stmt.condition_rhs.get()) ||
                    (stmt.branch_local &&
                        contains_callable_invocation(stmt.branch_local->initializer.get())))
                throw std::runtime_error("using effects require transitive call summaries");
            if (stmt.kind == Statement::Kind::ArrayCommitFilled) {
                if (contains_callable_invocation(stmt.array_commit_length.get()))
                    throw std::runtime_error("filled array commit requires pure scalar length");
                for (const auto& item : stmt.array_commit_values)
                    if (contains_callable_invocation(item.get()))
                        throw std::runtime_error("filled array commit requires pure scalar fills");
            }
            for (const auto& nested : stmt.then_statements)
                validate_statement_effects(nested);
            for (const auto& nested : stmt.else_statements)
                validate_statement_effects(nested);
        };
        for (const auto& stmt : fn.statements)
            validate_statement_effects(stmt);
        for (const auto& guard : fn.early_returns)
            if (contains_callable_invocation(guard.condition.get()) ||
                contains_callable_invocation(guard.lhs.get()) ||
                contains_callable_invocation(guard.rhs.get()) ||
                contains_callable_invocation(guard.result.get()))
                throw std::runtime_error("using effects require transitive call summaries");
        if (contains_callable_invocation(fn.result.get()))
            throw std::runtime_error("using effects require transitive call summaries");
    }

    static void validate(const Program& program) {
        validate_type_only_declarations(program);
        const NativeClassLayouts layouts(program);
        std::unordered_set<std::string> classes;
        for (const auto& cls : program.classes) {
            if (cls.constructor_exported && !cls.exported)
                throw std::runtime_error("public constructor requires a public class");
            if (!classes.insert(cls.name).second)
                throw std::runtime_error("duplicate class declaration '" + cls.name + "'");
        }
        NativeSignatures functions;
        for (const auto& fn : program.functions) {
            // Reject hand-constructed or incorrectly deserialized programs:
            // parser-only pending aliases never enter ownership or LLVM ABI.
            if (!fn.pending_return_type_alias.empty())
                throw std::runtime_error("unresolved native scalar return type before ABI admission");
            for (const auto& parameter : fn.parameters)
                if (!parameter.pending_type_alias.empty())
                    throw std::runtime_error("unresolved native scalar parameter type before ABI admission");
            validate_declared_effects(fn, program);
            std::function<bool(const Statement&)> ordered_frame = [&](const Statement& s) {
                return s.kind == Statement::Kind::ScopedLocal || s.kind == Statement::Kind::ScalarWrite ||
                    std::any_of(s.then_statements.begin(), s.then_statements.end(), ordered_frame) ||
                    std::any_of(s.else_statements.begin(), s.else_statements.end(), ordered_frame);
            };
            if ((!fn.ready_await_actor.empty() || !fn.done_await_actor.empty()) &&
                    std::any_of(fn.statements.begin(), fn.statements.end(), ordered_frame))
                throw std::runtime_error("native source await requires continuation-aware statement lowering");
            // Compiler-owned ABI imports and generated exports may be
            // inserted before source function definitions. LLVM silently
            // renames duplicate Function::Create symbols (e.g. abort.1),
            // which would disconnect source identity from the actual linker
            // symbol. Reject unqualified native/wasm symbols *before* IR.
            // Module-qualified names are emitted as module.name and do not
            // collide at the ELF/Wasm symbol boundary.
            if (fn.name.rfind("oreslang_int_array_", 0) == 0)
                throw std::runtime_error("reserved native Array<int> ABI symbol");
            if (program.module.empty()) {
                const bool runtime_symbol =
                    fn.name == "abort" ||
                    fn.name == "__oreslang_guest_main" ||
                    fn.name.rfind("oreslang_owned_i64_region_", 0) == 0 ||
                    fn.name.rfind("ores_actor_i64_heap_", 0) == 0 ||
                    fn.name.rfind("oreslang_source_actor_", 0) == 0 ||
                    fn.name.rfind("oreslang_source_task_", 0) == 0 ||
                    fn.name.rfind("oreslang_actor_", 0) == 0 ||
                    fn.name.rfind("oreslang_wasm_actor_", 0) == 0;
                if (runtime_symbol)
                    throw std::runtime_error("reserved native runtime ABI symbol '" +
                        fn.name + "'");
            }
            if (std::count_if(fn.locals.begin(), fn.locals.end(),
                    [](const LocalBinding& local) { return local.int_array; }) > 64)
                throw std::runtime_error("native Array<int> owner limit exceeded (64)");
            // Overload identity is strictly name and explicit argument count.
            // Parameter types, borrow modes and returns cannot select a slot.
            if (fn.name == "main" && functions.count("main"))
                throw std::runtime_error("duplicate native entrypoint 'main'; main cannot be overloaded");
            NativeCallSignature signature;
            signature.generics = fn.generic_parameters;
            for (const auto& param : fn.parameters) {
                signature.parameters.push_back(param.name);
                signature.parameter_generic_types.push_back(param.generic_type);
                signature.parameter_scalar_types.push_back(param.scalar_type);
                signature.parameter_is_scalar.push_back(
                    param.class_name.empty() && param.generic_type.empty() && param.sum_type.empty());
            }
            signature.return_type = fn.return_type;
            signature.returns_void = fn.returns_void;
            signature.async_source_i64 = fn.async_source_i64;
            if (!functions[fn.name].emplace(fn.parameters.size(), std::move(signature)).second)
                throw std::runtime_error("function '" + fn.name + "' already has arity "
                    + std::to_string(fn.parameters.size()) +
                    "; parameter and return types do not select overloads at byte " +
                    std::to_string(fn.offset));
            std::unordered_set<std::string> params;
            for (const auto& param : fn.parameters) {
                if (!params.insert(param.name).second) {
                    throw std::runtime_error("duplicate parameter '" + param.name
                                             + "' at byte " + std::to_string(fn.offset));
                }
            }
        }
        std::unordered_map<std::string, const ActorDecl*> actor_types;
        for (const auto& actor : program.actors) {
            if (!actor_types.emplace(actor.name, &actor).second || classes.count(actor.name)
                    || functions.count(actor.name))
                throw std::runtime_error("duplicate actor/class/function symbol '" + actor.name + "'");
            std::unordered_set<std::string> fields;
            std::unordered_set<std::string> mutables;
            const std::unordered_set<std::string> empty;
            for (const auto& field : actor.fields) {
                validate_expression(*field.initializer, empty, {});
                if (!fields.insert(field.name).second)
                    throw std::runtime_error("duplicate actor field '" + field.name + "'");
                if (field.field_mutable) mutables.insert(field.name);
            }
            const auto& method = actor.on_message;
            const std::string& parameter = method.parameters.at(0).name;
            if (!fields.insert(parameter).second)
                throw std::runtime_error("actor parameter shadows field '" + parameter + "'");
            // Validate in declaration order. An owned actor-handler local
            // becomes visible only AFTER its initializer is fully checked.
            // Never treat a forward reference as an arbitrary field or let
            // an undefined identifier reach LLVM slot lookup.
            for (const auto& local : method.locals) {
                if (!local.heap_owned || local.owner_mutable || !local.initializer)
                    throw std::runtime_error(
                        "native actor owned local requires immutable initialized slot");
                validate_expression(*local.initializer, fields, functions);
                if (!fields.insert(local.name).second)
                    throw std::runtime_error(
                        "actor owned local shadows field, parameter or previous owner '" +
                        local.name + "'");
            }
            for (const auto& write : method.actor_field_writes) {
                if (!mutables.count(write.field))
                    throw std::runtime_error("cannot assign immutable or unknown actor field '" + write.field + "'");
                validate_expression(*write.expression, fields, functions);
            }
            validate_expression(*method.result, fields, functions);
            if (functions.count("oreslang_actor_" + actor.name + "_init") ||
                    functions.count("oreslang_actor_" + actor.name + "_on_message"))
                throw std::runtime_error("conflicting generated actor symbol");
        }
        // Same-file contracts are compile-time structural checks. Requiring
        // matching *public* members prevents accidental admission through
        // private functions that external callers cannot invoke.
        std::unordered_map<std::string, const ModuleContract*> available_contracts;
        for (const auto& contract : program.contracts) {
            if (!available_contracts.emplace(contract.name, &contract).second) {
                throw std::runtime_error("duplicate native module contract '" + contract.name + "'");
            }
        }
        std::unordered_set<std::string> seen_contracts;
        for (const auto& ref : program.conforms) {
            if (!seen_contracts.insert(ref).second) {
                throw std::runtime_error("duplicate native conforms contract '" + ref + "'");
            }
            const auto contract_it = available_contracts.find(ref);
            if (contract_it == available_contracts.end()) {
                throw std::runtime_error("unknown native module contract '" + ref + "'");
            }
            for (const auto& required : contract_it->second->methods) {
                const auto fn = std::find_if(program.functions.begin(),
                        program.functions.end(), [&](const Function& candidate) {
                            return candidate.name == required.name;
                        });
                if (fn == program.functions.end() || !fn->exported) {
                    throw std::runtime_error("missing public native module contract method '"
                            + ref + "." + required.name + "'");
                }
                if (fn->returns_void || !fn->return_sum.empty()) {
                    throw std::runtime_error("native module contract requires int return");
                }
                if (fn->parameters.size() != required.parameter_modes.size()) {
                    throw std::runtime_error("native module contract parameter arity mismatch '"
                            + ref + "." + required.name + "'");
                }
                for (std::size_t n = 0; n < required.parameter_modes.size(); ++n) {
                    if (!fn->parameters[n].class_name.empty() || !fn->parameters[n].sum_type.empty() ||
                            fn->parameters[n].mode != required.parameter_modes[n]) {
                        throw std::runtime_error("native module contract ownership mode mismatch '"
                                + ref + "." + required.name + "'");
                    }
                }
            }
        }
        for (const auto& fn : program.functions) {
            std::unordered_set<std::string> params;
            for (const auto& param : fn.parameters) params.insert(param.name);
            std::unordered_set<std::string> live_actors;
            std::unordered_map<std::string, const ActorDecl*> references;
            if (!fn.actor_spawns.empty() || !fn.ready_await_actor.empty()
                    || !fn.done_await_actor.empty()
                    || !fn.actor_sends.empty() || !fn.actor_stops.empty()) {
                if (fn.name != "main" || !fn.parameters.empty() || fn.returns_void ||
                        !fn.early_returns.empty() || !fn.statements.empty())
                    throw std::runtime_error(
                        "source actor bootstrap requires zero-argument int main without early returns");
            }
            for (const auto& spawn : fn.actor_spawns) {
                const auto it = actor_types.find(spawn.actor_type);
                if (it == actor_types.end())
                    throw std::runtime_error("unknown actor type '" + spawn.actor_type + "'");
                if (!live_actors.insert(spawn.name).second || params.count(spawn.name))
                    throw std::runtime_error("duplicate actor reference '" + spawn.name + "'");
                references.emplace(spawn.name, it->second);
            }
            if (!fn.ready_future_name.empty()) {
                // A Future<void> alias must be consumed once, cannot shadow
                // any live actor/parameter/local, and cannot be forged from
                // an arbitrary scalar or a different actor capability.
                if (fn.ready_await_actor != fn.ready_future_actor)
                    throw std::runtime_error(
                        "Future<void> readiness token must be awaited exactly once");
                if (params.count(fn.ready_future_name) ||
                        live_actors.count(fn.ready_future_name) ||
                        std::any_of(fn.locals.begin(), fn.locals.end(),
                            [&](const LocalBinding& item) {
                                return item.name == fn.ready_future_name;
                            }))
                    throw std::runtime_error(
                        "Future<void> readiness token shadows a live owner");
            }
            if (!fn.ready_await_actor.empty()) {
                /*
                 * First executable suspension is deliberately tiny: one actor
                 * capability, no scalar/class/array owner crossing suspension,
                 * no defer/guard CFG, and no generic Future<T> representation.
                 */
                // Resolve the HasFuture operand against a *live ActorRef*
                // before checking suspended locals. Otherwise a scalar with
                // the same spelling could surface only a local-frame error,
                // obscuring the admission boundary for non-awaitable values.
                if (!live_actors.count(fn.ready_await_actor))
                    throw std::runtime_error(
                        "await get_ready requires a live direct ActorRef receiver");
                const auto actor_witness = references.find(fn.ready_await_actor);
                if (actor_witness == references.end() ||
                        actor_witness->second == nullptr)
                    throw std::runtime_error(
                        "await get_ready requires a live direct ActorRef receiver");
                const bool token_form =
                    fn.ready_await_form == NativeAwaitForm::FutureValue;
                const NativeAwaitDescriptor candidate{
                    token_form ? NativeAwaitTargetKind::FutureVoid
                               : NativeAwaitTargetKind::ActorRef,
                    fn.ready_await_form,
                    actor_witness->second->name,
                    fn.ready_await_actor,
                    true, // resolved through the live ActorRef registry above
                    token_form && fn.ready_future_actor == fn.ready_await_actor,
                    token_form && !fn.ready_future_name.empty(),
                    token_form ? std::string_view{fn.ready_future_actor}
                               : std::string_view{}
                };
                const NativeAwaitResolution resolution =
                    resolve_native_has_future(candidate);
                if (resolution.abi != NativeAwaitAbi::SourceActorReadyControl ||
                        !resolution.returns_void ||
                        resolution.consumes_future_token != token_form)
                    throw std::runtime_error(
                        "native HasFuture await requires verified get_ready projection");
                if (fn.actor_spawns.size() != 1 || !fn.locals.empty())
                    throw std::runtime_error(
                        "await get_ready requires one source actor without suspended locals");
                /*
                 * The first vertical slice evaluates the final scalar result on
                 * the root continuation before trusted-main stop. Keep that
                 * movement observationally inert until stop itself can be
                 * lowered into nonblocking continuation cleanup.
                 */
                if (contains_callable_invocation(fn.result.get()))
                    throw std::runtime_error(
                        "await get_ready requires side-effect-free scalar return");
            }
            if (!fn.done_await_actor.empty()) {
                /*
                 * get_done is a physical-quiescence boundary. The opaque
                 * ActorRef stays runtime-owned until the root CONTROL
                 * continuation completes and trusted host reclamation runs.
                 */
                if (fn.actor_spawns.size() != 1 || !fn.locals.empty())
                    throw std::runtime_error(
                        "await get_done requires one source actor without suspended locals");
                if (!references.count(fn.done_await_actor))
                    throw std::runtime_error(
                        "await get_done requires a direct ActorRef receiver");
                if (contains_callable_invocation(fn.result.get()))
                    throw std::runtime_error(
                        "await get_done requires side-effect-free scalar return");
            }
            if (fn.async_source_i64) {
                if (fn.name == "main" || fn.exported ||
                        !fn.parameters.empty() || fn.returns_void ||
                        fn.return_type != ScalarType::I64 ||
                        !fn.return_class.empty() || !fn.return_sum.empty() ||
                        !fn.generic_parameters.empty() ||
                        fn.has_declared_effects || !fn.locals.empty() ||
                        !fn.statements.empty() || !fn.early_returns.empty() ||
                        !fn.actor_spawns.empty() || !fn.actor_actions.empty() ||
                        !fn.actor_field_writes.empty() ||
                        !fn.source_tasks.empty() ||
                        !fn.future_pipelines.empty() ||
                        !fn.ready_await_actor.empty() ||
                        !fn.done_await_actor.empty() ||
                        fn.scalar_while || fn.caught_throw_value ||
                        !fn.result ||
                        !native_capture_free_task_tree(*fn.result))
                    throw std::runtime_error(
                        "native async Future<int> producer v1 must be a private "
                        "capture-free zero-argument int producer");
            }
            if (!fn.future_pipelines.empty()) {
                if (fn.name != "main" || !fn.parameters.empty() ||
                        fn.future_pipelines.size() != 1 ||
                        !fn.source_tasks.empty() ||
                        !fn.generic_parameters.empty() ||
                        fn.has_declared_effects ||
                        fn.scalar_while || fn.caught_throw_value ||
                        !fn.actor_spawns.empty() ||
                        !fn.actor_actions.empty() ||
                        !fn.actor_field_writes.empty() ||
                        !fn.ready_await_actor.empty() ||
                        !fn.done_await_actor.empty())
                    throw std::runtime_error(
                        "native Future<int> pipeline v1 requires one checked "
                        "pipeline in synchronous zero-argument main");
                const auto& pipeline = fn.future_pipelines.front();
                if (!pipeline.awaited || pipeline.stages.empty() ||
                        pipeline.plans.size() != pipeline.stages.size())
                    throw std::runtime_error(
                        "native Future<int> pipeline v1 requires one consumed "
                        "checked pipeline plan");
                if (fn.locals.empty() ||
                        fn.locals.front().future_await_name != pipeline.name)
                    throw std::runtime_error(
                        "native Future<int> pipeline await must be the first scalar local");
                if (std::any_of(
                        fn.locals.begin(), fn.locals.end(),
                        [&](const LocalBinding& local) {
                            return local.name == pipeline.name;
                        }))
                    throw std::runtime_error(
                        "source Future<int> binding cannot be shadowed by a scalar local");
                types::validate_future_pipeline_chain(pipeline.plans);
                for (const auto& plan : pipeline.plans)
                    types::validate_executable_future_pipeline_plan(plan);
            }
            if (!fn.source_tasks.empty()) {
                if (fn.name != "main" || !fn.parameters.empty() ||
                        fn.source_tasks.size() != 1 ||
                        !fn.generic_parameters.empty() ||
                        fn.has_declared_effects ||
                        fn.scalar_while ||
                        fn.caught_throw_value ||
                        !fn.actor_spawns.empty() ||
                        !fn.actor_actions.empty() ||
                        !fn.actor_field_writes.empty() ||
                        !fn.ready_await_actor.empty() ||
                        !fn.done_await_actor.empty())
                    throw std::runtime_error(
                        "native Task<int> v1 requires one capture-free Task in synchronous zero-argument main");
                const auto& task = fn.source_tasks.front();
                if (!task.awaited || !task.result ||
                        !native_capture_free_task_tree(*task.result))
                    throw std::runtime_error(
                        "native Task<int> v1 requires one consumed capture-free producer");
                const std::unordered_map<std::string, ScalarType> empty_variables;
                if (validate_scalar_expression(
                        *task.result, empty_variables, functions) !=
                        ScalarType::I64)
                    throw std::runtime_error(
                        "native Task<int> producer must return int");
                if (fn.locals.empty() ||
                        fn.locals.front().task_await_name != task.name)
                    throw std::runtime_error(
                        "native Task<int> v1 await must be the first scalar local");
                if (std::any_of(
                        fn.locals.begin(), fn.locals.end(),
                        [&](const LocalBinding& local) {
                            return local.name == task.name;
                        }))
                    throw std::runtime_error(
                        "source Task<int> binding cannot be shadowed by a scalar local");
            }
            for (const auto& binding : fn.locals) {
                // Validate before publishing the name: reject self/forward reads.
                if (binding.int_array) {
                    // Construction was parsed as exactly new Array<int>().
                    // Its initializer is a trusted runtime operation, never
                    // a user-callable scalar or transferable class value.
                } else if (binding.new_class || binding.move_class) {
                    if (classes.count(binding.class_name) == 0)
                        throw std::runtime_error("unknown class '" + binding.class_name + "'");
                    if (binding.move_class && !params.count(binding.move_source))
                        throw std::runtime_error("unknown class move source '" + binding.move_source + "'");
                    if (binding.new_class) {
                        const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                            [&](const ClassDecl& c) { return c.name == binding.class_name; });
                        if (cls == program.classes.end())
                            throw std::runtime_error("unknown class constructor");
                        if (binding.constructor_arguments.size() != cls->constructor_parameters.size())
                            throw std::runtime_error("native class constructor argument arity mismatch for '" +
                                                     cls->name + "'");
                        for (std::size_t i = 0; i < binding.constructor_arguments.size(); ++i) {
                            const auto& arg = binding.constructor_arguments[i];
                            const auto* field = constructor_field(*cls, i);
                            if (field && field->int_array && arg->kind == Expression::Kind::Take &&
                                    arg->left && arg->left->kind == Expression::Kind::Variable) {
                                validate_expression(*arg, params, functions);
                                continue;
                            }
                            if (!native_constructor_scalar_tree(*arg))
                                throw std::runtime_error("native scalar constructor argument requires pure integer expression");
                            validate_expression(*arg, params, functions);
                        }
                    }
                } else if (!binding.task_await_name.empty()) {
                    const bool task_exists = std::any_of(
                        fn.source_tasks.begin(), fn.source_tasks.end(),
                        [&](const SourceTaskI64Binding& task) {
                            return task.name == binding.task_await_name &&
                                task.awaited;
                        });
                    if (!task_exists)
                        throw std::runtime_error(
                            "native Task<int> await lost producer provenance");
                } else if (!binding.future_await_name.empty()) {
                    const bool future_exists = std::any_of(
                        fn.future_pipelines.begin(), fn.future_pipelines.end(),
                        [&](const SourceFuturePipelineI64Binding& pipeline) {
                            return pipeline.name == binding.future_await_name &&
                                pipeline.awaited;
                        });
                    if (!future_exists)
                        throw std::runtime_error(
                            "native Future<int> await lost pipeline provenance");
                } else {
                    validate_expression(*binding.initializer, params, functions);
                }
                if (live_actors.count(binding.name))
                    throw std::runtime_error("scalar local shadows actor reference '" + binding.name + "'");
                if (!params.insert(binding.name).second) {
                    throw std::runtime_error("duplicate owned local '" + binding.name
                        + "' at byte " + std::to_string(binding.offset));
                }
            }
            using NameSet = std::unordered_set<std::string>;
            std::function<void(const Statement&, NameSet&)> check_statement_names;
            check_statement_names = [&](const Statement& stmt, NameSet& names) {
                if (stmt.kind == Statement::Kind::ForIndex) {
                    validate_expression(*stmt.branch_local->initializer, names, functions);
                    NameSet body = names;
                    if (!body.insert(stmt.branch_local->name).second) throw std::runtime_error("native index loop shadows live owner");
                    validate_expression(*stmt.condition, body, functions); validate_expression(*stmt.loop_step, body, functions);
                    for (const auto& action : stmt.then_statements) check_statement_names(action, body);
                    return;
                }
                if (stmt.kind == Statement::Kind::Return) {
                    if (stmt.expression) validate_expression(*stmt.expression, names, functions);
                    return;
                }
                if (stmt.kind == Statement::Kind::ForEach) {
                    validate_expression(*stmt.expression, names, functions);
                    NameSet body = names;
                    if (!body.insert(stmt.branch_local->name).second)
                        throw std::runtime_error("native for-of element shadows a live owner");
                    for (const auto& action : stmt.then_statements) check_statement_names(action, body);
                    return;
                }
                if (stmt.kind == Statement::Kind::Conditional) {
                    if (stmt.condition) validate_expression(*stmt.condition, names, functions);
                    else {
                        validate_expression(*stmt.condition_lhs, names, functions);
                        validate_expression(*stmt.condition_rhs, names, functions);
                    }
                    NameSet then_names = names;
                    NameSet else_names = names;
                    for (const auto& action : stmt.then_statements)
                        check_statement_names(action, then_names);
                    for (const auto& action : stmt.else_statements)
                        check_statement_names(action, else_names);
                    // Neither arm's lexical locals escape into the parent scope.
                    return;
                }
                if (stmt.kind == Statement::Kind::ArrayCommitFilled) {
                    if (!names.count(stmt.receiver))
                        throw std::runtime_error("unknown array commit class receiver '" +
                            stmt.receiver + "'");
                    validate_expression(*stmt.array_commit_length, names, functions);
                    for (const auto& item : stmt.array_commit_values)
                        validate_expression(*item, names, functions);
                    return;
                }
                if (stmt.kind == Statement::Kind::ScopedLocal) {
                    const LocalBinding& local = *stmt.branch_local;
                    validate_expression(*local.initializer, names, functions);
                    if (!names.insert(local.name).second)
                        throw std::runtime_error("duplicate branch-local name '" +
                            local.name + "' at byte " + std::to_string(local.offset));
                    return;
                }
                if ((stmt.kind == Statement::Kind::FieldWrite ||
                        stmt.kind == Statement::Kind::ScalarWrite ||
                        (stmt.kind == Statement::Kind::Defer && !stmt.receiver.empty())) &&
                        !names.count(stmt.receiver))
                    throw std::runtime_error("unknown field receiver '" + stmt.receiver + "'");
                validate_expression(*stmt.expression, names, functions);
            };
            for (const auto& stmt : fn.statements)
                check_statement_names(stmt, params);
            // Validate effects in lexical order. In particular, a send after
            // stop must not pass by consulting the all-spawns reference map.
            for (const auto& action : fn.actor_actions) {
                if (action.kind == ActorAction::Kind::Send) {
                    const auto& send = fn.actor_sends.at(action.index);
                    if (!references.count(send.reference))
                        throw std::runtime_error("unknown actor message receiver '" + send.reference + "'");
                    if (!live_actors.count(send.reference))
                        throw std::runtime_error("actor message sent after stop '" + send.reference + "'");
                    validate_expression(*send.payload, params, functions);
                } else {
                    const auto& stop = fn.actor_stops.at(action.index);
                    if (!live_actors.erase(stop))
                        throw std::runtime_error("invalid or repeated actor stop '" + stop + "'");
                }
            }
            if (!fn.done_await_actor.empty() && live_actors.count(fn.done_await_actor))
                throw std::runtime_error("await get_done requires stop before await");
            if (!live_actors.empty())
                throw std::runtime_error("actor reference must be stopped before return");
            if (fn.caught_throw_value) {
                // The thrown i64 expression and scoped defers see the original
                // lexical environment. Catch binding enters only on handler
                // admission, before checking the catch return expression.
                validate_expression(*fn.caught_throw_value, params, functions);
                for (const auto& defer : fn.caught_throw_defers)
                    check_statement_names(defer, params);
                if (!params.insert(fn.catch_binding).second)
                    throw std::runtime_error("duplicate caught throw binding '" +
                        fn.catch_binding + "'");
            }
            for (const auto& guard : fn.early_returns) {
                if (guard.condition) validate_expression(*guard.condition, params, functions);
                else {
                    validate_expression(*guard.lhs, params, functions);
                    validate_expression(*guard.rhs, params, functions);
                }
                validate_expression(*guard.result, params, functions);
            }
            if (fn.result) validate_expression(*fn.result, params, functions);

            // Independent scalar type pass. Name/arity validation above must
            // complete first so this pass never guesses through unresolved
            // symbols or accidentally turns a typing failure into binding.
            using TypeMap = std::unordered_map<std::string, ScalarType>;
            TypeMap scalar_types;

            auto class_scalars = [&](const std::string& owner, const std::string& type) {
                if (type.empty()) return;
                for (const auto& leaf : layouts.at(type).leaves) {
                    scalar_types.emplace(owner + "." + leaf.path, leaf.field->scalar_type);
                    if (leaf.field->int_array) scalar_types.emplace(owner + "." + leaf.path + ".$element", leaf.field->array_element_type);
                }
            };
            for (const auto& parameter : fn.parameters) class_scalars(parameter.name, parameter.class_name);
            for (const auto& binding : fn.locals) class_scalars(binding.name, binding.class_name);
            for (const auto& param : fn.parameters)
                scalar_types.emplace(param.name, param.scalar_type);

            auto require_scalar = [&](const Expression& expr, ScalarType expected,
                                      const char* where, const TypeMap& env) {
                const ScalarType actual =
                    validate_scalar_expression(expr, env, functions);
                if (actual != expected)
                    throw std::runtime_error(std::string(where) + " requires " +
                        scalar_type_name(expected) + ", got " +
                        scalar_type_name(actual) + " at byte " +
                        std::to_string(expr.offset));
            };

            for (const auto& binding : fn.locals) {
                if (binding.int_array || binding.new_class || binding.move_class) {
                    scalar_types.emplace(binding.name, ScalarType::I64);
                    if (binding.int_array)
                        scalar_types.emplace(binding.name + ".$element", binding.array_element_type);
                    if (binding.new_class) {
                        const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                            [&](const ClassDecl& c) { return c.name == binding.class_name; });
                        for (std::size_t i = 0; i < binding.constructor_arguments.size(); ++i)
                            require_scalar(*binding.constructor_arguments[i],
                                cls->constructor_parameter_types.empty() ? ScalarType::I64 : cls->constructor_parameter_types.at(i),
                                "native class constructor argument", scalar_types);
                    }
                    continue;
                }
                if (!binding.task_await_name.empty()) {
                    if (binding.scalar_type != ScalarType::I64)
                        throw std::runtime_error(
                            "native Task<int> await result must bind int");
                } else {
                    require_scalar(*binding.initializer, binding.scalar_type,
                        "native scalar local initializer", scalar_types);
                }
                scalar_types.emplace(binding.name, binding.scalar_type);
            }

            std::function<void(const Statement&, TypeMap&)> check_statement_types;
            check_statement_types = [&](const Statement& stmt, TypeMap& env) {
                auto scoped_type = [&](const LocalBinding& local, TypeMap& scope) {
                    scope.emplace(local.name, local.scalar_type);
                    if (!local.class_name.empty()) for (const auto& leaf : layouts.at(local.class_name).leaves) {
                        scope.emplace(local.name + "." + leaf.path, leaf.field->scalar_type);
                        if (leaf.field->int_array) scope.emplace(local.name + "." + leaf.path + ".$element", leaf.field->array_element_type);
                    }
                };
                if (stmt.kind == Statement::Kind::ForIndex) {
                    require_scalar(*stmt.branch_local->initializer, ScalarType::I64, "index initializer", env);
                    TypeMap body = env; scoped_type(*stmt.branch_local, body);
                    require_scalar(*stmt.condition, ScalarType::Bool, "index condition", body);
                    require_scalar(*stmt.loop_step, ScalarType::I64, "index update", body);
                    for (const auto& action : stmt.then_statements) check_statement_types(action, body);
                    return;
                }
                if (stmt.kind == Statement::Kind::Return) {
                    if (stmt.expression) require_scalar(*stmt.expression, fn.return_type, "structured return", env);
                    return;
                }
                if (stmt.kind == Statement::Kind::ForEach) {
                    (void)validate_scalar_expression(*stmt.expression, env, functions);
                    TypeMap body = env; scoped_type(*stmt.branch_local, body);
                    for (const auto& action : stmt.then_statements) check_statement_types(action, body);
                    return;
                }
                if (stmt.kind == Statement::Kind::Conditional) {
                    if (stmt.condition) {
                        require_scalar(*stmt.condition, ScalarType::Bool,
                            "native if condition", env);
                    } else {
                        require_scalar(*stmt.condition_lhs, ScalarType::I64,
                            "native if equality left operand", env);
                        require_scalar(*stmt.condition_rhs, ScalarType::I64,
                            "native if equality right operand", env);
                    }
                    TypeMap then_env = env;
                    TypeMap else_env = env;
                    for (const auto& action : stmt.then_statements)
                        check_statement_types(action, then_env);
                    for (const auto& action : stmt.else_statements)
                        check_statement_types(action, else_env);
                    return;
                }
                if (stmt.kind == Statement::Kind::ArrayCommitFilled) {
                    require_scalar(*stmt.array_commit_length, ScalarType::I64,
                        "native array commit length", env);
                    for (const auto& item : stmt.array_commit_values)
                        require_scalar(*item, ScalarType::I64,
                            "native array commit fill", env);
                    return;
                }
                if (stmt.kind == Statement::Kind::ScopedLocal) {
                    const LocalBinding& local = *stmt.branch_local;
                    require_scalar(*local.initializer, local.scalar_type,
                        "native branch-local initializer", env);
                    scoped_type(local, env);
                    return;
                }
                if (stmt.kind == Statement::Kind::FieldWrite ||
                        (stmt.kind == Statement::Kind::Defer && !stmt.receiver.empty())) {
                    const auto target = env.find(stmt.receiver + "." + stmt.field);
                    require_scalar(*stmt.expression, target == env.end() ? ScalarType::I64 : target->second,
                        "native class field write", env);
                    return;
                }
                if (stmt.kind == Statement::Kind::ScalarWrite) {
                    require_scalar(*stmt.expression, env.at(stmt.receiver), "native scalar assignment", env);
                    return;
                }
                if (stmt.expression)
                    (void)validate_scalar_expression(*stmt.expression, env, functions);
            };
            for (const auto& stmt : fn.statements)
                check_statement_types(stmt, scalar_types);

            for (const auto& action : fn.actor_actions) {
                if (action.kind == ActorAction::Kind::Send)
                    require_scalar(*fn.actor_sends.at(action.index).payload,
                        ScalarType::I64, "native actor message payload", scalar_types);
            }
            if (fn.caught_throw_value)
                require_scalar(*fn.caught_throw_value, ScalarType::I64,
                    "native thrown scalar", scalar_types);
            for (const auto& guard : fn.early_returns) {
                if (guard.condition) {
                    require_scalar(*guard.condition, ScalarType::Bool,
                        "native guarded return condition", scalar_types);
                } else {
                    require_scalar(*guard.lhs, ScalarType::I64,
                        "native return guard left operand", scalar_types);
                    require_scalar(*guard.rhs, ScalarType::I64,
                        "native return guard right operand", scalar_types);
                }
                require_scalar(*guard.result, fn.return_type,
                    "native guarded return", scalar_types);
            }
            if (fn.result && !fn.returns_void)
                require_scalar(*fn.result, fn.return_type,
                    "native function return", scalar_types);
        }
    }

    bool extension_slot_exists(const std::string& receiver,
                               const std::string& method,
                               std::size_t arity) const {
        const auto family = extension_methods_.find(receiver);
        if (family == extension_methods_.end()) return false;
        const auto member = family->second.find(method);
        return member != family->second.end() && member->second.count(arity) != 0;
    }

    bool extension_method_exists(const std::string& receiver,
                                 const std::string& method) const {
        const auto family = extension_methods_.find(receiver);
        return family != extension_methods_.end() &&
               family->second.count(method) != 0;
    }

    bool extension_name_exists(const std::string& method) const {
        for (const auto& family : extension_methods_)
            if (family.second.count(method) != 0) return true;
        return false;
    }

    using AliasBindings = std::unordered_map<std::string, std::string>;

    void lower_extension_expression(std::unique_ptr<Expression>& slot,
                                    const AliasBindings& aliases) const {
        if (!slot) return;
        Expression& expr = *slot;
        lower_extension_expression(expr.left, aliases);
        lower_extension_expression(expr.right, aliases);
        lower_extension_expression(expr.condition, aliases);
        lower_extension_expression(expr.condition_lhs, aliases);
        lower_extension_expression(expr.condition_rhs, aliases);
        lower_extension_expression(expr.when_true, aliases);
        lower_extension_expression(expr.when_false, aliases);
        for (auto& argument : expr.arguments)
            lower_extension_expression(argument, aliases);

        if (expr.kind != Expression::Kind::ArrayCall || !expr.left ||
                expr.left->kind != Expression::Kind::Variable)
            return;

        const std::size_t explicit_arity = expr.arguments.size();
        const auto receiver_binding = aliases.find(expr.left->name);
        const bool receiver_is_named_alias =
                receiver_binding != aliases.end() && receiver_binding->second != "$raw$";
        if (receiver_is_named_alias &&
                extension_slot_exists(receiver_binding->second, expr.name,
                                      explicit_arity)) {
            auto call = make_expression(Expression::Kind::Call, expr.offset);
            call->name = extension_function_name(receiver_binding->second, expr.name);
            call->arguments.reserve(explicit_arity + 1);
            call->argument_names.reserve(explicit_arity + 1);
            call->arguments.push_back(std::move(expr.left));
            call->argument_names.emplace_back();
            for (auto& argument : expr.arguments)
                call->arguments.push_back(std::move(argument));
            if (expr.argument_names.empty()) {
                for (std::size_t i = 0; i < explicit_arity; ++i)
                    call->argument_names.emplace_back();
            } else {
                for (auto& name : expr.argument_names)
                    call->argument_names.push_back(std::move(name));
            }
            slot = std::move(call);
            return;
        }

        if (receiver_binding != aliases.end()) {
            const std::string actual = receiver_binding->second == "$raw$"
                ? "<raw-scalar>" : receiver_binding->second;
            if (receiver_is_named_alias &&
                    extension_method_exists(receiver_binding->second, expr.name)) {
                throw std::runtime_error(
                    "EXTENSION_ARITY_MISMATCH: method '" + expr.name +
                    "' exists for receiver alias '" + actual +
                    "' but not at explicit arity " +
                    std::to_string(explicit_arity) + " at byte " +
                    std::to_string(expr.offset));
            }
            if (extension_name_exists(expr.name)) {
                throw std::runtime_error(
                    "EXTENSION_RECEIVER_MISMATCH: method '" + expr.name +
                    "' is not defined for receiver alias '" + actual +
                    "' at byte " + std::to_string(expr.offset));
            }
        }
    }

    void lower_extension_statements(std::vector<Statement>& statements,
                                    AliasBindings aliases) const {
        for (auto& statement : statements) {
            lower_extension_expression(statement.expression, aliases);
            lower_extension_expression(statement.condition, aliases);
            lower_extension_expression(statement.condition_lhs, aliases);
            lower_extension_expression(statement.condition_rhs, aliases);
            lower_extension_expression(statement.array_commit_length, aliases);
            for (auto& item : statement.array_commit_values)
                lower_extension_expression(item, aliases);
            if (statement.branch_local) {
                lower_extension_expression(statement.branch_local->initializer, aliases);
                if (!statement.branch_local->pending_type_alias.empty()) {
                    aliases[statement.branch_local->name] =
                        statement.branch_local->pending_type_alias;
                } else if (!statement.branch_local->int_array &&
                           statement.branch_local->class_name.empty()) {
                    aliases[statement.branch_local->name] = "$raw$";
                }
            }
            if (!statement.then_statements.empty())
                lower_extension_statements(statement.then_statements, aliases);
            if (!statement.else_statements.empty())
                lower_extension_statements(statement.else_statements, aliases);
        }
    }

    void lower_extension_calls(Program& program) const {
        for (auto& fn : program.functions) {
            AliasBindings aliases;
            for (const auto& parameter : fn.parameters) {
                if (!parameter.pending_type_alias.empty()) {
                    aliases[parameter.name] = parameter.pending_type_alias;
                } else if (parameter.class_name.empty()) {
                    // Generic/scalar parameters have no named alias identity.
                    aliases[parameter.name] = "$raw$";
                }
            }
            for (auto& local : fn.locals) {
                for (auto& ctor : local.constructor_arguments)
                    lower_extension_expression(ctor, aliases);
                lower_extension_expression(local.initializer, aliases);
                if (!local.pending_type_alias.empty()) {
                    aliases[local.name] = local.pending_type_alias;
                } else if (!local.int_array && local.class_name.empty()) {
                    aliases[local.name] = "$raw$";
                }
            }
            lower_extension_statements(fn.statements, aliases);
            for (auto& guard : fn.early_returns) {
                lower_extension_expression(guard.condition, aliases);
                lower_extension_expression(guard.lhs, aliases);
                lower_extension_expression(guard.rhs, aliases);
                lower_extension_expression(guard.result, aliases);
            }
            for (auto& send : fn.actor_sends)
                lower_extension_expression(send.payload, aliases);
            for (auto& write : fn.actor_field_writes)
                lower_extension_expression(write.expression, aliases);
            lower_extension_expression(fn.caught_throw_value, aliases);
            for (auto& deferred : fn.caught_throw_defers)
                lower_extension_expression(deferred.expression, aliases);
            lower_extension_expression(fn.result, aliases);
        }
    }

    bool alias_requires_constraint_validation(const std::string& name) const {
        std::unordered_set<std::string> seen;
        std::string current = name;
        for (std::size_t depth = 0; depth < 129; ++depth) {
            if (constrained_aliases_.count(current)) return true;
            if (!seen.insert(current).second) return false;
            const auto target = scalar_alias_targets_.find(current);
            if (target == scalar_alias_targets_.end()) return false;
            current = target->second;
            if (current == "int" || current == "bool") return false;
        }
        return false;
    }

    void resolve_alias_uses(Program& program) const {
        auto reject_unvalidated_runtime_alias = [&](const std::string& name) {
            if (alias_requires_constraint_validation(name)) {
                throw std::runtime_error(
                    "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED: constrained alias chain from '" +
                    name + "' cannot enter a runtime value path before checked provenance");
            }
        };
        // Complete type aliases in trait/interface signatures never create
        // executable instances, but require validated scalar ABI types.
        for (auto& declaration : program.type_only_declarations) {
            if (declaration.kind == TypeOnlyDecl::Kind::Alias) continue;
            for (auto& member : declaration.methods) {
                for (auto& parameter_type : member.parameter_types) {
                    if (parameter_type != "int" && parameter_type != "bool") {
                        const std::string name = parameter_type;
                        if (alias_requires_constraint_validation(name)) {
                            throw std::runtime_error(
                                "CONSTRAINT_TYPE_ONLY_SIGNATURE_BRIDGE_REQUIRED: constrained alias '" +
                                name + "' cannot be erased in trait/interface parameter metadata");
                        }
                        parameter_type = resolve_scalar_alias(name) == ScalarType::Bool
                            ? "bool" : "int";
                    }
                }
                if (member.return_type != "int" &&
                        member.return_type != "bool" && member.return_type != "void") {
                    const std::string name = member.return_type;
                    if (alias_requires_constraint_validation(name)) {
                        throw std::runtime_error(
                            "CONSTRAINT_TYPE_ONLY_SIGNATURE_BRIDGE_REQUIRED: constrained alias '" +
                            name + "' cannot be erased in trait/interface return metadata");
                    }
                    member.return_type = resolve_scalar_alias(name) == ScalarType::Bool
                        ? "bool" : "int";
                }
            }
        }
        std::function<void(Statement&)> check_statement = [&](Statement& stmt) {
            if (stmt.branch_local && !stmt.branch_local->pending_type_alias.empty()) {
                reject_unvalidated_runtime_alias(stmt.branch_local->pending_type_alias);
                stmt.branch_local->scalar_type = resolve_scalar_alias(
                    stmt.branch_local->pending_type_alias);
                stmt.branch_local->pending_type_alias.clear();
            }
            for (auto& child : stmt.then_statements) check_statement(child);
            for (auto& child : stmt.else_statements) check_statement(child);
        };
        for (auto& fn : program.functions) {
            for (auto& param : fn.parameters) {
                if (param.pending_type_alias.empty()) continue;
                reject_unvalidated_runtime_alias(param.pending_type_alias);
                param.scalar_type = resolve_scalar_alias(param.pending_type_alias);
                param.pending_type_alias.clear();
                if (param.scalar_type == ScalarType::Bool &&
                        param.mode != Parameter::Mode::Value)
                    throw std::runtime_error("native bool borrow ABI is not yet admitted;"
                                             " scalar alias cannot bypass value-only bool");
            }
            if (!fn.pending_return_type_alias.empty()) {
                reject_unvalidated_runtime_alias(fn.pending_return_type_alias);
                fn.return_type = resolve_scalar_alias(fn.pending_return_type_alias);
                fn.pending_return_type_alias.clear();
            }
            for (auto& local : fn.locals) {
                if (local.pending_type_alias.empty()) continue;
                reject_unvalidated_runtime_alias(local.pending_type_alias);
                local.scalar_type = resolve_scalar_alias(local.pending_type_alias);
                local.pending_type_alias.clear();
                if (local.heap_owned && local.scalar_type == ScalarType::Bool)
                    throw std::runtime_error("native owned bool region lowering is not admitted");
            }
            for (auto& statement : fn.statements) check_statement(statement);
        }
    }

    static void validate_type_only_declarations(const Program& program) {
        using TypeKind = TypeOnlyDecl::Kind;
        std::unordered_map<std::string, std::size_t> declarations;
        for (std::size_t i = 0; i < program.type_only_declarations.size(); ++i) {
            const auto& decl = program.type_only_declarations[i];
            // Both 'declare type' and 'define type' produce an identical,
            // completed alias. Neither introduces a promise or an extra slot.
            if (!declarations.emplace(decl.name, i).second)
                throw std::runtime_error("TYPE_DECL_NAME_COLLISION: duplicate type-only declaration '"
                                         + decl.name + "'");
        }
        // Validate the complete catalog, including unused aliases
        // and completed aliases that target other completed definitions.
        // Native aliases are transparent i64/bool spellings, never
        // nominal class layouts, trait objects, or runtime constructors.
        std::vector<unsigned char> alias_state(program.type_only_declarations.size(), 0);
        std::function<ScalarType(std::size_t, std::size_t)> resolve_alias =
                [&](std::size_t index, std::size_t depth) -> ScalarType {
            const auto& decl = program.type_only_declarations[index];
            if (decl.kind != TypeKind::Alias)
                throw std::runtime_error("TYPE_ALIAS_KIND_MISMATCH: non-alias scalar target '"
                                         + decl.name + "'");
            if (alias_state[index] == 1)
                throw std::runtime_error("TYPE_ALIAS_CYCLE: cyclic type alias '" + decl.name + "'");
            if (depth > 128)
                throw std::runtime_error("TYPE_ALIAS_DEPTH_BUDGET: maximum alias depth is 128");
            if (decl.alias_target == "int") return ScalarType::I64;
            if (decl.alias_target == "bool") return ScalarType::Bool;
            auto target = declarations.find(decl.alias_target);
            if (target == declarations.end())
                throw std::runtime_error("TYPE_ALIAS_UNKNOWN_TARGET: '" + decl.name +
                                         "' names missing type '" + decl.alias_target + "'");
            const auto& parent = program.type_only_declarations[target->second];
            if (parent.kind != TypeKind::Alias)
                throw std::runtime_error("TYPE_ALIAS_KIND_MISMATCH: alias '" + decl.name +
                                         "' cannot refer to trait/interface '" + parent.name + "'");
            alias_state[index] = 1;
            ScalarType resolved = resolve_alias(target->second, depth + 1);
            alias_state[index] = 2;
            return resolved;
        };
        for (std::size_t i = 0; i < program.type_only_declarations.size(); ++i) {
            if (program.type_only_declarations[i].kind == TypeKind::Alias &&
                    alias_state[i] == 0)
                (void)resolve_alias(i, 1);
        }
        for (const auto& cls : program.classes) {
            if (declarations.count(cls.name))
                throw std::runtime_error("TYPE_DECL_NAME_COLLISION: class '" + cls.name
                                         + "' collides with a type-only declaration");
        }
        for (const auto& actor : program.actors) {
            if (declarations.count(actor.name))
                throw std::runtime_error("TYPE_DECL_NAME_COLLISION: actor '" + actor.name
                                         + "' collides with a type-only declaration");
        }
        for (const auto& contract : program.contracts) {
            if (declarations.count(contract.name))
                throw std::runtime_error("TYPE_DECL_NAME_COLLISION: contract '" + contract.name
                                         + "' collides with a type-only declaration");
        }
        // Reject missing parents, interface/trait kind laundering and cycles
        // before the LLVM backend sees any type metadata.
        std::vector<unsigned char> state(program.type_only_declarations.size(), 0);
        std::function<void(std::size_t, std::size_t)> visit =
                [&](std::size_t index, std::size_t depth) {
            if (state[index] == 2) return;
            if (state[index] == 1)
                throw std::runtime_error("TYPE_PARENT_CYCLE: cycle in trait/interface inheritance");
            if (depth > 128)
                throw std::runtime_error("TYPE_PARENT_DEPTH_BUDGET: maximum inheritance depth is 128");
            state[index] = 1;
            const auto& decl = program.type_only_declarations[index];
            for (const auto& parent : decl.parents) {
                auto found = declarations.find(parent);
                if (found == declarations.end())
                    throw std::runtime_error("TYPE_PARENT_UNKNOWN: '" + decl.name +
                                             "' extends unknown type '" + parent + "'");
                const auto& inherited = program.type_only_declarations[found->second];
                if (inherited.kind != decl.kind ||
                        inherited.kind == TypeKind::Alias)
                    throw std::runtime_error("TYPE_PARENT_KIND_MISMATCH: '" + decl.name +
                                             "' cannot extend a different type-only kind '" +
                                             parent + "'");
                visit(found->second, depth + 1);
            }
            state[index] = 2;
        };
        for (std::size_t i = 0; i < program.type_only_declarations.size(); ++i)
            visit(i, 1);
    }



    void advance() { token_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        if (token_.text == "gpu" || token_.text == "cpu" || token_.text == "quantum") {
            const char* domain = token_.text == "cpu"
                    ? "native cpu execution block boundary unsupported: "
                    : "native gpu/quantum boundary unsupported: ";
            throw std::runtime_error(std::string(domain)
                    + "validated effects, ownership, host/device lowering and explicit "
                    "backend admission required; nested cpu/gpu blocks cannot "
                    "silently execute on the host; no implicit CPU fallback at byte "
                    + std::to_string(token_.offset));
        }
        if (token_.text == "throw" || token_.text == "raise" ||
            token_.text == "panic" || token_.text == "recover") {
            throw std::runtime_error(
                "native exceptional-control boundary unsupported: "
                "typed unwind, defer/finally, recover and supervisor lowering required"
                " at byte " + std::to_string(token_.offset));
        }
        // Syntactic admission is a safety boundary: rejecting actor/channel
        // keywords also means no unverified borrowed reference can enter a
        // suspended/native execution frame or deferred callback.
        if (token_.text == "select" || token_.text == "nb-select" ||
            token_.text == "readch" || token_.text == "writech" ||
            token_.text == "nb" ||
            token_.text == "do" || token_.text == "await" ||
            token_.text == "yield" || token_.text == "cooperate") {
            throw std::runtime_error(
                "native async/channel/select boundary unsupported: "
                "suspension-aware borrow checking and cancellation/actor lowering required"
                " at byte " + std::to_string(token_.offset));
        }
        throw std::runtime_error(reason + " at byte " + std::to_string(token_.offset));
    }

    std::unordered_map<std::string, ForwardType> forwards_;
    std::unordered_map<std::string, ScalarAlias> defined_type_aliases_;
    std::string_view source_;
    Lexer lexer_;
    Token token_;
    std::unordered_set<std::string> module_names_;
    std::string method_scope_;
    std::unordered_set<std::string> class_receivers_;
    std::vector<Function> methods_;
    bool experimental_owned_int_;
    bool experimental_rt_scalar_loop_;
    // Type-only declarations retain source alias identity until checked
    // catalog validation and extension-call rewriting complete.
    std::unordered_map<std::string, std::string> scalar_alias_targets_;
    std::unordered_set<std::string> type_only_nominals_;
    std::unordered_set<std::string> constrained_aliases_;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_set<std::size_t>>> extension_methods_;
    std::unordered_set<std::string> extension_public_slots_;
    bool extension_header_active_ = false;
    std::string extension_receiver_alias_;
    std::string extension_method_name_;
    std::size_t extension_offset_ = 0;
    bool actor_threads_enum_seen_ = false;
    std::size_t expression_depth_ = 0;
    std::size_t conditional_depth_ = 0;
};

void resolve_future_pipeline_contracts(Program& program) {
    auto exact = [&](const std::string& name, std::size_t arity) -> Function& {
        Function* found = nullptr;
        for (auto& candidate : program.functions) {
            if (candidate.name != name || candidate.parameters.size() != arity)
                continue;
            if (found)
                throw std::runtime_error(
                    "Future pipeline callable is ambiguous by name/arity '" +
                    name + "'");
            found = &candidate;
        }
        if (!found)
            throw std::runtime_error(
                "Future pipeline references unknown callable '" + name + "'");
        return *found;
    };

    auto scalar_type = [] {
        types::AutoType type;
        type.kind = types::AutoTypeKind::Scalar;
        return type;
    };

    for (auto& producer : program.functions) {
        if (!producer.async_source_i64) continue;
        if (!producer.parameters.empty() || producer.returns_void ||
                producer.return_type != ScalarType::I64 ||
                !producer.return_class.empty() || !producer.return_sum.empty() ||
                !producer.generic_parameters.empty() ||
                producer.has_declared_effects || !producer.locals.empty() ||
                !producer.statements.empty() || !producer.early_returns.empty() ||
                !producer.actor_spawns.empty() || !producer.actor_actions.empty() ||
                !producer.actor_field_writes.empty() ||
                !producer.source_tasks.empty() ||
                !producer.future_pipelines.empty() ||
                !producer.ready_await_actor.empty() ||
                !producer.done_await_actor.empty() ||
                producer.scalar_while || producer.caught_throw_value ||
                !producer.result ||
                !native_capture_free_task_tree(*producer.result))
            throw std::runtime_error(
                "native async Future<int> producer v1 must be capture-free, "
                "zero-argument, pure and return one int expression");
    }

    for (auto& owner : program.functions) {
        for (auto& pipeline : owner.future_pipelines) {
            if (!pipeline.root_consumed)
                throw std::runtime_error(
                    "native Future<int> pipeline v1 execution requires an explicit rt take root");
            Function& producer = exact(pipeline.producer_function, 0);
            if (!producer.async_source_i64)
                throw std::runtime_error(
                    "Future<int> pipeline root must be an exact async fnc producer");

            pipeline.plans.clear();
            pipeline.plans.reserve(pipeline.stages.size());
            FunctionEffects prior = producer.declared_effects;
            for (std::size_t i = 0; i < pipeline.stages.size(); ++i) {
                Function& stage = exact(pipeline.stages[i], 1);
                const Parameter& parameter = stage.parameters.front();
                if (stage.async_source_i64 ||
                        parameter.mode != Parameter::Mode::Value ||
                        parameter.scalar_type != ScalarType::I64 ||
                        !parameter.class_name.empty() ||
                        !parameter.generic_type.empty() ||
                        !parameter.sum_type.empty() ||
                        stage.returns_void ||
                        stage.return_type != ScalarType::I64 ||
                        !stage.return_class.empty() || !stage.return_sum.empty() ||
                        !stage.generic_parameters.empty() ||
                        stage.has_declared_effects ||
                        !stage.locals.empty() || !stage.statements.empty() ||
                        !stage.early_returns.empty() ||
                        !stage.actor_spawns.empty() ||
                        !stage.actor_actions.empty() ||
                        !stage.actor_field_writes.empty() ||
                        !stage.source_tasks.empty() ||
                        !stage.future_pipelines.empty() ||
                        !stage.ready_await_actor.empty() ||
                        !stage.done_await_actor.empty() ||
                        stage.scalar_while || stage.caught_throw_value ||
                        !stage.result ||
                        !native_pure_scalar_tree(*stage.result))
                    throw std::runtime_error(
                        "native Future<int> pipeline stage v1 requires one pure "
                        "sync int -> int function with no captures/effects");

                types::FuturePipelinePlan plan;
                plan.declaration_identity =
                    static_cast<std::uint64_t>(stage.offset) + 1u;
                plan.pipeline_node_identity =
                    (static_cast<std::uint64_t>(pipeline.offset) + 1u) * 257u +
                    static_cast<std::uint64_t>(i) + 1u;
                plan.input_type = scalar_type();
                plan.output_type = scalar_type();
                plan.prior_effects = prior;
                plan.stage_effects = stage.declared_effects;
                plan.cumulative_effects =
                    prior.united(stage.declared_effects);
                plan.effect_provenance =
                    types::PipelineEffectProvenance::CompleteStatic;
                plan.input_ownership = i == 0
                    ? types::pipeline_root_ownership(pipeline.root_consumed)
                    : types::pipeline_internal_ownership();
                plan.root_stage = i == 0;
                types::validate_executable_future_pipeline_plan(plan);
                prior = plan.cumulative_effects;
                pipeline.plans.push_back(std::move(plan));
            }
            types::validate_future_pipeline_chain(pipeline.plans);

            // Type/shape checking stages plans only. Executable authority is
            // admitted by OwnershipChecker after the complete ownership pass.
        }
    }
}

} // namespace

Program parse_unresolved_program(std::string_view source, bool experimental_owned_int,
                      bool experimental_rt_scalar_loop) {
    Program program = Parser(source, experimental_owned_int,
                             experimental_rt_scalar_loop).parse();
    if (experimental_rt_scalar_loop &&
            (program.functions.size() != 1 || !program.functions.front().scalar_while ||
             !program.classes.empty() || !program.actors.empty() ||
             !program.module.empty()))
        throw std::runtime_error("experimental scalar loop requires exactly one standalone function");
    return program;
}

void resolve_and_validate_program(Program& program) {
    if (!program.imports.empty())
        throw std::runtime_error("native imports require filesystem module resolution");
    resolve_native_methods(program);
    resolve_native_sums(program);
    resolve_future_pipeline_contracts(program);
    Parser::validate_complete(program);
    validate_i64_ownership(program);
    try {
        validate_native_scalar_types(program);
    } catch (...) {
        // Preserve the established diagnostic/check ordering for the native
        // compiler while ensuring a later scalar-type failure revokes every
        // lowering authorization published by the ownership pass.
        for (auto& function : program.functions)
            for (auto& pipeline : function.future_pipelines)
                pipeline.ownership_admitted = false;
        throw;
    }
}

Program parse_program(std::string_view source, bool experimental_owned_int,
                      bool experimental_rt_scalar_loop) {
    auto program = parse_unresolved_program(source, experimental_owned_int, experimental_rt_scalar_loop);
    resolve_and_validate_program(program);
    return program;
}

} // namespace oreslang
