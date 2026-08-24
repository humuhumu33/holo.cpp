// holo_stream.h — Phase 1 core: the verified streaming loader.
//
// Translation of the browser reader's algorithm (holo-apps/apps/q/core/holo-stream-load.mjs,
// proven live) into the std::streambuf the Phase 0 probe validated against
// llama_model_load_from_split_futures:
//
//   * ONE sequential GET per origin (curl subprocess) — bytes arrive in file order
//   * every block's BLAKE3 is checked against the manifest BEFORE the byte is served
//   * reads are served from the verified arrived-prefix; a read past it blocks until
//     the bytes it needs have arrived AND verified
//   * a hash mismatch is fatal and loud: it names the block, expected, got — and the
//     engine never sees the byte
//   * origin failover: if an origin dies mid-stream, the next origin resumes with a
//     Range request from the verified high-water mark (never re-trusting old bytes)
//
// Manifest (JSON, produced by tools/holo-pack.cpp):
//   { "file": ..., "size": N, "block_size": B, "b3": "<whole-file hash>",
//     "blocks": [ "<b3 of block 0>", "<b3 of block 1>", ... ] }
// Block i covers [i*B, min((i+1)*B, N)). The whole-file b3 is the content address.

#pragma once

#include "../vendor/blake3/blake3.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace holo {

inline std::string b3_hex(const uint8_t * data, size_t len) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    static const char * hex = "0123456789abcdef";
    std::string s(BLAKE3_OUT_LEN * 2, '0');
    for (size_t i = 0; i < BLAKE3_OUT_LEN; i++) { s[2*i] = hex[out[i] >> 4]; s[2*i+1] = hex[out[i] & 15]; }
    return s;
}

struct manifest {
    size_t                   size = 0, block_size = 0;
    std::string              b3;      // whole-file content address
    std::vector<std::string> blocks;  // per-block b3

    // Minimal JSON pull-parse for the fixed shape holo-pack emits (no dependency).
    static manifest parse(const std::string & json) {
        manifest m;
        auto num = [&](const char * key) -> size_t {
            auto p = json.find(std::string("\"") + key + "\"");
            if (p == std::string::npos) throw std::runtime_error(std::string("manifest: missing ") + key);
            p = json.find(':', p);
            return (size_t) strtoull(json.c_str() + p + 1, nullptr, 10);
        };
        m.size       = num("size");
        m.block_size = num("block_size");
        auto p = json.find("\"b3\"");
        p = json.find('"', json.find(':', p));
        m.b3 = json.substr(p + 1, json.find('"', p + 1) - p - 1);
        p = json.find("\"blocks\"");
        p = json.find('[', p);
        size_t e = json.find(']', p);
        while (true) {
            p = json.find('"', p);
            if (p == std::string::npos || p > e) break;
            size_t q = json.find('"', p + 1);
            m.blocks.push_back(json.substr(p + 1, q - p - 1));
            p = q + 1;
        }
        size_t expect = (m.size + m.block_size - 1) / m.block_size;
        if (m.blocks.size() != expect)
            throw std::runtime_error("manifest: block count mismatch");
        return m;
    }

    static manifest parse_file(const std::string & path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("manifest: cannot open " + path);
        std::stringstream ss; ss << f.rdbuf();
        return parse(ss.str());
    }
};

// The verified arrived-prefix: fetcher appends, verifier advances, readers wait on it.
struct verified_buffer {
    std::vector<uint8_t>    data;          // full-size, filled front to back
    std::atomic<size_t>     verified{0};   // bytes proven good — readers may touch [0, verified)
    size_t                  arrived = 0;   // bytes received (>= verified only inside the lock)
    std::mutex              mu;
    std::condition_variable cv;
    std::string             error;         // set once, fatal

    explicit verified_buffer(size_t n) : data(n) {}

    void fail(const std::string & why) {
        std::lock_guard<std::mutex> lk(mu);
        if (error.empty()) error = why;
        cv.notify_all();
    }
    // Block until [0, upto) is verified. Throws if the stream failed.
    void wait_for(size_t upto) {
        if (verified.load(std::memory_order_acquire) >= upto) return;
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return !error.empty() || verified.load() >= upto; });
        if (verified.load() < upto) throw std::runtime_error("holo stream failed: " + error);
    }
};

// Fetch driver: pulls bytes sequentially from origins (curl subprocess), verifies each
// completed block, publishes verified bytes. Runs on its own thread.
struct fetcher {
    const manifest &  m;
    verified_buffer & buf;
    std::vector<std::string> origins;   // URLs or local paths ("file" origin)
    long long         throttle_us_per_mb = 0;   // test hook: simulated link speed
    std::thread       th;
    std::atomic<bool> done{false};

