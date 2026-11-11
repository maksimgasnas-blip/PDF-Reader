/* Работа с PDF через MuPDF: рендер в DIB, поиск, аннотации, сохранение */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pdfdoc.h"

/* Диагностика: в релизе — только в отладчик, файлов не создаём. */
static void logerr(const char *where, const char *msg)
{
#ifdef PDFSTUDIO_DEBUG
    char buf[1024];
    wsprintfA(buf, "%s: %s", where, msg ? msg : "(null)");
    OutputDebugStringA(buf);
    OutputDebugStringA("\r\n");
#else
    (void)where; (void)msg;
#endif
}

/* ---------- кодировки ---------- */
static char *wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0) return NULL;
    s = (char *)malloc(n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}


/* ---------- пути с не-ASCII символами ---------- */
int pdfdoc_path_ascii(const wchar_t *path)
{
    if (!path) return 1;
    for (; *path; path++)
        if (*path > 126) return 0;
    return 1;
}

int pdfdoc_temp_path(wchar_t *out, int outCount, const wchar_t *ext)
{
    wchar_t dir[MAX_PATH];
    if (!out || outCount < 32) return 0;
    if (GetTempPathW(MAX_PATH - 16, dir) == 0) lstrcpynW(dir, L"C:\\", MAX_PATH);
    wsprintfW(out, L"%spdfstudio-%u-%u%s", dir,
              (unsigned)GetCurrentProcessId(), (unsigned)GetTickCount(),
              ext ? ext : L".pdf");
    return lstrlenW(out) < outCount;
}

/* перенос файла на место (в т.ч. между дисками) */
int pdfdoc_move_file(const wchar_t *src, const wchar_t *dst)
{
    if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING)) return 1;
    if (!CopyFileW(src, dst, FALSE)) return 0;
    DeleteFileW(src);
    return 1;
}

static void warn_cb(void *user, const char *message)
{
    (void)user; (void)message;   /* тихо: MuPDF бывает болтлив на битых файлах */
}

/* ---------- открытие/закрытие ---------- */
int pdfdoc_open(PdfDoc *d, const wchar_t *wpath)
{
    char *path;
    int err = 0;
    wchar_t src[MAX_PATH];

    memset(d, 0, sizeof(*d));
    lstrcpynW(src, wpath, MAX_PATH);
    path = wide_to_utf8(src);
    if (!path) return PDF_ERR;

    d->ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!d->ctx) { free(path); return PDF_ERR; }
    fz_set_warning_callback(d->ctx, warn_cb, NULL);
    fz_set_error_callback(d->ctx, warn_cb, NULL);

    fz_try(d->ctx)
        fz_register_document_handlers(d->ctx);
    fz_catch(d->ctx)
        err = 1;
    if (err) { free(path); pdfdoc_close(d); return PDF_ERR; }

    fz_try(d->ctx)
        d->doc = fz_open_document(d->ctx, path);
    fz_catch(d->ctx)
        err = 1;
    if ((err || !d->doc) && !pdfdoc_path_ascii(wpath)) {
        /* путь с русскими буквами не открылся — копируем во временный файл */
        wchar_t tmp[MAX_PATH];
        free(path);
        path = NULL;
        if (pdfdoc_temp_path(tmp, MAX_PATH, L".pdf")) {
            DeleteFileW(tmp);
            if (CopyFileW(wpath, tmp, FALSE)) {
                lstrcpynW(src, tmp, MAX_PATH);
                path = wide_to_utf8(src);
                err = 0;
                if (path) {
                    fz_try(d->ctx)
                        d->doc = fz_open_document(d->ctx, path);
                    fz_catch(d->ctx)
                        err = 1;
                } else err = 1;
            }
        }
    }
    if (err || !d->doc) { free(path); pdfdoc_close(d); return PDF_ERR; }

    d->path = path;
    if (lstrcmpiW(src, wpath) != 0) lstrcpynW(d->tempCopy, src, MAX_PATH);

    fz_try(d->ctx)
        err = fz_needs_password(d->ctx, d->doc) ? 2 : 0;
    fz_catch(d->ctx)
        err = 1;
    if (err == 2) return PDF_NEED_PASSWORD;
    if (err) return PDF_ERR;

    fz_try(d->ctx)
        d->pdf = pdf_specifics(d->ctx, d->doc);
    fz_catch(d->ctx) { }
    return PDF_OK;
}

int pdfdoc_auth(PdfDoc *d, const char *password)
{
    int ok = 0;
    if (!d || !d->doc) return 0;
    fz_try(d->ctx)
        ok = fz_authenticate_password(d->ctx, d->doc, password);
    fz_catch(d->ctx)
        ok = 0;
    if (ok) {
        fz_try(d->ctx)
            d->pdf = pdf_specifics(d->ctx, d->doc);
        fz_catch(d->ctx) { d->pdf = NULL; }
    }
    return ok;
}

