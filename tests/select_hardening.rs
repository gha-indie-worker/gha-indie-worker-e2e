use oreslang_format::{format_source, is_formatted};

#[test]
fn unbraced_select_body_ignores_arm_like_tokens_inside_nested_expressions() {
    let input = r#"select {
case readch input: val x
consume((when readch nested));
consume([case readch nested]);
default:
}
"#;
    let expected = r#"select {
  when readch input: val x {
    consume((when readch nested));
    consume([case readch nested]);
  }
  default: {
  }
}
"#;
    let formatted = format_source(input).unwrap();
    assert_eq!(formatted, expected);
    assert!(is_formatted(&formatted).unwrap());
    assert_eq!(format_source(&formatted).unwrap(), formatted);
}

#[test]
fn mismatched_select_header_delimiters_fail_closed() {
    for source in [
        "select { case writech output, call([value)): {} }",
        "select { when timeout (deadline]: {} }",
    ] {
        let err = format_source(source).unwrap_err();
        assert!(
            err.message().contains("mismatched delimiter"),
            "{source}: {err}"
        );
    }
}

#[test]
fn mismatched_or_unclosed_unbraced_select_body_delimiters_fail_closed() {
    for source in [
        "select { case readch input: val x call([x)); default: }",
        "select { case readch input: val x call([x]; default: }",
    ] {
        let err = format_source(source).unwrap_err();
        assert!(
            err.message()
                .contains("delimiter in unbraced select arm body"),
            "{source}: {err}"
        );
    }
}
