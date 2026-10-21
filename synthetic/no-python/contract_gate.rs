use std::{collections::{BTreeMap, BTreeSet}, fs, path::Path};

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


fn rol(value: u32, n: u32) -> u32 {
    (value << n) | (value >> (32 - n))
}

fn sha1(bytes: &[u8]) -> [u8; 20] {
    let mut h = [
        0x67452301u32,
        0xefcdab89,
        0x98badcfe,
        0x10325476,
        0xc3d2e1f0,
    ];
    let bit_len = (bytes.len() as u64) * 8;
    let mut data = bytes.to_vec();
    data.push(0x80);
    while data.len() % 64 != 56 {
        data.push(0);
    }
    data.extend_from_slice(&bit_len.to_be_bytes());
    for chunk in data.chunks_exact(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[i * 4..i * 4 + 4].try_into().expect("4-byte word"));
        }
        for i in 16..80 {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, word) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | ((!b) & d), 0x5a827999),
                20..=39 => (b ^ c ^ d, 0x6ed9eba1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8f1bbcdc),
                _ => (b ^ c ^ d, 0xca62c1d6),
            };
            let temp = rol(a, 5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(*word);
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        for (slot, value) in h.iter_mut().zip([a, b, c, d, e]) {
            *slot = slot.wrapping_add(value);
        }
    }
    let mut out = [0u8; 20];
    for (i, value) in h.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&value.to_be_bytes());
    }
    out
}

fn git_blob_sha1(bytes: &[u8]) -> String {
    let mut framed = format!("blob {}\0", bytes.len()).into_bytes();
    framed.extend_from_slice(bytes);
    hex(&sha1(&framed))
}

const STRUCTURAL_EXPECTED: [(&str, &str); 30] = [
    ("syn.explicit", "accept"),
    ("syn.shorthand", "accept"),
    ("syn.structural", "accept"),
    ("syn.optional.middle", "reject"),
    ("syn.optional.value", "accept"),
    ("shape.two", "accept"),
    ("shape.three", "accept"),
    ("shape.four", "accept"),
    ("shape.direct", "accept"),
    ("shape.one", "reject"),
    ("shape.wrong.required", "reject"),
    ("shape.wrong.optional", "reject"),
    ("shape.exact.extra", "reject"),
    ("shape.non.tuple", "reject"),
    ("shape.unknown.spread", "reject"),
    ("shape.private.extra", "reject"),
    ("overload.arity.first", "accept"),
    ("overload.same.arity", "reject"),
    ("overload.primitive", "reject"),
    ("overload.ambiguous.pack", "reject"),
    ("safety.mutable", "reject"),
    ("safety.async", "reject"),
    ("safety.actor", "reject"),
    ("safety.fixed.array", "reject"),
    ("safety.eval.once", "accept"),
    ("safety.no.implicit.pack", "reject"),
    ("safety.tuple.width.65", "reject"),
    ("safety.family.size.129", "reject"),
    ("safety.unknown.slot", "reject"),
    ("safety.mutable.borrow.slot", "reject"),
];

fn verify_structural_doc(doc: &Json, readme: &[u8], manifest: &[u8]) -> Result<(), String> {
    require(
        git_blob_sha1(readme) == "1a61b6267fd2185879628d7879db05804698a310",
        "v2 README Git blob drift",
    )?;
    require(
        git_blob_sha1(manifest) == "57c80adaa4029c6fd3e46a63c490cb193f951568",
        "v2 cases Git blob drift",
    )?;
    exact_keys(doc, &["contract", "status", "backend_ids", "cases"])?;
    require(
        string(field(doc, "contract")?)? == "oreslang.structural-positional-tuple.v2",
        "v2 contract name changed",
    )?;
    require(string(field(doc, "status")?)? == "target-only", "runtime parity promoted without proof")?;
    require(
        string_array(field(doc, "backend_ids")?)? == ["java", "llvm", "net", "wasm", "forge"],
        "backend order/scope drift",
    )?;
    let cases = array(field(doc, "cases")?)?;
    require(cases.len() == STRUCTURAL_EXPECTED.len(), "missing or extra v2 case")?;
    for (index, (case, expected)) in cases.iter().zip(STRUCTURAL_EXPECTED).enumerate() {
        exact_keys(case, &["id", "expect", "source", "reason"])?;
        require(
            string(field(case, "id")?)? == expected.0
                && string(field(case, "expect")?)? == expected.1,
            &format!("changed v2 case identity/order/outcome: {index}"),
        )?;
        for key in ["source", "reason"] {
            require(
                !string(field(case, key)?)?.trim().is_empty(),
                &format!("missing v2 {key}: {index}"),
            )?;
        }
    }
    Ok(())
}

