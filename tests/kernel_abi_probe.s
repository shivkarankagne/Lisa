// SPDX-License-Identifier: BUSL-1.1
//
// kernel_abi_probe — calls _lisa_search_ultra under hostile ABI conditions.
//
//   int abi_probe(const float* query, const float* vectors, int n, int dim,
//                 int k, float* out_dists, int* out_indices);
//
// Before the call:
//   - d8-d15 (callee-saved) are loaded with sentinel values 1.0 .. 8.0
//   - the upper 32 bits of x2 (n) and x3 (dim) are filled with garbage,
//     which AAPCS64 permits for 32-bit int arguments
//
// Returns 0 if every sentinel survived the call, 1 otherwise.
// The caller checks that the kernel's outputs are correct.

.global _abi_probe
.align 4

_abi_probe:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp d8, d9,   [sp, #16]
    stp d10, d11, [sp, #32]
    stp d12, d13, [sp, #48]
    stp d14, d15, [sp, #64]

    fmov d8,  #1.0
    fmov d9,  #2.0
    fmov d10, #3.0
    fmov d11, #4.0
    fmov d12, #5.0
    fmov d13, #6.0
    fmov d14, #7.0
    fmov d15, #8.0

    movk x2, #0xDEAD, lsl #48
    movk x2, #0xBEEF, lsl #32
    movk x3, #0xCAFE, lsl #48
    movk x3, #0xF00D, lsl #32

    bl _lisa_search_ultra

    mov w0, #0
    fmov d0, #1.0
    fcmp d8, d0
    b.ne .fail
    fmov d0, #2.0
    fcmp d9, d0
    b.ne .fail
    fmov d0, #3.0
    fcmp d10, d0
    b.ne .fail
    fmov d0, #4.0
    fcmp d11, d0
    b.ne .fail
    fmov d0, #5.0
    fcmp d12, d0
    b.ne .fail
    fmov d0, #6.0
    fcmp d13, d0
    b.ne .fail
    fmov d0, #7.0
    fcmp d14, d0
    b.ne .fail
    fmov d0, #8.0
    fcmp d15, d0
    b.ne .fail
    b .out

.fail:
    mov w0, #1

.out:
    ldp d8, d9,   [sp, #16]
    ldp d10, d11, [sp, #32]
    ldp d12, d13, [sp, #48]
    ldp d14, d15, [sp, #64]
    ldp x29, x30, [sp], #80
    ret
