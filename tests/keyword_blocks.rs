use oreslang_format::{format_source, is_formatted};

#[test]
fn compact_keyword_if_expands_inside_a_callable() {
    let input = r#"define class Builder as
  pub fnc build(): Result {
    if self.built; then return Err("builder already consumed"); fi
    return Ok(self.value)
  }
end
"#;
    let expected = r#"define class Builder as
  pub build() -> Result {
    if self.built; then
      return Err("builder already consumed");
    fi
    return Ok(self.value);
  }
end
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert!(is_formatted(&formatted).unwrap());
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn no_same_line_conditionals_in_one_line_callable() {
    let input = r#"fnc check() -> Result { if self.built; then return Err("already consumed"); fi return Ok(1); }"#;
    let expected = r#"fnc check() -> Result {
  if self.built; then
    return Err("already consumed");
  fi
  return Ok(1);
}
"#;
    assert_eq!(format_source(input).unwrap(), expected);
}

#[test]
fn nested_inline_if_else_elif_and_fi_are_separate() {
    let input = r#"fnc classify(int x) -> int {
if x < 0 then if x < -5 then return -5; else return -1; fi elif x == 0 then return 0; else return 1; fi
}
"#;
    let expected = r#"fnc classify(int x) -> int {
  if x < 0; then
    if x < -5; then
      return -5;
    else
      return -1;
    fi
  elif x == 0; then
    return 0;
  else
    return 1;
  fi
}
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn preserves_comments_and_literals_with_conditional_words() {
    let input = r#"fnc check(): String {
if obj.then; then return "else; fi; then"; fi // comment: then fi
return "http://example.com" // leave comment in place
}
"#;
    let expected = r#"fnc check() -> String {
  if obj.then; then
    return "else; fi; then";
  fi // comment: then fi
  return "http://example.com"; // leave comment in place
}
"#;
    assert_eq!(format_source(input).unwrap(), expected);
}

#[test]
fn leave_multiline_return_expressions_untouched() {
    let input = r#"fnc calc() -> int {
return calculate(
value,
another
)
}
"#;
    let formatted = format_source(input).unwrap();
    assert!(formatted.contains("return calculate(\n"));
    assert!(formatted.contains("  )\n"));
    assert!(!formatted.contains("return calculate(;"));
}

#[test]
fn return_value_semicolon_is_idempotent() {
    let first = format_source("fnc f() -> int {\nreturn compute(42)\n}\n").unwrap();
    assert!(first.contains("return compute(42);"));
    assert_eq!(format_source(&first).unwrap(), first);
}

#[test]
fn unrelated_lexical_if_and_fi_are_not_conditional_blocks() {
    let input = r#"fnc demo() -> int {
const fi = 10;
const name = "if x then return 0 fi";
return fi;
}
"#;
    let output = format_source(input).unwrap();
    assert!(output.contains("const fi = 10;"));
    assert!(output.contains("return fi;"));
    assert_eq!(format_source(&output).unwrap(), output);
}
