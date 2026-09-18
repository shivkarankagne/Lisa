// LISA ULTRA — ARM64 NEON kernel (macOS)
// FIX: compare byte offset against dim * 4, not dim
//
// ABI (AAPCS64):
//   x0 = query, x1 = vectors, w2 = n (int), w3 = dim (int), w4 = k (unused),
//   x5 = out_dists, x6 = out_indices.
//   The upper 32 bits of w2/w3 are undefined on entry, so they are
//   sign-extended before any 64-bit use.
//   Only caller-saved SIMD registers (v0-v7, v16-v31) are used.
//   v8-v15 (d8-d15) are callee-saved and must not be touched.
.global _lisa_search_ultra
.align 4

_lisa_search_ultra:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    sxtw x2, w2            // n:   int -> int64
    sxtw x3, w3            // dim: int -> int64

    // x3 = dim (floats)
    // Compute total bytes = dim * 4
    lsl x16, x3, #2        // x16 = dim * 4

    mov x9, #0             // vector index
.vector_loop:
    cmp x9, x2
    b.ge .done

    mul x13, x9, x3        // offset = index * dim
    lsl x13, x13, #2       // offset in bytes
    add x14, x1, x13       // current vector address

    movi v0.4s, #0         // accumulator
    mov x15, #0            // byte offset within vector

.dim_loop:
    cmp x15, x16           // compare byte offset to dim*4
    b.ge .dist_done

    ldr q4, [x0, x15]      // query[byte offset]
    ldr q5, [x14, x15]     // vector[byte offset]

    fsub v6.4s, v4.4s, v5.4s
    fmla v0.4s, v6.4s, v6.4s

    add x15, x15, #16      // advance 4 floats = 16 bytes
    b .dim_loop

.dist_done:
    faddp v1.4s, v0.4s, v0.4s
    faddp v1.4s, v1.4s, v1.4s

    str s1, [x5, x9, lsl #2]
    str w9, [x6, x9, lsl #2]

    add x9, x9, #1
    b .vector_loop

.done:
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
