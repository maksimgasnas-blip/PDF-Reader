/* flow.c — текстовый процессор внутри PDF Студии.
   PDF разбирается на строки, строки склеиваются в абзацы; каждый символ несёт
   свой стиль (кегль, цвет, жирный/курсив/подчёркнутый). Абзац знает
   выравнивание, список, отступы и интервалы. Всё верстается заново: перенос по
   словам, разрыв страниц, выравнивание по ширине. При сохранении из потока
   собирается новый PDF. */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "flow.h"
#include <mupdf/ucdn.h>

#define FL_MAX_PARA   20000
#define FL_MAX_SNAP   40
#define FL_MAXST      24
#define FL_LEAD       1.30f
#define FL_ASC        0.82f
#define FL_LIST_GAP   20.0f
#define FL_CELL_PAD   4.0f
#define FL_CELL_MIN   14.0f

#define FAT_BOLD      1
#define FAT_ITAL      2
#define FAT_UNDL      4

#define FL_NFAM       4
typedef struct {
    float size;
    unsigned color;
    unsigned char attr;
    unsigned char font;     /* семейство: 0 Times, 1 Arial, 2 Courier, 3 Arial Narrow */
} FlSt;

/* короткие имена для панели/меню и полные имена для GDI */
static const wchar_t *g_famShort[FL_NFAM] = { L"Times", L"Arial", L"Courier", L"Узкий" };
static const wchar_t *g_famGdi[FL_NFAM]   = { L"Times New Roman", L"Arial",
                                              L"Courier New", L"Arial Narrow" };
static const char    *g_famPdf[FL_NFAM]   = { "Times New Roman", "Arial",
                                              "Courier New", "Arial Narrow" };

typedef struct {
    HBITMAP       bmp;
    int           bwpx, bhpx;
} FlImgCache;

typedef struct {
    int          *cp;
    unsigned char *sty;        /* индекс стиля на каждый символ */
    int           n, cap;
    FlSt          st[FL_MAXST];
    int           nst;

    /* свойства абзаца */
    int           align;       /* 0 слева, 1 центр, 2 справа, 3 по ширине */
    int           list;        /* 0 нет, 1 маркеры, 2 нумерация */
    int           listNum;
    float         spaceBefore, spaceAfter;
    float         indent;      /* красная строка */
    float         lineSpacing; /* 1.0 — одинарный */

    /* картинка */
    int           isImage;
    fz_image     *img;
    HBITMAP       bmp;
    int           bwpx, bhpx;
    float         imgW, imgH;
    float         bx, by, bw, bh;
    int           ipage;

    /* таблица: все ячейки — обычные абзацы с общей меткой таблицы */
    int           tbl;         /* 0 — не в таблице, иначе номер таблицы */
    int           trow, tcol, tcols;
    float         cellX0, cellY0, cellX1, cellY1;

    /* служебное */
    int           l0, l1;      /* диапазон строк */
    int           nlines;
    float         minX0, maxX0, minC, maxC;
} FlPara;

typedef struct {
    int   para, start, end, page, first;
    float x, y;        /* x — левый край, y — базовая линия */
    float w;
    float spExtra;     /* добавка к каждому пробелу при выравнивании по ширине */
} FlLine;

typedef struct {
    int     np;
    FlPara *p;
    int     cpara, coff;
} FlSnap;

static void flog(const char *fmt, ...)
{
    FILE *f = fopen("C:\\pdflog.txt", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static struct {
    PdfDoc *d;
    int     ready;

    FlPara *p;   int np, capp;
    FlLine *ln;  int nl, capl;

    int     npages;
    float   pw, ph;
    float   margin_l, margin_t, margin_r, margin_b;
    int     pageSize;      /* 0 A4, 1 Letter, 2 A5 */
    int     landscape;

    int     dirty;
    int     cpara, coff;
    int     apara, aoff;
    int     dragging;
    FlSt    pend;          /* стиль для набора */

    char    hdr[256];      /* верхний колонтитул (UTF-8) */
    char    ftr[256];      /* нижний колонтитул (UTF-8) */
    int     pgnum;         /* нумерация страниц */
    int     tblSeq;        /* последний номер таблицы */

    FlSnap  undo[FL_MAX_SNAP];  int nundo;
    FlSnap  redo[FL_MAX_SNAP];  int nredo;
} F;

static void layout(void);

/* ---------- шрифты ---------- */

/* ресурсы 300..315: семейство*4 + начертание (0 обычное, 1 жирное, 2 курсив, 3 ж+к) */
static fz_font *g_ff[FL_NFAM][4];

static fz_font *fam_font(PdfDoc *d, int fam, int attr)
{
    int v;
    if (!d || !d->ctx) return NULL;
    if (fam < 0 || fam >= FL_NFAM) fam = 0;
    v = ((attr & FAT_BOLD) ? 1 : 0) + ((attr & FAT_ITAL) ? 2 : 0);
    if (g_ff[fam][v]) return g_ff[fam][v];
    {
        HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(300 + fam * 4 + v), (LPCWSTR)RT_RCDATA);
        HGLOBAL hg;
        void *data;
        DWORD len;
        fz_font *f = NULL;
        if (!hr) return NULL;
        hg = LoadResource(NULL, hr);
        if (!hg) return NULL;
        data = LockResource(hg);
        len = SizeofResource(NULL, hr);
        if (!data || !len) return NULL;
        fz_try(d->ctx) { f = fz_new_font_from_memory(d->ctx, g_famPdf[fam], data, (int)len, 0, 0); }
        fz_catch(d->ctx) { f = NULL; }
        g_ff[fam][v] = f;
        return f;
    }
}

static void fam_font_reset(void)
{
    int i, j;
    for (i = 0; i < FL_NFAM; i++)
        for (j = 0; j < 4; j++) g_ff[i][j] = NULL;   /* контекст MuPDF один на процесс */
}

/* ------------------------------------------------------------------ */
/* утилиты                                                             */

static void *xrealloc(void *p, size_t n) { return realloc(p, n); }

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == 0x00A0 ||
           (c >= 0x2000 && c <= 0x200B) || c == 0x202F || c == 0x205F ||
           c == 0x3000 || c == 0xFEFF;
}

static int sanitize_cp(int c)
{
    if (c < 32) return ' ';
    if (c >= 0x80 && c <= 0xA0) return ' ';
    if (c >= 0xE000 && c <= 0xF8FF) return ' ';
    if (c >= 0xF0000) return ' ';
    if (c == 0x00AD || c == 0x200B || c == 0x200C || c == 0x200D) return ' ';
    if (c == 0x2028 || c == 0x2029) return ' ';
    return c;
}

static void para_free(FlPara *p)
{
    if (!p) return;
    free(p->cp);
    free(p->sty);
    p->cp = NULL;
    p->sty = NULL;
    p->n = p->cap = 0;
    if (p->bmp) { DeleteObject(p->bmp); p->bmp = NULL; }
    p->img = NULL;
}

/* ---------- стили абзаца ---------- */

static int st_index(FlPara *p, FlSt s)
{
    int i;
    for (i = 0; i < p->nst; i++)
        if (p->st[i].attr == s.attr && p->st[i].color == s.color &&
            p->st[i].font == s.font &&
            fabsf(p->st[i].size - s.size) < 0.05f)
            return i;
    if (p->nst >= FL_MAXST) return p->nst > 0 ? p->nst - 1 : 0;
    p->st[p->nst] = s;
    return p->nst++;
}

static FlSt st_at(FlPara *p, int off)
{
    FlSt s;
    s.size = 11.0f;
    s.color = 0;
    s.attr = 0;
    s.font = 0;
    if (!p || p->nst <= 0) return s;
    if (p->n <= 0) return p->st[0];
    if (off < 0) off = 0;
    if (off >= p->n) off = p->n - 1;
    if (!p->sty) return p->st[0];
    if (p->sty[off] < p->nst) return p->st[p->sty[off]];
    return p->st[0];
}

/* ---------- массив символов ---------- */

static int para_reserve(FlPara *p, int need)
{
    if (need <= p->cap) return 1;
    {
        int cap = p->cap ? p->cap : 64;
        int *q;
        unsigned char *s;
        while (cap < need) cap *= 2;
        q = (int *)xrealloc(p->cp, (size_t)cap * sizeof(int));
        if (!q) return 0;
        p->cp = q;
        s = (unsigned char *)xrealloc(p->sty, (size_t)cap);
        if (!s) return 0;
        p->sty = s;
        p->cap = cap;
    }
    return 1;
}

static int para_insert(FlPara *p, int at, const int *cp, int n, int sty)
{
    if (n <= 0) return 1;
    if (!para_reserve(p, p->n + n)) return 0;
    memmove(p->cp + at + n, p->cp + at, (size_t)(p->n - at) * sizeof(int));
    memmove(p->sty + at + n, p->sty + at, (size_t)(p->n - at));
    memcpy(p->cp + at, cp, (size_t)n * sizeof(int));
    memset(p->sty + at, (unsigned char)(sty < 0 ? 0 : sty), (size_t)n);
    p->n += n;
    return 1;
}

static int para_delete(FlPara *p, int at, int n)
{
    if (n <= 0 || at < 0 || at + n > p->n) return 0;
    memmove(p->cp + at, p->cp + at + n, (size_t)(p->n - at - n) * sizeof(int));
    memmove(p->sty + at, p->sty + at + n, (size_t)(p->n - at - n));
    p->n -= n;
    return 1;
}

/* перенести символы src[from..to) в конец dst, перенеся стили */
static void para_append_from(FlPara *dst, FlPara *src, int from, int to)
{
    int i;
    if (from < 0) from = 0;
    if (to > src->n) to = src->n;
    for (i = from; i < to; i++) {
        FlSt s = st_at(src, i);
        int idx = st_index(dst, s);
        para_insert(dst, dst->n, &src->cp[i], 1, idx);
    }
}

static FlPara *para_new(void)
{
    FlPara *p;
    if (F.np >= FL_MAX_PARA) return NULL;
    if (F.np >= F.capp) {
        int cap = F.capp ? F.capp * 2 : 64;
        FlPara *q = (FlPara *)xrealloc(F.p, (size_t)cap * sizeof(FlPara));
        if (!q) return NULL;
        memset(q + F.capp, 0, (size_t)(cap - F.capp) * sizeof(FlPara));
        F.p = q;
        F.capp = cap;
    }
    p = &F.p[F.np++];
    memset(p, 0, sizeof(*p));
    p->lineSpacing = 1.0f;
    p->l0 = p->l1 = -1;
    return p;
}

/* вставить новый пустой абзац в позицию idx (сдвигая остальные) */
static FlPara *para_ins(int idx)
{
    FlPara tmp, *p;
    if (idx < 0) idx = 0;
    if (idx > F.np) idx = F.np;
    p = para_new();
    if (!p) return NULL;
    tmp = *p;
    memmove(&F.p[idx + 1], &F.p[idx], (size_t)(F.np - 1 - idx) * sizeof(FlPara));
    F.p[idx] = tmp;
    return &F.p[idx];
}

/* ------------------------------------------------------------------ */
/* сброс / сборка потока                                               */

static void reset_all(void)
{
    int i, k;
    for (i = 0; i < F.np; i++) para_free(&F.p[i]);
    free(F.p);  F.p = NULL; F.np = F.capp = 0;
    free(F.ln); F.ln = NULL; F.nl = F.capl = 0;
    for (i = 0; i < F.nundo; i++) {
        for (k = 0; k < F.undo[i].np; k++) para_free(&F.undo[i].p[k]);
        free(F.undo[i].p);
        F.undo[i].p = NULL;
        F.undo[i].np = 0;
    }
    for (i = 0; i < F.nredo; i++) {
        for (k = 0; k < F.redo[i].np; k++) para_free(&F.redo[i].p[k]);
        free(F.redo[i].p);
        F.redo[i].p = NULL;
        F.redo[i].np = 0;
    }
    F.nundo = F.nredo = 0;
}

void flow_reset(void)
{
    reset_all();
    F.d = NULL;
    F.ready = 0;
    F.dirty = 0;
    F.npages = 0;
    F.cpara = F.coff = 0;
    F.apara = F.aoff = 0;
    F.dragging = 0;
    memset(&F.pend, 0, sizeof(F.pend));
    F.pend.size = 11.0f;
}

int flow_ready(void)  { return F.ready; }
int flow_pages(void)  { return F.npages > 0 ? F.npages : 1; }
float flow_page_w(void) { return F.pw; }
float flow_page_h(void) { return F.ph; }
int flow_is_dirty(void) { return F.dirty; }
void flow_mark_clean(void) { F.dirty = 0; }

