use std::{
    collections::BTreeSet,
    env,
    fs::{self, OpenOptions},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    process::{Command, ExitStatus, Stdio},
    sync::{atomic::{AtomicBool, Ordering as AtomicOrdering}, Arc},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

#[cfg(unix)]
use std::os::unix::{
    fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
    process::CommandExt,
};

const MAX_OUTPUT_BYTES: usize = 65_536;
const MAX_FIXTURE_BYTES: u64 = 1_048_576;
const CANONICAL_SUITE_SHA256: &str = "5b5d1bc00def9b3a343cb3281e0bb0be4e5ca438095f621141049e2d7a1a25b3";

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
    fn new(bytes: &'a [u8]) -> Self { Self { bytes, at: 0 } }
    fn parse(mut self) -> Result<Json, String> {
        self.ws();
        let value = self.value()?;
        self.ws();
        if self.at != self.bytes.len() { return Err("trailing JSON".into()); }
        Ok(value)
    }
    fn ws(&mut self) {
        while self.at < self.bytes.len() && self.bytes[self.at].is_ascii_whitespace() { self.at += 1; }
    }
    fn value(&mut self) -> Result<Json, String> {
        self.ws();
        match self.bytes.get(self.at).copied() {
            Some(b'n') => { self.literal(b"null")?; Ok(Json::Null) }
            Some(b't') => { self.literal(b"true")?; Ok(Json::Bool(true)) }
            Some(b'f') => { self.literal(b"false")?; Ok(Json::Bool(false)) }
            Some(b'"') => Ok(Json::String(self.string()?)),
            Some(b'[') => self.array(),
            Some(b'{') => self.object(),
            Some(b'-' | b'0'..=b'9') => self.number(),
            _ => Err(format!("invalid JSON token at byte {}", self.at)),
        }
    }
    fn literal(&mut self, expected: &[u8]) -> Result<(), String> {
        if self.bytes.get(self.at..self.at + expected.len()) == Some(expected) {
            self.at += expected.len(); Ok(())
        } else { Err("invalid JSON literal".into()) }
    }
    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.at) != Some(&b'"') { return Err("expected JSON string".into()); }
        self.at += 1;
        let mut out = String::new();
        while self.at < self.bytes.len() {
            let byte = self.bytes[self.at];
            self.at += 1;
            match byte {
                b'"' => return Ok(out),
                b'\\' => {
                    let esc = *self.bytes.get(self.at).ok_or_else(|| "unterminated JSON escape".to_string())?;
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
                                    return Err("high surrogate without low surrogate".into());
                                }
                                self.at += 2;
                                let low = self.hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&low) { return Err("invalid low surrogate".into()); }
                                0x10000 + (((high - 0xD800) as u32) << 10) + (low - 0xDC00) as u32
                            } else if (0xDC00..=0xDFFF).contains(&high) {
                                return Err("unpaired low surrogate".into());
                            } else { high as u32 };
                            out.push(char::from_u32(scalar).ok_or_else(|| "invalid unicode scalar".to_string())?);
                        }
                        _ => return Err("invalid JSON escape".into()),
                    }
                }
                0x00..=0x1f => return Err("control byte in JSON string".into()),
                0x20..=0x7f => out.push(byte as char),
                _ => {
                    let start = self.at - 1;
                    let width = match byte {
                        0xC2..=0xDF => 2,
                        0xE0..=0xEF => 3,
                        0xF0..=0xF4 => 4,
                        _ => return Err("invalid UTF-8".into()),
                    };
                    let end = start + width;
                    let slice = self.bytes.get(start..end).ok_or_else(|| "truncated UTF-8".to_string())?;
                    out.push_str(std::str::from_utf8(slice).map_err(|_| "invalid UTF-8".to_string())?);
                    self.at = end;
                }
            }
        }
        Err("unterminated JSON string".into())
    }
    fn hex4(&mut self) -> Result<u16, String> {
        let mut value = 0u16;
        for _ in 0..4 {
            let byte = *self.bytes.get(self.at).ok_or_else(|| "truncated unicode escape".to_string())?;
            self.at += 1;
            value = value * 16 + match byte {
                b'0'..=b'9' => (byte - b'0') as u16,
                b'a'..=b'f' => (byte - b'a' + 10) as u16,
                b'A'..=b'F' => (byte - b'A' + 10) as u16,
                _ => return Err("invalid unicode escape".into()),
            };
        }
        Ok(value)
    }
    fn array(&mut self) -> Result<Json, String> {
        self.at += 1; self.ws();
        let mut values = Vec::new();
        if self.bytes.get(self.at) == Some(&b']') { self.at += 1; return Ok(Json::Array(values)); }
        loop {
            values.push(self.value()?); self.ws();
            match self.bytes.get(self.at) {
                Some(b',') => { self.at += 1; self.ws(); }
                Some(b']') => { self.at += 1; return Ok(Json::Array(values)); }
                _ => return Err("malformed JSON array".into()),
            }
        }
    }
    fn object(&mut self) -> Result<Json, String> {
        self.at += 1; self.ws();
        let mut fields = Vec::new();
        if self.bytes.get(self.at) == Some(&b'}') { self.at += 1; return Ok(Json::Object(fields)); }
        loop {
            let key = self.string()?;
            if fields.iter().any(|(existing, _)| existing == &key) {
                return Err(format!("duplicate manifest JSON key: {key}"));
            }
            self.ws();
            if self.bytes.get(self.at) != Some(&b':') { return Err("malformed JSON object".into()); }
            self.at += 1;
            let value = self.value()?;
            fields.push((key, value));
            self.ws();
            match self.bytes.get(self.at) {
                Some(b',') => { self.at += 1; self.ws(); }
                Some(b'}') => { self.at += 1; return Ok(Json::Object(fields)); }
                _ => return Err("malformed JSON object".into()),
            }
        }
    }
    fn number(&mut self) -> Result<Json, String> {
        let start = self.at;
        if self.bytes.get(self.at) == Some(&b'-') { self.at += 1; }
        match self.bytes.get(self.at) {
            Some(b'0') => { self.at += 1; if matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) { return Err("leading zero".into()); } }
            Some(b'1'..=b'9') => { self.at += 1; while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) { self.at += 1; } }
            _ => return Err("invalid JSON number".into()),
        }
        if self.bytes.get(self.at) == Some(&b'.') {
            self.at += 1;
            let before = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) { self.at += 1; }
            if self.at == before { return Err("invalid fraction".into()); }
        }
        if matches!(self.bytes.get(self.at), Some(b'e' | b'E')) {
            self.at += 1;
            if matches!(self.bytes.get(self.at), Some(b'+' | b'-')) { self.at += 1; }
            let before = self.at;
            while matches!(self.bytes.get(self.at), Some(b'0'..=b'9')) { self.at += 1; }
            if self.at == before { return Err("invalid exponent".into()); }
        }
        Ok(Json::Number(std::str::from_utf8(&self.bytes[start..self.at]).map_err(|_| "invalid number".to_string())?.to_string()))
    }
}

