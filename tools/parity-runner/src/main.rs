use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

// Preserve the original six required backends and existing 54 x 6 baseline.
// All eleven Atto compiler profiles can be selected, but enumeration alone
// is never evidence of guest execution or runtime permission.
const REQUIRED_BACKENDS: [&str; 6] = ["llvm", "java", "wasm", "dotnet", "forge", "cranelift"];
const BACKENDS: [(&str, &str); 11] = [
    ("llvm", "LLVM"),
    ("java", "JAVA"),
    ("wasm", "WASM"),
    ("dotnet", "DOTNET"),
    ("forge", "FORGE"),
    ("cranelift", "CRANELIFT"),
    ("js", "JS"),
    ("rs", "RS"),
    ("hdl", "HDL"),
    ("realtime", "REALTIME"),
    ("atto", "ATTO"),
];

#[derive(Debug, Deserialize)]
struct LegacyManifest {
    cases: Vec<LegacyCase>,
}

#[derive(Debug, Deserialize)]
struct LegacyCase {
    id: String,
    name: String,
    path: String,
    expected_kind: String,
    expected_value: Option<i32>,
    expected_diagnostic_category: Option<String>,
    validation_status: String,
    features: Vec<String>,
}

#[derive(Debug, Deserialize)]
struct ChecksManifest {
    checks: Vec<CheckRef>,
}

#[derive(Debug, Deserialize)]
struct CheckRef {
    name: String,
    expectation: String,
}

#[derive(Debug, Deserialize)]
struct Expectation {
    name: String,
    entry: String,
    stable_stdout_prefix: String,
    imported_files: usize,
    #[serde(rename = "maturity")]
    _maturity: String,
    #[serde(default)]
    features: Vec<String>,
    verified: bool,
    #[serde(default)]
    negative_cases: Vec<NegativeExpectation>,
}

#[derive(Debug, Deserialize)]
struct NegativeExpectation {
    path: String,
    expected_diagnostic_category: Option<String>,
    #[allow(dead_code)]
    reason: Option<String>,
}

#[derive(Debug)]
struct TestCase {
    suite: String,
    case_id: String,
    name: String,
    source: PathBuf,
    source_sha256: String,
    graph_sha256: Option<String>,
    graph_files: Option<Vec<String>>,
    expected_kind: String,
    expected_value: Option<i32>,
    expected_diagnostic_category: Option<String>,
    expected_stdout_prefix: Option<String>,
    validation_status: String,
    features: Vec<String>,
}

#[derive(Debug, Serialize)]
struct Invocation {
    status: String,
    exit_code: Option<i32>,
    stdout_hex: String,
    stderr_hex: String,
    duration_ms: u128,
    error: Option<String>,
}

#[derive(Debug, Serialize)]
struct Record {
    suite: String,
    case_id: String,
    name: String,
    backend: String,
    source_sha256: String,
    graph_sha256: Option<String>,
    graph_files: Option<Vec<String>>,
    validation_status: String,
    features: Vec<String>,
    expected_diagnostic_category: Option<String>,
    actual_diagnostic_category: Option<String>,
    status: String,
    compile: Option<Invocation>,
    run: Option<Invocation>,
}

#[derive(Debug, Serialize)]
struct Corpus {
    legacy_cases: usize,
    multifile_checks: usize,
    negative_probes: usize,
    total_cases: usize,
}

#[derive(Debug, Serialize)]
struct Report {
    schema_version: u32,
    executed: bool,
    corpus: Corpus,
    cases: usize,
    backends: Vec<String>,
    records: Vec<Record>,
    summary: BTreeMap<String, usize>,
}

fn hex(bytes: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        out.push(H[(b >> 4) as usize] as char);
        out.push(H[(b & 0x0f) as usize] as char);
    }
    out
}

fn ascii_lower_hex(input: &str) -> Option<String> {
    if !input.len().is_multiple_of(2) {
        return None;
    }
    let bytes = input.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len() / 2);
    let nibble = |value: u8| -> Option<u8> {
        match value {
            b'0'..=b'9' => Some(value - b'0'),
            b'a'..=b'f' => Some(value - b'a' + 10),
            b'A'..=b'F' => Some(value - b'A' + 10),
            _ => None,
        }
    };
    for pair in bytes.as_chunks::<2>().0 {
        let high = nibble(pair[0])?;
        let low = nibble(pair[1])?;
        decoded.push((high << 4 | low).to_ascii_lowercase());
    }
    Some(hex(&decoded))
}

type DiagnosticRules = BTreeMap<String, BTreeMap<String, Vec<String>>>;

