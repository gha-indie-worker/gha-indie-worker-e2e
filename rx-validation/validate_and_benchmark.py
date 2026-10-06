#!/usr/bin/env python3
from __future__ import annotations
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CP = os.environ["ORES_CLASSPATH"]
JAVA = os.environ.get("JAVA", "java")
CMD = [
    JAVA,
    "--enable-native-access=ALL-UNNAMED",
    "-Dpolyglot.engine.WarnInterpreterOnly=false",
    "-cp", CP,
    "dev.oreslang.launcher.OresMain",
]
CASES = {
    "pull": ("rx-validation/pull", "60000"),
    "callbacks": ("rx-validation/callbacks", "100000"),
    "channels": ("rx-validation/channels", "60000"),
}
PIPELINES = 20_000
SOURCE_ITEMS = 5

def invoke(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(CMD + args, cwd=ROOT, text=True, capture_output=True, timeout=180)

def checked(args: list[str]) -> str:
    result = invoke(args)
    if result.returncode or ": error:" in result.stderr:
        raise SystemExit(result.stdout + result.stderr)
    return result.stdout.strip()

for name, (base, expected) in CASES.items():
    checked(["--check", f"{base}/src/rx.ores"])
    checked(["--check", f"{base}/bench/pipeline.ores"])
    actual = checked([f"{base}/bench/pipeline.ores"])
    if actual != expected:
        raise SystemExit(f"{name}: expected checksum {expected}, got {actual!r}")
    print(f"VALID {name}: checksum={actual}")

print("\nBENCHMARK (2 warmups, 7 samples)")
for name, (base, expected) in CASES.items():
    program = f"{base}/bench/pipeline.ores"
    for _ in range(2):
        if checked([program]) != expected:
            raise SystemExit(f"{name}: warmup checksum changed")
    samples = []
    for _ in range(7):
        start = time.perf_counter()
        actual = checked([program])
        samples.append(time.perf_counter() - start)
        if actual != expected:
            raise SystemExit(f"{name}: measured checksum changed")
    median = statistics.median(samples)
    ordered = sorted(samples)
    p95 = ordered[-1]
    print(
        f"RESULT {name} "
        f"median_ms={median*1000:.3f} "
        f"p95_ms={p95*1000:.3f} "
        f"pipelines_per_s={PIPELINES/median:.1f} "
        f"ns_per_source_item={median*1e9/(PIPELINES*SOURCE_ITEMS):.1f}"
    )
