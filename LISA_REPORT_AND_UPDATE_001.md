# LISA — Report and Update 001

**Date:** 2026-09-14
**Type:** Combined status update and work report
**Governing document:** ASTRA_INSTRUCTIONS.md
**Work package:** Retrieval engine — first correctness milestone

---

# PART 1 — STATUS UPDATE

## 1.1 Where we are right now

LISA's retrieval engine has reached its first correctness milestone.

The scalar reference is correct. The ARM64 assembly kernel is correct. They agree with each other on every case tested, verified by two independent methods. A real benchmark has been run under stated conditions. Several real bugs were found and fixed.

This is not the end of the retrieval work package. It is the first point at which we can say, with evidence, that the retrieval foundation is correct.

## 1.2 What is done

| Item | Status |
| :--- | :--- |
| Scalar reference implementation | DONE |
| Stable retrieval API | DONE |
| Correct top-k selection | DONE |
| ARM64 NEON assembly kernel | DONE |
| Arbitrary-dimension handling | DONE |
| Differential testing | DONE |
| Compiler-independent verification | DONE |
| Benchmark harness | DONE |
| Real benchmark under stated conditions | DONE |

## 1.3 What is not done

| Item | Status |
| :--- | :--- |
| Edge-case tests (dim=1, n=0, k=0, negative inputs) | NOT DONE |
| Sanitizers (ASan / UBSan) | NOT DONE |
| Git checkpoint for this work package | NOT DONE |
| Reproducible fuzz across compiler levels | PARTIAL |
| Storage, documents, embeddings, RAG, inference | NOT IN SCOPE |

## 1.4 What changed since the last checkpoint

- Assembly kernel had two real bugs. Both fixed.
- Wrapper now handles arbitrary dimensions by padding.
- Fuzzer's tolerance logic was found unreliable at -O2. Replaced by dump-and-compare.
- The previously reported 1.474 ms figure was invalidated and replaced by measured numbers.

## 1.5 What is next

1. Edge-case tests.
2. Sanitizers.
3. Git checkpoint.
4. Next work package, decided after 1–3.

---

# PART 2 — WORK REPORT

## 2.1 Scope

Retrieval engine work package:

- scalar reference
- assembly kernel
- arbitrary-dimension handling
- correctness verification
- benchmark measurement

Out of scope: storage, documents, embeddings, RAG, inference, HTTP, CLI, ANN.

## 2.2 Files

| File | Purpose |
| :--- | :--- |
| `src/retrieval/retrieval.h` | Stable public API |
| `src/retrieval/retrieval_scalar.c` | Scalar reference implementation |
| `src/kernels/arm64/lisa_ultra_mac.s` | ARM64 NEON assembly kernel |
| `src/kernels/arm64/lisa_asm_wrapper.c` | Top-k wrapper, arbitrary-dimension padding |
| `tests/test_retrieval.c` | Differential test: scalar vs naive sort |
| `tests/test_retrieval_asm.c` | Differential test: scalar vs assembly |
| `tests/dump_case.c` | Compiler-independent dump harness |
| `benchmark/bench_retrieval.c` | Scalar benchmark |
| `benchmark/bench_compare.c` | Scalar vs assembly benchmark |

## 2.3 Public API

