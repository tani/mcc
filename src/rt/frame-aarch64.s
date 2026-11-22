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