void pdfdoc_close(PdfDoc *d)
{
    if (!d) return;
    if (d->tempCopy[0]) {
        DeleteFileW(d->tempCopy);
        d->tempCopy[0] = 0;
    }
    if (d->ctx) {
        if (d->ui_font) fz_drop_font(d->ctx, d->ui_font);
        {
            int fi, fj;
            for (fi = 0; fi < 3; fi++)
                for (fj = 0; fj < 4; fj++)
                    if (d->base_font[fi][fj]) {
                        fz_drop_font(d->ctx, d->base_font[fi][fj]);
                        d->base_font[fi][fj] = NULL;
                    }
        }
        d->metrics_font = NULL;
        if (d->doc) fz_drop_document(d->ctx, d->doc);
        fz_drop_context(d->ctx);
    }
    if (d->path) free(d->path);
    memset(d, 0, sizeof(*d));
}

int pdfdoc_pages(PdfDoc *d)
{
    int n = 0;
    if (!d || !d->doc) return 0;
    fz_try(d->ctx)
        n = fz_count_pages(d->ctx, d->doc);
    fz_catch(d->ctx)
        n = 0;
    return n;
}

void pdfdoc_page_size(PdfDoc *d, int page, float *w, float *h)
{
    fz_page *pg = NULL;
    fz_rect r = fz_empty_rect;
    *w = *h = 0;
    fz_var(pg);
    fz_try(d->ctx) {
        pg = fz_load_page(d->ctx, d->doc, page);
        r = fz_bound_page(d->ctx, pg);
    }
    fz_catch(d->ctx) { }
    if (pg) fz_drop_page(d->ctx, pg);
    *w = r.x1 - r.x0;
    *h = r.y1 - r.y0;
}

/* ---------- рендер ---------- */
int pdfdoc_render(PdfDoc *d, int page, float scale, HBITMAP *out, int *ow, int *oh)
{
    fz_page *pg = NULL;
    fz_pixmap *pix = NULL;
    fz_matrix ctm;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp = NULL;
    int y, w = 0, h = 0, stride, ok = 0;

    fz_var(pg);
    fz_var(pix);
    fz_var(ok);

    if (!d || !d->doc || !out) return 0;

    fz_try(d->ctx) {
        pg = fz_load_page(d->ctx, d->doc, page);
        ctm = fz_scale(scale, scale);
        pix = fz_new_pixmap_from_page(d->ctx, pg, ctm, fz_device_bgr(d->ctx), 0);
        w = pix->w;
        h = pix->h;
        ok = 1;
    }
    fz_catch(d->ctx)
        ok = 0;

    if (!ok || !pix || w <= 0 || h <= 0) {
        if (pix) fz_drop_pixmap(d->ctx, pix);
        if (pg) fz_drop_page(d->ctx, pg);
        return 0;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;              /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);

    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        fz_drop_pixmap(d->ctx, pix);
        fz_drop_page(d->ctx, pg);
        return 0;
    }

    stride = ((w * 3) + 3) & ~3;
    for (y = 0; y < h; y++) {
        memcpy((unsigned char *)bits + y * stride,
               pix->samples + (size_t)y * pix->stride,
               (size_t)w * 3);
    }

    fz_drop_pixmap(d->ctx, pix);
    fz_drop_page(d->ctx, pg);

    *out = bmp;
    if (ow) *ow = w;
    if (oh) *oh = h;
    return 1;
}

/* ---------- поиск ---------- */
int pdfdoc_search_page(PdfDoc *d, int page, const char *needle, fz_rect *hits, int max)
{
    fz_page *pg = NULL;
    fz_quad quads[256];
    int marks[256];
    int n = 0, i, ok = 0;

    fz_var(pg);
    fz_var(ok);
    if (!d || !d->doc || !needle || !needle[0] || max <= 0) return 0;
    if (max > 256) max = 256;

    fz_try(d->ctx) {
        pg = fz_load_page(d->ctx, d->doc, page);
        n = fz_search_page(d->ctx, pg, needle, marks, quads, max);
        ok = 1;
    }
    fz_catch(d->ctx)
        ok = 0;
    if (!ok) n = 0;
    for (i = 0; i < n && hits; i++) hits[i] = fz_rect_from_quad(quads[i]);
    if (pg) fz_drop_page(d->ctx, pg);
    return n;
}

