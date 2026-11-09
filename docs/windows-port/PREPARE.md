# Prepare playlists on Windows: how Windows rekordbox decodes audio (measured)

Upstream 1.3.0's **Prepare** writes the Stems Cache entries rekordbox would write, ahead of time. An entry is
found again only if the chunk the helper builds is **bit for bit** the chunk rekordbox sends the model (the key is
the SHA-256 of the chunk's floats). So Prepare on Windows needs Windows rekordbox's own decoding, rule by rule.
This page records what was measured on **rekordbox 7.2.14, Windows 11** (2026-11-09).

## Method
- A research bridge (`RBSTEMS_DUMP_DIR`, `bridge/win/build.sh`; CI: `.github/workflows/windows-research.yml`,
  never released) writes every chunk rekordbox sends (`<ms>-<key>.mix`, 2 x 485,100 float32, planar).
- Real tracks from the user's library were loaded in rekordbox (read only), and the file was decoded again
  outside rekordbox by candidate decoders; a decoder matches when every chunk is identical (max difference 0).
- Chunking is as on macOS: chunk i starts at `i * 483052 - 1024`, 485,100 samples, zero padded, the track's final
  sample never sent (`helper/rbsp.h`).
- The scripts are in `helper/win/research/` (`mp3rb.py`, `cmp_ffmpeg.py`, `cmp_raw.py`, `mfprobe.*`,
  `resample_probe.py`).

## Results (44.1 kHz, stereo)
| Format | rekordbox's reader | Reproduced by | Result |
|---|---|---|---|
| MP3 | `FileReadMp3`: its frame map, each frame fed alone to **its own `libmpg123.dll`** (`mpg123_open_feed`, `feed`, `decode_frame`), 16-bit | upstream's `helper/mp3.c` rules, loading `libmpg123.dll` from rekordbox's folder; 16-bit / 32768 | **exact** (The Flashbulb, 4 chunks; Xing frame skipped, encoder delay not trimmed: the output is 2,257 samples later than ffmpeg's) |
| AAC / M4A (LC) | `FileReadAac` / `FileReadMp4` through **Media Foundation** | `IMFSourceReader` asked for **16-bit PCM** (NAudio's `MediaFoundationReader` default), 16-bit / 32768 | **exact** (3 chunks; length equals ffmpeg's). A 32-bit float request is **not** equal (up to 8.5e-3 off) |
| FLAC | `FileReadFlac` | any correct decode (ffmpeg's float output) | **exact** (Lane 8, 16 chunks) |
| AIFF (16-bit) | `FileReadAiff` | plain decode | **exact** (35 chunks) |
| WAV (16-bit) | its WAV reader | plain decode | **exact** (17 chunks) |
| ALAC (in M4A) | `FileReadAlac` | plain decode (ffmpeg's) | **exact** (36 chunks) |
| other rates (tested 48 kHz FLAC) | `SamplingFunctionSRC` (Kaiser sinc, 29 taps) | upstream's `resample.c` ported to NumPy | **not exact yet**: the same structure (pairs from both ends with FMA), but about 10% of samples are one float32 step off, in nearly every filter phase. The coefficient table and the summation order are the suspects; an impulse-response file would tell (not made: it needs a new file in the library) |

In the library (69 files profiled by `ffprobe`), 59 of 60 M4A, 38 of 40 FLAC and all MP3, AIFF and WAV are
44.1 kHz, so resampling matters for a few percent of tracks.

## Not measured yet
- 24-bit AIFF/WAV/FLAC, mono and more than two channels, HE-AAC and fragmented MP4, 8-bit.
- MP3 files with other frame layouts (VBR with VBRI, mixed headers, ID3 in the middle): the rules are upstream's,
  and the one MP3 tested has an ID3 tag and a Xing frame.
- rekordbox versions other than 7.2.14.

## Decisions
- Prepare on Windows supports **44.1 kHz MP3, AAC (M4A), ALAC, FLAC, AIFF and WAV**. Any other rate is reported as
  skipped ("not supported on Windows yet"), never prepared wrongly. Rates are added when the resampler matches.
- The AAC reader must request **16-bit PCM** from Media Foundation.
- The helper uses **rekordbox's own `libmpg123.dll`**, found in its folder, like the macOS helper.
- Same format of kept sets, manifests and entries as macOS (kept files are byte-identical).

## Helper status (`rbsp-prepare.exe`, branch `integrate/upstream-1.3.0`)
Running log; newest last.
- **Shim (`helper/win/posix.h`)**: the POSIX calls upstream's sources use (stat with nanosecond mtime, openat family,
  directory listing, dlopen, getline) over `bridge/win/win_compat.h`. Upstream `manifest.c`, `mp4.c`, `resample.c`
  and `mp3.c` compile **unchanged** with mingw-w64 (`-include win/posix.h`). Compiled only; not yet linked or run.
- **Not started**: CLI (`prepare_win.c`), library reader (SQLCipher via rekordbox's `sqlite3.dll`), kept sets,
  entry writer, model run, FLAC/AIFF/WAV decode, Media Foundation AAC, build script, CI.
- **Cannot be tested here**: anything needing rekordbox's DLLs, Media Foundation, a real `master.db`.