fn classify_diagnostic(
    rules: &DiagnosticRules,
    backend: &str,
    invocation: &Invocation,
) -> Option<String> {
    let backend_rules = rules.get(backend)?;
    let raw = format!("{}{}", invocation.stdout_hex, invocation.stderr_hex);
    let haystack = ascii_lower_hex(&raw)?;
    for (category, needles) in backend_rules {
        for needle in needles {
            let encoded = hex(needle.to_ascii_lowercase().as_bytes());
            if haystack.contains(&encoded) {
                return Some(category.clone());
            }
        }
    }
    None
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let bytes = fs::read(path).map_err(|e| format!("failed to read source for SHA-256: {e}"))?;
    let digest = Sha256::digest(bytes);
    Ok(format!("{digest:x}"))
}

fn sha256_bytes(bytes: &[u8]) -> Result<String, String> {
    let digest = Sha256::digest(bytes);
    Ok(format!("{digest:x}"))
}

fn safe_file(root: &Path, path_text: &str) -> Result<PathBuf, String> {
    let path = root
        .join(path_text)
        .canonicalize()
        .map_err(|e| format!("{path_text}: {e}"))?;
    if !path.starts_with(root) || !path.is_file() {
        return Err(format!("invalid source path: {path_text}"));
    }
    Ok(path)
}

fn import_target(line: &str) -> Option<String> {
    let line = line.trim();
    if !line.starts_with("import ") {
        return None;
    }
    let marker = " from \"";
    let start = line.find(marker)? + marker.len();
    let rest = &line[start..];
    let end = rest.find('"')?;
    Some(rest[..end].to_string())
}

fn local_imports(path: &Path, root: &Path) -> Result<Vec<PathBuf>, String> {
    let text = fs::read_to_string(path).map_err(|e| e.to_string())?;
    let mut result = Vec::new();
    for target in text.lines().filter_map(import_target) {
        if !target.starts_with('.') {
            continue;
        }
        let mut candidate = path
            .parent()
            .ok_or_else(|| format!("source has no parent: {}", path.display()))?
            .join(&target);
        if candidate.extension().is_none() {
            candidate.set_extension("ores");
        }
        let candidate = candidate
            .canonicalize()
            .map_err(|e| format!("{} imports {target}: {e}", path.display()))?;
        if !candidate.starts_with(root) || !candidate.is_file() {
            return Err(format!(
                "invalid local import from {}: {target}",
                path.display()
            ));
        }
        result.push(candidate);
    }
    Ok(result)
}

fn reachable_graph(entry: &Path, root: &Path) -> Result<Vec<PathBuf>, String> {
    let mut pending = vec![entry.to_path_buf()];
    let mut seen = BTreeSet::new();
    while let Some(current) = pending.pop() {
        if !seen.insert(current.clone()) {
            continue;
        }
        pending.extend(local_imports(&current, root)?);
    }
    Ok(seen.into_iter().collect())
}

fn relative_path(path: &Path, root: &Path) -> Result<String, String> {
    path.strip_prefix(root)
        .map(|p| p.to_string_lossy().replace('\\', "/"))
        .map_err(|e| e.to_string())
}

fn fingerprint_graph(paths: &[PathBuf], root: &Path) -> Result<String, String> {
    let mut payload = Vec::new();
    for path in paths {
        let rel = relative_path(path, root)?;
        payload.extend_from_slice(rel.as_bytes());
        payload.push(0);
        payload.extend_from_slice(sha256_file(path)?.as_bytes());
        payload.push(b'\n');
    }
    sha256_bytes(&payload)
}

fn lowercase_kebab(name: &str) -> bool {
    !name.is_empty()
        && name.split('-').all(|part| {
            !part.is_empty()
                && part
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        })
}

