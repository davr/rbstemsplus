# Windows port: notes and findings

These are facts that matter for the port, recorded as they turn up. **Verified** means checked here or in a source. **Assumed** means it still needs the Windows probe or a real PC.

## The user's decisions
- Scope: **full app parity** on Windows.
- App: **C# .NET 8 WinForms**, a single self-contained exe for win-x64.
- Bridge: a **separate Windows fork** in `bridge/win/`. The macOS bridge (`bridge/rbstems_bridge.c`) and the rest of the macOS app stay untouched.

## rekordbox on Windows: probe results from the user's PC (2026-11-08)
Windows 11 Pro 26200, x64, PowerShell 5.1. The full report is not committed (it is the user's).

**Installs**
- Several versions are installed side by side: 6.7.4, 6.8.5, 7.0.4, 7.0.8 and **7.2.14** (the one in use).
- rekordbox 7 lives in **`C:\Program Files\rekordbox\rekordbox 7.x.y\`**; rekordbox 6 used `C:\Program Files\Pioneer\...`.
- The uninstall key is `HKLM\...\Uninstall\Pioneer rekordbox 7.2.14`. `DisplayName` is "rekordbox 7.2.14 64bit", `DisplayVersion` is `7.2.14.0323`, and the publisher is AlphaTheta. **`InstallLocation` is empty**: take the folder from `UninstallString` (`<dir>\Uninstall rekordbox.exe`).
- `HKLM\SOFTWARE\AlphaTheta\rekordbox7.2.14\Capabilities` exists, one key per version.
- `rekordbox.exe` reports FileVersion `7.2.14.0` and ProductVersion `7.2.14.0323`. It is Authenticode-signed by **CN=AlphaTheta Corporation**, thumbprint `A17B3657248EBBA9870D66890D90D8BF00EE22D8`. Its files are owned by BUILTIN\Administrators.
- While the probe ran, an installer named `Install_rekordbox_x64_7_2_14` was running. The watcher must wait for processes named `Install_rekordbox*` and `Upmgr rekordbox`.

**ONNX Runtime**
- `onnxruntime.dll` sits in the install folder, beside `rekordbox.exe`. Size 17,729,456, sha256 `6fd1738a02a71d40dbb5609166c96209473957313f4921b1170b61ad5c6f8e3a`, signed by AlphaTheta.
- Its version resource says only **"Internal Build"**, so the version must come from `GetVersionString` or from the standalone `1.x.y` string in the file. The stock ORT keeps `"1.18.1"` that way.
- It is a **DirectML build**. It delay-loads `DirectML.dll` (ordinal 2, `DMLCreateDevice1`), `d3d12`, `dxgi` and `dxcore`. `DirectML.dll` is 1.14.1, the DirectML that ORT 1.18 ships with, so ORT is very likely 1.18.x. The probe update reads the version string to confirm.
- **Exports, with their ordinals:**
  - `OrtGetApiBase` @1
  - `OrtSessionOptionsAppendExecutionProviderEx_DML` @2
  - `OrtSessionOptionsAppendExecutionProvider_CPU` @3
  - `OrtSessionOptionsAppendExecutionProvider_DML` @4
- **The importers bind by ordinal:** `rekordbox.exe` imports ordinals 4 and 1, and **`Demucs3.dll`** imports ordinals 1 and 4. The bridge's `.def` must give these four functions the same ordinals, and should not export other functions at low ordinals.
  - Signatures (ORT 1.18 `dml_provider_factory.h`):
    - `OrtSessionOptionsAppendExecutionProvider_DML(OrtSessionOptions*, int device_id)`
    - `OrtSessionOptionsAppendExecutionProviderEx_DML(OrtSessionOptions*, IDMLDevice*, ID3D12CommandQueue*)`
  - A pass-through with the same argument registers is fine: a void* for each pointer.
- `rekordbox.exe` also uses ORT for other models in `<install>\models\` (cue detection, embeddings). Those sessions pass through.

**STEMS**
- The STEMS separator is **`Demucs3.dll`** (429,488 bytes) in the install folder. `rekordbox.exe` names it as a string, so it probably loads it at run time. `Spleeter.dll` is the older separator.
- rekordbox also ships **TensorFlow** (`tensorflow_cc.dll`). The model folder has `stft_model\` and `istft_model\` (Keras SavedModels), so on Windows the STFT and iSTFT run in TensorFlow and the Demucs network runs in ORT.
- The model is at **`%APPDATA%\Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx`**, 335,027,281 bytes, sha256 **`435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a` = `PIONEER_0002`**, the same file as on macOS. `demucs3_ver.txt` holds `0002`, with a `demucs3_ver.bak` beside it. The same model means the same `mix`/`mag` to `x`/`xt` interface and the same rebuild maths.
- Downloads come from `https://dl.rekordbox.com/application/models/demucs3/` (`demucs3_xxxx.zip`) and `.../models/stems/`.

**Second probe run (2026-11-08)**
- **ORT is 1.18.0.** That is the standalone version string in Pioneer's `onnxruntime.dll`, so the bridge's 1.18 OrtApi layout applies.
- **How `Demucs3.dll` opens the model:**
  - It exports `demucs3Initialize`, `demucs3PreOperation`, `demucs3OperationAt{Index,Once}{OtherWithBass,SeparatedBass}`, `demucs3PostOperation`, `demucs3Finalize`, `demucs3CancelOperation` and `demucs3IsInitialized`.
  - Its strings include `"/hdemucs.onnx"` (a **forward slash**), `"mix"`, `"mag"`, `"xt"`, `"./stft_model"` and `"./istft_model"`. It imports `MultiByteToWideChar`.
  - So it builds `<model dir>/hdemucs.onnx`, widens it and calls **CreateSession by path**. The bridge matches the suffix `\hdemucs.onnx` or `/hdemucs.onnx`, ignoring case, and hashes the file. A CreateSessionFromArray hook isn't needed.
- `Spleeter.dll` is TensorFlow only (no ORT), so it is irrelevant.
- `Demucs3.dll` was built with MSVC (`...\Modules\Separator\Demucs3\build\win\x64\Release\Demucs3.pdb`). It imports ORT ordinals 1 and 4 only.

**Still unknown**
- Whether rekordbox checks the signatures of its own DLLs. Only a real test will show.
- Whether rekordbox appends the DML execution provider to the Demucs session. Our model may fall back to CPU for ops DML doesn't support.

**Compat list** for Windows to start with: rekordbox `7.2.14` (the user's version), ORT prefix `1.18.` (confirmed: 1.18.0), STEMS Engine `0002`.

## Stock ONNX Runtime 1.18.1 for win-x64 (verified)
- `onnxruntime-win-x64-1.18.1.zip` from microsoft/onnxruntime releases. A copy is in `build/ort/` (gitignored).
- `onnxruntime.dll` exports only `OrtGetApiBase` and `OrtSessionOptionsAppendExecutionProvider_CPU`.
- It ships `onnxruntime_providers_shared.dll` next to it.
- On Windows `ORTCHAR_T` is `wchar_t`, so the model paths given to CreateSession are UTF-16.
- The bridge only checks that the version string starts with `"1.18."`, so the stock DLL works for the tests.

## The macOS architecture (what is being ported)
Paths and flow are in the macOS sources:
- `Paths.swift`: every path;
- `Scripts.swift`: the root install and uninstall scripts, with exit codes in RootExit `:30-40`;
- `Models.swift`: the Demucs v4 swap, done as the user;
- `Compat.swift`: compatibility, from the payload lists or the built-in ones;
- `Payload.swift`: the signed `payload.json`, ECDSA P-256 DER over its exact bytes, keys in `keys/*.pub.pem`;
- `Watcher.swift`: a launchd agent;
- `Location.swift`: the external cache drive, identified by the `.rbstemsplus-cache` marker's UUID.

How the bridge behaves:
- It replaces 3 OrtApi entries: CreateSession, ReleaseSession and Run.
- It caches results only for `.../hdemucs.onnx` sessions whose output `x` is all zeros (our model), or for Pioneer models listed in `pioneer_models`, rebuilt, with `rekordbox_model=1`.

Shared contracts the Windows side must keep:
- the `config.ini` grammar (`tests/config-fixtures.txt`);
- the marker (`tests/marker-fixtures.txt`);
- the `bridge-state` keys (`pid`, `start`, `location`, `state`, `reason`);
- the `"rbstems bridge: "` marker string;
- the caps string `rbstems-cache: rebuild=1 location=1 pioneer=<sha>;`.

## Build environment (this container)
- Ubuntu 24.04. Installed with apt: `mingw-w64` (gcc 13), `dotnet-sdk-8.0` (8.0.133), `wine` (9.0, 64-bit works).
- `pip install onnx numpy` worked (used by `bridge/tests/make_model.py`).
- **downloads.xiph.org is blocked** by the proxy. The same libFLAC 1.5.0 tarball, with the same sha256 `f2c1c765…920`, comes from `https://github.com/xiph/flac/releases/download/1.5.0/flac-1.5.0.tar.xz`. `bridge/win/build.sh` uses that URL as its fallback.
- GitHub release downloads work.

## Design notes for the Windows bridge
- **Descriptors** are CRT fds over NT handles. Relative opens use NtCreateFile with RootDirectory, which is the openat counterpart.
- **No following links:** FILE_OPEN_REPARSE_POINT, and a reparse point is refused with ELOOP.
- **Sharing:** all share modes, so readers never block a delete or rename. Deletes use FileDispositionInformationEx with POSIX semantics, and renames use FileRenameInformationEx with REPLACE and POSIX; both fall back to the plain forms.
- **Directory listing** is `GetFileInformationByHandleEx(FileFullDirectoryInfo)`. It gives the attributes, mtime and allocation size without opening each entry.
- **Pinning:** `st_dev` is the volume serial and `st_ino` the 64-bit file index, the counterpart of macOS's dev/ino pin.
- **The vDSP FFT scale:** the inverse `zrip` followed by `ztoc` gives the plain Hermitian sum, with no 1/N. `rebuild_spec` multiplies by sqrt(N)/N. `rb_irfft` matches this.
- **Loading the real DLL:** `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | APPLICATION_DIR | SYSTEM32`, so the real ORT's own dependencies (DirectML.dll in rekordbox's folder) still resolve.
- **The real DLL's location:** `%ProgramData%\rbstemsplus\ort\onnxruntime.dll`. The installer must set ACLs there (ProgramData lets Users create files by default). The bridge checks the owner and DACL: only Administrators, SYSTEM or TrustedInstaller may write.
- **Folder name:** a chosen location's folder is `RB Stems Plus Cache` on Windows. There is no `.noindex` suffix, and a folder at a drive's root is allowed.
- **printf:** `__USE_MINGW_ANSI_STDIO=1` is needed for `%zu` and `%lld` with msvcrt.

## Probe script (`scripts/windows-probe.ps1`)
- Run it with `powershell -NoProfile -ExecutionPolicy Bypass -File .\windows-probe.ps1`. `-Deep` adds a slow scan of every DLL.
- `-RekordboxDir "..."` scans **only** that folder. The user has several old rekordbox versions installed. The registry entries are still listed, but not scanned. The AppData model folders are still scanned, since they're per user, not per version.
- It saves `rbstemsplus-probe.txt` on the Desktop, with the user name replaced by `<user>`.
- It parses PE export, import and delay-import tables in pure PowerShell, so it doesn't need Add-Type or C#. Windows PowerShell 5.1 can't do bitwise operations on UInt64 values with the top bit set, so a thunk's ordinal flag is read from its top byte instead.
- It couldn't be test-run here: downloading pwsh was denied by the sandbox's auto-mode classifier.

## The user's rekordbox binaries (2026-11-08)
- The user uploaded `rekordbox.exe`, `rekordboxAgent.exe`, `onnxruntime.dll`, `DirectML.dll`, `Demucs3.dll` and `Spleeter.dll` (7.2.14) at `https://davr.org/rekordboxbinaries.zip`.
  - **Use https**: plain http fails DNS, because only the HTTPS proxy is configured.
  - The sha256 of `onnxruntime.dll` and `Demucs3.dll` match the probe.
  - **Not committed.** These are Pioneer's files: keep them in the scratch area.
- The Windows bridge test suite passes against Pioneer's `onnxruntime.dll` under wine. Point `RBSTEMS_WIN_ORT=<folder with onnxruntime.dll and DirectML.dll>` at it. The DirectML provider is never appended in the tests, so `d3d12` isn't needed.

## Windows bridge: lessons
- `windows.h` defines `near` and `far`: `#undef` them in test code.
- The ORT header uses MSVC SAL (`_Frees_ptr_opt_`): define it as empty for mingw.
- libFLAC 1.5 builds a pthread-based multithreaded encoder by default: build it with `--disable-multithreading` for mingw, so the DLL doesn't need winpthread.
- `FOLDERID_*` GUIDs need `-luuid`.
- In an exe, `&ImportedFunction` is the exe's own import stub. To find which module a DLL export lives in, use the address of a **data** export declared `__declspec(dllimport)` (`rbstems_marker`).
- objdump prints the name table as `[   n] Name`, where n is ordinal minus base. Under awk, "[" and "n]" are separate fields.
- The free-space floor is min(10% of the disk, 50 GB). A test that wants to be "under the floor" with a 1000 GB disk needs less than 50 GB free.
- The suite takes about 1 minute under wine.

## Windows app core: design notes
- **Signing keys:** `keys/release.pub.pem` and `keys/backup.pub.pem` are **embedded resources** of RBStemsPlus.Core (`CompiledKeys.cs` reads them), so there is no generated Keys file. A test build passes `-p:RbspTestKey=<pem>` (only that key is embedded) and `-p:RbspTestBase=<url>`, which defines `RBSTEMSPLUS_TEST_BUILD` and an AssemblyMetadata attribute that `Consts.DownloadBase` reads.
- **Releases:** the Windows release has its own `payload-windows.json` plus `.sig`, with the same schema as `payload.json`. `bridge.file` is `onnxruntime.dll` and `app.zip` is `rbstemsplus-windows.zip`. The model asset is shared with macOS. The built-in compat list is rekordbox 7.2.14, ORT `1.18.`, engine 0002.
- **Downloads:** HttpClient with a redirect handler that refuses non-HTTPS (the counterpart of `curl --proto-redir =https`), resuming with Range.
- **Elevated operations:** the app re-runs itself with `--elevated install-cache <dir> <version> <bridge> <sha>` or `--elevated uninstall-cache [dirs]` through UAC. They are serialized by the `Global\io.github.rbstemsplus.admin` mutex. Exit codes are `AdminExit`.
- **ACLs:** `Elevated.SecureRoot` makes `%ProgramData%\rbstemsplus` protected (not inheriting): Administrators and SYSTEM get full control, Users read and execute, the owner is Administrators. Children inherit.
- **Model swap:** the same decisions as `Models.swift` (save Pioneer's with its `.engine`, `mayBePioneers`, check again before the replace), using `File.Move(overwrite)`, which is MoveFileEx(REPLACE_EXISTING) on Windows.
- **bridge-state start times:** the bridge writes the process creation time as whole seconds; .NET's `Process.StartTime` may round differently, so a difference of up to 1 second is accepted.
- **Small divergence:** `ConfigFile.ParseNumber` uses .NET invariant parsing. Unlike strtod, it doesn't take hex floats (`0x1p1`) or `inf`. Out-of-range values are refused anyway, and no fixture uses hex.

## Building the Windows app (lessons)
- **Ubuntu's `dotnet-sdk-8.0` package lacks `Microsoft.NET.Sdk.WindowsDesktop`**, so WinForms won't build with it. Use Microsoft's SDK: `curl -fsSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0 --install-dir <dir>`, then `DOTNET_ROOT=<dir> PATH=<dir>:$PATH`. `EnableWindowsTargeting` lets it build on Linux.
- **`Location` collides** with `Control.Location` inside a Form. The core class is `CacheLocation`.
- **Wine 9 can't start a single-file .NET bundle** (`0x8007046C` loading System.Private.CoreLib). To test under wine, publish with `-p:PublishSingleFile=false`. The release is single-file with compression: 72 MB, against 162 MB without.
- **Wine doesn't persist DACLs or owners** set by SetAccessControl on this filesystem: files report the wine user as owner, with Everyone allowed to read. ACL behaviour must be tested on real Windows.
- `AppVersion` reads the **core** assembly's version, so the version is set for every project in `windows/Directory.Build.props` (`-p:RbspVersion=`).

## Release pipeline notes
- The test build's model is downloaded from the published release, `https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/stemsplus-model.onnx` (308,572,524 bytes, sha256 `0cc50d87…`). The agent proxy drops long transfers, so the download resumes (`curl -C -`, 6 attempts).
- `bootstrap.ps1` placeholders, filled by `build-windows.sh`: `$SigningKeys = ""`, `$PayloadMinimum = ""`, and the `$DownloadBase = "https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/"` line (replaced for `--test`).
- A P-256 SubjectPublicKeyInfo is 91 bytes with the uncompressed point at offset 26 (0x04, then X and Y). The bootstrap's SPKI-to-blob conversion relies on that, and it was checked with openssl.
- The harness's real-library ordinal check applies only to a DirectML build: Microsoft's CPU build numbers its exports @1 and @2.
- `release.sh --windows` is opt-in, so the upstream macOS flow is unchanged without it.