static void append_line_text(FlPara *p, int *cp, int n, FlSt style)
{
    int i = 0, j = n, k, sp = ' ';
    int idx;

    while (i < j && is_space(sanitize_cp(cp[i]))) i++;
    while (j > i && is_space(sanitize_cp(cp[j - 1]))) j--;
    if (j <= i) return;

    idx = st_index(p, style);
    if (p->n > 0) {
        int last = p->cp[p->n - 1];
        if (last != '-' && last != 0x2010 && last != 0x2011 && !is_space(last))
            para_insert(p, p->n, &sp, 1, idx);
    }
    for (k = i; k < j; k++) {
        int c = sanitize_cp(cp[k]);
        if (is_space(c)) {
            if (p->n > 0 && is_space(p->cp[p->n - 1])) continue;
            c = ' ';
        }
        para_insert(p, p->n, &c, 1, idx);
    }
}

static void build_from_page(PdfDoc *d, fz_stext_page *st, int pageNo,
                            int *cur, float *lastBaseline, float *lastX,
                            float *lastSize, float *lastCenter)
{
    fz_context *ctx = d->ctx;
    fz_stext_block *blk;
    int *tmp = NULL;
    int tmpcap = 0;

    for (blk = st->first_block; blk; blk = blk->next) {
        if (blk->type == FZ_STEXT_BLOCK_TEXT) {
            fz_stext_line *line;
            for (line = blk->u.t.first_line; line; line = line->next) {
                fz_stext_char *ch;
                int n = 0, i, needNew;
                float size = 0, baseline = 0, x0 = line->bbox.x0, x1 = line->bbox.x1;
                unsigned color = 0;
                int bold = 0, italic = 0;
                FlSt style;

                if (line->wmode != 0) continue;
                for (ch = line->first_char; ch; ch = ch->next) n++;
                if (n <= 0) continue;
                if (n > tmpcap) {
                    int *q = (int *)xrealloc(tmp, (size_t)n * 2 * sizeof(int));
                    if (!q) break;
                    tmp = q;
                    tmpcap = n * 2;
                }
                i = 0;
                for (ch = line->first_char; ch; ch = ch->next)
                    tmp[i++] = ch->c > 0 ? ch->c : 32;
                for (ch = line->first_char; ch && !size; ch = ch->next) {
                    if (ch->c > 0) {
                        size = ch->size > 1.0f ? ch->size : 10.0f;
                        baseline = ch->origin.y;
                        color = (unsigned)ch->color;
                        if (ch->font) {
                            bold = fz_font_is_bold(ctx, ch->font);
                            italic = fz_font_is_italic(ctx, ch->font);
                        }
                    }
                }
                if (size <= 0) continue;

                style.size = size;
                style.color = color;
                style.attr = (unsigned char)((bold ? FAT_BOLD : 0) | (italic ? FAT_ITAL : 0));

                {
                    float center = (x0 + x1) * 0.5f;
                    int centerMatch = fabsf(center - *lastCenter) < 4.0f;
                    needNew = (*cur < 0) || (*lastSize <= 0) ||
                              fabsf(size - *lastSize) > 0.6f ||
                              (!centerMatch && fabsf(x0 - *lastX) > size * 1.1f) ||
                              (baseline - *lastBaseline) > size * 1.9f ||
                              (baseline - *lastBaseline) < -0.5f;
                    *lastCenter = center;
                }

                if (needNew) {
                    FlPara *p = para_new();
                    if (!p) break;
                    *cur = F.np - 1;
                    p->align = 0;
                    if (*lastBaseline > -1e8f && baseline - *lastBaseline > size * 1.0f) {
                        float gap = (baseline - *lastBaseline) - size * FL_LEAD;
                        if (gap < 0) gap = 0;
                        if (gap > 26) gap = 26;
                        p->spaceBefore = gap;
                    }
                    if (x0 - F.margin_l > 3.0f && x0 - F.margin_l < 72.0f)
                        p->indent = x0 - F.margin_l;
                    p->ipage = pageNo;
                }

                append_line_text(&F.p[*cur], tmp, n, style);
                {
                    FlPara *pp = &F.p[*cur];
                    float center = (x0 + x1) * 0.5f;
                    if (pp->nlines == 0) {
                        pp->minX0 = pp->maxX0 = x0;
                        pp->minC = pp->maxC = center;
                    } else {
                        if (x0 < pp->minX0) pp->minX0 = x0;
                        if (x0 > pp->maxX0) pp->maxX0 = x0;
                        if (center < pp->minC) pp->minC = center;
                        if (center > pp->maxC) pp->maxC = center;
                    }
                    pp->nlines++;
                }
                *lastBaseline = baseline;
                *lastX = x0;
                *lastSize = size;
            }
        } else if (blk->type == FZ_STEXT_BLOCK_IMAGE) {
            fz_rect r = blk->bbox;
            float w = r.x1 - r.x0, h = r.y1 - r.y0;
            float pageArea = F.pw * F.ph;
            if (!blk->u.i.image || w < 8 || h < 8) continue;
            if (pageArea > 0 && (w * h) > pageArea * 0.55f) continue;
            {
                FlPara *p = para_new();
                if (!p) break;
                p->isImage = 1;
                p->img = blk->u.i.image;
                fz_keep_image(ctx, p->img);
                p->imgW = w;
                p->imgH = h;
                p->spaceBefore = 6;
                p->ipage = pageNo;
                *cur = -1;
                *lastBaseline = -1e9f;
                *lastSize = 0;
                *lastCenter = -1e9f;
            }
        }
    }
    free(tmp);
}

static void page_geometry(int sizeIdx, int landscape, float *w, float *h)
{
    float pw = 595.0f, ph = 842.0f;
    if (sizeIdx == 1) { pw = 612.0f; ph = 792.0f; }        /* Letter */
    else if (sizeIdx == 2) { pw = 420.0f; ph = 595.0f; }    /* A5 */
    if (landscape) { float t = pw; pw = ph; ph = t; }
    *w = pw;
    *h = ph;
}

int flow_build(PdfDoc *d)
{
    fz_context *ctx;
    int npages, pg;
    int cur = -1;
    float lastBaseline = -1e9f, lastX = 0, lastSize = 0, lastCenter = -1e9f;

    flow_reset();
    if (!d || !d->ctx || !d->doc) return 0;
    F.d = d;
    ctx = d->ctx;

    npages = pdfdoc_pages(d);
    pdfdoc_page_size(d, 0, &F.pw, &F.ph);
    if (F.pw <= 1 || F.ph <= 1) { F.pw = 595; F.ph = 842; }
    F.pageSize = 0;
    F.landscape = F.pw > F.ph;
    if (F.pw > 500 && F.pw < 700 && F.ph > 700 && F.ph < 900) F.pageSize = 0;
    else if (F.pw > 580 && F.pw < 640 && F.ph > 760 && F.ph < 820) F.pageSize = 1;

    F.margin_l = F.margin_r = 42;
    F.margin_t = F.margin_b = 42;
    if (F.pw < 400) F.margin_l = F.margin_r = 24;

    for (pg = 0; pg < npages && pg < 5000; pg++) {
        fz_page *page = NULL;
        fz_stext_page *st = NULL;
        fz_stext_options opts;
        memset(&opts, 0, sizeof(opts));
        opts.flags = FZ_STEXT_PRESERVE_IMAGES;

        fz_try(ctx) {
            page = fz_load_page(ctx, d->doc, pg);
            st = fz_new_stext_page_from_page(ctx, page, &opts);
        }
        fz_catch(ctx) { st = NULL; }

        if (st) {
            fz_try(ctx) {
                build_from_page(d, st, pg, &cur, &lastBaseline, &lastX, &lastSize,
                                &lastCenter);
            }
            fz_always(ctx) { fz_drop_stext_page(ctx, st); }
            fz_catch(ctx) { }
        }
        if (page) fz_drop_page(ctx, page);
        cur = -1;
        lastBaseline = -1e9f;
        lastSize = 0;
        lastCenter = -1e9f;
    }

    if (F.np == 0) {
        flow_reset();
        return 0;
    }

    /* центрированные абзацы: строки начинаются по-разному, середины совпадают */
    {
        int i;
        for (i = 0; i < F.np; i++) {
            FlPara *p = &F.p[i];
            if (p->nlines >= 2 && (p->maxX0 - p->minX0) > 6.0f &&
                (p->maxC - p->minC) < 4.0f)
                p->align = 1;
        }
    }

    F.ready = 1;
    F.dirty = 0;
    F.cpara = F.coff = 0;
    F.apara = F.aoff = 0;
    if (F.np > 0 && F.p[0].nst > 0) F.pend = F.p[0].st[0];
    layout();
    return 1;
}

/* ------------------------------------------------------------------ */
/* измерения                                                           */

/* ширина символа в шрифте семейства/начертания (для вёрстки и для PDF) */
static float cpw_style(PdfDoc *d, FlSt s, int cp)
{
    float size = s.size > 1.0f ? s.size : 11.0f;
    fz_font *f;
    int gid;
    if (cp == '\t') return size * 2.0f;
    f = fam_font(d, s.font, s.attr);
    if (!f) return pdfdoc_cp_width(d, cp, size);
    gid = fz_encode_character(d->ctx, f, cp > 0 ? cp : 32);
    if (!gid) gid = fz_encode_character(d->ctx, f, '?');
    return (float)fz_advance_glyph(d->ctx, f, gid, 0) * size;
}

static float cpw(PdfDoc *d, int cp, float size)
{
    FlSt s;
    memset(&s, 0, sizeof(s));
    s.size = size;
    return cpw_style(d, s, cp);
}

static float ch_w(FlPara *p, int i)
{
    return cpw_style(F.d, st_at(p, i), p->cp[i]);
}

/* ------------------------------------------------------------------ */
/* вёрстка                                                             */

static int line_add(int para, int start, int end, int page, float x, float y,
                    float w, float spExtra, int first)
{
    FlLine *L;
    if (F.nl >= F.capl) {
        int cap = F.capl ? F.capl * 2 : 512;
        FlLine *q = (FlLine *)xrealloc(F.ln, (size_t)cap * sizeof(FlLine));
        if (!q) return 0;
        F.ln = q;
        F.capl = cap;
    }
    L = &F.ln[F.nl++];
    L->para = para;
    L->start = start;
    L->end = end;
    L->page = page;
    L->x = x;
    L->y = y;
    L->w = w;
    L->spExtra = spExtra;
    L->first = first;
    return 1;
}

/* конец строки, начинающейся с i0, при доступной ширине w */
static int wrap_line(FlPara *p, int i0, float w)
{
    float x = 0;
    int i = i0, lastSp = -1;
    if (w < 6) w = 6;
    while (i < p->n) {
        float cw;
        if (p->cp[i] == '\n') return i;
        cw = ch_w(p, i);
        if (x + cw > w && i > i0) {
            if (lastSp > i0) return lastSp + 1;
            return i;
        }
        if (p->cp[i] == ' ') lastSp = i;
        x += cw;
        i++;
    }
    return p->n;
}

static float line_max_size(FlPara *p, int a, int b)
{
    float m = 0;
    int i;
    for (i = a; i < b; i++) {
        float s = st_at(p, i).size;
        if (s > m) m = s;
    }
    return m > 1.0f ? m : 11.0f;
}

/* разложить текст одного абзаца; *y — текущая базовая точка сверху,
   *page — текущая страница. Используется и для обычных абзацев, и для ячеек. */
static void layout_text(FlPara *p, int idx, float xText, float wrapW,
                        float *y, int *page, float bottom)
{
    float baseSize = p->n > 0 ? st_at(p, 0).size : 11.0f;
    int i0 = 0;

    if (baseSize < 1.0f) baseSize = 11.0f;
    if (p->lineSpacing <= 0.05f) p->lineSpacing = 1.0f;

    if (p->n == 0) {
        float lh = baseSize * FL_LEAD * p->lineSpacing;
        if (*y + lh > bottom && *y > F.margin_t) { (*page)++; *y = F.margin_t; }
        p->l0 = F.nl;
        line_add(idx, 0, 0, *page, xText, *y + baseSize * FL_ASC, 0, 0, 1);
        p->l1 = F.nl;
        *y += lh;
        return;
    }

    while (i0 < p->n) {
        float firstIndent = (i0 == 0 ? p->indent : 0);
        float wAvail = wrapW - firstIndent;
        int i1 = wrap_line(p, i0, wAvail);
        float wl = 0, maxSize, lh, x;
        int k, spaces = 0, lastLine;
        float spExtra = 0;

        if (i1 <= i0) i1 = i0 + 1;
        for (k = i0; k < i1; k++) {
            wl += ch_w(p, k);
            if (p->cp[k] == ' ') spaces++;
        }
        maxSize = line_max_size(p, i0, i1);
        lh = maxSize * FL_LEAD * p->lineSpacing;

        if (*y + lh > bottom && *y > F.margin_t) { (*page)++; *y = F.margin_t; }

        lastLine = (i1 >= p->n);
        x = xText + firstIndent;
        if (p->align == 1) x = xText + firstIndent + (wAvail - wl) * 0.5f;
        else if (p->align == 2) x = xText + firstIndent + (wAvail - wl);
        else if (p->align == 3 && !lastLine && spaces > 0 && wl < wAvail)
            spExtra = (wAvail - wl) / (float)spaces;

        if (p->l0 < 0) p->l0 = F.nl;
        line_add(idx, i0, i1, *page, x, *y + maxSize * FL_ASC, wl, spExtra,
                 i0 == 0);
        p->l1 = F.nl;
        *y += lh;
        i0 = i1;
    }
}

