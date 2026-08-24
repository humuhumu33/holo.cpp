# throttled-server.py — a localhost origin that serves one file at a fixed rate, so cold-start
# comparisons see the same wire. Supports Range (holo's failover resume asks for it).
#
#   python bench/throttled-server.py <file> <port> <MB_per_s>
import http.server
import os
import sys
import time

FILE, PORT, RATE = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
SIZE = os.path.getsize(FILE)
CHUNK = 1 << 20


class H(http.server.BaseHTTPRequestHandler):
    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", str(SIZE))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        start, end = 0, SIZE - 1
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            start = int(a or 0)
            end = int(b) if b else SIZE - 1
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{SIZE}")
        else:
            self.send_response(200)
        length = end - start + 1
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        t0 = time.monotonic()
        sent = 0
        with open(FILE, "rb") as f:
            f.seek(start)
            while sent < length:
                data = f.read(min(CHUNK, length - sent))
                if not data:
                    break
                try:
                    self.wfile.write(data)
                except (ConnectionError, BrokenPipeError):
                    return
                sent += len(data)
                # pace to RATE MB/s from the start of this response
                target = sent / (RATE * 1024 * 1024)
                lag = target - (time.monotonic() - t0)
                if lag > 0:
                    time.sleep(lag)

    def log_message(self, *a):
        pass


print(f"serving {FILE} ({SIZE} bytes) on :{PORT} at {RATE} MB/s", flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
