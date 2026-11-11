/* textedit.h — правка текста на странице (каретка, выделение, переносы) */
#ifndef TEXTEDIT_H
#define TEXTEDIT_H

#include <windows.h>
#include "pdfdoc.h"

void  te_set_doc(PdfDoc *d);
int   te_is_active(void);
int   te_active_page(void);
/* начать редактирование блока под точкой (координаты страницы) */
int   te_begin(PdfDoc *d, int page, float pageX, float pageY, float scale, int ox, int oy);
int   te_commit(void);            /* сохранить правку в список */
void  te_cancel(void);            /* отменить редактирование блока */
char *te_text_utf8(void);         /* текущий текст (malloc) */

int   te_insert(int ch);
int   te_delete(int back);
int   te_undo(void);
void  te_select_all(void);
int   te_move_h(int dc, int extend);
int   te_move_v(int dc, int extend);
int   te_home_end(int end, int whole, int extend);
void  te_place_caret(float pageX, float pageY, int extend);
int   te_hit_block(float pageX, float pageY);
void  te_set_origin(float scale, int ox, int oy);

/* цвет фона под блоком — спрашиваем у хоста (страница может быть не белой) */
typedef struct {
    COLORREF c[4];   /* цвета фона у углов блока: ЛВ, ПВ, ЛН, ПН */
    int      uniform;/* 1 — фон однородный */
} TeBgInfo;
typedef void (*TeBgFn)(int page, const fz_rect *box, TeBgInfo *out);
void  te_set_bg_provider(TeBgFn fn);

void  te_paint(HDC dc);
void  te_paint_edits(HDC dc, int page, float scale, int ox, int oy);

int   te_apply_all(PdfDoc *d);    /* запечь все правки в PDF (перед сохранением) */
int   te_edit_count(void);
void  te_edit_clear(void);
void  te_edit_drop_page(int page);

#endif

void te_bg_cache_reset(void);
