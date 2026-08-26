import { assert } from "./helpers.mjs";

const avenues = [
  "direct_read_only_db",
  "stateless_http",
  "stateful_tcp",
  "nats_async",
];
assert(avenues.length === 4, "four web/api avenues");
console.log("gha-indie-worker four-avenue catalog ok");

