/* wui.c — оформление в стиле Windows XP (Luna), целиком на GDI.
 *
 * Состав: синий заголовок с кнопками окна, строка меню (рабочие ниспадающие
 * меню), панель инструментов с иконками, линейка, левая панель-рейка,
 * правая панель «Инструменты», строка состояния.
 *
 * Всё рисуется в память (двойная буферизация) и выводится одним BitBlt —
 * поэтому при движении мыши ничего не мерцает.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "cmds.h"
#include "wui.h"

/* ---------- метрики ---------- */
#define CAP_H      28
#define MENU_H     22
#define TB_H       34
#define FMT_H      28
#define RULER_H    22
#define STAT_H     22
#define RAIL_W     28
#define TOOLS_W    196

/* ---------- цвета XP Luna ---------- */
#define CAP_L      RGB(  0, 84,227)
#define CAP_R      RGB( 58,147,255)
#define CAP_TX     RGB(255,255,255)
#define CAP_BRD    RGB(  0, 60,180)
#define WBTN_FACE  RGB( 62,130,229)
#define WBTN_HOT   RGB( 92,160,245)
#define WBTN_CLOSE RGB(224, 72, 52)
#define XPBG       RGB(236,233,216)
#define XPBG_LT    RGB(250,249,244)
#define XPBG_DK    RGB(214,209,194)
#define XPLINE     RGB(172,168,153)
#define XPLINE_LT  RGB(255,255,255)
#define XPTX       RGB(  0,  0,  0)
#define XPGRAY     RGB(109,109,109)
#define XPDIS      RGB(167,163,148)
#define XPSEL      RGB( 49,106,197)
#define XPSEL_TX   RGB(255,255,255)
#define TBHOT      RGB(253,216,131)
#define TBHOT_BRD  RGB(196,168,101)
#define RUL_BG     RGB(255,255,255)
#define RUL_OUT    RGB(236,233,216)
#define RUL_TICK   RGB(120,120,120)
#define PAGE_BG    RGB(118,118,118)
#define STAT_BG    RGB(236,233,216)

/* ---------- идентификаторы иконок ---------- */
enum {
    IC_NONE = 0,
    IC_OPEN, IC_SAVE, IC_PRINT, IC_COPY, IC_SELECT, IC_EDIT, IC_SEARCH, IC_NEXT,
    IC_PREV, IC_PEN, IC_HIGHLIGHT, IC_RECT, IC_TEXT, IC_NOTE, IC_ERASE,
    IC_ROTL, IC_ROTR, IC_DUP, IC_DEL, IC_UP, IC_DOWN, IC_OCR, IC_OCRALL,
    IC_UNDO, IC_ZOOMIN, IC_ZOOMOUT, IC_FITW, IC_ZOOM100,
    IC_WMIN, IC_WMAX, IC_WRESTORE, IC_WCLOSE,
    IC_VIEWPAGE, IC_VIEWFIT, IC_TOOLS, IC_MORE,
    IC_GRID2, IC_GRID4, IC_CONT
};

/* ---------- пункты меню ---------- */
#define MI_SEP      1
#define MI_ALWAYS   2      /* доступно без открытого документа */
#define MI_CHECKED  4      /* «галочка» — панель инструментов */

typedef struct {
    const wchar_t *label;
    int            cmd;
    const wchar_t *accel;
    int            flags;
} MItem;

