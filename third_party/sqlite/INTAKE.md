# SQLite — intake record

| Field | Value |
| :--- | :--- |
| Project | SQLite |
| Source URL | https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip |
| Version / tag | 3.53.4 (amalgamation 3530400) |
| Commit or archive hash | SHA3-256 628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e (matches sqlite.org download page) |
| License (SPDX) | blessing (public domain) |
| Files used | `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h` |
| Local modifications | None |
| Required notices | None required; attribution given in THIRD_PARTY_NOTICES |
| Added binary size | Measured when first linked into `lisa` (W2) |
| Shipped in binary | Yes (from W2) |
| Date of intake | 2026-09-18 |
| Reviewed by | |

## Why this component

Storage v2 (plan decision 4) keeps vectors in LISA's own memory-mapped
file and uses SQLite for chunk metadata, transactions, the write-ahead
log, locking, and FTS5 keyword search (BM25). SQLite is public domain,
a single C file, portable to every future platform, and the most widely
deployed embedded database. Writing an equivalent transactional store
in-house would take months.

## Build configuration

Set in the top-level `CMakeLists.txt` (target `lisa_sqlite`):

| Option | Why |
| :--- | :--- |
| `SQLITE_ENABLE_FTS5` | Keyword search (W7) |
| `SQLITE_THREADSAFE=1` | Ingest runs in a background thread (W6) |
| `SQLITE_DQS=0` | Reject double-quoted string literals (safer SQL) |
| `SQLITE_DEFAULT_MEMSTATUS=0` | Not needed; small speedup |
| `SQLITE_OMIT_LOAD_EXTENSION` | No runtime loading of native code |
| `SQLITE_OMIT_DEPRECATED` | Smaller, no legacy APIs |
| `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` | NORMAL sync in WAL mode: durable at checkpoints, safe against corruption |

## Update procedure

Download the new amalgamation zip from sqlite.org, verify its SHA3-256
against the download page, replace the three files, update this record,
and run the full test suite in both build configurations.
