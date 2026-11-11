/* textedit.c — правка текста «как в Word» прямо на странице PDF.
 *
 * Возможности: каретка, выделение мышью и Shift+стрелками, ввод/удаление,
 * стрелки/Home/End/PgUp/PgDn, Enter (новый абзац), Ctrl+Z (отмена),
 * автоматический перенос строк по ширине блока, жирный/курсив/кегль
 * наследуются от исходного текста.
 *
 * Правки хранятся списком и применяются к документу при сохранении
 * (см. pdfdoc_text_apply): старый текст вырезается, новый рисуется
 * встроенным шрифтом — в PDF остаётся настоящий текст.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "pdfdoc.h"
#include "textedit.h"

#define TE_MAX_LINES  512
#define TE_UNDO_MAX   32

typedef struct {
    int   page;
    fz_rect box;
    float fontSize, leading;
    float baseline;
    int   bold, italic;
    char  font[64];        /* имя исходного шрифта — чтобы правка рисовалась тем же */
    unsigned int color;
    unsigned int bg;       /* цвет фона под блоком (0 = белый) */
    int nofill;            /* 1 — фон неоднородный, в PDF не закрашивать */
    char *text;            /* UTF-8 */
} TeEdit;

typedef struct {
    int   cap, n;
    int  *cp;
} TeSnapshot;

static struct {
    int          active;
    int          page;
    PdfTextBlock blk;       /* исходный блок (box, шрифт, кегль) */
    int         *cp;
    int          n, cap;
    int          caret, anchor;
    int          prefCol;   /* «желаемая» колонка для стрелок вверх/вниз */
    float        scale;     /* пикселей на пункт */
    int          ox, oy;    /* origin страницы на экране */
    TeSnapshot   undo[TE_UNDO_MAX];
    int          undoTop;
} te;

static void bg_for(int page, const fz_rect *box, TeBgInfo *out);
static COLORREF bg_avg(TeBgInfo *bg);

static TeEdit  g_edits[256];
static int     g_editCount;

/* ---------- список правок ---------- */
int te_edit_count(void) { return g_editCount; }

void te_edit_clear(void)
{
    int i;
    for (i = 0; i < g_editCount; i++) free(g_edits[i].text);
    g_editCount = 0;
}

const PdfTextBlock *te_edit_get(int i);

/* очистить правки для страницы (например, при удалении страницы) */
void te_edit_drop_page(int page)
{
    int i;
    for (i = 0; i < g_editCount; ) {
        if (g_edits[i].page == page) {
            free(g_edits[i].text);
            memmove(&g_edits[i], &g_edits[i + 1], sizeof(TeEdit) * (g_editCount - i - 1));
            g_editCount--;
        } else i++;
    }
}

/* ---------- буфер ---------- */
static void cp_reserve(int need)
{
    if (need <= te.cap) return;
    while (te.cap < need) te.cap = te.cap ? te.cap * 2 : 256;
    te.cp = (int *)realloc(te.cp, sizeof(int) * te.cap);
}

static void undo_push(void)
{
    TeSnapshot *s;
    if (te.undoTop == TE_UNDO_MAX) {
        /* сдвигаем историю */
        int i;
        free(te.undo[0].cp);
        memmove(&te.undo[0], &te.undo[1], sizeof(TeSnapshot) * (TE_UNDO_MAX - 1));
        te.undoTop--;
    }
    s = &te.undo[te.undoTop++];
    s->n = te.n;
    s->cap = te.n + 1;
    s->cp = (int *)malloc(sizeof(int) * s->cap);
    if (s->cp && te.n > 0) memcpy(s->cp, te.cp, sizeof(int) * te.n);
    else if (s->cp) s->cp[0] = 0;
}

static void undo_clear(void)
{
    int i;
    for (i = 0; i < te.undoTop; i++) free(te.undo[i].cp);
    te.undoTop = 0;
}