static const MItem m_file[] = {
    { L"Открыть",        IDC_OPEN,  L"Ctrl+O", MI_ALWAYS },
    { L"Сохранить",      IDC_SAVE,  L"Ctrl+S", 0 },
    { L"Сохранить как…", IDC_SAVEAS, NULL,     0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Печать…",        IDC_PRINT, L"Ctrl+P", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Выход",          IDC_QUIT,  NULL,     MI_ALWAYS }
};
static const MItem m_edit[] = {
    { L"Отменить",            IDC_FMT_UNDO, L"Ctrl+Z", 0 },
    { L"Повторить",           IDC_FMT_REDO, L"Ctrl+Y", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Вырезать",            IDC_DOCCUT,   L"Ctrl+X", 0 },
    { L"Копировать",          IDC_DOCCOPY,  L"Ctrl+C", 0 },
    { L"Вставить",            IDC_DOCPASTE, L"Ctrl+V", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Найти…",              IDC_FIND,     L"Ctrl+F", 0 },
    { L"Найти далее",         IDC_FINDNEXT, L"F3",     0 },
    { L"Найти и заменить…",   IDC_FINDREPL, L"Ctrl+H", MI_ALWAYS },
    { NULL, 0, NULL, MI_SEP },
    { L"Копировать текст страницы", IDC_COPYTEXT, NULL, 0 },
    { L"Отменить правки текста",    IDC_TEXTUNDO, NULL, 0 }
};

static const MItem m_format[] = {
    { L"Режим документа (текст как в Word)", IDC_DOCMODE, L"Ctrl+Shift+D",
      MI_CHECKED | MI_ALWAYS },
    { NULL, 0, NULL, MI_SEP },
    { L"Шрифт: Times New Roman", IDC_FONT_BASE + 0, NULL, MI_CHECKED },
    { L"Шрифт: Arial",           IDC_FONT_BASE + 1, NULL, MI_CHECKED },
    { L"Шрифт: Courier New",     IDC_FONT_BASE + 2, NULL, MI_CHECKED },
    { L"Шрифт: Arial Narrow",    IDC_FONT_BASE + 3, NULL, MI_CHECKED },
    { NULL, 0, NULL, MI_SEP },
    { L"Полужирный",   IDC_FMT_B, L"Ctrl+B", 0 },
    { L"Курсив",       IDC_FMT_I, L"Ctrl+I", 0 },
    { L"Подчёркнутый", IDC_FMT_U, L"Ctrl+U", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"По левому краю", IDC_ALIGN_L, L"Ctrl+L", 0 },
    { L"По центру",      IDC_ALIGN_C, L"Ctrl+E", 0 },
    { L"По правому краю", IDC_ALIGN_R, L"Ctrl+R", 0 },
    { L"По ширине",      IDC_ALIGN_J, L"Ctrl+J", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Маркированный список", IDC_LIST_B, NULL, 0 },
    { L"Нумерованный список",  IDC_LIST_N, NULL, 0 },
    { L"Убрать список",        IDC_LIST_NONE, NULL, 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Кегль больше",  IDC_FMT_BIGGER,  NULL, 0 },
    { L"Кегль меньше",  IDC_FMT_SMALLER, NULL, 0 },
    { L"Межстрочный интервал", IDC_SPACING, NULL, 0 },
    { L"Красная строка +", IDC_INDENTP, NULL, 0 },
    { L"Красная строка −", IDC_INDENTM, NULL, 0 },
    { L"Интервал перед абзацем", IDC_SPACEBEFORE, NULL, 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Верхний колонтитул…", IDC_HEADER,  NULL, 0 },
    { L"Нижний колонтитул…",  IDC_FOOTER,  NULL, 0 },
    { L"Номера страниц",      IDC_PAGENUM, NULL, MI_CHECKED },
    { NULL, 0, NULL, MI_SEP },
    { L"Страница A4",     IDC_PAGE_A4,     NULL, 0 },
    { L"Страница A5",     IDC_PAGE_A5,     NULL, 0 },
    { L"Страница Letter", IDC_PAGE_LETTER, NULL, 0 },
    { L"Книжная",         IDC_PORTR,       NULL, 0 },
    { L"Альбомная",       IDC_LANDR,       NULL, 0 },
    { L"Поля 20 мм…",     IDC_MARGINS,     NULL, 0 }
};
static const MItem m_view[] = {
    { L"По ширине",     IDC_FITW,    L"0",  0 },
    { L"Масштаб 100 %", IDC_ZOOM100, NULL,  0 },
    { L"Увеличить",     IDC_ZOOMIN,  L"+",  0 },
    { L"Уменьшить",     IDC_ZOOMOUT, L"−",  0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Режим документа (текст как в Word)", IDC_DOCMODE, L"Ctrl+Shift+D", MI_CHECKED },
    { NULL, 0, NULL, MI_SEP },
    { L"Панель инструментов", IDC_TOOLSPANEL, NULL, MI_ALWAYS | MI_CHECKED }
};
static const MItem m_doc[] = {
    { L"Вставить таблицу 3×3", IDC_INSERT_TABLE, L"Ctrl+T", 0 },
    { L"Добавить строку",      IDC_TABLE_ROW,    NULL, 0 },
    { L"Добавить столбец",     IDC_TABLE_COL,    NULL, 0 },
    { L"Удалить таблицу",      IDC_TABLE_DEL,    NULL, 0 },
    { L"Вставить картинку…",   IDC_INSERT_IMG,   NULL, MI_ALWAYS },
    { NULL, 0, NULL, MI_SEP },
    { L"Повернуть влево",  IDC_ROTLEFT,  L"Ctrl+Shift+R", 0 },
    { L"Повернуть вправо", IDC_ROTRIGHT, L"Ctrl+R",       0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Дублировать страницу", IDC_PAGEDUP, L"Ctrl+D", 0 },
    { L"Удалить страницу",     IDC_PAGEDEL, NULL,      0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Переместить выше", IDC_PAGEUP,   NULL, 0 },
    { L"Переместить ниже", IDC_PAGEDOWN, NULL, 0 }
};
static const MItem m_tools[] = {
    { L"Выбор",         IDC_TOOLBASE + TOOL_SELECT,    NULL, 0 },
    { L"Правка текста", IDC_EDITTEXT,                  L"F2", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Маркер",        IDC_TOOLBASE + TOOL_HIGHLIGHT, L"H", 0 },
    { L"Рамка",         IDC_TOOLBASE + TOOL_RECT,      L"R", 0 },
    { L"Заметка",       IDC_TOOLBASE + TOOL_NOTE,      L"N", 0 },
    { L"Перо",          IDC_TOOLBASE + TOOL_INK,       L"I", 0 },
    { L"Ластик",        IDC_TOOLBASE + TOOL_ERASE,     L"E", 0 },
    { NULL, 0, NULL, MI_SEP },
    { L"Распознать страницу",     IDC_OCR,    NULL, 0 },
    { L"Распознать весь документ", IDC_OCRALL, NULL, 0 }
};
static const MItem m_help[] = {
    { L"Сделать программой по умолчанию", IDC_SETDEFAULT, NULL, MI_ALWAYS },
    { NULL, 0, NULL, MI_SEP },
    { L"О программе", IDC_STATUS + 1, NULL, MI_ALWAYS }
};

typedef struct {
    const wchar_t *title;
    const MItem   *items;
    int            n;
} Menu;

static const Menu g_menus[] = {
    { L"Файл",        m_file,  (int)(sizeof(m_file)/sizeof(m_file[0])) },
    { L"Правка",      m_edit,  (int)(sizeof(m_edit)/sizeof(m_edit[0])) },
    { L"Вид",         m_view,  (int)(sizeof(m_view)/sizeof(m_view[0])) },
    { L"Формат",      m_format, (int)(sizeof(m_format)/sizeof(m_format[0])) },
    { L"Документ",    m_doc,   (int)(sizeof(m_doc)/sizeof(m_doc[0])) },
    { L"Инструменты", m_tools, (int)(sizeof(m_tools)/sizeof(m_tools[0])) },
    { L"Справка",     m_help,  (int)(sizeof(m_help)/sizeof(m_help[0])) }
};
#define MENU_COUNT ((int)(sizeof(g_menus)/sizeof(g_menus[0])))

/* ---------- элементы горячих областей ---------- */
typedef struct {
    RECT r;
    int  kind;        /* 1 — панель, 2 — меню, 3 — статус, 4 — кнопка окна,
                         5 — панель инструментов (справа), 6 — рейка, 7 — прочее,
                         8 — кнопка панели форматирования, 9 — образец цвета,
                         10 — надпись на панели форматирования */
    int  aux;         /* номер меню / команда / номер кнопки окна */
    int  cmd;
    int  icon;
    const wchar_t *tip;
    const wchar_t *label;   /* надпись на панели форматирования */
    unsigned color;         /* цвет образца */
} Hot;

#define MAX_HOT 340
static Hot g_hot[MAX_HOT];
static int g_hotN;
static int g_hover = -1, g_press = -1;

/* ---------- состояние ---------- */
static wchar_t g_title[512] = L"PDF Студия";
static wchar_t g_status[512] = L"";
static int  g_open, g_dirty, g_docmode;
static int  g_page, g_pages;
static float g_zoom = 1.0f;
static int  g_tool;
static int  g_cw, g_ch;
static int  g_maximized;
static int  g_tools = 0;              /* правая панель */
static int  g_menuActive = -1;        /* открытое меню */
static int  g_zoomDrag, g_zoomPending;
static int  g_repaint;
static int  g_pageX, g_pageW;         /* линейка */
static int  g_caretX = -1;
static int  g_mouseX = -1, g_mouseY = -1;
static int  g_rulerHot;
static int  g_repaintRuler;
static int  g_viewMode;         /* 0 одна, 1 непрерывно, 2 две, 3 несколько */

/* формат в каретке — панель форматирования показывает его состояние */
static int      g_fmtB, g_fmtI, g_fmtU;
static float    g_fmtSize = 11.0f;
static wchar_t  g_fontShort[24] = L"Times";
static unsigned g_fmtColor;
static int      g_fmtAlign, g_fmtList;
static float    g_fmtSpacing = 1.0f;
static int      g_pgnum;

static HWND      g_main;
static HINSTANCE g_inst;
static HFONT     f_ui, f_bold, f_small, f_title;
static HBRUSH    b_bg;

static RECT g_searchRect, g_pageRect;

/* ---------- мелкая графика ---------- */
static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT x;
    HBRUSH br = CreateSolidBrush(c);
    SetRect(&x, l, t, r, b);
    FillRect(dc, &x, br);
    DeleteObject(br);
}

static void hline(HDC dc, int x1, int x2, int y, COLORREF c)
{
    fill(dc, x1, y, x2, y + 1, c);
}

static void vline(HDC dc, int x, int y1, int y2, COLORREF c)
{
    fill(dc, x, y1, x + 1, y2, c);
}

static void grad_v(HDC dc, int l, int t, int r, int b, COLORREF c1, COLORREF c2)
{
    int y;
    int h = b - t;
    if (h <= 0) return;
    for (y = 0; y < h; y++) {
        int k = (h > 1) ? (y * 255 / (h - 1)) : 0;
        COLORREF c = RGB(GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * k / 255,
                         GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * k / 255,
                         GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * k / 255);
        hline(dc, l, r, t + y, c);
    }
}

static void grad_h(HDC dc, int l, int t, int r, int b, COLORREF c1, COLORREF c2)
{
    int x;
    int w = r - l;
    if (w <= 0) return;
    for (x = 0; x < w; x++) {
        int k = (w > 1) ? (x * 255 / (w - 1)) : 0;
        COLORREF c = RGB(GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * k / 255,
                         GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * k / 255,
                         GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * k / 255);
        vline(dc, l + x, t, b, c);
    }
}

static void round_rect(HDC dc, int l, int t, int r, int b, int rad, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, p);
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(p);
}

static void frame_rect_c(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, l, t, r, b);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(p);
}

static int text_w(HDC dc, HFONT f, const wchar_t *s)
{
    SIZE sz;
    HGDIOBJ old = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static void text_at(HDC dc, HFONT f, COLORREF c, const wchar_t *s, RECT r, UINT fmt)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
}

/* ---------- векторные иконки ---------- */
static void p_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int wd)
{
    HPEN p = CreatePen(PS_SOLID, wd, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void p_rect(HDC dc, int l, int t, int r, int b, COLORREF c, int fillit, int wd)
{
    HPEN p = CreatePen(PS_SOLID, wd, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob;
    if (fillit) {
        HBRUSH br = CreateSolidBrush(c);
        ob = SelectObject(dc, br);
        Rectangle(dc, l, t, r, b);
        SelectObject(dc, ob);
        DeleteObject(br);
    } else {
        ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, l, t, r, b);
        SelectObject(dc, ob);
    }
    SelectObject(dc, op);
    DeleteObject(p);
}

static void p_poly(HDC dc, const POINT *pt, int n, COLORREF c, int fillit)
{
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob;
    if (fillit) {
        HBRUSH br = CreateSolidBrush(c);
        ob = SelectObject(dc, br);
        Polygon(dc, pt, n);
        SelectObject(dc, ob);
        DeleteObject(br);
    } else {
        ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Polyline(dc, pt, n);
        SelectObject(dc, ob);
    }
    SelectObject(dc, op);
    DeleteObject(p);
}

static void p_circle(HDC dc, int cx, int cy, int r, COLORREF c, int fillit, int wd)
{
    HPEN p = CreatePen(PS_SOLID, wd, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob;
    if (fillit) {
        HBRUSH br = CreateSolidBrush(c);
        ob = SelectObject(dc, br);
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, ob);
        DeleteObject(br);
    } else {
        ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, ob);
    }
    SelectObject(dc, op);
    DeleteObject(p);
}

static void icon_geometry(HDC dc, int id, int small)
{
    COLORREF blue  = RGB( 40, 90,170);
    COLORREF lblue = RGB( 90,150,235);
    COLORREF gray  = small ? RGB(110,110,110) : RGB(90, 90, 90);
    COLORREF dgray = RGB(120,120,120);
    COLORREF yellow= RGB(255,205, 60);
    COLORREF orange= RGB(235,125, 45);
    COLORREF green = RGB( 90,170, 90);
    COLORREF red   = RGB(205, 70, 60);
    COLORREF ink   = RGB( 40, 40, 40);

    switch (id) {
    case IC_OPEN:                       /* жёлтая папка */
        p_rect(dc, 1, 4, 15, 14, yellow, 1, 1);            /* корпус */
        p_rect(dc, 1, 2, 7, 5, yellow, 1, 1);              /* язычок */
        p_line(dc, 1, 4, 15, 4, RGB(214,162,32), 1);
        p_rect(dc, 1, 4, 15, 14, RGB(190,140,20), 0, 1);   /* контур */
        p_rect(dc, 2, 7, 14, 13, RGB(255,233,150), 1, 1);  /* передняя створка */
        p_rect(dc, 2, 7, 14, 13, RGB(214,162,32), 0, 1);
        break;
    case IC_SAVE:                       /* дискета */
        p_rect(dc, 1, 1, 15, 15, RGB(64,104,172), 1, 1);
        p_rect(dc, 1, 1, 15, 15, RGB(38,72,132), 0, 1);
        p_rect(dc, 4, 1, 12, 6, RGB(232,235,242), 1, 1);   /* шторка */
        p_rect(dc, 4, 1, 12, 6, RGB(150,158,175), 0, 1);
        p_rect(dc, 8, 2, 10, 5, RGB(90,100,120), 1, 1);
        p_rect(dc, 3, 8, 13, 15, RGB(246,247,250), 1, 1);  /* этикетка */
        p_rect(dc, 3, 8, 13, 15, RGB(150,158,175), 0, 1);
        p_line(dc, 5, 11, 11, 11, RGB(120,130,150), 1);
        p_line(dc, 5, 13, 11, 13, RGB(120,130,150), 1);
        break;
    case IC_PRINT:                      /* принтер */
        p_rect(dc, 4, 1, 12, 5, RGB(250,250,252), 1, 1);   /* лист сверху */
        p_rect(dc, 4, 1, 12, 5, RGB(140,145,155), 0, 1);
        p_rect(dc, 1, 5, 15, 11, RGB(150,155,165), 1, 1);  /* корпус */
        p_rect(dc, 1, 5, 15, 11, RGB(105,110,120), 0, 1);
        p_rect(dc, 4, 9, 12, 15, RGB(252,252,253), 1, 1);  /* выходящий лист */
        p_rect(dc, 4, 9, 12, 15, RGB(140,145,155), 0, 1);
        p_line(dc, 5, 12, 11, 12, RGB(160,165,175), 1);
        p_line(dc, 3, 7, 4, 7, RGB(90,190,90), 1);         /* индикатор */
        break;
    case IC_COPY:
        p_rect(dc, 2, 2, 10, 12, gray, 0, 1);
        p_rect(dc, 5, 4, 13, 14, blue, 0, 1);
        break;
    case IC_SELECT: {
        POINT p[7] = { {3,1},{3,13},{6,10},{8,15},{10,14},{8,9},{12,9} };
        p_poly(dc, p, 7, ink, 1);
        break;
    }
    case IC_EDIT:                       /* страница с карандашом */
        p_rect(dc, 1, 1, 10, 14, RGB(252, 252, 253), 1, 1);
        p_rect(dc, 1, 1, 10, 14, RGB(150, 155, 170), 0, 1);
        p_line(dc, 3, 4, 8, 4, RGB(150, 155, 170), 1);
        p_line(dc, 3, 6, 8, 6, RGB(150, 155, 170), 1);
        p_line(dc, 3, 8, 6, 8, RGB(150, 155, 170), 1);
        p_line(dc, 8, 13, 14, 6, RGB(240, 170, 40), 2);
        p_line(dc, 14, 6, 15, 4, RGB(90, 90, 100), 1);
        break;
    case IC_SEARCH:
        p_circle(dc, 6, 6, 4, blue, 0, 2);
        p_line(dc, 9, 9, 14, 14, blue, 2);
        break;
    case IC_NEXT: {
        POINT p[3] = { {5,3},{12,8},{5,13} };
        p_poly(dc, p, 3, green, 1);
        break;
    }
    case IC_PREV: {
        POINT p[3] = { {11,3},{4,8},{11,13} };
        p_poly(dc, p, 3, green, 1);
        break;
    }
    case IC_PEN:
        p_line(dc, 2, 14, 11, 5, RGB(190,60,60), 2);
        p_line(dc, 11, 5, 14, 2, orange, 2);
        p_poly(dc, (POINT[]){{2,14},{5,15},{1,15}}, 3, ink, 1);
        break;
    case IC_HIGHLIGHT: {                /* маркер под углом */
        POINT body[4] = { {9, 1}, {14, 4}, {8, 11}, {4, 8} };
        POINT nib[4]  = { {4, 8}, {8, 11}, {5, 14}, {1, 12} };
        POINT tip[3]  = { {1, 12}, {5, 14}, {2, 15} };
        p_poly(dc, body, 4, RGB(250, 205, 60), 1);
        p_poly(dc, nib, 4, RGB(255, 232, 140), 1);
        p_poly(dc, tip, 3, RGB(120, 95, 45), 1);
        p_line(dc, 1, 15, 15, 15, RGB(255, 214, 60), 1);
        break;
    }
    case IC_RECT:
        p_rect(dc, 2, 4, 14, 13, blue, 0, 2);
        break;
    case IC_TEXT:
        p_line(dc, 2, 3, 14, 3, blue, 2);
        p_line(dc, 8, 3, 8, 14, blue, 2);
        p_line(dc, 5, 14, 11, 14, blue, 1);
        break;
    case IC_NOTE:
        p_rect(dc, 2, 2, 14, 12, yellow, 1, 1);
        p_rect(dc, 2, 2, 14, 12, RGB(190,150,30), 0, 1);
        p_line(dc, 4, 5, 11, 5, RGB(120,100,20), 1);
        p_line(dc, 4, 8, 11, 8, RGB(120,100,20), 1);
        break;
    case IC_ERASE:
        p_rect(dc, 2, 8, 13, 14, RGB(250,170,180), 1, 1);
        p_rect(dc, 2, 8, 13, 14, red, 0, 1);
        p_line(dc, 4, 4, 9, 8, red, 2);
        break;
    case IC_ROTL:
    case IC_ROTR: {
        int flip = (id == IC_ROTR);
        p_line(dc, flip ? 11 : 5, 3, flip ? 7 : 9, 1, blue, 1);
        p_line(dc, flip ? 1 : 15, 8, flip ? 4 : 12, 4, blue, 1);
        p_line(dc, flip ? 1 : 15, 8, flip ? 6 : 10, 12, blue, 2);
        p_line(dc, flip ? 3 : 13, 4, flip ? 6 : 10, 12, blue, 2);
        p_line(dc, 8, 12, 8, 15, blue, 1);
        break;
    }
    case IC_DUP:
        p_rect(dc, 2, 2, 10, 12, gray, 0, 1);
        p_rect(dc, 5, 4, 13, 14, blue, 0, 1);
        break;
    case IC_DEL:
        p_line(dc, 3, 3, 13, 13, red, 2);
        p_line(dc, 13, 3, 3, 13, red, 2);
        break;
    case IC_UP: {
        POINT p[3] = { {8,2},{14,10},{2,10} };
        p_poly(dc, p, 3, green, 1);
        p_rect(dc, 4, 11, 12, 14, green, 1, 1);
        break;
    }
    case IC_DOWN: {
        POINT p[3] = { {8,14},{14,6},{2,6} };
        p_poly(dc, p, 3, green, 1);
        p_rect(dc, 4, 2, 12, 5, green, 1, 1);
        break;
    }
    case IC_OCR:
        p_rect(dc, 3, 1, 13, 15, RGB(240,240,245), 1, 1);
        p_rect(dc, 3, 1, 13, 15, gray, 0, 1);
        p_line(dc, 5, 5, 11, 5, blue, 1);
        p_line(dc, 5, 7, 11, 7, blue, 1);
        p_line(dc, 5, 9, 9, 9, blue, 1);
        p_circle(dc, 10, 11, 3, red, 0, 1);
        p_line(dc, 12, 13, 14, 15, red, 1);
        break;
    case IC_OCRALL:
        p_rect(dc, 1, 3, 9, 15, RGB(240,240,245), 1, 1);
        p_rect(dc, 1, 3, 9, 15, gray, 0, 1);
        p_rect(dc, 5, 1, 13, 13, RGB(250,250,252), 1, 1);
        p_rect(dc, 5, 1, 13, 13, blue, 0, 1);
        p_line(dc, 7, 4, 11, 4, blue, 1);
        p_line(dc, 7, 6, 11, 6, blue, 1);
        break;
    case IC_UNDO: {
        POINT p[3] = { {2,7},{8,2},{8,12} };
        p_poly(dc, p, 3, blue, 1);
        p_line(dc, 8, 8, 13, 8, blue, 2);
        p_line(dc, 13, 8, 13, 12, blue, 2);
        break;
    }
    case IC_ZOOMIN:
    case IC_ZOOMOUT:
        p_circle(dc, 6, 6, 4, blue, 0, 2);
        p_line(dc, 9, 9, 14, 14, blue, 2);
        p_line(dc, 4, 6, 8, 6, ink, 1);
        if (id == IC_ZOOMIN) p_line(dc, 6, 4, 6, 8, ink, 1);
        break;
    case IC_FITW:
        p_rect(dc, 3, 2, 13, 14, blue, 0, 1);
        p_line(dc, 5, 8, 11, 8, blue, 1);
        p_poly(dc, (POINT[]){{5,6},{2,8},{5,10}}, 3, blue, 1);
        p_poly(dc, (POINT[]){{11,6},{14,8},{11,10}}, 3, blue, 1);
        break;
    case IC_GRID2:                      /* две страницы в ряд */
        p_rect(dc, 1, 2, 7, 14, blue, 0, 1);
        p_rect(dc, 9, 2, 15, 14, RGB(120,150,200), 0, 1);
        break;
    case IC_GRID4:                      /* сетка страниц */
        p_rect(dc, 1, 1, 7, 7, blue, 0, 1);
        p_rect(dc, 9, 1, 15, 7, RGB(120,150,200), 0, 1);
        p_rect(dc, 1, 9, 7, 15, RGB(120,150,200), 0, 1);
        p_rect(dc, 9, 9, 15, 15, blue, 0, 1);
        break;
    case IC_CONT:                       /* непрерывная лента страниц */
        p_rect(dc, 3, 1, 13, 5, blue, 0, 1);
        p_rect(dc, 3, 6, 13, 10, RGB(120,150,200), 0, 1);
        p_rect(dc, 3, 11, 13, 15, RGB(160,180,215), 0, 1);
        break;
    case IC_ZOOM100:
        p_rect(dc, 1, 3, 15, 13, blue, 0, 1);
        p_line(dc, 4, 6, 4, 10, blue, 1);
        p_line(dc, 11, 6, 11, 10, blue, 1);
        p_line(dc, 6, 6, 6, 10, dgray, 1);
        p_line(dc, 9, 6, 9, 10, dgray, 1);
        break;
    case IC_WMIN:
        p_line(dc, 3, 12, 13, 12, RGB(255,255,255), 2);
        break;
    case IC_WMAX:
        p_rect(dc, 4, 4, 12, 11, RGB(255,255,255), 0, 1);
        break;
    case IC_WRESTORE:
        p_rect(dc, 3, 6, 10, 12, RGB(255,255,255), 0, 1);
        p_line(dc, 5, 6, 5, 4, RGB(255,255,255), 1);
        p_line(dc, 5, 4, 12, 4, RGB(255,255,255), 1);
        p_line(dc, 12, 4, 12, 10, RGB(255,255,255), 1);
        break;
    case IC_WCLOSE:
        p_line(dc, 4, 4, 12, 12, RGB(255,255,255), 2);
        p_line(dc, 12, 4, 4, 12, RGB(255,255,255), 2);
        break;
    case IC_VIEWPAGE:
        p_rect(dc, 4, 2, 12, 14, blue, 0, 1);
        p_line(dc, 6, 5, 10, 5, lblue, 1);
        p_line(dc, 6, 7, 10, 7, lblue, 1);
        break;
    case IC_VIEWFIT:
        p_rect(dc, 2, 3, 14, 13, blue, 0, 1);
        p_line(dc, 4, 8, 12, 8, blue, 1);
        break;
    case IC_TOOLS:
        p_line(dc, 3, 13, 10, 6, RGB(150,155,165), 3);
        p_line(dc, 10, 6, 13, 3, RGB(120,125,135), 3);
        p_circle(dc, 12, 4, 2, RGB(120,125,135), 0, 2);
        break;
    default:
        break;
    }
}

static COLORREF mixcol(COLORREF a, COLORREF b)
{
    return RGB((GetRValue(a) + GetRValue(b)) / 2,
               (GetGValue(a) + GetGValue(b)) / 2,
               (GetBValue(a) + GetBValue(b)) / 2);
}

/* Иконка рисуется в 4 раза крупнее в 32-битный DIB, а потом уменьшается
   StretchBlt'ом в HALFTONE — GDI сам усредняет пиксели, и вместо «кривых»
   однопиксельных ступенек получается ровная сглаженная картинка. */
static void icon_render(HDC dc, int x, int y, int size, int id, int disabled, COLORREF bg)
{
    const int ss = 4;
    int W = size * ss, i, n;
    HDC mem;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    HGDIOBJ ob;
    int saved, oldmode;

    if (size <= 0) return;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -W;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mem = CreateCompatibleDC(dc);
    if (!mem) return;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(mem); return; }
    ob = SelectObject(mem, bmp);

    { /* фон под иконкой — чтобы при уменьшении края смешались с ним, а не с чёрным */
        HBRUSH br = CreateSolidBrush(bg);
        RECT r = { 0, 0, W, W };
        FillRect(mem, &r, br);
        DeleteObject(br);
    }

    saved = SaveDC(mem);
    SetMapMode(mem, MM_ANISOTROPIC);
    SetWindowExtEx(mem, 16, 16, NULL);
    SetViewportExtEx(mem, W, W, NULL);
    SetViewportOrgEx(mem, 0, 0, NULL);
    SetBkMode(mem, TRANSPARENT);
    icon_geometry(mem, id, size <= 16);
    RestoreDC(mem, saved);

    if (disabled) {                      /* приглушаем к цвету панели */
        DWORD *p = (DWORD *)bits;
        int br_ = GetRValue(bg), bg_ = GetGValue(bg), bb_ = GetBValue(bg);
        n = W * W;
        for (i = 0; i < n; i++) {
            DWORD v = p[i];
            int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            r = (r * 40 + br_ * 60) / 100;
            g = (g * 40 + bg_ * 60) / 100;
            b = (b * 40 + bb_ * 60) / 100;
            p[i] = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
        }
    }

    oldmode = SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, x, y, NULL);
    StretchBlt(dc, x, y, size, size, mem, 0, 0, W, W, SRCCOPY);
    SetStretchBltMode(dc, oldmode);

    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void draw_icon_c(HDC dc, int x, int y, int size, int id, COLORREF col, COLORREF bg)
{
    (void)col;
    icon_render(dc, x, y, size, id, 0, bg);
}

static void draw_icon(HDC dc, int x, int y, int size, int id, int disabled, COLORREF bg)
{
    icon_render(dc, x, y, size, id, disabled, bg);
}

/* ---------- состояние элементов ---------- */
static int item_enabled(int cmd, int flags)
{
    if (flags & MI_ALWAYS) return 1;
    /* эти команды имеют смысл и без открытого документа — иначе кнопка
       «Открыть» на панели выглядела неактивной и не нажималась на пустом окне */
    if (cmd == IDC_OPEN || cmd == IDC_QUIT || cmd == IDC_SETDEFAULT ||
        cmd == IDC_STATUS + 1 || cmd == IDC_TOOLSPANEL || cmd == IDC_FINDREPL)
        return 1;
    /* форматирование имеет смысл только в режиме документа */
    if (cmd >= IDC_FMT_B && cmd <= IDC_PAGENUM)
        return g_open && g_docmode;
    return g_open;
}

static int tool_of(int cmd)
{
    if (cmd >= IDC_TOOLBASE && cmd < IDC_TOOLBASE + TOOL_COUNT) return cmd - IDC_TOOLBASE;
    if (cmd == IDC_EDITTEXT) return -2;             /* отдельная кнопка */
    return -1;
}

static int item_checked(int cmd)
{
    int t = tool_of(cmd);
    if (t == -2) return g_tool == TOOL_EDIT;
    if (t >= 0) return g_tool == t;
    if (cmd == IDC_TOOLSPANEL) return g_tools;
    if (cmd == IDC_VIEW1)     return g_viewMode == 0;
    if (cmd == IDC_VIEWCONT)  return g_viewMode == 1;
    if (cmd == IDC_VIEW2)     return g_viewMode == 2;
    if (cmd == IDC_VIEWMULTI) return g_viewMode == 3;
    if (cmd == IDC_DOCMODE)   return g_docmode;
    if (cmd == IDC_FMT_B)     return g_fmtB;
    if (cmd == IDC_FMT_I)     return g_fmtI;
    if (cmd == IDC_FMT_U)     return g_fmtU;
    if (cmd == IDC_ALIGN_L)   return g_fmtAlign == 0;
    if (cmd == IDC_ALIGN_C)   return g_fmtAlign == 1;
    if (cmd == IDC_ALIGN_R)   return g_fmtAlign == 2;
    if (cmd == IDC_ALIGN_J)   return g_fmtAlign == 3;
    if (cmd == IDC_LIST_B)    return g_fmtList == 1;
    if (cmd == IDC_LIST_N)    return g_fmtList == 2;
    if (cmd == IDC_PAGENUM)   return g_pgnum;
    if (cmd >= IDC_FMT_COLOR && cmd <= IDC_FMT_COLOR + 5)
        return g_fmtColor == ((unsigned[]){ 0x000000, 0xC00000, 0x0000C0,
                                            0x008000, 0xE07000, 0x808080 })[cmd - IDC_FMT_COLOR];
    return 0;
}

/* ---------- горячие области ---------- */
static int hot_addf(RECT r, int kind, int aux, int cmd, int icon,
                    const wchar_t *tip, const wchar_t *label, unsigned color)
{
    if (g_hotN >= MAX_HOT) return -1;
    g_hot[g_hotN].r = r;
    g_hot[g_hotN].kind = kind;
    g_hot[g_hotN].aux = aux;
    g_hot[g_hotN].cmd = cmd;
    g_hot[g_hotN].icon = icon;
    g_hot[g_hotN].tip = tip;
    g_hot[g_hotN].label = label;
    g_hot[g_hotN].color = color;
    return g_hotN++;
}

static void hot_add(RECT r, int kind, int aux, int cmd, int icon, const wchar_t *tip)
{
    hot_addf(r, kind, aux, cmd, icon, tip, NULL, 0);
}

static int hit_index(int x, int y)
{
    int i;
    for (i = g_hotN - 1; i >= 0; i--) {
        RECT r = g_hot[i].r;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

/* ---------- раскладка ---------- */
/* высота полосы форматирования: она видна всегда, но активна в режиме документа */
static int fmt_h(void) { return FMT_H; }

static const unsigned g_pal[6] = {
    0x000000, 0xC00000, 0x0000C0, 0x008000, 0xE07000, 0x808080
};
static const wchar_t *g_palTip[6] = {
    L"Цвет текста: чёрный", L"Цвет текста: красный", L"Цвет текста: синий",
    L"Цвет текста: зелёный", L"Цвет текста: оранжевый", L"Цвет текста: серый"
};

static void layout_all(HDC dc)
{
    int x, y, i;
    RECT r;

    g_hotN = 0;

    /* кнопки окна */
    {
        int bw = 24, gap = 2, bx;
        bx = g_cw - 3 - (bw * 3 + gap * 2);
        for (i = 0; i < 3; i++) {
            r.left = bx + i * (bw + gap); r.right = r.left + bw;
            r.top = 4; r.bottom = CAP_H - 4;
            hot_add(r, 4, i, 0,
                    i == 0 ? IC_WMIN : i == 1 ? (g_maximized ? IC_WRESTORE : IC_WMAX) : IC_WCLOSE,
                    i == 0 ? L"Свернуть" : i == 1 ? (g_maximized ? L"Восстановить" : L"Развернуть")
                                                  : L"Закрыть");
        }
    }

    /* строка меню */
    {
        int mx = 6, my = CAP_H + 2;
        for (i = 0; i < MENU_COUNT; i++) {
            int w = text_w(dc, f_ui, g_menus[i].title) + 16;
            r.left = mx; r.right = mx + w; r.top = CAP_H; r.bottom = CAP_H + MENU_H;
            hot_add(r, 2, i, 0, 0, NULL);
            mx += w;
        }
        (void)my;
    }

    /* панель инструментов */
    {
        int ty = CAP_H + MENU_H;
        int by = ty + 3, bs = 28;

        x = 4;

        /* файл */
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_OPEN, IC_OPEN, L"Открыть (Ctrl+O)"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_SAVE, IC_SAVE, L"Сохранить (Ctrl+S)"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_PRINT, IC_PRINT, L"Печать (Ctrl+P)"); x += bs + 6;
        vline(dc, x - 3, by + 2, by + bs - 2, XPLINE);
        vline(dc, x - 2, by + 2, by + bs - 2, XPLINE_LT);
        x += 4;

        /* страницы */
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_PREV, IC_PREV, L"Предыдущая страница (PgUp)"); x += bs + 3;
        g_pageRect.left = x;
        g_pageRect.top = by + 4;
        g_pageRect.right = x + 54;
        g_pageRect.bottom = by + bs - 4;
        x += 58;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_NEXT, IC_NEXT, L"Следующая страница (PgDn)"); x += bs + 6;
        vline(dc, x - 3, by + 2, by + bs - 2, XPLINE);
        vline(dc, x - 2, by + 2, by + bs - 2, XPLINE_LT);
        x += 4;

        /* масштаб */
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_ZOOMOUT, IC_ZOOMOUT, L"Уменьшить (−)"); x += bs + 1;
        r.left = x; r.right = x + 46; r.top = by; r.bottom = by + bs;
        hot_add(r, 7, 0, IDC_ZOOM100, 0, L"Масштаб (щелчок — 100 %)"); x += 48;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_ZOOMIN, IC_ZOOMIN, L"Увеличить (+)"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_ZOOM100, IC_ZOOM100, L"Масштаб 100 %"); x += bs + 6;
        vline(dc, x - 3, by + 2, by + bs - 2, XPLINE);
        vline(dc, x - 2, by + 2, by + bs - 2, XPLINE_LT);
        x += 4;

        /* режим показа страниц */
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_VIEW1, IC_VIEWPAGE, L"Одна страница"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_VIEWCONT, IC_CONT, L"Непрерывно: все страницы подряд"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_VIEW2, IC_GRID2, L"Две страницы в ряд"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_VIEWMULTI, IC_GRID4, L"Несколько страниц (по ширине окна)"); x += bs + 6;
        vline(dc, x - 3, by + 2, by + bs - 2, XPLINE);
        vline(dc, x - 2, by + 2, by + bs - 2, XPLINE_LT);
        x += 4;

        /* инструменты */
        {
            static const int ticon[TOOL_COUNT] = {
                IC_SELECT, IC_TEXT, IC_HIGHLIGHT, IC_RECT, IC_NOTE, IC_PEN, IC_ERASE, IC_EDIT
            };
            static const wchar_t *tn[TOOL_COUNT] = {
                L"Выбор", L"Текст", L"Маркер", L"Рамка", L"Заметка", L"Перо", L"Ластик",
                L"Правка текста (F2)"
            };
            int t;
            for (t = 0; t < TOOL_COUNT; t++) {
                int cmd = (t == TOOL_EDIT) ? IDC_EDITTEXT : (IDC_TOOLBASE + t);
                r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
                hot_add(r, 1, 0, cmd, ticon[t], tn[t]);
                x += bs + 1;
            }
        }
        x += 4;
        vline(dc, x - 3, by + 2, by + bs - 2, XPLINE);
        vline(dc, x - 2, by + 2, by + bs - 2, XPLINE_LT);
        x += 6;

        /* поиск */
        x += text_w(dc, f_ui, L"Найти:") + 4;
        g_searchRect.left = x;
        g_searchRect.top = by + 4;
        g_searchRect.right = x + 150;
        g_searchRect.bottom = by + bs - 4;
        x += 154;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_FIND, IC_SEARCH, L"Найти (Ctrl+F)"); x += bs + 1;
        r.left = x; r.right = x + bs; r.top = by; r.bottom = by + bs;
        hot_add(r, 1, 0, IDC_FINDNEXT, IC_NEXT, L"Найти далее (F3)"); x += bs + 1;
        (void)y; (void)ty;
    }

    /* панель форматирования */
    {
        int fy = CAP_H + MENU_H + TB_H;
        int by = fy + 3, bh = FMT_H - 6, fx = 4, k;

        r.top = by; r.bottom = by + bh;

        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_FMT_B, 0, L"Полужирный (Ctrl+B)", L"Ж", 0); fx += 27;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_FMT_I, 0, L"Курсив (Ctrl+I)", L"К", 0); fx += 27;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_FMT_U, 0, L"Подчёркнутый (Ctrl+U)", L"Ч", 0); fx += 31;
        vline(dc, fx - 3, by + 2, by + bh - 2, XPLINE);
        fx += 4;

        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_FMT_SMALLER, 0, L"Кегль меньше", L"A−", 0); fx += 27;
        r.left = fx; r.right = fx + 34;
        hot_addf(r, 10, 0, 0, 0, L"Кегль (шрифт текста)", L"", 0); fx += 35;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_FMT_BIGGER, 0, L"Кегль больше", L"A+", 0); fx += 31;
        r.left = fx; r.right = fx + text_w(dc, f_ui, g_fontShort) + 18;
        hot_addf(r, 8, 0, IDC_FONTMENU, 0, L"Шрифт (семейство шрифта)", g_fontShort, 0); fx = r.right + 4;
        vline(dc, fx - 3, by + 2, by + bh - 2, XPLINE);
        fx += 4;

        for (k = 0; k < 6; k++) {
            r.left = fx; r.right = fx + 17; r.top = by + 3; r.bottom = by + bh - 3;
            hot_addf(r, 9, 0, IDC_FMT_COLOR + k, 0, g_palTip[k], NULL, g_pal[k]);
            fx += 19;
        }
        r.top = by; r.bottom = by + bh;
        fx += 2;
        vline(dc, fx - 3, by + 2, by + bh - 2, XPLINE);
        fx += 4;

        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_ALIGN_L, 0, L"По левому краю (Ctrl+L)", L"Л", 0); fx += 27;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_ALIGN_C, 0, L"По центру (Ctrl+E)", L"Ц", 0); fx += 27;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_ALIGN_R, 0, L"По правому краю (Ctrl+R)", L"П", 0); fx += 27;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_ALIGN_J, 0, L"По ширине (Ctrl+J)", L"Ш", 0); fx += 31;
        vline(dc, fx - 3, by + 2, by + bh - 2, XPLINE);
        fx += 4;

        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_LIST_B, 0, L"Маркированный список", L"•", 0); fx += 27;
        r.left = fx; r.right = fx + 30;
        hot_addf(r, 8, 0, IDC_LIST_N, 0, L"Нумерованный список", L"1.", 0); fx += 34;
        vline(dc, fx - 3, by + 2, by + bh - 2, XPLINE);
        fx += 4;

        r.left = fx; r.right = fx + text_w(dc, f_ui, L"1,15") + 14;
        hot_addf(r, 8, 0, IDC_SPACING, 0, L"Межстрочный интервал", L"", 0); fx = r.right + 3;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_INDENTP, 0, L"Красная строка +", L"→", 0); fx += 27;
        r.left = fx; r.right = fx + 26;
        hot_addf(r, 8, 0, IDC_INDENTM, 0, L"Красная строка −", L"←", 0); fx += 31;
        vline(dc, fx - 3, by + 2, by + bh - 2, XPLINE);
        fx += 4;

        r.left = fx; r.right = fx + text_w(dc, f_ui, L"Отмена") + 14;
        hot_addf(r, 8, 0, IDC_FMT_UNDO, 0, L"Отменить (Ctrl+Z)", L"Отмена", 0);
        fx = r.right + 3;
        r.left = fx; r.right = fx + text_w(dc, f_ui, L"Повтор") + 14;
        hot_addf(r, 8, 0, IDC_FMT_REDO, 0, L"Повторить (Ctrl+Y)", L"Повтор", 0);
    }

    /* левая рейка */
    {
        int ry = CAP_H + MENU_H + TB_H + fmt_h();
        int bw = RAIL_W - 6;
        r.left = 3; r.right = 3 + bw; r.top = ry + 4; r.bottom = ry + 4 + bw;
        hot_add(r, 6, 0, IDC_TOOLSPANEL, IC_TOOLS, L"Панель инструментов");
        r.left = 3; r.right = 3 + bw; r.top = ry + 4 + bw + 4; r.bottom = ry + 4 + bw * 2 + 4;
        hot_add(r, 6, 1, IDC_COPYTEXT, IC_COPY, L"Копировать текст страницы");
    }

    /* правая панель «Инструменты» */
    if (g_tools) {
        int px = g_cw - TOOLS_W;
        int py = CAP_H + MENU_H + TB_H + fmt_h() + RULER_H;
        int yy = py + 26, t;
        static const int ticon[TOOL_COUNT] = {
            IC_SELECT, IC_TEXT, IC_HIGHLIGHT, IC_RECT, IC_NOTE, IC_PEN, IC_ERASE, IC_EDIT
        };
        static const wchar_t *tn[TOOL_COUNT] = {
            L"Выбор", L"Текст", L"Маркер", L"Рамка", L"Заметка", L"Перо", L"Ластик",
            L"Правка текста"
        };
        for (t = 0; t < TOOL_COUNT; t++) {
            int cmd = (t == TOOL_EDIT) ? IDC_EDITTEXT : (IDC_TOOLBASE + t);
            r.left = px + 6; r.right = px + TOOLS_W - 8;
            r.top = yy; r.bottom = yy + 24;
            hot_add(r, 5, 0, cmd, ticon[t], tn[t]);
            yy += 25;
        }
        yy += 6;
        r.left = px + 6; r.right = px + TOOLS_W - 8; r.top = yy; r.bottom = yy + 24;
        hot_add(r, 5, 0, IDC_OCR, IC_OCR, L"Распознать стр.");
        yy += 25;
        r.left = px + 6; r.right = px + TOOLS_W - 8; r.top = yy; r.bottom = yy + 24;
        hot_add(r, 5, 0, IDC_OCRALL, IC_OCRALL, L"Распознать всё");
        yy += 31;
        r.left = px + 6; r.right = px + TOOLS_W - 8; r.top = yy; r.bottom = yy + 24;
        hot_add(r, 5, 0, IDC_COPYTEXT, IC_COPY, L"Копировать текст");
        yy += 31;
        r.left = px + 6; r.right = px + TOOLS_W - 8; r.top = yy; r.bottom = yy + 24;
        hot_add(r, 5, 0, IDC_TEXTUNDO, IC_UNDO, L"Отменить правки");
        (void)py;
    }

    /* строка состояния */
    {
        int sy = g_ch - STAT_H;
        r.left = g_cw - 356; r.right = g_cw - 276; r.top = sy; r.bottom = g_ch;
        hot_add(r, 3, 0, 0, 0, L"Номер страницы");
        r.left = g_cw - 272; r.right = g_cw - 226; r.top = sy; r.bottom = g_ch;
        hot_add(r, 3, 4, IDC_ZOOM100, 0, L"Масштаб: щелчок — 100 %");
        r.left = g_cw - 220; r.right = g_cw - 96; r.top = sy + 4; r.bottom = g_ch - 4;
        hot_add(r, 3, 1, 0, 0, L"Масштаб: потяните ползунок");
        r.left = g_cw - 90; r.right = g_cw - 56; r.top = sy; r.bottom = g_ch;
        hot_add(r, 3, 2, IDC_FITW, 0, L"По ширине");
        r.left = g_cw - 54; r.right = g_cw - 20; r.top = sy; r.bottom = g_ch;
        hot_add(r, 3, 3, IDC_ZOOM100, 0, L"Масштаб 100 %");
    }

    g_rulerHot = 0;
}