fn parse_json(raw: &[u8]) -> Result<Json, String> {
    if std::str::from_utf8(raw).is_err() { return Err("manifest must be valid UTF-8".into()); }
    Parser::new(raw).parse()
}
fn object(value: &Json) -> Result<&[(String, Json)], String> {
    match value { Json::Object(v) => Ok(v), _ => Err("expected JSON object".into()) }
}
fn array(value: &Json) -> Result<&[Json], String> {
    match value { Json::Array(v) => Ok(v), _ => Err("expected JSON array".into()) }
}
fn field<'a>(value: &'a Json, key: &str) -> Result<&'a Json, String> {
    object(value)?.iter().find(|(name, _)| name == key).map(|(_, value)| value).ok_or_else(|| format!("missing field {key}"))
}
fn string(value: &Json) -> Result<&str, String> {
    match value { Json::String(v) => Ok(v), _ => Err("expected JSON string".into()) }
}
fn exact_keys(value: &Json, expected: &[&str]) -> Result<(), String> {
    let got: BTreeSet<&str> = object(value)?.iter().map(|(key, _)| key.as_str()).collect();
    let want: BTreeSet<&str> = expected.iter().copied().collect();
    if got == want { Ok(()) } else { Err("unexpected manifest fields".into()) }
}

fn rotr(x: u32, n: u32) -> u32 { (x >> n) | (x << (32 - n)) }
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
    let mut data=bytes.to_vec(); data.push(0x80);
    while data.len()%64 != 56 { data.push(0); }
    data.extend_from_slice(&bit_len.to_be_bytes());
    for chunk in data.chunks_exact(64) {
        let mut w=[0u32;64];
        for i in 0..16 { w[i]=u32::from_be_bytes(chunk[i*4..i*4+4].try_into().expect("word")); }
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
            hh=g;g=f;f=e;e=d.wrapping_add(t1);d=c;c=b;b=a;a=t1.wrapping_add(t2);
        }
        for (slot,value) in h.iter_mut().zip([a,b,c,d,e,f,g,hh]) { *slot=slot.wrapping_add(value); }
    }
    let mut out=[0u8;32];
    for (i,value) in h.iter().enumerate(){out[i*4..i*4+4].copy_from_slice(&value.to_be_bytes());}
    out
}
fn hex(bytes: &[u8]) -> String {
    const H:&[u8;16]=b"0123456789abcdef";
    let mut out=String::with_capacity(bytes.len()*2);
    for &b in bytes { out.push(H[(b>>4) as usize] as char); out.push(H[(b&15) as usize] as char); }
    out
}

