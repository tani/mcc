#define _POSIX_C_SOURCE 200809L
#include "compiler.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef enum { LINK, ASSEMBLE, ASSEMBLY, PREPROCESS, CHECK } Mode;

typedef struct {
    Context options;
    Mode mode;
    Strings inputs, link_args, scratch;
    const char *output, *directory, *toolchain;
    bool stopped;
    unsigned serial;
} Driver;

static void usage(FILE *out)
{
    fputs("Usage: mcc [-std=c11|-std=gnu11] [-E|-fsyntax-only|-S|-c] [-o FILE] INPUT...\n"
          "       -I DIR, -isystem DIR, -D MACRO[=VALUE], -U MACRO\n"
          "       -static, -shared, -L DIR, -l LIBRARY, -pthread, -Wl,OPTIONS\n"
          "       --target=aarch64-linux-gnu or --target=x86_64-linux-gnu\n"
          "       Uses native Linux GCC and glibc by default.\n", out);
}

static const Target *native_target(void)
{
#if defined(__x86_64__)
    return &x86_64_linux;
#else
    return &aarch64_linux;
#endif
}

static bool starts(const char *argument, const char *prefix)
{
    return strncmp(argument, prefix, strlen(prefix)) == 0;
}

static bool suffix(const char *path, const char *ending)
{
    size_t len = strlen(path), tail = strlen(ending);
    return len >= tail && equal(path + len - tail, ending);
}

static const char *operand(Driver *d, int argc, char **argv, int *i, size_t prefix)
{
    const char *arg = argv[*i];
    if (arg[prefix])
        return arg + prefix;
    if (*i + 1 < argc)
        return argv[++*i];
    error(&d->options, (Loc){0}, "'%s' requires an argument", arg);
    return "";
}

static void parse_options(Driver *d, int argc, char **argv)
{
    Context *c = &d->options;
    bool options = true;
    for (int i = 1; i < argc && !c->failed; i++) {
        const char *arg = argv[i];
        if (options && equal(arg, "--")) {
            options = false;
        } else if (!options || arg[0] != '-' || equal(arg, "-")) {
            PUSH(c, d->inputs, arg);
            PUSH(c, d->link_args, NULL);
        } else if (equal(arg, "--help")) {
            usage(stdout);
            d->stopped = true;
            return;
        } else if (equal(arg, "--version")) {
            printf("mcc 0.1 (%s Linux)\n", c->target->display_name);
            d->stopped = true;
            return;
        } else if (starts(arg, "--target=")) {
            const char *triple = arg + 9;
            if (equal(triple, "aarch64-linux-gnu"))
                c->target = &aarch64_linux;
            else if (equal(triple, "x86_64-linux-gnu"))
                c->target = &x86_64_linux;
            else
                error(c, (Loc){0}, "unsupported target '%s'", triple);
        } else if (equal(arg, "-std=c11") || equal(arg, "-std=gnu11")) {
            c->gnu = equal(arg, "-std=gnu11");
        } else if (equal(arg, "-pedantic-errors")) {
            c->pedantic = true;
        } else if (equal(arg, "-E") || equal(arg, "-S") || equal(arg, "-c") ||
                   equal(arg, "-fsyntax-only")) {
            d->mode = equal(arg, "-E") ? PREPROCESS : equal(arg, "-S") ? ASSEMBLY :
                      equal(arg, "-c") ? ASSEMBLE : CHECK;
        } else if (starts(arg, "-o")) {
            d->output = operand(d, argc, argv, &i, 2);
        } else if (starts(arg, "-isystem")) {
            PUSH(c, c->system_includes, operand(d, argc, argv, &i, 8));
        } else if (starts(arg, "-I")) {
            PUSH(c, c->includes, operand(d, argc, argv, &i, 2));
        } else if (starts(arg, "-D") || starts(arg, "-U")) {
            const char *value = operand(d, argc, argv, &i, 2);
            PUSH(c, c->macro_options, format(c, "%c%s", arg[1], value));
        } else if (equal(arg, "-O0") || equal(arg, "-fPIC") || equal(arg, "-fpic")) {
            /* Code is unoptimized and position-independent. */
        } else if (equal(arg, "-static") || equal(arg, "-shared") ||
                   equal(arg, "-pthread") || equal(arg, "-pie") || equal(arg, "-no-pie") ||
                   starts(arg, "-Wl,")) {
            PUSH(c, d->link_args, arg);
        } else if (starts(arg, "-L") || starts(arg, "-l")) {
            const char *value = operand(d, argc, argv, &i, 2);
            PUSH(c, d->link_args, format(c, "-%c%s", arg[1], value));
        } else {
            error(c, (Loc){0}, "unknown option '%s'", arg);
        }
    }
    if (!c->failed && !d->inputs.len)
        error(c, (Loc){0}, "no input files");
    if (d->output && d->inputs.len > 1 && d->mode != LINK && d->mode != PREPROCESS)
        error(c, (Loc){0}, "cannot use -o with multiple inputs in this mode");
    if (d->output && equal(d->output, "-") && d->mode != PREPROCESS && d->mode != ASSEMBLY)
        error(c, (Loc){0}, "stdout output requires -E or -S");
}

