// holo-run.cpp — Phase 1 / Gate 1 binary: verified streaming load + generation.
//
//   holo-run <manifest.json> <origin> [origin2 ...] --tensors <tensors.txt>
//            [--prompt TEXT] [-n N] [--throttle-us-per-mb X]
//
// Origins are tried in order: a URL streams via curl (one sequential GET, Range resume on
// failover), a local path reads from disk. Every block's BLAKE3 is verified against the
// manifest BEFORE its bytes are served to the engine. Output must be byte-identical to a
// plain file-path load of the same model (Gate 1.1); a tampered block must be refused
// loudly (Gate 1.2); load-behind-arrival overlap is reported (Gate 1.3).

#include "../src/holo_stream.h"

#include "llama.h"
#include "llama-cpp.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static clk::time_point T0;
static double ms() { return std::chrono::duration<double, std::milli>(clk::now() - T0).count(); }

// A dead/exhausted origin makes the engine's loader fail mid-stream and tear down its worker
// threads, which surfaces as "terminate called without an active exception" from inside C frames
// we do not patch. During the load window, treat that as the clean refusal it is: flush and exit
// rc=1 (not the default abort's rc=127), matching holo-cli's behavior.
static volatile bool g_loading = false;
static void holo_terminate() {
    if (g_loading) { std::fprintf(stderr, "[holo] REFUSED: origin failed during verified load\n"); std::fflush(nullptr); _Exit(1); }
    std::abort();
}

int main(int argc, char ** argv) {
    std::string manifest_path, tensors_path, prompt = "The capital of Japan is";
    std::vector<std::string> origins;
    int       n_gen    = 8;
    long long throttle = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--tensors")            tensors_path = argv[++i];
        else if (a == "--prompt")             prompt = argv[++i];
        else if (a == "-n")                   n_gen = atoi(argv[++i]);
        else if (a == "--throttle-us-per-mb") throttle = atoll(argv[++i]);
        else if (manifest_path.empty())       manifest_path = a;
        else                                  origins.push_back(a);
    }
    if (manifest_path.empty() || origins.empty() || tensors_path.empty()) {
        fprintf(stderr, "usage: %s <manifest.json> <origin>... --tensors <tensors.txt> [--prompt T] [-n N] [--throttle-us-per-mb X]\n", argv[0]);
        return 2;
    }

    T0 = clk::now();
    std::set_terminate(holo_terminate);
    try {
    holo::manifest m = holo::manifest::parse_file(manifest_path);
    fprintf(stderr, "[%8.1fms] resolved  holo:b3:%.16s…  (%zu bytes, %zu blocks)\n", ms(), m.b3.c_str(), m.size, m.blocks.size());

    holo::verified_buffer vb(m.size);
    holo::fetcher         fx(m, vb, origins, throttle);
    g_loading = true;
    fx.start();

    llama_backend_init();
    const char * ctx_tag = "holo";
    const char * paths[1] = { "holo://model" };

    std::thread fulfiller([&] { try {
        // tensor list: tiny, read it whole from disk
        {
            auto tb = std::make_unique<std::filebuf>();
            tb->open(tensors_path, std::ios::in | std::ios::binary);
            llama_model_load_fulfill_split_future(tensors_path.c_str(), ctx_tag,
                                                  std::unique_ptr<std::streambuf>(std::move(tb)));
        }
        // model: the verified stream
        llama_model_load_fulfill_split_future("holo://model", ctx_tag,
                                              std::unique_ptr<std::streambuf>(std::make_unique<holo::stream_buf>(vb)));
    } catch (const std::exception & e) { fprintf(stderr, "[holo] fulfil aborted: %s\n", e.what()); } catch (...) { fprintf(stderr, "[holo] fulfil aborted\n"); } });

    llama_model_params mp = llama_model_default_params();
    mp.use_mmap = false;
    llama_model * model = llama_model_load_from_split_futures(paths, 1, ctx_tag, tensors_path.c_str(), mp);
    double t_load = ms();
    fulfiller.join();
    fx.join();
    if (!model) {
        fprintf(stderr, "[%8.1fms] REFUSED: %s\n", ms(), "model not loaded (see error above)");
        return 1;
    }
    g_loading = false;
    fprintf(stderr, "[%8.1fms] load OK — verified %zu/%zu bytes before serving\n", t_load, vb.verified.load(), m.size);

    const llama_vocab * vocab = llama_model_get_vocab(model);
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 512;
    llama_context * lctx = llama_init_from_model(model, cp);

    llama_token toks[128];
    int n = llama_tokenize(vocab, prompt.c_str(), (int) prompt.size(), toks, 128, true, false);
    llama_batch batch = llama_batch_get_one(toks, n);
    llama_decode(lctx, batch);
    fprintf(stderr, "[%8.1fms] first token\n", ms());
    for (int i = 0; i < n_gen; i++) {
        const float * logits = llama_get_logits_ith(lctx, -1);
        int nv = llama_vocab_n_tokens(vocab), best = 0;
        for (int t = 1; t < nv; t++) if (logits[t] > logits[best]) best = t;
        char piece[64];
        int pn = llama_token_to_piece(vocab, best, piece, sizeof piece, 0, false);
        fwrite(piece, 1, pn > 0 ? pn : 0, stdout);
        fflush(stdout);
        llama_batch nb = llama_batch_get_one(&best, 1);
        llama_decode(lctx, nb);
    }
    printf("\n");
    fprintf(stderr, "[%8.1fms] done — %d tokens\n", ms(), n_gen);
    llama_free(lctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
    // a dead/exhausted origin surfaces as a thrown runtime_error from the stream; catch it here so
    // holo-run exits cleanly rc=1 with the message, matching the mid-stream refusal path, instead
    // of unwinding to std::terminate (rc=127).
    } catch (const std::exception & e) {
        fprintf(stderr, "[%8.1fms] REFUSED: %s\n", ms(), e.what());
        return 1;
    }
}
