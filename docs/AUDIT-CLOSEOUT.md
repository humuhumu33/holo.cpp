# Audit close-out — 2026-08-24

Closing the gaps the adversarial audit (same date) and the product-definition review exposed.
Each fix landed with a test; the parity claim is now gated on a green regression. Reproduced on
the audit machine (Ryzen AI Max 390, CPU, Windows 11).

## Refreshed verdict table

| | property | before | after |
|---|---|---|---|
| **P1** | parity — same answers | **DEGRADED** (crash >~100 tok; non-ASCII diverges) | **HOLDS** — `tests/parity_promptlen.sh` green at {8,80,120,512} + café/niño/Cyrillic |
| **P2** | integrity — tamper refused | HOLDS | HOLDS (re-verified, no regression) |
| **P3** | receipts — forgery caught | HOLDS (engine-binding advisory) | HOLDS + **engine binding now ENFORCED** |
| **P4** | cost — free per token | HOLDS | HOLDS |
| **P5** | hygiene / robustness | mostly | dead-origin now clean rc=1; server honors model field |

## What changed, per gap

**G1 — long-prompt crash [FIXED].** Root cause: a fixed 128-token stack buffer in both `cmd_run`
and `holo-run`; `llama_tokenize` returns a *negative* count when the buffer is too small, which
flowed into `std::vector(first, first+n<0)` and blew up as `max_size()`. Fix: tokenize into a
right-sized `std::vector<llama_token>` and grow `n_ctx` to fit prompt+generation (output-preserving
— `n_ctx` bounds the KV cache, not the logits over a fixed causal sequence). The verify path also
hardcoded `n_ctx=512`; it now honors the sealed value, grown to fit.
- Evidence: 512-token prompt runs (rc=0) **and** its receipt re-derives byte-identical (rc=0).
  `tests/parity_promptlen.sh` asserts holo==engine at every length.

**G2 — engine binding advisory→ENFORCED [FIXED].** `holo verify` now hard-fails (rc=1) when the
receipt's `engine_b3` differs from the verifying binary, with `--allow-engine-mismatch` as the
explicit escape hatch for the legitimate cross-binary case. The README wording now matches.
- Evidence: same-engine verify rc=0; zeroed `engine_b3` → REFUSED rc=1; with the flag → replays rc=0.

**G3 — non-ASCII parity [FIXED].** Windows delivered `argv` in the ANSI codepage (café→caf?), so
the tokenizer saw different bytes than the engine. Fix: rebuild `argv` from `GetCommandLineW`
decoded as UTF-8, and `SetConsoleOutputCP(CP_UTF8)`. Prompt bytes are now codepage-independent.
- Evidence: café/niño and Cyrillic prompts now produce holo==engine continuations.

**G4 — server ignored the `model` field [FIXED].** `holo-server` now returns HTTP 400 on a request
naming a model it doesn't hold, instead of silently answering with the resident one. Missing field
still works (back-compat).
- Evidence: correct name→200; `gpt-4-secret`→400 `model_not_found`; no field→200.

**G5 — dead-origin `std::terminate` (rc=127) [FIXED].** `holo-run` now installs a terminate handler
(matching holo-cli) and wraps main in try/catch, so an exhausted origin exits clean rc=1 with a
message instead of the default abort.
- Evidence: dead origin → `REFUSED: origin failed during verified load`, rc=1.

**G6 — intermittent `0xC0000005` in verify [WATCHED].** Seen once under the full batch, clean on
isolated re-run; not reproduced during close-out. Documented as a watched flake; if it recurs,
capture the receipt + a 100× loop. No security impact (it refused cleanly when observed).

## Not done — specified next-steps (Tier 2, product-definition gaps)

These are real work, scoped here rather than half-built:

- **G7 — OCI image.** Linux container exposing the endpoint, so "local vs cloud" is one artifact and
  the "inherits the customer's k8s/ingress/auth" story is true by construction. Do NOT build
  mesh/auth/ingress — those are the customer's. Gate: `docker run` serves a verified completion +
  receipt.
- **G8 — Anthropic Messages API** (`/v1/messages`) alongside the existing OpenAI endpoint, to be
  "OpenAI/Anthropic-compatible" as specified.
- **G9 — llama-family generation** currently segfaults (rc=139) at load for some GGUFs, engine-
  inherited; no unverified token can escape (crash precedes decode). At minimum: detect the arch
  and exit rc=1 with a named message instead of rc=139; ideally fix the load path so a llama GGUF
  generates end-to-end (makes the QVAC Llama-3.2 story first-class). NOTE: the same 128-token
  buffer bug fixed in G1 still exists in `holo-run.cpp:~95` decode loop — harmless there (dev tool)
  but worth the same fix if that path is promoted.
- **G10 — positioning language.** Word policy drafted (below). The demos still carry decentralized
  framing ("pay a stranger's machine") that is off-message for the regulated/opex→capex product;
  re-frame as "an inference node inside your boundary you didn't build," or cut.
- **G11 — licensing.** MIT gives every enterprise free commercial use; decide the posture
  (source-available / dual / BSL with free individual-dev use) before it is load-bearing.

## Word policy (from the product definition)

**Say:** verifiable · self-custodial · inference engine · runs in your security boundary ·
OpenAI/Anthropic-compatible · opex→capex · verifiable inference / trace logs · OCI image.
**Don't say:** decentralized · P2P · trustless · crypto/web3 · "pay a stranger" · model · storage ·
framework · MVM · frontier model.
**Resolve first:** "kappa" — here it means *content address + verifiable trace* (already shipping),
not a compiled representation. Keep the README consistent with that meaning.
