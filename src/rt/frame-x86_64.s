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
