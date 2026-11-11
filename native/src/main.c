/* PDF Студия — нативное приложение Win32 (C) на MuPDF.
 *
 * Читает и правит PDF без браузера и без сторонних runtime:
 * один .exe, статически слинкованный, работает на Windows XP…11.
 * Оформление — в стиле Microsoft Word 2016 (лента, шапка, строка состояния).
 *
 * Горячие клавиши: Ctrl+O открыть, Ctrl+S сохранить, Ctrl+P печать,
 * Ctrl+F поиск, PgUp/PgDn страницы, +/- масштаб, 0 по ширине,
 * V выбор, T текст, H маркер, R рамка, N заметка, I перо, E ластик, F2 правка.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "pdfdoc.h"
#include "textedit.h"
#include "flow.h"

#define BUILD_DATE __DATE__ " " __TIME__
#include "wui.h"
#include "cmds.h"
#include "nav.h"

#ifndef FZ_VERSION
#define FZ_VERSION "unknown"
#endif

#define COL_DESK     RGB(118, 118, 118)   /* «стол» вокруг страницы */
#define WM_APP_REDRAW  (WM_APP + 1)
#define IDR_TESS_RUS   200
#define IDR_TESS_ENG   201

/* ---------- глобальное состояние ---------- */
static HINSTANCE   g_inst;
static HWND        g_main, g_view, g_search, g_pageNo;
static int         g_tools = 0;
static PdfDoc      g_doc;
static int         g_open;
static wchar_t     g_path[MAX_PATH];
static int         g_page, g_pages;
static int         g_docmode;                /* 1 — режим документа (текст как в Word) */
static int         g_skipChar;               /* погасить WM_CHAR после Ctrl-сочетания */
static float       g_zoom = 1.0f;
static int         g_tool = TOOL_SELECT;
static int         g_hasBmp;
static HBITMAP     g_bmp;
static int         g_bmpW, g_bmpH;
static float       g_bmpScale;
static int         g_scrollX, g_scrollY;
static int         g_drag, g_dragTool;
static POINT       g_dragFrom, g_dragTo;      /* в координатах клиента view */
static fz_point    g_ink[4096];
static int         g_inkCount;
static fz_rect     g_hits[256];
static int         g_hitCount, g_hitIndex;
static int         g_editDrag;                /* тянем выделение в редакторе текста */
static HACCEL      g_accel;
static HACCEL      g_accelDoc;
static HFONT       g_font;
static wchar_t     g_statusText[512];
static int         g_tracking;

/* --- режим показа страниц --- */
#define PAGE_GAP   12
#define PGCACHE    8
#define TOPMARGIN  12

static int         g_cont;                    /* 1 — на экране несколько страниц */
static int         g_cols = 1;                /* колонок: 0 = авто по ширине окна */
static int         g_colsN = 1;               /* фактически колонок */
static int         g_cellW, g_cellH;          /* ячейка сетки (px) */
static int         g_docW, g_docH;            /* размер всех страниц (px) */

static void doc_layout(void);
static void page_rect_v(int page, RECT *r);
static void page_origin_ex(int page, int *ox, int *oy);
static void cache_clear(void);
static void scroll_to_hit(void);
static HBITMAP page_bitmap(int page, float scale, int *pw, int *ph);
static void cache_drop_page(int page);
static void update_nav(void);
static int  render_page(void);
static void layout(HWND h);
static int         g_tipFor = -1, g_tipX, g_tipY;
static wchar_t     g_tipShown[256];

#ifndef SM_CXPADDEDBORDER
#define SM_CXPADDEDBORDER 92
#endif

#define ZOOM_MIN 0.10f
#define ZOOM_MAX 6.00f

/* ---------- мелкое ---------- */
static void dbg(const char *fmt, ...) __attribute__((unused));
static void dbg(const char *fmt, ...)
{
    FILE *f = fopen("C:\\pdflog.txt", "a");
    if (!f) return;
    {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    }
    fputc('\n', f);
    fclose(f);
}


static void ui_sync(void);

static void status(const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    lstrcpynW(g_statusText, buf, 512);
    wui_set_status(g_statusText);
    if (g_main) InvalidateRect(g_main, NULL, FALSE);
}

static const wchar_t *base_name(const wchar_t *p)
{
    const wchar_t *s = p, *last = p;
    if (!p) return L"";
    for (; *s; s++)
        if (*s == L'\\' || *s == L'/') last = s + 1;
    return last;
}

static void set_title(void)
{
    wchar_t buf[MAX_PATH + 64];
    const wchar_t *name = g_open ? base_name(g_path) : L"файл не открыт";
    const wchar_t *star = (g_open && g_doc.dirty) ? L" *" : L"";
    wsprintfW(buf, L"%s%s — PDF Студия", name, star);
    SetWindowTextW(g_main, buf);
    wui_set_title(buf);
}

/* формат в каретке -> панель форматирования */
static void fmt_sync(void)
{
    FlowFmt f;
    if (!g_docmode || !flow_ready()) {
        wui_set_format(0, 0, 0, 11.0f, 0, 0, 0, 1.0f);
        return;
    }
    flow_fmt_get(&f);
    wui_set_format(f.bold, f.italic, f.underline, f.size, f.color, f.align, f.list,
                   f.spacing);
}

/* обновить состояние ленты/статуса из переменных приложения */
static void ui_sync(void)
{
    fmt_sync();
    if (g_docmode && flow_ready()) {
        FlowFmt ff;
        flow_fmt_get(&ff);
        wui_set_font(flow_font_name(ff.font));
    } else wui_set_font(L"Times");
    wui_set_open(g_open);
    wui_set_dirty(g_open ? g_doc.dirty : 0);
    wui_set_docmode(g_docmode);
    wui_set_page(g_open ? g_page : 0, g_open ? g_pages : 0);
    wui_set_zoom(g_zoom);
    wui_set_tool(g_tool);
    wui_set_tools(g_tools);
    wui_set_pagenum(g_docmode ? flow_get_pagenum() : 0);
    if (g_pageNo && GetFocus() != g_pageNo) {
        wchar_t b[32];
        if (g_open) wsprintfW(b, L"%d", g_page + 1);
        else lstrcpyW(b, L"");
        SetWindowTextW(g_pageNo, b);
    }
    if (g_main) InvalidateRect(g_main, NULL, FALSE);
}

/* UTF-8 <-> UTF-16 */
static char *u8_from_w(const wchar_t *w)
{
    int n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char *)malloc(n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static void w_from_u8(const char *u, wchar_t *out, int outCount)
{
    if (!out || outCount <= 0) return;
    out[0] = 0;
    if (!u) return;
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, outCount);
}

/* ---------- буфер обмена ---------- */
static int clipboard_put_utf8(const char *t)
{
    HGLOBAL h;
    wchar_t *w;
    int n, ok;
    if (!t || !t[0]) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, t, -1, NULL, 0);
    if (n <= 0) return 0;
    h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!h) return 0;
    w = (wchar_t *)GlobalLock(h);
    if (!w) { GlobalFree(h); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, t, -1, w, n);
    GlobalUnlock(h);
    if (!OpenClipboard(g_main)) { GlobalFree(h); return 0; }
    EmptyClipboard();
    ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
    CloseClipboard();
    if (!ok) GlobalFree(h);
    return ok;
}

/* ---------- диалог ввода строки ---------- */
typedef struct {
    wchar_t *out;
    int      outn;
    int      done;
    HWND     edit;
} PromptCtx;

static LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    PromptCtx *c = (PromptCtx *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetWindowTextW(c->edit, c->out, c->outn);
            c->done = 1;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int prompt_text(HWND owner, const wchar_t *caption, const wchar_t *label,
                       wchar_t *out, int outn, int password, int multiline)
{
    static int registered;
    PromptCtx ctx;
    HWND h, ed;
    MSG msg;
    RECT ro;
    int x, y, wdt = 460, hgt = multiline ? 220 : 150;

    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"PdfStudioPrompt";
        RegisterClassW(&wc);
        registered = 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.outn = outn;
    out[0] = 0;

    GetWindowRect(owner ? owner : g_main, &ro);
    x = ro.left + ((ro.right - ro.left) - wdt) / 2;
    y = ro.top + ((ro.bottom - ro.top) - hgt) / 2;

    h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PdfStudioPrompt", caption,
                        WS_POPUP | WS_CAPTION | WS_SYSMENU,
                        x, y, wdt, hgt, owner, NULL, g_inst, NULL);
    if (!h) return 0;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&ctx);

    CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE,
                    12, 10, wdt - 30, 20, h, NULL, g_inst, NULL);

    ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                         (password ? ES_PASSWORD : 0) |
                         (multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL) : 0),
                         12, 34, wdt - 38, multiline ? 120 : 24, h, (HMENU)100, g_inst, NULL);
    ctx.edit = ed;
    SendMessageW(ed, WM_SETFONT, (WPARAM)g_font, TRUE);

    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    wdt - 210, multiline ? 166 : 76, 90, 26, h, (HMENU)IDOK, g_inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    wdt - 112, multiline ? 166 : 76, 90, 26, h, (HMENU)IDCANCEL, g_inst, NULL);

    ShowWindow(h, SW_SHOW);
    SetFocus(ed);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == ed) {
            if (msg.wParam == VK_RETURN &&
                (!multiline || (GetKeyState(VK_CONTROL) & 0x8000))) {
                SendMessageW(h, WM_COMMAND, IDOK, 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(h, WM_COMMAND, IDCANCEL, 0);
                continue;
            }
        }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return ctx.done;
}

/* ---------- просмотр страницы ---------- */
static void free_page_bitmap(void)
{
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = NULL; }
    g_hasBmp = 0;
    g_bmpW = g_bmpH = 0;
}

static int render_page(void)
{
    free_page_bitmap();
    if (!g_open) return 0;
    if (g_docmode && flow_ready()) {          /* режим документа: своя вёрстка */
        float sc = g_zoom * (96.0f / 72.0f);
        HBITMAP b2 = NULL;
        int w2 = 0, h2 = 0;
        g_pages = flow_pages();
        if (g_page >= g_pages) g_page = g_pages - 1;
        if (g_page < 0) g_page = 0;
        if (!flow_render(&g_doc, g_page, sc, &b2, &w2, &h2)) return 0;
        g_bmp = b2; g_bmpW = w2; g_bmpH = h2; g_bmpScale = sc; g_hasBmp = 1;
        cache_drop_page(g_page);
        ui_sync();
        return 1;
    }
    {
        float scale = g_zoom * (96.0f / 72.0f);
        HBITMAP bmp = NULL;
        int w = 0, h = 0;
        if (!pdfdoc_render(&g_doc, g_page, scale, &bmp, &w, &h)) return 0;
        g_bmp = bmp; g_bmpW = w; g_bmpH = h; g_bmpScale = scale; g_hasBmp = 1;
        cache_drop_page(g_page);      /* кэш не должен держать устаревшую копию */
    }
    ui_sync();
    return 1;
}

static void fit_width(void)
{
    RECT rc;
    float w = 0, h = 0;
    if (!g_open) return;
    GetClientRect(g_view, &rc);
    g_colsN = (g_cont && g_cols > 0) ? g_cols : 1;
    pdfdoc_page_size(&g_doc, g_page, &w, &h);
    if (w > 1 && rc.right > 20) {
        float avail = (float)(rc.right - 24 - (g_colsN - 1) * PAGE_GAP) / (float)g_colsN;
        float target = avail / w;
        g_zoom = target * (72.0f / 96.0f);
        if (g_zoom < ZOOM_MIN) g_zoom = ZOOM_MIN;
        if (g_zoom > ZOOM_MAX) g_zoom = ZOOM_MAX;
    }
}

static void update_scrollbars(void)
{
    RECT rc;
    int cw, ch, dw, dh;
    GetClientRect(g_view, &rc);
    cw = rc.right; ch = rc.bottom;
    doc_layout();
    dw = g_cont ? g_docW : g_bmpW;
    dh = g_cont ? (g_docH + 2 * TOPMARGIN > g_bmpH ? g_docH + 2 * TOPMARGIN : g_bmpH) : g_bmpH;
    if (g_hasBmp && dw > cw) {
        SetScrollRange(g_view, SB_HORZ, 0, dw - cw, FALSE);
        if (g_scrollX > dw - cw) g_scrollX = dw - cw;
    } else { g_scrollX = 0; SetScrollRange(g_view, SB_HORZ, 0, 0, TRUE); }
    if (g_hasBmp && dh > ch) {
        SetScrollRange(g_view, SB_VERT, 0, dh - ch, FALSE);
        if (g_scrollY > dh - ch) g_scrollY = dh - ch;
    } else { g_scrollY = 0; SetScrollRange(g_view, SB_VERT, 0, 0, TRUE); }
    if (g_scrollX < 0) g_scrollX = 0;
    if (g_scrollY < 0) g_scrollY = 0;
}

/* ---------- просмотр: одна страница / непрерывно / две / несколько ---------- */

typedef struct { int page, w, h; float scale; DWORD used; HBITMAP bmp; } PgCache;
static PgCache g_pc[PGCACHE];
static DWORD   g_pcTick;

static float view_scale(void) { return g_zoom * (96.0f / 72.0f); }

/* кэш отрисованных страниц (чтобы прокрутка не пересчитывала MuPDF) */
static HBITMAP page_bitmap(int page, float scale, int *pw, int *ph)
{
    int i, slot = -1, empty = -1, lru = 0;
    for (i = 0; i < PGCACHE; i++)          /* другой масштаб — сразу освобождаем */
        if (g_pc[i].bmp && g_pc[i].scale != scale) {
            DeleteObject(g_pc[i].bmp);
            memset(&g_pc[i], 0, sizeof(g_pc[i]));
        }
    for (i = 0; i < PGCACHE; i++) {
        if (g_pc[i].bmp && g_pc[i].page == page && g_pc[i].scale == scale) {
            g_pc[i].used = ++g_pcTick;
            *pw = g_pc[i].w; *ph = g_pc[i].h;
            return g_pc[i].bmp;
        }
        if (!g_pc[i].bmp) { if (empty < 0) empty = i; }
        else if (g_pc[i].used < g_pc[lru].used) lru = i;
    }
    slot = (empty >= 0) ? empty : lru;
    if (g_pc[slot].bmp) { DeleteObject(g_pc[slot].bmp); g_pc[slot].bmp = NULL; }
    g_pc[slot].page = -1;
    {
        HBITMAP b = NULL;
        int w = 0, h = 0;
        if (!pdfdoc_render(&g_doc, page, scale, &b, &w, &h)) return NULL;
        g_pc[slot].page = page; g_pc[slot].bmp = b; g_pc[slot].w = w; g_pc[slot].h = h;
        g_pc[slot].scale = scale; g_pc[slot].used = ++g_pcTick;
        *pw = w; *ph = h;
        return b;
    }
}

static void cache_drop_page(int page)
{
    int i;
    for (i = 0; i < PGCACHE; i++)
        if (g_pc[i].bmp && g_pc[i].page == page) {
            DeleteObject(g_pc[i].bmp);
            memset(&g_pc[i], 0, sizeof(g_pc[i]));
        }
}

static void cache_clear(void)
{
    int i;
    for (i = 0; i < PGCACHE; i++) {
        if (g_pc[i].bmp) DeleteObject(g_pc[i].bmp);
        memset(&g_pc[i], 0, sizeof(g_pc[i]));
    }
}

/* геометрия сетки страниц */
static void doc_layout(void)
{
    int n, cols, rows, maxw = 0, maxh = 0;
    float scale = view_scale();

    if (!g_cont || !g_open) {
        g_docW = g_bmpW; g_docH = g_bmpH;
        g_cellW = g_bmpW; g_cellH = g_bmpH; g_colsN = 1;
        return;
    }
    n = g_pages;
    {
        float w = 0, h = 0;
        pdfdoc_page_size(&g_doc, 0, &w, &h);
        maxw = (int)(w * scale + 0.5f);
        maxh = (int)(h * scale + 0.5f);
    }
    if (maxw < 1) maxw = g_bmpW ? g_bmpW : 600;
    if (maxh < 1) maxh = g_bmpH ? g_bmpH : 800;

    cols = g_cols;
    if (cols <= 0) {                      /* авто: сколько влезает по ширине */
        RECT rc;
        GetClientRect(g_view, &rc);
        cols = (rc.right - 24 + PAGE_GAP) / (maxw + PAGE_GAP);
        if (cols < 1) cols = 1;
        if (cols > 6) cols = 6;
    }
    rows = (n + cols - 1) / cols;
    if (rows < 1) rows = 1;
    g_colsN = cols;
    g_cellW = maxw + PAGE_GAP;
    g_cellH = maxh + PAGE_GAP;
    g_docW = cols * g_cellW - PAGE_GAP;
    g_docH = rows * g_cellH - PAGE_GAP;
}

