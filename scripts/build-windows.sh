#!/bin/bash
# Builds the Windows release assets into dist/ (next to the macOS ones scripts/build.sh makes):
#   rbstemsplus-windows.zip     the app: RBStemsPlus.exe (single file, self-contained, x64), rbsp-prepare.exe (the prepare helper), NOTICE, LICENSE
#   onnxruntime.dll             the Windows bridge (bridge/win/build.sh)
#   stemsplus-model.onnx        the model: shared with macOS (already in dist/, or RBSTEMSPLUS_MODEL)
#   bootstrap.ps1               the pasted installer (scripts/bootstrap.ps1, the public keys written in)
#   payload-windows.json        versions, file names, SHA-256s, the compatible versions, the commit
#   SHA256SUMS-windows          SHA-256 of all of the above
# payload-windows.json.sig is made by scripts/release.sh --windows (or, for a test build, here).
#
#   scripts/build-windows.sh              a release, into dist/: only from a commit, with
#                                         keys/release.pub.pem and keys/backup.pub.pem
#   scripts/build-windows.sh --test URL   a test build into dist-test/: the app and bootstrap.ps1
#                                         download from URL and trust only the test key
#                                         (build/test-keys/, the same one scripts/build.sh --test makes)
#
# Runs on Linux or macOS. Needs: mingw-w64 (x86_64-w64-mingw32-gcc), Microsoft's .NET 8 SDK (with
# the Windows Desktop SDK: dotnet-install.sh, not a distribution's source-built package), openssl,
# python3, and whatever bridge/win/build.sh needs.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
dist="$root/dist"
test_base=""
if [ "${1:-}" = "--test" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 [--test URL]" >&2; exit 2; }
  test_base="$2"; dist="$root/dist-test"
elif [ $# -ne 0 ]; then
  echo "usage: $0 [--test URL]" >&2; exit 2
fi
version="$(tr -d '[:space:]' < "$root/VERSION")"
die() { echo "build-windows.sh: $*" >&2; exit 1; }
sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
size() { wc -c < "$1" | tr -d ' '; }

commit="$(git -C "$root" rev-parse HEAD)" || die "not a git checkout"
changes="$(git -C "$root" status --porcelain)"
if [ -z "$test_base" ]; then
  [ -z "${RBSTEMS_DUMP_DIR:-}" ] || die "RBSTEMS_DUMP_DIR is set (a research bridge that dumps every chunk): unset it for a release"
  [ -z "$changes" ] || die "uncommitted or untracked files (git status): a release is built only from a commit"
  for k in release backup; do [ -f "$root/keys/$k.pub.pem" ] || die "keys/$k.pub.pem is missing"; done
elif [ -n "$changes" ]; then
  commit="$commit-dirty"
fi

# --- edit for each release ---------------------------------------------------------------------
# the model: the same export as macOS (scripts/build.sh's model_sha256 and model_size)
model_sha256="$(sed -n 's/^model_sha256="\([0-9a-f]\{64\}\)"$/\1/p' "$root/scripts/build.sh")"
model_size="$(sed -n 's/^model_size=\([0-9]*\)$/\1/p' "$root/scripts/build.sh")"
previous_models="$(sed -n 's/^previous_models="\(.*\)"$/\1/p' "$root/scripts/build.sh")"
# payload-windows.json's "compatible" block: the rekordbox versions Stems Cache may be installed on
# (Windows: tested with 7.2.14), the ONNX Runtime the bridge expects (Pioneer's is 1.18.0,
# DirectML), the STEMS Engine versions the model fits
compat_rekordbox="7.2.14"
compat_ort_prefix="1.18."
compat_stems_engine="0002"
# -----------------------------------------------------------------------------------------------
[[ "$model_sha256" =~ ^[0-9a-f]{64}$ ]] && [ -n "$model_size" ] || die "couldn't read model_sha256 and model_size from scripts/build.sh"
grep -qs "$model_sha256" "$root/windows/RBStemsPlus.Core/Paths.cs" || die "the model SHA-256 isn't OurModel in windows/RBStemsPlus.Core/Paths.cs"

mkdir -p "$dist"
app_zip="rbstemsplus-windows.zip" bridge="onnxruntime.dll" model="stemsplus-model.onnx"

pubkey_b64() {
  local text
  text="$(openssl ec -pubin -in "$1" -noout -text 2>/dev/null)" && grep -q 'ASN1 OID: prime256v1' <<< "$text" || return 1
  openssl ec -pubin -in "$1" -outform DER 2>/dev/null | base64 | tr -d '\n'
}
test_keys="$root/build/test-keys"
dotnet_test=()
if [ -n "$test_base" ]; then
  if [ ! -f "$test_keys/test-key.pem" ] || [ ! -f "$test_keys/test.pub.pem" ]; then
    mkdir -p "$test_keys"
    (umask 077 && openssl ecparam -name prime256v1 -genkey -noout -out "$test_keys/test-key.pem") || die "couldn't make the test key"
    openssl ec -in "$test_keys/test-key.pem" -pubout -out "$test_keys/test.pub.pem" 2>/dev/null || die "couldn't write the test public key"
  fi
  trusted="$(pubkey_b64 "$test_keys/test.pub.pem")" || die "build/test-keys/test.pub.pem isn't a P-256 key"
  dotnet_test=(-p:RbspTestKey="$test_keys/test.pub.pem" -p:RbspTestBase="$test_base")
else
  trusted="$(pubkey_b64 "$root/keys/release.pub.pem") $(pubkey_b64 "$root/keys/backup.pub.pem")" || die "keys/*.pub.pem aren't P-256 keys"
fi

echo "== the bridge"
"$root/bridge/win/build.sh" "$root/build/bridge-win" > "$root/build/bridge-win.log" 2>&1 || { tail -20 "$root/build/bridge-win.log"; die "the bridge didn't build"; }
cp "$root/build/bridge-win/$bridge" "$dist/$bridge"
# not part of the signed payload: a diagnostic tool, published next to it (rbsp-bench.exe)
"$root/bridge/win/build.sh" --bench "$dist" > "$root/build/bench-win.log" 2>&1 || { tail -20 "$root/build/bench-win.log"; die "rbsp-bench didn't build"; }

echo "== the prepare helper"
# rbsp-prepare.exe (helper/win/): next to RBStemsPlus.exe in the app's zip (and unzipped into dist, for the CI artifact)
"$root/helper/win/build.sh" "$root/build/helper-win" > "$root/build/helper-win.log" 2>&1 || { tail -20 "$root/build/helper-win.log"; die "rbsp-prepare didn't build"; }
cp "$root/build/helper-win/rbsp-prepare.exe" "$dist/rbsp-prepare.exe"

echo "== the app"
command -v dotnet > /dev/null || die "dotnet not found (Microsoft's .NET 8 SDK)"
pub="$root/build/windows-publish"
rm -rf "$pub"
dotnet publish "$root/windows/RBStemsPlus/RBStemsPlus.csproj" -c Release -o "$pub" -p:RbspVersion="$version" \
  -p:ContinuousIntegrationBuild=true -p:Deterministic=true "${dotnet_test[@]}" > "$root/build/windows-publish.log" 2>&1 \
  || { tail -30 "$root/build/windows-publish.log"; die "dotnet publish failed"; }
[ -f "$pub/RBStemsPlus.exe" ] || die "no RBStemsPlus.exe"
if [ -z "$test_base" ]; then
  if grep -aq "TEST BUILD" "$pub/RBStemsPlus.exe"; then die "a release app contains the test build note"; fi
fi
python3 -I - "$pub/RBStemsPlus.exe" "$root/NOTICE" "$root/LICENSE" "$dist/$app_zip" "$dist/rbsp-prepare.exe" <<'EOF'
import sys, zipfile
exe, notice, lic, out, helper = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for path, name in ((exe, "RBStemsPlus.exe"), (helper, "rbsp-prepare.exe"), (notice, "NOTICE"), (lic, "LICENSE")):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))   # fixed date: the same bytes each build
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        z.writestr(info, open(path, "rb").read())
EOF

