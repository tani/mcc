.data
.p2align 3
.global __dso_handle
.hidden __dso_handle
.type __dso_handle, %object
__dso_handle:
    .quad __dso_handle
.size __dso_handle, 8

.text
.p2align 2
.Lmcc_finalize:
    adrp x0, __dso_handle
    add x0, x0, :lo12:__dso_handle
    b __cxa_finalize

.section .fini_array,"aw",%fini_array
.p2align 3
    .quad .Lmcc_finalize
