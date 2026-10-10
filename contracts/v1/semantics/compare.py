#!/usr/bin/env python3
"""Fail-closed comparison of two independent backend conformance reports."""
import argparse
import json
from pathlib import Path
import sys

from run import validate_suite

def compare(suite, digest, llvm, graal):
    try:
        import jsonschema
    except ImportError as exc:
        raise ValueError("jsonschema is required to compare reports") from exc
    report_schema = json.loads((Path(__file__).parent / "report.schema.json").read_text())
    records = {}
    for backend, report in (("llvm", llvm), ("graal", graal)):
        jsonschema.validate(report, report_schema)
        if report["backend"] != backend or report["suite_sha256"] != digest:
            raise ValueError(f"wrong backend or suite hash for {backend}")
        ids = [r["id"] for r in report["results"]]
        expected = [case["id"] for case in suite["cases"]]
        if len(ids) != len(set(ids)) or set(ids) != set(expected):
            raise ValueError(f"duplicate, missing or extra test IDs for {backend}")
        records[backend] = {r["id"]: r for r in report["results"]}
    common = 0
    missing = 0
    defects = []
    for case in suite["cases"]:
        statuses = []
        for backend in ("llvm", "graal"):
            record = records[backend][case["id"]]
            status = record["status"]
            if case["backends"][backend] == "pending":
                if status != "pending" or record.get("observed_exit") is not None:
                    defects.append(f"{case['id']} {backend}: pending expected, got {status}")
                else:
                    missing += 1
            else:
                if status != "pass":
                    defects.append(f"{case['id']} {backend}: required test status {status}")
                elif case["kind"] == "native_exit" and record.get("observed_exit") != case["expected_exit"]:
                    defects.append(f"{case['id']} {backend}: unexpected observed native exit")
                elif case["kind"] == "diagnostic" and record.get("diagnostic_class") != case["diagnostic_class"]:
                    defects.append(f"{case['id']} {backend}: wrong diagnostic class")
            statuses.append(status)
        if case["backends"]["llvm"] == "required" and case["backends"]["graal"] == "required":
            if statuses == ["pass", "pass"]:
                common += 1
    for defect in defects:
        print("FAIL:", defect, file=sys.stderr)
    print(f"COMMON_VERIFIED={common} BACKEND_PENDING={missing} DEFECTS={len(defects)}")
    return 1 if defects else 0

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--llvm", required=True, type=Path)
    p.add_argument("--graal", required=True, type=Path)
    opts = p.parse_args()
    suite, digest = validate_suite()
    return compare(suite, digest, json.loads(opts.llvm.read_text()), json.loads(opts.graal.read_text()))

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as ex:
        print(f"Comparison infrastructure error: {ex}", file=sys.stderr)
        sys.exit(2)
