.text
.global mcc_test_stack_pointer
mcc_test_stack_pointer:
    leaq 8(%rsp), %rax
    ret
.global mcc_test_thread_pointer
mcc_test_thread_pointer:
    movq %fs:0, %rax
    ret
.weak mcc_test_longjmp
.global mcc_test_perturb_and_jump
mcc_test_perturb_and_jump:
    movq $501, %r12
    jmp mcc_test_longjmp
.global read_chain
read_chain:
    movl (%r10), %eax
    ret
.global raw_call
raw_call:
    addq $4, %rax
    addsd %xmm0, %xmm0
    ret
.weak receive
.global mcc_test_verify_receive
mcc_test_verify_receive:
    subq $24, %rsp
    movq %r12, (%rsp)
    movl $77, %edi
    call receive
    testl %eax, %eax
    jne .Lverify_fail
    cmpq (%rsp), %r12
    jne .Lverify_fail
    xorl %eax, %eax
    jmp .Lverify_done
.Lverify_fail:
    movl $1, %eax
.Lverify_done:
    addq $24, %rsp
    ret
