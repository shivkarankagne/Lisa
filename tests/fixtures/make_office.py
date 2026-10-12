#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
"""Generate tests/fixtures/sample.pptx, sample.xlsx and sample.csv: minimal
files exercising what LISA's extractors must handle. Run from the repository
root; the outputs are committed.

pptx: presentation order differs from slide file numbers (slide2.xml is
shown first), speaker notes, a slide-number field (must not appear),
entities, a title in docProps/core.xml.
xlsx: a header row, shared strings with a rich-text run and a phonetic
guide (must not appear), an inline string, a gap column, numbers, a boolean,
two sheets (order from workbook.xml).
csv: quoted delimiter, embedded newline, "" escape, header row."""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PKG = "http://schemas.openxmlformats.org/package/2006/relationships"
A = 'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"'
P = 'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"'
R = f'xmlns:r="{REL}"'
CORE = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
 xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{}</dc:title></cp:coreProperties>"""


def write(name, parts):
    with zipfile.ZipFile(os.path.join(HERE, name), "w", zipfile.ZIP_DEFLATED) as z:
        for part, data in parts:
            info = zipfile.ZipInfo(part, date_time=(2026, 1, 1, 0, 0, 0))  # reproducible bytes
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8"))


def slide(*paras):
    body = "".join(f"<a:p>{p}</a:p>" for p in paras)
    return f'<?xml version="1.0" encoding="UTF-8"?><p:sld {A} {P} {R}><p:cSld><p:spTree><p:sp><p:txBody>{body}</p:txBody></p:sp></p:spTree></p:cSld></p:sld>'


def run(t):
    return f"<a:r><a:t>{t}</a:t></a:r>"


pptx = [
    ("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>'),
    ("docProps/core.xml", CORE.format("Q3 Sales Kickoff")),
    ("ppt/presentation.xml", f'<?xml version="1.0"?><p:presentation {P} {R}><p:sldIdLst>'
     '<p:sldId id="256" r:id="rId7"/><p:sldId id="257" r:id="rId3"/></p:sldIdLst></p:presentation>'),
    ("ppt/_rels/presentation.xml.rels", f'<?xml version="1.0"?><Relationships xmlns="{PKG}">'
     f'<Relationship Id="rId3" Type="{REL}/slide" Target="slides/slide1.xml"/>'
     f'<Relationship Id="rId7" Type="{REL}/slide" Target="slides/slide2.xml"/></Relationships>'),
    # Shown first:
    ("ppt/slides/slide2.xml", slide(run("Agenda"), run("Pricing &amp; packaging") + run(" for Q3"),
                                    '<a:fld id="{X}" type="slidenum"><a:t>99</a:t></a:fld>')),
    ("ppt/slides/_rels/slide2.xml.rels", f'<?xml version="1.0"?><Relationships xmlns="{PKG}">'
     f'<Relationship Id="rId2" Type="{REL}/notesSlide" Target="../notesSlides/notesSlide1.xml"/></Relationships>'),
    ("ppt/notesSlides/notesSlide1.xml", slide(run("Remind reps: discounts above 15% need VP approval."))),
    # Shown second:
    ("ppt/slides/slide1.xml", slide(run("Targets"), run("West region quota is 4.2M")).replace(
        "</p:spTree>", '<p:graphicFrame><a:graphic><a:graphicData><a:tbl>'
        '<a:tr><a:tc><a:txBody><a:p>' + run("Item") + '</a:p></a:txBody></a:tc><a:tc><a:txBody><a:p>' + run("Cost") + '</a:p></a:txBody></a:tc></a:tr>'
        '<a:tr><a:tc><a:txBody><a:p>' + run("Laptops") + '</a:p></a:txBody></a:tc><a:tc><a:txBody><a:p>' + run("$42,000") + '</a:p></a:txBody></a:tc></a:tr>'
        '</a:tbl></a:graphicData></a:graphic></p:graphicFrame></p:spTree>')),
]
write("sample.pptx", pptx)

S = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
xlsx = [
    ("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>'),
    ("docProps/core.xml", CORE.format("Regional Plan")),
    ("xl/workbook.xml", f'<?xml version="1.0"?><workbook {S} {R}><sheets>'
     '<sheet name="Revenue" sheetId="1" r:id="rId2"/><sheet name="Owners &amp; Notes" sheetId="2" r:id="rId1"/></sheets></workbook>'),
    ("xl/_rels/workbook.xml.rels", f'<?xml version="1.0"?><Relationships xmlns="{PKG}">'
     f'<Relationship Id="rId1" Type="{REL}/worksheet" Target="worksheets/sheet2.xml"/>'
     f'<Relationship Id="rId2" Type="{REL}/worksheet" Target="/xl/worksheets/sheet1.xml"/></Relationships>'),
    ("xl/sharedStrings.xml", f'<?xml version="1.0"?><sst {S} count="5" uniqueCount="5">'
     '<si><t>Region</t></si><si><t>Revenue</t></si><si><t>West</t></si>'
     '<si><r><t>East </t></r><r><rPr><b/></rPr><t>Coast</t></r><rPh sb="0" eb="1"><t>PHONETIC</t></rPh></si>'
     '<si><t>Active</t></si></sst>'),
    ("xl/worksheets/sheet1.xml", f'<?xml version="1.0"?><worksheet {S}><sheetData>'
     '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c><c r="D1" t="s"><v>4</v></c></row>'
     '<row r="2"><c r="A2" t="s"><v>2</v></c><c r="B2"><v>120000</v></c><c r="D2" t="b"><v>1</v></c></row>'
     '<row r="3"><c r="A3" t="s"><v>3</v></c><c r="B3"><v>98500.5</v></c><c r="D3" t="b"><v>0</v></c></row>'
     '<row r="4"/>'
     '</sheetData></worksheet>'),
    ("xl/worksheets/sheet2.xml", f'<?xml version="1.0"?><worksheet {S}><sheetData>'
     '<row r="1"><c r="A1" t="inlineStr"><is><t>Owner: Priya &lt;finance&gt;</t></is></c></row>'
     '</sheetData></worksheet>'),
]
write("sample.xlsx", xlsx)

with open(os.path.join(HERE, "sample.csv"), "w", encoding="utf-8", newline="") as f:
    f.write('Customer,Plan,Notes\r\n'
            'Acme Corp,Pro,"Renewal due Nov 1, 2026"\r\n'
            '"Globex, Inc.",Basic,"Asked about ""SSO""\nand audit logs"\r\n'
            ',,\r\n')
print("ok")