echo "== the model"
if [ -f "$dist/$model" ] && [ "$(size "$dist/$model")" = "$model_size" ] && [ "$(sha256 "$dist/$model")" = "$model_sha256" ]; then
  echo "dist/$model is already there"
elif [ -n "${RBSTEMSPLUS_MODEL:-}" ] && [ -f "$RBSTEMSPLUS_MODEL" ]; then
  [ "$(sha256 "$RBSTEMSPLUS_MODEL")" = "$model_sha256" ] || die "$RBSTEMSPLUS_MODEL doesn't have SHA-256 $model_sha256"
  cp "$RBSTEMSPLUS_MODEL" "$dist/$model.part" && mv "$dist/$model.part" "$dist/$model"
else
  # the same file the macOS release publishes: downloaded once, checked against its SHA-256 and size
  url="${RBSTEMSPLUS_MODEL_URL:-https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/$model}"
  echo "downloading the model from $url"
  ok=
  for attempt in 1 2 3 4 5 6; do                       # resumed after a dropped connection
    if curl -fsSL --proto =https --proto-redir =https -C - -o "$dist/$model.part" "$url"; then ok=1; break; fi
    echo "  (interrupted: resuming, attempt $((attempt + 1)))"; sleep $((attempt * 2))
  done
  [ -n "$ok" ] || die "couldn't download the model: put it at dist/$model or set RBSTEMSPLUS_MODEL"
  [ "$(size "$dist/$model.part")" = "$model_size" ] && [ "$(sha256 "$dist/$model.part")" = "$model_sha256" ] \
    || { rm -f "$dist/$model.part"; die "the downloaded model isn't SHA-256 $model_sha256"; }
  mv "$dist/$model.part" "$dist/$model"
