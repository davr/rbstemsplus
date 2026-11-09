"""make_model.py OUT.onnx: a tiny stand-in for the Demucs model with its real input and output shapes:
mix [1,2,L], mag [1,4,2048,F] -> x [1,16,2048,F] (mag four times), xt [1,8,L] (mix four times)."""
import sys
import onnx
from onnx import TensorProto as T, helper as h
mix = h.make_tensor_value_info("mix", T.FLOAT, [1, 2, "L"])
mag = h.make_tensor_value_info("mag", T.FLOAT, [1, 4, 2048, "F"])
x = h.make_tensor_value_info("x", T.FLOAT, [1, 16, 2048, "F"])
xt = h.make_tensor_value_info("xt", T.FLOAT, [1, 8, "L"])
g = h.make_graph([h.make_node("Concat", ["mag"] * 4, ["x"], axis=1), h.make_node("Concat", ["mix"] * 4, ["xt"], axis=1)],
                 "tiny", [mix, mag], [x, xt])
m = h.make_model(g, opset_imports=[h.make_opsetid("", 13)])
m.ir_version = 8
onnx.checker.check_model(m)
onnx.save(m, sys.argv[1])
