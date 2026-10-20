#!/usr/bin/env python3
"""Fail-closed structural validator for Oreslang reachability reports.

This intentionally avoids a third-party JSON Schema runtime so every backend CI
can validate the baseline contract with stock Python. The committed JSON Schema
remains the interchange definition; this script enforces the security-critical
subset and cross-field invariants.
"""
from __future__ import annotations
import argparse, json, re, sys
from pathlib import Path

SHA = re.compile(r"^[0-9a-f]{40}$")
KINDS = {"function","type","module","runtime","capability","metadata","data","asset","hardware","other"}
ARTIFACTS = {"executable","library","firmware","wasm","mobile-bundle","fpga","asic","test","other"}
POLICIES = {"explicit","conservative","rejected"}

class GateError(ValueError):
    pass

def _text(v, name, limit):
    if not isinstance(v, str) or not v or len(v) > limit:
        raise GateError(f"{name} must be non-empty text <= {limit} chars")
    return v

def _node(v, name):
    if not isinstance(v, dict) or set(v) - {"id","kind","reason","parent"}:
        raise GateError(f"{name} has unknown or malformed fields")
    for k in ("id","kind","reason"):
        if k not in v:
            raise GateError(f"{name} missing {k}")
    _text(v["id"], f"{name}.id", 512)
    _text(v["reason"], f"{name}.reason", 2048)
    if v["kind"] not in KINDS:
        raise GateError(f"{name}.kind is unknown")
    if "parent" in v:
        _text(v["parent"], f"{name}.parent", 512)

def validate(doc):
    if not isinstance(doc, dict):
        raise GateError("report must be an object")
    required={"schema_version","backend","profile","target","artifact_kind","roots","retained","removed","dynamic_regions"}
    allowed=required|{"compiler_revision","artifact_bytes","notes"}
    if set(doc)-allowed:
        raise GateError("unknown top-level fields: "+", ".join(sorted(set(doc)-allowed)))
    if required-set(doc):
        raise GateError("missing top-level fields: "+", ".join(sorted(required-set(doc))))
    if doc["schema_version"]!="1.0.0":
        raise GateError("unsupported schema_version")
    for k,limit in (("backend",64),("profile",64),("target",128)):
        _text(doc[k], k, limit)
    if doc["artifact_kind"] not in ARTIFACTS:
        raise GateError("unknown artifact_kind")
    if "compiler_revision" in doc and (not isinstance(doc["compiler_revision"],str) or not SHA.fullmatch(doc["compiler_revision"])):
        raise GateError("compiler_revision must be a 40-hex Git SHA")
    for name,limit in (("roots",100000),("retained",1000000)):
        values=doc[name]
        if not isinstance(values,list) or len(values)>limit:
            raise GateError(f"{name} must be a bounded array")
        seen=set()
        for i,node in enumerate(values):
            _node(node,f"{name}[{i}]")
            if node["id"] in seen:
                raise GateError(f"duplicate {name} node id: {node['id']}")
            seen.add(node["id"])
    retained_ids={n["id"] for n in doc["retained"]}
    for root in doc["roots"]:
        if root["id"] not in retained_ids:
            raise GateError("every root must also be retained: "+root["id"])
    removed=doc["removed"]
    if not isinstance(removed,dict) or set(removed)-{"total","by_kind"} or "total" not in removed:
        raise GateError("removed must contain total and optional by_kind")
    if type(removed["total"]) is not int or removed["total"]<0:
        raise GateError("removed.total must be a nonnegative integer")
    by=removed.get("by_kind",{})
    if not isinstance(by,dict) or any(type(v) is not int or v<0 for v in by.values()):
        raise GateError("removed.by_kind must map names to nonnegative integers")
    if by and sum(by.values())!=removed["total"]:
        raise GateError("removed.by_kind must sum to removed.total")
    dynamic=doc["dynamic_regions"]
    if not isinstance(dynamic,list) or len(dynamic)>10000:
        raise GateError("dynamic_regions must be a bounded array")
    for i,item in enumerate(dynamic):
        if not isinstance(item,dict) or set(item)!={"id","policy","reason"}:
            raise GateError(f"dynamic_regions[{i}] malformed")
        _text(item["id"],f"dynamic_regions[{i}].id",512)
        _text(item["reason"],f"dynamic_regions[{i}].reason",2048)
        if item["policy"] not in POLICIES:
            raise GateError(f"dynamic_regions[{i}].policy unknown")
    if "artifact_bytes" in doc and (type(doc["artifact_bytes"]) is not int or doc["artifact_bytes"]<0):
        raise GateError("artifact_bytes must be a nonnegative integer")
    notes=doc.get("notes",[])
    if not isinstance(notes,list) or len(notes)>1000 or any(not isinstance(x,str) or len(x)>4096 for x in notes):
        raise GateError("notes must be a bounded string array")
    return len(doc["retained"])

def main(argv=None):
    p=argparse.ArgumentParser()
    p.add_argument("report",type=Path)
    a=p.parse_args(argv)
    try:
        doc=json.loads(a.report.read_text(encoding="utf-8"))
        count=validate(doc)
    except (OSError,UnicodeError,json.JSONDecodeError,GateError) as e:
        print("REACHABILITY_REPORT_DENIED: "+str(e),file=sys.stderr)
        return 1
    print(f"REACHABILITY_REPORT_OK retained={count} removed={doc['removed']['total']}")
    return 0

if __name__=="__main__":
    raise SystemExit(main())
