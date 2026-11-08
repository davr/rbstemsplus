# Windows port: status

> **Current state (2026-11-08, paused at the user's request).** Read "Where things stand" first.

## Where things stand
- **It works on the user's PC** (rekordbox 7.2.14, Windows 11): Demucs v4 and Stems Cache install, cache hits are served, and uninstall and reinstall work.
- **CI is green** on Linux and native Windows: the bridge suite (119 passed natively, ACL tests included), 110 app tests, and the `bootstrap.ps1` install test.
- **Latest test release:** `windows-test-4` on davr/rbstemsplus (commit 51cc402). It adds progress bars, the Update Stems Cache button, the log-spam fix and the bridge's provider and speed lines. Install it with `irm https://github.com/davr/rbstemsplus/releases/download/windows-test-4/bootstrap.ps1 | iex`.
  - To publish another one: put a new tag in `.github/windows-prerelease.tag` and push.
- **Committed after windows-test-4, not yet in a release:**
  - 23f1376: the app shows only the admin-log lines from the current elevated step.
- **windows-test-6 (this branch, `claude/affectionate-ride-f082bb`):**
  - `gpu_v4=0` default (Demucs v4 stays on the CPU unless asked), the Settings checkbox, tests;
  - per-chunk timing lines in `bridge.log` (hook, model, bridge, rekordbox's time between chunks);
  - a watchdog with a CPU twin for GPU runs, and one shared GPU session per model;
  - `rbsp-bench.exe` ([GPU-BENCH.md](GPU-BENCH.md)); the bridge suite has 126 tests;
  - results of windows-test-5 and the reasons are in [GPU.md](GPU.md). The fixed-shape model stays on `claude/dml-model`.
- **In progress: the GPU override** (the user asked for it, on by default). See [GPU.md](GPU.md).
  - **Done, bridge side** (`bridge/win/rbstems_bridge_win.c`):
    - a `gpu` key in config.ini, default 1;
    - `pick_adapter()` takes the DXGI adapter with the most dedicated video memory, skipping software adapters;
    - `add_dml()` sets sequential execution and adds DirectML with Pioneer's `…_DML` export;
    - `create_demucs()` applies this to every Demucs session (ours and rekordbox's), with the memory pattern off, and falls back to the CPU on any failure.
    - The "Demucs session opened" line says GPU (with the adapter name) or CPU, and why.
  - **Tested only partly:** it builds with no warnings, and 118 pass under wine with Microsoft's ORT, whose missing DirectML export means the CPU fallback path.
  - **Not tested yet:**
    - under wine with Pioneer's DirectML DLL (that run was stopped by the user);
    - on a real GPU (only the user's PC can do that).
  - **Still to do:**
    1. ~~The app side~~ done: `Config.Gpu` (default true), `ConfigFile.WriteGpu`, a Settings checkbox, `gpu=` in the probe text, a Core test (111 pass).
    2. Still open: a bridge test for `gpu=0` ("on the CPU (gpu=0 in config.ini)").
    3. Still open: run the suite with Pioneer's DLL (`RBSTEMS_WIN_ORT=<folder with Pioneer's onnxruntime.dll and DirectML.dll>`), expecting the fallback under wine.
    4. Docs: GPU.md and `docs/windows.md` mention `gpu=`.
    5. `windows-test-5` is published by the tag file. The user then updates Stems Cache with the button, loads a new track, and sends `bridge.log`, which shows the GPU line and "the model took … s" per chunk.
    6. If DirectML is slow or refuses our model, that is case B in GPU.md: a benchmark tool and a DirectML-friendly export.

This is the working list for the Windows port. The approved design is in [PLAN.md](PLAN.md), and the facts learned along the way are in [NOTES.md](NOTES.md). The list is updated as work moves forward.

Branch: `claude/affectionate-ride-f082bb`.

Legend: [x] done · [~] in progress · [ ] to do · [!] blocked or needs the user

## Phase 0: discovery
- [x] `scripts/windows-probe.ps1`: a read-only report covering the items below. Written and pushed. PowerShell couldn't be installed here (the sandbox denied it), so the script was only reviewed by reading it for PS 5.1 pitfalls, never run.
  - the rekordbox install and its version;
  - the ORT DLLs, with each one's exports, sha256 and signer;
  - rekordbox.exe's imports;
  - the model folder and its hashes.
- [x] First probe run received (rekordbox 7.2.14); findings are in NOTES.md.
- [x] Probe updated: reads ORT's version string from the DLL, scans `Demucs3.dll`/`Spleeter.dll` strings, and fixes the process-modules error.
- [x] Second probe run: ORT **1.18.0**, and `Demucs3.dll` opens `<dir>/hdemucs.onnx` with CreateSession. **Phase 0 is done.** That output fills `exports.def`, the default paths and the compat lists.

## Phase 1: Windows bridge (`bridge/win/`): done
- [x] `win_compat.h`: the Win32 stand-ins.
- [x] `rbstems_bridge_win.c`: fully ported. It compiles with no warnings under mingw-w64 gcc 13.
- [x] `exports.def`: Pioneer's ordinals (@1 `OrtGetApiBase`, @2 `Ex_DML`, @3 `_CPU`, @4 `_DML`) and @5 `rbstems_marker` (DATA).
- [x] `build.sh` has three modes: a release build, `--test REAL_PATH OUT` and `--unit OUT`. It checks:
  - the exports, by name and ordinal;
  - the imports (system DLLs only);
  - the marker and caps strings;
  - that a release build has no test hooks.
- [x] `tests/harness.c` and `tests/fake_ort.c` (Windows ports), `tests/unit.c`, and `tests/run.sh` (wine on Linux, native in Git Bash). The suite covers:
  - SHA-256 vectors, and the FFT against a direct DFT;
  - the 70 Windows config fixtures and the marker fixtures;
  - missing, passthrough and load, with ordinal checks;
  - exact hits, and near hits on and off;
  - disabled, and the free-space floor;
  - ageing out old entries;
  - a chosen location with and without its marker, and bridge-state;
  - the rebuilt-model cases (good, off the list, rekordbox_model=0, bad, loud, nan, frames);
  - the ACL refusal (Windows only).
- [x] **117 passed, 0 failed** under wine against Microsoft's ORT 1.18.1 **and against Pioneer's own `onnxruntime.dll` (1.18.0, DirectML)**, from the user's zip. The one skip is the ACL test, which needs real Windows.
- [x] `tests/config-fixtures-windows.txt`, generated by `tests/make_windows_fixtures.py` from the macOS fixtures, plus 12 Windows-only cases.
- [ ] Not yet ported from the macOS suite: `bridge/tests/fuzz.c` (fuzz targets), and the disk-image location tests (`location.sh`: drive unplugged, read-only, recheck). They need a Windows CI runner (VHD attach) or a real PC.

## Phase 2: app core (C# .NET 8, `windows/`)
- [x] Layout:
  - `windows/RBStemsPlus.Core/` targets net8.0: the logic, testable on Linux, with Windows-only calls guarded;
  - `windows/RBStemsPlus/` targets net8.0-windows: WinForms, single-file, self-contained, win-x64;
  - `windows/RBStemsPlus.Tests/` uses xUnit and runs on Linux with `dotnet test`.
- [x] Core written and building with no warnings. **108 xUnit tests pass on Linux**, including all 70 Windows config fixtures and the marker fixtures, which agree with the bridge. The files:
  - `Paths`, `Config` (the grammar, reader and writer), `Payload` (Manifest, PlainJsonKeys, floor, fetch, assets), `Signing` (ECDSA P-256 DER);
  - `Compat`, `Rekordbox` (discovery, versions, Authenticode, bridge marker and caps, ORT version string), `Models` (Demucs v4 swap);
  - `State` (installed, dismissed, request, busy, bridge-state, floor), `Location` (marker, path rules, sizes, external drive), `Elevated` (cache install and uninstall as admin).
- Swift references: `app/Sources/Config.swift`, `Payload.swift`, `Signing.swift`, `Compat.swift`, `Rekordbox.swift`, `Models.swift`, `State.swift`, `Location.swift`.
- NuGet (xUnit) works from this container; `dotnet test` runs here.

## Phases 3 to 5: elevated operations, UI, watcher, update, report (`windows/RBStemsPlus/`)
- [x] `Program.cs` runs four ways: the app, `--watch`, `--elevated <op>` and `--probe`.
- [x] `Engine.cs` installs, uninstalls and reinstalls the two features, reinstalls what went missing, and does the complete uninstall.
  - **Order:** fetch the payload (or use the saved one offline), download and verify the assets, wait for rekordbox to close, then change things.
  - **Elevation:** UAC through `Verb=runas`; a declined prompt (1223) maps to `NotAdmin`.
  - **Watcher:** `WatcherRegistration` uses HKCU Run, detects being turned off through `StartupApproved\Run`, and asks the watcher to quit through a named event.
- [x] `MainForm.cs`: the lights (green, red or yellow, with tips), the buttons, the status line, the log, the menus, and TaskDialog confirmations. A second launch brings the window forward through a "show" file.
- [x] `SettingsForm.cs`: max GB and days, Change… (a folder on an external drive, checked and given a marker), Use This PC, Clear Saved Stems.
- [x] `Watcher.cs`: FileSystemWatcher on `Program Files\rekordbox` and the models folder; it waits for the installer and for rekordbox to close; three choices (Reinstall Now, Remind Me Later, Don't Ask Again).
- [x] `Updater.cs`: daily check, zip sha and version check, rename the running exe to `.old`, copy the new one in, restart. It also handles the self-delete at the end of the complete uninstall.
- [x] `Report.cs`: a zip on the Desktop with the probe, logs, config and crash names, the user name replaced; it also opens a prefilled GitHub issue.
- [x] **Verified under wine** with Pioneer's real `onnxruntime.dll`:
  - `--probe` finds rekordbox 7.2.14.0323, ORT 1.18.0, signed by AlphaTheta;
  - `--elevated install-cache` backs up Pioneer's DLL with `SHA256SUMS`, makes the ORT copy and puts the bridge in place;
  - `--elevated uninstall-cache` puts Pioneer's DLL back byte for byte and removes `ProgramData\rbstemsplus`.
- [!] **Not verifiable under wine:**
  - the ACLs (wine doesn't store them here, so the release bridge, correctly, refuses the real library under wine);
  - the UI (no display, and wine can't run the single-file bundle).
  - Both need Windows: CI (windows-latest) or the user's PC.
- [x] Fixed: `%ProgramData%\rbstemsplus` pre-created by another account (a junction, or planted files) is removed and made again before the first install (`Elevated.ReclaimRoot`).
- [ ] Not ported (smaller macOS features):
  - the "leftover action" resume prompt (it is only logged);
  - offering to move stems between locations;
  - the Stems Cache update prompt when the payload's bridge is newer than the installed one;
  - LocationMonitor's drive-change events (the window refreshes every 5 s instead).

## Phase 6: release, bootstrap, CI, docs
- [x] `scripts/build-windows.sh [--test URL]` runs on Linux or macOS.
  - It makes `rbstemsplus-windows.zip` (66 MB), `onnxruntime.dll`, `bootstrap.ps1` (keys written in), `payload-windows.json` and `SHA256SUMS-windows`.
  - The model comes from `dist/`, from `RBSTEMSPLUS_MODEL`, or is downloaded (resumably) from the published release and checked by sha256.
  - A test build signs with `build/test-keys`. **A local test build worked**, and the C# test verifies its openssl signature.
- [x] `scripts/bootstrap.ps1`, the `irm … | iex` installer, works on PS 5.1 and 7.
  - It checks the ECDSA signature itself: SPKI to a CNG EccPublicBlob, DER to r‖s, then ECDsaCng.
  - It refuses duplicate JSON keys and payloads older than its minimum, checks the zip's sha256, installs to `%LOCALAPPDATA%\Programs\RB Stems Plus`, adds a Start menu shortcut, and starts the app.
  - **Not run here** (no PowerShell); the CI windows job runs it against the served test build.
- [x] `scripts/release.sh --windows` builds, checks, signs (same key) and uploads the Windows assets with the macOS release. Without `--windows`, nothing changes.
- [x] `.github/workflows/windows.yml`:
  - **Linux job:** the bridge tests under wine, `dotnet test`, a test build, and the artifact upload.
  - **Windows job:** the bridge tests natively in MSYS2 (with the ACL test), `dotnet test` (with the ACL test of `SecureRoot`), and `bootstrap.ps1` against the test build served on localhost.
  - **Not run yet:** this session can't trigger GitHub Actions; it runs on push.
- [x] Docs: `docs/windows.md` (install, what changes, lights, updates, uninstall, troubleshooting, building), the README intro and FAQ, `docs/RELEASING.md`, and the Makefile targets `windows`, `bridge-win` and `windows-test`.

## Test releases (prereleases on davr/rbstemsplus)
- `.github/workflows/windows-prerelease.yml` builds a **test build** whose app and `bootstrap.ps1` download from the prerelease itself, signed with a key made fresh on the runner, and publishes it.
- **How to start it:** change `.github/windows-prerelease.tag` (one line, the new tag, e.g. `windows-test-2`) and push the branch.
  - This session can't push tags (the git proxy refuses), and `workflow_dispatch` only works once the workflow file is on the default branch.
  - GitHub creates the tag on the built commit.
- The first one is `windows-test-1`, started at 4e539e3.
- **CI `windows` job, native Windows (MSYS2):**
  - objdump's text output differs between binutils versions, so `build.sh` now reads the PE tables itself (`bridge/win/pe_tables.py`);
  - real ACLs made the test bridge refuse MSYS2's temp folder, so `run.sh` now locks the test folders as the installer does;
  - MSYS2 rewrote `icacls /grant` into a path, so `icacls` now runs with `MSYS2_ARG_CONV_EXCL`;
  - result: **115 of 118 pass natively, the real ACL refusal test included.** The 3 left are the chosen-location tests, still being diagnosed; `run.sh` now prints the end of `bridge.log` on a failure.
- **Progress display:** `bootstrap.ps1` shows numbered steps (1 to 7) and Write-Progress bars with MB, percent and speed. The app shows a step line ("Step 2 of 3: …") and a progress bar: a percentage for downloads and the 300 MB model copies, a moving bar otherwise.
- **Prereleases:** `windows-test-1` (first) and `windows-test-2` (with the progress display).

## First real-PC test (the user, rekordbox 7.2.14, windows-test-1, 2026-11-08): it works
- **The install:**
  - Demucs v4 installed: Pioneer's model saved, ours in place;
  - Stems Cache installed through UAC: Pioneer's DLL saved, the bridge in place;
  - `rekordbox_model=1` was written and the watcher registered.
- **rekordbox ran with the bridge.** The release bridge loaded Pioneer's ORT 1.18.0 from `C:\ProgramData\rbstemsplus\ort` (the real ACL check passed) and opened our model's sessions with memory pattern off.
- **The cache:**
  - track A missed and was cached; track B missed;
  - back on track A, all 30 cached chunks came back as hits, about 0.5 s each, and the rest were separated;
  - there were no errors.
- **Slow separation:** about 13 s per 11 s chunk, about 0.85x real time, apparently on the CPU. Analysis and plan are in [GPU.md](GPU.md). `windows-test-3` logs the providers and the speed.
- **Bug found after the test:** on real Windows `ReOpenFile` refuses the bridge's handles. It is used for the hit's "last used" stamp, so the LRU and the 60-day cleanup didn't see use, and for the chosen location's write check. Fixed with `rb_reopen` (reopen by final path, then check it is the same file), plus a test that a hit refreshes the entry's time. The user needs `windows-test-3`, then Uninstall and Install Stems Cache, for the fixed bridge: the app doesn't yet offer bridge updates.

## Second real-PC run (windows-test-3 app, 2026-11-08)
- After a restart, the cached track came back as 30 hits again.
- **Running `bootstrap.ps1` updates only the app, not the bridge in rekordbox's folder.** The log still showed test-1's bridge (b8bec9428c50: no provider or speed lines).
  - Fixed in windows-test-4: an **Update Stems Cache** button appears when the payload's bridge sha differs from the installed one (the Mac app's "Stems Cache update").
- **Log spam:** "saved payload-windows.json ignored", every 5 s. Each test build trusts only its own key, so test-1's saved payload fails test-3's check.
  - Fixed: logged once per reason, and the app fetches a fresh payload at launch whenever the saved one doesn't verify, not just once a day.

## Verification still owed
- [x] The bridge builds under mingw, and the export, ordinal and import checks pass.
- [x] The harness passes under wine with the fake ORT, Microsoft's ORT 1.18.1 and **Pioneer's ORT 1.18.0**: 117 passed.
- [x] `dotnet test` passes (110).
- [x] Elevated install and uninstall under wine, with Pioneer's DLL, restore byte for byte.
- [ ] The CI windows job: the native bridge tests with ACLs, the app's ACL test, and `bootstrap.ps1`.
- [!] **A real end-to-end test on the user's Windows PC with rekordbox 7.2.14:**
  - install both features (UAC for Stems Cache);
  - load a track with STEMS twice, and check that `%LOCALAPPDATA%\rbstemsplus\Logs\bridge.log` shows "hit";
  - check that rekordbox starts at all with the bridge in place (does rekordbox check its DLLs' integrity?);
  - uninstall completely, and check that Pioneer's files come back.
- [!] **Unknown until that test:** does rekordbox append the DirectML provider to the Demucs session, and does our Demucs v4 model run under DML? It falls back to CPU for unsupported ops.
