# Сторонние компоненты

## MuPDF 1.24.9 (Artifex Software)
- Сайт: https://mupdf.com/
- Лицензия: **GNU Affero General Public License v3.0** (AGPL-3.0)
- Как используется: статически линкуется в локально собранное приложение;
  через него выполняются рендеринг страниц, разбор текста, поиск, аннотации,
  работа с формами, повороты/удаление/вставка страниц, шифрование (AES/RC4),
  сохранение PDF.
- Исходники: архив `mupdf-1.24.9-source.tar.gz` с https://mupdf.com/downloads/archive/
  (в него же входят `thirdparty/`: freetype, harfbuzz, openjpeg, jbig2dec, lcms2,
  mujs, zlib, gumbo и др.). Команда сборки — в `README.md`.

Так как MuPDF распространяется под AGPL-3.0, **собранный бинарник `PDF-Studio.exe`
как целое требует соблюдения AGPL-3.0**: его можно использовать, изучать, изменять
и распространять при выполнении условий лицензии. Исходный код приложения
и скрипты сборки находятся в этом репозитории; архив MuPDF здесь не хранится. При
распространении бинарника необходимо также предоставить соответствующий полный
исходный код MuPDF и выполнить остальные условия лицензии. Это не влияет на
бесплатность для конечного пользователя:
никаких платежей, подписок, ограничений на число файлов или водяных знаков нет.

Код самого приложения лицензирован по MIT — см. `LICENSE`; дополнительные
компоненты и их условия перечислены далее и в исходном архиве MuPDF.

## Зависимости MuPDF
- Архив MuPDF содержит сторонние компоненты в `thirdparty/`, включая FreeType,
  HarfBuzz, OpenJPEG, JBIG2Dec, LCMS2, MuJS, zlib и Gumbo.
- Версии и лицензионные уведомления этих компонентов находятся в архиве исходников.
- При распространении сборки сохраняйте все применимые лицензии и уведомления.

## Tesseract OCR 5.x и Leptonica (Ray Smith / Google и др.)
- Сайт: https://github.com/tesseract-ocr/tesseract , https://github.com/DanBloomberg/leptonica
- Лицензия: **Apache License 2.0**
- Как используется: статически линкуется при сборке MuPDF с Tesseract
  (объекты `tessocr.o`, `ocr-device.o` в `libmupdf.a` и весь tesseract/leptonica
  в `libmupdf-third.a`); распознаёт скан страницы в невидимый текстовый слой.
  Сборка движка: `make OS=mingw32-cross build=release libs tesseract=yes`.

## Языковые модели tessdata_fast (rus, eng)
- Источник: https://github.com/tesseract-ocr/tessdata_fast
- Лицензия: **Apache License 2.0**
- Как используется: файлы `native/tessdata/rus.traineddata` и
  `eng.traineddata` вшиваются в Windows exe как ресурсы RCDATA (200/201) и
  распаковываются в `%TEMP%\PDFStudioOCR` при первом распознавании.

## Прочее
- Иконки, шрифты и демонстрационные PDF находятся в `assets/` и `examples/`;
  сведения о лицензии шрифтов приведены в `assets/fonts/LICENSE.txt`.
