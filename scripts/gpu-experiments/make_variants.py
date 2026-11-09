#!/usr/bin/env python3
"""Experimental copies of the Demucs v4 model for DirectML (see docs/windows-port/GPU-BENCH.md):

    make_variants.py MODEL.onnx OUT_DIR [--samples 485100] [--frames 474]

Writes into OUT_DIR:
  stemsplus-model-fp16.onnx      the model with fp16 weights and activations (half the video memory); inputs and outputs stay fp32
  stemsplus-model-dml.onnx       fixed shapes, shape arithmetic folded away (onnxsim)
  stemsplus-model-dml-fp16.onnx  both

The input normalisation (sums over the whole chunk, values in the millions: more than fp16 holds) stays fp32: every node
before the first convolution or normalisation layer. Nothing here is verified against the CPU model; the bench's output
statistics (mean/max of xt) are compared with the fp32 model's.
"""
import argparse, os, sys, warnings
import onnx
warnings.filterwarnings("ignore")
from onnxconverter_common import float16


def to_fp16(m):
    first = next(i for i, n in enumerate(m.graph.node) if n.op_type in ("Conv", "InstanceNormalization", "GroupNormalization"))
    block = [n.name for n in m.graph.node[:first]]
    # The converter rewrites every unblocked Cast(to=float) into Cast(to=float16), so its output stops matching a consumer
    # that stays fp32 (bench run 2: "/Cast_15_output_0 float16 does not match expected float"). Keep those Casts as they are.
    casts = [n.name for n in m.graph.node[first:]
             if n.op_type == "Cast" and any(a.name == "to" and a.i == onnx.TensorProto.FLOAT for a in n.attribute)]
    block += casts
    print(f"  fp32 nodes: {len(block) - len(casts)} input normalisation + {len(casts)} Cast-to-float, of {len(m.graph.node)}")
    return float16.convert_float_to_float16(m, keep_io_types=True, node_block_list=block, max_finite_val=65504.0)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("src"); a.add_argument("out")
    a.add_argument("--samples", type=int, default=485100)
    a.add_argument("--frames", type=int, default=474)
    o = a.parse_args()
    os.makedirs(o.out, exist_ok=True)
    m = onnx.load(o.src)
    print("fp16, dynamic shapes")
    onnx.save(to_fp16(m), os.path.join(o.out, "stemsplus-model-fp16.onnx"))
    from onnxsim import simplify
    shapes = {"mix": [1, 2, o.samples], "mag": [1, 4, 2048, o.frames]}
    s, ok = simplify(m, overwrite_input_shapes=shapes, test_input_shapes=shapes)
    if not ok:
        sys.exit("onnxsim: the simplified model doesn't match the original")
    onnx.save(s, os.path.join(o.out, "stemsplus-model-dml.onnx"))
    print("fp16, fixed shapes")
    onnx.save(to_fp16(s), os.path.join(o.out, "stemsplus-model-dml-fp16.onnx"))
    for f in sorted(os.listdir(o.out)):
        print(f"{f}: {os.path.getsize(os.path.join(o.out, f)) / 2**20:.0f} MB")


main()
