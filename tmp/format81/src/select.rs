//! Normalize static select arms before the indentation pass. Token spans keep
//! comments and literal bytes intact; dynamic `select from` is untouched.
use crate::FormatError;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Copy)]
pub(crate) struct Token<'a> {
    pub(crate) text: &'a str,
    pub(crate) start: usize,
    pub(crate) end: usize,
}

pub(crate) fn tokens(source: &str) -> Vec<Token<'_>> {
    let mut result = Vec::new();
    let mut chars = source.char_indices().peekable();
    while let Some((start, c)) = chars.next() {
        if c.is_whitespace() {
            continue;
        }
        if c == '/' && chars.peek().is_some_and(|(_, c)| *c == '/') {
            for (_, c) in chars.by_ref() {
                if c == '\n' {
                    break;
                }
            }
            continue;
        }
        if c == '/' && chars.peek().is_some_and(|(_, c)| *c == '*') {
            chars.next();
            // The compiler supports nested block comments. Ignore the entire
            // nested span so a fake "select" in an outer comment cannot be
            // mistaken for executable source after the inner */.
            let mut depth = 1usize;
            while let Some((_, ch)) = chars.next() {
                if ch == '/' && chars.peek().is_some_and(|(_, next)| *next == '*') {
                    chars.next();
                    depth = depth.saturating_add(1);
                } else if ch == '*' && chars.peek().is_some_and(|(_, next)| *next == '/') {
                    chars.next();
                    depth -= 1;
                    if depth == 0 {
                        break;
                    }
                }
            }
            continue;
        }
        let mut end = start + c.len_utf8();
        if matches!(c, '"' | '\'' | '`') {
            let mut escaped = false;
            for (pos, next) in chars.by_ref() {
                end = pos + next.len_utf8();
                if escaped {
                    escaped = false;
                } else if next == '\\' {
                    escaped = true;
                } else if next == c {
                    break;
                }
            }
        } else if c.is_alphanumeric() || c == '_' {
            while let Some(&(pos, next)) = chars.peek() {
                if !next.is_alphanumeric() && next != '_' {
                    break;
                }
                chars.next();
                end = pos + next.len_utf8();
            }
        }
        result.push(Token {
            text: &source[start..end],
            start,
            end,
        });
    }
    result
}

fn line_start(source: &str, pos: usize) -> usize {
    source[..pos].rfind('\n').map_or(0, |i| i + 1)
}

pub(crate) fn break_before(source: &str, pos: usize, edits: &mut BTreeMap<usize, String>) {
    if !source[line_start(source, pos)..pos].trim().is_empty() {
        if edits
            .range(..=pos)
            .next_back()
            .is_some_and(|(&previous, edit)| {
                edit.ends_with('\n') && source[previous..pos].trim().is_empty()
            })
        {
            return;
        }
        edits.entry(pos).or_default().push('\n');
    }
}

pub(crate) fn break_after(source: &str, pos: usize, edits: &mut BTreeMap<usize, String>) {
    let rest = source[pos..].split('\n').next().unwrap_or("").trim();
    // A trailing comment belongs to this header/closer.
    if !rest.is_empty() && !rest.starts_with("//") && !rest.starts_with("/*") {
        edits.entry(pos).or_default().push('\n');
    }
}

