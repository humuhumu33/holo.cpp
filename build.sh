#!/usr/bin/env bash
# build.sh — engine (CMake, submodule) then the holo tools (g++). Tested: Windows/MinGW g++ 15,
# should work on Linux/macOS with the platform's default toolchain (unproven — reports welcome).
set -e
cd "$(dirname "$0")"

EXTRA=""
case "$(uname -s)" in
  MINGW*|MSYS*) EXTRA='-DCMAKE_C_FLAGS=-D_WIN32_WINNT=0x0A00 -DCMAKE_CXX_FLAGS=-D_WIN32_WINNT=0x0A00' ;;
esac

# 1) the engine, unmodified
cmake -S engine -B engine/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF -DGGML_NATIVE=ON $EXTRA
cmake --build engine/build -j --target llama-completion llama-bench 2>/dev/null || cmake --build engine/build -j

Q=engine
LIBS="-Wl,--start-group $Q/build/src/libllama.a $Q/build/ggml/src/ggml.a $Q/build/ggml/src/ggml-base.a $Q/build/ggml/src/ggml-cpu.a -Wl,--end-group -fopenmp"
case "$(uname -s)" in MINGW*|MSYS*) LIBS="$LIBS -lwinmm";; esac

# 2) BLAKE3 (portable reference implementation, vendored)
gcc -c -O3 -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
    vendor/blake3/blake3.c vendor/blake3/blake3_dispatch.c vendor/blake3/blake3_portable.c

# 3) the holo tools
g++ -std=c++17 -O2 tools/holo-pack.cpp blake3*.o -o holo-pack
g++ -std=c++17 -O2 tools/holo-cli.cpp  blake3*.o -I $Q/include -I $Q/ggml/include $LIBS -o holo
cp holo holo-cli 2>/dev/null || cp holo.exe holo-cli.exe
g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 tools/holo-server.cpp blake3*.o \
    -I $Q/include -I $Q/ggml/include -I $Q/vendor/cpp-httplib -I $Q/vendor \
    $Q/build/vendor/cpp-httplib/libcpp-httplib.a $LIBS -lws2_32 -lcrypt32 -o holo-server \
    2>/dev/null || echo "note: holo-server build needs the vendored cpp-httplib lib (Windows flags shown in tools/holo-server.cpp)"

echo "done: ./holo, ./holo-cli, ./holo-pack, ./holo-server"
