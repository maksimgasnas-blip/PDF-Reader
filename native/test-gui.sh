#!/bin/sh
# Дымовой тест GUI под wine + Xvfb: открыть PDF, переключить вкладки,
# открыть панель «Файл», снять скриншоты.
#
#   ./test-gui.sh [файл.pdf] [папка-для-снимков]
#
# Требуется: wine (32-бит), xvfb, imagemagick, xdotool.
# Клавиатура под wine не доставляется, поэтому тест только «мышиный».
set -e
PDF="${1:-examples/demo.pdf}"
OUTDIR="${2:-/tmp/shots}"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-pdfstudio}"
export WINEDEBUG=-all
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg}"
mkdir -p "$OUTDIR" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cd "$(dirname "$0")/.."

DISP=:88
Xvfb $DISP -screen 0 1280x900x24 >/dev/null 2>&1 &
XV=$!
sleep 3
export DISPLAY=$DISP

wine release/PDF-Studio.exe "Z:\\$(pwd | sed 's|/|\\|g')\\$PDF" >/dev/null 2>&1 &
sleep 32
import -window root "$OUTDIR/01-main.png"

xdotool mousemove 260 48 click 1; sleep 2     # вкладка «ВСТАВКА»
import -window root "$OUTDIR/02-insert.png"
xdotool mousemove 108 48 click 1; sleep 2     # назад на «ГЛАВНАЯ»
xdotool mousemove 40 48 click 1; sleep 2      # «Файл»
import -window root "$OUTDIR/03-backstage.png"
xdotool key Escape; sleep 1

wineserver -k >/dev/null 2>&1 || true
kill $XV 2>/dev/null || true
echo "снимки в $OUTDIR:"
ls "$OUTDIR"
