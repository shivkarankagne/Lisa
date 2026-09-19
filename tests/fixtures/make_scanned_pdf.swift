// SPDX-License-Identifier: BUSL-1.1
// Generate tests/fixtures/scanned.pdf: two pages that are only images of
// text (no text layer), like a phone scan, to test OCR. Run from the
// repository root with `swift tests/fixtures/make_scanned_pdf.swift`; the
// output is committed.
import AppKit
import PDFKit

let pages = [
    "RENT AGREEMENT\n\nThis agreement is made between the Owner\nand the Tenant for the premises at Yenda.\n\nCont. 2nd Page",
    "1. The Tenant shall pay a monthly rent of\nRs. 2,000 (Rupees Two Thousand only).\n\n2. The lease is for eleven months.",
]

func pageImage(_ text: String) -> NSImage {
    let size = NSSize(width: 1240, height: 1754)  // A4 at 150 dpi
    let img = NSImage(size: size)
    img.lockFocus()
    NSColor(calibratedWhite: 0.96, alpha: 1).setFill()   // off-white paper
    NSRect(origin: .zero, size: size).fill()
    let style = NSMutableParagraphStyle()
    style.lineSpacing = 10
    let attrs: [NSAttributedString.Key: Any] = [
        .font: NSFont(name: "Times New Roman", size: 40) ?? NSFont.systemFont(ofSize: 40),
        .foregroundColor: NSColor(calibratedWhite: 0.1, alpha: 1),
        .paragraphStyle: style,
    ]
    text.draw(in: NSRect(x: 110, y: 200, width: 1020, height: 1400), withAttributes: attrs)
    img.unlockFocus()
    // Flatten to a JPEG, like a camera scan: no vector text survives in the PDF.
    let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
    let jpeg = rep.representation(using: .jpeg, properties: [.compressionFactor: 0.5])!
    return NSImage(data: jpeg)!
}

let doc = PDFDocument()
for (i, t) in pages.enumerated() {
    doc.insert(PDFPage(image: pageImage(t))!, at: i)
}
let out = URL(fileURLWithPath: "tests/fixtures/scanned.pdf")
doc.write(to: out)
print(out.path)