/* ---------- аннотации ---------- */
int pdfdoc_annot_add(PdfDoc *d, int page, int kind, fz_rect r,
                     const fz_point *pts, int npts, const char *text)
{
    pdf_page *pp = NULL;
    pdf_annot *a = NULL;
    int ok = 0;

    fz_var(pp);
    fz_var(a);
    fz_var(ok);

    if (!d || !d->pdf) return 0;

    fz_try(d->ctx) {
        pp = pdf_load_page(d->ctx, d->pdf, page);
        switch (kind) {
        case ANNO_HIGHLIGHT: {
            fz_quad q = fz_quad_from_rect(r);
            float col[3] = { 1.0f, 0.85f, 0.15f };
            a = pdf_create_annot(d->ctx, pp, PDF_ANNOT_HIGHLIGHT);
            /* у text-markup аннотаций нет /Rect: рамка вычисляется из quad points */
            pdf_set_annot_quad_points(d->ctx, a, 1, &q);
            pdf_set_annot_color(d->ctx, a, 3, col);
            if (text && text[0]) pdf_set_annot_contents(d->ctx, a, text);
            break;
        }
        case ANNO_RECT: {
            float col[3] = { 0.85f, 0.1f, 0.1f };
            a = pdf_create_annot(d->ctx, pp, PDF_ANNOT_SQUARE);
            pdf_set_annot_rect(d->ctx, a, r);
            pdf_set_annot_color(d->ctx, a, 3, col);
            pdf_set_annot_border_width(d->ctx, a, 1.5f);
            if (text && text[0]) pdf_set_annot_contents(d->ctx, a, text);
            break;
        }
        case ANNO_FREETEXT: {
            float col[3] = { 0.0f, 0.0f, 0.0f };
            a = pdf_create_annot(d->ctx, pp, PDF_ANNOT_FREE_TEXT);
            pdf_set_annot_rect(d->ctx, a, r);
            pdf_set_annot_contents(d->ctx, a, text ? text : "");
            pdf_set_annot_default_appearance(d->ctx, a, "Helv", 12, 3, col);
            break;
        }
        case ANNO_INK: {
            float col[3] = { 0.9f, 0.15f, 0.15f };
            a = pdf_create_annot(d->ctx, pp, PDF_ANNOT_INK);
            /* у Ink рамка тоже вычисляется из штрихов, /Rect вручную не ставим */
            pdf_set_annot_color(d->ctx, a, 3, col);
            pdf_set_annot_border_width(d->ctx, a, 1.5f);
            if (pts && npts > 1)
                pdf_add_annot_ink_list(d->ctx, a, npts, (fz_point *)pts);
            if (text && text[0]) pdf_set_annot_contents(d->ctx, a, text);
            break;
        }
        case ANNO_NOTE:
        default: {
            a = pdf_create_annot(d->ctx, pp, PDF_ANNOT_TEXT);
            pdf_set_annot_rect(d->ctx, a, r);
            pdf_set_annot_contents(d->ctx, a, text ? text : "");
            break;
        }
        }
        if (a) {
            pdf_set_annot_author(d->ctx, a, "PDF Studio");
            pdf_update_annot(d->ctx, a);
        }
        pdf_update_page(d->ctx, pp);
        ok = 1;
    }
    fz_catch(d->ctx)
        { ok = 0; logerr("annot_add", fz_caught_message(d->ctx)); }

    if (pp) {
        fz_try(d->ctx) { pdf_drop_page(d->ctx, pp); } fz_catch(d->ctx) { }
    }
    if (ok) d->dirty = 1;
    return ok;
}

int pdfdoc_annot_delete_at(PdfDoc *d, int page, fz_point pt)
{
    pdf_page *pp = NULL;
    pdf_annot *a;
    int found = 0;

    fz_var(pp);
    fz_var(found);
    if (!d || !d->pdf) return 0;

    fz_try(d->ctx) {
        pp = pdf_load_page(d->ctx, d->pdf, page);
        for (a = pdf_first_annot(d->ctx, pp); a; a = pdf_next_annot(d->ctx, a)) {
            fz_rect r = pdf_bound_annot(d->ctx, a);
            fz_rect rr = fz_expand_rect(r, 4.0f);
            if (fz_is_point_inside_rect(pt, rr)) {
                pdf_delete_annot(d->ctx, pp, a);
                found = 1;
                break;
            }
        }
        if (found) pdf_update_page(d->ctx, pp);
    }
    fz_catch(d->ctx)
        found = 0;
    if (pp) {
        fz_try(d->ctx) { pdf_drop_page(d->ctx, pp); } fz_catch(d->ctx) { }
    }
    if (found) d->dirty = 1;
    return found;
}

