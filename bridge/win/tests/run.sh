#!/bin/bash
# The Windows bridge's tests: builds a test bridge (bridge/win/build.sh --test), the harness, a fake
# "real" library and unit.exe, then runs them. On Linux everything runs under wine; on Windows
# (Git Bash / MSYS2) natively. Prints "ok"/"FAIL"/"skip" lines and a summary; exits 1 on a FAIL (and
# on a skip with RBSTEMS_NO_SKIP=1, as CI and a release want).
#
#   bridge/win/tests/run.sh
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc), python3 (stdlib only), and for the cache tests a stock
# ONNX Runtime 1.18.x for win-x64: RBSTEMS_WIN_ORT=<folder holding onnxruntime.dll>, else
# build/ort/onnxruntime-win-x64-1.18.1/lib (downloaded from microsoft/onnxruntime's GitHub release
# when missing). The bridge only checks the "1.18." version, so Microsoft's build stands in for
# Pioneer's (which is 1.18.0, DirectML).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbstems-wintests.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
CC="${CC_WIN:-x86_64-w64-mingw32-gcc}"
command -v "$CC" > /dev/null || CC=gcc                    # MSYS2's MINGW64 shell
passes=0 fails=0 skips=0

pass() { passes=$((passes + 1)); }
fail() {
  fails=$((fails + 1)); echo "FAIL $*"
  # the bridge's own words, for a failure seen only on another machine (CI)
  if [ -n "${h:-}" ] && [ -f "$h/AppData/Local/rbstemsplus/Logs/bridge.log" ]; then
    tail -n 6 "$h/AppData/Local/rbstemsplus/Logs/bridge.log" | sed 's/^/      bridge.log: /' | cut -c1-260
  fi
}
skip() { skips=$((skips + 1)); echo "skip $*"; }
expect() { if eval "$1"; then pass; else fail "$2"; fi; }

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) native=1; WINE=; winpath() { cygpath -w "$1"; } ;;
  *) native=; WINE="${WINE:-wine}"; export WINEDEBUG=-all; winpath() { printf 'Z:%s' "$1" | tr '/' '\\'; }
     command -v "$WINE" > /dev/null || { echo "bridge/win/tests/run.sh: wine not found (apt install wine)" >&2; exit 1; } ;;
esac

# ---------------------------------------------------------------- build

app="$scratch/app"; real_dir="$scratch/rt/real"; mkdir -p "$app" "$real_dir" "$scratch/model"
# icacls with MSYS2's argument conversion off (it would turn /grant into a path)
icacls_w() { MSYS2_ARG_CONV_EXCL='*' icacls "$@"; }
if [ -n "$native" ]; then
  # on Windows the bridge checks the real library's permissions for real (acl_ok): it and its two
  # parent folders may be changed only by Administrators, SYSTEM (and, in a test build, this
  # account), as the installer sets %ProgramData%\rbstemsplus. MSYS2's temp folder lets others write.
  for d in "$scratch/rt" "$real_dir"; do
    icacls_w "$(winpath "$d")" /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' "$USERNAME:(OI)(CI)F" > /dev/null \
      || { echo "FAIL couldn't set the test folder's permissions"; exit 1; }
  done
fi
"$root/bridge/win/build.sh" --test "$(winpath "$real_dir/onnxruntime.dll")" "$app" > "$scratch/build.log" 2>&1 \
  || { tail -20 "$scratch/build.log"; echo "FAIL the test bridge didn't build"; exit 1; }
"$root/bridge/win/build.sh" --unit "$app" >> "$scratch/build.log" 2>&1 || { tail -20 "$scratch/build.log"; echo "FAIL unit.exe didn't build"; exit 1; }
"$CC" -O1 -Wall -I"$root/bridge" -o "$app/harness.exe" "$here/harness.c" "$app/onnxruntime.dll" -static-libgcc || { echo "FAIL the harness didn't build"; exit 1; }
"$CC" -shared -O1 -o "$scratch/fake_ort.dll" "$here/fake_ort.c" -static-libgcc || { echo "FAIL the fake library didn't build"; exit 1; }

ort="${RBSTEMS_WIN_ORT:-$root/build/ort/onnxruntime-win-x64-1.18.1/lib}"
if [ ! -f "$ort/onnxruntime.dll" ] && [ -z "${RBSTEMS_WIN_ORT:-}" ]; then
  mkdir -p "$root/build/ort"
  curl -fsSL -o "$root/build/ort/ort.zip" https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/onnxruntime-win-x64-1.18.1.zip \
    && (cd "$root/build/ort" && unzip -oq ort.zip) || echo "(couldn't download ONNX Runtime 1.18.1: the cache tests are skipped)"
