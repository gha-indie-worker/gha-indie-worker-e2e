use std::{collections::BTreeSet, fs, path::Path};

#[cfg(not(test))]
use std::{env, path::PathBuf, process};

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

    fn parse(mut self) -> Result<Json, String> {
        self.ws();
        let value = self.value()?;
        self.ws();
        if self.at != self.bytes.len() {
            return Err(format!("trailing JSON at byte {}", self.at));
        }
        Ok(value)
    }

    fn ws(&mut self) {
        while self.at < self.bytes.len() && self.bytes[self.at].is_ascii_whitespace() {
            self.at += 1;
        }
    }

    fn value(&mut self) -> Result<Json, String> {
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
            Some(other) => Err(format!("invalid JSON token {:?} at byte {}", other as char, self.at)),
            None => Err("unexpected end of JSON".into()),
        }
    }

    fn literal(&mut self, expected: &[u8]) -> Result<(), String> {
        if self.bytes.get(self.at..self.at + expected.len()) == Some(expected) {
            self.at += expected.len();
            Ok(())
        } else {
            Err(format!("invalid JSON literal at byte {}", self.at))
        }
    }

    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.at) != Some(&b'"') {
            return Err(format!("expected string at byte {}", self.at));
        }
        self.at += 1;
        let mut out = String::new();
        while self.at < self.bytes.len() {
            let b = self.bytes[self.at];
            self.at += 1;
            match b {
                b'"' => return Ok(out),
                b'\\' => {
                    let esc = *self
                        .bytes
                        .get(self.at)
                        .ok_or_else(|| "unterminated JSON escape".to_string())?;
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
                            let first = self.hex4()?;
                            let scalar = if (0xD800..=0xDBFF).contains(&first) {
                                if self.bytes.get(self.at..self.at + 2) != Some(b"\\u") {
                                    return Err("high surrogate without low surrogate".into());
                                }
                                self.at += 2;
                                let low = self.hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&low) {
                                    return Err("invalid low surrogate".into());
                                }
                                0x10000 + (((first - 0xD800) as u32) << 10) + (low - 0xDC00) as u32
                            } else if (0xDC00..=0xDFFF).contains(&first) {
                                return Err("unpaired low surrogate".into());
                            } else {
                                first as u32
                            };
                            out.push(char::from_u32(scalar).ok_or_else(|| "invalid unicode scalar".to_string())?);
                        }
                        _ => return Err(format!("invalid JSON escape at byte {}", self.at - 1)),
                    }
                }
                0x00..=0x1f => return Err("control character in JSON string".into()),
                0x20..=0x7f => out.push(b as char),
                _ => {
                    let start = self.at - 1;
                    let width = match b {
                        0xC2..=0xDF => 2,
                        0xE0..=0xEF => 3,
                        0xF0..=0xF4 => 4,
                        _ => return Err("invalid UTF-8 in JSON string".into()),
                    };
                    let end = start + width;
                    let slice = self
                        .bytes
                        .get(start..end)
                        .ok_or_else(|| "truncated UTF-8 in JSON string".to_string())?;
                    let s = std::str::from_utf8(slice).map_err(|_| "invalid UTF-8 in JSON string".to_string())?;
                    out.push_str(s);
                    self.at = end;
                }
            }
        }
        Err("unterminated JSON string".into())
    }

    fn hex4(&mut self) -> Result<u16, String> {
        let mut value = 0u16;
        for _ in 0..4 {
            let b = *self
                .bytes
                .get(self.at)
                .ok_or_else(|| "truncated unicode escape".to_string())?;
            self.at += 1;
            value = value
                .checked_mul(16)
                .ok_or_else(|| "unicode escape overflow".to_string())?
                + match b {
                    b'0'..=b'9' => (b - b'0') as u16,
                    b'a'..=b'f' => (b - b'a' + 10) as u16,
                    b'A'..=b'F' => (b - b'A' + 10) as u16,
                    _ => return Err("invalid hex digit in unicode escape".into()),
                };
        }
        Ok(value)
    }

    fn array(&mut self) -> Result<Json, String> {
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
                _ => return Err(format!("expected ',' or ']' at byte {}", self.at)),
            }
        }
    }

    fn object(&mut self) -> Result<Json, String> {
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
                return Err(format!("duplicate JSON key: {key}"));
            }
            self.ws();
            if self.bytes.get(self.at) != Some(&b':') {
                return Err(format!("expected ':' at byte {}", self.at));
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
                _ => return Err(format!("expected ',' or '}}' at byte {}", self.at)),
            }
        }
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.at;
        if self.bytes.get(self.at) == Some(&b'-') {
            self.at += 1;
        }
        match self.bytes.get(self.at) {
            Some(b'0') => {
                self.at += 1;
                if matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                    return Err("leading zero in JSON number".into());
                }
            }
            Some(b'1'..=b'9') => {
                self.at += 1;
                while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                    self.at += 1;
                }
            }
            _ => return Err(format!("invalid JSON number at byte {start}")),
        }
        if self.bytes.get(self.at) == Some(&b'.') {
            self.at += 1;
            let frac = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                self.at += 1;
            }
            if self.at == frac {
                return Err("missing fractional digits".into());
            }
        }
        if matches!(self.bytes.get(self.at), Some(b'e' | b'E')) {
            self.at += 1;
            if matches!(self.bytes.get(self.at), Some(b'+' | b'-')) {
                self.at += 1;
            }
            let exp = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                self.at += 1;
            }
            if self.at == exp {
                return Err("missing exponent digits".into());
            }
        }
        let raw = std::str::from_utf8(&self.bytes[start..self.at])
            .map_err(|_| "invalid UTF-8 in number".to_string())?;
        Ok(Json::Number(raw.to_string()))
    }
}

