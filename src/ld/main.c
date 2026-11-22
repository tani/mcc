#include "ld.h"

int main(int argc, char **argv)
{
    Context c = {.program = "mld"};
    bool hosted = argc > 1 && equal(argv[1], "--hosted");
    Strings args = {0};
    for (int i = hosted ? 2 : 1; i < argc; i++)
        PUSH(&c, args, argv[i]);
    bool ok = mld_link(&c, args, hosted);
    release(&c);
    return ok ? 0 : 1;
}