int wui_chrome_height(void) { return CAP_H + MENU_H + TB_H + fmt_h() + RULER_H; }
int wui_status_height(void) { return STAT_H; }
int wui_menu_open(void) { return g_menuActive >= 0; }
int wui_tools_visible(void) { return g_tools; }

void wui_set_view_mode(int m) { g_viewMode = m; }
int  wui_view_mode(void) { return g_viewMode; }
int wui_consume_repaint(void) { int r = g_repaint; g_repaint = 0; return r; }

RECT wui_view_rect(void)
{
    RECT r;
    r.left = RAIL_W;
    r.top = CAP_H + MENU_H + TB_H + fmt_h() + RULER_H;
    r.right = g_cw - (g_tools ? TOOLS_W : 0);
    r.bottom = g_ch - STAT_H;
    if (r.bottom < r.top + 40) r.bottom = r.top + 40;
    if (r.right < r.left + 40) r.right = r.left + 40;
    return r;
}

RECT wui_search_rect(void) { return g_searchRect; }

RECT wui_ruler_rect(void)
{
    RECT r;
    r.left = 0;
    r.right = g_cw;
    r.top = CAP_H + MENU_H + TB_H + fmt_h();
    r.bottom = r.top + RULER_H;
    return r;
}
RECT wui_page_rect(void) { return g_pageRect; }