fn parse_json(raw: &[u8]) -> Result<Json, String> {
    let text = std::str::from_utf8(raw).map_err(|_| "JSON must be valid UTF-8".to_string())?;
    if text.starts_with('\u{feff}') {
        return Err("UTF-8 BOM is prohibited".into());
    }
    Parser::new(raw).parse()
}

fn object(value: &Json) -> Result<&[(String, Json)], String> {
    match value {
        Json::Object(v) => Ok(v),
        _ => Err("expected JSON object".into()),
    }
}

fn array(value: &Json) -> Result<&[Json], String> {
    match value {
        Json::Array(v) => Ok(v),
        _ => Err("expected JSON array".into()),
    }
}

fn field<'a>(value: &'a Json, key: &str) -> Result<&'a Json, String> {
    object(value)?
        .iter()
        .find(|(name, _)| name == key)
        .map(|(_, value)| value)
        .ok_or_else(|| format!("missing field: {key}"))
}

fn string(value: &Json) -> Result<&str, String> {
    match value {
        Json::String(v) => Ok(v),
        _ => Err("expected JSON string".into()),
    }
}

fn exact_keys(value: &Json, expected: &[&str]) -> Result<(), String> {
    let got: BTreeSet<&str> = object(value)?.iter().map(|(k, _)| k.as_str()).collect();
    let want: BTreeSet<&str> = expected.iter().copied().collect();
    if got != want {
        return Err(format!("unexpected object keys: got {got:?}, want {want:?}"));
    }
    Ok(())
}

fn string_array(value: &Json) -> Result<Vec<&str>, String> {
    array(value)?
        .iter()
        .map(string)
        .collect::<Result<Vec<_>, _>>()
}

fn require(condition: bool, message: &str) -> Result<(), String> {
    if condition {
        Ok(())
    } else {
        Err(message.to_string())
    }
}

fn rotr(x: u32, n: u32) -> u32 {
    (x >> n) | (x << (32 - n))
}

fn sha256(bytes: &[u8]) -> [u8; 32] {
    const K: [u32; 64] = [
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    ];
    let mut h=[0x6a09e667u32,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
    let bit_len=(bytes.len() as u64)*8;
    let mut data=bytes.to_vec();
    data.push(0x80);
    while data.len()%64 != 56 { data.push(0); }
    data.extend_from_slice(&bit_len.to_be_bytes());
    for chunk in data.chunks_exact(64) {
        let mut w=[0u32;64];
        for i in 0..16 {
            w[i]=u32::from_be_bytes(chunk[i*4..i*4+4].try_into().expect("4-byte word"));
        }
        for i in 16..64 {
            let s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            let s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16].wrapping_add(s0).wrapping_add(w[i-7]).wrapping_add(s1);
        }
        let [mut a,mut b,mut c,mut d,mut e,mut f,mut g,mut hh]=h;
        for i in 0..64 {
            let s1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            let ch=(e&f)^((!e)&g);
            let t1=hh.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            let maj=(a&b)^(a&c)^(b&c);
            let t2=s0.wrapping_add(maj);
            hh=g; g=f; f=e; e=d.wrapping_add(t1); d=c; c=b; b=a; a=t1.wrapping_add(t2);
        }
        let vals=[a,b,c,d,e,f,g,hh];
        for i in 0..8 { h[i]=h[i].wrapping_add(vals[i]); }
    }
    let mut out=[0u8;32];
    for (i,x) in h.iter().enumerate(){
        out[i*4..i*4+4].copy_from_slice(&x.to_be_bytes());
    }
    out
}

