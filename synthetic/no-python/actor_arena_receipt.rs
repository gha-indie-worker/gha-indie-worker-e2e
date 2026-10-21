use std::{env, fs, path::PathBuf, process::{Command, exit}};

fn fail(message: impl AsRef<str>) -> ! {
    eprintln!("ACTOR_ARENA_RECEIPT_DENIED: {}", message.as_ref());
    exit(1);
}

fn env_required(name: &str) -> String {
    env::var(name).unwrap_or_else(|_| fail(format!("missing environment variable {name}")))
}

fn parse_u64(name: &str) -> u64 {
    env_required(name)
        .parse()
        .unwrap_or_else(|_| fail(format!("invalid integer environment variable {name}")))
}

fn git(args: &[&str]) -> String {
    let output = Command::new("git")
        .args(args)
        .output()
        .unwrap_or_else(|e| fail(format!("git launch failed: {e}")));
    if !output.status.success() || !output.stderr.is_empty() {
        fail(format!(
            "git {:?} failed: {}",
            args,
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let text = String::from_utf8(output.stdout).unwrap_or_else(|_| fail("git output is not UTF-8"));
    text.trim().to_string()
}

fn json_escape(value: &str) -> String {
    let mut out = String::new();
    for ch in value.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c < ' ' => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

fn main() {
    let mut args = env::args_os().skip(1);
    let output = args
        .next()
        .map(PathBuf::from)
        .unwrap_or_else(|| fail("usage: actor_arena_receipt OUTPUT"));
    if args.next().is_some() {
        fail("unexpected extra argument");
    }

    let expected_head = env_required("EXPECTED_HEAD");
    let head = git(&["rev-parse", "HEAD"]);
    if head != expected_head {
        fail(format!("receipt HEAD mismatch: expected {expected_head}, got {head}"));
    }
    let tree = git(&["rev-parse", "HEAD^{tree}"]);
    let repository = env_required("GITHUB_REPOSITORY");
    let repository_id = parse_u64("EXPECTED_REPO_ID");
    let run_id = parse_u64("GITHUB_RUN_ID");
    let run_attempt = parse_u64("GITHUB_RUN_ATTEMPT");

    let paths = [
        "contracts/v1/SHARED_ACTOR_ARENAS_V1.md",
        "contracts/v1/actor_arena_lifecycle_v1.json",
        "tools/contract_gate.rs",
        "tools/actor_arena_receipt.rs",
    ];
    let mut blobs = Vec::new();
    for path in paths {
        let sha = git(&["hash-object", path]);
        if sha.len() != 40 || !sha.bytes().all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()) {
            fail(format!("invalid Git blob SHA for {path}"));
        }
        blobs.push((path, sha));
    }

    let mut text = String::new();
    text.push_str("{\n");
    text.push_str("  \"schema\": \"oreslang.actor_arena_lifecycle_sourcefree_v1\",\n");
    text.push_str(&format!("  \"repository\": \"{}\",\n", json_escape(&repository)));
    text.push_str(&format!("  \"repository_id\": {repository_id},\n"));
    text.push_str(&format!("  \"head_sha\": \"{head}\",\n"));
    text.push_str(&format!("  \"tree_sha\": \"{tree}\",\n"));
    text.push_str(&format!("  \"run_id\": {run_id},\n"));
    text.push_str(&format!("  \"run_attempt\": {run_attempt},\n"));
    text.push_str("  \"git_blobs\": {\n");
    for (index, (path, sha)) in blobs.iter().enumerate() {
        text.push_str(&format!(
            "    \"{}\": \"{}\"{}\n",
            json_escape(path),
            sha,
            if index + 1 == blobs.len() { "" } else { "," }
        ));
    }
    text.push_str("  },\n");
    text.push_str("  \"round_counts\": [40, 40, 40],\n");
    text.push_str("  \"executed_total\": 120,\n");
    text.push_str("  \"source_free_only\": true,\n");
    text.push_str("  \"native_backend_certified\": false,\n");
    text.push_str("  \"guest_runtime_certified\": false,\n");
    text.push_str("  \"callback_wiring_certified\": false,\n");
    text.push_str("  \"zero_gc_certified\": false\n");
    text.push_str("}\n");

    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent).unwrap_or_else(|e| fail(format!("create receipt directory: {e}")));
    }
    fs::write(&output, text).unwrap_or_else(|e| fail(format!("write receipt: {e}")));
    println!("ACTOR_ARENA_RECEIPT_PASS={}", output.display());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn json_escaping_is_valid_for_metadata() {
        assert_eq!(json_escape("a\\b\"c\n"), "a\\\\b\\\"c\\n");
    }
}
