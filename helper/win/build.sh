#!/bin/sh
# Builds rbsp-prepare.exe (x64) with mingw-w64 on Linux or macOS: the macOS prepare helper's
# sources, mostly unchanged, over the POSIX shim in helper/win/ (posix.h and the stub headers next
# to it), plus the Windows files here (decode, library, kept sets, glue). libFLAC is built from the
# bridge's tarball (version and sha256 from bridge/win/build.sh).
#
#   helper/win/build.sh [--test] [OUT]   into OUT (default build/helper-win). --test (for tests/run.sh only):
#                                        the model named by $RBSP_TEST_OURS counts as ours (the tests' tiny model)
#
# rekordbox's library key isn't in this repository: as helper/build.sh does, it is the one
# pyrekordbox publishes, both files fetched from a pinned commit (sha256-checked), or read from
# RBSP_LIBRARY_KEY_FILE (offline), checked against its sha256 and built in masked.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
hs="$root/helper"
test_ours=
if [ "${1:-}" = "--test" ]; then test_ours=1; shift; fi
out="${1:-$root/build/helper-win}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
HOST=x86_64-w64-mingw32
CC="${CC_WIN:-$HOST-gcc}"
CXX="${CXX_WIN:-$HOST-g++}"
command -v "$CC" >/dev/null || { echo "helper/win/build.sh: $CC not found (install mingw-w64)" >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "helper/win/build.sh: $CXX not found (install g++-mingw-w64)" >&2; exit 1; }
FLAC_VERSION="$(sed -n 's/^FLAC_VERSION=//p' "$root/bridge/win/build.sh")"
FLAC_SHA256="$(sed -n 's/^FLAC_SHA256=//p' "$root/bridge/win/build.sh")"
FLAC_URLS="https://downloads.xiph.org/releases/flac/flac-$FLAC_VERSION.tar.xz https://github.com/xiph/flac/releases/download/$FLAC_VERSION/flac-$FLAC_VERSION.tar.xz"
LIBKEY_SHA256=ff44e8729e6dfe792d4cef556e4cc1ed3f27225604ede12877a5a04a776c0665
PYREKORDBOX_COMMIT=2dcb57f2ea907c51fa15850f5433ec09ff3223ff   # pyrekordbox v0.5.0 (MIT)
PYREKORDBOX_DATABASE_SHA256=1f9dc4ba70f079942eaf4a6fd3fcf8d1816fcd03a5723408f512e3b3683c7a3b
PYREKORDBOX_UTILS_SHA256=7da83d3de766a2f352001e77b0caff92872569239d449648c1dc03fb28022289

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
# fetch FILE SHA256 URL...: FILE once, kept only when its sha256 matches
fetch() {
  f="$1"; want="$2"; shift 2
  if [ -f "$f" ] && [ "$(sha256 "$f")" = "$want" ]; then return 0; fi
  mkdir -p "$(dirname "$f")"
  for u in "$@"; do
    echo "downloading $u"
    if curl -fsSL -o "$f.part" "$u" && [ "$(sha256 "$f.part")" = "$want" ]; then mv -f "$f.part" "$f"; return 0; fi
    rm -f "$f.part"
  done
  echo "$f: no download matched sha256 $want" >&2
  exit 1
}

obj="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-obj.XXXXXX")"      # holds the key while the build runs
trap 'rm -rf "${obj:?}"' EXIT

# ---------------------------------------------------------------- libFLAC
tarball="$root/build/flac/flac-$FLAC_VERSION.tar.xz"
fetch "$tarball" "$FLAC_SHA256" $FLAC_URLS
flac="$root/build/helper-win/flac-$FLAC_VERSION"
if [ ! -f "$flac/lib/libFLAC.a" ]; then
  scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-flac.XXXXXX")"
  tar -xJf "$tarball" -C "$scratch"
  echo "building libFLAC $FLAC_VERSION for $HOST"
  (
    cd "$scratch/flac-$FLAC_VERSION"
    ./configure --quiet --host="$HOST" --enable-static --disable-shared --disable-ogg --disable-programs \
      --disable-examples --disable-cpplibs --disable-doxygen-docs --disable-version-from-git \
      --disable-stack-smash-protection --disable-multithreading CC="$CC" CFLAGS="-O2 -U_FORTIFY_SOURCE"
    make -s -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" -C src/libFLAC
  ) > "$scratch/build.log" 2>&1 || { tail -30 "$scratch/build.log" >&2; exit 1; }
  mkdir -p "$flac/lib" "$flac/include/FLAC"
  cp "$scratch/flac-$FLAC_VERSION/include/FLAC/"*.h "$flac/include/FLAC/"
  cp "$scratch/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$flac/lib/libFLAC.a"
  rm -rf "$scratch"
fi

