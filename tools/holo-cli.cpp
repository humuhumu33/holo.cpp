// holo-cli.cpp — Phase 2: the `holo` front door. Store + resolver + verified run.
//
//   holo pull <ref>                     acquire, verify, store; print the content address
//   holo run  <ref> [--prompt T] [-n N] ensure local, verified-load, generate, SEAL a receipt
//   holo verify <receipt>               re-derive the sealed answer; byte-identical or fail loud
//   holo ls                             what the store holds
//
// <ref> forms (interchangeable):
//   hf:owner/repo[/file.gguf]   Hugging Face; file defaults to the repo's first .gguf
//   ./path/to/model.gguf        local file (imported into the store by address)
//   holo:b3:<hex>               content address (must already be in store or mirrored)
//   <alias>                     short name from ~/.hologram/refs/
//
// Store layout (~/.hologram):
//   blobs/<b3>            verified bytes                 manifests/<b3>.json   block manifest
//   tensors/<b3>.txt      tensor-name contract (LF)      refs/<alias>.json     {"b3":..,"urls":[..]}
//
// Trust model, stated plainly: first acquisition of an hf: or path ref is trust-on-first-use —
// the address is DERIVED from the bytes and printed. From then on (and for any holo:b3: ref)
// every read is block-verified against the pinned manifest. Warm runs fetch zero bytes.

#include "../src/holo_stream.h"

#include "llama.h"
#include "llama-cpp.h"

#include <chrono>
#include <cstdlib>
#include <direct.h>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <functional>

using clk = std::chrono::steady_clock;
static clk::time_point T0;
static double ms() { return std::chrono::duration<double, std::milli>(clk::now() - T0).count(); }

