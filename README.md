# holo.cpp

**Fast, verifiable inference of content-addressed models — on your machine.**

The compute engine is [`qvac-fabric-llm.cpp`](https://github.com/tetherto/qvac-fabric-llm.cpp)
(Tether's fork of [`llama.cpp`](https://github.com/ggml-org/llama.cpp)), consumed unmodified
as a pinned submodule. holo.cpp contributes what no inference engine ships:

- **Content-addressed models.** A model is named by its bytes (`holo:b3:<hash>` — a BLAKE3
  content address). Whatever you type — a Hugging Face id, a file path, an alias — the tool
  prints the address it resolved to before the first token.
- **Verified streaming.** Models stream over one HTTP GET; every 8 MB block is
  BLAKE3-verified **before** the engine sees a byte, load overlaps the wire, and a tampered
  block is refused loudly, by name. Origin failover resumes from the verified high-water.
- **Receipts on every answer.** Each run seals `model address ‖ engine hash ‖ input ids ‖
  output ids ‖ sampler+seed` — real bytes, content-addressed by its own hash.
  `holo verify <address>` re-derives the answer token-for-token or names the first
  divergence. A forged receipt is refuted by the compute, not the paperwork.

**Measured** (see [bench/RESULTS.md](bench/RESULTS.md); 835 MB BitNet TQ2_0, CPU, matched
configs, interleaved runs, harness in-repo): greedy output **character-identical** to
`llama-completion`; decode speed a statistical tie (17.6 vs 16.9 tok/s median, overlapping
spreads — same kernels, so verification costs **zero per token**); verified load costs
~560 ms once on 835 MB; a single flipped bit anywhere in the model is refused by name with
zero tokens emitted (the engine loads the same tampered file without complaint); and on a
validated 50 MB/s wire the verified streaming path finishes **~0.6 s sooner** than the
engine's own unverified download-then-load. Ties, losses and exclusions are reported in the
same tables.

## Use

```
holo pull hf:owner/repo/model.gguf     # acquire + verify + store; prints holo:b3:<address>
holo run  <address|alias|hf:|path>     # verified load, generate, seal a receipt
holo verify <receipt-address>          # re-derive; byte-identical or loud failure
holo-server <ref> --port 8000          # OpenAI-compatible API; x-holo-receipt on answers
holo-cli -m model.gguf -p "..." -n 32  # llama.cpp-compatible flags, drop-in argv
```

Any GGUF runs unmodified — your existing files work as-is. An unmodified OpenAI SDK works
against `holo-server` with only a base-URL change.

## Build

```bash
git clone --recurse-submodules https://github.com/humuhumu33/holo.cpp
cd holo.cpp
bash build.sh          # builds the engine (CMake) then the holo tools (g++)
```

Windows/MinGW note: the engine needs `-D_WIN32_WINNT=0x0A00` (handled by build.sh). A
prebuilt Windows bundle with runtime DLLs included is attached to the GitHub release.

## Repository

```
engine/     qvac-fabric-llm.cpp, pinned submodule — the compute engine, unmodified
src/        holo_stream.h — the verified streaming loader (~230 lines)
tools/      holo-cli, holo-server, holo-pack, probe-futures
bench/      harness + published results, raw logs included
docs/       PHASE-0..5 — every phase gate with its evidence
```

The integration point is the engine's own public API —
`llama_model_load_from_split_futures` fed by a custom `std::streambuf` over the verified
arrived-prefix. **Zero patches to the engine.**

## Honesty rules

Every performance claim in this repo names its regime, ships its harness, and reports the
configurations where the comparator wins. Numbers are dated and expire. What is not yet
demonstrated (GPU legs, cross-machine replay, serving concurrency) is listed in
[docs/PHASE-5.md](docs/PHASE-5.md), not implied.

## License

MIT. This project would not exist without `llama.cpp` and `qvac-fabric-llm.cpp` — the
engine keeps its identity, its license, and the credit for every token per second.
