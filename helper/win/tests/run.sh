#!/bin/bash
# rbsp-prepare.exe's tests: builds it (helper/win/build.sh --test), then runs it under wine (natively
# on Windows: Git Bash / MSYS2) against the bridge's stock-ONNX-Runtime test model and generated
# audio. Prints "ok"/"FAIL"/"skip" lines and a summary; exits 1 on a FAIL.
#
#   helper/win/tests/run.sh
#
# Needs mingw-w64, python3 with numpy-free stdlib only, the bridge's test model (python3 onnx, as
# bridge/win/tests/run.sh) and ONNX Runtime 1.18.x for win-x64 (build/ort, as the bridge's tests).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-helper-tests.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
CC="${CC_WIN:-x86_64-w64-mingw32-gcc}"
command -v "$CC" > /dev/null || CC=gcc
passes=0 fails=0 skips=0
pass() { passes=$((passes + 1)); }
fail() { fails=$((fails + 1)); echo "FAIL $*"; }
skip() { skips=$((skips + 1)); echo "skip $*"; }
expect() { if eval "$1"; then pass; else fail "$2"; fi; }
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) WINE=; winpath() { cygpath -w "$1"; } ;;
  *) WINE="${WINE:-wine}"; export WINEDEBUG=-all; winpath() { printf 'Z:%s' "$1" | tr '/' '\\'; } ;;
esac

app="$scratch/app"; mkdir -p "$app"
"$root/helper/win/build.sh" --test "$app" > "$scratch/build.log" 2>&1 || { tail -20 "$scratch/build.log"; echo "FAIL rbsp-prepare.exe didn't build"; exit 1; }
"$CC" -O1 -Wall -o "$app/drive.exe" "$here/drive.c" || { echo "FAIL drive.exe didn't build"; exit 1; }
ort="${RBSTEMS_WIN_ORT:-$root/build/ort/onnxruntime-win-x64-1.18.1/lib}"
if [ ! -f "$ort/onnxruntime.dll" ] && [ -z "${RBSTEMS_WIN_ORT:-}" ]; then
  mkdir -p "$root/build/ort"
  curl -fsSL -o "$root/build/ort/ort.zip" https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/onnxruntime-win-x64-1.18.1.zip \
    && (cd "$root/build/ort" && unzip -oq ort.zip) || true
fi
[ -f "$ort/onnxruntime.dll" ] || { echo "FAIL no ONNX Runtime for the model tests"; exit 1; }
mkdir -p "$scratch/model" "$scratch/audio" "$scratch/rb" "$scratch/c6" "$scratch/empty"
python3 -I "$here/make_model.py" "$scratch/model/hdemucs.onnx" || { echo "FAIL no test model"; exit 1; }
ours="$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$scratch/model/hdemucs.onnx")"
python3 -I "$here/make_audio.py" "$scratch/audio" "$ours" || { echo "FAIL test audio"; exit 1; }
keys=($(python3 -c 'import json,sys;print(*json.load(open(sys.argv[1]))["keys"])' "$scratch/audio/a.expected.json"))

P() { winpath "$1"; }
prep() { (cd "$app" && RBSP_TEST_OURS="$ours" RBSP_TEST_FREE_GB=900 $WINE ./rbsp-prepare.exe "$@") 2>&1 | tr -d '\r'; }
drive() { (cd "$app" && RBSP_TEST_OURS="$ours" RBSP_TEST_FREE_GB=900 $WINE ./drive.exe "$@") 2>/dev/null | tr -d '\r'; }
common=(--model "$(P "$scratch/model/hdemucs.onnx")" --ort "$(P "$ort/onnxruntime.dll")" --rekordbox "$(P "$scratch/rb")" --threads 1)

# ---------------------------------------------------------------- usage and refusals
out="$(prep)"
expect 'grep -q "^usage:" <<< "$out"' "no arguments: usage"
mkdir -p "$scratch/c0" "$scratch/k0"
out="$(prep --model "$(P "$app/rbsp-prepare.exe")" --cache "$(P "$scratch/c0")" "$(P "$scratch/audio/a.wav")")"
expect 'grep -q "not a model the bridge caches" <<< "$out"' "a model that isn't ours or Pioneer's is refused: $out"
echo junk > "$scratch/audio/x.ogg"
out="$(prep --playlists --rekordbox "$(P "$scratch/empty")" --library "$(P "$scratch/none.db")" 2>&1)"
expect 'grep -q "sqlite3.dll" <<< "$out"' "--playlists without rekordbox's sqlite3.dll says so: $out"

# ---------------------------------------------------------------- the library (a plain SQLite stand-in for master.db)
SQLITE_VERSION=3460100 SQLITE_SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
sq="$root/build/sqlite/sqlite-amalgamation-$SQLITE_VERSION"
if [ ! -f "$sq/sqlite3.c" ]; then
  mkdir -p "$root/build/sqlite"
  curl -fsSL -o "$root/build/sqlite/a.zip" "https://www.sqlite.org/2024/sqlite-amalgamation-$SQLITE_VERSION.zip" \
    && [ "$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$root/build/sqlite/a.zip")" = "$SQLITE_SHA256" ] \
    && (cd "$root/build/sqlite" && unzip -oq a.zip) || rm -f "$root/build/sqlite/a.zip"
