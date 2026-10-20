//! Expand single-line keyword conditionals before the indentation pass.
//! Comments, strings, and template literals remain opaque.

use crate::{LexState, is_ident_char};

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Header,
    Body,
}

#[derive(Clone, Copy)]
struct Token<'a> {
    text: &'a str,
    start: usize,
    end: usize,
}

struct Lexed<'a> {
    tokens: Vec<Token<'a>>,
    line_comment: Option<usize>,
    block_comment_seen: bool,
}

fn lex_line<'a>(line: &'a str, state: &mut LexState) -> Lexed<'a> {
    let mut tokens = Vec::new();
    let mut chars = line.char_indices().peekable();
    let mut line_comment = None;
    let mut block_comment_seen = state.block_comment;

    while let Some((start, ch)) = chars.next() {
        let next = chars.peek().map(|&(_, ch)| ch);
        if state.block_comment {
            if ch == '*' && next == Some('/') {
                chars.next();
                state.block_comment = false;
            }
            continue;
        }
        if ch == '/' && next == Some('/') {
            line_comment = Some(start);
            break;
        }
        if ch == '/' && next == Some('*') {
            chars.next();
            state.block_comment = true;
            block_comment_seen = true;
            continue;
        }
        if matches!(ch, '"' | '\'' | '\x60') {
            let mut escape = false;
            let mut end = start + ch.len_utf8();
            state.quote = Some(ch);
            for (pos, next) in chars.by_ref() {
                end = pos + next.len_utf8();
                if escape {
                    escape = false;
                } else if next == '\\' {
                    escape = true;
                } else if next == ch {
                    state.quote = None;
                    break;
                }
            }
            tokens.push(Token {
                text: &line[start..end],
                start,
                end,
            });
            continue;
        }
        if ch.is_whitespace() {
            continue;
        }
        if is_ident_char(ch) {
            let mut end = start + ch.len_utf8();
            while let Some(&(pos, c)) = chars.peek() {
                if !is_ident_char(c) {
                    break;
                }
                chars.next();
                end = pos + c.len_utf8();
            }
            tokens.push(Token {
                text: &line[start..end],
                start,
                end,
            });
        } else {
            tokens.push(Token {
                text: &line[start..start + ch.len_utf8()],
                start,
                end: start + ch.len_utf8(),
            });
        }
    }

    Lexed {
        tokens,
        line_comment,
        block_comment_seen,
    }
}

