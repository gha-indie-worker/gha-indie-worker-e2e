use std::{
    collections::{BTreeMap, BTreeSet},
    env,
    fmt,
    fs,
    io::Write,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

const MAX_API_JSON_BYTES: usize = 4 * 1024 * 1024;
const MAX_PAGES: usize = 10;

#[derive(Clone, Debug, PartialEq)]
enum Json {
    Null,
    Bool(bool),
    Number(String),
    String(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

struct Parser<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Parser<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, at: 0 }
    }

    fn parse(mut self) -> Result<Json, VerifyError> {
        self.ws();
        let value = self.value()?;
        self.ws();
        if self.at != self.bytes.len() {
            return denied("malformed GitHub API JSON response");
        }
        Ok(value)
    }

    fn ws(&mut self) {
        while self.at < self.bytes.len() && self.bytes[self.at].is_ascii_whitespace() {
            self.at += 1;
        }
    }

    fn value(&mut self) -> Result<Json, VerifyError> {
        self.ws();
        match self.bytes.get(self.at).copied() {
            Some(b'n') => {
                self.literal(b"null")?;
                Ok(Json::Null)
            }
            Some(b't') => {
                self.literal(b"true")?;
                Ok(Json::Bool(true))
            }
            Some(b'f') => {
                self.literal(b"false")?;
                Ok(Json::Bool(false))
            }
            Some(b'"') => Ok(Json::String(self.string()?)),
            Some(b'[') => self.array(),
            Some(b'{') => self.object(),
            Some(b'-' | b'0'..=b'9') => self.number(),
            _ => denied("malformed GitHub API JSON response"),
        }
    }

    fn literal(&mut self, expected: &[u8]) -> Result<(), VerifyError> {
        if self.bytes.get(self.at..self.at + expected.len()) == Some(expected) {
            self.at += expected.len();
            Ok(())
        } else {
            denied("malformed GitHub API JSON response")
        }
    }

    fn string(&mut self) -> Result<String, VerifyError> {
        if self.bytes.get(self.at) != Some(&b'"') {
            return denied("malformed GitHub API JSON response");
        }
        self.at += 1;
        let mut out = String::new();
        while self.at < self.bytes.len() {
            let byte = self.bytes[self.at];
            self.at += 1;
            match byte {
                b'"' => return Ok(out),
                b'\\' => {
                    let esc = *self
                        .bytes
                        .get(self.at)
                        .ok_or_else(|| VerifyError::Denied("malformed GitHub API JSON response".into()))?;
                    self.at += 1;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{0008}'),
                        b'f' => out.push('\u{000c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let high = self.hex4()?;
                            let scalar = if (0xD800..=0xDBFF).contains(&high) {
                                if self.bytes.get(self.at..self.at + 2) != Some(b"\\u") {
                                    return denied("malformed GitHub API JSON response");
                                }
                                self.at += 2;
                                let low = self.hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&low) {
                                    return denied("malformed GitHub API JSON response");
                                }
                                0x10000
                                    + (((high - 0xD800) as u32) << 10)
                                    + (low - 0xDC00) as u32
                            } else if (0xDC00..=0xDFFF).contains(&high) {
                                return denied("malformed GitHub API JSON response");
                            } else {
                                high as u32
                            };
                            out.push(
                                char::from_u32(scalar)
                                    .ok_or_else(|| VerifyError::Denied(
                                        "malformed GitHub API JSON response".into(),
                                    ))?,
                            );
                        }
                        _ => return denied("malformed GitHub API JSON response"),
                    }
                }
                0x00..=0x1f => return denied("malformed GitHub API JSON response"),
                0x20..=0x7f => out.push(byte as char),
                _ => {
                    let start = self.at - 1;
                    let width = match byte {
                        0xC2..=0xDF => 2,
                        0xE0..=0xEF => 3,
                        0xF0..=0xF4 => 4,
                        _ => return denied("malformed GitHub API JSON response"),
                    };
                    let end = start + width;
                    let slice = self
                        .bytes
                        .get(start..end)
                        .ok_or_else(|| VerifyError::Denied(
                            "malformed GitHub API JSON response".into(),
                        ))?;
                    out.push_str(
                        std::str::from_utf8(slice)
                            .map_err(|_| VerifyError::Denied(
                                "malformed GitHub API JSON response".into(),
                            ))?,
                    );
                    self.at = end;
                }
            }
        }
        denied("malformed GitHub API JSON response")
    }

    fn hex4(&mut self) -> Result<u16, VerifyError> {
        let mut value = 0u16;
        for _ in 0..4 {
            let byte = *self
                .bytes
                .get(self.at)
                .ok_or_else(|| VerifyError::Denied("malformed GitHub API JSON response".into()))?;
            self.at += 1;
            value = value
                .checked_mul(16)
                .ok_or_else(|| VerifyError::Denied("malformed GitHub API JSON response".into()))?
                + match byte {
                    b'0'..=b'9' => (byte - b'0') as u16,
                    b'a'..=b'f' => (byte - b'a' + 10) as u16,
                    b'A'..=b'F' => (byte - b'A' + 10) as u16,
                    _ => return denied("malformed GitHub API JSON response"),
                };
        }
        Ok(value)
    }

    fn array(&mut self) -> Result<Json, VerifyError> {
        self.at += 1;
        self.ws();
        let mut values = Vec::new();
        if self.bytes.get(self.at) == Some(&b']') {
            self.at += 1;
            return Ok(Json::Array(values));
        }
        loop {
            values.push(self.value()?);
            self.ws();
            match self.bytes.get(self.at) {
                Some(b',') => {
                    self.at += 1;
                    self.ws();
                }
                Some(b']') => {
                    self.at += 1;
                    return Ok(Json::Array(values));
                }
                _ => return denied("malformed GitHub API JSON response"),
            }
        }
    }

    fn object(&mut self) -> Result<Json, VerifyError> {
        self.at += 1;
        self.ws();
        let mut fields = Vec::new();
        if self.bytes.get(self.at) == Some(&b'}') {
            self.at += 1;
            return Ok(Json::Object(fields));
        }
        loop {
            let key = self.string()?;
            if fields.iter().any(|(existing, _)| existing == &key) {
                return denied("GitHub API JSON has duplicate object keys");
            }
            self.ws();
            if self.bytes.get(self.at) != Some(&b':') {
                return denied("malformed GitHub API JSON response");
            }
            self.at += 1;
            let value = self.value()?;
            fields.push((key, value));
            self.ws();
            match self.bytes.get(self.at) {
                Some(b',') => {
                    self.at += 1;
                    self.ws();
                }
                Some(b'}') => {
                    self.at += 1;
                    return Ok(Json::Object(fields));
                }
                _ => return denied("malformed GitHub API JSON response"),
            }
        }
    }

    fn number(&mut self) -> Result<Json, VerifyError> {
        let start = self.at;
        if self.bytes.get(self.at) == Some(&b'-') {
            self.at += 1;
        }
        match self.bytes.get(self.at) {
            Some(b'0') => {
                self.at += 1;
                if matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                    return denied("malformed GitHub API JSON response");
                }
            }
            Some(b'1'..=b'9') => {
                self.at += 1;
                while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                    self.at += 1;
                }
            }
            _ => return denied("malformed GitHub API JSON response"),
        }
        let mut floating = false;
        if self.bytes.get(self.at) == Some(&b'.') {
            floating = true;
            self.at += 1;
            let before = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                self.at += 1;
            }
            if self.at == before {
                return denied("malformed GitHub API JSON response");
            }
        }
        if matches!(self.bytes.get(self.at), Some(b'e' | b'E')) {
            floating = true;
            self.at += 1;
            if matches!(self.bytes.get(self.at), Some(b'+' | b'-')) {
                self.at += 1;
            }
            let before = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                self.at += 1;
            }
            if self.at == before {
                return denied("malformed GitHub API JSON response");
            }
        }
        let raw = std::str::from_utf8(&self.bytes[start..self.at])
            .map_err(|_| VerifyError::Denied("malformed GitHub API JSON response".into()))?;
        if (!floating && raw.len() > 64) || (floating && raw.len() > 128) {
            return denied("GitHub API JSON numeric token is oversized");
        }
        if floating {
            let parsed: f64 = raw
                .parse()
                .map_err(|_| VerifyError::Denied("malformed GitHub API JSON response".into()))?;
            if !parsed.is_finite() {
                return denied("GitHub API JSON contains a nonfinite number");
            }
        }
        Ok(Json::Number(raw.to_string()))
    }
}

