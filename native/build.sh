#!/bin/sh
# Сборка PDF-Studio.exe для Windows (32-бит, статически слинковано).
#
#   1) собрать библиотеки MuPDF под mingw:
#        tar xzf mupdf-1.24.9-source.tar.gz -C /tmp
#        cd /tmp/mupdf-1.24.9-source && make OS=mingw32-cross build=release libs USE_TESSERACT=no -j2
#   2) ./build.sh
#
# Результат: release/PDF-Studio.exe — один файл без зависимостей (Windows XP…11).
# Внимание: не собирайте в каталог с именем dist/build/out — такие каталоги
# не попадают в снапшот воркспейса, и готовый exe «пропадёт».

set -e
CROSS=i686-w64-mingw32
MUPDF=${MUPDF:-/tmp/mupdf-1.24.9-source}
LIBDIR=${LIBDIR:-$MUPDF/build/mingw32-cross/release-tesseract}
if [ ! -f "$LIBDIR/libmupdf.a" ]; then LIBDIR="$MUPDF/build/mingw32-cross/release"; fi
OUT=${OUT:-../release}

cd "$(dirname "$0")"
mkdir -p build "$OUT"

$CROSS-windres src/app.rc -O coff -o build/app.res

$CROSS-gcc \
  -O2 -s -municode -mwindows \
  -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
  -fno-asynchronous-unwind-tables \
  -I"$MUPDF/include" \
  -o "$OUT/PDF-Studio.exe" \
  src/main.c src/pdfdoc.c src/pdftext.c src/textedit.c src/flow.c src/wui.c build/app.res \
  "$LIBDIR/libmupdf.a" "$LIBDIR/libmupdf-third.a" \
  -lstdc++ -lcomdlg32 -lcomctl32 -lwinpthread -lgdi32 -luser32 -lshell32 -lole32 -ladvapi32 -lws2_32 \
  -static -static-libgcc -static-libstdc++

# UPX (необязательно, но очень желательно: 40 МБ -> 21 МБ)
if [ "${NOUPX:-0}" != "1" ] && command -v upx >/dev/null 2>&1; then
  upx -q --best --lzma --no-backup "$OUT/PDF-Studio.exe" || echo "UPX не сработал, оставляем несжатый exe"
fi

echo "готово: $OUT/PDF-Studio.exe"
ls -l "$OUT/PDF-Studio.exe"