static int undo_pop(void)
{
    TeSnapshot *s;
    if (te.undoTop == 0) return 0;
    s = &te.undo[--te.undoTop];
    cp_reserve(s->n + 1);
    if (s->n > 0) memcpy(te.cp, s->cp, sizeof(int) * s->n);
    te.n = s->n;
    if (te.caret > te.n) te.caret = te.n;
    if (te.anchor > te.n) te.anchor = te.n;
    free(s->cp);
    return 1;
}

/* ---------- раскладка ---------- */
/* pdfdoc_wrap_text требует PdfDoc; держим указатель на текущий документ */
static PdfDoc *g_doc;
void te_set_doc(PdfDoc *d) { g_doc = d; }

static int layout_ex(int *lineStart, int *lineEnd)
{
    float width;
    if (!te.active || !g_doc) return 0;
    width = te.blk.box.x1 - te.blk.box.x0 - 1.0f;
    if (width < 8.0f) width = 8.0f;
    return pdfdoc_wrap_text(g_doc, te.cp, te.n, te.blk.fontSize, width,
                            lineStart, lineEnd, TE_MAX_LINES);
}

/* позиция каретки: строка и x (в пунктах от левого края блока) */
static void caret_pos(int *outLine, float *outX)
{
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i, k;
    int idx = te.caret;

    lines = layout_ex(ls, le);
    for (i = 0; i < lines; i++) {
        if (idx <= le[i] || i == lines - 1) {
            float x = 0;
            for (k = ls[i]; k < idx && k < le[i]; k++)
                x += pdfdoc_cp_width(g_doc, te.cp[k], te.blk.fontSize);
            *outLine = i;
            *outX = x;
            return;
        }
    }
    *outLine = 0;
    *outX = 0;
}

/* индекс символа по координате (пункты от левого края блока, сверху) */
static int hit_index(float px, float py)
{
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i, k;
    float lead = te.blk.leading > 0 ? te.blk.leading : te.blk.fontSize * 1.2f;

    lines = layout_ex(ls, le);
    i = (int)(py / lead);
    if (i < 0) i = 0;
    if (i >= lines) i = lines - 1;

    for (k = ls[i]; k < le[i]; k++) {
        float w = pdfdoc_cp_width(g_doc, te.cp[k], te.blk.fontSize);
        if (px < w * 0.5f) return k;
        px -= w;
    }
    return le[i];
}

/* ---------- API редактора ---------- */
int te_is_active(void) { return te.active; }
int te_active_page(void) { return te.page; }

int te_begin(PdfDoc *d, int page, float pageX, float pageY, float scale, int ox, int oy)
{
    PdfTextBlock blk;
    fz_point pt;
    int n = 0;

    if (!d || !d->doc) return 0;
    te_set_doc(d);
    pt.x = pageX;
    pt.y = pageY;
    if (!pdfdoc_text_block_at(d, page, pt, &blk)) return 0;
    {
        TeBgInfo bi;
        bg_for(page, &blk.box, &bi);
        blk.bg = (unsigned)bg_avg(&bi);
        blk.nofill = 1;   /* ТЕСТ: не заливать никогда */
    }

    te.active = 1;
    te.page = page;
    te.blk = blk;
    te.scale = scale;
    te.ox = ox;
    te.oy = oy;

    free(te.cp);
    te.cp = pdfdoc_utf8_to_cp(blk.text, &n);
    te.n = n;
    te.cap = n + 64;
    /* Кегль и шрифт исходника не меняем: правка рисуется тем же семейством
       (Times/Arial/Courier). Иначе абзац после клика выглядел «другим
       шрифтом» и мельче остального текста. */
    pdfdoc_metrics_set(d, pdfdoc_text_font(d, &te.blk));
    te.caret = te.n;
    te.anchor = te.caret;
    te.prefCol = -1;
    undo_clear();
    return 1;
}

/* вернуть текст правки в UTF-8 (malloc) */
char *te_text_utf8(void)
{
    return pdfdoc_cp_to_utf8(te.cp, te.n);
}

