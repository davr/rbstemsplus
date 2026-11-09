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