#[cfg(unix)]
fn unique_regular(meta: &fs::Metadata) -> bool {
    meta.file_type().is_file() && meta.nlink() == 1
}
#[cfg(not(unix))]
fn unique_regular(meta: &fs::Metadata) -> bool { meta.file_type().is_file() }

#[cfg(unix)]
fn same_identity(a: &fs::Metadata, b: &fs::Metadata) -> bool {
    a.dev()==b.dev() && a.ino()==b.ino() && a.mode()==b.mode() && a.len()==b.len() && a.nlink()==b.nlink()
        && a.mtime()==b.mtime() && a.mtime_nsec()==b.mtime_nsec() && a.ctime()==b.ctime() && a.ctime_nsec()==b.ctime_nsec()
}
#[cfg(not(unix))]
fn same_identity(a: &fs::Metadata, b: &fs::Metadata) -> bool {
    a.len()==b.len() && a.permissions().readonly()==b.permissions().readonly()
}

fn read_bounded_regular(path: &Path) -> Result<Vec<u8>, String> {
    let original=fs::symlink_metadata(path).map_err(|e|format!("lstat {}: {e}",path.display()))?;
    if !unique_regular(&original) { return Err("source fixture must be a unique regular file".into()); }
    if original.len()==0 || original.len()>MAX_FIXTURE_BYTES { return Err("invalid source length".into()); }
    let mut options=OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    { options.custom_flags(0x20000 | 0x800); }
    let mut file=options.open(path).map_err(|e|format!("open {}: {e}",path.display()))?;
    let opened=file.metadata().map_err(|e|format!("fstat {}: {e}",path.display()))?;
    if !unique_regular(&opened) { return Err("fixture replaced by nonregular file".into()); }
    if !same_identity(&original,&opened) { return Err("fixture changed before read".into()); }
    let mut data=Vec::new();
    std::io::Read::by_ref(&mut file).take(MAX_FIXTURE_BYTES+1).read_to_end(&mut data).map_err(|e|format!("read {}: {e}",path.display()))?;
    let after=file.metadata().map_err(|e|format!("fstat after {}: {e}",path.display()))?;
    if !same_identity(&opened,&after) { return Err("fixture changed during read".into()); }
    let final_meta=fs::symlink_metadata(path).map_err(|e|format!("final lstat {}: {e}",path.display()))?;
    if !same_identity(&after,&final_meta) { return Err("fixture path changed during read".into()); }
    if data.is_empty() || data.len() as u64>MAX_FIXTURE_BYTES { return Err("invalid source length".into()); }
    Ok(data)
}

#[derive(Clone)]
struct Case {
    id: String,
    file: String,
    kind: String,
    value: Option<String>,
}
struct Suite { cases: Vec<Case>, digest: String }

fn valid_id(value:&str)->bool {
    let bytes=value.as_bytes();
    (2..=80).contains(&bytes.len()) && bytes[0].is_ascii_lowercase()
        && bytes[1..].iter().all(|b|b.is_ascii_lowercase()||b.is_ascii_digit()||matches!(b,b'.'|b'-'))
}
fn valid_fixture_name(value:&str)->bool {
    let Some(stem)=value.strip_prefix("fixtures/").and_then(|v|v.strip_suffix(".ores")) else{return false};
    !stem.is_empty() && stem.bytes().all(|b|b.is_ascii_lowercase()||b.is_ascii_digit()||b==b'-')
}
fn canonical_i64(value:&str)->bool {
    let raw=value.strip_prefix('-').unwrap_or(value);
    if raw.is_empty() || !raw.bytes().all(|b|b.is_ascii_digit()) { return false; }
    if raw.len()>1 && raw.starts_with('0') { return false; }
    value.parse::<i64>().is_ok()
}
fn expected_case(id:&str)->Option<(&'static str,Option<&'static str>)> {
    match id {
        "scalar.literal-42"=>Some(("accept",Some("42"))),
        "scalar.precedence"=>Some(("accept",Some("28"))),
        "scalar.unary"=>Some(("accept",Some("-21"))),
        "scalar.i64-min"=>Some(("accept",Some("-9223372036854775808"))),
        "scalar.i64-max"=>Some(("accept",Some("9223372036854775807"))),
        "scalar.i64-max-minus-one"=>Some(("accept",Some("9223372036854775806"))),
        "reject.unknown"|"reject.division"|"reject.actor"|"reject.async"|"reject.structural"=>Some(("reject",None)),
        _=>None,
    }
}
fn expected_filename(id:&str,kind:&str)->Result<String,String>{
    let suffix=id.split_once('.').map(|(_,s)|s).ok_or_else(||"invalid case id".to_string())?;
    Ok(format!("fixtures/{}{}.ores",if kind=="reject"{"reject-"}else{""},suffix))
}

