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
| other rates (tested 48 kHz FLAC) | `SamplingFunctionSRC` (Kaiser sinc, 29 taps) | upstream's `resample.c` with **one change for x64**: in the convolution, which takes the taps in pairs from both ends, the first tap of each pair is a fused multiply-add and the second a multiply then an add (on arm64 both are fused); the middle tap is fused | **exact** (YACHT, Psychic City (Classixx Remix), 48 kHz FLAC: all 23 chunks, both channels). Found by trying the orders and fusings against the dumps: `helper/win/research/resample_x64_search.py`, checked by `resample_x64_verify.py` |

In a sample of the library (69 files profiled by `ffprobe`) nearly everything is 44.1 kHz, but real playlists have
more: 3 of 15 tracks in one, all 48 kHz (MP3 and FLAC).

## Not measured yet
- 24-bit AIFF/WAV/FLAC, mono and more than two channels, HE-AAC and fragmented MP4, 8-bit.
- MP3 files with other frame layouts (VBR with VBRI, mixed headers, ID3 in the middle): the rules are upstream's,
  and the one MP3 tested has an ID3 tag and a Xing frame.
- rekordbox versions other than 7.2.14.

## Decisions
- Prepare on Windows supports **MP3, AAC (M4A), ALAC, FLAC, AIFF and WAV**. A file that isn't at 44.1 kHz goes through
  rekordbox's resampler as its x64 build has it (measured exactly at 48 kHz with FLAC; the other rates and the other
  decoders ahead of it use the same code and are not measured one by one). The fused multiply-adds are the processor's
  own (the helper's resampler is compiled with `-mfma`); on a processor without FMA a file at another rate is skipped,
  because what rekordbox does there isn't known.
- The AAC reader must request **16-bit PCM** from Media Foundation.
- The helper uses **rekordbox's own `libmpg123.dll`**, found in its folder, like the macOS helper.
- Same format of kept sets, manifests and entries as macOS (kept files are byte-identical).

## Helper status (`rbsp-prepare.exe`, branch `integrate/upstream-1.3.0`)
Running log; newest last. Build: `make helper-win` (`helper/win/build.sh`); tests: `make helper-win-test` (wine).
- **Shim (`helper/win/posix.h` and the stub headers beside it)**: the POSIX, vDSP, CommonCrypto, CoreFoundation, fts,
  statfs and dlopen calls upstream's sources use, over `bridge/win/win_compat.h`. **Unchanged upstream files**:
  `manifest.c`, `mp3.c`, `mp4.c`, `resample.c`, `entry.c`, `model.c`, `prepare.c`. Two of them get a one-line `sed`
  at build time (`prepare.c`: the Unix absolute-path test; `model.c`: ONNX Runtime's UTF-16 model path); the build
  stops if the line they match ever changes. `main` and `model_run` are wrapped by `glue_win.c`.
- **Windows versions**: `decode_win.c` (probe and decode), `kept_win.c` (kept.c with its clang blocks as a callback and
  `%APPDATA%\rbstemsplus\kept`), `library_win.c` (library.c over rekordbox's own `sqlite3.dll`: no SQLCipher in our
  binary), `big_win.c`, `readable_win.c`, `glue_win.c`.
- **Works (tested under wine, `helper/win/tests/run.sh`, 31 checks)**: usage and refusals; `--playlists` and `--tracks`
  against a plain SQLite build of rekordbox's tables (order, folders first, deleted rows, duplicates, streaming tracks,
  UTF-8 paths); `--estimate`; a run with a tiny model through Microsoft's ONNX Runtime: entries written under the
  **same keys an independent Python implementation of rekordbox's chunking computes** (WAV; AIFF, **FLAC and ALAC**
  decode to the same entries, bit for bit), kept set, manifest, "unchanged since it was prepared" on a second run,
  estimate "ready" afterwards; the app's **stop** event (`Local\rbsp-prepare-<pid>-stop`: finishes the chunk, writes the
  set, final line, exit 0), **pause** event (blocks before the next model run until reset; stop still gets through a
  pause); the **CUDA path's fallback** (`%ProgramData%\rbstemsplus\ort-cuda` that can't start: the CPU takes over).
- **ALAC**: Apple's open-source decoder (Apache 2.0, `macosforge/alac` `codec/`, fetched at build time and sha256-pinned,
  in NOTICE), 16/24/32-bit, mono or stereo, 44.1 kHz.
- **CUDA (`cuda_win.c`)**: our model on `ort-cuda`'s ONNX Runtime with the bridge's provider options (device 0, heuristic
  cuDNN search, arena `kSameAsRequested`, 5 GiB cap), the folder's DLLs preloaded as the bridge does; any failure at load,
  session or run falls back to the CPU session (a note on stderr: `rbsp-prepare: CUDA: ...`). `RBSP_NO_CUDA=1` turns it off.
  The CUDA success path itself is **untestable here** (no GPU); it is the bridge's code path reused.
- **Written, not testable here**: MP3 decode (needs rekordbox's `libmpg123.dll`), AAC through Media Foundation (wine has no
  AAC decoder), the real SQLCipher key (the test `sqlite3.dll` accepts any key).
- **Not done**: other sample rates (skipped, by decision), the app's updater only swaps `RBStemsPlus.exe`, so a helper
  update needs `windows/RBStemsPlus/Updater.cs` to copy `rbsp-prepare.exe` too (the install command copies the whole zip).
