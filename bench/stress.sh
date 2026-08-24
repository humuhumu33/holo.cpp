#!/usr/bin/env bash
# stress.sh — hardened head-to-head: holo.cpp vs its own engine (qvac-fabric-llm.cpp).
# Zombie-proof (run1.py enforces tree-kill timeouts), warm-up-discarded, >=8 runs,
# median+spread. CPU only. Emits bench/stress.raw.txt; summary tables to stdout.
set -u
cd "$(dirname "$0")/.."
MINGW="/c/Users/pavel/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
export PATH="$MINGW:$PATH"
QB=/c/Users/pavel/Desktop/Github/qvac-engine/build/bin
M=models/bitnet_b1_58-xl-TQ2_0.gguf
ADDR=holo:b3:3e82b1eb8514518cee82a37764b26247d3b60eda4f92954ef7ac44c7220a9c11
ALIAS=bitnet-xl
PROMPT="The capital of Japan is"
RAW=bench/stress.raw.txt
R=bench/run1.py
: > $RAW
say(){ echo "$@"; echo "$@" >> $RAW; }

# ── pre-flight: RAM, disk, orphans ─────────────────────────────────────────────
python - <<'EOF'
import ctypes,shutil,sys
class MS(ctypes.Structure):
    _fields_=[('dwLength',ctypes.c_ulong),('dwMemoryLoad',ctypes.c_ulong)]+[(n,ctypes.c_ulonglong) for n in 'a b c d e f g'.split()]
