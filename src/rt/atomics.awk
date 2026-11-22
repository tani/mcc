# AArch64 out-of-line atomic ABI. Use the base ISA's exclusive accesses
# so the same code works on CPUs with and without LSE.

function word(bits) { printf "\t.word 0x%08x\n", bits }

function function_start(name) {
    print ".p2align 2\n.global " name "\n.hidden " name "\n.type " name ", %function\n" name ":"
}

function function_end(name) { print ".size " name ", .-" name }

function fetch(op, size, order, acquire, release, barrier,    name, width, reg, instruction) {
    name = "__aarch64_" op size "_" order
    width = size == 8 ? "x" : "w"
    reg = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3
    instruction = ops[op]
    function_start(name)
    if (barrier) word(0xd5033bbf) # dmb ish
    print ".L" name "_retry:"
    # [ldxr|ldaxr] {w,x}2, [x1]
    word(0x085f7c22 + reg * 0x40000000 + acquire * 0x8000)
    if (op == "swp")
        printf "\tmov %s3, %s0\n", width, width
    else if (op == "ldclr")
        printf "\tmvn %s3, %s0\n\tand %s3, %s2, %s3\n", width, width, width, width, width
    else
        printf "\t%s %s3, %s2, %s0\n", instruction, width, width, width
    # [stxr|stlxr] w4, {w,x}3, [x1]
    word(0x08047c23 + reg * 0x40000000 + release * 0x8000)
    print "\tcbnz w4, .L" name "_retry"
    if (barrier) word(0xd5033bbf)
    printf "\tmov %s0, %s2\n\tret\n", width, width
    function_end(name)
}

function cas(size, order, acquire, release, barrier,    name, width, reg) {
    name = "__aarch64_cas" size "_" order
    function_start(name)
    if (barrier) word(0xd5033bbf)
    if (size == 1) print "\tand w0, w0, #255"
    if (size == 2) print "\tand w0, w0, #65535"
    print ".L" name "_retry:"
    if (size == 16) {
        word(0xc87f1885 + acquire * 0x8000) # [ldxp|ldaxp] x5, x6, [x4]
        print "\tcmp x5, x0\n\tb.ne .L" name "_unchanged"
        print "\tcmp x6, x1\n\tb.ne .L" name "_unchanged"
        word(0xc8270c82 + release * 0x8000) # [stxp|stlxp] w7, x2, x3, [x4]
        print "\tb .L" name "_test\n.L" name "_unchanged:"
        # A successful store confirms that both loaded halves came from one value.
        word(0xc8271885 + release * 0x8000) # [stxp|stlxp] w7, x5, x6, [x4]
        print ".L" name "_test:\n\tcbnz w7, .L" name "_retry\n\tmov x0, x5\n\tmov x1, x6"
    } else {
        width = size == 8 ? "x" : "w"
        reg = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3
        word(0x085f7c43 + reg * 0x40000000 + acquire * 0x8000) # load {w,x}3 from [x2]
        printf "\tcmp %s3, %s0\n\tb.ne .L%s_clear\n", width, width, name
        word(0x08047c41 + reg * 0x40000000 + release * 0x8000) # store {w,x}1 to [x2], status w4
        print "\tcbnz w4, .L" name "_retry\n\tb .L" name "_done\n.L" name "_clear:"
        word(0xd5033f5f) # clrex
        printf ".L%s_done:\n\tmov %s0, %s3\n", name, width, width
    }
    if (barrier) word(0xd5033bbf)
    print "\tret"
    function_end(name)
}

BEGIN {
    print ".text"
    split("ldadd ldclr ldeor ldset swp", names)
    ops["ldadd"] = "add"
    ops["ldeor"] = "eor"
    ops["ldset"] = "orr"
    split("relax acq rel acq_rel sync", orders)
    for (i = 1; i <= 5; i++) {
        acquire = i == 2 || i >= 4
        release = i >= 3
        for (size = 1; size <= 16; size *= 2) {
            cas(size, orders[i], acquire, release, i == 5)
            for (j = 1; size <= 8 && j <= 5; j++)
                fetch(names[j], size, orders[i], acquire, release, i == 5)
        }
    }
}