#[derive(Clone, Debug)]
enum VerifyError {
    Denied(String),
    NotExecuted(String),
}

impl fmt::Display for VerifyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Denied(message) | Self::NotExecuted(message) => f.write_str(message),
        }
    }
}

fn denied<T>(message: impl Into<String>) -> Result<T, VerifyError> {
    Err(VerifyError::Denied(message.into()))
}

fn strict_api_json(raw: &[u8]) -> Result<Json, VerifyError> {
    if raw.len() > MAX_API_JSON_BYTES {
        return denied("GitHub API JSON response is missing or oversized");
    }
    std::str::from_utf8(raw)
        .map_err(|_| VerifyError::Denied("malformed GitHub API JSON response".into()))?;
    let value = Parser::new(raw).parse()?;
    match value {
        Json::Object(_) => Ok(value),
        _ => denied("GitHub API response is not a JSON object"),
    }
}

fn object(value: &Json) -> Result<&[(String, Json)], VerifyError> {
    match value {
        Json::Object(fields) => Ok(fields),
        _ => denied("value is not a JSON object"),
    }
}

fn object_mut(value: &mut Json) -> Result<&mut Vec<(String, Json)>, VerifyError> {
    match value {
        Json::Object(fields) => Ok(fields),
        _ => denied("value is not a JSON object"),
    }
}

fn array(value: &Json) -> Result<&[Json], VerifyError> {
    match value {
        Json::Array(values) => Ok(values),
        _ => denied("value is not a JSON array"),
    }
}

fn array_mut(value: &mut Json) -> Result<&mut Vec<Json>, VerifyError> {
    match value {
        Json::Array(values) => Ok(values),
        _ => denied("value is not a JSON array"),
    }
}

fn field<'a>(value: &'a Json, key: &str) -> Result<&'a Json, VerifyError> {
    object(value)?
        .iter()
        .find(|(name, _)| name == key)
        .map(|(_, value)| value)
        .ok_or_else(|| VerifyError::Denied(format!("missing JSON field: {key}")))
}

fn field_mut<'a>(value: &'a mut Json, key: &str) -> Result<&'a mut Json, VerifyError> {
    object_mut(value)?
        .iter_mut()
        .find(|(name, _)| name == key)
        .map(|(_, value)| value)
        .ok_or_else(|| VerifyError::Denied(format!("missing JSON field: {key}")))
}

fn string(value: &Json) -> Result<&str, VerifyError> {
    match value {
        Json::String(value) => Ok(value),
        _ => denied("expected JSON string"),
    }
}

fn positive_int(value: &Json, label: &str) -> Result<u64, VerifyError> {
    let Json::Number(raw) = value else {
        return denied(format!("{label} is not a positive integer"));
    };
    if raw.contains(['.', 'e', 'E']) || raw.starts_with('-') {
        return denied(format!("{label} is not a positive integer"));
    }
    let parsed: u64 = raw
        .parse()
        .map_err(|_| VerifyError::Denied(format!("{label} is not a positive integer")))?;
    if parsed == 0 {
        return denied(format!("{label} is not a positive integer"));
    }
    Ok(parsed)
}

fn boolean(value: &Json, label: &str) -> Result<bool, VerifyError> {
    match value {
        Json::Bool(value) => Ok(*value),
        _ => denied(format!("{label} is not a boolean")),
    }
}

fn sha(value: &str) -> Result<&str, VerifyError> {
    if value.len() != 40
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return denied("expected a lowercase 40-digit commit/tree SHA");
    }
    Ok(value)
}

fn repo_for(backend: &str) -> Option<(&'static str, u64)> {
    match backend {
        "java" => Some(("ores-truffle-oreslang/oreslang-source.java", 1_400_792_274)),
        "llvm" => Some(("ores-truffle-oreslang/oreslang-source.llvm", 1_410_897_874)),
        "net" => Some(("ores-truffle-oreslang/oreslang-source.net", 1_419_704_375)),
        "wasm" => Some(("ores-truffle-oreslang/oreslang-source.wasm", 1_419_858_954)),
        "forge" => Some(("ores-truffle-oreslang/oreslang-source.forge", 1_419_703_674)),
        "rx_pull" => Some(("ores-truffle-oreslang/rx-ores", 1_407_870_560)),
        "rx_callbacks" => Some(("ores-truffle-oreslang/rx-ores-callbacks", 1_407_871_059)),
        "rx_channels" => Some(("ores-truffle-oreslang/rx-oreslang-channels", 1_407_757_001)),
        _ => None,
    }
}

fn forbidden_evidence_char(ch: char) -> bool {
    let cp = ch as u32;
    ch.is_control()
        || matches!(
            cp,
            0x00AD
                | 0x061C
                | 0x06DD
                | 0x070F
                | 0x08E2
                | 0x180E
                | 0x2028
                | 0x2029
                | 0x2060
                | 0xFEFF
        )
        || (0x0600..=0x0605).contains(&cp)
        || (0x0890..=0x0891).contains(&cp)
        || (0x200B..=0x200F).contains(&cp)
        || (0x202A..=0x202E).contains(&cp)
        || (0x2061..=0x2064).contains(&cp)
        || (0x2066..=0x206F).contains(&cp)
        || (0xFFF9..=0xFFFB).contains(&cp)
        || cp == 0x110BD
        || cp == 0x110CD
        || (0x13430..=0x1343F).contains(&cp)
        || (0x1BCA0..=0x1BCA3).contains(&cp)
        || (0x1D173..=0x1D17A).contains(&cp)
        || cp == 0xE0001
        || (0xE0020..=0xE007F).contains(&cp)
}

fn evidence_label(value: &str, limit: usize) -> bool {
    !value.trim().is_empty()
        && value.chars().count() <= limit
        && !value.chars().any(forbidden_evidence_char)
}

fn executable_step_name(name: &str) -> bool {
    if !evidence_label(name, 250) {
        return false;
    }
    let normalized = name.trim().to_lowercase();
    if [
        "set up job",
        "complete job",
        "initialize containers",
        "start containers",
        "stop containers",
    ]
    .contains(&normalized.as_str())
    {
        return false;
    }
    !(normalized.starts_with("pre ") || normalized.starts_with("post "))
}

fn trusted_workflow_path(path: &str) -> bool {
    path.starts_with(".github/workflows/")
        && (path.ends_with(".yml") || path.ends_with(".yaml"))
        && path
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '.' | '_' | '/' | '-'))
        && path
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != "..")
}

fn run_workflow_path_matches(actual: &str, expected: &str) -> bool {
    if actual == expected {
        return true;
    }
    let prefix = format!("{expected}@");
    let Some(reference) = actual.strip_prefix(&prefix) else {
        return false;
    };
    !reference.is_empty()
        && reference
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '_' | '.' | '/' | '-'))
        && reference
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != "..")
}

