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
    Strings inputs, link_args, scratch, after_includes;
    const char *output, *directory;
    bool stopped, strip_line_markers, stack_usage;
    unsigned serial;
} Driver;

static void usage(FILE *out)
{
    fputs("Usage: mcc [-std=DIALECT] [-E|-fsyntax-only|-S|-c] [-o FILE] INPUT...\n"
          "       -I DIR, -isystem DIR, -idirafter DIR, -D MACRO[=VALUE], -U MACRO\n"
          "       -P to omit line markers in -E output\n"
          "       -w, -Werror[=NAME], -Wno-error[=NAME], -Wsystem-headers\n"
          "       -fgnu89-inline, -fno-gnu89-inline, -fshort-enums, -fno-short-enums\n"
          "       -fpack-struct[=N], -fno-pack-struct, -fvisibility=MODE\n"
          "       -ffreestanding, -fhosted, -fno-builtin, -fno-builtin-NAME\n"
          "       -fplt, -fno-plt, -fpatchable-function-entry=N[,M], -fstack-usage\n"
          "       -static, -shared, -L DIR, -l LIBRARY, -pthread, -Wl,OPTIONS\n"
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

static void patchable_entry_option(Context *c, const char *arg)
{
    const char *value = arg + sizeof "-fpatchable-function-entry=" - 1;
    char *end;
    unsigned long count = strtoul(value, &end, 10), before = 0;
    bool valid = *value >= '0' && *value <= '9';
    if (*end == ',') {
        value = end + 1;
        valid &= *value >= '0' && *value <= '9';
        before = strtoul(value, &end, 10);
    }
    if (!valid || *end || count > MAX_PATCHABLE_ENTRY || before > count) {
        error(c, (Loc){0}, "invalid option '%s': expected N[,M] with 0 <= M <= N <= %u",
              arg, (unsigned)MAX_PATCHABLE_ENTRY);
        return;
    }
    c->patchable_entry = (PatchableEntry){.count = (unsigned)count, .before = (unsigned)before};
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

static void dialect_option(Context *c, const char *name)
{
    bool gnu = starts(name, "gnu");
    const char *version = gnu ? name + 3 : *name == 'c' ? name + 1 : "";
    static const struct { const char *name; CStandard standard; } versions[] = {
        {"89", C90}, {"90", C90}, {"9x", C99}, {"99", C99}, {"1x", C11},
        {"11", C11}, {"17", C17}, {"18", C17}, {"2x", C23}, {"23", C23}, {"2y", C2Y},
    };
    for (size_t i = 0; i < sizeof versions / sizeof *versions; i++) {
        if (equal(version, versions[i].name)) {
            c->dialect.standard = versions[i].standard;
            c->dialect.gnu = gnu;
            return;
        }
    }
    error(c, (Loc){0}, "unknown C dialect '%s'", name);
}

static void parse_options(Driver *d, int argc, char **argv)
{
    Context *c = &d->options;
    bool options = true, force_c99_inline = false;
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
        } else if (starts(arg, "-std=")) {
            dialect_option(c, arg + 5);
        } else if (equal(arg, "-ffreestanding") || equal(arg, "-fno-hosted")) {
            c->freestanding = c->no_builtin = true;
        } else if (equal(arg, "-fhosted") || equal(arg, "-fno-freestanding")) {
            c->freestanding = c->no_builtin = false;
        } else if (equal(arg, "-fdelete-null-pointer-checks") || equal(arg, "-fno-delete-null-pointer-checks")) {
            c->no_delete_null_pointer_checks = equal(arg, "-fno-delete-null-pointer-checks");
        } else if (equal(arg, "-fstack-usage") || equal(arg, "-fno-stack-usage")) {
            d->stack_usage = equal(arg, "-fstack-usage");
        } else if (equal(arg, "-fplt") || equal(arg, "-fno-plt")) {
            c->no_plt = equal(arg, "-fno-plt");
        } else if (starts(arg, "-fpatchable-function-entry=")) {
            patchable_entry_option(c, arg);
        } else if (equal(arg, "-fno-builtin") || equal(arg, "-fbuiltin")) {
            c->no_builtin = equal(arg, "-fno-builtin");
        } else if (starts(arg, "-fno-builtin-")) {
            const char *name = arg + sizeof "-fno-builtin-" - 1;
            if (!*name)
                error(c, (Loc){0}, "'-fno-builtin-' requires a function name");
            else if (starts(name, "__builtin_"))
                error(c, (Loc){0}, "cannot disable built-in function '%s'", name);
            else
                PUSH(c, c->disabled_builtins, name);
        } else if (equal(arg, "-fshort-enums") || equal(arg, "-fno-short-enums")) {
            c->dialect.short_enums = equal(arg, "-fshort-enums");
        } else if (equal(arg, "-fexcess-precision=16") || equal(arg, "-fexcess-precision=standard") ||
                   equal(arg, "-fexcess-precision=fast")) {
            c->dialect.round_float16_ops = equal(arg, "-fexcess-precision=16");
        } else if (equal(arg, "-fpack-struct") || equal(arg, "-fno-pack-struct")) {
            c->force_pack = equal(arg, "-fpack-struct");
            c->default_pack_alignment = c->force_pack;
        } else if (starts(arg, "-fpack-struct=")) {
            const char *setting = arg + strlen("-fpack-struct=");
            char *end;
            unsigned long long alignment = strtoull(setting, &end, 10);
            if (*setting < '0' || *setting > '9' || *end || !alignment ||
                !valid_pack_alignment(alignment)) {
                error(c, (Loc){0}, "invalid structure packing alignment '%s'", setting);
            } else {
                c->force_pack = false;
                c->default_pack_alignment = (unsigned char)alignment;
            }
        } else if (starts(arg, "-fvisibility=")) {
            const char *setting = arg + strlen("-fvisibility=");
            c->default_visibility = visibility_kind(setting);
            if (!c->default_visibility)
                error(c, (Loc){0}, "invalid symbol visibility '%s'", setting);
        } else if (equal(arg, "-fgnu89-inline") || equal(arg, "-fno-gnu89-inline")) {
            c->dialect.gnu_inline = equal(arg, "-fgnu89-inline");
            force_c99_inline = !c->dialect.gnu_inline;
        } else if (equal(arg, "-pedantic-errors")) {
            c->dialect.pedantic = PEDANTIC_ERROR;
        } else if (equal(arg, "-pedantic") || equal(arg, "-Wpedantic")) {
            if (c->dialect.pedantic != PEDANTIC_ERROR)
                c->dialect.pedantic = PEDANTIC_WARN;
        } else if (equal(arg, "-Wno-pedantic")) {
            c->dialect.pedantic = PEDANTIC_OFF;
        } else if (equal(arg, "-Wsystem-headers") || equal(arg, "-Wno-system-headers")) {
            c->warn_system_headers = equal(arg, "-Wsystem-headers");
        } else if (warning_argument(c, arg)) {
            continue;
        } else if (equal(arg, "-P")) {
            d->strip_line_markers = true;
        } else if (equal(arg, "-ftrampoline-impl=stack")) {
            error(c, (Loc){0}, "stack trampolines require an executable stack");
        } else if (equal(arg, "-E") || equal(arg, "-S") || equal(arg, "-c") ||
                   equal(arg, "-fsyntax-only")) {
            d->mode = equal(arg, "-E") ? PREPROCESS : equal(arg, "-S") ? ASSEMBLY :
                      equal(arg, "-c") ? ASSEMBLE : CHECK;
        } else if (starts(arg, "-o")) {
            d->output = operand(d, argc, argv, &i, 2);
        } else if (starts(arg, "-isystem")) {
            PUSH(c, c->system_includes, operand(d, argc, argv, &i, 8));
        } else if (starts(arg, "-idirafter")) {
            PUSH(c, d->after_includes, operand(d, argc, argv, &i, 10));
        } else if (starts(arg, "-I")) {
            PUSH(c, c->includes, operand(d, argc, argv, &i, 2));
        } else if (starts(arg, "-D") || starts(arg, "-U")) {
            const char *value = operand(d, argc, argv, &i, 2);
            PUSH(c, c->macro_options, format(c, "%c%s", arg[1], value));
        } else if (equal(arg, "-O0") || equal(arg, "-fPIC") || equal(arg, "-fpic") ||
                   equal(arg, "-ftrampoline-impl=heap")) {
            /* Code is unoptimized, position-independent, and uses heap trampolines. */
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
    if (c->dialect.standard == C90) {
        if (force_c99_inline)
            error(c, (Loc){0}, "-fno-gnu89-inline requires C99 or later");
        c->dialect.gnu_inline = true;
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

static bool compile_source(Driver *d, const char *path, FILE *out, FILE *stack_report)
{
    Context c = {
        .target = d->options.target, .dialect = d->options.dialect,
        .includes = d->options.includes, .system_includes = d->options.system_includes,
        .warn_system_headers = d->options.warn_system_headers,
        .warnings_as_errors = d->options.warnings_as_errors,
        .warn_all = d->options.warn_all,
        .warn_extra = d->options.warn_extra,
        .no_delete_null_pointer_checks = d->options.no_delete_null_pointer_checks,
        .no_plt = d->options.no_plt,
        .patchable_entry = d->options.patchable_entry,
        .fallthrough_level = d->options.fallthrough_level,
        .format_level = d->options.format_level,
        .alloca_warning_limit = d->options.alloca_warning_limit,
        .alloca_warning_limit_set = d->options.alloca_warning_limit_set,
        .vla_warning_limit = d->options.vla_warning_limit,
        .vla_warning_limit_set = d->options.vla_warning_limit_set,
        .frame_warning_limit = d->options.frame_warning_limit,
        .frame_warning_limit_set = d->options.frame_warning_limit_set,
        .stack_warning_limit = d->options.stack_warning_limit,
        .stack_warning_limit_set = d->options.stack_warning_limit_set,
        .syntax_only = d->mode == CHECK, .stack_usage_report = stack_report,
        .ignored_attributes = d->options.ignored_attributes,
        .suppress_warnings = d->options.suppress_warnings,
        .force_pack = d->options.force_pack, .default_pack_alignment = d->options.default_pack_alignment,
        .default_visibility = d->options.default_visibility,
        .freestanding = d->options.freestanding, .no_builtin = d->options.no_builtin,
        .disabled_builtins = d->options.disabled_builtins,
        .macro_options = d->options.macro_options, .input = equal(path, "-") ? "<stdin>" : path
    };
    memcpy(c.warnings, d->options.warnings, sizeof c.warnings);
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
            write_tokens(&c, out, tokens, !d->strip_line_markers);
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

static const char *stage_file(Driver *d, const char *destination, bool executable)
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
    if (fchmod(fd, (executable ? 0777 : 0666) & ~mask) < 0) {
        error(c, (Loc){0}, "%s: %s", name, strerror(errno));
        close(fd);
        unlink(name);
        return NULL;
    }
    close(fd);
    PUSH(c, d->scratch, name);
    return name;
}

static const char *stage(Driver *d, const char *destination)
{
    return stage_file(d, destination, d->mode == LINK);
}

/* GCC derives auxiliary files from the requested final output, not from the
   compiler driver's temporary assembly and object paths. */
static const char *stack_report_output(Driver *d, const char *input, const char *destination)
{
    Context *c = &d->options;
    if (!d->stack_usage)
        return NULL;
    if (d->mode == LINK) {
        const char *output = d->output ? d->output : "a.out";
        const char *name = strrchr(output, '/');
        name = name ? name + 1 : output;
        const char *source = basename_output(d, input, "su");
        int length = (int)strlen(output);
        if (equal(name, "a.out"))
            length -= 4;
        return format(c, "%.*s-%s", length, output, source);
    }
    if (!destination || equal(destination, "-"))
        return basename_output(d, input, "su");
    const char *name = strrchr(destination, '/');
    name = name ? name + 1 : destination;
    const char *dot = strrchr(name, '.');
    size_t length = dot && dot != name ? (size_t)(dot - destination) : strlen(destination);
    return format(c, "%.*s.su", (int)length, destination);
}

static bool publish(Driver *d, const char *path, const char *output)
{
    if (rename(path, output) == 0)
        return true;
    error(&d->options, (Loc){0}, "%s: %s", output, strerror(errno));
    return false;
}

static bool from_source(Driver *d, const char *path, const char *output, const char *report_destination)
{
    bool standard = output && equal(output, "-");
    FILE *file = standard ? stdout :
                 output ? fopen(output, d->mode == PREPROCESS ? "ab" : "wb") : tmpfile();
    if (!file) {
        error(&d->options, (Loc){0}, "cannot open compiler output: %s", strerror(errno));
        return false;
    }
    const char *report_stage = report_destination ? stage_file(d, report_destination, false) : NULL;
    FILE *report = report_stage ? fopen(report_stage, "wb") : NULL;
    if (report_destination && !report) {
        if (report_stage)
            error(&d->options, (Loc){0}, "cannot open stack usage output: %s", strerror(errno));
        if (!standard)
            fclose(file);
        return false;
    }
    bool ok = compile_source(d, path, file, report);
    if (report) {
        bool failed = ferror(report) != 0;
        if (fclose(report) == EOF)
            failed = true;
        if (failed) {
            error(&d->options, (Loc){0}, "cannot finish stack usage output: %s", strerror(errno));
            ok = false;
        }
    }
    if ((standard ? fflush(file) : fclose(file)) == EOF) {
        error(&d->options, (Loc){0}, "cannot finish compiler output: %s", strerror(errno));
        ok = false;
    }
    return ok && (!report_stage || publish(d, report_stage, report_destination));
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
    bool ok = mld_link(&c, args);
    release(&c);
    return ok && publish(d, output, dest);
}

static bool run_driver(Driver *d)
{
    Context *c = &d->options;
    Strings objects = {0}, destinations = {0}, reports = {0};
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
            const char *dest = NULL;
            if (d->mode == ASSEMBLE && !(dest = input_output(d, &destinations, input, "o")))
                return false;
            const char *report = source ? stack_report_output(d, input, dest) : NULL;
            if (report) {
                if (dest && equal(dest, report)) {
                    error(c, (Loc){0}, "stack usage output aliases compiler output '%s'", dest);
                    return false;
                }
                for (size_t j = 0; j < reports.len; j++)
                    if (equal(reports.data[j], report)) {
                        error(c, (Loc){0}, "multiple inputs map to stack usage output '%s'", report);
                        return false;
                    }
                PUSH(c, reports, report);
            }
            const char *assembly = source ? temporary(d, "s") : input;
            if (!assembly || (source && !from_source(d, input, assembly, report)))
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
            if (!from_source(d, input, NULL, NULL))
                return false;
        } else if (d->mode == PREPROCESS) {
            if (!from_source(d, input, combined ? combined : "-", NULL))
                return false;
        } else {
            const char *dest = input_output(d, &destinations, input, "s");
            if (!dest)
                return false;
            const char *report = stack_report_output(d, input, dest);
            if (report && equal(dest, report)) {
                error(c, (Loc){0}, "stack usage output aliases compiler output '%s'", dest);
                return false;
            }
            const char *output = equal(dest, "-") ? dest : stage(d, dest);
            if (!output || !from_source(d, input, output, report) ||
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
    Driver d = {.options = {.target = native_target(), .dialect = {.standard = C11}}};
    parse_options(&d, argc, argv);
    system_headers(&d.options);
    for (size_t i = 0; i < d.after_includes.len; i++)
        PUSH(&d.options, d.options.system_includes, d.after_includes.data[i]);
    bool ok = !d.options.failed && (d.stopped || run_driver(&d));
    for (size_t i = 0; i < d.scratch.len; i++)
        unlink(d.scratch.data[i]);
    if (d.directory)
        rmdir(d.directory);
    release(&d.options);
    return ok ? 0 : 1;
}
