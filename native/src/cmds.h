/* cmds.h — идентификаторы команд и инструментов (общие для main.c и wui.c) */
#ifndef CMDS_H
#define CMDS_H

/* ---------- идентификаторы ---------- */
#define IDC_OPEN        1001
#define IDC_SAVE        1002
#define IDC_SAVEAS      1003
#define IDC_PRINT       1004
#define IDC_QUIT        1005
#define IDC_PREV        1010
#define IDC_NEXT        1011
#define IDC_ZOOMOUT     1012
#define IDC_ZOOMIN      1013
#define IDC_FITW        1014
#define IDC_PAGENO      1015
#define IDC_SEARCH      1020
#define IDC_FIND        1021
#define IDC_FINDNEXT    1022
#define IDC_TOOLBASE    1030          /* + номер инструмента */
#define IDC_STATUS      1040
#define IDC_COPYTEXT    1050
#define IDC_ROTRIGHT    1060
#define IDC_ROTLEFT     1061
#define IDC_PAGEDUP     1062
#define IDC_PAGEDEL     1063
#define IDC_PAGEUP      1064
#define IDC_PAGEDOWN    1065
#define IDC_EDITTEXT    1070
#define IDC_OCR         1071
#define IDC_OCRALL      1072
#define IDC_TEXTUNDO    1073
#define IDC_ZOOM100     1074
#define IDC_TOOLSPANEL  1075
#define IDC_VIEW1       1076          /* вид: одна страница */
#define IDC_VIEWCONT    1077          /* вид: непрерывно */
#define IDC_VIEW2       1078          /* вид: две страницы */
#define IDC_VIEWMULTI   1079          /* вид: несколько страниц (по ширине окна) */
#define IDC_SETDEFAULT  1080          /* прописаться приложением по умолчанию */
#define IDC_DOCMODE     1081          /* режим документа: текстовый процессор как в Word */

/* ---------- форматирование (режим документа) ---------- */
#define IDC_FMT_B       1090          /* полужирный */
#define IDC_FMT_I       1091          /* курсив */
#define IDC_FMT_U       1092          /* подчёркнутый */
#define IDC_FMT_SMALLER 1093          /* кегль меньше */
#define IDC_FMT_BIGGER  1094          /* кегль больше */
#define IDC_ALIGN_L     1095          /* по левому краю */
#define IDC_ALIGN_C     1096          /* по центру */
#define IDC_ALIGN_R     1097          /* по правому краю */
#define IDC_ALIGN_J     1098          /* по ширине */
#define IDC_LIST_B      1099          /* маркированный список */
#define IDC_LIST_N      1100          /* нумерованный список */
#define IDC_LIST_NONE   1101          /* убрать список */
#define IDC_SPACING     1102          /* межстрочный интервал (по кругу) */
#define IDC_INDENTP     1103          /* красная строка + */
#define IDC_INDENTM     1104          /* красная строка − */
#define IDC_FMT_UNDO    1105          /* отменить */
#define IDC_FMT_REDO    1106          /* повторить */
#define IDC_FMT_COLOR   1107          /* + индекс цвета 0..5 */
#define IDC_FINDREPL    1113          /* найти и заменить */
#define IDC_DOCCOPY     1114
#define IDC_DOCCUT      1115
#define IDC_DOCPASTE    1116
#define IDC_PAGE_A4     1120
#define IDC_PAGE_A5     1121
#define IDC_PAGE_LETTER 1122
#define IDC_PORTR       1123
#define IDC_LANDR       1124
#define IDC_MARGINS     1125
#define IDC_SPACEBEFORE 1126
#define IDC_HEADER      1127
#define IDC_FOOTER      1128
#define IDC_PAGENUM     1129
#define IDC_INSERT_TABLE 1130         /* вставить таблицу 3x3 */
#define IDC_TABLE_ROW    1131         /* добавить строку */
#define IDC_TABLE_COL    1132         /* добавить столбец */
#define IDC_TABLE_DEL    1133         /* удалить таблицу */
#define IDC_INSERT_IMG   1134         /* вставить картинку из файла */
#define IDC_FONT_BASE    1140         /* + семейство 0..3 */
#define IDC_FONTMENU     1144         /* кнопка «Шрифт» на панели */

#define TOOL_SELECT     0
#define TOOL_TEXT       1
#define TOOL_HIGHLIGHT  2
#define TOOL_RECT       3
#define TOOL_NOTE       4
#define TOOL_INK        5
#define TOOL_ERASE      6
#define TOOL_EDIT       7
#define TOOL_COUNT      8


#endif
