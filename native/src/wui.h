/* wui.h — оформление в стиле Windows XP (Luna).
 * Всё рисуется вручную на GDI (с двойной буферизацией), поэтому вид одинаков
 * на Windows XP … 11: синий заголовок с кнопками окна, строка меню,
 * панель инструментов, линейка, боковая панель и строка состояния. */
#ifndef WUI_H
#define WUI_H

#include <windows.h>

/* команды кнопок окна */
#define WCMD_NONE        0
#define WCMD_MINIMIZE    9001
#define WCMD_MAXIMIZE    9002
#define WCMD_CLOSE       9003
#define WCMD_FILEMENU    9004
#define WCMD_ZOOM100     9005
#define WCMD_SYSICON     9006

void  wui_init(HWND main, HINSTANCE inst, int fontQuality);
void  wui_set_title(const wchar_t *title);
void  wui_set_status(const wchar_t *text);
void  wui_set_page(int page, int pages);
void  wui_set_zoom(float zoom);
void  wui_set_tool(int tool);
void  wui_set_open(int open);
void  wui_set_dirty(int dirty);
void  wui_set_maximized(int maximized);

void  wui_layout(int clientW, int clientH);
int   wui_chrome_height(void);          /* заголовок + меню + панель + линейка */
int   wui_status_height(void);
RECT  wui_view_rect(void);              /* область страницы */
RECT  wui_search_rect(void);            /* поле поиска на панели инструментов */
RECT  wui_page_rect(void);              /* поле «номер страницы» на панели */
RECT  wui_ruler_rect(void);             /* полоса линейки (для точечной перерисовки) */

/* линейка: левый край и ширина страницы в клиентских координатах */
void  wui_set_ruler(int pageX, int pageW);
void  wui_set_caret(int x);             /* каретка текста (клиентские координаты), -1 — нет */

void  wui_paint(HDC dc, int w, int h);
int   wui_mouse_move(int x, int y);     /* 1 — нужна перерисовка */
int   wui_repaint_is_ruler(void);       /* перерисовка нужна только линейке */
void  wui_mouse_leave(void);
int   wui_mouse_down(int x, int y, int *cmd);
int   wui_mouse_up(int x, int y, int *cmd);
const wchar_t *wui_tip_at(int x, int y);
int   wui_in_chrome(int x, int y);
int   wui_in_status(int x, int y);
int   wui_caption_hittest(int x, int y); /* HTCAPTION / HTCLIENT / … */

int   wui_menu_open(void);              /* открыто ли ниспадающее меню */
void  wui_close_backstage(void);        /* закрыть меню (Esc) */
void  wui_menu_hover_switch(int screenX, int screenY); /* переход к соседнему меню */
void  wui_repaint_menu(void);           /* перерисовать меню поверх окна */
int   wui_consume_repaint(void);
void  wui_zoom_geometry(int pageX, int pageW);
int   wui_zoom_pending(float *zoom);
int   wui_tools_visible(void);
void  wui_set_view_mode(int mode);       /* 0 одна, 1 непрерывно, 2 две, 3 несколько */
int   wui_view_mode(void);
void  wui_set_tools(int on);
void  wui_set_docmode(int on);
void  wui_set_format(int bold, int italic, int under, float size, unsigned color,
                     int align, int list, float spacing);
void  wui_set_font(const wchar_t *shortName);   /* имя семейства на кнопке «Шрифт» */
void  wui_set_pagenum(int on);
int   wui_docmode(void);

/* совместимость со старым кодом */
void  wui_set_tab(int tab);
void  wui_set_backstage(int on);

#endif