static int invoke(Driver *d, Strings args, int stdout_fd)
{
    Context *c = &d->options;
    char **argv = alloc(c, (args.len + 1) * sizeof *argv);
    for (size_t i = 0; i < args.len; i++)
        argv[i] = copy(c, args.data[i], strlen(args.data[i]));
    posix_spawn_file_actions_t actions;
    int status = posix_spawn_file_actions_init(&actions);
    bool initialized = status == 0;
    if (!status && stdout_fd >= 0)
        status = posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);
    pid_t child;
    if (!status)
        status = posix_spawnp(&child, argv[0], &actions, NULL, argv, environ);
    if (initialized)
        posix_spawn_file_actions_destroy(&actions);
    if (status) {
        error(c, (Loc){0}, "cannot start %s: %s", args.data[0], strerror(status));
        return 1;
    }
    int result;
    do {
        status = waitpid(child, &result, 0) < 0 ? errno : 0;
    } while (status == EINTR);
    if (status || !WIFEXITED(result) || WEXITSTATUS(result)) {
        error(c, (Loc){0}, "%s failed", args.data[0]);
        return 1;
    }
    return 0;
}

static const char *temporary(Driver *d, const char *ext)
{
    Context *c = &d->options;
    if (!d->directory) {
        const char *base = getenv("TMPDIR");
        char *dir = format(c, "%s/mcc.XXXXXX", base && *base ? base : "/tmp");
        if (!mkdtemp(dir)) {
            error(c, (Loc){0}, "cannot create temporary directory: %s", strerror(errno));
            return NULL;
        }
        d->directory = dir;
    }
    const char *path = format(c, "%s/%u.%s", d->directory, ++d->serial, ext);
    PUSH(c, d->scratch, path);
    return path;
}

static bool validate_toolchain(Driver *d)
{
    Context *c = &d->options;
    FILE *output = tmpfile();
    if (!output) {
        error(c, (Loc){0}, "cannot open temporary file for toolchain verification");
        return false;
    }
    Strings cmd = {0};
    PUSH(c, cmd, d->toolchain);
    PUSH(c, cmd, "-dumpmachine");
    bool ok = invoke(d, cmd, fileno(output)) == 0;
    if (ok) {
        char result[160];
        rewind(output);
        const char *arch = format(c, "%s-", c->target->arch);
        if (!fgets(result, sizeof result, output) || !starts(result, arch) ||
            !suffix(result, "-linux-gnu\n")) {
            error(c, (Loc){0}, "GCC must target %s Linux with glibc", c->target->arch);
            ok = false;
        }
    }
    fclose(output);
    return ok;
}

static void system_headers(Context *c)
{
    const char *compiler_headers = getenv("MCC_COMPILER_INCLUDE");
    PUSH(c, c->system_includes,
         compiler_headers && *compiler_headers ? compiler_headers : MCC_BUILTIN_INCLUDE);
    const char *libc_headers = getenv("MCC_SYSTEM_INCLUDE");
    if (libc_headers && *libc_headers) {
        PUSH(c, c->system_includes, libc_headers);
        return;
    }
    if (c->target == native_target()) {
        PUSH(c, c->system_includes, "/usr/local/include");
        PUSH(c, c->system_includes, format(c, "/usr/include/%s-linux-gnu", c->target->arch));
        PUSH(c, c->system_includes, "/usr/include");
    } else {
        PUSH(c, c->system_includes, format(c, "/usr/%s-linux-gnu/include", c->target->arch));
    }
}