fi
if [ -f "$sq/sqlite3.c" ] && "$CC" -shared -O1 -w -DSQLITE_API='__declspec(dllexport)' -DSQLITE_THREADSAFE=1 -I"$sq" "$sq/sqlite3.c" "$here/fakekey.c" -static-libgcc -o "$scratch/rb/sqlite3.dll"; then
  python3 -I "$here/make_library.py" "$scratch/master.db"
  pl="$(prep --playlists --rekordbox "$(P "$scratch/rb")" --library "$(P "$scratch/master.db")")"
  expect '[ "$(wc -l <<< "$pl")" = 5 ] && sed -n 1p <<< "$pl" | grep -q "\"playlist\":\"1\".*\"name\":\"Folder F\",\"kind\":\"folder\",\"tracks\":0" && sed -n 2p <<< "$pl" | grep -q "\"playlist\":\"2\".*\"parent\":\"1\".*\"tracks\":5" && sed -n 3p <<< "$pl" | grep -q "\"name\":\"Zed\"" && sed -n 4p <<< "$pl" | grep -q "\"name\":\"Set B\"" && sed -n 5p <<< "$pl" | grep -q "\"kind\":\"intelligent\""' "--playlists: parents first, rekordbox's order, deleted ones left out: $pl"
  tr_="$(prep --tracks 2 --rekordbox "$(P "$scratch/rb")" --library "$(P "$scratch/master.db")")"
  expect '[ "$(wc -l <<< "$tr_")" = 3 ] && sed -n 1p <<< "$tr_" | grep -qF "{\"file\":\"C:\\\\Music\\\\a.wav\"}" && sed -n 2p <<< "$tr_" | grep -qF "C:/Music/b.flac" && sed -n 3p <<< "$tr_" | grep -q "M.*sica.*mp3"' "--tracks: in rekordbox's order, each path once, streaming left out: $tr_"
  expect 'grep -q "is a folder, not a playlist" <<< "$(prep --tracks 1 --rekordbox "$(P "$scratch/rb")" --library "$(P "$scratch/master.db")")"' "--tracks on a folder is refused"
  expect 'grep -q "has no playlist 99" <<< "$(prep --tracks 99 --rekordbox "$(P "$scratch/rb")" --library "$(P "$scratch/master.db")")"' "--tracks on an unknown playlist is refused"
else skip "the library tests (no SQLite amalgamation)"; fi

# ---------------------------------------------------------------- estimate
cache="$scratch/c1"; kdir="$scratch/k1"; mkdir -p "$cache" "$kdir"
wav="$(P "$scratch/audio/a.wav")"; aif="$(P "$scratch/audio/a.aiff")"
est="$(prep --estimate "${common[@]}" --cache "$(P "$cache")" --kept-dir "$(P "$kdir")" "$wav" "$aif" "$(P "$scratch/audio/none.wav")" "$(P "$scratch/audio/x.ogg")")"
expect 'grep -q "\"result\":\"prepare\",\"seconds\":11.0,\"entries\":2" <<< "$est"' "estimate: the WAV needs 2 entries: $est"
expect '[ "$(grep -c "\"result\":\"prepare\"" <<< "$est")" = 2 ]' "estimate: WAV and AIFF both to prepare"
expect 'grep -q "none.wav\",\"result\":\"skipped\",\"why\":\"the file isn.t there" <<< "$est"' "estimate: a missing file is skipped"
expect 'grep -q "x.ogg\",\"result\":\"skipped\",\"why\":\".ogg: not a file type" <<< "$est"' "estimate: an unknown type is skipped"
expect 'grep -q "{\"estimate\":4,\"ready\":0,\"prepare\":2,\"skipped\":2" <<< "$est"' "estimate summary: $(tail -1 <<< "$est")"

# ---------------------------------------------------------------- a run
run="$(prep "${common[@]}" --cache "$(P "$cache")" --kept-dir "$(P "$kdir")" --keep t1 "$wav")"
expect 'grep -q "\"result\":\"prepared\",\"written\":2,\"already\":0,\"silent\":0,\"refused\":0" <<< "$run"' "run: two entries written: $run"
expect 'grep -q "^{\"done\":1,\"runs\":2" <<< "$run" && grep -q "\"kept\":\"t1\"" <<< "$run"' "run: done line: $(tail -1 <<< "$run")"
for k in "${keys[@]}"; do
  expect '[ -f "$cache/$ours/${k:0:2}/$k.flac" ] && [ -f "$cache/$ours/${k:0:2}/$k.desc" ]' "run: entry $k (the key from rekordbox chunking of the WAV) is in the cache"