static std::string store_root() {
    const char * h = getenv("HOLO_HOME");
    if (h) return h;
    const char * up = getenv("USERPROFILE");
    return std::string(up ? up : ".") + "/.hologram";
}
static void ensure_dirs() {
    std::string r = store_root();
    _mkdir(r.c_str());
    for (const char * d : { "/blobs", "/manifests", "/tensors", "/refs", "/receipts" }) _mkdir((r + d).c_str());
}
static bool exists(const std::string & p) { std::ifstream f(p, std::ios::binary); return (bool) f; }
static std::string slurp(const std::string & p) {
    std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static void spit(const std::string & p, const std::string & s) {
    std::ofstream f(p, std::ios::binary); f << s;
}
static std::string run_capture(const std::string & cmd) {
    FILE * p = _popen(cmd.c_str(), "rb");
    if (!p) return "";
    std::string out; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    _pclose(p);
    return out;
}

// ── manifest minting while the bytes are already in the verified buffer ─────────
static holo::manifest mint_manifest(const std::vector<uint8_t> & data, size_t block_size, std::string * json_out) {
    holo::manifest m;
    m.size = data.size(); m.block_size = block_size;
    blake3_hasher whole; blake3_hasher_init(&whole);
    std::ostringstream blocks;
    size_t nb = (m.size + block_size - 1) / block_size;
    for (size_t i = 0; i < nb; i++) {
        size_t off = i * block_size, len = std::min(block_size, m.size - off);
        blake3_hasher_update(&whole, data.data() + off, len);
        m.blocks.push_back(holo::b3_hex(data.data() + off, len));
        blocks << "    \"" << m.blocks.back() << '"' << (i + 1 < nb ? "," : "") << "\n";
    }
    uint8_t o[BLAKE3_OUT_LEN]; blake3_hasher_finalize(&whole, o, BLAKE3_OUT_LEN);
    static const char * hx = "0123456789abcdef";
    for (int i = 0; i < BLAKE3_OUT_LEN; i++) { m.b3 += hx[o[i] >> 4]; m.b3 += hx[o[i] & 15]; }
    std::ostringstream j;
    j << "{\n  \"size\": " << m.size << ",\n  \"block_size\": " << block_size
      << ",\n  \"b3\": \"" << m.b3 << "\",\n  \"blocks\": [\n" << blocks.str() << "  ]\n}\n";
    *json_out = j.str();
    return m;
}

// ── GGUF tensor-name walk (the loader's tensor_list contract; LF endings, strict) ──
static std::string gguf_tensor_list(const uint8_t * d, size_t n) {
    auto rd = [&](size_t & p, size_t k) { uint64_t v = 0; memcpy(&v, d + p, k); p += k; return v; };
    size_t p = 0;
    if (n < 24 || memcmp(d, "GGUF", 4)) throw std::runtime_error("not a GGUF file");
    p = 8;
    uint64_t n_tensors = rd(p, 8), n_kv = rd(p, 8);
    auto skip_str = [&] { uint64_t l = rd(p, 8); p += l; };
    static const size_t tsz[] = { 1,1,2,2,4,4,4,1,0,0,8,8,8 };
    std::function<void(uint32_t)> skip_val = [&](uint32_t t) {
        if (t == 8) skip_str();
        else if (t == 9) { uint32_t et = (uint32_t) rd(p, 4); uint64_t c = rd(p, 8);
            if (et == 8) { while (c--) skip_str(); } else if (et == 9) { while (c--) skip_val(9); } else p += tsz[et] * c; }
        else p += tsz[t];
    };
    for (uint64_t i = 0; i < n_kv; i++) { skip_str(); skip_val((uint32_t) rd(p, 4)); }
    std::string out;
    for (uint64_t i = 0; i < n_tensors; i++) {
        uint64_t l = rd(p, 8);
        out.append((const char *) d + p, l); out += '\n'; p += l;
        uint32_t nd = (uint32_t) rd(p, 4); p += 8 * nd + 4 + 8;
    }
    return out;
}

// ── receipt plumbing ────────────────────────────────────────────────────────────
// The engine identity is the BLAKE3 of THIS binary — real bytes, not a version string.
static std::string engine_b3() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string bytes = slurp(path);
    return holo::b3_hex((const uint8_t *) bytes.data(), bytes.size());
}
static std::string ids_json(const std::vector<int> & v) {
    std::ostringstream o; o << "[";
    for (size_t i = 0; i < v.size(); i++) o << (i ? "," : "") << v[i];
    o << "]"; return o.str();
}
static std::vector<int> parse_ids(const std::string & j, const char * key) {
    std::vector<int> v;
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return v;
    p = j.find('[', p); size_t e = j.find(']', p);
    size_t q = p;
    while (q < e) {
        q = j.find_first_of("-0123456789", q + 1);
        if (q == std::string::npos || q > e) break;
        v.push_back(atoi(j.c_str() + q));
        q = j.find_first_not_of("-0123456789", q);
    }
    return v;
}
static std::string str_field(const std::string & j, const char * key) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return "";
    p = j.find('"', j.find(':', p));
    return j.substr(p + 1, j.find('"', p + 1) - p - 1);
}

// ── the resolver ────────────────────────────────────────────────────────────────
struct resolved { std::string b3; std::vector<std::string> origins; };

static resolved resolve(const std::string & ref);

static resolved acquire(const std::vector<std::string> & origins, size_t size_hint, const std::string & alias) {
    // Stream from origins into RAM, mint manifest+address, land in the store.
    // size needed up front: origins[0] local file → stat; URL → curl -I.
    size_t size = size_hint;
    if (!size) {
        if (origins[0].rfind("http", 0) == 0) {
            std::string h = run_capture("curl -sIL \"" + origins[0] + "\"");
            for (const char * k : { "x-linked-size:", "X-Linked-Size:", "content-length:", "Content-Length:" }) {
                auto q = h.find(k);
                if (q != std::string::npos) { size = strtoull(h.c_str() + q + strlen(k), nullptr, 10); if (size) break; }
            }
        } else {
            std::ifstream f(origins[0], std::ios::binary | std::ios::ate);
            size = (size_t) f.tellg();
        }
    }
    if (!size) throw std::runtime_error("cannot determine size of " + origins[0]);

    // trust-on-first-use manifest: every block accepted, hashed as it arrives
    holo::manifest tofu; tofu.size = size; tofu.block_size = 8 << 20;
    size_t nb = (size + tofu.block_size - 1) / tofu.block_size;
    tofu.blocks.assign(nb, "*");                          // "*" = record, don't check
    holo::verified_buffer vb(size);
    holo::fetcher fx(tofu, vb, origins);
    fx.start(); fx.join();
    if (vb.verified.load() != size) throw std::runtime_error("acquisition failed: " + vb.error);

    std::string mjson;
    holo::manifest m = mint_manifest(vb.data, 8 << 20, &mjson);
    std::string root = store_root();
    spit(root + "/manifests/" + m.b3 + ".json", mjson);
    { std::ofstream f(root + "/blobs/" + m.b3, std::ios::binary); f.write((const char *) vb.data.data(), (std::streamsize) m.size); }
    spit(root + "/tensors/" + m.b3 + ".txt", gguf_tensor_list(vb.data.data(), m.size));
    std::ostringstream rj;
    rj << "{ \"b3\": \"" << m.b3 << "\", \"urls\": [";
    for (size_t i = 0; i < origins.size(); i++)
        if (origins[i].rfind("http", 0) == 0) rj << (i ? "," : "") << "\"" << origins[i] << "\"";
    rj << "] }\n";
    if (!alias.empty()) spit(root + "/refs/" + alias + ".json", rj.str());
    fprintf(stderr, "[%8.1fms] stored  holo:b3:%s  (%zu bytes)\n", ms(), m.b3.c_str(), m.size);
    return { m.b3, { root + "/blobs/" + m.b3 } };
}