    fetcher(const manifest & mf, verified_buffer & vb, std::vector<std::string> orgs, long long throttle = 0)
        : m(mf), buf(vb), origins(std::move(orgs)), throttle_us_per_mb(throttle) {}

    void start() { th = std::thread([this] { run(); }); }
    void join()  { if (th.joinable()) th.join(); }

    // Verify every complete block in [verified, arrived); publish on success.
    // Returns false (and fails the buffer) on a hash mismatch.
    bool verify_arrived(size_t arrived_to, bool at_eof) {
        size_t v = buf.verified.load();
        while (v < arrived_to) {
            size_t bi        = v / m.block_size;
            size_t block_end = std::min((bi + 1) * m.block_size, m.size);
            if (arrived_to < block_end && !at_eof) break;           // block incomplete — wait for more bytes
            size_t bs  = block_end - bi * m.block_size;
            std::string got = b3_hex(buf.data.data() + bi * m.block_size, bs);
            if (got != m.blocks[bi]) {
                char msg[256];
                snprintf(msg, sizeof msg,
                         "block %zu/%zu FAILED verification (expected b3:%.16s…, got b3:%.16s…) — refusing to serve it",
                         bi, m.blocks.size(), m.blocks[bi].c_str(), got.c_str());
                std::fprintf(stderr, "[holo] %s\n", msg);
                buf.fail(msg);
                return false;
            }
            {
                std::lock_guard<std::mutex> lk(buf.mu);
                buf.verified.store(block_end, std::memory_order_release);
            }
            buf.cv.notify_all();
            v = block_end;
        }
        return true;
    }

    void run() {
        size_t at = 0;                       // resume point = verified high-water only
        for (size_t oi = 0; oi < origins.size() && at < m.size; oi++) {
            const std::string & org = origins[oi];
            if (oi) std::fprintf(stderr, "[holo] origin failover -> %s (resuming at %zu)\n", org.c_str(), at);
            FILE * p = nullptr;
            bool   is_url = org.rfind("http", 0) == 0;
            std::string cmd;
            if (is_url) {
                cmd = "curl -s -f --retry 2 -r " + std::to_string(at) + "- \"" + org + "\"";
                p = _popen(cmd.c_str(), "rb");
            } else {
                p = fopen(org.c_str(), "rb");
                if (p && at) fseek(p, (long) at, SEEK_SET);
            }
            if (!p) continue;
            std::vector<uint8_t> chunk(1 << 20);
            while (at < m.size) {
                size_t want = std::min(chunk.size(), m.size - at);
                size_t got  = fread(buf.data.data() + at, 1, want, p);
                if (got == 0) break;                       // origin dropped — try next
                if (throttle_us_per_mb > 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(throttle_us_per_mb * (long long) got / (1 << 20)));
                {
                    std::lock_guard<std::mutex> lk(buf.mu);
                    buf.arrived = at + got;
                }
                at += got;
                if (!verify_arrived(at, at == m.size)) { if (is_url) _pclose(p); else fclose(p); done = true; return; }
            }
            if (is_url) _pclose(p); else fclose(p);
        }
        if (at < m.size) buf.fail("no origin serves the remaining bytes (verified " + std::to_string(buf.verified.load()) + "/" + std::to_string(m.size) + ")");
        done = true;
    }
};

// The streambuf handed to llama_model_load_fulfill_split_future. Serves only verified bytes.
struct stream_buf : std::streambuf {
    verified_buffer & buf;
    size_t            pos = 0;
    char              one;

    explicit stream_buf(verified_buffer & b) : buf(b) { setg(&one, &one, &one); }

    std::streamsize xsgetn(char * s, std::streamsize n) override {
        size_t end = std::min(pos + (size_t) n, buf.data.size());
        if (end > pos) buf.wait_for(end);
        size_t got = end - pos;
        memcpy(s, buf.data.data() + pos, got);
        pos = end;
        return (std::streamsize) got;
    }
    int underflow() override {
        if (pos >= buf.data.size()) return traits_type::eof();
        buf.wait_for(pos + 1);
        one = (char) buf.data[pos++];
        setg(&one, &one, &one + 1);
        return traits_type::to_int_type(one);
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
        size_t sz = buf.data.size();
        pos = dir == std::ios_base::beg ? (size_t) off : dir == std::ios_base::cur ? pos + (size_t) off : sz + (size_t) off;
        setg(&one, &one, &one);
        return (pos_type) pos;
    }
    pos_type seekpos(pos_type p, std::ios_base::openmode mo) override {
        return seekoff((off_type) p, std::ios_base::beg, mo);
    }
};

}  // namespace holo
