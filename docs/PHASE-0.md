# Phase 0 — baseline, toolchain, and the two decisions

Status: **Gate 0 closed 2026-08-24**. Every line here is something that was run on this machine on
2026-08-24, not something read in a README. Where a claim comes from source rather than
execution, it says so.

Machine: AMD Ryzen AI Max 390 (Zen 5, 12C/24T, 32 GB), Radeon 8060S / gfx1151 integrated GPU,
Windows 11.

## Decisions

**DECISION 1 — submodule, not fork.** `qvac-fabric-llm.cpp` is consumed as a git submodule
pinned to a SHA. All Hologram code lives in this repo; theirs stays pristine so a sync is
`git checkout <newer-sha>` plus a gate re-run rather than a merge. Revisit only if a phase
proves we must patch their internals — and if so, keep the patch small enough to upstream.

Upstream pinned at **`4919828`** ("Synchronize master with temp-9840 (#196)"), fetched
2026-08-24. Their stated llama.cpp baseline is `b7349`; ggml version reported by their build
is `0.15.3`.

**DECISION 2 — the name.** `holo.cpp`. It places the project in the family a user already
understands (`llama.cpp`, `bitnet.cpp`, `whisper.cpp`) and says what it is before they read a
word.

## Toolchain on this machine

| tool | version | note |
|---|---|---|
| cmake | 4.1.2 | |
| ninja | 1.13.1 | |
| g++ | 15.2.0 MinGW-W64 UCRT | the only C++ compiler present |
| MSVC (`cl`) | absent | |
| clang | absent | |
| python | 3.11.9 | needed for `convert_hf_to_gguf.py` |
| glslc | **absent** | no Vulkan SDK → **the Vulkan backend cannot be built here** |
| vulkaninfo | present | the runtime exists; the shader compiler does not |

**Consequence for every number this project publishes from this box: CPU only.** The gfx1151
GPU is reachable in principle via Vulkan, but building that backend requires the Vulkan SDK
for `glslc`. Installing it is a Phase 0 follow-up, not an assumption.

## Build finding — it does not build out of the box on MinGW

`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` fails at ~40% with:

```
vendor/cpp-httplib/httplib.h:16: error: #error "cpp-httplib doesn't support Windows 8 or
lower. Please use Windows 10 or later."
vendor/cpp-httplib/httplib.cpp:1467: error: '::CreateFile2' has not been declared
```

Cause: MinGW defaults `_WIN32_WINNT` to a pre-Windows-10 API level, so `cpp-httplib`'s guard
trips and `CreateFile2` is not declared. Not a defect in their code — a toolchain default.

**Fix, verified:**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF -DGGML_NATIVE=ON \
  -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000" \
  -DCMAKE_CXX_FLAGS="-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000"
```

This is the first line of our install documentation, and a candidate upstream contribution:
setting the API level in their CMake for MinGW targets would fix it for everyone. Also noted:
OpenSSL was not found, so HTTPS support in the bundled httplib is disabled — irrelevant for
our loader, which does its own transport, but relevant to anyone using their downloader.

## Model finding — our reference model's published GGUF cannot be loaded

Our wafer `HOLOGRAMTECH/q-bitnet-2b` compiles from
`microsoft/bitnet-b1.58-2B-4T-gguf`, a repo containing exactly one file:
`ggml-model-i2_s.gguf`.

Their ggml declares `GGML_TYPE_TQ1_0` and `GGML_TYPE_TQ2_0`. **There is no
`GGML_TYPE_I2_S`** — that type belongs to microsoft/BitNet's own fork. So the exact file our
wafer derives from is not loadable by this engine, confirming risk 2 in the implementation
prompt.

What *is* available:

- `LLM_ARCH_BITNET` is present (`src/llama-arch.h:90`).
- `convert_hf_to_gguf.py` accepts `--outtype tq2_0` (line 60, mapping at line 210), so a TQ2_0
  build can be produced from `microsoft/bitnet-b1.58-2B-4T` (safetensors).
- A survey of Hugging Face found **no ready-made TQ2_0 of 2B-4T**. The nearest third-party
  GGUF (`tdh111/bitnet-b1.58-2B-4T-GGUF`) ships `iq2_bn` variants, which are ik_llama.cpp's
  type, not TQ2_0.

**Plan:** convert `microsoft/bitnet-b1.58-2B-4T` → TQ2_0 locally. This becomes the comparator
artifact, and its conversion is itself a documented step. Fallback named in their README if
conversion misbehaves: `gianni-cor/bitnet_b1_58-xl-TQ2_0`.

**Note for the benchmark contract:** our engine and theirs will therefore run *different
quantizations of the same weights* (`t2`/`q3f` wafer vs TQ2_0 GGUF). Token-identical output
across the two is neither expected nor meaningful. Byte-identical parity remains a gate
against our own browser engine only.

## The futures loader — read from source, pending an empirical probe

Declarations confirmed in this checkout:

```c
// include/llama.h:506
llama_model * llama_model_load_from_split_futures(
    const char ** paths, size_t n_paths, const char * context,
    const char * tensor_list_file, llama_model_params params);

// include/llama-cpp.h:35,37
llama_model * llama_model_load_from_buffer(std::vector<uint8_t> && data, llama_model_params);
bool llama_model_load_fulfill_split_future(const char * path, const char * context,
    std::unique_ptr<std::basic_streambuf<char>> && streambuf);
