#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate tests/fixtures/two_pages.pdf: a minimal, valid two-page PDF with
a text layer (Helvetica) and a Title in the document information
dictionary. Run from the repository root; the output is committed."""

import os

pages = [
    ["LISA Test Manual", "Pump bearings must be inspected every 500 hours."],
    ["Section 2", "Vibration above 40 Hz indicates wear."],
]

objs = []  # (number, bytes)

def add(body):
    objs.append(body)
    return len(objs)

catalog = add(None)
pages_obj = add(None)
font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
kids = []
for lines in pages:
    ops = ["BT", "/F1 14 Tf", "72 720 Td", "18 TL"]
    for line in lines:
        ops.append("(%s) Tj T*" % line.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)"))
    ops.append("ET")
    stream = "\n".join(ops).encode("latin-1")
    content = add(b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream")
    page = add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] "
               b"/Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
               % (pages_obj, font, content))
    kids.append(page)
objs[catalog - 1] = b"<< /Type /Catalog /Pages %d 0 R >>" % pages_obj
objs[pages_obj - 1] = (b"<< /Type /Pages /Kids [" + b" ".join(b"%d 0 R" % k for k in kids)
                       + b"] /Count %d >>" % len(kids))
info = add(b"<< /Title (LISA Test Manual) /Producer (LISA test fixture) >>")

out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
offsets = []
for i, body in enumerate(objs, start=1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += (b"trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (len(objs) + 1, catalog, info, xref))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "two_pages.pdf")
with open(path, "wb") as f:
    f.write(out)
print("wrote", path, len(out), "bytes")