fn structural_mutation_suite(good: &Json, readme: &[u8], manifest: &[u8]) -> Result<usize, String> {
    let mut rejected = 0usize;
    let check = |name: &str, candidate: Json, md: &[u8], raw: &[u8], rejected: &mut usize| -> Result<(), String> {
        if verify_structural_doc(&candidate, md, raw).is_ok() {
            Err(format!("structural mutation escaped gate: {name}"))
        } else {
            *rejected += 1;
            Ok(())
        }
    };

    {
        let mut d = good.clone();
        array_mut(field_mut(&mut d, "cases")?)?.pop();
        check("case removed", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let cases = array_mut(field_mut(&mut d, "cases")?)?;
        cases.push(cases[0].clone());
        check("case duplicated", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        array_mut(field_mut(&mut d, "cases")?)?.reverse();
        check("case order reversed", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let cases = array_mut(field_mut(&mut d, "cases")?)?;
        *field_mut(&mut cases[0], "expect")? = Json::String("reject".into());
        check("outcome changed", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let cases = array_mut(field_mut(&mut d, "cases")?)?;
        *field_mut(&mut cases[0], "source")? = Json::String(String::new());
        check("source emptied", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        *field_mut(&mut d, "status")? = Json::String("verified".into());
        check("status promoted", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        array_mut(field_mut(&mut d, "backend_ids")?)?.pop();
        check("backend removed", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        object_mut(&mut d)?.push(("unknown".into(), Json::String("unreviewed".into())));
        check("unknown field", d, readme, manifest, &mut rejected)?;
    }
    {
        let mut md = readme.to_vec();
        md.push(b'\n');
        check("README drift", good.clone(), &md, manifest, &mut rejected)?;
    }
    {
        let mut raw = manifest.to_vec();
        raw.push(b'\n');
        check("manifest drift", good.clone(), readme, &raw, &mut rejected)?;
    }
    require(rejected == 10, "structural mutation inventory drift")?;
    for raw in [
        b"{\"id\":\"x\",\"id\":\"x\"}".as_slice(),
        b"NaN".as_slice(),
        b"[]".as_slice(),
        b"{\"x\":Infinity}".as_slice(),
    ] {
        require(parse_json(raw).is_err(), "malformed structural JSON accepted")?;
    }
    Ok(rejected + 4)
}

fn verify_structural_v2(root: &Path) -> Result<(), String> {
    let readme_path = root.join("contracts/structural/v2/README.md");
    let cases_path = root.join("contracts/structural/v2/cases.json");
    let readme = fs::read(&readme_path).map_err(|e| format!("read {}: {e}", readme_path.display()))?;
    let manifest = fs::read(&cases_path).map_err(|e| format!("read {}: {e}", cases_path.display()))?;
    let doc = parse_json(&manifest)?;
    verify_structural_doc(&doc, &readme, &manifest)?;
    let rejected = structural_mutation_suite(&doc, &readme, &manifest)?;
    require(rejected == 14, "structural self-test count drift")
}


struct I64Expr<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> I64Expr<'a> {
    fn new(text: &'a str) -> Self {
        Self { bytes: text.as_bytes(), at: 0 }
    }

    fn parse(mut self) -> Result<i64, String> {
        let value = self.expr()?;
        if self.at != self.bytes.len() {
            return Err("unsupported source numeric expression".into());
        }
        Ok(value)
    }

    fn expr(&mut self) -> Result<i64, String> {
        let mut value = self.term()?;
        loop {
            match self.bytes.get(self.at) {
                Some(b'+') => {
                    self.at += 1;
                    value = value
                        .checked_add(self.term()?)
                        .ok_or_else(|| "i64 intermediate overflow".to_string())?;
                }
                Some(b'-') => {
                    self.at += 1;
                    value = value
                        .checked_sub(self.term()?)
                        .ok_or_else(|| "i64 intermediate overflow".to_string())?;
                }
                _ => return Ok(value),
            }
        }
    }

    fn term(&mut self) -> Result<i64, String> {
        let mut value = self.unary()?;
        while self.bytes.get(self.at) == Some(&b'*') {
            self.at += 1;
            value = value
                .checked_mul(self.unary()?)
                .ok_or_else(|| "i64 intermediate overflow".to_string())?;
        }
        Ok(value)
    }

    fn unary(&mut self) -> Result<i64, String> {
        if self.bytes.get(self.at) == Some(&b'-') {
            self.at += 1;
            let start = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
                self.at += 1;
            }
            if self.at > start {
                let raw = std::str::from_utf8(&self.bytes[start..self.at])
                    .map_err(|_| "invalid integer literal".to_string())?;
                let magnitude: u128 = raw.parse().map_err(|_| "invalid integer literal".to_string())?;
                if magnitude == (1u128 << 63) {
                    return Ok(i64::MIN);
                }
                if magnitude > i64::MAX as u128 {
                    return Err("out of literal bounds".into());
                }
                return (magnitude as i64)
                    .checked_neg()
                    .ok_or_else(|| "i64 intermediate overflow".to_string());
            }
            let value = self.unary()?;
            return value
                .checked_neg()
                .ok_or_else(|| "i64 intermediate overflow".to_string());
        }
        self.primary()
    }

    fn primary(&mut self) -> Result<i64, String> {
        if self.bytes.get(self.at) == Some(&b'(') {
            self.at += 1;
            let value = self.expr()?;
            if self.bytes.get(self.at) != Some(&b')') {
                return Err("unclosed numeric group".into());
            }
            self.at += 1;
            return Ok(value);
        }
        let start = self.at;
        while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) {
            self.at += 1;
        }
        if self.at == start {
            return Err("unsupported source numeric expression".into());
        }
        let raw = std::str::from_utf8(&self.bytes[start..self.at])
            .map_err(|_| "invalid integer literal".to_string())?;
        let magnitude: u128 = raw.parse().map_err(|_| "invalid integer literal".to_string())?;
        if magnitude > i64::MAX as u128 {
            return Err("out of literal bounds".into());
        }
        Ok(magnitude as i64)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct DecimalExact {
    negative: bool,
    digits: Vec<u8>,
    exponent10: i32,
}

impl DecimalExact {
    fn zero() -> Self {
        Self { negative: false, digits: vec![0], exponent10: 0 }
    }

    fn parse(text: &str) -> Result<Self, String> {
        if text.is_empty() {
            return Err("empty decimal".into());
        }
        let (negative, rest) = if let Some(value) = text.strip_prefix('-') {
            (true, value)
        } else if let Some(value) = text.strip_prefix('+') {
            (false, value)
        } else {
            (false, text)
        };
        if rest.is_empty() {
            return Err("invalid decimal".into());
        }
        let (mantissa, exponent_part) = match rest.find(['e', 'E']) {
            Some(index) => (&rest[..index], Some(&rest[index + 1..])),
            None => (rest, None),
        };
        let exponent: i32 = match exponent_part {
            Some(value) if !value.is_empty() => value.parse().map_err(|_| "invalid decimal exponent".to_string())?,
            Some(_) => return Err("invalid decimal exponent".into()),
            None => 0,
        };
        let mut coefficient = String::new();
        let mut fractional = 0i32;
        let mut dot_seen = false;
        for ch in mantissa.chars() {
            match ch {
                '0'..='9' => {
                    coefficient.push(ch);
                    if dot_seen {
                        fractional += 1;
                    }
                }
                '.' if !dot_seen => dot_seen = true,
                _ => return Err("invalid exact decimal".into()),
            }
        }
        if coefficient.is_empty() {
            return Err("invalid exact decimal".into());
        }
        let trimmed = coefficient.trim_start_matches('0');
        if trimmed.is_empty() {
            return Ok(Self::zero());
        }
        let digits = trimmed.bytes().rev().map(|b| b - b'0').collect();
        let mut value = Self {
            negative,
            digits,
            exponent10: exponent - fractional,
        };
        value.normalize();
        Ok(value)
    }

    fn normalize(&mut self) {
        while self.digits.len() > 1 && self.digits.last() == Some(&0) {
            self.digits.pop();
        }
        while self.digits.len() > 1 && self.digits.first() == Some(&0) {
            self.digits.remove(0);
            self.exponent10 += 1;
        }
        if self.digits.iter().all(|digit| *digit == 0) {
            *self = Self::zero();
        }
    }

    fn shifted_digits(&self, exponent: i32) -> Vec<u8> {
        let shift = usize::try_from(self.exponent10 - exponent).expect("nonnegative decimal shift");
        let mut out = vec![0; shift];
        out.extend_from_slice(&self.digits);
        out
    }

    fn cmp_abs_digits(a: &[u8], b: &[u8]) -> std::cmp::Ordering {
        let mut a_len = a.len();
        while a_len > 1 && a[a_len - 1] == 0 { a_len -= 1; }
        let mut b_len = b.len();
        while b_len > 1 && b[b_len - 1] == 0 { b_len -= 1; }
        match a_len.cmp(&b_len) {
            std::cmp::Ordering::Equal => {
                for i in (0..a_len).rev() {
                    match a[i].cmp(&b[i]) {
                        std::cmp::Ordering::Equal => {}
                        other => return other,
                    }
                }
                std::cmp::Ordering::Equal
            }
            other => other,
        }
    }

    fn add_abs(a: &[u8], b: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(a.len().max(b.len()) + 1);
        let mut carry = 0u8;
        for i in 0..a.len().max(b.len()) {
            let sum = a.get(i).copied().unwrap_or(0) + b.get(i).copied().unwrap_or(0) + carry;
            out.push(sum % 10);
            carry = sum / 10;
        }
        if carry != 0 { out.push(carry); }
        out
    }

    fn sub_abs(a: &[u8], b: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(a.len());
        let mut borrow = 0i16;
        for i in 0..a.len() {
            let mut value = i16::from(a[i]) - borrow - i16::from(b.get(i).copied().unwrap_or(0));
            if value < 0 {
                value += 10;
                borrow = 1;
            } else {
                borrow = 0;
            }
            out.push(value as u8);
        }
        while out.len() > 1 && out.last() == Some(&0) { out.pop(); }
        out
    }

    fn add(&self, other: &Self) -> Self {
        let exponent = self.exponent10.min(other.exponent10);
        let a = self.shifted_digits(exponent);
        let b = other.shifted_digits(exponent);
        let (negative, digits) = if self.negative == other.negative {
            (self.negative, Self::add_abs(&a, &b))
        } else {
            match Self::cmp_abs_digits(&a, &b) {
                std::cmp::Ordering::Greater => (self.negative, Self::sub_abs(&a, &b)),
                std::cmp::Ordering::Less => (other.negative, Self::sub_abs(&b, &a)),
                std::cmp::Ordering::Equal => return Self::zero(),
            }
        };
        let mut result = Self { negative, digits, exponent10: exponent };
        result.normalize();
        result
    }

    fn mul(&self, other: &Self) -> Self {
        if self == &Self::zero() || other == &Self::zero() {
            return Self::zero();
        }
        let mut accum = vec![0u16; self.digits.len() + other.digits.len()];
        for (i, a) in self.digits.iter().enumerate() {
            for (j, b) in other.digits.iter().enumerate() {
                accum[i + j] += u16::from(*a) * u16::from(*b);
            }
        }
        for i in 0..accum.len() - 1 {
            let carry = accum[i] / 10;
            accum[i] %= 10;
            accum[i + 1] += carry;
        }
        while accum.last().is_some_and(|value| *value >= 10) {
            let last = accum.len() - 1;
            let carry = accum[last] / 10;
            accum[last] %= 10;
            accum.push(carry);
        }
        let digits = accum.into_iter().map(|value| value as u8).collect();
        let mut result = Self {
            negative: self.negative ^ other.negative,
            digits,
            exponent10: self.exponent10 + other.exponent10,
        };
        result.normalize();
        result
    }
}

#[derive(Clone, Copy)]
enum NumericOperand {
    Int(i64),
    FloatBits(u64),
}

fn parse_hex_u64(text: &str) -> Result<u64, String> {
    if text.len() != 16 || !text.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("binary64 bits must be 8 bytes".into());
    }
    u64::from_str_radix(text, 16).map_err(|_| "invalid binary64 bits".to_string())
}

fn numeric_operand(value: &Json) -> Result<NumericOperand, String> {
    match string(field(value, "kind")?)? {
        "int64" => {
            let value: i64 = string(field(value, "decimal")?)?
                .parse()
                .map_err(|_| "int64 out of range".to_string())?;
            Ok(NumericOperand::Int(value))
        }
        "bits" => Ok(NumericOperand::FloatBits(parse_hex_u64(string(field(value, "hex")?)?)?)),
        _ => Err("unknown numeric operand kind".into()),
    }
}

fn float_is_nan(bits: u64) -> bool {
    ((bits >> 52) & 0x7ff) == 0x7ff && (bits & ((1u64 << 52) - 1)) != 0
}

fn compare_abs_int_float(integer: u64, bits: u64) -> std::cmp::Ordering {
    let exponent_bits = ((bits >> 52) & 0x7ff) as i32;
    let fraction = bits & ((1u64 << 52) - 1);
    if exponent_bits == 0x7ff {
        return std::cmp::Ordering::Less;
    }
    if exponent_bits == 0 && fraction == 0 {
        return integer.cmp(&0);
    }
    let (mantissa, exponent2) = if exponent_bits == 0 {
        (fraction, -1074)
    } else {
        ((1u64 << 52) | fraction, exponent_bits - 1023 - 52)
    };
    if exponent2 >= 0 {
        match (mantissa as u128).checked_shl(exponent2 as u32) {
            Some(float_integer) => (integer as u128).cmp(&float_integer),
            None => std::cmp::Ordering::Less,
        }
    } else {
        let shift = (-exponent2) as u32;
        match (integer as u128).checked_shl(shift) {
            Some(scaled_integer) => scaled_integer.cmp(&(mantissa as u128)),
            None => std::cmp::Ordering::Greater,
        }
    }
}

fn compare_int_float(integer: i64, bits: u64) -> Option<std::cmp::Ordering> {
    if float_is_nan(bits) {
        return None;
    }
    let negative_float = (bits >> 63) != 0;
    let exponent_bits = (bits >> 52) & 0x7ff;
    let fraction = bits & ((1u64 << 52) - 1);
    if exponent_bits == 0x7ff && fraction == 0 {
        return Some(if negative_float {
            std::cmp::Ordering::Greater
        } else {
            std::cmp::Ordering::Less
        });
    }
    if integer < 0 && !negative_float {
        return Some(std::cmp::Ordering::Less);
    }
    if integer >= 0 && negative_float && (bits & 0x7fff_ffff_ffff_ffff) != 0 {
        return Some(std::cmp::Ordering::Greater);
    }
    let abs_cmp = compare_abs_int_float(integer.unsigned_abs(), bits & 0x7fff_ffff_ffff_ffff);
    Some(if integer < 0 && negative_float { abs_cmp.reverse() } else { abs_cmp })
}

fn compare_numeric(left: NumericOperand, right: NumericOperand) -> Option<std::cmp::Ordering> {
    match (left, right) {
        (NumericOperand::Int(a), NumericOperand::Int(b)) => Some(a.cmp(&b)),
        (NumericOperand::Int(a), NumericOperand::FloatBits(b)) => compare_int_float(a, b),
        (NumericOperand::FloatBits(a), NumericOperand::Int(b)) => compare_int_float(b, a).map(|o| o.reverse()),
        (NumericOperand::FloatBits(a), NumericOperand::FloatBits(b)) => {
            f64::from_bits(a).partial_cmp(&f64::from_bits(b))
        }
    }
}

fn json_bool(value: &Json) -> Result<bool, String> {
    match value {
        Json::Bool(value) => Ok(*value),
        _ => Err("expected JSON boolean".into()),
    }
}

fn shape(value: &Json, explicit: Option<&Json>) -> Result<(usize, usize), String> {
    let rows = array(value)?;
    let row_count = rows.len();
    let column_count = if let Some(first) = rows.first() {
        array(first)?.len()
    } else if let Some(explicit) = explicit {
        let dims = array(explicit)?;
        if dims.len() != 2 { return Err("shape must have two dimensions".into()); }
        let Json::Number(columns) = &dims[1] else { return Err("shape dimension must be integer".into()); };
        columns.parse::<usize>().map_err(|_| "invalid shape dimension".to_string())?
    } else {
        0
    };
    for row in rows {
        require(array(row)?.len() == column_count, "ragged matrix")?;
    }
    if let Some(explicit) = explicit {
        let dims = array(explicit)?;
        if dims.len() != 2 { return Err("shape must have two dimensions".into()); }
        let parse_dim = |value: &Json| -> Result<usize, String> {
            let Json::Number(raw) = value else { return Err("shape dimension must be integer".into()); };
            raw.parse().map_err(|_| "invalid shape dimension".to_string())
        };
        require(
            parse_dim(&dims[0])? == row_count && parse_dim(&dims[1])? == column_count,
            "declared shape differs from data",
        )?;
    }
    Ok((row_count, column_count))
}

fn explicit_field<'a>(value: &'a Json, key: &str) -> Option<&'a Json> {
    object(value).ok()?.iter().find(|(name, _)| name == key).map(|(_, value)| value)
}

fn matrix_values(value: &Json) -> Result<Vec<Vec<DecimalExact>>, String> {
    array(value)?
        .iter()
        .map(|row| {
            array(row)?
                .iter()
                .map(|entry| DecimalExact::parse(string(entry)?))
                .collect::<Result<Vec<_>, String>>()
        })
        .collect()
}

fn exact_matrix_product(case: &Json) -> Result<(Vec<Vec<DecimalExact>>, (usize, usize)), String> {
    let lhs_json = field(case, "lhs")?;
    let rhs_json = field(case, "rhs")?;
    let (m, k) = shape(lhs_json, explicit_field(case, "lhs_shape"))?;
    let (n_rows, n) = shape(rhs_json, explicit_field(case, "rhs_shape"))?;
    require(k == n_rows, "shape_mismatch")?;
    let lhs = matrix_values(lhs_json)?;
    let rhs = matrix_values(rhs_json)?;
    let mut product = vec![vec![DecimalExact::zero(); n]; m];
    for i in 0..m {
        for j in 0..n {
            let mut total = DecimalExact::zero();
            for p in 0..k {
                total = total.add(&lhs[i][p].mul(&rhs[p][j]));
            }
            product[i][j] = total;
        }
    }
    Ok((product, (m, n)))
}

fn verify_numeric_precision(doc: &Json) -> Result<(), String> {
    require(string(field(doc, "schema_version")?)? == "1.0.0", "numeric schema version drift")?;
    require(
        string(field(doc, "status")?)? == "reference-only-not-guest-execution",
        "numeric reference status drift",
    )?;
    for case in array(field(doc, "binary64_comparisons")?)? {
        let ordering = compare_numeric(numeric_operand(field(case, "lhs")?)?, numeric_operand(field(case, "rhs")?)?);
        let actual = [
            ("eq", ordering == Some(std::cmp::Ordering::Equal)),
            ("lt", ordering == Some(std::cmp::Ordering::Less)),
            ("le", matches!(ordering, Some(std::cmp::Ordering::Less | std::cmp::Ordering::Equal))),
            ("gt", ordering == Some(std::cmp::Ordering::Greater)),
            ("ge", matches!(ordering, Some(std::cmp::Ordering::Greater | std::cmp::Ordering::Equal))),
        ];
        for (key, value) in actual {
            require(json_bool(field(case, key)?)? == value, "binary64 comparison vector drift")?;
        }
    }
    for case in array(field(doc, "decimal_matrices")?)? {
        let expected_error = explicit_field(case, "error").map(string).transpose()?;
        if expected_error == Some("shape_mismatch") {
            require(exact_matrix_product(case).is_err(), "shape mismatch matrix unexpectedly accepted")?;
            continue;
        }
        let (actual, actual_shape) = exact_matrix_product(case)?;
        let expected_json = field(case, "expected")?;
        let expected_shape = shape(expected_json, explicit_field(case, "expected_shape"))?;
        require(actual_shape == expected_shape, "matrix result shape drift")?;
        let expected = matrix_values(expected_json)?;
        require(actual == expected, "exact decimal matrix vector drift")?;
    }
    Ok(())
}

fn verify_numeric_source_i64(raw: &[u8], doc: &Json) -> Result<(), String> {
    require(
        git_blob_sha1(raw) == "bbc319c19ecd007109d33592e210091eb267e5a2",
        "source i64 fixture Git blob drift",
    )?;
    require(string(field(doc, "schema_version")?)? == "1.0.0", "source i64 schema drift")?;
    require(
        string(field(doc, "status")?)? == "reference-only-not-compiled-execution",
        "source i64 status drift",
    )?;
    let cases = array(field(doc, "cases")?)?;
    require(cases.len() == 7, "source i64 case inventory drift")?;
    let mut ids = BTreeSet::new();
    for case in cases {
        require(ids.insert(string(field(case, "id")?)?), "duplicate source i64 case id")?;
        let expression = string(field(case, "expression")?)?;
        let expected: i64 = string(field(case, "expected")?)?
            .parse()
            .map_err(|_| "invalid expected i64".to_string())?;
        require(I64Expr::new(expression).parse()? == expected, "source i64 oracle drift")?;
    }
    for (expression, expected) in [
        ("-9223372036854775808", i64::MIN),
        ("-9223372036854775808+1", i64::MIN + 1),
        ("-9223372036854775807-1", i64::MIN),
    ] {
        require(I64Expr::new(expression).parse()? == expected, "i64 minimum handling drift")?;
    }
    for expression in [
        "9223372036854775808",
        "9223372036854775808-1",
        "(9223372036854775808)-1",
        "0-9223372036854775808",
        "9223372036854775807+1",
        "-9223372036854775808-1",
        "-(-9223372036854775808)",
        "--9223372036854775808",
        "-9223372036854775808*-1",
        "3037000500*3037000500",
        "+1",
        "1/2",
        "True",
        "1.0",
    ] {
        require(I64Expr::new(expression).parse().is_err(), "invalid i64 source expression admitted")?;
    }
    Ok(())
}

fn verify_numeric_backend(root: &Path, precision_raw: &[u8], precision: &Json) -> Result<(), String> {
    require(
        git_blob_sha1(precision_raw) == "cb28b5678d380f02c5e4e43bd763da04bdb996fe",
        "reference vectors changed without cross-backend review",
    )?;
    verify_numeric_precision(precision)?;
    let backend_path = root.join("contracts/v1/numeric-parity/backend.json");
    let backend_raw = fs::read(&backend_path).map_err(|e| format!("read {}: {e}", backend_path.display()))?;
    let backend = parse_json(&backend_raw)?;
    require(string(field(&backend, "schema_version")?)? == "1.0.0", "numeric backend schema drift")?;
    require(string(field(&backend, "backend")?)? == "wasm", "numeric backend identity drift")?;
    require(
        string(field(&backend, "reference_fixture_git_blob_sha")?)?
            == "cb28b5678d380f02c5e4e43bd763da04bdb996fe",
        "numeric precision fixture pin drift",
    )?;
    require(
        string(field(&backend, "source_i64_fixture_git_blob_sha")?)?
            == "bbc319c19ecd007109d33592e210091eb267e5a2",
        "source i64 fixture pin drift",
    )?;
    let features = [
        "source_i64",
        "checked_i64_overflow",
        "binary64_ordering",
        "binary32_arithmetic",
        "bigint_exact",
        "decimal_exact",
        "complex_binary64",
        "decimal_matrix",
        "source_division_to_float",
    ];
    let capabilities = field(&backend, "capabilities")?;
    let evidence = field(&backend, "evidence")?;
    exact_keys(capabilities, &features)?;
    exact_keys(evidence, &features)?;
    let statuses = BTreeSet::from([
        "unsupported_explicit",
        "delegated_unverified",
        "implemented_unverified",
        "reference_only",
        "blocked_unsound",
        "needs_audit",
        "verified_compiled",
    ]);
    for feature in features {
        let status = string(field(capabilities, feature)?)?;
        require(statuses.contains(status), "unknown numeric capability status")?;
        require(status != "verified_compiled", "compiled parity requires independently verified protected CI evidence")?;
        require(object(field(evidence, feature)?)?.is_empty(), "unverified feature cannot attach self-reported test evidence")?;
    }
    require(
        string(field(capabilities, "source_i64")?)? == "delegated_unverified",
        "Wasm source_i64 capability scope drift",
    )?;
    Ok(())
}

fn verify_numeric(root: &Path) -> Result<(), String> {
    let precision_path = root.join("contracts/v1/numeric-precision/cases.json");
    let precision_raw = fs::read(&precision_path).map_err(|e| format!("read {}: {e}", precision_path.display()))?;
    let precision = parse_json(&precision_raw)?;
    verify_numeric_backend(root, &precision_raw, &precision)?;

    let source_path = root.join("contracts/v1/numeric-parity/source-i64-cases.json");
    let source_raw = fs::read(&source_path).map_err(|e| format!("read {}: {e}", source_path.display()))?;
    let source = parse_json(&source_raw)?;
    verify_numeric_source_i64(&source_raw, &source)
}


const ARENA_BACKENDS: [&str; 5] = ["java", "llvm", "net", "wasm", "forge"];
const ARENA_INVARIANTS: [&str; 24] = [
    "actor-private-by-default",
    "actorref-never-grants-arena-access",
    "persistent-outlives-turn-not-retirement",
    "scratch-invalid-after-every-turn-exit",
    "stale-generation-and-wrong-incarnation-rejected",
    "no-guest-raw-pointer-or-memorysegment-exposure",
    "logical-retirement-revokes-new-authority-before-quiescence",
    "external-lifetime-pins-grant-no-memory-access",
    "continuation-borrow-ffi-io-owners-must-register-lifetime-pins",
    "physical-reclaim-waits-for-live-turns-borrows-and-external-pins",
    "late-callbacks-cannot-resurrect-retired-authority",
    "external-pin-release-is-idempotent",
    "shared-domain-reclaim-waits-for-members-and-leases",
    "reused-capacity-is-zeroized-before-admission",
    "retirement-is-monotonic-and-idempotent",
    "unsupported-shared-domain-or-general-lowering-fails-closed",
    "quota-charge-precedes-allocation-and-failed-admission-is-accounting-neutral",
    "rejected-construction-does-not-consume-capability-identity",
    "nontrivial-values-drop-exactly-once-before-bulk-reclaim",
    "identity-and-generation-counters-never-wrap-into-live-authority",
    "double-release-and-retire-races-cannot-underflow-accounting",
    "cross-actor-capability-transfer-never-grants-private-arena-authority",
    "snapshot-restore-cannot-revive-retired-or-stale-authority",
    "reclaim-failure-keeps-logical-retirement-and-dirty-capacity-unpublished",
];

fn arena_stage(backend: &str) -> &'static str {
    match backend {
        "java" => "candidate-host-runtime-ffm",
        "llvm" => "candidate-compiled-i64-substrate",
        "net" => "candidate-host-only-unmanaged",
        "wasm" => "candidate-host-only-page-pool",
        "forge" => "candidate-host-only-native-arena",
        _ => "",
    }
}

fn arena_pin_model(backend: &str) -> &'static str {
    match backend {
        "java" => "persistent-region-loan-prototype",
        "llvm" => "native-loan-accounting-prototype",
        "net" | "wasm" | "forge" => "external-lifetime-lease-prototype",
        _ => "",
    }
}

fn arena_shared_domain(backend: &str) -> &'static str {
    match backend {
        "llvm" => "host-prototype-unverified",
        "java" | "net" | "wasm" | "forge" => "unsupported",
        _ => "",
    }
}