static void layout(void)
{
    int i, page = 0, listCounter = 0;
    float y = 0;
    float left = 0, right = 0, bottom = 0, avail = 0;

    F.nl = 0;
    if (!F.ready) { F.npages = 1; return; }

    left = F.margin_l;
    right = F.pw - F.margin_r;
    bottom = F.ph - F.margin_b;
    avail = right - left;
    y = F.margin_t;

    for (i = 0; i < F.np; i++) {
        FlPara *p = &F.p[i];
        p->l0 = p->l1 = -1;
        p->listNum = 0;

        /* ---------- таблица ---------- */
        if (p->tbl) {
            int id = p->tbl;
            int ncols = p->tcols > 0 ? p->tcols : 1;
            float cellW = avail / (float)ncols;
            float rowTop = y;
            int j = i;

            while (j < F.np && F.p[j].tbl == id) {
                int r = F.p[j].trow;
                float rowH = 0;

                if (rowTop + FL_CELL_MIN > bottom && rowTop > F.margin_t + 1) {
                    page++;
                    rowTop = F.margin_t;
                }
                while (j < F.np && F.p[j].tbl == id && F.p[j].trow == r) {
                    FlPara *q = &F.p[j];
                    float cx = left + (float)q->tcol * cellW;
                    float cy = rowTop + FL_CELL_PAD;
                    q->l0 = q->l1 = -1;
                    q->cellX0 = cx;
                    q->cellX1 = cx + cellW;
                    q->cellY0 = rowTop;
                    q->cellY1 = rowTop;
                    q->ipage = page;
                    layout_text(q, j, cx + FL_CELL_PAD, cellW - 2.0f * FL_CELL_PAD,
                                &cy, &page, bottom);
                    if (cy - rowTop > rowH) rowH = cy - rowTop;
                    j++;
                }
                if (rowH < FL_CELL_MIN) rowH = FL_CELL_MIN;
                rowH += FL_CELL_PAD;
                {
                    int k;
                    for (k = i; k < j; k++)
                        if (F.p[k].cellY0 == rowTop) F.p[k].cellY1 = rowTop + rowH;
                }
                rowTop += rowH;
            }
            y = rowTop + 8.0f;
            i = j - 1;
            continue;
        }

        if (p->isImage) {
            float w = p->imgW, h = p->imgH;
            if (w <= 0) w = 100;
            if (h <= 0) h = 100;
            if (w > avail) { h = h * avail / w; w = avail; }
            y += p->spaceBefore;
            if (y + h > bottom && y > F.margin_t) { page++; y = F.margin_t; }
            p->bx = left + (avail - w) * 0.5f;
            p->by = y;
            p->bw = w;
            p->bh = h;
            p->ipage = page;
            y += h + p->spaceAfter + 8;
            continue;
        }

        if (p->lineSpacing <= 0.05f) p->lineSpacing = 1.0f;
        if (p->list == 2) { listCounter++; p->listNum = listCounter; }
        else if (p->list == 0) listCounter = 0;

        y += p->spaceBefore;
        {
            float xText = left + (p->list ? FL_LIST_GAP : 0);
            float wrapW = right - xText;
            layout_text(p, i, xText, wrapW, &y, &page, bottom);
        }
        y += p->spaceAfter;
    }
    F.npages = page + 1;
}

/* ------------------------------------------------------------------ */
/* отмена / повтор                                                     */

static void snap_free(FlSnap *s)
{
    int k;
    for (k = 0; k < s->np; k++) para_free(&s->p[k]);
    free(s->p);
    s->p = NULL;
    s->np = 0;
}

static FlSnap snap_take(void)
{
    FlSnap s;
    int i;
    memset(&s, 0, sizeof(s));
    s.np = F.np;
    s.cpara = F.cpara;
    s.coff = F.coff;
    if (F.np <= 0) return s;
    s.p = (FlPara *)calloc((size_t)F.np, sizeof(FlPara));
    if (!s.p) { s.np = 0; return s; }
    for (i = 0; i < F.np; i++) {
        FlPara *src = &F.p[i];
        FlPara *dst = &s.p[i];
        *dst = *src;
        dst->cp = NULL;
        dst->sty = NULL;
        dst->cap = 0;
        dst->bmp = NULL;
        dst->img = NULL;
        if (src->n > 0 && src->cp) {
            dst->cp = (int *)malloc((size_t)src->n * sizeof(int));
            dst->sty = (unsigned char *)malloc((size_t)src->n);
            if (dst->cp && dst->sty) {
                memcpy(dst->cp, src->cp, (size_t)src->n * sizeof(int));
                memcpy(dst->sty, src->sty, (size_t)src->n);
                dst->cap = src->n;
            } else {
                free(dst->cp); free(dst->sty);
                dst->cp = NULL; dst->sty = NULL;
                dst->n = 0;
            }
        }
    }
    return s;
}

static void state_restore(FlSnap *s)
{
    int i;
    for (i = 0; i < F.np; i++) para_free(&F.p[i]);
    free(F.p);
    F.p = NULL;
    F.np = F.capp = 0;

    if (s->np > 0) {
        F.p = (FlPara *)calloc((size_t)s->np, sizeof(FlPara));
        if (F.p) {
            F.capp = s->np;
            F.np = s->np;
            for (i = 0; i < s->np; i++) {
                F.p[i] = s->p[i];
                F.p[i].bmp = NULL;
                s->p[i].cp = NULL;
                s->p[i].sty = NULL;
                s->p[i].n = 0;
                s->p[i].cap = 0;
            }
        }
    }
    F.cpara = s->cpara;
    F.coff = s->coff;
    if (F.cpara >= F.np) F.cpara = F.np > 0 ? F.np - 1 : 0;
    if (F.cpara < 0) F.cpara = 0;
    if (F.cpara < F.np && F.coff > F.p[F.cpara].n) F.coff = F.p[F.cpara].n;
    F.apara = F.cpara;
    F.aoff = F.coff;
    snap_free(s);
    F.dirty = 1;
    layout();
}

static void snap_push(void)
{
    if (F.nundo >= FL_MAX_SNAP) {
        snap_free(&F.undo[0]);
        memmove(&F.undo[0], &F.undo[1], (size_t)(FL_MAX_SNAP - 1) * sizeof(FlSnap));
        F.nundo = FL_MAX_SNAP - 1;
    }
    F.undo[F.nundo++] = snap_take();
    while (F.nredo > 0) { F.nredo--; snap_free(&F.redo[F.nredo]); }
}

static void snap_undo(void)
{
    if (F.nundo <= 0) return;
    if (F.nredo >= FL_MAX_SNAP) { snap_free(&F.redo[0]); F.nredo = FL_MAX_SNAP - 1; }
    F.redo[F.nredo++] = snap_take();
    state_restore(&F.undo[--F.nundo]);
}

static void snap_redo(void)
{
    if (F.nredo <= 0) return;
    if (F.nundo >= FL_MAX_SNAP) { snap_free(&F.undo[0]); F.nundo = FL_MAX_SNAP - 1; }
    F.undo[F.nundo++] = snap_take();
    state_restore(&F.redo[--F.nredo]);
}

/* ------------------------------------------------------------------ */
/* выделение и каретка                                                 */

static int sel_ordered(int *pa, int *oa, int *pb, int *ob)
{
    int a = F.apara, b = F.aoff, c = F.cpara, e = F.coff;
    if (a < c || (a == c && b <= e)) { *pa = a; *oa = b; *pb = c; *ob = e; }
    else { *pa = c; *oa = e; *pb = a; *ob = b; }
    return 1;
}

int flow_has_selection(void) { return F.apara != F.cpara || F.aoff != F.coff; }

static void sel_clear(void)
{
    F.apara = F.cpara;
    F.aoff = F.coff;
}

static void caret_set(int para, int off)
{
    if (para < 0) para = 0;
    if (para >= F.np) para = F.np - 1;
    if (para < 0) { F.cpara = 0; F.coff = 0; return; }
    if (off < 0) off = 0;
    if (off > F.p[para].n) off = F.p[para].n;
    F.cpara = para;
    F.coff = off;
    if (F.p[para].nst > 0) F.pend = st_at(&F.p[para], off > 0 ? off - 1 : 0);
}

static int hit_test(float px, float py, int *outPara, int *outOff)
{
    int i, best = -1;
    float bd = 1e9f;
    for (i = 0; i < F.nl; i++) {
        FlLine *L = &F.ln[i];
        FlPara *p = &F.p[L->para];
        float asc = line_max_size(p, L->start, L->end) * FL_ASC;
        float desc = line_max_size(p, L->start, L->end) * (FL_LEAD - FL_ASC);
        float top = L->y - asc, bot = L->y + desc, d = 0;
        if (py < top) d = top - py;
        else if (py > bot) d = py - bot;
        if (px >= L->x - 40 && px <= L->x + L->w + 40) d *= 0.5f;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) { *outPara = 0; *outOff = 0; return 0; }
    {
        FlLine *L = &F.ln[best];
        FlPara *p = &F.p[L->para];
        float x = L->x;
        int k, off = L->start;
        for (k = L->start; k < L->end; k++) {
            float w = ch_w(p, k) + (p->cp[k] == ' ' ? L->spExtra : 0);
            if (px < x + w * 0.5f) break;
            x += w;
            off++;
        }
        *outPara = L->para;
        *outOff = off;
    }
    return 1;
}

void flow_click(int page, float pageX, float pageY, int shift)
{
    int pa, of;
    (void)page;
    if (!F.ready) return;
    if (hit_test(pageX, pageY, &pa, &of)) {
        if (!shift) { F.apara = pa; F.aoff = of; }
        caret_set(pa, of);
        F.dragging = 1;
    }
}

void flow_drag(float pageX, float pageY)
{
    int pa, of;
    if (!F.ready || !F.dragging) return;
    if (hit_test(pageX, pageY, &pa, &of)) caret_set(pa, of);
}

int flow_dragging(void) { return F.dragging; }
void flow_drag_end(void) { F.dragging = 0; }

void flow_select_all(void)
{
    if (!F.ready || F.np <= 0) return;
    F.apara = 0;
    F.aoff = 0;
    F.cpara = F.np - 1;
    F.coff = F.p[F.np - 1].n;
}

int flow_caret_page(void)
{
    int i;
    for (i = 0; i < F.nl; i++) {
        FlLine *L = &F.ln[i];
        if (L->para != F.cpara) continue;
        if (F.coff >= L->start && F.coff <= L->end) return L->page;
        if (F.coff > L->end) return L->page;
    }
    if (F.cpara >= 0 && F.cpara < F.np) {
        FlPara *p = &F.p[F.cpara];
        if (p->isImage) return p->ipage;
        if (p->l0 >= 0 && p->l0 < F.nl) return F.ln[p->l0].page;
    }
    return 0;
}

