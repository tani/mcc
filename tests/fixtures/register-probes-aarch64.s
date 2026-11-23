.text
.global mcc_test_stack_pointer
mcc_test_stack_pointer:
    mov x0, sp
    ret
.global mcc_test_thread_pointer
mcc_test_thread_pointer:
    mrs x0, tpidr_el0
    ret
.weak mcc_test_longjmp
.global mcc_test_perturb_and_jump
mcc_test_perturb_and_jump:
    mov x19, #501
    sub sp, sp, #16
    str x19, [sp]
    ldr d8, [sp]
    add sp, sp, #16
    b mcc_test_longjmp
.global read_chain
read_chain:
    ldr w0, [x18]
    ret
.global raw_call
raw_call:
    add x0, x0, #4
    fadd d0, d0, d0
    ret
.weak receive
.global mcc_test_verify_receive
mcc_test_verify_receive:
    sub sp, sp, #32
    str x30, [sp, #24]
    str x19, [sp]
    str d8, [sp, #8]
    mov x0, #77
    bl receive
    cmp x0, #0
    b.ne .Lverify_fail
    ldr x9, [sp]
    cmp x19, x9
    b.ne .Lverify_fail
    str d8, [sp, #16]
    ldr x9, [sp, #8]
    ldr x10, [sp, #16]
    cmp x9, x10
    b.ne .Lverify_fail
    mov x0, #0
    b .Lverify_done
.Lverify_fail:
    mov x0, #1
.Lverify_done:
    ldr x30, [sp, #24]
    add sp, sp, #32
    ret
