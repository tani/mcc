#define _GNU_SOURCE
#include "ld.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *find_libc(Context *c, const char *root, const char *triple, bool native)
{
    const char *configured = getenv("MCC_SYSTEM_LIB");
    if (configured && *configured)
        return configured;
    const char *dirs[] = {
        format(c, "%s/usr/lib/%s", root, triple),
        format(c, "%s/usr/%s/lib", root, triple),
        format(c, "%s/usr/lib64", root),
        format(c, "%s/usr/lib", root),
    };
    size_t count = native || *root ? sizeof dirs / sizeof *dirs : 2;
    for (size_t i = 0; i < count; i++)
        if (access(format(c, "%s/crti.o", dirs[i]), R_OK) == 0)
            return dirs[i];
    error(c, (Loc){0}, "cannot find glibc startup files for %s; set MCC_SYSTEM_LIB", triple);
    return NULL;
}

static const char *runtime_directory(Context *c, const char *root, const char *triple)
{
    const char *configured = getenv("MCC_RUNTIME_LIB");
    if (configured && *configured)
        return configured;
    const char *layouts[] = {"gcc", "gcc-cross"};
    for (size_t i = 0; i < sizeof layouts / sizeof *layouts; i++) {
        const char *base = format(c, "%s/usr/lib/%s/%s", root, layouts[i], triple);
        DIR *versions = opendir(base);
        if (!versions)
            continue;
        const char *best = NULL, *path = NULL;
        struct dirent *entry;
        while ((entry = readdir(versions))) {
            const char *name = entry->d_name;
            if (*name < '0' || *name > '9' || (best && strverscmp(name, best) <= 0))
                continue;
            const char *candidate = format(c, "%s/%s", base, name);
            if (access(format(c, "%s/libgcc.a", candidate), R_OK) == 0) {
                best = copy(c, name, strlen(name));
                path = candidate;
            }
        }
        closedir(versions);
        if (path)
            return path;
    }
    return NULL;
}

static const char *interpreter(Context *c, uint16_t machine, const char *libc, const char *root)
{
    const char *path = machine == EM_AARCH64 ? "/lib/ld-linux-aarch64.so.1" : "/lib64/ld-linux-x86-64.so.2";
    if (!*root && getenv("MCC_SYSTEM_LIB")) {
        const char *candidate = format(c, "%s%s", libc, strrchr(path, '/'));
        if (access(candidate, R_OK) == 0)
            return candidate;
    }
    return path;
}

static void add_runtime_file(Context *c, Strings *inputs, const char *directory, const char *name)
{
    if (directory) {
        const char *file = format(c, "%s/%s", directory, name);
        if (access(file, R_OK) == 0)
            PUSH(c, *inputs, file);
    }
}

static bool link_mode(const char *arg, const char *name)
{
    return arg[0] == '-' && equal(arg + (arg[1] == '-' ? 2 : 1), name);
}

static const char *next_value(Context *c, int argc, char **argv, int *i, const char *value)
{
    const char *arg = argv[*i];
    if (!*value && *i + 1 < argc)
        value = argv[++*i];
    if (!*value)
        error(c, (Loc){0}, "'%s' requires a value", arg);
    return value;
}