fn verify_actor_arena_doc(doc: &Json) -> Result<(), String> {
    exact_keys(
        doc,
        &["contract", "version", "scope", "required_invariants", "backend_support"],
    )?;
    require(
        string(field(doc, "contract")?)? == "oreslang-shared-actor-arenas-v1",
        "contract identity/version mismatch",
    )?;
    require(
        field(doc, "version")? == &Json::Number("1".into()),
        "contract identity/version mismatch",
    )?;
    require(
        string(field(doc, "scope")?)?
            == "source-free lifecycle/capability declarations; not guest runtime, compiler, AOT, callback-wiring, or zero-GC certification",
        "scope must not promote runtime certification",
    )?;
    require(
        string_array(field(doc, "required_invariants")?)? == ARENA_INVARIANTS,
        "actor-arena lifecycle invariants drifted",
    )?;

    let support = field(doc, "backend_support")?;
    exact_keys(support, &ARENA_BACKENDS)?;
    let required = [
        "candidate_stage",
        "general_guest_value_lowering",
        "shared_domain_arena",
        "lifetime_pin_model",
        "continuation_ffi_io_wiring",
        "zero_host_gc_certified",
        "native_aot_certified",
        "exact_head_ci_certified",
    ];
    for backend in ARENA_BACKENDS {
        let row = field(support, backend)?;
        exact_keys(row, &required)?;
        require(
            string(field(row, "candidate_stage")?)? == arena_stage(backend),
            "candidate stage promotion/drift",
        )?;
        require(
            string(field(row, "general_guest_value_lowering")?)? == "unsupported",
            "general guest lowering promoted without proof",
        )?;
        require(
            string(field(row, "shared_domain_arena")?)? == arena_shared_domain(backend),
            "shared domain declaration drift",
        )?;
        require(
            string(field(row, "lifetime_pin_model")?)? == arena_pin_model(backend),
            "lifetime pin model drift",
        )?;
        require(
            string(field(row, "continuation_ffi_io_wiring")?)? == "unverified",
            "callback wiring promoted without proof",
        )?;
        for flag in [
            "zero_host_gc_certified",
            "native_aot_certified",
            "exact_head_ci_certified",
        ] {
            require(
                field(row, flag)? == &Json::Bool(false),
                "unverified certification promoted",
            )?;
        }
    }
    Ok(())
}