void wui_set_ruler(int pageX, int pageW) { g_pageX = pageX; g_pageW = pageW; }
void wui_set_caret(int x) { g_caretX = x; }
void wui_zoom_geometry(int pageX, int pageW) { g_pageX = pageX; g_pageW = pageW; }

/* ---------- отрисовка ---------- */
static void paint_caption(HDC dc)
{
    int i;
    grad_h(dc, 0, 0, g_cw, CAP_H, CAP_L, CAP_R);
    hline(dc, 0, g_cw, CAP_H - 1, CAP_BRD);

    /* значок приложения */
    round_rect(dc, 5, 5, 23, 23, 4, RGB(255, 255, 255));
    text_at(dc, f_bold, CAP_L, L"P", (RECT){ 5, 5, 23, 23 },
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* название */
    {
        RECT r = { 30, 0, g_cw - 90, CAP_H };
        text_at(dc, f_bold, CAP_TX, g_title, r,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* кнопки окна — в стиле XP */
    for (i = 0; i < g_hotN; i++) {
        if (g_hot[i].kind != 4) continue;
        {
            RECT r = g_hot[i].r;
            int hovered = (g_hover == i);
            int pressed = (g_press == i);
            int isclose = (g_hot[i].aux == 2);
            COLORREF base = isclose ? WBTN_CLOSE : WBTN_FACE;
            COLORREF top = isclose ? RGB(240,120,100) : RGB(120,175,250);
            COLORREF bot = isclose ? RGB(190, 40, 30) : RGB( 45,105,200);
            if (pressed) { top = base; bot = base; }
            else if (hovered) { top = isclose ? RGB(250,150,130) : RGB(150,195,255); }
            round_rect(dc, r.left, r.top, r.right, r.bottom, 4, CAP_BRD);
            if (r.right - r.left > 4) {
                int saved = SaveDC(dc);
                HRGN rgn = CreateRoundRectRgn(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, 4, 4);
                SelectClipRgn(dc, rgn);
                grad_v(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, top, bot);
                RestoreDC(dc, saved);
                DeleteObject(rgn);
            }
            draw_icon_c(dc, (r.left + r.right - 12) / 2, (r.top + r.bottom - 12) / 2, 12,
                        g_hot[i].icon, RGB(255, 255, 255), mixcol(top, bot));
        }
    }
}

static void paint_menubar(HDC dc)
{
    int i, y = CAP_H;
    fill(dc, 0, y, g_cw, y + MENU_H, XPBG);
    hline(dc, 0, g_cw, y + MENU_H - 1, XPBG_DK);

    for (i = 0; i < g_hotN; i++) {
        if (g_hot[i].kind != 2) continue;
        {
            RECT r = g_hot[i].r;
            int hot = (g_hover == i) || (g_menuActive == g_hot[i].aux);
            if (hot) fill(dc, r.left, r.top + 1, r.right, r.bottom - 2, XPSEL);
            text_at(dc, f_ui, hot ? XPSEL_TX : XPTX, g_menus[g_hot[i].aux].title, r,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

static void paint_toolbar(HDC dc)
{
    int y = CAP_H + MENU_H, i;
    fill(dc, 0, y, g_cw, y + TB_H, XPBG);
    grad_v(dc, 0, y, g_cw, y + 3, XPBG_LT, XPBG);
    hline(dc, 0, g_cw, y + TB_H - 1, XPBG_DK);

    for (i = 0; i < g_hotN; i++) {
        RECT r = g_hot[i].r;
        if (g_hot[i].kind == 1) {
            int disabled = !item_enabled(g_hot[i].cmd, 0);
            int checked = item_checked(g_hot[i].cmd);
            int hot = (g_hover == i);
            int pressed = (g_press == i);
            if (disabled) {
                draw_icon(dc, (r.left + r.right - 20) / 2, (r.top + r.bottom - 20) / 2,
                          20, g_hot[i].icon, 1, XPBG);
                continue;
            }
            if (pressed || checked) {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, TBHOT_BRD);
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, TBHOT);
            } else if (hot) {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, TBHOT_BRD);
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, RGB(255,235,180));
            }
            draw_icon(dc, (r.left + r.right - 20) / 2, (r.top + r.bottom - 20) / 2,
                      20, g_hot[i].icon, 0,
                      (pressed || checked) ? TBHOT : hot ? RGB(255, 235, 180) : XPBG);
        } else if (g_hot[i].kind == 7) {
            /* поле «по ширине»: подпись с процентом */
            wchar_t b[32];
            RECT t;
            frame_rect_c(dc, r.left, r.top + 4, r.right, r.bottom - 4, RGB(160,160,150));
            fill(dc, r.left + 1, r.top + 5, r.right - 1, r.bottom - 5, RGB(255,255,255));
            wsprintfW(b, L"%d %%", (int)(g_zoom * 100.0f + 0.5f));
            t = r;
            text_at(dc, f_ui, XPTX, b, t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    /* подпись поля поиска и рамка вокруг EDIT */
    {
        int sx = g_searchRect.left;
        int ty = CAP_H + MENU_H;
        text_at(dc, f_ui, XPTX, L"Найти:",
                (RECT){ sx - text_w(dc, f_ui, L"Найти:") - 4, ty + 3, sx - 2, ty + TB_H },
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        frame_rect_c(dc, g_searchRect.left - 1, g_searchRect.top - 1,
                     g_searchRect.right + 1, g_searchRect.bottom + 1, RGB(127,157,185));
    }

    /* рамка поля номера страницы */
    frame_rect_c(dc, g_pageRect.left - 1, g_pageRect.top - 1,
                 g_pageRect.right + 1, g_pageRect.bottom + 1, RGB(127,157,185));
}

static void paint_fmtbar(HDC dc)
{
    int y, i, right = 0;
    y = CAP_H + MENU_H + TB_H;
    fill(dc, 0, y, g_cw, y + FMT_H, XPBG);
    grad_v(dc, 0, y, g_cw, y + 2, XPBG_LT, XPBG);
    hline(dc, 0, g_cw, y + FMT_H - 1, XPBG_DK);

    for (i = 0; i < g_hotN; i++) {
        RECT r = g_hot[i].r;
        if (g_hot[i].kind == 9) {                 /* образец цвета */
            fill(dc, r.left, r.top, r.right, r.bottom, (COLORREF)g_hot[i].color);
            frame_rect_c(dc, r.left, r.top, r.right, r.bottom, RGB(130,130,120));
            if (g_fmtColor == g_hot[i].color) {
                frame_rect_c(dc, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1,
                             XPSEL);
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, XPSEL);
            }
        } else if (g_hot[i].kind == 10) {         /* надпись: кегль */
            wchar_t b[16];
            if (!g_docmode) lstrcpyW(b, L"—");
            else wsprintfW(b, L"%d", (int)(g_fmtSize + 0.5f));
            text_at(dc, f_ui, XPTX, b, r,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (g_hot[i].kind == 8) {          /* кнопка форматирования */
            int checked = item_checked(g_hot[i].cmd);
            int hot = (g_hover == i);
            int pressed = (g_press == i);
            int disabled = !item_enabled(g_hot[i].cmd, 0);
            const wchar_t *label = g_hot[i].label;
            wchar_t sp[16];

            if (g_hot[i].cmd == IDC_SPACING) {
                if (g_fmtSpacing < 1.05f) lstrcpyW(sp, L"1,0");
                else if (g_fmtSpacing < 1.2f) lstrcpyW(sp, L"1,15");
                else if (g_fmtSpacing < 1.7f) lstrcpyW(sp, L"1,5");
                else lstrcpyW(sp, L"2,0");
                label = sp;
            }
            if (pressed || checked) {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, TBHOT_BRD);
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, TBHOT);
            } else if (hot && !disabled) {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, TBHOT_BRD);
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1,
                     RGB(255,235,180));
            } else {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, RGB(206,202,188));
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, XPBG_LT);
            }
            text_at(dc, checked ? f_bold : f_ui, disabled ? XPDIS : XPTX, label, r,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if ((g_hot[i].kind == 8 || g_hot[i].kind == 9 || g_hot[i].kind == 10) &&
            r.right > right)
            right = r.right;
    }

    /* почему кнопки серые — прямо на панели */
    if (!g_docmode) {
        int hx = right + 14;
        if (hx > g_cw - 400) hx = 730;
        text_at(dc, f_ui, RGB(110, 106, 94),
                L"Оформление — только в режиме документа (Ctrl+Shift+D)",
                (RECT){ hx, y, g_cw - 8, y + FMT_H },
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void paint_ruler(HDC dc)
{
    int y = CAP_H + MENU_H + TB_H + fmt_h();
    int x;

    fill(dc, 0, y, g_cw, y + RULER_H, RUL_OUT);
    hline(dc, 0, g_cw, y + RULER_H - 1, XPLINE);

    /* бледные деления по всей ширине */
    for (x = 0; x < g_cw; x += 19)
        vline(dc, x, y + 12, y + RULER_H - 4, RGB(205,205,198));

    /* область страницы — белая; поля — светлее/темнее */
    if (g_pageW > 0) {
        int px = g_pageX, pw = g_pageW;
        fill(dc, px, y + 2, px + pw, y + RULER_H - 2, RUL_BG);
        /* границы полей (12 % слева/справа) */
        {
            int m = pw / 8;
            fill(dc, px, y + 2, px + m, y + RULER_H - 2, RGB(246,246,246));
            fill(dc, px + pw - m, y + 2, px + pw, y + RULER_H - 2, RGB(246,246,246));
            vline(dc, px + m, y + 4, y + RULER_H - 4, RGB(180,180,180));
            vline(dc, px + pw - m, y + 4, y + RULER_H - 4, RGB(180,180,180));
        }
        /* деления: 1 см ≈ 37,8 px × масштаб страницы */
        {
            float scale = (float)pw / 595.0f;       /* A4 в точках */
            int cm = (int)(37.8f * scale);
            int n = 0;
            if (cm < 8) cm = 8;
            for (x = px; x < px + pw; x += cm / 2, n++) {
                int tall = (n % 2) == 0;
                vline(dc, x, y + (tall ? 6 : 11), y + RULER_H - 3, RUL_TICK);
                if (tall && n > 0 && (n / 2) % 2 == 0) {
                    wchar_t b[8];
                    wsprintfW(b, L"%d", n / 2);
                    text_at(dc, f_small, RGB(110,110,110), b,
                            (RECT){ x + 2, y + 3, x + 26, y + 14 },
                            DT_LEFT | DT_TOP | DT_SINGLELINE);
                }
            }
        }
        /* маркер каретки текста */
        if (g_caretX >= px && g_caretX <= px + pw) {
            POINT tri[3] = { {g_caretX - 4, y + RULER_H - 2}, {g_caretX + 4, y + RULER_H - 2},
                             {g_caretX, y + RULER_H - 8} };
            p_poly(dc, tri, 3, RGB(40, 90, 170), 1);
        }
    } else {
        int cm = 38;
        for (x = 8; x < g_cw - 8; x += cm / 2) {
            vline(dc, x, y + 8, y + RULER_H - 4, RUL_TICK);
        }
    }

    /* курсор мыши над линейкой — тонкая вертикаль */
    if (g_mouseY > y && g_mouseY < y + RULER_H) {
        vline(dc, g_mouseX, y + 2, y + RULER_H - 2, RGB(120,160,220));
    }
}

static void paint_rail(HDC dc)
{
    int y = CAP_H + MENU_H + TB_H + fmt_h() + RULER_H;
    int i;
    fill(dc, 0, y, RAIL_W, g_ch - STAT_H, XPBG);
    vline(dc, RAIL_W - 1, y, g_ch - STAT_H, XPLINE);
    vline(dc, RAIL_W - 2, y, g_ch - STAT_H, XPLINE_LT);

    for (i = 0; i < g_hotN; i++) {
        if (g_hot[i].kind != 6) continue;
        {
            RECT r = g_hot[i].r;
            int hot = (g_hover == i);
            int checked = item_checked(g_hot[i].cmd);
            if (checked || hot) {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, TBHOT_BRD);
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1,
                     checked ? TBHOT : RGB(255, 235, 180));
            }
            draw_icon(dc, (r.left + r.right - 16) / 2, (r.top + r.bottom - 16) / 2,
                      16, g_hot[i].icon, 0,
                      checked ? TBHOT : hot ? RGB(255, 235, 180) : XPBG);
        }
    }
}

static void paint_tools_panel(HDC dc)
{
    int x, y;
    int i;

    if (!g_tools) return;
    x = g_cw - TOOLS_W;
    y = CAP_H + MENU_H + TB_H + fmt_h() + RULER_H;

    fill(dc, x, y, g_cw, g_ch - STAT_H, XPBG);
    vline(dc, x, y, g_ch - STAT_H, XPLINE);
    vline(dc, x + 1, y, g_ch - STAT_H, XPLINE_LT);

    /* заголовок панели */
    grad_v(dc, x + 2, y + 2, g_cw, y + 22, RGB(250,250,246), RGB(226,222,208));
    text_at(dc, f_bold, XPTX, L"Инструменты",
            (RECT){ x + 8, y + 2, g_cw - 4, y + 22 },
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (i = 0; i < g_hotN; i++) {
        if (g_hot[i].kind != 5) continue;
        {
            RECT r = g_hot[i].r;
            int checked = item_checked(g_hot[i].cmd);
            int hot = (g_hover == i);
            int disabled = !item_enabled(g_hot[i].cmd, 0);
            if (checked) {
                fill(dc, r.left, r.top, r.right, r.bottom, XPSEL);
            } else if (hot && !disabled) {
                frame_rect_c(dc, r.left, r.top, r.right, r.bottom, RGB(120,170,230));
                fill(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, RGB(232,242,254));
            }
            draw_icon(dc, r.left + 4, (r.top + r.bottom - 16) / 2, 16, g_hot[i].icon, disabled,
                      checked ? XPSEL : (hot && !disabled) ? RGB(232, 242, 254) : XPBG);
            text_at(dc, f_ui, checked ? XPSEL_TX : (disabled ? XPDIS : XPTX),
                    g_hot[i].tip,
                    (RECT){ r.left + 26, r.top, r.right - 4, r.bottom },
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

static void paint_status(HDC dc)
{
    int y = g_ch - STAT_H, i;
    fill(dc, 0, y, g_cw, g_ch, STAT_BG);
    hline(dc, 0, g_cw, y, XPLINE_LT);
    hline(dc, 0, g_cw, y + 1, XPBG_DK);

    /* сообщение */
    text_at(dc, f_ui, XPTX, g_status[0] ? g_status : L"Готово",
            (RECT){ 8, y, g_cw - 366, g_ch },
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* страница */
    {
        wchar_t b[64];
        RECT r = { g_cw - 356, y, g_cw - 276, g_ch };
        if (g_pages > 0) wsprintfW(b, L"Стр. %d из %d", g_page + 1, g_pages);
        else lstrcpyW(b, L"Стр. —");
        text_at(dc, f_ui, XPTX, b, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    /* масштаб: ползунок и проценты */
    for (i = 0; i < g_hotN; i++) {
        if (g_hot[i].kind != 3) continue;
        if (g_hot[i].aux == 1) {
            RECT r = g_hot[i].r;
            int cxm = (r.top + r.bottom) / 2, t, kx;
            fill(dc, r.left, cxm - 1, r.right, cxm + 1, RGB(160,160,150));
            t = (int)((g_zoom * 100.0f - 10.0f) / 390.0f * (float)(r.right - r.left));
            if (t < 0) t = 0;
            if (t > r.right - r.left) t = r.right - r.left;
            kx = r.left + t;
            {
                POINT tri[3] = { {kx - 5, cxm - 6}, {kx + 5, cxm - 6}, {kx, cxm + 4} };
                p_poly(dc, tri, 3, RGB(60,110,180), 1);
            }
        } else if (g_hot[i].aux == 4) {
            continue;
        } else if (g_hot[i].aux == 2) {
            RECT r = g_hot[i].r;
            if (g_hover == i) fill(dc, r.left, r.top + 1, r.right, r.bottom - 1, RGB(255,235,180));
            draw_icon(dc, (r.left + r.right - 16) / 2, (r.top + r.bottom - 16) / 2,
                      16, IC_FITW, 0, g_hover == i ? RGB(255, 235, 180) : STAT_BG);
        } else if (g_hot[i].aux == 3) {
            RECT r = g_hot[i].r;
            if (g_hover == i) fill(dc, r.left, r.top + 1, r.right, r.bottom - 1, RGB(255,235,180));
            draw_icon(dc, (r.left + r.right - 16) / 2, (r.top + r.bottom - 16) / 2,
                      16, IC_ZOOM100, 0, g_hover == i ? RGB(255, 235, 180) : STAT_BG);
        }
    }
    {
        wchar_t b[32];
        wsprintfW(b, L"%d %%", (int)(g_zoom * 100.0f + 0.5f));
        frame_rect_c(dc, g_cw - 272, y + 4, g_cw - 226, g_ch - 4, RGB(160,160,150));
        fill(dc, g_cw - 271, y + 5, g_cw - 227, g_ch - 5, RGB(255,255,255));
        text_at(dc, f_ui, XPTX, b, (RECT){ g_cw - 272, y, g_cw - 226, g_ch },
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* ---------- ниспадающее меню (своё окно XP-вида) ---------- */
#define MI_H 22
static HWND g_menuWnd;
static int  g_menuIndex = -1;

static int menu_item_count(int idx)
{
    int i, n = 0;
    for (i = 0; i < g_menus[idx].n; i++)
        if (!(g_menus[idx].items[i].flags & MI_SEP)) n++;
    return n;
}

static int g_menuLabelW, g_menuAccelW;

static void menu_size(int idx, int *pw, int *ph)
{
    int i, w, h = 10;
    HDC dc = GetDC(NULL);
    g_menuLabelW = 0;
    g_menuAccelW = 0;
    for (i = 0; i < g_menus[idx].n; i++) {
        const MItem *it = &g_menus[idx].items[i];
        int tw;
        if (it->flags & MI_SEP) { h += 7; continue; }
        tw = text_w(dc, f_ui, it->label);
        if (tw > g_menuLabelW) g_menuLabelW = tw;
        if (it->accel) {
            tw = text_w(dc, f_small, it->accel);
            if (tw > g_menuAccelW) g_menuAccelW = tw;
        }
        h += MI_H;
    }
    ReleaseDC(NULL, dc);
    w = 30 + g_menuLabelW + 12;
    if (g_menuAccelW > 0) w += g_menuAccelW + 16;
    if (w < 140) w = 140;
    *pw = w;
    *ph = h;
}

static int menu_item_at(int idx, int x, int y, RECT *items)
{
    int i, yy = 6;
    for (i = 0; i < g_menus[idx].n; i++) {
        const MItem *it = &g_menus[idx].items[i];
        if (it->flags & MI_SEP) { yy += 7; continue; }
        items[i].left = 0; items[i].right = 0;
        items[i].top = yy;
        items[i].bottom = yy + MI_H;
        items[i].left = 0;
        if (y >= yy && y < yy + MI_H) return i;
        yy += MI_H;
    }
    return -1;
}

static void menu_paint(HWND h, HDC dc, int w, int hh, int idx, int hover, int press)
{
    int i, yy = 6;
    fill(dc, 0, 0, w, hh, RGB(255, 255, 255));
    fill(dc, 0, 0, 26, hh, RGB(245, 243, 237));
    frame_rect_c(dc, 0, 0, w - 1, hh - 1, RGB(145, 155, 156));

    for (i = 0; i < g_menus[idx].n; i++) {
        const MItem *it = &g_menus[idx].items[i];
        int disabled;
        if (it->flags & MI_SEP) {
            hline(dc, 26, w - 6, yy + 3, RGB(214, 210, 194));
            yy += 7;
            continue;
        }
        disabled = !item_enabled(it->cmd, it->flags);
        if (i == hover && !disabled) {
            fill(dc, 26, yy, w - 1, yy + MI_H, XPSEL);
            if (press) fill(dc, 26, yy, w - 1, yy + MI_H, RGB(30, 80, 160));
        }
        /* галочка */
        if (it->flags & MI_CHECKED) {
            if (item_checked(it->cmd))
                p_line(dc, 8, yy + 11, 12, yy + 15, disabled ? XPDIS : XPTX, 2),
                p_line(dc, 12, yy + 15, 18, yy + 6, disabled ? XPDIS : XPTX, 2);
        }
        text_at(dc, f_ui, disabled ? XPDIS : (i == hover ? XPSEL_TX : XPTX), it->label,
                (RECT){ 30, yy, 30 + g_menuLabelW + 2, yy + MI_H },
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (it->accel)
            text_at(dc, f_small, (i == hover) ? XPSEL_TX : XPGRAY, it->accel,
                    (RECT){ w - 8 - g_menuAccelW, yy, w - 8, yy + MI_H },
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        yy += MI_H;
    }
    (void)h;
}

static void menu_open(int idx, int x, int y);

static LRESULT CALLBACK MenuProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static int hover = -1, press = -1;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ ob = SelectObject(mem, bmp);
            menu_paint(h, mem, rc.right, rc.bottom, g_menuIndex, hover, press);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        RECT items[64];
        int i = menu_item_at(g_menuIndex, x, y, items);
        if (i != hover) { hover = i; InvalidateRect(h, NULL, FALSE); }
        /* переключение на соседнее меню при движении по заголовкам */
        if (y < 0) {
            POINT pt;
            pt.x = x; pt.y = y;
            ClientToScreen(h, &pt);
            wui_menu_hover_switch(pt.x, pt.y);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        RECT items[64];
        int i;
        if (x < 0 || y < 0) {           /* клик мимо меню — закрыть */
            wui_close_backstage();
            PostMessageW(g_main, WM_APP + 8, 0, 0);
            return 0;
        }
        i = menu_item_at(g_menuIndex, x, y, items);
        if (i < 0) return 0;
        press = i;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        RECT items[64];
        int i = menu_item_at(g_menuIndex, x, y, items);
        if (i >= 0 && i == press) {
            const MItem *it = &g_menus[g_menuIndex].items[i];
            if (item_enabled(it->cmd, it->flags)) {
                int cmd = it->cmd;
                ShowWindow(h, SW_HIDE);
                g_menuActive = -1;
                ReleaseCapture();
                SendMessageW(g_main, WM_COMMAND, cmd, 0);
                PostMessageW(g_main, WM_APP + 8, 0, 0);
                return 0;
            }
        }
        press = -1;
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            ShowWindow(h, SW_HIDE);
            g_menuActive = -1;
            PostMessageW(g_main, WM_APP + 8, 0, 0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void wui_menu_hover_switch(int sx, int sy)
{
    POINT pt;
    int i;
    if (g_menuActive < 0 || !g_main) return;
    pt.x = sx; pt.y = sy;
    ScreenToClient(g_main, &pt);
    for (i = 0; i < g_hotN; i++) {
        if (g_hot[i].kind != 2) continue;
        if (pt.x >= g_hot[i].r.left && pt.x < g_hot[i].r.right &&
            pt.y >= g_hot[i].r.top && pt.y < g_hot[i].r.bottom) {
            if (g_hot[i].aux != g_menuActive) {
                RECT r = g_hot[i].r;
                menu_open(g_hot[i].aux, r.left, r.bottom);
            }
            return;
        }
    }
}

static void menu_open(int idx, int x, int y)
{
    static int reg;
    int w, h;

    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = g_inst;
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"PdfStudioMenuW";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    menu_size(idx, &w, &h);
    if (x + w > g_cw - 2) x = g_cw - w - 2;
    if (y + h > g_ch - 2) y = g_ch - h - 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (!g_menuWnd)
        g_menuWnd = CreateWindowExW(0, L"PdfStudioMenuW", L"",
                                    WS_CHILD | WS_CLIPSIBLINGS, x, y, w, h,
                                    g_main, NULL, g_inst, NULL);
    if (!g_menuWnd) return;
    g_menuIndex = idx;
    g_menuActive = idx;
    SetWindowPos(g_menuWnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    InvalidateRect(g_menuWnd, NULL, TRUE);
    SetCapture(g_menuWnd);
    PostMessageW(g_main, WM_APP + 10, 0, 0);      /* спрятать подсказку */
    g_repaint = 1;
}

void wui_repaint_menu(void)
{
    if (g_menuWnd && IsWindowVisible(g_menuWnd)) {
        InvalidateRect(g_menuWnd, NULL, FALSE);
        UpdateWindow(g_menuWnd);
    }
}

void wui_close_backstage(void)
{
    if (g_menuWnd) ShowWindow(g_menuWnd, SW_HIDE);
    g_menuActive = -1;
    if (GetCapture() == g_menuWnd) ReleaseCapture();
    g_repaint = 1;
}

/* ---------- публичный интерфейс ---------- */
void wui_init(HWND main, HINSTANCE inst, int fontQuality)
{
    LOGFONTW lf;

    g_main = main;
    g_inst = inst;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -12;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = (BYTE)fontQuality;
    lstrcpyW(lf.lfFaceName, L"Tahoma");
    f_ui = CreateFontIndirectW(&lf);
    if (!f_ui) f_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    lf.lfHeight = -11;
    f_small = CreateFontIndirectW(&lf);

    lf.lfHeight = -12;
    lf.lfWeight = FW_BOLD;
    f_bold = CreateFontIndirectW(&lf);

    lf.lfHeight = -15;
    lf.lfWeight = FW_BOLD;
    f_title = CreateFontIndirectW(&lf);

    b_bg = CreateSolidBrush(XPBG);
}

void wui_set_title(const wchar_t *title)
{
    lstrcpynW(g_title, title ? title : L"", 512);
    g_repaint = 1;
}

void wui_set_status(const wchar_t *text) { lstrcpynW(g_status, text ? text : L"", 512); }
void wui_set_page(int page, int pages) { g_page = page; g_pages = pages; g_repaint = 1; }
void wui_set_zoom(float zoom) { g_zoom = zoom; g_repaint = 1; }
void wui_set_tool(int tool) { g_tool = tool; g_repaint = 1; }
void wui_set_open(int open) { g_open = open; g_repaint = 1; }
void wui_set_docmode(int on) { g_docmode = on; g_repaint = 1; }

void wui_set_pagenum(int on) { g_pgnum = on ? 1 : 0; g_repaint = 1; }

void wui_set_font(const wchar_t *shortName)
{
    if (!shortName) shortName = L"";
    lstrcpynW(g_fontShort, shortName, 24);
    g_repaint = 1;
}

void wui_set_format(int bold, int italic, int under, float size, unsigned color,
                    int align, int list, float spacing)
{
    g_fmtB = bold;
    g_fmtI = italic;
    g_fmtU = under;
    g_fmtSize = size;
    g_fmtColor = color;
    g_fmtAlign = align;
    g_fmtList = list;
    g_fmtSpacing = spacing;
    g_repaint = 1;
}
int  wui_docmode(void) { return g_docmode; }
void wui_set_dirty(int dirty) { g_dirty = dirty; g_repaint = 1; }
void wui_set_maximized(int m) { g_maximized = m; g_repaint = 1; }
void wui_set_tools(int on) { g_tools = on ? 1 : 0; g_repaint = 1; }
void wui_set_tab(int tab) { (void)tab; }
void wui_set_backstage(int on) { (void)on; }

void wui_layout(int clientW, int clientH)
{
    HDC dc = g_main ? GetDC(g_main) : CreateCompatibleDC(NULL);
    g_cw = clientW;
    g_ch = clientH;
    if (dc) {
        layout_all(dc);
        if (g_main) ReleaseDC(g_main, dc); else DeleteDC(dc);
    }
}

int wui_in_chrome(int x, int y)
{
    (void)x;
    return y < wui_chrome_height();
}

int wui_in_status(int x, int y)
{
    (void)x;
    return y >= g_ch - STAT_H;
}

int wui_caption_hittest(int x, int y)
{
    int i;
    for (i = 0; i < g_hotN; i++) {
        RECT r = g_hot[i].r;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            if (g_hot[i].kind == 4 || g_hot[i].kind == 2 || g_hot[i].kind == 1 ||
                g_hot[i].kind == 6 || g_hot[i].kind == 5 || g_hot[i].kind == 7)
                return HTCLIENT;
        }
    }
    if (y < CAP_H) return HTCAPTION;
    return HTCLIENT;
}

void wui_paint(HDC dc, int w, int h)
{
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ ob;

    g_cw = w;
    g_ch = h;

    /* двойная буферизация: всё рисуем в память, затем один BitBlt */
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, w, h);
    ob = SelectObject(mem, bmp);

    layout_all(mem);

    fill(mem, 0, 0, w, h, XPBG);
    paint_caption(mem);
    paint_menubar(mem);
    paint_toolbar(mem);
    paint_fmtbar(mem);
    paint_ruler(mem);
    paint_rail(mem);
    paint_tools_panel(mem);
    paint_status(mem);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int wui_mouse_move(int x, int y)
{
    int i;

    g_mouseX = x;
    g_mouseY = y;

    if (g_zoomDrag) {
        for (i = 0; i < g_hotN; i++) {
            if (g_hot[i].kind == 3 && g_hot[i].aux == 1) {
                RECT r = g_hot[i].r;
                int span = r.right - r.left;
                float t = (float)(x - r.left) / (float)(span > 0 ? span : 1);
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                g_zoom = (10.0f + t * 390.0f) / 100.0f;
                g_zoomPending = 1;
                g_repaint = 1;
                return 1;
            }
        }
        return 1;
    }

    g_repaintRuler = 0;
    i = hit_index(x, y);
    if (i != g_hover) {
        g_hover = i;
        g_repaint = 1;
        return 1;
    }
    if (y >= CAP_H + MENU_H + TB_H + fmt_h() &&
        y < CAP_H + MENU_H + TB_H + fmt_h() + RULER_H) {
        g_repaint = 1;                  /* курсор над линейкой */
        g_repaintRuler = 1;
        return 1;
    }
    return 0;
}

void wui_mouse_leave(void)
{
    if (g_hover != -1) { g_hover = -1; g_repaint = 1; }
    if (g_mouseY >= 0) { g_mouseX = g_mouseY = -1; g_repaint = 1; }
}

int wui_repaint_is_ruler(void) { return g_repaintRuler; }

int wui_zoom_pending(float *z)
{
    if (!g_zoomPending) return 0;
    g_zoomPending = 0;
    if (z) *z = g_zoom;
    return 1;
}

int wui_mouse_down(int x, int y, int *cmd)
{
    int i = hit_index(x, y);

    if (g_menuActive >= 0) {
        wui_close_backstage();
        return 1;
    }

    g_press = i;
    if (i < 0) return 0;

    switch (g_hot[i].kind) {
    case 2:                              /* заголовок меню */
    {
        RECT r = g_hot[i].r;
        menu_open(g_hot[i].aux, r.left, r.bottom);
        g_press = -1;
        return 1;
    }
    case 4:                              /* кнопки окна */
        *cmd = g_hot[i].aux == 0 ? WCMD_MINIMIZE
             : g_hot[i].aux == 1 ? WCMD_MAXIMIZE : WCMD_CLOSE;
        g_press = -1;
        return 1;
    case 3:
        if (g_hot[i].aux == 1) { g_zoomDrag = 1; wui_mouse_move(x, y); }
        else if (g_hot[i].cmd) *cmd = g_hot[i].cmd;
        return 1;
    case 7:                              /* поле процента */
        *cmd = IDC_ZOOM100;
        return 1;
    default:
        g_repaint = 1;
        return 1;
    }
}

int wui_mouse_up(int x, int y, int *cmd)
{
    int i = hit_index(x, y);

    if (g_zoomDrag) {
        g_zoomDrag = 0;
        g_press = -1;
        return 1;
    }

    if (g_press >= 0 && i == g_press) {
        Hot *hp = &g_hot[i];
        int c = hp->cmd;
        g_press = -1;
        g_repaint = 1;
        switch (hp->kind) {
        case 1: case 5: case 6:
            if (c && item_enabled(c, 0)) { *cmd = c; return 1; }
            return 1;
        case 2:
            return 1;
        default:
            break;
        }
        if (c) { *cmd = c; return 1; }
        return 1;
    }
    g_press = -1;
    g_repaint = 1;
    return 1;
}

const wchar_t *wui_tip_at(int x, int y)
{
    int i = hit_index(x, y);
    if (i < 0) return NULL;
    /* над строкой меню подсказок быть не должно: там только названия пунктов */
    if (g_hot[i].kind == 2) return NULL;
    return g_hot[i].tip;
}