fn hex(bytes: &[u8]) -> String {
    const H:&[u8;16]=b"0123456789abcdef";
    let mut out=String::with_capacity(bytes.len()*2);
    for &b in bytes {
        out.push(H[(b>>4) as usize] as char);
        out.push(H[(b&15) as usize] as char);
    }
    out
}

const INTEGRATION_PROTOCOL: &str = "oreslang-ecosystem-integration";
const INTEGRATION_NOTE: &str =
    "Declared integration touchpoints only; no CI or runtime parity is asserted.";

fn upstreams(repo: &str) -> Option<&'static [&'static str]> {
    match repo {
        "oreslang-source.java" => Some(&["oreslang-common-source", "oreslang-target-profiles"]),
        "oreslang-source.llvm" => Some(&["oreslang-common-source", "oreslang-target-profiles"]),
        "oreslang-source.wasm" => Some(&["oreslang-source.llvm", "oreslang-target-profiles", "oreslang-wasm"]),
        "oreslang-source.js" => Some(&["oreslang-common-source", "oreslang-target-profiles"]),
        "oreslang-source.hdl" => Some(&["oreslang-common-source", "oreslang-target-profiles"]),
        "oreslang-source.forge" => Some(&["oreslang-common-source", "oreslang-target-profiles"]),
        "oreslang-source.net" => Some(&["oreslang-common-source", "oreslang-target-profiles"]),
        _ => None,
    }
}

fn verify_integration_bytes(raw: &[u8], repo: &str) -> Result<(), String> {
    if raw.is_empty() || raw.len() > 4096 {
        return Err("manifest is empty or oversized".into());
    }
    let expected_upstreams = upstreams(repo).ok_or_else(|| "unknown source repository".to_string())?;
    let doc = parse_json(raw)?;
    exact_keys(
        &doc,
        &["schemaVersion", "protocol", "repository", "role", "upstreams", "evidence", "note"],
    )?;
    require(field(&doc, "schemaVersion")? == &Json::Number("1".into()), "unsupported schema version")?;
    require(string(field(&doc, "protocol")?)? == INTEGRATION_PROTOCOL, "unsupported protocol")?;
    require(
        string(field(&doc, "repository")?)? == format!("ores-truffle-oreslang/{repo}"),
        "repository identity mismatch",
    )?;
    require(string(field(&doc, "role")?)? == "compiler", "unexpected integration role")?;
    let got = string_array(field(&doc, "upstreams")?)?;
    require(got == expected_upstreams, "unexpected, reordered, or duplicate upstream dependencies")?;
    require(string(field(&doc, "evidence")?)? == "not-attested", "unverified parity/CI attestation is forbidden")?;
    require(string(field(&doc, "note")?)? == INTEGRATION_NOTE, "evidence disclaimer was changed")?;
    Ok(())
}

fn verify_integration_root(root: &Path) -> Result<(), String> {
    let repo = root
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or_else(|| "repository root has no UTF-8 basename".to_string())?;
    let path = root.join(".oreslang/integration.json");
    let meta = fs::symlink_metadata(&path).map_err(|e| format!("manifest missing: {e}"))?;
    if meta.file_type().is_symlink() || !meta.is_file() {
        return Err("manifest missing or symlinked".into());
    }
    if meta.len() > 4096 {
        return Err("manifest exceeds size limit".into());
    }
    let raw = fs::read(&path).map_err(|e| format!("read manifest: {e}"))?;
    verify_integration_bytes(&raw, repo)
}