fn timestamp(value: &str, label: &str) -> Result<[u32; 6], VerifyError> {
    let bytes = value.as_bytes();
    if bytes.len() != 20
        || bytes[4] != b'-'
        || bytes[7] != b'-'
        || bytes[10] != b'T'
        || bytes[13] != b':'
        || bytes[16] != b':'
        || bytes[19] != b'Z'
    {
        return denied(format!("{label} lacks a UTC execution timestamp"));
    }
    let parse = |start: usize, end: usize| -> Result<u32, VerifyError> {
        value[start..end]
            .parse()
            .map_err(|_| VerifyError::Denied(format!("{label} has an invalid execution timestamp")))
    };
    let year = parse(0, 4)?;
    let month = parse(5, 7)?;
    let day = parse(8, 10)?;
    let hour = parse(11, 13)?;
    let minute = parse(14, 16)?;
    let second = parse(17, 19)?;
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let days = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if leap => 29,
        2 => 28,
        _ => 0,
    };
    if days == 0 || day == 0 || day > days || hour > 23 || minute > 59 || second > 59 {
        return denied(format!("{label} has an invalid execution timestamp"));
    }
    Ok([year, month, day, hour, minute, second])
}

trait Fetcher {
    fn fetch(&mut self, path: &str) -> Result<Json, VerifyError>;
}

struct ApiFetcher;

impl Fetcher for ApiFetcher {
    fn fetch(&mut self, path: &str) -> Result<Json, VerifyError> {
        api_get(path)
    }
}