fn verify_actor_arena_raw(raw: &[u8]) -> Result<Json, String> {
    require(
        !raw.is_empty() && raw.len() <= 64 * 1024,
        "invalid actor-arena contract bytes",
    )?;
    require(!raw.starts_with(&[0xef, 0xbb, 0xbf]), "BOM forbidden")?;
    let doc = parse_json(raw).map_err(|_| "invalid canonical UTF-8 JSON".to_string())?;
    verify_actor_arena_doc(&doc)?;
    Ok(doc)
}

fn arena_backend_row_mut<'a>(doc: &'a mut Json, backend: &str) -> Result<&'a mut Json, String> {
    field_mut(field_mut(doc, "backend_support")?, backend)
}

fn actor_arena_mutation_suite(base: &Json, raw: &[u8]) -> Result<usize, String> {
    verify_actor_arena_doc(base)?;
    let mut checks = 1usize;

    require(
        verify_actor_arena_raw(b"{\"contract\":\"x\",\"contract\":\"y\"}").is_err(),
        "duplicate wire key accepted",
    )?;
    checks += 1;

    let mut bom = vec![0xef, 0xbb, 0xbf];
    bom.extend_from_slice(raw);
    require(verify_actor_arena_raw(&bom).is_err(), "BOM accepted")?;
    checks += 1;

    let reject = |name: &str, candidate: Json| -> Result<(), String> {
        require(
            verify_actor_arena_doc(&candidate).is_err(),
            &format!("actor arena mutation escaped: {name}"),
        )
    };

    {
        let mut d = base.clone();
        let support = object_mut(field_mut(&mut d, "backend_support")?)?;
        support.retain(|(key, _)| key != "forge");
        reject("missing backend", d)?;
    }
    checks += 1;

    {
        let mut d = base.clone();
        object_mut(field_mut(&mut d, "backend_support")?)?
            .push(("other".into(), Json::Object(Vec::new())));
        reject("unknown backend", d)?;
    }
    checks += 1;

    {
        let mut d = base.clone();
        array_mut(field_mut(&mut d, "required_invariants")?)?.pop();
        reject("invariant drop", d)?;
    }
    checks += 1;

    {
        let mut d = base.clone();
        array_mut(field_mut(&mut d, "required_invariants")?)?.reverse();
        reject("invariant reorder", d)?;
    }
    checks += 1;

    for invariant in [
        "actorref-never-grants-arena-access",
        "scratch-invalid-after-every-turn-exit",
        "logical-retirement-revokes-new-authority-before-quiescence",
        "external-lifetime-pins-grant-no-memory-access",
        "continuation-borrow-ffi-io-owners-must-register-lifetime-pins",
        "physical-reclaim-waits-for-live-turns-borrows-and-external-pins",
        "late-callbacks-cannot-resurrect-retired-authority",
        "external-pin-release-is-idempotent",
        "shared-domain-reclaim-waits-for-members-and-leases",
        "quota-charge-precedes-allocation-and-failed-admission-is-accounting-neutral",
        "rejected-construction-does-not-consume-capability-identity",
        "nontrivial-values-drop-exactly-once-before-bulk-reclaim",
        "identity-and-generation-counters-never-wrap-into-live-authority",
        "double-release-and-retire-races-cannot-underflow-accounting",
        "cross-actor-capability-transfer-never-grants-private-arena-authority",
        "snapshot-restore-cannot-revive-retired-or-stale-authority",
        "reclaim-failure-keeps-logical-retirement-and-dirty-capacity-unpublished",
    ] {
        let mut d = base.clone();
        let invariants = array_mut(field_mut(&mut d, "required_invariants")?)?;
        invariants.retain(|value| value != &Json::String(invariant.into()));
        reject(invariant, d)?;
        checks += 1;
    }

    for (backend, key, value, name) in [
        ("java", "general_guest_value_lowering", "implemented", "java general lowering"),
        ("llvm", "general_guest_value_lowering", "implemented", "llvm general lowering"),
        ("java", "lifetime_pin_model", "external-lifetime-lease-prototype", "java pin model"),
        ("llvm", "lifetime_pin_model", "external-lifetime-lease-prototype", "llvm pin model"),
        ("net", "lifetime_pin_model", "none", "net pin model"),
        ("wasm", "lifetime_pin_model", "none", "wasm pin model"),
        ("forge", "lifetime_pin_model", "none", "forge pin model"),
        ("llvm", "shared_domain_arena", "implemented", "llvm shared domain"),
    ] {
        let mut d = base.clone();
        *field_mut(arena_backend_row_mut(&mut d, backend)?, key)? = Json::String(value.into());
        reject(name, d)?;
        checks += 1;
    }

    {
        let mut all_rejected = true;
        for backend in ["java", "net", "wasm", "forge"] {
            let mut d = base.clone();
            *field_mut(arena_backend_row_mut(&mut d, backend)?, "shared_domain_arena")? =
                Json::String("implemented".into());
            all_rejected &= verify_actor_arena_doc(&d).is_err();
        }
        require(all_rejected, "non-LLVM shared domain promotion escaped")?;
    }
    checks += 1;

    for (key, value, name) in [
        ("continuation_ffi_io_wiring", Json::String("implemented".into()), "callback wiring"),
        ("zero_host_gc_certified", Json::Bool(true), "zero host GC"),
        ("native_aot_certified", Json::Bool(true), "native AOT"),
        ("exact_head_ci_certified", Json::Bool(true), "exact head CI"),
    ] {
        let mut all_rejected = true;
        for backend in ARENA_BACKENDS {
            let mut d = base.clone();
            *field_mut(arena_backend_row_mut(&mut d, backend)?, key)? = value.clone();
            all_rejected &= verify_actor_arena_doc(&d).is_err();
        }
        require(all_rejected, &format!("{name} promotion escaped"))?;
        checks += 1;
    }

    {
        let mut d = base.clone();
        *field_mut(arena_backend_row_mut(&mut d, "llvm")?, "candidate_stage")? =
            Json::String("candidate-host-only-native-arena".into());
        reject("candidate stage drift", d)?;
    }
    checks += 1;

    {
        let mut d = base.clone();
        object_mut(arena_backend_row_mut(&mut d, "net")?)?
            .retain(|(key, _)| key != "lifetime_pin_model");
        reject("missing pin model", d)?;
    }
    checks += 1;

    {
        let mut d = base.clone();
        object_mut(arena_backend_row_mut(&mut d, "forge")?)?
            .push(("callback_magic".into(), Json::Bool(true)));
        reject("unknown backend field", d)?;
    }
    checks += 1;

    require(checks == 40, "actor arena test inventory differs from pinned 40")?;
    Ok(checks)
}

