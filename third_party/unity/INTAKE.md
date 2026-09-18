# Unity — intake record

| Field | Value |
| :--- | :--- |
| Project | Unity Test (ThrowTheSwitch) |
| Source URL | https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.7.0.tar.gz |
| Version / tag | v2.7.0 (tag object b6763fbd9cedfacaa89e2ad9fd00d615a234e355) |
| Commit or archive hash | SHA-256 e84eb301ca7967831e68b1728f911e87fa2d345d8ddb64f897bc2f2ee24a321c |
| License (SPDX) | MIT |
| Files used | `src/unity.c`, `src/unity.h`, `src/unity_internals.h` |
| Local modifications | None |
| Required notices | MIT license text (test-only; not distributed in the binary) |
| Added binary size | 0 — test-only |
| Shipped in binary | No (test-only) |
| Date of intake | 2026-09-18 |
| Reviewed by | |

## Why this component

A small, pure-C unit-test framework with no dependencies. New LISA tests
use it instead of hand-written check macros. Existing tests keep their
current style (converting them is out of scope).

## Build configuration

Built as the static library `lisa_unity`, linked only into test
executables.

## Update procedure

Download the new tag archive, record its SHA-256, replace the three
files, update this record, run the full test suite.