```c
float lisa_l2_squared(const float* a, const float* b, int dim);

int lisa_search_exact(
    const float* query,
    const float* vectors,
    int n,
    int dim,
    int k,
    lisa_result_t* result
);
No public API changes since first definition.

2.4 Correctness verification
Scalar reference vs naive sort
All cases PASS. PASS: all tests passed

Scalar reference vs assembly kernel
All cases PASS. PASS: scalar and assembly agree

Fuzzing at -O0
PASS: 100 iterations, no mismatches

Fuzzing at -O2
Reported false failures. Traced to compiler miscompilation of the comparison, not to a kernel bug.

Compiler-independent dump comparison
For all tested cases, scalar and assembly outputs were byte-for-byte identical.

Example, n=416 dim=1 k=17 seed=12345:

text
scalar           asm
378 0.000001456   378 0.000001456
305 0.000009635   305 0.000009635
14  0.000012929   14  0.000012929
311 0.000019101   311 0.000019101
32  0.000027810   32  0.000027810
176 0.000032181   176 0.000032181
83  0.000046064   83  0.000046064
115 0.000071636   115 0.000071636
44  0.000086956   44  0.000086956
345 0.000192640   345 0.000192640
221 0.000322579   221 0.000322579
397 0.000734014   397 0.000734014
258 0.000874366   258 0.000874366
360 0.001270273   360 0.001270273
253 0.001371092   253 0.001371092
33  0.001448455   33  0.001448455
2   0.001582270   2   0.001582270
2.5 Bugs found and fixed
Bug 1 — Kernel loop condition
The assembly kernel compared a byte offset against a float dimension. It processed only a fraction of each vector's dimensions. Fixed by computing dim * 4 once and comparing against that.

Bug 2 — Arbitrary dimensions
The kernel assumes dim % 4 == 0. Other dimensions produced wrong results. Fixed by padding query and vectors to the next multiple of 4 with zeros.

Bug 3 — Stride mismatch in first tail fix
The first padding attempt passed a reduced dimension to the kernel, which then used it as the vector stride. Vectors after index 0 were read from the wrong location. Fixed by padding the full dataset to dim_padded and calling the kernel with the padded dimension.

Bug 4 — Fuzzer tolerance formula
At -O2 the tolerance formula produced impossible values and reported false failures. Replaced by dump-and-compare, which is compiler-independent.

2.6 Benchmark
Conditions
Condition	Value
Hardware	Apple M2
OS	macOS
Dataset	10,000 random vectors
Dimension	768
k	5
Queries	100
Seed	42
Compiler	gcc -O2
Timing	CLOCK_MONOTONIC, single-threaded
Cache	Warm
Results
Implementation	min	max	mean
Scalar	5.575 ms	6.204 ms	5.644 ms
Assembly	1.620 ms	1.650 ms	1.644 ms
Speedup: 3.43x

Invalidated number
The previously reported 1.474 ms came from the buggy kernel. It is invalid. It must not be used in any README, release note, or marketing material.

2.7 Known limitations
Limitation	Impact
Top-k in C wrapper	Not optimized
No memory-mapped vectors	Vectors in memory
Single-threaded	No parallelism
In-process fuzzer unreliable at -O2	Use dump-and-compare
No sanitizers run	ASan / UBSan not applied
No CI	Tests run manually
2.8 What we do NOT claim
We do not claim 1.474 ms.

We do not claim the assembly kernel beats Qdrant. That comparison is exact vs approximate.

We do not claim correctness beyond the tested inputs.

We do not claim performance beyond the stated conditions.

PART 3 — PROBLEMS AND SOLUTIONS (FOR FUTURE REFERENCE)
3.1 Toolchain problems
Symptom	Cause	Fix
Assembler rejects prfm with # and post-index	Apple syntax differs from GNU	Use prfm pldl1keep, [x7] then add x7, x7, #64
Assembler rejects ldr q4, [x0, x15, lsl #2]	No scaled register offset for 128-bit loads on Apple	Keep byte offset in register, ldr q4, [x0, x15], increment by 16
Assembler rejects fmov s0, v0.4s[0]	Apple requires constant lane index	Store to stack, load back: str s1, [sp,#-16]! / ldr s0, [sp],#16
fmov s0, #42.0 rejected	ARM64 float immediates limited	Load from .rodata
@PAGE rejected on Linux	Apple-only syntax	On Linux use :lo12:
Undefined symbol _lisa_search_ultra	macOS requires leading underscore	Prefix global symbols with _
ld: library 'System' not found	ld direct invocation lacks SDK path	Use gcc, or pass -syslibroot
taskset not found	Linux-only tool	Use renice; no CPU pinning on macOS
-fopenmp unsupported	Apple Clang lacks OpenMP	Use GCD or a GCC that supports OpenMP
3.2 Correctness bugs
Symptom	Cause	Fix
Assembly distances roughly 1/4 of scalar	Byte offset compared to float dimension	Compute dim * 4 once, compare against that
Failures only when dim % 4 != 0	Kernel processes 4 floats per iteration	Pad query and vectors to next multiple of 4
Tail fix made things worse	Kernel used the reduced dimension as stride	Pad the full dataset to dim_padded, call kernel with that
FAIL: scalar=-1 asm=-1	Test passed NULL, expected allocation	Allocate inputs in the test
3.3 Test harness problems
Symptom	Cause	Fix
Fuzzer reports diff=0 as failure	Compiler miscompiled the comparison at -O2	Use -O0 or dump-and-compare
tol negative	Tolerance formula too clever for -O2	Use absolute tolerance or dump-and-compare
File write did not take effect	Heredoc not completed	Verify with grep for a unique string after writing
3.4 Lessons
Correctness before optimization. The kernel was fixed before any benchmark was trusted.

Two independent verification methods. When the in-process fuzzer was unreliable, the dump comparison resolved the ambiguity.

Unit discipline. The byte-vs-float bug came from mixing units.

Toolchain assumptions are dangerous. Apple is not GNU. Linux tools do not exist on macOS.

Do not trust a single number. Every number comes with conditions.

A test that reports impossible values is not a test — it is a bug in the test.

PART 4 — QUICK REFERENCE
Symptom to section map
Symptom	Section
Assembler rejects prefetch, lsl, fmov	3.1
Undefined symbol in linker	3.1
fmov #float rejected	3.1
@PAGE rejected	3.1
Distances too small, or dim % 4 failures	3.2
Fuzzer reports impossible failure	3.3
PART 5 — NEXT WORK
Edge-case tests: dim = 1, n = 0, k = 0, negative inputs.

AddressSanitizer and UndefinedBehaviorSanitizer on all tests.

Reproducible dump-and-compare fuzz harness.

Git checkpoint for this work package.

Decide next work package after 1–4.

PART 6 — REPRODUCE
text
gcc -O0 -g -o tests/test_retrieval \
    tests/test_retrieval.c \
    src/retrieval/retrieval_scalar.c \
    -lm
./tests/test_retrieval

gcc -O2 -o tests/test_retrieval_asm \
    tests/test_retrieval_asm.c \
    src/retrieval/retrieval_scalar.c \
    src/kernels/arm64/lisa_asm_wrapper.c \
    src/kernels/arm64/lisa_ultra_mac.s \
    -lm
./tests/test_retrieval_asm

gcc -O2 -o benchmark/bench_compare \
    benchmark/bench_compare.c \
    src/retrieval/retrieval_scalar.c \
    src/kernels/arm64/lisa_asm_wrapper.c \
    src/kernels/arm64/lisa_ultra_mac.s \
    -lm
./benchmark/bench_compare 10000 768 5 100
End of combined report and update 001.