static resolved resolve(const std::string & ref) {
    std::string root = store_root();
    if (ref.rfind("holo:b3:", 0) == 0) {
        std::string hex = ref.substr(8);
        if (!exists(root + "/manifests/" + hex + ".json"))
            throw std::runtime_error("no mirror serves holo:b3:" + hex.substr(0, 16) + "… — `holo pull` it from an hf: or path ref first");
        return { hex, { root + "/blobs/" + hex } };
    }
    if (ref.rfind("hf:", 0) == 0) {
        std::string rest = ref.substr(3), repo = rest, file;
        size_t s1 = rest.find('/'); size_t s2 = rest.find('/', s1 + 1);
        if (s2 != std::string::npos) { repo = rest.substr(0, s2); file = rest.substr(s2 + 1); }
        if (file.empty()) {
            std::string api = run_capture("curl -sL \"https://huggingface.co/api/models/" + repo + "\"");
            size_t p = 0;
            while ((p = api.find(".gguf\"", p)) != std::string::npos) {
                size_t q = api.rfind('"', p);
                file = api.substr(q + 1, p + 5 - q - 1);
                break;
            }
            if (file.empty()) throw std::runtime_error("no .gguf in hf:" + repo);
        }
        std::string url = "https://huggingface.co/" + repo + "/resolve/main/" + file;
        std::string alias = repo.substr(repo.find('/') + 1);
        return acquire({ url }, 0, alias);
    }
    if (exists(ref)) return acquire({ ref }, 0, "");
    if (exists(root + "/refs/" + ref + ".json")) {
        std::string j = slurp(root + "/refs/" + ref + ".json");
        size_t p = j.find("\"b3\"");
        p = j.find('"', j.find(':', p));
        std::string hex = j.substr(p + 1, j.find('"', p + 1) - p - 1);
        return resolve("holo:b3:" + hex);
    }
    throw std::runtime_error("cannot resolve '" + ref + "' (not an hf: ref, file, holo:b3: address, or known alias)");
}