fn verify_actor_arena_lifecycle(root: &Path) -> Result<usize, String> {
    let path = root.join("contracts/v1/actor_arena_lifecycle_v1.json");
    let raw = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let doc = verify_actor_arena_raw(&raw)?;
    actor_arena_mutation_suite(&doc, &raw)
}

fn object_mut(value: &mut Json) -> Result<&mut Vec<(String, Json)>, String> {
    match value {
        Json::Object(v) => Ok(v),
        _ => Err("expected mutable JSON object".into()),
    }
}

fn array_mut(value: &mut Json) -> Result<&mut Vec<Json>, String> {
    match value {
        Json::Array(v) => Ok(v),
        _ => Err("expected mutable JSON array".into()),
    }
}

fn field_mut<'a>(value: &'a mut Json, key: &str) -> Result<&'a mut Json, String> {
    object_mut(value)?
        .iter_mut()
        .find(|(name, _)| name == key)
        .map(|(_, value)| value)
        .ok_or_else(|| format!("missing mutable field: {key}"))
}

#[derive(Clone)]
struct ActorStage {
    role: String,
    inbound: String,
    outbound: String,
}

fn actor_text(value: &Json, allow_empty: bool) -> Result<String, String> {
    let text = string(value)?;
    if (!allow_empty && text.is_empty()) || text.contains('\0') {
        return Err("empty or NUL actor graph component".into());
    }
    if text.len() > 256 {
        return Err("actor graph UTF-8 byte limit".into());
    }
    Ok(text.to_string())
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for &byte in bytes {
        hash = (hash ^ u64::from(byte)).wrapping_mul(0x100000001b3);
    }
    hash
}

fn verify_actor_pipeline_doc(data: &Json) -> Result<(), String> {
    require(
        string(field(data, "schema")?)? == "oreslang.actor-pipeline-routing.v1",
        "wrong routing fixture schema",
    )?;
    let operations = field(data, "operations")?;
    let forward_all = string_array(field(operations, "forward_all")?)?;
    require(
        forward_all == ["worker-a", "worker-b", "worker-c"],
        "canonical three-worker fan-out order",
    )?;
    let round_robin = string_array(field(operations, "forward_round_robin")?)?;
    require(
        round_robin
            == ["worker-a", "worker-b", "worker-c", "worker-a", "worker-b", "worker-c"],
        "canonical six-step RR vector",
    )?;

    let by_key = array(field(operations, "by_key")?)?;
    let expected_keys = ["customer-7", "alpha", "beta", "🌍", "", "\0"];
    require(by_key.len() == expected_keys.len(), "missing/reordered six canonical routing keys")?;
    for (entry, expected_key) in by_key.iter().zip(expected_keys) {
        let key = string(field(entry, "key")?)?;
        require(key == expected_key, "missing/reordered six canonical routing keys")?;
        require(key.len() <= 4096, "routing key byte budget exceeded")?;
        let hash = fnv1a64(key.as_bytes());
        require(
            string(field(entry, "fnv1a64_hex")?)? == format!("{hash:016x}"),
            "routing key hash mismatch",
        )?;
        let index = (hash % 3) as usize;
        require(
            forward_all[index] == string(field(entry, "target")?)?,
            "routing key target mismatch",
        )?;
    }

    let graph = field(data, "graph")?;
    let stages = array(field(graph, "stages")?)?;
    let edges = array(field(graph, "connections")?)?;
    require((2..=1024).contains(&stages.len()), "graph stage budget violated")?;
    require((1..=4096).contains(&edges.len()), "graph edge budget violated")?;

    let mut ids: BTreeMap<String, ActorStage> = BTreeMap::new();
    for stage in stages {
        let id = actor_text(field(stage, "id")?, false)?;
        let _actor_type = actor_text(field(stage, "actor_type")?, false)?;
        let inbound = actor_text(field(stage, "inbound")?, true)?;
        let outbound = actor_text(field(stage, "outbound")?, true)?;
        let role = string(field(stage, "role")?)?.to_string();
        let valid_role = match role.as_str() {
            "readable" => inbound.is_empty() && !outbound.is_empty(),
            "writable" => !inbound.is_empty() && outbound.is_empty(),
            "transform" | "duplex" => !inbound.is_empty() && !outbound.is_empty(),
            _ => false,
        };
        require(valid_role && !ids.contains_key(&id), "invalid role or duplicate graph instance")?;
        ids.insert(id, ActorStage { role, inbound, outbound });
    }

    let mut degrees: BTreeMap<String, usize> = ids.keys().map(|id| (id.clone(), 0)).collect();
    let mut adjacency: BTreeMap<String, Vec<String>> =
        ids.keys().map(|id| (id.clone(), Vec::new())).collect();
    let mut pairs = BTreeSet::new();
    for edge in edges {
        let source = actor_text(field(edge, "source")?, false)?;
        let target = actor_text(field(edge, "target")?, false)?;
        let payload = actor_text(field(edge, "payload")?, false)?;
        let source_stage = ids.get(&source).ok_or_else(|| "unknown graph source".to_string())?;
        let target_stage = ids.get(&target).ok_or_else(|| "unknown graph target".to_string())?;
        require(source != target, "self graph edge")?;
        require(
            pairs.insert((source.clone(), target.clone())),
            "duplicate endpoint pair",
        )?;
        require(
            source_stage.outbound == payload
                && target_stage.inbound == payload
                && source_stage.role != "writable"
                && target_stage.role != "readable",
            "invalid graph edge or payload/role mismatch",
        )?;
        *degrees.get_mut(&target).ok_or_else(|| "target degree missing".to_string())? += 1;
        adjacency
            .get_mut(&source)
            .ok_or_else(|| "source adjacency missing".to_string())?
            .push(target);
    }

    for (id, stage) in &ids {
        let inbound = *degrees.get(id).ok_or_else(|| "degree missing".to_string())?;
        let outbound = adjacency.get(id).ok_or_else(|| "adjacency missing".to_string())?.len();
        let connected = !(inbound == 0 && outbound == 0)
            && match stage.role.as_str() {
                "readable" => outbound > 0,
                "writable" => inbound > 0,
                "transform" => inbound > 0 && outbound > 0,
                "duplex" => true,
                _ => false,
            };
        require(connected, "pipeline actor missing connected role port")?;
    }

    let roots: Vec<String> = degrees
        .iter()
        .filter(|(_, count)| **count == 0)
        .map(|(id, _)| id.clone())
        .collect();
    require(
        roots.iter().any(|id| ids.get(id).is_some_and(|stage| matches!(stage.role.as_str(), "readable" | "duplex"))),
        "no authorized ingress root",
    )?;

    let mut topo_degrees = degrees.clone();
    let mut ready = roots.clone();
    let mut pos = 0usize;
    let mut visited = 0usize;
    while pos < ready.len() {
        let source = ready[pos].clone();
        pos += 1;
        visited += 1;
        for target in adjacency.get(&source).ok_or_else(|| "adjacency missing".to_string())? {
            let degree = topo_degrees
                .get_mut(target)
                .ok_or_else(|| "topology degree missing".to_string())?;
            *degree = degree.checked_sub(1).ok_or_else(|| "topology degree underflow".to_string())?;
            if *degree == 0 {
                ready.push(target.clone());
            }
        }
    }
    require(visited == stages.len(), "cyclic actor graph")?;
    for id in ids.keys() {
        let outbound = !adjacency.get(id).ok_or_else(|| "adjacency missing".to_string())?.is_empty();
        let inbound = adjacency.values().any(|targets| targets.contains(id));
        require(outbound || inbound, "isolated graph actor")?;
    }

    let producer = adjacency
        .get("producer-1")
        .ok_or_else(|| "producer-1 missing".to_string())?;
    require(
        producer.iter().map(String::as_str).collect::<Vec<_>>() == forward_all,
        "fan-out order does not match published graph",
    )?;
    require(
        producer.len() != 1
            && string(field(field(operations, "forward")?, "error")?)? == "ambiguous forward",
        "ambiguous one-to-many forward not rejected",
    )?;
    let forward_to = field(operations, "forward_to")?;
    let target = string(field(forward_to, "target")?)?;
    require(
        producer.iter().any(|candidate| candidate == target)
            && string(field(forward_to, "expected")?)? == target,
        "explicit target route is not connected",
    )?;
    let invalid_target = string(field(field(operations, "invalid_target")?, "target")?)?;
    require(
        !producer.iter().any(|candidate| candidate == invalid_target),
        "invalid target unexpectedly connected",
    )?;
    for (index, target) in round_robin.iter().enumerate() {
        require(
            *target == producer[index % producer.len()],
            "published RR not deterministic",
        )?;
    }
    require("🌍".len() == 4, "UTF-8 scalar width drift")?;
    Ok(())
}

