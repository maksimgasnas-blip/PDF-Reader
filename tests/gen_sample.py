#!/usr/bin/env python3
"""Генератор тестового PDF для проверки правки текста и режима документа.

Пишет 5 страниц A4: заголовок «Page N of 5» (Helvetica-Bold 18),
несколько абзацев Times-Roman 8 pt с переносами, плюс узкая правая
колонка — как в файле пользователя sample5.pdf.
"""
import sys

W, H = 595, 842

LOREM = [
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et",
    "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex",
    "ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu",
    "fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt",
    "mollit anim id est laborum.",
]

RIGHT = ["bore et", "ip ex", "ugiat", "anim"]   # узкая правая колонка


def esc(s):
    return s.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")


def page_content(n):
    out = []
    out.append("BT /F2 18 Tf 1 0 0 1 170 762 Tm (Page %d of 5) Tj ET" % n)
    y = 720
    for para in range(3):
        out.append("BT /F1 8 Tf 1 0 0 1 72 %d Tm %.2f TL" % (y, 9.5))
        for i, line in enumerate(LOREM):
            if i:
                out.append("T*")
            out.append("(%s) Tj" % esc(line))
        out.append("ET")
        # узкая правая колонка
        out.append("BT /F1 8 Tf 1 0 0 1 512 %d Tm 9.5 TL" % (y - 1))
        for i, line in enumerate(RIGHT):
            if i:
                out.append("T*")
            out.append("(%s) Tj" % esc(line))
        out.append("ET")
        y -= 105
    return "\n".join(out)


def build(path):
    objs = []
    objs.append("<< /Type /Catalog /Pages 2 0 R >>")
    kids = " ".join("%d 0 R" % (3 + 2 * i) for i in range(5))
    objs.append("<< /Type /Pages /Count 5 /Kids [%s] >>" % kids)
    for i in range(5):
        pid = 3 + 2 * i
        cid = pid + 1
        objs.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
                    "/Resources << /Font << /F1 13 0 R /F2 14 0 R >> >> "
                    "/Contents %d 0 R >>" % (W, H, cid))
        data = page_content(i + 1).encode("latin-1")
        objs.append("<< /Length %d >>\nstream\n" % len(data) +
                    data.decode("latin-1") + "\nendstream")
    objs.append("<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman "
                "/Encoding /WinAnsiEncoding >>")
    objs.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
                "/Encoding /WinAnsiEncoding >>")

    out = bytearray(b"%PDF-1.4\n")
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += ("%d 0 obj\n" % (i + 1)).encode() + o.encode("latin-1") + b"\nendobj\n"
    xref = len(out)
    out += ("xref\n0 %d\n" % (len(objs) + 1)).encode()
    out += b"0000000000 65535 f \n"
    for o in offs:
        out += ("%010d 00000 n \n" % o).encode()
    out += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, xref)).encode()
    open(path, "wb").write(out)
    print("написан", path, len(out), "байт")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "/tmp/sample5.pdf")
