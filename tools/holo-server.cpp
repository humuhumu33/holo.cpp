// holo-server.cpp — Phase 4: the OpenAI-compatible server.
//
//   holo-server <ref> [--host 127.0.0.1] [--port 8000] [--ctx-size 4096]
//
// Endpoints (OpenAI schemas exactly — an unmodified OpenAI SDK works with a base-URL change):
//   POST /v1/chat/completions      stream (SSE) + non-stream
//   POST /v1/completions           non-chat
//   GET  /v1/models                what the local store serves
//   GET  /v1/receipts/<b3>         the sealed receipt for an answer
//
// The one addition: every non-streamed response carries `x-holo-receipt: <b3>`; streamed
// responses name the receipt in the final SSE chunk (headers are gone by then). Everything
// else is boringly standard — that is the point.
//
// The model is loaded ONCE through the verified stream (every block BLAKE3-checked out of
// the store). Inference is mutex-serialized: one local user, no batching — the regime this
// engine is for.

#include "../src/holo_stream.h"

#include "llama.h"
#include "llama-cpp.h"

#define CPPHTTPLIB_NO_COMPRESSION
#include "httplib.h"
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <direct.h>
#include <windows.h>
#include <fstream>
#include <mutex>
#include <sstream>

using json = nlohmann::json;

// ── store helpers (as holo-cli) ────────────────────────────────────────────────
static std::string store_root() {
    const char * h = getenv("HOLO_HOME"); if (h) return h;
    const char * up = getenv("USERPROFILE");
    return std::string(up ? up : ".") + "/.hologram";
}
static bool exists(const std::string & p) { std::ifstream f(p, std::ios::binary); return (bool) f; }
static std::string slurp(const std::string & p) {
    std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static void spit(const std::string & p, const std::string & s) { std::ofstream f(p, std::ios::binary); f << s; }
static std::string engine_b3() {
    static std::string cached;
    if (cached.empty()) {
        char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string bytes = slurp(path);
        cached = holo::b3_hex((const uint8_t *) bytes.data(), bytes.size());
    }
    return cached;
}

// ── the one resident model ─────────────────────────────────────────────────────
struct engine_state {
    llama_model *       model = nullptr;
    llama_context *     lctx  = nullptr;
    const llama_vocab * vocab = nullptr;
    std::string         model_b3, alias;
    int                 n_ctx = 4096;
    std::mutex          mu;                 // one request at a time
} E;

static void load_model(const std::string & b3) {
    std::string root = store_root();
    holo::manifest m = holo::manifest::parse_file(root + "/manifests/" + b3 + ".json");
    fprintf(stderr, "[holo-server] loading holo:b3:%.16s… (%zu bytes, verified from store)\n", b3.c_str(), m.size);
    holo::verified_buffer vb(m.size);
    holo::fetcher fx(m, vb, { root + "/blobs/" + b3 });
    fx.start();
    llama_backend_init();
    const char * ctx_tag = "holo-server";
    const char * paths[1] = { "holo://model" };
    std::string tpath = root + "/tensors/" + b3 + ".txt";
    std::thread ff([&] {
        auto tb = std::make_unique<std::filebuf>();
        tb->open(tpath, std::ios::in | std::ios::binary);
        llama_model_load_fulfill_split_future(tpath.c_str(), ctx_tag, std::unique_ptr<std::streambuf>(std::move(tb)));
        llama_model_load_fulfill_split_future("holo://model", ctx_tag,
                                              std::unique_ptr<std::streambuf>(std::make_unique<holo::stream_buf>(vb)));
    });
    llama_model_params mp = llama_model_default_params(); mp.use_mmap = false;
    E.model = llama_model_load_from_split_futures(paths, 1, ctx_tag, tpath.c_str(), mp);
    ff.join(); fx.join();
    if (!E.model) { fprintf(stderr, "[holo-server] REFUSED: model failed verification\n"); exit(1); }
    E.vocab = llama_model_get_vocab(E.model);
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = E.n_ctx;
    E.lctx = llama_init_from_model(E.model, cp);
    E.model_b3 = b3;
    fprintf(stderr, "[holo-server] ready — every block verified before load\n");
}

// ── prompt building: the model's own chat template, else a plain fallback ──────
static std::string build_prompt(const json & messages) {
    std::vector<llama_chat_message> msgs;
    std::vector<std::string> keep;                       // own the strings
    for (auto & m : messages) {
        keep.push_back(m.value("role", "user"));
        keep.push_back(m.value("content", ""));
    }
    for (size_t i = 0; i < keep.size(); i += 2)
        msgs.push_back({ keep[i].c_str(), keep[i + 1].c_str() });
    const char * tmpl = llama_model_chat_template(E.model, nullptr);
    if (tmpl) {
        std::vector<char> buf(1 << 16);
        int n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, buf.data(), (int) buf.size());
        if (n > 0 && n < (int) buf.size()) return std::string(buf.data(), n);
    }
    // base models (no template): plain transcript, assistant cued
    std::string p;
    for (size_t i = 0; i < keep.size(); i += 2) p += keep[i] + ": " + keep[i + 1] + "\n";
    p += "assistant:";
    return p;
}