fn statement_boundary(tokens: &[Token<'_>], index: usize) -> bool {
    index == 0
        || matches!(
            tokens[index - 1].text,
            ";" | "{" | "}" | "then" | "else" | "fi"
        )
}

fn has_top_level_keyword_header(tokens: &[Token<'_>], start: usize) -> bool {
    let (mut parens, mut brackets) = (0usize, 0usize);

    for (offset, token) in tokens[start..].iter().enumerate() {
        match token.text {
            "(" => parens += 1,
            ")" => {
                if parens == 0 {
                    return false;
                }
                parens -= 1;
            }
            "[" => brackets += 1,
            "]" => {
                if brackets == 0 {
                    return false;
                }
                brackets -= 1;
            }
            "{" if parens == 0 && brackets == 0 => return false,
            "then" | "do" if parens == 0 && brackets == 0 => {
                let absolute = start + offset;
                if absolute == 0 || tokens[absolute - 1].text != "." {
                    return true;
                }
            }
            _ => {}
        }
    }

    false
}

fn inline_return_before(tokens: &[Token<'_>], index: usize) -> bool {
    let mut start = 0usize;
    for previous in (0..index).rev() {
        if matches!(
            tokens[previous].text,
            ";" | "{" | "}" | "then" | "else" | "fi"
        ) {
            start = previous + 1;
            break;
        }
    }
    let statement = &tokens[start..index];
    statement
        .first()
        .is_some_and(|token| token.text == "return")
        && statement.len() >= 2
}

fn return_expression_is_multiline(tokens: &[Token<'_>], next: &str) -> bool {
    let expression_head = tokens.get(1).map(|token| token.text);
    let expression_tail = tokens.last().map(|token| token.text);

    matches!(expression_head, Some("match" | "select" | "switch" | "do"))
        || matches!(
            expression_tail,
            Some(
                "match"
                    | "select"
                    | "switch"
                    | "over"
                    | "from"
                    | "with"
                    | "as"
                    | "is"
                    | "await"
                    | "then"
                    | "do"
            )
        )
        || [".", "?.", "(", "[", "+", "-", "*", "/", "&&", "||", "|>"]
            .iter()
            .any(|prefix| next.starts_with(prefix))
        || ["on", "when", "case", "default", "as", "is", "with", "over"]
            .iter()
            .any(|word| crate::starts_word(next, word))
}

fn split_keyword_blocks(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut lex = LexState::default();
    let mut frames: Vec<Phase> = Vec::new();

    for (line_number, line) in source.split('\n').enumerate() {
        if line_number != 0 {
            output.push('\n');
        }
        let scanned = lex_line(line, &mut lex);
        let tokens = scanned.tokens;
        let mut cuts: Vec<usize> = Vec::new();
        let mut saw_keyword_arm = false;
        let (mut parens, mut brackets, mut braces) = (0usize, 0usize, 0usize);

        for (index, token) in tokens.iter().enumerate() {
            match token.text {
                "(" => parens += 1,
                ")" => parens = parens.saturating_sub(1),
                "[" => brackets += 1,
                "]" => brackets = brackets.saturating_sub(1),
                "{" => braces += 1,
                "}" => braces = braces.saturating_sub(1),
                _ => {}
            }
            if parens != 0 || brackets != 0 || braces != 0 {
                continue;
            }

            match token.text {
                "if" if statement_boundary(&tokens, index) => {
                    // Braced if/elif arms are a separate grammatical form.
                    // Require an actual keyword header terminator to opt in.
                    let has_keyword_header = has_top_level_keyword_header(&tokens, index + 1);
                    if has_keyword_header && (index == 0 || tokens[index - 1].text != "else") {
                        frames.push(Phase::Header);
                        if index > 0 {
                            cuts.push(token.start);
                        }
                    }
                }
                "then" | "do"
                    if frames.last() == Some(&Phase::Header)
                        && index > 0
                        && tokens[index - 1].text != "." =>
                {
                    if let Some(frame) = frames.last_mut() {
                        *frame = Phase::Body;
                    }
                    saw_keyword_arm = true;
                    if index + 1 < tokens.len() {
                        cuts.push(token.end);
                    }
                }
                "elif" | "elseif" if !frames.is_empty() && statement_boundary(&tokens, index) => {
                    saw_keyword_arm = true;
                    cuts.push(token.start);
                    if let Some(frame) = frames.last_mut() {
                        *frame = Phase::Header;
                    }
                }
                "else" if !frames.is_empty() && statement_boundary(&tokens, index) => {
                    saw_keyword_arm = true;
                    cuts.push(token.start);
                    if let Some(frame) = frames.last_mut() {
                        *frame = if tokens.get(index + 1).is_some_and(|t| t.text == "if") {
                            Phase::Header
                        } else {
                            Phase::Body
                        };
                    }
                    if frames.last() == Some(&Phase::Body) && index + 1 < tokens.len() {
                        cuts.push(token.end);
                    }
                }
                "fi" if frames.last() == Some(&Phase::Body)
                    && (statement_boundary(&tokens, index)
                        || inline_return_before(&tokens, index)
                        || tokens
                            .get(index.wrapping_sub(1))
                            .is_some_and(|t| t.text == ")")) =>
                {
                    saw_keyword_arm = true;
                    cuts.push(token.start);
                    frames.pop();
                    if index + 1 < tokens.len() && tokens[index + 1].text != ";" {
                        cuts.push(token.end);
                    }
                }
                ";" if saw_keyword_arm
                    && frames.last() == Some(&Phase::Body)
                    && index + 1 < tokens.len()
                    && !matches!(tokens[index + 1].text, "do" | "then") =>
                {
                    cuts.push(token.end);
                }
                _ => {}
            }
        }

        cuts.sort_unstable();
        cuts.dedup();
        let mut last = 0;
        for cut in cuts {
            if line[cut..].trim().is_empty() {
                continue;
            }
            if line[last..cut].trim().is_empty() {
                // A preceding semicolon already inserted the newline.
                // Discard the whitespace between that terminator and fi.
                last = cut;
                continue;
            }
            output.push_str(line[last..cut].trim_end());
            output.push('\n');
            last = cut;
        }
        output.push_str(&line[last..]);
    }
    output
}

fn terminate_simple_returns(source: &str) -> String {
    let mut output = String::with_capacity(source.len());
    let mut lex = LexState::default();
    let lines: Vec<&str> = source.split('\n').collect();

    for (index, line) in lines.iter().enumerate() {
        if index != 0 {
            output.push('\n');
        }
        let initial = lex;
        let scanned = lex_line(line, &mut lex);
        let tokens = &scanned.tokens;
        let code_end = scanned.line_comment.unwrap_or(line.len());
        let code = line[..code_end].trim_end();

        // Add a terminator only when the return is definitely complete on
        // this line. Bare returns are always complete. Expression returns
        // fail closed around multiline/block syntax so formatting cannot
        // silently terminate a future match/select/continuation early.
        let is_return = !initial.block_comment
            && !scanned.block_comment_seen
            && tokens.first().is_some_and(|token| token.text == "return")
            && !code.ends_with(';');
        let bare_return = is_return && tokens.len() == 1;
        let simple_expression_return = is_return
            && tokens.len() >= 2
            && !code.ends_with('{')
            && !code.ends_with('}')
            && !code.ends_with(',')
            && !code.ends_with('.')
            && !code.ends_with(':')
            && !code.ends_with('?')
            && !code.ends_with('+')
            && !code.ends_with('-')
            && !code.ends_with('*')
            && !code.ends_with('/')
            && !code.ends_with('=')
            && !code.ends_with('>')
            && !code.ends_with('<')
            && !code.ends_with('|')
            && !code.ends_with('&')
            && !code.contains("->")
            && !code.contains("=>")
            && !tokens
                .iter()
                .any(|token| token.text == "{" || token.text == "}");

        let (mut parens, mut brackets) = (0isize, 0isize);
        let mut invalid = false;
        for token in tokens {
            match token.text {
                "(" => parens += 1,
                ")" => parens -= 1,
                "[" => brackets += 1,
                "]" => brackets -= 1,
                _ => {}
            }
            invalid |= parens < 0 || brackets < 0;
        }
        let next = lines.get(index + 1).map_or("", |line| line.trim_start());
        let definitely_complete = bare_return
            || (simple_expression_return
                && !return_expression_is_multiline(tokens, next)
                && !invalid
                && parens == 0
                && brackets == 0);

        if definitely_complete {
            let at = code.len();
            output.push_str(&line[..at]);
            output.push(';');
            output.push_str(&line[at..]);
        } else {
            output.push_str(line);
        }
    }
    output
}

/// Break compact conditional arms and safely terminate full-line returns.
pub(crate) fn normalize(source: &str) -> String {
    terminate_simple_returns(&split_keyword_blocks(source))
}

#[cfg(test)]
mod tests {
    use super::normalize;

    #[test]
    fn expands_keyword_if_with_terminator() {
        assert_eq!(
            normalize("if self.built; then return Err(\"builder already consumed\"); fi"),
            "if self.built; then\n return Err(\"builder already consumed\");\nfi"
        );
    }

    #[test]
    fn inserts_return_terminator() {
        assert_eq!(
            normalize("if done then return Err(\"not ready\") fi"),
            "if done then\n return Err(\"not ready\");\nfi"
        );
    }

    #[test]
    fn comments_and_literals_remain_opaque() {
        let line = "if ready; then return \"else; fi; then\"; fi // then fi";
        assert_eq!(
            normalize(line),
            "if ready; then\n return \"else; fi; then\";\nfi // then fi"
        );
    }

    #[test]
    fn contextual_then_and_do_inside_braced_conditions_do_not_open_keyword_blocks() {
        for source in [
            "if predicate.then() {\nwork();\n}\nfi",
            "if predicate.do() {\nwork();\n}\nfi",
            "if predicate(then) {\nwork();\n}\nfi",
            "if predicate(do) {\nwork();\n}\nfi",
        ] {
            assert_eq!(normalize(source), source);
        }
    }

    #[test]
    fn top_level_then_after_nested_contextual_tokens_is_still_recognized() {
        assert_eq!(
            normalize("if predicate(then) && obj.do(); then return 1 fi"),
            "if predicate(then) && obj.do(); then\n return 1;\nfi"
        );
    }

    #[test]
    fn closes_inline_if_after_literal_and_identifier_returns_without_semicolons() {
        assert_eq!(
            normalize("if ready then return 1 fi"),
            "if ready then\n return 1;\nfi"
        );
        assert_eq!(
            normalize("if ready then return value fi"),
            "if ready then\n return value;\nfi"
        );
    }

    #[test]
    fn fi_as_return_operand_is_not_confused_with_the_structural_closer() {
        assert_eq!(
            normalize("if ready; then return fi; fi"),
            "if ready; then\n return fi;\nfi"
        );
    }

    #[test]
    fn canonicalizes_bare_return_and_preserves_line_comment() {
        assert_eq!(normalize("return"), "return;");
        assert_eq!(normalize("return // done"), "return; // done");
    }

    #[test]
    fn refuses_to_terminate_multiline_expression_introducers() {
        assert_eq!(
            normalize("return match value over\non _ -> 1"),
            "return match value over\non _ -> 1"
        );
        assert_eq!(
            normalize("return select from plan"),
            "return select from plan"
        );
        assert_eq!(
            normalize("return value\n  .transform()"),
            "return value\n  .transform()"
        );
        assert_eq!(
            normalize("return value\n  as Result"),
            "return value\n  as Result"
        );
    }

    #[test]
    fn still_terminates_obviously_complete_async_and_cast_returns() {
        assert_eq!(normalize("return await task()"), "return await task();");
        assert_eq!(
            normalize("return value as Result"),
            "return value as Result;"
        );
        assert_eq!(normalize("return obj.match()"), "return obj.match();");
    }
}