int flow_caret_box(float *x, float *y, float *w, float *h)
{
    int i;
    for (i = 0; i < F.nl; i++) {
        FlLine *L = &F.ln[i];
        FlPara *p;
        float cx;
        int k;
        if (L->para != F.cpara) continue;
        if (F.coff < L->start || F.coff > L->end) continue;
        p = &F.p[L->para];
        cx = L->x;
        for (k = L->start; k < F.coff && k < L->end; k++)
            cx += ch_w(p, k) + (p->cp[k] == ' ' ? L->spExtra : 0);
        if (x) *x = cx;
        if (y) *y = L->y - line_max_size(p, L->start, L->end) * FL_ASC;
        if (w) *w = 1.0f;
        if (h) *h = line_max_size(p, L->start, L->end) * FL_LEAD * p->lineSpacing;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* правка текста                                                       */

static void delete_selection(void)
{
    int pa, oa, pb, ob;
    if (!flow_has_selection()) return;
    sel_ordered(&pa, &oa, &pb, &ob);
    if (pa == pb) {
        para_delete(&F.p[pa], oa, ob - oa);
    } else {
        int del, i;
        para_delete(&F.p[pa], oa, F.p[pa].n - oa);
        {
            FlPara *dst = &F.p[pa], *src = &F.p[pb];
            for (i = ob; i < src->n; i++) {
                FlSt s = st_at(src, i);
                int idx = st_index(dst, s);
                para_insert(dst, dst->n, &src->cp[i], 1, idx);
            }
        }
        del = pb - pa;
        for (i = pa + 1; i <= pb; i++) para_free(&F.p[i]);
        memmove(&F.p[pa + 1], &F.p[pb + 1], (size_t)(F.np - pb - 1) * sizeof(FlPara));
        F.np -= del;
        memset(&F.p[F.np], 0, (size_t)del * sizeof(FlPara));
    }
    F.cpara = pa;
    F.coff = oa;
    sel_clear();
    F.dirty = 1;
}

static void insert_cps(const int *cp, int n)
{
    FlPara *p;
    FlSt st;
    int idx;
    if (n <= 0 || F.cpara < 0 || F.cpara >= F.np) return;
    p = &F.p[F.cpara];
    /* в ячейке таблицы и в непустом абзаце берём формат самого текста,
       иначе «кегль заголовка» прилипал к новому тексту */
    st = F.pend;
    if (p->tbl) {                       /* в ячейке — формат самой ячейки */
        FlSt ps = st_at(p, F.coff > 0 ? F.coff - 1 : 0);
        if (ps.size > 1.0f) st = ps;
        F.pend = st;
    }
    idx = st_index(p, st);
    para_insert(&F.p[F.cpara], F.coff, cp, n, idx);
    F.coff += n;
    sel_clear();
    F.dirty = 1;
}

static void split_para(void)
{
    FlPara q, *p;
    int pos, i;

    if (F.cpara < 0 || F.cpara >= F.np) return;
    p = &F.p[F.cpara];
    pos = F.coff;
    if (pos < 0) pos = 0;
    if (pos > p->n) pos = p->n;

    memset(&q, 0, sizeof(q));
    q.lineSpacing = p->lineSpacing;
    q.align = p->align;
    q.list = p->list;
    q.spaceBefore = 0;
    q.spaceAfter = p->spaceAfter;
    q.indent = 0;
    q.l0 = q.l1 = -1;

    for (i = pos; i < p->n; i++) {
        FlSt s = st_at(p, i);
        int idx = st_index(&q, s);
        para_insert(&q, q.n, &p->cp[i], 1, idx);
    }
    p->n = pos;

    if (!para_new()) { para_free(&q); return; }
    if (F.np - 2 > F.cpara) {
        memmove(&F.p[F.cpara + 2], &F.p[F.cpara + 1],
                (size_t)(F.np - 2 - F.cpara) * sizeof(FlPara));
        memset(&F.p[F.np - 1], 0, sizeof(FlPara));
    }
    F.p[F.cpara + 1] = q;

    F.cpara = F.cpara + 1;
    F.coff = 0;
    sel_clear();
    F.dirty = 1;
}

static void merge_with_prev(void)
{
    FlPara *a, *b;
    if (F.cpara <= 0) return;
    a = &F.p[F.cpara - 1];
    b = &F.p[F.cpara];
    {
        int pos = a->n, i;
        for (i = 0; i < b->n; i++) {
            FlSt s = st_at(b, i);
            int idx = st_index(a, s);
            para_insert(a, a->n, &b->cp[i], 1, idx);
        }
        F.cpara = F.cpara - 1;
        F.coff = pos;
    }
    para_free(b);
    memmove(&F.p[F.cpara + 1], &F.p[F.cpara + 2],
            (size_t)(F.np - F.cpara - 2) * sizeof(FlPara));
    F.np--;
    memset(&F.p[F.np], 0, sizeof(FlPara));
    sel_clear();
    F.dirty = 1;
}

static void merge_with_next(void)
{
    FlPara *a, *b;
    int i;
    if (F.cpara < 0 || F.cpara + 1 >= F.np) return;
    a = &F.p[F.cpara];
    b = &F.p[F.cpara + 1];
    for (i = 0; i < b->n; i++) {
        FlSt s = st_at(b, i);
        int idx = st_index(a, s);
        para_insert(a, a->n, &b->cp[i], 1, idx);
    }
    para_free(b);
    memmove(&F.p[F.cpara + 1], &F.p[F.cpara + 2],
            (size_t)(F.np - F.cpara - 2) * sizeof(FlPara));
    F.np--;
    memset(&F.p[F.np], 0, sizeof(FlPara));
    sel_clear();
    F.dirty = 1;
}

static int para_prev_text(int i)
{
    i--;
    while (i >= 0 && F.p[i].isImage) i--;
    return i;
}
static int para_next_text(int i)
{
    i++;
    while (i < F.np && F.p[i].isImage) i++;
    return i < F.np ? i : -1;
}

static void move_h(int dir, int shift)
{
    if (!shift && flow_has_selection()) {
        int pa, oa, pb, ob;
        sel_ordered(&pa, &oa, &pb, &ob);
        if (dir < 0) caret_set(pa, oa); else caret_set(pb, ob);
        sel_clear();
        return;
    }
    if (dir < 0) {
        if (F.coff > 0) caret_set(F.cpara, F.coff - 1);
        else {
            int pv = para_prev_text(F.cpara);
            if (pv >= 0) caret_set(pv, F.p[pv].n);
        }
    } else {
        if (F.cpara < F.np && F.coff < F.p[F.cpara].n) caret_set(F.cpara, F.coff + 1);
        else {
            int nx = para_next_text(F.cpara);
            if (nx >= 0) caret_set(nx, 0);
        }
    }
    if (!shift) sel_clear();
}

static void move_v(int dir, int shift)
{
    float cx = 0, cy = 0, cw = 0, ch = 0;
    int i, best = -1;
    float bd = 1e9f;
    if (!flow_caret_box(&cx, &cy, &cw, &ch)) return;
    for (i = 0; i < F.nl; i++) {
        FlLine *L = &F.ln[i];
        float d;
        if (L->para == F.cpara && F.coff >= L->start && F.coff <= L->end) continue;
        d = (dir > 0) ? (L->y - cy) : (cy - L->y);
        if (d <= 1.0f) continue;
        d = fabsf(d - F.p[L->para].lineSpacing * 14.0f) + fabsf(L->x - cx) * 0.01f;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    {
        FlLine *L = &F.ln[best];
        FlPara *p = &F.p[L->para];
        float x = L->x;
        int k, off = L->start;
        for (k = L->start; k < L->end; k++) {
            float w = ch_w(p, k) + (p->cp[k] == ' ' ? L->spExtra : 0);
            if (cx < x + w * 0.5f) break;
            x += w;
            off++;
        }
        caret_set(L->para, off);
    }
    if (!shift) sel_clear();
}

static int is_word_cp(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c >= 0x00C0;
}

static void move_word(int dir, int shift)
{
    if (F.cpara < 0 || F.cpara >= F.np) return;
    if (dir < 0) {
        while (F.coff > 0 && !is_word_cp(F.p[F.cpara].cp[F.coff - 1])) F.coff--;
        while (F.coff > 0 && is_word_cp(F.p[F.cpara].cp[F.coff - 1])) F.coff--;
    } else {
        int n = F.p[F.cpara].n;
        while (F.coff < n && !is_word_cp(F.p[F.cpara].cp[F.coff])) F.coff++;
        while (F.coff < n && is_word_cp(F.p[F.cpara].cp[F.coff])) F.coff++;
    }
    caret_set(F.cpara, F.coff);
    if (!shift) sel_clear();
}

void flow_char(int ch)
{
    int cp[2];
    if (!F.ready || ch < 32) return;
    snap_push();
    if (flow_has_selection()) delete_selection();
    cp[0] = ch;
    insert_cps(cp, 1);
    layout();
}

/* следующая ячейка таблицы; в конце таблицы — новая строка */
void flow_table_next_cell(void)
{
    int id = flow_in_table(), i;

    if (!id) return;
    snap_push();
    for (i = F.cpara + 1; i < F.np; i++) {
        if (F.p[i].tbl != id) break;
        caret_set(i, 0);
        sel_clear();
        layout();
        return;
    }
    F.nundo--; snap_free(&F.undo[F.nundo]);
    flow_table_row_add();
}

void flow_vkey(int vk, int ctrl, int shift)
{
    if (!F.ready) return;

    /* Tab в таблице — переход к следующей ячейке (как в Word) */
    if (vk == VK_TAB && flow_in_table()) {
        flow_table_next_cell();
        return;
    }

    switch (vk) {
    case VK_LEFT:  if (ctrl) move_word(-1, shift); else move_h(-1, shift); return;
    case VK_RIGHT: if (ctrl) move_word(1, shift); else move_h(1, shift); return;
    case VK_UP:    move_v(-1, shift); return;
    case VK_DOWN:  move_v(1, shift); return;
    case VK_HOME:
        if (ctrl) { caret_set(0, 0); }
        else {
            int i;
            for (i = 0; i < F.nl; i++) {
                FlLine *L = &F.ln[i];
                if (L->para == F.cpara && F.coff >= L->start && F.coff <= L->end) {
                    caret_set(F.cpara, L->start);
                    break;
                }
            }
        }
        if (!shift) sel_clear();
        return;
    case VK_END:
        if (ctrl) { caret_set(F.np - 1, F.p[F.np - 1].n); }
        else {
            int i;
            for (i = 0; i < F.nl; i++) {
                FlLine *L = &F.ln[i];
                if (L->para == F.cpara && F.coff >= L->start && F.coff <= L->end) {
                    caret_set(F.cpara, L->end);
                    break;
                }
            }
        }
        if (!shift) sel_clear();
        return;
    case VK_BACK:
        snap_push();
        if (flow_has_selection()) delete_selection();
        else if (F.coff > 0) {
            para_delete(&F.p[F.cpara], F.coff - 1, 1);
            F.coff--;
            F.dirty = 1;
        } else if (F.cpara > 0 && !flow_in_table()) merge_with_prev();
        else { F.nundo--; snap_free(&F.undo[F.nundo]); }
        layout();
        return;
    case VK_DELETE:
        snap_push();
        if (flow_has_selection()) delete_selection();
        else if (F.cpara < F.np && F.coff < F.p[F.cpara].n) {
            para_delete(&F.p[F.cpara], F.coff, 1);
            F.dirty = 1;
        } else if (F.cpara + 1 < F.np && !flow_in_table()) merge_with_next();
        else { F.nundo--; snap_free(&F.undo[F.nundo]); }
        layout();
        return;
    case VK_RETURN:
        if (flow_in_table()) { flow_table_next_cell(); return; }
        snap_push();
        if (flow_has_selection()) delete_selection();
        split_para();
        layout();
        return;
    case 'A':
        if (ctrl) flow_select_all();
        return;
    case 'Z':
        if (ctrl) snap_undo();
        return;
    case 'Y':
        if (ctrl) snap_redo();
        return;
    }
}

void flow_paste_text(const char *utf8)
{
    int n, i = 0, cnt = 0;
    int *cp;
    if (!F.ready || !utf8) return;
    n = (int)strlen(utf8);
    cp = (int *)malloc((size_t)n * sizeof(int) + 4);
    if (!cp) return;
    while (i < n) {
        unsigned char c = (unsigned char)utf8[i];
        int v = c;
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            v = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            v = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            v = ((c & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) |
                ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F); i += 4;
        } else { i += 1; v = '?'; }
        if (v == '\r') continue;
        cp[cnt++] = v;
    }
    snap_push();
    if (flow_has_selection()) delete_selection();
    if (cnt > 0 && is_space(cp[0]) && F.cpara >= 0 && F.cpara < F.np &&
        F.coff > 0 && F.p[F.cpara].n > 0 &&
        is_space(F.p[F.cpara].cp[F.coff - 1])) {
        memmove(cp, cp + 1, (size_t)(cnt - 1) * sizeof(int));
        cnt--;
    }
    {
        int start = 0, k;
        for (k = 0; k <= cnt; k++) {
            if (k == cnt || cp[k] == '\n') {
                if (k > start) insert_cps(cp + start, k - start);
                if (k < cnt) split_para();
                start = k + 1;
            }
        }
    }
    free(cp);
    layout();
}

char *flow_selected_utf8(void)
{
    int pa, oa, pb, ob, total = 0, i, pos = 0;
    char *buf;
    if (!F.ready) return NULL;
    if (!flow_has_selection()) {
        /* без выделения — весь документ */
        pa = 0; oa = 0;
        pb = F.np - 1;
        ob = F.np > 0 ? F.p[F.np - 1].n : 0;
    } else {
        sel_ordered(&pa, &oa, &pb, &ob);
    }
    for (i = pa; i <= pb && i < F.np; i++) total += F.p[i].n * 4 + 8;
    buf = (char *)malloc((size_t)total + 8);
    if (!buf) return NULL;
    for (i = pa; i <= pb && i < F.np; i++) {
        int s = (i == pa) ? oa : 0;
        int e = (i == pb) ? ob : F.p[i].n;
        int k;
        if (i > pa) buf[pos++] = '\n';
        for (k = s; k < e; k++) {
            int c = F.p[i].cp[k];
            if (c < 0x80) buf[pos++] = (char)c;
            else if (c < 0x800) {
                buf[pos++] = (char)(0xC0 | (c >> 6));
                buf[pos++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                buf[pos++] = (char)(0xE0 | (c >> 12));
                buf[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (c & 0x3F));
            } else {
                buf[pos++] = (char)(0xF0 | (c >> 18));
                buf[pos++] = (char)(0x80 | ((c >> 12) & 0x3F));
                buf[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
                buf[pos++] = (char)(0x80 | (c & 0x3F));
            }
        }
    }
    buf[pos] = 0;
    return buf;
}

/* ------------------------------------------------------------------ */
/* форматирование                                                      */

void flow_fmt_get(FlowFmt *f)
{
    FlPara *p;
    FlSt s;
    if (!f) return;
    memset(f, 0, sizeof(*f));
    if (!F.ready || F.np <= 0) {
        f->size = 11.0f;
        f->spacing = 1.0f;
        return;
    }
    if (F.cpara < 0) F.cpara = 0;
    if (F.cpara >= F.np) F.cpara = F.np - 1;
    p = &F.p[F.cpara];
    s = p->n > 0 ? st_at(p, F.coff > 0 ? F.coff - 1 : 0) : F.pend;
    f->size = s.size;
    f->color = s.color;
    f->bold = (s.attr & FAT_BOLD) ? 1 : 0;
    f->italic = (s.attr & FAT_ITAL) ? 1 : 0;
    f->underline = (s.attr & FAT_UNDL) ? 1 : 0;
    f->font = s.font;
    f->align = p->align;
    f->list = p->list;
    f->spacing = p->lineSpacing;
    f->indent = p->indent;
    f->hasSel = flow_has_selection();
}

/* границы слова под кареткой: без выделения формат меняется у слова */
static void word_bounds(FlPara *p, int off, int *a, int *b)
{
    int s = off, e = off;
    if (!p || p->isImage) { *a = *b = 0; return; }
    while (s > 0 && !is_space(p->cp[s - 1])) s--;
    while (e < p->n && !is_space(p->cp[e])) e++;
    if (s == e) {
        /* каретка на пробеле или в конце — меняем символ справа */
        if (e < p->n) e++;
        else if (s > 0) s--;
    }
    *a = s;
    *b = e;
}

int flow_fmt_list(void)
{
    if (!F.ready || F.np <= 0) return 0;
    if (F.cpara < 0 || F.cpara >= F.np) return 0;
    return F.p[F.cpara].list;
}

/* изменить стили в диапазоне; what: 'a' attr, 's' size, 'c' color */
static void style_range(int what, int ival, unsigned cval)
{
    int pa, oa, pb, ob, i;
    int haveSel = flow_has_selection();

    if (!F.ready || F.np <= 0) return;
    if (haveSel) sel_ordered(&pa, &oa, &pb, &ob);
    else {
        pa = pb = F.cpara;
        if (pa < 0) return;
        word_bounds(&F.p[pa], F.coff, &oa, &ob);
    }

    for (i = pa; i <= pb && i < F.np; i++) {
        FlPara *p = &F.p[i];
        int from = (i == pa) ? oa : 0;
        int to = (i == pb) ? ob : p->n;
        int k;
        if (p->isImage) continue;
        for (k = from; k < to && k < p->n; k++) {
            FlSt s = st_at(p, k);
            if (what == 'a') {
                if (ival > 0) s.attr |= (unsigned char)ival;
                else s.attr = (unsigned char)(s.attr & ~(unsigned char)(-ival));
            } else if (what == 's') {
                s.size = (float)ival;
            } else if (what == 'c') {
                s.color = cval;
            } else if (what == 'f') {
                s.font = (unsigned char)(ival < 0 || ival >= FL_NFAM ? 0 : ival);
            }
            p->sty[k] = (unsigned char)st_index(p, s);
        }
    }

    /* если выделения нет — запоминаем формат для набора */
    if (!haveSel && F.cpara >= 0 && F.cpara < F.np) {
        FlPara *p = &F.p[F.cpara];
        FlSt s = p->n > 0 ? st_at(p, F.coff > 0 ? F.coff - 1 : 0) : F.pend;
        if (what == 'a') {
            if (ival > 0) s.attr |= (unsigned char)ival;
            else s.attr = (unsigned char)(s.attr & ~(unsigned char)(-ival));
        } else if (what == 's') s.size = (float)ival;
        else if (what == 'c') s.color = cval;
        else if (what == 'f') s.font = (unsigned char)(ival < 0 || ival >= FL_NFAM ? 0 : ival);
        F.pend = s;
    }
    F.dirty = 1;
    layout();
}

void flow_fmt_attr(int bit)
{
    FlowFmt f;
    flow_fmt_get(&f);
    snap_push();
    if (bit == FAT_BOLD)      style_range('a', f.bold ? -FAT_BOLD : FAT_BOLD, 0);
    else if (bit == FAT_ITAL) style_range('a', f.italic ? -FAT_ITAL : FAT_ITAL, 0);
    else if (bit == FAT_UNDL) style_range('a', f.underline ? -FAT_UNDL : FAT_UNDL, 0);
}

void flow_fmt_size(float absSize)
{
    int v = (int)(absSize + 0.5f);
    if (v < 4) v = 4;
    if (v > 200) v = 200;
    snap_push();
    style_range('s', v, 0);
}

void flow_fmt_size_step(int dir)
{
    FlowFmt f;
    float next;
    flow_fmt_get(&f);
    if (dir > 0) {
        if (f.size < 11.5f) next = 12;
        else if (f.size < 13.5f) next = 14;
        else if (f.size < 15.5f) next = 16;
        else if (f.size < 17.5f) next = 18;
        else next = f.size + 4;
    } else {
        if (f.size > 17.5f) next = 18;
        else if (f.size > 15.5f) next = 16;
        else if (f.size > 13.5f) next = 14;
        else if (f.size > 11.5f) next = 12;
        else next = 10;
    }
    flow_fmt_size(next);
}

const wchar_t *flow_font_name(int fam)
{
    if (fam < 0 || fam >= FL_NFAM) fam = 0;
    return g_famShort[fam];
}

void flow_fmt_font(int fam)
{
    snap_push();
    style_range('f', fam, 0);
}

void flow_fmt_color(unsigned c)
{
    snap_push();
    style_range('c', 0, c);
}

/* свойства абзаца — на все абзацы выделения (или текущий) */
static void para_range_props(void (*fn)(FlPara *, int), int arg)
{
    int pa, pb, i;
    if (!F.ready || F.np <= 0) return;
    if (flow_has_selection()) {
        int oa, ob;
        sel_ordered(&pa, &oa, &pb, &ob);
    } else { pa = pb = F.cpara; }
    if (pa < 0) pa = 0;
    if (pb >= F.np) pb = F.np - 1;
    for (i = pa; i <= pb; i++) fn(&F.p[i], arg);
    F.dirty = 1;
    layout();
}

static void set_align_fn(FlPara *p, int a) { p->align = a; }
static void set_list_fn(FlPara *p, int k) { p->list = k; }
static void set_indent_fn(FlPara *p, int d)
{
    p->indent += (float)d;
    if (p->indent < 0) p->indent = 0;
    if (p->indent > 200) p->indent = 200;
}
static void set_spbefore_fn(FlPara *p, int d)
{
    p->spaceBefore += (float)d;
    if (p->spaceBefore < 0) p->spaceBefore = 0;
    if (p->spaceBefore > 80) p->spaceBefore = 80;
}

void flow_align(int a) { snap_push(); para_range_props(set_align_fn, a); }
void flow_list(int kind) { snap_push(); para_range_props(set_list_fn, kind); }
void flow_indent(int dir) { snap_push(); para_range_props(set_indent_fn, dir * 12); }
void flow_space_before(int dir) { snap_push(); para_range_props(set_spbefore_fn, dir * 6); }

void flow_spacing_next(void)
{
    int pa, pb, i;
    float v = 1.0f, next = 1.0f;
    if (!F.ready || F.np <= 0) return;
    v = F.p[F.cpara].lineSpacing;
    if (v < 1.05f) next = 1.15f;
    else if (v < 1.2f) next = 1.5f;
    else if (v < 1.7f) next = 2.0f;
    else next = 1.0f;
    snap_push();
    if (flow_has_selection()) {
        int oa, ob;
        sel_ordered(&pa, &oa, &pb, &ob);
    } else { pa = pb = F.cpara; }
    for (i = pa; i <= pb && i < F.np; i++) F.p[i].lineSpacing = next;
    F.dirty = 1;
    layout();
}

/* ------------------------------------------------------------------ */
/* параметры страницы                                                  */

void flow_page_metrics(float *ml, float *mt, float *mr, float *mb, int *landscape, int *sizeIdx)
{
    if (ml) *ml = F.margin_l;
    if (mt) *mt = F.margin_t;
    if (mr) *mr = F.margin_r;
    if (mb) *mb = F.margin_b;
    if (landscape) *landscape = F.landscape;
    if (sizeIdx) *sizeIdx = F.pageSize;
}

void flow_page_setup(int sizeIdx, int landscape)
{
    if (!F.ready) return;
    snap_push();
    F.pageSize = sizeIdx;
    F.landscape = landscape;
    page_geometry(sizeIdx, landscape, &F.pw, &F.ph);
    F.dirty = 1;
    layout();
}

void flow_margins_mm(float ml, float mt, float mr, float mb)
{
    if (!F.ready) return;
    snap_push();
    F.margin_l = ml * 2.8346f;
    F.margin_t = mt * 2.8346f;
    F.margin_r = mr * 2.8346f;
    F.margin_b = mb * 2.8346f;
    if (F.margin_l < 6) F.margin_l = 6;
    if (F.margin_t < 6) F.margin_t = 6;
    if (F.margin_r < 6) F.margin_r = 6;
    if (F.margin_b < 6) F.margin_b = 6;
    F.dirty = 1;
    layout();
}

/* ------------------------------------------------------------------ */
/* колонтитулы                                                         */

void flow_set_header(const char *utf8)
{
    lstrcpynA(F.hdr, utf8 ? utf8 : "", 256);
    F.dirty = 1;
}

void flow_set_footer(const char *utf8)
{
    lstrcpynA(F.ftr, utf8 ? utf8 : "", 256);
    F.dirty = 1;
}

void flow_set_pagenum(int on) { F.pgnum = on ? 1 : 0; F.dirty = 1; }
int  flow_get_pagenum(void) { return F.pgnum; }
char *flow_get_header(void) { return F.hdr; }
char *flow_get_footer(void) { return F.ftr; }

/* ------------------------------------------------------------------ */
/* таблицы и картинки                                                  */

int flow_in_table(void)
{
    if (!F.ready || F.cpara < 0 || F.cpara >= F.np) return 0;
    return F.p[F.cpara].tbl;
}

static void cell_defaults(FlPara *q, int id, int row, int col, int ncols)
{
    FlSt st;
    q->tbl = id;
    q->trow = row;
    q->tcol = col;
    q->tcols = ncols;
    q->align = 0;
    q->list = 0;
    q->indent = 0;
    q->spaceBefore = 0;
    q->spaceAfter = 0;
    q->lineSpacing = 1.0f;
    st = F.pend;
    /* в ячейке не должно оказаться 18-пунктового «кегля заголовка»
       и жирного начертания заголовка */
    if (st.size <= 1.0f || st.size > 12.0f) st.size = 11.0f;
    st.attr = 0;
    st_index(q, st);
}

int flow_insert_table(int rows, int cols)
{
    int at, id, r, c, first;

    if (!F.ready) return 0;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows > 40) rows = 40;
    if (cols > 10) cols = 10;

    snap_push();
    at = (F.cpara >= 0 && F.cpara < F.np) ? F.cpara + 1 : F.np;
    id = ++F.tblSeq;
    first = at;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            FlPara *q = para_ins(at);
            if (!q) break;
            cell_defaults(q, id, r, c, cols);
            at++;
        }
    }
    F.cpara = first;
    F.coff = 0;
    F.apara = first;
    F.aoff = 0;
    F.dirty = 1;
    layout();
    return 1;
}

int flow_table_row_add(void)
{
    int id = flow_in_table(), last = -1, i, ncols, row, at;

    if (!id) return 0;
    ncols = F.p[F.cpara].tcols;
    row = 0;
    for (i = 0; i < F.np; i++) {
        if (F.p[i].tbl != id) continue;
        last = i;
        if (F.p[i].trow + 1 > row) row = F.p[i].trow + 1;
    }
    if (last < 0) return 0;
    snap_push();
    at = last + 1;
    for (i = 0; i < ncols; i++) {
        FlPara *q = para_ins(at++);
        if (!q) break;
        cell_defaults(q, id, row, i, ncols);
    }
    F.cpara = last + 1;
    F.coff = 0;
    F.dirty = 1;
    layout();
    return 1;
}

int flow_table_col_add(void)
{
    int id = flow_in_table(), i, ncols, row, lastrow = -1, at;

    if (!id) return 0;
    ncols = F.p[F.cpara].tcols;
    if (ncols >= 10) return 0;
    for (i = 0; i < F.np; i++)
        if (F.p[i].tbl == id && F.p[i].trow > lastrow) lastrow = F.p[i].trow;
    if (lastrow < 0) return 0;
    snap_push();
    /* новые ячейки дописываем в конец каждой строки */
    for (row = lastrow; row >= 0; row--) {
        at = -1;
        for (i = 0; i < F.np; i++)
            if (F.p[i].tbl == id && F.p[i].trow == row) at = i + 1;
        if (at < 0) continue;
        {
            FlPara *q = para_ins(at);
            if (q) cell_defaults(q, id, row, ncols, ncols + 1);
        }
    }
    for (i = 0; i < F.np; i++)
        if (F.p[i].tbl == id) F.p[i].tcols = ncols + 1;
    F.cpara = 0;
    for (i = 0; i < F.np; i++)
        if (F.p[i].tbl == id && F.p[i].trow == 0 && F.p[i].tcol == ncols) F.cpara = i;
    F.coff = 0;
    F.dirty = 1;
    layout();
    return 1;
}

int flow_table_delete(void)
{
    int id = flow_in_table(), i, n = 0, at = -1;

    if (!id) return 0;
    snap_push();
    for (i = 0; i < F.np; i++) {
        if (F.p[i].tbl == id) {
            if (at < 0) at = i;
            para_free(&F.p[i]);
            n++;
        }
    }
    /* сдвигаем оставшиеся абзацы */
    {
        int w = 0;
        for (i = 0; i < F.np; i++) {
            if (F.p[i].tbl == id) continue;
            if (w != i) F.p[w] = F.p[i];
            w++;
        }
        F.np = w;
    }
    if (at >= F.np) at = F.np - 1;
    if (at < 0) at = 0;
    F.cpara = at;
    F.coff = 0;
    F.apara = at;
    F.aoff = 0;
    F.dirty = 1;
    layout();
    return 1;
}

int flow_insert_image(const wchar_t *wpath)
{
    FILE *f;
    long len;
    unsigned char *data;
    fz_buffer *buf = NULL;
    fz_image *img = NULL;
    FlPara *p;
    int ok = 0;

    if (!F.ready || !F.d || !F.d->ctx || !wpath) return 0;
    f = _wfopen(wpath, L"rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 40L * 1024 * 1024) { fclose(f); return 0; }
    data = (unsigned char *)malloc((size_t)len);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) { free(data); fclose(f); return 0; }
    fclose(f);

    fz_try(F.d->ctx) {
        buf = fz_new_buffer_from_copied_data(F.d->ctx, data, (size_t)len);
        img = fz_new_image_from_buffer(F.d->ctx, buf);
    }
    fz_catch(F.d->ctx) { img = NULL; }
    free(data);
    if (buf) fz_drop_buffer(F.d->ctx, buf);
    if (!img) return 0;

    snap_push();
    {
        int at = (F.cpara >= 0 && F.cpara < F.np) ? F.cpara + 1 : F.np;
        float px;
        /* если курсор в таблице — вставляем после всей таблицы, иначе её разорвёт */
        if (at > 0 && at <= F.np && F.p[at - 1].tbl) {
            int id = F.p[at - 1].tbl, i2;
            while (at < F.np && F.p[at].tbl == id) at++;
            (void)i2;
        }
        p = para_ins(at);
        if (!p) return 0;
        p->isImage = 1;
        p->img = img;
        p->spaceBefore = 8;
        /* пиксели -> пункты по разрешению картинки */
        px = img->xres > 1 ? (float)img->xres : 96.0f;
        p->imgW = (float)img->w * 72.0f / px;
        p->imgH = (float)img->h * 72.0f / px;
        if (p->imgH > F.ph * 0.7f) {
            float k = F.ph * 0.7f / p->imgH;
            p->imgW *= k;
            p->imgH *= k;
        }
        F.cpara = at + 1 < F.np ? at + 1 : at;
        F.coff = 0;
        F.apara = F.cpara;          /* снять выделение: иначе остаются синие полосы */
        F.aoff = F.coff;
        F.dirty = 1;
        layout();
        ok = 1;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* поиск и замена                                                      */

static int *utf8_to_cp(const char *s, int *outN)
{
    int n = (int)strlen(s), i = 0, cnt = 0;
    int *cp = (int *)malloc(((size_t)n + 1) * sizeof(int));
    if (!cp) { if (outN) *outN = 0; return NULL; }
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int v = c;
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            v = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            v = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            v = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); i += 4;
        } else { i += 1; v = '?'; }
        cp[cnt++] = v;
    }
    if (outN) *outN = cnt;
    return cp;
}

