#!/usr/bin/env bash
# Downloads Ghidra and the ghidra-emotionengine-reloaded extension (PS2 support) into
# third_party/ghidra/, checks both archives against tools/setup/ghidra.sha256, and writes the
# language id of the EE processor into config/ghidra_language.txt.
# Running it again downloads and unpacks nothing that is already in place.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

GHIDRA_URL="https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip"
EXT_URL="https://github.com/chaoticgd/ghidra-emotionengine-reloaded/releases/download/v2.1.38/ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip"

DEST="third_party/ghidra"
GHIDRA_ZIP_NAME="ghidra_12.1.4_PUBLIC_20260921.zip"
EXT_ZIP_NAME="ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip"
GHIDRA_TOP="ghidra_12.1.4_PUBLIC"
EXT_TOP="ghidra-emotionengine-reloaded"
GHIDRA_DIR="$DEST/$GHIDRA_TOP"
EXT_PARENT="$GHIDRA_DIR/Ghidra/Extensions"
SUMS="tools/setup/ghidra.sha256"
LANG_FILE="config/ghidra_language.txt"

log() { echo "[fetch_ghidra] $*"; }
die() { echo "[fetch_ghidra] ERROR: $*" >&2; exit 1; }

# download URL OUTPUT: writes to OUTPUT.part and renames it only when curl succeeded
download() {
    local url="$1" out="$2"
    if [[ -f "$out" ]]; then
        log "already downloaded: $out"
        return
    fi
    log "downloading $url"
    curl -L --fail --retry 3 -o "$out.part" "$url"
    mv "$out.part" "$out"
}

# unpack ZIP PARENT TOP: unzips into a temporary folder, then moves PARENT/TOP into place
unpack() {
    local zip="$1" parent="$2" top="$3" tmp
    if [[ -d "$parent/$top" ]]; then
        log "already unpacked: $parent/$top"
        return
    fi
    log "unpacking $zip into $parent"
    mkdir -p "$parent"
    tmp="$(mktemp -d "$parent/.unpack.XXXXXX")"
    unzip -q "$zip" -d "$tmp"
    [[ -d "$tmp/$top" ]] || die "$zip has no top-level folder named '$top'"
    mv "$tmp/$top" "$parent/$top"
    rm -rf "$tmp"
}

mkdir -p "$DEST"
download "$GHIDRA_URL" "$DEST/$GHIDRA_ZIP_NAME"
download "$EXT_URL" "$DEST/$EXT_ZIP_NAME"

# Checksums: created on the first run, verified on every run after that
if [[ ! -f "$SUMS" ]]; then
    log "creating $SUMS"
    (cd "$DEST" && sha256sum "$GHIDRA_ZIP_NAME" "$EXT_ZIP_NAME") > "$SUMS"
fi
log "verifying archives against $SUMS"
(cd "$DEST" && sha256sum --check --strict "$ROOT/$SUMS") || die "checksum mismatch, see above"

unpack "$DEST/$GHIDRA_ZIP_NAME" "$DEST" "$GHIDRA_TOP"
unpack "$DEST/$EXT_ZIP_NAME" "$EXT_PARENT" "$EXT_TOP"

test -x "$GHIDRA_DIR/support/analyzeHeadless" || die "$GHIDRA_DIR/support/analyzeHeadless is not executable"

# Language id of the PS2 processor: the one id in the extension's .ldefs files that contains 5900
shopt -s nullglob
ldefs=("$EXT_PARENT"/*/data/languages/*.ldefs)
shopt -u nullglob
[[ ${#ldefs[@]} -gt 0 ]] || die "no .ldefs files found under $EXT_PARENT/*/data/languages/"

ids="$(grep -rho 'id="[^"]*"' "${ldefs[@]}" | sed 's/^id="//; s/"$//' | sort -u || true)"
log "language ids found in the .ldefs files:"
printf '%s\n' "$ids" | sed 's/^/[fetch_ghidra]   /'

matches="$(printf '%s\n' "$ids" | grep -F 5900 || true)"
count="$(printf '%s\n' "$matches" | grep -c . || true)"
if [[ "$count" != "1" ]]; then
    die "expected exactly one language id containing 5900, found $count (listed above)"
fi

mkdir -p "$(dirname "$LANG_FILE")"
printf '%s\n' "$matches" > "$LANG_FILE"
log "wrote $LANG_FILE: $matches"
log "done"
