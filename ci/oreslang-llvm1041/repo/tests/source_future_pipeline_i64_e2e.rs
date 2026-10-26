use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH)
            .expect("clock").as_nanos();
        let path = std::env::temp_dir().join(format!(
            "ores-future-pipeline-{}-{stamp}", std::process::id()));
        fs::create_dir(&path).expect("create temp");
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

fn compile(compiler: &Path, dir: &Path, name: &str, source: &str)
    -> std::process::Output
{
    let input = dir.join(name);
    fs::write(&input, source).expect("write source");
    let output = Command::new(compiler)
        .arg(&input)
        .current_dir(dir)
        .output()
        .expect("run oreslang-llvmc");
    assert_eq!(fs::read_to_string(&input).expect("read source"), source);
    output
}

fn rejected(compiler: &Path, dir: &Path, name: &str, source: &str, diagnostic: &str) {
    let output = compile(compiler, dir, name, source);
    assert!(!output.status.success(), "{name}: expected rejection");
    assert!(output.stdout.iter().all(u8::is_ascii_whitespace),
        "{name}: rejection emitted LLVM IR");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains(diagnostic),
        "{name}: missing diagnostic {diagnostic:?}; stderr={stderr}");
}

fn link_and_run(clang: &Path, root: &Path, dir: &Path, stem: &str, ir: &[u8]) {
    let ll = dir.join(format!("{stem}.ll"));
    let exe = dir.join(stem);
    fs::write(&ll, ir).expect("write IR");
    let build = Command::new(clang)
        .arg(&ll)
        .arg(root.join("src/main/cpp/oreslang/runtime/SourceTaskRuntime.c"))
        .arg(root.join("src/main/cpp/oreslang/runtime/NativeActorScheduler.c"))
        .arg("-I").arg(root.join("src/main/cpp"))
        .arg("-std=c11")
        .arg("-pthread")
        .arg("-DORESLANG_SOURCE_TASK_TESTING=1")
        .arg("-o").arg(&exe)
        .current_dir(dir)
        .output().expect("run clang");
    assert!(build.status.success(),
        "{stem}: native link failed: {}", String::from_utf8_lossy(&build.stderr));

    let run = Command::new(&exe).current_dir(dir)
        .output().expect("run future native binary");
    assert_eq!(run.status.code(), Some(42),
        "{stem}: native result must be 42; stderr={}",
        String::from_utf8_lossy(&run.stderr));
    let trace = String::from_utf8_lossy(&run.stderr);
    assert!(trace.contains("SOURCE_TASK_RUNTIME_PASS"));
    assert!(trace.contains("producer_on_carrier=1"));
    assert!(trace.contains("carriers=1"));
}

