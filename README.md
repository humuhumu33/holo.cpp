# holo.cpp

**Fast, verifiable inference of content-addressed models — on your machine.**

The compute engine is [`qvac-fabric-llm.cpp`](https://github.com/tetherto/qvac-fabric-llm.cpp)
(Tether's fork of [`llama.cpp`](https://github.com/ggml-org/llama.cpp)), consumed as a pinned
submodule. holo.cpp contributes what no engine ships: models resolved by **content address**
(a name derived from the bytes), streamed and verified block-by-block before a byte is
decoded, and answers sealed with a receipt anyone can re-derive.

Status: **Phase 0 complete** — see [docs/PHASE-0.md](docs/PHASE-0.md) for the baseline,
build fixes, and the futures-loader probe results (the loader streams; the access pattern is
store-coverable; measured ε = 0.66 s between last byte and load-complete on a simulated
50 MB/s link).

Plan: `HOLO-ENGINE-IMPLEMENTATION-PROMPT.md` (7 phases, each with a falsifiable gate).
Research record: the engine field study, reuse audit, surface spec and benchmark contract in
`hologram-engine-study/`.

## Layout

```
docs/       phase gate records — the claims and their evidence
tools/      probe-futures.cpp (loader probe) · gen-tensor-list.py
bench/      baseline.json and, later, the published harness
models/     local artifacts (not committed)
```

## License

MIT. The engine keeps its own MIT license and its identity — this project would not exist
without llama.cpp and qvac-fabric-llm.cpp.