static int cp_lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c == 0x0401) return 0x0451;
    return c;
}

static int find_in_para(FlPara *p, const int *needle, int nn, int from)
{
    int i, k;
    if (nn <= 0 || p->isImage || p->n < nn) return -1;
    for (i = from; i + nn <= p->n; i++) {
        for (k = 0; k < nn; k++)
            if (cp_lower(p->cp[i + k]) != cp_lower(needle[k])) break;
        if (k == nn) return i;
    }
    return -1;
}

int flow_find_next(const char *utf8, int fromStart)
{
    int nn = 0, *needle, startPara, i;
    if (!F.ready || !utf8 || !utf8[0]) return 0;
    needle = utf8_to_cp(utf8, &nn);
    if (!needle || nn <= 0) { free(needle); return 0; }

    if (fromStart || F.np <= 0) startPara = 0;
    else startPara = F.cpara;

    for (i = startPara; i < F.np; i++) {
        int from = 0;
        int pos;
        if (!fromStart && i == F.cpara) from = F.coff;
        pos = find_in_para(&F.p[i], needle, nn, from);
        if (pos < 0 && !fromStart && from > 0)
            pos = find_in_para(&F.p[i], needle, nn, 0);
        if (pos >= 0) {
            F.apara = i; F.aoff = pos;
            F.cpara = i; F.coff = pos + nn;
            free(needle);
            layout();
            return 1;
        }
    }
    /* с начала документа, если дошли до конца */
    if (!fromStart) {
        free(needle);
        return flow_find_next(utf8, 1);
    }
    free(needle);
    return 0;
}

