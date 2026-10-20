//! Canonical, configuration-free formatting for Oreslang.
//!
//! The formatter deliberately has no style options. `format_source` is the SDK
//! entry point used by the CLI, editors, build tools, and language servers.

use std::fmt;

mod inline_blocks;
mod keyword_blocks;
mod match_switch;
mod select;
mod unused_imports;

const INDENT: &str = "  ";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FormatError {
    line: usize,
    message: String,
}

impl FormatError {
    fn new(line: usize, message: impl Into<String>) -> Self {
        Self {
            line,
            message: message.into(),
        }
    }

    pub fn line(&self) -> usize {
        self.line
    }

    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for FormatError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "line {}: {}", self.line, self.message)
    }
}

impl std::error::Error for FormatError {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DefineKind {
    Switch,
    Match,
    Select,
    Module,
    Class,
    Actor,
    Interface,
    Contract,
    Trait,
    Struct,
    Callable,
    Other,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum BraceKind {
    Callable,
    Module,
    Class,
    Interface,
    Contract,
    Trait,
    Struct,
    Actor,
    Other,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct BraceFrame {
    kind: BraceKind,
    indent: usize,
    order: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct DefineFrame {
    kind: DefineKind,
    order: usize,
    brace_depth: usize,
    keyword_depth: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PrevEvent {
    Other,
    CallableEnd(usize),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum KeywordTerminator {
    FiKeyword,
    FiBraced,
    Done,
}

#[derive(Debug, Default, Clone, Copy)]
struct LexState {
    block_comment: bool,
    quote: Option<char>,
    escape: bool,
}

/// Split the compiler's compact optional-`as` end-delimited callable form
/// before the ordinary line-oriented indentation pass. This is deliberately
/// narrow: only free/module-style `fnc`/`routine` headers with an explicit
/// slim arrow and a final visible `end` are expanded. Unsupported actor,
/// static, or abstract callable forms remain untouched so the main formatter
/// can fail closed on them.
fn expand_compact_end_delimited_callables(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut lex = LexState::default();

    for (line_index, line) in source.split('\n').enumerate() {
        if line_index != 0 {
            output.push('\n');
        }

        let initial = lex;
        let words = visible_word_spans(line, initial);
        let mut next_lex = lex;
        let _ = scan_visible(line, &mut next_lex);
        lex = next_lex;

        let word = |span: WordSpan| &line[span.start..span.end];
        let forbidden = words.iter().any(|span| {
            matches!(word(*span), "actor" | "isoactor" | "static" | "abstract")
        });
        if forbidden
            || !words
                .iter()
                .any(|span| matches!(word(*span), "fnc" | "routine"))
        {
            output.push_str(line);
            continue;
        }

        let Some(close) = callable_parameter_close(line, line.len(), initial) else {
            output.push_str(line);
            continue;
        };
        let Some(as_span) = words
            .iter()
            .copied()
            .find(|span| span.start > close && word(*span) == "as")
        else {
            output.push_str(line);
            continue;
        };
        let Some(end_span) = words.last().copied().filter(|span| word(*span) == "end") else {
            output.push_str(line);
            continue;
        };
        if as_span.end > end_span.start {
            output.push_str(line);
            continue;
        }

        let header_tail = &line[close + 1..as_span.start];
        let mut header_lex = LexState::default();
        let visible_header: String = scan_visible(header_tail, &mut header_lex)
            .into_iter()
            .collect();
        let visible_header_words = visible_word_spans(&visible_header, LexState::default());
        let contains_body_word = visible_header_words.iter().any(|span| {
            matches!(
                &visible_header[span.start..span.end],
                "return"
                    | "if"
                    | "elif"
                    | "else"
                    | "fi"
                    | "do"
                    | "done"
                    | "select"
                    | "match"
                    | "switch"
                    | "const"
                    | "let"
                    | "val"
                    | "for"
                    | "while"
                    | "loop"
                    | "try"
                    | "catch"
                    | "finally"
                    | "throw"
                    | "raise"
                    | "panic"
                    | "await"
                    | "spawn"
                    | "break"
                    | "continue"
                    | "yield"
                    | "defer"
            )
        });
        if !visible_header.contains("->")
            || visible_header.contains(';')
            || visible_header.contains('{')
            || visible_header.contains('}')
            || contains_body_word
        {
            output.push_str(line);
            continue;
        }

        // A trailing comment after the callable closer remains attached to
        // the `end` line. Validate the whole suffix lexically rather than
        // trusting its prefix: `end /* comment */ stray` must stay on the
        // fail-closed path instead of being partially normalized.
        let suffix = &line[end_span.end..];
        let mut suffix_lex = LexState::default();
        let visible_suffix: String = scan_visible(suffix, &mut suffix_lex)
            .into_iter()
            .collect();
        if !visible_suffix.trim().is_empty() {
            output.push_str(line);
            continue;
        }

        let body = line[as_span.end..end_span.start].trim();
        output.push_str(line[..as_span.end].trim_end());
        output.push('\n');
        if !body.is_empty() {
            output.push_str(body);
            output.push('\n');
        }
        output.push_str(&line[end_span.start..]);
    }

    output
}

/// Format Oreslang source using the one canonical style.
///
/// The current formatter is intentionally configuration-free:
///
/// - two-space indentation, never tabs for indentation;
/// - braced static select arms, indented one level inside the select;
/// - compact callable bodies expanded to one statement per line;
/// - inline keyword conditionals expanded into indented then/else/fi lines;
/// - safe single-line return expressions receive explicit semicolon terminators;
/// - LF line endings and exactly one final newline;
/// - trailing whitespace removed outside comment payloads;
/// - at most one ordinary blank line;
/// - exactly two blank lines between sibling executable callable bodies;
/// - executable callable return separators are canonicalized to `->`;
/// - interface/trait callable signatures are canonicalized to `=>`;
/// - legacy scheduler spelling `rt yield` is canonicalized to `rt cooperate`;
/// - bare forever-loop aliases `for { ... }` / `for do ... done` are canonicalized to `loop`;
/// - `end`, `fi`, `done`, braces, class/interface/contract/trait/struct/module nesting,
///   actor bodies, and strings/comments are indentation-aware.
///
/// It canonicalizes recognized declaration-modifier prefixes, but does not
/// reorder unrelated declarations or unknown grammar whose meaning depends on
/// the compiler's evolving syntax.
pub fn format_source(source: &str) -> Result<String, FormatError> {
    reject_multiline_literals(source)?;
    let normalized = source.replace("\r\n", "\n").replace('\r', "\n");
    let normalized = canonicalize_hash_symbol_quotes(&normalized);
    // Expand a compact end-delimited callable before nested dispatch
    // normalizers insert line breaks. Otherwise a body beginning with
    // `do select` / match / switch can be split before the outer callable
    // frame is recognized, causing an inner brace/end to steal ownership.
    let normalized = expand_compact_end_delimited_callables(&normalized);
    let normalized = match_switch::normalize(&normalized);
    let normalized = select::normalize(&normalized)?;
    let normalized = inline_blocks::normalize(&normalized);
    let normalized = keyword_blocks::normalize(&normalized);
    let mut out: Vec<String> = Vec::new();
    let mut indent = 0usize;
    let mut block_comment_indent_shift = 0isize;
    let mut pending_blank = false;
    let mut prev_event = PrevEvent::Other;
    let mut define_stack: Vec<DefineFrame> = Vec::new();
    let mut brace_stack: Vec<BraceFrame> = Vec::new();
    let mut keyword_stack: Vec<KeywordTerminator> = Vec::new();
    let mut lex = LexState::default();
    let mut next_order = 0usize;
    // Expression-bodied pipe lambdas require an explicit statement semicolon.
    // While a body spans lines, indent it one continuation level without
    // changing the structural brace/keyword nesting used by the language.
    let mut lambda_expr_continuation: Option<usize> = None;
    let mut pending_assignment_continuation = false;
    // Parenthesized instance-method parameters can span lines. Preserve one
    // continuation indent without treating their declarations as statements.
    let mut pending_method_parameter_depth = 0usize;

    for (line_index, raw_line) in normalized.lines().enumerate() {
        let line_no = line_index + 1;

        // Interior lines of a block comment are opaque source text. Their
        // leading whitespace belongs to the comment, not Oreslang indentation.
        // Keep the bytes intact while still advancing the lexical state.
        if lex.block_comment {
            let mut comment_lex = lex;
            let visible = scan_visible(raw_line, &mut comment_lex);
            if visible.iter().all(|ch| ch.is_whitespace()) {
                // Reindent the whole comment uniformly relative to its opener,
                // preserving indentation *within* the comment and all text.
                let content = if raw_line.is_empty() {
                    String::new()
                } else if block_comment_indent_shift >= 0 {
                    format!(
                        "{}{}",
                        " ".repeat(block_comment_indent_shift as usize),
                        raw_line
                    )
                } else {
                    let leading = raw_line.len() - raw_line.trim_start_matches([' ', '\t']).len();
                    let removed = leading.min((-block_comment_indent_shift) as usize);
                    raw_line[removed..].to_owned()
                };
                out.push(content);
                lex = comment_lex;
                pending_blank = false;
                continue;
            }
        }

        // Ordinary trailing whitespace is canonicalized, but whitespace
        // *inside* a line comment or a continuing block comment is payload.
        let mut trailing_lex = lex;
        scan_visible(raw_line, &mut trailing_lex);
        let preserve_comment_tail = trailing_lex.block_comment || has_line_comment(raw_line, lex);
        let trimmed_end = if preserve_comment_tail {
            raw_line
        } else {
            raw_line.trim_end()
        };
        if trimmed_end.trim().is_empty() {
            pending_blank = true;
            continue;
        }

        let mut content = trimmed_end.trim_start().to_string();
        content = canonicalize_conditional_line(&content, lex);
        content = canonicalize_forever_loop_line(&content, lex);
        content = canonicalize_rt_cooperate_line(&content, lex);
        content = canonicalize_generator_sugar_line(&content, lex);
        content = canonicalize_declaration_line(
            &content,
            lex,
            keyword_stack.is_empty() && in_direct_instance_container(&define_stack, &brace_stack),
        );
        // Field syntax is contextual: do not rewrite local const/let/val,
        // actor state, method bodies, or trait signatures.
        if keyword_stack.is_empty() && in_direct_storage_container(&define_stack, &brace_stack) {
            content = canonicalize_storage_field_line(&content, lex).unwrap_or(content);
        }
        content =
            canonicalize_legacy_catchall(&content, define_stack.last(), brace_stack.len(), lex);
        let mut structural_lex = lex;
        let structural_content: String = scan_visible(&content, &mut structural_lex)
            .into_iter()
            .collect();
        let structural = structural_content.trim_start();

        let starts_end = starts_block_keyword(structural, "end");
        let starts_fi = starts_block_keyword(structural, "fi");
        let starts_done = starts_block_keyword(structural, "done");
        let branch_line =
            starts_block_keyword(structural, "else") || starts_block_keyword(structural, "elif");

        let fi_keyword_indented =
            starts_fi && matches!(keyword_stack.last(), Some(KeywordTerminator::FiKeyword));
        let branch_keyword_indented =
            branch_line && matches!(keyword_stack.last(), Some(KeywordTerminator::FiKeyword));

        if starts_fi {
            match keyword_stack.pop() {
                Some(KeywordTerminator::FiKeyword | KeywordTerminator::FiBraced) => {}
                Some(KeywordTerminator::Done) => {
                    return Err(FormatError::new(
                        line_no,
                        "encountered `fi` where `done` was expected",
                    ));
                }
                None => {
                    return Err(FormatError::new(
                        line_no,
                        "encountered `fi` without a matching `if ... then` block",
                    ));
                }
            }
        } else if starts_done {
            match keyword_stack.pop() {
                Some(KeywordTerminator::Done) => {}
                Some(KeywordTerminator::FiKeyword | KeywordTerminator::FiBraced) => {
                    return Err(FormatError::new(
                        line_no,
                        "encountered `done` where `fi` was expected",
                    ));
                }
                None => {
                    return Err(FormatError::new(
                        line_no,
                        "encountered `done` without a matching `... do` block",
                    ));
                }
            }
        }

        if branch_line
            && !matches!(
                keyword_stack.last(),
                Some(KeywordTerminator::FiKeyword | KeywordTerminator::FiBraced)
            )
        {
            return Err(FormatError::new(
                line_no,
                "encountered `else`/`elif` without a matching `if ... then` block",
            ));
        }

        let leading_closing_braces = leading_closing_braces(structural);
        let keyword_dedent = usize::from(
            starts_end || starts_done || fi_keyword_indented || branch_keyword_indented,
        );
        let pre_dedent = leading_closing_braces + keyword_dedent;
        indent = indent.saturating_sub(pre_dedent);

        let mut ended_end_callable = None;
        if starts_end {
            let frame = define_stack.last().ok_or_else(|| {
                FormatError::new(
                    line_no,
                    "encountered `end` without a matching define, callable, or dispatch block",
                )
            })?;
            if brace_stack.len() != frame.brace_depth || keyword_stack.len() != frame.keyword_depth
            {
                return Err(FormatError::new(
                    line_no,
                    "encountered `end` before closing a nested block",
                ));
            }
            if frame.kind == DefineKind::Callable {
                ended_end_callable = Some(indent);
            }
            define_stack.pop();
        }

        let signature_context = nearest_signature_context(&define_stack, &brace_stack);
        let closes_multiline_method =
            pending_method_parameter_depth > 0 && structural.trim_start().starts_with(')');
        let end_delimited_callable = end_delimited_callable_allowed(&define_stack, &brace_stack)
            && is_end_delimited_callable_header(structural);
        let callable_body = is_callable_body_header(structural, signature_context)
            || end_delimited_callable
            || (closes_multiline_method && structural.contains('{'));
        let callable_signature = is_callable_signature(structural, signature_context);
        if callable_body || callable_signature {
            content = canonicalize_callable_parameter_order(&content, lex);
        }
        if callable_body {
            content = canonicalize_callable_arrow(&content, "->", lex);
        } else if callable_signature {
            let desired = if nearest_contract_context(&define_stack, &brace_stack) {
                "->"
            } else {
                "=>"
            };
            content = canonicalize_callable_arrow(&content, desired, lex);
        }

        let brace_depth_before_line = brace_stack.len();
        let assignment_lambda_continuation =
            pending_assignment_continuation && starts_pipe_lambda_expression(structural);
        let current_indent = indent
            + usize::from(lambda_expr_continuation.is_some())
            + usize::from(assignment_lambda_continuation)
            + usize::from(pending_method_parameter_depth > 0 && !closes_multiline_method);
        let annotation_line = structural.starts_with('@');
        let desired_blanks = if out.is_empty() || is_closer_line(structural) {
            0
        } else if (callable_body || annotation_line)
            && matches!(prev_event, PrevEvent::CallableEnd(i) if i == current_indent)
        {
            // Members of a named container remain adjacent unless the author
            // explicitly separated them. Top-level callables keep the existing
            // two-blank-line canonical style.
            let in_named_container = define_stack.iter().any(|frame| {
                matches!(
                    frame.kind,
                    DefineKind::Module
                        | DefineKind::Class
                        | DefineKind::Actor
                        | DefineKind::Interface
                        | DefineKind::Contract
                        | DefineKind::Trait
                        | DefineKind::Struct
                )
            }) || brace_stack.iter().any(|frame| {
                matches!(
                    frame.kind,
                    BraceKind::Module
                        | BraceKind::Class
                        | BraceKind::Interface
                        | BraceKind::Contract
                        | BraceKind::Trait
                        | BraceKind::Struct
                        | BraceKind::Actor
                )
            });
            if in_named_container {
                usize::from(pending_blank)
            } else {
                2
            }
        } else if pending_blank {
            1
        } else {
            0
        };
        push_blank_lines(&mut out, desired_blanks);
        pending_blank = false;

        if !lex.block_comment && structural_lex.block_comment {
            let leading = raw_line.len() - raw_line.trim_start_matches([' ', '\t']).len();
            block_comment_indent_shift =
                (current_indent * INDENT.len()) as isize - leading as isize;
        }
        out.push(format!("{}{}", INDENT.repeat(current_indent), content));

        let brace_events: Vec<char> = structural
            .chars()
            .filter(|c| matches!(c, '{' | '}'))
            .collect();
        lex = structural_lex;

        let opens = brace_events.iter().filter(|&&c| c == '{').count();
        let closes = brace_events.iter().filter(|&&c| c == '}').count();
        let mut callable_open_available = callable_body;
        let mut container_open_available = brace_container_kind(structural);
        let mut ended_callable = ended_end_callable;
        for event in brace_events {
            match event {
                '{' => {
                    let kind = if callable_open_available {
                        callable_open_available = false;
                        BraceKind::Callable
                    } else if let Some(kind) = container_open_available.take() {
                        kind
                    } else {
                        BraceKind::Other
                    };
                    next_order += 1;
                    brace_stack.push(BraceFrame {
                        kind,
                        indent: current_indent,
                        order: next_order,
                    });
                }
                '}' => {
                    if define_stack
                        .last()
                        .is_some_and(|frame| frame.brace_depth >= brace_stack.len())
                    {
                        return Err(FormatError::new(
                            line_no,
                            "encountered `}` before closing an end-delimited block",
                        ));
                    }
                    let frame = brace_stack.pop().ok_or_else(|| {
                        FormatError::new(line_no, "encountered `}` without a matching `{`")
                    })?;
                    if frame.kind == BraceKind::Callable {
                        ended_callable = Some(frame.indent);
                    }
                }
                _ => unreachable!(),
            }
        }

        if let Some(ended_indent) = ended_callable {
            prev_event = PrevEvent::CallableEnd(ended_indent);
        } else {
            prev_event = PrevEvent::Other;
        }

        let nonleading_closes = closes.saturating_sub(leading_closing_braces);
        if opens >= nonleading_closes {
            indent += opens - nonleading_closes;
        } else {
            indent = indent.saturating_sub(nonleading_closes - opens);
        }

        if let Some(kind) = define_kind(structural) {
            if !structural.contains('{') && !ends_statement(structural) {
                next_order += 1;
                define_stack.push(DefineFrame {
                    kind,
                    order: next_order,
                    brace_depth: brace_stack.len(),
                    keyword_depth: keyword_stack.len(),
                });
                indent += 1;
            }
        }

        if end_delimited_callable {
            next_order += 1;
            define_stack.push(DefineFrame {
                kind: DefineKind::Callable,
                order: next_order,
                brace_depth: brace_stack.len(),
                keyword_depth: keyword_stack.len(),
            });
            indent += 1;
        }

        // Switch subjects may end with an optional semicolon, but the block
        // itself always closes with end, independently of its braced arms.
        if let Some(kind) =
            end_delimited_dispatch_kind(structural).filter(|_| !structural.contains('{'))
        {
            next_order += 1;
            define_stack.push(DefineFrame {
                kind,
                order: next_order,
                brace_depth: brace_stack.len(),
                keyword_depth: keyword_stack.len(),
            });
            indent += 1;
        }

        if opens_conditional_block(structural) && !branch_line {
            keyword_stack.push(KeywordTerminator::FiKeyword);
            indent += 1;
        } else if starts_word(structural, "if") && structural.contains('{') {
            keyword_stack.push(KeywordTerminator::FiBraced);
        } else if opens_do_block(structural) && !branch_line {
            keyword_stack.push(KeywordTerminator::Done);
            indent += 1;
        }

        if branch_line && branch_keyword_indented {
            indent += 1;
        }

        if let Some(base_brace_depth) = lambda_expr_continuation {
            // A semicolon inside a nested block lambda/object literal is not
            // the terminator for the outer expression-bodied lambda statement.
            if ends_statement(structural) && brace_stack.len() <= base_brace_depth {
                lambda_expr_continuation = None;
            }
        } else if opens_multiline_expression_body_pipe_lambda(structural) {
            lambda_expr_continuation = Some(brace_depth_before_line);
        }

        pending_assignment_continuation = structural.trim_end().ends_with('=');

        let parens_open = structural.chars().filter(|&ch| ch == '(').count();
        let parens_close = structural.chars().filter(|&ch| ch == ')').count();
        if pending_method_parameter_depth > 0 {
            let depth = pending_method_parameter_depth + parens_open;
            if parens_close > depth {
                return Err(FormatError::new(
                    line_no,
                    "unbalanced multiline method parameters",
                ));
            }
            pending_method_parameter_depth = depth - parens_close;
        } else if keyword_stack.is_empty()
            && in_direct_instance_container(&define_stack, &brace_stack)
            && structural.trim_end().ends_with('(')
            && !structural.contains('=')
            && !structural.contains('.')
            && parens_open > parens_close
        {
            pending_method_parameter_depth = parens_open - parens_close;
        }
    }

    if pending_method_parameter_depth != 0 {
        return Err(FormatError::new(
            normalized.lines().count().max(1),
            "unterminated multiline method parameters",
        ));
    }

    if lambda_expr_continuation.is_some() {
        return Err(FormatError::new(
            normalized.lines().count().max(1),
            "unterminated expression-bodied lambda statement (expected explicit ';')",
        ));
    }

    if lex.block_comment || lex.quote.is_some() {
        return Err(FormatError::new(
            normalized.lines().count().max(1),
            "unterminated string or block comment",
        ));
    }
    if !brace_stack.is_empty() {
        return Err(FormatError::new(
            normalized.lines().count().max(1),
            "unterminated `{` block",
        ));
    }
    if !define_stack.is_empty() {
        return Err(FormatError::new(
            normalized.lines().count().max(1),
            "unterminated define, callable, or dispatch block (expected `end`)",
        ));
    }
    if let Some(terminator) = keyword_stack.last() {
        let message = match terminator {
            KeywordTerminator::FiKeyword | KeywordTerminator::FiBraced => {
                "unterminated `if` block (expected `fi`)"
            }
            KeywordTerminator::Done => "unterminated `... do` block (expected `done`)",
        };
        return Err(FormatError::new(normalized.lines().count().max(1), message));
    }

    while out.last().is_some_and(|line| line.is_empty()) {
        out.pop();
    }

    let mut result = out.join("\n");
    result.push('\n');
    Ok(unused_imports::comment_unused(&result))
}

/// Returns `true` when `source` is already in canonical form.
pub fn is_formatted(source: &str) -> Result<bool, FormatError> {
    Ok(format_source(source)? == source)
}

fn push_blank_lines(out: &mut Vec<String>, count: usize) {
    while out.last().is_some_and(|line| line.is_empty()) {
        out.pop();
    }
    for _ in 0..count {
        out.push(String::new());
    }
}

/// Normalize catch-all aliases only while directly inside a legacy
/// end-delimited switch/match arm list. Nested arm bodies remain untouched.
fn canonicalize_legacy_catchall(
    line: &str,
    scope: Option<&DefineFrame>,
    brace_depth: usize,
    lex: LexState,
) -> String {
    // Never rewrite source text inside a continued multiline comment
    // or quoted literal. The structural lexer has not run yet.
    if lex.block_comment || lex.quote.is_some() {
        return line.to_owned();
    }
    let Some(scope) = scope.filter(|f| f.brace_depth == brace_depth) else {
        return line.to_owned();
    };
    match scope.kind {
        DefineKind::Switch if starts_word(line, "_") => {
            format!("default{}", &line[1..])
        }
        DefineKind::Match if starts_word(line, "on") => {
            let rest = line[2..].trim_start();
            // 'on _' is the only canonical catch-all spelling for match.
            // Do not turn it into the switch-only 'default' keyword.
            if starts_word(rest, "default") {
                format!("on _{}", &rest["default".len()..])
            } else {
                line.to_owned()
            }
        }
        _ => line.to_owned(),
    }
}

/// Recognize dispatch headers in expression position as well as statement
/// position. In particular, `return match<T> ... over` and a declaration
/// initializer `const T x = match<T> ... over` both own the following `end`.
/// Tokenization prevents literals/comments or `foo.match(...)` from opening a
/// fictitious block.
fn end_delimited_dispatch_kind(line: &str) -> Option<DefineKind> {
    let lexed = select::tokens(line);
    let mut index = 0usize;
    match lexed.first()?.text {
        "return" | "do" => index = 1,
        "match" | "switch" | "select" => {}
        _ => {
            // Source declaration or assignment: only a direct RHS dispatch
            // expression may own the next end. Do not mistake '==' for '='.
            let eq = lexed.iter().position(|token| token.text == "=")?;
            if lexed.get(eq + 1).is_some_and(|token| token.text == "=")
                || eq > 0 && lexed[eq - 1].text == "="
            {
                return None;
            }
            index = eq + 1;
        }
    }

    match lexed.get(index)?.text {
        "match" if lexed.iter().skip(index + 1).any(|t| t.text == "over") => {
            Some(DefineKind::Match)
        }
        "switch" => Some(DefineKind::Switch),
        "select" if lexed.iter().skip(index + 1).any(|t| t.text == "over") => {
            Some(DefineKind::Select)
        }
        _ => None,
    }
}

fn starts_word(line: &str, word: &str) -> bool {
    if !line.starts_with(word) {
        return false;
    }
    line[word.len()..]
        .chars()
        .next()
        .is_none_or(|c| !is_ident_char(c))
}

// Contextual names such as fi(), else(), and done() are ordinary calls,
// not structural keyword lines. Preserve them even after inline expansion.
fn starts_block_keyword(line: &str, word: &str) -> bool {
    starts_word(line, word)
        && !matches!(
            line[word.len()..].trim_start().chars().next(),
            Some('(' | '[' | '.')
        )
}

fn is_ident_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

fn leading_closing_braces(line: &str) -> usize {
    line.chars()
        .take_while(|c| c.is_whitespace() || *c == '}')
        .filter(|c| *c == '}')
        .count()
}

fn is_closer_line(line: &str) -> bool {
    line.starts_with('}')
        || starts_block_keyword(line, "end")
        || starts_block_keyword(line, "fi")
        || starts_block_keyword(line, "done")
}

fn ends_statement(line: &str) -> bool {
    line.trim_end().ends_with(';')
}

fn nearest_contract_context(defines: &[DefineFrame], braces: &[BraceFrame]) -> bool {
    let mut newest: Option<(usize, bool)> = None;

    for frame in defines {
        let is_contract = frame.kind == DefineKind::Contract;
        if newest.is_none_or(|(order, _)| frame.order > order) {
            newest = Some((frame.order, is_contract));
        }
    }
    for frame in braces {
        let is_contract = frame.kind == BraceKind::Contract;
        if newest.is_none_or(|(order, _)| frame.order > order) {
            newest = Some((frame.order, is_contract));
        }
    }

    newest.map(|(_, is_contract)| is_contract).unwrap_or(false)
}

fn nearest_signature_context(defines: &[DefineFrame], braces: &[BraceFrame]) -> bool {
    let mut newest: Option<(usize, bool)> = None;

    for frame in defines {
        let is_signature = matches!(
            frame.kind,
            DefineKind::Interface | DefineKind::Contract | DefineKind::Trait
        );
        if newest.is_none_or(|(order, _)| frame.order > order) {
            newest = Some((frame.order, is_signature));
        }
    }

    for frame in braces {
        let is_signature = match frame.kind {
            BraceKind::Interface | BraceKind::Contract | BraceKind::Trait => Some(true),
            BraceKind::Callable
            | BraceKind::Module
            | BraceKind::Class
            | BraceKind::Struct
            | BraceKind::Actor => Some(false),
            BraceKind::Other => None,
        };
        if let Some(is_signature) = is_signature {
            if newest.is_none_or(|(order, _)| frame.order > order) {
                newest = Some((frame.order, is_signature));
            }
        }
    }

    newest
        .map(|(_, is_signature)| is_signature)
        .unwrap_or(false)
}

// Only class/actor immediate members can use implicit receivers. Nested
// local functions and expressions must not be rewritten as methods.
fn in_direct_instance_container(define_stack: &[DefineFrame], brace_stack: &[BraceFrame]) -> bool {
    let latest_define = define_stack.last().map(|frame| {
        (
            frame.order,
            matches!(frame.kind, DefineKind::Class | DefineKind::Actor),
        )
    });
    let latest_brace = brace_stack.last().map(|frame| {
        (
            frame.order,
            matches!(frame.kind, BraceKind::Class | BraceKind::Actor),
        )
    });
    match (latest_define, latest_brace) {
        (Some((a, value)), Some((b, other))) => {
            if a > b {
                value
            } else {
                other
            }
        }
        (Some((_, value)), None) | (None, Some((_, value))) => value,
        (None, None) => false,
    }
}

// Concrete class member slots use final (fixed binding) and mut (mutable
// referent), while lexical bindings retain const/val/let. Do not rewrite
// unimplemented named-struct storage, actor state, trait contracts, or
// code nested within a callable.
fn in_direct_storage_container(define_stack: &[DefineFrame], brace_stack: &[BraceFrame]) -> bool {
    let latest_define = define_stack
        .last()
        .map(|frame| (frame.order, matches!(frame.kind, DefineKind::Class)));
    let latest_brace = brace_stack
        .last()
        .map(|frame| (frame.order, matches!(frame.kind, BraceKind::Class)));
    match (latest_define, latest_brace) {
        (Some((a, value)), Some((b, other))) => {
            if a > b {
                value
            } else {
                other
            }
        }
        (Some((_, value)), None) | (None, Some((_, value))) => value,
        (None, None) => false,
    }
}

// This is deliberately conservative until the formatter has a full parser:
// only single-line, typed member declarations with recognizable prefixes and
// an unambiguous initializer separator are normalized. Suspicious syntax,
// nested expression prefixes, comments, and multiline signatures are left
// unchanged for compiler diagnostics rather than rewritten by guesswork.
// Only a top-level declaration separator may become a field initializer.
// Named generic arguments use '=' (e.g. Box<messages=Message>) and must never
// be mistaken for the member initializer; nested type contracts may use ':'.
// Leave malformed, commented or callable prefixes untouched.
fn canonical_storage_field_separator(line: &str, initial: LexState) -> Option<usize> {
    if initial.block_comment || initial.quote.is_some() {
        return None;
    }
    let mut chars = line.char_indices().peekable();
    let mut generic_depth = 0usize;
    while let Some((at, ch)) = chars.next() {
        let next = chars.peek().map(|(_, c)| *c);
        if ch == '/' && matches!(next, Some('/' | '*')) {
            return None;
        }
        if matches!(ch, '"' | '\'' | '`') {
            return None; // literals never start a field declaration
        }
        if ch == '<' {
            generic_depth += 1;
            continue;
        }
        if ch == '>' {
            generic_depth = generic_depth.checked_sub(1)?;
            continue;
        }
        if generic_depth == 0 {
            if matches!(ch, '(' | ')' | '[' | ']' | '{' | '}') {
                return None; // callable or compound non-field prefix
            }
            if matches!(ch, '=' | ':' | ';') {
                return Some(at);
            }
        }
    }
    None
}

fn canonicalize_storage_field_line(line: &str, initial: LexState) -> Option<String> {
    let separator = canonical_storage_field_separator(line, initial)?;
    // A legacy uninitialized val field may still be filled in its class
    // constructor. Rewriting it to final mut would manufacture an invalid
    // declaration before the coordinated breaking constructor migration.
    // Preserve ALL uninitialized member spellings until both compilers
    // enforce declaration-site initialization consistently.
    if line.as_bytes().get(separator) == Some(&b';') {
        return None;
    }
    let lhs = &line[..separator];
    if lhs
        .chars()
        .any(|c| matches!(c, '(' | ')' | '{' | '}' | '[' | ']'))
        || lhs.contains("//")
        || lhs.contains("/*")
        || lhs.contains("->")
        || lhs.contains("=>")
    {
        return None;
    }
    let words = visible_word_spans(lhs, initial);
    if words.len() < 2 {
        return None;
    }
    let mut cursor = 0usize;
    let visibility = match &lhs[words[cursor].start..words[cursor].end] {
        "pub" | "private" => {
            cursor += 1;
            Some(&lhs[words[0].start..words[0].end])
        }
        _ => None,
    };
    if cursor >= words.len() {
        return None;
    }
    let binding = match &lhs[words[cursor].start..words[cursor].end] {
        "const" | "final" => {
            cursor += 1;
            Some("final")
        }
        "val" => {
            cursor += 1;
            Some("final mut")
        }
        "let" => {
            cursor += 1;
            None
        }
        _ => None,
    };
    let mut mutable = false;
    if cursor < words.len() && &lhs[words[cursor].start..words[cursor].end] == "mut" {
        if binding == Some("final mut") {
            return None; // duplicate val mut remains a compiler error
        }
        mutable = true;
        cursor += 1;
    }
    if words.len() < cursor + 2 {
        return None; // not a typed field (e.g. legacy name: Type)
    }
    let field = words.last()?;
    if !lhs[field.end..].trim().is_empty() {
        return None;
    }
    let type_and_name = lhs[words[cursor].start..field.end].trim();
    if type_and_name.is_empty() || type_and_name.contains("::") {
        return None;
    }
    // The declaration type may contain generic args, but the text before
    // the name must not itself contain an initializer or a callable.
    let mut prefix = String::new();
    if let Some(vis) = visibility {
        prefix.push_str(vis);
        prefix.push(' ');
    }
    if let Some(binding) = binding {
        prefix.push_str(binding);
        prefix.push(' ');
    } else if mutable {
        prefix.push_str("mut ");
    }
    if mutable && binding == Some("final") {
        prefix.push_str("mut ");
    }
    prefix.push_str(type_and_name);
    if line.as_bytes().get(separator) == Some(&b';') {
        prefix.push_str(&line[separator..]);
    } else {
        prefix.push(':');
        prefix.push(' ');
        prefix.push_str(line[separator + 1..].trim_start());
    }
    Some(prefix)
}

fn canonicalize_declaration_line(line: &str, initial: LexState, instance_member: bool) -> String {
    let words = visible_word_spans(line, initial);
    if words.is_empty() {
        return line.to_string();
    }

    (instance_member
        .then(|| canonicalize_instance_method_fnc(line, &words))
        .flatten())
    .or_else(|| canonicalize_class_declaration(line, &words))
    .or_else(|| canonicalize_defined_interface_or_contract(line, &words))
    .or_else(|| canonicalize_actor_declaration(line, &words))
    .or_else(|| canonicalize_callable_declaration(line, &words))
    .unwrap_or_else(|| line.to_string())
}

// Legacy "fnc name(...)" inside a class or actor is an instance method,
// not a static, reifiable function. Drop only that declaration marker.
// In particular never touch static fnc, comments, strings or nested callables.
fn canonicalize_instance_method_fnc(line: &str, words: &[WordSpan]) -> Option<String> {
    let is_modifier = |word: &str| {
        matches!(
            word,
            "pub"
                | "private"
                | "async"
                | "suspends"
                | "suspend"
                | "critical"
                | "structural"
                | "abstract"
        )
    };
    let mut prefix_len = 0;
    let mut saw_fnc = false;
    let mut modifiers: Vec<&str> = Vec::new();
    for span in words {
        let word = &line[span.start..span.end];
        if word == "fnc" {
            if saw_fnc {
                return None;
            }
            saw_fnc = true;
        } else if is_modifier(word) {
            if modifiers.contains(&word) {
                return None;
            }
            modifiers.push(word);
        } else {
            break;
        }
        prefix_len += 1;
    }
    if !saw_fnc || prefix_len >= words.len() {
        return None;
    }
    // No static modifier, comments, punctuation or expressions may appear
    // among the declaration prefix. This also rejects invalid combinations
    // without manufacturing plausible-looking source.
    if !plain_word_prefix(line, &words[..=prefix_len])
        || (modifiers.contains(&"pub") && modifiers.contains(&"private"))
        || (modifiers.contains(&"suspend") && modifiers.contains(&"suspends"))
    {
        return None;
    }
    let name = &words[prefix_len];
    let suffix = line[name.end..].trim_start();
    if !(suffix.starts_with('(') || suffix.starts_with('<')) {
        return None;
    }

    let mut canonical: Vec<&str> = Vec::new();
    for modifier in [
        "pub",
        "private",
        "async",
        "suspend",
        "suspends",
        "critical",
        "structural",
        "abstract",
    ] {
        if modifiers.contains(&modifier) {
            canonical.push(modifier);
        }
    }
    let mut out = String::with_capacity(line.len());
    out.push_str(&line[..words[0].start]);
    if !canonical.is_empty() {
        out.push_str(&canonical.join(" "));
        out.push(' ');
    }
    out.push_str(&line[name.start..]);
    Some(out)
}

fn canonicalize_class_declaration(line: &str, words: &[WordSpan]) -> Option<String> {
    let is_prefix_word =
        |word: &str| matches!(word, "define" | "class" | "pub" | "private" | "abstract");
    let prefix_len = words
        .iter()
        .take_while(|span| is_prefix_word(&line[span.start..span.end]))
        .count();
    if prefix_len == 0 || prefix_len == words.len() {
        return None;
    }

    let prefix = &words[..prefix_len];
    if !plain_word_prefix(line, prefix) {
        return None;
    }

    let mut saw_define = false;
    let mut visibility: Option<&str> = None;
    let mut is_abstract = false;
    let mut saw_class = false;

    for span in prefix {
        match &line[span.start..span.end] {
            "define" if !saw_define => saw_define = true,
            "pub" | "private" if visibility.is_none() => {
                visibility = Some(&line[span.start..span.end])
            }
            "abstract" if !is_abstract => is_abstract = true,
            "class" if !saw_class => saw_class = true,
            _ => return None,
        }
    }

    if !saw_define || !saw_class {
        return None;
    }

    let mut canonical = vec!["define"];
    if let Some(visibility) = visibility {
        canonical.push(visibility);
    }
    if is_abstract {
        canonical.push("abstract");
    }
    canonical.push("class");

    Some(rewrite_word_prefix(line, prefix, &canonical))
}

fn canonicalize_defined_interface_or_contract(line: &str, words: &[WordSpan]) -> Option<String> {
    let is_prefix_word = |word: &str| {
        matches!(
            word,
            "define" | "interface" | "contract" | "pub" | "private" | "static"
        )
    };
    let prefix_len = words
        .iter()
        .take_while(|span| is_prefix_word(&line[span.start..span.end]))
        .count();
    if prefix_len == 0 || prefix_len == words.len() {
        return None;
    }

    let prefix = &words[..prefix_len];
    if !plain_word_prefix(line, prefix) {
        return None;
    }

    let mut saw_define = false;
    let mut visibility: Option<&str> = None;
    let mut is_static = false;
    let mut kind: Option<&str> = None;

    for span in prefix {
        match &line[span.start..span.end] {
            "define" if !saw_define => saw_define = true,
            "pub" | "private" if visibility.is_none() => {
                visibility = Some(&line[span.start..span.end])
            }
            "static" if !is_static => is_static = true,
            "interface" | "contract" if kind.is_none() => kind = Some(&line[span.start..span.end]),
            _ => return None,
        }
    }

    let kind = kind?;
    if !saw_define || (is_static && kind != "contract") {
        return None;
    }

    let mut canonical = vec!["define"];
    if let Some(visibility) = visibility {
        canonical.push(visibility);
    }
    if is_static {
        canonical.push("static");
    }
    canonical.push(kind);

    Some(rewrite_word_prefix(line, prefix, &canonical))
}

fn canonicalize_actor_declaration(line: &str, words: &[WordSpan]) -> Option<String> {
    let is_prefix_word = |word: &str| {
        matches!(
            word,
            "pub"
                | "private"
                | "static"
                | "async"
                | "generator"
                | "nlex"
                | "pure"
                | "trap"
                | "structural"
                | "abstract"
                | "shared"
                | "untrusted"
                | "actor"
                | "isoactor"
                | "fnc"
                | "routine"
        )
    };

    let prefix_len = words
        .iter()
        .take_while(|span| is_prefix_word(&line[span.start..span.end]))
        .count();
    if prefix_len == 0 || prefix_len == words.len() {
        return None;
    }

    let prefix = &words[..prefix_len];
    if !plain_word_prefix(line, prefix) {
        return None;
    }

    let mut visibility: Option<&str> = None;
    let mut is_static = false;
    let mut is_async = false;
    let mut is_generator = false;
    let mut is_nlex = false;
    let mut is_pure = false;
    let mut is_trap = false;
    let mut is_structural = false;
    let mut is_abstract = false;
    let mut actor_mode: Option<&str> = None;
    let mut actor_marker: Option<&str> = None;
    let mut kind: Option<&str> = None;

    for span in prefix {
        let word = &line[span.start..span.end];
        let duplicate = match word {
            "pub" | "private" => {
                if visibility.is_some() {
                    true
                } else {
                    visibility = Some(word);
                    false
                }
            }
            "static" => set_once(&mut is_static),
            "async" => set_once(&mut is_async),
            "generator" => set_once(&mut is_generator),
            "nlex" => set_once(&mut is_nlex),
            "pure" => set_once(&mut is_pure),
            "trap" => set_once(&mut is_trap),
            "structural" => set_once(&mut is_structural),
            "abstract" => set_once(&mut is_abstract),
            "shared" | "untrusted" => {
                if actor_mode.is_some() {
                    true
                } else {
                    actor_mode = Some(word);
                    false
                }
            }
            "actor" | "isoactor" => {
                if actor_marker.is_some() {
                    true
                } else {
                    actor_marker = Some(word);
                    false
                }
            }
            "fnc" | "routine" => {
                if kind.is_some() {
                    true
                } else {
                    kind = Some(word);
                    false
                }
            }
            _ => true,
        };
        if duplicate {
            return None;
        }
    }

    let actor_marker = actor_marker?;
    let mut canonical = Vec::new();
    if let Some(visibility) = visibility {
        canonical.push(visibility);
    }
    if is_static {
        canonical.push("static");
    }
    if is_async {
        canonical.push("async");
    }
    if is_generator {
        canonical.push("generator");
    }
    if is_nlex {
        canonical.push("nlex");
    }
    if is_pure {
        canonical.push("pure");
    }
    if is_trap {
        canonical.push("trap");
    }
    if is_structural {
        canonical.push("structural");
    }
    if is_abstract {
        canonical.push("abstract");
    }
    if let Some(actor_mode) = actor_mode {
        canonical.push(actor_mode);
    }
    canonical.push(actor_marker);
    if let Some(kind) = kind {
        canonical.push(kind);
    }

    Some(rewrite_word_prefix(line, prefix, &canonical))
}

fn canonicalize_callable_declaration(line: &str, words: &[WordSpan]) -> Option<String> {
    let is_prefix_word = |word: &str| {
        matches!(
            word,
            "pub"
                | "private"
                | "static"
                | "async"
                | "generator"
                | "nlex"
                | "pure"
                | "trap"
                | "structural"
                | "abstract"
                | "quantum"
                | "fnc"
                | "routine"
        )
    };

    let prefix_len = words
        .iter()
        .take_while(|span| is_prefix_word(&line[span.start..span.end]))
        .count();
    if prefix_len == 0 || prefix_len == words.len() {
        return None;
    }

    let prefix = &words[..prefix_len];
    if !plain_word_prefix(line, prefix) {
        return None;
    }

    let mut visibility: Option<&str> = None;
    let mut is_static = false;
    let mut is_async = false;
    let mut is_generator = false;
    let mut is_nlex = false;
    let mut is_pure = false;
    let mut is_trap = false;
    let mut is_structural = false;
    let mut is_abstract = false;
    let mut is_quantum = false;
    let mut kind: Option<&str> = None;

    for span in prefix {
        let word = &line[span.start..span.end];
        let duplicate = match word {
            "pub" | "private" => {
                if visibility.is_some() {
                    true
                } else {
                    visibility = Some(word);
                    false
                }
            }
            "static" => set_once(&mut is_static),
            "async" => set_once(&mut is_async),
            "generator" => set_once(&mut is_generator),
            "nlex" => set_once(&mut is_nlex),
            "pure" => set_once(&mut is_pure),
            "trap" => set_once(&mut is_trap),
            "structural" => set_once(&mut is_structural),
            "abstract" => set_once(&mut is_abstract),
            "quantum" => set_once(&mut is_quantum),
            "fnc" | "routine" => {
                if kind.is_some() {
                    true
                } else {
                    kind = Some(word);
                    false
                }
            }
            _ => true,
        };
        if duplicate {
            return None;
        }
    }

    let kind = kind?;

    // The compiler currently admits quantum only on fnc/static fnc. Keep
    // unsupported combinations untouched so semantic diagnostics remain the
    // compiler's job rather than being obscured by formatter normalization.
    if is_quantum && kind != "fnc" {
        return None;
    }

    let mut canonical = Vec::new();
    if let Some(visibility) = visibility {
        canonical.push(visibility);
    }
    if is_quantum {
        canonical.push("quantum");
    }
    if is_static {
        canonical.push("static");
    }
    if is_async {
        canonical.push("async");
    }
    if is_generator {
        canonical.push("generator");
    }
    if is_nlex {
        canonical.push("nlex");
    }
    if is_pure {
        canonical.push("pure");
    }
    if is_trap {
        canonical.push("trap");
    }
    if is_structural {
        canonical.push("structural");
    }
    if is_abstract {
        canonical.push("abstract");
    }
    canonical.push(kind);

    Some(rewrite_word_prefix(line, prefix, &canonical))
}

fn set_once(value: &mut bool) -> bool {
    if *value {
        true
    } else {
        *value = true;
        false
    }
}

fn plain_word_prefix(line: &str, words: &[WordSpan]) -> bool {
    let Some(first) = words.first() else {
        return false;
    };
    if !line[..first.start].trim().is_empty() {
        return false;
    }
    words
        .windows(2)
        .all(|pair| line[pair[0].end..pair[1].start].trim().is_empty())
}

fn rewrite_word_prefix(line: &str, words: &[WordSpan], canonical: &[&str]) -> String {
    let first = words.first().expect("non-empty declaration prefix");
    let last = words.last().expect("non-empty declaration prefix");
    let mut result = String::with_capacity(line.len());
    result.push_str(&line[..first.start]);
    result.push_str(&canonical.join(" "));
    result.push_str(&line[last.end..]);
    result
}

fn define_kind(line: &str) -> Option<DefineKind> {
    // Bare, end-delimited actor declarations are legal without 'define'.
    // Keep this narrow: actor callables (actor fnc) must never open a frame.
    let header: Vec<&str> = line.split_whitespace().collect();
    if header.last() == Some(&"as") {
        if let Some(actor_index) = header
            .iter()
            .position(|word| matches!(*word, "actor" | "isoactor"))
        {
            if actor_index + 3 == header.len()
                && header[..actor_index]
                    .iter()
                    .all(|word| matches!(*word, "pub" | "private" | "shared" | "untrusted"))
            {
                return Some(DefineKind::Actor);
            }
        }
    }
    let mut words = line.split_whitespace();
    if words.next()? != "define" {
        return None;
    }

    for word in words {
        match word {
            "pub" | "private" | "abstract" | "shared" | "untrusted" | "static" => continue,
            "module" => return Some(DefineKind::Module),
            "class" => return Some(DefineKind::Class),
            "actor" | "isoactor" => return Some(DefineKind::Actor),
            "interface" => return Some(DefineKind::Interface),
            "contract" => return Some(DefineKind::Contract),
            "trait" => return Some(DefineKind::Trait),
            "struct" => return Some(DefineKind::Struct),
            _ => return Some(DefineKind::Other),
        }
    }
    Some(DefineKind::Other)
}

fn brace_container_kind(line: &str) -> Option<BraceKind> {
    let prefix = line.split('{').next()?;
    let words: Vec<&str> = prefix
        .split(|c: char| !is_ident_char(c))
        .filter(|word| !word.is_empty())
        .collect();
    if words.contains(&"interface") {
        Some(BraceKind::Interface)
    } else if words.contains(&"contract") {
        Some(BraceKind::Contract)
    } else if words.contains(&"trait") {
        Some(BraceKind::Trait)
    } else if words.contains(&"class") {
        Some(BraceKind::Class)
    } else if words.contains(&"module") {
        Some(BraceKind::Module)
    } else if words.contains(&"struct") {
        Some(BraceKind::Struct)
    } else if words.contains(&"actor") {
        Some(BraceKind::Actor)
    } else {
        None
    }
}

fn end_delimited_callable_allowed(defines: &[DefineFrame], braces: &[BraceFrame]) -> bool {
    let mut newest: Option<(usize, bool)> = None;

    for frame in defines {
        let allowed = frame.kind == DefineKind::Module;
        if newest.is_none_or(|(order, _)| frame.order > order) {
            newest = Some((frame.order, allowed));
        }
    }
    for frame in braces {
        let allowed = frame.kind == BraceKind::Module;
        if newest.is_none_or(|(order, _)| frame.order > order) {
            newest = Some((frame.order, allowed));
        }
    }

    newest.map(|(_, allowed)| allowed).unwrap_or(true)
}

fn is_end_delimited_callable_header(structural: &str) -> bool {
    let line = structural.trim();
    if line.contains('{')
        || line.ends_with(';')
        || contains_word(line, "actor")
        || contains_word(line, "isoactor")
        || contains_word(line, "static")
        || contains_word(line, "abstract")
    {
        return false;
    }
    if !(contains_word(line, "fnc") || contains_word(line, "routine")) || !line.contains('(') {
        return false;
    }
    let Some(close) = callable_parameter_close(line, line.len(), LexState::default()) else {
        return false;
    };
    line[close + 1..].trim_start().starts_with("->")
}

fn is_callable_body_header(structural: &str, signature_context: bool) -> bool {
    if !structural.contains('{') || !structural.contains('(') {
        return false;
    }
    let lower = structural.trim_start();
    for kw in [
        "if", "for", "while", "switch", "match", "catch", "recover", "defer", "when", "case",
        "default",
    ] {
        if starts_word(lower, kw) {
            return false;
        }
    }
    if lower.starts_with("obj{") || lower.starts_with("arr[") {
        return false;
    }
    if contains_word(lower, "fnc") || contains_word(lower, "routine") {
        return true;
    }
    // Methods omit `fnc`. A braced callable inside interface/trait is a default
    // implementation and therefore still uses executable `->` syntax.
    if signature_context
        || structural.contains("self")
        || structural.contains(") ->")
        || structural.contains("): ")
        || structural.contains(") =>")
    {
        return true;
    }
    // Class/actor method shape, including `[Symbol.iterator]()`.
    let prefix = lower.split('(').next().unwrap_or("").trim();
    !prefix.contains('=') && !prefix.contains('.') && !prefix.ends_with("new")
}

fn is_callable_signature(structural: &str, signature_context: bool) -> bool {
    signature_context
        && structural.trim_end().ends_with(';')
        && structural.contains('(')
        && !structural.contains('{')
}

fn contains_word(line: &str, word: &str) -> bool {
    line.split(|c: char| !is_ident_char(c))
        .any(|part| part == word)
}

fn canonicalize_callable_arrow(line: &str, desired: &str, initial: LexState) -> String {
    let Some(close_paren) = callable_parameter_close(line, line.len(), initial) else {
        return line.to_string();
    };
    // Braces within parameter types are not the callable's body delimiter.
    let after_parameters = close_paren + 1;
    let body_at = first_visible_char_index(&line[after_parameters..], '{', LexState::default())
        .map(|index| after_parameters + index)
        .or_else(|| line.rfind(';'))
        .unwrap_or(line.len());
    let tail = &line[close_paren + 1..body_at];
    let trimmed = tail.trim_start();
    let leading_ws = tail.len() - trimmed.len();
    let sep_len = if trimmed.starts_with("=>") || trimmed.starts_with("->") {
        2
    } else if trimmed.starts_with(':') {
        1
    } else {
        return line.to_string();
    };

    let absolute = close_paren + 1 + leading_ws;
    let before = line[..absolute].trim_end();
    let after = line[absolute + sep_len..].trim_start();
    format!("{before} {desired} {after}")
}

fn canonicalize_callable_parameter_order(line: &str, initial: LexState) -> String {
    let Some((open, close)) = callable_parameter_bounds(line, line.len(), initial) else {
        return line.to_string();
    };

    let parameters = &line[open + 1..close];
    if parameters.trim().is_empty() || parameters.contains("//") || parameters.contains("/*") {
        return line.to_string();
    }

    let Some(parts) = split_top_level_parameters(parameters) else {
        return line.to_string();
    };

    let mut changed = false;
    let mut canonical = Vec::with_capacity(parts.len());
    for part in parts {
        let trimmed = part.trim();
        if let Some(rewritten) = canonicalize_name_first_parameter(trimmed) {
            changed = true;
            canonical.push(rewritten);
        } else {
            canonical.push(trimmed.to_string());
        }
    }

    if !changed {
        return line.to_string();
    }

    let mut result = String::with_capacity(line.len());
    result.push_str(&line[..open + 1]);
    result.push_str(&canonical.join(", "));
    result.push_str(&line[close..]);
    result
}

fn split_top_level_parameters(parameters: &str) -> Option<Vec<&str>> {
    let mut parts = Vec::new();
    let mut start = 0usize;
    let mut paren = 0usize;
    let mut bracket = 0usize;
    let mut brace = 0usize;
    let mut angle = 0usize;
    let mut quote: Option<char> = None;
    let mut escape = false;

    for (index, ch) in parameters.char_indices() {
        if let Some(q) = quote {
            if escape {
                escape = false;
            } else if ch == '\\' {
                escape = true;
            } else if ch == q {
                quote = None;
            }
            continue;
        }
        if matches!(ch, '"' | '\'' | '`') {
            quote = Some(ch);
            continue;
        }

        match ch {
            '(' => paren += 1,
            ')' => paren = paren.checked_sub(1)?,
            '[' => bracket += 1,
            ']' => bracket = bracket.checked_sub(1)?,
            '{' => brace += 1,
            '}' => brace = brace.checked_sub(1)?,
            '<' => angle += 1,
            '>' => angle = angle.checked_sub(1)?,
            ',' if paren == 0 && bracket == 0 && brace == 0 && angle == 0 => {
                parts.push(&parameters[start..index]);
                start = index + ch.len_utf8();
            }
            _ => {}
        }
    }

    if quote.is_some() || paren != 0 || bracket != 0 || brace != 0 || angle != 0 {
        return None;
    }

    parts.push(&parameters[start..]);
    Some(parts)
}

fn canonicalize_name_first_parameter(parameter: &str) -> Option<String> {
    let mut paren = 0usize;
    let mut bracket = 0usize;
    let mut brace = 0usize;
    let mut angle = 0usize;
    let mut quote: Option<char> = None;
    let mut escape = false;
    let mut colon = None;

    for (index, ch) in parameter.char_indices() {
        if let Some(q) = quote {
            if escape {
                escape = false;
            } else if ch == '\\' {
                escape = true;
            } else if ch == q {
                quote = None;
            }
            continue;
        }
        if matches!(ch, '"' | '\'' | '`') {
            quote = Some(ch);
            continue;
        }

        match ch {
            '(' => paren += 1,
            ')' => paren = paren.checked_sub(1)?,
            '[' => bracket += 1,
            ']' => bracket = bracket.checked_sub(1)?,
            '{' => brace += 1,
            '}' => brace = brace.checked_sub(1)?,
            '<' => angle += 1,
            '>' => angle = angle.checked_sub(1)?,
            ':' if paren == 0 && bracket == 0 && brace == 0 && angle == 0 && colon.is_some() => {
                return None;
            }
            ':' if paren == 0 && bracket == 0 && brace == 0 && angle == 0 => {
                colon = Some(index);
            }
            _ => {}
        }
    }

    let colon = colon?;
    let name = parameter[..colon].trim();
    let ty = parameter[colon + 1..].trim();

    let mut chars = name.chars();
    let first = chars.next()?;
    if !(first.is_ascii_alphabetic() || first == '_')
        || !chars.all(is_ident_char)
        || ty.is_empty()
        || ty.contains('=')
    {
        return None;
    }

    Some(format!("{ty} {name}"))
}

fn callable_parameter_bounds(
    line: &str,
    limit: usize,
    initial: LexState,
) -> Option<(usize, usize)> {
    let chars: Vec<(usize, char)> = line[..limit].char_indices().collect();
    let mut state = initial;
    let mut depth = 0usize;
    let mut open = None;
    let mut i = 0usize;

    while i < chars.len() {
        let (index, ch) = chars[i];
        let next = chars.get(i + 1).map(|(_, c)| *c);

        if state.block_comment {
            if ch == '*' && next == Some('/') {
                state.block_comment = false;
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }

        if let Some(q) = state.quote {
            if state.escape {
                state.escape = false;
            } else if ch == '\\' {
                state.escape = true;
            } else if ch == q {
                state.quote = None;
            }
            i += 1;
            continue;
        }

        if ch == '/' && next == Some('/') {
            break;
        }
        if ch == '/' && next == Some('*') {
            state.block_comment = true;
            i += 2;
            continue;
        }
        if matches!(ch, '"' | '\'' | '`') {
            state.quote = Some(ch);
            i += 1;
            continue;
        }

        match ch {
            '(' => {
                if depth == 0 {
                    open = Some(index);
                }
                depth += 1;
            }
            ')' if depth > 0 => {
                depth -= 1;
                if depth == 0 {
                    return Some((open?, index));
                }
            }
            _ => {}
        }
        i += 1;
    }

    None
}

fn callable_parameter_close(line: &str, limit: usize, initial: LexState) -> Option<usize> {
    callable_parameter_bounds(line, limit, initial).map(|(_, close)| close)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct WordSpan {
    start: usize,
    end: usize,
}

fn canonicalize_conditional_line(line: &str, initial: LexState) -> String {
    let words = visible_word_spans(line, initial);
    if words.is_empty() {
        return line.to_string();
    }

    let word = |span: WordSpan| &line[span.start..span.end];
    let first = word(words[0]);

    let mut replacements: Vec<(usize, usize, String)> = Vec::new();
    let conditional = if first == "if" {
        true
    } else if first == "elseif" {
        replacements.push((words[0].start, words[0].end, "elif".to_string()));
        true
    } else if first == "elif" {
        true
    } else if first == "else" && words.get(1).is_some_and(|span| word(*span) == "if") {
        replacements.push((words[0].start, words[1].end, "elif".to_string()));
        true
    } else {
        false
    };

    if !conditional {
        return line.to_string();
    }

    // Braced conditionals do not use then/do. We still canonicalize branch
    // aliases above so all source converges on "elif".
    if first_visible_char_index(line, '{', initial).is_none() {
        if let Some(last) = words.last().copied() {
            let last_word = word(last);
            if last_word == "do" || last_word == "then" {
                let prefix = &line[..last.start];
                let before = prefix.trim_end();
                // The branch introducer is syntactic. A semicolon immediately
                // before then/do is legacy punctuation, not a statement end.
                // Preserve an explicit conditional separator while
                // normalizing all surrounding whitespace and the then marker.
                let has_separator = before.ends_with(';');
                let replacement_start = if has_separator {
                    before.trim_end_matches(';').trim_end().len()
                } else {
                    before.len()
                };
                let replacement = if has_separator { "; then" } else { " then" }.to_string();
                replacements.push((replacement_start, last.end, replacement));
            }
        }
    }

    if replacements.is_empty() {
        return line.to_string();
    }

    replacements.sort_by_key(|(start, _, _)| *start);
    for pair in replacements.windows(2) {
        debug_assert!(
            pair[0].1 <= pair[1].0,
            "conditional rewrites must not overlap"
        );
    }

    let mut result = line.to_string();
    for (start, end, replacement) in replacements.into_iter().rev() {
        result.replace_range(start..end, &replacement);
    }
    result
}

fn canonicalize_forever_loop_line(line: &str, initial: LexState) -> String {
    let words = visible_word_spans(line, initial);
    let Some(first) = words.first().copied() else {
        return line.to_string();
    };
    if &line[first.start..first.end] != "for" {
        return line.to_string();
    }

    // Decide from visible syntax, not raw bytes, so comments cannot trick the
    // formatter into rewriting a conventional or iterator for-loop.
    let mut state = initial;
    let visible: String = scan_visible(line, &mut state).into_iter().collect();
    let trimmed = visible.trim_start();
    if !starts_word(trimmed, "for") {
        return line.to_string();
    }
    let rest = trimmed["for".len()..].trim_start();
    let bare_braced = rest.starts_with('{');
    let bare_keyword = starts_word(rest, "do") && rest["do".len()..].trim().is_empty();
    if !bare_braced && !bare_keyword {
        return line.to_string();
    }

    let mut result = line.to_string();
    result.replace_range(first.start..first.end, "loop");
    result
}

fn canonicalize_generator_sugar_line(line: &str, initial: LexState) -> String {
    let words = visible_word_spans(line, initial);
    if words.len() < 2 {
        return line.to_string();
    }

    let is_modifier_word = |word: &str| {
        matches!(
            word,
            "pub"
                | "private"
                | "static"
                | "async"
                | "generator"
                | "gen"
                | "nlex"
                | "pure"
                | "trap"
                | "structural"
                | "abstract"
                | "quantum"
                | "fnc"
                | "routine"
        )
    };
    let is_callable_kind = |word: &str| matches!(word, "fnc" | "routine");

    for (index, span) in words.iter().enumerate() {
        let word = &line[span.start..span.end];
        if !matches!(word, "gen" | "generator") {
            continue;
        }
        if !words[..=index]
            .iter()
            .all(|part| is_modifier_word(&line[part.start..part.end]))
        {
            continue;
        }

        let kind_before = words[..index]
            .iter()
            .any(|part| is_callable_kind(&line[part.start..part.end]));
        let mut kind_after = false;
        for part in words.iter().skip(index + 1) {
            let next_word = &line[part.start..part.end];
            if !is_modifier_word(next_word) {
                break;
            }
            if is_callable_kind(next_word) {
                kind_after = true;
                break;
            }
        }
        if !kind_before && !kind_after {
            continue;
        }

        let next_start = words.get(index + 1).map_or(line.len(), |part| part.start);
        let gap = &line[span.end..next_start];
        let Some(star_offset) = gap.find('*') else {
            continue;
        };
        if gap[..star_offset].chars().any(|c| !c.is_whitespace())
            || gap[star_offset + 1..].chars().any(|c| !c.is_whitespace())
        {
            continue;
        }

        let mut result = line.to_string();
        result.replace_range(span.start..span.end + star_offset + 1, "generator");
        return result;
    }

    line.to_string()
}

fn starts_pipe_lambda_expression(line: &str) -> bool {
    let trimmed = line.trim_start();
    starts_word(trimmed, "nlex") && trimmed["nlex".len()..].trim_start().starts_with('|')
        || trimmed.starts_with('|')
}

fn opens_multiline_expression_body_pipe_lambda(line: &str) -> bool {
    if ends_statement(line) {
        return false;
    }
    let Some(arrow) = line.rfind("->") else {
        return false;
    };
    let before = &line[..arrow];
    if before.chars().filter(|c| *c == '|').count() < 2 {
        return false;
    }

    // The compiler's lexical rule is exact: only an immediate expression-level
    // '{' after the arrow selects the statement-block lambda form. Everything
    // else is an expression body and therefore needs the outer statement's
    // explicit semicolon.
    !line[arrow + 2..].trim_start().starts_with('{')
}

fn canonicalize_rt_cooperate_line(line: &str, initial: LexState) -> String {
    let words = visible_word_spans(line, initial);
    if words.len() < 2 {
        return line.to_string();
    }

    let first = &line[words[0].start..words[0].end];
    let second = &line[words[1].start..words[1].end];
    if first != "rt" || second != "yield" {
        return line.to_string();
    }

    // Rewrite only the runtime namespace operation token. Generator
    // `yield value` remains untouched, as do strings/comments containing
    // the compatibility spelling.
    let mut result = line.to_string();
    result.replace_range(words[1].start..words[1].end, "cooperate");
    result
}

fn visible_word_spans(line: &str, initial: LexState) -> Vec<WordSpan> {
    let chars: Vec<(usize, char)> = line.char_indices().collect();
    let mut words = Vec::new();
    let mut state = initial;
    let mut i = 0usize;

    while i < chars.len() {
        let (start, c) = chars[i];
        let next = chars.get(i + 1).map(|(_, c)| *c);

        if state.block_comment {
            if c == '*' && next == Some('/') {
                state.block_comment = false;
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }

        if let Some(q) = state.quote {
            if state.escape {
                state.escape = false;
            } else if c == '\\' {
                state.escape = true;
            } else if c == q {
                state.quote = None;
            }
            i += 1;
            continue;
        }

        if c == '/' && next == Some('/') {
            break;
        }
        if c == '/' && next == Some('*') {
            state.block_comment = true;
            i += 2;
            continue;
        }
        if matches!(c, '"' | '\'' | '`') {
            state.quote = Some(c);
            i += 1;
            continue;
        }

        if is_ident_char(c) {
            let mut end = start + c.len_utf8();
            i += 1;
            while i < chars.len() && is_ident_char(chars[i].1) {
                end = chars[i].0 + chars[i].1.len_utf8();
                i += 1;
            }
            words.push(WordSpan { start, end });
            continue;
        }

        i += 1;
    }

    words
}

fn opens_conditional_block(line: &str) -> bool {
    let t = line.trim_end();
    starts_word(t.trim_start(), "if")
        && (t.ends_with(" then")
            || t.ends_with("; then")
            || t.ends_with(" do")
            || t.ends_with("; do"))
}

fn opens_do_block(line: &str) -> bool {
    let t = line.trim_end();
    (t.ends_with(" do") || t.ends_with("; do")) && !starts_word(t.trim_start(), "done")
}

/// Canonicalize only single-quoted # Symbols; normal strings/comments remain
/// byte-for-byte unchanged. The Oreslang lexer accepts both quote styles,
/// but the formatter renders the Symbol's decoded name with double quotes.
///
/// This is deliberately a source-aware pass, not a global quote replacement:
/// it tracks nested block comments, line comments, strings and backtick literals.
fn canonicalize_hash_symbol_quotes(source: &str) -> String {
    let bytes = source.as_bytes();
    let mut result = String::with_capacity(source.len());
    let mut index = 0usize;
    let mut block_depth = 0usize;
    let mut quote: Option<u8> = None;
    while index < bytes.len() {
        let byte = bytes[index];
        let next = bytes.get(index + 1).copied();
        if block_depth > 0 {
            if byte == b'/' && next == Some(b'*') {
                block_depth += 1;
                result.push_str("/*");
                index += 2;
                continue;
            }
            if byte == b'*' && next == Some(b'/') {
                block_depth -= 1;
                result.push_str("*/");
                index += 2;
                continue;
            }
        } else if let Some(delimiter) = quote {
            if byte == b'\\' && next.is_some() {
                // A quoted escape may be followed by a multibyte Unicode
                // scalar. Never slice UTF-8 at an arbitrary byte index.
                let escaped = source[index + 1..].chars().next().expect("valid UTF-8");
                result.push('\\');
                result.push(escaped);
                index += 1 + escaped.len_utf8();
                continue;
            }
            if byte == delimiter {
                quote = None;
            }
        } else {
            if byte == b'/' && next == Some(b'/') {
                if let Some(end) = source[index..].find('\n') {
                    result.push_str(&source[index..index + end + 1]);
                    index += end + 1;
                } else {
                    result.push_str(&source[index..]);
                    break;
                }
                continue;
            }
            if byte == b'/' && next == Some(b'*') {
                block_depth = 1;
                result.push_str("/*");
                index += 2;
                continue;
            }
            if byte == b'#' && next == Some(b'\'') {
                if let Some((end, canonical)) = canonicalize_single_quoted_symbol(source, index) {
                    result.push_str(&canonical);
                    index = end;
                    continue;
                }
            }
            if matches!(byte, b'\'' | b'"' | b'`') {
                quote = Some(byte);
            }
        }
        let ch = source[index..].chars().next().expect("valid UTF-8");
        result.push(ch);
        index += ch.len_utf8();
    }
    result
}

/// Return the byte index after the closing quote and its equivalent
/// double-quoted hash Symbol. Malformed escapes stay unchanged for the
/// compiler's normal diagnostics, rather than being silently repaired.
fn canonicalize_single_quoted_symbol(source: &str, start: usize) -> Option<(usize, String)> {
    let mut result = String::from("#\"");
    let mut chars = source[start + 2..].char_indices();
    while let Some((offset, ch)) = chars.next() {
        match ch {
            '\'' => {
                result.push('"');
                return Some((start + 2 + offset + 1, result));
            }
            '\n' | '\r' => return None,
            '"' => result.push_str("\\\""),
            '\\' => {
                let (_, escaped) = chars.next()?;
                match escaped {
                    '\'' => result.push('\''),
                    '"' => result.push_str("\\\""),
                    '\\' => result.push_str("\\\\"),
                    'n' | 'r' | 't' => {
                        result.push('\\');
                        result.push(escaped);
                    }
                    'u' => {
                        result.push_str("\\u");
                        for _ in 0..4 {
                            let (_, hex) = chars.next()?;
                            if !hex.is_ascii_hexdigit() {
                                return None;
                            }
                            result.push(hex);
                        }
                    }
                    _ => return None,
                }
            }
            _ => result.push(ch),
        }
    }
    None
}

fn reject_multiline_literals(source: &str) -> Result<(), FormatError> {
    let lines: Vec<&str> = source.split('\n').collect();
    let mut state = LexState::default();

    for (index, line) in lines.iter().enumerate() {
        let logical = line.strip_suffix('\r').unwrap_or(line);
        let mut next = state;
        scan_visible(logical, &mut next);

        if index + 1 < lines.len() && next.quote.is_some() {
            return Err(FormatError::new(
                index + 1,
                "multiline string/template literals are not formatted yet; refusing to change literal bytes",
            ));
        }

        state = next;
    }

    Ok(())
}

fn first_visible_char_index(line: &str, needle: char, initial: LexState) -> Option<usize> {
    let chars: Vec<(usize, char)> = line.char_indices().collect();
    let mut state = initial;
    let mut i = 0usize;

    while i < chars.len() {
        let (byte_index, c) = chars[i];
        let next = chars.get(i + 1).map(|(_, c)| *c);

        if state.block_comment {
            if c == '*' && next == Some('/') {
                state.block_comment = false;
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }

        if let Some(q) = state.quote {
            if state.escape {
                state.escape = false;
            } else if c == '\\' {
                state.escape = true;
            } else if c == q {
                state.quote = None;
            }
            i += 1;
            continue;
        }

        if c == '/' && next == Some('/') {
            return None;
        }
        if c == '/' && next == Some('*') {
            state.block_comment = true;
            i += 2;
            continue;
        }
        if matches!(c, '"' | '\'' | '`') {
            state.quote = Some(c);
            i += 1;
            continue;
        }
        if c == needle {
            return Some(byte_index);
        }

        i += 1;
    }

    None
}

// Detect genuine line comments without matching text in strings or blocks.
fn has_line_comment(line: &str, initial: LexState) -> bool {
    let mut state = initial;
    let mut chars = line.chars().peekable();
    while let Some(ch) = chars.next() {
        let next = chars.peek().copied();
        if state.block_comment {
            if ch == '*' && next == Some('/') {
                state.block_comment = false;
                chars.next();
            }
            continue;
        }
        if let Some(quote) = state.quote {
            if state.escape {
                state.escape = false;
            } else if ch == '\\' {
                state.escape = true;
            } else if ch == quote {
                state.quote = None;
            }
            continue;
        }
        if ch == '/' && next == Some('/') {
            return true;
        }
        if ch == '/' && next == Some('*') {
            state.block_comment = true;
            chars.next();
            continue;
        }
        if ch == '"' || ch == '\'' || ch == '\u{60}' {
            state.quote = Some(ch);
        }
    }
    false
}

fn scan_visible(line: &str, state: &mut LexState) -> Vec<char> {
    let chars: Vec<char> = line.chars().collect();
    let mut visible = Vec::new();
    let mut i = 0usize;

    while i < chars.len() {
        let c = chars[i];
        let next = chars.get(i + 1).copied();

        if state.block_comment {
            if c == '*' && next == Some('/') {
                state.block_comment = false;
                visible.push(' ');
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }

        if let Some(q) = state.quote {
            if state.escape {
                state.escape = false;
            } else if c == '\\' {
                state.escape = true;
            } else if c == q {
                state.quote = None;
            }
            i += 1;
            continue;
        }

        if c == '/' && next == Some('/') {
            break;
        }
        if c == '/' && next == Some('*') {
            visible.push(' ');
            state.block_comment = true;
            i += 2;
            continue;
        }
        if matches!(c, '"' | '\'' | '`') {
            visible.push(' ');
            state.quote = Some(c);
            i += 1;
            continue;
        }

        visible.push(c);
        i += 1;
    }

    visible
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonicalizes_rt_yield_without_touching_generator_yield_or_literals() {
        let src = r#"fnc generator* values(): Iterator<int> {
  rt yield;
  rt yield();
  yield 1;
  yield* values();
  stdio.stdout.write("rt yield");
  // rt yield
  return;
}
"#;

        let got = format_source(src).unwrap();
        assert!(got.contains("rt cooperate;"));
        assert!(got.contains("rt cooperate();"));
        assert!(got.contains("yield 1;"));
        assert!(got.contains("yield* values();"));
        assert!(got.contains("\"rt yield\""));
        assert!(got.contains("// rt yield"));
        assert!(!got.contains("  rt yield;"));
        assert!(!got.contains("  rt yield();"));
    }

    #[test]
    fn canonicalizes_declaration_modifier_order() {
        let src = r#"pub define class X as {
}

class pub define Y as
end

define class pub abstract Z as {
}

fnc pub async foo() {
}

pure nlex async pub fnc bar() {
}

fnc nlex pure async pub baz() {
}

fnc structural static pub helper() {
}

static fnc quantum pub qsolve() {
}

fnc structural signature() => String;

actor pub fnc untrusted async worker() {
}

fnc pub actor untrusted async worker_after_fnc() {
}

routine shared pub actor async worker_routine_after_kind() {
}

actor shared pub Account {
}

interface pub define Api {
}

define contract pub Contract {
}
"#;

        let got = format_source(src).unwrap();
        assert!(got.contains("define pub class X as {"));
        assert!(got.contains("define pub class Y as"));
        assert!(got.contains("define pub abstract class Z as {"));
        assert!(got.contains("pub async fnc foo() {"));
        assert!(got.contains("pub async nlex pure fnc bar() {"));
        assert!(got.contains("pub async nlex pure fnc baz() {"));
        assert!(got.contains("pub static structural fnc helper() {"));
        assert!(got.contains("pub quantum static fnc qsolve() {"));
        assert!(got.contains("structural fnc signature() => String;"));
        assert!(got.contains("pub async untrusted actor fnc worker() {"));
        assert!(got.contains("pub async untrusted actor fnc worker_after_fnc() {"));
        assert!(got.contains("pub async shared actor routine worker_routine_after_kind() {"));
        assert!(got.contains("pub shared actor Account {"));
        assert!(got.contains("define pub interface Api {"));
        assert!(got.contains("define pub contract Contract {"));
        assert!(!got.contains("interface pub define"));
        assert!(!got.contains("define contract pub"));
        assert!(!got.contains("actor pub fnc"));
        assert!(!got.contains("actor shared pub"));
        assert!(!got.contains("pub define class"));
        assert!(!got.contains("class pub define"));
        assert!(!got.contains("define class pub"));
        assert!(!got.contains("fnc pub async"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn leaves_ambiguous_or_commented_modifier_prefixes_untouched() {
        let src = r#"pub /* keep */ define class X as {
}

pub private fnc conflict() {
}

shared untrusted actor Conflict {
}

define pub private interface Broken {
}
"#;

        let got = format_source(src).unwrap();
        assert!(got.contains("pub /* keep */ define class X as {"));
        assert!(got.contains("pub private fnc conflict() {"));
        assert!(got.contains("shared untrusted actor Conflict {"));
        assert!(got.contains("define pub private interface Broken {"));
    }

    #[test]
    fn contracts_are_signature_contexts_in_end_and_brace_forms() {
        let src = r#"define contract EndContract as
  fnc end_sig() -> String;
end

define contract BraceContract {
  fnc brace_sig(): String;
}
"#;

        let got = format_source(src).unwrap();
        assert!(got.contains("fnc end_sig() -> String;"));
        assert!(got.contains("fnc brace_sig() -> String;"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    fn permutations(tokens: &[&str]) -> Vec<Vec<String>> {
        fn visit(rest: Vec<String>, prefix: Vec<String>, out: &mut Vec<Vec<String>>) {
            if rest.is_empty() {
                out.push(prefix);
                return;
            }
            for index in 0..rest.len() {
                let mut next_rest = rest.clone();
                let token = next_rest.remove(index);
                let mut next_prefix = prefix.clone();
                next_prefix.push(token);
                visit(next_rest, next_prefix, out);
            }
        }

        let mut out = Vec::new();
        visit(
            tokens.iter().map(|token| (*token).to_string()).collect(),
            Vec::new(),
            &mut out,
        );
        out
    }

    #[test]
    fn canonicalizes_all_supported_modifier_permutations() {
        for order in permutations(&["define", "class", "pub", "abstract"]) {
            let source = format!("{} Box as {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(got, "define pub abstract class Box as {\n}\n");
        }

        for order in permutations(&["pub", "async", "nlex", "fnc"]) {
            let source = format!("{} work() {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(got, "pub async nlex fnc work() {\n}\n");
        }

        for order in permutations(&["pub", "quantum", "fnc"]) {
            let source = format!("{} qwork() {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(got, "pub quantum fnc qwork() {\n}\n");
        }

        for order in permutations(&["pub", "quantum", "static", "fnc"]) {
            let source = format!("{} qstatic() {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(got, "pub quantum static fnc qstatic() {\n}\n");
        }

        let invalid_quantum_actor = "quantum actor fnc bad() {\n}\n";
        assert_eq!(
            format_source(invalid_quantum_actor).unwrap(),
            invalid_quantum_actor,
            "formatter must leave unsupported quantum actor syntax for compiler diagnostics"
        );

        let mut actor_cases = 0;
        for order in permutations(&["pub", "async", "untrusted", "actor", "fnc"]) {
            actor_cases += 1;
            let source = format!("{} worker() {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(got, "pub async untrusted actor fnc worker() {\n}\n");
        }
        assert_eq!(actor_cases, 120);

        let mut actor_routine_cases = 0;
        for order in permutations(&["pub", "async", "shared", "actor", "routine"]) {
            actor_routine_cases += 1;
            let source = format!("{} worker_routine() {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(
                got,
                "pub async shared actor routine worker_routine() {\n}\n"
            );
        }
        assert_eq!(actor_routine_cases, 120);

        let mut isoactor_cases = 0;
        for order in permutations(&["pub", "async", "isoactor", "fnc"]) {
            isoactor_cases += 1;
            let source = format!("{} private_worker() {{\n}}\n", order.join(" "));
            let got = format_source(&source).unwrap();
            assert_eq!(got, "pub async isoactor fnc private_worker() {\n}\n");
        }
        assert_eq!(isoactor_cases, 24);
    }

    #[test]
    fn canonicalizes_static_contract_declaration_order_without_normalizing_static_interface() {
        let src = r#"static contract define pub Factory as
  fnc build(int id): int;
end
"#;
        let got = format_source(src).unwrap();
        assert!(got.starts_with("define pub static contract Factory as\n"));
        assert!(got.contains("fnc build(int id) -> int;"));
        assert_eq!(format_source(&got).unwrap(), got);

        // The formatter must fail closed on a nonsensical static
        // instance-interface declaration rather than preserving a malformed
        // end-delimited frame or rewriting it into canonical-looking source.
        let invalid = "static interface define Bad as\nend\n";
        assert!(format_source(invalid).is_err());
    }

    #[test]
    fn formats_end_delimited_free_and_module_callables() {
        let src = r#"fnc classify(int x) -> int
if x > 0 then
return 1
else
return 0
fi
end

define module math as
fnc identity(int x) -> int
return x
end
end
"#;

        let got = format_source(src).unwrap();
        let expected = r#"fnc classify(int x) -> int
  if x > 0 then
    return 1;
  else
    return 0;
  fi
end

define module math as
  fnc identity(int x) -> int
    return x;
  end
end
"#;
        assert_eq!(got, expected);
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn retired_callable_do_separator_fails_closed() {
        assert!(format_source("fnc identity(int x) -> int do return x; end\n").is_err());
        assert!(format_source("routine identity(int x) -> int do return x; end\n").is_err());
    }

    #[test]
    fn expands_optional_as_compact_end_delimited_callables() {
        let compact = "fnc identity(int x) -> int as return x; end\n";
        let expected = "fnc identity(int x) -> int as\n  return x;\nend\n";
        let got = format_source(compact).unwrap();
        assert_eq!(got, expected);
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn expands_optional_as_compact_keyword_control_flow() {
        let compact =
            "fnc classify(int x) -> int as if x > 0 then return 1; else return 0; fi end\n";
        let expected = r#"fnc classify(int x) -> int as
  if x > 0 then
    return 1;
  else
    return 0;
  fi
end
"#;
        let got = format_source(compact).unwrap();
        assert_eq!(got, expected);
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn optional_as_after_higher_order_return_type_is_preserved() {
        let compact = "fnc callback() -> () => void as return factory(); end\n";
        let got = format_source(compact).unwrap();
        assert!(got.starts_with("fnc callback() -> () => void as\n"));
        assert!(got.contains("  return factory();\n"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn compact_callable_expands_before_nested_match_end_ownership() {
        let src = "fnc classify(int x) -> int as match x 1 -> { return 1; } else -> { return 0; } end end\n";
        let got = format_source(src).unwrap();
        assert!(got.starts_with("fnc classify(int x) -> int as\n"));
        assert!(got.contains("  match x"));
        assert!(got.contains("return 1;"));
        assert!(got.contains("return 0;"));
        assert!(got.ends_with("end\n"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn compact_separator_can_precede_real_do_select_body() {
        let src = r#"fnc consume(Channel<int> incoming) -> void as do select { case readch incoming: val number { return; } } end
"#;
        let got = format_source(src).unwrap();
        assert!(got.starts_with("fnc consume(Channel<int> incoming) -> void as\n"));
        assert!(got.contains("  do select {\n"));
        assert!(got.contains("    when readch incoming: val number {\n"));
        assert!(got.ends_with("end\n"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn next_line_do_select_remains_a_body_statement() {
        let src = r#"fnc consume(Channel<int> incoming) -> void
do select {
case readch incoming: val number {
return;
}
}
end
"#;
        let expected = r#"fnc consume(Channel<int> incoming) -> void
  do select {
    when readch incoming: val number {
      return;
    }
  }
end
"#;
        let got = format_source(src).unwrap();
        assert_eq!(got, expected);
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn multiline_callable_as_never_opens_a_done_frame() {
        let src = "fnc identity(int x) -> int as\nreturn x\nend\n";
        let expected = "fnc identity(int x) -> int as\n  return x;\nend\n";
        let got = format_source(src).unwrap();
        assert_eq!(got, expected);
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn compact_callable_as_preserves_trailing_end_comment() {
        let src = "fnc identity(int x) -> int as return x; end // callable\n";
        let got = format_source(src).unwrap();
        assert!(got.contains("end // callable\n"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn compact_callable_rejects_visible_syntax_after_block_comment_suffix() {
        let src = "fnc identity(int x) -> int as return x; end /* comment */ stray\n";
        assert!(format_source(src).is_err());
    }

    #[test]
    fn compact_callable_as_ignores_delimiters_hidden_in_comments_and_literals() {
        let src =
            "fnc value() -> int /* ; as do end */ as /* ; end */ return 7; end // as do end\n";
        let got = format_source(src).unwrap();
        assert!(got.starts_with("fnc value() -> int /* ; as do end */ as\n"));
        assert!(got.contains("  /* ; end */ return 7;\n"));
        assert!(got.ends_with("end // as do end\n"));
        assert_eq!(format_source(&got).unwrap(), got);

        let string_body = "fnc text() -> string as return \"do end fi;\"; end\n";
        let string_got = format_source(string_body).unwrap();
        assert!(string_got.contains("  return \"do end fi;\";\n"));
        assert_eq!(format_source(&string_got).unwrap(), string_got);
    }

    #[test]
    fn compact_callable_as_preserves_nested_lambda_boundaries() {
        let src =
            "fnc make() -> () => string as return || -> { return \"do end\"; }; end\n";
        let got = format_source(src).unwrap();
        assert!(got.starts_with("fnc make() -> () => string as\n"));
        assert!(got.contains("return || -> {"));
        assert!(got.contains("\"do end\""));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn body_level_cast_as_cannot_impersonate_callable_separator() {
        let src = "fnc casty() -> int return value as int; end\n";
        assert!(format_source(src).is_err());
    }

    #[test]
    fn visible_semicolon_before_callable_as_is_not_reinterpreted_as_a_body() {
        let src = "fnc bad() -> int; as return 1; end\n";
        assert!(format_source(src).is_err());
    }

    #[test]
    fn shared_java_round_trip_corpus_formats_idempotently() {
        let source = include_str!("../tests/fixtures/end-delimited-roundtrip.ores");
        let once = format_source(source).unwrap();
        let twice = format_source(&once).unwrap();
        assert_eq!(twice, once);
        assert!(once.contains("fnc identity(int x) -> int as\n"));
        assert!(once.contains("fnc callback() -> () => void as\n"));
        assert!(once.contains("fnc text() -> string /* ; as do end */ as\n"));
        assert!(once.contains("fnc double(int x) -> int as\n"));
    }

    #[test]
    fn routine_uses_the_same_end_delimited_as_formatting() {
        let compact = "routine identity(int x) -> int as return x; end\n";
        let got = format_source(compact).unwrap();
        assert_eq!(got, "routine identity(int x) -> int as\n  return x;\nend\n");
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn contract_arrows_are_executable_style_but_interfaces_stay_type_level() {
        let src = r#"define contract ModuleApi as
  fnc load(int id) => tuple (int, string);
end

define static contract StaticApi as
  fnc build(int id): Result<Option<tuple (int, string)>, string>;
end

define interface InstanceApi as
  fnc run(int id) -> int;
end

define trait Retryable as
  fnc retry(): bool;
end
"#;

        let got = format_source(src).unwrap();
        assert!(got.contains("fnc load(int id) -> tuple (int, string);"));
        assert!(got.contains(
            "fnc build(int id) -> Result<Option<tuple (int, string)>, string>;"
        ));
        assert!(got.contains("fnc run(int id) => int;"));
        assert!(got.contains("fnc retry() => bool;"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn end_delimited_actor_or_class_methods_are_not_accepted_as_free_callable_frames() {
        let class_method = r#"define class C as
  run(int x) -> int
    return x
  end
end
"#;
        assert!(format_source(class_method).is_err());

        let actor_callable = r#"actor fnc run(int x) -> int
  return x
end
"#;
        assert!(format_source(actor_callable).is_err());
    }

    #[test]
    fn large_tuple_return_spelling_is_preserved_and_idempotent() {
        let slots = (0..65).map(|_| "int").collect::<Vec<_>>().join(", ");
        let src = format!(
            "fnc identity(tuple ({slots}) value) -> tuple ({slots})\nreturn value\nend\n"
        );
        let got = format_source(&src).unwrap();
        assert!(got.contains(&format!("tuple ({slots}) value")));
        assert!(got.contains(&format!("-> tuple ({slots})")));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn delimiter_words_inside_literals_and_comments_do_not_close_callable() {
        let src = r#"fnc words() -> string
const string value = "end fi"
return value // end fi
end
"#;
        let got = format_source(src).unwrap();
        assert!(got.contains("\"end fi\""));
        assert!(got.contains("// end fi"));
        assert_eq!(format_source(&got).unwrap(), got);
    }

    #[test]
    fn static_or_abstract_end_delimited_free_callable_fails_closed() {
        let top_static = r#"static fnc run() -> int
return 1
end
"#;
        assert!(format_source(top_static).is_err());

        let module_static = r#"define module M as
static fnc run() -> int
return 1
end
end
"#;
        assert!(format_source(module_static).is_err());

        let top_abstract = r#"abstract fnc run() -> int
return 1
end
"#;
        assert!(format_source(top_abstract).is_err());
    }

    #[test]
    fn canonicalizes_executable_and_signature_arrows() {
        let src = r#"define interface Named
  fnc name(): String;
end

define trait Retryable
  fnc retry() -> bool;
end

define class User implements Named, Retryable as
  pub name() => String {
    return "u";
  }
end
"#;
        let got = format_source(src).unwrap();
        assert!(got.contains("fnc name() => String;"));
        assert!(got.contains("fnc retry() => bool;"));
        assert!(got.contains("pub name() -> String {"));
    }
}