fn actor_mutation_suite(good: &Json) -> Result<usize, String> {
    let mut rejected = 0usize;
    let check = |name: &str, candidate: Json, rejected: &mut usize| -> Result<(), String> {
        if verify_actor_pipeline_doc(&candidate).is_ok() {
            Err(format!("unsafe actor fixture accepted: {name}"))
        } else {
            *rejected += 1;
            Ok(())
        }
    };

    {
        let mut d = good.clone();
        array_mut(field_mut(field_mut(&mut d, "operations")?, "by_key")?)?.pop();
        check("missing NUL golden vector", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let entries = array_mut(field_mut(field_mut(&mut d, "operations")?, "by_key")?)?;
        *field_mut(&mut entries[0], "target")? = Json::String("worker-b".into());
        check("forged key destination", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let entries = array_mut(field_mut(field_mut(&mut d, "operations")?, "by_key")?)?;
        *field_mut(&mut entries[1], "fnv1a64_hex")? = Json::String("0000000000000000".into());
        check("forged FNV hash", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let entries = array_mut(field_mut(field_mut(&mut d, "operations")?, "by_key")?)?;
        *field_mut(&mut entries[0], "key")? = Json::String("x".repeat(4097));
        check("oversized routing key", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        array_mut(field_mut(field_mut(&mut d, "operations")?, "forward_round_robin")?)?.reverse();
        check("altered RR sequence", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        *field_mut(field_mut(field_mut(&mut d, "operations")?, "forward_to")?, "target")? =
            Json::String("other".into());
        check("unknown supposedly connected target", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        *field_mut(field_mut(field_mut(&mut d, "operations")?, "invalid_target")?, "target")? =
            Json::String("worker-a".into());
        check("invalid target suddenly connected", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        *field_mut(field_mut(field_mut(&mut d, "operations")?, "forward")?, "error")? =
            Json::String("choose first".into());
        check("silent default forward", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        *field_mut(&mut stages[2], "id")? = Json::String("worker-a".into());
        check("duplicate worker instance", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        *field_mut(&mut stages[1], "id")? = Json::String("worker\0x".into());
        check("NUL actor instance", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        *field_mut(&mut stages[1], "id")? = Json::String("🌍".repeat(65));
        check("overlong UTF-8 instance", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        *field_mut(&mut stages[1], "role")? = Json::String("supervisor".into());
        check("invalid actor role", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        *field_mut(&mut stages[1], "role")? = Json::String("transform".into());
        *field_mut(&mut stages[1], "outbound")? = Json::String("Event".into());
        check("terminal transform", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let graph = field_mut(&mut d, "graph")?;
        {
            let stages = array_mut(field_mut(graph, "stages")?)?;
            for index in [1usize, 2usize] {
                *field_mut(&mut stages[index], "role")? = Json::String("transform".into());
                *field_mut(&mut stages[index], "outbound")? = Json::String("Event".into());
            }
        }
        let edges = array_mut(field_mut(graph, "connections")?)?;
        edges.push(Json::Object(vec![
            ("source".into(), Json::String("worker-a".into())),
            ("target".into(), Json::String("worker-b".into())),
            ("payload".into(), Json::String("Event".into())),
        ]));
        edges.push(Json::Object(vec![
            ("source".into(), Json::String("worker-b".into())),
            ("target".into(), Json::String("worker-a".into())),
            ("payload".into(), Json::String("Event".into())),
        ]));
        check("genuine transform cycle", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let edges = array_mut(field_mut(field_mut(&mut d, "graph")?, "connections")?)?;
        *field_mut(&mut edges[0], "payload")? = Json::String("Wrong".into());
        check("nominal payload mismatch", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let edges = array_mut(field_mut(field_mut(&mut d, "graph")?, "connections")?)?;
        edges.push(edges[0].clone());
        check("duplicate endpoint pair", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let edges = array_mut(field_mut(field_mut(&mut d, "graph")?, "connections")?)?;
        *field_mut(&mut edges[0], "target")? = Json::String("unknown".into());
        check("unknown destination", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let edges = array_mut(field_mut(field_mut(&mut d, "graph")?, "connections")?)?;
        *field_mut(&mut edges[0], "target")? = Json::String("producer-1".into());
        check("self edge", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        stages.push(Json::Object(vec![
            ("id".into(), Json::String("orphan".into())),
            ("actor_type".into(), Json::String("T".into())),
            ("role".into(), Json::String("readable".into())),
            ("inbound".into(), Json::String(String::new())),
            ("outbound".into(), Json::String("Event".into())),
        ]));
        check("isolated extra actor", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let stages = array_mut(field_mut(field_mut(&mut d, "graph")?, "stages")?)?;
        let sample = stages[0].clone();
        for _ in 0..1022 {
            stages.push(sample.clone());
        }
        check("vertex quota exceeded", d, &mut rejected)?;
    }
    {
        let mut d = good.clone();
        let edges = array_mut(field_mut(field_mut(&mut d, "graph")?, "connections")?)?;
        let sample = edges[0].clone();
        for _ in 0..4094 {
            edges.push(sample.clone());
        }
        check("edge quota exceeded", d, &mut rejected)?;
    }
    require(rejected == 21, "actor mutation inventory drift")?;
    Ok(rejected)
}

fn verify_actor_pipeline(root: &Path) -> Result<(), String> {
    let path = root.join("contracts/v1/actor-pipeline-routing-v1.json");
    let raw = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let doc = parse_json(&raw)?;
    verify_actor_pipeline_doc(&doc)?;
    let rejected = actor_mutation_suite(&doc)?;
    require(rejected == 21, "actor mutation suite incomplete")?;
    require(
        parse_json(b"{\"key\":\"\\ud800\"}").is_err(),
        "invalid Unicode routing key accepted by JSON parser",
    )?;
    Ok(())
}

fn push_field(out: &mut Vec<u8>, text: &str) -> Result<(), String> {
    let raw = text.as_bytes();
    if raw.is_empty() || raw.len() > 512 {
        return Err("empty or unbounded ABI field".into());
    }
    out.extend_from_slice(&(raw.len() as u32).to_be_bytes());
    out.extend_from_slice(raw);
    Ok(())
}

fn positive_i32(value: &Json) -> Result<u32, String> {
    let Json::Number(raw) = value else {
        return Err("invalid i32 enum tag".into());
    };
    if raw.chars().any(|ch| matches!(ch, '.' | 'e' | 'E')) {
        return Err("invalid i32 enum tag".into());
    }
    let parsed: i64 = raw.parse().map_err(|_| "invalid i32 enum tag".to_string())?;
    if !(1..=0x7fff_ffff).contains(&parsed) {
        return Err("invalid i32 enum tag".into());
    }
    Ok(parsed as u32)
}

fn nullary_schema_bytes(identity: &str, visibility: &str, cases: &Json) -> Result<Vec<u8>, String> {
    require(matches!(visibility, "private" | "public"), "unsupported export visibility")?;
    require(
        identity.contains('.') && !identity.starts_with('.') && !identity.ends_with('.'),
        "unqualified enum symbol",
    )?;
    let cases = array(cases)?;
    require((1..=1024).contains(&cases.len()), "invalid AOT enum case count")?;

    let mut out = Vec::new();
    push_field(&mut out, "ores-nullary-enum-tag32-schema-v2")?;
    push_field(&mut out, identity)?;
    push_field(&mut out, visibility)?;
    push_field(&mut out, &cases.len().to_string())?;

    let mut previous = 0u32;
    let mut names = BTreeSet::new();
    for case in cases {
        let tag = positive_i32(field(case, "tag")?)?;
        let name = string(field(case, "name")?)?;
        if tag <= previous || !names.insert(name.to_string()) {
            return Err("unordered or duplicated enum case".into());
        }
        let mut chars = name.chars();
        let first = chars.next().ok_or_else(|| "nonportable enum case".to_string())?;
        if !first.is_ascii() || !(first.is_ascii_alphabetic() || first == '_') {
            return Err("nonportable enum case".into());
        }
        if !chars.all(|ch| ch.is_ascii_alphanumeric() || ch == '_') {
            return Err("nonportable enum case".into());
        }
        previous = tag;
        push_field(&mut out, name)?;
        out.extend_from_slice(&tag.to_be_bytes());
    }
    Ok(out)
}

fn verify_nullary_tag32(root: &Path) -> Result<(), String> {
    let path = root.join("contracts/v2/enum_tag32_schema_v2_vectors.json");
    let raw = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let doc = parse_json(&raw)?;
    require(
        string(field(&doc, "version")?)? == "nullary-enum-tag32-schema-v2",
        "tag32 vector version drift",
    )?;
    require(
        string(field(&doc, "status")?)? == "host-only-not-guest-executable",
        "tag32 status drift",
    )?;
    let identity = string(field(&doc, "qualified_enum_id")?)?;
    let cases = field(&doc, "cases")?;
    let layouts = array(field(&doc, "layouts")?)?;
    let mut visibilities = BTreeSet::new();
    let mut hashes = BTreeSet::new();
    let mut originals = Vec::new();
    for layout in layouts {
        let visibility = string(field(layout, "visibility")?)?;
        visibilities.insert(visibility);
        let wire = nullary_schema_bytes(identity, visibility, cases)?;
        let wire_hex = hex(&wire);
        require(
            string(field(layout, "canonical_bytes_hex")?)? == wire_hex,
            "tag32 canonical bytes drift",
        )?;
        let digest = hex(&sha256(&wire));
        require(
            string(field(layout, "schema_sha256")?)? == digest,
            "tag32 schema hash drift",
        )?;
        hashes.insert(digest);
        originals.push(wire);
    }
    require(
        visibilities == BTreeSet::from(["private", "public"]),
        "tag32 visibility inventory drift",
    )?;
    require(hashes.len() == 2, "public/private tag32 schemas coalesced")?;

    let exported = nullary_schema_bytes(identity, "public", cases)?;
    let renamed = nullary_schema_bytes(&(identity.to_string() + "2"), "private", cases)?;
    let mut changed = cases.clone();
    let Json::Array(items) = &mut changed else {
        return Err("cases must be array".into());
    };
    let last = items.last_mut().ok_or_else(|| "cases missing".to_string())?;
    let Json::Object(fields) = last else {
        return Err("case must be object".into());
    };
    let tag = fields
        .iter_mut()
        .find(|(key, _)| key == "tag")
        .ok_or_else(|| "case tag missing".to_string())?;
    let current = positive_i32(&tag.1)?;
    tag.1 = Json::Number((current + 1).to_string());
    let modified = nullary_schema_bytes(identity, "private", &changed)?;
    let private = nullary_schema_bytes(identity, "private", cases)?;
    let unique: BTreeSet<Vec<u8>> = [private, exported, renamed, modified].into_iter().collect();
    require(unique.len() == 4, "tag32 schema changes coalesced")?;
    require(originals.len() == 2, "tag32 layouts incomplete")?;
    Ok(())
}

fn codepoint_forbidden(cp: u32) -> bool {
    let java_whitespace = cp == 0x1680
        || cp == 0x2028
        || cp == 0x2029
        || cp == 0x205f
        || cp == 0x3000
        || (0x2000..=0x2006).contains(&cp)
        || (0x2008..=0x200a).contains(&cp);
    let space_chars = matches!(cp, 0x00a0 | 0x2007 | 0x202f);
    let bidi = matches!(cp, 0x061c | 0x2060 | 0xfeff)
        || (0x200b..=0x200f).contains(&cp)
        || (0x202a..=0x202e).contains(&cp)
        || (0x2066..=0x2069).contains(&cp);
    java_whitespace || space_chars || bidi || cp <= 0x20 || (0x7f..=0x9f).contains(&cp)
}

fn valid_nominal_id(value: &str) -> Result<(), String> {
    if value.is_empty() || value.chars().all(|c| codepoint_forbidden(c as u32)) {
        return Err("empty nominal identity".into());
    }
    if value.chars().count() > 512 {
        return Err("nominal identity character budget exceeded".into());
    }
    for ch in value.chars() {
        if codepoint_forbidden(ch as u32) {
            return Err("control or invisible codepoint".into());
        }
    }
    if value.len() > 512 {
        return Err("nominal identity UTF-8 budget exceeded".into());
    }
    Ok(())
}

fn qualified_nominal(value: &str) -> Result<(), String> {
    valid_nominal_id(value)?;
    require(
        value.contains('.') && !value.starts_with('.') && !value.ends_with('.') && !value.contains(".."),
        "unqualified enum symbol",
    )
}

fn eos_encode(value: &str) -> Result<String, String> {
    valid_nominal_id(value)?;
    Ok(format!("{}_{}", value.len(), hex(value.as_bytes())))
}

fn eos_symbol(vector: &Json) -> Result<String, String> {
    let domain = string(field(vector, "domain")?)?;
    require(matches!(domain, "MODULE" | "INSTANCE" | "STATIC"), "invalid callable domain")?;
    let owner = string(field(vector, "owner")?)?;
    qualified_nominal(owner)?;
    let name = string(field(vector, "name")?)?;
    valid_nominal_id(name)?;
    let params = array(field(vector, "params")?)?;
    require(params.len() <= 64, "signature arity bound")?;

    let mut result = format!(
        "eos2{}{}{}a{}_",
        eos_encode(domain)?,
        eos_encode(owner)?,
        eos_encode(name)?,
        params.len()
    );
    for parameter in params {
        let kind = string(field(parameter, "kind")?)?;
        let identity = string(field(parameter, "identity")?)?;
        match kind {
            "FIXED" => {
                require(
                    matches!(identity, "bool" | "int" | "string" | "float"),
                    "unknown fixed scalar",
                )?;
                require(
                    object(parameter)?.iter().all(|(key, _)| key != "case"),
                    "unknown fixed scalar",
                )?;
                result.push('F');
                result.push_str(&eos_encode(identity)?);
            }
            "ENUM" => {
                qualified_nominal(identity)?;
                require(
                    object(parameter)?.iter().all(|(key, _)| key != "case"),
                    "whole enum cannot carry a case",
                )?;
                result.push('E');
                result.push_str(&eos_encode(identity)?);
            }
            "CASE" => {
                qualified_nominal(identity)?;
                let case = string(field(parameter, "case")?)?;
                result.push('C');
                result.push_str(&eos_encode(identity)?);
                result.push_str(&eos_encode(case)?);
            }
            _ => return Err("unknown parameter kind".into()),
        }
    }
    Ok(result)
}

fn verify_slot_abi(root: &Path) -> Result<(), String> {
    let path = root.join("contracts/v2/enum_case_slot_abi_vectors.json");
    let raw = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let doc = parse_json(&raw)?;
    require(
        string(field(&doc, "version")?)? == "enum-case-slot-abi-v2",
        "EOS2 vector version drift",
    )?;
    require(
        string(field(&doc, "status")?)? == "host-reference-only-not-guest-executable",
        "EOS2 status drift",
    )?;
    let bounds = field(&doc, "bounds")?;
    for (key, expected) in [
        ("max_parameters", "64"),
        ("max_candidates", "128"),
        ("max_enum_cases", "1024"),
        ("max_registered_enums", "4096"),
        ("max_identity_utf8_bytes", "512"),
    ] {
        require(
            field(bounds, key)? == &Json::Number(expected.into()),
            "EOS2 bound drift",
        )?;
    }
    let mut symbols = BTreeSet::new();
    for vector in array(field(&doc, "vectors")?)? {
        let actual = eos_symbol(vector)?;
        require(
            string(field(vector, "expected")?)? == actual,
            "EOS2 golden symbol drift",
        )?;
        require(symbols.insert(actual), "EOS2 symbol collision")?;
    }
    require(symbols.len() >= 8, "EOS2 vector inventory shrank")?;

    for value in [
        "", ".Foo", "root..Foo", "root.Foo.", "root.\u{202e}Foo", "root.\u{200d}Foo",
        "root.\u{0080}Foo", "root.\u{feff}Foo",
    ] {
        assert!(qualified_nominal(value).is_err(), "malformed nominal admitted: {value:?}");
    }
    for value in ["\u{00a0}", "\u{2007}", "\u{202f}"] {
        assert!(eos_encode(value).is_err(), "invisible separator admitted");
    }
    require(eos_encode(&"A".repeat(513)).is_err(), "513-character identity admitted")?;
    require(eos_encode(&"é".repeat(300)).is_err(), "oversized UTF-8 identity admitted")?;
    require(
        eos_encode(&"A".repeat(512))? == format!("512_{}", "41".repeat(512)),
        "512-byte identity boundary drift",
    )?;
    Ok(())
}

#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
enum ModelParam {
    Fixed(&'static str),
    Enum(&'static str),
    Case(&'static str, &'static str),
}

fn model_domain(enum_id: &str) -> &'static [&'static str] {
    match enum_id {
        "root.Foo" => &["A", "B", "C"],
        "root.Bar" => &["X", "Y"],
        _ => &[],
    }
}

fn model_params() -> Vec<ModelParam> {
    vec![
        ModelParam::Fixed("int"),
        ModelParam::Fixed("string"),
        ModelParam::Enum("root.Foo"),
        ModelParam::Case("root.Foo", "A"),
        ModelParam::Case("root.Foo", "B"),
        ModelParam::Case("root.Foo", "C"),
        ModelParam::Enum("root.Bar"),
        ModelParam::Case("root.Bar", "X"),
        ModelParam::Case("root.Bar", "Y"),
    ]
}

fn param_enum_id(param: &ModelParam) -> Option<&'static str> {
    match param {
        ModelParam::Enum(id) | ModelParam::Case(id, _) => Some(id),
        ModelParam::Fixed(_) => None,
    }
}

fn overlap_algorithm(left: &[ModelParam], right: &[ModelParam]) -> &'static str {
    let mut left_subset = true;
    let mut right_subset = true;
    let mut disjoint = false;
    for (a, b) in left.iter().zip(right) {
        if matches!(a, ModelParam::Fixed(_)) || matches!(b, ModelParam::Fixed(_)) {
            if a != b {
                return "INVALID";
            }
            continue;
        }
        if param_enum_id(a) != param_enum_id(b) {
            disjoint = true;
            continue;
        }
        match (a, b) {
            (ModelParam::Enum(_), ModelParam::Case(_, _)) => left_subset = false,
            (ModelParam::Case(_, _), ModelParam::Enum(_)) => right_subset = false,
            (ModelParam::Case(_, a_case), ModelParam::Case(_, b_case)) if a_case != b_case => {
                disjoint = true;
            }
            _ => {}
        }
    }
    if disjoint {
        "DISJOINT"
    } else if left_subset && right_subset {
        "EQUAL"
    } else if left_subset {
        "LEFT_NARROWER"
    } else if right_subset {
        "RIGHT_NARROWER"
    } else {
        "INVALID"
    }
}

type ModelValue = Vec<(String, Option<String>)>;

fn denotation(signature: &[ModelParam]) -> BTreeSet<ModelValue> {
    let mut result: BTreeSet<ModelValue> = BTreeSet::from([Vec::new()]);
    for param in signature {
        let values: Vec<(String, Option<String>)> = match param {
            ModelParam::Fixed(name) => vec![(name.to_string(), None)],
            ModelParam::Enum(id) => model_domain(id)
                .iter()
                .map(|case| (id.to_string(), Some((*case).to_string())))
                .collect(),
            ModelParam::Case(id, case) => vec![(id.to_string(), Some((*case).to_string()))],
        };
        let mut next = BTreeSet::new();
        for prefix in &result {
            for value in &values {
                let mut tuple = prefix.clone();
                tuple.push(value.clone());
                next.insert(tuple);
            }
        }
        result = next;
    }
    result
}

fn overlap_oracle(left: &[ModelParam], right: &[ModelParam]) -> &'static str {
    for (a, b) in left.iter().zip(right) {
        if (matches!(a, ModelParam::Fixed(_)) || matches!(b, ModelParam::Fixed(_))) && a != b {
            return "INVALID";
        }
    }
    let left_values = denotation(left);
    let right_values = denotation(right);
    if left_values.is_disjoint(&right_values) {
        "DISJOINT"
    } else if left_values == right_values {
        "EQUAL"
    } else if left_values.is_subset(&right_values) {
        "LEFT_NARROWER"
    } else if right_values.is_subset(&left_values) {
        "RIGHT_NARROWER"
    } else {
        "INVALID"
    }
}

fn verify_overlap_model() -> Result<(), String> {
    let params = model_params();
    let mut signatures = Vec::new();
    for a in &params {
        for b in &params {
            signatures.push(vec![a.clone(), b.clone()]);
        }
    }
    require(signatures.len() == 81, "overlap model signature inventory drift")?;
    for left in &signatures {
        for right in &signatures {
            require(
                overlap_algorithm(left, right) == overlap_oracle(left, right),
                "overlap model disagrees with finite-domain oracle",
            )?;
        }
    }
    let crossed_left = vec![ModelParam::Case("root.Foo", "A"), ModelParam::Enum("root.Bar")];
    let crossed_right = vec![ModelParam::Enum("root.Foo"), ModelParam::Case("root.Bar", "Y")];
    require(
        overlap_algorithm(&crossed_left, &crossed_right) == "INVALID",
        "crossed specialization must reject",
    )?;
    let narrow = vec![ModelParam::Case("root.Foo", "A")];
    let whole = vec![ModelParam::Enum("root.Foo")];
    require(
        overlap_algorithm(&narrow, &whole) == "LEFT_NARROWER"
            && overlap_algorithm(&whole, &narrow) == "RIGHT_NARROWER",
        "parent/case nesting drift",
    )?;
    Ok(())
}

fn verify_enum_v2(root: &Path) -> Result<(), String> {
    verify_nullary_tag32(root)?;
    verify_slot_abi(root)?;
    verify_overlap_model()
}

#[cfg(not(test))]
fn usage() -> ! {
    eprintln!("usage: contract_gate <integration|serde|reactive|actor-pipeline|actor-arena-lifecycle|structural-v2|numeric|enum-v2> [repository-root]");
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
        "actor-pipeline" => verify_actor_pipeline(&root),
        "actor-arena-lifecycle" => verify_actor_arena_lifecycle(&root).map(|count| {
            println!("ACTOR_ARENA_LIFECYCLE_V1_PASS checks={count} source-free-policy-only");
        }),
        "structural-v2" => verify_structural_v2(&root),
        "numeric" => verify_numeric(&root),
        "enum-v2" => verify_enum_v2(&root),
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
    fn sha1_known_vector() {
        assert_eq!(hex(&sha1(b"abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
        assert_eq!(git_blob_sha1(b"test\n"), "9daeafb9864cf43055ae93beb0afd6c7d144bfa4");
    }

    #[test]
    fn exact_decimal_arithmetic() {
        let a = DecimalExact::parse("1e80").unwrap();
        let b = DecimalExact::parse("-1e80").unwrap();
        assert_eq!(a.add(&DecimalExact::parse("1").unwrap()).add(&b), DecimalExact::parse("1").unwrap());
        assert_eq!(DecimalExact::parse("0.1").unwrap().mul(&DecimalExact::parse("0.3").unwrap()), DecimalExact::parse("0.03").unwrap());
    }

    #[test]
    fn checked_i64_parser_rejects_overflow() {
        assert_eq!(I64Expr::new("2+3*4").parse().unwrap(), 14);
        assert_eq!(I64Expr::new("-9223372036854775808").parse().unwrap(), i64::MIN);
        assert!(I64Expr::new("9223372036854775807+1").parse().is_err());
        assert!(I64Expr::new("1/2").parse().is_err());
    }

    #[test]
    fn numeric_real_vectors() {
        verify_numeric(Path::new(".")).unwrap();
    }

    #[test]
    fn structural_v2_self_test() {
        verify_structural_v2(Path::new(".")).unwrap();
    }

    #[test]
    fn actor_arena_lifecycle_real_fixture() {
        assert_eq!(verify_actor_arena_lifecycle(Path::new(".")).unwrap(), 40);
    }

    #[test]
    fn actor_pipeline_mutations() {
        verify_actor_pipeline(Path::new(".")).unwrap();
    }

    #[test]
    fn enum_overlap_exhaustive() {
        verify_overlap_model().unwrap();
    }

    #[test]
    fn enum_nominal_boundaries() {
        assert!(qualified_nominal("root.Foo").is_ok());
        assert!(qualified_nominal("root..Foo").is_err());
        assert!(eos_encode("\u{00a0}").is_err());
        assert!(eos_encode(&"A".repeat(512)).is_ok());
        assert!(eos_encode(&"A".repeat(513)).is_err());
    }

    #[test]
    fn real_contracts_pass() {
        verify_serde(Path::new(".")).unwrap();
        verify_reactive(Path::new(".")).unwrap();
        verify_integration_root(Path::new(".")).unwrap();
        verify_enum_v2(Path::new(".")).unwrap();
    }
}
