//! Expand compact callable bodies before the indentation pass. Only add line
//! breaks at lexically visible, statement-level boundaries. In particular,
//! quoted braces and semicolons, comments, argument lists and struct literals
//! must never be mistaken for body delimiters.

use crate::{LexState, is_callable_body_header, scan_visible};

pub(crate) fn normalize(source: &str) -> String {
    let mut result = String::with_capacity(source.len());
    let mut lex = LexState::default();

    for (line_index, line) in source.split('\n').enumerate() {
        if line_index != 0 {
            result.push('\n');
        }

        let initial = lex;
        let punctuation = visible_punctuation(line, &mut lex);
        let opening = punctuation
            .iter()
            .enumerate()
            .find_map(|(index, &(at, ch))| {
                if ch != '{' {
                    return None;
                }

                // An inline struct type or default argument can contain braces
                // before the callable's actual body.
                let (mut parens, mut brackets) = (0isize, 0isize);
                for &(_, ch) in &punctuation[..index] {
                    match ch {
                        '(' => parens += 1,
                        ')' => parens -= 1,
                        '[' => brackets += 1,
                        ']' => brackets -= 1,
                        _ => {}
                    }
                }
                if parens != 0 || brackets != 0 {
                    return None;
                }

                let mut header_lex = initial;
                let visible: String = scan_visible(&line[..at], &mut header_lex)
                    .into_iter()
                    .collect();
                let header = visible.trim_start().trim_end();
                if header.ends_with("struct") || header.ends_with("trait") {
                    return None;
                }
                if is_callable_body_header(&format!("{header} {{"), false) {
                    Some(index)
                } else {
                    None
                }
            });

        let Some(opening) = opening else {
            result.push_str(line);
            continue;
        };

        // Keep incomplete and multiline bodies on the existing formatter path.
        // Never splice a newline across a line comment or string token.
        let mut depth = 0usize;
        let mut closing = None;
        for (index, &(_, ch)) in punctuation.iter().enumerate().skip(opening) {
            match ch {
                '{' => depth += 1,
                '}' => {
                    depth -= 1;
                    if depth == 0 {
                        closing = Some(index);
                        break;
                    }
                }
                _ => {}
            }
        }
        let Some(closing) = closing else {
            result.push_str(line);
            continue;
        };

        let mut cuts = vec![punctuation[opening].0 + 1, punctuation[closing].0];
        let (mut nested, mut parens, mut brackets) = (0usize, 0usize, 0usize);
        for &(at, ch) in &punctuation[opening + 1..closing] {
            let has_following_code = !line[at + 1..punctuation[closing].0].trim().is_empty();
            match ch {
                '{' => nested += 1,
                '}' => nested = nested.saturating_sub(1),
                '(' => parens += 1,
                ')' => parens = parens.saturating_sub(1),
                '[' => brackets += 1,
                ']' => brackets = brackets.saturating_sub(1),
                // Preserve the condition terminator in if/elif headers.
                // The keyword-block pass expands the body after 'then'.
                ';' if nested == 0 && parens == 0 && brackets == 0 && has_following_code => {
                    let after = line[at + 1..punctuation[closing].0].trim_start();
                    let header_suffix =
                        crate::starts_word(after, "then") || crate::starts_word(after, "do");
                    let prefix = &line[punctuation[opening].0 + 1..at];
                    let words = crate::visible_word_spans(prefix, crate::LexState::default());
                    let has_if = words.iter().any(|word| {
                        matches!(&prefix[word.start..word.end], "if" | "elif" | "elseif")
                    });
                    if !(header_suffix && has_if) {
                        cuts.push(at + 1);
                    }
                }
                _ => {}
            }
        }
        cuts.sort_unstable();
        cuts.dedup();

        let mut start = 0;
        for cut in cuts {
            result.push_str(line[start..cut].trim_end());
            result.push('\n');
            start = cut;
        }
        result.push_str(&line[start..]);
    }
    result
}

/// Return byte offsets of syntax punctuation, excluding strings and comments.
/// Scan state is carried across lines so multiline block comments are opaque.
fn visible_punctuation(line: &str, state: &mut LexState) -> Vec<(usize, char)> {
    let mut result = Vec::new();
    let mut chars = line.char_indices().peekable();
    while let Some((at, ch)) = chars.next() {
        let next = chars.peek().map(|&(_, ch)| ch);
        if state.block_comment {
            if ch == '*' && next == Some('/') {
                chars.next();
                state.block_comment = false;
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
            break;
        }
        if ch == '/' && next == Some('*') {
            chars.next();
            state.block_comment = true;
            continue;
        }
        if ch == '"' || ch == '\'' || ch == 0x60 as char {
            state.quote = Some(ch);
            continue;
        }
        if matches!(ch, '{' | '}' | ';' | '(' | ')' | '[' | ']') {
            result.push((at, ch));
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::normalize;

    #[test]
    fn only_splits_visible_statement_boundaries() {
        let source = r#"fnc run() -> String { val s = "};"; /* ; } */ return s; }"#;
        assert_eq!(
            normalize(source),
            "fnc run() -> String {\n val s = \"};\";\n /* ; } */ return s;\n}"
        );
    }

    #[test]
    fn never_breaks_structs_or_for_headers() {
        let source = r#"const v = infer struct {foo: ";"};\nfnc f() { for (i = 0; i < 3; i++) { do_work(); } return; }"#;
        assert!(normalize(source).starts_with("const v = infer struct {foo: \";\"};"));
        assert!(normalize(source).contains("for (i = 0; i < 3; i++)"));
    }
}