fn verify_serde(root: &Path) -> Result<(), String> {
    let path = root.join("contracts/v1/serde_boundary_v1.json");
    let raw = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let digest = hex(&sha256(&raw));
    require(
        digest == "0057fa040cd23857ee14b232472c5056a74e11c9528b8b06dbe348023d97106d",
        &format!("serde v1 contract drift: {digest}; bump contract/version intentionally"),
    )?;
    let doc = parse_json(&raw)?;
    require(
        string(field(&doc, "contract")?)? == "oreslang.sendable-serializable.v1",
        "unsupported serde contract revision",
    )?;
    let boundaries = field(&doc, "boundaries")?;
    exact_keys(boundaries, &["actor_data", "network_data", "supervisor_control"])?;
    let actor = field(boundaries, "actor_data")?;
    let network = field(boundaries, "network_data")?;
    let control = field(boundaries, "supervisor_control")?;
    require(
        string(field(actor, "unimplemented_policy")?)? == "reject_before_enqueue",
        "unsupported actor DATA must fail closed",
    )?;
    require(
        string(field(network, "unimplemented_policy")?)? == "reject_before_io",
        "unsupported network codec must fail closed",
    )?;
    require(
        string(field(control, "unimplemented_policy")?)? == "reject_without_data_fallback",
        "supervisor CONTROL must not downgrade into actor DATA",
    )?;
    let formats: BTreeSet<&str> = string_array(field(network, "formats")?)?.into_iter().collect();
    require(
        formats == BTreeSet::from(["json", "messagepack_v2", "protobuf"]),
        "unknown portable network format",
    )?;
    for (_, value) in object(field(&doc, "invariants")?)? {
        require(value == &Json::Bool(true), "all sealed ownership / codec / authority invariants required")?;
    }
    let wire = field(&doc, "wire_profile")?;
    require(
        field(wire, "messagepack_wire_major")? == &Json::Number("2".into()),
        "unreviewed actor wire version",
    )?;
    let envelope = string_array(field(wire, "messagepack_envelope_fields")?)?;
    require(
        envelope == ["wire_major", "schema_id", "shape_sha256", "payload"],
        "unreviewed actor frame shape",
    )?;
    let actor_forbid: BTreeSet<&str> = string_array(field(actor, "forbid")?)?.into_iter().collect();
    require(actor_forbid.contains("unproven_option_result"), "unproven tagged actor payload must reject")?;
    let control_forbid: BTreeSet<&str> = string_array(field(control, "forbid")?)?.into_iter().collect();
    require(control_forbid.contains("actor_data_envelope"), "CONTROL cannot be serialized as DATA")?;
    Ok(())
}

fn verify_reactive_doc(doc: &Json) -> Result<(), String> {
    require(string(field(doc, "contract")?)? == "oreslang.reactive-signal.v1", "wrong reactive contract id")?;
    require(string(field(doc, "consumer_signal")?)? == "Result<Option<T>, Option<E>>", "consumer signal ABI drift")?;
    require(string(field(doc, "producer_frame")?)? == "Result<Option<T>, E>", "producer frame ABI drift")?;
    let variants = field(doc, "consumer_variants")?;
    exact_keys(variants, &["NEXT", "COMPLETE", "CANCELLED", "ERROR"])?;
    for (key, value) in [
        ("NEXT", "Ok(Some(T))"),
        ("COMPLETE", "Ok(None)"),
        ("CANCELLED", "Err(None)"),
        ("ERROR", "Err(Some(E))"),
    ] {
        require(string(field(variants, key)?)? == value, "four-way consumer variant mapping drift")?;
    }

    let features = field(doc, "features")?;
    let feature_names = [
        "canonical_consumer_signal",
        "producer_frame",
        "channel_source",
        "future_source",
        "mixed_channel_future_select",
        "observable",
        "credit_pipe",
    ];
    exact_keys(features, &feature_names)?;
    for name in feature_names {
        let status = string(field(features, name)?)?;
        require(
            matches!(status, "implemented" | "syntax_only" | "unsupported"),
            "invalid feature status",
        )?;
    }

    require(
        string(field(doc, "unsupported_policy")?)? == "reject_before_lowering",
        "unsupported reactive source must fail closed before lowering",
    )?;
    require(string(field(doc, "backend")?)? == "wasm", "backend identity drift")?;

    let semantics = field(doc, "semantics")?;
    let semantic_names = [
        "cancellation_is_consumer_control",
        "nested_option_is_data",
        "nested_result_err_is_data",
        "borrowed_producer_not_cancelled_by_adapter",
        "pending_writes_must_be_bounded",
    ];
    exact_keys(semantics, &semantic_names)?;
    for name in semantic_names {
        require(field(semantics, name)? == &Json::Bool(true), "reactive semantics drift")?;
    }

    let evidence = field(doc, "executed_evidence")?;
    require(object(evidence)?.is_empty(), "metadata cannot certify reactive implementation")?;
    for name in feature_names {
        require(
            string(field(features, name)?)? == "unsupported",
            "metadata cannot certify reactive implementation",
        )?;
    }
    Ok(())
}