fn api_get(path: &str) -> Result<Json, VerifyError> {
    let token = env::var("GH_TOKEN")
        .ok()
        .or_else(|| env::var("GITHUB_TOKEN").ok())
        .ok_or_else(|| VerifyError::Denied("a read-only GitHub API token is required".into()))?;
    if !path.starts_with("repos/") || path.contains("..") || path.contains("//") {
        return denied("invalid GitHub API resource");
    }
    if token.is_empty()
        || token
            .chars()
            .any(|ch| ch.is_control() || matches!(ch, '"' | '\'' | '\\'))
    {
        return denied("invalid GitHub API token representation");
    }

    let url = format!("https://api.github.com/{path}");
    let mut child = Command::new("curl")
        .args([
            "--silent",
            "--show-error",
            "--max-time",
            "20",
            "--max-filesize",
            "4194304",
            "--proto",
            "=https",
            "--request",
            "GET",
            "--header",
            "@-",
            "--write-out",
            "\n__ORES_HTTP_STATUS__%{http_code}",
            &url,
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|_| VerifyError::Denied("authenticated GitHub API query failed".into()))?;

    {
        let mut stdin = child
            .stdin
            .take()
            .ok_or_else(|| VerifyError::Denied("authenticated GitHub API query failed".into()))?;
        writeln!(stdin, "Accept: application/vnd.github+json")
            .and_then(|_| writeln!(stdin, "Authorization: Bearer {token}"))
            .and_then(|_| writeln!(stdin, "X-GitHub-Api-Version: 2022-11-28"))
            .and_then(|_| writeln!(stdin, "User-Agent: oreslang-exact-ci-evidence-v1"))
            .map_err(|_| VerifyError::Denied("authenticated GitHub API query failed".into()))?;
    }

    let output = child
        .wait_with_output()
        .map_err(|_| VerifyError::Denied("authenticated GitHub API query failed".into()))?;
    if !output.status.success() {
        return denied("authenticated GitHub API query failed");
    }
    if output.stdout.len() > MAX_API_JSON_BYTES + 128 {
        return denied("GitHub API JSON response is missing or oversized");
    }
    const MARKER: &[u8] = b"\n__ORES_HTTP_STATUS__";
    let Some(index) = output
        .stdout
        .windows(MARKER.len())
        .rposition(|window| window == MARKER)
    else {
        return denied("GitHub API did not report HTTP status");
    };
    let status = std::str::from_utf8(&output.stdout[index + MARKER.len()..])
        .map_err(|_| VerifyError::Denied("GitHub API status is malformed".into()))?;
    if status != "200" {
        return denied("GitHub API did not return HTTP 200");
    }
    strict_api_json(&output.stdout[..index])
}

#[derive(Clone)]
struct Inputs {
    backend: String,
    commit: String,
    tree: String,
    run_id: u64,
    run_attempt: u64,
    expected_event: String,
    expected_run_path: String,
    workflow_path: String,
    workflow_id: u64,
    workflow_blob_sha: String,
    required_jobs: Vec<String>,
    required_steps: Vec<String>,
    job_step_pairs: Vec<(String, String)>,
}

struct Summary {
    backend: String,
    repository: String,
    repository_id: u64,
    run_id: u64,
    run_attempt: u64,
    workflow_path: String,
    workflow_run_path: String,
    event: String,
    head_commit: String,
    head_tree: String,
    workflow_blob_sha: String,
    verified_jobs: usize,
    successful_non_housekeeping_steps: usize,
}

fn fetch_jobs(
    fetcher: &mut dyn Fetcher,
    prefix: &str,
    run_id: u64,
) -> Result<Vec<Json>, VerifyError> {
    let mut jobs = Vec::new();
    let mut total = None;
    for page in 1..=MAX_PAGES {
        let path = format!(
            "{prefix}/actions/runs/{run_id}/jobs?filter=latest&per_page=100&page={page}"
        );
        let payload = fetcher.fetch(&path)?;
        let reported_total = positive_int(field(&payload, "total_count")?, "workflow jobs total")?;
        if reported_total > 1000 {
            return denied("workflow jobs total is invalid or excessive");
        }
        if let Some(previous) = total {
            if previous != reported_total {
                return denied("workflow job pages disagree on count");
            }
        } else {
            total = Some(reported_total);
        }
        let found = array(field(&payload, "jobs")?)?;
        if found.len() > 100 {
            return denied("workflow jobs page malformed");
        }
        jobs.extend_from_slice(found);
        if jobs.len() >= reported_total as usize {
            break;
        }
        if found.len() != 100 {
            return denied("incomplete GitHub Actions job pagination");
        }
    }
    let total = total.ok_or_else(|| VerifyError::Denied("not all GitHub Actions jobs were retrieved".into()))?;
    if jobs.is_empty() || jobs.len() != total as usize {
        return denied("not all GitHub Actions jobs were retrieved");
    }
    Ok(jobs)
}

fn reject_non_success_run(
    jobs: &[Json],
    run_id: u64,
    commit: &str,
    attempt: u64,
) -> Result<(), VerifyError> {
    let mut no_job_body_executed = true;
    let allowed = BTreeSet::from([
        "failure",
        "cancelled",
        "skipped",
        "timed_out",
        "action_required",
        "stale",
    ]);
    for item in jobs {
        let job_id = positive_int(field(item, "id")?, "job id")?;
        let _ = job_id;
        if positive_int(field(item, "run_id")?, "job run id")? != run_id
            || string(field(item, "head_sha")?)? != commit
            || positive_int(field(item, "run_attempt")?, "job run attempt")? != attempt
            || string(field(item, "status")?)? != "completed"
            || !allowed.contains(string(field(item, "conclusion")?)?)
        {
            return denied("failed workflow job identity/status mismatch");
        }
        match field(item, "runner_id") {
            Ok(Json::Null) => {}
            Ok(Json::Number(raw)) => {
                if raw.contains(['.', 'e', 'E']) || raw.starts_with('-') {
                    return denied("failed workflow job runner id is malformed");
                }
                let value: u64 = raw
                    .parse()
                    .map_err(|_| VerifyError::Denied(
                        "failed workflow job runner id is malformed".into(),
                    ))?;
                if value > 0 {
                    no_job_body_executed = false;
                }
            }
            Err(_) => {}
            _ => return denied("failed workflow job runner id is malformed"),
        }
        if let Ok(value) = field(item, "runner_name") {
            match value {
                Json::Null => {}
                Json::String(name) if name.is_empty() => {}
                Json::String(name) if evidence_label(name, 250) => no_job_body_executed = false,
                _ => return denied("failed workflow job runner name is malformed"),
            }
        }
        if let Ok(steps) = field(item, "steps") {
            match steps {
                Json::Null => {}
                Json::Array(values) if values.is_empty() => {}
                Json::Array(_) => no_job_body_executed = false,
                _ => return denied("failed workflow job steps are malformed"),
            }
        }
    }
    if no_job_body_executed {
        Err(VerifyError::NotExecuted(
            "workflow completed without any allocated runner or executed step records".into(),
        ))
    } else {
        denied("workflow run did not conclude successfully")
    }
}

fn verify(fetcher: &mut dyn Fetcher, input: &Inputs) -> Result<Summary, VerifyError> {
    let (repository, repository_id) =
        repo_for(&input.backend).ok_or_else(|| VerifyError::Denied("unrecognized Oreslang backend".into()))?;
    sha(&input.commit)?;
    sha(&input.tree)?;
    sha(&input.workflow_blob_sha)?;
    if !trusted_workflow_path(&input.workflow_path) {
        return denied("expected an exact trusted workflow path under .github/workflows");
    }
    if input.run_id == 0 || input.workflow_id == 0 || input.run_attempt == 0 {
        return denied("independently pinned IDs and run attempt must be positive");
    }
    if !run_workflow_path_matches(&input.expected_run_path, &input.workflow_path) {
        return denied("expected independently pinned exact workflow run path");
    }
    if !matches!(input.expected_event.as_str(), "push" | "pull_request") {
        return denied("only independently selected push or pull_request event is permitted");
    }
    if input.required_jobs.is_empty() || input.required_steps.is_empty() {
        return denied("explicit job and executable step names are required");
    }
    if input
        .required_jobs
        .iter()
        .chain(input.required_steps.iter())
        .any(|name| !evidence_label(name, 180))
    {
        return denied("invalid required job or step name");
    }
    if input.required_jobs.iter().collect::<BTreeSet<_>>().len() != input.required_jobs.len()
        || input.required_steps.iter().collect::<BTreeSet<_>>().len() != input.required_steps.len()
    {
        return denied("duplicate required CI proof name");
    }
    if input.required_steps.iter().any(|step| !executable_step_name(step)) {
        return denied("required CI step must not be runner housekeeping");
    }
    if input.required_jobs.len() > 1 && input.job_step_pairs.is_empty() {
        return denied("multiple required jobs need explicit job-step bindings");
    }
    if input.job_step_pairs.len() > 100
        || input
            .job_step_pairs
            .iter()
            .any(|(job, step)| {
                !input.required_jobs.contains(job) || !input.required_steps.contains(step)
            })
        || input.job_step_pairs.iter().collect::<BTreeSet<_>>().len()
            != input.job_step_pairs.len()
    {
        return denied("invalid or duplicate required job-step binding");
    }
    if !input.job_step_pairs.is_empty() {
        let bound_jobs: BTreeSet<&String> =
            input.job_step_pairs.iter().map(|(job, _)| job).collect();
        let bound_steps: BTreeSet<&String> =
            input.job_step_pairs.iter().map(|(_, step)| step).collect();
        if bound_jobs != input.required_jobs.iter().collect()
            || !input.required_steps.iter().all(|step| bound_steps.contains(step))
        {
            return denied("all required jobs and steps must have explicit bindings");
        }
    }

    let prefix = format!("repos/{repository}");
    let metadata = fetcher.fetch(&prefix)?;
    if positive_int(field(&metadata, "id")?, "repository id")? != repository_id
        || string(field(&metadata, "full_name")?)? != repository
        || !boolean(field(&metadata, "private")?, "repository privacy")?
        || boolean(field(&metadata, "archived")?, "repository archived")?
        || boolean(field(&metadata, "disabled")?, "repository disabled")?
    {
        return denied("GitHub repository identity/visibility mismatch");
    }

    let commit = fetcher.fetch(&format!("{prefix}/git/commits/{}", input.commit))?;
    if string(field(&commit, "sha")?)? != input.commit
        || string(field(field(&commit, "tree")?, "sha")?)? != input.tree
    {
        return denied("source commit does not point to supplied exact tree");
    }

    let workflow = fetcher.fetch(&format!(
        "{prefix}/contents/{}?ref={}",
        input.workflow_path, input.commit
    ))?;
    if string(field(&workflow, "type")?)? != "file"
        || string(field(&workflow, "path")?)? != input.workflow_path
        || string(field(&workflow, "sha")?)? != input.workflow_blob_sha
    {
        return denied("workflow blob differs from independently reviewed SHA");
    }

    let run_path = format!("{prefix}/actions/runs/{}", input.run_id);
    let run = fetcher.fetch(&run_path)?;
    let attempt = positive_int(field(&run, "run_attempt")?, "run attempt")?;
    if attempt != input.run_attempt {
        return denied("run attempt differs from independently pinned attempt");
    }
    if positive_int(field(&run, "id")?, "run id")? != input.run_id
        || positive_int(field(field(&run, "repository")?, "id")?, "run repository id")?
            != repository_id
        || positive_int(
            field(field(&run, "head_repository")?, "id")?,
            "run head repository id",
        )? != repository_id
        || string(field(&run, "head_sha")?)? != input.commit
        || string(field(&run, "event")?)? != input.expected_event
        || positive_int(field(&run, "workflow_id")?, "run workflow id")? != input.workflow_id
        || string(field(&run, "path")?)? != input.expected_run_path
        || string(field(&run, "status")?)? != "completed"
    {
        return denied("workflow run is not a completed exact-repository HEAD execution");
    }

    let jobs = fetch_jobs(fetcher, &prefix, input.run_id)?;
    if string(field(&run, "conclusion")?)? != "success" {
        reject_non_success_run(&jobs, input.run_id, &input.commit, attempt)?;
        return denied("workflow run did not conclude successfully");
    }

    let mut seen_ids = BTreeSet::new();
    let mut job_names = BTreeSet::new();
    let mut steps_by_job: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    let mut successful_non_housekeeping_steps = 0usize;

    for job in &jobs {
        let job_id = positive_int(field(job, "id")?, "job id")?;
        if !seen_ids.insert(job_id) {
            return denied("duplicated workflow job id");
        }
        if positive_int(field(job, "run_id")?, "job run id")? != input.run_id
            || string(field(job, "head_sha")?)? != input.commit
            || positive_int(field(job, "run_attempt")?, "job run attempt")? != attempt
            || string(field(job, "status")?)? != "completed"
            || string(field(job, "conclusion")?)? != "success"
        {
            return denied("workflow contains a failed, skipped, or unexecuted job");
        }
        if positive_int(field(job, "runner_id")?, "runner id")? == 0 {
            return denied("workflow job lacks a real allocated runner");
        }
        let runner_name = string(field(job, "runner_name")?)?;
        if !evidence_label(runner_name, 250) {
            return denied("workflow job lacks a real allocated runner");
        }
        let job_start = timestamp(string(field(job, "started_at")?)?, "job start")?;
        let job_end = timestamp(string(field(job, "completed_at")?)?, "job completion")?;
        if job_end < job_start {
            return denied("workflow job completion precedes its start");
        }
        let job_name = string(field(job, "name")?)?;
        if !evidence_label(job_name, 250) {
            return denied("missing workflow job name");
        }
        if !job_names.insert(job_name.to_string()) {
            return denied("ambiguous repeated workflow job name");
        }
        let steps = array(field(job, "steps")?)?;
        if steps.is_empty() {
            return denied("workflow job has zero executed steps");
        }
        let mut seen_step_names = BTreeSet::new();
        let mut last_number = 0u64;
        let mut previous_step_end = job_start;
        let mut useful = false;
        let mut observed = BTreeSet::new();
        for step in steps {
            if string(field(step, "status")?)? != "completed"
                || string(field(step, "conclusion")?)? != "success"
            {
                return denied("workflow step was not completed successfully");
            }
            let number = positive_int(field(step, "number")?, "step number")?;
            if number <= last_number {
                return denied("workflow steps have duplicate or out-of-order numbers");
            }
            last_number = number;
            let step_start = timestamp(string(field(step, "started_at")?)?, "step start")?;
            let step_end = timestamp(string(field(step, "completed_at")?)?, "step completion")?;
            if !(job_start <= step_start
                && step_start <= step_end
                && step_end <= job_end)
            {
                return denied("workflow step timing falls outside completed job");
            }
            if step_start < previous_step_end {
                return denied("workflow step timestamps overlap or contradict execution order");
            }
            previous_step_end = step_end;
            let step_name = string(field(step, "name")?)?;
            if !evidence_label(step_name, 250) {
                return denied("workflow step has no name");
            }
            if !seen_step_names.insert(step_name.to_string()) {
                return denied("ambiguous duplicate workflow step name in job");
            }
            observed.insert(step_name.to_string());
            if executable_step_name(step_name) {
                useful = true;
                successful_non_housekeeping_steps += 1;
            }
        }
        if !useful {
            return denied("workflow job only executed runner housekeeping");
        }
        steps_by_job.insert(job_name.to_string(), observed);
    }

    if !input.required_jobs.iter().all(|name| job_names.contains(name)) {
        return denied("required workflow job is missing");
    }
    let required_steps_observed: BTreeSet<&String> = input
        .required_jobs
        .iter()
        .filter_map(|job| steps_by_job.get(job))
        .flat_map(|steps| steps.iter())
        .collect();
    if !input
        .required_steps
        .iter()
        .all(|step| required_steps_observed.contains(step))
    {
        return denied("required executable CI step is missing");
    }
    if input.job_step_pairs.iter().any(|(job, step)| {
        !steps_by_job
            .get(job)
            .is_some_and(|steps| steps.contains(step))
    }) {
        return denied("required executable step did not run in its bound CI job");
    }

    let final_run = fetcher.fetch(&run_path)?;
    if positive_int(field(&final_run, "id")?, "final run id")? != input.run_id
        || positive_int(field(&final_run, "run_attempt")?, "final run attempt")? != attempt
        || string(field(&final_run, "head_sha")?)? != input.commit
        || string(field(&final_run, "event")?)? != input.expected_event
        || positive_int(field(&final_run, "workflow_id")?, "final workflow id")? != input.workflow_id
        || string(field(&final_run, "status")?)? != "completed"
        || string(field(&final_run, "conclusion")?)? != "success"
        || string(field(&final_run, "path")?)? != input.expected_run_path
        || positive_int(
            field(field(&final_run, "repository")?, "id")?,
            "final run repository id",
        )? != repository_id
        || positive_int(
            field(field(&final_run, "head_repository")?, "id")?,
            "final head repository id",
        )? != repository_id
    {
        return denied("workflow run changed during evidence collection");
    }

    let final_metadata = fetcher.fetch(&prefix)?;
    if positive_int(field(&final_metadata, "id")?, "final repository id")? != repository_id
        || string(field(&final_metadata, "full_name")?)? != repository
        || !boolean(field(&final_metadata, "private")?, "final repository privacy")?
        || boolean(field(&final_metadata, "archived")?, "final repository archived")?
        || boolean(field(&final_metadata, "disabled")?, "final repository disabled")?
    {
        return denied("source repository changed identity or privacy during evidence collection");
    }

    Ok(Summary {
        backend: input.backend.clone(),
        repository: repository.to_string(),
        repository_id,
        run_id: input.run_id,
        run_attempt: attempt,
        workflow_path: input.workflow_path.clone(),
        workflow_run_path: input.expected_run_path.clone(),
        event: input.expected_event.clone(),
        head_commit: input.commit.clone(),
        head_tree: input.tree.clone(),
        workflow_blob_sha: input.workflow_blob_sha.clone(),
        verified_jobs: jobs.len(),
        successful_non_housekeeping_steps,
    })
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

fn summary_json(summary: &Summary) -> String {
    format!(
        "{{\"backend\":\"{}\",\"repository\":\"{}\",\"repository_id\":{},\"run_id\":{},\"run_attempt\":{},\"workflow_path\":\"{}\",\"workflow_run_path\":\"{}\",\"event\":\"{}\",\"head_commit\":\"{}\",\"head_tree\":\"{}\",\"workflow_blob_sha\":\"{}\",\"verified_jobs\":{},\"successful_non_housekeeping_steps\":{},\"statement\":\"GITHUB_RUN_EXECUTION_VERIFIED_NOT_GUEST_PARITY_CERTIFIED\"}}",
        json_escape(&summary.backend),
        json_escape(&summary.repository),
        summary.repository_id,
        summary.run_id,
        summary.run_attempt,
        json_escape(&summary.workflow_path),
        json_escape(&summary.workflow_run_path),
        json_escape(&summary.event),
        summary.head_commit,
        summary.head_tree,
        summary.workflow_blob_sha,
        summary.verified_jobs,
        summary.successful_non_housekeeping_steps
    )
}

fn s(value: &str) -> Json {
    Json::String(value.to_string())
}
fn n(value: u64) -> Json {
    Json::Number(value.to_string())
}
fn b(value: bool) -> Json {
    Json::Bool(value)
}
fn obj(fields: Vec<(&str, Json)>) -> Json {
    Json::Object(
        fields
            .into_iter()
            .map(|(key, value)| (key.to_string(), value))
            .collect(),
    )
}
fn arr(values: Vec<Json>) -> Json {
    Json::Array(values)
}

#[derive(Clone)]
struct MapFetcher {
    values: BTreeMap<String, Json>,
}

impl Fetcher for MapFetcher {
    fn fetch(&mut self, path: &str) -> Result<Json, VerifyError> {
        self.values
            .get(path)
            .cloned()
            .ok_or_else(|| VerifyError::Denied(format!("synthetic missing resource: {path}")))
    }
}

#[derive(Clone)]
struct Fixture {
    input: Inputs,
    prefix: String,
    values: BTreeMap<String, Json>,
}

fn canonical_fixture(backend: &str) -> Fixture {
    let (repo, repo_id) = repo_for(backend).expect("known backend");
    let commit = "a".repeat(40);
    let tree = "b".repeat(40);
    let blob = "c".repeat(40);
    let prefix = format!("repos/{repo}");
    let run_path = format!("{prefix}/actions/runs/17");
    let jobs_path =
        format!("{run_path}/jobs?filter=latest&per_page=100&page=1");
    let workflow_path = ".github/workflows/ci.yml";

    let job = obj(vec![
        ("id", n(31)),
        ("name", s("unit")),
        ("run_id", n(17)),
        ("run_attempt", n(2)),
        ("head_sha", s(&commit)),
        ("status", s("completed")),
        ("conclusion", s("success")),
        ("runner_id", n(1_000_002_089)),
        ("runner_name", s("GitHub Actions 1")),
        ("started_at", s("2026-10-16T10:00:00Z")),
        ("completed_at", s("2026-10-16T10:03:00Z")),
        (
            "steps",
            arr(vec![
                obj(vec![
                    ("name", s("Set up job")),
                    ("status", s("completed")),
                    ("conclusion", s("success")),
                    ("number", n(1)),
                    ("started_at", s("2026-10-16T10:00:00Z")),
                    ("completed_at", s("2026-10-16T10:00:59Z")),
                ]),
                obj(vec![
                    ("name", s("Execute test suite")),
                    ("status", s("completed")),
                    ("conclusion", s("success")),
                    ("number", n(2)),
                    ("started_at", s("2026-10-16T10:01:00Z")),
                    ("completed_at", s("2026-10-16T10:01:59Z")),
                ]),
                obj(vec![
                    ("name", s("Complete job")),
                    ("status", s("completed")),
                    ("conclusion", s("success")),
                    ("number", n(3)),
                    ("started_at", s("2026-10-16T10:02:00Z")),
                    ("completed_at", s("2026-10-16T10:03:00Z")),
                ]),
            ]),
        ),
    ]);

    let run = obj(vec![
        ("id", n(17)),
        ("repository", obj(vec![("id", n(repo_id))])),
        ("head_repository", obj(vec![("id", n(repo_id))])),
        ("head_sha", s(&commit)),
        ("path", s(workflow_path)),
        ("event", s("push")),
        ("status", s("completed")),
        ("conclusion", s("success")),
        ("run_attempt", n(2)),
        ("workflow_id", n(991)),
    ]);

    let mut values = BTreeMap::new();
    values.insert(
        prefix.clone(),
        obj(vec![
            ("id", n(repo_id)),
            ("full_name", s(repo)),
            ("private", b(true)),
            ("archived", b(false)),
            ("disabled", b(false)),
        ]),
    );
    values.insert(
        format!("{prefix}/git/commits/{commit}"),
        obj(vec![("sha", s(&commit)), ("tree", obj(vec![("sha", s(&tree))]))]),
    );
    values.insert(
        format!("{prefix}/contents/{workflow_path}?ref={commit}"),
        obj(vec![
            ("type", s("file")),
            ("path", s(workflow_path)),
            ("sha", s(&blob)),
        ]),
    );
    values.insert(run_path, run);
    values.insert(
        jobs_path,
        obj(vec![("total_count", n(1)), ("jobs", arr(vec![job]))]),
    );

    Fixture {
        input: Inputs {
            backend: backend.to_string(),
            commit,
            tree,
            run_id: 17,
            run_attempt: 2,
            expected_event: "push".into(),
            expected_run_path: workflow_path.into(),
            workflow_path: workflow_path.into(),
            workflow_id: 991,
            workflow_blob_sha: blob,
            required_jobs: vec!["unit".into()],
            required_steps: vec!["Execute test suite".into()],
            job_step_pairs: Vec::new(),
        },
        prefix,
        values,
    }
}

fn verify_fixture(fixture: Fixture) -> Result<Summary, VerifyError> {
    let mut fetcher = MapFetcher {
        values: fixture.values,
    };
    verify(&mut fetcher, &fixture.input)
}

fn expect_denied(fixture: Fixture, label: &str, count: &mut usize) -> Result<(), String> {
    match verify_fixture(fixture) {
        Err(VerifyError::Denied(_)) => {
            *count += 1;
            Ok(())
        }
        Err(VerifyError::NotExecuted(message)) => {
            Err(format!("{label}: expected denial, got NOT_EXECUTED: {message}"))
        }
        Ok(_) => Err(format!("{label}: unsafe evidence fixture accepted")),
    }
}

fn path_mut<'a>(fixture: &'a mut Fixture, path: &str) -> Result<&'a mut Json, String> {
    fixture
        .values
        .get_mut(path)
        .ok_or_else(|| format!("missing synthetic path: {path}"))
}

