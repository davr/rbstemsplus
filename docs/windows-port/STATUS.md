# Windows port: status

This is the working list for the Windows port. The approved design is in [PLAN.md](PLAN.md), and the facts learned along the way are in [NOTES.md](NOTES.md). The list is updated as work moves forward.

Branch: `claude/affectionate-ride-f082bb`.

Legend: [x] done · [~] in progress · [ ] to do · [!] blocked or needs the user

## Phase 0: discovery
- [x] `scripts/windows-probe.ps1`: a read-only report covering the items below. Written and pushed. PowerShell couldn't be installed here (the sandbox denied it), so the script was only reviewed by reading it for PS 5.1 pitfalls, never run.
  - the rekordbox install and its version;
  - the ORT DLLs, with each one's exports, sha256 and signer;
  - rekordbox.exe's imports;
  - the model folder and its hashes.
- [!] **Waiting on the user:** run the probe on a real Windows PC and send back `rbstemsplus-probe.txt`. That output fills `exports.def`, the default paths and the compat lists.

## Phase 1: Windows bridge (`bridge/win/`)
- [~] `win_compat.h`: Win32 stand-ins. Written, not yet compiled. It covers:
  - NtCreateFile relative opens;
  - `rb_stat`;
  - unlink and rename with POSIX semantics;
  - directory listing;
  - volumes and free space;
  - SRWLOCK and INIT_ONCE;
  - a serial queue with delays, and a parallel apply;
  - SHA-256;
  - an inverse real FFT.
- [ ] `rbstems_bridge_win.c`: a copy of `bridge/rbstems_bridge.c`, ported:
  - blocks become callbacks; dispatch becomes `rb_queue`;
  - CC_SHA256 becomes `rb_sha256`; vDSP becomes `rb_irfft`;
  - the paths go to Known Folders;
  - the model path is `wchar_t`;
  - the ACL check replaces `only_root_can_change`;
  - `LoadLibraryExW`;
  - volume checks for the chosen location.
- [ ] `exports.def`: `OrtGetApiBase`, `OrtSessionOptionsAppendExecutionProvider_CPU`, `_DML`, `_CUDA` and `rbstems_marker`.
- [ ] `bridge/win/build.sh`:
  - libFLAC for the mingw host;
  - checks that the exports and imports match;
  - a test build (`--test REAL_PATH`).
- [ ] Tests:
  - port `harness.c` and `fake_ort.c` to Windows;
  - a test runner (bash, run under wine locally and Git Bash in CI);
  - FFT check against numpy;
  - config and marker fixtures through a fuzz-style unit test.

## Phase 2: app core (C# .NET 8 WinForms, `windows/RBStemsPlus/`)
- [ ] csproj (net8.0-windows, win-x64, single file, self-contained), manifest (asInvoker, PerMonitorV2)
- [ ] `Paths.cs`, `Config.cs` (grammar = `app/Sources/Config.swift`), `Payload.cs`, `Signing.cs`, `Compat.cs`
- [ ] `Rekordbox.cs` (discovery, version, Authenticode, bridge marker and caps), `Models.cs` (Demucs v4 swap)
- [ ] `State.cs` (bridge-state), `Location.cs` (external drives)
- [ ] xUnit tests (`windows/RBStemsPlus.Tests/`), using `tests/config-fixtures.txt` and `tests/marker-fixtures.txt`

## Phase 3: elevated operations
- [ ] `--elevated install-cache|uninstall-cache|update-cache` (UAC runas), RootExit-style codes, Global mutex, backups + SHA256SUMS, ProgramData ACLs, rollback

## Phase 4: UI
- [ ] MainForm (lights, buttons, log, status line, menus), TaskDialog alerts, SettingsForm, LocationMonitor

## Phase 5: watcher, update, report
- [ ] `--watch` via HKCU Run + FileSystemWatcher; self-update (rename running exe); report zip

## Phase 6: release, bootstrap, CI, docs
- [ ] `scripts/build-windows.sh`, `scripts/bootstrap.ps1`, `release.sh` additions, `.github/workflows/windows.yml`
- [ ] README Windows section, `docs/windows.md`, RELEASING.md, Makefile targets

## Verification still owed
- [ ] Bridge builds under mingw; the export and import checks pass
- [ ] Harness under wine with the fake ORT and the stock ORT 1.18.1 (cache miss, then hit)
- [ ] `dotnet test` green
- [!] A real end-to-end test on a Windows PC with rekordbox 7 (the user)
