.text
.global mcc_capture_frame
.hidden mcc_capture_frame
.type mcc_capture_frame, @function
mcc_capture_frame:
    movq %rbx, 24(%rdi)
    movq 0(%rbp), %rax
    movq %rax, 48(%rdi)
    leaq 16(%rbp), %rax
    movq %rax, 56(%rdi)
    movq %r12, 96(%rdi)
    movq %r13, 104(%rdi)
    movq %r14, 112(%rdi)
    movq %r15, 120(%rdi)
    movq 8(%rbp), %rax
    movq %rax, 128(%rdi)
    ret
.size mcc_capture_frame, .-mcc_capture_frame

.global mcc_install_frame
.hidden mcc_install_frame
.type mcc_install_frame, @function
mcc_install_frame:
    movq 0(%rdi), %rax
    movq 8(%rdi), %rdx
    movq 24(%rdi), %rbx
    movq 48(%rdi), %rbp
    movq 96(%rdi), %r12
    movq 104(%rdi), %r13
    movq 112(%rdi), %r14
    movq 120(%rdi), %r15
    movq 56(%rdi), %rsp
    jmp *128(%rdi)
.size mcc_install_frame, .-mcc_install_frame
.text
.global __mcc_save_nonlocal
.hidden __mcc_save_nonlocal
.type __mcc_save_nonlocal, @function
__mcc_save_nonlocal:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq 0(%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
.size __mcc_save_nonlocal, .-__mcc_save_nonlocal

.global __mcc_restore_nonlocal
.hidden __mcc_restore_nonlocal
.type __mcc_restore_nonlocal, @function
__mcc_restore_nonlocal:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rdx
    movl $1, %eax
    jmp *%rdx
.size __mcc_restore_nonlocal, .-__mcc_restore_nonlocal

.global __mcc_lock_trampoline_list
.hidden __mcc_lock_trampoline_list
.type __mcc_lock_trampoline_list, @function
__mcc_lock_trampoline_list:
.Lmcc_trampoline_lock_retry:
    movl $1, %eax
    .byte 0x48, 0x87, 0x07 # xchgq %rax, (%rdi)
    cmpq $1, %rax
    je .Lmcc_trampoline_lock_retry
    ret
.size __mcc_lock_trampoline_list, .-__mcc_lock_trampoline_list

.global __mcc_unlock_trampoline_list
.hidden __mcc_unlock_trampoline_list
.type __mcc_unlock_trampoline_list, @function
__mcc_unlock_trampoline_list:
    movq %rsi, 0(%rdi)
    ret
.size __mcc_unlock_trampoline_list, .-__mcc_unlock_trampoline_list
