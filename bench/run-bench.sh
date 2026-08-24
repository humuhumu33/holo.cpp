#!/usr/bin/env bash
# run-bench.sh — the Phase 5 harness. Emits bench/results.raw.txt; every command disclosed.
# Suites: parity, single-stream (x3), verify (x3), cold-start @ controlled rate (x3 each side).
# Comparator: qvac-fabric-llm.cpp's own llama-completion — SAME engine underneath, so every
# delta is attributable to the holo loader/verifier, nothing else.
set -u
cd "$(dirname "$0")/.."
MINGW="/c/Users/pavel/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
export PATH="$MINGW:$PATH"
Q=/c/Users/pavel/Desktop/Github/qvac-engine/build/bin
M=models/bitnet_b1_58-xl-TQ2_0.gguf
PROMPT="The capital of Japan is"
OUT=bench/results.raw.txt
: > $OUT
log(){ echo "$@" | tee -a $OUT; }
now_ms(){ python -c "import time;print(int(time.monotonic_ns()/1e6))"; }

log "## machine: AMD Ryzen AI Max 390, 12C/24T, 32GB, Windows 11, CPU backend, -march=native"
log "## engine: qvac-fabric-llm.cpp @ 4919828 | model: bitnet_b1_58-xl-TQ2_0.gguf (834553152 B)"
log "## date: $(date -u +%Y-%m-%dT%H:%MZ) | greedy temp=0 unless stated | 3 runs where marked"
log ""

# ── PARITY: same engine, same file, greedy — outputs must be identical ─────────
log "== parity (greedy, 32 tokens)"
"$Q/llama-completion.exe" -m $M -p "$PROMPT" -n 32 --temp 0 -t 12 2>/dev/null | tr -d '\r' > /tmp/par_a.txt
./holo-cli.exe -m $M -p "$PROMPT" -n 32 --temp 0 -t 12 2>/dev/null | tr -d '\r' > /tmp/par_b.txt
# llama-completion echoes the prompt; holo prints only the continuation — compare continuations
python - <<'PY' | tee -a bench/results.raw.txt
a=open('/tmp/par_a.txt').read().strip()
b=open('/tmp/par_b.txt').read().strip()
if a.startswith("The capital of Japan is"): a=a[len("The capital of Japan is"):]
a,b=a.strip(),b.strip()
print("parity:", "IDENTICAL" if a==b else f"DIFFER\n  a={a!r}\n  b={b!r}")
PY

# ── SINGLE-STREAM (warm store, warm page cache), 3 runs each ───────────────────
log ""
log "== single-stream warm (n=64 greedy, -t 12), 3 runs each"
for i in 1 2 3; do
  "$Q/llama-completion.exe" -m $M -p "$PROMPT" -n 64 --temp 0 -t 12 2>/tmp/lc.err >/dev/null
  L=$(grep -oE "load time = *[0-9.]+" /tmp/lc.err | grep -oE "[0-9.]+")
  P=$(grep "prompt eval time" /tmp/lc.err | grep -oE "[0-9.]+ tokens per second" | grep -oE "[0-9.]+")
  E=$(grep -E "^.* eval time" /tmp/lc.err | grep -v prompt | grep -oE "[0-9.]+ tokens per second" | grep -oE "[0-9.]+")
  log "llama-completion run$i: load_ms=$L prompt_tps=$P decode_tps=$E"
done
for i in 1 2 3; do
  ./holo.exe run bitnet-xl --prompt "$PROMPT" -n 64 2>/tmp/h.err >/dev/null
  LOAD=$(grep "load OK" /tmp/h.err | grep -oE "^\[ *[0-9.]+" | grep -oE "[0-9.]+")
  PE=$(grep "prompt evaluated" /tmp/h.err | grep -oE "^\[ *[0-9.]+" | grep -oE "[0-9.]+")
  FT=$(grep "first token" /tmp/h.err | grep -oE "^\[ *[0-9.]+" | grep -oE "[0-9.]+")
  DN=$(grep "done —" /tmp/h.err | grep -oE "^\[ *[0-9.]+" | grep -oE "[0-9.]+")
  NT=$(grep "done —" /tmp/h.err | grep -oE "[0-9]+ tokens" | grep -oE "[0-9]+")
  log "holo run$i: load_verify_ms=$LOAD ttft_after_load_ms=$(python -c "print(round($FT-$LOAD,1))") decode_tps=$(python -c "print(round(($NT-1)/(($DN-$FT)/1000),2))") n=$NT"
done

# ── VERIFY replay cost, 3 runs ─────────────────────────────────────────────────
log ""
log "== verify_replay (8-token receipt), 3 runs"
RECEIPT=$(ls -t ~/.hologram/receipts/*.json | head -1 | xargs basename | sed 's/.json//')
for i in 1 2 3; do
  T0=$(now_ms); ./holo.exe verify $RECEIPT >/tmp/v.out 2>/dev/null; RC=$?; T1=$(now_ms)
  log "verify run$i: wall_ms=$((T1-T0)) rc=$RC $(head -c 40 /tmp/v.out)"
done

# ── COLD-START @ 50 MB/s controlled wire, 3 runs each side ─────────────────────
log ""
log "== cold-start, localhost origin throttled to 50 MB/s (both sides see the same wire)"
python bench/throttled-server.py $M 8950 50 & SRV=$!
sleep 2
for i in 1 2 3; do
  T0=$(now_ms)
  ./holo-run.exe models/model.manifest.json "http://127.0.0.1:8950/m" --tensors models/tensors.txt -n 8 2>/tmp/cs.err >/dev/null
  T1=$(now_ms)
  FT=$(grep "first token" /tmp/cs.err | grep -oE "[0-9.]+" | head -1)
  log "holo stream run$i: ttft_from_url_ms=$FT total_ms=$((T1-T0)) (verified in-flight)"
done
for i in 1 2 3; do
  T0=$(now_ms)
  curl -s -o /tmp/dl.gguf "http://127.0.0.1:8950/m"
  T1=$(now_ms)
  "$Q/llama-completion.exe" -m /tmp/dl.gguf -p "$PROMPT" -n 8 --temp 0 -t 12 2>/tmp/lc2.err >/dev/null
  T2=$(now_ms)
  # first token ~= download + load + prompt eval (llama-completion reports load+prompt times)
  L=$(grep -oE "load time = *[0-9.]+" /tmp/lc2.err | grep -oE "[0-9.]+")
  P=$(grep "prompt eval time" /tmp/lc2.err | grep -oE "= *[0-9.]+ ms" | grep -oE "[0-9.]+" | head -1)
  TTFT=$(python -c "print(round($((T1-T0))+$L+$P,1))")
  log "download-then-load run$i: download_ms=$((T1-T0)) load_ms=$L prompt_ms=$P ttft_from_url_ms=$TTFT total_ms=$((T2-T0)) (NO verification)"
  rm -f /tmp/dl.gguf
done
kill $SRV 2>/dev/null
log ""
log "## real-internet datapoint (2026-08-24, ~6 MB/s HF link): holo ttft_from_url = 139235 ms, verification+load ~0.1 s visible over wire time (docs/PHASE-1.md)"
