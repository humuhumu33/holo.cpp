# Practical use cases, told with a real model

`bash demo-usecases.sh` runs three stories on the exact model
[QVAC](https://github.com/tetherto/qvac) installs in its own quickstart:
`unsloth/Llama-3.2-1B-Instruct-Q4_0.gguf`, which QVAC's registry pins by
sha256 `66bfbb2d…70c2`. Nothing is mocked; every story checks its own outcome. Captured
2026-08-24.

## Story 1. Does the file the world downloads match what the vendor pinned?

QVAC anchors trust in its catalog (a sha256 in a config file). holo anchors trust in nothing
but the bytes. Pull the model and the two agree, independently:

```
QVAC registry sha256 : 66bfbb2d48bdb77cd56bd03ef820deff3c4a74b1a09de3b917ae13e72c1a70c2
our pull's    sha256 : 66bfbb2d48bdb77cd56bd03ef820deff3c4a74b1a09de3b917ae13e72c1a70c2
holo content address : holo:b3:5da836c932ea56a4b224d9aba378d9b3…
✔ the bytes we pulled from Hugging Face ARE the bytes Tether pinned
```

**The use case: supply chain integrity.** A model reaches a user through mirrors, CDNs and
caches nobody vouches for. Two independent naming systems agreeing on one file is a
checkable guarantee that the mirror served the real thing. From here the content address
travels with the model, so the check repeats for free on every later load.

## Story 2. One bit flips in the app's model cache. Who notices?

A single bit is flipped in the stored model, then two engines run it:

```
[the engine every local AI app uses today]
  it loaded, ran, and produced degraded output — with no error, no warning
  ✔ the user will never know why it broke

[holo, same corrupted cache]
  block 47/93 FAILED verification (expected b3:66db5a54…, got b3:f61e8f4a…) — refusing to serve it
  ✔ refused by block name, zero tokens
```

**The use case: silent corruption and the PoisonGPT class.** Disk rot, a bad flush, or a
surgically edited "sleeper" model all look identical to a normal load. Every popular engine
runs them and says nothing. holo refuses before the first token, and names the block.

## Story 3. You pay a stranger's machine to run inference. Did it?

QVAC's headline feature is P2P delegation: your prompt runs on a peer. Today nothing binds
the peer's answer to the model you asked for. holo's receipt does:

```
[the peer runs your prompt and returns answer + receipt]
  answer : "Red, blue, and yellow…"
  receipt: holo:b3:c00b5f39…  (binds model address + engine hash + every token)

[you, auditing the peer]
  VERIFIED: re-derived byte-identical — it really ran this model on your prompt   ✔

[a cheating peer: right model, fabricated answer, perfectly formed receipt]
  REFUSED: re-derivation diverged at output position 1   ✔ caught at the exact token
```

**The use case: trustless / paid inference.** In any market where you pay an untrusted
machine to run a model (P2P swarms, decentralized inference networks), a receipt that
re-derives byte-for-byte is slashing evidence, not a suspicion. A cheat is located at the
exact token it was introduced.

## Honest notes

- Stories 1 and 2 run on QVAC's exact Llama 3.2 model. Story 3's *generation* is shown on
  the BitNet demo model: the receipt mechanism is architecture-independent, but running some
  llama-family GGUFs through holo's streaming loader currently hits a crash after load
  (tracked; the engine loads the same file fine directly, so it is a loader-integration bug,
  not a model problem). Fixing it is the next task on this branch.
- Bigger showcase on deck: **INTELLECT-1**, the first decentrally trained model, has public
  GGUF builds and is the natural model for Story 3's decentralized-inference framing once
  the loader bug is fixed. It needs ~6 GB free disk.
- Everything here is CPU, one machine, dated, and rerunnable: `bash demo-usecases.sh`.
