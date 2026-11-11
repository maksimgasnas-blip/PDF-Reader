# Сторонние компоненты

## MuPDF 1.24.9 (Artifex Software)
- Сайт: https://mupdf.com/
- Лицензия: **GNU Affero General Public License v3.0** (AGPL-3.0)
- Как используется: статически слинкован в `release/PDF-Studio.exe`;
  через него выполняются рендеринг страниц, разбор текста, поиск, аннотации,
  работа с формами, повороты/удаление/вставка страниц, шифрование (AES/RC4),
  сохранение PDF.
- Исходники: архив `mupdf-1.24.9-source.tar.gz` с https://mupdf.com/downloads/archive/
  (в него же входят `thirdparty/`: freetype, harfbuzz, openjpeg, jbig2dec, lcms2,
  mujs, zlib, gumbo и др.). Команда сборки — в `README.md`.

Так как MuPDF распространяется под AGPL-3.0, **собранный бинарник `PDF-Studio.exe`
как целое распространяется под AGPL-3.0**: его можно свободно использовать,
изучать, изменять и распространять, а при распространении нужно предоставлять
исходный код (он лежит в `native/src` рядом с командами сборки) и сохранять
ту же лицензию. Это не влияет на бесплатность для конечного пользователя:
никаких платежей, подписок, ограничений на число файлов или водяных знаков нет.

Код самого приложения (то, что не является частью MuPDF) лицензирован по MIT —
см. `LICENSE`; при распространении бинарника действует AGPL-3.0, как описано выше.

## Tesseract OCR 5.x и Leptonica (Ray Smith / Google и др.)
- Сайт: https://github.com/tesseract-ocr/tesseract , https://github.com/DanBloomberg/leptonica
- Лицензия: **Apache License 2.0**
- Как используется: статически слинкован в `release/PDF-Studio.exe`
  (объекты `tessocr.o`, `ocr-device.o` в `libmupdf.a` и весь tesseract/leptonica
  в `libmupdf-third.a`); распознаёт скан страницы в невидимый текстовый слой.
  Сборка движка: `make OS=mingw32-cross build=release libs tesseract=yes`.

## Языковые модели tessdata_fast (rus, eng)
- Источник: https://github.com/tesseract-ocr/tessdata_fast
- Лицензия: **Apache License 2.0**
- Как используется: файлы `native/tessdata/rus.traineddata` (3,86 МБ) и
  `eng.traineddata` (4,11 МБ) вшиты в exe как ресурсы RCDATA (200/201) и
  распаковываются в `%TEMP%\PDFStudioOCR` при первом распознавании —
  ничего скачивать и устанавливать не нужно.

## Прочее
- Иконка `assets/icon.ico` — собственная, сгенерирована скриптом.
- Примеры PDF в `examples/` созданы для тестов (`contract.pdf` — через fpdf2).