static bool compile_source(Driver *d, const char *path, FILE *out)
{
    Context c = {
        .target = d->options.target, .gnu = d->options.gnu, .pedantic = d->options.pedantic,
        .includes = d->options.includes, .system_includes = d->options.system_includes,
        .macro_options = d->options.macro_options, .input = equal(path, "-") ? "<stdin>" : path
    };
    FILE *input = equal(path, "-") ? stdin : fopen(path, "rb");
    if (!input) {
        error(&d->options, (Loc){0}, "%s: %s", path, strerror(errno));
        return false;
    }
    Tokens tokens = {0};
    bool ok = preprocess(&c, c.input, input, &tokens);
    if (input != stdin)
        fclose(input);
    if (ok) {
        if (d->mode == PREPROCESS)
            write_tokens(out, tokens);
        else
            ok = parse(&c, tokens) && c.target->emit(&c, out);
    }
    if (ferror(out)) {
        error(&d->options, (Loc){0}, "writing compiler output failed");
        ok = false;
    }
    release(&c);
    return ok;
}

static const char *basename_output(Driver *d, const char *path, const char *ext)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *dot = strrchr(name, '.');
    int len = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    if (equal(name, "-"))
        return format(&d->options, "stdin.%s", ext);
    return format(&d->options, "%.*s.%s", len, name, ext);
}

static const char *stage(Driver *d, const char *destination)
{
    Context *c = &d->options;
    struct stat output;
    if (!stat(destination, &output)) {
        if (!S_ISREG(output.st_mode)) {
            error(c, (Loc){0}, "%s: output is not a regular file", destination);
            return NULL;
        }
        for (size_t i = 0; i < d->inputs.len; i++) {
            struct stat input;
            if (!stat(d->inputs.data[i], &input) && input.st_dev == output.st_dev &&
                input.st_ino == output.st_ino) {
                error(c, (Loc){0}, "output '%s' aliases an input", destination);
                return NULL;
            }
        }
    }
    char *name = format(c, "%s.mcc.XXXXXX", destination);
    int fd = mkstemp(name);
    if (fd < 0) {
        error(c, (Loc){0}, "%s: %s", destination, strerror(errno));
        return NULL;
    }
    mode_t mask = umask(0);
    umask(mask);
    if (fchmod(fd, (d->mode == LINK ? 0777 : 0666) & ~mask) < 0) {
        error(c, (Loc){0}, "%s: %s", name, strerror(errno));
        close(fd);
        unlink(name);
        return NULL;
    }
    close(fd);
    PUSH(c, d->scratch, name);
    return name;
}

static bool publish(Driver *d, const char *path, const char *output)
{
    if (rename(path, output) == 0)
        return true;
    error(&d->options, (Loc){0}, "%s: %s", output, strerror(errno));
    return false;
}

static bool from_source(Driver *d, const char *path, const char *output)
{
    bool standard = output && equal(output, "-");
    FILE *file = standard ? stdout :
                 output ? fopen(output, d->mode == PREPROCESS ? "ab" : "wb") : tmpfile();
    if (!file) {
        error(&d->options, (Loc){0}, "cannot open compiler output: %s", strerror(errno));
        return false;
    }
    bool ok = compile_source(d, path, file);
    if ((standard ? fflush(file) : fclose(file)) == EOF) {
        error(&d->options, (Loc){0}, "cannot finish compiler output: %s", strerror(errno));
        ok = false;
    }
    return ok;
}

static bool assemble(Driver *d, const char *input, const char *obj)
{
    Context *c = &d->options;
    Strings cmd = {0};
    PUSH(c, cmd, d->toolchain);
    PUSH(c, cmd, "-x");
    PUSH(c, cmd, "assembler");
    PUSH(c, cmd, "-c");
    PUSH(c, cmd, "-o");
    PUSH(c, cmd, obj);
    PUSH(c, cmd, input[0] == '-' ? format(c, "./%s", input) : input);
    return invoke(d, cmd, -1) == 0;
}