fn load_cases(root: &Path) -> Result<(Vec<TestCase>, Corpus), String> {
    let legacy: LegacyManifest = serde_json::from_str(
        &fs::read_to_string(root.join("manifest.json")).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;

    let mut cases = Vec::new();
    for case in &legacy.cases {
        let source = safe_file(root, &case.path)?;
        cases.push(TestCase {
            suite: "legacy".into(),
            case_id: case.id.clone(),
            name: case.name.clone(),
            source_sha256: sha256_file(&source)?,
            source,
            graph_sha256: None,
            graph_files: None,
            expected_kind: case.expected_kind.clone(),
            expected_value: case.expected_value,
            expected_diagnostic_category: case.expected_diagnostic_category.clone(),
            expected_stdout_prefix: None,
            validation_status: case.validation_status.clone(),
            features: case.features.clone(),
        });
    }

    let checks_manifest: ChecksManifest = serde_json::from_str(
        &fs::read_to_string(root.join("checks/manifest.json")).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    let mut manifest_names = BTreeSet::new();
    let mut negative_probes = 0usize;

    for item in &checks_manifest.checks {
        if !manifest_names.insert(item.name.clone()) {
            return Err(format!("duplicate check name: {}", item.name));
        }
        if !lowercase_kebab(&item.name) {
            return Err(format!(
                "check name must be lowercase kebab-case: {}",
                item.name
            ));
        }
        let expectation_path = safe_file(root, &item.expectation)?;
        let expectation: Expectation = serde_json::from_str(
            &fs::read_to_string(&expectation_path).map_err(|e| e.to_string())?,
        )
        .map_err(|e| format!("{}: {e}", item.expectation))?;
        if expectation.name != item.name {
            return Err(format!("check name mismatch: {}", item.name));
        }

        let check_dir = root
            .join("checks")
            .join(&item.name)
            .canonicalize()
            .map_err(|e| e.to_string())?;
        let entry = safe_file(root, &expectation.entry)?;
        if entry.parent() != Some(check_dir.as_path()) {
            return Err(format!(
                "entry is outside its check directory: {}",
                expectation.entry
            ));
        }

        let direct_imports: BTreeSet<PathBuf> = local_imports(&entry, root)?.into_iter().collect();
        if direct_imports.len() != expectation.imported_files {
            return Err(format!(
                "{}: main imports {} local files, expected {}",
                item.name,
                direct_imports.len(),
                expectation.imported_files
            ));
        }
        let graph = reachable_graph(&entry, root)?;
        if graph.len() < 4 {
            return Err(format!(
                "{}: reachable source graph must contain at least four files",
                item.name
            ));
        }
        let graph_files = graph
            .iter()
            .map(|p| relative_path(p, root))
            .collect::<Result<Vec<_>, _>>()?;
        let graph_sha256 = fingerprint_graph(&graph, root)?;
        cases.push(TestCase {
            suite: "checks".into(),
            case_id: item.name.clone(),
            name: item.name.clone(),
            source_sha256: sha256_file(&entry)?,
            source: entry,
            graph_sha256: Some(graph_sha256),
            graph_files: Some(graph_files),
            expected_kind: "exit".into(),
            expected_value: Some(0),
            expected_diagnostic_category: None,
            expected_stdout_prefix: Some(expectation.stable_stdout_prefix.clone()),
            validation_status: if expectation.verified {
                "validated".into()
            } else {
                "unvalidated".into()
            },
            features: expectation.features.clone(),
        });

        for (index, negative) in expectation.negative_cases.iter().enumerate() {
            let source = safe_file(root, &negative.path)?;
            if source.parent() != Some(check_dir.as_path()) {
                return Err(format!(
                    "negative probe is outside its check directory: {}",
                    negative.path
                ));
            }
            negative_probes += 1;
            let mut negative_features = expectation.features.clone();
            negative_features.push("negative-probe".into());
            cases.push(TestCase {
                suite: "checks".into(),
                case_id: format!("{}/negative-{}", item.name, index + 1),
                name: format!("{}-negative-{}", item.name, index + 1),
                source_sha256: sha256_file(&source)?,
                source,
                graph_sha256: None,
                graph_files: None,
                expected_kind: "compile_reject".into(),
                expected_value: None,
                expected_diagnostic_category: negative.expected_diagnostic_category.clone(),
                expected_stdout_prefix: None,
                validation_status: "unvalidated".into(),
                features: negative_features,
            });
        }
    }

    let mut actual_names = BTreeSet::new();
    for entry in fs::read_dir(root.join("checks")).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if !entry.file_type().map_err(|e| e.to_string())?.is_dir() {
            continue;
        }
        if entry.path().join("expectation.json").is_file() {
            actual_names.insert(entry.file_name().to_string_lossy().to_string());
        }
    }
    if actual_names != manifest_names {
        return Err(
            "checks/manifest.json does not exactly enumerate expectation-bearing check directories"
                .into(),
        );
    }

    let corpus = Corpus {
        legacy_cases: legacy.cases.len(),
        multifile_checks: checks_manifest.checks.len(),
        negative_probes,
        total_cases: cases.len(),
    };
    Ok((cases, corpus))
}

fn expand(
    template: &str,
    source: &Path,
    artifact: &Path,
    workdir: &Path,
) -> Result<Vec<String>, String> {
    let parts = shell_words::split(template).map_err(|e| e.to_string())?;
    Ok(parts
        .into_iter()
        .map(|p| {
            p.replace("{source}", &source.to_string_lossy())
                .replace("{artifact}", &artifact.to_string_lossy())
                .replace("{workdir}", &workdir.to_string_lossy())
        })
        .collect())
}

// Canonical Atto actor-critical cases are opt-in, never mixed into the
// historical .ores baseline. Source pins are authenticated before a compiler
// adapter can run; authored source alone never upgrades a guest to PASS.
fn load_actor_critical_cases(root: &Path) -> Result<(Vec<TestCase>, Corpus), String> {
    let registry: serde_json::Value = serde_json::from_slice(
        &fs::read(root.join("contracts/atto-actor-critical-v1/all-eleven-backend-admission.json"))
            .map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    if registry["contract_id"] != "atto-actor-critical-all-eleven-v1"
        || registry["status"] != "AUTHORED_NOT_EXECUTED"
        || registry["canonical_contract"]["id"] != "atto-actor-critical-v1"
    {
        return Err("unrecognized or promoted actor-critical admission registry".into());
    }

    let definitions = [
        (
            "two-actor-snapshot-positive",
            "contracts/atto-actor-critical-v1/fixtures/two-actor-snapshot-positive.atto",
            "a6189d92c813426ecf54a09cf535c3874de45942",
            "exit",
            None,
        ),
        (
            "deny-send-inside-read",
            "contracts/atto-actor-critical-v1/fixtures/deny-send-inside-read.atto",
            "89eb278a139eb13079e8eda69a943f94591671cc",
            "compile_reject",
            Some("actor_state_loan_escape_to_mailbox"),
        ),
        (
            "deny-write-inside-read",
            "contracts/atto-actor-critical-v1/fixtures/deny-write-inside-read.atto",
            "b279055abc2ee5175299a74fe5b66662581a87cf",
            "compile_reject",
            Some("write_through_read_only_actor_loan"),
        ),
    ];
    let negative_manifest = registry["canonical_contract"]["negative_cases"]
        .as_array()
        .ok_or("missing actor-critical negative manifest")?;
    if negative_manifest.len() != 2
        || registry["canonical_contract"]["positive_fixture"] != definitions[0].1
        || registry["canonical_contract"]["positive_git_blob_sha"] != definitions[0].2
    {
        return Err("canonical actor-critical source identity mismatch".into());
    }
    let canonical: serde_json::Value = serde_json::from_slice(
        &fs::read(root.join("contracts/atto-actor-critical-v1/contract.json"))
            .map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    if canonical["status"] != "AUTHORED_NOT_EXECUTED"
        || canonical["positive"]["expected_stdout_line"] != "ATTO_TWO_ACTOR_LOGICAL_ISOLATION=PASS"
    {
        return Err("canonical actor-critical guest expectations changed".into());
    }

    let mut cases = Vec::new();
    for (index, (id, path, pin, kind, category)) in definitions.into_iter().enumerate() {
        if index > 0 {
            let entry = &negative_manifest[index - 1];
            if entry["id"] != id
                || entry["path"] != path
                || entry["git_blob_sha"] != pin
                || entry["expected_category"] != category.expect("negative category")
            {
                return Err(format!("actor-critical negative manifest drift: {id}"));
            }
        }
        let source = safe_file(root, path)?;
        if fs::symlink_metadata(root.join(path))
            .map_err(|e| e.to_string())?
            .file_type()
            .is_symlink()
        {
            return Err(format!(
                "actor-critical source must be a regular file: {path}"
            ));
        }
        let output = Command::new("git")
            .arg("hash-object")
            .arg("--no-filters")
            .arg(&source)
            .output()
            .map_err(|e| format!("git hash-object failed: {e}"))?;
        if !output.status.success()
            || String::from_utf8(output.stdout)
                .map_err(|e| e.to_string())?
                .trim()
                != pin
        {
            return Err(format!("actor-critical fixture blob differs: {path}"));
        }
        cases.push(TestCase {
            suite: "atto-actor-critical-v1".into(),
            case_id: id.into(),
            name: id.into(),
            source_sha256: sha256_file(&source)?,
            source,
            graph_sha256: None,
            graph_files: None,
            expected_kind: kind.into(),
            expected_value: if index == 0 { Some(0) } else { None },
            expected_diagnostic_category: category.map(str::to_owned),
            expected_stdout_prefix: if index == 0 {
                Some("ATTO_TWO_ACTOR_LOGICAL_ISOLATION=PASS".into())
            } else {
                None
            },
            // No backend has a certified compiler+guest receipt yet. Even
            // explicit --execute --allow-unvalidated is exploratory only.
            validation_status: "unvalidated".into(),
            features: vec![
                "actor".into(),
                "sealed-state".into(),
                "critical-loan".into(),
            ],
        });
    }
    Ok((
        cases,
        Corpus {
            legacy_cases: 0,
            multifile_checks: 0,
            negative_probes: 2,
            total_cases: 3,
        },
    ))
}

// Source compilers and guest programs are not trusted to emit bounded output.
// Drain to EOF even after the capture limit, so a full pipe cannot block a
// child, but never promote truncated diagnostics to parity success.
const MAX_CAPTURE_BYTES: usize = 1024 * 1024;

struct BoundedCapture {
    bytes: Vec<u8>,
    exceeded: bool,
}

fn read_bounded(mut pipe: impl Read) -> std::io::Result<BoundedCapture> {
    let mut bytes = Vec::new();
    let mut exceeded = false;
    let mut chunk = [0u8; 8192];
    loop {
        let count = match pipe.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => n,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        };
        let remaining = MAX_CAPTURE_BYTES.saturating_sub(bytes.len());
        let accepted = count.min(remaining);
        bytes.extend_from_slice(&chunk[..accepted]);
        if count > accepted {
            exceeded = true;
        }
    }
    Ok(BoundedCapture { bytes, exceeded })
}

fn invoke(argv: &[String], workdir: &Path, timeout: Duration) -> Invocation {
    if argv.is_empty() {
        return Invocation {
            status: "infrastructure_error".into(),
            exit_code: None,
            stdout_hex: String::new(),
            stderr_hex: String::new(),
            duration_ms: 0,
            error: Some("empty command".into()),
        };
    }
    let start = Instant::now();
    let mut command = Command::new(&argv[0]);
    command
        .args(&argv[1..])
        .current_dir(workdir)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => {
            return Invocation {
                status: "infrastructure_error".into(),
                exit_code: None,
                stdout_hex: String::new(),
                stderr_hex: String::new(),
                duration_ms: start.elapsed().as_millis(),
                error: Some(error.to_string()),
            }
        }
    };
    // Independently drain both pipes with fixed storage while the child runs.
    let stdout = child.stdout.take().expect("piped stdout");
    let stderr = child.stderr.take().expect("piped stderr");
    let stdout_reader = std::thread::spawn(move || read_bounded(stdout));
    let stderr_reader = std::thread::spawn(move || read_bounded(stderr));
    let mut timed_out = false;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Ok(status),
            Ok(None) if start.elapsed() >= timeout => {
                timed_out = true;
                terminate(&mut child);
                break child.wait();
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
            Err(error) => {
                terminate(&mut child);
                let _ = child.wait();
                break Err(error);
            }
        }
    };
    let stdout = stdout_reader.join().expect("stdout reader panicked");
    let stderr = stderr_reader.join().expect("stderr reader panicked");
    match (status, stdout, stderr) {
        (Ok(_), Ok(stdout), Ok(stderr)) if stdout.exceeded || stderr.exceeded => Invocation {
            status: "output_limit_exceeded".into(),
            exit_code: None,
            stdout_hex: hex(&stdout.bytes),
            stderr_hex: hex(&stderr.bytes),
            duration_ms: start.elapsed().as_millis(),
            error: Some(format!(
                "compiler or guest output exceeded {} bytes per stream; truncated output cannot certify parity",
                MAX_CAPTURE_BYTES
            )),
        },
        (Ok(status), Ok(stdout), Ok(stderr)) => Invocation {
            status: if timed_out { "timeout" } else { "completed" }.into(),
            exit_code: if timed_out { None } else { status.code() },
            stdout_hex: hex(&stdout.bytes),
            stderr_hex: hex(&stderr.bytes),
            duration_ms: start.elapsed().as_millis(),
            error: None,
        },
        (Err(error), _, _) | (_, Err(error), _) | (_, _, Err(error)) => Invocation {
            status: "infrastructure_error".into(),
            exit_code: None,
            stdout_hex: String::new(),
            stderr_hex: String::new(),
            duration_ms: start.elapsed().as_millis(),
            error: Some(error.to_string()),
        },
    }
}