int pdfdoc_page_has_annots(PdfDoc *d, int page)
{
    pdf_page *pp = NULL;
    int n = 0;
    fz_var(pp);
    if (!d || !d->pdf) return 0;
    fz_try(d->ctx) {
        pp = pdf_load_page(d->ctx, d->pdf, page);
        n = pdf_first_annot(d->ctx, pp) ? 1 : 0;
    }
    fz_catch(d->ctx)
        n = 0;
    if (pp) {
        fz_try(d->ctx) { pdf_drop_page(d->ctx, pp); } fz_catch(d->ctx) { }
    }
    return n;
}

/* ---------- текст страницы ---------- */
char *pdfdoc_page_text(PdfDoc *d, int page)
{
    fz_page *pg = NULL;
    fz_stext_page *tp = NULL;
    fz_buffer *buf = NULL;
    char *out = NULL;
    int ok = 0;

    fz_var(pg);
    fz_var(tp);
    fz_var(buf);
    fz_var(ok);

    if (!d || !d->doc) return NULL;

    fz_try(d->ctx) {
        pg = fz_load_page(d->ctx, d->doc, page);
        tp = fz_new_stext_page_from_page(d->ctx, pg, NULL);
        buf = fz_new_buffer(d->ctx, 1024);
        {
            fz_output *out = fz_new_output_with_buffer(d->ctx, buf);
            fz_print_stext_page_as_text(d->ctx, out, tp);
            fz_close_output(d->ctx, out);
            fz_drop_output(d->ctx, out);
        }
        out = (char *)malloc(buf->len + 1);
        if (out) {
            memcpy(out, buf->data, buf->len);
            out[buf->len] = 0;
        }
        ok = 1;
    }
    fz_catch(d->ctx)
        ok = 0;

    if (buf) fz_drop_buffer(d->ctx, buf);
    if (tp) fz_drop_stext_page(d->ctx, tp);
    if (pg) fz_drop_page(d->ctx, pg);
    if (!ok && out) { free(out); out = NULL; }
    return out;
}

/* ---------- операции со страницами ---------- */
int pdfdoc_page_rotate(PdfDoc *d, int page, int delta)
{
    pdf_obj *obj;
    int rot, ok = 0;

    if (!d || !d->pdf) return 0;
    fz_try(d->ctx) {
        obj = pdf_lookup_page_obj(d->ctx, d->pdf, page);
        if (!obj) fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "нет такой страницы");
        rot = pdf_dict_get_inheritable_int(d->ctx, obj, PDF_NAME(Rotate));
        rot = ((rot + delta) % 360 + 360) % 360;
        pdf_dict_put_int(d->ctx, obj, PDF_NAME(Rotate), rot);
        ok = 1;
    }
    fz_catch(d->ctx) { ok = 0; logerr("page_rotate", fz_caught_message(d->ctx)); }
    if (ok) d->dirty = 1;
    return ok;
}

int pdfdoc_page_delete(PdfDoc *d, int page)
{
    int ok = 0, n;

    if (!d || !d->pdf) return 0;
    n = fz_count_pages(d->ctx, d->doc);
    if (n <= 1 || page < 0 || page >= n) return 0;
    fz_try(d->ctx) { pdf_delete_page(d->ctx, d->pdf, page); ok = 1; }
    fz_catch(d->ctx) { ok = 0; logerr("page_delete", fz_caught_message(d->ctx)); }
    if (ok) d->dirty = 1;
    return ok;
}

int pdfdoc_page_duplicate(PdfDoc *d, int page)
{
    int ok = 0, n;

    if (!d || !d->pdf) return 0;
    n = fz_count_pages(d->ctx, d->doc);
    if (page < 0 || page >= n) return 0;
    fz_try(d->ctx) { pdf_graft_page(d->ctx, d->pdf, page + 1, d->pdf, page); ok = 1; }
    fz_catch(d->ctx) { ok = 0; logerr("page_duplicate", fz_caught_message(d->ctx)); }
    if (ok) d->dirty = 1;
    return ok;
}

int pdfdoc_page_move(PdfDoc *d, int page, int delta)
{
    int ok = 0, n;

    if (!d || !d->pdf || delta == 0) return 0;
    n = fz_count_pages(d->ctx, d->doc);
    if (page < 0 || page >= n) return 0;
    if (delta > 0 && page + 1 >= n) return 0;
    if (delta < 0 && page - 1 < 0) return 0;
    fz_try(d->ctx) {
        if (delta > 0) {
            pdf_graft_page(d->ctx, d->pdf, page + 2, d->pdf, page);
            pdf_delete_page(d->ctx, d->pdf, page);
        } else {
            pdf_graft_page(d->ctx, d->pdf, page - 1, d->pdf, page);
            pdf_delete_page(d->ctx, d->pdf, page + 1);
        }
        ok = 1;
    }
    fz_catch(d->ctx) { ok = 0; logerr("page_move", fz_caught_message(d->ctx)); }
    if (ok) d->dirty = 1;
    return ok;
}

