.data
.p2align 3
.global __dso_handle
.hidden __dso_handle
.type __dso_handle, @object
__dso_handle:
    .quad __dso_handle
.size __dso_handle, 8

.text
.Lmcc_finalize:
    leaq __dso_handle(%rip), %rdi
    jmp __cxa_finalize@PLT

.section .fini_array,"aw",@fini_array
.p2align 3
    .quad .Lmcc_finalize
