/* pdftext.c — работа с текстовым слоем PDF: разбор на блоки, метрики шрифта,
 * перевёрстка строк, замена текста (с запеканием в PDF) и OCR страницы.
 *
 * Идея: правка текста «как в Word» внутри страницы. Пользователь редактирует
 * блок, приложение хранит правку отдельно, а при сохранении:
 *   1) удаляет исходный текст в области блока (redaction, только текст —
 *      картинки и графика не трогаются),
 *   2) рисует новую плитку нужного цвета и новый текст встроенным шрифтом
 *      (Noto Sans, покрывает кириллицу) — текст остаётся настоящим текстом PDF.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdfdoc.h"
#include <mupdf/ucdn.h>

/* ---------- шрифт для нового текста ---------- */
fz_font *pdfdoc_ui_font(PdfDoc *d)
{
    int len = 0, sub = 0;
    const unsigned char *data;
    fz_font *font;

    if (!d || !d->ctx) return NULL;
    if (d->ui_font) return d->ui_font;

    /* Noto Sans покрывает латиницу, кириллицу и греческий */
    data = fz_lookup_noto_font(d->ctx, UCDN_SCRIPT_LATIN, FZ_LANG_UNSET, &len, &sub);
    if (!data)
        data = fz_lookup_noto_font(d->ctx, UCDN_SCRIPT_CYRILLIC, FZ_LANG_UNSET, &len, &sub);
    if (!data || len <= 0) return NULL;

    font = fz_new_font_from_memory(d->ctx, "NotoSans", data, len, sub, 0);
    if (!font) return NULL;
    if (!fz_encode_character(d->ctx, font, 0x0410)) {   /* «А» */
        fz_drop_font(d->ctx, font);
        return NULL;
    }
    d->ui_font = font;
    return font;
}

/* ---------- шрифт исходного блока ----------
   Раньше правка всегда рисовалась Noto Sans и кегль поджимался под ширину
   блока. Из-за этого после правки абзац выглядел другим шрифтом и «мельче»
   остального текста, а в режиме документа абзац разъезжался. Теперь для
   правки берётся стандартный шрифт того же семейства и начертания, что и в
   исходнике (Times/Arial/Courier), и кегль сохраняется. */

