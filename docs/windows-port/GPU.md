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

## rekordbox's own model is on the CPU too (user, 2026-11-08)
- `Demucs session opened: model 435c987855f7, rekordbox's session options, on the CPU (rekordbox added no GPU provider), rekordbox's own: stems rebuilt`
- `miss … rebuilt, the model took 2.4 s: 4.50x real time`
- So rekordbox's model is faster only because it is lighter (Demucs v3); it isn't using the GPU. **Decision (user): force DirectML for the Demucs sessions, on by default.**

## Result of the override (windows-test-5, user's PC, RTX 4060 Laptop 8 GB, 2026-11-08)
- **The GPU choice works:** the RTX 4060 was picked over the AMD 780M and the software adapter.
- **rekordbox's own model (Demucs v3) is fast on DirectML:** `the model took 0.3 s: 41x real time`, against 2.4 s (4.5x) on the CPU.
  - But chunks still arrive only every ~1.45 s, so ~1.1 s per chunk goes on something other than `Run`: rekordbox's own TensorFlow STFT/iSTFT, decoding, or the bridge's own work on rekordbox's thread. windows-test-6 logs the split (`hook X s (model Y, bridge Z), rekordbox W s since the last chunk`, plus an average every 20 chunks).
- **Our Demucs v4 on DirectML breaks rekordbox:**
  - two GPU sessions open and no chunk ever completes; rekordbox stays on "Initializing STEMS Engine";
  - Task Manager: 3D at 95%, **dedicated memory 7.7 of 8 GB, shared 12.7 GB**. The weights and activations don't fit, and the overflow goes through system memory over PCIe;
  - our export has 9,800 nodes (555 `Shape` ops: dynamic shapes), which may also split between the GPU and CPU.

### What windows-test-6 changes
- **`gpu_v4=0` is the new default:** `gpu=1` now means rekordbox's own model on the GPU; Demucs v4 stays on the CPU (0.88x) unless `gpu_v4=1` (Settings: "Also use the GPU for Demucs v4 (experimental)").
- **A watchdog:** a GPU run gets a deadline (the larger of 25 s and 3x the chunk's length). Past it, the run is terminated and answered by a CPU twin of the session, and later runs of that model use the twin. Logged once (`GPU run took over its deadline ... this model now runs on the CPU`). A stall inside one GPU operator can't be interrupted until it returns, but the typical spill-to-shared-memory slowness is between operators.
- **One GPU session per model:** rekordbox opens the model twice; the copies now share one session (runs are serialised), so the GPU holds the weights once.
- **`rbsp-bench.exe`** to find out why Demucs v4 stalls (memory, graph splitting or one slow operator) without rekordbox: see [GPU-BENCH.md](GPU-BENCH.md).
- The fixed-shape model (targets graph splitting only) is on branch `claude/dml-model`; its files are on release `windows-test-6dml2`. `rbsp-bench.exe` can test it.

## The override (in progress: see STATUS.md "Where things stand")
- `gpu=1` in config.ini is the default; `gpu=0` turns it off.
- For every Demucs session, the bridge clones rekordbox's options, sets the memory pattern off and sequential execution, and calls Pioneer's `OrtSessionOptionsAppendExecutionProvider_DML(o, adapter)`.
- The adapter is the DXGI adapter with the most dedicated video memory. Software adapters are skipped. Each adapter is logged once (`GPU: adapter N: <name>, X GB`).
- **Fallback, logged:**
  - no DirectML export: Microsoft's CPU build in the tests;
  - no adapter, or DirectML refused;
  - CreateSession with DirectML fails.
  - In each case ours runs on the CPU with the memory pattern off, and rekordbox's model gets rekordbox's own options.
- **Risk:** our export may hold operators DirectML lacks. ORT then splits the graph and copies between the GPU and CPU, which may be slower than the CPU alone. The speed line per miss will show it, which would make it case B.

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

## C, started (2026-11-09): the CUDA path in the bridge (not built or run yet)
- `bridge/win/rbstems_bridge_win.c`, section "the GPU (CUDA)": for our model only, with an NVIDIA adapter and a CUDA ONNX Runtime 1.18 at `%ProgramData%\rbstemsplus\ort-cuda\onnxruntime.dll` (administrators-only, same checks as `ort`), the bridge opens one shared CUDA session. Rekordbox still gets a normal CPU session of Pioneer's library (the fallback, our thread count); each Run copies the inputs across, runs CUDA, and copies the outputs back into Pioneer's tensors.
- A CUDA error or a missed deadline turns CUDA off for good: that run and every later one go to the CPU session. `config.ini` `cuda=0` turns it off (default 1; it does nothing without the `ort-cuda` folder). With CUDA, DirectML is not added to our model's session.
- Still to do: build it in CI (no mingw here), the installer/CI side that puts `ort-cuda` in place, a test stand-in, real-PC runs.
- **The runtime DLLs are not in the NVIDIA driver.** cuBLAS, cuDNN and the CUDA runtime come with the toolkit or have to ship with the package. ONNX Runtime 1.18 needs CUDA 12 (or 11.8) and cuDNN 9 (8 for 11.8). The test PC has only CUDA 13.4 (`cublas64_13.dll`), which 1.18 can't use. So the download is the ~1.5 to 2 GB estimated above, or the user installs CUDA 12 and cuDNN 9 themselves.

### CUDA bench results (RTX 4060 Laptop, 8 GB, 2026-11-09; `rbsp-bench --cuda`, fixed-shape fp32 model)
- ONNX Runtime 1.18.0 **CUDA 12 build needs cuDNN 8** (`cudnn64_8.dll`; 8.9.7 works), plus cuBLAS 12, cuFFT 11, cuRAND 10 and the CUDA 12 runtime next to its `onnxruntime.dll`. The pip `nvidia-*-cu12` wheels for Windows carry them. cuDNN 9 is not enough: the provider fails to load with error 126.
- Defaults: the first run 1.8 s, later runs about 5 s (2x real time) because the arena takes all 8.12 GB and spills.
- `arena_extend_strategy=kSameAsRequested`: 0.55 s per 11 s chunk (20x real time), 6.9 GB peak.
- plus `gpu_mem_limit=5368709120`: 0.45 to 0.56 s (20 to 24x), **3.9 GB peak**. The bridge uses these two.
- All 2966 nodes run on CUDA. The CPU path is 3.4x.
