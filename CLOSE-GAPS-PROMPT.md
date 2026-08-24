# PROMPT: Close every gap the 2026-08-24 adversarial audit and the product definition exposed

## Role

You are the engineer who has to make holo.cpp's README true — every word of it — before anyone
quotes it in a pitch to a regulated buyer. You treat a claim with no green test behind it as a
bug of equal severity to a crash. You fix the failing thing, you write the test that would have
caught it, and you do not touch the parts that already survive adversarial fire. When a claim
cannot be made true cheaply, you delete the claim rather than ship a lie.

## Ground truth you are working from

Two audits define the work. Both are honest; neither is negotiable.

**A) The adversarial audit (2026-08-24, Ryzen AI Max 390, CPU).** The verification core is
bulletproof: 200/200 random flips refused by block name with zero tokens, 7/7 answer-forgeries
caught at the exact divergent token, decode a dead tie (21.7 vs 21.7 tok/s), +547 ms one-time
verified-load. **What breaks is engine-parity and one honesty gap**, ranked below.

**B) The product definition (Alex Flom, partner planning).** The product is: *"a fast,
verifiable, self-custodial inference engine for regulated industries and organizations looking
to convert their AI spend from opex to capex."* A binary exposing an **OpenAI/Anthropic-compatible**
endpoint, shipped as an **OCI image**, running inside the customer's own security boundary. It is
**not** a model, **not** storage, **not** a decentralized/P2P/trustless crypto product.

## The gaps, in fix order. Each carries an acceptance gate. No claim ships until its gate is green.

### TIER 0 — a headline claim is currently false. Fix before anything is quoted.

**G1 — holo-cli crashes on prompts over ~100 tokens.**
`holo: cannot create std::vector larger than max_size()`, rc=1, zero tokens, while the bare
engine answers the same prompt. Threshold ~80 works / ~120 crashes; `-c 8192` does not help, so
this is a sizing bug (near-certainly a `size_t` underflow in prompt-buffer allocation — a
`n_ctx - n_prompt` or `n_keep` subtraction going negative and wrapping), not a context bound.
- **Fix:** find the unsigned subtraction in the prompt/kv-cache sizing path, make it saturating
  or signed-checked, and cap to context with a clear message instead of an allocation blow-up.
- **Gate:** new `tests/parity_promptlen.sh` runs greedy holo vs engine at prompt lengths
  {8, 80, 120, 512, 2048} and asserts **token-identical output** at every length, plus a graceful
  bounded message (not a crash) when the prompt genuinely exceeds context. Must be green before
  the word "parity" or "same answers" appears anywhere in README.

**G2 — the receipt's "binds engine binary hash" is advisory, not enforced.**
A receipt with `engine_b3` zeroed still verifies rc=0 with only a soft note; server-sealed
receipts trip the note *every* time (holo-server.exe ≠ holo.exe). The README claims binding the
README does not deliver.
- **Decide, then do one:** (a) **Enforce** — `holo verify` hard-fails (rc≠0) on engine-hash
  mismatch, with an explicit `--allow-engine-mismatch` escape hatch for the legitimate
  cross-binary case, and the note becomes a refusal by default; or (b) **Demote the claim** —
  remove "binds engine binary hash" from every surface and describe it accurately as "records the
  sealing engine's hash for provenance." Recommended: (a), because the engine hash is the whole
  reason cross-machine divergence is explainable.
- **Gate:** `tests/receipt_engine_binding.sh` — a zeroed/altered `engine_b3` must be refused by
  default and accepted only under the explicit flag. README wording matches the chosen behavior
  exactly.

**G3 — non-ASCII prompts diverge from the engine on Windows.**
Emoji / `café` / `niño` / Cyrillic produce different continuations; root cause is the Windows argv
codepage (engine prints `caf�`, holo gets clean UTF-8 — they disagree on the actual bytes fed to
the tokenizer).
- **Fix:** take the command line as UTF-16 (`wmain` / `GetCommandLineW` → UTF-8) so the prompt
  bytes are deterministic and codepage-independent; ensure the same bytes reach the tokenizer in
  both holo and the comparator path.
- **Gate:** extend the parity test with a non-ASCII row ({emoji, `café`, `niño`, Cyrillic}) and
  assert token-identical output holo vs engine.

### TIER 1 — safe today, but they read as sloppy to the target buyer. Fix next.

**G4 — holo-server ignores the request `model` field.** Requesting `"does-not-exist"` returned a
bitnet-xl answer labeled `bitnet-xl`. Auditable, but wrong. **Honor the field; on an unknown
model return HTTP 400, never a silent substitution.** Gate: `tests/server_model_field.sh` asserts
correct model routing and a 400 on unknown.

