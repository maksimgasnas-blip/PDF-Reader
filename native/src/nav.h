#ifndef PDFSTUDIO_NAV_H
#define PDFSTUDIO_NAV_H

/* Small, dependency-free helpers for validating navigation field input. */
static int nav_parse_page(const wchar_t *text, int pageCount, int *pageOut)
{
    unsigned long value = 0;
    const wchar_t *p = text;

    if (!text || !pageOut || pageCount <= 0) return 0;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p < L'0' || *p > L'9') return 0;
    while (*p >= L'0' && *p <= L'9') {
        unsigned long digit = (unsigned long)(*p - L'0');
        /* Check before multiplying so long input cannot wrap on 32-bit builds. */
        if (value > (unsigned long)pageCount / 10 ||
            (value == (unsigned long)pageCount / 10 &&
             digit > (unsigned long)pageCount % 10)) return 0;
        value = value * 10 + digit;
        ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p || value < 1 || value > (unsigned long)pageCount) return 0;
    *pageOut = (int)value - 1;
    return 1;
}

#endif