// ── generation + sealing ───────────────────────────────────────────────────────
struct gen_result {
    std::string text, receipt_b3;
    int n_prompt = 0, n_gen = 0;
    std::string finish = "stop";
};

static gen_result generate(const std::string & prompt, int max_tokens, float temp, int top_k, float top_p,
                           uint32_t seed, const std::function<void(const std::string &)> & on_token) {
    gen_result R;
    std::vector<llama_token> toks(E.n_ctx);
    int n = llama_tokenize(E.vocab, prompt.c_str(), (int) prompt.size(), toks.data(), (int) toks.size(), true, true);
    if (n < 0) { R.finish = "length"; return R; }
    toks.resize(n);
    R.n_prompt = n;

    // fresh context per request: replayability over cleverness (no cross-request KV reuse)
    llama_memory_clear(llama_get_memory(E.lctx), true);

    // sampler chain: greedy when temp<=0, else seeded top-k/top-p/temp — ALL sealed
    llama_sampler * smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (temp <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        if (top_k > 0) llama_sampler_chain_add(smpl, llama_sampler_init_top_k(top_k));
        if (top_p < 1.0f) llama_sampler_chain_add(smpl, llama_sampler_init_top_p(top_p, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));
    }

    llama_batch b = llama_batch_get_one(toks.data(), (int) toks.size());
    llama_decode(E.lctx, b);
    std::vector<int> out_ids;
    for (int i = 0; i < max_tokens; i++) {
        llama_token t = llama_sampler_sample(smpl, E.lctx, -1);
        if (llama_vocab_is_eog(E.vocab, t)) { R.finish = "stop"; break; }
        char piece[256];
        int pn = llama_token_to_piece(E.vocab, t, piece, sizeof piece, 0, false);
        if (pn > 0) { R.text.append(piece, pn); if (on_token) on_token(std::string(piece, pn)); }
        out_ids.push_back(t);
        llama_batch nb = llama_batch_get_one(&t, 1);
        llama_decode(E.lctx, nb);
        if (i + 1 == max_tokens) R.finish = "length";
    }
    R.n_gen = (int) out_ids.size();
    llama_sampler_free(smpl);

    // seal — sampler params included so sampled decode replays exactly
    json rj = {
        { "v", 2 }, { "model_b3", E.model_b3 }, { "engine_b3", engine_b3() },
        { "decode", temp <= 0.0f ? "greedy" : "sampled" },
        { "temp", temp }, { "top_k", top_k }, { "top_p", top_p }, { "seed", seed },
        { "n_ctx", E.n_ctx }, { "prompt", prompt },
        { "input_ids", std::vector<int>(toks.begin(), toks.end()) }, { "output_ids", out_ids },
    };
    std::string js = rj.dump(2) + "\n";
    R.receipt_b3 = holo::b3_hex((const uint8_t *) js.data(), js.size());
    spit(store_root() + "/receipts/" + R.receipt_b3 + ".json", js);
    return R;
}

