#define _POSIX_C_SOURCE 200809L
#include "as/as.h"
#include "ld/ld.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { LINK, ASSEMBLE, ASSEMBLY, PREPROCESS, CHECK } Mode;

typedef struct {
    Context options;
    Mode mode;
    Strings inputs, link_args, scratch;
    const char *output, *directory;
    bool stopped, freestanding;
    unsigned serial;
} Driver;

static void usage(FILE *out)
{
    fputs("Usage: mcc [-std=c11|-std=gnu11] [-E|-fsyntax-only|-S|-c] [-o FILE] INPUT...\n"
          "       -I DIR, -isystem DIR, -D MACRO[=VALUE], -U MACRO\n"
          "       -static, -shared, -nostdlib, -L DIR, -l LIBRARY, -pthread, -Wl,OPTIONS\n"
          "       --target=aarch64-linux-gnu or --target=x86_64-linux-gnu\n"
          "       Includes mas and mld. Hosted links use the target's glibc.\n", out);
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
        } else if (equal(arg, "-nostdlib")) {
            d->freestanding = true;
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
    const char *root = getenv("MCC_SYSROOT");
    if (!root)
        root = "";
    if (c->target == native_target())
        PUSH(c, c->system_includes, format(c, "%s/usr/local/include", root));
    if (c->target != native_target() || *root)
        PUSH(c, c->system_includes, format(c, "%s/usr/%s-linux-gnu/include", root, c->target->arch));
    if (c->target == native_target() || *root) {
        PUSH(c, c->system_includes, format(c, "%s/usr/include/%s-linux-gnu", root, c->target->arch));
        PUSH(c, c->system_includes, format(c, "%s/usr/include", root));
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
    Context c = {.program = "mas"};
    ElfOptions options = {.output = obj, .machine = elf_machine(format(&c, "%s-linux-gnu", d->options.target->arch))};
    PUSH(&c, options.inputs, input);
    bool ok = mas_assemble(&c, &options);
    release(&c);
    return ok;
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

static void link_argument(Context *c, Strings *args, const char *arg)
{
    if (!starts(arg, "-Wl,")) {
        PUSH(c, *args, arg);
        return;
    }
    for (const char *part = arg + 4;;) {
        const char *comma = strchr(part, ',');
        if (!comma) {
            PUSH(c, *args, part);
            break;
        }
        PUSH(c, *args, copy(c, part, (size_t)(comma - part)));
        part = comma + 1;
    }
}

static bool link_objects(Driver *d, Strings objects)
{
    const char *dest = d->output ? d->output : "a.out";
    const char *output = stage(d, dest);
    if (!output)
        return false;
    Context c = {.program = "mld"};
    Strings args = {0};
    PUSH(&c, args, format(&c, "--target=%s-linux-gnu", d->options.target->arch));
    PUSH(&c, args, "-o");
    PUSH(&c, args, output);
    size_t next = 0;
    for (size_t i = 0; i < d->link_args.len; i++)
        link_argument(&c, &args, d->link_args.data[i] ? d->link_args.data[i] : objects.data[next++]);
    bool ok = mld_link(&c, args, !d->freestanding);
    release(&c);
    return ok && publish(d, output, dest);
}

static bool run_driver(Driver *d)
{
    Context *c = &d->options;
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
    Driver d = {.options = {.target = native_target()}};
    parse_options(&d, argc, argv);
    system_headers(&d.options);
    bool ok = !d.options.failed && (d.stopped || run_driver(&d));
    for (size_t i = 0; i < d.scratch.len; i++)
        unlink(d.scratch.data[i]);
    if (d.directory)
        rmdir(d.directory);
    release(&d.options);
    return ok ? 0 : 1;
}
