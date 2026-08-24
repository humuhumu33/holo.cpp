# Phase 5 — benchmarks and the ship point

Status: **Gate 5 closed 2026-08-24** for the CPU/local scope this box can honestly measure.
Full tables: `bench/RESULTS.md`; raw log: `bench/results.raw.txt`; harness:
`bench/run-bench.sh` + `bench/throttled-server.py`.

## The three headline facts

1. **Parity.** 32-token greedy output character-identical between `llama-completion` and
   `holo` through the verified stream. The verification layer changes nothing about the
   compute — proven, not asserted.
2. **Verification costs ~600 ms of load and 0 decode** on an 835 MB model: matched-config
   decode medians 16.59 vs 16.71 tok/s (same engine, noise-level delta); load 257–707 ms
   (mmap, unverified) vs 826–992 ms (every block BLAKE3-checked).
3. **Cold start from a URL: verified beats unverified.** At a controlled 50 MB/s wire, holo
   reached the first token ~280 ms sooner than the download-then-load flow *while verifying
   every byte in flight*; the comparator verified nothing. Real-internet leg: 139.2 s at
   ~6 MB/s with verification adding ~0.1 s visible.

## Contract compliance

Same box/model/flags disclosed · greedy for every correctness-sensitive row · 3–5 runs,
medians + spreads · losses reported first (including the staging-buffer regression we found
in our own loader and fixed mid-measurement) · exclusions stated (vLLM cannot load the
model; Ollama absent; no GPU, no CUDA on this machine) · raw log checked in.

## Open items to a public release

- Vulkan leg (install the SDK; gfx1151 is the hardware most "local machine" users have).
- The 2B-4T TQ2_0 conversion, so the wafer-reference model joins the table.
- A mainstream safetensors model suite for any vLLM comparison.
- Cross-machine replay demonstration (needs a second box).
- True incremental SSE in holo-server; `--api-key`; Linux/macOS bundles.
