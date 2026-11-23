.text
.global mcc_capture_frame
.hidden mcc_capture_frame
.type mcc_capture_frame, %function
mcc_capture_frame:
    stp x19, x20, [x0, #152]
    stp x21, x22, [x0, #168]
    stp x23, x24, [x0, #184]
    stp x25, x26, [x0, #200]
    stp x27, x28, [x0, #216]
    ldp x9, x10, [x29]
    add x11, x29, #16
    stp x9, x10, [x0, #232]
    stp x11, x10, [x0, #248]
    str d8, [x0, #576]
    str d9, [x0, #584]
    str d10, [x0, #592]
    str d11, [x0, #600]
    str d12, [x0, #608]
    str d13, [x0, #616]
    str d14, [x0, #624]
    str d15, [x0, #632]
    ret
.size mcc_capture_frame, .-mcc_capture_frame

.global mcc_install_frame
.hidden mcc_install_frame
.type mcc_install_frame, %function
mcc_install_frame:
    ldp x19, x20, [x0, #152]
    ldp x21, x22, [x0, #168]
    ldp x23, x24, [x0, #184]
    ldp x25, x26, [x0, #200]
    ldp x27, x28, [x0, #216]
    ldp x29, x30, [x0, #232]
    ldr d8, [x0, #576]
    ldr d9, [x0, #584]
    ldr d10, [x0, #592]
    ldr d11, [x0, #600]
    ldr d12, [x0, #608]
    ldr d13, [x0, #616]
    ldr d14, [x0, #624]
    ldr d15, [x0, #632]
    ldp x10, x16, [x0, #248]
    ldp x0, x1, [x0]
    mov sp, x10
    br x16
.size mcc_install_frame, .-mcc_install_frame

.global mcc_strip_pac
.hidden mcc_strip_pac
.type mcc_strip_pac, %function
mcc_strip_pac:
    mov x9, x30
    mov x30, x0
    .word 0xd50320ff // xpaclri
    mov x0, x30
    mov x30, x9
    ret
.size mcc_strip_pac, .-mcc_strip_pac
.text
.p2align 2
.global __clear_cache
.hidden __clear_cache
.type __clear_cache, %function
__clear_cache:
    cmp x0, x1
    b.hs .Lclear_cache_done
    mrs x3, ctr_el0
    // The fields encode the log2 of the number of words per line.
    lsr x5, x3, #16
    and x5, x5, #15
    mov x4, #4
    lsl x4, x4, x5
    sub x5, x4, #1
    mvn x5, x5
    and x2, x0, x5
.Lclean_data_cache:
    .word 0xd50b7b22 // dc cvau, x2
    add x2, x2, x4
    cmp x2, x1
    b.lo .Lclean_data_cache
    .word 0xd5033b9f // dsb ish
    and x5, x3, #15
    mov x4, #4
    lsl x4, x4, x5
    sub x5, x4, #1
    mvn x5, x5
    and x0, x0, x5
.Linvalidate_instruction_cache:
    .word 0xd50b7520 // ic ivau, x0
    add x0, x0, x4
    cmp x0, x1
    b.lo .Linvalidate_instruction_cache
    .word 0xd5033b9f // dsb ish
    .word 0xd5033fdf // isb
.Lclear_cache_done:
    ret
.size __clear_cache, .-__clear_cache

// x19–x30, the stack pointer, then d8–d15.
.global __mcc_save_nonlocal
.hidden __mcc_save_nonlocal
.type __mcc_save_nonlocal, %function
__mcc_save_nonlocal:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    str d8, [x0, #104]
    str d9, [x0, #112]
    str d10, [x0, #120]
    str d11, [x0, #128]
    str d12, [x0, #136]
    str d13, [x0, #144]
    str d14, [x0, #152]
    str d15, [x0, #160]
    mov w0, #0
    ret
.size __mcc_save_nonlocal, .-__mcc_save_nonlocal

.global __mcc_restore_nonlocal
.hidden __mcc_restore_nonlocal
.type __mcc_restore_nonlocal, %function
__mcc_restore_nonlocal:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9, [x0, #96]
    ldr d8, [x0, #104]
    ldr d9, [x0, #112]
    ldr d10, [x0, #120]
    ldr d11, [x0, #128]
    ldr d12, [x0, #136]
    ldr d13, [x0, #144]
    ldr d14, [x0, #152]
    ldr d15, [x0, #160]
    mov sp, x9
    mov w0, #1
    ret
.size __mcc_restore_nonlocal, .-__mcc_restore_nonlocal

.global __mcc_lock_trampoline_list
.hidden __mcc_lock_trampoline_list
.type __mcc_lock_trampoline_list, %function
__mcc_lock_trampoline_list:
    mov x1, #1
.Lmcc_trampoline_lock_retry:
    .word 0xc85ffc02 // ldaxr x2, [x0]
    cmp x2, x1
    b.eq .Lmcc_trampoline_lock_retry
    .word 0xc8037c01 // stxr w3, x1, [x0]
    cbnz w3, .Lmcc_trampoline_lock_retry
    mov x0, x2
    ret
.size __mcc_lock_trampoline_list, .-__mcc_lock_trampoline_list

.global __mcc_unlock_trampoline_list
.hidden __mcc_unlock_trampoline_list
.type __mcc_unlock_trampoline_list, %function
__mcc_unlock_trampoline_list:
    .word 0xc89ffc01 // stlr x1, [x0]
    ret
.size __mcc_unlock_trampoline_list, .-__mcc_unlock_trampoline_list
