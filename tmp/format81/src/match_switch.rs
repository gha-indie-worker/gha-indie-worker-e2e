//! Canonicalize optional separators between completed switch/match arms.
//! Only remove punctuation immediately after a verified arm body; quoted and
//! commented characters, and commas inside case alternatives, are untouched.

use crate::select::{Token, break_after, break_before, matching_brace, tokens};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Copy)]
enum Kind {
    Switch,
    Match,
}

fn arm_starts(kind: Kind, text: &str) -> bool {
    match kind {
        Kind::Switch => matches!(text, "case" | "default" | "_"),
        Kind::Match => matches!(text, "on" | "default"),
    }
}

fn outer_open(tokens: &[Token<'_>], at: usize, kind: Kind) -> Option<usize> {
    let mut parens = 0usize;
    let mut saw_over = false;
    for (i, token) in tokens.iter().enumerate().skip(at + 1) {
        match token.text {
            "(" => parens += 1,
            ")" => parens = parens.saturating_sub(1),
            // A legacy keyword-delimited switch/match reaches an arm label
            // *before* any outer '{'. Its arm's own body brace must never
            // be mistaken for a switch/match block opener.
            "case" | "default" | "_" if parens == 0 && matches!(kind, Kind::Switch) => return None,
            "on" | "default" if parens == 0 && saw_over && matches!(kind, Kind::Match) => {
                return None;
            }
            "over" if parens == 0 => saw_over = true,
            "{" if parens == 0 => {
                if matches!(kind, Kind::Switch) || saw_over {
                    return Some(i);
                }
                return None;
            }
            "end" | ";" if parens == 0 => return None,
            _ => {}
        }
    }
    None
}

pub(crate) fn normalize(source: &str) -> String {
    let lexed = tokens(source);
    let mut removed = BTreeSet::<usize>::new();
    let mut inserted = BTreeMap::<usize, &'static str>::new();
    let mut edits = BTreeMap::<usize, String>::new();

    for (i, token) in lexed.iter().enumerate() {
        let kind = match token.text {
            "switch" => Kind::Switch,
            "match" => Kind::Match,
            _ => continue,
        };
        // Ignore calls and member names that merely contain these words.
        if i > 0 && lexed[i - 1].text == "." {
            continue;
        }
        let Some(open) = outer_open(&lexed, i, kind) else {
            continue;
        };
        let Some(close) = matching_brace(&lexed, open) else {
            continue;
        };
        break_after(source, lexed[open].end, &mut edits);
        break_before(source, lexed[close].start, &mut edits);
        let mut arm = open + 1;
        while arm < close {
            if !arm_starts(kind, lexed[arm].text) {
                arm += 1;
                continue;
            }
            break_before(source, lexed[arm].start, &mut edits);
            // Scan only within the block, skipping parenthesized tuple
            // bindings and switch scrutinee alternatives.
            let mut parens = 0usize;
            let mut brackets = 0usize;
            let mut body = arm + 1;
            while body < close {
                match lexed[body].text {
                    "(" => parens += 1,
                    ")" => parens = parens.saturating_sub(1),
                    "[" => brackets += 1,
                    "]" => brackets = brackets.saturating_sub(1),
                    "{" if parens == 0 && brackets == 0 => {
                        // The first brace in 'on Foo as {v} -> { ... }'
                        // is a *record pattern*, not an executable block.
                        // Only skip a balanced shape if '->' follows it.
                        if let Some(shape_end) = matching_brace(&lexed, body) {
                            if shape_end + 2 < close
                                && lexed[shape_end + 1].text == "-"
                                && lexed[shape_end + 2].text == ">"
                            {
                                body = shape_end + 1;
                                continue;
                            }
                        }
                        // An inline 'structural { ... }' pattern shape is
                        // not the arm body; keep it intact and scan onward.
                        if body > arm && lexed[body - 1].text == "structural" {
                            let Some(shape_end) = matching_brace(&lexed, body) else {
                                break;
                            };
                            body = shape_end + 1;
                            continue;
                        }
                        break;
                    }
                    _ => {}
                }
                body += 1;
            }
            if body == close {
                break;
            }
            let Some(end) = matching_brace(&lexed, body).filter(|end| *end < close) else {
                break;
            };
            break_after(source, lexed[body].end, &mut edits);
            break_before(source, lexed[end].start, &mut edits);
            // Rewrite only verified catch-all arm heads, never tuple discards.
            let start = arm;
            if matches!(kind, Kind::Switch) && lexed[start].text == "_" {
                for pos in lexed[start].start..lexed[start].end {
                    removed.insert(pos);
                }
                inserted.insert(lexed[start].start, "default");
            }
            // Canonical Java/GraalVM match syntax uses 'on _ ->' for its
            // catch-all; never silently rewrite it to switch's 'default'.
            // Migrate only the legacy 'on default' spelling to 'on _'.
            if matches!(kind, Kind::Match)
                && lexed[start].text == "on"
                && start + 1 < body
                && lexed[start + 1].text == "default"
            {
                for pos in lexed[start + 1].start..lexed[start + 1].end {
                    removed.insert(pos);
                }
                inserted.insert(lexed[start + 1].start, "_");
            }
            let mut next = end + 1;
            while next < close && matches!(lexed[next].text, "," | ";") {
                // Delete only visible delimiter bytes (comments and all
                // surrounding text stay exactly where they were).
                for pos in lexed[next].start..lexed[next].end {
                    removed.insert(pos);
                }
                next += 1;
            }
            arm = next;
        }
    }

    if removed.is_empty() && edits.is_empty() {
        return source.to_owned();
    }
    let mut result = String::with_capacity(source.len() + inserted.len() * 8 + edits.len());
    for (pos, ch) in source.char_indices() {
        if let Some(prefix) = edits.get(&pos) {
            result.push_str(prefix);
        }
        if let Some(replacement) = inserted.get(&pos) {
            result.push_str(replacement);
        }
        if !removed.contains(&pos) {
            result.push(ch);
        }
    }
    if let Some(suffix) = edits.get(&source.len()) {
        result.push_str(suffix);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::normalize;

    #[test]
    fn strips_only_arm_separators_not_case_alternatives() {
        let input = "switch (v) {\n  _ { return 0; },\n  case \"foo\", \"bar\" { return 1; };\n}\n";
        let output = normalize(input);
        assert!(output.contains("default {\n"));
        assert!(output.contains("case \"foo\", \"bar\" {\n"));
        assert_eq!(normalize(&output), output);
    }

    #[test]
    fn leaves_unrelated_braces_comments_and_literals_intact() {
        let input = "fnc f() {\n  let s = \"case } , ;\";\n  select { default: { return; }, }\n  match msg over {\n    on Tag as (a, b) { log(a, b); }, // a comment\n    on _ { return; };\n  }\n}\n";
        let output = normalize(input);
        assert!(output.contains("select { default: { return; }, }"));
        assert!(output.contains("on Tag as (a, b) {"));
        assert!(output.contains("on _ {"));
        assert!(output.contains("\"case } , ;\""));
    }

    #[test]
    fn canonicalizes_only_catchall_head_tokens() {
        let source = r#"switch v {
  _ { return "star"; }
  case "foo", "bar" { return "baz"; }
}
match msg over {
  on _ as remaining { handle(remaining); }
  on Thing.Tuple as (_, item) { handle(item); }
}
match msg over {
  on default { return; }
}
"#;
        let output = normalize(source);
        assert!(output.contains("default {"), "{output}");
        assert!(output.contains("on _ as remaining {"), "{output}");
        assert!(output.contains("on Thing.Tuple as (_, item) {"), "{output}");
        assert!(output.contains("default {"), "{output}");
        assert!(!output.contains("on default"), "{output}");
        assert!(output.contains("on _ as"), "{output}");
        assert_eq!(normalize(&output), output);
    }

    #[test]
    fn never_reinterprets_legacy_switch_arm_braces_as_outer_switch() {
        let legacy = "define module M as\n  switch 1;\n    case 1 -> {\n      switch 2\n        case 2 -> {}\n      end\n    }\n  end\nend\n";
        assert_eq!(normalize(legacy), legacy);
    }

    #[test]
    fn preserves_comments_between_on_and_catchall_alias() {
        let source = r#"match v over {
  on /*why*/ _ as item { handle(item); }
  on 7 { handle(7); }
}
"#;
        let output = normalize(source);
        assert!(output.contains("on /*why*/ _ as item"), "{output}");
        assert_eq!(normalize(&output), output);
    }

    #[test]
    fn ignores_underscores_in_unrelated_syntax_and_strings() {
        let source = r#"fnc f() {
  const [x, _] = [1, 2];
  val quoted = "on _ { }";
  return quoted;
}"#;
        assert_eq!(normalize(source), source);
    }
}
