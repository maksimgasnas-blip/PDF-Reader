#include <stdio.h>
#include <wchar.h>
#include "../native/src/nav.h"

int main(void)
{
    int page = -1;
    if (!nav_parse_page(L"1", 12, &page) || page != 0) return 1;
    if (!nav_parse_page(L" 12 ", 12, &page) || page != 11) return 2;
    if (nav_parse_page(L"0", 12, &page)) return 3;
    if (nav_parse_page(L"13", 12, &page)) return 4;
    if (nav_parse_page(L"1x", 12, &page)) return 5;
    if (nav_parse_page(L"", 12, &page)) return 6;
    if (nav_parse_page(L"1", 0, &page)) return 7;
    if (nav_parse_page(NULL, 12, &page)) return 8;
    if (nav_parse_page(L"   ", 12, &page)) return 9;
    if (nav_parse_page(L"+1", 12, &page)) return 10;
    if (nav_parse_page(L"1 2", 12, &page)) return 11;
    if (nav_parse_page(L"1234567890123456789012345678901234567890", 12, &page)) return 12;
    if (!nav_parse_page(L"00012", 12, &page) || page != 11) return 13;
    printf("nav_test: 13 cases passed\n");
    return 0;
}
