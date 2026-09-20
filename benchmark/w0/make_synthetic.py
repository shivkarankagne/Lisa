#!/usr/bin/env python3
# SPDX-License-Identifier: BUSL-1.1
"""Generate the synthetic half of the W0/W12 corpus: ordinary business
documents (agreements, policies, invoices, reports, logs) whose facts are
known exactly, so answers can be scored. Deterministic: the same bytes
every run.

    benchmark/w0/make_synthetic.py [<dir>]      (default: benchmark/w0/corpus)

Formats: .md, .txt, .docx, and two image-only PDFs (scanned agreements)
when run on macOS, to exercise text recognition.
"""

import os
import subprocess
import sys
import zipfile

DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
OUT = os.path.join(DIR, "synthetic")
os.makedirs(OUT, exist_ok=True)

# ---- plain text and Markdown --------------------------------------------------

TEXT = {
"hr-leave-policy.md": """# Leave policy (Snowdrop Medtech)

Effective 1 April 2026.

## Annual leave

Every confirmed employee receives **24 days** of paid annual leave per
calendar year, credited monthly at two days per month.

Up to **10 unused days** may be carried into the next year with the
written approval of the reporting manager. Days beyond that lapse on
31 March.

## Sick leave

Twelve days per year. A medical certificate is required from the third
consecutive day.

## Notice period

Employees who resign serve **two months** of notice. The company may
waive part of it in writing.
""",

"expense-policy.md": """# Travel and expense policy

- Domestic per diem: **Rs. 2,000 per day**, no receipts required.
- Hotel: up to Rs. 6,500 per night in metro cities, Rs. 4,000 elsewhere.
- Air travel: economy class only; business class needs a director's
  written approval.
- Any single expense above **Rs. 25,000** needs prior approval from the
  finance head.
- Claims must be filed within 30 days of the expense. Later claims are
  not reimbursed.
""",

"warranty-terms.md": """# Warranty — Snowdrop reader

The Snowdrop reader carries a warranty of **24 months** from the date of
delivery, covering manufacturing defects.

The warranty does not cover water damage, a broken display, use with
third-party cartridges, or any unit whose serial number has been
removed.

Replacement units are shipped within five working days of the defective
unit reaching our service centre in Hyderabad.
""",

"sla-support.md": """# Support service level agreement

| Severity | First response | Resolution target |
| :--- | :--- | :--- |
| Critical (line stopped) | 4 hours | 24 hours |
| High | 8 business hours | 3 working days |
| Normal | 2 business days | 10 working days |

Platform uptime commitment: **99.5 percent** per calendar quarter,
measured excluding announced maintenance windows.

Support hours are 09:00 to 19:00 IST, Monday to Saturday. Critical
issues are handled at any hour on the emergency number.
""",

"maintenance-log-pump.md": """# Maintenance log — pump P-7

| Date | Work | Technician |
| :--- | :--- | :--- |
| 2026-01-12 | Routine inspection, vibration 18 Hz, normal | R. Mehta |
| 2026-02-09 | Vibration rising, 34 Hz; lubrication done | R. Mehta |
| 2026-03-03 | Vibration **42 Hz**, above the 40 Hz limit; bearing ordered | S. Iyer |
| 2026-03-17 | **Main bearing replaced**; vibration back to 16 Hz | S. Iyer |

Bearings on this pump must be inspected every **500 running hours**.
""",

"incident-report-2026-03.md": """# Incident report 2026-03-17

**Impact:** production line 2 stopped for **six hours**, from 06:10 to
12:10, and 1,400 units were not produced.

**Root cause:** the main bearing of pump P-7 seized after weeks of
rising vibration. The 40 Hz vibration limit was exceeded on 3 March and
the replacement bearing arrived only on 17 March.

**Corrective action:** spare bearings for P-7 and P-9 are now kept in
store, and the vibration alarm limit is lowered to 35 Hz.
""",

"product-spec-snowdrop.md": """# Snowdrop reader — specification

| | |
| :--- | :--- |
| Weight | **320 g** with battery |
| Battery life | **8 hours** of continuous use |
| Charging | USB-C, 2 hours to full |
| Sample | one drop of blood, 15 microlitres |
| Result time | 4 minutes |
| Operating temperature | 10 to 40 degrees Celsius |
| Storage of cartridges | 2 to 8 degrees Celsius |

The reader stores 500 results and transfers them over Bluetooth 5.0.
""",

"safety-sop.md": """# Standard operating procedure — floor safety

1. Safety shoes and eye protection are worn on the production floor at
   all times.
2. Fire drills take place **every quarter**. On the alarm, leave the
   building and assemble at **Gate B**, where the floor warden counts
   everyone.
3. Chemical spills larger than one litre are reported to the shift
   supervisor before cleaning.
4. Only trained staff operate the autoclave; the log is signed after
   every cycle.
""",

"calibration-certificate.txt": """CALIBRATION CERTIFICATE

Certificate number: CAL-2026-0417
Instrument: Digital thermometer, model DT-9
Serial number: 88241
Calibrated on: 2026-02-14
Calibrated by: Adilabad Metrology Services

Result: within tolerance. Measured deviation +/- 0.2 degrees Celsius
across the range 0 to 60 degrees Celsius.

This certificate is valid for TWELVE MONTHS from the date of
calibration, that is until 2027-02-14.
""",

"invoice-2026-114.txt": """INVOICE

Invoice number: SNW-2026-114
Date: 2026-03-05
Bill to: Kinwat District Hospital, Nanded

Item                                 Qty     Rate        Amount
Snowdrop reader                        5   22,000      110,000
Test cartridges (box of 50)           10    2,500       25,000
Installation and training              1   12,500       12,500

Subtotal                                              147,500
GST 12 percent                                         17,700
TOTAL DUE                                             165,200

Payment terms: 30 days from the invoice date.
Late payment attracts interest of 1.5 percent per month.
Bank: State Bank of India, account 3388291047, IFSC SBIN0004321.
""",

"insurance-policy.txt": """GROUP HEALTH INSURANCE - SUMMARY

Policy number: GHI-88-2026-114
Insurer: Nirmal General Insurance
Period: 1 April 2026 to 31 March 2027

Sum insured per family: Rs. 5,00,000
Deductible per claim: Rs. 25,000
Room rent limit: Rs. 5,000 per day
Pre-existing conditions: covered after 24 months of continuous cover
Maternity: covered up to Rs. 50,000 after a nine month waiting period

Claims must be intimated within 48 hours of admission on 1800-111-234.
""",

"bank-statement-extract.txt": """ACCOUNT STATEMENT (extract)

Account: 3388291047, Snowdrop Medtech Private Limited
Period: 1 March 2026 to 31 March 2026

Date        Description                      Debit      Credit    Balance
2026-03-02  Opening balance                                     4,12,880
2026-03-05  NEFT Kinwat District Hospital               165,200  5,78,080
2026-03-09  Salary transfer                  3,10,000            2,68,080
2026-03-14  Rent - Yenda premises              2,000            2,66,080
2026-03-21  Supplier payment - Kirloskar      48,500            2,17,580
2026-03-28  Interest credited                              1,180  2,18,760

Closing balance on 31 March 2026: Rs. 2,18,760.
""",

"travel-itinerary.txt": """TRAVEL ITINERARY - S. KAGNE

Trip: Adilabad to Bengaluru, medical devices conference

Outbound: 12 April 2026, flight 6E-542, Hyderabad 08:15 to Bengaluru
09:35. Report at the airport by 07:15.

Hotel: Sunrise Residency, Indiranagar, 12 to 15 April, booking
reference SR-2026-8841, check-in 14:00, check-out 11:00.

Return: 15 April 2026, flight 6E-771, Bengaluru 19:40 to Hyderabad
21:00.

Conference pass collected at counter 4; the session on point-of-care
diagnostics is on 13 April at 11:30 in hall 2.
""",

"training-manual.md": """# Using the Snowdrop reader

1. Charge the reader for two hours before first use.
2. Press and hold the power button for three seconds.
3. Scan the cartridge barcode when the screen asks for it.
4. Apply one drop of blood to the marked circle on the cartridge.
5. Insert the cartridge; the result appears in **four minutes**.
6. Press "Save" to store the result, or "Send" to transfer it over
   Bluetooth.

If the screen shows error **E-14**, the cartridge has expired. Error
E-22 means the sample was too small; repeat with a fresh cartridge.
""",

"hindi-notice.md": """# कार्यालय सूचना

दिनांक: 5 मार्च 2026

सभी कर्मचारियों को सूचित किया जाता है कि होली के अवसर पर कार्यालय
**25 मार्च 2026** को बंद रहेगा।

मासिक किराया हर महीने की **पाँच तारीख** तक जमा करना अनिवार्य है।
देर से भुगतान पर प्रति माह डेढ़ प्रतिशत ब्याज लिया जाएगा।

आपातकालीन स्थिति में श्री आर. मेहता से 9949812816 पर संपर्क करें।
""",

"marathi-letter.txt": """कार्यालयीन पत्र

दिनांक: 10 मार्च 2026

विषय: कर्मचारी प्रशिक्षण कार्यक्रम

सर्व कर्मचाऱ्यांसाठी प्रशिक्षण कार्यक्रम 20 मार्च 2026 रोजी सकाळी
10 वाजता सभागृहात आयोजित करण्यात आला आहे. उपस्थिती अनिवार्य आहे.

प्रशिक्षणाचा कालावधी तीन दिवसांचा असेल.

धन्यवाद,
व्यवस्थापक
""",

"meeting-notes-2026-02-18.md": """# Weekly review, 18 February 2026

Present: S. Kagne, R. Mehta, P. Nair, A. Kulkarni

- Cartridge yield improved to **92 percent** after the new sealing
  process; target for March is 95 percent.
- The Kinwat hospital order (five readers) ships on 5 March.
- Pump P-7 vibration is rising again; R. Mehta to order a spare bearing.
- Hiring: two field engineers approved, joining by 1 April.
- Next review: 25 February at 10:00.
""",

"quality-checklist.txt": """INCOMING QUALITY CHECK - CARTRIDGES

Batch: CT-2026-031
Received: 2026-03-08
Quantity: 2,000 cartridges

Checks performed:
1. Packaging intact                               PASS
2. Expiry date at least 9 months away             PASS (expiry 2027-06)
3. Cold chain log, 2 to 8 degrees C maintained    PASS
4. Sample test, 20 cartridges                     PASS (20 of 20)
5. Barcode scan                                   PASS

Accepted by: A. Kulkarni. Rejected quantity: zero.
Cartridges failing check 3 are quarantined and returned to the supplier
within seven days.
""",
}