```

Implementation (`src/llama-mmap.h:94-124`, `src/llama.cpp:1301-1320`): `llama_future_file_buffer`
holds a `std::promise`/`std::future` keyed by `(promise_key = path, context)`. Its operations
**block until the promise is fulfilled**. `fulfill_promise` hands over a complete
`llama_file_buffer`, which wraps a caller-supplied `std::basic_streambuf<char>`.

Two readings follow, and the difference decides our headline claim:

- **Granularity is per split file, not per block.** So "stream a model in" means "declare N
  splits and fulfil them as they arrive", with `tensor_list_file` telling the loader what lives
  where before any bytes exist.
- **But the buffer we hand over is *our* `streambuf`.** A custom streambuf can block in
  `underflow()` until the bytes it needs have arrived and been verified. If the loader reads
  tensors in non-decreasing file order, that yields overlapped download and load with no
  patch to their code at all.

**Source finding (2026-08-24), stronger than the header suggested:** the consumer of the
futures path is a class literally named `IncrementalSplitsTensorLoad`
(`src/llama-model-load.cpp:110-160`). Split loading is **lazy and ordered**: splits after the
first are "delayed files", and `load_tensor_metadata()` pulls delayed splits one at a time,
in order, only when a requested tensor has not yet been seen (`:145-155`). Split index order
is enforced (`:99-101` — "invalid split file loading order"). So download and model-build
genuinely overlap by design: split N+1 can still be arriving while split N's tensors are
processed.

**The honest physics, same as we measured in the browser:** *decode* still cannot start until
every weight is resident — overlap kills the download-then-load serialization, it does not
beat the wire. The claim to carry is "load completes moments after the last byte arrives,
instead of starting then" — worth tens of seconds on a 700 MB model over a real link, and
measurable. "First token before download completes" is **not** supported for a dense model
and must not be said.

**Still to run empirically:** the slow-fulfilment probe, to measure the actual overlap and
confirm no hidden full-file seek. Also worth noting: `expected_tensors` refuses unknown
tensors (`:145`) — the `tensor_list_file` is a *contract*, which suits us; our manifest can
generate it.

## Empirical probe results (2026-08-24) — Gate 0 CLOSED

Probe: `tools/probe-futures.cpp` — an instrumented `std::streambuf` fed to
`llama_model_load_from_split_futures`, logging every seek and read, with an optional
arrival-rate throttle that charges wire delay only for bytes beyond the high-water mark
(re-reads of arrived bytes are free, as they are from a local store). Model:
`bitnet_b1_58-xl-TQ2_0.gguf` (834,553,152 bytes) — fallback per the model finding above.

**The futures path works end to end.** Model loads through our streambuf and generates
greedy output identical to the file-path baseline (" Tokyo, the largest city in the
country"). PROVEN.

**Read pattern (MEASURED):** 130,188 reads; 774 seeks, of which 532 are 266 repetitions of
a `data-start ↔ EOF` pair — per-tensor `size()` probes, answered for free by a manifest —
leaving ~121 real data seeks, all local (few-MB span), over a broadly monotonic sweep of the
tensor region. **Verdict: an arrived-prefix local store fully covers the access pattern.**
No pathological random access.

**Overlap (MEASURED, simulated 50 MB/s link, arrival-model throttle):** wire time ≈ 16.7 s;
load returned **0.66 s after the last byte**; first decode +62 ms. Unthrottled streambuf
load: 1.35 s (vs 0.47 s for the native file path — streambuf overhead exists and is
tolerable).

**The honest headline this forces:** at 700 MB–1 GB scale on a fast link, overlapped loading
saves roughly the load time (~0.5–1.5 s) versus download-then-load — real but modest. The
cold-start *number* alone will not carry the product at this size; the carried claims are:
**verification at zero added wall-time** (BLAKE3 overlapped with the wire), content-addressed
resume/mirror-racing/dedupe, and sealed KV commons for turn-1 TTFT. Larger models and slower
links widen the overlap win; measure, do not extrapolate.

**Baseline (CPU, this machine)** — `bench/baseline.json`: load 466 ms, prompt eval
57.6 tok/s, decode 10.8 tok/s on the XL TQ2_0 via `llama-completion`. Note their fork's CLI
split: `llama-cli` is an interactive UI; non-interactive generation lives in
`llama-completion` — an argv-compatibility fact for Phase 4.

Runtime note for install docs: MinGW-built binaries need the MinGW `bin` on PATH for
`libstdc++`/`libgomp` DLLs (exit 127 otherwise). The web-UI target (`llama-ui-assets`, an
npm build) is the only target that failed; everything inference-relevant (97 exes,
`libllama.a`, ggml static libs) built clean.

## Gate 0 status

- [x] Repo initialised, name chosen, upstream pinned at `4919828`
- [x] Toolchain surveyed; Vulkan gap identified
- [x] Build fix found and documented
- [x] Engine builds clean (97 binaries; only the npm web-UI asset step fails — irrelevant)
- [x] Reference-class BitNet TQ2_0 model running (fallback XL; 2B-4T conversion pending)
- [x] Baseline numbers in `bench/baseline.json`
- [x] **Futures-loader probe: YES** — incremental delivery works through a custom streambuf;
      access pattern is store-coverable; overlap measured at ε = 0.66 s after last byte

Remaining Phase 0 follow-ups (do not block Phase 1): convert `microsoft/bitnet-b1.58-2B-4T`
to TQ2_0 locally; install the Vulkan SDK for the gfx1151 GPU backend.
