#!/usr/bin/env bash
# Downloads the PCSX2 v2.8.2 AppImage (used only as a reference emulator, see
# docs/REFERENCE_CAPTURE.md) into third_party/pcsx2-ref/, records its SHA-256 in
# tools/setup/pcsx2_ref.sha256 on the first download, verifies it on every run, and makes it
# executable. Running it again downloads nothing that is already in place.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

APPIMAGE_NAME="pcsx2-v2.8.2-linux-appimage-x64-Qt.AppImage"
URL="https://github.com/PCSX2/pcsx2/releases/download/v2.8.2/$APPIMAGE_NAME"
DEST="third_party/pcsx2-ref"
SUMS="tools/setup/pcsx2_ref.sha256"

log() { echo "[fetch_pcsx2_ref] $*"; }
die() { echo "[fetch_pcsx2_ref] ERROR: $*" >&2; exit 1; }

mkdir -p "$DEST"

if [[ -f "$DEST/$APPIMAGE_NAME" ]]; then
    log "already downloaded: $DEST/$APPIMAGE_NAME"
else
    log "downloading $URL"
    curl -L --fail --retry 3 -o "$DEST/$APPIMAGE_NAME.part" "$URL"
    mv "$DEST/$APPIMAGE_NAME.part" "$DEST/$APPIMAGE_NAME"
fi

# Checksum: created on the first run, verified on every run after that
if [[ ! -f "$SUMS" ]]; then
    log "creating $SUMS"
    (cd "$DEST" && sha256sum "$APPIMAGE_NAME") > "$SUMS"
fi
log "verifying the AppImage against $SUMS"
(cd "$DEST" && sha256sum --check --strict "$ROOT/$SUMS") || die "checksum mismatch, see above"

chmod +x "$DEST/$APPIMAGE_NAME"
test -x "$DEST/$APPIMAGE_NAME" || die "$DEST/$APPIMAGE_NAME is not executable"
log "done"