# ---------------------------------------------------------------- Apple's ALAC decoder
# github.com/macosforge/alac (Apache License 2.0), the codec/ files, each sha256-checked; used by alac_win.cpp
alac="$root/build/helper-win/alac"
alac_get() {   # alac_get FILE SHA256
  fetch "$alac/$1" "$2" "https://raw.githubusercontent.com/macosforge/alac/master/codec/$1"
}
alac_get ALACAudioTypes.h a0fe17bf82191d28c500b9ee1e773d49700df5e33ad862ed61b13c8a094c03b9
alac_get ALACBitUtilities.c 276ac7d7f2d240a4b0437d7e222afcba4c78bd8fd8104d0e9c6b0d45f67abcfe
alac_get ALACBitUtilities.h 18d1c8f957a403cc5b9b7d99e9d3987001adeac6ceadb4a4b1b5dc1ea4bff8ca
alac_get ALACDecoder.cpp b071c3a82bcb0b5e8ad704cafb514aaeba5b0869b7dcc83fa79561bae994c019
alac_get ALACDecoder.h 779a1686417687f3ba107f9a12bb0f4c892885c0654d0bd5db7133bc4edfe24c
alac_get EndianPortable.c 34b4d4c4dd599e05ac43f4a58eef61ef2eb5a7093e46209eebdf6ac7024b3dfc
alac_get EndianPortable.h 5c3b1df02e8bef3adb4feee6f0ed0684b2d000d44e57a0c2ed754668794d40b0
alac_get ag_dec.c 622d7551ef041d82fa6462292155c48fcc2d8b912ed0241e202b86afa3536747
alac_get aglib.h ef715d64666e4a4faaa0fafcd5050bd5b7f8c45e9760382735f37cf183c55778
alac_get dp_dec.c 1f8e8b25e61c5b667ad729a9c33e3916df7eb4883125dcac7266d719cbf23516
alac_get dplib.h fd2b9806f8cf2386dcbdc919ddf56c18608ce1b0812a74b6b9d0b0eb9df67d78
alac_get matrix_dec.c ae9dd8abc1a9d1cf7f6256c0b15ffd72f9d51c6ffdae9b833f6e96d9d8a03495
alac_get matrixlib.h 00839b4672b4ae7887a4d0448d6258165753b1e5f85f84c53c67e7e7b8b9d76d

# ---------------------------------------------------------------- rekordbox's library key
keyfile="${RBSP_LIBRARY_KEY_FILE:-}"
if [ -n "$keyfile" ]; then
  [ -r "$keyfile" ] || { echo "RBSP_LIBRARY_KEY_FILE: $keyfile can't be read" >&2; exit 1; }
else
  pyrb="$root/build/pyrekordbox/$PYREKORDBOX_COMMIT"
  url="https://raw.githubusercontent.com/dylanljones/pyrekordbox/$PYREKORDBOX_COMMIT/pyrekordbox"
  fetch "$pyrb/database.py" "$PYREKORDBOX_DATABASE_SHA256" "$url/masterdb/database.py"
  fetch "$pyrb/utils.py" "$PYREKORDBOX_UTILS_SHA256" "$url/utils.py"
  keyfile="$obj/libkey.raw"
  # pyrekordbox's deobfuscate(BLOB): base85, XOR with BLOB_KEY, zlib. Both read as literals,
  # nothing of pyrekordbox's is run.
  python3 -I - "$pyrb/database.py" "$pyrb/utils.py" > "$keyfile" <<'PY' || { echo "couldn't read the key from pyrekordbox's files" >&2; exit 1; }
import ast, base64, re, sys, zlib
def literal(path, name):
    m = re.search(r'^' + name + r'\s*=\s*(b"[^"]*")', open(path, encoding="utf-8").read(), re.M)
    if not m:
        sys.exit(path + ": no " + name)
    return ast.literal_eval(m.group(1))
blob, k = literal(sys.argv[1], "BLOB"), literal(sys.argv[2], "BLOB_KEY")
data = base64.b85decode(blob)
sys.stdout.buffer.write(zlib.decompress(bytes(b ^ k[i % len(k)] for i, b in enumerate(data))))
PY
fi
[ "$(tr -d '\r\n' < "$keyfile" | { sha256sum 2>/dev/null || shasum -a 256; } | cut -d' ' -f1)" = "$LIBKEY_SHA256" ] \
  || { echo "$keyfile doesn't hold rekordbox's library key (sha256 differs)" >&2; exit 1; }
# the key, masked: byte i stored as key[i] + 0x5a + 13i (library_win.c undoes it)
tr -d '\r\n' < "$keyfile" | od -An -tu1 -v | awk '
  BEGIN { n = 0 }
  { for (f = 1; f <= NF; f++) { b[n] = ($f + 90 + 13 * n) % 256; n++ } }
  END { printf "static const unsigned char LIBKEY[] = {"; for (i = 0; i < n; i++) printf "%s%d", i ? "," : "", b[i];
        printf "};\n#define LIBKEY_LEN %d\n", n }' > "$obj/libkey.h"
# the masked bytes must give the key back (its sha256), or the build stops
python3 -I - "$obj/libkey.h" "$LIBKEY_SHA256" <<'PY' || { echo "the masked key doesn't decode to the key: build.sh's masking and library_win.c's differ" >&2; exit 1; }
import hashlib, re, sys
b = [int(x) for x in re.search(r'\{([0-9,]+)\}', open(sys.argv[1]).read()).group(1).split(',')]
k = bytes((v - 0x5a - 13 * i) % 256 for i, v in enumerate(b))
sys.exit(0 if hashlib.sha256(k).hexdigest() == sys.argv[2] else 1)
PY
rm -f "$obj/libkey.raw"