done
expect '[ "$(wc -l < "$kdir/t1.kept")" = 4 ] && grep -q "^rbstemsplus-kept 1" "$kdir/t1.kept"' "the kept set has its header and two entries: $(cat "$kdir/t1.kept" 2>&1 | head -5 | cut -c1-80)"
expect 'grep -q "^file .*a.wav" "$kdir/t1.manifest"' "the manifest names the file"
run2="$(prep "${common[@]}" --cache "$(P "$cache")" --kept-dir "$(P "$kdir")" --keep t1 "$wav")"
expect 'grep -q "\"written\":0,\"already\":2,.*unchanged since it was prepared" <<< "$run2"' "a second run finds it prepared: $run2"
est2="$(prep --estimate "${common[@]}" --cache "$(P "$cache")" --kept-dir "$(P "$kdir")" --keep t1 "$wav")"
expect 'grep -q "\"result\":\"ready\",\"entries\":2" <<< "$est2"' "estimate after the run: ready: $est2"
# AIFF decodes to the same samples, so the same entries
run3="$(prep "${common[@]}" --cache "$(P "$cache")" --kept-dir "$(P "$kdir")" --no-keep "$aif")"
expect 'grep -q "\"written\":0,\"already\":2" <<< "$run3"' "the AIFF gives the WAV's entries: $run3"

# ---------------------------------------------------------------- the CUDA path falls back to the CPU
# a "CUDA ONNX Runtime" that has no CUDA (Microsoft's CPU build): the session fails, the CPU runs, entries are written
c5="$scratch/c5"; k5="$scratch/k5"; mkdir -p "$c5" "$k5"
out="$(cd "$app" && RBSP_TEST_OURS="$ours" RBSP_TEST_FREE_GB=900 RBSP_TEST_CUDA_PATH="$(P "$ort/onnxruntime.dll")" $WINE ./rbsp-prepare.exe "${common[@]}" --cache "$(P "$c5")" --kept-dir "$(P "$k5")" --no-keep "$wav" 2>&1 | tr -d '\r')"
expect 'grep -q "CUDA: the CUDA session failed, the CPU is used" <<< "$out" && grep -q "\"written\":2" <<< "$out"' "CUDA that can't start: the CPU takes over and the entries are written: $out"
out="$(cd "$app" && RBSP_TEST_OURS="$ours" RBSP_TEST_FREE_GB=900 RBSP_TEST_CUDA_PATH="$(P "$scratch/nothing/onnxruntime.dll")" $WINE ./rbsp-prepare.exe "${common[@]}" --cache "$(P "$scratch/c6")" --no-keep "$wav" 2>&1 | tr -d '\r')"
expect 'grep -q "\"written\":2" <<< "$out"' "no CUDA library: the CPU is used: $out"

# ---------------------------------------------------------------- stop and pause (the app's events)
c2="$scratch/c2"; k2="$scratch/k2"; mkdir -p "$c2" "$k2"
out="$(drive stop-at-chunk0 ./rbsp-prepare.exe "${common[@]}" --cache "$(P "$c2")" --kept-dir "$(P "$k2")" --keep t2 "$wav")"
expect 'grep -q "\"result\":\"stopped\"" <<< "$out" && grep -q "\"stopped\":\"stopped\"" <<< "$out" && grep -q "drive: exit 0" <<< "$out"' "stop event: the run stops, prints its final line and exits 0: $out"
expect 'grep -q "\"written\":1" <<< "$out" && [ "$(wc -l < "$k2/t2.kept")" = 3 ]' "stop event: the finished chunk is kept, the set written"
c3="$scratch/c3"; k3="$scratch/k3"; mkdir -p "$c3" "$k3"
out="$(drive pause-then-resume ./rbsp-prepare.exe "${common[@]}" --cache "$(P "$c3")" --kept-dir "$(P "$k3")" --keep t3 "$wav")"
at0="$(sed -n 's/^\([0-9]*\) {"at":0,.*/\1/p' <<< "$out")"; res="$(sed -n 's/^drive: resumed at \([0-9]*\)/\1/p' <<< "$out")"
done_at="$(sed -n 's/^\([0-9]*\) {"file":.*"result":"prepared".*/\1/p' <<< "$out")"
expect '[ -n "$done_at" ] && [ -n "$res" ] && [ "$done_at" -ge "$res" ]' "pause event: nothing finishes before the resume ($done_at vs $res): $out"
expect 'grep -q "\"written\":2" <<< "$out" && grep -q "drive: exit 0" <<< "$out"' "pause event: after the resume the run completes"
c4="$scratch/c4"; k4="$scratch/k4"; mkdir -p "$c4" "$k4"
out="$(drive pause-and-stop ./rbsp-prepare.exe "${common[@]}" --cache "$(P "$c4")" --kept-dir "$(P "$k4")" --keep t4 "$wav")"
expect 'grep -q "\"stopped\":\"stopped\"" <<< "$out" && grep -q "drive: exit 0" <<< "$out"' "stop gets through a pause: $out"

echo "$passes passed, $fails failed, $skips skipped"
[ "$fails" = 0 ] && { [ "$skips" = 0 ] || [ -z "${RBSTEMS_NO_SKIP:-}" ]; }