fn main() {
    let mut args = std::env::args_os().skip(1);
    let compiler = PathBuf::from(args.next()
        .expect("usage: future-pipeline-e2e COMPILER CLANG SOURCE_ROOT"))
        .canonicalize().expect("compiler");
    let clang = PathBuf::from(args.next().expect("missing clang"))
        .canonicalize().expect("clang");
    let root = PathBuf::from(args.next().expect("missing source root"))
        .canonicalize().expect("source root");
    let dir = Temp::new();

    let consumed = fs::read_to_string(
        root.join("tests/fixtures/source-future-pipeline-i64.ores"))
        .expect("read canonical Future pipeline fixture");
    let output = compile(&compiler, &dir.0, "consumed.ores", &consumed);
    assert!(output.status.success(),
        "consumed Future source compile failed: {}",
        String::from_utf8_lossy(&output.stderr));
    let ir = String::from_utf8_lossy(&output.stdout);
    for needle in [
        "oreslang_source_task_start_i64",
        "oreslang_source_task_await_i64",
        "oreslang_source_task_runtime_shutdown",
        ".future.pending.consumed.producer",
        "pending.plus_one.value",
        "pending.twice.value",
    ] {
        assert!(ir.contains(needle), "missing Future pipeline IR marker {needle}");
    }
    link_and_run(&clang, &root, &dir.0, "consumed", &output.stdout);

    let parenthesized = consumed.replace(
        "rt take source()", "rt take(source())");
    let parenthesized_output = compile(
        &compiler, &dir.0, "consumed-parenthesized.ores", &parenthesized);
    assert!(parenthesized_output.status.success(),
        "parenthesized rt take Future source compile failed: {}",
        String::from_utf8_lossy(&parenthesized_output.stderr));
    let parenthesized_ir = String::from_utf8_lossy(&parenthesized_output.stdout);
    assert!(parenthesized_ir.contains(".future.pending.consumed.producer"),
        "parenthesized rt take lost consumed-root lowering marker");
    link_and_run(
        &clang, &root, &dir.0, "consumed-parenthesized",
        &parenthesized_output.stdout);

    let async_stage = fs::read_to_string(
        root.join("tests/fixtures/source-future-pipeline-async-stage-i64.ores"))
        .expect("read async-stage Future pipeline fixture");
    let async_output = compile(
        &compiler, &dir.0, "async-stage.ores", &async_stage);
    assert!(async_output.status.success(),
        "async-stage Future source compile failed: {}",
        String::from_utf8_lossy(&async_output.stderr));
    let async_ir = String::from_utf8_lossy(&async_output.stdout);
    for needle in [
        ".future.pending.consumed.producer",
        "pending.plus_one_async.value",
        "pending.twice.value",
    ] {
        assert!(async_ir.contains(needle),
            "missing async-stage Future pipeline IR marker {needle}");
    }
    link_and_run(
        &clang, &root, &dir.0, "async-stage", &async_output.stdout);

    let borrowed = consumed.replace("rt take source()", "source()");
    rejected(&compiler, &dir.0, "borrowed.ores", &borrowed,
        "pipeline v1 execution requires an explicit rt take root");

    rejected(&compiler, &dir.0, "direct-async.ores",
        r#"async fnc source(): int { return 21; }
pub fnc main(): int { return source(); }
"#, "async source call requires checked Future<int> pipeline lowering");

    rejected(&compiler, &dir.0, "bad-async-stage-param.ores",
        r#"async fnc source(): int { return 21; }
async fnc bad(bool value): int { return value; }
pub fnc main(): int {
    let Future<int> pending = rt take source() |> bad();
    let int result = await pending;
    return result;
}
"#, "native async Future<int> v1 callable must be capture-free");

    rejected(&compiler, &dir.0, "impure-stage.ores",
        r#"async fnc source(): int { return 21; }
fnc keep(int value): int { let int copy = value; return copy; }
pub fnc main(): int {
    let Future<int> pending = rt take source() |> keep();
    let int result = await pending;
    return result;
}
"#, "pipeline stage v1 requires one pure int -> int callable");

    rejected(&compiler, &dir.0, "async-stage-capture.ores",
        r#"async fnc source(): int { return 21; }
async fnc keep(int value): int { return value + missing; }
pub fnc main(): int {
    let Future<int> pending = rt take source() |> keep();
    let int result = await pending;
    return result;
}
"#, "body may reference only its exact value-int parameter");

    rejected(&compiler, &dir.0, "extra-stage-arg.ores",
        r#"async fnc source(): int { return 21; }
fnc add(int value): int { return value + 1; }
pub fnc main(): int {
    let Future<int> pending = rt take source() |> add(1);
    let int result = await pending;
    return result;
}
"#, "pipeline stage requires no extra arguments");

    rejected(&compiler, &dir.0, "take-lifted.ores",
        r#"async fnc source(): int { return 21; }
fnc keep(int value): int { return value; }
pub fnc main(): int {
    let Future<int> pending = rt take (source() |> keep()) |> keep();
    let int result = await pending;
    return result;
}
"#, "taking an already-lifted pipeline is not yet supported");

    rejected(&compiler, &dir.0, "unawaited.ores",
        r#"async fnc source(): int { return 21; }
fnc keep(int value): int { return value; }
pub fnc main(): int {
    let Future<int> pending = rt take source() |> keep();
    return 0;
}
"#, "requires one consumed checked pipeline plan");

    let stages = std::iter::repeat(" |> keep()").take(129).collect::<String>();
    let too_many = format!(
        "async fnc source(): int {{ return 1; }}\nfnc keep(int value): int {{ return value; }}\npub fnc main(): int {{\n let Future<int> pending = rt take source(){stages};\n let int result = await pending;\n return result;\n}}\n");
    rejected(&compiler, &dir.0, "too-many.ores", &too_many,
        "Future pipeline exceeds 128 stages");

    let tree_source = dir.0.join("tree-shake.ores");
    fs::write(&tree_source, &consumed).expect("write tree-shake source");
    let tree = Command::new(&compiler)
        .arg("--tree-shake")
        .arg(&tree_source)
        .current_dir(&dir.0)
        .output()
        .expect("run tree-shake Future compiler");
    assert!(!tree.status.success(),
        "Future pipeline unexpectedly entered metadata-unaware tree shaking");
    assert!(tree.stdout.iter().all(u8::is_ascii_whitespace),
        "tree-shake rejection emitted LLVM IR");
    let tree_stderr = String::from_utf8_lossy(&tree.stderr);
    assert!(tree_stderr.contains(
        "tree shaking currently rejects Task/Future pipeline metadata edges"),
        "wrong tree-shake Future diagnostic: {tree_stderr}");

    println!("PASS: source Future<int> producer/async-stage -> checked |> -> scheduler carrier -> await -> native 42");
}
