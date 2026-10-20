use oreslang_format::{format_source, is_formatted};

#[test]
fn compact_module_members_expand_into_statement_lines() {
    let input = r#"define module store as
  pub fnc exists(): bool { return fs.exists(config.data_path()); }
  pub fnc read(): String { return fs.read_text(config.data_path()); }
  pub fnc write(String value): void { fs.write_text(config.data_path(), value); return; }
  pub fnc append(String value): void { fs.append_text(config.data_path(), value); return; }
  pub fnc remove(): void { fs.remove(config.data_path()); return; }
end
"#;
    // Executable return annotations use the formatter's existing -> spelling.
    let expected = r#"define module store as
  pub fnc exists() -> bool {
    return fs.exists(config.data_path());
  }
  pub fnc read() -> String {
    return fs.read_text(config.data_path());
  }
  pub fnc write(String value) -> void {
    fs.write_text(config.data_path(), value);
    return;
  }
  pub fnc append(String value) -> void {
    fs.append_text(config.data_path(), value);
    return;
  }
  pub fnc remove() -> void {
    fs.remove(config.data_path());
    return;
  }
end
"#;
    let result = format_source(input).unwrap();
    assert_eq!(result, expected);
    assert_eq!(format_source(&result).unwrap(), result);
    assert!(is_formatted(&result).unwrap());
    assert!(!is_formatted(input).unwrap());
}

#[test]
fn top_level_functions_retain_existing_blank_line_policy() {
    let input = "fnc one() { return 1; }\nfnc two() { return 2; }\n";
    let expected = "fnc one() {\n  return 1;\n}\n\n\nfnc two() {\n  return 2;\n}\n";
    assert_eq!(format_source(input).unwrap(), expected);
}

#[test]
fn quoted_and_commented_punctuation_is_not_split_or_dedented() {
    let source = r#"define module Store as
  pub fnc one(): String { const str = "{;}//"; /* }; */ return str; }
  // fnc fake() { return 1; }
  pub fnc two(): String { return "a;b}c"; }
end
"#;
    let expected = r#"define module Store as
  pub fnc one() -> String {
    const str = "{;}//";
    /* }; */ return str;
  }
  // fnc fake() { return 1; }
  pub fnc two() -> String {
    return "a;b}c";
  }
end
"#;
    assert_eq!(format_source(source).unwrap(), expected);
}

#[test]
fn ignores_noncallable_braces_and_semicolons_in_nested_expressions() {
    let source = r#"const v = infer struct { foo: ";", bar: "}" };
fnc build() { const x = infer struct { foo: ";", bar: "}" }; return x; }
"#;
    let expected = r#"const v = infer struct { foo: ";", bar: "}" };
fnc build() {
  const x = infer struct { foo: ";", bar: "}" };
  return x;
}
"#;
    assert_eq!(format_source(source).unwrap(), expected);
}

#[test]
fn preserves_existing_multiline_callables() {
    let source = "fnc ready() {\n  return fs.exists(\"/tmp\");\n}\n";
    assert_eq!(format_source(source).unwrap(), source);
}

#[test]
fn compact_empty_callable_expands_and_remains_idempotent() {
    let result = format_source("fnc noop() {}\n").unwrap();
    assert_eq!(result, "fnc noop() {\n}\n");
    assert_eq!(format_source(&result).unwrap(), result);
}