fn load_suite(root:&Path)->Result<Suite,String>{
    let here=root.join("contracts/scalar/v1");
    let suite_bytes=read_bounded_regular(&here.join("cases.json"))?;
    let suite=parse_json(&suite_bytes)?;
    exact_keys(&suite,&["schema_version","cases"])?;
    if string(field(&suite,"schema_version")?)?!="1.0.0"{return Err("unsupported manifest fields or version".into());}
    let entries=array(field(&suite,"cases")?)?;
    if !(1..=100).contains(&entries.len()){return Err("invalid case inventory".into());}
    let fixture_root=here.join("fixtures");
    let fixture_meta=fs::symlink_metadata(&fixture_root).map_err(|e|format!("fixture dir: {e}"))?;
    if fixture_meta.file_type().is_symlink()||!fixture_meta.is_dir(){return Err("fixture directory must be an ordinary directory".into());}
    let canonical_root=fs::canonicalize(&fixture_root).map_err(|e|format!("fixture root: {e}"))?;
    let mut seen=BTreeSet::new();
    let mut declared=BTreeSet::new();
    let mut cases=Vec::new();
    let mut digest_input=suite_bytes.clone();
    for entry in entries{
        let keys: BTreeSet<&str>=object(entry)?.iter().map(|(k,_)|k.as_str()).collect();
        let allowed3=BTreeSet::from(["id","file","kind"]);
        let allowed4=BTreeSet::from(["id","file","kind","value"]);
        if keys!=allowed3&&keys!=allowed4{return Err("unexpected case fields".into());}
        let id=string(field(entry,"id")?)?.to_string();
        let filename=string(field(entry,"file")?)?.to_string();
        let kind=string(field(entry,"kind")?)?.to_string();
        if !valid_id(&id)||!seen.insert(id.clone()){return Err("invalid or duplicate case id".into());}
        if !valid_fixture_name(&filename){return Err("invalid fixture filename".into());}
        if !declared.insert(filename.clone()){return Err(format!("duplicate source fixture: {filename}"));}
        let source=here.join(&filename);
        let source_meta=fs::symlink_metadata(&source).map_err(|e|format!("fixture lstat: {e}"))?;
        if source_meta.file_type().is_symlink(){return Err("fixture path escapes fixture root".into());}
        let parent=fs::canonicalize(source.parent().ok_or_else(||"fixture has no parent".to_string())?).map_err(|e|format!("fixture parent: {e}"))?;
        if parent!=canonical_root{return Err("fixture path escapes fixture root".into());}
        let data=read_bounded_regular(&source)?;
        if data.contains(&0){return Err("embedded NUL in source fixture".into());}
        std::str::from_utf8(&data).map_err(|_|"fixture must be valid UTF-8".to_string())?;
        digest_input.extend_from_slice(filename.as_bytes());digest_input.push(0);digest_input.extend_from_slice(&data);
        let value=object(entry)?.iter().find(|(k,_)|k=="value").map(|(_,v)|string(v)).transpose()?.map(str::to_string);
        match kind.as_str(){
            "accept"=>if value.as_deref().is_none_or(|v|!canonical_i64(v)){return Err("positive case requires canonical decimal value".into());},
            "reject"=>if value.is_some(){return Err("negative case may not specify a value".into());},
            _=>return Err("invalid case kind".into()),
        }
        let expected=expected_case(&id).ok_or_else(||format!("unknown or redefined v1 case: {id}"))?;
        if expected.0!=kind||expected.1!=value.as_deref(){return Err(format!("unknown or redefined v1 case: {id}"));}
        if filename!=expected_filename(&id,&kind)?{return Err("v1 case filename does not match its identity".into());}
        cases.push(Case{id,file:filename,kind,value});
    }
    if seen.len()!=11{return Err("v1 case inventory changed".into());}
    let disk: BTreeSet<String>=fs::read_dir(&fixture_root).map_err(|e|format!("list fixtures: {e}"))?
        .map(|entry|entry.map_err(|e|e.to_string()).and_then(|e|e.file_name().into_string().map_err(|_|"non-UTF8 fixture name".to_string())).map(|name|format!("fixtures/{name}")))
        .collect::<Result<_,_>>()?;
    if disk!=declared{return Err("orphan/missing fixture".into());}
    Ok(Suite{cases,digest:hex(&sha256(&digest_input))})
}