fn terminate(child: &mut std::process::Child) {
    // The adapter launches Java as a child. Kill its isolated process group so
    // a timeout cannot leave Java running or keep the capture pipes open.
    #[cfg(unix)]
    unsafe {
        libc::killpg(child.id() as libc::pid_t, libc::SIGKILL);
    }
    let _ = child.kill();
}

fn safe_temp_component(value: &str) -> String {
    value
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect()
}

fn temp_dir(case_id: &str, backend: &str) -> Result<PathBuf, String> {
    let path = env::temp_dir().join(format!(
        "ores-parity-{}-{}-{}",
        std::process::id(),
        safe_temp_component(case_id),
        safe_temp_component(backend)
    ));
    if path.exists() {
        fs::remove_dir_all(&path).map_err(|e| e.to_string())?;
    }
    fs::create_dir_all(&path).map_err(|e| e.to_string())?;
    Ok(path)
}

fn evaluate_case(
    case: &TestCase,
    diagnostic_rules: &DiagnosticRules,
    backend: &str,
    env_name: &str,
    execute: bool,
    allow_unvalidated: bool,
    timeout: Duration,
) -> Result<Record, String> {
    let mut record = Record {
        suite: case.suite.clone(),
        case_id: case.case_id.clone(),
        name: case.name.clone(),
        backend: backend.into(),
        source_sha256: case.source_sha256.clone(),
        graph_sha256: case.graph_sha256.clone(),
        graph_files: case.graph_files.clone(),
        validation_status: case.validation_status.clone(),
        features: case.features.clone(),
        expected_diagnostic_category: case.expected_diagnostic_category.clone(),
        actual_diagnostic_category: None,
        status: "not_run".into(),
        compile: None,
        run: None,
    };
    if !execute {
        return Ok(record);
    }
    if case.validation_status != "validated" && !allow_unvalidated {
        record.status = "unvalidated".into();
        return Ok(record);
    }

    let prefix = format!("ORES_PARITY_{env_name}");
    let compile_template = env::var(format!("{prefix}_COMPILE")).ok();
    let run_template = env::var(format!("{prefix}_RUN")).ok();
    if compile_template.is_none() || (case.expected_kind == "exit" && run_template.is_none()) {
        record.status = "backend_not_configured".into();
        return Ok(record);
    }

    let workdir = temp_dir(&case.case_id, backend)?;
    let artifact = workdir.join("program");
    let compile_argv = expand(
        compile_template.as_ref().expect("checked above"),
        &case.source,
        &artifact,
        &workdir,
    )?;
    let compile = invoke(&compile_argv, &workdir, timeout);
    let compile_status = compile.status.clone();
    let compile_exit = compile.exit_code;
    record.compile = Some(compile);

    if compile_status != "completed" || compile_exit.is_none() || compile_exit == Some(125) {
        record.status = "compile_infrastructure_failure".into();
    } else if case.expected_kind == "compile_reject" {
        if compile_exit == Some(0) {
            record.status = "unexpected_compile_success".into();
        } else {
            record.actual_diagnostic_category = record
                .compile
                .as_ref()
                .and_then(|c| classify_diagnostic(diagnostic_rules, backend, c));
            record.status = match (
                case.expected_diagnostic_category.as_deref(),
                record.actual_diagnostic_category.as_deref(),
            ) {
                (Some(expected), Some(actual)) if expected == actual => {
                    if case.validation_status == "validated" {
                        "expected_reject".into()
                    } else {
                        "exploratory_expected_reject_not_parity_pass".into()
                    }
                }
                (_, Some(_)) => "wrong_diagnostic".into(),
                _ => "rejection_unclassified".into(),
            };
        }
    } else if compile_exit != Some(0) {
        record.actual_diagnostic_category = record
            .compile
            .as_ref()
            .and_then(|c| classify_diagnostic(diagnostic_rules, backend, c));
        let select_case = case.features.iter().any(|feature| {
            matches!(
                feature.as_str(),
                "select" | "do-select" | "nb-select" | "do-nb-select"
            )
        });
        record.status = if select_case
            && record.actual_diagnostic_category.as_deref() == Some("unsupported_select")
        {
            "intentional_capability_reject".into()
        } else {
            "compile_failure".into()
        };
    } else {
        let run_argv = expand(
            run_template.as_ref().expect("checked above"),
            &case.source,
            &artifact,
            &workdir,
        )?;
        let run = invoke(&run_argv, &workdir, timeout);
        let run_status = run.status.clone();
        let run_exit = run.exit_code;
        let stdout_hex = run.stdout_hex.clone();
        record.run = Some(run);
        record.status = if run_status != "completed" || run_exit.is_none() || run_exit == Some(125)
        {
            "runtime_infrastructure_failure".into()
        } else if run_exit != case.expected_value {
            "wrong_result".into()
        } else if let Some(prefix) = &case.expected_stdout_prefix {
            if !stdout_hex.starts_with(&hex(prefix.as_bytes())) {
                "wrong_stdout".into()
            } else if case.validation_status == "validated" {
                "pass".into()
            } else {
                "exploratory_match_not_parity_pass".into()
            }
        } else if case.validation_status == "validated" {
            "pass".into()
        } else {
            "exploratory_match_not_parity_pass".into()
        };
    }
    let _ = fs::remove_dir_all(workdir);
    Ok(record)
}