/* сохранить текущее состояние в список правок */
int te_commit(void)
{
    TeEdit *e;
    int i;

    if (!te.active) return 0;
    te.active = 0;
    undo_clear();
    pdfdoc_metrics_set(g_doc, NULL);

    /* ищем уже существующую правку этого блока */
    e = NULL;
    for (i = 0; i < g_editCount; i++)
        if (g_edits[i].page == te.page &&
            g_edits[i].box.x0 == te.blk.box.x0 && g_edits[i].box.y0 == te.blk.box.y0)
            e = &g_edits[i];

    if (!e) {
        if (g_editCount >= (int)(sizeof(g_edits) / sizeof(g_edits[0]))) return 0;
        e = &g_edits[g_editCount++];
        memset(e, 0, sizeof(*e));
    } else {
        free(e->text);
    }
    e->page = te.page;
    e->box = te.blk.box;
    e->fontSize = te.blk.fontSize;
    e->baseline = te.blk.baseline;
    e->leading = te.blk.leading;
    e->bold = te.blk.bold;
    e->italic = te.blk.italic;
    memcpy(e->font, te.blk.font, sizeof(e->font));
    e->color = te.blk.color;
    e->bg = te.blk.bg;
    e->nofill = te.blk.nofill;
    e->text = pdfdoc_cp_to_utf8(te.cp, te.n);
    return 1;
}

void te_cancel(void)
{
    if (!te.active) return;
    te.active = 0;
    undo_clear();
    pdfdoc_metrics_set(g_doc, NULL);
}

/* ---------- ввод ---------- */
int te_insert(int ch)
{
    if (!te.active) return 0;
    if (te.anchor != te.caret) {
        int a = te.anchor < te.caret ? te.anchor : te.caret;
        int b = te.anchor < te.caret ? te.caret : te.anchor;
        undo_push();
        memmove(&te.cp[a], &te.cp[b], sizeof(int) * (te.n - b));
        te.n -= (b - a);
        te.caret = a;
        te.anchor = a;
    } else {
        undo_push();
    }
    cp_reserve(te.n + 2);
    memmove(&te.cp[te.caret + 1], &te.cp[te.caret], sizeof(int) * (te.n - te.caret));
    te.cp[te.caret] = ch;
    te.caret++;
    te.n++;
    te.anchor = te.caret;
    te.prefCol = -1;
    return 1;
}

static void delete_range(int a, int b)
{
    memmove(&te.cp[a], &te.cp[b], sizeof(int) * (te.n - b));
    te.n -= (b - a);
    te.caret = a;
    te.anchor = a;
}

int te_delete(int back)
{
    if (!te.active) return 0;
    if (te.anchor != te.caret) {
        int a = te.anchor < te.caret ? te.anchor : te.caret;
        int b = te.anchor < te.caret ? te.caret : te.anchor;
        undo_push();
        delete_range(a, b);
        te.prefCol = -1;
        return 1;
    }
    if (back) {
        if (te.caret == 0) return 0;
        undo_push();
        delete_range(te.caret - 1, te.caret);
    } else {
        if (te.caret >= te.n) return 0;
        undo_push();
        delete_range(te.caret, te.caret + 1);
    }
    te.prefCol = -1;
    return 1;
}

int te_undo(void)
{
    if (!te.active) return 0;
    return undo_pop();
}

void te_select_all(void)
{
    if (!te.active) return;
    te.anchor = 0;
    te.caret = te.n;
}

void te_set_caret_index(int idx, int extend)
{
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i, k;
    if (idx < 0) idx = 0;
    if (idx > te.n) idx = te.n;
    te.caret = idx;
    if (!extend) te.anchor = idx;
    te.prefCol = -1;
    /* пересчитать «желаемую» колонку */
    lines = layout_ex(ls, le);
    for (i = 0; i < lines; i++) {
        if (idx >= ls[i] && idx <= le[i]) {
            float x = 0;
            for (k = ls[i]; k < idx; k++) x += pdfdoc_cp_width(g_doc, te.cp[k], te.blk.fontSize);
            te.prefCol = (int)x;
            break;
        }
    }
}