fn temp_dir(prefix:&str)->Result<PathBuf,String>{
    let nonce=SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e|e.to_string())?.as_nanos();
    let path=env::temp_dir().join(format!("{prefix}-{}-{nonce}",std::process::id()));
    #[cfg(unix)]{
        let mut builder=fs::DirBuilder::new();builder.mode(0o700);builder.create(&path).map_err(|e|format!("create temp: {e}"))?;
    }
    #[cfg(not(unix))]{fs::create_dir(&path).map_err(|e|format!("create temp: {e}"))?;}
    Ok(path)
}

fn snapshot_suite(root:&Path,suite:&Suite,destination:&Path)->Result<PathBuf,String>{
    let here=root.join("contracts/scalar/v1");
    let snapshot=destination.join("fixtures");
    fs::create_dir(&snapshot).map_err(|e|format!("create snapshot: {e}"))?;
    let suite_bytes=read_bounded_regular(&here.join("cases.json"))?;
    let mut digest_input=suite_bytes;
    for case in &suite.cases{
        let data=read_bounded_regular(&here.join(&case.file))?;
        digest_input.extend_from_slice(case.file.as_bytes());digest_input.push(0);digest_input.extend_from_slice(&data);
        let target=snapshot.join(Path::new(&case.file).file_name().ok_or_else(||"fixture basename missing".to_string())?);
        let mut file=OpenOptions::new().write(true).create_new(true).open(&target).map_err(|e|format!("snapshot create: {e}"))?;
        file.write_all(&data).map_err(|e|format!("snapshot write: {e}"))?;
    }
    if hex(&sha256(&digest_input))!=suite.digest{return Err("fixture corpus changed during snapshot".into());}
    Ok(snapshot)
}

#[derive(Debug)]
enum CaptureError { Io(String), Timeout }
struct Captured { status: ExitStatus, stdout: Vec<u8>, stderr: Vec<u8>, exceeded: bool }

fn kill_group(child:&mut std::process::Child){
    #[cfg(unix)]{
        let group=format!("-{}",child.id());
        let _=Command::new("kill").args(["-KILL","--",&group]).stdout(Stdio::null()).stderr(Stdio::null()).status();
    }
    let _=child.kill();
}

