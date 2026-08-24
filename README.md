# holo.cpp

### Verified local AI inference. Same speed, same answers, now provable.

Flip one bit in a model file and every popular engine will load it and answer anyway,
telling you nothing. Ask a rented machine to run a model and nothing ties its answer to the
model you paid for. holo.cpp closes both gaps, and the benchmarks say it costs you nothing.

```bash
bash demo.sh        # the whole argument in 15 seconds, on your machine
```

Everything below was measured on 2026-08-24 (AMD Ryzen AI Max 390, CPU, one machine). Every
number ships with the script that produced it. Nothing here is a mock.

## The evidence, in three lines

| claim | holo | the engine alone |
|---|---|---|
| one bit flipped in an 835 MB model | **refused, named the block, zero tokens** | ran it, answered normally, said nothing |
| a forged answer receipt | **refused, named the exact wrong token** | no such concept |
| decode speed (8 run median, same model) | **17.6 tok/s** | 16.9 tok/s |

The protection is free because the compute engine underneath *is*
[qvac-fabric-llm.cpp](https://github.com/tetherto/qvac-fabric-llm.cpp) (Tether's fork of
[llama.cpp](https://github.com/ggml-org/llama.cpp)), vendored unmodified. holo adds a
verifying loader and re-derivable receipts through the engine's own public API, zero
patches. Same kernels, same output, plus proof.

## Why it matters, with real examples

**Supply chain. A model reaches you through mirrors nobody vouches for.**
`demo-usecases.sh` pulls the exact Llama 3.2 model [QVAC](https://github.com/tetherto/qvac)
ships in its quickstart and shows two independent naming systems agree on the bytes:

```
QVAC registry sha256 : 66bfbb2d…70c2      (trust in their catalog)
holo content address : holo:b3:5da836c9…  (trust in nothing but the file)
✔ the bytes we pulled from Hugging Face ARE the bytes Tether pinned
```

**Silent corruption and poisoned weights.** Disk rot, a bad flush, or a surgically edited
"sleeper" model all look identical to a normal load. Flip one bit and:

```
[the engine every local AI app uses today]   loaded it, answered, warned you of nothing
[holo]  block 47/93 FAILED verification (expected b3:66db5a54…, got b3:f61e8f4a…) — refused, 0 tokens
```

**Delegated inference you can audit.** When a prompt runs on a node you didn't build — a
shared cluster, a vendor endpoint, another team's box inside your boundary — the answer comes
with a receipt binding model, engine and every token. A genuine answer re-derives byte for
byte; a substituted or fabricated one is caught at the exact token it diverged:

```
✔ VERIFIED: re-derived byte-identical — this model really produced this answer to this prompt
✘ REFUSED: re-derivation diverged at output position 1 — the substitution, located exactly
```

That last line is the difference between suspicion and proof. Full walkthroughs:
[docs/USE-CASES.md](docs/USE-CASES.md).

## How it works

Three ideas, each simple:

1. **A model is named by its bytes**, a BLAKE3 content address. Type a Hugging Face id, a
   file path, or an alias; holo prints the address it resolved before the first token.
2. **Every 8 MB block is checked before it is decoded.** A tampered block is refused by name.
   Checking overlaps the download, so streaming from a URL it costs nothing.
3. **Every answer is sealed and replayable.** The receipt binds model address, engine binary
   hash, exact input and output tokens, sampler and seed. `holo verify` re-runs the compute
   and matches token for token or names the first divergence. The engine binding is enforced:
   a receipt sealed by a different binary is refused unless you pass `--allow-engine-mismatch`.
   A forgery is refuted by compute, not paperwork.

## How it compares

Five approaches call themselves verified inference; they solve different problems. Sources
and dates in [docs/COMPARISON.md](docs/COMPARISON.md).

| | catches poisoned weights before decode | proves the exact answer | overhead per token | runs on a laptop CPU | trust you must add |
|---|:---:|:---:|:---:|:---:|---|
| **holo.cpp** | **yes** | **yes, exact token** | **none (measured tie)** | **yes** | none; you hold the model |
| zkML (DeepProve) | no | yes, but ~GPT-2 scale is the frontier | ~1000×+ at LLM scale | no | none (math) |
| TEE (H100 CC) | no (attests the binary, not your file) | attests where code ran, not the answer | ~4 to 8% | no (needs H100) | the hardware vendor's root |
| TOPLOC | no (checks activations while serving) | probabilistic, not exact | small | no (GPU validator) | statistical tolerance |

Read it plainly: zkML wins the one thing holo cannot do (verify **without** the model, on
chain); TEEs win privacy and near native speed if you trust NVIDIA and own an H100; TOPLOC
screens ~100× cheaper than replay but never gives you an exact "diverged at token 1". holo
owns the corner none of them target: **exact, zero overhead, on the machine you already
have.** Its honest cost is that the verifier must hold the model, and cross machine replay
is designed but not yet demonstrated.

## The full numbers

CPU, one machine, BitNet 1.3B ternary (834,553,152 bytes). Method, raw logs and every tie
or loss in [bench/RESULTS.md](bench/RESULTS.md); rerun with `bench/stress.sh`.

| measurement | holo | engine alone |
|---|---:|---:|
| decode, median of 8 interleaved runs | **17.6 tok/s** | 16.9 tok/s |
| greedy output, matched settings | identical | identical |
| load, no verification (mmap) | | 307 ms |
| load, all 100 blocks verified | 865 ms | |
| cold start from a 50 MB/s URL, to done | **16.4 s, verified** | 17.0 s, unverified |
| tamper positions refused (first, header, mid, deep, last) | **5 of 5** | 0 of 5 |

Verification adds about half a second once at load and nothing per token. Streaming from a
URL it rides inside the download, so the verified path finishes ahead of the naive one.

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

Windows with MinGW needs `-D_WIN32_WINNT=0x0A00`, which build.sh handles. A prebuilt Windows
bundle with runtime DLLs is attached to the GitHub release.

## What is proven, and what is not

Every performance claim names its regime, ships its harness, and reports where the
comparator ties or wins. Not yet demonstrated, and not implied: GPU backends, cross machine
replay (designed and specified, awaiting a second machine), serving concurrency. Receipts
require the verifier to hold the model. One known bug: some llama family GGUFs crash in the
streaming loader after load, tracked in the issues. Numbers are dated and expire.

## License

MIT. This project would not exist without llama.cpp and qvac-fabric-llm.cpp. The engine
keeps its identity, its license, and the credit for every token per second.
