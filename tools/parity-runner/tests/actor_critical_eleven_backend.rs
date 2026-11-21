//! Eleven-compiler admission inventory; never upgrades authored .atto fixtures
//! or host Rust models to compiler/guest execution evidence.
use serde_json::Value;
use std::{
    collections::BTreeSet,
    fs,
    path::{Path, PathBuf},
    process::Command,
};

const BACKENDS: [(&str, &str); 11] = [
    ("java", "atto-lang/atto-source.java"),
    ("llvm", "atto-lang/atto-source.llvm"),
    ("dotnet", "atto-lang/atto-source.net"),
    ("rs", "atto-lang/atto-source.rs"),
    ("wasm", "atto-lang/atto-source.wasm"),
    ("forge", "atto-lang/atto-source.forge"),
    ("cranelift", "atto-lang/atto-source.cranelift"),
    ("js", "atto-lang/atto-source.js"),
    ("hdl", "atto-lang/atto-source.hdl"),
    ("realtime", "atto-lang/atto-source.realtime"),
    ("atto", "atto-lang/atto-source.atto"),
];

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn admission() -> Value {
    let data =
        fs::read(root().join("contracts/atto-actor-critical-v1/all-eleven-backend-admission.json"))
            .expect("read eleven compiler admission contract");
    serde_json::from_slice(&data).expect("valid admission JSON")
}

