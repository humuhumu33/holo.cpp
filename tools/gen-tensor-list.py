# gen-tensor-list.py — emit the tensor_list_file the futures loader requires:
# one tensor name per line (src/llama-model-load-input.cpp:36-57 reads it with getline
# into a std::set used as a strict contract at src/llama-model-load.cpp:145).
#
# usage: python tools/gen-tensor-list.py model.gguf > tensors.txt
import sys, struct

GGUF_MAGIC = b"GGUF"
T_SIZES = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}

def read_str(f):
    (n,) = struct.unpack("<Q", f.read(8))
    return f.read(n).decode("utf-8", "replace")

def skip_value(f, t):
    if t == 8:  # string
        (n,) = struct.unpack("<Q", f.read(8)); f.seek(n, 1)
    elif t == 9:  # array
        (et,) = struct.unpack("<I", f.read(4))
        (cnt,) = struct.unpack("<Q", f.read(8))
        if et == 8:
            for _ in range(cnt):
                (n,) = struct.unpack("<Q", f.read(8)); f.seek(n, 1)
        elif et == 9:
            for _ in range(cnt): skip_value(f, 9)
        else:
            f.seek(T_SIZES[et] * cnt, 1)
    else:
        f.seek(T_SIZES[t], 1)

with open(sys.argv[1], "rb") as f:
    assert f.read(4) == GGUF_MAGIC, "not a GGUF file"
    (version,) = struct.unpack("<I", f.read(4))
    (n_tensors,) = struct.unpack("<Q", f.read(8))
    (n_kv,) = struct.unpack("<Q", f.read(8))
    for _ in range(n_kv):
        read_str(f)
        (t,) = struct.unpack("<I", f.read(4))
        skip_value(f, t)
    for _ in range(n_tensors):
        name = read_str(f)
        (nd,) = struct.unpack("<I", f.read(4))
        f.seek(8 * nd + 4 + 8, 1)   # dims + type + offset
        print(name)