bool hosted_link(Context *c, int argc, char **argv)
{
    LinkOptions options = {.elf = {.output = "a.out", .entry = "_start", .machine = elf_machine(NULL)},
                           .hosted = true, .pie = true};
    Strings inputs = {0};
    bool static_link = false, flags = true;
    for (int i = 0; i < argc && !c->failed; i++) {
        const char *arg = argv[i];
        if (!flags && arg[0] == '-' && arg[1]) {
            PUSH(c, inputs, format(c, "./%s", arg));
        } else if (!flags || arg[0] != '-' || !arg[1]) {
            PUSH(c, inputs, arg);
        } else if (equal(arg, "--")) {
            flags = false;
        } else if (!strncmp(arg, "--target=", 9)) {
            options.elf.machine = elf_machine(arg + 9);
        } else if (link_mode(arg, "shared")) {
            options.shared = true;
        } else if (link_mode(arg, "static")) {
            static_link = true;
        } else if (link_mode(arg, "pie") || link_mode(arg, "no-pie")) {
            options.pie = link_mode(arg, "pie");
        } else if (equal(arg, "-E") || equal(arg, "--export-dynamic")) {
            options.export_all = true;
        } else if (!strncmp(arg, "--entry=", 8)) {
            options.elf.entry = next_value(c, argc, argv, &i, arg + 8);
        } else if (equal(arg, "-e") || equal(arg, "--entry")) {
            options.elf.entry = next_value(c, argc, argv, &i, "");
        } else if (arg[1] == 'L' || arg[1] == 'o' || arg[1] == 'l') {
            const char *value = next_value(c, argc, argv, &i, arg + 2);
            if (arg[1] == 'o') {
                options.elf.output = value;
            } else if (arg[1] == 'L') {
                PUSH(c, options.libraries, value);
            } else {
                PUSH(c, inputs, format(c, "-l%s", value));
            }
        } else if (link_mode(arg, "rpath") || !strncmp(arg, "--rpath=", 8) || !strncmp(arg, "-rpath=", 7)) {
            const char *assignment = strchr(arg, '=');
            const char *value = next_value(c, argc, argv, &i, assignment ? assignment + 1 : "");
            PUSH(c, options.runpaths, value);
        } else if (link_mode(arg, "soname") || equal(arg, "-h") || !strncmp(arg, "--soname=", 9)) {
            options.soname = next_value(c, argc, argv, &i, !strncmp(arg, "--soname=", 9) ? arg + 9 : "");
        } else if (equal(arg, "-pthread")) {
            PUSH(c, inputs, "-lpthread");
        } else if (equal(arg, "-z")) {
            const char *value = next_value(c, argc, argv, &i, "");
            if (!c->failed && !equal(value, "relro") && !equal(value, "now") &&
                !equal(value, "noexecstack") && !equal(value, "separate-code"))
                error(c, (Loc){0}, "unsupported hosted link option '-z %s'", value);
        } else if (equal(arg, "--eh-frame-hdr") || equal(arg, "--fatal-warnings")) {
            continue;
        } else if (equal(arg, "--start-group") || equal(arg, "--end-group") ||
                   equal(arg, "--as-needed") || equal(arg, "--no-as-needed") ||
                   equal(arg, "-Bstatic") || equal(arg, "-Bdynamic")) {
            PUSH(c, inputs, arg);
        } else {
            error(c, (Loc){0}, "unsupported hosted link option '%s'", arg);
        }
    }
    uint16_t machine = options.elf.machine;
    if (!c->failed && (machine == EM_NONE || !inputs.len))
        error(c, (Loc){0}, "hosted linking requires input files and a supported target");
    if (!c->failed && static_link && options.shared)
        error(c, (Loc){0}, "-static and -shared cannot be used together");
    if (c->failed)
        return false;
    const char *triple = machine == EM_AARCH64 ? "aarch64-linux-gnu" : "x86_64-linux-gnu";
    bool native = machine == elf_machine(NULL);
    const char *root = getenv("MCC_SYSROOT");
    if (!root)
        root = "";
    const char *libc = find_libc(c, root, triple, native);
    if (!libc)
        return false;
    const char *runtime = runtime_directory(c, root, triple);
    options.root = root;
    options.pie &= !static_link;
    if (!static_link && !options.shared)
        options.interpreter = interpreter(c, machine, libc, root);
    if (!static_link && !*root && getenv("MCC_SYSTEM_LIB"))
        PUSH(c, options.runpaths, libc);
    PUSH(c, options.libraries, libc);
    const char *static_dir = getenv("MCC_STATIC_LIB");
    if (static_dir && *static_dir)
        PUSH(c, options.libraries, static_dir);
    if (runtime)
        PUSH(c, options.libraries, runtime);

    Strings *files = &options.elf.inputs;
    if (static_link)
        PUSH(c, *files, "-Bstatic");
    if (!options.shared)
        PUSH(c, *files, format(c, "%s/%s", libc, options.pie ? "Scrt1.o" : "crt1.o"));
    PUSH(c, *files, format(c, "%s/crti.o", libc));
    bool pic = options.shared || options.pie;
    add_runtime_file(c, files, runtime, pic ? "crtbeginS.o" : static_link ? "crtbeginT.o" : "crtbegin.o");
    for (size_t i = 0; i < inputs.len; i++)
        PUSH(c, *files, inputs.data[i]);
    PUSH(c, *files, "--start-group");
    add_runtime_file(c, files, runtime, "libgcc.a");
    if (static_link) {
        PUSH(c, *files, "-Bstatic");
        add_runtime_file(c, files, runtime, "libgcc_eh.a");
    } else {
        PUSH(c, *files, "-Bdynamic");
    }
    PUSH(c, *files, "-lc");
    PUSH(c, *files, "--end-group");
    add_runtime_file(c, files, runtime, pic ? "crtendS.o" : "crtend.o");
    PUSH(c, *files, format(c, "%s/crtn.o", libc));
    return link_elf(c, &options);
}