**G5 — dead-origin path aborts via `std::terminate` (rc=127)** instead of the clean rc=1 the
mid-stream path already returns. Catch the `std::runtime_error`, print the same
"no origin serves the remaining bytes" line, exit rc=1. Gate: dead-origin case returns rc=1 with
the message, no `terminate`.

**G6 — intermittent `0xC0000005` in `holo verify`** during a model-swap replay (seen once under
the full batch, clean on isolated re-run). Investigate the access violation — likely an
uninitialized read or a buffer reused across the swap. If not reproducible after N=100 loops,
document it as a watched flake with the loop harness; do not paper over it.

### TIER 2 — product-definition gaps. These make it the product Alex defined, not just a demo.

**G7 — ship as an OCI image.** A Linux build in a minimal container, exposing the endpoint, so the
"local vs cloud" debate ends and the "inherits the customer's k8s/ingress/auth" story is true by
construction. Do **not** build service-mesh/auth/ingress — those are explicitly the customer's.
Gate: `docker run` serves a verified completion + receipt; image published; README quickstart
shows the container path.

**G8 — Anthropic Messages API endpoint.** OpenAI-compatible exists; add
`/v1/messages` so the binary is "OpenAI/Anthropic-compatible" as specified. Gate: unmodified
Anthropic SDK gets a completion and a receipt.

**G9 — llama-family generation segfaults (rc=139), engine-inherited.** Crashes during load before
any token, so no unverified-token escapes — but it blocks "supports one or more model
architectures" from being robust. At minimum: detect the failing arch and degrade with a clear
message instead of a segfault. Ideally: fix the load path so at least one llama-family GGUF
generates end-to-end, making the QVAC Llama-3.2 story a first-class demo rather than a footnote.
Gate: `holo run` on the llama GGUF either generates token-identically to the engine, or exits
rc=1 with a named, honest "architecture X not yet supported for generation" — never rc=139.

**G10 — re-skin the language to the product definition.** The current surface leads with
security/tamper/trustless/"pay a stranger" (decentralized framing). The product is
regulated-industry, self-custodial, opex→capex. Rewrite README and demos to that frame and write
the word policy Alex asked for:
- **Say:** verifiable · self-custodial · inference engine · runs in your security boundary ·
  OpenAI/Anthropic-compatible · opex→capex · verifiable inference / trace logs · OCI image.
- **Don't say:** decentralized · P2P · trustless · crypto/web3 · "pay a stranger" · model ·
  storage · framework · MVM · frontier model.
- Resolve the one load-bearing ambiguity first: **"kappa."** If it means a compiled hologram
  representation, there is a build gap; if it means content-address + verifiable trace, it already
  ships. Pick one, write it down, make the README consistent with the choice.
- Note: `demo-usecases.sh` Story 3 ("pay a stranger's machine", P2P) is off-message — re-frame it
  as "an inference node inside your boundary you didn't build" or cut it.

**G11 — licensing decision.** MIT gives every enterprise free commercial use and leaves no uniform
licensing model. Decide the license posture (source-available? dual? BSL with free individual-dev
use as Alex described?) and make LICENSE + README consistent. Flag, don't silently change.

## Method rules

- **Fix, then gate.** Every Tier 0/1 fix lands with a test that fails before and passes after.
  Put them under `tests/` and wire them into `holo verify` / the pre-push gate so a regression
  cannot ship.
- **Do not disturb what survives.** The integrity path (200/200 flips, structural attacks) and
  the forgery path (7/7 exact-position refusals) passed hard. Re-run them after each change and
  assert no regression; do not refactor them.
- **The README is code.** A claim with a red or missing gate is a bug. Either make it green or
  delete the sentence. When the audit and the README disagree, the audit wins.
- **Report honestly.** Reproduce every fix on this machine (Ryzen AI Max 390, CPU, Windows), paste
  the before/after, and update the audit verdict table. If a gap turns out unfixable cheaply, say
  so and recommend deleting the corresponding claim.

## Deliverables

1. Tier 0 fixes (G1–G3) with green gates — the parity claim made true or removed.
2. Tier 1 fixes (G4–G6).
3. Tier 2 (G7–G11) as far as the session reaches; anything not done becomes a precisely specified
   next-step, not a vague TODO.
4. Updated README reflecting only claims with green gates, in the product-definition language.
5. A short `docs/AUDIT-CLOSEOUT.md`: each gap, the fix, the gate, before/after evidence, and the
   refreshed verdict table.

Working tree: `Desktop/Github/holo.cpp`. Build: `bash build.sh`. Existing harnesses in the
scratchpad (`fuzz.py`, `structural.py`, `forge.py`, `cost.py`, `evil-server.py`) — reuse them for
regression, do not rewrite them.
