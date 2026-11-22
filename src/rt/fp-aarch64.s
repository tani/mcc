.text
.p2align 2
.global mcc_get_fpcr
.hidden mcc_get_fpcr
.type mcc_get_fpcr, %function
mcc_get_fpcr:
    .word 0xd53b4400 // mrs x0, fpcr
    ret
.size mcc_get_fpcr, .-mcc_get_fpcr

.global mcc_raise_fp
.hidden mcc_raise_fp
.type mcc_raise_fp, %function
mcc_raise_fp:
    .word 0xd53b4421 // mrs x1, fpsr
    orr w0, w0, w1
    .word 0xd51b4420 // msr fpsr, x0
    ret
.size mcc_raise_fp, .-mcc_raise_fp