/* ---------- поля формы ---------- */
static pdf_obj *field_of(fz_context *ctx, pdf_annot *w)
{
    pdf_obj *o = pdf_annot_obj(ctx, w);
    pdf_obj *p = o ? pdf_dict_get(ctx, o, PDF_NAME(Parent)) : NULL;
    return p ? p : o;
}

static void copy_str(char *dst, int dstlen, const char *src)
{
    if (!dst || dstlen <= 0) return;
    if (!src) { dst[0] = 0; return; }
    lstrcpynA(dst, src, dstlen);
}

/* первый ключ /AP /N — имя включённого состояния флажка/переключателя */
static void widget_on_name(fz_context *ctx, pdf_annot *w, char *out, int outlen)
{
    pdf_obj *ap, *n, *key;
    out[0] = 0;
    ap = pdf_dict_get(ctx, pdf_annot_obj(ctx, w), PDF_NAME(AP));
    n = ap ? pdf_dict_get(ctx, ap, PDF_NAME(N)) : NULL;
    key = n ? pdf_dict_get_key(ctx, n, 0) : NULL;
    if (key) copy_str(out, outlen, pdf_to_name(ctx, key));
}

/* найти поле под точкой; страница остаётся загруженной у вызывающего */
static pdf_annot *widget_find(PdfDoc *d, pdf_page *pp, fz_point pt)
{
    pdf_annot *w;
    for (w = pdf_first_widget(d->ctx, pp); w; w = pdf_next_widget(d->ctx, w)) {
        fz_rect r = pdf_bound_widget(d->ctx, w);
        if (pt.x >= r.x0 && pt.x <= r.x1 && pt.y >= r.y0 && pt.y <= r.y1)
            return w;
    }
    return NULL;
}

int pdfdoc_widget_at(PdfDoc *d, int page, fz_point pt, PdfWidget *out)
{
    pdf_page *pp = NULL;
    int found = 0;

    if (!d || !d->pdf || !out) return 0;
    out->type = 0; out->readonly = 0;
    out->name[0] = out->value[0] = out->on[0] = 0;

    fz_try(d->ctx) {
        pdf_annot *w;
        pdf_obj *f;
        pp = pdf_load_page(d->ctx, d->pdf, page);
        w = widget_find(d, pp, pt);
        if (w) {
            f = field_of(d->ctx, w);
            out->type = (int)pdf_widget_type(d->ctx, w);
            out->readonly = pdf_widget_is_readonly(d->ctx, w);
            copy_str(out->name, sizeof(out->name),
                     f ? pdf_dict_get_text_string(d->ctx, f, PDF_NAME(T)) : NULL);
            copy_str(out->value, sizeof(out->value), pdf_annot_field_value(d->ctx, w));
            widget_on_name(d->ctx, w, out->on, sizeof(out->on));
            found = 1;
        }
    }
    fz_catch(d->ctx) { found = 0; logerr("widget_at", fz_caught_message(d->ctx)); }

    if (pp) { fz_try(d->ctx) { pdf_drop_page(d->ctx, pp); } fz_catch(d->ctx) { } }
    return found;
}

int pdfdoc_widget_set(PdfDoc *d, int page, fz_point pt, const char *utf8)
{
    pdf_page *pp = NULL;
    int ok = 0;

    if (!d || !d->pdf) return 0;
    fz_try(d->ctx) {
        pdf_annot *w;
        pdf_obj *f;
        pp = pdf_load_page(d->ctx, d->pdf, page);
        w = widget_find(d, pp, pt);
        if (!w) fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "под точкой нет поля");
        f = field_of(d->ctx, w);
        if (!f) fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "у поля нет словаря");
        if (pdf_widget_is_readonly(d->ctx, w))
            fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "поле только для чтения");

        if (utf8) {
            pdf_set_field_value(d->ctx, d->pdf, f, utf8, 1);
        } else {
            /* переключить флажок/переключатель */
            char on[128];
            const char *cur;
            int same;
            widget_on_name(d->ctx, w, on, sizeof(on));
            if (!on[0]) fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "неизвестное состояние поля");
            cur = pdf_annot_field_value(d->ctx, w);
            same = cur && !lstrcmpA(cur, on);
            pdf_set_field_value(d->ctx, d->pdf, f, same ? "Off" : on, 1);
        }
        pdf_update_page(d->ctx, pp);
        ok = 1;
    }
    fz_catch(d->ctx) { ok = 0; logerr("widget_set", fz_caught_message(d->ctx)); }

    if (pp) { fz_try(d->ctx) { pdf_drop_page(d->ctx, pp); } fz_catch(d->ctx) { } }
    if (ok) d->dirty = 1;
    return ok;
}

