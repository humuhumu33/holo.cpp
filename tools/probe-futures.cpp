// probe-futures.cpp — Phase 0 empirical probe of qvac-fabric-llm.cpp's futures loader.
//
// Question this answers (docs/PHASE-0.md, Gate 0):
//   When a model is fed through llama_model_load_from_split_futures with a streambuf we
//   control, in what order does the loader read bytes (sequential vs seeking), and how does
//   load progress overlap byte availability?
//
// Method: wrap the on-disk GGUF in an instrumented streambuf that (a) logs every seek and
// every read span, (b) optionally throttles delivery to simulate a network arrival rate.
// The log IS the answer: a monotonically increasing read cursor with no backward seeks means
// a network-backed streambuf can deliver the model as bytes arrive; backward seeks map out
// exactly what must be buffered.
//
// Build (from holo.cpp root; QVAC = path to the qvac-engine checkout with a completed build):
//   g++ -std=c++17 -O2 tools/probe-futures.cpp \
//       -I "$QVAC/include" -I "$QVAC/ggml/include" \
//       -L "$QVAC/build/src" -L "$QVAC/build/ggml/src" \
//       -lllama -lggml -lggml-base -lggml-cpu -o probe-futures.exe
//
// Run:
//   probe-futures.exe <model.gguf> <tensors.txt> [delay_us_per_mb]
//   (tensors.txt = one tensor name per line; generate with tools/gen-tensor-list.py)

#include "llama.h"
#include "llama-cpp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static clk::time_point T0;
static double ms() {
    return std::chrono::duration<double, std::milli>(clk::now() - T0).count();
}

// A streambuf over a file that logs seeks/reads and can throttle, emulating network arrival.
struct probe_stats {
    std::atomic<size_t> nreads{0}, nseeks_fwd{0}, nseeks_back{0}, hiwater{0};
    size_t fsize = 0;
};

struct probe_buf : std::streambuf {
    std::shared_ptr<probe_stats> stats = std::make_shared<probe_stats>();
    std::ifstream f;
    size_t        fsize   = 0;
    size_t        pos     = 0;      // our logical cursor
    long long     delay_us_per_mb = 0;   // simulated arrival rate
    char          chunk[1 << 16];
    std::string   tag;

    probe_buf(const char * path, long long delay, std::string t) : delay_us_per_mb(delay), tag(std::move(t)) {
        f.open(path, std::ios::binary);
        f.seekg(0, std::ios::end);
        fsize = (size_t) f.tellg();
        f.seekg(0);
        stats->fsize = fsize;
        setg(chunk, chunk, chunk);  // empty get area → every read hits underflow/xsgetn
    }

    // --- logging helpers -------------------------------------------------
    void log_seek(size_t from, size_t to) {
        if (to > from) stats->nseeks_fwd++; else if (to < from) stats->nseeks_back++;
        std::fprintf(stderr, "[%9.1fms] %s seek  %zu -> %zu (%s)\n", ms(), tag.c_str(), from, to,
                     to >= from ? "fwd" : "BACK");
    }
    void log_read(size_t at, size_t len) {
        size_t nr = ++stats->nreads;
        size_t hw = stats->hiwater.load();
        while (at + len > hw && !stats->hiwater.compare_exchange_weak(hw, at + len)) {}
        if (nr <= 40 || nr % 500 == 0)
            std::fprintf(stderr, "[%9.1fms] %s read  %zu +%zu  (hiwater %.1f%%)\n", ms(), tag.c_str(), at, len,
                         100.0 * stats->hiwater.load() / fsize);
    }
    void throttle(size_t len) {
        if (delay_us_per_mb > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us_per_mb * (long long) len / (1 << 20)));
    }

    // --- streambuf protocol ---------------------------------------------
    std::streamsize xsgetn(char * s, std::streamsize n) override {
        // Charge wire delay only for bytes not yet "arrived" (beyond the high-water mark);
        // re-reads of arrived bytes are served by the local store for free.
        size_t hw = stats->hiwater.load();
        size_t fresh = (pos + (size_t) n > hw) ? pos + (size_t) n - hw : 0;
        log_read(pos, (size_t) n);
        throttle(fresh);
        f.seekg((std::streamoff) pos);
        f.read(s, n);
        std::streamsize got = f.gcount();
        pos += (size_t) got;
        return got;
    }
    int underflow() override {
        log_read(pos, 1);
        f.seekg((std::streamoff) pos);
        f.read(chunk, 1);
        if (f.gcount() < 1) return traits_type::eof();
        pos += 1;
        setg(chunk, chunk, chunk + 1);
        return traits_type::to_int_type(chunk[0]);
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
        size_t target = dir == std::ios_base::beg ? (size_t) off
                      : dir == std::ios_base::cur ? pos + (size_t) off
                                                  : fsize + (size_t) off;
        if (target != pos) log_seek(pos, target);
        pos = target;
        setg(chunk, chunk, chunk);
        return (pos_type) pos;
    }
    pos_type seekpos(pos_type p, std::ios_base::openmode m) override {
        return seekoff((off_type) p, std::ios_base::beg, m);
    }

};