/* навигация: 0 влево/вправо, 1 вверх/вниз, 2 home/end */
int te_move(int dir, int extend)
{
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i, cur, col;
    if (!te.active) return 0;

    if (dir == 0) {                        /* влево/вправо */
        int step = extend ? 1 : 1;
        (void)step;
        if (!extend && te.anchor != te.caret) {
            te.caret = te.anchor < te.caret ? te.anchor : te.caret;
            return 1;
        }
        return 0;                          /* знак обрабатывается в te_move_h */
    }
    lines = layout_ex(ls, le);
    (void)lines;
    cur = 0;
    for (i = 0; i < TE_MAX_LINES; i++) { (void)i; break; }
    (void)cur; (void)col;
    return 0;
}

/* горизонтальное движение (dc = -1 влево, +1 вправо) */
int te_move_h(int dc, int extend)
{
    if (!te.active) return 0;
    if (!extend && te.anchor != te.caret) {
        te.caret = dc < 0 ? (te.anchor < te.caret ? te.anchor : te.caret)
                          : (te.anchor > te.caret ? te.anchor : te.caret);
        te.anchor = te.caret;
        return 1;
    }
    if (dc < 0 && te.caret > 0) te.caret--;
    else if (dc > 0 && te.caret < te.n) te.caret++;
    if (!extend) te.anchor = te.caret;
    te.prefCol = -1;
    return 1;
}

/* вертикальное движение (dc = -1 вверх, +1 вниз) с сохранением колонки */
int te_move_v(int dc, int extend)
{
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i, line = 0;
    float y, x;
    int idx;

    if (!te.active) return 0;
    lines = layout_ex(ls, le);
    for (i = 0; i < lines; i++)
        if (te.caret >= ls[i] && te.caret <= le[i]) { line = i; break; }

    if (te.prefCol < 0) {
        float xx = 0;
        for (i = ls[line]; i < te.caret; i++) xx += pdfdoc_cp_width(g_doc, te.cp[i], te.blk.fontSize);
        te.prefCol = (int)xx;
    }
    line += dc;
    if (line < 0) line = 0;
    if (line >= lines) line = lines - 1;

    /* по x ищем ближайший индекс в строке */
    x = (float)te.prefCol;
    idx = le[line];
    for (i = ls[line]; i < le[line]; i++) {
        float w = pdfdoc_cp_width(g_doc, te.cp[i], te.blk.fontSize);
        if (x < w * 0.5f) { idx = i; break; }
        x -= w;
    }
    te.caret = idx;
    if (!extend) te.anchor = idx;
    (void)y;
    return 1;
}

/* Home/End строки (whole = весь текст) */
int te_home_end(int end, int whole, int extend)
{
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i;
    if (!te.active) return 0;
    if (whole) {
        te.caret = end ? te.n : 0;
    } else {
        lines = layout_ex(ls, le);
        for (i = 0; i < lines; i++)
            if (te.caret >= ls[i] && te.caret <= le[i]) {
                te.caret = end ? le[i] : ls[i];
                break;
            }
    }
    if (!extend) te.anchor = te.caret;
    te.prefCol = -1;
    return 1;
}

/* клик по странице (координаты в пунктах) — поставить каретку / начать выделение */
void te_place_caret(float pageX, float pageY, int extend)
{
    if (!te.active) return;
    te_set_caret_index(hit_index(pageX - te.blk.box.x0, pageY - te.blk.box.y0), extend);
}

/* попал ли клик в редактируемый блок (в координатах страницы) */
int te_hit_block(float pageX, float pageY)
{
    if (!te.active) return 0;
    return (pageX >= te.blk.box.x0 - 4 && pageX <= te.blk.box.x1 + 4 &&
            pageY >= te.blk.box.y0 - 4 && pageY <= te.blk.box.y1 + 4) ? 1 : 0;
}

void te_set_origin(float scale, int ox, int oy)
{
    te.scale = scale;
    te.ox = ox;
    te.oy = oy;
}

