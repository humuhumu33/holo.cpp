# Phase 3 — receipts and verification

Status: **Gate 3 closed 2026-08-24** (with one honestly-stated limitation: "machine B" was a
fresh process on the same machine — cross-machine replay needs a second box and is listed as
an open item, not claimed).

## The receipt

Sealed automatically on every `holo run` — no flag, no opt-in:

```json
{ "v": 1,
  "model_b3":  "<address of the verified weights>",
  "engine_b3": "<BLAKE3 of the holo binary itself>",
  "decode": "greedy", "n_ctx": 512,
  "prompt": "...", "input_ids": [...], "output_ids": [...] }
```

Every field is **real bytes**: the model address is what the block-verified loader actually
served; the engine identity is the hash of the executing binary, not a version string; the
ids are what went in and came out. The receipt is itself content-addressed — its filename is
the BLAKE3 of its bytes — so a receipt cannot be edited without changing its name. This is
the design the prior study demanded after finding `witness.verify()`-style trace replay
proves nothing about bytes.

`holo verify <address>` re-loads the model through the verified stream, replays the sealed
input ids, greedy-decodes, and compares position by position.

## Gate 3 results (all PROVEN, 2026-08-24)

**Replay.** `holo run bitnet-xl --prompt "The capital of France is"` → " Paris, the largest
city in the country", sealed as `holo:b3:bfe16846…` at a **seal cost of 25 ms** (dominated
by hashing the 7 MB engine binary; cacheable). A fresh process re-derived all 8 output
tokens byte-identically: `VERIFIED`.

**Determinism, again.** Re-running the same prompt re-minted a receipt with the **identical
address** — same bytes in, same bytes out, same name.

**Three refusals, each loud and specific:**

| attack | result |
|---|---|
| receipt bytes edited | `receipt bytes do not match their address (tampered receipt)` — caught before any compute |
| stored weights tampered (1 bit at byte 200,000,000) | `block 23/100 FAILED verification (expected b3:f0402c2f…, got b3:803f7009…) — refusing to serve it`, rc=1; replay never ran |
| forged receipt sealing a wrong answer (correct address, output id #2 bumped) | `REFUSED: re-derivation diverged at output position 2 (got 278, sealed 279)`, rc=1 |

That third row is the important one: a receipt that *looks* perfect is refuted by
re-derivation. The proof is the compute, not the paperwork.

## Limits, stated plainly

- **Same-machine replay only is proven.** Cross-machine determinism of ggml CPU kernels
  (thread count, SIMD path) is expected for identical binaries but NOT yet demonstrated —
  the receipt carries `engine_b3` precisely so a divergent replay can say *why*. Open item.
- Greedy only. Sampled decode needs the seed + sampler chain sealed (designed, not built).
- The prompt is stored unescaped; JSON-injection via a crafted prompt could malform a
  receipt. Escape at seal time — small fix, noted.
- Engine hash is recomputed per run (25 ms); cache by mtime.