static int name_has(const char *name, const char *what)
{
    int i, j;
    if (!name || !*name || !what || !*what) return 0;
    for (i = 0; name[i]; i++) {
        for (j = 0; what[j]; j++) {
            char a = name[i + j], b = what[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (!what[j]) return 1;
    }
    return 0;
}

int pdfdoc_block_family(const PdfTextBlock *blk)
{
    const char *n = blk ? blk->font : NULL;
    if (name_has(n, "courier") || name_has(n, "mono") ||
        name_has(n, "consol") || name_has(n, "typewriter")) return 2;
    if (name_has(n, "times") || name_has(n, "serif") || name_has(n, "roman") ||
        name_has(n, "georgia") || name_has(n, "garamond") || name_has(n, "cambria") ||
        name_has(n, "antiqua") || name_has(n, "мин")) return 1;
    return 0;
}

/* «Нужен ли шрифт Noto»: базовые шрифты не покрывают кириллицу и греческий */
static int text_needs_noto(const char *utf8)
{
    int i = 0;
    if (!utf8) return 0;
    while (utf8[i]) {
        unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) {
            unsigned v = ((c & 0x0F) << 12);
            if (i + 1 < (int)strlen(utf8)) v |= (unsigned)(utf8[i + 1] & 0x3F) << 6;
            if (i + 2 < (int)strlen(utf8)) v |= (unsigned)(utf8[i + 2] & 0x3F);
            if (v >= 0x0400 && v <= 0x04FF) return 1;      /* кириллица */
            if (v >= 0x0370 && v <= 0x03FF) return 1;      /* греческий */
            i += 3;
        } else i += 4;
    }
    return 0;
}

static fz_font *base_font(PdfDoc *d, int family, int bold, int italic)
{
    static const char *serif[4] = { "Times-Roman", "Times-Italic",
                                    "Times-Bold", "Times-BoldItalic" };
    static const char *sans[4] = { "Helvetica", "Helvetica-Oblique",
                                   "Helvetica-Bold", "Helvetica-BoldOblique" };
    static const char *mono[4] = { "Courier", "Courier-Oblique",
                                   "Courier-Bold", "Courier-BoldOblique" };
    const char *nm;
    fz_font *f = NULL;
    int key;

    if (family < 0 || family > 2) family = 0;
    key = (bold ? 2 : 0) + (italic ? 1 : 0);
    if (d->base_font[family][key]) return d->base_font[family][key];
    nm = family == 1 ? serif[key] : family == 2 ? mono[key] : sans[key];
    fz_try(d->ctx) { f = fz_new_base14_font(d->ctx, nm); }
    fz_catch(d->ctx) { f = NULL; }
    if (!f) return NULL;
    d->base_font[family][key] = f;
    return f;
}

fz_font *pdfdoc_text_font(PdfDoc *d, const PdfTextBlock *blk)
{
    fz_font *f;
    if (!d || !d->ctx) return NULL;
    if (text_needs_noto(blk ? blk->text : NULL)) return pdfdoc_ui_font(d);
    f = base_font(d, pdfdoc_block_family(blk), blk ? blk->bold : 0, blk ? blk->italic : 0);
    return f ? f : pdfdoc_ui_font(d);
}

void pdfdoc_metrics_set(PdfDoc *d, fz_font *f)
{
    if (d) d->metrics_font = f;
}

/* ширина одного символа (UTF-32) в пунктах */
/* склейка «буква + комбинирующий знак» в готовый символ (й, ё, é …),
 * иначе знак убираем — шрифт может не содержать отдельных диакритик */
static int compose_cp(int base, int mark)
{
    if (mark >= 0x0300 && mark <= 0x036F) {
        switch (mark) {
        case 0x0306:   /* breve */
            switch (base) {
            case 0x0438: return 0x0439;   /* й */
            case 0x0418: return 0x0419;   /* Й */
            case 0x0430: return 0x04D1;   /* ӑ */
            case 0x0410: return 0x04D0;
            case 0x0435: return 0x04D7;
            case 0x0415: return 0x04D6;
            case 0x043E: return 0x04E7;
            case 0x041E: return 0x04E6;
            case 0x0443: return 0x045E;   /* ў */
            case 0x0423: return 0x040E;
            }
            break;
        case 0x0308:   /* diaeresis */
            switch (base) {
            case 0x0435: return 0x0451;   /* ё */
            case 0x0415: return 0x0401;   /* Ё */
            case 0x0438: return 0x0456 > 0 ? 0x0457 : base; /* ї */
            case 0x0418: return 0x0407;
            case 0x0430: return 0x00E4;
            case 0x043E: return 0x00F6;
            case 0x0443: return 0x00FC;
            case 0x0435 + 0x10000: return base;
            }
            /* латиница с умлаутом */
            switch (base) {
            case 'a': return 0x00E4; case 'A': return 0x00C4;
            case 'e': return 0x00EB; case 'E': return 0x00CB;
            case 'i': return 0x00EF; case 'I': return 0x00CF;
            case 'o': return 0x00F6; case 'O': return 0x00D6;
            case 'u': return 0x00FC; case 'U': return 0x00DC;
            case 'y': return 0x00FF;
            }
            break;
        case 0x0301:   /* acute */
            switch (base) {
            case 'a': return 0x00E1; case 'A': return 0x00C1;
            case 'e': return 0x00E9; case 'E': return 0x00C9;
            case 'i': return 0x00ED; case 'I': return 0x00CD;
            case 'o': return 0x00F3; case 'O': return 0x00D3;
            case 'u': return 0x00FA; case 'U': return 0x00DA;
            case 0x0433: return 0x0453;   /* ѓ */
            case 0x0413: return 0x0403;
            case 0x043A: return 0x045C;   /* ќ */
            case 0x041A: return 0x040C;
            }
            break;
        case 0x0300:   /* grave */
            switch (base) {
            case 'a': return 0x00E0; case 'A': return 0x00C0;
            case 'e': return 0x00E8; case 'E': return 0x00C8;
            case 'i': return 0x00EC; case 'I': return 0x00CC;
            case 'o': return 0x00F2; case 'O': return 0x00D2;
            case 'u': return 0x00F9; case 'U': return 0x00D9;
            }
            break;
        case 0x0327:   /* cedilla */
            if (base == 'c') return 0x00E7;
            if (base == 'C') return 0x00C7;
            break;
        case 0x0303:   /* tilde */
            if (base == 'n') return 0x00F1;
            if (base == 'N') return 0x00D1;
            if (base == 'a') return 0x00E3;
            if (base == 'o') return 0x00F5;
            break;
        }
        return base;   /* знак отбрасываем */
    }
    return -1;         /* не комбинирующий */
}

float pdfdoc_cp_width(PdfDoc *d, int cp, float size)
{
    fz_font *font = (d && d->metrics_font) ? d->metrics_font : pdfdoc_ui_font(d);
    int gid;
    if (!font) return size * 0.5f;
    gid = fz_encode_character(d->ctx, font, cp > 0 ? cp : 32);
    if (!gid) gid = fz_encode_character(d->ctx, font, '?');
    return (float)fz_advance_glyph(d->ctx, font, gid, 0) * size;
}

/* Подбор кегля под ширину блока.
   Шрифт, которым мы пишем правку (Noto Sans), шире шрифтов, которыми обычно
   набраны PDF (Times/Arial). Если оставить исходный кегль, каждая строка
   становится шире блока и последнее слово «уезжает» на новую строку — текст
   после правки разваливается. Поэтому кегль слегка уменьшается ровно настолько,
   чтобы самая широкая исходная строка снова уложилась в ширину блока. */
float pdfdoc_fit_size(PdfDoc *d, const int *cp, int n, float size, float width)
{
    float best = 0, w = 0, s;
    int i;
    if (!d || !cp || n <= 0 || width <= 2 || size <= 0) return size;
    for (i = 0; i < n; i++) {
        if (cp[i] == '\n') {
            if (w > best) best = w;
            w = 0;
            continue;
        }
        w += pdfdoc_cp_width(d, cp[i], size);
    }
    if (w > best) best = w;
    if (best <= width * 1.01f) return size;   /* шрифт тот же — кегль не трогаем */
    s = size * (width / best) * 0.99f;
    if (s < size * 0.90f) s = size * 0.90f;   /* максимум −10 %, иначе текст «мельчает» */
    return s;
}

/* перевод UTF-32 -> UTF-8 (malloc) */
char *pdfdoc_cp_to_utf8(const int *cp, int n)
{
    int i, need = 0;
    char *out, *p;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {
        int c = cp[i];
        need += (c < 0x80) ? 1 : (c < 0x800) ? 2 : (c < 0x10000) ? 3 : 4;
    }
    out = (char *)malloc(need + 1);
    if (!out) return NULL;
    p = out;
    for (i = 0; i < n; i++) {
        unsigned int c = (unsigned int)cp[i];
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = (char)(0xC0 | (c >> 6)); *p++ = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            *p++ = (char)(0xE0 | (c >> 12));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        } else {
            *p++ = (char)(0xF0 | (c >> 18));
            *p++ = (char)(0x80 | ((c >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p = 0;
    return out;
}

/* UTF-8 -> UTF-32 (malloc), возвращает число символов */
int *pdfdoc_utf8_to_cp(const char *s, int *outCount)
{
    int cap = 32, n = 0;
    int *cp;
    if (outCount) *outCount = 0;
    if (!s) return NULL;
    cp = (int *)malloc(sizeof(int) * cap);
    if (!cp) return NULL;
    while (*s) {
        int c = (unsigned char)*s++;
        int len = 1, i;
        if (c >= 0xF0) { c &= 0x07; len = 4; }
        else if (c >= 0xE0) { c &= 0x0F; len = 3; }
        else if (c >= 0xC0) { c &= 0x1F; len = 2; }
        for (i = 1; i < len; i++) {
            if (!*s) break;
            c = (c << 6) | ((unsigned char)*s++ & 0x3F);
        }
        if (n + 1 >= cap) {
            int *np;
            cap *= 2;
            np = (int *)realloc(cp, sizeof(int) * cap);
            if (!np) { free(cp); return NULL; }
            cp = np;
        }
        /* «буква + комбинирующий знак» -> готовый символ */
        if (n > 0 && c >= 0x0300 && c <= 0x036F) {
            int comp = compose_cp(cp[n - 1], c);
            if (comp >= 0) { cp[n - 1] = comp; continue; }
        }
        cp[n++] = c;
    }
    if (outCount) *outCount = n;
    return cp;
}

/* ---------- поиск текстового блока под точкой ---------- */
static int block_chars(fz_stext_block *b)
{
    int n = 0;
    fz_stext_line *ln;
    fz_stext_char *ch;
    for (ln = b->u.t.first_line; ln; ln = ln->next)
        for (ch = ln->first_char; ch; ch = ch->next) n++;
    return n;
}

/* Блок под точкой. Берём не самый маленький, а самый «содержательный»:
 * иначе клик по строке попадал в блок-огрызок (маркер списка, пустой блок)
 * и правка появлялась в странном месте. */
static int block_under(fz_stext_block *blocks, fz_point pt, fz_stext_block **best)
{
    fz_stext_block *b, *found = NULL;
    int bestChars = -1;
    float bestArea = 0;

    for (b = blocks; b; b = b->next) {
        int n;
        float area;
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        if (pt.x < b->bbox.x0 || pt.x > b->bbox.x1 ||
            pt.y < b->bbox.y0 || pt.y > b->bbox.y1) continue;
        n = block_chars(b);
        area = (b->bbox.x1 - b->bbox.x0) * (b->bbox.y1 - b->bbox.y0);
        if (n > bestChars || (n == bestChars && found && area < bestArea)) {
            found = b; bestChars = n; bestArea = area;
        }
    }
    *best = found;
    return found ? 1 : 0;
}

int pdfdoc_text_block_at(PdfDoc *d, int page, fz_point pt, PdfTextBlock *out)
{
    fz_page *pg = NULL;
    fz_stext_page *tp = NULL;
    int ok = 0;

    if (!d || !d->doc || !out) return 0;
    memset(out, 0, sizeof(*out));

    fz_try(d->ctx) {
        fz_stext_block *b = NULL;
        fz_stext_options opts;

        pg = fz_load_page(d->ctx, d->doc, page);
        memset(&opts, 0, sizeof(opts));
        opts.flags = FZ_STEXT_PRESERVE_SPANS;
        tp = fz_new_stext_page_from_page(d->ctx, pg, &opts);

        if (block_under(tp->first_block, pt, &b)) {
            fz_stext_line *ln, *first = b->u.t.first_line, *last = b->u.t.last_line;
            fz_stext_char *ch;
            char *buf = NULL;
            size_t len = 0, cap = 0;
            float lastY = 0;
            int firstLine = 1, atSpace = 1;

            out->box = b->bbox;
            out->x0 = b->bbox.x0;
            out->fontSize = 11.0f;
            out->leading = 0;
            out->bold = 0;
            out->italic = 0;
            out->color = 0;
            out->font[0] = 0;

            /* сборка текста: части одной строки склеиваем (многие генераторы
             * пишут каждое слово отдельным текстовым объектом) */
            for (ln = first; ln; ln = ln->next) {
                float y = ln->first_char ? ln->first_char->origin.y : lastY;
                float sameLimit = (out->fontSize > 1.0f ? out->fontSize : 11.0f) * 0.45f;
                int sameLine = !firstLine && (y - lastY < sameLimit) && (lastY - y < sameLimit);

                if (!firstLine) {
                    int needSpace = !atSpace;
                    if (!sameLine) {
                        if (len + 1 >= cap) { cap = cap ? cap * 2 : 256; buf = (char *)realloc(buf, cap); }
                        if (!buf) fz_throw(d->ctx, FZ_ERROR_LIMIT, "нет памяти");
                        buf[len++] = '\n'; atSpace = 1; needSpace = 0;
                    }
                    if (needSpace) {
                        if (len + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
                        if (!buf) fz_throw(d->ctx, FZ_ERROR_LIMIT, "нет памяти");
                        buf[len++] = ' '; atSpace = 1;
                    }
                }

                for (ch = ln->first_char; ch; ch = ch->next) {
                    unsigned int c = (unsigned int)ch->c;
                    char tmp[4];
                    int tn = 0;

                    if (!out->font[0] && ch->font && ch->font->name) {
                        const char *nm = ch->font->name;
                        const char *plus = strchr(nm, '+');
                        lstrcpynA(out->font, plus ? plus + 1 : nm, sizeof(out->font));
                        out->bold = fz_font_is_bold(d->ctx, ch->font);
                        out->italic = fz_font_is_italic(d->ctx, ch->font);
                        out->color = (unsigned int)(ch->color & 0xFFFFFF);
                        out->fontSize = ch->size > 1.0f ? ch->size : 11.0f;
                    }
                    if (c < 0x80) tmp[tn++] = (char)c;
                    else if (c < 0x800) { tmp[tn++] = (char)(0xC0 | (c >> 6)); tmp[tn++] = (char)(0x80 | (c & 0x3F)); }
                    else if (c < 0x10000) {
                        tmp[tn++] = (char)(0xE0 | (c >> 12));
                        tmp[tn++] = (char)(0x80 | ((c >> 6) & 0x3F));
                        tmp[tn++] = (char)(0x80 | (c & 0x3F));
                    } else {
                        tmp[tn++] = (char)(0xF0 | (c >> 18));
                        tmp[tn++] = (char)(0x80 | ((c >> 12) & 0x3F));
                        tmp[tn++] = (char)(0x80 | ((c >> 6) & 0x3F));
                        tmp[tn++] = (char)(0x80 | (c & 0x3F));
                    }
                    if (len + (size_t)tn + 1 > cap) {
                        cap = (len + (size_t)tn + 1) * 2;
                        buf = (char *)realloc(buf, cap);
                        if (!buf) fz_throw(d->ctx, FZ_ERROR_LIMIT, "нет памяти");
                    }
                    memcpy(buf + len, tmp, (size_t)tn);
                    len += (size_t)tn;
                    atSpace = (c == ' ');
                }
                lastY = y;
                firstLine = 0;
            }
            if (len + 1 > cap) { cap = len + 1; buf = (char *)realloc(buf, cap); }
            if (!buf) fz_throw(d->ctx, FZ_ERROR_LIMIT, "нет памяти");
            buf[len] = 0;
            out->text = buf;

            /* базовая линия первой строки */
            if (first && first->first_char)
                out->baseline = first->first_char->origin.y;
            /* межстрочный интервал по базовым линиям */
            if (first && first->next && first->first_char && first->next->first_char) {
                out->leading = first->next->first_char->origin.y - first->first_char->origin.y;
                if (out->leading < 0) out->leading = -out->leading;
            }
            if (out->leading < out->fontSize * 0.8f) out->leading = out->fontSize * 1.2f;
            ok = 1;
        }
    }
    fz_catch(d->ctx) { ok = 0; }

    if (tp) fz_drop_stext_page(d->ctx, tp);
    if (pg) fz_drop_page(d->ctx, pg);
    return ok;
}

/* ---------- раскладка строк (перенос по ширине) ---------- */
/* Возвращает число строк; lineStart/lineEnd — индексы символов в cp[].
 * wordsOnly: перенос только по пробелам (как в Word). */
int pdfdoc_wrap_text(PdfDoc *d, const int *cp, int n, float size, float width,
                     int *lineStart, int *lineEnd, int maxLines)
{
    int lines = 0, i = 0;

    if (n <= 0) { lineStart[0] = lineEnd[0] = 0; return 1; }

    while (i < n && lines < maxLines) {
        int start = i, lastSpace = -1;
        float w = 0;

        lineStart[lines] = start;
        while (i < n) {
            int c = cp[i];
            float cw;
            if (c == '\n') break;          /* перевод строки в текст не попадает */
            cw = pdfdoc_cp_width(d, c, size);
            /* +0.6 pt допуска: метрики нового шрифта чуть отличаются от
               исходных, иначе неизменённая строка «переезжает» на новую */
            if (w + cw > width + 0.6f && i > start) {
                if (c != ' ' && lastSpace > start) {  /* переносим по последнему пробелу */
                    i = lastSpace + 1;
                }
                break;
            }
            if (c == ' ') lastSpace = i;
            w += cw;
            i++;
        }
        /* не оставляем висящие пробелы в конце строки */
        while (i > start && cp[i - 1] == ' ') i--;
        if (i == start && i < n && cp[i] != ' ' && cp[i] != '\n') i++;  /* страховка */
        lineEnd[lines] = i;
        lines++;
        /* пропускаем пробелы/перевод строки в начале следующей строки */
        while (i < n && (cp[i] == ' ' || cp[i] == '\n')) i++;
    }
    if (lines == 0) { lineStart[0] = lineEnd[0] = 0; lines = 1; }
    return lines;
}

/* ---------- запекание правки в PDF ---------- */
int pdfdoc_text_apply(PdfDoc *d, int page, const PdfTextBlock *blk)
{
    pdf_page *pp = NULL;
    fz_buffer *buf = NULL;
    fz_font *font = NULL;
    fz_path *bg = NULL;
    fz_text *text = NULL;
    fz_device *dev = NULL;
    int ok = 0;

    fz_font *prevMetrics = d ? d->metrics_font : NULL;
    int metricsSet = 0;

    if (!d || !d->pdf || !blk || !blk->text) return 0;
    /* рисуем тем же семейством/начертанием, что и в исходнике */
    font = pdfdoc_text_font(d, blk);
    if (!font) return 0;
    pdfdoc_metrics_set(d, font);
    metricsSet = 1;

    fz_var(pp);
    fz_var(buf);
    fz_var(bg);
    fz_var(text);
    fz_var(dev);
    fz_var(ok);

    fz_try(d->ctx) {
        pdf_obj *resources, *pageobj, *contents, *stream;
        fz_rect box = blk->box;
        fz_rect mediabox;
        fz_matrix pctm, topctm;
        int *cp = NULL, n = 0;
        int lineStart[512], lineEnd[512], lines, li;
        float y, leading = blk->leading > 0 ? blk->leading : blk->fontSize * 1.2f;
        float fontSize = blk->fontSize;

        pp = pdf_load_page(d->ctx, d->pdf, page);

        /* 1) убираем исходный текст в области блока (только текст) */
        {
            pdf_annot *red = pdf_create_annot(d->ctx, pp, PDF_ANNOT_REDACT);
            pdf_redact_options ropts;
            pdf_set_annot_rect(d->ctx, red, box);
            memset(&ropts, 0, sizeof(ropts));
            ropts.black_boxes = 0;
            ropts.image_method = PDF_REDACT_IMAGE_NONE;
            ropts.line_art = PDF_REDACT_LINE_ART_NONE;
            ropts.text = PDF_REDACT_TEXT_REMOVE;
            pdf_redact_page(d->ctx, d->pdf, pp, &ropts);
        }

        /* 2) новая графика: плитка + текст */
        resources = pdf_page_resources(d->ctx, pp);
        if (!resources) {
            resources = pdf_new_dict(d->ctx, d->pdf, 4);
            pdf_dict_put(d->ctx, pp->obj, PDF_NAME(Resources), resources);
        }
        buf = fz_new_buffer(d->ctx, 1024);
        /* координаты страницы (y вниз) -> система PDF (y вверх) */
        pdf_page_transform(d->ctx, pp, &mediabox, &pctm); /* fitz -> PDF */
        topctm = pctm;
        dev = pdf_new_pdf_device(d->ctx, d->pdf, topctm, resources, buf);
        (void)mediabox;

        /* область закраски чуть шире строки, чтобы перекрыть выносные элементы */
        box.y0 -= blk->fontSize * 0.22f;
        box.y1 += blk->fontSize * 0.28f;
        box.x0 -= 0.5f;
        box.x1 += 0.5f;

        /* Фон НЕ закрашиваем: redaction удаляет только текстовый слой, а
           подложка (цвет, градиент, фото, плашка) остаётся ровно такой, как в
           исходнике. Любая заливка — даже «средним» цветом — давала видимый
           прямоугольник на нетривиальном фоне. */
        (void)bg;
        /* текст */
        cp = pdfdoc_utf8_to_cp(blk->text, &n);
        if (!cp) fz_throw(d->ctx, FZ_ERROR_LIMIT, "нет памяти");
        /* кегль под ширину блока: иначе строки разъезжаются (см. pdfdoc_fit_size) */
        fontSize = pdfdoc_fit_size(d, cp, n, blk->fontSize, box.x1 - box.x0 - 1.0f);
        leading = fontSize * (blk->leading > 0 && blk->fontSize > 0
                              ? (blk->leading / blk->fontSize) : 1.2f);
        lines = pdfdoc_wrap_text(d, cp, n, fontSize,
                                 box.x1 - box.x0 - 1.0f, lineStart, lineEnd, 512);

        text = fz_new_text(d->ctx);
        y = blk->baseline;
        if (y < blk->box.y0 - blk->fontSize * 0.5f || y > blk->box.y1 + blk->fontSize * 0.5f)
            y = blk->box.y1 - blk->fontSize * 0.25f;             /* страховка по рамке */
        for (li = 0; li < lines; li++) {
            float x = box.x0 + 0.5f;
            int i;
            for (i = lineStart[li]; i < lineEnd[li]; i++) {
                int gid = fz_encode_character(d->ctx, font, cp[i]);
                fz_matrix trm;
                if (!gid) gid = fz_encode_character(d->ctx, font, '?');
                trm = fz_make_matrix(fontSize, 0, 0, -fontSize, x, y);
                fz_show_glyph(d->ctx, text, font, trm, gid, cp[i], 0, 0,
                              FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                x += (float)fz_advance_glyph(d->ctx, font, gid, 0) * fontSize;
            }
            y += leading;
        }
        {
            float col[3];
            col[0] = ((blk->color >> 16) & 0xFF) / 255.0f;
            col[1] = ((blk->color >> 8) & 0xFF) / 255.0f;
            col[2] = (blk->color & 0xFF) / 255.0f;
            if (blk->color == 0) { col[0] = col[1] = col[2] = 0.0f; }
            fz_fill_text(d->ctx, dev, text, fz_identity, fz_device_rgb(d->ctx),
                         col, 1.0f, fz_default_color_params);
        }

        fz_close_device(d->ctx, dev);
        fz_drop_device(d->ctx, dev);
        dev = NULL;
        fz_drop_text(d->ctx, text);
        text = NULL;
        fz_drop_path(d->ctx, bg);
        bg = NULL;
        free(cp);
        cp = NULL;

        /* 3) дописываем поток в страницу */
        pageobj = pp->obj;
        stream = pdf_add_stream(d->ctx, d->pdf, buf, NULL, 1);
        contents = pdf_dict_get(d->ctx, pageobj, PDF_NAME(Contents));
        if (!contents) {
            pdf_dict_put(d->ctx, pageobj, PDF_NAME(Contents), stream);
        } else if (pdf_is_array(d->ctx, contents)) {
            pdf_array_push(d->ctx, contents, stream);
        } else {
            pdf_obj *arr = pdf_new_array(d->ctx, d->pdf, 2);
            pdf_array_push(d->ctx, arr, contents);
            pdf_array_push(d->ctx, arr, stream);
            pdf_dict_put(d->ctx, pageobj, PDF_NAME(Contents), arr);
        }

        pdf_update_page(d->ctx, pp);
        ok = 1;
    }
    fz_catch(d->ctx) { ok = 0; }

    if (dev) fz_drop_device(d->ctx, dev);
    if (text) fz_drop_text(d->ctx, text);
    if (bg) fz_drop_path(d->ctx, bg);
    if (buf) fz_drop_buffer(d->ctx, buf);
    if (pp) { fz_try(d->ctx) { pdf_drop_page(d->ctx, pp); } fz_catch(d->ctx) { } }

    if (ok) d->dirty = 1;
    if (metricsSet) pdfdoc_metrics_set(d, prevMetrics);
    return ok;
}

/* ---------- OCR страницы: невидимый текстовый слой ---------- */
int pdfdoc_ocr_page(PdfDoc *d, int page, const char *lang, const char *datadir, int *outChars)
{
    fz_page *pg = NULL;
    fz_stext_page *tp = NULL;
    fz_device *dev = NULL;
    int ok = 0;

    if (outChars) *outChars = 0;
    if (!d || !d->doc) return 0;

    fz_var(pg);
    fz_var(tp);
    fz_var(dev);
    fz_var(ok);

    fz_try(d->ctx) {
        fz_stext_options opts;
        fz_rect mediabox;
        fz_matrix ctm = fz_identity;
        fz_stext_block *b;
        int count = 0;

        if (!d->pdf) fz_throw(d->ctx, FZ_ERROR_ARGUMENT, "не PDF");

        pg = fz_load_page(d->ctx, d->doc, page);
        mediabox = fz_bound_page(d->ctx, pg);
        memset(&opts, 0, sizeof(opts));
        opts.flags = FZ_STEXT_PRESERVE_SPANS;
        tp = fz_new_stext_page(d->ctx, mediabox);

        /* OCR: распознаём страницу в структурный текст */
        {
            fz_device *stextDev = fz_new_stext_device(d->ctx, tp, &opts);
            dev = fz_new_ocr_device(d->ctx, stextDev, ctm, mediabox, 1, lang, datadir, NULL, NULL);
            fz_run_page(d->ctx, pg, dev, ctm, NULL);
            fz_close_device(d->ctx, dev);
            fz_drop_device(d->ctx, dev);
            dev = NULL;
            fz_close_device(d->ctx, stextDev);
            fz_drop_device(d->ctx, stextDev);
        }

        /* запекаем распознанный текст невидимым слоем (режим 3 Tr) */
        for (b = tp->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            count++;
        }
        if (count > 0) {
            PdfTextBlock blk;
            fz_stext_block *tb = NULL;
            memset(&blk, 0, sizeof(blk));
            blk.box = mediabox;
            blk.fontSize = 10.0f;
            blk.leading = 12.0f;
            blk.color = 0x000000;
            blk.text = (char *)malloc(1);
            if (!blk.text) fz_throw(d->ctx, FZ_ERROR_LIMIT, "нет памяти");
            blk.text[0] = 0;
            (void)tb;
            /* полный слой рисуем через отдельную функцию ниже */
            free(blk.text);
        }

        /* Здесь важное: собираем fz_text прямо из распознанных символов */
        if (d->pdf) {
            pdf_page *pp = NULL;
            fz_text *text = NULL;
            fz_buffer *buf = NULL;
            fz_font *font = pdfdoc_ui_font(d);
            int chars = 0;

            fz_var(pp);
            fz_var(text);
            fz_var(buf);
            fz_try(d->ctx) {
                pdf_obj *resources, *pageobj, *contents, *stream;
                fz_device *wdev = NULL;

                pp = pdf_load_page(d->ctx, d->pdf, page);
                resources = pdf_page_resources(d->ctx, pp);
                if (!resources) {
                    resources = pdf_new_dict(d->ctx, d->pdf, 4);
                    pdf_dict_put(d->ctx, pp->obj, PDF_NAME(Resources), resources);
                }
                buf = fz_new_buffer(d->ctx, 1024);
                {
                    fz_rect mb; fz_matrix pc2;
                    pdf_page_transform(d->ctx, pp, &mb, &pc2);
                    wdev = pdf_new_pdf_device(d->ctx, d->pdf, pc2, resources, buf);
                }
                text = fz_new_text(d->ctx);

                for (b = tp->first_block; b; b = b->next) {
                    fz_stext_line *ln;
                    if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
                    for (ln = b->u.t.first_line; ln; ln = ln->next) {
                        fz_stext_char *ch;
                        for (ch = ln->first_char; ch; ch = ch->next) {
                            fz_matrix trm;
                            float sz = ch->size > 1.0f ? ch->size : 10.0f;
                            int gid;
                            if (ch->c <= 0) continue;
                            gid = fz_encode_character(d->ctx, font, ch->c);
                            if (!gid) continue;
                            trm = fz_make_matrix(sz, 0, 0, -sz, ch->origin.x, ch->origin.y);
                            fz_show_glyph(d->ctx, text, font, trm, gid, ch->c, 0, 0,
                                          FZ_BIDI_NEUTRAL, FZ_LANG_UNSET);
                            chars++;
                        }
                    }
                }

                if (chars > 0) {
                    float col[3] = { 0, 0, 0 };
                    /* невидимый режим отрисовки текста */
                    fz_append_string(d->ctx, buf, "3 Tr\n");
                    fz_fill_text(d->ctx, wdev, text, fz_identity, fz_device_rgb(d->ctx),
                                 col, 1.0f, fz_default_color_params);
                    fz_close_device(d->ctx, wdev);
                    fz_drop_device(d->ctx, wdev);
                    wdev = NULL;

                    pageobj = pp->obj;
                    stream = pdf_add_stream(d->ctx, d->pdf, buf, NULL, 1);
                    contents = pdf_dict_get(d->ctx, pageobj, PDF_NAME(Contents));
                    if (!contents) {
                        pdf_dict_put(d->ctx, pageobj, PDF_NAME(Contents), stream);
                    } else if (pdf_is_array(d->ctx, contents)) {
                        pdf_array_push(d->ctx, contents, stream);
                    } else {
                        pdf_obj *arr = pdf_new_array(d->ctx, d->pdf, 2);
                        pdf_array_push(d->ctx, arr, contents);
                        pdf_array_push(d->ctx, arr, stream);
                        pdf_dict_put(d->ctx, pageobj, PDF_NAME(Contents), arr);
                    }
                    pdf_update_page(d->ctx, pp);
                    if (outChars) *outChars = chars;
                    ok = 1;
                    d->dirty = 1;
                } else {
                    ok = 0;
                }

                if (wdev) fz_drop_device(d->ctx, wdev);
            }
            fz_always(d->ctx) {
                if (text) fz_drop_text(d->ctx, text);
                if (buf) fz_drop_buffer(d->ctx, buf);
                if (pp) pdf_drop_page(d->ctx, pp);
            }
            fz_catch(d->ctx) { ok = 0; }
        }
    }
    fz_catch(d->ctx) { ok = 0; }

    if (dev) fz_drop_device(d->ctx, dev);
    if (tp) fz_drop_stext_page(d->ctx, tp);
    if (pg) fz_drop_page(d->ctx, pg);
    return ok;
}