int flow_replace_all(const char *utf8find, const char *utf8repl)
{
    int nn = 0, nr = 0, *needle, *repl, i, count = 0;
    if (!F.ready || !utf8find || !utf8find[0]) return 0;
    needle = utf8_to_cp(utf8find, &nn);
    repl = utf8_to_cp(utf8repl ? utf8repl : "", &nr);
    if (!needle || nn <= 0) { free(needle); free(repl); return 0; }

    snap_push();
    for (i = 0; i < F.np; i++) {
        FlPara *p = &F.p[i];
        int from = 0, pos;
        if (p->isImage) continue;
        while ((pos = find_in_para(p, needle, nn, from)) >= 0) {
            int idx = p->n > 0 && pos < p->n ? p->sty[pos] :
                      (p->nst > 0 ? p->st[0].attr : 0);
            para_delete(p, pos, nn);
            if (nr > 0) para_insert(p, pos, repl, nr, idx);
            from = pos + nr;
            count++;
            if (count > 100000) break;
        }
    }
    free(needle);
    free(repl);
    if (count > 0) { F.dirty = 1; layout(); }
    else { F.nundo--; snap_free(&F.undo[F.nundo]); }
    return count;
}

int flow_replace_one(const char *utf8find, const char *utf8repl)
{
    int nn = 0, nr = 0, *needle, *repl, pos;
    if (!F.ready || !flow_has_selection()) return 0;
    needle = utf8_to_cp(utf8find, &nn);
    repl = utf8_to_cp(utf8repl ? utf8repl : "", &nr);
    if (!needle || nn <= 0 || F.cpara >= F.np) { free(needle); free(repl); return 0; }
    pos = find_in_para(&F.p[F.cpara], needle, nn, F.aoff);
    if (pos != F.aoff) { free(needle); free(repl); return 0; }
    snap_push();
    {
        FlPara *p = &F.p[F.cpara];
        int idx = p->n > 0 && pos < p->n ? p->sty[pos] : 0;
        para_delete(p, pos, nn);
        if (nr > 0) para_insert(p, pos, repl, nr, idx);
        F.coff = pos + nr;
        sel_clear();
    }
    free(needle);
    free(repl);
    F.dirty = 1;
    layout();
    return 1;
}

/* ------------------------------------------------------------------ */
/* статистика                                                          */

void flow_stats(int *pages, int *paras, int *chars)
{
    int i, n = 0;
    for (i = 0; i < F.np; i++) n += F.p[i].n;
    if (pages) *pages = flow_pages();
    if (paras) *paras = F.np;
    if (chars) *chars = n;
}

int flow_words(void)
{
    int i, k, words = 0, inWord = 0;
    for (i = 0; i < F.np; i++) {
        FlPara *p = &F.p[i];
        if (p->isImage) continue;
        for (k = 0; k < p->n; k++) {
            if (is_space(p->cp[k])) inWord = 0;
            else if (!inWord) { inWord = 1; words++; }
        }
        inWord = 0;
    }
    return words;
}

/* ------------------------------------------------------------------ */
/* шрифт и отрисовка                                                   */

static int   g_fontTried;
static HANDLE g_fontMem;
static wchar_t g_face[LF_FACESIZE] = L"Arial";

static void ensure_ui_font(PdfDoc *d)
{
    int len = 0, sub = 0;
    DWORD cnt = 0;
    const unsigned char *data;
    HDC dc;
    HFONT hf, old;
    wchar_t got[LF_FACESIZE];

    if (g_fontTried || !d || !d->ctx) return;
    g_fontTried = 1;

    data = fz_lookup_noto_font(d->ctx, UCDN_SCRIPT_LATIN, FZ_LANG_UNSET, &len, &sub);
    if (!data || len <= 0) return;
    g_fontMem = AddFontMemResourceEx((PVOID)data, (DWORD)len, NULL, &cnt);
    if (!g_fontMem) return;

    lstrcpyW(g_face, L"Noto Sans");
    dc = GetDC(NULL);
    {
        LOGFONTW lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -20;
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpyW(lf.lfFaceName, g_face);
        hf = CreateFontIndirectW(&lf);
        old = (HFONT)SelectObject(dc, hf);
        got[0] = 0;
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        DeleteObject(hf);
    }
    ReleaseDC(NULL, dc);
    if (lstrcmpiW(got, L"Noto Sans") != 0) lstrcpyW(g_face, L"Arial");
}

#define FC_MAX 96
typedef struct { int key; HFONT hf; } FontSlot;
static FontSlot g_fc[FC_MAX];
static int g_fcN;

static void fc_clear(void)
{
    int i;
    for (i = 0; i < g_fcN; i++) if (g_fc[i].hf) DeleteObject(g_fc[i].hf);
    g_fcN = 0;
}

static HFONT font_for(FlSt s, float scale)
{
    int px = (int)(s.size * scale + 0.5f);
    int fam = s.font < FL_NFAM ? s.font : 0;
    int key;
    int i;
    LOGFONTW lf;

    if (px < 1) px = 1;
    key = ((px & 0x3FF) << 8) | ((s.attr & 7) << 4) | fam;
    for (i = 0; i < g_fcN; i++)
        if (g_fc[i].key == key) return g_fc[i].hf;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = (s.attr & FAT_BOLD) ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)((s.attr & FAT_ITAL) ? 1 : 0);
    lf.lfUnderline = (BYTE)((s.attr & FAT_UNDL) ? 1 : 0);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lstrcpyW(lf.lfFaceName, g_famGdi[fam]);

    if (g_fcN >= FC_MAX) fc_clear();
    g_fc[g_fcN].key = key;
    g_fc[g_fcN].hf = CreateFontIndirectW(&lf);
    return g_fc[g_fcN++].hf;
}

