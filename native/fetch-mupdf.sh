#!/bin/sh
# Download and build MuPDF 1.24.9 for the 32-bit Windows application.
# Requires i686-w64-mingw32 gcc/g++, make, curl or wget, and Python 3.
# Set WORK=/path/to/cache to keep downloaded sources outside this repository.

set -eu

VER=1.24.9
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK=${WORK:-$SCRIPT_DIR}
mkdir -p "$WORK"
WORK=$(CDPATH= cd -- "$WORK" && pwd)
SRC="$WORK/mupdf-$VER-source"
ARCHIVE="$WORK/mupdf.tar.gz"
URL="https://mupdf.com/downloads/archive/mupdf-$VER-source.tar.gz"

if [ ! -d "$SRC" ]; then
    echo "Downloading MuPDF $VER"
    if [ -f "$ARCHIVE" ] && ! tar tzf "$ARCHIVE" >/dev/null 2>&1; then
        rm -f "$ARCHIVE"
    fi
    if [ ! -f "$ARCHIVE" ]; then
        if command -v curl >/dev/null 2>&1; then
            curl -L --fail -o "$ARCHIVE" "$URL"
        elif command -v wget >/dev/null 2>&1; then
            wget -O "$ARCHIVE" "$URL"
        else
            echo "Install curl or wget to download MuPDF." >&2
            exit 1
        fi
    fi
    tar xzf "$ARCHIVE" -C "$WORK"
fi

# mingw-w64's stat type may not match MuPDF's legacy _wstat declaration.
PATCH_FILE="$SRC/source/fitz/directory.c"
if [ ! -f "$PATCH_FILE" ]; then
    echo "MuPDF source archive is incomplete: missing $PATCH_FILE" >&2
    exit 1
fi
if grep -q 'ret = _wstat(wpath, &info);' "$PATCH_FILE" 2>/dev/null; then
    echo "Patching MuPDF directory.c for mingw-w64"
    python3 - "$PATCH_FILE" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    text = f.read()
old = "\tstruct stat info;\n\tint ret;\n\n\tret = _wstat(wpath, &info);"
new = "\tstruct _stat32 info;\n\tint ret;\n\n\tret = _wstat32(wpath, &info);"
if old not in text:
    raise SystemExit("Expected source context not found; refusing to patch unknown MuPDF source")
with open(path, "w", encoding="utf-8", newline="") as f:
    f.write(text.replace(old, new, 1))
PY
fi

echo "Building MuPDF and Tesseract libraries"
cd "$SRC"
make OS=mingw32-cross build=release libs tesseract=yes -j2

echo "MuPDF libraries are ready in: $SRC/build/mingw32-cross/release-tesseract"
echo "Next: run native/build.sh"
