# Assemble read-only copies of the runtime files into mcc and mld.
BEGIN {
    print ".section .rodata\n.p2align 3"
    print ".global " name "\n.hidden " name "\n" name ":"
}

{
    printf "\t.byte %s", $1
    for (i = 2; i <= NF; i++) printf ",%s", $i
    printf "\n"
    size += NF
}

END {
    print ".p2align 3\n.global " name "_size\n.hidden " name "_size\n" name "_size:"
    print "\t.quad " size
}