// ── run ─────────────────────────────────────────────────────────────────────────
static int cmd_run(const resolved & r, const std::string & prompt, int n_gen) {
    std::string root = store_root();
    holo::manifest m = holo::manifest::parse_file(root + "/manifests/" + r.b3 + ".json");
    fprintf(stderr, "[%8.1fms] resolved  holo:b3:%.16s…  (%zu bytes, warm)\n", ms(), r.b3.c_str(), m.size);
    std::string tpath = root + "/tensors/" + r.b3 + ".txt";

    holo::verified_buffer vb(m.size);           // verify-on-read: warm bytes re-checked block-wise
    holo::fetcher fx(m, vb, r.origins);
    fx.start();

    llama_backend_init();
    const char * ctx_tag = "holo";
    const char * paths[1] = { "holo://model" };
    std::thread fulfiller([&] {
        auto tb = std::make_unique<std::filebuf>();
        tb->open(tpath, std::ios::in | std::ios::binary);
        llama_model_load_fulfill_split_future(tpath.c_str(), ctx_tag, std::unique_ptr<std::streambuf>(std::move(tb)));
        llama_model_load_fulfill_split_future("holo://model", ctx_tag,
                                              std::unique_ptr<std::streambuf>(std::make_unique<holo::stream_buf>(vb)));
    });
    llama_model_params mp = llama_model_default_params();
    mp.use_mmap = false;
    llama_model * model = llama_model_load_from_split_futures(paths, 1, ctx_tag, tpath.c_str(), mp);
    fulfiller.join(); fx.join();
    if (!model) { fprintf(stderr, "REFUSED: model not loaded\n"); return 1; }
    fprintf(stderr, "[%8.1fms] load OK — %zu/%zu bytes verified\n", ms(), vb.verified.load(), m.size);

    const llama_vocab * vocab = llama_model_get_vocab(model);
    llama_context_params cp = llama_context_default_params(); cp.n_ctx = 512;
    llama_context * lctx = llama_init_from_model(model, cp);
    llama_token toks[128];
    int n = llama_tokenize(vocab, prompt.c_str(), (int) prompt.size(), toks, 128, true, false);
    std::vector<int> in_ids(toks, toks + n), out_ids;
    llama_batch b = llama_batch_get_one(toks, n);
    llama_decode(lctx, b);
    for (int i = 0; i < n_gen; i++) {
        const float * lg = llama_get_logits_ith(lctx, -1);
        int nv = llama_vocab_n_tokens(vocab), best = 0;
        for (int t = 1; t < nv; t++) if (lg[t] > lg[best]) best = t;
        char piece[64];
        int pn = llama_token_to_piece(vocab, best, piece, sizeof piece, 0, false);
        fwrite(piece, 1, pn > 0 ? pn : 0, stdout); fflush(stdout);
        out_ids.push_back(best);
        llama_batch nb = llama_batch_get_one(&best, 1);
        llama_decode(lctx, nb);
    }
    printf("\n");

    // seal: real bytes only — model = verified blob address, engine = b3(this binary),
    // ids = what actually went in and came out; greedy, so replay is exact.
    double t_seal0 = ms();
    std::ostringstream rj;
    rj << "{\n  \"v\": 1,\n  \"model_b3\": \"" << r.b3 << "\",\n  \"engine_b3\": \"" << engine_b3()
       << "\",\n  \"decode\": \"greedy\",\n  \"n_ctx\": " << cp.n_ctx
       << ",\n  \"prompt\": \"" << prompt << "\",\n  \"input_ids\": " << ids_json(in_ids)
       << ",\n  \"output_ids\": " << ids_json(out_ids) << "\n}\n";
    std::string rjson = rj.str();
    std::string rb3 = holo::b3_hex((const uint8_t *) rjson.data(), rjson.size());
    spit(store_root() + "/receipts/" + rb3 + ".json", rjson);
    double seal_cost = ms() - t_seal0;
    fprintf(stderr, "[%8.1fms] done — %d tokens\n", ms(), n_gen);
    fprintf(stderr, "⟡ sealed  holo:b3:%.16s…   (seal cost %.2f ms)\n", rb3.c_str(), seal_cost);
    fprintf(stderr, "  verify:  holo verify %s\n", rb3.c_str());
    llama_free(lctx); llama_model_free(model); llama_backend_free();
    return 0;
}

