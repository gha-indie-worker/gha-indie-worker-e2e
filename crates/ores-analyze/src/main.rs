use ores_analysis::{analyze, Profile, Program, MAX_IR_BYTES};
use std::{
    env, fs,
    io::{self, Read},
    process::ExitCode,
};
fn read_ir(path: &str) -> io::Result<String> {
    let reader: Box<dyn Read> = if path == "-" {
        Box::new(io::stdin())
    } else {
        Box::new(fs::File::open(path)?)
    };
    let mut bounded = reader.take(MAX_IR_BYTES as u64 + 1);
    let mut input = String::new();
    bounded.read_to_string(&mut input)?;
    if input.len() > MAX_IR_BYTES {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "effect IR exceeds 8 MiB limit"));
    }
    Ok(input)
}
fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: ores-analyze <input.json|-> <general|soft|hard>");
        return ExitCode::from(2);
    }
    let profile = match args[2].as_str() {
        "general" => Profile::General,
        "soft" => Profile::SoftRealtime,
        "hard" => Profile::HardRealtime,
        _ => {
            eprintln!("unknown profile: {}", args[2]);
            return ExitCode::from(2);
        }
    };
    let input = read_ir(&args[1]);
    let program: Program = match input {
        Ok(data) => match serde_json::from_str(&data) {
            Ok(program) => program,
            Err(error) => {
                eprintln!("invalid effect IR: {error}");
                return ExitCode::from(2);
            }
        },
        Err(error) => {
            eprintln!("cannot read input: {error}");
            return ExitCode::from(2);
        }
    };
    let report = analyze(&program, profile);
    match serde_json::to_string_pretty(&report) {
        Ok(json) => println!("{json}"),
        Err(error) => {
            eprintln!("serialization failed: {error}");
            return ExitCode::from(2);
        }
    }
    if report.has_errors() {
        ExitCode::from(1)
    } else {
        ExitCode::SUCCESS
    }
}