static void ensure_image_bmp(PdfDoc *d, FlPara *p)
{
    fz_pixmap *pix;
    if (p->bmp || !p->img) return;
    pix = NULL;
    fz_try(d->ctx) {
        pix = fz_get_pixmap_from_image(d->ctx, p->img, NULL, NULL, NULL, NULL);
    }
    fz_catch(d->ctx) { pix = NULL; }
    if (pix) {
        fz_pixmap *rgb = NULL;
        fz_try(d->ctx) {
            rgb = fz_convert_pixmap(d->ctx, pix, fz_device_bgr(d->ctx), NULL, NULL,
                                    fz_default_color_params, 1);
        }
        fz_catch(d->ctx) { rgb = NULL; }
        if (rgb) {
            int w = fz_pixmap_width(d->ctx, rgb);
            int h = fz_pixmap_height(d->ctx, rgb);
            int stride = fz_pixmap_stride(d->ctx, rgb);
            int ncomp = fz_pixmap_components(d->ctx, rgb);
            BITMAPINFO bi;
            HDC dc = GetDC(NULL);
            void *bits = NULL;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bi.bmiHeader.biCompression = BI_RGB;
            p->bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (p->bmp && bits) {
                int yy, xx;
                const unsigned char *src = fz_pixmap_samples(d->ctx, rgb);
                unsigned char *dst = (unsigned char *)bits;
                for (yy = 0; yy < h; yy++) {
                    const unsigned char *s2 = src + (size_t)yy * stride;
                    unsigned char *t = dst + (size_t)yy * (size_t)w * 3;
                    for (xx = 0; xx < w; xx++) {
                        if (ncomp >= 3) {
                            /* fz_device_bgr даёт уже B,G,R — ровно как в 24-битном DIB */
                            t[xx * 3 + 0] = s2[xx * ncomp + 0];
                            t[xx * 3 + 1] = s2[xx * ncomp + 1];
                            t[xx * 3 + 2] = s2[xx * ncomp + 2];
                        } else {
                            t[xx * 3 + 0] = t[xx * 3 + 1] = t[xx * 3 + 2] = s2[xx * ncomp];
                        }
                    }
                }
                p->bwpx = w;
                p->bhpx = h;
            }
            fz_drop_pixmap(d->ctx, rgb);
        }
        fz_drop_pixmap(d->ctx, pix);
    }
}

/* UTF-8 -> кодпоинты */
static int utf8_cps(const char *s, int *out, int max)
{
    int i = 0, n = 0, len;
    if (!s) return 0;
    len = (int)strlen(s);
    while (i < len && n < max) {
        unsigned char c = (unsigned char)s[i];
        int v = c;
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            v = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            v = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            v = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); i += 4;
        } else { i += 1; v = '?'; }
        out[n++] = v;
    }
    return n;
}

static float cps_width(const int *cp, int n, float size)
{
    float w = 0;
    int i;
    for (i = 0; i < n; i++) w += cpw(F.d, cp[i], size);
    return w;
}

static void paint_hf(HDC mem, int page, float scale)
{
    int cps[300], n;
    float size = 9.0f;
    HFONT hf = font_for((FlSt){ size, 0, 0 }, scale);
    HGDIOBJ old = SelectObject(mem, hf);
    float pageW = F.pw - F.margin_l - F.margin_r;

    SetTextColor(mem, RGB(70, 70, 70));
    SetTextAlign(mem, TA_LEFT | TA_BASELINE);

    if (F.hdr[0]) {
        n = utf8_cps(F.hdr, cps, 300);
        {
            float w = cps_width(cps, n, size);
            int i, k = 0;
            float x = F.margin_l + (pageW - w) * 0.5f;
            for (i = 0; i < n; i++) {
                wchar_t wc = (wchar_t)cps[i];
                if (w >= 0) TextOutW(mem, (int)(x * scale), (int)((F.margin_t * 0.55f) * scale),
                                     &wc, 1);
                x += cpw(F.d, cps[i], size);
                k++;
            }
            (void)k;
        }
    }
    if (F.ftr[0] || F.pgnum) {
        wchar_t num[16];
        int i, len;
        float total = 0, x;

        num[0] = 0;
        if (F.pgnum) wsprintfW(num, L"%d", page + 1);
        n = F.ftr[0] ? utf8_cps(F.ftr, cps, 300) : 0;
        total = cps_width(cps, n, size);
        if (F.pgnum) {
            float nw = 0;
            len = (int)lstrlenW(num);
            for (i = 0; i < len; i++) nw += cpw(F.d, (int)num[i], size);
            total += nw + (n > 0 ? size * 0.5f : 0);
        }
        x = F.margin_l + (pageW - total) * 0.5f;
        for (i = 0; i < n; i++) {
            wchar_t wc = (wchar_t)cps[i];
            TextOutW(mem, (int)(x * scale), (int)((F.ph - F.margin_b * 0.45f) * scale), &wc, 1);
            x += cpw(F.d, cps[i], size);
        }
        if (F.pgnum) {
            if (n > 0) x += size * 0.5f;
            len = (int)lstrlenW(num);
            for (i = 0; i < len; i++) {
                TextOutW(mem, (int)(x * scale), (int)((F.ph - F.margin_b * 0.45f) * scale),
                         &num[i], 1);
                x += cpw(F.d, (int)num[i], size);
            }
        }
    }
    SelectObject(mem, old);
}

static void draw_marker(HDC dc, FlPara *p, float scale, float xText, float yBaseline,
                        float size)
{
    wchar_t buf[24];
    HFONT hf;
    int w;
    if (!p->list) return;
    if (p->list == 1) lstrcpyW(buf, L"\x2022");
    else wsprintfW(buf, L"%d.", p->listNum > 0 ? p->listNum : 1);
    hf = font_for((FlSt){ size, 0, 0 }, scale);
    {
        HGDIOBJ old = SelectObject(dc, hf);
        SIZE sz;
        SetTextColor(dc, RGB(0, 0, 0));
        GetTextExtentPoint32W(dc, buf, (int)lstrlenW(buf), &sz);
        w = sz.cx;
        TextOutW(dc, (int)((xText - 6) * scale) - w, (int)(yBaseline * scale), buf, (int)lstrlenW(buf));
        SelectObject(dc, old);
    }
}

