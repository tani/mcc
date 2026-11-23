.text
.global __mcc_atomic_load_word
.hidden __mcc_atomic_load_word
.type __mcc_atomic_load_word, @function
__mcc_atomic_load_word:
    cmpl $1, %esi
    je .Lload1
    cmpl $2, %esi
    je .Lload2
    cmpl $4, %esi
    je .Lload4
    movq (%rdi), %rax
    ret
.Lload1:
    movzbl (%rdi), %eax
    ret
.Lload2:
    movzwl (%rdi), %eax
    ret
.Lload4:
    movl (%rdi), %eax
    ret
.size __mcc_atomic_load_word, .-__mcc_atomic_load_word

.global __mcc_atomic_store_word
.hidden __mcc_atomic_store_word
.type __mcc_atomic_store_word, @function
__mcc_atomic_store_word:
    cmpl $1, %edx
    je .Lstore1
    cmpl $2, %edx
    je .Lstore2
    cmpl $4, %edx
    je .Lstore4
    .byte 0x48, 0x87, 0x37 # xchgq %rsi, (%rdi)
    ret
.Lstore1:
    .byte 0x40, 0x86, 0x37 # xchgb %sil, (%rdi)
    ret
.Lstore2:
    .byte 0x66, 0x87, 0x37 # xchgw %si, (%rdi)
    ret
.Lstore4:
    .byte 0x87, 0x37 # xchgl %esi, (%rdi)
    ret
.size __mcc_atomic_store_word, .-__mcc_atomic_store_word

.global __mcc_atomic_cas_word
.hidden __mcc_atomic_cas_word
.type __mcc_atomic_cas_word, @function
__mcc_atomic_cas_word:
    movq %rsi, %rax
    cmpl $1, %ecx
    je .Lcas1
    cmpl $2, %ecx
    je .Lcas2
    cmpl $4, %ecx
    je .Lcas4
    .byte 0xf0, 0x48, 0x0f, 0xb1, 0x17 # lock cmpxchgq %rdx, (%rdi)
    ret
.Lcas1:
    .byte 0xf0, 0x0f, 0xb0, 0x17 # lock cmpxchgb %dl, (%rdi)
    movzbl %al, %eax
    ret
.Lcas2:
    .byte 0xf0, 0x66, 0x0f, 0xb1, 0x17 # lock cmpxchgw %dx, (%rdi)
    movzwl %ax, %eax
    ret
.Lcas4:
    .byte 0xf0, 0x0f, 0xb1, 0x17 # lock cmpxchgl %edx, (%rdi)
    movl %eax, %eax
    ret
.size __mcc_atomic_cas_word, .-__mcc_atomic_cas_word

.global __mcc_atomic_fence
.hidden __mcc_atomic_fence
.type __mcc_atomic_fence, @function
__mcc_atomic_fence:
    .byte 0x0f, 0xae, 0xf0 # mfence
    ret
.size __mcc_atomic_fence, .-__mcc_atomic_fence
