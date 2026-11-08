#!/bin/sh
# Builds the Windows bridge, onnxruntime.dll (x64), with mingw-w64, on Linux or macOS (brew install
# mingw-w64), with libFLAC (no Ogg) built from the official source and linked in statically.
# Exports exactly exports.def: Pioneer's four entry points at Pioneer's ordinals, and rbstems_marker.
#
#   bridge/win/build.sh [OUT]                 release build into OUT (default: build/bridge-win)
#   bridge/win/build.sh --test REAL_PATH OUT  for bridge/win/tests only: the bridge loads the real
#                                             library from REAL_PATH (a Windows path) instead of
#                                             %ProgramData%\rbstemsplus\ort\onnxruntime.dll
#   bridge/win/build.sh --unit OUT            bridge/win/tests/unit.c (the bridge's source #included,
#                                             a test build) into OUT/unit.exe
#
# libFLAC's tarball is kept in build/flac/ (gitignored, shared with bridge/build.sh) and checked
# against its sha256 on every build: downloads.xiph.org first, then GitHub's xiph/flac release
# (the same file). libFLAC is built from it every time, in a scratch folder.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

FLAC_VERSION=1.5.0
FLAC_SHA256=f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920
FLAC_URLS="https://downloads.xiph.org/releases/flac/flac-$FLAC_VERSION.tar.xz https://github.com/xiph/flac/releases/download/$FLAC_VERSION/flac-$FLAC_VERSION.tar.xz"
HOST=x86_64-w64-mingw32
CC="${CC_WIN:-$HOST-gcc}"
command -v "$CC" > /dev/null || { [ -z "${CC_WIN:-}" ] && command -v gcc > /dev/null && gcc -dumpmachine | grep -q "^$HOST" && CC=gcc; } || true   # MSYS2's MINGW64 shell
LIB=onnxruntime.dll