void flow_paint(HDC dc, int page, float scale, int ox, int oy)
{
    HDC mem;
    HBITMAP bmp, oldbmp;
    int W, H, i;
    int selA, selO, selB, selE, hasSel;

    if (!F.ready) return;
    ensure_ui_font(F.d);
    if (F.nl == 0) layout();
    if (page < 0) page = 0;
    if (page >= F.npages) page = F.npages - 1;

    W = (int)(F.pw * scale + 0.5f);
    H = (int)(F.ph * scale + 0.5f);
    if (W < 8) W = 8;
    if (H < 8) H = 8;

    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, W, H);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);
    {
        RECT r;
        r.left = 0; r.top = 0; r.right = W; r.bottom = H;
        FillRect(mem, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }

    sel_ordered(&selA, &selO, &selB, &selE);
    hasSel = flow_has_selection();

    SetBkMode(mem, TRANSPARENT);
    SetTextAlign(mem, TA_LEFT | TA_BASELINE);

    /* выделение */
    if (hasSel) {
        HBRUSH hb = CreateSolidBrush(RGB(0x33, 0x99, 0xFF));
        for (i = 0; i < F.nl; i++) {
            FlLine *L = &F.ln[i];
            FlPara *p = &F.p[L->para];
            int s, e, k;
            float x0, x1, y0, y1;
            if (L->page != page || p->isImage) continue;
            if (L->para < selA || L->para > selB) continue;
            s = L->start;
            e = L->end;
            if (L->para == selA && s < selO) s = selO;
            if (L->para == selB && e > selE) e = selE;
            if (e < s) continue;
            x0 = L->x;
            x1 = L->x;
            for (k = L->start; k < e; k++) {
                float w = ch_w(p, k) + (p->cp[k] == ' ' ? L->spExtra : 0);
                if (k < s) x0 += w;
                x1 += w;
            }
            if (x1 <= x0) continue;     /* нулевое выделение не рисуем */
            y0 = L->y - line_max_size(p, L->start, L->end) * FL_ASC;
            y1 = y0 + line_max_size(p, L->start, L->end) * FL_LEAD * p->lineSpacing;
            {
                RECT r;
                r.left = (int)(x0 * scale);
                r.top = (int)(y0 * scale);
                r.right = (int)(x1 * scale) + 1;
                r.bottom = (int)(y1 * scale);
                FillRect(mem, &r, hb);
            }
        }
        DeleteObject(hb);
    }

    /* текст */
    for (i = 0; i < F.nl; i++) {
        FlLine *L = &F.ln[i];
        FlPara *p = &F.p[L->para];
        int k;
        float x;
        if (L->page != page || p->isImage) continue;

        if (p->list && L->first)
            draw_marker(mem, p, scale, L->x, L->y,
                        p->n > 0 ? st_at(p, 0).size : 11.0f);

        x = L->x;
        k = L->start;
        while (k < L->end) {
            FlSt s = st_at(p, k);
            HFONT hf = font_for(s, scale);
            HGDIOBJ old = SelectObject(mem, hf);
            int runStart = k;
            while (k < L->end) {
                FlSt s2 = st_at(p, k);
                if (s2.attr != s.attr || s2.font != s.font ||
                    fabsf(s2.size - s.size) > 0.05f || s2.color != s.color)
                    break;
                k++;
            }
            if (s.color == 0) SetTextColor(mem, RGB(0, 0, 0));
            else
                SetTextColor(mem, RGB((s.color >> 16) & 0xFF, (s.color >> 8) & 0xFF,
                                      s.color & 0xFF));
            SetTextAlign(mem, TA_LEFT | TA_BASELINE);
            {
                int j;
                for (j = runStart; j < k; j++) {
                    int cp = p->cp[j];
                    float w = cpw_style(F.d, s, cp);
                    if (cp != ' ') {
                        wchar_t wc = (wchar_t)(cp > 0xFFFF ? '?' : cp);
                        TextOutW(mem, (int)(x * scale + 0.5f), (int)(L->y * scale), &wc, 1);
                    }
                    x += w + (cp == ' ' ? L->spExtra : 0);
                }
            }
            SelectObject(mem, old);
        }
    }

    /* картинки */
    for (i = 0; i < F.np; i++) {
        FlPara *p = &F.p[i];
        if (!p->isImage || p->ipage != page) continue;
        ensure_image_bmp(F.d, p);
        if (p->bmp && p->bwpx > 0) {
            HDC tmp = CreateCompatibleDC(mem);
            HBITMAP ob = (HBITMAP)SelectObject(tmp, p->bmp);
            SetStretchBltMode(mem, HALFTONE);
            StretchBlt(mem, (int)(p->bx * scale), (int)(p->by * scale),
                       (int)(p->bw * scale), (int)(p->bh * scale),
                       tmp, 0, 0, p->bwpx, p->bhpx, SRCCOPY);
            SelectObject(tmp, ob);
            DeleteDC(tmp);
        }
    }

    /* рамки таблиц */
    {
        int drawn = 0;
        HGDIOBJ oldPen = NULL, oldBr = NULL;
        for (i = 0; i < F.np; i++) {
            FlPara *p = &F.p[i];
            if (!p->tbl || p->cellX1 <= p->cellX0 || p->ipage != page) continue;
            if (!drawn) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
                oldPen = SelectObject(mem, pen);
                oldBr = SelectObject(mem, GetStockObject(NULL_BRUSH));
                drawn = 1;
            }
            Rectangle(mem, (int)(p->cellX0 * scale), (int)(p->cellY0 * scale),
                      (int)(p->cellX1 * scale), (int)(p->cellY1 * scale));
        }
        if (drawn) {
            HGDIOBJ pen = SelectObject(mem, oldPen);
            SelectObject(mem, oldBr);
            DeleteObject(pen);      /* это наш PS_SOLID */
        }
    }

    /* колонтитулы */
    paint_hf(mem, page, scale);

    /* каретка */
    {
        float cx, cy, cw, chh;
        if (!hasSel && flow_caret_page() == page &&
            flow_caret_box(&cx, &cy, &cw, &chh)) {
            RECT r;
            r.left = (int)(cx * scale);
            r.top = (int)(cy * scale);
            r.right = r.left + 1;
            r.bottom = (int)((cy + chh) * scale);
            FillRect(mem, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
    }

    BitBlt(dc, ox, oy, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int flow_render(PdfDoc *d, int page, float scale, HBITMAP *out, int *ow, int *oh)
{
    int W, H;
    HDC dc, mem;
    HBITMAP bmp = NULL;
    void *bits = NULL;
    BITMAPINFO bi;

    if (!F.ready || !out) return 0;
    W = (int)(F.pw * scale + 0.5f);
    H = (int)(F.ph * scale + 0.5f);
    if (W < 8 || H < 8) return 0;

    dc = GetDC(NULL);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!bmp) return 0;

    mem = CreateCompatibleDC(NULL);
    SelectObject(mem, bmp);
    flow_paint(mem, page, scale, 0, 0);
    DeleteDC(mem);

    *out = bmp;
    if (ow) *ow = W;
    if (oh) *oh = H;
    (void)d;
    return 1;
}

/* ------------------------------------------------------------------ */
/* запись в PDF                                                        */

static char *utf8_of(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* ASCII-имя во временном файле рядом с целью (для путей с кириллицей) */
static int temp_near(const wchar_t *dst, wchar_t *out, int cnt)
{
    int i, cut = -1;
    if (!dst || !out || cnt < 32) return 0;
    for (i = 0; dst[i]; i++)
        if (dst[i] == L'\\' || dst[i] == L'/') cut = i;
    if (cut < 0 || cut + 24 >= cnt) return 0;
    memcpy(out, dst, (size_t)(cut + 1) * sizeof(wchar_t));
    out[cut + 1] = 0;
    lstrcatW(out, L"pdfstudio-tmp.pdf");
    return 1;
}

static int save_pdf_to(fz_context *ctx, pdf_document *doc, const char *path8)
{
    int ok = 1;
    fz_try(ctx) {
        pdf_write_options opts = pdf_default_write_options;
        opts.do_compress = 1;
        opts.do_garbage = 1;
        pdf_save_document(ctx, doc, path8, &opts);
    }
    fz_catch(ctx) {
        ok = 0;
    }
    return ok;
}

static void fill_rect_pdf(fz_context *ctx, fz_device *dev, fz_rect r, const float *col)
{
    fz_path *path = fz_new_path(ctx);
    fz_try(ctx) {
        fz_rectto(ctx, path, r.x0, r.y0, r.x1, r.y1);
        fz_fill_path(ctx, dev, path, 0, fz_identity, fz_device_rgb(ctx), col, 1.0f,
                     fz_default_color_params);
    }
    fz_always(ctx) fz_drop_path(ctx, path);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void rgb_of(unsigned c, float *col)
{
    if (c == 0) { col[0] = col[1] = col[2] = 0; return; }
    col[0] = ((c >> 16) & 0xFF) / 255.0f;
    col[1] = ((c >> 8) & 0xFF) / 255.0f;
    col[2] = (c & 0xFF) / 255.0f;
}

int flow_export(PdfDoc *d, const wchar_t *wpath)
{
    fz_context *ctx;
    pdf_document *out = NULL;
    fz_font *font;
    int page, ok = 0;

    if (!F.ready || !d || !d->ctx) return 0;
    ctx = d->ctx;
    font = pdfdoc_ui_font(d);
    if (!font) return 0;
    layout();

    fz_try(ctx) {
        out = pdf_create_document(ctx);
        for (page = 0; page < F.npages; page++) {
            fz_rect mediabox = fz_make_rect(0, 0, F.pw, F.ph);
            pdf_obj *resources = NULL;
            fz_buffer *contents = NULL;
            fz_device *dev;
            pdf_obj *pageobj;
            int i;

            dev = pdf_page_write(ctx, out, mediabox, &resources, &contents);

            for (i = 0; i < F.nl; i++) {
                FlLine *L = &F.ln[i];
                FlPara *p = &F.p[L->para];
                int k;
                float x;

                if (L->page != page || p->isImage) continue;

                /* маркер списка */
                if (p->list && L->first) {
                    fz_text *mt;
                    wchar_t wbuf[24];
                    int wi;
                    float mx = L->x;
                    mt = fz_new_text(ctx);
                    if (p->list == 1) lstrcpyW(wbuf, L"\x2022");
                    else wsprintfW(wbuf, L"%d.", p->listNum > 0 ? p->listNum : 1);
                    {
                        float msize = p->n > 0 ? st_at(p, 0).size : 11.0f;
                        float totw = 0;
                        for (wi = 0; wbuf[wi]; wi++)
                            totw += cpw(d, (int)wbuf[wi], msize);
                        {
                            float gx = mx - 6 - totw;
                            for (wi = 0; wbuf[wi]; wi++) {
                                int gid = fz_encode_character(ctx, font, (int)wbuf[wi]);
                                fz_matrix trm;
                                if (!gid) gid = fz_encode_character(ctx, font, '?');
                                trm = fz_make_matrix(msize, 0, 0, -msize, gx, L->y);
                                fz_show_glyph(ctx, mt, font, trm, gid, (int)wbuf[wi], 0, 0,
                                              FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                                gx += cpw(d, (int)wbuf[wi], msize);
                            }
                        }
                    }
                    {
                        float col[3];
                        rgb_of(0, col);
                        fz_fill_text(ctx, dev, mt, fz_identity, fz_device_rgb(ctx),
                                     col, 1.0f, fz_default_color_params);
                    }
                    fz_drop_text(ctx, mt);
                }

                /* стилевые отрезки строки */
                x = L->x;
                k = L->start;
                while (k < L->end) {
                    FlSt s = st_at(p, k);
                    int runStart = k, j;
                    float sz = s.size > 1.0f ? s.size : 11.0f;
                    float runX0 = x;
                    fz_text *text;
                    float col[3];
                    fz_font *rfont = fam_font(d, s.font, s.attr);

                    while (k < L->end) {
                        FlSt s2 = st_at(p, k);
                        if (s2.attr != s.attr || s2.font != s.font ||
                            fabsf(s2.size - s.size) > 0.05f || s2.color != s.color)
                            break;
                        k++;
                    }
                    if (!rfont) rfont = font;
                    text = fz_new_text(ctx);
                    for (j = runStart; j < k; j++) {
                        int cp = p->cp[j];
                        float w = cpw_style(d, s, cp);
                        if (cp != ' ' && cp >= 32) {
                            int gid = fz_encode_character(ctx, rfont, cp);
                            fz_matrix trm;
                            if (!gid) gid = fz_encode_character(ctx, rfont, '?');
                            trm = fz_make_matrix(sz, 0, 0, -sz, x, L->y);
                            fz_show_glyph(ctx, text, rfont, trm, gid, cp, 0, 0,
                                          FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                        }
                        x += w + (cp == ' ' ? L->spExtra : 0);
                    }
                    rgb_of(s.color, col);
                    fz_fill_text(ctx, dev, text, fz_identity, fz_device_rgb(ctx), col,
                                 1.0f, fz_default_color_params);
                    if (s.attr & FAT_UNDL) {
                        fz_rect ur;
                        float ulw = sz * 0.055f;
                        float uy = L->y + sz * 0.12f;
                        ur = fz_make_rect(runX0, uy, x, uy + ulw);
                        fill_rect_pdf(ctx, dev, ur, col);
                    }
                    fz_drop_text(ctx, text);
                }
            }

            /* рамки таблиц */
            for (i = 0; i < F.np; i++) {
                FlPara *p = &F.p[i];
                fz_path *tp;
                fz_stroke_state ts = fz_default_stroke_state;
                float gcol[3];
                if (!p->tbl || p->ipage != page || p->cellX1 <= p->cellX0) continue;
                ts.linewidth = 0.6f;
                tp = fz_new_path(ctx);
                fz_moveto(ctx, tp, p->cellX0, p->cellY0);
                fz_lineto(ctx, tp, p->cellX1, p->cellY0);
                fz_lineto(ctx, tp, p->cellX1, p->cellY1);
                fz_lineto(ctx, tp, p->cellX0, p->cellY1);
                fz_closepath(ctx, tp);
                gcol[0] = 0.45f; gcol[1] = 0.45f; gcol[2] = 0.45f;
                fz_stroke_path(ctx, dev, tp, &ts, fz_identity, fz_device_rgb(ctx),
                               gcol, 1.0f, fz_default_color_params);
                fz_drop_path(ctx, tp);
            }

            for (i = 0; i < F.np; i++) {
                FlPara *p = &F.p[i];
                if (!p->isImage || !p->img || p->ipage != page) continue;
                {
                    /* единичный квадрат картинки (y вниз) -> прямоугольник на странице */
                    fz_matrix ctm = fz_make_matrix(p->bw, 0, 0, p->bh, p->bx, p->by);
                    fz_fill_image(ctx, dev, p->img, ctm, 1.0f, fz_default_color_params);
                }
            }

            /* колонтитулы и номер страницы */
            if (F.hdr[0] || F.ftr[0] || F.pgnum) {
                float hsize = 9.0f;
                float pageW = F.pw - F.margin_l - F.margin_r;
                int cps[300], n2, i2;

                if (F.hdr[0]) {
                    int hcp[300];
                    int hn = utf8_cps(F.hdr, hcp, 300);
                    float w = cps_width(hcp, hn, hsize);
                    float hx = F.margin_l + (pageW - w) * 0.5f;
                    fz_text *t = fz_new_text(ctx);
                    for (i2 = 0; i2 < hn; i2++) {
                        int gid = fz_encode_character(ctx, font, hcp[i2]);
                        fz_matrix trm;
                        if (!gid) gid = fz_encode_character(ctx, font, '?');
                        trm = fz_make_matrix(hsize, 0, 0, -hsize, hx,
                                             F.margin_t * 0.55f);
                        fz_show_glyph(ctx, t, font, trm, gid, hcp[i2], 0, 0,
                                      FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                        hx += cpw(d, hcp[i2], hsize);
                    }
                    {
                        float col[3];
                        rgb_of(0x464646, col);
                        fz_fill_text(ctx, dev, t, fz_identity, fz_device_rgb(ctx), col,
                                     1.0f, fz_default_color_params);
                    }
                    fz_drop_text(ctx, t);
                }
                if (F.ftr[0] || F.pgnum) {
                    int fcp[300];
                    wchar_t num[16];
                    int fn2 = F.ftr[0] ? utf8_cps(F.ftr, fcp, 300) : 0;
                    int nl = 0;
                    float total = cps_width(fcp, fn2, hsize);
                    float fx;
                    fz_text *t;

                    num[0] = 0;
                    if (F.pgnum) { wsprintfW(num, L"%d", page + 1); nl = (int)lstrlenW(num); }
                    if (nl) {
                        int k2;
                        for (k2 = 0; k2 < nl; k2++) total += cpw(d, (int)num[k2], hsize);
                        if (fn2 > 0) total += hsize * 0.5f;
                    }
                    fx = F.margin_l + (pageW - total) * 0.5f;
                    t = fz_new_text(ctx);
                    for (i2 = 0; i2 < fn2; i2++) {
                        int gid = fz_encode_character(ctx, font, fcp[i2]);
                        fz_matrix trm;
                        if (!gid) gid = fz_encode_character(ctx, font, '?');
                        trm = fz_make_matrix(hsize, 0, 0, -hsize, fx,
                                             F.ph - F.margin_b * 0.45f);
                        fz_show_glyph(ctx, t, font, trm, gid, fcp[i2], 0, 0,
                                      FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                        fx += cpw(d, fcp[i2], hsize);
                    }
                    if (nl) {
                        int k2;
                        if (fn2 > 0) fx += hsize * 0.5f;
                        for (k2 = 0; k2 < nl; k2++) {
                            int gid = fz_encode_character(ctx, font, (int)num[k2]);
                            fz_matrix trm;
                            if (!gid) gid = fz_encode_character(ctx, font, '?');
                            trm = fz_make_matrix(hsize, 0, 0, -hsize, fx,
                                                 F.ph - F.margin_b * 0.45f);
                            fz_show_glyph(ctx, t, font, trm, gid, (int)num[k2], 0, 0,
                                          FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                            fx += cpw(d, (int)num[k2], hsize);
                        }
                    }
                    {
                        float col[3];
                        rgb_of(0x464646, col);
                        fz_fill_text(ctx, dev, t, fz_identity, fz_device_rgb(ctx), col,
                                     1.0f, fz_default_color_params);
                    }
                    fz_drop_text(ctx, t);
                }
            }

            fz_close_device(ctx, dev);
            fz_drop_device(ctx, dev);
            pageobj = pdf_add_page(ctx, out, mediabox, 0, resources, contents);
            pdf_insert_page(ctx, out, page, pageobj);
            pdf_drop_obj(ctx, pageobj);
        }
        ok = 1;
    }
    fz_catch(ctx) {
        flog("flow_export: исключение: %s", fz_caught_message(ctx));
        ok = 0;
    }

    if (ok && out) {
        wchar_t tmp[MAX_PATH];
        int useTmp = !pdfdoc_path_ascii(wpath) && temp_near(wpath, tmp, MAX_PATH);
        char *p8;

        if (useTmp) {
            /* MuPDF не открывает путь с кириллицей: пишем рядом под ASCII-именем
               и переносим на место средствами Win32 (тот же диск — без копий) */
            DeleteFileW(tmp);
            p8 = utf8_of(tmp);
            ok = (p8 && save_pdf_to(ctx, out, p8)) ? pdfdoc_move_file(tmp, wpath) : 0;
            if (!ok) DeleteFileW(tmp);
            free(p8);
        } else {
            p8 = utf8_of(wpath);
            ok = p8 ? save_pdf_to(ctx, out, p8) : 0;
            free(p8);
        }
    }

    if (out) fz_drop_document(ctx, (fz_document *)out);
    if (ok) F.dirty = 0;
    return ok;
}