// ── verify: re-derive the sealed answer; byte-identical output ids or loud failure ──
static int cmd_verify(const std::string & ref) {
    std::string hex = ref;
    if (hex.rfind("holo:b3:", 0) == 0) hex = hex.substr(8);
    std::string rpath = store_root() + "/receipts/" + hex + ".json";
    if (!exists(rpath) && exists(ref)) rpath = ref;      // allow a receipt file path
    if (!exists(rpath)) throw std::runtime_error("no receipt holo:b3:" + hex.substr(0, 16) + "… in the store");
    std::string j = slurp(rpath);

    // a receipt is content-addressed too: its bytes must match its name
    std::string self = holo::b3_hex((const uint8_t *) j.data(), j.size());
    if (rpath.find(hex) != std::string::npos && self != hex)
        throw std::runtime_error("receipt bytes do not match their address (tampered receipt)");

    std::string model_b3 = str_field(j, "model_b3"), eng = str_field(j, "engine_b3");
    std::vector<int> in_ids = parse_ids(j, "input_ids"), want = parse_ids(j, "output_ids");
    if (model_b3.empty() || in_ids.empty()) throw std::runtime_error("malformed receipt");
    if (eng != engine_b3())
        fprintf(stderr, "note: engine differs from the sealing engine — replay may legitimately diverge\n");

    resolved r = resolve("holo:b3:" + model_b3);
    holo::manifest m = holo::manifest::parse_file(store_root() + "/manifests/" + model_b3 + ".json");
    holo::verified_buffer vb(m.size);
    holo::fetcher fx(m, vb, r.origins);
    fx.start();
    llama_backend_init();
    const char * ctx_tag = "holo-verify";
    const char * paths[1] = { "holo://model" };
    std::string tpath = store_root() + "/tensors/" + model_b3 + ".txt";
    std::thread fulfiller([&] {
        auto tb = std::make_unique<std::filebuf>();
        tb->open(tpath, std::ios::in | std::ios::binary);
        llama_model_load_fulfill_split_future(tpath.c_str(), ctx_tag, std::unique_ptr<std::streambuf>(std::move(tb)));
        llama_model_load_fulfill_split_future("holo://model", ctx_tag,
                                              std::unique_ptr<std::streambuf>(std::make_unique<holo::stream_buf>(vb)));
    });
    llama_model_params mp = llama_model_default_params(); mp.use_mmap = false;
    llama_model * model = llama_model_load_from_split_futures(paths, 1, ctx_tag, tpath.c_str(), mp);
    fulfiller.join(); fx.join();
    if (!model) { fprintf(stderr, "REFUSED: model failed verification — cannot replay\n"); return 1; }

    llama_context_params cp = llama_context_default_params();
    std::string nctx = str_field(j, "n_ctx");
    cp.n_ctx = 512;
    llama_context * lctx = llama_init_from_model(model, cp);
    std::vector<llama_token> toks(in_ids.begin(), in_ids.end());
    llama_batch b = llama_batch_get_one(toks.data(), (int) toks.size());
    llama_decode(lctx, b);
    const llama_vocab * vocab = llama_model_get_vocab(model);
    for (size_t i = 0; i < want.size(); i++) {
        const float * lg = llama_get_logits_ith(lctx, -1);
        int nv = llama_vocab_n_tokens(vocab), best = 0;
        for (int t = 1; t < nv; t++) if (lg[t] > lg[best]) best = t;
        if (best != want[i]) {
            fprintf(stderr, "REFUSED: re-derivation diverged at output position %zu (got %d, sealed %d)\n", i, best, want[i]);
            llama_free(lctx); llama_model_free(model); llama_backend_free();
            return 1;
        }
        llama_token bt = (llama_token) best;
        llama_batch nb = llama_batch_get_one(&bt, 1);
        llama_decode(lctx, nb);
    }
    printf("VERIFIED: re-derived byte-identical — %zu output tokens match holo:b3:%.16s…\n", want.size(), hex.c_str());
    llama_free(lctx); llama_model_free(model); llama_backend_free();
    return 0;
}

int main(int argc, char ** argv) {
    T0 = clk::now();
    ensure_dirs();
    if (argc < 2) { fprintf(stderr, "usage: holo pull|run|ls <ref> [--prompt T] [-n N]\n"); return 2; }
    std::string cmd = argv[1];
    try {
        if (cmd == "ls") {
            std::string out = run_capture("cmd /c dir /b \"" + store_root() + "\\refs\" 2>nul");
            printf("%s", out.c_str());
            return 0;
        }
        if (argc < 3) { fprintf(stderr, "usage: holo %s <ref>\n", cmd.c_str()); return 2; }
        std::string ref = argv[2], prompt = "The capital of Japan is";
        int n_gen = 8;
        for (int i = 3; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--prompt") prompt = argv[++i];
            else if (a == "-n") n_gen = atoi(argv[++i]);
        }
        if (cmd == "verify") return cmd_verify(ref);
        resolved r = resolve(ref);
        printf("holo:b3:%s\n", r.b3.c_str());
        if (cmd == "run") return cmd_run(r, prompt, n_gen);
        return 0;
    } catch (const std::exception & e) {
        fprintf(stderr, "holo: %s\n", e.what());
        return 1;
    }
}
