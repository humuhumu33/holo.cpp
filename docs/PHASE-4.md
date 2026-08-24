# Phase 4 — the familiar surface

Status: **Gate 4 closed 2026-08-24**, with the install leg proven as far as one machine
allows (a copied bare directory with zero environment setup runs; a true stranger's machine
is untested and said so).

## What was built

- **`holo-server`** (`tools/holo-server.cpp`, ~300 lines) — OpenAI-compatible server on the
  engine's own vendored cpp-httplib + nlohmann/json (zero new dependencies).
  `/v1/chat/completions` (stream + non-stream), `/v1/completions`, `/v1/models`,
  `/v1/receipts/<b3>`. Model loaded once through the verified stream; every answer sealed;
  non-streamed responses carry `x-holo-receipt`, streamed responses name the receipt in the
  final SSE chunk (headers are gone by then). Chat prompts use the model's own template via
  `llama_chat_apply_template`, plain-transcript fallback for base models.
- **llama.cpp argv compatibility** in `holo`/`holo-cli`: `-m -p -f -n -c -t -s --temp
  --top-k --top-p --repeat-penalty -e -ngl` (ignored with a note on this CPU build),
  unknown flags warned-and-ignored. Sampling uses the engine's own sampler chain; sampled
  runs seal `temp/top_k/top_p/rep/seed`, so replay stays exact (receipt v2).
- **`dist/holo-win64.zip`** (25 MB) — holo, holo-cli, holo-server plus the four MinGW
  runtime DLLs, so nothing depends on PATH. This kills the exit-127 DLL trap found in
  Phase 0.

## Gate 4 results (PROVEN 2026-08-24)

**Unmodified OpenAI SDK** (`openai` 2.24.0, base-URL change only): chat non-stream with
`usage` and `finish_reason` ✓ · chat streaming with the receipt address recovered from the
final chunk ✓ · legacy `completions` ✓.

**Ten llama.cpp command lines, only the binary name changed — 10/10 pass**, including the
qvac-fabric-llm.cpp README quickstart line verbatim, temp+seed sampling, top-k/top-p,
repeat penalty, `--escape`, prompt-from-file, and long-form flags.

**Bare-directory run**: the zip contents copied to an empty directory run with no PATH, no
MinGW, no setup — `holo run bitnet-xl` generated and sealed normally.

## An incident worth its lesson

The first argv suite run "failed 10/10" — every process died with `std::bad_alloc`. Cause: a
**zombie `llama-cli.exe` from the morning's baseline attempt holding 8.08 GB of commit**
(Git Bash `timeout` kills the bash wrapper, not the Windows child; the interactive UI kept
waiting for input forever). System commit was exhausted; killing PID 39940 freed it and the
suite passed 10/10 unchanged. Recorded because it is exactly the class of failure the error
taxonomy exists for: the engine refused honestly (`bad_alloc`, loud), and the operational
lesson — never orphan interactive children on Windows — goes in the harness rules for
Phase 5.

## Debts

- Server is mutex-serialized, one resident model, `n_ctx` fixed at start — correct for the
  single-user regime, stated openly.
- `stream:true` currently buffers the whole generation, then emits all SSE chunks at once —
  correct wire format, not yet incremental delivery. True streaming needs cpp-httplib's
  chunked content provider; small change, listed for Phase 5 polish.
- No `--api-key` gate on the server yet; bind is 127.0.0.1 by default.
- Windows-only bundle; Linux/macOS builds are cmake-trivial but unproven here.