fn is_verified_execution_status(status: &str) -> bool {
    matches!(status, "pass" | "expected_reject")
}

/// Six core backends are mandatory by default; opt-in profiles are explicit.
fn is_backend_selected(backend: &str, selected: Option<&str>, all_backends: bool) -> bool {
    match selected {
        Some(name) => name == backend,
        None => all_backends || REQUIRED_BACKENDS.contains(&backend),
    }
}

fn main() -> Result<(), String> {
    let args: Vec<String> = env::args().collect();
    let execute = args.iter().any(|arg| arg == "--execute");
    let allow_unvalidated = args.iter().any(|arg| arg == "--allow-unvalidated");
    let all_backends = args.iter().any(|arg| arg == "--all-backends");
    let actor_critical = args.iter().any(|arg| arg == "--actor-critical");
    let selected_backend = args
        .iter()
        .position(|arg| arg == "--backend")
        .map(|index| {
            args.get(index + 1)
                .filter(|name| BACKENDS.iter().any(|(backend, _)| backend == name))
                .cloned()
                .ok_or_else(|| {
                    "--backend requires llvm, java, wasm, dotnet, forge, cranelift, js, rs, hdl, realtime or atto"
                        .to_string()
                })
        })
        .transpose()?;
    if all_backends && selected_backend.is_some() {
        return Err("--all-backends conflicts with --backend".into());
    }
    let output = args
        .windows(2)
        .find(|window| window[0] == "--output")
        .map(|window| window[1].clone())
        .unwrap_or_else(|| "parity-results.json".into());
    let timeout_seconds: u64 = args
        .windows(2)
        .find(|window| window[0] == "--timeout")
        .map(|window| window[1].parse::<u64>().map_err(|e| e.to_string()))
        .transpose()?
        .unwrap_or(30);
    if timeout_seconds == 0 {
        return Err("--timeout must be positive".into());
    }

    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()
        .map_err(|e| e.to_string())?;
    let (cases, corpus) = if actor_critical {
        load_actor_critical_cases(&root)?
    } else {
        load_cases(&root)?
    };
    let diagnostic_rules: DiagnosticRules = serde_json::from_str(
        &fs::read_to_string(root.join("diagnostics.json")).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    let timeout = Duration::from_secs(timeout_seconds);
    let mut records = Vec::new();
    for case in &cases {
        for &(backend, env_name) in &BACKENDS {
            if !is_backend_selected(
                backend,
                selected_backend.as_deref(),
                all_backends || actor_critical,
            ) {
                continue;
            }
            records.push(evaluate_case(
                case,
                &diagnostic_rules,
                backend,
                env_name,
                execute,
                allow_unvalidated,
                timeout,
            )?);
        }
    }

    let mut summary = BTreeMap::new();
    for record in &records {
        *summary.entry(record.status.clone()).or_insert(0) += 1;
    }
    let report = Report {
        schema_version: 2,
        executed: execute,
        cases: cases.len(),
        corpus,
        backends: BACKENDS
            .iter()
            .filter(|entry| {
                is_backend_selected(
                    entry.0,
                    selected_backend.as_deref(),
                    all_backends || actor_critical,
                )
            })
            .map(|entry| entry.0.to_string())
            .collect(),
        records,
        summary,
    };
    fs::write(
        &output,
        serde_json::to_vec_pretty(&report).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    println!(
        "{}",
        serde_json::to_string(&report.summary).map_err(|e| e.to_string())?
    );

    // Inventory-only audits are intentionally not execution claims. When
    // execution is requested, only validated passes may exit successfully.
    // Unrecognized or exploratory statuses must fail closed.
    let hard_fail = execute
        && report
            .records
            .iter()
            .any(|record| !is_verified_execution_status(&record.status));
    if hard_fail {
        std::process::exit(1);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        ascii_lower_hex, classify_diagnostic, hex, import_target, is_backend_selected,
        is_verified_execution_status, lowercase_kebab, Invocation, BACKENDS, REQUIRED_BACKENDS,
    };
    use std::collections::BTreeMap;

    #[test]
    fn full_eleven_compiler_roster_requires_explicit_opt_in() {
        let unique: std::collections::BTreeSet<&str> =
            BACKENDS.iter().map(|(name, _)| *name).collect();
        assert_eq!(unique.len(), 11);
        assert_eq!(REQUIRED_BACKENDS.len(), 6);
        assert_eq!(
            BACKENDS
                .iter()
                .filter(|(name, _)| is_backend_selected(name, None, false))
                .count(),
            6
        );
        assert_eq!(
            BACKENDS
                .iter()
                .filter(|(name, _)| is_backend_selected(name, None, true))
                .count(),
            11
        );
        for backend in ["js", "rs", "hdl", "realtime", "atto"] {
            assert!(!is_backend_selected(backend, None, false));
            assert!(is_backend_selected(backend, None, true));
            assert!(is_backend_selected(backend, Some(backend), false));
            assert!(!is_backend_selected(backend, Some("java"), true));
        }
    }

    #[test]
    fn exploratory_and_unrecognized_execution_results_fail_closed() {
        assert!(is_verified_execution_status("pass"));
        assert!(is_verified_execution_status("expected_reject"));
        for status in [
            "not_run",
            "unvalidated",
            "exploratory_match_not_parity_pass",
            "exploratory_expected_reject_not_parity_pass",
            "intentional_capability_reject",
            "backend_not_configured",
            "compile_infrastructure_failure",
            "runtime_infrastructure_failure",
            "unknown_future_status",
        ] {
            assert!(!is_verified_execution_status(status), "{status}");
        }
    }

    #[test]
    fn diagnostic_classification_is_ascii_case_insensitive() {
        let mut backend = BTreeMap::new();
        backend.insert(
            "unsupported_select".to_string(),
            vec!["static select requires checked channel/future arbitration".to_string()],
        );
        let mut rules = BTreeMap::new();
        rules.insert("dotnet".to_string(), backend);
        let invocation = Invocation {
            status: "completed".into(),
            exit_code: Some(1),
            stdout_hex: String::new(),
            stderr_hex: hex(b"Static select requires checked Channel/Future arbitration in .NET"),
            duration_ms: 1,
            error: None,
        };
        assert_eq!(
            classify_diagnostic(&rules, "dotnet", &invocation).as_deref(),
            Some("unsupported_select")
        );
    }

    #[test]
    fn ascii_lower_hex_rejects_malformed_capture() {
        assert!(ascii_lower_hex("0").is_none());
        assert!(ascii_lower_hex("zz").is_none());
    }

    #[test]
    fn parses_relative_import_target() {
        assert_eq!(
            import_target(r#"import fnc value from "./dep.ores";"#),
            Some("./dep.ores".into())
        );
        assert_eq!(import_target("pub fnc main(): void { return; }"), None);
    }

    #[test]
    fn enforces_lowercase_kebab_names() {
        assert!(lowercase_kebab("actor-system"));
        assert!(lowercase_kebab("nb-match"));
        assert!(!lowercase_kebab("Actor-System"));
        assert!(!lowercase_kebab("actor_system"));
        assert!(!lowercase_kebab("actor--system"));
    }

    #[test]
    fn hex_encoding_is_stable() {
        assert_eq!(hex(b"PARITY_OK"), "5041524954595f4f4b");
    }

    #[cfg(unix)]
    #[test]
    fn drains_output_larger_than_pipe_buffers() {
        let result = super::invoke(
            &[
                "sh".into(),
                "-c".into(),
                "head -c 131072 /dev/zero; head -c 131072 /dev/zero >&2".into(),
            ],
            &std::env::temp_dir(),
            std::time::Duration::from_secs(5),
        );
        assert_eq!(result.status, "completed");
        assert_eq!(result.exit_code, Some(0));
        assert_eq!(result.stdout_hex.len(), 262144);
        assert_eq!(result.stderr_hex.len(), 262144);
    }

    #[test]
    fn bounded_capture_drains_and_rejects_oversized_bytes() {
        let input = vec![b'x'; super::MAX_CAPTURE_BYTES + 8193];
        let capture = super::read_bounded(std::io::Cursor::new(input)).unwrap();
        assert_eq!(capture.bytes.len(), super::MAX_CAPTURE_BYTES);
        assert!(capture.exceeded);
        let exact_input = vec![b'y'; super::MAX_CAPTURE_BYTES];
        let exact = super::read_bounded(std::io::Cursor::new(exact_input)).unwrap();
        assert!(!exact.exceeded);
        assert_eq!(exact.bytes.len(), super::MAX_CAPTURE_BYTES);
    }

    #[cfg(unix)]
    #[test]
    fn oversized_compiler_stdout_and_stderr_never_certify_parity() {
        for command in ["head -c 1048577 /dev/zero", "head -c 1048577 /dev/zero >&2"] {
            let result = super::invoke(
                &["sh".into(), "-c".into(), command.into()],
                &std::env::temp_dir(),
                std::time::Duration::from_secs(5),
            );
            assert_eq!(result.status, "output_limit_exceeded");
            assert_eq!(result.exit_code, None);
            assert!(result
                .error
                .as_deref()
                .unwrap()
                .contains("cannot certify parity"));
            assert!(result.stdout_hex.len() <= 2 * super::MAX_CAPTURE_BYTES);
            assert!(result.stderr_hex.len() <= 2 * super::MAX_CAPTURE_BYTES);
        }
    }

    #[cfg(unix)]
    #[test]
    fn timeout_terminates_launcher_children_and_retains_diagnostics() {
        let start = std::time::Instant::now();
        let result = super::invoke(
            &[
                "sh".into(),
                "-c".into(),
                "echo before-timeout >&2; sleep 10".into(),
            ],
            &std::env::temp_dir(),
            std::time::Duration::from_millis(100),
        );
        assert_eq!(result.status, "timeout");
        assert!(result.stderr_hex.contains(&hex(b"before-timeout")));
        assert!(start.elapsed() < std::time::Duration::from_secs(3));
    }
}
