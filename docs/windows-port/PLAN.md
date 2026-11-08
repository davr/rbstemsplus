# Port RB Stems Plus to Windows

## Context
RB Stems Plus is macOS-only. It has two parts:
- **The bridge** (`bridge/rbstems_bridge.c`, 2.8k lines of C). It stands in for rekordbox's `libonnxruntime.1.18.0.dylib` and caches Demucs stems as FLAC. Every other call goes to Pioneer's real ORT.
- **The app** (`app/Sources/*.swift`, about 6k lines of Swift/AppKit). It does the Demucs v4 model swap, installs the bridge as root (re-signing rekordbox.app), and covers the watcher, settings, external cache drive, self-update, signed payloads and reports.

The user wants **full app parity on Windows**:
- The app is a **C# .NET 8 WinForms** program.
- The bridge is a **separate Windows fork** (`bridge/win/`), so the macOS bridge is untouched.
- The macOS app, scripts and release flow keep working unchanged. Windows assets are added next to them.

**Unknowns.** Neither the repo nor the web says where rekordbox for Windows puts `onnxruntime.dll`, which functions it exports or imports (CPU only? DirectML?), or which models it ships. One fact is confirmed: rekordbox's settings folder on Windows is `%APPDATA%\Pioneer\rekordbox6`, the counterpart of macOS's `~/Library/Application Support/Pioneer/rekordbox6`. So the work starts with a probe the user runs on a real Windows PC. Every path is discovered at runtime, with assumed fallbacks, and the export list is a generated `.def`, so the probe results only change data, not design.