/* ---------- отрисовка ---------- */
static HFONT make_gdi_font(int family, int bold, int italic, float size, float scale)
{
    LOGFONTW lf;
    int h = (int)(size * scale + 0.5f);
    if (h < 6) h = 6;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -h;
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)(italic ? 1 : 0);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY;
    lstrcpyW(lf.lfFaceName, family == 1 ? L"Times New Roman"
                       : family == 2 ? L"Courier New" : L"Arial");
    return CreateFontIndirectW(&lf);
}

/* нарисовать строку из cp[] с точной межсимвольной разбивкой */
static void draw_line(HDC dc, const int *cp, int from, int to, float x, float y,
                      float size, float scale, int ox, int oy)
{
    int i;
    int px = ox + (int)(x * scale + 0.5f);
    int py = oy + (int)((y - size * 0.80f) * scale + 0.5f);  /* y — базовая линия */
    for (i = from; i < to; i++) {
        wchar_t wbuf[2];
        int adv;
        if (cp[i] == '\n' || cp[i] == '\r') continue;      /* перевод строки не рисуем */
        if (cp[i] == ' ') { px += (int)(pdfdoc_cp_width(g_doc, ' ', size) * scale + 0.5f); continue; }
        wbuf[0] = (wchar_t)cp[i];
        wbuf[1] = 0;
        TextOutW(dc, px, py, wbuf, 1);
        adv = (int)(pdfdoc_cp_width(g_doc, cp[i], size) * scale + 0.5f);
        px += adv > 0 ? adv : 1;
    }
}

/* ---------- цвет фона под текстом ---------- */
static TeBgFn g_bgFn;

void te_set_bg_provider(TeBgFn fn) { g_bgFn = fn; }

static void bg_for(int page, const fz_rect *box, TeBgInfo *out)
{
    int i;
    out->uniform = 1;
    for (i = 0; i < 4; i++) out->c[i] = RGB(255, 255, 255);
    if (g_bgFn) g_bgFn(page, box, out);
}

/* средний цвет четырёх углов — для сохранения в PDF */
static COLORREF bg_avg(TeBgInfo *bg)
{
    int r = 0, g = 0, b = 0, i;
    for (i = 0; i < 4; i++) {
        r += GetRValue(bg->c[i]); g += GetGValue(bg->c[i]); b += GetBValue(bg->c[i]);
    }
    return RGB(r / 4, g / 4, b / 4);
}

