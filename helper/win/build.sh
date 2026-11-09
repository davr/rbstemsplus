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
command -v "$CC" >/dev/null || { echo "helper/win/build.sh: $CC not found (install mingw-w64)" >&2; exit 1; }
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
      --disable-stack-smash-protection --disable-multithreading CC="$CC" CFLAGS="-O2"
    make -s -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" -C src/libFLAC
  ) > "$scratch/build.log" 2>&1 || { tail -30 "$scratch/build.log" >&2; exit 1; }
  mkdir -p "$flac/lib" "$flac/include/FLAC"
  cp "$scratch/flac-$FLAC_VERSION/include/FLAC/"*.h "$flac/include/FLAC/"
  cp "$scratch/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$flac/lib/libFLAC.a"
  rm -rf "$scratch"
fi

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
cflags="${test_ours:+-DRBP_TEST }-O2 -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-cast-function-type -Wno-unused-result -DFLAC__NO_DLL -D_FILE_OFFSET_BITS=32"
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
# model.c: ONNX Runtime takes a UTF-16 path here; main's model_run is glue_win.c's (it waits while paused)
sed 's|R->CreateSession(env, path, o, &m->s)|R->CreateSession(env, rbp_wide_path(path), o, \&m->s)|' "$hs/model.c" > "$obj/model_gen.c"
[ "$(grep -c 'rbp_wide_path(path)' "$obj/model_gen.c")" = 1 ] || { echo "helper/model.c changed: its CreateSession call no longer matches helper/win/build.sh's" >&2; exit 1; }
cc -Dmodel_run=model_run_inner -c "$obj/model_gen.c" -o "$obj/model.o"
cc -ffp-contract=off -c "$hs/resample.c" -o "$obj/resample.o"          # rekordbox's fused multiply-adds only where it has them
for f in mp3 mp4 manifest entry; do cc -c "$hs/$f.c" -o "$obj/$f.o"; done
for f in decode_win kept_win library_win big_win readable_win glue_win; do cc -c "$here/$f.c" -o "$obj/$f.o"; done
"$CC" "$obj"/*.o "$flac/lib/libFLAC.a" -static-libgcc -lmfplat -lmfreadwrite -lmfuuid -lole32 -lshell32 -ladvapi32 -luuid -o "$out/rbsp-prepare.exe"
echo "built $out/rbsp-prepare.exe"