Sources: [Pioneer forum, rekordbox6 AppData folder](https://forums.pioneerdj.com/hc/en-us/community/posts/900002288626-Rekordbox-6-move-database-file-location-on-same-machine), [DJ.Studio, rekordbox folders on Windows](https://help.dj.studio/en/articles/8365382-locating-integration-folders).

## Layout (all new, under `windows/`, `bridge/win/` and `scripts/`)
```
bridge/win/rbstems_bridge_win.c   fork of the bridge, ported to Win32
bridge/win/exports.def            exports (OrtGetApiBase, ..._CPU, rbstems_marker, + probe-found extras)
bridge/win/build.sh               mingw-w64 cross build (+ static libFLAC 1.5.0, same sha256 pin)
bridge/win/tests/                 harness port (harness.c/fake_ort.c reused via #ifdef where trivial)
windows/RBStemsPlus/              WinForms app (net8.0-windows, single-file self-contained, win-x64)
windows/RBStemsPlus.Tests/        xUnit tests
scripts/windows-probe.ps1         discovery script the user runs first
scripts/bootstrap.ps1             `irm …/bootstrap.ps1 | iex` installer
scripts/build-windows.sh          bridge + dotnet publish + zip + payload-windows.json
.github/workflows/windows.yml     CI on windows-latest
docs/windows.md                   install / troubleshooting for Windows
```

## Phase 0: discovery probe (`scripts/windows-probe.ps1`)
This is a read-only PowerShell script that prints a report for the user to paste back:
- **rekordbox install:** from the `HKLM\…\Uninstall\*` keys (InstallLocation, DisplayVersion), with a fallback scan of `C:\Program Files\Pioneer\rekordbox 7*`. It reports `rekordbox.exe`'s FileVersion and Authenticode signer.
- **ORT DLLs:** every `onnxruntime*.dll` and `DirectML.dll` under the install, with each one's version resource, SHA-256, Authenticode signer and **export table** (parsed from the PE).
- **rekordbox.exe imports:** its import table entries for onnxruntime, normal and delay-load.
- **Model folder:** the contents of `%APPDATA%\Pioneer\rekordbox6\models\` (and `%LOCALAPPDATA%`/`%PROGRAMDATA%` Pioneer as fallbacks), with each file's SHA-256 and `demucs3_ver.txt`.

The results fill three things: `exports.def`, the default paths in `Paths.cs`, and the initial Windows compat list (rekordbox versions, ORT prefix, STEMS Engine, Pioneer model sha). The Windows Pioneer model sha may equal `PIONEER_0002`. The app also gets a `--probe` flag that runs the same discovery, for reports.

## Phase 1: Windows bridge (`bridge/win/rbstems_bridge_win.c`)
Copy the macOS bridge and port it, keeping the cache format, the matching logic, the config grammar, the marker, `bridge-state` and the log identical.

| macOS | Windows replacement |
|---|---|
| libdispatch queues (`writer`, `maint`), `dispatch_after`, `dispatch_apply`, blocks `^{}` | A small serial work queue (CreateThread + SRWLOCK + CONDITION_VARIABLE) with delayed items. A parallel `for` loop over the Win32 thread pool. Blocks become callback + `void *ctx` (e.g. `each_name`) |
| pthread mutex / once | SRWLOCK / `InitOnceExecuteOnce` (no winpthreads DLL dependency) |
| CommonCrypto SHA-256 | BCrypt (`BCryptHash`, SHA256) |
| Accelerate `vDSP_fft_zrip` inverse (`rebuild_spec`, :2229) | An in-file radix-2 real inverse FFT, N=4096, scaled to match vDSP's convention (the comment at :2242). Tested against numpy `irfft` |
| `dlopen(REAL_PATH)` | `LoadLibraryExW(%ProgramData%\rbstemsplus\ort\onnxruntime.dll, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR\|SEARCH_DEFAULT_DIRS)`. The rekordbox folder stays on the search path for DirectML.dll |
| `only_root_can_change` (:859) | DACL check: the owner is Administrators, SYSTEM or TrustedInstaller, and no ACE gives write access to any other SID |
| `home_dir` from the user database | `SHGetKnownFolderPath` (RoamingAppData for config, LocalAppData for logs and the cache), never environment variables |
| `openat`/`unlinkat`/`fstatat`/`renameat`/`fdopendir`/`futimes`, `O_NOFOLLOW` | UTF-8 paths, turned into wide strings at a small `w_*` helper layer. Reparse points (symlinks, junctions) are refused on every component. Before each batch the base folder is pinned by volume serial + file ID (`GetFileInformationByHandleEx`), the counterpart of the dev/ino pin. Deletes use `FileDispositionInfoEx` on an opened handle. Replace-renames use `MoveFileExW(REPLACE_EXISTING)` |
| `statfs`/`getattrlist`/CFURL free space | `GetVolumeInformationByHandleW`, `GetDriveTypeW`, `GetDiskFreeSpaceExW`. The free floor rule stays the same: 10% of the disk, at most 50 GB |
| `sysctl` process start | `GetProcessTimes` |
| `newlocale`/`strtod_l` | `_create_locale`/`_strtod_l` |
| model path check `"/hdemucs.onnx"` (:2364) | `ORTCHAR_T` is `wchar_t`: a case-insensitive suffix match on `\hdemucs.onnx` or `/hdemucs.onnx`. The model is hashed with `_wfopen` |
| `._` exFAT companion accounting | dropped (macOS-only) |
| exports `_CoreML` | `.def`: `OrtGetApiBase`, `OrtSessionOptionsAppendExecutionProvider_CPU` and `rbstems_marker`. Any other export the probe finds (e.g. `_DML`) forwards to the real DLL through a generic x64 `jmp [ptr]` thunk |

Paths:
- The cache is `%LOCALAPPDATA%\rbstemsplus\Cache\<model>\<xx>\<key>.flac|.desc`.
- Logs and `bridge-state` go in `%LOCALAPPDATA%\rbstemsplus\Logs`.
- Config is `%APPDATA%\rbstemsplus\config.ini`.
- A chosen location is `<drive>\RB Stems Plus Cache\`, with the same `.rbstemsplus-cache` marker.

`bridge/win/build.sh` uses `x86_64-w64-mingw32-gcc -shared -O2 -static-libgcc`, with libFLAC configured for `--host=x86_64-w64-mingw32`. It then checks the build the way `bridge/build.sh` does:
- the exports match `exports.def` exactly (`x86_64-w64-mingw32-objdump -p`);
- the only imports are system DLLs (kernel32, bcrypt, advapi32, shell32, ole32, ucrt/msvcrt);
- the marker and caps strings are present;
- a release build contains no test path.

## Phase 2: Windows app core (`windows/RBStemsPlus/`, no UI yet)
Each Swift module gets a C# counterpart. The Swift files are the reference for the logic and the messages:
- `Paths.cs` ← `Paths.swift`:
  - per-user data in `%APPDATA%\rbstemsplus\`: `installed`, `dismissed`, `request`, `busy`, `config.ini`, `originals\`, `payload\`, `update\`, `payload-version`, `location-hint`;
  - logs and the default cache in `%LOCALAPPDATA%\rbstemsplus\`;
  - the admin-only folder `%ProgramData%\rbstemsplus\` with `ort\` and `pioneer\<ver>\`;
  - the app installed per user in `%LOCALAPPDATA%\Programs\RB Stems Plus\`.
- `Rekordbox.cs`: discovery (registry, then scan), the version from FileVersionInfo, the `onnxruntime.dll` version resource, Authenticode via `WinVerifyTrust` plus a signer-name check, bridge detection by the `"rbstems bridge: "` string, and caps parsing (same as `Rekordbox.swift:14-51`). It also finds the model folder and reads `demucs3_ver.txt`.
- `Config.cs` ← `Config.swift`: the same grammar and writer, tested against `tests/config-fixtures.txt`. The `cache_dir` rule becomes a Windows absolute path whose last component is `RB Stems Plus Cache`, not the root of a drive.
- `Payload.cs`, `Signing.cs` ← `Payload.swift`, `Signing.swift`:
  - the app reads `payload-windows.json` (same schema, with `bridge.file = onnxruntime.dll` and `app.zip = rbstemsplus-windows.zip`) and its `.sig`;
  - signatures are checked with `ECDsa.ImportSubjectPublicKeyInfo` + `VerifyData(..., DSASignatureFormat.Rfc3279DerSequence)`, using the same keys in `keys/`, compiled in by the build script;
  - it keeps the rollback floor, duplicate-key rejection, and resumable sha-checked downloads (`HttpClient`, HTTPS only).
- `Compat.cs` ← `Compat.swift`: version lists from the payload, falling back to built-in lists. Being an admin isn't required, because UAC asks.
- `Models.cs` ← `Models.swift`: the Demucs v4 swap runs as the user: save the original to `originals\<sha>.onnx` + `.engine`, copy to a `.hdemucs.new` temp file, check its sha, flush, then `File.Replace`/`MoveFileEx`. Restore picks the best original the same way.
- `State.cs`, `Location.cs` ← `State.swift`, `Location.swift`:
  - `bridge-state` is parsed with the same pid + start-time check;
  - external drives are allowed when `DriveType == Removable`, or Fixed with a USB/1394/SD bus type (`IOCTL_STORAGE_QUERY_PROPERTY`);
  - network, system and boot volumes are refused; NTFS, exFAT and FAT32 are allowed.

## Phase 3: elevated operations (replace `Scripts.swift` + `Admin.swift`)
- Stems Cache install, uninstall and update relaunch the same exe with `--elevated <op> <args>` through `ShellExecute` `runas`, so UAC appears instead of a password sheet. The exit codes mirror `RootExit` (`Scripts.swift:30-40`).
- The elevated path, the counterpart of the root-script header (`Scripts.swift:70-272`):
  1. Take the `Global\rbstemsplus` mutex.
  2. Refuse if rekordbox.exe or Pioneer's installer is running.
  3. Check the version and signer against the arguments.
  4. Back up Pioneer's `onnxruntime.dll` to `pioneer\<ver>\` with `SHA256SUMS`, and copy it to `ort\`.
  5. Set explicit ACLs on `%ProgramData%\rbstemsplus`: Administrators and SYSTEM full control, Users read and execute, inheritance off.
  6. Put the bridge in place as `onnxruntime.dll.rbsp-new`, verify it, then swap it in with `MoveFileEx`. Any failure rolls back.
- No re-signing is needed: Windows doesn't check the signature of DLLs a normal app loads. Phase 0 and the first real test must confirm rekordbox has no integrity check of its own.
- Uninstall restores Pioneer's DLL byte for byte, checks it against `SHA256SUMS` and the Authenticode signature, then removes `%ProgramData%\rbstemsplus`.

## Phase 4: UI (WinForms)
- **MainForm** ← `Controller.swift`: two status lights (green, red or yellow, with tooltips), the action buttons on the left, the log on the right, and a status line under the buttons. Menus: Settings, Update RB Stems Plus, Help (Troubleshooting, Create Report).
- **Dialogs** use .NET 8 `TaskDialog`. The text follows the macOS alerts (`Sheet.swift`, `Actions.swift`), with Windows wording (UAC instead of password and App Management).
- **SettingsForm** ← `Settings.swift`/`LocationActions.swift`: the size limit, the days limit, Clear Saved Stems, and the cache location (Change… / Use This PC).
- **LocationMonitor** listens for `WM_DEVICECHANGE` and watches `bridge-state` with a `FileSystemWatcher`, and keeps the 10-minute size refresh.

## Phase 5: watcher, update, report
- **Watcher** ← `Watcher.swift`:
  - `RBStemsPlus.exe --watch` starts from `HKCU\…\Run`;
  - it detects being turned off through `StartupApproved\Run`, to show "Reinstall reminders are off";
  - it watches the rekordbox install folder's parent and the model folder with `FileSystemWatcher`, and runs the same decision logic: wait for the installer, then for rekordbox to close, then compare `installed` with what is present, then show the dialog;
  - "Reinstall Now" writes `request` and starts the app;
  - a single instance is enforced by a named mutex.
- **Update** ← `Update.swift`: download the zip and check its sha, extract to `update\`, check the version, then after exit rename the running exe to `.old`, move the new one in, relaunch, and delete `.old` at the next start.
- **Report** ← `Report.swift`: a zip on the Desktop with the logs, config, versions and `--probe` output, plus rekordbox's recent logs and WER crash reports, with the user name redacted. The Open GitHub Issue button opens the browser.

## Phase 6: release pipeline, bootstrap, CI, docs
- **`scripts/build-windows.sh`** works on macOS or Linux with mingw-w64 and dotnet-sdk-8. It runs `bridge/win/build.sh`, then `dotnet publish -r win-x64 --self-contained -p:PublishSingleFile=true`, and zips the result into `dist/rbstemsplus-windows.zip`. It also writes `dist/onnxruntime.dll` and `dist/payload-windows.json`, and adds them to `SHA256SUMS`.
- **`scripts/release.sh`** also signs `payload-windows.json` the same way it signs `payload.json` (openssl, :332-342) and uploads `bootstrap.ps1`. The model asset `stemsplus-model.onnx` is shared.
- **`scripts/bootstrap.ps1`** is the counterpart of `bootstrap.sh`:
  - it runs on Windows PowerShell 5.1;
  - it checks the signature with the keys embedded: the SPKI is turned into a CNG `ECCPublicBlob` and the DER signature into r‖s for `ECDsaCng`;
  - it checks each file's sha, installs to `%LOCALAPPDATA%\Programs\RB Stems Plus`, adds a Start menu shortcut, and opens the app.
- **`.github/workflows/windows.yml`** runs on windows-latest with MSYS2 mingw:
  - it builds the bridge and the app, and runs the xUnit tests;
  - it runs the bridge harness against `fake_ort` and against a stock **onnxruntime 1.18.x win-x64** DLL from microsoft/onnxruntime releases, with models from `bridge/tests/make_model.py`. The bridge only checks the `"1.18."` version string, so the cache, rebuild, space and memory tests run there for real.
- **Docs:**
  - `README.md` gets a "Windows" section (install command, UAC instead of password, no macOS permission steps), and the FAQ answer "Mac only" changes;
  - a new `docs/windows.md` covers troubleshooting;
  - `docs/RELEASING.md` gets the Windows steps;
  - `Makefile` gets `windows`, `windows-test` and `bridge-win` targets.

## Verification
1. **In this container:**
   - install `mingw-w64`, `dotnet-sdk-8.0` and `wine64` from Ubuntu's apt;
   - `bridge/win/build.sh` builds, and the export and import checks pass;
   - `dotnet build` + `dotnet test` (config and marker fixtures, payload and signature with a test key, compat, bridge-state parsing, Models swap on temp folders);
   - the bridge harness under wine with `fake_ort`, plus a stock ORT 1.18 DLL if the network allows;
   - `make check` and the existing macOS files are unchanged (`git diff --stat app/ bridge/*.c bridge/build.sh`).
2. **CI:** `windows.yml` is green on windows-latest, with no harness test skipped.
3. **On the user's Windows PC (needed, I can't do it):**
   - run `windows-probe.ps1` and send the output, which fills `exports.def` and the compat lists;
   - then a `build-windows.sh --test URL` build: install Demucs v4, install Stems Cache (UAC), load a track with STEMS twice, and check that the second load is fast and `bridge.log` shows a hit;
   - update rekordbox and check the watcher prompts;
   - uninstall and check that Pioneer's `onnxruntime.dll` and model come back byte for byte.
4. **Commit and push:** each phase as its own commit on `claude/affectionate-ride-f082bb`.