fn sha_is_exact(s: &str) -> bool {
    s.len() == 40
        && s.bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn validate(value: &Value) -> Result<(), String> {
    if value["schema_version"] != 1
        || value["contract_id"] != "atto-actor-critical-all-eleven-v1"
        || value["status"] != "AUTHORED_NOT_EXECUTED"
    {
        return Err("contract version/status mismatch".into());
    }
    if value["canonical_contract"]["positive_git_blob_sha"]
        != "a6189d92c813426ecf54a09cf535c3874de45942"
        || value["canonical_contract"]["id"] != "atto-actor-critical-v1"
    {
        return Err("pinned canonical positive fixture changed".into());
    }
    if value["reference_rust_model"]["evidence_tier"] != "HOST_MODEL_ONLY"
        || value["reference_rust_model"]["guest_parity"] != "NOT_EXECUTED"
        || !value["reference_rust_model"]["source_head_sha"]
            .as_str()
            .is_some_and(sha_is_exact)
        || !value["reference_rust_model"]["root_tree_sha"]
            .as_str()
            .is_some_and(sha_is_exact)
    {
        return Err("Rust model must not be promoted to guest authority".into());
    }
    let rows = value["required_backend_repositories"]
        .as_array()
        .ok_or("backend roster missing")?;
    if rows.len() != BACKENDS.len() {
        return Err("not all eleven compiler backends enumerated".into());
    }
    let mut seen = BTreeSet::new();
    for row in rows {
        let backend = row["backend"].as_str().ok_or("missing backend ID")?;
        let repository = row["repository"].as_str().ok_or("missing repository")?;
        if !seen.insert(backend) {
            return Err("duplicate backend ID".into());
        }
        if !BACKENDS.contains(&(backend, repository)) {
            return Err(format!(
                "noncanonical backend/repository identity: {backend}"
            ));
        }
        if !row["source_head_sha"].as_str().is_some_and(sha_is_exact) {
            return Err(format!("missing exact compiler snapshot SHA: {backend}"));
        }
        for field in ["compiler_guest_status", "semantic_negative_status"] {
            if row[field] != "NOT_EXECUTED" {
                return Err(format!("{backend} has unjustified status: {field}"));
            }
        }
        for field in ["physical_loan_status", "provenance_binding_status"] {
            if row[field] != "NOT_VERIFIED" {
                return Err(format!("{backend} has unjustified safety proof: {field}"));
            }
        }
        if !row["receipt"].is_null() {
            return Err(format!("{backend} has an unaudited fabricated receipt"));
        }
    }

    let negatives = value["canonical_contract"]["negative_cases"]
        .as_array()
        .ok_or("negative cases missing")?;
    if negatives.len() != 2
        || negatives[0]["id"] != "deny-send-inside-read"
        || negatives[0]["expected_category"] != "actor_state_loan_escape_to_mailbox"
        || negatives[0]["git_blob_sha"] != "89eb278a139eb13079e8eda69a943f94591671cc"
        || negatives[1]["id"] != "deny-write-inside-read"
        || negatives[1]["expected_category"] != "write_through_read_only_actor_loan"
        || negatives[1]["git_blob_sha"] != "b279055abc2ee5175299a74fe5b66662581a87cf"
    {
        return Err("negative semantic cases or blob IDs drifted".into());
    }
    if value["admission"]["source_bytes"] != "SAME_THREE_PINNED_ATTO_BLOBS_FOR_ALL_BACKENDS" {
        return Err("backend-specific guest source substitution forbidden".into());
    }
    let gates = value["admission"]["requires"]
        .as_array()
        .ok_or("required evidence gates missing")?;
    for gate in [
        "compiler_exact_head_and_tree",
        "real_backend_compiler_invocation",
        "positive_guest_actual_stdout_exact",
        "both_negative_semantic_diagnostics_not_generic_parse_errors",
        "runtime_minted_ref_and_authenticated_actor_incarnation",
        "physical_rw_guard_held_through_load_store",
        "stop_panic_cancel_quiescence_before_reclaim",
        "nonzero_actions_runner_and_executed_steps",
        "no_backend_substitution",
        "no_host_only_rust_model_as_guest_receipt",
    ] {
        if !gates.iter().any(|v| v.as_str() == Some(gate)) {
            return Err(format!("mandatory release gate missing: {gate}"));
        }
    }
    Ok(())
}

fn assert_regular_source(path: &Path) {
    assert!(path.is_file(), "missing source file: {}", path.display());
    assert!(
        !fs::symlink_metadata(path).unwrap().file_type().is_symlink(),
        "symbolic links are not trusted corpus files"
    );
}

#[test]
fn all_eleven_compiler_repositories_have_distinct_fail_closed_rows() {
    validate(&admission()).unwrap();
}

#[test]
fn exact_same_positive_and_negative_atto_bytes_are_pinned() {
    let contract = admission();
    let source = [
        (
            "contracts/atto-actor-critical-v1/fixtures/two-actor-snapshot-positive.atto",
            "a6189d92c813426ecf54a09cf535c3874de45942",
        ),
        (
            "contracts/atto-actor-critical-v1/fixtures/deny-send-inside-read.atto",
            "89eb278a139eb13079e8eda69a943f94591671cc",
        ),
        (
            "contracts/atto-actor-critical-v1/fixtures/deny-write-inside-read.atto",
            "b279055abc2ee5175299a74fe5b66662581a87cf",
        ),
    ];
    assert_eq!(
        contract["canonical_contract"]["positive_fixture"],
        source[0].0
    );
    for (relative, sha) in source {
        let absolute = root().join(relative);
        assert_regular_source(&absolute);
        let observed = Command::new("git")
            .arg("hash-object")
            .arg("--no-filters")
            .arg(&absolute)
            .output()
            .expect("git required to authenticate fixture blobs");
        assert!(observed.status.success(), "git hash-object failed");
        let observed = String::from_utf8(observed.stdout).unwrap();
        assert_eq!(observed.trim(), sha, "fixture blob differs: {relative}");
    }
}

#[test]
fn missing_duplicate_or_swapped_backend_cannot_pass_audit() {
    let mut v = admission();
    v["required_backend_repositories"]
        .as_array_mut()
        .unwrap()
        .pop();
    assert!(validate(&v).is_err());

    let mut v = admission();
    let rows = v["required_backend_repositories"].as_array_mut().unwrap();
    rows[1] = rows[0].clone();
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["required_backend_repositories"][0]["repository"] =
        Value::String("atto-lang/atto-source.js".into());
    assert!(validate(&v).is_err());
}

#[test]
fn fake_pass_guest_receipt_and_unverified_physical_loan_are_denied() {
    let mut v = admission();
    v["required_backend_repositories"][0]["compiler_guest_status"] = Value::String("PASS".into());
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["required_backend_repositories"][1]["semantic_negative_status"] =
        Value::String("SKIPPED_AS_PASS".into());
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["required_backend_repositories"][2]["physical_loan_status"] =
        Value::String("VERIFIED".into());
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["required_backend_repositories"][3]["receipt"] =
        serde_json::json!({"run_id": 1, "success": true});
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["reference_rust_model"]["evidence_tier"] = Value::String("COMPILER_GUEST_EXECUTED".into());
    assert!(validate(&v).is_err());
}

#[test]
fn missing_identity_source_rewrite_or_missing_gate_are_denied() {
    let mut v = admission();
    v["required_backend_repositories"][7]["source_head_sha"] = Value::String("short".into());
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["canonical_contract"]["positive_git_blob_sha"] = Value::String("0".repeat(40));
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["admission"]["source_bytes"] = Value::String("PER_BACKEND".into());
    assert!(validate(&v).is_err());

    let mut v = admission();
    v["admission"]["requires"].as_array_mut().unwrap().clear();
    assert!(validate(&v).is_err());
}