/* тёмный фон — светлая каретка и наоборот */
static COLORREF contrast_color(COLORREF bg)
{
    int lum = GetRValue(bg) * 3 + GetGValue(bg) * 6 + GetBValue(bg);
    return lum > 5 * 128 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

/* Кэш «страница без текста»: даёт настоящий фон под правкой
   (фото, градиент, двухцветный лист) — никакой заливки-заплатки. */
static struct {
    PdfDoc *doc;
    int     page;
    float   scale;
    HBITMAP bmp;
    int     w, h;
} g_nf;

void te_bg_cache_reset(void)
{
    if (g_nf.bmp) { DeleteObject(g_nf.bmp); }
    g_nf.bmp = NULL; g_nf.doc = NULL; g_nf.page = -1; g_nf.scale = 0; g_nf.w = g_nf.h = 0;
}

static HBITMAP nf_page_bitmap(int page, float scale)
{
    if (!g_doc) return NULL;
    if (g_nf.bmp && g_nf.doc == g_doc && g_nf.page == page &&
        g_nf.scale > scale - 0.01f && g_nf.scale < scale + 0.01f)
        return g_nf.bmp;
    if (g_nf.bmp) { DeleteObject(g_nf.bmp); g_nf.bmp = NULL; }
    g_nf.doc = NULL; g_nf.page = -1; g_nf.scale = 0; g_nf.w = g_nf.h = 0;
    {
        HBITMAP b = NULL;
        int w = 0, h = 0;
        if (!pdfdoc_render_notext(g_doc, page, scale, &b, &w, &h)) return NULL;
        g_nf.doc = g_doc; g_nf.page = page; g_nf.scale = scale;
        g_nf.bmp = b; g_nf.w = w; g_nf.h = h;
        return b;
    }
}

/* Заливка прямоугольника градиентом по четырём углам.
   Делаем крошечный DIB 2x2 с цветами углов и растягиваем его в HALFTONE —
   GDI сам интерполирует, получается мягкий градиент без «заплатки». */
static void fill_gradient(HDC dc, RECT *r, const COLORREF c[4])
{
    BITMAPINFO bi;
    HDC mem;
    HGDIOBJ old;
    DWORD *px;
    int saved;

    if (r->right - r->left < 1 || r->bottom - r->top < 1) return;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 2;
    bi.bmiHeader.biHeight = -2;              /* сверху вниз */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mem = CreateCompatibleDC(dc);
    if (!mem) return;
    {
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        if (!bmp) { DeleteDC(mem); return; }
        px[0] = RGB(GetBValue(c[0]), GetGValue(c[0]), GetRValue(c[0]));  /* ЛВ */
        px[1] = RGB(GetBValue(c[1]), GetGValue(c[1]), GetRValue(c[1]));  /* ПВ */
        px[2] = RGB(GetBValue(c[2]), GetGValue(c[2]), GetRValue(c[2]));  /* ЛН */
        px[3] = RGB(GetBValue(c[3]), GetGValue(c[3]), GetRValue(c[3]));  /* ПН */
        old = SelectObject(mem, bmp);
        saved = SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, r->left, r->top, NULL);
        StretchBlt(dc, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   mem, 0, 0, 2, 2, SRCCOPY);
        SetStretchBltMode(dc, saved);
        SelectObject(mem, old);
        DeleteObject(bmp);
    }
    DeleteDC(mem);
}

