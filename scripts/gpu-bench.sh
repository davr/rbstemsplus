#!/usr/bin/env bash
# Benchmarks the Windows bridge's separation speed on this machine's GPU, with rekordbox's own ONNX
# Runtime (a DirectML build) and DirectML.dll, without rekordbox. Run in MSYS2 (MINGW64) on Windows
# by .github/workflows/windows-gpu.yml; needs python3, curl and gcc there.
#
#   RBSTEMS_PIONEER_DIR   a folder with Pioneer's onnxruntime.dll (1.18.0, DirectML) and DirectML.dll
#                         (from rekordbox's install folder; not in this repository)
#   RBSTEMS_MODEL_TAG     the release whose stemsplus-model.onnx and stemsplus-model-dml.onnx to use
#   RBSTEMS_MODEL_REPO    owner/repo of that release (default davr/rbstemsplus)
#   RBSTEMS_BENCH_CHUNKS  chunks per run (default 4; the first is warm-up)
#
# Three runs, each a fresh account folder: the dynamic model on the CPU (gpu=0), the dynamic model on
# DirectML (gpu=1), the fixed-shape model on DirectML (gpu=1, hdemucs-dml.onnx beside the model).
# Prints the bridge's GPU lines and each run's speed, and a table for $GITHUB_STEP_SUMMARY.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
pdir="${RBSTEMS_PIONEER_DIR:?set RBSTEMS_PIONEER_DIR to a folder with the rekordbox onnxruntime.dll and DirectML.dll}"
tag="${RBSTEMS_MODEL_TAG:-windows-test-6dml2}"
repo="${RBSTEMS_MODEL_REPO:-davr/rbstemsplus}"
chunks="${RBSTEMS_BENCH_CHUNKS:-4}"
[ -f "$pdir/onnxruntime.dll" ] && [ -f "$pdir/DirectML.dll" ] || { echo "$pdir needs onnxruntime.dll and DirectML.dll"; exit 1; }
winpath() { cygpath -w "$1"; }
work="${RBSTEMS_BENCH_DIR:-$root/build/gpu-bench}"
rm -rf "$work"; mkdir -p "$work/rt/real" "$work/app" "$work/models"
icacls_w() { MSYS2_ARG_CONV_EXCL='*' icacls "$@"; }

echo "== the GPU(s) Windows sees"
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | Format-Table -AutoSize | Out-String -Width 200"

echo "== Pioneer's runtime, locked like the installer does (the bridge refuses a library others can change)"
cp "$pdir/onnxruntime.dll" "$pdir/DirectML.dll" "$work/rt/real/"
for d in "$work/rt" "$work/rt/real"; do
  icacls_w "$(winpath "$d")" /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' "$USERNAME:(OI)(CI)F" > /dev/null
done

echo "== the models from $repo release $tag"
for f in stemsplus-model.onnx stemsplus-model-dml.onnx; do
  for attempt in 1 2 3 4 5 6; do
    curl -fsSL --proto =https -C - -o "$work/models/$f" "https://github.com/$repo/releases/download/$tag/$f" && break
    echo "  (interrupted: resuming)"; sleep $((attempt * 2))
  done
done
sha() { sha256sum "$1" | cut -d' ' -f1; }
dyn_sha="$(sha "$work/models/stemsplus-model.onnx")"; dml_sha="$(sha "$work/models/stemsplus-model-dml.onnx")"
echo "dynamic $dyn_sha"; echo "fixed   $dml_sha"

echo "== the test bridge and the harness"
"$root/bridge/win/build.sh" --test "$(winpath "$work/rt/real/onnxruntime.dll")" "$work/app" > "$work/build.log" 2>&1 || { tail -20 "$work/build.log"; exit 1; }
gcc -O1 -Wall -I"$root/bridge" -o "$work/app/harness.exe" "$root/bridge/win/tests/harness.c" "$work/app/onnxruntime.dll" -static-libgcc
cp "$work/rt/real/DirectML.dll" "$work/app/"

summary="${GITHUB_STEP_SUMMARY:-$work/summary.md}"
gpu_name="$(powershell -NoProfile -Command '(Get-CimInstance Win32_VideoController | Select-Object -First 1).Name' | tr -d '\r')"
{ echo "### Separation speed on $gpu_name"
  echo; echo "| run | session line | average (without the warm-up chunk) |"; echo "|---|---|---|"; } >> "$summary"

bench() {   # name  gpu(0|1)  with_fixed(0|1)
  local name="$1" gpu="$2" fixed="$3" h="$work/home-$1" m="$work/model-$1"
  rm -rf "$h" "$m"; mkdir -p "$h/AppData/Roaming/rbstemsplus" "$m"
  printf 'gpu=%s\n' "$gpu" > "$h/AppData/Roaming/rbstemsplus/config.ini"
  cp "$work/models/stemsplus-model.onnx" "$m/hdemucs.onnx"
  [ "$fixed" = 1 ] && cp "$work/models/stemsplus-model-dml.onnx" "$m/hdemucs-dml.onnx"
  echo "== run: $name (gpu=$gpu, fixed-shape model: $fixed)"
  local out
  out="$(cd "$work/app" && RBSTEMS_TEST_HOME="$(winpath "$h")" RBSTEMS_TEST_OUR_MODEL="$dyn_sha" RBSTEMS_TEST_DML_MODEL="$dml_sha" \
        RBSTEMS_TEST_FREE_GB=1000 RBSTEMS_TEST_DISK_GB=1000 ./harness.exe bench "$(winpath "$m/hdemucs.onnx")" "$chunks" 2>&1 | tr -d '\r')" || true
  echo "$out"
  local log="$h/AppData/Local/rbstemsplus/Logs/bridge.log"
  echo "-- bridge.log (GPU and session lines)"; grep -E "GPU|Demucs session|DirectML|failed|ERROR" "$log" 2>/dev/null || echo "(no bridge.log)"
  local line avg
  line="$(grep -m1 "Demucs session opened" "$log" 2>/dev/null | sed 's/^.*Demucs session opened: //' || true)"
  avg="$(grep "bench average" <<< "$out" | sed 's/bench average (without the first): //' || true)"
  echo "| $name | ${line:-no session line} | ${avg:-FAILED} |" >> "$summary"
}
bench cpu-dynamic 0 0
bench gpu-dynamic 1 0
bench gpu-fixed 1 1
echo; cat "$summary"
