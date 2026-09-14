// LISA ULTRA — ARM64 NEON kernel (macOS)
// FIX: compare byte offset against dim * 4, not dim
.global _lisa_search_ultra
.align 4

_lisa_search_ultra:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

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

    fsub v8.4s, v4.4s, v5.4s
    fmla v0.4s, v8.4s, v8.4s

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
