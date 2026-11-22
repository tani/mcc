#include "as.h"

int main(int argc, char **argv)
{
    Context c = {.program = "mas"};
    Strings args = {0};
    for (int i = 1; i < argc; i++)
        PUSH(&c, args, argv[i]);
    ElfOptions options = elf_options(&c, args, false);
    bool ok = !c.failed && (options.stopped || mas_assemble(&c, &options));
    release(&c);
    return ok ? 0 : 1;
}
