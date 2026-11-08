# Separation speed on Windows: the GPU

## What the first real-PC test showed (rekordbox 7.2.14, 2026-11-08)
These timings come from `bridge.log`, test build `windows-test-1`.

- **Demucs v4 ran at about 0.85x real time.** Each chunk rekordbox sends is 485,100 samples, about 11.0 s of audio, and a miss took about **13 s**. On an Apple Silicon Mac the same model runs at about 5x real time.
- **Two tracks separated at once.** When a second track was loaded, misses came every ~7 s, alternating between the two tracks. Each separation then ran at about half that rate.
- **The cache worked:**
  - all 30 chunks of the first track came back as hits, about 0.5 s each, about 15 s for the track;
  - its remaining chunks were then separated.

## CUDA isn't the missing piece
rekordbox for Windows ships a **DirectML** build of ONNX Runtime 1.18.0:
- `onnxruntime.dll` exports `…_DML` and `…Ex_DML`;
- `DirectML.dll` 1.14.1 sits beside it;
- `Demucs3.dll` imports ordinal 4, the `_DML` export.

DirectML runs on any DirectX 12 GPU (NVIDIA, AMD, Intel). There is **no CUDA** in rekordbox's runtime, so nothing can be "turned on" to use CUDA.

So the questions are, in order:
1. **Does rekordbox ask for the GPU at all** for the Demucs session? A setting, the GPU, or a driver may keep it on the CPU.
2. **If it does, does our model run on it?** Our Demucs v4 export may hold operators DirectML lacks. ONNX Runtime then splits the graph and runs those parts on the CPU, copying data between the two. That can be slower than the CPU alone.

## Result of step 1 (windows-test-3 bridge, user's PC, 2026-11-08)
- `Demucs session opened: model 0cc50d877629, memory pattern off, on the CPU (rekordbox added no GPU provider)` appeared for both sessions.
- **No "asked for the … execution provider" line at all.** Demucs3.dll called neither `…_DML` nor `…_CPU` for these sessions, so ONNX Runtime ran them with its default, the CPU.
- Every miss: `the model took 12.5 s: 0.88x real time`.
- So this is **case A** below, unless rekordbox has a GPU setting that is off. Open questions:
  - is there a GPU option in rekordbox's Preferences (Extensions › STEMS, or Advanced)?
  - is rekordbox's own model also on the CPU? Uninstall Demucs v4, load a new track, and read the "Demucs session opened" line.
- **Next step: case A.** The bridge adds DirectML to our model's session itself, behind `gpu=auto|off` in config.ini, and falls back to the CPU if DirectML fails. The speed lines show whether it helps. If our export falls back to the CPU on many operators, that becomes case B.

## Step 1: diagnose (in test build `windows-test-3`)
The bridge now logs:
- each execution provider rekordbox asks for (CPU, or DirectML with its device number), and whether that worked;
- for the Demucs session, `on the GPU (DirectML)` or `on the CPU (rekordbox added no GPU provider)`;
- for each miss, how long the model took and the speed as a multiple of real time.

Compare our model with rekordbox's own. With `rekordbox_model=1` the bridge times rekordbox's model's misses too, so uninstall Demucs v4, load a fresh track, and read the miss lines.

## Step 2: depends on step 1

**A. rekordbox doesn't ask for the GPU, even for its own model.**
- Check rekordbox's settings and the GPU driver (DirectX 12 feature level, an up-to-date driver).
- If rekordbox never uses the GPU on this PC, the bridge can add DirectML **for our model's session only**:
  - get DirectML through `GetExecutionProviderApi("DML")` on the cloned options, keeping memory pattern off and sequential execution, as DirectML requires;
  - add a `gpu=auto|off` key to config.ini and a Settings switch;
  - fall back to rekordbox's options if DirectML fails to start.
- Cost: small, in the bridge and the app.

**B. rekordbox asks for DirectML, but our model is slow on it.**
- Ship `rbsp-bench.exe`, a small diagnostic next to the app:
  - it loads Pioneer's ONNX Runtime (the copy in `%ProgramData%\rbstemsplus\ort`) and runs one 485,100-sample chunk through our model and Pioneer's, on the CPU and on DirectML;
  - it reports the time of each, and logs which nodes ran where, using a verbose custom logger (`CreateEnvWithCustomLogger`).
- With the list of operators that fall back, **re-export Demucs v4 for DirectML**:
  - a fixed chunk length, an opset DirectML 1.14 covers fully, and no operators it lacks;
  - publish it as a second model asset, for example `stemsplus-model-dml.onnx`, named in `payload-windows.json`, with the app picking it on Windows.
  - The export pipeline lives outside this repository (`scripts/build.sh`: `model_src`), so this needs the model's author or that pipeline.
- Cost: medium. The model work is the bulk.

**C. CUDA, only if DirectML can't get close on NVIDIA GPUs.**
- The bridge would run our model's session in a **separate** `onnxruntime-gpu` (CUDA 12 + cuDNN 9: about 1.5 to 2 GB of DLLs, NVIDIA only), as a second ONNX Runtime inside rekordbox's process.
- The second runtime would load under its own file name, with tensors copied between the two runtimes.
- It would be an optional download in the app.
- Cost: large, plus a heavy download and a second runtime to keep in step. **Not recommended before A and B.**

## What would help from the user
- Run `windows-test-3`. Load a track you haven't loaded yet, wait a few chunks, and send `bridge.log`. The lines to look for are "asked for the … execution provider" and "Demucs session opened … on the GPU/CPU".
- Your GPU model, and whether rekordbox's own model is faster than ours on this PC.
