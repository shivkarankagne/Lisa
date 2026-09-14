# LISA — Retrieval Engine Test Suite

This directory contains all tests for the retrieval engine (Module 5).

It is not a general test directory. Each file here exists for a specific
reason and is documented below.

## Layout

| File | Purpose |
| :--- | :--- |
| `test_retrieval.c` | Differential test: scalar reference vs naive brute-force sort |
| `test_retrieval_asm.c` | Differential test: scalar reference vs ARM64 assembly kernel |
| `test_edge_cases.c` | Edge cases: NULL pointers, zero/negative inputs, k>n, dim=1, dim % 4 != 0 |
| `dump_case.c` | Dump scalar and asm outputs for a single case (manual inspection) |
| `fuzz_gen.c` | Deterministic per-seed case generator (dump to file) |
| `fuzz_compare.py` | Compare scalar and asm blocks with stated tolerance |
| `fuzz_run.sh` | Run fuzz_gen across many seeds, compare with fuzz_compare.py |
| `fuzz_debug.c` | Debug helper, not part of the standard test run |

## How to build and run

### Scalar vs naive sort

    gcc -O0 -g -o test_retrieval \
        test_retrieval.c \
        ../src/retrieval/retrieval_scalar.c \
        -lm
    ./test_retrieval

### Scalar vs assembly

    gcc -O0 -g -o test_retrieval_asm \
        test_retrieval_asm.c \
        ../src/retrieval/retrieval_scalar.c \
        ../src/kernels/arm64/lisa_asm_wrapper.c \
        ../src/kernels/arm64/lisa_ultra_mac.s \
        -lm
    ./test_retrieval_asm

### Edge cases

    gcc -O0 -g -o test_edge_cases \
        test_edge_cases.c \
        ../src/retrieval/retrieval_scalar.c \
        ../src/kernels/arm64/lisa_asm_wrapper.c \
        ../src/kernels/arm64/lisa_ultra_mac.s \
        -lm
    ./test_edge_cases

### Edge cases with sanitizers

    gcc -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        -o test_edge_cases_asan \
        test_edge_cases.c \
        ../src/retrieval/retrieval_scalar.c \
        ../src/kernels/arm64/lisa_asm_wrapper.c \
        ../src/kernels/arm64/lisa_ultra_mac.s \
        -lm
    ./test_edge_cases_asan

### Fuzz harness

    gcc -O0 -g -o fuzz_gen \
        fuzz_gen.c \
        ../src/retrieval/retrieval_scalar.c \
        ../src/kernels/arm64/lisa_asm_wrapper.c \
        ../src/kernels/arm64/lisa_ultra_mac.s \
        -lm

    ./fuzz_run.sh 200 12345

## Comparison rule for fuzz_run.sh

The scalar and assembly kernels compute the same L2 squared distance.
They do not produce bit-identical floating-point results, because
floating-point addition is not associative and the two implementations
accumulate in a different order (scalar: one float at a time;
assembly: NEON fmla over 4-float vectors, then horizontal sum).

We therefore compare as follows:

- **Indices must match exactly.**
  Any difference in the top-k index sequence is a real failure.

- **Distances must match within a stated tolerance:**

      |d_s - d_a| <= max(ABS_TOL, REL_TOL * |d_s|)

  with ABS_TOL = 1e-4 and REL_TOL = 1e-4.

Justification for the tolerance:

- IEEE 754 single precision has about 7 decimal digits of precision.
- Accumulating over hundreds of dimensions can produce rounding errors
  on the order of 1e-5 relative.
- A tolerance of 1e-4 is one order of magnitude above that, which is
  the standard engineering margin for float accumulation.
- The tolerance is not used to hide real bugs. Indices are still compared
  exactly, and any distance difference beyond the tolerance is a failure.

## What these tests verify

- The scalar reference matches a naive brute-force sort (top-k correct).
- The scalar reference and the assembly kernel produce equivalent top-k
  on arbitrary dimensions, arbitrary n, and arbitrary k.
- NULL pointers, zero/negative inputs, k>n, dim=1, and dim not a multiple
  of 4 are handled without undefined behavior (verified with sanitizers).
- The fuzz harness exercises the kernels on many random cases from seeds.

## What these tests do NOT verify

- Performance. See ../benchmark/ for that.
- Storage, documents, embeddings, RAG, inference. Out of scope for this
  work package.
- ANN behavior. Not implemented.

## Notes

- `-O0` is used deliberately for comparison-sensitive tests.
  On this toolchain, at `-O2`, the in-process comparison has been observed
  to miscompile and report false failures. The fuzz harness avoids this by
  dumping results and comparing them in Python.

## Order differences at ties

Two top-k results are considered equivalent if:

1. The set of top-k indices is identical.
2. For each index present in both, the distance matches within:

       |d_s - d_a| <= max(1e-4, 1e-4 * |d_s|)

3. If the order differs at some position, the distances of the two
   indices at that position must be within the same tolerance of each
   other in both implementations. In that case the two vectors are
   equidistant at comparison precision, and the order is decided by
   floating-point rounding.

If any of the three fails, the case is reported as a real failure with
the exact case, position, indices, and distances.

This rule is implemented in `fuzz_compare.py` and used by `fuzz_run.sh`.
