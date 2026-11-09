# rbsp-bench.exe: a model on your GPU, without rekordbox

A diagnostic that runs one chunk of a Demucs ONNX model, repeatedly, on the GPU (DirectML) or CPU, with Pioneer's ONNX Runtime. It reports the time of each run, the **peak dedicated and shared video memory**, and **how ONNX Runtime placed the nodes**. It is published as an asset of each test prerelease (`rbsp-bench.exe`) and isn't part of the signed install.

Quit rekordbox first (it holds GPU memory), then in PowerShell, in the folder you saved it to:

```powershell
# our Demucs v4 (what Stems Cache installed there) on the GPU; --timeout ends a stalled run
.\rbsp-bench.exe --model "$env:APPDATA\Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx" --timeout 120

# the same on the CPU, for the baseline
.\rbsp-bench.exe --model "<same file>" --cpu

# rekordbox's own model (Stems Cache keeps Pioneer's original)
.\rbsp-bench.exe --model "$env:APPDATA\rbstemsplus\originals\hdemucs.onnx"

# the fixed-shape model (download stemsplus-model-dml.onnx from release windows-test-6dml2)
.\rbsp-bench.exe --model "$HOME\Downloads\stemsplus-model-dml.onnx" --timeout 120
```

If a path is wrong, find it with `dir /s "$env:APPDATA\rbstemsplus\originals"`.

Other options: `--threads N` (CPU threads with `--cpu`, default 4: the bench showed Demucs v4 at 3.2x real time with 4 threads against 0.88x inside rekordbox), `--dml N` (the adapter; default the one with the most video memory, as the bridge picks), `--runs N`, `--samples N`, `--frames N`, `--ort DLL` (default `%ProgramData%\rbstemsplus\ort\onnxruntime.dll`), `--rb DIR` (the folder with `DirectML.dll`; default the newest under `C:\Program Files\rekordbox`), `--profile` (writes ONNX Runtime's profile json), `--verbose` (every ORT log line).

## Reading the output
- **`run N: X s = Yx real time`**: Y above 1 keeps up with playback. Run 0 is a warm-up.
- **`video memory ... peak: dedicated A GB, shared B GB`**:
  - dedicated near the card's size, with shared climbing = **the model doesn't fit**; it needs a smaller or chunked model, not just the fixed-shape one;
  - small shared and a fast run = the GPU is fine for this model.
- **`[ort]` placement lines** (`Node(s) placed on [CPUExecutionProvider]. Number of nodes: N`): many CPU nodes, or many memcpy lines, = **graph splitting**; the fixed-shape model should help.
- **`... still running after N s; video memory now: ...`** every 5 s during a run: dedicated at the card's size with shared growing = memory spill.
- **`ENDED at the timeout`** = a run that stalls (the rekordbox hang); the memory line tells which of the above.
- With neither memory nor placement explaining a slow run, use `--profile`: the json names the slow operators.

Send the whole output of each command.

## Results so far (RTX 4060 Laptop 8 GB, rekordbox 7.2.14, 2026-11-09)
- **rekordbox's own model on DirectML:** 0.25 s per chunk (43x real time), 1.2 GB of video memory, 304 of 1,368 nodes on the CPU. Fine.
- **Our Demucs v4 on the CPU:** 4 threads 2.5x, 12 threads 3.4x real time (3.2 s per chunk). rekordbox's own session options gave 0.88x; windows-test-8 sets up to 12 threads for our model's CPU sessions.
- **Our Demucs v4 on DirectML:** the first run never finishes, with dedicated memory at the card's size (8.9 to 9.5 GB; shared 0.1 to 4 GB). The same with a 2 s chunk, and with the fixed-shape model, where all nodes sit on DirectML. So it isn't graph splitting or the chunk length.
- **Memory arithmetic (fixed-shape model, from its graph):** weights 287 MB; activations alive at once ~0.6 GB, but all intermediate tensors together 31.9 GB, of which about a hundred attention matrices are 220 MB each. DirectML doesn't recycle activation memory the way the CPU does, so this is what to shrink.

## Memory experiments (windows-test-8)
Extra assets of the prerelease, made by `scripts/gpu-experiments/make_variants.py`:
- `stemsplus-model-fp16.onnx`, `stemsplus-model-dml-fp16.onnx`: fp16 weights and activations (half the memory); inputs and outputs stay fp32, and the input normalisation stays fp32.
- `stemsplus-model-dml.onnx`: the fixed-shape model again.

`--opt 0|1|2|99` sets ONNX Runtime's graph optimization level, which also decides what DirectML fuses. The bench prints `output xt: ... mean|.| ... max|.|`; for the bench's own input the fp32 CPU model gives mean|.| 0.2589, max|.| 1.581 over 3,880,800 values (an fp16 result close to those is right; NaNs or `not finite` mean fp16 overflowed).
