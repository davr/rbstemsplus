# The DirectML model (experimental, branch `claude/dml-model`)

`make_dml_model.py IN.onnx OUT.onnx [--check]` turns the Demucs v4 model into a copy DirectML can run whole:
the inputs are fixed to `mix [1,2,485100]` and `mag [1,4,2048,474]` (every chunk rekordbox sent in the first
real test was 485,100 samples; 474 frames is what the model produces for that length), and onnxsim folds the
shape arithmetic away. No PyTorch source is needed.

Result on `stemsplus-model.onnx` (sha256 `0cc50d87…`), run here:

| | before | after |
|---|---:|---:|
| nodes | 9,800 | 3,368 |
| Shape | 555 | 0 |
| Constant | 3,143 | 50 |
| Concat | 346 | 4 |
| Range, Mod, Where, ScatterND, Tile, Sin, Cos, ReduceProd, ConstantOfShape | 34, 26, 25, 8, 8, 6, 6, 5, 57 | 0 |
| Gather | 348 | 58 |
| Conv / ConvTranspose / InstanceNormalization / Erf / MatMul | 186 / 20 / 148 / 112 / 108 | 186 / 18 / 148 / 112 / 48 |

On random inputs (CPU): `x` is all zero in both (the bridge relies on that to recognise our model), and `xt` differs by at most
3.1e-6 (|xt| up to 3.2). The file is 349 MB (the original 309 MB: the folded constants are stored).

How it is used:
- `scripts/build-windows.sh` runs it, checks `x` is zero and `xt` differs by under 1e-4, hashes the result and builds the
  bridge with `-DRBSTEMS_DML_SHA=<that hash>`; the payload lists it as `model_dml`.
- The app installs it as `hdemucs-dml.onnx` beside rekordbox's `hdemucs.onnx`, and removes it when Demucs v4 is uninstalled.
- With `gpu=1` the bridge opens it instead of `hdemucs.onnx` for our model, only if its SHA-256 is the compiled-in one; if it
  can't be opened it uses the dynamic model on DirectML as before. It accepts 485,100-sample chunks only.

Needs: `pip install onnx onnxsim onnxruntime numpy`.