/* прямоугольник страницы в виртуальных координатах */
static void page_rect_v(int page, RECT *r)
{
    int pw, ph, cx, cy;
    float w = 0, h = 0;
    if (!g_cont) {
        r->left = 0; r->top = 0; r->right = g_bmpW; r->bottom = g_bmpH;
        return;
    }
    pdfdoc_page_size(&g_doc, page, &w, &h);
    pw = (int)(w * view_scale() + 0.5f);
    ph = (int)(h * view_scale() + 0.5f);
    cx = page % (g_colsN > 0 ? g_colsN : 1);
    cy = page / (g_colsN > 0 ? g_colsN : 1);
    r->left = cx * g_cellW + (g_cellW - PAGE_GAP - pw) / 2;
    r->top = cy * g_cellH + (g_cellH - PAGE_GAP - ph) / 2;
    r->right = r->left + pw;
    r->bottom = r->top + ph;
}

/* смещение страницы на экране (с учётом центрирования и прокрутки) */
static void page_origin_ex(int page, int *ox, int *oy)
{
    RECT rc, pr;
    GetClientRect(g_view, &rc);
    if (!g_cont) {
        *ox = g_hasBmp ? (rc.right - g_bmpW) / 2 : 0;
        *oy = TOPMARGIN;
        if (g_hasBmp && g_bmpW > rc.right) *ox = 0;
        *ox -= g_scrollX;
        *oy -= g_scrollY;
        return;
    }
    page_rect_v(page, &pr);
    *ox = pr.left - g_scrollX;
    *oy = pr.top - g_scrollY;
    if (g_docW < rc.right) *ox += (rc.right - g_docW) / 2;
    if (g_docH < rc.bottom) *oy += (rc.bottom - g_docH) / 2;
}

static void page_origin(int *ox, int *oy) { page_origin_ex(g_page, ox, oy); }

/* страница под точкой клиента (в режиме нескольких страниц) */
static int page_at(int x, int y)
{
    int vx, vy, col, row, p;
    RECT rc;
    if (!g_cont) return g_page;
    GetClientRect(g_view, &rc);
    doc_layout();
    vx = x + g_scrollX;
    vy = y + g_scrollY;
    if (g_docW < rc.right) vx -= (rc.right - g_docW) / 2;
    if (g_docH < rc.bottom) vy -= (rc.bottom - g_docH) / 2;
    if (vx < 0 || vy < 0) return -1;
    col = vx / (g_cellW > 0 ? g_cellW : 1);
    row = vy / (g_cellH > 0 ? g_cellH : 1);
    if (col >= g_colsN) col = g_colsN - 1;
    p = row * g_colsN + col;
    if (p < 0 || p >= g_pages) return -1;
    return p;
}

static fz_point client_to_page(int x, int y)
{
    fz_point p;
    int ox, oy, pg;
    if (g_cont) {
        pg = page_at(x, y);
        if (pg >= 0 && pg != g_page) {
            if (te_is_active()) { if (te_commit()) g_doc.dirty = 1; }
            g_page = pg;
            g_hitCount = 0;
            render_page();
            update_nav();
        }
    }
    page_origin(&ox, &oy);
    p.x = (float)(x - ox) / (g_bmpScale > 0 ? g_bmpScale : 1.0f);
    p.y = (float)(y - oy) / (g_bmpScale > 0 ? g_bmpScale : 1.0f);
    return p;
}

/* ---------- модели OCR: распаковка встроенных ресурсов ---------- */
static int extract_resource(int id, const char *target)
{
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(id), (LPCWSTR)(UINT_PTR)RT_RCDATA);
    HGLOBAL gl;
    const void *data;
    DWORD size;
    FILE *f;

    if (!res) return 0;
    size = SizeofResource(g_inst, res);
    gl = LoadResource(g_inst, res);
    if (!gl) return 0;
    data = LockResource(gl);
    if (!data) return 0;
    f = fopen(target, "wb");
    if (!f) return 0;
    fwrite(data, 1, size, f);
    fclose(f);
    return 1;
}

static const char *tessdata_dir(void)
{
    static char dir[1024];
    static int done;
    wchar_t tmp[MAX_PATH], wdir[MAX_PATH];
    char rus[1200], eng[1200];

    if (done) return dir[0] ? dir : NULL;
    done = 1;

    if (!GetTempPathW(MAX_PATH, tmp)) return NULL;
    wsprintfW(wdir, L"%sPDFStudioOCR", tmp);
    CreateDirectoryW(wdir, NULL);
    WideCharToMultiByte(CP_UTF8, 0, wdir, -1, dir, sizeof(dir), NULL, NULL);

    wsprintfA(rus, "%s\\rus.traineddata", dir);
    wsprintfA(eng, "%s\\eng.traineddata", dir);
    {
        FILE *f = fopen(rus, "rb");
        if (f) {
            long sz;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
            if (sz < 100000) extract_resource(IDR_TESS_RUS, rus);
        } else extract_resource(IDR_TESS_RUS, rus);
    }
    {
        FILE *f = fopen(eng, "rb");
        if (f) {
            long sz;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
            if (sz < 100000) extract_resource(IDR_TESS_ENG, eng);
        } else extract_resource(IDR_TESS_ENG, eng);
    }
    return dir;
}

/* ---------- открытие / сохранение ---------- */
static void do_open_file(const wchar_t *wpath);
static void do_save(int saveAs);
static int  do_save_doc(void);
static void do_toggle_docmode(void);
static void doc_after_edit(void);
static void select_tool(int t);

static void update_nav(void)
{
    ui_sync();
}

/* переключение режима показа страниц */
static void set_view(int mode)
{
    switch (mode) {
    case 0: g_cont = 0; g_cols = 1; break;   /* одна страница            */
    case 1: g_cont = 1; g_cols = 1; break;   /* непрерывно, одна колонка */
    case 2: g_cont = 1; g_cols = 2; break;   /* две страницы             */
    default:                                 /* несколько страниц         */
        g_cont = 1;
        {
            RECT rc;
            GetClientRect(g_view, &rc);
            g_cols = (rc.right >= 1000) ? 3 : 2;
        }
        break;
    }
    wui_set_view_mode(mode);
    g_scrollX = g_scrollY = 0;
    cache_clear();
    if (g_main) layout(g_main);
    if (g_open) {
        if (g_cont) fit_width();
        render_page();                 /* битмап текущей страницы — под новый масштаб */
    }
    update_scrollbars();
    if (g_view) InvalidateRect(g_view, NULL, FALSE);
    status(mode == 0 ? L"Вид: одна страница" :
           mode == 1 ? L"Вид: непрерывно — все страницы подряд" :
           mode == 2 ? L"Вид: две страницы в ряд" : L"Вид: несколько страниц (по ширине окна)");
}

static void go_page(int page)
{
    if (!g_open) return;
    if (page < 0) page = 0;
    if (page >= g_pages) page = g_pages - 1;
    if (page == g_page) return;
    if (te_is_active()) { if (te_commit()) g_doc.dirty = 1; }
    g_page = page;
    g_hitCount = 0;
    render_page();
    update_scrollbars();
    g_scrollY = 0; g_scrollX = 0;
    if (g_cont) {                       /* показать нужную страницу в сетке */
        RECT pr, rc;
        int vw, vh;
        doc_layout();
        page_rect_v(g_page, &pr);
        GetClientRect(g_view, &rc);
        vw = g_docW > rc.right ? g_docW : 0;
        vh = g_docH > rc.bottom ? g_docH : 0;
        if (vh) {
            g_scrollY = pr.top - TOPMARGIN;
            if (g_scrollY > vh - rc.bottom) g_scrollY = vh - rc.bottom;
            if (g_scrollY < 0) g_scrollY = 0;
            SetScrollPos(g_view, SB_VERT, g_scrollY, TRUE);
        }
        if (vw) {
            g_scrollX = pr.left - 12;
            if (g_scrollX > vw - rc.right) g_scrollX = vw - rc.right;
            if (g_scrollX < 0) g_scrollX = 0;
            SetScrollPos(g_view, SB_HORZ, g_scrollX, TRUE);
        }
    }
    InvalidateRect(g_view, NULL, FALSE);
    update_nav();
}

static void do_open_file(const wchar_t *wpath)
{
    PdfDoc nd;
    int rc;
    wchar_t pass[128];

    memset(&nd, 0, sizeof(nd));
    te_bg_cache_reset();
    rc = pdfdoc_open(&nd, wpath);
    if (rc == PDF_NEED_PASSWORD) {
        if (!prompt_text(g_main, L"Документ защищён", L"Пароль:", pass, 128, 1, 0)) {
            pdfdoc_close(&nd);
            return;
        }
        {
            char *p8 = u8_from_w(pass);
            rc = pdfdoc_auth(&nd, p8 ? p8 : "");
            free(p8);
        }
        if (rc != PDF_OK) {
            MessageBoxW(g_main, L"Неверный пароль.", L"PDF Студия", MB_ICONWARNING);
            pdfdoc_close(&nd);
            return;
        }
    } else if (rc != PDF_OK) {
        MessageBoxW(g_main, L"Не удалось открыть файл.", L"PDF Студия", MB_ICONERROR);
        pdfdoc_close(&nd);
        return;
    }

    if (g_open) { pdfdoc_close(&g_doc); te_edit_clear(); flow_reset(); g_docmode = 0; }
    g_doc = nd;
    g_open = 1;
    flow_reset();
    g_docmode = 0;
    lstrcpynW(g_path, wpath, MAX_PATH);
    g_pages = pdfdoc_pages(&g_doc);
    g_page = 0;
    g_hitCount = 0;
    g_scrollX = g_scrollY = 0;
    cache_clear();
    te_set_doc(&g_doc);

    fit_width();
    render_page();
    update_scrollbars();
    set_title();
    update_nav();
    select_tool(TOOL_SELECT);
    InvalidateRect(g_view, NULL, FALSE);
    status(L"Открыт: %s — страниц: %d", base_name(wpath), g_pages);
}

static void do_open(void)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH];
    static const wchar_t filter[] =
        L"Документы PDF\0*.pdf\0Все файлы\0*.*\0\0";

    file[0] = 0;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Открыть PDF";
    if (GetOpenFileNameW(&ofn)) do_open_file(file);
}

static int do_save_to(const wchar_t *wpath)
{
    int applied;
    wchar_t tmp[MAX_PATH + 8];

    if (!g_open) return 0;
    if (g_docmode) return do_save_doc();
    if (te_is_active()) { if (te_commit()) g_doc.dirty = 1; }
    applied = te_apply_all(&g_doc);
    te_edit_clear();
    te_bg_cache_reset();
    if (applied > 0) g_doc.dirty = 1;

    wsprintfW(tmp, L"%s.tmp", wpath);
    DeleteFileW(tmp);                 /* остаток от прошлой неудачной записи */
    if (!pdfdoc_save(&g_doc, tmp)) {
        DeleteFileW(tmp);
        status(L"Не удалось записать файл");
        return 0;
    }
    /* освобождаем целевой файл — он открыт движком */
    pdfdoc_close(&g_doc);
    g_open = 0;
    if (!MoveFileExW(tmp, wpath, MOVEFILE_REPLACE_EXISTING)) {
        /* Файл занят (например, открыт другой копией программы) или защищён.
           Не теряем правку: открываем сохранённую копию и говорим об этом. */
        status(L"Файл занят — открыта копия %s", base_name(tmp));
        do_open_file(tmp);
        return 0;
    }
    if (lstrcmpiW(wpath, g_path) != 0) lstrcpynW(g_path, wpath, MAX_PATH);
    status(L"Сохранено: %s", base_name(wpath));
    return 1;
}

static void do_save(int saveAs)
{
    wchar_t file[MAX_PATH];
    OPENFILENAMEW ofn;
    static const wchar_t filter[] = L"Документы PDF\0*.pdf\0\0";

    if (!g_open) { status(L"Файл не открыт"); return; }

    if (g_docmode) {                 /* режим документа: пишем перевёрстанный PDF */
        do_save_doc();
        return;
    }

    if (!saveAs) {
        wchar_t keep[MAX_PATH];
        lstrcpynW(keep, g_path, MAX_PATH);
        if (do_save_to(keep)) {
            do_open_file(keep);                     /* переоткрыть с диска */
            status(L"Сохранено: %s", base_name(keep));
        }
        return;
    }

    lstrcpynW(file, g_path, MAX_PATH);
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Сохранить как";
    ofn.lpstrDefExt = L"pdf";
    if (GetSaveFileNameW(&ofn)) {
        wchar_t keep[MAX_PATH];
        lstrcpynW(keep, file, MAX_PATH);
        if (do_save_to(keep)) do_open_file(keep);
    }
}

