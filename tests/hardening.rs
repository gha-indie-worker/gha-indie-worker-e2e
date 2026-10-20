use oreslang_format::{format_source, is_formatted};

#[test]
fn minified_keyword_conditionals_expand_without_existing_semicolons() {
    let input = r#"fnc choose(bool a, bool b) -> int { if a then return 1 fi if b then return 2 fi return 3 }"#;
    let expected = r#"fnc choose(bool a, bool b) -> int {
  if a; then
    return 1;
  fi
  if b; then
    return 2;
  fi
  return 3;
}
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert!(is_formatted(&formatted).unwrap());
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn bare_returns_gain_terminators_without_touching_loop_structure() {
    let input = r#"fnc stop() -> void {
if ready then
while busy; do
tick();
done
return
fi
}
"#;
    let expected = r#"fnc stop() -> void {
  if ready; then
    while busy; do
      tick();
    done
    return;
  fi
}
"#;
    assert_eq!(format_source(input).unwrap(), expected);
}

#[test]
fn fi_identifier_can_still_be_returned_inside_keyword_if() {
    let input = r#"fnc read() -> int {
const fi = 7;
if ready; then return fi; fi
return 0
}
"#;
    let expected = r#"fnc read() -> int {
  const fi = 7;
  if ready; then
    return fi;
  fi
  return 0;
}
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn return_line_comments_receive_semicolon_before_comment() {
    let input = "fnc value() -> int {\nreturn 42 // answer\n}\n";
    let expected = "fnc value() -> int {\n  return 42; // answer\n}\n";
    assert_eq!(format_source(input).unwrap(), expected);
}

#[test]
fn braced_if_and_nested_done_blocks_remain_on_the_existing_path() {
    let input = r#"fnc work() -> void {
if ready {
work();
} else {
fallback();
}
fi
if again; then
while busy; do
tick();
done
return;
fi
}
"#;
    let formatted = format_source(input).unwrap();
    assert!(formatted.contains("if ready {\n"));
    assert!(formatted.contains("} else {\n"));
    assert!(formatted.contains("fi\n"));
    assert!(formatted.contains("while busy; do\n"));
    assert!(formatted.contains("done\n"));
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn literals_and_argument_semicolons_do_not_create_fake_statement_splits() {
    let input =
        r#"fnc check() -> Result { if ready then return parse("fi; then", callback(1, 2)) fi }"#;
    let expected = r#"fnc check() -> Result {
  if ready; then
    return parse("fi; then", callback(1, 2));
  fi
}
"#;
    assert_eq!(format_source(input).unwrap(), expected);
}

#[test]
fn contextual_then_and_do_names_inside_braced_if_conditions_are_preserved() {
    let input = r#"fnc check() -> void {
if predicate.then() {
work();
}
fi
if predicate(do) {
work();
}
fi
}
"#;
    let expected = r#"fnc check() -> void {
  if predicate.then() {
    work();
  }
  fi
  if predicate(do) {
    work();
  }
  fi
}
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn actual_keyword_then_after_nested_contextual_tokens_still_expands() {
    let input = r#"fnc check() -> int {
if predicate(then) && helper.do(); then return 1 fi
return 0
}
"#;
    let expected = r#"fnc check() -> int {
  if predicate(then) && helper.do(); then
    return 1;
  fi
  return 0;
}
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert!(is_formatted(&formatted).unwrap());
}
