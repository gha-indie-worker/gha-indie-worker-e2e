#!/usr/bin/env python3
"""Reference-only consistency checks for callable-syntax-v1.json.

This is not an Oreslang parser. It pins the shared admission decisions so
backend fixtures can import one finite oracle instead of duplicating policy.
"""

from __future__ import annotations

import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "fixtures" / "callable-syntax-v1.json"


def expected_declaration(
    domain: str,
    body: str,
    separator: str,
    body_boundary: str | None = None,
    body_separator_line: str | None = None,
) -> str:
    if body == "end":
        if body_boundary == "do":
            return "reject"
        if body_boundary == "as" and body_separator_line != "header":
            return "reject"
        return (
            "accept"
            if domain in {"root_function", "module_function"} and separator == "->"
            else "reject"
        )
    if body == "brace":
        return (
            "accept"
            if domain in {
                "root_function",
                "module_function",
                "instance_method",
                "static_method",
                "actor_callable",
            }
            and separator in {":", "->", "none"}
            else "reject"
        )
    if body == "signature":
        if domain in {"module_contract_signature", "static_contract_signature"}:
            return "accept" if separator in {"->", ":", "=>"} else "reject"
        if domain in {"interface_signature", "trait_signature"}:
            return "accept" if separator == "=>" else "reject"
    return "reject"


class CallableSyntaxV1ContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data = json.loads(FIXTURE.read_text(encoding="utf-8"))

    def test_schema_and_ids(self) -> None:
        self.assertEqual(self.data["schema"], "oreslang-callable-syntax-v1")
        ids: list[str] = []
        for group in ("cases", "return_flow", "tuple_types"):
            ids.extend(item["id"] for item in self.data[group])
        self.assertEqual(len(ids), len(set(ids)))

    def test_declaration_matrix_matches_v1_policy(self) -> None:
        for case in self.data["cases"]:
            with self.subTest(case=case["id"]):
                self.assertEqual(
                    case["expected"],
                    expected_declaration(
                        case["domain"],
                        case["body"],
                        case["separator"],
                        case.get("body_boundary"),
                        case.get("body_separator_line"),
                    ),
                )

    def test_return_flow_matrix(self) -> None:
        for case in self.data["return_flow"]:
            with self.subTest(case=case["id"]):
                if case.get("return_type") == "void" and case.get("returns_value"):
                    expected = "reject"
                elif case.get("return_type") == "void":
                    expected = "accept"
                else:
                    expected = "reject" if case.get("normal_fallthrough") else "accept"
                self.assertEqual(case["expected"], expected)

    def test_tuple_spellings_preserve_existing_arity_policy(self) -> None:
        by_id = {case["id"]: case for case in self.data["tuple_types"]}
        self.assertEqual(by_id["tuple-large-65"]["expected"], "accept")
        self.assertEqual(by_id["tuple-existing-large-65"]["expected"], "accept")
        self.assertEqual(by_id["tuple-large-65"]["slots"], by_id["tuple-existing-large-65"]["slots"])

    def test_required_safety_edges_are_present(self) -> None:
        by_id = {case["id"]: case for case in self.data["cases"]}
        for case_id in (
            "root-end-implicit-return",
            "root-end-colon",
            "class-method-end",
            "class-static-end",
            "actor-callable-end",
            "interface-arrow",
        ):
            self.assertEqual(by_id[case_id]["expected"], "reject")
        self.assertEqual(by_id["module-contract-arrow"]["expected"], "accept")
        self.assertEqual(by_id["static-contract-arrow"]["expected"], "accept")
        self.assertEqual(by_id["root-end-arrow-as"]["expected"], "accept")
        self.assertEqual(by_id["root-end-arrow-as"]["body_boundary"], "as")
        self.assertEqual(by_id["root-end-arrow-as"]["body_separator_line"], "header")
        self.assertEqual(by_id["root-end-arrow-as-leading-do-select"]["expected"], "accept")
        self.assertEqual(by_id["root-end-arrow-as-leading-do-select"]["first_statement"], "do_select")
        self.assertEqual(by_id["root-end-arrow-as-next-line"]["expected"], "reject")
        self.assertEqual(by_id["root-end-arrow-as-next-line"]["body_separator_line"], "body")
        self.assertEqual(by_id["root-end-arrow-do-retired"]["expected"], "reject")
        self.assertEqual(by_id["root-end-arrow-leading-do-select"]["expected"], "accept")
        self.assertEqual(by_id["root-end-arrow-leading-do-select"]["first_statement"], "do_select")
        self.assertEqual(by_id["root-end-colon-as"]["expected"], "reject")


if __name__ == "__main__":
    unittest.main()
