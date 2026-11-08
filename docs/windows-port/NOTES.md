# Windows port: notes and findings

These are facts that matter for the port, recorded as they turn up. **Verified** means checked here or in a source. **Assumed** means it still needs the Windows probe or a real PC.

## The user's decisions
- Scope: **full app parity** on Windows.
- App: **C# .NET 8 WinForms**, a single self-contained exe for win-x64.
- Bridge: a **separate Windows fork** in `bridge/win/`. The macOS bridge (`bridge/rbstems_bridge.c`) and the rest of the macOS app stay untouched.

## rekordbox on Windows
- Verified: rekordbox's settings folder is `%APPDATA%\Pioneer\rekordbox6`. On macOS it is `~/Library/Application Support/Pioneer/rekordbox6` (Pioneer forum).
- Assumed: the model is at `%APPDATA%\Pioneer\rekordbox6\models\demucs3_model\hdemucs.onnx`, with `demucs3_ver.txt` beside it, the same layout as macOS (`app/Sources/Paths.swift:53`).
- Assumed: rekordbox installs under `C:\Program Files\Pioneer\rekordbox 7.x.y\`, findable through the `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*` keys (InstallLocation, DisplayVersion).
- Assumed: `onnxruntime.dll` sits next to `rekordbox.exe`. Unknown: whether it is a DirectML build, and whether rekordbox checks its DLLs' integrity.
- Nothing public describes rekordbox's Windows ORT. The web searches turned up nothing.

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
- Run it with `powershell -NoProfile -ExecutionPolicy Bypass -File .\windows-probe.ps1`. Add `-RekordboxDir "..."` if rekordbox isn't where the registry says, and `-Deep` for a slow scan of every DLL.
- It saves `rbstemsplus-probe.txt` on the Desktop, with the user name replaced by `<user>`.
- It parses PE export, import and delay-import tables in pure PowerShell, so it doesn't need Add-Type or C#. Windows PowerShell 5.1 can't do bitwise operations on UInt64 values with the top bit set, so a thunk's ordinal flag is read from its top byte instead.
- It couldn't be test-run here: downloading pwsh was denied by the sandbox's auto-mode classifier.
