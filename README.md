# holo.cpp

**You cannot tell if the AI model on your disk has been tampered with. You cannot prove an
answer came from the model you think produced it. holo.cpp fixes both, for free.**

A model file today is just bytes from the internet. Change one bit of it and every popular
inference engine will load it and answer cheerfully, telling you nothing. An answer today
is just text. Nothing ties it to the exact model, program and settings that produced it.
Anyone who cares about supply chain integrity, reproducible results, or paying untrusted
machines to run inference has this problem. An answer you cannot check is an answer you are
merely trusting.

## How it works

Three ideas, each simple on its own:

1. **A model is named by its bytes.** The name is a content address: a BLAKE3 hash derived
   from the file itself. Whatever you type (a Hugging Face id, a file path, a short alias),
   holo prints the address it resolved before the first token appears.
2. **Every block is checked before it is decoded.** Models stream in 8 MB blocks and each
   block must match its hash before the engine sees a single byte. A tampered block is
   refused by name. Checking overlaps the download, so on a network link it costs nothing.
3. **Every answer is sealed and replayable.** Each run writes a receipt: model address,
   engine binary hash, exact input and output tokens, sampler and seed. `holo verify`
   re-runs the computation and either matches token for token or names the first
   divergence. A forged receipt is refuted by compute, not paperwork.

The compute engine underneath is
[qvac-fabric-llm.cpp](https://github.com/tetherto/qvac-fabric-llm.cpp) (Tether's fork of
[llama.cpp](https://github.com/ggml-org/llama.cpp)), vendored unmodified as a pinned
submodule. holo adds the loader, the verifier and the receipts through the engine's own
public API, with zero patches. Same kernels, same speed, same answers.

## See it in 15 seconds

```bash
bash demo.sh
```

Three acts. Nothing is mocked, every act checks its own outcome, and the script exits
nonzero if reality disagrees with the story. Measured on 2026-08-24; full transcript and
methodology in [docs/DEMO.md](docs/DEMO.md).

**Act I. Flip one bit in an 835 MB model.**

| engine | result |
|---|---|
| llama.cpp family, no verification | answered normally, said nothing |
| holo | `block 47/100 FAILED verification (expected b3:9d42caec…, got b3:2da844c4…)`, zero tokens, exit 1 |

**Act II. Forge a receipt (one output token changed, correctly self addressed).**

| receipt | result |
|---|---|
| genuine | `VERIFIED: re-derived byte-identical` |
| forged | `REFUSED: re-derivation diverged at output position 2 (got 278, sealed 279)` |

**Act III. What did the protection cost?**

| metric | holo (verified) | engine (unverified) |
|---|---:|---:|
| decode speed, this demo run | 21.2 tok/s | 21.6 tok/s |
| decode speed, 8 run median | 17.6 tok/s | 16.9 tok/s |
| output | identical | identical |

Whole demo: 15 seconds on a warm store.

## The numbers

Measured 2026-08-24 on an AMD Ryzen AI Max 390 (12 cores, 32 GB, Windows 11), CPU backend,
model BitNet 1.3B ternary (834,553,152 bytes). Full method, raw logs and every tie or loss
in [bench/RESULTS.md](bench/RESULTS.md); harness in `bench/stress.sh`, rerun it yourself.

**Speed: verification is free per token.**

| | holo | engine alone |
|---|---:|---:|
| decode, median of 8 interleaved runs | **17.6 tok/s** | 16.9 tok/s |
| output parity, greedy, matched settings | identical | identical |

**Load: verification costs about half a second, once.**

| | median | spread |
|---|---:|---|
| engine mmap load, no checks | 307 ms | 295 to 334 ms |
| holo verified load, all 100 blocks checked | 865 ms | 854 to 944 ms |

**Cold start from a URL: the verified path finishes ahead of the naive one.**
Controlled 50 MB/s wire, every run validated, 4 runs each:

| | median time to done | verified? |
|---|---:|---|
| holo streaming (checks ride inside the download) | **16.4 s** | every block |
| download, then load | 17.0 s | nothing |

**Tamper detection: 5 of 5 positions refused.** First byte, header, midpoint, deep
interior, final byte. Each one bit flip was refused by block name with both hashes shown,
zero tokens emitted, exit 1. The unmodified engine loaded every one of those files without
complaint.

## Use it

```
holo pull hf:owner/repo/model.gguf     acquire, verify, store; prints holo:b3:<address>
holo run  <address|alias|hf:|path>     verified load, generate, seal a receipt
holo verify <receipt-address>          re-derive; byte identical or loud failure
holo-server <ref> --port 8000          OpenAI compatible API; x-holo-receipt on answers
holo-cli -m model.gguf -p "..." -n 32  llama.cpp compatible flags, drop in argv
```

Any GGUF runs unmodified. An unmodified OpenAI SDK works against `holo-server` with only a
base URL change.

## Build

```bash
git clone --recurse-submodules https://github.com/humuhumu33/holo.cpp
cd holo.cpp
bash build.sh
```

Windows with MinGW needs `-D_WIN32_WINNT=0x0A00`, which build.sh handles. A prebuilt
Windows bundle with runtime DLLs is attached to the GitHub release.

## Repository

```
engine/     qvac-fabric-llm.cpp, pinned submodule, unmodified compute engine
src/        holo_stream.h, the verified streaming loader (about 230 lines)
tools/      holo-cli, holo-server, holo-pack, probe-futures
bench/      harness plus published results, raw logs included
docs/       DEMO.md and PHASE-0 through PHASE-5, every claim with its evidence
demo.sh     the three act demo, self asserting
```

## Honesty rules

Every performance claim names its regime, ships its harness, and reports where the
comparator wins or ties. What is not yet demonstrated is listed, not implied: no GPU legs,
no cross machine replay yet (designed and specified, awaiting a second machine), no serving
concurrency, and receipts require the verifier to hold the model. Numbers are dated and
expire.

## License

MIT. This project would not exist without llama.cpp and qvac-fabric-llm.cpp. The engine
keeps its identity, its license, and the credit for every token per second.