/* отрисовка произвольной правки (текст уже в UTF-32) */
static void paint_text_block(HDC dc, int page, const int *cp, int n, const PdfTextBlock *blk,
                             float scale, int ox, int oy,
                             int selA, int selB, int showCaret, int caret,
                             int whiteOut)
{
    COLORREF bg = RGB(255, 255, 255);
    TeBgInfo bi;
    int haveBg = 0;
    int ls[TE_MAX_LINES], le[TE_MAX_LINES], lines, i, k;
    float lead = blk->leading > 0 ? blk->leading : blk->fontSize * 1.2f;
    float base0 = blk->baseline;
    /* страховка: если базовая линия вне рамки блока — считаем по рамке */
    if (base0 < blk->box.y0 - blk->fontSize * 0.5f || base0 > blk->box.y1 + blk->fontSize * 0.5f)
        base0 = blk->box.y1 - blk->fontSize * 0.25f;
    float width = blk->box.x1 - blk->box.x0 + 0.6f;
    HFONT font;
    HGDIOBJ old;

    if (width < 8) width = 8;
    lines = pdfdoc_wrap_text(g_doc, cp, n, blk->fontSize, width, ls, le, TE_MAX_LINES);

    if (whiteOut) {
        RECT r;
        /* Закрашиваем фоном страницы, а не белым. Если фон неоднородный
           (двухцветный лист, плашка, лёгкий градиент) — тянем градиент по
           четырём углам, снятым со страницы: заплатки не видно. */
        bg_for(page, &blk->box, &bi);
        haveBg = 1;
        bg = bg_avg(&bi);
        r.left = ox + (int)(blk->box.x0 * scale) - 2;
        r.top = oy + (int)((blk->box.y0 - blk->fontSize * 0.22f) * scale) - 1;
        r.right = ox + (int)(blk->box.x1 * scale) + 2;
        r.bottom = oy + (int)((blk->box.y1 + blk->fontSize * 0.28f) * scale) + 1;
        /* если текст переверстался в большее число строк — закрашиваем и
           добавленные снизу строки, иначе старый текст просвечивает «призраком» */
        if (lines > 1) {
            int extra = (int)((lines - 1) * lead * scale);
            if (extra > 0) {
                int lim = oy + (int)((blk->box.y1 + 12.0f * blk->fontSize) * scale);
                r.bottom += extra;
                if (r.bottom > lim) r.bottom = lim;
            }
        }
        {
            HBITMAP nf = nf_page_bitmap(page, scale);
            if (nf) {
                /* лучший вариант: берём настоящий фон страницы без букв */
                HDC mem = CreateCompatibleDC(dc);
                if (mem) {
                    HGDIOBJ ob = SelectObject(mem, nf);
                    /* источник — та же область, но в координатах страницы */
                    BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                           mem, r.left - ox, r.top - oy, SRCCOPY);
                    SelectObject(mem, ob);
                    DeleteDC(mem);
                }
            } else if (bi.c[0] == bi.c[1] && bi.c[1] == bi.c[2] && bi.c[2] == bi.c[3]) {
                HBRUSH bb = CreateSolidBrush(bi.c[0]);
                FillRect(dc, &r, bb);
                DeleteObject(bb);
            } else {
                fill_gradient(dc, &r, bi.c);
            }
        }
    }

    font = make_gdi_font(pdfdoc_block_family(blk), blk->bold, blk->italic,
                         blk->fontSize, scale);
    old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    /* выделение */
    if (selA >= 0 && selB > selA) {
        HBRUSH sb = CreateSolidBrush(RGB(0, 0, 128));
        for (i = 0; i < lines; i++) {
            int a = selA, b = selB;
            float x0 = 0, x1 = 0;
            int cnt = 0;
            if (b <= ls[i] || a >= le[i]) continue;
            if (a < ls[i]) a = ls[i];
            if (b > le[i]) b = le[i];
            for (k = ls[i]; k < a; k++) x0 += pdfdoc_cp_width(g_doc, cp[k], blk->fontSize);
            x1 = x0;
            for (k = a; k < b; k++) { x1 += pdfdoc_cp_width(g_doc, cp[k], blk->fontSize); cnt++; }
            if (cnt == 0) { x1 = x0 + 2.0f; }
            {
                RECT r;
                float baseline = base0 + i * lead;
                r.left = ox + (int)((blk->box.x0 + x0) * scale) - 1;
                r.right = ox + (int)((blk->box.x0 + x1) * scale) + 1;
                r.top = oy + (int)((baseline - blk->fontSize * 0.82f) * scale);
                r.bottom = oy + (int)((baseline + blk->fontSize * 0.22f) * scale);
                FillRect(dc, &r, sb);
            }
        }
        DeleteObject(sb);
    }

    /* текст поверх выделения (часть до/после выделения рисуем чёрным) */
    SetTextColor(dc, RGB((blk->color >> 16) & 0xFF, (blk->color >> 8) & 0xFF, blk->color & 0xFF));
    for (i = 0; i < lines; i++) {
        float baseline = base0 + i * lead;
        if (selA >= 0 && selB > selA && !(selB <= ls[i] || selA >= le[i])) {
            int a = selA < ls[i] ? ls[i] : selA;
            int b = selB > le[i] ? le[i] : selB;
            float xa = 0;
            for (k = ls[i]; k < a; k++) xa += pdfdoc_cp_width(g_doc, cp[k], blk->fontSize);
            SetTextColor(dc, RGB(255, 255, 255));
            draw_line(dc, cp, a, b, blk->box.x0 + xa, baseline, blk->fontSize, scale, ox, oy);
            SetTextColor(dc, RGB((blk->color >> 16) & 0xFF, (blk->color >> 8) & 0xFF, blk->color & 0xFF));
            if (a > ls[i])
                draw_line(dc, cp, ls[i], a, blk->box.x0, baseline, blk->fontSize, scale, ox, oy);
            if (b < le[i]) {
                float xb = xa;
                for (k = a; k < b; k++) xb += pdfdoc_cp_width(g_doc, cp[k], blk->fontSize);
                draw_line(dc, cp, b, le[i], blk->box.x0 + xb, baseline, blk->fontSize, scale, ox, oy);
            }
        } else {
            draw_line(dc, cp, ls[i], le[i], blk->box.x0, baseline, blk->fontSize, scale, ox, oy);
        }
    }

    /* каретка */
    if (showCaret) {
        int li;
        float x = 0;
        for (li = 0; li < lines; li++)
            if (caret >= ls[li] && caret <= le[li]) break;
        if (li >= lines) li = lines - 1;
        for (k = ls[li]; k < caret; k++) x += pdfdoc_cp_width(g_doc, cp[k], blk->fontSize);
        {
            float baseline = base0 + li * lead;
            int cx = ox + (int)((blk->box.x0 + x) * scale);
            RECT r;
            r.left = cx; r.right = cx + (scale > 1.5f ? 2 : 1);
            r.top = oy + (int)((baseline - blk->fontSize * 0.82f) * scale);
            r.bottom = oy + (int)((baseline + blk->fontSize * 0.22f) * scale);
            {
                HBRUSH cb = CreateSolidBrush(contrast_color(haveBg ? bg : RGB(255, 255, 255)));
                FillRect(dc, &r, cb);
                DeleteObject(cb);
            }
        }
    }

    SelectObject(dc, old);
    DeleteObject(font);
}