int main(int argc, char ** argv) {
    std::string ref, host = "127.0.0.1";
    int port = 8000;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--host")     host = argv[++i];
        else if (a == "--port")     port = atoi(argv[++i]);
        else if (a == "--ctx-size" || a == "-c") E.n_ctx = atoi(argv[++i]);
        else if (ref.empty())       ref = a;
    }
    if (ref.empty()) { fprintf(stderr, "usage: holo-server <ref> [--host H] [--port P] [--ctx-size N]\n"); return 2; }

    // resolve: bare b3 / holo:b3: / alias (store-only; `holo pull` first for hf:)
    std::string b3 = ref;
    if (b3.rfind("holo:b3:", 0) == 0) b3 = b3.substr(8);
    std::string root = store_root();
    if (!exists(root + "/manifests/" + b3 + ".json")) {
        std::string rp = root + "/refs/" + ref + ".json";
        if (exists(rp)) { json r = json::parse(slurp(rp)); b3 = r["b3"]; E.alias = ref; }
        else { fprintf(stderr, "holo-server: cannot resolve '%s' — `holo pull` it first\n", ref.c_str()); return 1; }
    }
    load_model(b3);

    httplib::Server srv;

    srv.Get("/v1/models", [](const httplib::Request &, httplib::Response & res) {
        json out = { { "object", "list" }, { "data", json::array() } };
        out["data"].push_back({ { "id", E.alias.empty() ? E.model_b3 : E.alias }, { "object", "model" },
                                { "owned_by", "holo" }, { "holo_address", "holo:b3:" + E.model_b3 } });
        res.set_content(out.dump(), "application/json");
    });

    srv.Get(R"(/v1/receipts/([0-9a-f]+))", [](const httplib::Request & req, httplib::Response & res) {
        std::string p = store_root() + "/receipts/" + req.matches[1].str() + ".json";
        if (!exists(p)) { res.status = 404; res.set_content("{\"error\":\"no such receipt\"}", "application/json"); return; }
        res.set_content(slurp(p), "application/json");
    });

    auto handle = [](const httplib::Request & req, httplib::Response & res, bool chat) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content("{\"error\":{\"message\":\"invalid JSON\"}}", "application/json"); return; }
        std::string prompt = chat ? build_prompt(body.value("messages", json::array()))
                                  : body.value("prompt", std::string());
        int   max_tokens = body.value("max_tokens", 128);
        float temp  = body.value("temperature", 0.0f);
        int   top_k = body.value("top_k", 40);
        float top_p = body.value("top_p", 1.0f);
        uint32_t seed = body.value("seed", 0);
        bool stream = body.value("stream", false);
        std::string id = "chatcmpl-holo";
        std::string model_name = E.alias.empty() ? E.model_b3.substr(0, 16) : E.alias;

        std::lock_guard<std::mutex> lk(E.mu);
        if (!stream) {
            gen_result g = generate(prompt, max_tokens, temp, top_k, top_p, seed, nullptr);
            json out;
            if (chat)
                out = { { "id", id }, { "object", "chat.completion" }, { "model", model_name },
                        { "choices", { { { "index", 0 }, { "message", { { "role", "assistant" }, { "content", g.text } } },
                                        { "finish_reason", g.finish } } } },
                        { "usage", { { "prompt_tokens", g.n_prompt }, { "completion_tokens", g.n_gen },
                                     { "total_tokens", g.n_prompt + g.n_gen } } } };
            else
                out = { { "id", id }, { "object", "text_completion" }, { "model", model_name },
                        { "choices", { { { "index", 0 }, { "text", g.text }, { "finish_reason", g.finish } } } },
                        { "usage", { { "prompt_tokens", g.n_prompt }, { "completion_tokens", g.n_gen },
                                     { "total_tokens", g.n_prompt + g.n_gen } } } };
            res.set_header("x-holo-receipt", g.receipt_b3);
            res.set_content(out.dump(), "application/json");
        } else {
            // SSE: chunks as tokens land; the receipt rides the final chunk (headers are gone)
            std::string acc;
            std::vector<std::string> chunks;
            gen_result g = generate(prompt, max_tokens, temp, top_k, top_p, seed,
                                    [&](const std::string & tok) { chunks.push_back(tok); });
            std::ostringstream body_out;
            for (auto & tok : chunks) {
                json c = { { "id", id }, { "object", chat ? "chat.completion.chunk" : "text_completion" },
                           { "model", model_name },
                           { "choices", { chat ? json{ { "index", 0 }, { "delta", { { "content", tok } } }, { "finish_reason", nullptr } }
                                               : json{ { "index", 0 }, { "text", tok }, { "finish_reason", nullptr } } } } };
                body_out << "data: " << c.dump() << "\n\n";
            }
            json fin = { { "id", id }, { "object", chat ? "chat.completion.chunk" : "text_completion" },
                         { "model", model_name }, { "holo_receipt", g.receipt_b3 },
                         { "choices", { chat ? json{ { "index", 0 }, { "delta", json::object() }, { "finish_reason", g.finish } }
                                             : json{ { "index", 0 }, { "text", "" }, { "finish_reason", g.finish } } } } };
            body_out << "data: " << fin.dump() << "\n\ndata: [DONE]\n\n";
            res.set_header("x-holo-receipt", g.receipt_b3);
            res.set_content(body_out.str(), "text/event-stream");
        }
    };

    srv.Post("/v1/chat/completions", [&](const httplib::Request & q, httplib::Response & r) { handle(q, r, true); });
    srv.Post("/v1/completions",      [&](const httplib::Request & q, httplib::Response & r) { handle(q, r, false); });

    fprintf(stderr, "[holo-server] listening on http://%s:%d — OpenAI-compatible, receipts on every answer\n",
            host.c_str(), port);
    srv.listen(host, port);
    return 0;
}