/* ---------- выделение текста прямоугольником ---------- */
char *pdfdoc_text_in_rect(PdfDoc *d, int page, fz_rect sel,
                          fz_rect *boxes, int maxBoxes, int *nBoxes)
{
    fz_page *pg = NULL;
    fz_stext_page *tp = NULL;
    fz_buffer *buf = NULL;
    char *out = NULL;
    int ok = 0;

    if (nBoxes) *nBoxes = 0;
    if (!d || !d->doc) return NULL;

    fz_var(pg);
    fz_var(tp);
    fz_var(buf);
    fz_var(ok);

    fz_try(d->ctx) {
        fz_stext_block *b;
        fz_stext_line *ln;
        int boxes_n = 0;

        pg = fz_load_page(d->ctx, d->doc, page);
        tp = fz_new_stext_page_from_page(d->ctx, pg, NULL);
        buf = fz_new_buffer(d->ctx, 256);

        for (b = tp->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (ln = b->u.t.first_line; ln; ln = ln->next) {
                fz_stext_char *ch;
                fz_rect lineBox = fz_empty_rect;
                int lineTouched = 0;

                for (ch = ln->first_char; ch; ch = ch->next) {
                    fz_rect cb = fz_rect_from_quad(ch->quad);
                    float cx = (cb.x0 + cb.x1) * 0.5f;
                    float cy = (cb.y0 + cb.y1) * 0.5f;

                    if (cx >= sel.x0 && cx <= sel.x1 && cy >= sel.y0 && cy <= sel.y1) {
                        char utf8[8];
                        int n = fz_runetochar(utf8, ch->c);
                        fz_append_data(d->ctx, buf, utf8, (size_t)n);
                        lineBox = fz_union_rect(lineBox, cb);
                        lineTouched = 1;
                    }
                }
                if (lineTouched) {
                    fz_append_byte(d->ctx, buf, '\n');
                    if (boxes && boxes_n < maxBoxes) boxes[boxes_n++] = lineBox;
                }
            }
        }
        if (nBoxes) *nBoxes = boxes_n;

        out = (char *)malloc(buf->len + 1);
        if (out) {
            memcpy(out, buf->data, buf->len);
            out[buf->len] = 0;
        }
        ok = 1;
    }
    fz_catch(d->ctx) { ok = 0; logerr("text_in_rect", fz_caught_message(d->ctx)); }

    if (buf) fz_drop_buffer(d->ctx, buf);
    if (tp) fz_drop_stext_page(d->ctx, tp);
    if (pg) fz_drop_page(d->ctx, pg);
    if (!ok && out) { free(out); out = NULL; }
    return out;
}

/* ---------- сохранение ---------- */
int pdfdoc_save(PdfDoc *d, const wchar_t *wpath)
{
    char *path;
    pdf_write_options opts;
    int ok = 1;
    wchar_t target[MAX_PATH];

    if (!d || !d->pdf) return 0;
    lstrcpynW(target, wpath, MAX_PATH);
    path = wide_to_utf8(target);
    if (!path) return 0;

    opts = pdf_default_write_options;
    opts.do_compress = 1;
    opts.do_compress_images = 1;
    opts.do_compress_fonts = 1;
    opts.do_garbage = 1;

    fz_try(d->ctx)
        pdf_save_document(d->ctx, d->pdf, path, &opts);
    fz_catch(d->ctx)
        ok = 0;

    free(path);
    if (!ok && !pdfdoc_path_ascii(wpath)) {
        /* не-ASCII имя не записалось напрямую — пишем во временный файл */
        wchar_t tmp[MAX_PATH];
        if (pdfdoc_temp_path(tmp, MAX_PATH, L".pdf")) {
            char *t8;
            DeleteFileW(tmp);
            t8 = wide_to_utf8(tmp);
            if (t8) {
                ok = 1;
                fz_try(d->ctx)
                    pdf_save_document(d->ctx, d->pdf, t8, &opts);
                fz_catch(d->ctx)
                    ok = 0;
                free(t8);
                if (ok) {
                    ok = pdfdoc_move_file(tmp, wpath);
                    if (!ok) DeleteFileW(tmp);
                }
            }
        }
    }
    if (ok) d->dirty = 0;
    return ok;
}

/* ---------- рендер страницы БЕЗ текста (чистый фон) ---------- */
/* Устройство-прокси: глушит все текстовые операции, остальное отдаёт
   настоящему draw-устройству. Нужно, чтобы в предпросмотре под правкой
   показывать настоящий фон страницы, а не заливку-«заплатку». */
