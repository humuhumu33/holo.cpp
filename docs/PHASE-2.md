# Phase 2 — the store and the resolver

Status: **Gate 2 closed 2026-08-24.** The `holo` front door exists: `tools/holo-cli.cpp`
(~280 lines) builds to `holo.exe`, on top of the Phase 1 stream (`src/holo_stream.h`).

## Store

`~/.hologram/` (override: `HOLO_HOME`): `blobs/<b3>` verified bytes · `manifests/<b3>.json`
block manifests · `tensors/<b3>.txt` the loader's tensor-list contract (LF, minted from the
GGUF header at pull time) · `refs/<alias>.json` name → address + origin URLs.

**Verify on write and on read.** A pull hashes every block as it arrives; a run re-verifies
every block out of the local store before the engine sees it (the warm 835 MB load below
spends its 1.16 s doing exactly that). A corrupted blob is caught on the next run, not
trusted because it is local.

**Trust model, stated plainly.** First acquisition of an `hf:` or path ref is
trust-on-first-use: the address is *derived* from the bytes and printed. Every subsequent
read — and any `holo:b3:` ref — is checked against the pin. This is the same trust shape as
every package manager's first fetch, made explicit and portable: share the printed address
and the other machine verifies what you actually ran.

## Gate 2 results (all PROVEN on this machine, 2026-08-24)

**Four reference forms, interchangeable:**

| form | command | result |
|---|---|---|
| path | `holo pull models/bitnet_b1_58-xl-TQ2_0.gguf` | stored; address `holo:b3:3e82b1eb…` — **byte-identical to the Phase 1 holo-pack mint**: addressing is deterministic across implementations |
| address | `holo run holo:b3:3e82b1eb…` | warm run, 834,553,152/834,553,152 bytes re-verified in 1,157 ms, " Tokyo, the largest city in the country" |
| alias | `holo run bitnet-xl` | resolves via `refs/`, identical address line, correct output |
| hf: | `holo run hf:ggml-org/models/tinyllamas/stories260K.gguf` | pulled over HTTPS, stored as `holo:b3:1b2a49c5…`, generated "…there was a little girl named Lily" |

**Warm runs fetch zero bytes** — the origin for a stored model is the blob path; no network
process is spawned.

**Third-party GGUF unmodified** — stories260K (f32, llama arch) went pull→store→run with no
conversion, no flags. Table stake 5 holds.

**Every command prints the content address before the first token** — the surface-spec rule
that makes verification arrive without a tutorial.

## Debts carried forward

- **Pull buffers the whole file in RAM** before landing it in the store (fine to ~2 GB
  models on this machine; stream-to-disk is straightforward and needed for 7B+).
- **Block-granular mirror retry on hash mismatch** still pending (tamper currently fails the
  stream; the store + manifest make per-block re-fetch natural).
- `hf:` repo-level resolution picks the first `.gguf` sibling — fine for single-model repos,
  needs quant-preference logic for multi-quant repos.
- `holo ls` lists aliases only; should list blobs with sizes and addresses.
- Receipts (`⟡ ran holo:b3:…` is printed, but nothing is *sealed* yet) — that is Phase 3's
  whole job.