fn verify_reactive(root: &Path) -> Result<(), String> {
    let path = root.join("contracts/v1/reactive-signal-v1.json");
    let raw = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let doc = parse_json(&raw)?;
    verify_reactive_doc(&doc)
}

#[cfg(not(test))]
fn usage() -> ! {
    eprintln!("usage: contract_gate <integration|serde|reactive> [repository-root]");
    process::exit(2);
}

#[cfg(not(test))]
fn main() {
    let mut args = env::args_os().skip(1);
    let command = args.next().and_then(|s| s.into_string().ok()).unwrap_or_else(|| usage());
    let root = args.next().map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    if args.next().is_some() {
        usage();
    }
    let result = match command.as_str() {
        "integration" => verify_integration_root(&root),
        "serde" => verify_serde(&root),
        "reactive" => verify_reactive(&root),
        _ => usage(),
    };
    match result {
        Ok(()) => println!("{command}: OK"),
        Err(error) => {
            eprintln!("{command}: DENIED: {error}");
            process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn integration_json(repo: &str) -> Vec<u8> {
        let deps = upstreams(repo).expect("known repo");
        let deps = deps
            .iter()
            .map(|s| format!("\"{s}\""))
            .collect::<Vec<_>>()
            .join(",");
        format!(
            "{{\"schemaVersion\":1,\"protocol\":\"{INTEGRATION_PROTOCOL}\",\"repository\":\"ores-truffle-oreslang/{repo}\",\"role\":\"compiler\",\"upstreams\":[{deps}],\"evidence\":\"not-attested\",\"note\":\"{INTEGRATION_NOTE}\"}}"
        )
        .into_bytes()
    }

    #[test]
    fn sha256_known_vector() {
        assert_eq!(
            hex(&sha256(b"abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn integration_all_backends() {
        for repo in [
            "oreslang-source.java",
            "oreslang-source.llvm",
            "oreslang-source.wasm",
            "oreslang-source.js",
            "oreslang-source.hdl",
            "oreslang-source.forge",
            "oreslang-source.net",
        ] {
            verify_integration_bytes(&integration_json(repo), repo).unwrap();
        }
    }

    #[test]
    fn integration_adversarial_inputs_fail_closed() {
        let raw = integration_json("oreslang-source.java");
        let mut duplicate = raw.clone();
        duplicate.pop();
        duplicate.extend_from_slice(b",\"evidence\":\"not-attested\"}");
        assert!(verify_integration_bytes(&duplicate, "oreslang-source.java").is_err());
        assert!(verify_integration_bytes(&raw, "oreslang-source.invented").is_err());
        assert!(verify_integration_bytes(b"{\"schemaVersion\":NaN}", "oreslang-source.java").is_err());
        assert!(verify_integration_bytes(&vec![b' '; 4097], "oreslang-source.java").is_err());
        let mut bom = vec![0xef, 0xbb, 0xbf];
        bom.extend_from_slice(&raw);
        assert!(verify_integration_bytes(&bom, "oreslang-source.java").is_err());
        let mut bad_utf8 = vec![0xff];
        bad_utf8.extend_from_slice(&raw);
        assert!(verify_integration_bytes(&bad_utf8, "oreslang-source.java").is_err());
    }

    #[test]
    fn parser_rejects_duplicate_keys_and_non_json_constants() {
        assert!(parse_json(b"{\"x\":1,\"x\":1}").is_err());
        assert!(parse_json(b"{\"x\":Infinity}").is_err());
        assert!(parse_json(b"NaN").is_err());
    }

    #[test]
    fn reactive_real_contract_and_forged_promotion() {
        let raw = fs::read("contracts/v1/reactive-signal-v1.json").expect("reactive contract");
        let good = parse_json(&raw).expect("valid JSON");
        verify_reactive_doc(&good).unwrap();

        let mut forged = good.clone();
        let Json::Object(root) = &mut forged else { panic!("root object") };
        let features = root.iter_mut().find(|(k, _)| k == "features").expect("features");
        let Json::Object(items) = &mut features.1 else { panic!("features object") };
        items.iter_mut().find(|(k, _)| k == "observable").expect("observable").1 =
            Json::String("implemented".into());
        assert!(verify_reactive_doc(&forged).is_err());
    }

    #[test]
    fn real_contracts_pass() {
        verify_serde(Path::new(".")).unwrap();
        verify_reactive(Path::new(".")).unwrap();
        verify_integration_root(Path::new(".")).unwrap();
    }
}
