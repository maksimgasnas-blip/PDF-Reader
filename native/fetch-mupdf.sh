#!/bin/sh
# Download and build MuPDF 1.24.9 for the 32-bit Windows application.
# Requires i686-w64-mingw32 gcc/g++, make, curl or wget, Python 3 and unzip.
# Set WORK=/path/to/cache to keep downloaded sources outside this repository.

set -eu

VER=1.24.9
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK=${WORK:-$SCRIPT_DIR}
mkdir -p "$WORK"
WORK=$(CDPATH= cd -- "$WORK" && pwd)
SRC="$WORK/mupdf-$VER-source"
ARCHIVE="$WORK/mupdf.tar.gz"
URL="https://github.com/ArtifexSoftware/mupdf.git"

if [ ! -d "$SRC/.git" ]; then
    echo "Cloning MuPDF $VER including pinned third-party submodules"
    GIT_TMP=$(mktemp -d "${TMPDIR:-/tmp}/pdfstudio-mupdf.XXXXXX")
    trap 'rm -rf "$GIT_TMP"' 0 HUP INT TERM
    git clone --depth 1 --branch "$VER" --recurse-submodules "$URL" "$GIT_TMP/source"
    cp -a "$GIT_TMP/source/." "$SRC/"
    rm -rf "$GIT_TMP"
    trap - 0 HUP INT TERM
else
    git -C "$SRC" -c protocol.file.allow=always submodule update --init --recursive
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

# Avoid giant string literals for bundled font resources; embed font binaries
# as object data instead (also avoids the C literal length limit on GCC).
MAKEFILE="$SRC/Makefile"
if ! grep -q 'HAVE_OBJCOPY=yes' "$MAKEFILE"; then
    sed -i '/^ifeq "$(OS)" "mingw32-cross"$/,/^endif$/ s/^  HAVE_WIN32=yes$/  HAVE_WIN32=yes\n  HAVE_OBJCOPY=yes/' "$MAKEFILE"
fi

echo "Building MuPDF and Tesseract libraries"
cd "$SRC"
make OS=mingw32-cross build=release USE_SYSTEM_LIBS=no USE_SYSTEM_ZLIB=no USE_SYSTEM_LCMS2=no OBJCOPY=i686-w64-mingw32-objcopy libs tesseract=yes -j1

# Bundle the exact MuPDF and pinned third-party source used by this build.
tar czf "$ARCHIVE" -C "$WORK" --exclude="mupdf-$VER-source/.git" --exclude="mupdf-$VER-source/build" "mupdf-$VER-source"

echo "MuPDF libraries are ready in: $SRC/build/mingw32-cross/release-tesseract"
echo "MuPDF source archive: $ARCHIVE"
echo "Next: run native/build.sh"
