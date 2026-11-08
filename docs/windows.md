# RB Stems Plus on Windows

RB Stems Plus has the same two features on Windows as on the Mac:
- **Demucs v4** swaps rekordbox's stems model for Meta's Demucs v4.
- **Stems Cache** saves the stems rekordbox separates, so the next load of a track is much faster.

Read the [main README](../README.md) for what each one does, the sound and speed comparisons, and
the [honest risks](../README.md#honest-risks). This page covers what is different on Windows.

> **Status: new.** The Windows version was tested with rekordbox **7.2.14** on Windows 11. Its
> STEMS model is the same file as on the Mac, and its ONNX Runtime is 1.18.0 (DirectML build).

## Before you install

- **Windows 10 or 11, 64-bit.**
- **rekordbox 7**, installed from rekordbox.com, with STEMS working and turned on at least once,
  so rekordbox has downloaded its STEMS Engine (*Preferences › Extensions › STEMS*).
- **Stems Cache needs an administrator's permission.** Windows asks once each time Stems Cache is
  installed, reinstalled or removed. Demucs v4 doesn't need it.
- **Disk space:** about 1.5 GB, plus the saved stems (20 GB at most by default).

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

Other files:
- **Saved stems:** `%LOCALAPPDATA%\rbstemsplus\Cache`, or a folder named
  `RB Stems Plus Cache` on an external drive, chosen in Settings.
- **Settings:** `%APPDATA%\rbstemsplus\config.ini`.
- **Logs:** `%LOCALAPPDATA%\rbstemsplus\Logs`.
- **The copy of Pioneer's ONNX Runtime that Stems Cache uses:**
  `C:\ProgramData\rbstemsplus\ort\onnxruntime.dll`. Only administrators can change it. Stems
  Cache refuses to load a copy that other accounts can change.
- **Reinstall reminders:** a small helper, *RB Stems Plus Watcher*, starts when you sign in. It
  appears in *Settings › Apps › Startup*. It only asks you to reinstall after a rekordbox update;
  it never changes anything by itself.

Unlike on the Mac, nothing is re-signed. rekordbox's own files keep AlphaTheta's signature, except
the one `onnxruntime.dll` that Stems Cache replaces.

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

`.github/workflows/windows.yml` runs all of these, plus the install command and the ACL checks on
a real Windows runner.
