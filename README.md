# Source-free exact Git tree bridge fixture

This repository branch is a synthetic 3-file fixture only, constructed solely
to exercise exact-root Git tree reconstruction via the GitHub connector.

It contains **no Oreslang private source, artifacts, credentials or tokens**.
A mirrored root tree with the same SHA demonstrates content identity, but
**not** independent authorization to move private source. The workflow
runs with its own contents:read repository token and never fetches another repo.

The negative matrix models fail-closed source/destination protection,
independent review, exact source HEAD/tree, actual checkout tree and
positive GitHub runner/step identity. It is a mock governance model, not
a test of an Oreslang compiler, JS Worker, WASM, .NET or Native AOT runtime.
