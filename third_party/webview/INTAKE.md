# webview — intake record

| Field | Value |
| :--- | :--- |
| Project | webview/webview (cross-platform web view window, C API) |
| Source URL | https://github.com/webview/webview/archive/refs/tags/0.12.0.tar.gz |
| Version / tag | 0.12.0 (commit 3ab4b5d722438fc8a13e6ca830c5e2372d19a01d) |
| Commit or archive hash | SHA-256 e2c8d0bed3fcd13e624074448b7d3b1d0b0d03546a15839f17ee0013f2cba1db |
| License (SPDX) | MIT |
| Files used | `core/include/webview.h`, `core/include/webview/webview.h`, `core/src/webview.cc` |
| Local modifications | None |
| Required notices | MIT license text |
| Added binary size | Measured with W10 (report 011) |
| Shipped in binary | Yes |
| Date of intake | 2026-09-19 |
| Reviewed by | |

## Why this component

Plan decision 14 and §5 (W10): `lisa gui` shows the embedded web UI in
a native window using the operating system's web view (WKWebView on
macOS; WebView2 / WebKitGTK on other platforms later). No Electron, no
bundled browser.

## Build configuration

Built as `lisa_webview` (C++11, warnings suppressed) with
`WEBVIEW_STATIC`; links the system WebKit and Cocoa frameworks. macOS
only in 1.0; other platforms add their web view dependency when ported.

## Update procedure

Download the new tag archive, record its SHA-256, replace the three
files, update this record, run the full test suite and open `lisa gui`.
