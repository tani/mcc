#define _POSIX_C_SOURCE 200809L
#include "compiler.h"

#include <errno.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

bool run_command(Context *c, Strings args)
{
    char **argv = alloc(c, (args.len + 1) * sizeof *argv);
    for (size_t i = 0; i < args.len; i++)
        argv[i] = copy(c, args.data[i], strlen(args.data[i]));
    pid_t child;
    int status = posix_spawnp(&child, argv[0], NULL, NULL, argv, environ);
    if (status) {
        error(c, (Loc){0}, "cannot start %s: %s", args.data[0], strerror(status));
        return false;
    }
    int result;
    do {
        status = waitpid(child, &result, 0) < 0 ? errno : 0;
    } while (status == EINTR);
    if (status || !WIFEXITED(result) || WEXITSTATUS(result)) {
        error(c, (Loc){0}, "%s failed", args.data[0]);
        return false;
    }
    return true;
}