fi
have_real=; [ -f "$ort/onnxruntime.dll" ] && have_real=1
have_model=
if [ -n "$have_real" ] && command -v python3 > /dev/null; then
  python3 -I "$root/bridge/tests/make_model.py" "$scratch/model/hdemucs.onnx" && have_model=1
  for v in "good 0.64 -0.64 0.32 -0.32" "bad 0.64 0.64 0.64 0.64" "loud 200 -200 100 -100" "nan nan 0 0 0"; do
    set -- $v
    mkdir -p "$scratch/pmodel/$1" && python3 -I "$root/bridge/tests/make_model.py" --pioneer "$2" "$3" "$4" "$5" "$scratch/pmodel/$1/hdemucs.onnx" || have_model=
  done
fi
ours=; [ -n "$have_model" ] && ours="$(python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$scratch/model/hdemucs.onnx")"
sha() { python3 -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }

# ---------------------------------------------------------------- helpers

point() { rm -f "$real_dir"/*.dll; [ -z "$1" ] || cp "$1"/*.dll "$real_dir/" 2>/dev/null || cp "$1" "$real_dir/onnxruntime.dll"; }
fresh() { h="$scratch/home-$1"; rm -rf "$h"; mkdir -p "$h"; free=1000; disk=1000; pioneer=; near=1; anyvol=; hcache=; }
logf() { echo "$h/AppData/Local/rbstemsplus/Logs/bridge.log"; }
statef() { echo "$h/AppData/Local/rbstemsplus/Logs/bridge-state"; }
cachef() { echo "$h/AppData/Local/rbstemsplus/Cache"; }
has() { grep -qF -- "$1" "$(logf)" 2>/dev/null; }
count() { grep -cF -- "$1" "$(logf)" 2>/dev/null || true; }
config() { mkdir -p "$h/AppData/Roaming/rbstemsplus"; printf "$1" > "$h/AppData/Roaming/rbstemsplus/config.ini"; }
run_exe() {                                              # runs an exe from $app with the test environment
  local e=(env -u RBSTEMS_TEST_FREE_GB -u RBSTEMS_TEST_DISK_GB -u RBSTEMS_TEST_OUR_MODEL -u RBSTEMS_TEST_PIONEER_MODEL
           -u RBSTEMS_TEST_NEAR_HITS -u RBSTEMS_TEST_ANY_VOLUME -u RBSTEMS_HARNESS_CACHE RBSTEMS_TEST_HOME="$(winpath "$h")")
  [ -n "$near" ] && e+=(RBSTEMS_TEST_NEAR_HITS="$near")
  [ -n "$free" ] && e+=(RBSTEMS_TEST_FREE_GB="$free")
  [ -n "$disk" ] && e+=(RBSTEMS_TEST_DISK_GB="$disk")
  [ -n "$ours" ] && e+=(RBSTEMS_TEST_OUR_MODEL="$ours")
  [ -n "$pioneer" ] && e+=(RBSTEMS_TEST_PIONEER_MODEL="$pioneer")
  [ -n "$anyvol" ] && e+=(RBSTEMS_TEST_ANY_VOLUME=1)
  [ -n "$hcache" ] && e+=(RBSTEMS_HARNESS_CACHE="$(winpath "$hcache")")
  (cd "$app" && "${e[@]}" $WINE "./$1" "${@:2}") 2>/dev/null | tr -d '\r'
}
harness() { run_exe harness.exe "$@"; }
harness_ok() {
  local out; out="$(harness "$@")"
  if [ -n "$out" ] && ! grep -q FAIL <<< "$out"; then pass; else fail "harness $1: $(grep FAIL <<< "$out" | head -3)"; fi
}
strip() { sed -E 's/ \(x zero [01], (finite [01], error|worst) [^)]*\)//' | tr '\n' ' '; }
outcome() { harness run "$(winpath "$scratch/model/hdemucs.onnx")" "$@" | strip; }
poutcome() { local m="$1"; shift; harness prun "$(winpath "$scratch/pmodel/$m/hdemucs.onnx")" "$@" | strip; }

# ---------------------------------------------------------------- unit checks

fresh unit
expect '[ "$(run_exe unit.exe sha256 | grep -c "^ok")" = 4 ]' "SHA-256 test vectors: $(run_exe unit.exe sha256 | grep FAIL)"
expect 'run_exe unit.exe irfft | grep -q "^ok"' "the inverse FFT matches a direct DFT: $(run_exe unit.exe irfft)"

# the config fixtures (tests/config-fixtures-windows.txt), as the bridge parses them
ff="$root/tests/config-fixtures-windows.txt"
ran=0
while IFS=$'\t' read -r name expected; do
  ran=$((ran + 1))
  python3 -I "$root/bridge/tests/fixtures.py" write "$ff" "$name" "$scratch/fixture.ini" || { fail "fixture $name: couldn't be written"; continue; }
  got="$(run_exe unit.exe config "$(winpath "$scratch/fixture.ini")")"
  e=1 g=20 dd=60 where= dir=
  for w in $expected; do
    case "$w" in enabled=*) e="${w#*=}" ;; max_gb=*) g="${w#*=}" ;; max_days=*) dd="${w#*=}" ;; location=*) where="${w#*=}" ;; dir=*) dir="${w#*=}" ;; esac
  done
  if [ "$where" = unreadable ]; then want="location=unreadable"
  else want="enabled=$e max_gb=$(printf %g "$g") max_days=$(printf %g "$dd") location=$where${dir:+ dir=$dir}"; fi
  expect '[ "$got" = "$want" ]' "config fixture $name: got \"$got\", want \"$want\""
done <<< "$(python3 -I "$root/bridge/tests/fixtures.py" list "$ff")"
expect '[ "$ran" = "$(grep -vc "^#" "$ff")" ] && [ "$ran" -gt 60 ]' "every config fixture ran: $ran"

# the marker fixtures (tests/marker-fixtures.txt, shared with macOS)
mf="$root/tests/marker-fixtures.txt"
while IFS=$'\t' read -r name expected; do
  python3 -I "$root/bridge/tests/fixtures.py" write "$mf" "$name" "$scratch/marker" || { fail "marker fixture $name: couldn't be written"; continue; }
  got="$(run_exe unit.exe marker "$(winpath "$scratch/marker")")"
  expect '[ "$got" = "$expected" ]' "marker fixture $name: got $got, want $expected"
done <<< "$(python3 -I "$root/bridge/tests/fixtures.py" list "$mf")"

# ---------------------------------------------------------------- loading

fresh missing; point ""
harness_ok missing
expect 'has "ERROR no real ONNX Runtime library"' "missing: logged"

fresh fake; point "$scratch/fake_ort.dll"
harness_ok passthrough "$(winpath "$real_dir/onnxruntime.dll")"
expect 'has "NOT 1.18.x: passing everything through"' "a 1.17 library: passed through, logged"
expect 'grep -q "^state=" "$(statef)" && grep -q "^pid=[0-9]" "$(statef)" && grep -q "^start=[0-9]" "$(statef)"' "bridge-state written (pid, start, state)"

if [ -z "$have_real" ]; then
  skip "the cache tests (need ONNX Runtime 1.18.x for win-x64: RBSTEMS_WIN_ORT)"
else
  fresh load; point "$ort"
  harness_ok load "$(winpath "$real_dir/onnxruntime.dll")"

  # ---------------------------------------------------------------- exact and near hits
  if [ -z "$have_model" ]; then skip "the model tests (need python3)"; else
    # which Demucs sessions go on the GPU: rekordbox's own with gpu=1 (default), ours only with gpu_v4=1 (this runtime has no DirectML:
    # the line says why a session stays on the CPU)
    fresh gpu1; near=
    out="$(outcome a)"
    expect 'has "gpu_v4=0: Demucs v4 on the GPU is experimental"' "our model, defaults: stays on the CPU (gpu_v4=0): got \"$out\""
    expect 'has "memory pattern off, "' "our model on the CPU: sets the thread count (session line): got \"$out\""
    fresh gpu2; near=; config 'gpu=0\n'
    out="$(outcome a)"
    expect 'has "on the CPU (gpu=0 in config.ini)"' "gpu=0: on the CPU, and says so: got \"$out\""
    fresh gpu3; near=; config 'gpu_v4=1\n'
    out="$(outcome a)"
    expect '! has "gpu_v4=0" && has "GPU: " && has "runs on the CPU"' "gpu_v4=1: the bridge tries DirectML for our model (none here: it says so, runs on the CPU): got \"$out\""
    # the GPU watchdog (the stand-in: a "GPU" session on this CPU runtime, with a slow run and a short deadline)
    fresh wd1; near=; config 'gpu_v4=1\n'
    out="$(RBSTEMS_TEST_GPU_AS_CPU=1 RBSTEMS_TEST_RUN_DELAY_MS=600 RBSTEMS_TEST_GPU_TIMEOUT_MS=150 outcome a d)"
    expect '[ "$out" = "a model d model " ]' "a stalled GPU run: answered by the CPU twin, later chunks too: got \"$out\""
    expect '[ "$(count "this model now runs on the CPU")" = 1 ] && [ "$(count "CPU twin of the GPU session opened")" = 1 ] && ! has ERROR' "a stalled GPU run: logged once"
    fresh wd2; near=; config 'gpu_v4=1\n'
    out="$(RBSTEMS_TEST_GPU_AS_CPU=1 outcome a d)"
    expect '[ "$out" = "a model d model " ] && ! has "this model now runs on the CPU"' "a GPU run in time: no fallback: got \"$out\""
    # rbsp-bench.exe, on the CPU with the tiny model (no GPU here)
    "$root/bridge/win/build.sh" --bench "$scratch/bench" > /dev/null 2>&1 && cp "$ort/onnxruntime.dll" "$scratch/bench/ort.dll"
    bout="$(cd "$scratch/bench" && $WINE ./rbsp-bench.exe --model "$(winpath "$scratch/model/hdemucs.onnx")" --cpu --runs 2 --samples 4096 --frames 4 --ort "$(winpath "$scratch/bench/ort.dll")" 2>/dev/null | tr -d '\r')"
    expect 'grep -q "average of 2 runs" <<< "$bout" && grep -q "running on the CPU" <<< "$bout" && ! grep -q FAIL <<< "$bout"' "rbsp-bench runs a model on the CPU: got \"$bout\""
    fresh sh1; near=; config 'gpu_v4=1\n'
    out="$(RBSTEMS_TEST_GPU_AS_CPU=1 harness share "$(winpath "$scratch/model/hdemucs.onnx")")"
    expect '! grep -q FAIL <<< "$out" && [ "$(count "shared with an open session")" = 1 ] && [ "$(count "Demucs session opened")" = 3 ]' "GPU sessions of one model are shared: got \"$out\""
    fresh exact; near=
    out="$(outcome a wait a d wait d a)"
    expect '[ "$out" = "a model stored 1 a cache d model stored 2 d cache a cache " ]' "exact hits: got \"$out\""
    expect 'has "near hits off" && [ "$(count "hit  ")" = 3 ] && ! has ERROR' "exact hits: logged, no ERROR"
    flac="$(find "$(cachef)" -name '*.flac' | head -1)"
    expect '[ -n "$flac" ] && [ -f "${flac%.flac}.desc" ] && [ -f "$(cachef)/$ours/$(basename "$(dirname "$flac")")/$(basename "$flac")" ]' \
      "the entry is <cache>\\<model>\\<xx>\\<key>.flac with its .desc"
    touch -d '3 days ago' "$flac"
    out="$(outcome a d)"
    expect '[ "$out" = "a cache d cache " ]' "a new process finds them: got \"$out\""
    # a hit stamps its entry "last used" (the limits evict the least recently used)
    expect '[ "$(find "$(cachef)" -name "*.flac" -newermt "-1 hour" | wc -l | tr -d " ")" = 2 ]' "a hit marks its entry as used now (its modification time)"
    out="$(outcome b)"
    expect '[ "$out" = "b model " ]' "near hits off (the default): a shifted chunk runs the model: got \"$out\""

    fresh near; near=1
    out="$(outcome a wait b c)"
    expect '[ "$out" = "a model stored 1 b cache c model " ]' "near hits on: shifted served, tone burst refused: got \"$out\""
    expect '[ "$(grep -c "near [0-9a-f]* for" "$(logf)")" = 1 ] && has "rejected (residual)"' "near hits: logged (1 served, 1 refused for its residual)"

    fresh disabled; config 'enabled=0\n'
    out="$(outcome a sleep a)"
    expect '[ "$out" = "a model stored 0 a model " ] && has ", not cached"' "enabled=0: nothing cached: got \"$out\""

    fresh floor; free=40; disk=1000                       # the floor: 10% of 1000 GB, at most 50 GB
    out="$(outcome a sleep a)"
    expect '[ "$out" = "a model stored 0 a model " ] && has "saving paused"' "under the free-space floor: nothing saved: got \"$out\""

    fresh aged; config 'max_days=1\n'
    out="$(outcome a wait)"
    f="$(find "$(cachef)" -name '*.flac' | head -1)"
    touch -d '3 days ago' "$f" "${f%.flac}.desc"
    out="$(outcome sleep)"
    expect '[ ! -e "$f" ] && has "removed 1 unused for 1 days"' "an entry unused for max_days is removed at the next load"

    # a chosen location (any volume: a test build's RBSTEMS_TEST_ANY_VOLUME)
    fresh chosen; anyvol=1
    loc="$scratch/ext dir/RB Stems Plus Cache"; mkdir -p "$loc"; rm -rf "$loc"/*
    printf 'id=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\nformat=rbstems-v1\n' > "$loc/.rbstemsplus-cache"
    config "cache_dir=\"$(winpath "$loc" | sed 's/\\/\\\\/g')\"\ncache_id=0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\n"
    hcache="$loc"
    out="$(RBSTEMS_HARNESS_WAIT_FOR="cache: using" outcome a wait a)"
    expect '[ "$out" = "a model stored 1 a cache " ] && [ -n "$(find "$loc" -name "*.flac")" ] && [ -z "$(find "$(cachef)" -name "*.flac" 2>/dev/null)" ]' \
      "a chosen location: entries saved and served there, not in the default folder: got \"$out\""
    expect 'grep -q "^state=available" "$(statef)" && grep -qF "location=$(winpath "$loc")" "$(statef)"' "bridge-state: the chosen location, available"
    rm -f "$loc/.rbstemsplus-cache"
    out="$(RBSTEMS_HARNESS_WAIT_FOR="not available" outcome a sleep)"
    expect '[ "$out" = "a model stored 1 " ] && has "its folder'"'"'s marker is missing" && grep -q "^reason=marker" "$(statef)"' \
      "a chosen location without its marker: unavailable (reason marker), nothing read: got \"$out\""

    # ---------------------------------------------------------------- rebuilt (rekordbox's own) models
    W="w=0.64,-0.64,0.32,-0.32"
    psha="$(sha "$scratch/pmodel/good/hdemucs.onnx")"
    rid="$(printf 'rbstems-rebuild-1:%s' "$psha" | python3 -c 'import hashlib,sys;print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())')"
    fresh rb-on; pioneer="$psha"; config 'rekordbox_model=1\n'
    out="$(poutcome good "$W" a wait a b d wait)"
    expect '[ "$out" = "a model stored 1 a cache b cache d model stored 2 " ]' "rebuilt: model, then rebuilt stems served: got \"$out\""
    expect '[ -d "$(cachef)/$rid" ] && [ ! -e "$(cachef)/$psha" ] && ! has "not cached"' "rebuilt: entries in the rebuild's own folder, nothing refused"
    pioneer=
    out="$(poutcome good "$W" a b sleep)"
    expect '[ "$out" = "a model b model stored 2 " ]' "rebuilt: a model off the list finds nothing: got \"$out\""
    fresh rb-off; pioneer="$psha"
    out="$(poutcome good "$W" a sleep a)"
    expect '[ "$out" = "a model stored 0 a model " ] && has "not cached here (rekordbox_model=0)"' "rekordbox_model=0: passes through: got \"$out\""
    for v in "bad w=0.64,0.64,0.64,0.64 don't-add-up" "loud w=200,-200,100,-100 beyond-the-cache's-range" "nan w=nan,0,0,0 aren't-finite"; do
      read -r name ws why <<< "$v"; why="${why//-/ }"
      fresh "rb-$name"; pioneer="$(sha "$scratch/pmodel/$name/hdemucs.onnx")"; config 'rekordbox_model=1\n'
      out="$(poutcome "$name" "$ws" a sleep)"
      expect '[ "$out" = "a model stored 0 " ] && has "$why"' "rebuilt $name: nothing cached ($why): got \"$out\""
    done
    fresh rb-frames; pioneer="$psha"; config 'rekordbox_model=1\n'
    out="$(poutcome good "$W" frames=86 a sleep)"
    expect '[ "$out" = "a model stored 0 " ] && has "unexpected shapes for a rebuilt model"' "rebuilt: other shapes aren't cached: got \"$out\""
  fi

  # ---------------------------------------------------------------- the real library's permissions (Windows only)
  if [ -n "$native" ]; then
    fresh acl; point "$ort"
    icacls_w "$(winpath "$real_dir/onnxruntime.dll")" /grant '*S-1-1-0:(M)' > /dev/null
    harness missing > /dev/null
    expect 'has "can be changed by others than administrators: not loaded"' "a real library Everyone can change is refused"
  else
    skip "the real library's ACL check (wine doesn't enforce ACLs: runs on Windows)"
  fi
fi

echo
echo "bridge/win/tests: $passes passed, $fails failed, $skips skipped"
[ "$fails" = 0 ] || exit 1
[ "$skips" = 0 ] || [ "${RBSTEMS_NO_SKIP:-}" != 1 ] || { echo "skips aren't allowed (RBSTEMS_NO_SKIP=1)"; exit 1; }
exit 0
