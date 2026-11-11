/* flow.h — текстовый процессор: PDF → поток абзацев → правка «как в Word» →
   заново свёрстанный PDF. Посимвольные стили (жирный/курсив/подчёркнутый,
   кегль, цвет), выравнивание, списки, интервалы, параметры страницы. */
#ifndef FLOW_H
#define FLOW_H

#include <windows.h>
#include "pdfdoc.h"

/* формат в каретке / выделении — для панели форматирования */
typedef struct {
    float    size;        /* кегль */
    unsigned color;       /* 0xRRGGBB */
    int      bold, italic, underline;
    int      font;        /* семейство: 0 Times, 1 Arial, 2 Courier, 3 Узкий */
    int      align;       /* 0 слева, 1 по центру, 2 справа, 3 по ширине */
    int      list;        /* 0 нет, 1 маркеры, 2 нумерация */
    float    spacing;     /* межстрочный: 1.0, 1.15, 1.5, 2.0 */
    float    indent;      /* красная строка, пт */
    int      hasSel;
} FlowFmt;

void  flow_reset(void);
int   flow_build(PdfDoc *d);
int   flow_ready(void);

int   flow_pages(void);
float flow_page_w(void);
float flow_page_h(void);

void  flow_paint(HDC dc, int page, float scale, int ox, int oy);
int   flow_render(PdfDoc *d, int page, float scale, HBITMAP *out, int *ow, int *oh);

/* мышь */
void  flow_click(int page, float pageX, float pageY, int shift);
void  flow_drag(float pageX, float pageY);
int   flow_dragging(void);
void  flow_drag_end(void);

/* клавиатура */
void  flow_char(int ch);
void  flow_vkey(int vk, int ctrl, int shift);
void  flow_select_all(void);
int   flow_has_selection(void);
void  flow_paste_text(const char *utf8);
char *flow_selected_utf8(void);

/* сохранение документа */
int   flow_is_dirty(void);
void  flow_mark_clean(void);
int   flow_caret_page(void);
int   flow_caret_box(float *x, float *y, float *w, float *h);
int   flow_export(PdfDoc *d, const wchar_t *wpath);
void  flow_stats(int *pages, int *paras, int *chars);
int   flow_words(void);

/* ---------- форматирование ---------- */
void  flow_fmt_get(FlowFmt *f);
int   flow_fmt_list(void);             /* 0 нет, 1 маркеры, 2 нумерация */
void  flow_fmt_attr(int bit);          /* 1 жирный, 2 курсив, 4 подчёркнутый */
void  flow_fmt_size(float absSize);     /* 0 — изменить на шаг (+2 / −2 по delta) */
void  flow_fmt_size_step(int dir);
void  flow_fmt_color(unsigned c);
void  flow_fmt_font(int fam);
const wchar_t *flow_font_name(int fam);           /* 0 Times, 1 Arial, 2 Courier, 3 Узкий */
void  flow_align(int a);
void  flow_list(int kind);              /* 0 снять, 1 маркеры, 2 нумерация */
void  flow_spacing_next(void);
void  flow_indent(int dir);             /* красная строка ±12 пт */
void  flow_space_before(int dir);       /* интервал перед абзацем ±6 пт */

/* ---------- параметры страницы ---------- */
void  flow_page_metrics(float *ml, float *mt, float *mr, float *mb, int *landscape, int *sizeIdx);
void  flow_page_setup(int sizeIdx, int landscape);   /* 0 A4, 1 Letter, 2 A5 */
void  flow_margins_mm(float ml, float mt, float mr, float mb);

/* ---------- колонтитулы и нумерация ---------- */
void  flow_set_header(const char *utf8);   /* NULL/"" — убрать */
void  flow_set_footer(const char *utf8);
void  flow_set_pagenum(int on);
int   flow_get_pagenum(void);
char *flow_get_header(void);
char *flow_get_footer(void);

/* ---------- таблицы ---------- */
int   flow_insert_table(int rows, int cols);
int   flow_table_row_add(void);
int   flow_table_col_add(void);
int   flow_table_delete(void);
int   flow_in_table(void);
void  flow_table_next_cell(void);

/* ---------- картинки ---------- */
int   flow_insert_image(const wchar_t *wpath);

/* ---------- поиск и замена ---------- */
int   flow_find_next(const char *utf8, int fromStart);
int   flow_replace_all(const char *utf8find, const char *utf8repl);
int   flow_replace_one(const char *utf8find, const char *utf8repl);

#endif
