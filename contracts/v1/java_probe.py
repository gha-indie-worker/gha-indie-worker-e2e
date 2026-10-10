#!/usr/bin/env python3
"""Invoke the existing Graal/Java Oreslang compiler's --check mode.

The launcher reads classpath resolved by Maven; this probe does not execute
guest Oreslang code. Calling via argv avoids shell quoting mistakes.
"""
import os
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise ValueError("usage: java_probe.py <fixture.ores>")
    path = Path(sys.argv[1]).resolve()
    if not path.is_file() or path.suffix != ".ores":
        raise ValueError("invalid source fixture")
    project = Path(__file__).resolve().parents[2]
    cpfile = project / "target" / "conformance-classpath.txt"
    deps = cpfile.read_text().strip()
    if not deps:
        raise ValueError("missing Maven dependency classpath")
    classes = project / "target" / "classes"
    argv = ["java", "-cp", str(classes) + os.pathsep + deps,
            "dev.oreslang.launcher.OresMain", "--check", str(path)]
    result = subprocess.run(argv, cwd=project, timeout=80, check=False)
    return result.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.TimeoutExpired) as ex:
        print(f"Java compiler probe failed: {ex}", file=sys.stderr)
        sys.exit(2)