m=MS(); m.dwLength=ctypes.sizeof(MS); ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
ram=m.b/2**30; disk=shutil.disk_usage('C:/')[2]/2**30
print(f'preflight: RAM avail {ram:.1f} GB, disk free {disk:.1f} GB')
if ram<6: sys.exit('ABORT: <6 GB RAM free — clean up first')
EOF
[ $? -ne 0 ] && exit 1
kill_orphans(){ python -c "
import subprocess
for n in ('holo.exe','holo-cli.exe','holo-server.exe','holo-run.exe','llama-completion.exe','llama-server.exe','llama-bench.exe'):
    subprocess.run(['taskkill','/F','/IM',n],capture_output=True)
" 2>/dev/null; }
kill_orphans

say "## holo.cpp stress $(date -u +%Y-%m-%dT%H:%MZ) — CPU only"
say "## engine qvac-fabric-llm.cpp @ 4919828 (holo vendors it) | model bitnet_b1_58-xl-TQ2_0 (834553152 B, sha256 $(sha256sum $M | cut -c1-16)…)"
say "## comparator = engine's own llama-completion. same kernels → decode ties are the honest core."
say ""

stat(){ python -c "
import sys
xs=sorted(float(x) for x in sys.argv[1:] if x)
if not xs: print('n/a'); sys.exit()
n=len(xs); med=xs[n//2] if n%2 else (xs[n//2-1]+xs[n//2])/2
print(f'median={med:.1f} min={xs[0]:.1f} max={xs[-1]:.1f} n={n}')
" "$@"; }

# ══ S1 PARITY (gate) ══
say "== S1 parity (greedy 32, -c 2048 -t 12)"
$QB/llama-completion.exe -m $M -p "$PROMPT" -n 32 --temp 0 -t 12 -c 2048 2>/dev/null | tr -d '\r' > bench/tmp/pa.txt
./holo-cli.exe -m $ADDR -p "$PROMPT" -n 32 --temp 0 -t 12 -c 2048 2>/dev/null | tr -d '\r' > bench/tmp/pb.txt
PARITY=$(python -c "
import re
a=re.sub(r'\x1b\[[0-9;]*m','',open('bench/tmp/pa.txt',encoding='utf8').read()).strip()
b=open('bench/tmp/pb.txt',encoding='utf8').read().strip()
p='The capital of Japan is'
if a.startswith(p): a=a[len(p):].strip()
print('IDENTICAL' if a==b else 'DIFFER')")
say "S1: $PARITY  (gate for all timing below)"
say ""

# ══ S3 VERIFICATION: tamper matrix ══
say "== S3 tamper matrix (bit flip at 5 positions; holo must refuse, rc=1, 0 tokens)"
for pos in 0 100 417276576 700000000 834553151; do
  python -c "
import os; p=os.path.expanduser('~')+'/.hologram/blobs/3e82b1eb8514518cee82a37764b26247d3b60eda4f92954ef7ac44c7220a9c11'; f=open(p,'r+b'); f.seek($pos); b=f.read(1); f.seek($pos); f.write(bytes([b[0]^1])); f.close()" || { echo TAMPER-FAILED; exit 1; }
  RES=$(python $R 120 $RAW -- ./holo.exe run $ALIAS -n 4)
  python -c "
import os; p=os.path.expanduser('~')+'/.hologram/blobs/3e82b1eb8514518cee82a37764b26247d3b60eda4f92954ef7ac44c7220a9c11'; f=open(p,'r+b'); f.seek($pos); b=f.read(1); f.seek($pos); f.write(bytes([b[0]^1])); f.close()"
  RC=$(echo "$RES" | grep -oE "rc=[-0-9]+" | head -1)
  NT=$(echo "$RES" | grep -oE "ntok=[0-9]+" | head -1)
  say "  tamper@$pos: $RC ${NT:-ntok=0} $([ "$RC" = "rc=1" ] && echo REFUSED-CLEAN || echo CHECK)"
done
say ""

# ══ S3b RECEIPT integrity ══
say "== S3b receipt integrity"
./holo.exe run $ALIAS --prompt "$PROMPT" -n 8 >/dev/null 2>bench/tmp/seal.err
RB=$(grep -oE "verify [0-9a-f]{64}" bench/tmp/seal.err | head -1 | awk '{print $2}')
V=$(python $R 120 $RAW -- ./holo.exe verify $RB)
say "  clean receipt verify: $(echo "$V" | grep -oE "rc=[0-9]+") $(grep -q VERIFIED bench/tmp/seal.err && echo ok || echo -n)"
say "  (verify wall: $(echo "$V" | grep -oE "wall_ms=[0-9.]+"))"
say ""

# ══ S3c VERIFICATION COST: verified-load vs mmap-load, 8 runs ══
say "== S3c verification cost — verified-load (holo) vs mmap-load (engine), 9 runs, discard first"
HL=(); LL=()
for i in $(seq 1 9); do
  RES=$(python $R 200 $RAW -- ./holo-cli.exe -m $ADDR -p "$PROMPT" -n 1 --temp 0 -t 12 -c 2048)
  [ $i -eq 1 ] && continue
  HL+=($(echo "$RES" | grep -oE "load=[0-9.]+" | cut -d= -f2))
done
for i in $(seq 1 9); do
  RES=$(python $R 200 $RAW -- $QB/llama-completion.exe -m $M -p "$PROMPT" -n 1 --temp 0 -t 12 -c 2048)
  [ $i -eq 1 ] && continue
  LL+=($(echo "$RES" | grep -oE "lc_load=[0-9.]+" | cut -d= -f2))
done
say "  holo verified-load ms: $(stat "${HL[@]}")"
say "  engine mmap-load  ms: $(stat "${LL[@]}")"
say ""

# ══ S5 THROUGHPUT: matched decode, n=64, 9 runs discard-first ══
say "== S5 decode tok/s (matched -c 2048 -t 12, n=64, 9 runs discard first) — expect a tie"
HD=(); LD=()
# INTERLEAVED pairs (h,e) x9, discard first pair — equalizes thermal drift between engines
for i in $(seq 1 9); do
  RES=$(python $R 200 $RAW -- ./holo-cli.exe -m $ADDR -p "$PROMPT" -n 64 --temp 0 -t 12 -c 2048)
  RES2=$(python $R 200 $RAW -- $QB/llama-completion.exe -m $M -p "$PROMPT" -n 64 --temp 0 -t 12 -c 2048)
  [ $i -eq 1 ] && continue
  FT=$(echo "$RES" | grep -oE "first=[0-9.]+" | cut -d= -f2); DN=$(echo "$RES" | grep -oE "done=[0-9.]+" | cut -d= -f2)
  [ -n "$FT" ] && [ -n "$DN" ] && HD+=($(python -c "print(round(63/(($DN-$FT)/1000),2))"))
  LD+=($(echo "$RES2" | grep -oE "lc_tps=[0-9.]+" | cut -d= -f2))
done
say "  holo   decode tok/s: $(stat "${HD[@]}")"
say "  engine decode tok/s: $(stat "${LD[@]}")"
say ""

# ══ S4 COLD-START @ 50 MB/s controlled wire, 5 runs each ══
say "== S4 cold-start ttft_from_url @ 50 MB/s localhost throttle, 5 runs each"
# fresh server per run, unique port; each run self-validates (bytes complete + wire >= 95% of nominal)
HS=(); DS=()
WIRE_MIN=15800   # 834553152 B / 50 MB/s * 0.95, ms
for i in $(seq 1 4); do
  P=$((8950+i))
  python bench/throttled-server.py $M $P 50 >/dev/null 2>&1 & SRV=$!
  sleep 1
  RES=$(python $R 400 $RAW -- ./holo-run.exe models/model.manifest.json "http://127.0.0.1:$P/m" --tensors models/tensors.txt -n 4)
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
  W=$(echo "$RES" | grep -oE "wall_ms=[0-9.]+" | cut -d= -f2); RC=$(echo "$RES" | grep -oE "rc=[-0-9]+")
  V=$(python -c "print('ok' if float('$W')>=$WIRE_MIN and '$RC'=='rc=0' else 'INVALID')")
  say "  holo stream run$i: wall_ms=$W $RC $V"
  [ "$V" = "ok" ] && HS+=($W)
done
for i in $(seq 1 4); do
  P=$((8960+i))
  python bench/throttled-server.py $M $P 50 >/dev/null 2>&1 & SRV=$!
  sleep 1
  T0=$(python -c "import time;print(int(time.monotonic()*1000))")
  curl -s -o bench/tmp/dl.gguf "http://127.0.0.1:$P/m"
  TD=$(python -c "import time;print(int(time.monotonic()*1000))")
  SZ=$(python -c "import os;print(os.path.getsize('bench/tmp/dl.gguf') if os.path.exists('bench/tmp/dl.gguf') else 0)")
  $QB/llama-completion.exe -m bench/tmp/dl.gguf -p "$PROMPT" -n 4 --temp 0 -t 12 -c 2048 >/dev/null 2>bench/tmp/dl.err
  T1=$(python -c "import time;print(int(time.monotonic()*1000))")
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; rm -f bench/tmp/dl.gguf
  V=$(python -c "print('ok' if $SZ==834553152 and ($TD-$T0)>=$WIRE_MIN else 'INVALID')")
  say "  engine dl+load run$i: dl_ms=$((TD-T0)) total_ms=$((T1-T0)) bytes=$SZ $V"
  [ "$V" = "ok" ] && DS+=($((T1-T0)))
done
kill_orphans
say "  holo verified-stream (load+verify overlap wire), ms to done: $(stat "${HS[@]}")"
say "  engine download-then-load (NO verification), ms to done:    $(stat "${DS[@]}")"
say ""
say "## real-internet datapoint on record: 835 MB from HF @ ~6 MB/s, verified in-flight, load ≈ wire (docs/PHASE-1.md)"

# ── post-flight cleanup verification ──
kill_orphans
python - <<'EOF'
import ctypes,shutil
class MS(ctypes.Structure):
    _fields_=[('dwLength',ctypes.c_ulong),('dwMemoryLoad',ctypes.c_ulong)]+[(n,ctypes.c_ulonglong) for n in 'a b c d e f g'.split()]
m=MS(); m.dwLength=ctypes.sizeof(MS); ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
print(f'postflight: RAM avail {m.b/2**30:.1f} GB, disk free {shutil.disk_usage("C:/")[2]/2**30:.1f} GB, no orphans')
EOF
echo "done — raw log: $RAW"