fi

echo "== bootstrap.ps1"
[ "$(grep -c '^\$SigningKeys = ""$' "$root/scripts/bootstrap.ps1")" = 1 ] || die "scripts/bootstrap.ps1 needs exactly one \$SigningKeys = \"\" line"
[ "$(grep -c '^\$PayloadMinimum = ""$' "$root/scripts/bootstrap.ps1")" = 1 ] || die "scripts/bootstrap.ps1 needs exactly one \$PayloadMinimum = \"\" line"
[ "$(grep -c '^\$DownloadBase = "https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/"$' "$root/scripts/bootstrap.ps1")" = 1 ] || die "scripts/bootstrap.ps1 needs its \$DownloadBase line"
sed -e "s|^\$SigningKeys = \"\"\$|\$SigningKeys = \"$trusted\"|" -e "s|^\$PayloadMinimum = \"\"\$|\$PayloadMinimum = \"$version\"|" \
    ${test_base:+-e "s|^\$DownloadBase = \".*\"\$|\$DownloadBase = \"$test_base\"|"} \
    "$root/scripts/bootstrap.ps1" > "$dist/bootstrap.ps1"
grep -qF "\$SigningKeys = \"$trusted\"" "$dist/bootstrap.ps1" || die "the keys didn't go into bootstrap.ps1"

echo "== payload-windows.json"
json_list() { local out="" v; for v in $1; do out+="${out:+, }\"$v\""; done; echo "[$out]"; }
cat > "$dist/payload-windows.json" <<EOF
{
  "payload_version": "$version",
  "commit": "$commit",
  "app": {"version": "$version", "zip": "$app_zip", "sha256": "$(sha256 "$dist/$app_zip")"},
  "bridge": {"file": "$bridge", "sha256": "$(sha256 "$dist/$bridge")"},
  "model": {"file": "$model", "sha256": "$model_sha256", "size": $model_size},
  "previous_models": $(json_list "$previous_models"),
  "compatible": {
    "rekordbox": $(json_list "$compat_rekordbox"),
    "ort_version_prefix": "$compat_ort_prefix",
    "stems_engine": $(json_list "$compat_stems_engine")
  }
}
EOF
python3 -I -c 'import json,sys; json.load(open(sys.argv[1]))' "$dist/payload-windows.json" || die "payload-windows.json isn't valid JSON"

echo "== SHA256SUMS-windows"
(cd "$dist" && for f in "$app_zip" "$bridge" "$model" bootstrap.ps1 payload-windows.json; do echo "$(sha256 "$f")  $f"; done > SHA256SUMS-windows)
cat "$dist/SHA256SUMS-windows"

rm -f "$dist/payload-windows.json.sig"
if [ -n "$test_base" ]; then
  openssl dgst -sha256 -sign "$test_keys/test-key.pem" -out "$dist/payload-windows.json.sig" "$dist/payload-windows.json" || die "couldn't sign with the test key"
  openssl dgst -sha256 -verify "$test_keys/test.pub.pem" -signature "$dist/payload-windows.json.sig" "$dist/payload-windows.json" > /dev/null || die "the test signature doesn't verify"
fi
echo "Built RB Stems Plus $version for Windows ($commit) into $dist${test_base:+ (TEST BUILD, downloads from $test_base, signed with the test key)}"
[ -n "$test_base" ] || echo "Not signed: scripts/release.sh --windows signs and uploads it with the macOS release."