void te_paint(HDC dc)
{
    int selA = -1, selB = -1;
    if (!te.active) return;
    if (te.anchor != te.caret) {
        selA = te.anchor < te.caret ? te.anchor : te.caret;
        selB = te.anchor < te.caret ? te.caret : te.anchor;
    }
    paint_text_block(dc, te.page, te.cp, te.n, &te.blk, te.scale, te.ox, te.oy,
                     selA, selB, 1, te.caret, 1);
}

/* отрисовать все сохранённые правки страницы (вне режима редактирования) */
void te_paint_edits(HDC dc, int page, float scale, int ox, int oy)
{
    int i;
    for (i = 0; i < g_editCount; i++) {
        PdfTextBlock blk;
        int *cp, n = 0;
        if (g_edits[i].page != page) continue;
        memset(&blk, 0, sizeof(blk));
        blk.box = g_edits[i].box;
        blk.fontSize = g_edits[i].fontSize;
        blk.baseline = g_edits[i].baseline;
        blk.leading = g_edits[i].leading;
        blk.bold = g_edits[i].bold;
        blk.italic = g_edits[i].italic;
        blk.color = g_edits[i].color;
        blk.bg = g_edits[i].bg;
        blk.nofill = g_edits[i].nofill;
        memcpy(blk.font, g_edits[i].font, sizeof(blk.font));
        cp = pdfdoc_utf8_to_cp(g_edits[i].text, &n);
        if (!cp) continue;
            pdfdoc_metrics_set(g_doc, pdfdoc_text_font(g_doc, &blk));
            paint_text_block(dc, g_edits[i].page, cp, n, &blk, scale, ox, oy, -1, -1, 0, 0, 1);
            pdfdoc_metrics_set(g_doc, NULL);
        free(cp);
    }
}

/* ---------- применение всех правок к документу ---------- */
int te_apply_all(PdfDoc *d)
{
    int i, applied = 0;
    for (i = 0; i < g_editCount; i++) {
        PdfTextBlock blk;
        memset(&blk, 0, sizeof(blk));
        blk.box = g_edits[i].box;
        blk.fontSize = g_edits[i].fontSize;
        blk.baseline = g_edits[i].baseline;
        blk.leading = g_edits[i].leading;
        blk.bold = g_edits[i].bold;
        blk.italic = g_edits[i].italic;
        blk.color = g_edits[i].color;
        blk.bg = g_edits[i].bg;
        blk.nofill = g_edits[i].nofill;
        memcpy(blk.font, g_edits[i].font, sizeof(blk.font));
        blk.text = g_edits[i].text;
        if (pdfdoc_text_apply(d, g_edits[i].page, &blk)) applied++;
    }
    return applied;
}
