# The demo — one bit of poison, one forged receipt, one stopwatch

`bash demo.sh` — three acts, ~90 seconds warm, every act **asserts its own outcome** and the
script exits non-zero if reality disagrees with the story. Nothing is mocked: Act I really
flips a bit in a real 835 MB model and runs two real engines on it; Act II really forges a
receipt; Act III really measures. The poisoned byte is restored even on Ctrl-C.

## What you will see (captured 2026-08-24, this machine)

**Act I — the poison.** One bit flipped at byte 394,276,921 of 834,553,152.

```
[engine alone — qvac-fabric-llm.cpp/llama.cpp, no verification]
  it answered: " The capital of Japan is Tokyo, the largest city in the country"
  ✔ the engine ran the poisoned model WITHOUT COMPLAINT — you would never know

[holo — same poisoned bytes]
  [holo] block 47/100 FAILED verification (expected b3:9d42caec…, got b3:2da844c4…) — refusing to serve it
  ✔ holo REFUSED: named the block, showed both hashes, emitted ZERO tokens (rc=1)
```

That contrast **is** the product. With one flipped bit the output happened to stay benign;
a targeted patch (a PoisonGPT-style weight edit) would change answers, and the engine would
be exactly this silent about it.

**Act II — the forged receipt.** A clean answer seals and re-derives (`VERIFIED:
re-derived byte-identical`). Then the script forges a receipt — one output token changed,
*correctly self-addressed*, a perfect-looking lie:

```
REFUSED: re-derivation diverged at output position 2 (got 278, sealed 279)
```

The forgery is refuted by compute, not paperwork.

**Act III — the price.** Live matched run plus the 8-run medians on record
(`bench/RESULTS.md`): holo 17.6 vs engine 16.9 tok/s — a tie, same kernels. Verification
costs ~0.6 s once at load, zero per token; streamed from a URL it rides inside the
download, and the verified path finishes ahead of unverified download-then-load.

## Could the alternatives run this same demo?

The honest comparison is per-act, because "verification" means different things per camp
(full survey with sources: `HOLOGRAM/verified-inference-landscape/docs/`).

| | Act I (poisoned weights caught pre-decode) | Act II (exact forged-answer refutation) | Act III (zero overhead) | runs on this laptop |
|---|---|---|---|---|
| **zkML** ([DeepProve](https://lagrange.dev/blog/deepprove-1), the live SOTA) | no — proofs attest the computation, not the bytes on your disk | yes in principle, but the proving frontier is ~GPT-2 scale at <1 tok/s; this 1.3B model at 17 tok/s is orders of magnitude outside it | no (~1000×+ prover cost at LLM scale) | no |
| **TEE** ([H100 CC](https://arxiv.org/abs/2509.18886), Phala on OpenRouter) | partially — attests the binary and platform, not a per-file integrity check you control | attests *where/what code* ran; the answer itself is not independently re-derivable | close (4–8 % measured) | no — needs an H100 and NVIDIA's root of trust |
| **TOPLOC** ([arXiv:2501.16007](https://arxiv.org/abs/2501.16007)) | no — verifies activations during serving, not artifact bytes at rest | probabilistic (LSH tolerance bands) — the exact "diverged at position 2" is precisely what it trades away for its ~100× cheaper checks | yes (cheap hashing) | no (GPU validator) |
| **holo.cpp** | **yes — demonstrated** | **yes — demonstrated, exact position named** | **yes — measured tie** | **yes — that's the point** |

And the reverse column, stated with equal prominence: what the alternatives do that holo
cannot — zkML verifies **without the model** (succinct, on-chain-checkable); TEEs protect
**input privacy**; TOPLOC screens **~100× cheaper than replay**. holo's receipts require
the verifier to hold the model and re-run the compute, and cross-*machine* replay is
designed but not yet demonstrated (`verified-inference-landscape/docs/CROSS-MACHINE-DETERMINISM.md`).

## Running it yourself

```bash
git clone --recurse-submodules https://github.com/humuhumu33/holo.cpp && cd holo.cpp
bash build.sh
holo pull models/bitnet_b1_58-xl-TQ2_0.gguf     # or any GGUF you have
bash demo.sh
```

If any act's ✔ turns to ✘, the demo failed honestly — file the output as a bug.
