#include "as.h"

#include <string.h>

static bool run(Context *c, int argc, char **argv)
{
    ElfOptions options = {.output = "a.o", .machine = elf_machine(NULL)};
    bool flags = true;
    for (int i = 1; i < argc && !c->failed; i++) {
        const char *arg = argv[i];
        if (!flags || arg[0] != '-' || equal(arg, "-")) {
            PUSH(c, options.inputs, arg);
        } else if (equal(arg, "--")) {
            flags = false;
        } else if (equal(arg, "--help")) {
            puts("Usage: mas [--target=TARGET] [-o FILE] INPUT...\n"
                 "Targets: aarch64-linux-gnu, x86_64-linux-gnu");
            return true;
        } else if (equal(arg, "--version")) {
            puts("mas 0.1");
            return true;
        } else if (!strncmp(arg, "--target=", 9)) {
            options.machine = elf_machine(arg + 9);
            if (options.machine == EM_NONE)
                error(c, (Loc){0}, "unsupported target '%s'", arg + 9);
        } else if (arg[1] == 'o') {
            const char *value = arg[2] ? arg + 2 : i + 1 < argc ? argv[++i] : "";
            if (!*value)
                error(c, (Loc){0}, "'%s' requires a value", arg);
            else
                options.output = value;
        } else {
            error(c, (Loc){0}, "unsupported option '%s'", arg);
        }
    }
    if (!c->failed && !options.inputs.len)
        error(c, (Loc){0}, "no input files");
    if (!c->failed && options.machine == EM_NONE)
        error(c, (Loc){0}, "select an ELF target with --target");
    return !c->failed && mas_assemble(c, &options);
}

int main(int argc, char **argv)
{
    Context c = {.program = "mas"};
    bool ok = run(&c, argc, argv);
    release(&c);
    return ok ? 0 : 1;
}