/* ---------- печать ---------- */
static void do_print(void)
{
    PRINTDLGW pd;
    DOCINFOW di;
    int page;

    if (!g_open) { status(L"Файл не открыт"); return; }
    memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = g_main;
    pd.Flags = PD_RETURNDC | PD_ALLPAGES | PD_NOSELECTION | PD_USEDEVMODECOPIES;
    pd.nFromPage = 1;
    pd.nToPage = (WORD)g_pages;
    pd.nMinPage = 1;
    pd.nMaxPage = (WORD)g_pages;
    if (!PrintDlgW(&pd) || !pd.hDC) return;

    memset(&di, 0, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = g_path;
    if (StartDocW(pd.hDC, &di) <= 0) { DeleteDC(pd.hDC); return; }
    {
        int dpiX = GetDeviceCaps(pd.hDC, LOGPIXELSX);
        int dpiY = GetDeviceCaps(pd.hDC, LOGPIXELSY);
        for (page = 0; page < g_pages; page++) {
            float pw = 0, ph = 0;
            if (g_docmode) { pw = flow_page_w(); ph = flow_page_h(); }
            else pdfdoc_page_size(&g_doc, page, &pw, &ph);
            if (StartPage(pd.hDC) > 0) {
                float scale = (float)dpiX / 72.0f;
                HBITMAP bmp = NULL;
                int bw = 0, bh = 0;
                int okRender = g_docmode
                    ? flow_render(&g_doc, page, scale, &bmp, &bw, &bh)
                    : pdfdoc_render(&g_doc, page, scale, &bmp, &bw, &bh);
                if (okRender) {
                    HDC mem = CreateCompatibleDC(pd.hDC);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    StretchBlt(pd.hDC, 0, 0,
                               (int)(pw * dpiX / 72.0f), (int)(ph * dpiY / 72.0f),
                               mem, 0, 0, bw, bh, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                    DeleteObject(bmp);
                }
                EndPage(pd.hDC);
            }
        }
        EndDoc(pd.hDC);
    }
    DeleteDC(pd.hDC);
    status(L"Отправлено на печать");
}

/* ---------- поиск ---------- */
static void do_find(int next)
{
    wchar_t what[256];
    char *needle;
    int n;

    if (!g_open) { status(L"Файл не открыт"); return; }
    GetWindowTextW(g_search, what, 256);
    if (!what[0]) { status(L"Введите текст для поиска"); SetFocus(g_search); return; }
    needle = u8_from_w(what);
    if (!needle) return;
    n = pdfdoc_search_page(&g_doc, g_page, needle, g_hits, 256);
    free(needle);
    g_hitCount = n;
    g_hitIndex = 0;
    scroll_to_hit();
    InvalidateRect(g_view, NULL, FALSE);
    if (n > 0) status(L"Найдено на странице %d: %d", g_page + 1, n);
    else status(L"Не найдено: «%s»", what);
}

/* Navigate directly from the page field; invalid input never changes the page. */
static void do_goto_page(void)
{
    wchar_t text[32];
    int page;
    if (!g_open) { status(L"Сначала откройте PDF"); return; }
    GetWindowTextW(g_pageNo, text, 32);
    if (!nav_parse_page(text, g_pages, &page)) {
        status(L"Введите номер страницы от 1 до %d", g_pages);
        SetFocus(g_pageNo);
        SendMessageW(g_pageNo, EM_SETSEL, 0, -1);
        return;
    }
    go_page(page);
}

/* ---------- инструменты ---------- */
static void select_tool(int t)
{
    if (t == g_tool) { ui_sync(); return; }
    if (t != TOOL_EDIT && te_is_active()) {
        if (te_commit()) { g_doc.dirty = 1; set_title(); }
    }
    g_tool = t;
    {
        static const wchar_t *names[TOOL_COUNT] = {
            L"Выбор: выделяйте текст мышью, клик по полю формы — заполнение",
            L"Текст: растяните рамку — введите текст",
            L"Маркер: выделите фрагмент",
            L"Рамка: растяните прямоугольник",
            L"Заметка: клик — введите примечание",
            L"Перо: рисуйте от руки",
            L"Ластик: клик по правке — удалить",
            L"Правка текста: щёлкните по тексту и печатайте"
        };
        status(L"%s", names[t]);
    }
    ui_sync();
    SetFocus(g_view);
    InvalidateRect(g_view, NULL, FALSE);
}

/* ---------- операции со страницами ---------- */
static void do_page_rotate(int delta)
{
    if (!g_open) return;
    if (!pdfdoc_page_rotate(&g_doc, g_page, delta)) return;
    g_doc.dirty = 1;
    fit_width();
    render_page();
    update_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    set_title();
    status(delta > 0 ? L"Страница повёрнута вправо" : L"Страница повёрнута влево");
}

static void do_page_duplicate(void)
{
    if (!g_open) return;
    if (!pdfdoc_page_duplicate(&g_doc, g_page)) return;
    g_pages = pdfdoc_pages(&g_doc);
    g_doc.dirty = 1;
    g_page++;
    render_page();
    update_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    set_title();
    ui_sync();
    status(L"Страница продублирована (страниц: %d)", g_pages);
}

static void do_page_delete(void)
{
    if (!g_open) return;
    if (g_pages <= 1) { status(L"Последнюю страницу удалить нельзя"); return; }
    if (MessageBoxW(g_main, L"Удалить текущую страницу?", L"PDF Студия",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!pdfdoc_page_delete(&g_doc, g_page)) { status(L"Удалить не удалось"); return; }
    g_pages = pdfdoc_pages(&g_doc);
    if (g_page >= g_pages) g_page = g_pages - 1;
    g_doc.dirty = 1;
    render_page();
    update_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    set_title();
    ui_sync();
    status(L"Страница удалена (осталось: %d)", g_pages);
}

static void do_page_move(int delta)
{
    if (!g_open) return;
    if (!pdfdoc_page_move(&g_doc, g_page, delta)) return;
    g_doc.dirty = 1;
    g_page += delta;
    render_page();
    update_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    set_title();
    ui_sync();
    status(delta < 0 ? L"Страница перемещена выше" : L"Страница перемещена ниже");
}

/* ---------- OCR текущей страницы или всего документа ---------- */
static void do_ocr(int all)
{
    const char *dd = tessdata_dir();
    int chars = 0, pagesDone = 0;

    if (!g_open) { status(L"Файл не открыт"); return; }
    if (!dd) {
        MessageBoxW(g_main,
                    L"Не найдены языковые модели OCR.\n"
                    L"В сборке должны быть ресурсы rus.traineddata и eng.traineddata.",
                    L"PDF Студия", MB_ICONERROR);
        return;
    }
    SetCursor(LoadCursor(NULL, IDC_WAIT));
    status(L"Распознавание…");
    if (all) {
        int p;
        for (p = 0; p < g_pages; p++) {
            int c = 0;
            if (pdfdoc_ocr_page(&g_doc, p, "rus+eng", dd, &c)) { chars += c; pagesDone++; }
        }
    } else {
        int c = 0;
        if (pdfdoc_ocr_page(&g_doc, g_page, "rus+eng", dd, &c)) { chars += c; pagesDone = 1; }
    }
    SetCursor(LoadCursor(NULL, IDC_ARROW));

    if (pagesDone) {
        g_doc.dirty = 1;
        render_page();
        InvalidateRect(g_view, NULL, FALSE);
        set_title();
        status(L"Распознано страниц: %d, символов: %d (сохраните файл — Ctrl+S)",
               pagesDone, chars);
    } else {
        status(L"Распознать не удалось");
    }
}

/* ---------- копирование текста страницы ---------- */
static void copy_page_text(void)
{
    char *t;
    int n;
    if (!g_open) return;
    t = pdfdoc_page_text(&g_doc, g_page);
    if (!t || !t[0]) { status(L"На странице нет текста (скан? попробуйте OCR)"); free(t); return; }
    n = (int)strlen(t);
    clipboard_put_utf8(t);
    free(t);
    status(L"Скопировано в буфер обмена: %d символов", n);
}

/* ---------- подсветка результатов поиска ---------- */
static void paint_hits(HDC dc, int ox, int oy)
{
    int i;
    if (g_hitCount <= 0) return;
    for (i = 0; i < g_hitCount && i < 256; i++) {
        RECT r;
        r.left = ox + (int)(g_hits[i].x0 * g_bmpScale);
        r.top = oy + (int)(g_hits[i].y0 * g_bmpScale);
        r.right = ox + (int)(g_hits[i].x1 * g_bmpScale) + 1;
        r.bottom = oy + (int)(g_hits[i].y1 * g_bmpScale) + 1;
        if (i == g_hitIndex) {
            HBRUSH br = CreateSolidBrush(RGB(255, 140, 0));
            HGDIOBJ ob = SelectObject(dc, br);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, ob);
            DeleteObject(br);
        } else {
            HBRUSH br = CreateSolidBrush(RGB(255, 220, 0));
            FrameRect(dc, &r, br);
            DeleteObject(br);
        }
    }
}

/* Прокрутить так, чтобы текущее найденное вхождение было видно на экране. */
static void scroll_to_hit(void)
{
    RECT rc;
    int ox, oy, x0, y0, x1, y1, m = 48;

    if (!g_view || !g_hasBmp || g_hitCount <= 0) return;
    if (g_hitIndex < 0 || g_hitIndex >= g_hitCount) return;
    GetClientRect(g_view, &rc);
    page_origin(&ox, &oy);
    x0 = ox + (int)(g_hits[g_hitIndex].x0 * g_bmpScale);
    y0 = oy + (int)(g_hits[g_hitIndex].y0 * g_bmpScale);
    x1 = ox + (int)(g_hits[g_hitIndex].x1 * g_bmpScale);
    y1 = oy + (int)(g_hits[g_hitIndex].y1 * g_bmpScale);

    if (y0 - m < 0) g_scrollY += y0 - m;
    else if (y1 + m > rc.bottom) g_scrollY += (y1 + m) - rc.bottom;
    if (x0 - m < 0) g_scrollX += x0 - m;
    else if (x1 + m > rc.right) g_scrollX += (x1 + m) - rc.right;

    update_scrollbars();
    SetScrollPos(g_view, SB_VERT, g_scrollY, TRUE);
    SetScrollPos(g_view, SB_HORZ, g_scrollX, TRUE);
}


/* ---------- режим документа (текст как в Word) ---------- */
static int clipboard_get_utf8(char **out);

static void doc_scroll_to_caret(void)
{
    float cx = 0, cy = 0, cw = 0, ch = 0;
    RECT rc;
    int ox, oy, y;
    if (!flow_caret_box(&cx, &cy, &cw, &ch)) return;
    GetClientRect(g_view, &rc);
    page_origin(&ox, &oy);
    y = oy + (int)(cy * (g_bmpScale > 0 ? g_bmpScale : 1.0f));
    if (y < 10) g_scrollY += y - 30;
    else if (y > rc.bottom - 30) g_scrollY += y - (rc.bottom - 30);
    if (g_scrollY < 0) g_scrollY = 0;
    update_scrollbars();
    SetScrollPos(g_view, SB_VERT, g_scrollY, TRUE);
}

/* перерисовать после правки: страница с кареткой, прокрутка, звёздочка */
static void doc_after_edit(void)
{
    int pg;
    if (!g_docmode) return;
    g_pages = flow_pages();
    pg = flow_caret_page();
    if (pg != g_page) { g_page = pg; }
    if (g_page >= g_pages) g_page = g_pages - 1;
    if (g_page < 0) g_page = 0;
    if (flow_is_dirty() && !g_doc.dirty) { g_doc.dirty = 1; set_title(); }
    {
        int np = 0, npar = 0, nch = 0;
        flow_stats(&np, &npar, &nch);
        status(L"Режим документа: %d стр., %d абзацев, %d символов, %d слов (Ctrl+S — сохранить PDF)",
               np, npar, nch, flow_words());
    }
    render_page();
    doc_scroll_to_caret();
    InvalidateRect(g_view, NULL, TRUE);
}

static void do_toggle_docmode(void)
{
    if (!g_open) {
        status(L"Сначала откройте PDF (Ctrl+O)");
        MessageBoxW(g_main,
                    L"Сначала откройте PDF: Ctrl+O или перетащите файл в окно.\n\n"
                    L"Режим документа работает с текстом PDF, поэтому нужен открытый файл.",
                    L"Режим документа", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (te_is_active()) { if (te_commit()) g_doc.dirty = 1; }

    if (!g_docmode) {
        int pg = 0, pr = 0, ch = 0;
        /* Правки, сделанные в режиме страниц, ещё не в документе — иначе в
           режиме документа они бы просто потерялись. Запекаем их заранее. */
        if (te_edit_count() > 0) {
            te_apply_all(&g_doc);
            te_edit_clear();
            te_bg_cache_reset();
            g_doc.dirty = 1;
        }
        if (!flow_build(&g_doc)) {
            status(L"В этом PDF нет текстового слоя — режим документа недоступен (для сканов: «Распознать страницу»)");
            MessageBoxW(g_main,
                        L"В этом PDF нет текстового слоя — это скан или картинка.\n\n"
                        L"Режим документа (и оформление текста) работает только с текстом.\n"
                        L"Для сканов: «Инструменты → Распознать страницу», затем включите\n"
                        L"режим документа снова.",
                        L"Режим документа", MB_OK | MB_ICONWARNING);
            return;
        }
        g_docmode = 1;
        g_cont = 0; g_cols = 1;
        g_page = 0;
        select_tool(TOOL_SELECT);
        flow_stats(&pg, &pr, &ch);
        g_pages = flow_pages();
        fit_width();
        render_page();
        update_nav();
        update_scrollbars();
        status(L"Режим документа: %d стр., %d абзацев, %d символов. Ставьте курсор в текст и печатайте — абзацы перетекают, лишнее уходит на новую страницу", pg, pr, ch);
    } else {
        if (flow_is_dirty()) {
            int r = MessageBoxW(g_main, L"Изменения текста не сохранены.\n\nСохранить перевёрстанный PDF?",
                                L"Режим документа", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return;
            if (r == IDYES) {
                SendMessageW(g_main, WM_COMMAND, IDC_SAVEAS, 0);
                if (flow_is_dirty()) return;         /* сохранить не удалось — не выходим */
            }
        }
        g_docmode = 0;
        flow_reset();
        g_pages = pdfdoc_pages(&g_doc);
        if (g_page >= g_pages) g_page = g_pages - 1;
        if (g_page < 0) g_page = 0;
        render_page();
        update_nav();
        update_scrollbars();
        status(L"Режим просмотра страниц");
    }
    ui_sync();
    set_title();
    layout(g_main);                     /* полоса форматирования меняет высоту шапки */
    RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (g_view) { InvalidateRect(g_view, NULL, TRUE); UpdateWindow(g_view); }
}

/* сохранить результат вёрстки в PDF */
static int do_save_doc(void)
{
    wchar_t file[MAX_PATH];
    OPENFILENAMEW ofn;
    static const wchar_t filter[] = L"Документы PDF\0*.pdf\0\0";
    const wchar_t *base;
    int i, dot = -1;

    if (!g_open) return 0;
    lstrcpynW(file, g_path, MAX_PATH);
    base = base_name(file);
    for (i = (int)lstrlenW(file) - 1; i >= 0 && file[i] != L'\\' && file[i] != L'/'; i--)
        if (file[i] == L'.') { dot = i; break; }
    if (dot > 0) file[dot] = 0;
    lstrcatW(file, L" (документ).pdf");

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Сохранить документ как";
    ofn.lpstrDefExt = L"pdf";
    if (!GetSaveFileNameW(&ofn)) return 0;

    if (flow_export(&g_doc, file)) {
        flow_mark_clean();
        g_doc.dirty = 0;
        set_title();
        ui_sync();
        status(L"Документ сохранён: %s — таблицы и картинки записаны в PDF как графика "
               L"(в режиме страниц файл выглядит так же, как на экране)", base_name(file));
        return 1;
    }
    status(L"Не удалось записать файл");
    return 0;
}

static void fmt_after(void);
static void do_margins(void);
static void do_page_size(int idx);
static void do_orientation(int land);

static void do_format_cmd(int id)
{
    static const unsigned pal[6] = { 0x000000, 0xC00000, 0x0000C0,
                                     0x008000, 0xE07000, 0x808080 };

    if (!g_docmode || !flow_ready()) {
        status(L"Форматирование доступно в режиме документа (Вид → Режим документа)");
        return;
    }
    if (id >= IDC_FMT_COLOR && id <= IDC_FMT_COLOR + 5) {
        flow_fmt_color(pal[id - IDC_FMT_COLOR]);
        fmt_after();
        return;
    }
    switch (id) {
    case IDC_FMT_B:       flow_fmt_attr(1);  break;
    case IDC_FMT_I:       flow_fmt_attr(2);  break;
    case IDC_FMT_U:       flow_fmt_attr(4);  break;
    case IDC_FMT_BIGGER:  flow_fmt_size_step(1);  break;
    case IDC_FMT_SMALLER: flow_fmt_size_step(-1); break;
    case IDC_ALIGN_L:     flow_align(0); break;
    case IDC_ALIGN_C:     flow_align(1); break;
    case IDC_ALIGN_R:     flow_align(2); break;
    case IDC_ALIGN_J:     flow_align(3); break;
    case IDC_LIST_B:      flow_list(flow_fmt_list() == 1 ? 0 : 1); break;
    case IDC_LIST_N:      flow_list(flow_fmt_list() == 2 ? 0 : 2); break;
    case IDC_LIST_NONE:   flow_list(0); break;
    case IDC_SPACING:     flow_spacing_next(); break;
    case IDC_INDENTP:     flow_indent(1);  break;
    case IDC_INDENTM:     flow_indent(-1); break;
    case IDC_SPACEBEFORE: flow_space_before(1); break;
    case IDC_DOCCOPY: {
        char *t = flow_selected_utf8();
        if (t && t[0]) {
            clipboard_put_utf8(t);
            status(L"Скопировано: %d символов", (int)strlen(t));
        } else status(L"Сначала выделите текст");
        free(t);
        return;
    }
    case IDC_DOCCUT: {
        char *t = flow_selected_utf8();
        if (t && t[0] && flow_has_selection()) {
            clipboard_put_utf8(t);
            flow_vkey(VK_DELETE, 0, 0);
            fmt_after();
            status(L"Вырезано: %d символов", (int)strlen(t));
        } else status(L"Сначала выделите текст");
        free(t);
        return;
    }
    case IDC_DOCPASTE: {
        char *t = NULL;
        if (clipboard_get_utf8(&t) && t) {
            flow_paste_text(t);
            free(t);
            fmt_after();
        } else status(L"В буфере обмена пусто");
        return;
    }
    case IDC_PAGE_A4:     do_page_size(0); return;
    case IDC_PAGE_A5:     do_page_size(2); return;
    case IDC_PAGE_LETTER: do_page_size(1); return;
    case IDC_PORTR:       do_orientation(0); return;
    case IDC_LANDR:       do_orientation(1); return;
    case IDC_MARGINS:     do_margins(); return;
    case IDC_HEADER: {
        wchar_t b[256];
        char *u8;
        w_from_u8(flow_get_header(), b, 256);
        if (prompt_text(g_main, L"Верхний колонтитул",
                        L"Текст сверху страницы (пусто — убрать):", b, 256, 0, 0)) {
            u8 = u8_from_w(b);
            flow_set_header(u8);
            free(u8);
            fmt_after();
            status(L"Верхний колонтитул задан");
        }
        return;
    }
    case IDC_FOOTER: {
        wchar_t b[256];
        char *u8;
        w_from_u8(flow_get_footer(), b, 256);
        if (prompt_text(g_main, L"Нижний колонтитул",
                        L"Текст внизу страницы (пусто — убрать):", b, 256, 0, 0)) {
            u8 = u8_from_w(b);
            flow_set_footer(u8);
            free(u8);
            fmt_after();
            status(L"Нижний колонтитул задан");
        }
        return;
    }
    case IDC_PAGENUM:
        flow_set_pagenum(!flow_get_pagenum());
        fmt_after();
        status(flow_get_pagenum() ? L"Нумерация страниц включена"
                                  : L"Нумерация страниц выключена");
        return;
    default:
        return;
    }
    fmt_after();
}

/* поиск по потоку документа */
static void doc_find(int next)
{
    wchar_t what[256];
    char *n8;
    (void)next;

    if (!g_docmode || !flow_ready()) return;
    GetWindowTextW(g_search, what, 256);
    if (!what[0]) { status(L"Введите текст для поиска"); SetFocus(g_search); return; }
    n8 = u8_from_w(what);
    if (!n8) return;
    if (flow_find_next(n8, 0)) {
        doc_after_edit();
        status(L"Найдено: «%s» (F3 — дальше)", what);
    } else {
        status(L"Не найдено: «%s»", what);
    }
    free(n8);
}

/* найти и заменить */
static void do_replace(void)
{
    wchar_t find[256], repl[256];
    char *f8, *r8;
    int n;

    if (!g_docmode || !flow_ready()) {
        status(L"Замена доступна в режиме документа (Вид → Режим документа)");
        return;
    }
    GetWindowTextW(g_search, find, 256);
    if (!find[0]) {
        status(L"Сначала введите текст в поле «Найти» на панели");
        SetFocus(g_search);
        return;
    }
    repl[0] = 0;
    if (!prompt_text(g_main, L"Найти и заменить", L"Заменить на (пусто — удалить):",
                     repl, 256, 0, 0))
        return;
    f8 = u8_from_w(find);
    r8 = u8_from_w(repl);
    n = flow_replace_all(f8, r8);
    free(f8);
    free(r8);
    doc_after_edit();
    if (n > 0) status(L"Заменено вхождений: %d", n);
    else status(L"Не найдено: «%s»", find);
}

/* ---------- таблицы и картинки ---------- */
static void do_insert_table(void)
{
    if (!g_docmode || !flow_ready()) {
        status(L"Таблицы вставляются в режиме документа (Ctrl+Shift+D)");
        return;
    }
    if (flow_insert_table(3, 3)) {
        fmt_after();
        status(L"Таблица 3×3 вставлена: печатайте в ячейке, Tab — следующая ячейка");
    }
}

static void do_table_cmd(int id)
{
    int ok = 0;
    if (!g_docmode || !flow_ready()) { status(L"Таблицы — в режиме документа"); return; }
    if (!flow_in_table() && id != IDC_INSERT_TABLE) {
        status(L"Курсор не в таблице — щёлкните по ячейке");
        return;
    }
    if (id == IDC_TABLE_ROW) ok = flow_table_row_add();
    else if (id == IDC_TABLE_COL) ok = flow_table_col_add();
    else if (id == IDC_TABLE_DEL) ok = flow_table_delete();
    if (ok) { fmt_after(); status(L"Таблица изменена"); }
    else status(L"Не удалось изменить таблицу");
}

static void do_insert_image(void)
{
    wchar_t file[MAX_PATH];
    OPENFILENAMEW ofn;
    static const wchar_t filter[] =
        L"Картинки (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
        L"Все файлы (*.*)\0*.*\0\0";
    if (!g_docmode || !flow_ready()) {
        status(L"Картинка вставляется в режиме документа (Ctrl+Shift+D)");
        return;
    }
    memset(&ofn, 0, sizeof(ofn));
    file[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Вставить картинку";
    if (!GetOpenFileNameW(&ofn)) return;
    if (flow_insert_image(file)) {
        fmt_after();
        status(L"Картинка вставлена под курсором");
    } else status(L"Не удалось прочитать картинку (поддерживаются PNG, JPEG, BMP)");
}

static void do_font_menu(void)
{
    HMENU m;
    POINT pt;
    int i, cmd;

    if (!g_docmode || !flow_ready()) {
        status(L"Шрифт выбирается в режиме документа (Ctrl+Shift+D)");
        return;
    }
    m = CreatePopupMenu();
    for (i = 0; i < 4; i++) {
        UINT f = MF_STRING;
        FlowFmt ff;
        flow_fmt_get(&ff);
        if (ff.font == i) f |= MF_CHECKED;
        AppendMenuW(m, f, IDC_FONT_BASE + i, flow_font_name(i));
    }
    GetCursorPos(&pt);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                              pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd >= IDC_FONT_BASE && cmd < IDC_FONT_BASE + 4) {
        flow_fmt_font(cmd - IDC_FONT_BASE);
        fmt_after();
        status(L"Шрифт: %s", flow_font_name(cmd - IDC_FONT_BASE));
    }
}

/* ---------- форматирование ---------- */
static void fmt_after(void)
{
    fmt_sync();
    doc_after_edit();
    if (g_main) InvalidateRect(g_main, NULL, FALSE);
}

static void do_margins(void)
{
    wchar_t buf[128] = L"20 20 20 20";
    float ml = 20, mt = 20, mr = 20, mb = 20;
    int i, ok = 1;
    if (!g_docmode || !flow_ready()) {
        status(L"Параметры страницы доступны в режиме документа");
        return;
    }
    {
        float cur_l, cur_t, cur_r, cur_b;
        flow_page_metrics(&cur_l, &cur_t, &cur_r, &cur_b, NULL, NULL);
        wsprintfW(buf, L"%d %d %d %d", (int)(cur_l / 2.8346f + 0.5f),
                  (int)(cur_t / 2.8346f + 0.5f), (int)(cur_r / 2.8346f + 0.5f),
                  (int)(cur_b / 2.8346f + 0.5f));
    }
    if (!prompt_text(g_main, L"Поля страницы",
                     L"Поля в мм — левое верхнее правое нижнее:", buf, 128, 0, 0))
        return;
    {
        int v[4] = { 0, 0, 0, 0 };
        int cnt = 0, num = -1;
        for (i = 0; buf[i]; i++) {
            if (buf[i] >= L'0' && buf[i] <= L'9') {
                if (num < 0) num = 0;
                num = num * 10 + (buf[i] - L'0');
            } else {
                if (num >= 0 && cnt < 4) v[cnt++] = num;
                num = -1;
            }
        }
        if (num >= 0 && cnt < 4) v[cnt++] = num;
        if (cnt > 0) {
            ml = (float)v[0];
            mt = cnt > 1 ? (float)v[1] : ml;
            mr = cnt > 2 ? (float)v[2] : ml;
            mb = cnt > 3 ? (float)v[3] : mt;
        } else ok = 0;
    }
    if (!ok) { status(L"Не понял значения полей"); return; }
    flow_margins_mm(ml, mt, mr, mb);
    g_pages = flow_pages();
    if (g_page >= g_pages) g_page = g_pages - 1;
    fmt_after();
    status(L"Поля: %d/%d/%d/%d мм", (int)ml, (int)mt, (int)mr, (int)mb);
}

static void do_page_size(int idx)
{
    int land;
    if (!g_docmode || !flow_ready()) {
        status(L"Параметры страницы доступны в режиме документа");
        return;
    }
    flow_page_metrics(NULL, NULL, NULL, NULL, &land, NULL);
    flow_page_setup(idx, land);
    g_pages = flow_pages();
    fit_width();
    fmt_after();
    status(L"Размер страниц изменён (сохраните PDF: Ctrl+S)");
}

static void do_orientation(int land)
{
    int idx;
    if (!g_docmode || !flow_ready()) {
        status(L"Параметры страницы доступны в режиме документа");
        return;
    }
    flow_page_metrics(NULL, NULL, NULL, NULL, NULL, &idx);
    flow_page_setup(idx, land);
    g_pages = flow_pages();
    fit_width();
    fmt_after();
    status(land ? L"Альбомная ориентация" : L"Книжная ориентация");
}

/* клавиатура в режиме документа; 1 — клавиша обработана */
static int doc_key(int vk, int ctrl, int shift)
{
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_DELETE: case VK_TAB:
        flow_vkey(vk, ctrl, shift);
        doc_after_edit();
        if (ctrl) g_skipChar = 1;
        return 1;
    case VK_PRIOR:
        go_page(g_page - 1);
        return 1;
    case VK_NEXT:
        go_page(g_page + 1);
        return 1;
    case 'A':
        if (ctrl) {
            flow_select_all();
            render_page();
            InvalidateRect(g_view, NULL, TRUE);
            g_skipChar = 1;
            return 1;
        }
        g_skipChar = 0;
        return 0;
    case 'C':
        if (ctrl) {
            char *t = flow_selected_utf8();
            if (t && t[0]) { clipboard_put_utf8(t); status(L"Скопировано: %d символов", (int)strlen(t)); }
            free(t);
            return 1;
        }
        return 0;
    case 'X':
        if (ctrl) {
            char *t = flow_selected_utf8();
            if (t && t[0]) { clipboard_put_utf8(t); flow_vkey(VK_DELETE, 0, 0); doc_after_edit(); }
            free(t);
            return 1;
        }
        return 0;
    case 'V':
        if (ctrl) {
            char *t = NULL;
            if (clipboard_get_utf8(&t) && t) { flow_paste_text(t); free(t); doc_after_edit(); }
            return 1;
        }
        return 0;
    case 'Z':
        if (ctrl) { flow_vkey('Z', 1, 0); doc_after_edit(); return 1; }
        return 0;
    case 'Y':
        if (ctrl) { flow_vkey('Y', 1, 0); doc_after_edit(); return 1; }
        return 0;
    case 'B':
        if (ctrl) { flow_fmt_attr(1); fmt_after(); return 1; }
        return 0;
    case 'I':
        if (ctrl) { flow_fmt_attr(2); fmt_after(); return 1; }
        return 0;
    case 'U':
        if (ctrl) { flow_fmt_attr(4); fmt_after(); return 1; }
        return 0;
    case 'E':
        if (ctrl) { flow_align(1); fmt_after(); return 1; }
        return 0;
    case 'R':
        if (ctrl) { flow_align(2); fmt_after(); return 1; }
        return 0;
    case 'J':
        if (ctrl) { flow_align(3); fmt_after(); return 1; }
        return 0;
    case 'L':
        if (ctrl && shift) { flow_list(1); fmt_after(); return 1; }
        if (ctrl) { flow_align(0); fmt_after(); return 1; }
        return 0;
    case 'M':
        if (ctrl && shift) { flow_list(2); fmt_after(); return 1; }
        return 0;
    case 'F':
        if (ctrl) { doc_find(0); return 1; }
        return 0;
    case 'H':
        if (ctrl) { do_replace(); return 1; }
        return 0;
    }
    if (ctrl && shift && (vk == 'D')) { do_toggle_docmode(); g_skipChar = 1; return 1; }
    return 0;
}

static int clipboard_get_utf8(char **out)
{
    HANDLE h;
    wchar_t *w;
    char *s;
    int n;
    *out = NULL;
    if (!OpenClipboard(g_main)) return 0;
    h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return 0; }
    w = (wchar_t *)GlobalLock(h);
    if (!w) { CloseClipboard(); return 0; }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n > 0) {
        s = (char *)malloc(n + 1);
        if (s) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
            *out = s;
        }
    }
    GlobalUnlock(h);
    CloseClipboard();
    return *out != NULL;
}

/* ---------- окно страницы ---------- */
static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(h, &ps);
        HDC dc, mem = NULL;
        HBITMAP memBmp = NULL;
        HGDIOBJ memOld = NULL;
        RECT rc;
        int ox, oy;
        GetClientRect(h, &rc);
        /* Рисуем в память и показываем одним BitBlt. Иначе при перетаскивании
           (инструмент «маркер» и др.) видно мерцание: сначала серый фон, потом
           страница. */
        if (rc.right > 0 && rc.bottom > 0) {
            mem = CreateCompatibleDC(screen);
            memBmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
            if (mem && memBmp) memOld = SelectObject(mem, memBmp);
            else {
                if (mem) { DeleteDC(mem); mem = NULL; }
                if (memBmp) { DeleteObject(memBmp); memBmp = NULL; }
            }
        }
        dc = mem ? mem : screen;
        FillRect(dc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));

        /* ---- несколько страниц на экране ---- */
        if (g_hasBmp && g_cont && g_open) {
            int n = g_pages, i;
            float scale = view_scale();
            HBRUSH sh = CreateSolidBrush(RGB(96, 96, 96));
            HBRUSH fr = CreateSolidBrush(RGB(128, 128, 128));
            doc_layout();
            for (i = 0; i < n; i++) {
                RECT pr;
                int pw = 0, ph = 0, px, py;
                HBITMAP b;
                page_rect_v(i, &pr);
                page_origin_ex(i, &px, &py);
                if (px >= rc.right || py >= rc.bottom) continue;
                if (px + (pr.right - pr.left) < 0 || py + (pr.bottom - pr.top) < 0) continue;
                if (i == g_page && g_bmpScale > 0 &&
                    (g_bmpScale > scale ? g_bmpScale - scale : scale - g_bmpScale) < 0.01f) {
                    b = g_bmp; pw = g_bmpW; ph = g_bmpH;
                } else {
                    b = page_bitmap(i, scale, &pw, &ph);
                }
                if (!b) continue;
                {
                    int saved = SaveDC(dc);
                    HDC mem;
                    HGDIOBJ ob;
                    /* обрезаем по ячейке сетки: страницы разного формата не наезжают */
                    int cw2 = g_cellW - PAGE_GAP, ch2 = g_cellH - PAGE_GAP;
                    int cx2 = px - (cw2 - pw) / 2, cy2 = py - (ch2 - ph) / 2;
                    IntersectClipRect(dc, cx2 - 2, cy2 - 2, cx2 + cw2 + 2, cy2 + ch2 + 2);
                    mem = CreateCompatibleDC(dc);
                    ob = SelectObject(mem, b);
                    BitBlt(dc, px, py, pw, ph, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, ob);
                    DeleteDC(mem);
                    RestoreDC(dc, saved);
                }
                {
                    RECT sr;
                    sr.left = px + 3; sr.top = py + ph + 1;
                    sr.right = px + pw + 3; sr.bottom = py + ph + 3;
                    FillRect(dc, &sr, sh);
                }
                FrameRect(dc, &(RECT){ px - 1, py - 1, px + pw + 1, py + ph + 1 }, fr);
                if (i != g_page && !g_docmode) te_paint_edits(dc, i, scale, px, py);
            }
            DeleteObject(sh);
            DeleteObject(fr);

            page_origin(&ox, &oy);
            paint_hits(dc, ox, oy);
            goto view_drag;
        }

        if (g_hasBmp) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, g_bmp);
            page_origin(&ox, &oy);
            BitBlt(dc, ox, oy, g_bmpW, g_bmpH, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);

            /* тень страницы */
            {
                HBRUSH sb = CreateSolidBrush(RGB(96, 96, 96));
                RECT sr;
                sr.left = ox + 3; sr.top = oy + g_bmpH + 1;
                sr.right = ox + g_bmpW + 3; sr.bottom = oy + g_bmpH + 3;
                FillRect(dc, &sr, sb);
                DeleteObject(sb);
            }

            paint_hits(dc, ox, oy);

