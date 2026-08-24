# Benchmark results — holo.cpp vs qvac-fabric-llm.cpp

**Date 2026-08-24 · CPU only.** AMD Ryzen AI Max 390 (Zen 5, 12C/24T), 32 GB, Windows 11.
No Vulkan SDK, no NVIDIA GPU on this box — every number below is the CPU backend
(`-march=native`, OpenMP). Engine: `qvac-fabric-llm.cpp` @ `4919828`. Model:
`bitnet_b1_58-xl-TQ2_0.gguf` (834,553,152 B, sha256 `3c32e12dc1eadb8e…`, content address
`holo:b3:3e82b1eb…`). Harness: `bench/stress.sh` + `bench/run1.py` (zombie-proof tree-kill
timeouts) + `bench/throttled-server.py`; raw log `bench/stress.raw.txt`. Runs are
warm-up-discarded, decode runs **interleaved** between engines to cancel thermal drift, and
every cold-start run **self-validates** (byte count + wire time ≥ 95 % of nominal) or is
excluded and marked INVALID. The earlier, thinner run of these tables is preserved as
`RESULTS.v1.md`.

**The comparator is the engine holo itself vendors.** Same kernels underneath — so every
delta below is attributable to holo's loader and verifier, and decode ties are the expected,
honest result, not a failure. vLLM is excluded (cannot load this model: no GGUF loader, no
ternary path at rev `1baf372b`); Ollama is not installed and cannot ingest a bare GGUF
without a Modelfile import. Both exclusions per the contract.

## Top line

On the same hardware and the same model file, holo.cpp produces **character-identical
output** to `qvac-fabric-llm.cpp`, decodes at the **same speed** (median 17.6 vs 16.9 tok/s,
overlapping spreads), and adds what the engine does not have: every load is
**block-verified** — a single flipped bit anywhere in 835 MB is refused by name with zero
tokens emitted, while the engine loads the same tampered file without complaint — every
answer carries a **re-derivable receipt**, and over a 50 MB/s wire the verified streaming
path finishes **~0.6 s sooner** than the engine's own unverified download-then-load.
Verification's entire cost is ~0.6 s of load time on a warm 835 MB model — and effectively
zero when the model arrives over the network, because it rides inside the wire time.

## Ties first (the honest core)

**Output parity (gate).** Greedy 32-token continuation, matched `-c 2048 -t 12`:
**IDENTICAL, character for character.** This gate precedes and licenses every timing row.

**Decode throughput — a tie, as it must be.** n=64 greedy, matched `-c 2048 -t 12`,
8 interleaved pairs after a discarded warm-up pair:

| engine | decode tok/s median | min…max |
|---|---:|---|
| holo (block-verified load, no mmap) | **17.6** | 15.5…18.9 |
| llama-completion (mmap, no verification) | **16.9** | 16.5…19.5 |

The spreads overlap; the difference is noise. **Verification costs zero per token.**
Methodology note kept on purpose: an earlier batched (non-interleaved) run showed holo at
11.4 vs 16.9 — pure thermal/scheduler drift between the two batches; interleaving removed
it. The invalid run stays in the raw log.

## Verification (the capability the engine does not have)

**Tamper matrix.** One bit flipped at five positions in the stored model — first byte
(GGUF header), byte 100, the midpoint, byte 700,000,000, and the final byte:

```
tamper@0          rc=1  0 tokens   REFUSED-CLEAN
tamper@100        rc=1  0 tokens   REFUSED-CLEAN
tamper@417276576  rc=1  0 tokens   REFUSED-CLEAN
tamper@700000000  rc=1  0 tokens   REFUSED-CLEAN
tamper@834553151  rc=1  0 tokens   REFUSED-CLEAN
```

Each refusal names the failing block with both hashes
(`block 0/100 FAILED verification (expected b3:d0065ae3…, got b3:a0207242…)`), exits
non-zero, and the engine never sees the byte. The same tampered file handed to
`llama-completion` directly **loads and runs without complaint** — the engine has no
integrity check. (Hardening found by this harness: header-region tampering originally
exited via an engine-side abort *after* our refusal printed; fixed in `1ba3389` so all
positions exit rc=1 cleanly.)

**Receipts.** Every run seals `model address ‖ engine-binary hash ‖ input ids ‖ output ids
‖ sampler+seed`, content-addressed by its own hash. `holo verify` re-derives: a clean
receipt → `VERIFIED … byte-identical` in **1.2–1.4 s wall** for an 8-token answer
(≈ load + replay, no per-token surcharge). Prior gates (docs/PHASE-3.md): an edited receipt
is refused on address mismatch before any compute; a **forged** receipt with a correct
self-address and one altered output id is refused by re-derivation at the exact divergent
position. The engine has no equivalent concept.

**What verification costs, isolated.** Warm store, matched config, 8 runs each after
warm-up discard:

| load path | median ms | min…max |
|---|---:|---|
| engine mmap load (no verification) | **307** | 295…334 |
| holo verified load (all 100 blocks BLAKE3-checked) | **865** | 854…944 |

Overhead ≈ **560 ms on 835 MB** (≈ 1.5 GB/s effective verify+copy), paid once per process
start, never per token.

## Latency — cold start from a URL (the differentiator regime)

Controlled 50 MB/s localhost wire, **fresh server per run**, every run validated (full byte
count, wire ≥ 95 % of nominal) — 4 valid runs per side, zero invalid:

| path | ms to completion, median | min…max | verified? |
|---|---:|---|---|
| holo verified stream (load+verify overlap the wire) | **16,375** | 16,328…16,469 | **every block** |
| engine download → load (serial) | **16,984** | 16,922…17,000 | none |

holo finishes **~610 ms sooner, and verified** — load and verification are absorbed into
the wire time while the engine pays download then load serially. The margin ≈ the engine's
own load cost; it grows with model size and shrinks on faster links. Real-internet
datapoint on record (docs/PHASE-1.md): 835 MB from Hugging Face at ~6 MB/s, all blocks
verified in-flight, load complete ≈ wire time, first token +96 ms after the last byte.

## Robustness (phase gates, re-confirmed under this harness)

Truncated origin → automatic failover resuming at the verified high-water, identical
output. Missing redirect support, dead mirror, corrupt manifest → named errors, non-zero
exits, no hangs. Post-flight after the full suite: no orphaned processes; RAM and disk
within 0.5 GB of pre-flight.

## What this does NOT show

- **No GPU numbers.** Vulkan/Metal/CUDA legs need hardware/SDKs this box lacks.
- **No cross-machine replay.** Receipts re-derive in fresh processes on this machine;
  cross-machine determinism (same binary, different CPU) is designed but undemonstrated.
- **No serving concurrency.** holo-server serializes inference (single local user by
  design) and its SSE streaming is batch-flushed in v0.1. No concurrency claims are made.
- **One model family measured here** (BitNet TQ2_0; stories260K in the phase gates).
  Architecture coverage is inherited from the engine but not per-arch verified. The 2B-4T
  reference wafer model remains an open item: its published `i2_s` GGUF is not loadable by
  this engine family; a local TQ2_0 conversion is the path.
- **Numbers are dated (2026-08-24) and expire.** Re-run `bench/stress.sh` before quoting.
