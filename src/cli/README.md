# LISA CLI

Command-line interface for the LISA retrieval engine.

## Usage

    lisa --index <file> --dim <int> --query <file> [--topk <int>]
    lisa --collection <dir> --query <file> [--topk <int>]

## Flags

| Flag | Required | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `--index` | one of index/collection | — | Path to binary vector file |
| `--collection` | one of index/collection | — | Path to a storage collection directory |
| `--dim` | required with `--index`; optional with `--collection` | — | Dimension of vectors |
| `--query` | yes | — | Path to query file |
| `--topk` | no | 5 | Number of results |
| `--help` | no | — | Print usage |

`--index` and `--collection` are mutually exclusive.

## Input formats

### Index file (`--index`)

Binary, little-endian:

- 8 bytes header: `int32 n`, `int32 dim`
- `n * dim` float32 values, row-major

### Collection directory (`--collection`)

Directory previously created by `storage_create`. Contains:

- `header.bin` — 16 bytes: magic "LISA", version, n, dim
- `vectors.bin` — `n * dim` float32, row-major

The vector layout is identical to the index file. Searching a collection
produces the same results as searching an equivalent index file.

### Query file (`--query`)

Text. Floats separated by whitespace or commas. Must contain exactly `dim` values.

## Output

One line per result, ascending by distance:

    <index> <distance>

No header. No decoration. Machine-readable.

## Exit codes

| Code | Meaning |
| :--- | :--- |
| 0 | Success |
| 1 | Invalid arguments |
| 2 | Index file error |
| 3 | Query file error |
| 4 | Dimension mismatch |
| 5 | Retrieval engine error |
| 6 | Storage error |

## Examples

### Search a binary index file

    lisa --index vectors_768.bin --dim 768 --topk 5 --query query_768.txt

### Search a storage collection

    lisa --collection /path/to/collection --topk 5 --query query_768.txt

Both produce the same output format:

    6797 108.174782
    5564 109.033485
    1245 109.476166
    8030 110.118279
    3305 110.140114

## Build

    gcc -O0 -g -o build/lisa \
        src/cli/main.c \
        src/retrieval/retrieval_scalar.c \
        src/kernels/arm64/lisa_asm_wrapper.c \
        src/kernels/arm64/lisa_ultra_mac.s \
        src/storage/storage.c \
        -lm

## Non-goals

This CLI does not:

- serve HTTP
- run as a daemon
- create collections
- insert, update, or delete records
- provide JSON output
- provide configuration files

Those are separate work packages.

## Building

From the repository root:

    make

Produces `build/lisa`. See the root `README.md` for the full build
and test instructions.

The link line is defined only in the root `Makefile`. Do not duplicate
it here.

## Server mode

    lisa --serve --port <1-65535>

Starts the read-only HTTP API. Blocks until terminated.

Endpoints:

    GET  /health
         -> 200, body "ok\n"

    POST /search?collection=<path>&topk=<int>
         Headers: Content-Type: application/octet-stream
         Body:    dim * float32 LE
         -> 200, body text, one line per result:
                <index> <distance>\n
         -> 400 malformed request
         -> 404 collection not found
         -> 500 internal error

The server binds to 127.0.0.1 only. No TLS, no authentication,
no write endpoints.