test_path=
unit=
if [ "${1:-}" = "--unit" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 --unit OUT" >&2; exit 2; }
  unit=1
  test_path='Z:\nonexistent\onnxruntime.dll'
  shift
elif [ "${1:-}" = "--test" ]; then
  [ $# -eq 3 ] || { echo "usage: $0 --test REAL_PATH OUT" >&2; exit 2; }
  test_path="$2"
  case "$test_path" in *'"'*) echo "REAL_PATH can't contain \"" >&2; exit 2 ;; esac
  shift 2
fi
out="${1:-$root/build/bridge-win}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
command -v "$CC" >/dev/null || { echo "bridge/win/build.sh: $CC not found (install mingw-w64)" >&2; exit 1; }

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# ---------------------------------------------------------------- libFLAC

flac_root="$root/build/flac"
tarball="$flac_root/flac-$FLAC_VERSION.tar.xz"
mkdir -p "$flac_root"
if [ ! -f "$tarball" ] || [ "$(sha256 "$tarball")" != "$FLAC_SHA256" ]; then
  ok=
  for url in $FLAC_URLS; do
    echo "downloading $url"
    if curl -fsSL -o "$tarball.part" "$url" && [ "$(sha256 "$tarball.part")" = "$FLAC_SHA256" ]; then ok=1; break; fi
    rm -f "$tarball.part"
  done
  [ -n "$ok" ] || { echo "flac tarball: no download matched sha256 $FLAC_SHA256" >&2; exit 1; }
  mv -f "$tarball.part" "$tarball"
fi

jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/rbstems-flac-win.XXXXXX")"
trap 'rm -rf "${scratch:?}"' EXIT
prefix="$scratch/flac"
src="$scratch/src"
mkdir -p "$src" "$prefix/lib" "$prefix/include/FLAC"
tar -xJf "$tarball" -C "$src"
echo "building libFLAC $FLAC_VERSION for $HOST"
(
  cd "$src/flac-$FLAC_VERSION"
  ./configure --quiet --host="$HOST" \
    --enable-static --disable-shared --disable-ogg --disable-programs --disable-examples \
    --disable-cpplibs --disable-doxygen-docs --disable-version-from-git --disable-stack-smash-protection \
    --disable-multithreading \
    CC="$CC" CFLAGS="-O2"
  make -s -j"$jobs" -C src/libFLAC
) > "$flac_root/build-win.log" 2>&1 || { tail -30 "$flac_root/build-win.log" >&2; exit 1; }
cp "$src/flac-$FLAC_VERSION/include/FLAC/"*.h "$prefix/include/FLAC/"
cp "$src/flac-$FLAC_VERSION/src/libFLAC/.libs/libFLAC.a" "$prefix/lib/libFLAC.a"

# ---------------------------------------------------------------- the bridge

defines="-DFLAC__NO_DLL"
note="release"
if [ -n "$test_path" ]; then
  # the C string literal of the path: backslashes doubled
  lit="$(printf '%s' "$test_path" | sed 's/\\/\\\\/g')"
  defines="$defines -DRBSTEMS_TEST_REAL_PATH=\"$lit\""
  note="TEST: real library from $test_path"
fi
if [ -n "$unit" ]; then
  # shellcheck disable=SC2086
  "$CC" -O1 -Wall -Wextra -Wno-cast-function-type -Wno-unused-function -std=gnu11 \
    $defines -I"$here" -I"$here/.." -I"$prefix/include" "$here/tests/unit.c" "$prefix/lib/libFLAC.a" \
    -static-libgcc -lshell32 -lole32 -ladvapi32 -luuid -o "$out/unit.exe"
  echo "built $out/unit.exe"
  exit 0
fi
# shellcheck disable=SC2086
"$CC" -shared -O2 -Wall -Wextra -Wno-cast-function-type -Wno-unused-function -std=gnu11 \
  $defines -I"$here" -I"$here/.." -I"$prefix/include" \
  "$here/rbstems_bridge_win.c" "$here/exports.def" "$prefix/lib/libFLAC.a" \
  -static-libgcc -Wl,--no-insert-timestamp -Wl,--enable-stdcall-fixup \
  -lshell32 -lole32 -ladvapi32 -luuid \
  -o "$out/$LIB"

# ---------------------------------------------------------------- checks

fail() { echo "bridge/win/build.sh: $*" >&2; exit 1; }
lib="$out/$LIB"
# the PE tables themselves (pe_tables.py): objdump's text output differs between binutils versions
tables="$(python3 "$here/pe_tables.py" "$lib" | tr -d '\r')" || fail "couldn't read the DLL's PE tables"
echo "$tables" | grep -qx "machine x64" || fail "not an x64 DLL"
# exports and their ordinals, exactly as exports.def says (rekordbox.exe and Demucs3.dll import by ordinal)
got="$(echo "$tables" | sed -n 's/^export //p' | LC_ALL=C sort | tr '\n' ' ')"
want="1 OrtGetApiBase 2 OrtSessionOptionsAppendExecutionProviderEx_DML 3 OrtSessionOptionsAppendExecutionProvider_CPU 4 OrtSessionOptionsAppendExecutionProvider_DML 5 rbstems_marker "
[ "$got" = "$want" ] || fail "exports: $got (expected $want)"
# imports: system DLLs only (no libgcc, libwinpthread, libFLAC DLLs)
for d in $(echo "$tables" | sed -n 's/^import //p' | sort -u); do
  case "$d" in kernel32.dll|advapi32.dll|shell32.dll|ole32.dll|msvcrt.dll|ucrtbase.dll|api-ms-win-crt-*.dll|ntdll.dll|user32.dll) ;;
    *) fail "links $d" ;; esac
done
grep -aq "rbstems bridge: " "$lib" || fail "the \"rbstems bridge: \" marker string is missing"
grep -aq "rbstems-cache: rebuild=" "$lib" || fail "the capabilities string is missing"
if [ -z "$test_path" ]; then
  if grep -aq "TEST BUILD" "$lib"; then fail "a release build contains the test path override"; fi
  if grep -aq "RBSTEMS_TEST_" "$lib"; then fail "a release build reads test settings from the environment"; fi
fi
printf "built %s (x64, %s)\n" "$lib" "$note"
