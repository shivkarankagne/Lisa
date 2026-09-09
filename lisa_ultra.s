// LISA ULTRA — ARM64 NEON kernel (Linux ARM64)
.global lisa_search_ultra
.align 4

lisa_search_ultra:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    mov x9, #0
.vector_loop:
    cmp x9, x2
    b.ge .done

    mul x13, x9, x3
    lsl x13, x13, #2
    add x14, x1, x13

    movi v0.4s, #0
    mov x15, #0

.dim_loop:
    cmp x15, x3
    b.ge .dist_done

    ldr q4, [x0, x15]
    ldr q5, [x14, x15]

    fsub v8.4s, v4.4s, v5.4s
    fmla v0.4s, v8.4s, v8.4s

    add x15, x15, #16
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