static const char *input_output(Driver *d, Strings *destinations, const char *input, const char *ext)
{
    const char *dest = d->output ? d->output : basename_output(d, input, ext);
    for (size_t j = 0; j < destinations->len; j++)
        if (equal(destinations->data[j], dest)) {
            error(&d->options, (Loc){0}, "multiple inputs map to '%s'", dest);
            return NULL;
        }
    PUSH(&d->options, *destinations, dest);
    return dest;
}

static bool link_objects(Driver *d, Strings objects)
{
    Context *c = &d->options;
    const char *dest = d->output ? d->output : "a.out";
    const char *output = stage(d, dest);
    if (!output)
        return false;
    Strings cmd = {0};
    PUSH(c, cmd, d->toolchain);
    PUSH(c, cmd, "-o");
    PUSH(c, cmd, output[0] == '-' ? format(c, "./%s", output) : output);
    size_t next = 0;
    for (size_t i = 0; i < d->link_args.len; i++)
        PUSH(c, cmd, d->link_args.data[i] ? d->link_args.data[i] : objects.data[next++]);
    return invoke(d, cmd, -1) == 0 && publish(d, output, dest);
}

static bool run_driver(Driver *d)
{
    Context *c = &d->options;
    if ((d->mode == LINK || d->mode == ASSEMBLE) && !validate_toolchain(d))
        return false;
    Strings objects = {0}, destinations = {0};
    const char *combined = NULL;
    if (d->mode == PREPROCESS && d->output && !equal(d->output, "-")) {
        combined = stage(d, d->output);
        if (!combined)
            return false;
    }
    for (size_t i = 0; i < d->inputs.len; i++) {
        const char *input = d->inputs.data[i];
        bool source = suffix(input, ".c") || suffix(input, ".i") || equal(input, "-");
        if (!source && !suffix(input, ".s")) {
            if (d->mode == LINK && (suffix(input, ".o") || suffix(input, ".a") || suffix(input, ".so"))) {
                PUSH(c, objects, input[0] == '-' ? format(c, "./%s", input) : input);
                continue;
            }
            error(c, (Loc){0}, "unsupported input type: %s", input);
            return false;
        }
        if (d->mode == LINK || d->mode == ASSEMBLE) {
            const char *assembly = source ? temporary(d, "s") : input;
            if (!assembly || (source && !from_source(d, input, assembly)))
                return false;
            const char *dest = NULL;
            if (d->mode == ASSEMBLE && !(dest = input_output(d, &destinations, input, "o")))
                return false;
            const char *object = dest ? stage(d, dest) : temporary(d, "o");
            if (!object || !assemble(d, assembly, object))
                return false;
            if (dest) {
                if (!publish(d, object, dest))
                    return false;
            } else {
                PUSH(c, objects, object);
            }
            continue;
        }
        if (!source) {
            error(c, (Loc){0}, "%s requires C source", input);
            return false;
        }
        if (d->mode == CHECK) {
            if (!from_source(d, input, NULL))
                return false;
        } else if (d->mode == PREPROCESS) {
            if (!from_source(d, input, combined ? combined : "-"))
                return false;
        } else {
            const char *dest = input_output(d, &destinations, input, "s");
            if (!dest)
                return false;
            const char *output = equal(dest, "-") ? dest : stage(d, dest);
            if (!output || !from_source(d, input, output) ||
                (!equal(dest, "-") && !publish(d, output, dest)))
                return false;
        }
    }
    if (combined && !publish(d, combined, d->output))
        return false;
    return d->mode != LINK || link_objects(d, objects);
}

int main(int argc, char **argv)
{
    Driver d = {.options = {.target = native_target()}, .toolchain = getenv("MCC_TOOLCHAIN")};
    parse_options(&d, argc, argv);
    if (!d.toolchain || !*d.toolchain)
        d.toolchain = d.options.target == native_target() ? "gcc" :
                      format(&d.options, "%s-linux-gnu-gcc", d.options.target->arch);
    system_headers(&d.options);
    bool ok = !d.options.failed && (d.stopped || run_driver(&d));
    for (size_t i = 0; i < d.scratch.len; i++)
        unlink(d.scratch.data[i]);
    if (d.directory)
        rmdir(d.directory);
    release(&d.options);
    return ok ? 0 : 1;
}