typedef struct {
    fz_device super;
    fz_device *target;
} pdf_NoTextDev;

static void nt_fill_path(fz_context *ctx, fz_device *dev, const fz_path *p, int eo, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->fill_path) s->target->fill_path(ctx, s->target, p, eo, m, cs, c, a, cp); }
static void nt_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *p, const fz_stroke_state *st, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->stroke_path) s->target->stroke_path(ctx, s->target, p, st, m, cs, c, a, cp); }
static void nt_clip_path(fz_context *ctx, fz_device *dev, const fz_path *p, int eo, fz_matrix m, fz_rect sc)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->clip_path) s->target->clip_path(ctx, s->target, p, eo, m, sc); }
static void nt_clip_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *p, const fz_stroke_state *st, fz_matrix m, fz_rect sc)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->clip_stroke_path) s->target->clip_stroke_path(ctx, s->target, p, st, m, sc); }
static void nt_clip_text(fz_context *ctx, fz_device *dev, const fz_text *t, fz_matrix m, fz_rect sc)
{ (void)ctx; (void)dev; (void)t; (void)m; (void)sc; }
static void nt_clip_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *t, const fz_stroke_state *st, fz_matrix m, fz_rect sc)
{ (void)ctx; (void)dev; (void)t; (void)st; (void)m; (void)sc; }
static void nt_fill_shade(fz_context *ctx, fz_device *dev, fz_shade *sh, fz_matrix m, float a, fz_color_params cp)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->fill_shade) s->target->fill_shade(ctx, s->target, sh, m, a, cp); }
static void nt_fill_image(fz_context *ctx, fz_device *dev, fz_image *im, fz_matrix m, float a, fz_color_params cp)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->fill_image) s->target->fill_image(ctx, s->target, im, m, a, cp); }
static void nt_fill_image_mask(fz_context *ctx, fz_device *dev, fz_image *im, fz_matrix m, fz_colorspace *cs, const float *c, float a, fz_color_params cp)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->fill_image_mask) s->target->fill_image_mask(ctx, s->target, im, m, cs, c, a, cp); }
static void nt_clip_image_mask(fz_context *ctx, fz_device *dev, fz_image *im, fz_matrix m, fz_rect sc)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->clip_image_mask) s->target->clip_image_mask(ctx, s->target, im, m, sc); }
static void nt_pop_clip(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->pop_clip) s->target->pop_clip(ctx, s->target); }
static void nt_begin_mask(fz_context *ctx, fz_device *dev, fz_rect r, int lum, fz_colorspace *cs, const float *bc, fz_color_params cp)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->begin_mask) s->target->begin_mask(ctx, s->target, r, lum, cs, bc, cp); }
static void nt_end_mask(fz_context *ctx, fz_device *dev, fz_function *fn)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->end_mask) s->target->end_mask(ctx, s->target, fn); }
static void nt_begin_group(fz_context *ctx, fz_device *dev, fz_rect r, fz_colorspace *cs, int iso, int kn, int bm, float a)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->begin_group) s->target->begin_group(ctx, s->target, r, cs, iso, kn, bm, a); }
static void nt_end_group(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->end_group) s->target->end_group(ctx, s->target); }
static int  nt_begin_tile(fz_context *ctx, fz_device *dev, fz_rect area, fz_rect view, float xs, float ys, fz_matrix m, int id)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; return s->target->begin_tile ? s->target->begin_tile(ctx, s->target, area, view, xs, ys, m, id) : 0; }
static void nt_end_tile(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->end_tile) s->target->end_tile(ctx, s->target); }
static void nt_render_flags(fz_context *ctx, fz_device *dev, int set, int clear)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->render_flags) s->target->render_flags(ctx, s->target, set, clear); }
static void nt_set_default_colorspaces(fz_context *ctx, fz_device *dev, fz_default_colorspaces *dcs)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->set_default_colorspaces) s->target->set_default_colorspaces(ctx, s->target, dcs); }
static void nt_begin_layer(fz_context *ctx, fz_device *dev, const char *n)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->begin_layer) s->target->begin_layer(ctx, s->target, n); }
static void nt_end_layer(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->end_layer) s->target->end_layer(ctx, s->target); }
static void nt_begin_structure(fz_context *ctx, fz_device *dev, fz_structure st, const char *raw, int idx)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->begin_structure) s->target->begin_structure(ctx, s->target, st, raw, idx); }
static void nt_end_structure(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->end_structure) s->target->end_structure(ctx, s->target); }
static void nt_begin_metatext(fz_context *ctx, fz_device *dev, fz_metatext meta, const char *t)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->begin_metatext) s->target->begin_metatext(ctx, s->target, meta, t); }
static void nt_end_metatext(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; if (s->target->end_metatext) s->target->end_metatext(ctx, s->target); }
static void nt_close(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; fz_close_device(ctx, s->target); (void)ctx; }
static void nt_drop(fz_context *ctx, fz_device *dev)
{ pdf_NoTextDev *s = (pdf_NoTextDev *)dev; fz_drop_device(ctx, s->target); }

