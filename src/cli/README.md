# LISA CLI

Command-line interface for the LISA retrieval engine.

## Usage

    lisa --index <file> --dim <int> --query <file> [--topk <int>]

## Flags

| Flag | Required | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `--index` | yes | — | Path to binary vector file |
| `--dim` | yes | — | Dimension of vectors |
| `--query` | yes | — | Path to query file |
| `--topk` | no | 5 | Number of results |
| `--help` | no | — | Print usage |

## Input formats

### Index file

Binary, little-endian:

- 8 bytes header: `int32 n`, `int32 dim`
- `n * dim` float32 values, row-major

### Query file

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

## Example

    lisa --index vectors_768.bin --dim 768 --topk 5 --query query_768.txt

Output:

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
        -lm

## Non-goals

This CLI does not:

- serve HTTP
- run as a daemon
- manage storage
- manage memory
- provide JSON output
- provide configuration files

Those are separate work packages.