for name, body in TEXT.items():
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write(body)

# ---- Word documents ------------------------------------------------------------

def docx(name, title, paragraphs):
    W = 'xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
    body = "".join(
        "<w:p><w:r><w:t xml:space=\"preserve\">%s</w:t></w:r></w:p>"
        % p.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        for p in paragraphs)
    document = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                f'<w:document {W}><w:body>{body}<w:sectPr/></w:body></w:document>')
    core = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"'
            ' xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>%s</dc:title></cp:coreProperties>' % title)
    types = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
             '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
             '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>'
             '</Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
            '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>'
            '</Relationships>')
    with zipfile.ZipFile(os.path.join(OUT, name), "w", zipfile.ZIP_DEFLATED) as z:
        for part, data in [("[Content_Types].xml", types), ("_rels/.rels", rels),
                           ("word/document.xml", document), ("docProps/core.xml", core)]:
            info = zipfile.ZipInfo(part, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8"))

docx("rent-agreement-yenda.docx", "Rent agreement — Yenda premises", [
    "RENT AGREEMENT",
    "This agreement is made on 31 March 2026 between Kagne Narayana (the Owner) and "
    "Snowdrop Medtech Private Limited (the Tenant) for the premises at H.No 1-15, Yenda, "
    "Utnoor, Adilabad 504346.",
    "1. The Tenant shall pay a monthly rent of Rs. 18,500 (Rupees Eighteen Thousand Five "
    "Hundred only), payable by the fifth day of each month, excluding electricity and water.",
    "2. The Tenant has paid a refundable security deposit of Rs. 1,11,000, equal to six months' rent.",
    "3. This lease is for eleven months from 1 April 2026 and may be renewed by mutual consent.",
    "4. Either party may end this agreement by giving two months' written notice.",
    "5. The Tenant shall not sublet the premises without the Owner's written consent.",
])

docx("employment-offer-sharma.docx", "Offer of employment — A. Sharma", [
    "OFFER OF EMPLOYMENT",
    "Dear Anjali Sharma,",
    "We are pleased to offer you the position of Field Application Engineer at Snowdrop "
    "Medtech Private Limited, reporting to the Head of Operations at our Adilabad office.",
    "Your annual cost to company is Rs. 9,60,000, paid monthly. This includes a performance "
    "bonus of up to Rs. 96,000, paid after the annual review.",
    "Your joining date is 1 April 2026. The probation period is six months, during which "
    "either side may end the employment with one month's notice.",
    "Please confirm your acceptance by 20 March 2026.",
])

docx("vendor-agreement-kirloskar.docx", "Vendor agreement — bearings", [
    "VENDOR AGREEMENT",
    "Between Snowdrop Medtech Private Limited (the Buyer) and Kirloskar Spares (the Supplier), "
    "dated 15 January 2026.",
    "Payment terms: invoices are paid within 45 days of delivery and acceptance. Late payment "
    "attracts interest of 1.5 percent per month.",
    "Delivery: within 21 days of a purchase order. Delay beyond 30 days allows the Buyer to "
    "cancel the order without penalty.",
    "Warranty: 12 months on all supplied bearings from the date of delivery.",
    "This agreement runs for two years and renews automatically unless ended with 60 days' notice.",
])

docx("supplier-quote-cartridges.docx", "Quotation — cartridges", [
    "QUOTATION QT-2026-233",
    "Date: 2 February 2026. Valid for 30 days.",
    "Item: test cartridges, box of 50.",
    "Unit price: Rs. 1,250 per box for orders up to 400 boxes; Rs. 1,150 per box above that.",
    "Minimum order quantity: 500 boxes.",
    "Delivery: 14 days from the order, ex-works Pune.",
    "Payment: 50 percent advance, balance against delivery.",
])

docx("board-minutes-2026-02.docx", "Board minutes — February 2026", [
    "MINUTES OF THE BOARD MEETING",
    "Held on 24 February 2026 at the registered office, Adilabad.",
    "Present: S. Kagne (Director), K. Narayana (Director), P. Nair (Invitee).",
    "Resolved that the authorised share capital of the company be increased to Rs. 25,00,000.",
    "Resolved that the company open a current account with the State Bank of India, Adilabad "
    "branch, and that S. Kagne be authorised to operate it.",
    "Resolved that the annual audit for 2025-26 be entrusted to Mohindra & Associates, "
    "Chartered Accountants, for a fee of Rs. 45,000.",
])

# ---- scanned (image-only) PDFs, macOS only ---------------------------------------

SCANS = {
"scanned-delivery-note.pdf": "DELIVERY NOTE\n\nDN-2026-0455, dated 6 March 2026\n\n"
                             "Delivered to Kinwat District Hospital:\n"
                             "5 Snowdrop readers, serial numbers\nSNW-1121 to SNW-1125.\n\n"
                             "Received in good condition by Dr. P. Rao.",
"scanned-site-approval.pdf": "SITE APPROVAL\n\nThe district collector has approved the\n"
                             "installation of diagnostic equipment at\nPrimary Health Centre, Utnoor,\n"
                             "on 18 February 2026.\n\nApproval number: DC-UTN-2026-77.\n"
                             "Valid for one year from the date of issue.",
}

if sys.platform == "darwin":
    swift = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_make_scan.swift")
    with open(swift, "w", encoding="utf-8") as f:
        f.write('''import AppKit
import PDFKit
let out = CommandLine.arguments[1]
let text = CommandLine.arguments[2]
let size = NSSize(width: 1240, height: 1754)
let img = NSImage(size: size)
img.lockFocus()
NSColor(calibratedWhite: 0.95, alpha: 1).setFill()
NSRect(origin: .zero, size: size).fill()
let style = NSMutableParagraphStyle(); style.lineSpacing = 12
text.draw(in: NSRect(x: 120, y: 300, width: 1000, height: 1300), withAttributes: [
    .font: NSFont(name: "Times New Roman", size: 44) ?? NSFont.systemFont(ofSize: 44),
    .foregroundColor: NSColor(calibratedWhite: 0.08, alpha: 1), .paragraphStyle: style])
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
let jpeg = rep.representation(using: .jpeg, properties: [.compressionFactor: 0.5])!
let doc = PDFDocument()
doc.insert(PDFPage(image: NSImage(data: jpeg)!)!, at: 0)
doc.write(to: URL(fileURLWithPath: out))
''')
    for name, text in SCANS.items():
        path = os.path.join(OUT, name)
        if not os.path.exists(path):
            subprocess.run(["swift", swift, path, text], check=True)
    os.remove(swift)

print("%d synthetic documents in %s" % (len(os.listdir(OUT)), OUT))