int main(int argc, char ** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <model.gguf> <tensors.txt> [delay_us_per_mb]\n", argv[0]);
        return 2;
    }
    const char *    model_path  = argv[1];
    const char *    tensor_list = argv[2];
    const long long delay       = argc > 3 ? atoll(argv[3]) : 0;

    T0 = clk::now();
    llama_backend_init();

    const char * ctx_tag  = "probe";
    const char * paths[1] = { model_path };

    // Fulfiller thread: hand over the tensor list first, then the model bytes.
    // (Loader blocks on each future until fulfilled — this simulates arrival.)
    std::shared_ptr<probe_stats> mdl_stats;
    std::thread fulfiller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto tl = std::make_unique<probe_buf>(tensor_list, 0, "tlist");
        std::fprintf(stderr, "[%9.1fms] fulfilling tensor list\n", ms());
        llama_model_load_fulfill_split_future(tensor_list, ctx_tag,
                                              std::unique_ptr<std::streambuf>(std::move(tl)));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto mb   = std::make_unique<probe_buf>(model_path, delay, "model");
        mdl_stats = mb->stats;
        std::fprintf(stderr, "[%9.1fms] fulfilling model future (delay=%lldus/MB)\n", ms(), delay);
        llama_model_load_fulfill_split_future(model_path, ctx_tag,
                                              std::unique_ptr<std::streambuf>(std::move(mb)));
    });

    llama_model_params mp = llama_model_default_params();
    mp.use_mmap           = false;

    std::fprintf(stderr, "[%9.1fms] calling llama_model_load_from_split_futures\n", ms());
    llama_model * model = llama_model_load_from_split_futures(paths, 1, ctx_tag, tensor_list, mp);
    std::fprintf(stderr, "[%9.1fms] load returned: %s\n", ms(), model ? "OK" : "NULL");
    fulfiller.join();
    if (mdl_stats)
        std::fprintf(stderr, "[%9.1fms] model SUMMARY reads=%zu fwd_seeks=%zu BACK_seeks=%zu hiwater=%zu/%zu (%.1f%%)\n",
                     ms(), mdl_stats->nreads.load(), mdl_stats->nseeks_fwd.load(), mdl_stats->nseeks_back.load(),
                     mdl_stats->hiwater.load(), mdl_stats->fsize, 100.0 * mdl_stats->hiwater.load() / mdl_stats->fsize);
    if (!model) return 1;

    // Prove the model actually decodes: one short greedy generation.
    const llama_vocab * vocab = llama_model_get_vocab(model);
    llama_context_params cp   = llama_context_default_params();
    cp.n_ctx                  = 512;
    llama_context * lctx      = llama_init_from_model(model, cp);
    const char *    prompt    = "The capital of Japan is";
    llama_token     toks[64];
    int n = llama_tokenize(vocab, prompt, (int) strlen(prompt), toks, 64, true, false);
    llama_batch batch = llama_batch_get_one(toks, n);
    llama_decode(lctx, batch);
    std::fprintf(stderr, "[%9.1fms] first decode done (TTFT from process start)\n", ms());
    for (int i = 0; i < 8; i++) {
        const float * logits  = llama_get_logits_ith(lctx, -1);
        int           n_vocab = llama_vocab_n_tokens(vocab);
        int           best    = 0;
        for (int t = 1; t < n_vocab; t++)
            if (logits[t] > logits[best]) best = t;
        char piece[64];
        int  pn = llama_token_to_piece(vocab, best, piece, sizeof piece, 0, false);
        std::fwrite(piece, 1, pn > 0 ? pn : 0, stdout);
        llama_batch nb = llama_batch_get_one(&best, 1);
        llama_decode(lctx, nb);
    }
    std::printf("\n[%9.1fms] done\n", ms());
    llama_free(lctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
