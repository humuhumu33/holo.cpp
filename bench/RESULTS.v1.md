# Benchmark results — 2026-08-24

**Machine.** AMD Ryzen AI Max 390 (Zen 5, 12C/24T), 32 GB, Windows 11. **CPU backend only**
(no Vulkan SDK installed; no discrete NVIDIA GPU — no CUDA numbers are possible on this box).
Engine: `qvac-fabric-llm.cpp` @ `4919828`, Release, `-march=native`, OpenMP.
Model: `bitnet_b1_58-xl-TQ2_0.gguf` (834,553,152 bytes, BitNet ternary).

**Comparator.** `llama-completion` from the same build — the **same engine underneath**, so
every delta is attributable to the holo loader/verifier and nothing else. vLLM is excluded
from this suite because it cannot load this model at all (no GGUF loader, no ternary path at
rev `1baf372b`); Ollama is not installed here and cannot ingest a bare GGUF without a
Modelfile import. Both exclusions stated per the contract; a mainstream-model suite is the
open item for any vLLM comparison.

**Rules followed.** Same box, same model file, same flags where marked, greedy `--temp 0`,
raw log in `results.raw.txt`, harness in `run-bench.sh` + `throttled-server.py`. Losses are
reported first.

## Parity (gate for everything below)

32-token greedy continuation, matched `-c 2048 -t 12`:
**IDENTICAL, character for character**, between `llama-completion -m file.gguf` and
`holo-cli -m holo:b3:3e82b1eb…` through the verified stream:

> "Tokyo, the largest city in the country. Tokyo is a city of 12 million people, and it is
> the center of the Japanese economy. The city"

## Warm single-stream, matched config (`-c 2048 -t 12`, n=64, 5 runs)

| engine | decode tok/s (min…max) | median | load, ms |
|---|---|---:|---:|
| llama-completion (mmap, no verification) | 13.51 … 19.07 | **16.59** | 257–707 |
| holo (full BLAKE3 re-verify of all 100 blocks, no mmap) | 13.62 … 17.79 | **16.71** | 826–992 |

**Read this honestly:** decode speed is statistically identical — as it must be, same
kernels. The run-to-run spread (±20%) is thermal/system noise on a busy desktop and dwarfs
any engine delta. **What verification actually costs is load-time only: ~600 ms extra on an
835 MB model** (block re-verify + copy versus mmap). TTFT after load: ~99–144 ms (prompt
eval + first decode).

**A loss we found and fixed while measuring:** the first matched run showed holo ~20%
slower at decode. Cause: the loader kept its 835 MB staging buffer resident through
generation, fighting the weights for cache. Freeing it after load closed the gap. Recorded
because the measurement is the reason it was found.

## Cold start, controlled 50 MB/s wire (localhost throttled origin, 3 runs each)

| path | ttft_from_url, ms (median) | total, ms | verification |
|---|---:|---:|---|
| **holo** — stream + verify in-flight + load overlapped | **16,091** | 16,453–16,500 | **every block, BLAKE3** |
| download, then load (the `-hf`-style flow) | 16,368 | 16,922–16,968 | **none** |

holo reaches the first token **~280 ms sooner** and the prompt is answered ~470 ms sooner in
total — **while also verifying every byte**, which the comparator does not do at all. The
honest framing stands as predicted in Phase 0: at this model size the overlap buys back
roughly the load time; the structural claim is *verification at zero added wall-time*, not a
dramatic speed win. The margin grows with model size and load cost, not with link speed.

**Real-internet datapoint** (not simulated): 835 MB from huggingface.co at ~6 MB/s —
ttft_from_url = **139.2 s**, with verification and model build adding ~0.1 s visible over
the wire time (`docs/PHASE-1.md`).

## Verified replay (3 runs)

`holo verify` on an 8-token receipt: **2,828 / 2,828 / 2,891 ms** wall. That price buys:
re-verification of all 100 weight blocks, model rebuild, and position-by-position greedy
replay proven byte-identical. There is no comparator row — no other engine has this
operation.

## What we do NOT claim

- No GPU numbers of any kind yet (Vulkan SDK not installed; no CUDA hardware).
- No serving/concurrency numbers — vLLM's regime, not measured, not claimed.
- No cross-machine replay demonstration yet (same-machine, fresh-process only).
- The 2B-4T reference wafer model is not yet in this table — the published `i2_s` GGUF is
  not loadable by this engine family; a local TQ2_0 conversion is the open item.
- These numbers are from one busy desktop, dated above, and expire.
