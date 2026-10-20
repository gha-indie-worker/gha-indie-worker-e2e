# Reactive composition trace model v1

Draft test helper `analyze_reactive_events` consumes finite host-provided event records with `subscribe`, `request`, `next`, `complete`, `cancel`, `error`, `dispose` operations. Each event has a subscription identity; subscribe also identifies source and mode (`hot_single` or `cold`). Requests grant bounded downstream demand. A hot single-consumer source cannot masquerade as multicast. Failure and cancellation remain distinct from normal completion. Every observed terminal requires exactly one disposal.

This is a host **protocol model**, not runtime instrumentation, not a `--race` proof, not a scheduler simulator, and not a new `--fuzz` CLI option. These invariants should later be checked against freshly captured source actor/Observable/Stream traces from Java and LLVM; they do not replace existing race/fuzz tests. Versioned cross-repo contract: common-source #36, RX implementation #12. Run `python3 -m unittest discover -s tests -v`.