view_drag:
            /* перетаскиваемая правка */
            if (g_drag) {
                if (g_dragTool == TOOL_INK && g_inkCount > 1) {
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(43, 87, 154));
                    HGDIOBJ op = SelectObject(dc, pen);
                    int i;
                    for (i = 0; i < g_inkCount; i++) {
                        int x = ox + (int)(g_ink[i].x * g_bmpScale);
                        int y = oy + (int)(g_ink[i].y * g_bmpScale);
                        if (i == 0) MoveToEx(dc, x, y, NULL);
                        else LineTo(dc, x, y);
                    }
                    SelectObject(dc, op);
                    DeleteObject(pen);
                } else if (g_dragTool != TOOL_SELECT && g_dragTool != TOOL_EDIT) {
                    HPEN pen = CreatePen(PS_DOT, 1, RGB(43, 87, 154));
                    HGDIOBJ op = SelectObject(dc, pen);
                    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    Rectangle(dc, g_dragFrom.x, g_dragFrom.y, g_dragTo.x, g_dragTo.y);
                    SelectObject(dc, ob);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                } else if (g_dragTool == TOOL_SELECT) {
                    HPEN pen = CreatePen(PS_DOT, 1, RGB(68, 114, 196));
                    HGDIOBJ op = SelectObject(dc, pen);
                    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    Rectangle(dc, g_dragFrom.x, g_dragFrom.y, g_dragTo.x, g_dragTo.y);
                    SelectObject(dc, ob);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
            }

            /* правка текста «как в Word» (в режиме документа текст уже
               входит в перевёрстанный текст — наложения рисовать нельзя,
               иначе абзацы дублируются и «заплатки» закрывают текст) */
            if (g_open && !g_docmode) {
                te_paint_edits(dc, g_page, g_bmpScale, ox, oy);
                if (te_is_active() && te_active_page() == g_page) {
                    te_set_origin(g_bmpScale, ox, oy);
                    te_paint(dc);
                }
            }
        } else {
            const wchar_t *msg = g_open ? L"Страница не отрисована" : L"Откройте PDF: Ctrl+O";
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (mem) {
            BitBlt(screen, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            if (memOld) SelectObject(mem, memOld);
            if (memBmp) DeleteObject(memBmp);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_SIZE:
        update_scrollbars();
        InvalidateRect(h, NULL, TRUE);
        return 0;

    case WM_VSCROLL: {
        SCROLLINFO si;
        int pos;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(h, SB_VERT, &si);
        pos = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP:    pos -= 40; break;
        case SB_LINEDOWN:  pos += 40; break;
        case SB_PAGEUP:    pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:  pos += (int)si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        default: return 0;
        }
        if (pos < si.nMin) pos = si.nMin;
        if (pos > si.nMax) pos = si.nMax;
        g_scrollY = pos;
        SetScrollPos(h, SB_VERT, pos, TRUE);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }

    case WM_HSCROLL: {
        SCROLLINFO si;
        int pos;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(h, SB_HORZ, &si);
        pos = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP:    pos -= 40; break;
        case SB_LINEDOWN:  pos += 40; break;
        case SB_PAGEUP:    pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:  pos += (int)si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        default: return 0;
        }
        if (pos < si.nMin) pos = si.nMin;
        if (pos > si.nMax) pos = si.nMax;
        g_scrollX = pos;
        SetScrollPos(h, SB_HORZ, pos, TRUE);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            PostMessageW(g_main, WM_COMMAND,
                         delta > 0 ? IDC_ZOOMIN : IDC_ZOOMOUT, 0);
        } else {
            g_scrollY -= (delta / WHEEL_DELTA) * 60;
            if (g_scrollY < 0) g_scrollY = 0;
            SetScrollPos(h, SB_VERT, g_scrollY, TRUE);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        fz_point p;
        if (!g_open || !g_hasBmp) return 0;
        SetFocus(h);
        p = client_to_page(x, y);

        if (g_docmode) {                       /* текст как в Word */
            flow_click(g_page, p.x, p.y, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0);
            SetCapture(h);
            render_page();
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }

        if (g_tool == TOOL_EDIT) {
            if (te_is_active() && te_active_page() == g_page && te_hit_block(p.x, p.y)) {
                te_place_caret(p.x, p.y, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0);
                g_editDrag = 1;
                SetCapture(h);
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
            if (te_is_active()) {
                if (te_commit()) { g_doc.dirty = 1; set_title(); }
            }
            if (te_begin(&g_doc, g_page, p.x, p.y, g_bmpScale, 0, 0)) {
                g_editDrag = 1;
                SetCapture(h);
                status(L"Правка текста: печатайте; Esc — готово, Ctrl+Z — отмена");
            } else {
                status(L"Тут нет текста. Для сканов: «Распознать страницу»");
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }

        if (g_tool == TOOL_SELECT) {
            PdfWidget wg;
            if (pdfdoc_widget_at(&g_doc, g_page, p, &wg)) {
                if (wg.readonly) { status(L"Поле только для чтения"); return 0; }
                if (wg.type == 2 || wg.type == 5) {
                    if (pdfdoc_widget_set(&g_doc, g_page, p, NULL)) {
                        g_doc.dirty = 1;
                        render_page();
                        InvalidateRect(h, NULL, TRUE);
                        set_title();
                        status(L"Поле «%S» переключено", wg.name);
                    }
                } else {
                    wchar_t val[1024];
                    w_from_u8(wg.value, val, 1024);
                    if (prompt_text(g_main, L"Значение поля", L"Введите значение:",
                                    val, 1024, 0, 0)) {
                        char *v = u8_from_w(val);
                        if (pdfdoc_widget_set(&g_doc, g_page, p, v ? v : "")) {
                            g_doc.dirty = 1;
                            render_page();
                            InvalidateRect(h, NULL, TRUE);
                            set_title();
                            status(L"Значение поля сохранено");
                        }
                        free(v);
                    }
                }
                return 0;
            }
        }

        if (g_tool == TOOL_ERASE) {
            if (pdfdoc_annot_delete_at(&g_doc, g_page, p)) {
                g_doc.dirty = 1;
                render_page();
                InvalidateRect(h, NULL, TRUE);
                set_title();
                status(L"Правка удалена");
            } else status(L"Под курсором нет правки");
            return 0;
        }

        if (g_tool == TOOL_NOTE) {
            wchar_t txt[1024];
            if (prompt_text(g_main, L"Примечание", L"Текст примечания:", txt, 1024, 0, 1)) {
                char *t8 = u8_from_w(txt);
                fz_rect r;
                r.x0 = p.x; r.y0 = p.y; r.x1 = p.x + 20; r.y1 = p.y + 20;
                if (pdfdoc_annot_add(&g_doc, g_page, ANNO_NOTE, r, NULL, 0, t8 ? t8 : "")) {
                    g_doc.dirty = 1;
                    render_page();
                    InvalidateRect(h, NULL, TRUE);
                    set_title();
                    status(L"Заметка добавлена (сохраните файл: Ctrl+S)");
                }
                free(t8);
            }
            return 0;
        }

        SetCapture(h);
        g_drag = 1;
        g_dragTool = g_tool;
        g_dragFrom.x = x;
        g_dragFrom.y = y;
        g_dragTo = g_dragFrom;
        g_inkCount = 0;
        if (g_tool == TOOL_INK) g_ink[g_inkCount++] = p;
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (g_docmode && flow_dragging()) {
            fz_point pp = client_to_page(x, y);
            flow_drag(pp.x, pp.y);
            render_page();
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        if (g_editDrag) {
            fz_point p = client_to_page(x, y);
            te_place_caret(p.x, p.y, 1);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (!g_drag) return 0;
        g_dragTo.x = x;
        g_dragTo.y = y;
        if (g_dragTool == TOOL_INK) {
            fz_point p = client_to_page(x, y);
            if (g_inkCount < 4096) g_ink[g_inkCount++] = p;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (g_docmode) {
            if (flow_dragging()) {
                fz_point pp = client_to_page(x, y);
                flow_drag(pp.x, pp.y);
            }
            flow_drag_end();
            ReleaseCapture();
            render_page();
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        if (g_editDrag) { g_editDrag = 0; ReleaseCapture(); return 0; }
        if (!g_drag) return 0;
        ReleaseCapture();
        g_drag = 0;
        g_dragTo.x = x;
        g_dragTo.y = y;

        if (g_dragTool == TOOL_SELECT) {
            fz_rect sel;
            fz_point a = client_to_page(g_dragFrom.x, g_dragFrom.y);
            fz_point b = client_to_page(g_dragTo.x, g_dragTo.y);
            fz_rect boxes[256];
            int nb = 0;
            char *txt;
            sel.x0 = a.x < b.x ? a.x : b.x;
            sel.y0 = a.y < b.y ? a.y : b.y;
            sel.x1 = a.x < b.x ? b.x : a.x;
            sel.y1 = a.y < b.y ? b.y : a.y;
            txt = pdfdoc_text_in_rect(&g_doc, g_page, sel, boxes, 256, &nb);
            if (txt && txt[0]) {
                /* подсветить и скопировать */
                int i;
                HDC dc = GetDC(h);
                int ox, oy;
                page_origin(&ox, &oy);
                for (i = 0; i < nb; i++) {
                    RECT r;
                    r.left = ox + (int)(boxes[i].x0 * g_bmpScale);
                    r.top = oy + (int)(boxes[i].y0 * g_bmpScale);
                    r.right = ox + (int)(boxes[i].x1 * g_bmpScale) + 1;
                    r.bottom = oy + (int)(boxes[i].y1 * g_bmpScale) + 1;
                    InvertRect(dc, &r);
                }
                ReleaseDC(h, dc);
                clipboard_put_utf8(txt);
                status(L"Скопировано в буфер обмена: %d символов", (int)strlen(txt));
            }
            free(txt);
            return 0;
        }

        if (g_dragTool == TOOL_HIGHLIGHT || g_dragTool == TOOL_RECT ||
            g_dragTool == TOOL_TEXT || g_dragTool == TOOL_INK) {
            fz_rect r;
            fz_point a = client_to_page(g_dragFrom.x, g_dragFrom.y);
            fz_point b = client_to_page(g_dragTo.x, g_dragTo.y);
            int kind = ANNO_RECT;
            int ok = 0;

            r.x0 = a.x < b.x ? a.x : b.x;
            r.y0 = a.y < b.y ? a.y : b.y;
            r.x1 = a.x < b.x ? b.x : a.x;
            r.y1 = a.y < b.y ? b.y : a.y;

            if (g_dragTool == TOOL_HIGHLIGHT) kind = ANNO_HIGHLIGHT;
            else if (g_dragTool == TOOL_TEXT) kind = ANNO_FREETEXT;
            else if (g_dragTool == TOOL_INK) kind = ANNO_INK;

            if (kind == ANNO_INK) {
                if (g_inkCount > 1) ok = pdfdoc_annot_add(&g_doc, g_page, kind, r, g_ink, g_inkCount, NULL);
            } else if (kind == ANNO_FREETEXT) {
                wchar_t txt[1024];
                if (prompt_text(g_main, L"Текст", L"Текст надписи:", txt, 1024, 0, 1)) {
                    char *t8 = u8_from_w(txt);
                    ok = pdfdoc_annot_add(&g_doc, g_page, kind, r, NULL, 0, t8 ? t8 : "");
                    free(t8);
                }
            } else {
                if (r.x1 - r.x0 < 3 || r.y1 - r.y0 < 3) { InvalidateRect(h, NULL, TRUE); return 0; }
                ok = pdfdoc_annot_add(&g_doc, g_page, kind, r, NULL, 0, NULL);
            }
            if (ok) {
                g_doc.dirty = 1;
                render_page();
                InvalidateRect(h, NULL, TRUE);
                set_title();
                status(L"Правка добавлена (сохраните файл: Ctrl+S)");
            } else {
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }

    case WM_CHAR:
        if (g_docmode && g_skipChar) { g_skipChar = 0; return 0; }
        if (g_docmode) {
            if (w == 13) { flow_vkey(VK_RETURN, 0, 0); doc_after_edit(); }
            else if (w == 8) { flow_vkey(VK_BACK, 0, 0); doc_after_edit(); }
            else if (w >= 32 && w != 127) { flow_char((int)w); doc_after_edit(); }
            return 0;
        }
        if (te_is_active()) {
            if (w == 13) te_insert('\n');
            else if (w >= 32 && w != 127) te_insert((int)w);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        break;

    case WM_KEYDOWN: {
        if (g_docmode) {
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
            int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
            if (doc_key((int)w, ctrl, shift)) {
                if (ctrl) g_skipChar = 1;
                return 0;
            }
            if (w == VK_ESCAPE && wui_menu_open()) {
                wui_close_backstage();
                SendMessageW(g_main, WM_SIZE, 0, 0);
                InvalidateRect(g_main, NULL, FALSE);
            }
            return 0;                      /* прочие клавиши — не команды */
        }
        if (te_is_active()) {
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
            int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
            int done = 0;
            switch (w) {
            case VK_LEFT:   te_move_h(-1, shift); done = 1; break;
            case VK_RIGHT:  te_move_h(1, shift); done = 1; break;
            case VK_UP:     te_move_v(-1, shift); done = 1; break;
            case VK_DOWN:   te_move_v(1, shift); done = 1; break;
            case VK_HOME:   te_home_end(0, ctrl, shift); done = 1; break;
            case VK_END:    te_home_end(1, ctrl, shift); done = 1; break;
            case VK_BACK:   te_delete(1); done = 1; break;
            case VK_DELETE: te_delete(0); done = 1; break;
            case 'Z': if (ctrl) { te_undo(); done = 1; } break;
            case 'A': if (ctrl) { te_select_all(); done = 1; } break;
            case VK_ESCAPE:
                if (te_commit()) { g_doc.dirty = 1; set_title(); }
                status(L"Правка текста применена (сохраните файл: Ctrl+S)");
                done = 1;
                break;
            }
            if (done) {
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
            return 0;                       /* буквы уходят в текст, а не в команды */
        }
        if (w == VK_ESCAPE && wui_menu_open()) {
            wui_close_backstage();
            SendMessageW(g_main, WM_SIZE, 0, 0);
            InvalidateRect(g_main, NULL, FALSE);
            return 0;
        }
        switch (w) {
        case VK_PRIOR: SendMessageW(g_main, WM_COMMAND, IDC_PREV, 0); return 0;
        case VK_NEXT:  SendMessageW(g_main, WM_COMMAND, IDC_NEXT, 0); return 0;
        case VK_F3:    SendMessageW(g_main, WM_COMMAND, IDC_FINDNEXT, 0); return 0;
        case 'V': select_tool(TOOL_SELECT); return 0;
        case 'T': select_tool(TOOL_TEXT); return 0;
        case 'H': select_tool(TOOL_HIGHLIGHT); return 0;
        case 'R': select_tool(TOOL_RECT); return 0;
        case 'N': select_tool(TOOL_NOTE); return 0;
        case 'I': select_tool(TOOL_INK); return 0;
        case 'E': select_tool(TOOL_ERASE); return 0;
        case 'Q': select_tool(TOOL_EDIT); return 0;
        case VK_ADD: case VK_OEM_PLUS: SendMessageW(g_main, WM_COMMAND, IDC_ZOOMIN, 0); return 0;
        case VK_SUBTRACT: case VK_OEM_MINUS: SendMessageW(g_main, WM_COMMAND, IDC_ZOOMOUT, 0); return 0;
        case '0': SendMessageW(g_main, WM_COMMAND, IDC_FITW, 0); return 0;
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        wchar_t file[MAX_PATH];
        if (DragQueryFileW(drop, 0, file, MAX_PATH)) do_open_file(file);
        DragFinish(drop);
        return 0;
    }

    case WM_SETCURSOR:
        if (g_tool == TOOL_INK || g_tool == TOOL_EDIT) {
            SetCursor(LoadCursor(NULL, IDC_IBEAM));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- поля ввода на панели: Enter = применить ---------- */
static WNDPROC g_searchOldProc, g_pageNoOldProc;

static LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        SendMessageW(g_main, WM_COMMAND, IDC_PAGENO, 0);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN)) return 0;
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { SetFocus(g_view); return 0; }
    return CallWindowProcW(g_pageNoOldProc, h, m, w, l);
}

static LRESULT CALLBACK SearchProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        SendMessageW(g_main, WM_COMMAND, IDC_FIND, 0);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        SetFocus(g_view);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    return CallWindowProcW(g_searchOldProc, h, m, w, l);
}


/* ---------- всплывающая подсказка (своя, чтобы вид не зависел от версии Windows) ---------- */
static HWND g_tipWnd;
static wchar_t g_tipText[256];

static LRESULT CALLBACK TipProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        {
            HBRUSH br = CreateSolidBrush(RGB(200, 203, 208));
            FrameRect(dc, &rc, br);
            DeleteObject(br);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(38, 38, 38));
        SelectObject(dc, g_font);
        rc.left += 6; rc.right -= 6; rc.top += 4; rc.bottom -= 4;
        DrawTextW(dc, g_tipText, -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void tip_hide(void)
{
    if (g_tipWnd && IsWindowVisible(g_tipWnd)) ShowWindow(g_tipWnd, SW_HIDE);
}

static void tip_show(const wchar_t *text, int x, int y)
{
    static int reg;
    HDC dc;
    int tw, th;
    POINT pt;
    RECT wa;

    if (!text || !text[0]) { tip_hide(); return; }
    if (lstrcmpW(g_tipText, text) != 0) lstrcpynW(g_tipText, text, 256);
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = TipProc;
        wc.hInstance = g_inst;
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"PdfStudioTip";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    dc = CreateCompatibleDC(NULL);
    SelectObject(dc, g_font);
    {
        RECT r = { 0, 0, 240, 0 };
        DrawTextW(dc, g_tipText, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
        tw = r.right + 12;
        th = r.bottom + 8;
    }
    DeleteDC(dc);
    if (tw < 60) tw = 60;
    if (th < 22) th = 22;

    pt.x = x; pt.y = y;
    ClientToScreen(g_main, &pt);
    pt.y += 20;
    if (pt.x + tw > GetSystemMetrics(SM_CXSCREEN)) pt.x = GetSystemMetrics(SM_CXSCREEN) - tw - 4;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) SetRect(&wa, 0, 0, 1024, 768);
    if (pt.y + th > wa.bottom) pt.y = pt.y - th - 34;
    if (!g_tipWnd)
        g_tipWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"PdfStudioTip", L"",
                                   WS_POPUP, pt.x, pt.y, tw, th, g_main, NULL, g_inst, NULL);
    if (!g_tipWnd) return;
    SetWindowPos(g_tipWnd, HWND_TOPMOST, pt.x, pt.y, tw, th, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_tipWnd, NULL, TRUE);
}

/* ---------- цвет фона страницы под блоком текста ---------- */
/* Берём цвета у четырёх углов блока (чуть снаружи рамки, где нет букв):
   - однородный фон  -> заливка этим цветом (и запись его в PDF);
   - неоднородный    -> градиент по углам в предпросмотре, а в PDF заливку
                        не пишем вовсе (redaction уже убрал буквы, фон остаётся). */
static int color_median(int *r, int *g, int *b, int n, COLORREF *out)
{
    int i, j;
    if (n <= 0) return 0;
    for (i = 1; i < n; i++) {
        j = i;
        while (j > 0) {
            int tr, tg, tb;
            if (r[j - 1] + g[j - 1] + b[j - 1] <= r[j] + g[j] + b[j]) break;
            tr = r[j - 1]; tg = g[j - 1]; tb = b[j - 1];
            r[j - 1] = r[j]; g[j - 1] = g[j]; b[j - 1] = b[j];
            r[j] = tr; g[j] = tg; b[j] = tb;
            j--;
        }
    }
    *out = RGB(r[n / 2], g[n / 2], b[n / 2]);
    return 1;
}

static void te_bg_sample(int page, const fz_rect *box, TeBgInfo *out)
{
    HBITMAP bmp = NULL;
    float scale = 0;
    HDC dc;
    HGDIOBJ old;
    int w = 0, h = 0, i;
    int x0, y0, x1, y1;

    for (i = 0; i < 4; i++) out->c[i] = RGB(255, 255, 255);
    out->uniform = 1;

    if (page == g_page && g_hasBmp && g_bmp) {
        bmp = g_bmp; w = g_bmpW; h = g_bmpH; scale = g_bmpScale;
    } else {
        for (i = 0; i < PGCACHE; i++)
            if (g_pc[i].bmp && g_pc[i].page == page) {
                bmp = g_pc[i].bmp; w = g_pc[i].w; h = g_pc[i].h; scale = g_pc[i].scale;
                break;
            }
    }
    if (!bmp || scale <= 0) return;

    x0 = (int)(box->x0 * scale);
    y0 = (int)(box->y0 * scale);
    x1 = (int)(box->x1 * scale);
    y1 = (int)(box->y1 * scale);

    dc = CreateCompatibleDC(NULL);
    if (!dc) return;
    old = SelectObject(dc, bmp);
    {
        /* для каждого угла — три точки рядом с ним (снаружи блока) */
        static const int ox4[4] = { -1, 1, -1, 1 };
        static const int oy4[4] = { -1, -1, 1, 1 };
        int k;
        for (k = 0; k < 4; k++) {
            int px[3], py[3], rr[3], gg[3], bb[3], n = 0, t;
            COLORREF c = RGB(255, 255, 255);
            int bx = (ox4[k] < 0) ? x0 - 5 : x1 + 5;
            int by = (oy4[k] < 0) ? y0 - 5 : y1 + 5;
            int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;

            px[0] = bx; py[0] = (oy4[k] < 0) ? my : my;      /* у боковой кромки   */
            px[1] = (ox4[k] < 0) ? x0 + 3 : x1 - 3; py[1] = by; /* у верхней/нижней */
            px[2] = bx; py[2] = by;                           /*直接 угол          */
            for (t = 0; t < 3; t++) {
                COLORREF q;
                if (px[t] < 0 || py[t] < 0 || px[t] >= w || py[t] >= h) continue;
                q = GetPixel(dc, px[t], py[t]);
                if (q == CLR_INVALID) continue;
                rr[n] = GetRValue(q); gg[n] = GetGValue(q); bb[n] = GetBValue(q);
                n++;
            }
            color_median(rr, gg, bb, n, &c);
            out->c[k] = c;
        }
        /* однородность: разброс яркости между углами */
        {
            int lo = 999, hi = -1;
            for (k = 0; k < 4; k++) {
                int v = (GetRValue(out->c[k]) * 3 + GetGValue(out->c[k]) * 6 +
                         GetBValue(out->c[k])) / 10;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            out->uniform = (hi - lo <= 12) ? 1 : 0;
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
}

/* ---------- регистрация приложением по умолчанию для PDF ---------- */
#define PROGID_DOC  L"PDFStudio.Document"
#define APP_NAME_UI L"PDF Студия"

static void reg_write(const wchar_t *key, const wchar_t *val, const wchar_t *data)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, val, 0, REG_SZ, (const BYTE *)data,
                   (DWORD)((lstrlenW(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static void reg_assoc(const wchar_t *sub, const wchar_t *val, const wchar_t *data)
{
    wchar_t key[2 * MAX_PATH];
    wsprintfW(key, L"Software\\Classes\\%s", sub);
    reg_write(key, val, data);
}

/* Vista/7: официальный API. На XP объекта нет — просто ничего не произойдёт. */
static void assoc_set_default_api(void)
{
    static const GUID clsid = { 0x591209c7, 0x767b, 0x42b2,
                                { 0x9f, 0xba, 0x44, 0xee, 0x46, 0x15, 0xf2, 0xc7 } };
    static const GUID iid_ar = { 0x4e530b0a, 0xe611, 0x4c77,
                                 { 0xa3, 0xac, 0x90, 0x31, 0xd0, 0x22, 0x28, 0x1b } };
    typedef HRESULT (STDMETHODCALLTYPE *PFN_SETA)(void *, const wchar_t *, const wchar_t *, int);
    typedef ULONG   (STDMETHODCALLTYPE *PFN_REL)(void *);
    void **obj = NULL;
    HRESULT hr;

    hr = CoInitialize(NULL);
    {
        HRESULT hrc = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid_ar, (void **)&obj);
        /* проверяем и сам объект, и оба нужных слота таблицы методов:
           на системах, где объект есть, но реализован частично (и в wine),
           слот может быть NULL — вызов такого указателя = падение */
        if (SUCCEEDED(hrc) && obj && obj[2] && obj[3]) {
            PFN_SETA setdef = (PFN_SETA)obj[3];      /* SetAppAsDefault */
            setdef(obj, APP_NAME_UI, L".pdf", 1);    /* AT_FILEEXTENSION */
            ((PFN_REL)obj[2])(obj);                  /* Release */
        } else if (SUCCEEDED(hrc) && obj) {
            ((PFN_REL)obj[2])(obj);                  /* Release без вызова метода */
        }
    }
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
}

static int assoc_register(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 8], ico[MAX_PATH + 8];
    HKEY k;
    DWORD one = 1;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 0;
    wsprintfW(cmd, L"\"%s\" \"%%1\"", exe);
    wsprintfW(ico, L"%s,0", exe);

    /* ProgID */
    reg_assoc(PROGID_DOC, NULL, L"Документ PDF (PDF Студия)");
    reg_assoc(PROGID_DOC L"\\DefaultIcon", NULL, ico);
    reg_assoc(PROGID_DOC L"\\shell\\open", NULL, L"Открыть");
    reg_assoc(PROGID_DOC L"\\shell\\open\\command", NULL, cmd);

    /* расширение .pdf → наш ProgID (работает на XP/2000/Vista, HKCU — без прав админа) */
    reg_assoc(L".pdf", NULL, PROGID_DOC);
    reg_assoc(L".pdf\\OpenWithProgids", PROGID_DOC, L"");
    reg_assoc(L".pdf\\OpenWithList", L"PDF-Studio.exe", L"");

    /* для «Открыть с помощью» и списка программ */
    reg_assoc(L"Applications\\PDF-Studio.exe", NULL, APP_NAME_UI);
    reg_assoc(L"Applications\\PDF-Studio.exe\\DefaultIcon", NULL, ico);
    reg_assoc(L"Applications\\PDF-Studio.exe\\shell\\open\\command", NULL, cmd);
    reg_assoc(L"Applications\\PDF-Studio.exe\\SupportedTypes", L".pdf", L"");

    /* описание возможностей для «Программ по умолчанию» (Vista+) */
    reg_write(L"Software\\PDFStudio\\Capabilities", L"ApplicationName", APP_NAME_UI);
    reg_write(L"Software\\PDFStudio\\Capabilities", L"ApplicationDescription",
              L"Читает, правит и распознаёт PDF — бесплатно и без интернета");
    reg_write(L"Software\\PDFStudio\\Capabilities\\FileAssociations", L".pdf", PROGID_DOC);
    reg_write(L"Software\\RegisteredApplications", APP_NAME_UI, L"Software\\PDFStudio\\Capabilities");

    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PDFStudio", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"AssocAsked", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
        RegCloseKey(k);
    }

    assoc_set_default_api();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return 1;
}

static int assoc_is_default(void)
{
    HKEY k;
    wchar_t buf[256];
    DWORD sz = sizeof(buf), type;
    buf[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    RegQueryValueExW(k, NULL, NULL, &type, (BYTE *)buf, &sz);
    RegCloseKey(k);
    return lstrcmpiW(buf, PROGID_DOC) == 0;
}

static int assoc_asked(void)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PDFStudio", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    RegQueryValueExW(k, L"AssocAsked", NULL, &type, (BYTE *)&v, &sz);
    RegCloseKey(k);
    return v == 1;
}

/* если exe перенесли — обновляем путь в реестре, чтобы ярлыки не отвалились */
static void assoc_refresh_path(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 8], ico[MAX_PATH + 8];
    if (!assoc_is_default()) return;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    wsprintfW(cmd, L"\"%s\" \"%%1\"", exe);
    wsprintfW(ico, L"%s,0", exe);
    reg_assoc(PROGID_DOC L"\\shell\\open\\command", NULL, cmd);
    reg_assoc(PROGID_DOC L"\\DefaultIcon", NULL, ico);
    reg_assoc(L"Applications\\PDF-Studio.exe\\shell\\open\\command", NULL, cmd);
}

/* первый запуск: спросить, назначить ли себя программой для PDF */
static void assoc_first_run_prompt(void)
{
    if (assoc_asked()) { assoc_refresh_path(); return; }
    if (MessageBoxW(g_main,
            L"Сделать PDF Студию программой для открытия PDF-файлов?\n\n"
            L"Двойной щелчок по любому PDF будет открывать этот редактор.\n"
            L"Изменить решение можно в меню «Справка → Сделать программой по умолчанию».",
            L"PDF Студия", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES)
        assoc_register();
    else {
        HKEY k;
        DWORD one = 1;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PDFStudio", 0, NULL, 0,
                            KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"AssocAsked", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
            RegCloseKey(k);
        }
    }
}

/* ---------- раскладка ---------- */
static void layout(HWND h)
{
    RECT rc, vr, sr, pr;
    int ox, oy;

    GetClientRect(h, &rc);
    wui_layout(rc.right, rc.bottom);

    vr = wui_view_rect();
    MoveWindow(g_view, vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top, TRUE);

    sr = wui_search_rect();
    if (sr.right > sr.left)
        MoveWindow(g_search, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, TRUE);

    pr = wui_page_rect();
    if (pr.right > pr.left)
        MoveWindow(g_pageNo, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, TRUE);

    page_origin(&ox, &oy);
    wui_set_ruler(vr.left + ox, g_hasBmp ? g_bmpW : 0);

    ui_sync();
}

/* ---------- главное окно ---------- */
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        static int viewReg;
        LOGFONTW lf;

        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -12;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpyW(lf.lfFaceName, L"Segoe UI");
        g_font = CreateFontIndirectW(&lf);
        if (!g_font) {
            lstrcpyW(lf.lfFaceName, L"Tahoma");
            g_font = CreateFontIndirectW(&lf);
        }
        if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        wui_init(h, g_inst, CLEARTYPE_QUALITY);

        if (!viewReg) {
            WNDCLASSW wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = ViewProc;
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = L"PdfStudioView";
            RegisterClassW(&wc);
            viewReg = 1;
        }

        g_view = CreateWindowExW(0, L"PdfStudioView", L"",
                                 WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL |
                                 WS_CLIPSIBLINGS,
                                 0, 0, 10, 10, h, NULL, g_inst, NULL);

        g_search = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 10, 10, h, (HMENU)IDC_SEARCH, g_inst, NULL);
        SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_searchOldProc = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC,
                                                     (LONG_PTR)SearchProc);

        g_pageNo = CreateWindowExW(0, L"EDIT", L"1",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                   ES_NUMBER | ES_CENTER,
                                   0, 0, 10, 10, h, (HMENU)IDC_PAGENO, g_inst, NULL);
        SendMessageW(g_pageNo, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_pageNoOldProc = (WNDPROC)SetWindowLongPtrW(g_pageNo, GWLP_WNDPROC,
                                                     (LONG_PTR)PageProc);

        lstrcpynW(g_statusText,
                  L"Откройте PDF (Ctrl+O) или перетащите файл в окно · сборка " BUILD_DATE,
                  512);
        wui_set_status(g_statusText);
        select_tool(TOOL_SELECT);
        DragAcceptFiles(h, TRUE);
        layout(h);
        return 0;
    }

    case WM_SIZE:
        wui_set_maximized(IsZoomed(h) ? 1 : 0);
        if (g_view) layout(h);
        return 0;

    case WM_SETFOCUS:
        if (g_view) SetFocus(g_view);
        return 0;

    case WM_NCCALCSIZE:
        if (w) {
            RECT *rc = &((NCCALCSIZE_PARAMS *)l)->rgrc[0];
            if (IsZoomed(h)) {
                int pad = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                rc->left += pad; rc->right -= pad;
                rc->top += pad; rc->bottom -= pad;
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt;
        RECT wr;
        int bw = 6;
        pt.x = (short)LOWORD(l);
        pt.y = (short)HIWORD(l);
        GetWindowRect(h, &wr);
        if (!IsZoomed(h)) {
            int left = pt.x < wr.left + bw, right = pt.x >= wr.right - bw;
            int top = pt.y < wr.top + bw, bottom = pt.y >= wr.bottom - bw;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        return wui_caption_hittest(pt.x - wr.left, pt.y - wr.top);
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        RECT wa;
        mm->ptMinTrackSize.x = 780;
        mm->ptMinTrackSize.y = 540;
        /* окно без системной рамки (WS_POPUP): при развороте «на весь экран»
           оно иначе накрывает панель задач Windows. Ограничиваем рабочей
           областью — панель задач остаётся видимой и доступной. */
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            mm->ptMaxPosition.x = wa.left;
            mm->ptMaxPosition.y = wa.top;
            mm->ptMaxSize.x = wa.right - wa.left;
            mm->ptMaxSize.y = wa.bottom - wa.top;
        }
        return 0;
    }

    case WM_GETICON:
    case WM_SETICON:
        break;

    case WM_ACTIVATE:
    case WM_NCACTIVATE:
        return TRUE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        wui_paint(dc, rc.right, rc.bottom);
        EndPaint(h, &ps);
        wui_repaint_menu();
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(38, 38, 38));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        float z;
        if (!g_tracking) {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            g_tracking = 1;
        }
        if (wui_in_chrome(x, y) || wui_in_status(x, y) || wui_menu_open()) {
            int need = wui_mouse_move(x, y);
            {
                /* пока открыто меню, подсказки не показываем */
                const wchar_t *tip = wui_menu_open() ? NULL : wui_tip_at(x, y);
                if (!tip || (g_tipShown[0] && lstrcmpW(tip, g_tipShown) != 0) || !tip) {
                    if (!tip || lstrcmpW(tip, g_tipShown) != 0) { tip_hide(); g_tipShown[0] = 0; }
                }
                g_tipFor = 0;
                g_tipX = x; g_tipY = y;
                if (tip) { lstrcpynW(g_tipShown, tip, 256); SetTimer(h, 1, 650, NULL); }
                else { KillTimer(h, 1); tip_hide(); }
            }
            if (wui_zoom_pending(&z)) {
                if (g_open) {
                    g_zoom = z;
                    if (g_zoom < ZOOM_MIN) g_zoom = ZOOM_MIN;
                    if (g_zoom > ZOOM_MAX) g_zoom = ZOOM_MAX;
                    render_page();
                    update_scrollbars();
                    InvalidateRect(g_view, NULL, FALSE);
                }
            }
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }

    case WM_TIMER:
        if (w == 1) {
            KillTimer(h, 1);
            if (g_tipShown[0]) tip_show(g_tipShown, g_tipX, g_tipY);
        } else if (w == 2) {
            KillTimer(h, 2);
        }
        return 0;

    case WM_MOUSELEAVE:
        g_tracking = 0;
        KillTimer(h, 1);
        g_tipShown[0] = 0;
        tip_hide();
        wui_mouse_leave();
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int cmd = 0;
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        tip_hide();
        g_tipShown[0] = 0;
        KillTimer(h, 1);
        if (wui_in_chrome(x, y) || wui_in_status(x, y) || wui_menu_open()) {
            if (wui_mouse_down(x, y, &cmd)) {
                SetCapture(h);
                layout(h);
                InvalidateRect(h, NULL, FALSE);
                if (cmd == WCMD_MINIMIZE) ShowWindow(h, SW_MINIMIZE);
                else if (cmd == WCMD_MAXIMIZE) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
                else if (cmd == WCMD_CLOSE) SendMessageW(h, WM_CLOSE, 0, 0);
                else if (cmd) SendMessageW(h, WM_COMMAND, cmd, 0);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int cmd = 0;
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (GetCapture() == h) ReleaseCapture();
        if (wui_mouse_up(x, y, &cmd) && cmd) SendMessageW(h, WM_COMMAND, cmd, 0);
        layout(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w);
        switch (id) {
        case IDC_OPEN: do_open(); return 0;
        case IDC_DOCMODE: do_toggle_docmode(); return 0;
        case IDC_SAVE: do_save(0); return 0;
        case IDC_SAVEAS: do_save(1); return 0;
        case IDC_PRINT: do_print(); return 0;
        case IDC_QUIT: DestroyWindow(h); return 0;
        case IDC_COPYTEXT: copy_page_text(); return 0;
        case IDC_FINDREPL: do_replace(); return 0;
        case IDC_FMT_B: case IDC_FMT_I: case IDC_FMT_U:
        case IDC_FMT_BIGGER: case IDC_FMT_SMALLER:
        case IDC_ALIGN_L: case IDC_ALIGN_C: case IDC_ALIGN_R: case IDC_ALIGN_J:
        case IDC_LIST_B: case IDC_LIST_N: case IDC_LIST_NONE:
        case IDC_SPACING: case IDC_INDENTP: case IDC_INDENTM: case IDC_SPACEBEFORE:
        case IDC_DOCCOPY: case IDC_DOCCUT: case IDC_DOCPASTE:
        case IDC_PAGE_A4: case IDC_PAGE_A5: case IDC_PAGE_LETTER:
        case IDC_PORTR: case IDC_LANDR: case IDC_MARGINS:
        case IDC_HEADER: case IDC_FOOTER: case IDC_PAGENUM:
            do_format_cmd(id);
            return 0;
        case IDC_FIND:
            if (g_docmode) { doc_find(0); return 0; }
            do_find(0);
            return 0;
        case IDC_FINDNEXT:
            if (g_docmode) { doc_find(1); return 0; }
            if (g_hitCount > 0) {
                g_hitIndex = (g_hitIndex + 1) % g_hitCount;
                scroll_to_hit();
                InvalidateRect(g_view, NULL, FALSE);
                status(L"Вхождение %d из %d", g_hitIndex + 1, g_hitCount);
            } else do_find(1);
            return 0;
        case IDC_PREV: go_page(g_page - 1); return 0;
        case IDC_NEXT: go_page(g_page + 1); return 0;
        case IDC_ZOOMIN:
            if (g_open) {
                g_zoom = g_zoom * 1.25f;
                if (g_zoom > ZOOM_MAX) g_zoom = ZOOM_MAX;
                render_page(); update_scrollbars();
                InvalidateRect(g_view, NULL, FALSE);
                status(L"Масштаб: %d%%", (int)(g_zoom * 100));
            }
            return 0;
        case IDC_ZOOMOUT:
            if (g_open) {
                g_zoom = g_zoom / 1.25f;
                if (g_zoom < ZOOM_MIN) g_zoom = ZOOM_MIN;
                render_page(); update_scrollbars();
                InvalidateRect(g_view, NULL, FALSE);
                status(L"Масштаб: %d%%", (int)(g_zoom * 100));
            }
            return 0;
        case IDC_FITW:
            if (g_open) {
                fit_width();
                render_page(); update_scrollbars();
                InvalidateRect(g_view, NULL, FALSE);
                status(L"По ширине: %d%%", (int)(g_zoom * 100));
            }
            return 0;
        case IDC_ZOOM100:
            if (g_open) {
                g_zoom = 1.0f;
                render_page(); update_scrollbars();
                InvalidateRect(g_view, NULL, FALSE);
                status(L"Масштаб: 100%%");
            }
            return 0;
        case IDC_ROTRIGHT: do_page_rotate(90); return 0;
        case IDC_ROTLEFT: do_page_rotate(-90); return 0;
        case IDC_PAGEDUP: do_page_duplicate(); return 0;
        case IDC_PAGEDEL: do_page_delete(); return 0;
        case IDC_PAGEUP: do_page_move(-1); return 0;
        case IDC_PAGEDOWN: do_page_move(1); return 0;
        case IDC_VIEW1:     set_view(0); return 0;
        case IDC_VIEWCONT:  set_view(1); return 0;
        case IDC_VIEW2:     set_view(2); return 0;
        case IDC_VIEWMULTI: set_view(3); return 0;
        case IDC_SETDEFAULT:
            if (assoc_register()) {
                status(L"PDF Студия зарегистрирована как программа для PDF");
                MessageBoxW(g_main,
                    L"Готово. PDF Студия прописана как программа для PDF-файлов.\n\n"
                    L"Если Windows всё ещё открывает PDF другой программой:\n"
                    L"свойства файла → «Открыть с помощью» → «Выбрать программу» → "
                    L"PDF Студия → «Использовать всегда».",
                    L"Программа по умолчанию", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(g_main, L"Не удалось записать настройки в реестр.",
                            L"Программа по умолчанию", MB_OK | MB_ICONWARNING);
            }
            return 0;
        case IDC_TOOLSPANEL:
            g_tools = !g_tools;
            wui_set_tools(g_tools);
            layout(h);
            InvalidateRect(h, NULL, TRUE);
            InvalidateRect(g_view, NULL, FALSE);
            return 0;
        case IDC_PAGENO:
            do_goto_page();
            return 0;
        case IDC_EDITTEXT:
            select_tool(TOOL_EDIT);
            status(L"Правка текста: щёлкните по тексту на странице");
            return 0;
        case IDC_OCR: do_ocr(0); return 0;
        case IDC_OCRALL: do_ocr(1); return 0;
        case IDC_FMT_UNDO:
            if (g_docmode) { flow_vkey('Z', 1, 0); fmt_after(); }
            else status(L"Отмена правок доступна в режиме документа");
            return 0;
        case IDC_FMT_REDO:
            if (g_docmode) { flow_vkey('Y', 1, 0); fmt_after(); }
            else status(L"Повтор доступен в режиме документа");
            return 0;
        case IDC_TEXTUNDO:
            if (te_edit_count() > 0) {
                te_edit_clear();
                InvalidateRect(g_view, NULL, FALSE);
                status(L"Все несохранённые правки текста отменены");
            } else status(L"Нечего отменять");
            return 0;
        case IDC_STATUS + 1:
            MessageBoxW(h,
                L"PDF Студия — бесплатный редактор и читатель PDF.\n\n"
                L"Нативное приложение на C + MuPDF: без браузера, без .NET и Java,\n"
                L"один файл, работает на Windows XP…11.\n"
                L"Оформление — Windows XP (Luna).\n\n"
                L"Режим документа: текст течёт как в Word, панель форматирования\n"
                L"(полужирный, курсив, подчёркивание, кегль, цвет, выравнивание,\n"
                L"списки, интервалы, колонтитулы, номера страниц).\n\n"
                L"Сборка: " BUILD_DATE ", режим документа с оформлением.\n"
                L"Правка текста и OCR (tesseract, rus+eng) встроены в exe.\n\n"
                L"Рендеринг: MuPDF (AGPL-3.0). Код приложения: AGPL-3.0.",
                L"О программе", MB_ICONINFORMATION);
            return 0;
        default:
            if (id >= IDC_FMT_COLOR && id <= IDC_FMT_COLOR + 5) { do_format_cmd(id); return 0; }
            if (id == IDC_INSERT_TABLE) { do_insert_table(); return 0; }
            if (id == IDC_TABLE_ROW || id == IDC_TABLE_COL || id == IDC_TABLE_DEL) {
                do_table_cmd(id); return 0;
            }
            if (id == IDC_INSERT_IMG) { do_insert_image(); return 0; }
            if (id == IDC_FONTMENU) { do_font_menu(); return 0; }
            if (id >= IDC_FONT_BASE && id < IDC_FONT_BASE + 4) {
                flow_fmt_font(id - IDC_FONT_BASE);
                fmt_after();
                status(L"Шрифт: %s", flow_font_name(id - IDC_FONT_BASE));
                return 0;
            }
            if (id >= IDC_TOOLBASE && id < IDC_TOOLBASE + TOOL_COUNT)
                select_tool(id - IDC_TOOLBASE);
            return 0;
        }
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        wchar_t file[MAX_PATH];
        if (DragQueryFileW(drop, 0, file, MAX_PATH)) do_open_file(file);
        DragFinish(drop);
        return 0;
    }

    case WM_CLOSE:
        if (g_open && g_doc.dirty) {
            int r = MessageBoxW(h, L"Есть несохранённые правки. Сохранить?",
                                L"PDF Студия", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) do_save(0);
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        free_page_bitmap();
        pdfdoc_close(&g_doc);
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- самотест без GUI ---------- */

static void fill_rect_c(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, r, br);
    DeleteObject(br);
}

/* ---------- отладочный снимок окна без X/GUI: --shot out.bmp [pdf] [tab] [backstage] ---------- */
static int write_bmp(const char *path, int w, int h, const unsigned char *bits, int stride)
{
    FILE *f = fopen(path, "wb");
    unsigned char hdr[54];
    int y;
    if (!f) return 0;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    {
        unsigned int size = 54 + (unsigned int)stride * h;
        memcpy(hdr + 2, &size, 4);
        size = 54;
        memcpy(hdr + 10, &size, 4);
        size = 40;
        memcpy(hdr + 14, &size, 4);
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1;
        hdr[28] = 32;
        size = (unsigned int)stride * h;
        memcpy(hdr + 34, &size, 4);
    }
    fwrite(hdr, 1, 54, f);
    for (y = h - 1; y >= 0; y--) fwrite(bits + (size_t)y * stride, 1, stride, f);
    fclose(f);
    return 1;
}

static int shot_main(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *outPath;
    const wchar_t *pdf = NULL;
    int tab = 0, backstage = 0;
    int W = 1180, H = 860;
    BITMAPINFO bi;
    HDC mem;
    HBITMAP dib;
    void *bits = NULL;
    char outA[MAX_PATH * 3];
    LOGFONTW lf;

    if (argc < 3) return 9;
    outPath = argv[2];
    if (argc > 3 && argv[3][0]) pdf = argv[3];
    if (argc > 4) tab = _wtoi(argv[4]);
    if (argc > 5) backstage = _wtoi(argv[5]);

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -12;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    g_font = CreateFontIndirectW(&lf);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    wui_init(NULL, g_inst, CLEARTYPE_QUALITY);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;                 /* сверху вниз */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    mem = CreateCompatibleDC(NULL);
    dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) return 8;
    SelectObject(mem, dib);

    g_tools = backstage;                 /* в снимке: 4-й аргумент — правая панель */
    wui_set_tools(g_tools);
    (void)tab;

    if (pdf && pdf[0]) {
        if (pdfdoc_open(&g_doc, pdf) == PDF_OK) {
            g_open = 1;
            g_pages = pdfdoc_pages(&g_doc);
            g_page = 0;
            lstrcpynW(g_path, pdf, MAX_PATH);
            te_set_doc(&g_doc);
            g_zoom = 0.85f;
            render_page();
        }
    }

    wui_set_open(g_open);
    wui_set_page(g_open ? g_page : 0, g_open ? g_pages : 0);
    wui_set_zoom(g_zoom);
    wui_set_tool(g_tool);
    wui_set_title(g_open ? L"demo.pdf — PDF Студия" : L"PDF Студия");
    wui_set_status(g_open ? L"Открыт: demo.pdf — страниц: 1" : L"Откройте PDF (Ctrl+O)");

    wui_layout(W, H);
    {
        RECT vr = wui_view_rect();
        int pw = g_hasBmp ? g_bmpW : 0;
        int px = vr.left + (vr.right - vr.left - pw) / 2;
        wui_set_ruler(px, pw);
    }
    wui_paint(mem, W, H);

    /* страница внутрь области просмотра */
    {
        RECT vr = wui_view_rect();
        fill_rect_c(mem, &vr, RGB(118, 118, 118));
    }
    if (g_hasBmp) {
        RECT vr = wui_view_rect();
        HDC src = CreateCompatibleDC(mem);
        HGDIOBJ old = SelectObject(src, g_bmp);
        int ox = vr.left + (vr.right - vr.left - g_bmpW) / 2;
        int oy = vr.top + 12;
        {
            RECT sh;
            sh.left = ox + 4; sh.top = oy + g_bmpH + 1;
            sh.right = ox + g_bmpW + 4; sh.bottom = oy + g_bmpH + 4;
            fill_rect_c(mem, &sh, RGB(96, 96, 96));
        }
        HRGN clip = CreateRectRgn(vr.left, vr.top, vr.right, vr.bottom);
        SelectClipRgn(mem, clip);
        BitBlt(mem, ox, oy, g_bmpW, g_bmpH, src, 0, 0, SRCCOPY);
        SelectClipRgn(mem, NULL);
        DeleteObject(clip);
        SelectObject(src, old);
        DeleteDC(src);
    }

    WideCharToMultiByte(CP_ACP, 0, outPath, -1, outA, sizeof(outA), NULL, NULL);
    write_bmp(outA, W, H, (const unsigned char *)bits, W * 4);

    if (g_open) pdfdoc_close(&g_doc);
    ReleaseDC(NULL, mem);
    printf("shot: %s %dx%d\n", outA, W, H);
    return 0;
}

static int selftest_main(void)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    PdfDoc d;
    int rc = 9;

    if (!argv || argc < 4) { dbg("selftest: argc=%d", argc); return 9; }
    memset(&d, 0, sizeof(d));
    if (!lstrcmpiW(argv[1], L"--dumpnotext")) {
        /* отладка: сохранить «страницу без текста» в PPM */
        HBITMAP hb = NULL;
        int w = 0, h = 0, rc = pdfdoc_open(&d, argv[2]);
        dbg("dumpnotext: open rc=%d", rc);
        if (rc) return 2;
        if (!pdfdoc_render_notext(&d, 0, 1.5f, &hb, &w, &h)) { dbg("dumpnotext: render fail"); return 3; }
        {
            BITMAPINFO bi; HDC dc; void *bits = NULL; int y; FILE *f;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
            dc = CreateCompatibleDC(NULL);
            bits = calloc((size_t)(((w * 3) + 3) & ~3) * h + 8, 1);
            GetDIBits(dc, hb, 0, h, bits, &bi, DIB_RGB_COLORS);
            DeleteDC(dc);
            f = _wfopen(argv[3], L"wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (y = 0; y < h; y++) {
                    int x, st = ((w * 3) + 3) & ~3;
                    unsigned char *p = (unsigned char *)bits + (size_t)y * st;
                    for (x = 0; x < w; x++) { unsigned char t = p[x*3]; p[x*3] = p[x*3+2]; p[x*3+2] = t; }
                    fwrite(p, 1, (size_t)w * 3, f);
                }
                fclose(f);
                dbg("dumpnotext: wrote %dx%d", w, h);
            }
            free(bits);
        }
        return 0;
    }
    { int orc = pdfdoc_open(&d, argv[2]); dbg("selftest: open rc=%d", orc); if (orc) return 2; }
    te_set_doc(&d);

    if (!lstrcmpiW(argv[1], L"--selftest")) {
        float px = (float)_wtof(argv[4]), py = (float)_wtof(argv[5]);
        const wchar_t *ins = argc > 6 ? argv[6] : L" EDIT";
        int i, ok, n;
        char *before, *after;

        ok = te_begin(&d, 0, px, py, 1.0f, 0, 0);
        dbg("selftest: begin=%d", ok);
        if (!ok) { pdfdoc_close(&d); return 3; }
        before = te_text_utf8();
        dbg("selftest: было=[%s]", before ? before : "");
        free(before);
        te_home_end(1, 1, 0);
        for (i = 0; ins[i]; i++) te_insert((int)ins[i]);
        after = te_text_utf8();
        dbg("selftest: стало=[%s]", after ? after : "");
        free(after);
        dbg("selftest: commit=%d", te_commit());
        n = te_apply_all(&d);
        dbg("selftest: apply=%d", n);
        rc = pdfdoc_save(&d, argv[3]) ? 0 : 4;
        dbg("selftest: save rc=%d", rc);
    } else if (!lstrcmpiW(argv[1], L"--tbltest")) {
        /* --tbltest in.pdf out.pdf — таблица, ввод в ячейки, сохранение PDF */
        int i, ok;
        if (!flow_build(&d)) { dbg("tbltest: нет текста"); pdfdoc_close(&d); return 5; }
        ok = flow_insert_table(3, 3);
        dbg("tbltest: insert=%d в_таблице=%d", ok, flow_in_table());
        {
            const char *a = "A1";
            for (i = 0; a[i]; i++) flow_char(a[i]);
        }
        flow_table_next_cell();
        { const char *b = "B1"; for (i = 0; b[i]; i++) flow_char(b[i]); }
        flow_table_next_cell();
        { const char *c = "C1"; for (i = 0; c[i]; i++) flow_char(c[i]); }
        flow_select_all();
        flow_fmt_font(1);                       /* всё выделение — Arial */
        flow_fmt_size(9.0f);
        flow_vkey(VK_RIGHT, 0, 0);               /* снять выделение */
        { int j; for (j = 0; j < 4; j++) flow_table_next_cell(); }
        flow_fmt_font(2);                        /* новые символы — Courier */
        { const char *d2 = "Mono"; for (i = 0; d2[i]; i++) flow_char(d2[i]); }
        dbg("tbltest: row_add=%d col_add=%d", flow_table_row_add(), flow_table_col_add());
        {
            int pg = 0, pr = 0, ch = 0;
            FlowFmt ff;
            flow_stats(&pg, &pr, &ch);
            flow_fmt_get(&ff);
            dbg("tbltest: стр=%d абз=%d симв=%d формат_в_каретке: size=%.1f font=%d",
                pg, pr, ch, ff.size, ff.font);
        }
        rc = flow_export(&d, argv[3]) ? 0 : 4;
        dbg("tbltest: export rc=%d", rc);
    } else if (!lstrcmpiW(argv[1], L"--fmttest")) {
        /* --fmttest in.pdf out.pdf — проверка форматирования и записи */
        int pg = 0, pr = 0, ch = 0;
        if (!flow_build(&d)) { dbg("fmttest: нет текста"); pdfdoc_close(&d); return 5; }
        flow_stats(&pg, &pr, &ch);
        dbg("fmttest: собрано стр=%d абз=%d симв=%d", pg, pr, ch);
        flow_select_all();
        flow_fmt_attr(1);                    /* жирный */
        dbg("fmttest: жирный ок");
        flow_fmt_attr(4);                    /* подчёркнутый */
        dbg("fmttest: подчёркнутый ок");
        flow_align(1);
        dbg("fmttest: по центру ок");
        flow_align(3);
        dbg("fmttest: по ширине ок");
        flow_list(1);
        dbg("fmttest: список ок");
        flow_spacing_next();
        dbg("fmttest: интервал ок");
        flow_fmt_color(0xC00000);
        dbg("fmttest: цвет ок");
        flow_set_pagenum(1);
        {
            FlowFmt fm;
            flow_fmt_get(&fm);
            dbg("fmttest: формат bold=%d ital=%d und=%d size=%.1f color=%06X align=%d list=%d sp=%.2f",
                fm.bold, fm.italic, fm.underline, fm.size, fm.color, fm.align, fm.list,
                fm.spacing);
        }
        flow_stats(&pg, &pr, &ch);
        dbg("fmttest: после форматирования стр=%d абз=%d симв=%d слов=%d",
            flow_pages(), pr, ch, flow_words());
        {
            wchar_t outw[512];
            int rc;
            lstrcpynW(outw, argv[3], 512);
            if (!lstrcmpW(outw, L"@cyr")) lstrcpyW(outw, L"Z:\\tmp\\ТЕСТ документ.pdf");
            rc = flow_export(&d, outw) ? 0 : 4;
            pdfdoc_close(&d);
            return rc;
        }
    } else if (!lstrcmpiW(argv[1], L"--flowtest")) {
        /* --flowtest in.pdf out.pdf x y "текст" — проверка режима документа */
        float px = argc > 4 ? (float)_wtof(argv[4]) : 60.0f;
        float py = argc > 5 ? (float)_wtof(argv[5]) : 60.0f;
        wchar_t insbuf[8192];
        const wchar_t *ins = argc > 6 ? argv[6] : L" ДОКУМЕНТ";
        insbuf[0] = 0;
        if (ins[0] == L'@') {          /* @файл — текст из UTF-8 файла */
            FILE *f = _wfopen(ins + 1, L"rb");
            if (f) {
                char u8[8192];
                size_t got = fread(u8, 1, sizeof(u8) - 1, f);
                fclose(f);
                u8[got] = 0;
                MultiByteToWideChar(CP_UTF8, 0, u8, -1, insbuf, 8192);
                ins = insbuf;
                dbg("flowtest: текст из файла, символов=%d", (int)wcslen(ins));
            }
        }
        int pg = 0, pr = 0, ch = 0, i, before_pages;
        char *all;

        if (!flow_build(&d)) { dbg("flowtest: нет текста — режим недоступен"); pdfdoc_close(&d); return 5; }
        flow_stats(&pg, &pr, &ch);
        dbg("flowtest: собрано страниц=%d абзацев=%d символов=%d", pg, pr, ch);
        before_pages = flow_pages();
        flow_click(0, px, py, 0);
        for (i = 0; ins[i]; i++) {
            if (ins[i] == L'\n') flow_vkey(VK_RETURN, 0, 0);
            else flow_char((int)ins[i]);
        }
        flow_stats(&pg, &pr, &ch);
        dbg("flowtest: после правки страниц=%d абзацев=%d символов=%d",
            flow_pages(), pr, ch);
        dbg("flowtest: правка вставлена");
        flow_select_all();
        dbg("flowtest: выделено всё");
        all = flow_selected_utf8();
        dbg("flowtest: текст получен");
        dbg("flowtest: начало текста=[%.300s]", all ? all : "");
        free(all);
        dbg("flowtest: экспорт начат");
        {
            wchar_t outw[512];
            lstrcpynW(outw, argv[3], 512);
            if (!lstrcmpW(outw, L"@cyr")) lstrcpyW(outw, L"Z:\\tmp\\ТЕСТ документ.pdf");
            rc = flow_export(&d, outw) ? 0 : 4;
        }
        dbg("flowtest: export rc=%d (было страниц %d, стало %d)", rc, before_pages, flow_pages());
    } else if (!lstrcmpiW(argv[1], L"--selfocr")) {
        int chars = 0;
        const char *dd = tessdata_dir();
        clock_t t0 = clock();
        int ok = dd ? pdfdoc_ocr_page(&d, 0, "rus+eng", dd, &chars) : 0;
        dbg("selfocr: datadir=%s ok=%d chars=%d время=%.1f c",
            dd ? dd : "(нет)", ok, chars, (double)(clock() - t0) / CLOCKS_PER_SEC);
        if (ok) { te_apply_all(&d); rc = pdfdoc_save(&d, argv[3]) ? 0 : 4; }
        dbg("selfocr: save rc=%d", rc);
    }
    pdfdoc_close(&d);
    return rc;
}

static void take_command_line(WCHAR *cmdline)
{
    WCHAR *p = cmdline;
    WCHAR buf[MAX_PATH];
    int n = 0;
    if (!cmdline || !cmdline[0]) return;
    if (*p == L'"') { p++; while (*p && *p != L'"' && n < MAX_PATH - 1) buf[n++] = *p++; }
    else { while (*p && *p != L' ' && n < MAX_PATH - 1) buf[n++] = *p++; }
    buf[n] = 0;
    if (n > 3 && GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES)
        do_open_file(buf);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc;
    MSG msg;

    (void)prev;
    g_inst = inst;
    g_main = NULL;
    g_view = NULL;

    {
        const wchar_t *cl = GetCommandLineW();
        if (cl && (wcsstr(cl, L"--selftest") || wcsstr(cl, L"--selfocr") || wcsstr(cl, L"--dumpnotext") || wcsstr(cl, L"--flowtest") || wcsstr(cl, L"--fmttest") || wcsstr(cl, L"--tbltest")))
            return selftest_main();
        if (cl && wcsstr(cl, L"--shot"))
            return shot_main();
    }

    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *PFN_SPDA)(void);
        if (u32) {
            PFN_SPDA f = (PFN_SPDA)GetProcAddress(u32, "SetProcessDPIAware");
            if (f) f();
        }
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"PdfStudioMain";
    wc.hIcon = LoadIconW(inst, (LPCWSTR)1);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) return 1;

    {
        static ACCEL accel[] = {
            { FVIRTKEY | FCONTROL, 'O', IDC_OPEN },
            { FVIRTKEY | FCONTROL, 'S', IDC_SAVE },
            { FVIRTKEY | FCONTROL, 'P', IDC_PRINT },
            { FVIRTKEY | FCONTROL, 'F', IDC_FIND },
            { FVIRTKEY | FCONTROL, 'H', IDC_FINDREPL },
            { FVIRTKEY, VK_F2, IDC_EDITTEXT },
            { FVIRTKEY, VK_F3, IDC_FINDNEXT },
            { FVIRTKEY, VK_PRIOR, IDC_PREV },
            { FVIRTKEY, VK_NEXT, IDC_NEXT },
            { FVIRTKEY, VK_ADD, IDC_ZOOMIN },
            { FVIRTKEY, VK_OEM_PLUS, IDC_ZOOMIN },
            { FVIRTKEY, VK_SUBTRACT, IDC_ZOOMOUT },
            { FVIRTKEY, VK_OEM_MINUS, IDC_ZOOMOUT },
            { FVIRTKEY, '0', IDC_FITW },
            { FVIRTKEY | FCONTROL, 'R', IDC_ROTRIGHT },
            { FVIRTKEY | FCONTROL | FSHIFT, 'R', IDC_ROTLEFT },
            { FVIRTKEY | FCONTROL, 'D', IDC_PAGEDUP },
            { FVIRTKEY | FCONTROL, 'A', IDC_COPYTEXT },
            { FVIRTKEY | FCONTROL | FSHIFT, 'D', IDC_DOCMODE },
            { FVIRTKEY | FCONTROL, '1', IDC_VIEW1 },
            { FVIRTKEY | FCONTROL, '2', IDC_VIEW2 },
            { FVIRTKEY | FCONTROL, '3', IDC_VIEWMULTI },
            { FVIRTKEY | FCONTROL, '4', IDC_VIEWCONT },
        };
        g_accel = CreateAcceleratorTableW(accel, sizeof(accel) / sizeof(accel[0]));
    }
    {
        /* в режиме документа часть Ctrl-клавиш занята форматированием
           (Ctrl+B/I/U, Ctrl+L/E/R/J, Ctrl+A, Ctrl+Z/Y, Ctrl+F/H) — их не
           перехватываем таблицей, пусть доходят до окна документа */
        static ACCEL accelDoc[] = {
            { FVIRTKEY | FCONTROL, 'O', IDC_OPEN },
            { FVIRTKEY | FCONTROL, 'S', IDC_SAVE },
            { FVIRTKEY | FCONTROL, 'P', IDC_PRINT },
            { FVIRTKEY | FCONTROL, 'F', IDC_FIND },
            { FVIRTKEY | FCONTROL, 'H', IDC_FINDREPL },
            { FVIRTKEY | FCONTROL | FSHIFT, 'D', IDC_DOCMODE },
            { FVIRTKEY, VK_F3, IDC_FINDNEXT },
            { FVIRTKEY, VK_PRIOR, IDC_PREV },
            { FVIRTKEY, VK_NEXT, IDC_NEXT },
            { FVIRTKEY | FCONTROL, 'T', IDC_INSERT_TABLE },
            { FVIRTKEY | FCONTROL, 'M', IDC_INSERT_IMG }
        };
        g_accelDoc = CreateAcceleratorTableW(accelDoc,
                                            sizeof(accelDoc) / sizeof(accelDoc[0]));
    }

    g_main = CreateWindowExW(WS_EX_APPWINDOW, L"PdfStudioMain", L"PDF Студия",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX |
                             WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 860,
                             NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    ShowWindow(g_main, show ? show : SW_SHOW);
    UpdateWindow(g_main);
    take_command_line(cmdline);
    te_set_bg_provider(te_bg_sample);
    assoc_first_run_prompt();

    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!(g_docmode ? g_accelDoc : g_accel) ||
            !TranslateAcceleratorW(g_main, g_docmode ? g_accelDoc : g_accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
