.text
.global __mcc_atomic_load_word
.hidden __mcc_atomic_load_word
.type __mcc_atomic_load_word, %function
__mcc_atomic_load_word:
    cmp w1, #1
    b.eq .Lload1
    cmp w1, #2
    b.eq .Lload2
    cmp w1, #4
    b.eq .Lload4
    .word 0xc8dffc00 // ldar x0, [x0]
    ret
.Lload1:
    .word 0x08dffc00 // ldarb w0, [x0]
    ret
.Lload2:
    .word 0x48dffc00 // ldarh w0, [x0]
    ret
.Lload4:
    .word 0x88dffc00 // ldar w0, [x0]
    ret
.size __mcc_atomic_load_word, .-__mcc_atomic_load_word

.global __mcc_atomic_store_word
.hidden __mcc_atomic_store_word
.type __mcc_atomic_store_word, %function
__mcc_atomic_store_word:
    cmp w2, #1
    b.eq .Lstore1
    cmp w2, #2
    b.eq .Lstore2
    cmp w2, #4
    b.eq .Lstore4
    .word 0xc89ffc01 // stlr x1, [x0]
    ret
.Lstore1:
    .word 0x089ffc01 // stlrb w1, [x0]
    ret
.Lstore2:
    .word 0x489ffc01 // stlrh w1, [x0]
    ret
.Lstore4:
    .word 0x889ffc01 // stlr w1, [x0]
    ret
.size __mcc_atomic_store_word, .-__mcc_atomic_store_word

.global __mcc_atomic_fence
.hidden __mcc_atomic_fence
.type __mcc_atomic_fence, %function
__mcc_atomic_fence:
    .word 0xd5033bbf // dmb ish
    ret
.size __mcc_atomic_fence, .-__mcc_atomic_fence
