import json
import tempfile
import unittest
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"scripts"))
from reachability_report_gate import GateError, validate

def sample():
    return {
      "schema_version":"1.0.0","backend":"llvm","profile":"minimal",
      "target":"x86_64-unknown-linux-gnu","artifact_kind":"executable",
      "roots":[{"id":"app::main/0","kind":"function","reason":"entrypoint"}],
      "retained":[
        {"id":"app::main/0","kind":"function","reason":"entrypoint"},
        {"id":"app::helper/0","kind":"function","reason":"direct call","parent":"app::main/0"}
      ],
      "removed":{"total":2,"by_kind":{"function":1,"runtime":1}},
      "dynamic_regions":[]
    }

class ReachabilityReportTests(unittest.TestCase):
    def test_accepts_minimal_valid_report(self):
        self.assertEqual(validate(sample()),2)

    def test_root_must_be_retained(self):
        d=sample(); d["retained"]=d["retained"][1:]
        with self.assertRaises(GateError): validate(d)

    def test_removed_sum_is_exact(self):
        d=sample(); d["removed"]["by_kind"]["runtime"]=2
        with self.assertRaises(GateError): validate(d)

    def test_unknown_fields_fail_closed(self):
        d=sample(); d["surprise"]=True
        with self.assertRaises(GateError): validate(d)

    def test_boolean_is_not_integer_measurement(self):
        d=sample(); d["artifact_bytes"]=True
        with self.assertRaises(GateError): validate(d)

if __name__=="__main__":
    unittest.main()
