"""make_model.py OUT.onnx: a tiny stand-in for the Demucs model with its real input and output shapes:
mix [1,2,L], mag [1,4,2048,F] -> x [1,16,2048,F] (mag four times), xt [1,8,L] (mix four times).
Stdlib only: the protobuf is written by hand (ONNX IR 8, opset 13), as bridge/tests/make_model.py does."""
import struct
import sys


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def field(num, value):
    """value: int -> varint field; bytes/str -> length-delimited field."""
    if isinstance(value, int):
        return varint(num << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint(num << 3 | 2) + varint(len(value)) + value


FLOAT, INT64 = 1, 7


def tensor(name, dims, dtype, raw):
    return b"".join(field(1, d) for d in dims) + field(2, dtype) + field(8, name) + field(9, raw)


def value_info(name, dims):
    """dims: ints, or strings for symbolic dimensions."""
    shape = b"".join(field(1, field(1, d) if isinstance(d, int) else field(2, d)) for d in dims)
    return field(1, name) + field(2, field(1, field(1, FLOAT) + field(2, shape)))


def node(op, inputs, outputs, name):
    return (b"".join(field(1, i) for i in inputs) + b"".join(field(2, o) for o in outputs)
            + field(3, name) + field(4, op))



graph = b"".join([
    field(1, node("Tile", ["mag", "magreps"], ["x"], "tile_mag")),
    field(1, node("Tile", ["mix", "reps"], ["xt"], "tile_mix")),
    field(2, "rbsp_helper_test"),
    field(5, tensor("magreps", [4], INT64, struct.pack("<4q", 1, 4, 1, 1))),
    field(5, tensor("reps", [3], INT64, struct.pack("<3q", 1, 4, 1))),
    field(11, value_info("mix", [1, 2, "L"])),
    field(11, value_info("mag", [1, 4, 2048, "F"])),
    field(12, value_info("x", [1, 16, 2048, "F"])),
    field(12, value_info("xt", [1, 8, "L"])),
])
with open(sys.argv[1], "wb") as f:
    f.write(field(1, 8) + field(2, "rbsp-helper-tests") + field(7, graph) + field(8, field(1, "") + field(2, 13)))
