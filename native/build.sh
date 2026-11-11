#!/bin/sh
# Build the statically linked 32-bit Windows application.
# Usage: ./fetch-mupdf.sh && ./build.sh

set -eu

CROSS=${CROSS:-i686-w64-mingw32}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MUPDF=${MUPDF:-$SCRIPT_DIR/mupdf-1.24.9-source}
OUT=${OUT:-$REPO_DIR/release}
LIBDIR=${LIBDIR:-$MUPDF/build/mingw32-cross/release-tesseract}
if [ ! -f "$LIBDIR/libmupdf.a" ]; then
    if [ -f "$MUPDF/build/mingw32-cross/release/libmupdf.a" ]; then
        LIBDIR="$MUPDF/build/mingw32-cross/release"
    fi
fi

if [ ! -f "$LIBDIR/libmupdf.a" ] || [ ! -f "$LIBDIR/libmupdf-third.a" ]; then
    echo "MuPDF static libraries not found in $LIBDIR" >&2
    echo "Run native/fetch-mupdf.sh first or set MUPDF/LIBDIR." >&2
    exit 1
fi

cd "$SCRIPT_DIR"
mkdir -p "$SCRIPT_DIR/build" "$OUT"

"$CROSS-windres" src/app.rc -O coff -o build/app.res

"$CROSS-gcc" \
  -O2 -s -municode -mwindows \
  -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
  -fno-asynchronous-unwind-tables \
  -I"$MUPDF/include" \
  -o "$OUT/PDF-Studio.exe" \
  src/main.c src/pdfdoc.c src/pdftext.c src/textedit.c src/flow.c src/wui.c build/app.res \
  "$LIBDIR/libmupdf.a" "$LIBDIR/libmupdf-third.a" \
  -lstdc++ -lcomdlg32 -lcomctl32 -lwinpthread -lgdi32 -luser32 -lshell32 -lole32 -ladvapi32 -lws2_32 \
  -static -static-libgcc -static-libstdc++

if [ "${NOUPX:-0}" != "1" ] && command -v upx >/dev/null 2>&1; then
  upx -q --best --lzma --no-backup "$OUT/PDF-Studio.exe" || echo "UPX failed; keeping uncompressed executable"
fi

echo "Built: $OUT/PDF-Studio.exe"
ls -lh "$OUT/PDF-Studio.exe"
