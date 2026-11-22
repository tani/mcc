.text
.p2align 4
.global mcc_get_fpcr
.hidden mcc_get_fpcr
.type mcc_get_fpcr, @function
mcc_get_fpcr:
    .byte 0x0f, 0xae, 0x5c, 0x24, 0xf8 # stmxcsr -8(%rsp)
    movl -8(%rsp), %eax
    ret
.size mcc_get_fpcr, .-mcc_get_fpcr

.global mcc_raise_fp
.hidden mcc_raise_fp
.type mcc_raise_fp, @function
mcc_raise_fp:
    movl %edi, %eax
    andl $1, %eax
    andl $30, %edi
    shll $1, %edi
    orl %edi, %eax
    .byte 0x0f, 0xae, 0x5c, 0x24, 0xf8 # stmxcsr -8(%rsp)
    orl %eax, -8(%rsp)
    .byte 0x0f, 0xae, 0x54, 0x24, 0xf8 # ldmxcsr -8(%rsp)
    ret
.size mcc_raise_fp, .-mcc_raise_fp