# ---------------------------------------------------------------- the program
cflags="${test_ours:+-DRBP_TEST }-O2 -U_FORTIFY_SOURCE -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-cast-function-type -Wno-unused-result -DFLAC__NO_DLL -D_FILE_OFFSET_BITS=32"
inc="-include $here/posix.h -I$here -I$obj -I$hs -I$root/bridge -I$flac/include"
cc() { "$CC" $cflags $inc "$@"; }
# prepare.c: its absolute-path test is a Unix one ('/'): the same file, that test replaced, main renamed
sed "s|path\[0\] != '/'|!rbp_abs(path)|g" "$hs/prepare.c" > "$obj/prepare_gen.c"
if [ -n "$test_ours" ]; then
  sed -i 's|int ours = !strcmp(fp, OURS_V4);|int ours = !strcmp(fp, getenv("RBSP_TEST_OURS") ? getenv("RBSP_TEST_OURS") : OURS_V4);|' "$obj/prepare_gen.c"
  grep -q 'RBSP_TEST_OURS' "$obj/prepare_gen.c" || { echo "helper/prepare.c changed: the model test line no longer matches" >&2; exit 1; }
fi
[ "$(grep -c 'rbp_abs(path)' "$obj/prepare_gen.c")" = 2 ] || { echo "helper/prepare.c changed: its two path tests no longer match helper/win/build.sh's" >&2; exit 1; }
cc -Dmain=prepare_main -c "$obj/prepare_gen.c" -o "$obj/prepare.o"
# model.c, built twice: cpu_* (Pioneer's ONNX Runtime) and cuda_* (the CUDA one, the CUDA provider added to the
# session options); cuda_win.c chooses. ONNX Runtime takes a UTF-16 model path here (one sed line); main's model_run
# is glue_win.c's (it waits while paused) and calls cuda_win.c's model_run_inner.
sed 's|R->CreateSession(env, path, o, &m->s)|R->CreateSession(env, rbp_wide_path(path), o, \&m->s)|' "$hs/model.c" > "$obj/model_cpu.c"
[ "$(grep -c 'rbp_wide_path(path)' "$obj/model_cpu.c")" = 1 ] || { echo "helper/model.c changed: its CreateSession call no longer matches helper/win/build.sh's" >&2; exit 1; }
sed 's/|| fail(R->CreateSession(/|| fail(rbp_cuda_options(R, o), why, whyn) || fail(R->CreateSession(/; s|#include "rbsp.h"|#include "rbsp.h"\nOrtStatus *rbp_cuda_options(const OrtApi *, OrtSessionOptions *);|' "$obj/model_cpu.c" > "$obj/model_cuda.c"
[ "$(grep -c 'rbp_cuda_options(R, o)' "$obj/model_cuda.c")" = 1 ] || { echo "helper/model.c changed: the CUDA insertion point no longer matches" >&2; exit 1; }
cc -Dmodel_load_ort=cpu_model_load_ort -Dmodel_open=cpu_model_open -Dmodel_close=cpu_model_close -Dmodel_run=cpu_model_run -c "$obj/model_cpu.c" -o "$obj/model_cpu.o"
cc -Dmodel_load_ort=cuda_model_load_ort -Dmodel_open=cuda_model_open -Dmodel_close=cuda_model_close -Dmodel_run=cuda_model_run -c "$obj/model_cuda.c" -o "$obj/model_cuda.o"
cc -ffp-contract=off -c "$hs/resample.c" -o "$obj/resample.o"          # rekordbox's fused multiply-adds only where it has them
for f in mp3 mp4 manifest entry; do cc -c "$hs/$f.c" -o "$obj/$f.o"; done
for f in decode_win kept_win library_win big_win readable_win glue_win cuda_win; do cc -c "$here/$f.c" -o "$obj/$f.o"; done
# Apple's ALAC decoder: its own warnings aren't ours (multi-character constants, old C++)
for f in ALACBitUtilities EndianPortable ag_dec dp_dec matrix_dec; do "$CC" -O2 -U_FORTIFY_SOURCE -w -I"$alac" -c "$alac/$f.c" -o "$obj/alac_$f.o"; done
"$CXX" -O2 -U_FORTIFY_SOURCE -w -I"$alac" -c "$alac/ALACDecoder.cpp" -o "$obj/alac_ALACDecoder.o"
"$CXX" -O2 -U_FORTIFY_SOURCE -Wall -Wextra -Wno-multichar -I"$alac" -c "$here/alac_win.cpp" -o "$obj/alac_win.o"
"$CXX" "$obj"/*.o "$flac/lib/libFLAC.a" -static-libgcc -static-libstdc++ -lmfplat -lmfreadwrite -lmfuuid -lole32 -lshell32 -ladvapi32 -luuid -o "$out/rbsp-prepare.exe"
echo "built $out/rbsp-prepare.exe"