fn regression_self_test() -> Result<usize, String> {
    let mut checks = 0usize;

    let baseline = canonical_fixture("java");
    let summary = verify_fixture(baseline.clone()).map_err(|e| e.to_string())?;
    if summary.verified_jobs != 1 || summary.successful_non_housekeeping_steps != 1 {
        return Err("baseline evidence summary drift".into());
    }
    checks += 1;

    for backend in [
        "java",
        "llvm",
        "net",
        "wasm",
        "forge",
        "rx_pull",
        "rx_callbacks",
        "rx_channels",
    ] {
        let summary = verify_fixture(canonical_fixture(backend)).map_err(|e| e.to_string())?;
        if summary.backend != backend {
            return Err(format!("backend inventory mismatch: {backend}"));
        }
        checks += 1;
    }

    for bad in [
        "A".repeat(40),
        "z".repeat(40),
        "a".repeat(39),
        String::new(),
        "a".repeat(41),
        "g".repeat(40),
    ] {
        let mut fixture = baseline.clone();
        fixture.input.commit = bad;
        expect_denied(fixture, "invalid commit SHA", &mut checks)?;
    }

    for path in [
        "",
        "ci.yml",
        ".github/workflows/../ci.yml",
        ".github/workflows//ci.yml",
        ".github/workflows/ci.yml@main",
        ".github/workflows/ci.txt",
        ".github/workflows/./ci.yml",
        "../.github/workflows/ci.yml",
    ] {
        let mut fixture = baseline.clone();
        fixture.input.workflow_path = path.into();
        expect_denied(fixture, "invalid trusted workflow path", &mut checks)?;
    }

    for run_path in [
        ".github/workflows/ci.yml@",
        ".github/workflows/ci.yml@../main",
        ".github/workflows/ci.yml@main@wrong",
        ".github/workflows/ci.yml@refs//heads/main",
        ".github/workflows/ci.yml.bak@main",
        ".github/workflows/ci.yml@/refs/heads/main",
    ] {
        let mut fixture = baseline.clone();
        fixture.input.expected_run_path = run_path.into();
        expect_denied(fixture, "invalid run workflow path", &mut checks)?;
    }

    for event in [
        "workflow_dispatch",
        "workflow_run",
        "pull_request_target",
        "repository_dispatch",
        "schedule",
        "merge_group",
        "",
    ] {
        let mut fixture = baseline.clone();
        fixture.input.expected_event = event.into();
        expect_denied(fixture, "invalid expected event", &mut checks)?;
    }

    for name in [
        "Set up job",
        "Complete job",
        "Post actions/checkout",
        "Pre actions/checkout",
        "Initialize containers",
        "Start containers",
        "Stop containers",
    ] {
        let mut fixture = baseline.clone();
        fixture.input.required_steps = vec![name.into()];
        expect_denied(fixture, "housekeeping required step", &mut checks)?;
    }

    for malicious in [
        "CI\nFake",
        "CI\u{2028}Fake",
        "CI\u{202e}Fake",
        "CI\u{2066}Fake",
        "CI\u{200b}Fake",
        "   ",
    ] {
        let mut fixture = baseline.clone();
        fixture.input.required_jobs = vec![malicious.into()];
        expect_denied(fixture, "spoofed required job label", &mut checks)?;
    }

    for (field_name, replacement) in [
        ("id", n(1_400_792_275)),
        ("full_name", s("forged/repo")),
        ("private", b(false)),
        ("archived", b(true)),
        ("disabled", b(true)),
    ] {
        let mut fixture = baseline.clone();
        let prefix = fixture.prefix.clone();
        *field_mut(
            path_mut(&mut fixture, &prefix).map_err(|e| e.to_string())?,
            field_name,
        )
        .map_err(|e| e.to_string())? = replacement;
        expect_denied(fixture, "repository identity/privacy mutation", &mut checks)?;
    }

    {
        let mut fixture = baseline.clone();
        let path = format!("{}/git/commits/{}", fixture.prefix, fixture.input.commit);
        *field_mut(path_mut(&mut fixture, &path)?, "sha").map_err(|e| e.to_string())? =
            s(&"d".repeat(40));
        expect_denied(fixture, "forged commit SHA", &mut checks)?;
    }
    {
        let mut fixture = baseline.clone();
        let path = format!("{}/git/commits/{}", fixture.prefix, fixture.input.commit);
        *field_mut(
            field_mut(path_mut(&mut fixture, &path)?, "tree").map_err(|e| e.to_string())?,
            "sha",
        )
        .map_err(|e| e.to_string())? = s(&"d".repeat(40));
        expect_denied(fixture, "wrong commit tree", &mut checks)?;
    }

    for (field_name, replacement) in [
        ("type", s("dir")),
        ("path", s(".github/workflows/other.yml")),
        ("sha", s(&"d".repeat(40))),
    ] {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/contents/{}?ref={}",
            fixture.prefix, fixture.input.workflow_path, fixture.input.commit
        );
        *field_mut(path_mut(&mut fixture, &path)?, field_name).map_err(|e| e.to_string())? =
            replacement;
        expect_denied(fixture, "workflow blob metadata mutation", &mut checks)?;
    }

    for (field_name, replacement) in [
        ("head_sha", s(&"d".repeat(40))),
        ("event", s("workflow_dispatch")),
        ("workflow_id", n(992)),
        ("path", s(".github/workflows/other.yml")),
        ("status", s("in_progress")),
        ("run_attempt", n(3)),
    ] {
        let mut fixture = baseline.clone();
        let path = format!("{}/actions/runs/17", fixture.prefix);
        *field_mut(path_mut(&mut fixture, &path)?, field_name).map_err(|e| e.to_string())? =
            replacement;
        expect_denied(fixture, "run identity mutation", &mut checks)?;
    }

    for runner_id in [
        Json::Null,
        Json::Bool(false),
        Json::Bool(true),
        Json::Number("0".into()),
        Json::Number("-1".into()),
        Json::Number("1.0".into()),
        Json::String("1000002089".into()),
    ] {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/actions/runs/17/jobs?filter=latest&per_page=100&page=1",
            fixture.prefix
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        *field_mut(&mut jobs[0], "runner_id").map_err(|e| e.to_string())? = runner_id;
        expect_denied(fixture, "invalid runner id", &mut checks)?;
    }

    for runner_name in [
        "",
        "  ",
        "Runner\nSpoof",
        "Runner\u{0085}Spoof",
        "Runner\u{2028}Spoof",
        "Runner\u{202e}Spoof",
        "Runner\u{200b}Spoof",
    ] {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/actions/runs/17/jobs?filter=latest&per_page=100&page=1",
            fixture.prefix
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        *field_mut(&mut jobs[0], "runner_name").map_err(|e| e.to_string())? = s(runner_name);
        expect_denied(fixture, "spoofed runner name", &mut checks)?;
    }

    for (field_name, replacement) in [
        ("head_sha", s(&"d".repeat(40))),
        ("run_attempt", n(1)),
        ("status", s("in_progress")),
        ("conclusion", s("skipped")),
    ] {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/actions/runs/17/jobs?filter=latest&per_page=100&page=1",
            fixture.prefix
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        *field_mut(&mut jobs[0], field_name).map_err(|e| e.to_string())? = replacement;
        expect_denied(fixture, "job identity/status mutation", &mut checks)?;
    }

    for (field_name, replacement) in [
        ("status", s("in_progress")),
        ("conclusion", s("skipped")),
        ("number", n(1)),
        ("started_at", s("2026-10-16T09:59:59Z")),
        ("completed_at", s("2026-10-16T10:04:00Z")),
        ("name", s("Set up job")),
        ("name", s("Exec\nFake")),
        ("name", s("Exec\u{202e}Fake")),
    ] {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/actions/runs/17/jobs?filter=latest&per_page=100&page=1",
            fixture.prefix
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        let steps = array_mut(field_mut(&mut jobs[0], "steps").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        *field_mut(&mut steps[1], field_name).map_err(|e| e.to_string())? = replacement;
        expect_denied(fixture, "step execution mutation", &mut checks)?;
    }

    {
        let mut fixture = baseline.clone();
        fixture.input.required_jobs = Vec::new();
        fixture.input.required_steps = Vec::new();
        expect_denied(fixture, "missing execution requirements", &mut checks)?;
    }
    {
        let mut fixture = baseline.clone();
        fixture.input.required_jobs = vec!["unit".into(), "unit".into()];
        expect_denied(fixture, "duplicate required jobs", &mut checks)?;
    }
    {
        let mut fixture = baseline.clone();
        fixture.input.required_steps = vec!["Execute missing suite".into()];
        expect_denied(fixture, "missing required step", &mut checks)?;
    }
    {
        let mut fixture = baseline.clone();
        fixture.input.required_jobs = vec!["missing-job".into()];
        expect_denied(fixture, "missing required job", &mut checks)?;
    }

    {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/actions/runs/17/jobs?filter=latest&per_page=100&page=1",
            fixture.prefix
        );
        *field_mut(path_mut(&mut fixture, &path)?, "total_count").map_err(|e| e.to_string())? =
            n(2);
        expect_denied(fixture, "incomplete pagination", &mut checks)?;
    }
    {
        let mut fixture = baseline.clone();
        let path = format!(
            "{}/actions/runs/17/jobs?filter=latest&per_page=100&page=1",
            fixture.prefix
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        jobs.push(jobs[0].clone());
        *field_mut(path_mut(&mut fixture, &path)?, "total_count").map_err(|e| e.to_string())? =
            n(2);
        expect_denied(fixture, "duplicate job id", &mut checks)?;
    }

    {
        let mut fixture = baseline.clone();
        let run_path = format!("{}/actions/runs/17", fixture.prefix);
        *field_mut(path_mut(&mut fixture, &run_path)?, "conclusion").map_err(|e| e.to_string())? =
            s("failure");
        let jobs_path = format!(
            "{run_path}/jobs?filter=latest&per_page=100&page=1"
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &jobs_path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        *field_mut(&mut jobs[0], "conclusion").map_err(|e| e.to_string())? = s("failure");
        *field_mut(&mut jobs[0], "runner_id").map_err(|e| e.to_string())? = n(1);
        match verify_fixture(fixture) {
            Err(VerifyError::Denied(_)) => checks += 1,
            other => return Err(format!("failed executed job classification drift: {other:?}")),
        }
    }

    {
        let mut fixture = baseline.clone();
        let run_path = format!("{}/actions/runs/17", fixture.prefix);
        *field_mut(path_mut(&mut fixture, &run_path)?, "conclusion").map_err(|e| e.to_string())? =
            s("failure");
        let jobs_path = format!(
            "{run_path}/jobs?filter=latest&per_page=100&page=1"
        );
        let jobs = array_mut(field_mut(path_mut(&mut fixture, &jobs_path)?, "jobs").map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        *field_mut(&mut jobs[0], "conclusion").map_err(|e| e.to_string())? = s("failure");
        *field_mut(&mut jobs[0], "runner_id").map_err(|e| e.to_string())? = n(0);
        *field_mut(&mut jobs[0], "runner_name").map_err(|e| e.to_string())? = s("");
        *field_mut(&mut jobs[0], "steps").map_err(|e| e.to_string())? = arr(Vec::new());
        match verify_fixture(fixture) {
            Err(VerifyError::NotExecuted(_)) => checks += 1,
            other => return Err(format!("NOT_EXECUTED classification drift: {other:?}")),
        }
    }

    for raw in [
        b"{\"id\":17,\"id\":18}".as_slice(),
        b"{\"repository\":{\"id\":1,\"id\":2}}".as_slice(),
        b"[]".as_slice(),
        b"null".as_slice(),
        b"2".as_slice(),
        b"\"\"".as_slice(),
        b"{not-json}".as_slice(),
        b"{\"id\":NaN}".as_slice(),
        b"{\"id\":Infinity}".as_slice(),
        b"{\"id\":-Infinity}".as_slice(),
        b"{\"id\":+1}".as_slice(),
        b"{\"id\":01}".as_slice(),
        b"{\"id\":1} trailing".as_slice(),
    ] {
        if strict_api_json(raw).is_ok() {
            return Err(format!("ambiguous API JSON accepted: {:?}", String::from_utf8_lossy(raw)));
        }
        checks += 1;
    }

    let oversized_integer = format!("{{\"id\":{}}}", "9".repeat(65));
    if strict_api_json(oversized_integer.as_bytes()).is_ok() {
        return Err("oversized API integer accepted".into());
    }
    checks += 1;

    let oversized_float = format!("{{\"id\":1.{}}}", "1".repeat(130));
    if strict_api_json(oversized_float.as_bytes()).is_ok() {
        return Err("oversized API float accepted".into());
    }
    checks += 1;

    if !executable_step_name("Prepare build")
        || !executable_step_name("Exécuter native tests")
        || executable_step_name(" POST Checkout")
        || executable_step_name("pre Setup")
    {
        return Err("executable step label classifier drift".into());
    }
    checks += 4;

    if checks < 95 {
        return Err(format!("CI verifier regressions shrank below 95: {checks}"));
    }
    Ok(checks)
}

fn write_source_free_report(path: &Path) -> Result<(), String> {
    let required = |name: &str| {
        env::var(name).map_err(|_| format!("missing environment variable {name}"))
    };
    let expected_head = required("EXPECTED_HEAD")?;
    let observed_head = required("OBSERVED_HEAD")?;
    let observed_tree = required("OBSERVED_TREE")?;
    let source_outcome = required("SOURCE_OUTCOME")?;
    let regression_outcome = required("REGRESSION_OUTCOME")?;
    let repository_id = required("REPOSITORY_ID")?;
    let trigger_sha = required("RUN_TRIGGER_SHA")?;
    let trigger_event = required("RUN_TRIGGER_EVENT")?;
    let repository = required("GITHUB_REPOSITORY")?;
    let run_id = required("GITHUB_RUN_ID")?;
    let run_attempt = required("GITHUB_RUN_ATTEMPT")?;
    let runner_name = env::var("RUNNER_NAME").unwrap_or_default();

    sha(&expected_head).map_err(|e| e.to_string())?;
    sha(&observed_head).map_err(|e| e.to_string())?;
    sha(&observed_tree).map_err(|e| e.to_string())?;
    sha(&trigger_sha).map_err(|e| e.to_string())?;

    let trigger_matches =
        trigger_sha == observed_head && source_outcome == "success";

    let text = format!(
        "{{\n  \"schema\": \"oreslang-source-free-ci-evidence-v1\",\n  \"classification\": \"NOT_NATIVE_COMPILER_OR_GUEST_PARITY_CERTIFICATION\",\n  \"repository\": \"{}\",\n  \"repository_id\": \"{}\",\n  \"run_id\": \"{}\",\n  \"run_attempt\": \"{}\",\n  \"expected_commit\": \"{}\",\n  \"observed_checkout_commit\": \"{}\",\n  \"observed_checkout_tree\": \"{}\",\n  \"head_verification_outcome\": \"{}\",\n  \"regression_step_outcome\": \"{}\",\n  \"runner_name\": \"{}\",\n  \"workflow_trigger_sha\": \"{}\",\n  \"workflow_trigger_event\": \"{}\",\n  \"trigger_sha_matches_reported_checkout\": {},\n  \"verified_execution_claim\": false\n}}\n",
        json_escape(&repository),
        json_escape(&repository_id),
        json_escape(&run_id),
        json_escape(&run_attempt),
        expected_head,
        observed_head,
        observed_tree,
        json_escape(&source_outcome),
        json_escape(&regression_outcome),
        json_escape(&runner_name),
        trigger_sha,
        json_escape(&trigger_event),
        if trigger_matches { "true" } else { "false" },
    );
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create report directory: {e}"))?;
    }
    fs::write(path, text).map_err(|e| format!("write report: {e}"))?;
    Ok(())
}

fn usage() -> ! {
    eprintln!(
        "usage: verify_ci_run.rs --self-test | --write-source-free-report PATH | --backend BACKEND --commit SHA --tree SHA --run-id ID --run-attempt N --event EVENT --workflow-run-path PATH --workflow-path PATH --workflow-id ID --workflow-blob-sha SHA --require-job NAME --require-step NAME [--require-job-step JOB::STEP]"
    );
    std::process::exit(64);
}

fn main() {
    let mut args = env::args().skip(1).peekable();
    if args.peek().is_some_and(|arg| arg == "--self-test") {
        args.next();
        if args.next().is_some() {
            usage();
        }
        match regression_self_test() {
            Ok(count) => {
                println!("CI_EVIDENCE_REGRESSIONS={count}");
                return;
            }
            Err(error) => {
                eprintln!("CI_EVIDENCE_SELFTEST_FAILED: {error}");
                std::process::exit(1);
            }
        }
    }
    if args.peek().is_some_and(|arg| arg == "--write-source-free-report") {
        args.next();
        let path = args.next().map(PathBuf::from).unwrap_or_else(|| usage());
        if args.next().is_some() {
            usage();
        }
        match write_source_free_report(&path) {
            Ok(()) => {
                println!("SOURCE_FREE_EVIDENCE_WRITTEN=YES; EXTERNAL_VERIFICATION_REQUIRED");
                return;
            }
            Err(error) => {
                eprintln!("SOURCE_FREE_EVIDENCE_REJECTED: {error}");
                std::process::exit(1);
            }
        }
    }

    let mut backend = None;
    let mut commit = None;
    let mut tree = None;
    let mut run_id = None;
    let mut run_attempt = None;
    let mut event = None;
    let mut workflow_run_path = None;
    let mut workflow_path = None;
    let mut workflow_id = None;
    let mut workflow_blob_sha = None;
    let mut required_jobs = Vec::new();
    let mut required_steps = Vec::new();
    let mut job_step_pairs = Vec::new();

    while let Some(arg) = args.next() {
        let value = |args: &mut std::iter::Peekable<std::iter::Skip<std::env::Args>>| {
            args.next().unwrap_or_else(|| usage())
        };
        match arg.as_str() {
            "--backend" => backend = Some(value(&mut args)),
            "--commit" => commit = Some(value(&mut args)),
            "--tree" => tree = Some(value(&mut args)),
            "--run-id" => {
                run_id = Some(
                    value(&mut args)
                        .parse::<u64>()
                        .unwrap_or_else(|_| usage()),
                )
            }
            "--run-attempt" => {
                run_attempt = Some(
                    value(&mut args)
                        .parse::<u64>()
                        .unwrap_or_else(|_| usage()),
                )
            }
            "--event" => event = Some(value(&mut args)),
            "--workflow-run-path" => workflow_run_path = Some(value(&mut args)),
            "--workflow-path" => workflow_path = Some(value(&mut args)),
            "--workflow-id" => {
                workflow_id = Some(
                    value(&mut args)
                        .parse::<u64>()
                        .unwrap_or_else(|_| usage()),
                )
            }
            "--workflow-blob-sha" => workflow_blob_sha = Some(value(&mut args)),
            "--require-job" => required_jobs.push(value(&mut args)),
            "--require-step" => required_steps.push(value(&mut args)),
            "--require-job-step" => {
                let binding = value(&mut args);
                let Some((job, step)) = binding.split_once("::") else {
                    eprintln!("CI_RUN_EVIDENCE_REJECTED: job-step binding must use JOB::STEP");
                    std::process::exit(1);
                };
                job_step_pairs.push((job.to_string(), step.to_string()));
            }
            _ => usage(),
        }
    }

    let input = Inputs {
        backend: backend.unwrap_or_else(|| usage()),
        commit: commit.unwrap_or_else(|| usage()),
        tree: tree.unwrap_or_else(|| usage()),
        run_id: run_id.unwrap_or_else(|| usage()),
        run_attempt: run_attempt.unwrap_or_else(|| usage()),
        expected_event: event.unwrap_or_else(|| usage()),
        expected_run_path: workflow_run_path.unwrap_or_else(|| usage()),
        workflow_path: workflow_path.unwrap_or_else(|| usage()),
        workflow_id: workflow_id.unwrap_or_else(|| usage()),
        workflow_blob_sha: workflow_blob_sha.unwrap_or_else(|| usage()),
        required_jobs,
        required_steps,
        job_step_pairs,
    };

    let mut fetcher = ApiFetcher;
    match verify(&mut fetcher, &input) {
        Ok(summary) => println!("{}", summary_json(&summary)),
        Err(VerifyError::NotExecuted(error)) => {
            eprintln!("CI_RUN_EVIDENCE_NOT_EXECUTED: {error}");
            std::process::exit(2);
        }
        Err(VerifyError::Denied(error)) => {
            eprintln!("CI_RUN_EVIDENCE_REJECTED: {error}");
            std::process::exit(1);
        }
    }
}