fn capture_bounded(argv:&[String],timeout:Duration)->Result<Captured,CaptureError>{
    if argv.is_empty(){return Err(CaptureError::Io("empty command".into()));}
    let mut command=Command::new(&argv[0]);command.args(&argv[1..]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    #[cfg(unix)]{command.process_group(0);}
    let mut child=command.spawn().map_err(|e|CaptureError::Io(e.to_string()))?;
    let stdout=child.stdout.take().ok_or_else(||CaptureError::Io("stdout pipe unavailable".into()))?;
    let stderr=child.stderr.take().ok_or_else(||CaptureError::Io("stderr pipe unavailable".into()))?;
    let overflow=Arc::new(AtomicBool::new(false));
    let reader=|mut stream:Box<dyn Read+Send>, flag:Arc<AtomicBool>| thread::spawn(move||->io::Result<Vec<u8>>{
        let mut out=Vec::new();let mut buf=[0u8;4096];
        loop{
            let n=stream.read(&mut buf)?;if n==0{break;}
            let room=(MAX_OUTPUT_BYTES+1).saturating_sub(out.len());
            if room>0{out.extend_from_slice(&buf[..n.min(room)]);}
            if out.len()>MAX_OUTPUT_BYTES{flag.store(true,AtomicOrdering::SeqCst);break;}
        }
        Ok(out)
    });
    let out_thread=reader(Box::new(stdout),overflow.clone());
    let err_thread=reader(Box::new(stderr),overflow.clone());
    let deadline=Instant::now()+timeout;
    loop{
        if overflow.load(AtomicOrdering::SeqCst){kill_group(&mut child);break;}
        if child.try_wait().map_err(|e|CaptureError::Io(e.to_string()))?.is_some()
            && out_thread.is_finished() && err_thread.is_finished() {
            break;
        }
        if Instant::now()>=deadline{kill_group(&mut child);let _=child.wait();return Err(CaptureError::Timeout);}
        thread::sleep(Duration::from_millis(10));
    }
    let status=match child.try_wait().map_err(|e|CaptureError::Io(e.to_string()))?{Some(s)=>s,None=>child.wait().map_err(|e|CaptureError::Io(e.to_string()))?};
    let stdout=out_thread.join().map_err(|_|CaptureError::Io("stdout reader panicked".into()))?.map_err(|e|CaptureError::Io(e.to_string()))?;
    let stderr=err_thread.join().map_err(|_|CaptureError::Io("stderr reader panicked".into()))?.map_err(|e|CaptureError::Io(e.to_string()))?;
    Ok(Captured{status,stdout,stderr,exceeded:overflow.load(AtomicOrdering::SeqCst)})
}

fn strip_line_ending(value:&str)->Option<&str>{
    if let Some(v)=value.strip_suffix("\r\n"){Some(v)}
    else if let Some(v)=value.strip_suffix('\n'){Some(v)}
    else{Some(value)}
}
fn offset_message(body:&str,prefix:&str,source_len:u64)->bool{
    let Some(raw)=body.strip_prefix(prefix) else{return false};
    !raw.is_empty()&&raw.bytes().all(|b|b.is_ascii_digit())&&raw.parse::<u64>().is_ok_and(|v|v<=source_len)
}
fn diagnostic_ok(backend:&str,case_id:&str,stderr:&str,source_len:u64)->bool{
    let Some(body)=strip_line_ending(stderr) else{return false};
    if body.contains('\n')||body.contains('\r'){return false;}
    match (backend,case_id){
        ("forge","reject.unknown")|("forge","reject.structural")=>offset_message(body,"oresforge: expected integer at byte ",source_len),
        ("forge","reject.division")=>offset_message(body,"oresforge: division requires float result lowering; unsupported in the i64 source subset at byte ",source_len),
        ("forge","reject.actor")|("forge","reject.async")=>offset_message(body,"oresforge: unexpected token at byte ",source_len),
        ("net","reject.unknown")|("net","reject.structural")=>offset_message(body,"oresnet: Expected integer at offset ",source_len),
        ("net","reject.division")=>body=="oresnet: Division requires float result lowering; unsupported in the i64 source subset",
        ("net","reject.actor")|("net","reject.async")=>offset_message(body,"oresnet: Expected 'fnc' at offset ",source_len),
        _=>false,
    }
}

fn evaluate(backend:&str,mode:&str,case:&Case,source_len:u64,captured:&Captured)->Result<bool,String>{
    if captured.exceeded{return Ok(false);}
    let stdout=std::str::from_utf8(&captured.stdout).map_err(|_|"infrastructure: invalid UTF-8 in compiler output".to_string())?;
    let stderr=std::str::from_utf8(&captured.stderr).map_err(|_|"infrastructure: invalid UTF-8 in compiler output".to_string())?;
    let code=captured.status.code();
    if case.kind=="reject"{
        Ok(code==Some(1)&&stdout.is_empty()&&diagnostic_ok(backend,&case.id,stderr,source_len))
    }else if mode=="stdout-i64"{
        let value=case.value.as_deref().ok_or_else(||"accept case missing value".to_string())?;
        Ok(code==Some(0)&&stderr.is_empty()&&(stdout==value||stdout==format!("{value}\n")||stdout==format!("{value}\r\n")))
    }else{
        let stdout_ok=if backend=="net"{stdout.trim()=="Oreslang subset syntax and typed OresIR: OK"}else{true};
        Ok(code==Some(0)&&stdout_ok&&stderr.trim().is_empty())
    }
}

fn shlex_split(input:&str)->Result<Vec<String>,String>{
    let mut out=Vec::new();let mut current=String::new();let mut quote=None;let mut escape=false;let mut started=false;
    for ch in input.chars(){
        if escape{current.push(ch);escape=false;started=true;continue;}
        if ch=='\\'&&quote!=Some('\''){escape=true;started=true;continue;}
        if let Some(q)=quote{if ch==q{quote=None;}else{current.push(ch);}started=true;continue;}
        if ch=='\''||ch=='"'{quote=Some(ch);started=true;}
        else if ch.is_whitespace(){if started{out.push(std::mem::take(&mut current));started=false;}}
        else{current.push(ch);started=true;}
    }
    if escape||quote.is_some(){return Err("unterminated command quote/escape".into());}
    if started{out.push(current);}
    Ok(out)
}

fn run_case(command:&[String],source:&Path,case:&Case,mode:&str,timeout:Duration,backend:&str)->(bool,String){
    let argv:Vec<String>=command.iter().map(|part|part.replace("{source}",&source.to_string_lossy())).collect();
    let result=match capture_bounded(&argv,timeout){
        Ok(value)=>value,
        Err(CaptureError::Timeout)=>return(false,"infrastructure: timeout".into()),
        Err(CaptureError::Io(error))=>return(false,format!("infrastructure: {error}")),
    };
    if result.exceeded{return(false,"compiler output exceeded 64 KiB".into());}
    let source_len=fs::metadata(source).map(|m|m.len()).unwrap_or(u64::MAX);
    match evaluate(backend,mode,case,source_len,&result){
        Ok(true)=>(true,String::new()),
        Ok(false)=>{
            let stderr=String::from_utf8_lossy(&result.stderr);
            let stdout=String::from_utf8_lossy(&result.stdout);
            (false,format!("exit={:?} stderr={:?} stdout={:?}",result.status.code(),&stderr[..stderr.len().min(160)],&stdout[..stdout.len().min(160)]))
        }
        Err(error)=>(false,error),
    }
}

fn valid_revision(value:&str)->bool{value.len()==40&&value.bytes().all(|b|b.is_ascii_hexdigit()&&!b.is_ascii_uppercase())}
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

fn write_report(path:&Path,backend:&str,mode:&str,revision:&str,digest:&str,records:&[(String,bool)])->Result<(),String>{
    if let Some(parent)=path.parent(){fs::create_dir_all(parent).map_err(|e|format!("report directory: {e}"))?;}
    let level=if mode=="stdout-i64"{"source-executed-scalar"}else{"compiler-admission-only"};
    let mut text=format!("{{\n  \"schema_version\": \"1.0.0\",\n  \"backend\": \"{}\",\n  \"evidence_level\": \"{}\",\n  \"source_revision\": \"{}\",\n  \"suite_sha256\": \"{}\",\n  \"results\": [\n",json_escape(backend),level,revision,digest);
    for (index,(id,ok)) in records.iter().enumerate(){
        text.push_str(&format!("    {{\"id\": \"{}\", \"status\": \"{}\"}}{}\n",json_escape(id),if *ok{"pass"}else{"fail"},if index+1==records.len(){""}else{","}));
    }
    text.push_str("  ]\n}\n");
    fs::write(path,text).map_err(|e|format!("write report: {e}"))
}

fn self_test(root:&Path)->Result<(),String>{
    let suite=load_suite(root)?;
    if suite.digest!=CANONICAL_SUITE_SHA256{return Err(format!("canonical scalar digest drift: {}",suite.digest));}
    if suite.cases.len()!=11{return Err("canonical scalar inventory drift".into());}
    if parse_json(b"{\"x\":1,\"x\":2}").is_ok(){return Err("duplicate manifest key accepted".into());}
    if canonical_i64("01")||canonical_i64("+1")||!canonical_i64("-9223372036854775808"){return Err("canonical decimal admission drift".into());}
    let source_len=64;
    if !diagnostic_ok("forge","reject.unknown","oresforge: expected integer at byte 29\n",source_len){return Err("forge diagnostic rejected".into());}
    if diagnostic_ok("forge","reject.unknown","oresforge: expected integer at byte 999999\n",source_len){return Err("out-of-range diagnostic accepted".into());}
    if !diagnostic_ok("net","reject.division","oresnet: Division requires float result lowering; unsupported in the i64 source subset",source_len){return Err("net division diagnostic rejected".into());}
    let work=temp_dir("ores-scalar-selftest")?;
    let snapshot=snapshot_suite(root,&suite,&work)?;
    let original=root.join("contracts/scalar/v1/fixtures/literal-42.ores");
    let snap=snapshot.join("literal-42.ores");
    if fs::read(&original).map_err(|e|e.to_string())?!=fs::read(&snap).map_err(|e|e.to_string())?{return Err("snapshot mismatch".into());}
    fs::remove_dir_all(&work).map_err(|e|format!("cleanup selftest: {e}"))?;
    Ok(())
}

struct Options{
    backend:String,mode:String,command:String,revision:Option<String>,report:Option<PathBuf>,timeout:u64,self_test:bool,root:PathBuf,
}
fn parse_options()->Result<Options,String>{
    let mut backend=None;let mut mode=None;let mut command=None;let mut revision=None;let mut report=None;let mut timeout=20u64;let mut self_test=false;let mut root=PathBuf::from(".");
    let mut args=env::args_os().skip(1);
    while let Some(arg)=args.next(){
        let arg=arg.into_string().map_err(|_|"non-UTF8 argument".to_string())?;
        let next_string=|args:&mut std::iter::Skip<std::env::ArgsOs>,name:&str|->Result<String,String>{args.next().ok_or_else(||format!("missing {name} value"))?.into_string().map_err(|_|format!("non-UTF8 {name}"))};
        match arg.as_str(){
            "--backend"=>backend=Some(next_string(&mut args,"--backend")?),
            "--mode"=>mode=Some(next_string(&mut args,"--mode")?),
            "--command"=>command=Some(next_string(&mut args,"--command")?),
            "--revision"=>revision=Some(next_string(&mut args,"--revision")?),
            "--report"=>report=Some(PathBuf::from(next_string(&mut args,"--report")?)),
            "--timeout"=>timeout=next_string(&mut args,"--timeout")?.parse().map_err(|_|"invalid --timeout".to_string())?,
            "--repository-root"=>root=PathBuf::from(next_string(&mut args,"--repository-root")?),
            "--self-test"=>self_test=true,
            _=>return Err(format!("unknown argument: {arg}")),
        }
    }
    if self_test{return Ok(Options{backend:String::new(),mode:String::new(),command:String::new(),revision,report,timeout,self_test,root});}
    Ok(Options{backend:backend.ok_or_else(||"--backend required".to_string())?,mode:mode.ok_or_else(||"--mode required".to_string())?,command:command.ok_or_else(||"--command required".to_string())?,revision,report,timeout,self_test,root})
}

fn execute(options:Options)->Result<i32,String>{
    if options.self_test{self_test(&options.root)?;println!("SCALAR_RUNNER_SELFTEST_OK");return Ok(0);}
    let supported=match options.backend.as_str(){"forge"=>"stdout-i64","net"=>"admission",_=>return Err("unsupported backend/mode evidence pair".into())};
    if supported!=options.mode{return Err("unsupported backend/mode evidence pair".into());}
    if let Some(revision)=&options.revision{if !valid_revision(revision){return Err("--revision must be a full lowercase Git commit SHA".into());}}
    if options.report.is_some()&&options.revision.is_none(){return Err("--report requires --revision; unpinned execution evidence is not publishable".into());}
    if !(1..=300).contains(&options.timeout){return Err("--timeout must be 1..300 seconds".into());}
    let command=shlex_split(&options.command)?;
    if command.is_empty()||command.iter().map(|part|part.matches("{source}").count()).sum::<usize>()!=1{return Err("--command must contain exactly one {source} placeholder".into());}
    let suite=load_suite(&options.root)?;
    let work=temp_dir("ores-scalar-verified")?;
    let snapshot=snapshot_suite(&options.root,&suite,&work)?;
    let mut records=Vec::new();let mut failures=0usize;
    for case in &suite.cases{
        let source=snapshot.join(Path::new(&case.file).file_name().ok_or_else(||"fixture basename missing".to_string())?);
        let (ok,details)=run_case(&command,&source,case,&options.mode,Duration::from_secs(options.timeout),&options.backend);
        records.push((case.id.clone(),ok));if !ok{failures+=1;}
        println!("{} {}{}",if ok{"PASS"}else{"FAIL"},case.id,if ok{String::new()}else{format!(": {details}")});
    }
    if let Some(report)=&options.report{write_report(report,&options.backend,&options.mode,options.revision.as_deref().expect("validated revision"),&suite.digest,&records)?;}
    let _=fs::remove_dir_all(&work);
    println!("SCALAR_PARITY {} mode={} cases={} failures={}",options.backend,options.mode,records.len(),failures);
    Ok(if failures==0{0}else{1})
}

fn main(){
    match parse_options().and_then(execute){
        Ok(code)=>std::process::exit(code),
        Err(error)=>{eprintln!("SCALAR_PARITY_INFRASTRUCTURE_ERROR: {error}");std::process::exit(2);}
    }
}

#[cfg(test)]
mod tests{
    use super::*;
    #[test]fn sha256_vector(){assert_eq!(hex(&sha256(b"abc")),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");}
    #[test]fn command_split(){assert_eq!(shlex_split("tool --flag 'two words' {source}").unwrap(),vec!["tool","--flag","two words","{source}"]);}
    #[test]fn diagnostics_fail_closed(){
        assert!(diagnostic_ok("forge","reject.unknown","oresforge: expected integer at byte 29\n",64));
        assert!(!diagnostic_ok("forge","reject.unknown","oresforge: expected integer at byte 9999\n",64));
        assert!(!diagnostic_ok("wasm","reject.unknown","oresforge: expected integer at byte 29\n",64));
    }
    #[test]fn canonical_suite(){self_test(Path::new(".")).unwrap();}
    #[cfg(unix)]
    #[test]fn symlink_and_hardlink_rejected(){
        use std::os::unix::fs::symlink;
        let root=temp_dir("ores-scalar-filetest").unwrap();
        let file=root.join("x");fs::write(&file,b"x").unwrap();
        let symlink_path=root.join("s");symlink(&file,&symlink_path).unwrap();
        assert!(read_bounded_regular(&symlink_path).is_err());
        let hard=root.join("h");fs::hard_link(&file,&hard).unwrap();
        assert!(read_bounded_regular(&file).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
