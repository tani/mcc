#include "ld.h"

int main(int argc, char **argv)
{
    if (argc > 1 && equal(argv[1], "--help")) {
        puts("Usage: mld [--target=TARGET] [-o FILE] [OPTIONS] INPUT...\n"
             "Targets: aarch64-linux-gnu, x86_64-linux-gnu\n"
             "Links with the target's glibc. Executables are PIE by default.\n"
             "Options: -no-pie, -static, -shared, -L DIR, -l LIBRARY, -pthread,\n"
             "         -e SYMBOL, -rpath DIR, -soname NAME");
        return 0;
    }
    if (argc > 1 && equal(argv[1], "--version")) {
        puts("mld 0.1");
        return 0;
    }
    Context c = {.program = "mld"};
    Strings args = {0};
    for (int i = 1; i < argc; i++)
        PUSH(&c, args, argv[i]);
    bool ok = mld_link(&c, args);
    release(&c);
    return ok ? 0 : 1;
}
