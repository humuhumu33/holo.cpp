# run1.py — zombie-proof single-run executor for the stress harness.
# Git Bash `timeout` does not kill Windows children (measured: an orphaned llama-cli held
# 8 GB commit and poisoned every later run with bad_alloc). This runner enforces a hard
# timeout with taskkill /T (kills the tree), captures stderr marks, and prints one
# machine-readable result line:
#   RESULT rc=<n> wall_ms=<n> [mark_<name>=<ms> ...] out=<first 60 bytes of stdout>
#
#   python bench/run1.py <timeout_s> <logfile|-> -- <cmd> [args...]
import subprocess
import sys
import time
import re
import os

timeout_s = float(sys.argv[1])
logfile = sys.argv[2]
assert sys.argv[3] == "--"
cmd = sys.argv[4:]

env = dict(os.environ)
mingw = r"C:\Users\pavel\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
env["PATH"] = mingw + os.pathsep + env.get("PATH", "")

t0 = time.monotonic()
p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
try:
    out, err = p.communicate(timeout=timeout_s)
    rc = p.returncode
except subprocess.TimeoutExpired:
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
    out, err = p.communicate()
    rc = -9
wall = (time.monotonic() - t0) * 1000

marks = {}
for name, pat in [("load", rb"\[ *([0-9.]+)ms\] load OK"),
                  ("prompt", rb"\[ *([0-9.]+)ms\] prompt evaluated"),
                  ("first", rb"\[ *([0-9.]+)ms\] first token"),
                  ("done", rb"\[ *([0-9.]+)ms\] done"),
                  ("seal", rb"seal cost ([0-9.]+) ms"),
                  ("ntok", rb"done \xe2\x80\x94 ([0-9]+) tokens")]:
    m = re.search(pat, err)
    if m:
        marks[name] = float(m.group(1))
# llama-completion timing lines
for name, pat in [("lc_load", rb"load time = *([0-9.]+) ms"),
                  ("lc_prompt_ms", rb"prompt eval time = *([0-9.]+) ms"),
                  ("lc_tps", rb"eval time.*?\(.*?([0-9.]+) tokens per second\)")]:
    ms = re.findall(pat, err)
    if ms:
        marks[name] = float(ms[-1])

parts = [f"RESULT rc={rc}", f"wall_ms={wall:.1f}"]
for k, v in marks.items():
    parts.append(f"{k}={v}")
snippet = out.decode("utf8", "replace").strip().replace("\n", " ")[:60]
parts.append(f"out={snippet!r}")
line = " ".join(parts)
print(line)
if logfile != "-":
    with open(logfile, "a", encoding="utf8") as f:
        f.write("CMD " + " ".join(cmd) + "\n" + line + "\n")
        if rc not in (0,) and err:
            f.write("ERR " + err.decode("utf8", "replace")[-400:].replace("\n", " | ") + "\n")
