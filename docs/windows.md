# RB Stems Plus on Windows

RB Stems Plus has the same two features on Windows as on the Mac:
- **Demucs v4** swaps rekordbox's stems model for Meta's Demucs v4.
- **Stems Cache** saves the stems rekordbox separates, so the next load of a track is much faster.

Read the [main README](../README.md) for what each one does, the sound and speed comparisons, and
the [honest risks](../README.md#honest-risks). This page covers what is different on Windows.

> **Status: new.** The Windows version was tested with rekordbox **7.2.14** on Windows 11. Its
> STEMS model is the same file as on the Mac, and its ONNX Runtime is 1.18.0 (DirectML build).
> On an NVIDIA GPU, Demucs v4 can also run on the GPU through CUDA, about 7 times faster than on
> the CPU: see [Faster on an NVIDIA GPU (CUDA)](#faster-on-an-nvidia-gpu-cuda).

## Testing builds from this fork

Until an official, signed release includes Windows, test builds are published as **prereleases**
of this repository by the `windows-prerelease` workflow. Each says on its page how to install
it, for example (the newest so far is `windows-test-10`):

```powershell
irm https://github.com/davr/rbstemsplus/releases/download/windows-test-10/bootstrap.ps1 | iex
```

A test build's window title says "TEST BUILD". It downloads everything from its own prerelease,
and trusts only a key made for that build. A prerelease also holds `rbsp-bench.exe`, a tool that
times the model on your PC (see [For developers](#for-developers)).

To publish one: put a new tag name in `.github/windows-prerelease.tag` (one line, for example
`windows-test-11`) and push; the workflow builds, signs with a fresh test key and publishes.

## Before you install

- **Windows 10 or 11, 64-bit.**
- **rekordbox 7**, installed from rekordbox.com, with STEMS working and turned on at least once,
  so rekordbox has downloaded its STEMS Engine (*Preferences › Extensions › STEMS*).
- **Stems Cache needs an administrator's permission.** Windows asks once each time Stems Cache is
  installed, reinstalled or removed. Demucs v4 doesn't need it.
- **Disk space:** about 1.5 GB, plus the saved stems (20 GB at most by default). The optional CUDA
  runtime needs about 2.3 GB more.

## Install

1. **Quit rekordbox.**
2. **Open PowerShell.** Press the Windows key, type *PowerShell*, press Enter.
3. **Paste this line and press Enter:**

   ```powershell
   irm https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/bootstrap.ps1 | iex
   ```

   It checks the release's signature, puts **RB Stems Plus** in your Start menu and opens it. It
   installs to `%LOCALAPPDATA%\Programs\RB Stems Plus` and needs no administrator rights.
4. **Click Install Demucs v4, Install Stems Cache, or both.** For Stems Cache, Windows asks
   *"Do you want to allow this app to make changes to your device?"*: click **Yes**.
5. **Open rekordbox, load a track and turn on STEMS.** Load it again later: with Stems Cache the
   stems come back in seconds.

## What it changes on your PC

| | Demucs v4 | Stems Cache |
|---|---|---|
| What changes | `%APPDATA%\Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx` | `onnxruntime.dll` in `C:\Program Files\rekordbox\rekordbox 7.x.y\` |
| Pioneer's original kept in | `%APPDATA%\rbstemsplus\originals\` | `C:\ProgramData\rbstemsplus\pioneer\<version>\` |
| Needs administrator | no | yes (Windows asks) |

## Speed: CPU, DirectML and CUDA

How fast Demucs v4 separates a track depends on where it runs. Measured on an RTX 4060 Laptop GPU
(8 GB) with rekordbox 7.2.14; "real time" means a 3-minute track takes 3 minutes at 1x:

| Where it runs | Speed | Notes |
|---|---|---|
| rekordbox's own settings (the CPU, few threads) | 0.9x | what rekordbox does by itself |
| **CPU, up to 12 threads** (the default for Demucs v4) | 3.4x | Stems Cache sets the thread count |
| DirectML (Windows's GPU layer) | stalls | Demucs v4 needs about 32 GB of video memory there: it never finishes on an 8 GB GPU. Off by default (`gpu_v4=0`) |
| **CUDA** (NVIDIA only, installed by hand) | 20 to 25x | about 3 to 4 GB of video memory |

- Without CUDA nothing changes: Demucs v4 runs on the CPU. With CUDA installed it runs on the GPU
  and falls back to the CPU by itself if CUDA fails or stalls, so rekordbox never waits for it.
- rekordbox's own model can use the GPU through rekordbox's DirectML (`gpu=1`, the default).

## Faster on an NVIDIA GPU (CUDA)

This is **optional, by hand, and not part of the package**: the CUDA files are about 2.3 GB.
You need an NVIDIA GPU with about 4 GB of free video memory (only an 8 GB card was tested), a current NVIDIA driver
(CUDA 12 needs 527.41 or newer), and Stems Cache and Demucs v4 installed. It applies to Demucs v4
only. If anything is missing, Demucs v4 just stays on the CPU.

1. **Collect the files in one folder**, for example `C:\dj\ort-cuda`:
   - from ONNX Runtime 1.18.0's release,
     [`onnxruntime-win-x64-gpu-cuda12-1.18.0.zip`](https://github.com/microsoft/onnxruntime/releases/tag/v1.18.0):
     `onnxruntime.dll`, `onnxruntime_providers_cuda.dll`, `onnxruntime_providers_shared.dll`;
   - from NVIDIA's Python packages (`pip download <name>==<version> --no-deps --platform win_amd64
     --only-binary=:all:`, then open each `.whl` as a zip and take its `.dll` files):
     `nvidia-cuda-runtime-cu12` 12.4 (`cudart64_12.dll`), `nvidia-cublas-cu12` 12.4
     (`cublas64_12.dll`, `cublasLt64_12.dll`), `nvidia-cufft-cu12` 11.2.1.3 (`cufft64_11.dll`),
     `nvidia-curand-cu12` 10.3.5.147 (`curand64_10.dll`) and **`nvidia-cudnn-cu12` 8.9.7.29**
     (all of `cudnn*64_8.dll`, including the `_train` ones).
   - It must be **cuDNN 8**: this ONNX Runtime build links `cudnn64_8.dll`, and cuDNN 9 or a CUDA 13
     toolkit don't work with it.
2. **Install them, from an administrator PowerShell** (the bridge only loads files that nobody but
   administrators can change):

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\windows-install-cuda.ps1 -Source C:\dj\ort-cuda
   ```

   It copies the DLLs to `C:\ProgramData\rbstemsplus\ort-cuda` with administrator-only permissions.
   Install Stems Cache first: the script needs `C:\ProgramData\rbstemsplus` to exist.
   `-Remove` takes it away again. Uninstalling Stems Cache or RB Stems Plus deletes it too.
3. **Open rekordbox, load a track.** `%LOCALAPPDATA%\rbstemsplus\Logs\bridge.log` should say:
   - `CUDA: ONNX Runtime 1.18.0 loaded from C:\ProgramData\rbstemsplus\ort-cuda\onnxruntime.dll`
   - `CUDA: session opened for model ...`
   - `Demucs session opened: ... on the GPU (CUDA; this CPU session is the fallback)`
   - `CUDA run: ... s` for the first three chunks, then `miss ... the model took 0.4 s: 25x real time`.

How it works, briefly: Pioneer's ONNX Runtime has no CUDA, so the bridge loads a second, CUDA
ONNX Runtime next to it and runs only Demucs v4 there. It copies each chunk across, runs it,
copies the stems back, and keeps a CPU session as the fallback. Its memory is sized to the
requests with a 5 GiB limit (about 3.2 to 3.9 GB used). `cuda=0` in `config.ini` turns it off.

Other files:
- **Saved stems:** `%LOCALAPPDATA%\rbstemsplus\Cache`, or a folder named
  `RB Stems Plus Cache` on an external drive, chosen in Settings.
- **Settings:** `%APPDATA%\rbstemsplus\config.ini`.
- **GPU settings** (Settings checkboxes, or `config.ini`):
  - `gpu=1` (the default): rekordbox's own model runs on the GPU through DirectML; `gpu=0` keeps everything on the CPU.
  - `gpu_v4=0` (the default): Demucs v4 stays off DirectML, where it stalls on 8 GB GPUs. `gpu_v4=1` is experimental.
  - `cuda=1` (the default, `config.ini` only): Demucs v4 on CUDA when the runtime above is installed; `cuda=0` turns it off. It also needs `gpu=1`.
  - A GPU run that fails or takes too long is answered by the CPU and the model stays there until rekordbox restarts. `bridge.log` says which one ran.
- **Logs:** `%LOCALAPPDATA%\rbstemsplus\Logs`.
- **The copy of Pioneer's ONNX Runtime that Stems Cache uses:**
  `C:\ProgramData\rbstemsplus\ort\onnxruntime.dll`. Only administrators can change it. Stems
  Cache refuses to load a copy that other accounts can change.
- **The CUDA runtime (optional):** `C:\ProgramData\rbstemsplus\ort-cuda\`, administrator-only like `ort`.
- **Reinstall reminders:** a small helper, *RB Stems Plus Watcher*, starts when you sign in. It
  appears in *Settings › Apps › Startup*. It only asks you to reinstall after a rekordbox update;
  it never changes anything by itself.

Unlike on the Mac, nothing is re-signed. rekordbox's own files keep AlphaTheta's signature, except
the one `onnxruntime.dll` that Stems Cache replaces.

## Prepare playlists before a set (RB Stems Plus 1.3)

> **New on Windows.** Checked on rekordbox 7.2.14 with an NVIDIA GPU: tracks the Prepare tab prepared loaded in
> rekordbox with every part found in the cache (MP3, FLAC and AAC, at 44.1 and 48 kHz). What was measured, and
> what is still open, is in [`windows-port/PREPARE.md`](windows-port/PREPARE.md).

The window has three tabs: **Status** (the lights and your prepared playlists), **Prepare**, and **Manage**
(install, reinstall and uninstall each feature, and the log).

With Stems Cache installed, the **Prepare** tab makes a playlist's stems in advance, so every track of it loads
with its stems at once in rekordbox, as if you had loaded it before (it writes the same entries rekordbox would).

1. Choose a playlist on the left (rekordbox's own list, read only). The tab says how many tracks are ready, how
   many it will prepare, how much space that takes and about how long.
2. Choose until when to keep them (10 days by default), then click **Prepare**. It works in the background: you
   can keep using the PC and rekordbox. **Pause** and **Stop** are there; what is done stays done.
3. The **Status** tab lists the prepared playlists (5 at most, together at most 75% of Stems Cache's size limit)
   and whether each is still ready. When rekordbox opens, RB Stems Plus checks them too and shows a Windows
   notification if a track isn't fully prepared any more (changed, moved, removed from the cache, or added to
   the playlist): click **Prepare** again for those.

What to know:
- **Prepared stems are used only when rekordbox is set to *Prioritize sound quality*** (Preferences › Extensions
  › STEMS). With *Prioritize speed* rekordbox uses another engine and the Prepare tab says so.
- **Supported tracks: MP3, AAC (M4A), ALAC, FLAC, AIFF and WAV**, at 44.1 kHz or another rate (48 kHz was
  measured). A prepared entry is found by rekordbox only if the helper decodes the file exactly as rekordbox does;
  this was measured on rekordbox 7.2.14. A track the helper can't decode, or whose file isn't there, is listed as
  *Skipped* with the reason.
- **Known gap:** for some AAC (M4A) files the last few seconds of the track are separated by rekordbox when you
  load it (one part of about 11 s), because rekordbox ends the track slightly earlier than Windows's decoder does.
  The rest of the track is prepared.
- **Demucs v4 only for now.** With rekordbox's own model installed, preparing is refused.
- **Speed:** with the optional CUDA runtime (see above) about 25 times real time on an RTX 4060 laptop, so a
  15-track playlist takes about 7 minutes. Without it the CPU is used, at about 3 times real time.
- **A track that isn't at 44.1 kHz needs a processor with FMA** (every Intel and AMD processor since about 2015).
  On an older one such a track is skipped.
- **Where rekordbox's library is:** RB Stems Plus reads it where rekordbox keeps it, also when you moved it to
  another drive (rekordbox's *Preferences › Advanced › Database*).
- Installing, uninstalling, updating or clearing stems stops a run first (it asks). Quitting stops it too.
- The kept sets are files in `%APPDATA%\rbstemsplus\kept`; Stems Cache never removes their stems while the date
  hasn't passed (unless its size limit is lowered below them). **Stop Keeping** on the Status tab lets one go.

## The lights

The lights work as on the Mac:
- **Green:** the feature is on.
- **Red:** it's off.
- **Yellow:** Stems Cache is installed but not saving stems right now. Hold the pointer over the
  light to see why: the disk is low, its drive is unplugged, or rekordbox has no STEMS Engine yet.

## After a rekordbox update

Each rekordbox update for Windows installs a **new folder** (`rekordbox 7.x.y`) with Pioneer's
original files, so **Stems Cache is off until you reinstall it**. Demucs v4 keeps working until
rekordbox downloads a new STEMS Engine. When the update is done and rekordbox is closed, RB Stems
Plus asks you:
- **Reinstall Now** opens the app and reinstalls what went missing.
- **Remind Me Later** asks again later.
- **Don't Ask Again for This Version** stops asking until the next update.

## Updating RB Stems Plus

RB Stems Plus checks at most once a day. When a newer version is out, *RB Stems Plus › Update
RB Stems Plus* appears. It replaces the app in one step and restarts it.

## Uninstall

In the app:
- **Uninstall Stems Cache** puts Pioneer's `onnxruntime.dll` back, byte for byte.
- **Uninstall Demucs v4** puts rekordbox's own model back.
- **Uninstall RB Stems Plus Completely** does both, then deletes the app, its settings, logs and
  saved stems, and the Start menu shortcut.

Reinstalling rekordbox from rekordbox.com also always gives you Pioneer's files.

## Troubleshooting

- **A button is greyed out.** The line under the buttons says why: rekordbox isn't installed, its
  version isn't supported yet, or there's no STEMS Engine yet. RB Stems Plus downloads a new list
  of supported versions each day.
- **"Windows didn't give RB Stems Plus administrator rights."** You clicked **No** at Windows's
  prompt, or your account isn't an administrator. Try again from an administrator account.
- **"Quit rekordbox first."** rekordbox, or its installer, is still running. Check the taskbar
  and the system tray.
- **Stems Cache's light is yellow with "its drive isn't connected".** Connect the drive you chose in
  Settings, or choose **Use This PC**.
- **Demucs v4 is slow (about 3x real time).** That is the CPU speed. For an NVIDIA GPU see
  [Faster on an NVIDIA GPU (CUDA)](#faster-on-an-nvidia-gpu-cuda).
- **`bridge.log` says "CUDA: no CUDA ONNX Runtime installed" or "no NVIDIA GPU".** Nothing is
  wrong: Demucs v4 runs on the CPU. Install the CUDA runtime, or ignore it.
- **`bridge.log` says "CUDA: ... is ONNX Runtime x, not 1.18.x" or "cannot load".** The files in
  `ort-cuda` are the wrong version or incomplete (for example cuDNN 9 instead of 8). Use the exact
  versions listed above.
- **`bridge.log` says "real library ...: can be changed by others than administrators".** Install
  the CUDA files with `windows-install-cuda.ps1` from an administrator PowerShell, not by copying.
- **`bridge.log` says "CUDA run failed" or "took over its deadline".** That model now runs on the
  CPU until rekordbox restarts. Send the log if it keeps happening.
- **rekordbox closes when it opens a track, with CUDA installed.** Windows's event log names the
  module (`cudnn64_8.dll` meant a missing cuDNN part). Set `cuda=0` in `config.ini` and send the log.
- **The Prepare tab lists no playlists, or says rekordbox's playlists can't be read.** rekordbox must be
  installed and have been opened once. The reason is in the log on the **Manage** tab.
- **A prepared playlist says "1 track to prepare" on the Status tab but the Prepare tab says it is skipped.**
  The Status tab's check doesn't open the files, so a track that can't be prepared counts as not prepared yet.
- **Preparing stopped, or the app closed.** Open `%LOCALAPPDATA%\rbstemsplus\Logs\app.log`: it has each track's
  result, how the helper ended, and any error with its details. Send it with a report.
- **Making a problem report.**
  1. Choose *Help › Create Report*. It saves a zip on your Desktop and can open a new GitHub
     issue.
  2. Drag the zip into the issue.

  The report holds RB Stems Plus's logs and settings and what it found on your PC. It never
  includes your library, your music or track names, and your user name is replaced.

## For developers

How the Windows port works is in [`docs/windows-port/`](windows-port/): the plan, the status, and
the findings from rekordbox's Windows files. Building needs Linux or macOS with mingw-w64 and
Microsoft's .NET 8 SDK:

- the bridge:

  ```bash
  bridge/win/build.sh
  ```

- the bridge's tests (under wine on Linux):

  ```bash
  bridge/win/tests/run.sh
  ```

- the app's tests:

  ```bash
  dotnet test windows/RBStemsPlus.Tests
  ```

- every Windows release asset into `dist/`:

  ```bash
  scripts/build-windows.sh
  ```

- a test build that downloads from a local server and trusts only the test key:

  ```bash
  scripts/build-windows.sh --test http://127.0.0.1:8765/
  ```

- the benchmark tool, `rbsp-bench.exe` (built into `dist/` and attached to every prerelease): runs
  a Demucs model outside rekordbox with timing, video memory and node placement.

  ```powershell
  rbsp-bench --model hdemucs.onnx --cpu --threads 12                  # the CPU
  rbsp-bench --model hdemucs.onnx --dml 0                             # DirectML on adapter 0
  rbsp-bench --model hdemucs.onnx --cuda --ort C:\dj\ort-cuda\onnxruntime.dll `
             --cuda-opt arena_extend_strategy=kSameAsRequested --cuda-opt gpu_mem_limit=5368709120
  ```

  Add `--timeout S` so a stalled run ends; the options are listed by `rbsp-bench` alone.
- model variants for DirectML experiments (fp16, fixed shapes):
  `scripts/gpu-experiments/make_variants.py MODEL.onnx OUT_DIR`. They did not help: DirectML
  allocates every intermediate tensor in fp32 up front.
- installing the bridge into rekordbox without the UAC prompt (an already-elevated shell):

  ```powershell
  RBStemsPlus.exe --elevated install-cache "C:\Program Files\rekordbox\rekordbox 7.2.14" 7.2.14.0323 <bridge file> <its sha256>
  ```

`.github/workflows/windows.yml` runs all of these, plus the install command and the ACL checks on
a real Windows runner. The findings behind the GPU work are in
[`windows-port/GPU.md`](windows-port/GPU.md) and [`windows-port/GPU-BENCH.md`](windows-port/GPU-BENCH.md).
