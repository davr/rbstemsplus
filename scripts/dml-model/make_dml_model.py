#!/usr/bin/env python3
"""Makes the DirectML-friendly copy of the Demucs v4 model: fixed shapes, shape arithmetic folded away.

    make_dml_model.py IN.onnx OUT.onnx [--samples 485100] [--frames 474] [--check]

Fixes mix [1,2,SAMPLES] and mag [1,4,2048,FRAMES], simplifies with onnxsim (constant folding), and with --check
compares both models on random inputs (x must be all zero; xt must match). Prints operator counts before/after.
"""
import argparse, collections, sys
import numpy as np
import onnx
import onnxruntime as ort


def ops(m):
    return collections.Counter(n.op_type for n in m.graph.node)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("src"); a.add_argument("dst")
    a.add_argument("--samples", type=int, default=485100)
    a.add_argument("--frames", type=int, default=474)
    a.add_argument("--check", action="store_true")
    a.add_argument("--opt", action="store_true", help="also let ORT's basic optimiser fuse/fold (CPU-only fusions are avoided)")
    o = a.parse_args()
    from onnxsim import simplify
    m = onnx.load(o.src)
    before = ops(m)
    shapes = {"mix": [1, 2, o.samples], "mag": [1, 4, 2048, o.frames]}
    s, ok = simplify(m, overwrite_input_shapes=shapes, test_input_shapes=shapes)
    if not ok:
        sys.exit("onnxsim: the simplified model doesn't match the original")
    after = ops(s)
    onnx.save(s, o.dst)
    print(f"nodes {sum(before.values())} -> {sum(after.values())}")
    for k in sorted(set(before) | set(after), key=lambda k: -max(before[k], after[k])):
        print(f"  {k:22s} {before[k]:6d} -> {after[k]:6d}")
    for v in list(s.graph.input) + list(s.graph.output):
        print(v.name, [d.dim_value or d.dim_param for d in v.type.tensor_type.shape.dim])
    if o.check:
        rng = np.random.default_rng(1)
        feed = {"mix": (rng.standard_normal((1, 2, o.samples)) * .1).astype("f"),
                "mag": np.abs(rng.standard_normal((1, 4, 2048, o.frames))).astype("f")}
        r = []
        for p in (o.src, o.dst):
            r.append(ort.InferenceSession(p, providers=["CPUExecutionProvider"]).run(None, feed))
        print("x max |new|", float(abs(r[1][0]).max()), " x max |old|", float(abs(r[0][0]).max()))
        print("xt max abs diff", float(abs(r[0][1] - r[1][1]).max()), " xt max |old|", float(abs(r[0][1]).max()))


main()
