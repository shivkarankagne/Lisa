#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
"""Generate tests/fixtures/sample.docx: a minimal Word document exercising
what LISA's .docx extractor must handle — paragraphs, runs split
mid-word, tabs and line breaks, a table, XML entities, Hindi text,
tracked deletions (must not appear) and a title in docProps/core.xml.
Run from the repository root; the output is committed."""

import os
import zipfile

W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'

document = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document {W}><w:body>
<w:p><w:pPr><w:pStyle w:val="Title"/></w:pPr><w:r><w:t>RENT AGREEMENT</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">The Tenant shall pay a monthly rent of </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>Rs. 3,</w:t></w:r><w:r><w:t>000 (Rupees Three Thousand only)</w:t></w:r><w:r><w:t xml:space="preserve"> per month.</w:t></w:r></w:p>
<w:p><w:r><w:t>Deposit</w:t></w:r><w:r><w:tab/></w:r><w:r><w:t>Rs. 6,000</w:t></w:r><w:r><w:br/></w:r><w:r><w:t>Notice period: one month</w:t></w:r></w:p>
<w:p><w:r><w:delText>This deleted sentence must not be indexed.</w:delText></w:r><w:r><w:t>Terms &amp; conditions apply &lt;see clause 4&gt;.</w:t></w:r></w:p>
<w:tbl>
<w:tr><w:tc><w:p><w:r><w:t>Item</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Amount</w:t></w:r></w:p></w:tc></w:tr>
<w:tr><w:tc><w:p><w:r><w:t>Maintenance</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Rs. 500</w:t></w:r></w:p></w:tc></w:tr>
</w:tbl>
<w:p><w:r><w:t>किराया हर महीने की पाँच तारीख तक देना होगा।</w:t></w:r></w:p>
<w:sectPr/>
</w:body></w:document>
"""

core = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
 xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Sample Rent Agreement</dc:title></cp:coreProperties>
"""

content_types = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
</Types>
"""

rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
</Relationships>
"""

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sample.docx")
with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
    for name, data in [("[Content_Types].xml", content_types), ("_rels/.rels", rels),
                       ("word/document.xml", document), ("docProps/core.xml", core)]:
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))  # fixed: reproducible bytes
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, data.encode("utf-8"))
print(path)
