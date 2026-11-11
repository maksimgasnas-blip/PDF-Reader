#!/bin/sh
# Скачивает исходники MuPDF 1.24.9 и собирает статические библиотеки под
# 32-битную Windows (mingw32). Нужно один раз перед ./build.sh.
#
#   ./fetch-mupdf.sh              # в /tmp/mupdf
#   WORK=~/mupdf ./fetch-mupdf.sh # в свой каталог
#
# Требования: i686-w64-mingw32-gcc/g++, make, curl или wget, ~3 ГБ памяти
# (при меньшем объёме ОЗУ создайте swap: fallocate -l 3G /swapfile && mkswap
#  /swapfile && swapon /swapfile).

set -e

VER=1.24.9
WORK=${WORK:-/tmp/mupdf}
SRC="$WORK/mupdf-$VER-source"
URL="https://mupdf.com/downloads/archive/mupdf-$VER-source.tar.gz"

mkdir -p "$WORK"

if [ ! -d "$SRC" ]; then
    echo "== скачиваю MuPDF $VER"
    if [ -f "$WORK/mupdf.tar.gz" ]; then
        : # уже скачано
    elif command -v curl >/dev/null 2>&1; then
        curl -L --fail -o "$WORK/mupdf.tar.gz" "$URL"
    else
        wget -O "$WORK/mupdf.tar.gz" "$URL"
    fi
    echo "== распаковываю"
    tar xzf "$WORK/mupdf.tar.gz" -C "$WORK"
fi

# Патч для mingw-w64 12+: в system/fitz/directory.c struct stat не совпадает
# с _wstat (нужен _stat64/_wstat64). Без него сборка падает на directory.o.
if grep -q 'ret = _wstat(wpath, &info);' "$SRC/source/fitz/directory.c" 2>/dev/null; then
    echo "== патчу directory.c (_wstat -> _wstat32)"
    python3 - "$SRC/source/fitz/directory.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("""	struct stat info;
	int ret;

	ret = _wstat(wpath, &info);""", """	struct _stat32 info;
	int ret;

	ret = _wstat32(wpath, &info);""", 1)
open(p, 'w').write(s)
PY
fi

echo "== собираю библиотеки (несколько минут)"
cd "$SRC"
make OS=mingw32-cross build=release libs tesseract=yes -j2

echo
echo "готово: $SRC/build/mingw32-cross/release-tesseract/libmupdf.a"
echo "теперь: MUPDF=$SRC ./build.sh"
