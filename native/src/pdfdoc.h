/* Обёртка над MuPDF: открытие, рендер страницы в HBITMAP, поиск, аннотации, сохранение */
#ifndef PDFDOC_H
#define PDFDOC_H

#include <windows.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#define PDF_OK              0
#define PDF_ERR             1
#define PDF_NEED_PASSWORD   2

/* типы правок, создаваемых из интерфейса */
#define ANNO_NOTE       0   /* текстовое примечание на полях */
#define ANNO_HIGHLIGHT  1   /* маркер */
#define ANNO_RECT       2   /* рамка */
#define ANNO_FREETEXT   3   /* текст поверх страницы */
#define ANNO_INK        4   /* от руки */

typedef struct {
    fz_context  *ctx;
    fz_document *doc;
    pdf_document *pdf;
    char        *path;      /* UTF-8 путь к файлу */
    int          dirty;     /* были ли правки, не сохранённые на диск */
    fz_font     *ui_font;   /* шрифт для нового текста (Noto Sans, есть кириллица) */
    fz_font     *base_font[3][4]; /* базовые шрифты: 0 без засечек, 1 с засечками,
                                     2 моно; индекс [bold*2+italic] */
    fz_font     *metrics_font;    /* шрифт, которым сейчас меряется ширина текста */
    wchar_t      tempCopy[MAX_PATH];  /* временная ASCII-копия исходника (или "") */
} PdfDoc;

int  pdfdoc_open(PdfDoc *d, const wchar_t *wpath);
int  pdfdoc_auth(PdfDoc *d, const char *password);
void pdfdoc_close(PdfDoc *d);

int  pdfdoc_pages(PdfDoc *d);
void pdfdoc_page_size(PdfDoc *d, int page, float *w, float *h);

/* scale — пикселей на пункт (1.0 = 72 dpi, для 96 dpi нужно 96/72) */
int  pdfdoc_render(PdfDoc *d, int page, float scale, HBITMAP *out, int *ow, int *oh);
/* то же, но без текстового слоя — чистый фон страницы */
int  pdfdoc_render_notext(PdfDoc *d, int page, float scale, HBITMAP *out, int *ow, int *oh);

/* сколько вхождений найдено на странице; hits — прямоугольники в координатах страницы */
int  pdfdoc_search_page(PdfDoc *d, int page, const char *needle, fz_rect *hits, int max);

/* добавить правку; r — прямоугольник в координатах страницы, pts/npts — для пера */
int  pdfdoc_annot_add(PdfDoc *d, int page, int kind, fz_rect r,
                      const fz_point *pts, int npts, const char *text);
/* удалить правку, лежащую под точкой; возвращает 1, если что-то удалено */
int  pdfdoc_annot_delete_at(PdfDoc *d, int page, fz_point pt);
/* есть ли правки на странице */
int  pdfdoc_page_has_annots(PdfDoc *d, int page);

/* ---------- операции со страницами ---------- */
int  pdfdoc_page_rotate(PdfDoc *d, int page, int delta);   /* delta = +90 / -90 / 180 */
int  pdfdoc_page_delete(PdfDoc *d, int page);              /* последнюю страницу удалить нельзя */
int  pdfdoc_page_duplicate(PdfDoc *d, int page);
int  pdfdoc_page_move(PdfDoc *d, int page, int delta);      /* delta = -1 вверх, +1 вниз */

/* ---------- поля формы ---------- */
/* сведения о поле под точкой (координаты страницы) */
typedef struct {
    int  type;            /* pdf_widget_type: 7 = текстовое, 2 = флажок, 5 = переключатель, ... */
    int  readonly;
    char name[256];       /* имя поля (/T) */
    char value[1024];     /* текущее значение (UTF-8) */
    char on[128];         /* имя включённого состояния для флажка/переключателя */
} PdfWidget;

int  pdfdoc_widget_at(PdfDoc *d, int page, fz_point pt, PdfWidget *out);  /* 1 — поле найдено */
/* установить значение: для флажков/переключателей utf8 == NULL — переключить */
int  pdfdoc_widget_set(PdfDoc *d, int page, fz_point pt, const char *utf8);

/* выделение: текст в прямоугольнике (malloc, освобождать free) и рамки строк для подсветки */
char *pdfdoc_text_in_rect(PdfDoc *d, int page, fz_rect sel, fz_rect *boxes, int maxBoxes, int *nBoxes);

/* ---------- текстовый слой: правка «как в Word» ---------- */
typedef struct {
    fz_rect      box;          /* область блока в координатах страницы */
    float        fontSize;     /* кегль */
    float        leading;      /* межстрочный интервал */
    float        baseline;     /* y первой базовой линии (координаты страницы, y вниз) */
    float        x0;           /* левый край */
    int          bold, italic;
    unsigned int color;        /* 0xRRGGBB */
    char         font[64];     /* имя исходного шрифта */
    char        *text;         /* текст блока (UTF-8, \n между строками), malloc */
    unsigned     bg;           /* средний цвет фона под блоком (0x00BBGGRR), 0 = белый */
    unsigned     bgc[4];       /* цвета четырёх углов: ЛВ, ПВ, ЛН, ПН (для градиента) */
    int          nofill;       /* 1 — фон неоднородный (фото/градиент): не закрашивать */
} PdfTextBlock;

fz_font *pdfdoc_ui_font(PdfDoc *d);
/* семейство исходного шрифта: 0 — без засечек, 1 — с засечками, 2 — моно */
int  pdfdoc_block_family(const PdfTextBlock *blk);
/* шрифт для текста правки: то же семейство/начертание, что в исходнике */
fz_font *pdfdoc_text_font(PdfDoc *d, const PdfTextBlock *blk);
/* какой шрифт меряет ширину (NULL — обычный, Noto Sans) */
void pdfdoc_metrics_set(PdfDoc *d, fz_font *f);
int   pdfdoc_text_block_at(PdfDoc *d, int page, fz_point pt, PdfTextBlock *out);
float pdfdoc_cp_width(PdfDoc *d, int cp, float size);
/* кегль, при котором исходные строки блока не расползаются по ширине */
float pdfdoc_fit_size(PdfDoc *d, const int *cp, int n, float size, float width);
int   pdfdoc_wrap_text(PdfDoc *d, const int *cp, int n, float size, float width,
                       int *lineStart, int *lineEnd, int maxLines);
char *pdfdoc_cp_to_utf8(const int *cp, int n);
int  *pdfdoc_utf8_to_cp(const char *s, int *outCount);
/* запечь правку: убрать старый текст в области и нарисовать новый */
int   pdfdoc_text_apply(PdfDoc *d, int page, const PdfTextBlock *blk);
/* OCR страницы: невидимый текстовый слой; outChars — сколько символов */
int   pdfdoc_ocr_page(PdfDoc *d, int page, const char *lang, const char *datadir, int *outChars);

/* текст страницы в UTF-8 (malloc, освобождать free) */
char *pdfdoc_page_text(PdfDoc *d, int page);

int  pdfdoc_save(PdfDoc *d, const wchar_t *wpath);

/* ---------- обход ограничения CRT: файлы с русскими именами ----------
   _wfopen из mingw/msvcrt в локали «C» не открывает имена с символами вне
   ANSI-кодировки. Поэтому для таких путей работаем через временный файл с
   ASCII-именем, а переносим на место средствами Win32 (они Unicode-полные). */
int  pdfdoc_path_ascii(const wchar_t *path);
int  pdfdoc_temp_path(wchar_t *out, int outCount, const wchar_t *ext);
int  pdfdoc_move_file(const wchar_t *src, const wchar_t *dst);

#endif
