#!/usr/bin/env bash
set -euo pipefail

: "${EXPECTED_SOURCE_TREE_SHA:?Set EXPECTED_SOURCE_TREE_SHA to the exact private source Git tree SHA}"
if [[ ! ${EXPECTED_SOURCE_TREE_SHA} =~ ^[0-9a-f]{40}$ ]]; then
  echo "Invalid source Git tree SHA" >&2
  exit 2
fi
actual="$(git rev-parse 'HEAD^{tree}')"
if [[ "${actual}" != "${EXPECTED_SOURCE_TREE_SHA}" ]]; then
  echo "MISMATCH: checked-out Git tree does not match source PR" >&2
  exit 2
fi
git diff --quiet HEAD -- || { echo "Tracked files modified before test" >&2; exit 2; }
if [[ -n "$(git ls-files --others --exclude-standard)" ]]; then
  echo "Untracked checkout files present before exact-tree test; refusing contaminated inputs" >&2
  exit 2
fi
python3 -V
PYTHONPATH=src python3 -m unittest discover -s tests -p 'test_*.py' -v
git diff --quiet HEAD -- || { echo "Tracked files modified during test" >&2; exit 2; }
echo "ORES_CONCURRENCY_CI|backend=python-cli|source_tree=${actual}|result=PASS|scope=unittest"
