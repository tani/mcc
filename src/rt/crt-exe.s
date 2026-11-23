.data
.p2align 3
.global __dso_handle
.hidden __dso_handle
.type __dso_handle, %object
__dso_handle:
    .quad 0
.size __dso_handle, 8

.bss
.balign 8
.global __mcc_atomic_object_lock
.type __mcc_atomic_object_lock, %gnu_unique_object
__mcc_atomic_object_lock:
    .zero 8
.size __mcc_atomic_object_lock, 8