pub(crate) fn matching_brace(tokens: &[Token<'_>], open: usize) -> Option<usize> {
    let mut depth = 0usize;
    for (i, token) in tokens.iter().enumerate().skip(open) {
        match token.text {
            "{" => depth += 1,
            "}" => {
                depth = depth.checked_sub(1)?;
                if depth == 0 {
                    return Some(i);
                }
            }
            _ => {}
        }
    }
    None
}

fn is_arm_start(tokens: &[Token<'_>], index: usize) -> bool {
    match tokens[index].text {
        "when" | "case" => tokens.get(index + 1).is_some_and(|token| {
            matches!(
                token.text,
                "readch" | "writech" | "await" | "timeout" | "cancelled"
            )
        }),
        "default" => tokens.get(index + 1).is_some_and(|token| token.text == ":"),
        _ => false,
    }
}

fn matching_open(close: &str) -> Option<&'static str> {
    match close {
        ")" => Some("("),
        "]" => Some("["),
        "}" => Some("{"),
        _ => None,
    }
}

fn pop_matching(nesting: &mut Vec<&str>, close: &str) -> bool {
    matching_open(close).is_some_and(|expected| nesting.pop() == Some(expected))
}

/// Locate a slim arrow terminating an executable select arm header, without
/// mistaking a record destructuring pattern for the arm's statement block.
/// Legacy arrowless bodies intentionally return None for migration compatibility.
fn slim_arrow_before_select_body(
    tokens: &[Token<'_>],
    start: usize,
    limit: usize,
) -> Option<usize> {
    let mut nesting = Vec::new();
    let mut i = start;
    while i < limit {
        match tokens[i].text {
            "(" | "[" => nesting.push(tokens[i].text),
            ")" | "]" => {
                if !pop_matching(&mut nesting, tokens[i].text) {
                    return None;
                }
            }
            "{" if nesting.is_empty() => {
                // A record projection may begin directly at the header or
                // follow a binding kind/type. It is a projection *only* when
                // its balanced closing brace is immediately followed by ->.
                // Otherwise this first brace is the legacy statement body.
                let end = matching_brace(tokens, i)?;
                return (end + 2 < limit
                    && tokens[end + 1].text == "-"
                    && tokens[end + 2].text == ">")
                    .then_some(end + 1);
            }
            "-" if nesting.is_empty() && i + 1 < limit && tokens[i + 1].text == ">" => {
                return Some(i);
            }
            ";" if nesting.is_empty() => return None,
            _ => {}
        }
        i += 1;
    }
    None
}

/// Reject malformed executable arrows and incomplete typed binding headers
/// instead of wrapping tokens that belong to the header into a synthetic body.
/// This check is restricted to the token range after the *readiness colon*.
fn invalid_legacy_arm_header(
    tokens: &[Token<'_>],
    start: usize,
    limit: usize,
    read_binding: bool,
) -> bool {
    let mut i = start;
    let mut nesting = Vec::new();
    while i < limit {
        match tokens[i].text {
            "(" | "[" => nesting.push(tokens[i].text),
            ")" | "]" => {
                if !pop_matching(&mut nesting, tokens[i].text) {
                    // Preserve the established delimiter diagnostic from
                    // the unbraced-body parser below.
                    return false;
                }
            }
            "=" if nesting.is_empty() && tokens.get(i + 1).is_some_and(|t| t.text == ">") => {
                return true; // '=>' never delimits an executable select arm.
            }
            "{" if nesting.is_empty() => {
                let Some(end) = matching_brace(tokens, i) else {
                    return true; // unclosed pattern/body
                };
                if end + 2 < limit && tokens[end + 1].text == "=" && tokens[end + 2].text == ">" {
                    return true; // '{fields} => {body}' is never an arm.
                }
                if !read_binding {
                    return false;
                }
                // '{' can be the legacy body, but not the right-hand side of
                // an unfinished 'name: {record}' type annotation.
                return i > start && tokens[i - 1].text == ":";
            }
            ";" if nesting.is_empty() => return false,
            ":" if nesting.is_empty()
                && read_binding
                && (i == start + 1
                    || (i == start + 2
                        && matches!(tokens[start].text, "const" | "val" | "let"))) =>
            {
                return true; // typed 'name: T' without the required arrow
            }
            _ => {}
        }
        i += 1;
    }
    false
}

pub(crate) fn normalize(source: &str) -> Result<String, FormatError> {
    let tokens = tokens(source);
    let mut edits = BTreeMap::<usize, String>::new();
    let mut removed = BTreeSet::new();
    for (i, token) in tokens.iter().enumerate() {
        if token.text != "select" || (i > 0 && tokens[i - 1].text == ".") {
            continue;
        }
        let mut open = i + 1;
        if tokens
            .get(open)
            .is_some_and(|t| matches!(t.text, "first" | "fair" | "random"))
        {
            open += 1;
        }
        if tokens.get(open).is_none_or(|t| t.text != "{") {
            continue;
        }
        let error = |pos: usize, message: &str| {
            FormatError::new(
                source[..pos].bytes().filter(|b| *b == b'\n').count() + 1,
                message,
            )
        };
        let close = matching_brace(&tokens, open)
            .ok_or_else(|| error(token.start, "unclosed static select"))?;
        break_after(source, tokens[open].end, &mut edits);
        break_before(source, tokens[close].start, &mut edits);
        let mut arm = open + 1;
        while arm < close {
            if !matches!(tokens[arm].text, "when" | "case" | "default") {
                return Err(error(
                    tokens[arm].start,
                    "expected when/case/default inside select",
                ));
            }
            break_before(source, tokens[arm].start, &mut edits);
            let arm_keyword = tokens[arm].text;
            if arm_keyword == "case" {
                edits.entry(tokens[arm].start).or_default().push_str("when");
                for (offset, _) in source[tokens[arm].start..tokens[arm].end].char_indices() {
                    removed.insert(tokens[arm].start + offset);
                }
            }
            let operation = tokens.get(arm + 1).map(|t| t.text);
            let read = operation == Some("readch");
            let await_future = operation == Some("await");
            let readiness_without_colon = matches!(operation, Some("timeout" | "cancelled"));
            if matches!(arm_keyword, "when" | "case")
                && !matches!(
                    operation,
                    Some("readch" | "writech" | "await" | "timeout" | "cancelled")
                )
            {
                return Err(error(
                    tokens[arm].start,
                    "select arm must start with readch, writech, await, timeout, or cancelled",
                ));
            }

            let (body, header_end) = if readiness_without_colon {
                // timeout/cancelled have no ':' separator and the compiler
                // requires a braced body. Fail closed rather than guessing
                // where an unbraced cancellation expression ends.
                let mut cursor = arm + 2;
                let mut nesting = Vec::new();
                while cursor < close {
                    match tokens[cursor].text {
                        "{" if nesting.is_empty() => break,
                        "(" | "[" => nesting.push(tokens[cursor].text),
                        ")" | "]" if !pop_matching(&mut nesting, tokens[cursor].text) => {
                            return Err(error(
                                tokens[cursor].start,
                                "mismatched delimiter in timeout/cancelled select arm header",
                            ));
                        }
                        ")" | "]" => {}
                        _ => {}
                    }
                    cursor += 1;
                }
                if cursor == close || tokens[cursor].text != "{" {
                    return Err(error(
                        tokens[arm].start,
                        "timeout/cancelled select arms require a braced body",
                    ));
                }
                if cursor > arm + 2 && tokens[cursor - 1].text == ":" {
                    return Err(error(
                        tokens[cursor - 1].start,
                        "timeout/cancelled select arms do not use ':' before the body",
                    ));
                }
                if cursor <= arm + 2 {
                    return Err(error(
                        tokens[arm].start,
                        "timeout/cancelled select arm requires an operand",
                    ));
                }
                (cursor, tokens[cursor - 1].end)
            } else {
                // readch/writech/await/default use ':' between the readiness
                // expression and arm body. Skip ternary colons and nested
                // expressions while locating that separator.
                let mut header = arm + 1;
                let mut nesting = Vec::new();
                let mut ternaries = 0usize;
                while header < close {
                    match tokens[header].text {
                        "?" if nesting.is_empty() && tokens[header - 1].text != "as" => {
                            ternaries += 1;
                        }
                        ":" if nesting.is_empty() && ternaries > 0 => ternaries -= 1,
                        ":" if nesting.is_empty() => break,
                        "(" | "[" | "{" => nesting.push(tokens[header].text),
                        ")" | "]" | "}" if !pop_matching(&mut nesting, tokens[header].text) => {
                            return Err(error(
                                tokens[header].start,
                                "mismatched delimiter in select arm header",
                            ));
                        }
                        ")" | "]" | "}" => {}
                        _ => {}
                    }
                    header += 1;
                }
                if header == close || tokens[header].text != ":" {
                    return Err(error(tokens[arm].start, "select arm requires ':'"));
                }
                if let Some(arrow) = slim_arrow_before_select_body(&tokens, header + 1, close) {
                    let body = arrow + 2; // tokenizer represents '->' as '-' and '>'
                    if tokens.get(body).is_none_or(|t| t.text != "{") {
                        return Err(error(
                            tokens[arrow].start,
                            "select slim arrow '->' must be followed by a braced arm body",
                        ));
                    }
                    (body, tokens[body - 1].end)
                } else {
                    if invalid_legacy_arm_header(&tokens, header + 1, close, read || await_future) {
                        return Err(error(
                            tokens[arm].start,
                            "invalid select arm header: typed bindings require '->', and executable '=>' is forbidden",
                        ));
                    }
                    let mut body = header + 1;
                    if (read || await_future)
                        && tokens
                            .get(body)
                            .is_some_and(|t| matches!(t.text, "val" | "let" | "const"))
                    {
                        if !tokens.get(body + 1).is_some_and(|t| {
                            t.text
                                .chars()
                                .next()
                                .is_some_and(|c| c.is_alphabetic() || c == '_')
                                && t.text.chars().all(|c| c.is_alphanumeric() || c == '_')
                        }) {
                            return Err(error(
                                tokens[arm].start,
                                if read {
                                    "readch select arm binding requires a name"
                                } else {
                                    "await select arm binding requires a name"
                                },
                            ));
                        }
                        body += 2; // binding kind and name
                        if tokens.get(body).is_some_and(|t| t.text == ";") {
                            removed.insert(tokens[body].start);
                            body += 1;
                        }
                    }
                    let header_end = if body > 0 && tokens[body - 1].text == ";" {
                        tokens[body - 2].end
                    } else {
                        tokens[body - 1].end
                    };
                    (body, header_end)
                }
            };
            if body < close && tokens[body].text == "{" {
                let end = matching_brace(&tokens, body)
                    .filter(|end| *end < close)
                    .ok_or_else(|| error(tokens[body].start, "unclosed select arm body"))?;
                break_after(source, tokens[body].end, &mut edits);
                if body + 1 < end {
                    break_before(source, tokens[body + 1].start, &mut edits);
                }
                break_before(source, tokens[end].start, &mut edits);
                break_after(source, tokens[end].end, &mut edits);
                arm = end + 1;
            } else {
                edits.entry(header_end).or_default().push_str(" {");
                break_after(source, header_end, &mut edits);
                if body < close && !is_arm_start(&tokens, body) {
                    break_before(source, tokens[body].start, &mut edits);
                }
                let mut end = body;
                let mut nesting = Vec::new();
                while end < close {
                    if nesting.is_empty() && is_arm_start(&tokens, end) {
                        break;
                    }
                    match tokens[end].text {
                        "(" | "[" | "{" => nesting.push(tokens[end].text),
                        ")" | "]" | "}" if !pop_matching(&mut nesting, tokens[end].text) => {
                            return Err(error(
                                tokens[end].start,
                                "mismatched delimiter in unbraced select arm body",
                            ));
                        }
                        ")" | "]" | "}" => {}
                        _ => {}
                    }
                    end += 1;
                }
                if !nesting.is_empty() {
                    return Err(error(
                        tokens[body].start,
                        "unclosed delimiter in unbraced select arm body",
                    ));
                }
                let pos = tokens[end].start;
                let start = line_start(source, pos);
                let insertion = if source[start..pos].trim().is_empty() {
                    start
                } else {
                    pos
                };
                edits.entry(insertion).or_default().push_str("\n}\n");
                arm = end;
            }
        }
    }
    let mut output = String::new();
    for (pos, c) in source.char_indices() {
        if let Some(edit) = edits.get(&pos) {
            output.push_str(edit);
        }
        if !removed.contains(&pos) {
            output.push(c);
        }
    }
    if let Some(edit) = edits.get(&source.len()) {
        output.push_str(edit);
    }
    Ok(output)
}

#[cfg(test)]
mod nested_comment_regressions {
    use super::tokens;

    #[test]
    fn deeply_nested_comment_does_not_inject_false_select_tokens() {
        let source = "/* outer /* inner select { when await bad: {} } */ still outer */ select { when timeout 5s {} }";
        let words: Vec<_> = tokens(source).into_iter().map(|token| token.text).collect();
        assert_eq!(words.iter().filter(|&&word| word == "select").count(), 1);
        assert!(!words.contains(&"bad"));
        assert!(!words.contains(&"still"));
        assert!(words.contains(&"timeout"));
    }

    #[test]
    fn unterminated_nested_comment_never_exposes_fake_keyword() {
        let source = "/* outer /* inner */ select { when timeout 5s {} }";
        assert!(tokens(source).is_empty());
    }

    #[test]
    fn comment_boundaries_do_not_eat_real_select_tokens() {
        let source = "/* a /* b */ c */ select {} /* d */";
        let words: Vec<_> = tokens(source).into_iter().map(|token| token.text).collect();
        assert_eq!(words, vec!["select", "{", "}"]);
    }
}
