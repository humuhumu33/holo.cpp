// holo-pack.cpp — mint the block manifest for a model file: the content address and the
// per-block BLAKE3 list that holo_stream.h verifies against.
//
//   holo-pack <model.gguf> [block_size_mb=8] > model.manifest.json
//
// The whole-file b3 is the model's content address (holo:b3:<hex>). Blocks are plain
// ranges of the file — the file itself is untouched, so the same GGUF works everywhere.

#include "../vendor/blake3/blake3.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static std::string hex(const uint8_t * d, size_t n) {
    static const char * h = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; i++) { s[2*i] = h[d[i] >> 4]; s[2*i+1] = h[d[i] & 15]; }
    return s;
}

int main(int argc, char ** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file> [block_size_mb=8]\n", argv[0]); return 2; }
    const size_t bs = (argc > 2 ? (size_t) atoll(argv[2]) : 8) << 20;

    FILE * f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END); const uint64_t size = (uint64_t) _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
#else
    fseek(f, 0, SEEK_END); const uint64_t size = (uint64_t) ftell(f); fseek(f, 0, SEEK_SET);
#endif

    blake3_hasher whole; blake3_hasher_init(&whole);
    std::vector<uint8_t> buf(bs);
    std::vector<std::string> blocks;
    uint64_t done = 0;
    while (done < size) {
        size_t want = (size_t) (size - done < bs ? size - done : bs);
        if (fread(buf.data(), 1, want, f) != want) { fprintf(stderr, "short read at %llu\n", (unsigned long long) done); return 1; }
        blake3_hasher_update(&whole, buf.data(), want);
        blake3_hasher b; blake3_hasher_init(&b); blake3_hasher_update(&b, buf.data(), want);
        uint8_t o[BLAKE3_OUT_LEN]; blake3_hasher_finalize(&b, o, BLAKE3_OUT_LEN);
        blocks.push_back(hex(o, BLAKE3_OUT_LEN));
        done += want;
    }
    fclose(f);
    uint8_t o[BLAKE3_OUT_LEN]; blake3_hasher_finalize(&whole, o, BLAKE3_OUT_LEN);

    printf("{\n  \"file\": \"%s\",\n  \"size\": %llu,\n  \"block_size\": %zu,\n  \"b3\": \"%s\",\n  \"blocks\": [\n",
           argv[1], (unsigned long long) size, bs, hex(o, BLAKE3_OUT_LEN).c_str());
    for (size_t i = 0; i < blocks.size(); i++)
        printf("    \"%s\"%s\n", blocks[i].c_str(), i + 1 < blocks.size() ? "," : "");
    printf("  ]\n}\n");
    fprintf(stderr, "holo:b3:%s  (%llu bytes, %zu blocks of %zu MB)\n",
            hex(o, BLAKE3_OUT_LEN).c_str(), (unsigned long long) size, blocks.size(), bs >> 20);
    return 0;
}
