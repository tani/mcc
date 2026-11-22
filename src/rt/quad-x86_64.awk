# Bridge the ELF x86-64 binary128 ABI (one XMM register per operand) to
# the integer representation used by the C implementation.
function wrapper(name, wide_args, wide_result) {
    print ".p2align 4\n.global __" name "\n.hidden __" name "\n.type __" name ", @function\n__" name ":"
    print "\tsubq $40, %rsp"
    if (wide_args >= 1) print "\tmovaps %xmm0, 0(%rsp)\n\tmovq 0(%rsp), %rdi\n\tmovq 8(%rsp), %rsi"
    if (wide_args == 2) print "\tmovaps %xmm1, 16(%rsp)\n\tmovq 16(%rsp), %rdx\n\tmovq 24(%rsp), %rcx"
    print "\tcall mcc_" name
    if (wide_result) print "\tmovq %rax, 0(%rsp)\n\tmovq %rdx, 8(%rsp)\n\tmovaps 0(%rsp), %xmm0"
    print "\taddq $40, %rsp\n\tret\n.size __" name ", .-__" name
}

BEGIN {
    print ".text"
    count = split("addtf3 subtf3 multf3 divtf3", names)
    for (i = 1; i <= count; i++) wrapper(names[i], 2, 1)
    wrapper("negtf2", 1, 1)
    count = split("eqtf2 netf2 letf2 lttf2 getf2 gttf2 unordtf2", names)
    for (i = 1; i <= count; i++) wrapper(names[i], 2, 0)
    count = split("trunctfsf2 trunctfdf2 fixtfdi fixunstfdi fixtfsi fixunstfsi", names)
    for (i = 1; i <= count; i++) wrapper(names[i], 1, 0)
    count = split("extendsftf2 extenddftf2 floatditf floatunditf floatsitf floatunsitf", names)
    for (i = 1; i <= count; i++) wrapper(names[i], 0, 1)
}