static pdf_NoTextDev *nt_new(fz_context *ctx, fz_device *target)
{
    pdf_NoTextDev *s = fz_new_derived_device(ctx, pdf_NoTextDev);
    s->super.flags = target->flags;
    s->super.hints = target->hints;
    s->target = fz_keep_device(ctx, target);
    s->super.close_device = nt_close;
    s->super.drop_device = nt_drop;
    s->super.fill_path = nt_fill_path;
    s->super.stroke_path = nt_stroke_path;
    s->super.clip_path = nt_clip_path;
    s->super.clip_stroke_path = nt_clip_stroke_path;
    s->super.fill_text = NULL;         /* текст не рисуем */
    s->super.stroke_text = NULL;
    s->super.clip_text = nt_clip_text; /* клип по тексту не мешает */
    s->super.clip_stroke_text = nt_clip_stroke_text;
    s->super.ignore_text = NULL;
    s->super.fill_shade = nt_fill_shade;
    s->super.fill_image = nt_fill_image;
    s->super.fill_image_mask = nt_fill_image_mask;
    s->super.clip_image_mask = nt_clip_image_mask;
    s->super.pop_clip = nt_pop_clip;
    s->super.begin_mask = nt_begin_mask;
    s->super.end_mask = nt_end_mask;
    s->super.begin_group = nt_begin_group;
    s->super.end_group = nt_end_group;
    s->super.begin_tile = nt_begin_tile;
    s->super.end_tile = nt_end_tile;
    s->super.render_flags = nt_render_flags;
    s->super.set_default_colorspaces = nt_set_default_colorspaces;
    s->super.begin_layer = nt_begin_layer;
    s->super.end_layer = nt_end_layer;
    s->super.begin_structure = nt_begin_structure;
    s->super.end_structure = nt_end_structure;
    s->super.begin_metatext = nt_begin_metatext;
    s->super.end_metatext = nt_end_metatext;
    return s;
}

/* Фон страницы: то же, что pdfdoc_render, но без текстового слоя. */
int pdfdoc_render_notext(PdfDoc *d, int page, float scale, HBITMAP *out, int *ow, int *oh)
{
    fz_page *pg = NULL;
    fz_pixmap *pix = NULL;
    fz_device *draw = NULL;
    pdf_NoTextDev *proxy = NULL;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp = NULL;
    int y, w = 0, h = 0, stride, ok = 0;

    fz_var(pg); fz_var(pix); fz_var(draw); fz_var(proxy); fz_var(ok);
    if (!d || !d->doc || !out) return 0;

    fz_try(d->ctx) {
        pg = fz_load_page(d->ctx, d->doc, page);
        w = (int)(fz_bound_page(d->ctx, pg).x1 * scale + 0.5f);
        h = (int)(fz_bound_page(d->ctx, pg).y1 * scale + 0.5f);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        pix = fz_new_pixmap(d->ctx, fz_device_bgr(d->ctx), w, h, NULL, 0);
        fz_clear_pixmap_with_value(d->ctx, pix, 0xFF);
        draw = fz_new_draw_device(d->ctx, fz_identity, pix);
        proxy = nt_new(d->ctx, draw);
        fz_run_page(d->ctx, pg, (fz_device *)proxy, fz_scale(scale, scale), NULL);
        fz_close_device(d->ctx, (fz_device *)proxy);
        w = pix->w; h = pix->h;
        ok = 1;
    }
    fz_always(d->ctx) {
        fz_drop_device(d->ctx, (fz_device *)proxy);
        fz_drop_device(d->ctx, draw);
    }
    fz_catch(d->ctx)
        ok = 0;

    if (!ok || !pix) {
        if (pix) fz_drop_pixmap(d->ctx, pix);
        if (pg) fz_drop_page(d->ctx, pg);
        return 0;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        fz_drop_pixmap(d->ctx, pix);
        fz_drop_page(d->ctx, pg);
        return 0;
    }
    stride = ((w * 3) + 3) & ~3;
    for (y = 0; y < h; y++)
        memcpy((unsigned char *)bits + y * stride, pix->samples + (size_t)y * pix->stride, (size_t)w * 3);

    fz_drop_pixmap(d->ctx, pix);
    fz_drop_page(d->ctx, pg);
    *out = bmp;
    if (ow) *ow = w;
    if (oh) *oh = h;
    return 1;
}
