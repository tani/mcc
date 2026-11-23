#include "compiler.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct Arena {
    Arena *next;
    size_t used, capacity;
    max_align_t storage[];
};

void *alloc(Context *c, size_t bytes)
{
    size_t size = align_to(bytes ? bytes : 1, _Alignof(max_align_t));
    Arena *block = c->arena;
    if (!block || size > block->capacity - block->used) {
        size_t capacity = size > 32768 ? size : 32768;
        if (capacity > SIZE_MAX - sizeof *block)
            abort();
        block = calloc(1, sizeof *block + capacity);
        if (!block) {
            fprintf(stderr, "%s: out of memory\n", c->program ? c->program : "mcc");
            exit(1);
        }
        block->capacity = capacity;
        block->next = c->arena;
        c->arena = block;
    }
    void *result = (char *)block->storage + block->used;
    block->used += size;
    return result;
}

void *grow(Context *c, const void *old, size_t *capacity, size_t width)
{
    size_t count = *capacity;
    if (count > SIZE_MAX / 2 / width)
        abort();
    *capacity = count ? count * 2 : 16;
    void *result = alloc(c, *capacity * width);
    if (old)
        memcpy(result, old, count * width);
    return result;
}

char *copy(Context *c, const char *text, size_t size)
{
    char *result = alloc(c, size + 1);
    memcpy(result, text, size);
    return result;
}

char *format(Context *c, const char *fmt, ...)
{
    va_list args, length_args;
    va_start(args, fmt);
    va_copy(length_args, args);
    int length = vsnprintf(NULL, 0, fmt, length_args);
    va_end(length_args);
    if (length < 0)
        abort();
    char *result = alloc(c, (size_t)length + 1);
    vsnprintf(result, (size_t)length + 1, fmt, args);
    va_end(args);
    return result;
}

void release(Context *c)
{
    while (c->arena) {
        Arena *block = c->arena;
        c->arena = block->next;
        free(block);
    }
}

void error(Context *c, Loc loc, const char *fmt, ...)
{
    c->failed = true;
    if (++c->errors > 20)
        return;
    if (loc.file)
        fprintf(stderr, "%s:%u:%u: error: ", loc.file, loc.line, loc.column);
    else
        fprintf(stderr, "%s: error: ", c->program ? c->program : "mcc");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static const char *const warning_options[WARNING_COUNT] = {
    [WARNING_PEDANTIC] = "pedantic",
    [WARNING_CPP] = "cpp",
    [WARNING_ATTRIBUTES] = "attributes",
    [WARNING_PRAGMAS] = "pragmas",
    [WARNING_UNKNOWN_PRAGMAS] = "unknown-pragmas",
    [WARNING_INVALID_MEMORY_MODEL] = "invalid-memory-model",
    [WARNING_STRINGOP_OVERFLOW] = "stringop-overflow",
    [WARNING_STRINGOP_OVERREAD] = "stringop-overread",
    [WARNING_PRIORITY_CTOR_DTOR] = "prio-ctor-dtor",
    [WARNING_DEPRECATED_DECLARATIONS] = "deprecated-declarations",
    [WARNING_DISCARDED_QUALIFIERS] = "discarded-qualifiers",
    [WARNING_DESIGNATED_INIT] = "designated-init",
    [WARNING_FRAME_ADDRESS] = "frame-address",
    [WARNING_UNUSED_RESULT] = "unused-result",
    [WARNING_IMPLICIT_FALLTHROUGH] = "implicit-fallthrough",
    [WARNING_ATTRIBUTE_WARNING] = "attribute-warning",
    [WARNING_FORMAT] = "format",
    [WARNING_NONNULL] = "nonnull",
    [WARNING_NONNULL_COMPARE] = "nonnull-compare",
    [WARNING_UNINITIALIZED] = "uninitialized",
    [WARNING_MAYBE_UNINITIALIZED] = "maybe-uninitialized",
};

WarningKind warning_kind(const char *name)
{
    for (size_t i = 1; i < WARNING_COUNT; i++)
        if (equal(name, warning_options[i]))
            return (WarningKind)i;
    return WARNING_NONE;
}

struct IgnoredAttribute {
    const IgnoredAttribute *previous;
    const char *name;
};

static bool attribute_identifier(const char *text, size_t len)
{
    if (!len || !(isalpha((unsigned char)text[0]) || text[0] == '_'))
        return false;
    for (size_t i = 1; i < len; i++)
        if (!isalnum((unsigned char)text[i]) && text[i] != '_')
            return false;
    return true;
}

bool ignore_attribute(Context *c, const char *name)
{
    const char *separator = strstr(name, "::");
    if (!separator || !attribute_identifier(name, (size_t)(separator - name)) ||
        (separator[2] && !attribute_identifier(separator + 2, strlen(separator + 2))))
        return false;
    IgnoredAttribute *entry = alloc(c, sizeof *entry);
    *entry = (IgnoredAttribute){c->ignored_attributes, name};
    c->ignored_attributes = entry;
    return true;
}

bool attribute_ignored(Loc loc, const char *namespace, const char *name)
{
    if (!namespace)
        return false;
    size_t length = strlen(namespace);
    for (const IgnoredAttribute *entry = loc.ignored_attributes; entry; entry = entry->previous) {
        const char *ignored = entry->name;
        if (!strncmp(ignored, namespace, length) &&
            ignored[length] == ':' && (!ignored[length + 2] || equal(ignored + length + 2, name)))
            return true;
    }
    return false;
}

bool warning_argument(Context *c, const char *argument)
{
    if (equal(argument, "-Wall") || equal(argument, "-Wno-all")) {
        c->warn_all = equal(argument, "-Wall");
        return true;
    }
    if (equal(argument, "-Wextra") || equal(argument, "-W") || equal(argument, "-Wno-extra")) {
        c->warn_extra = !equal(argument, "-Wno-extra");
        return true;
    }
    static const char format_prefix[] = "-Wformat=";
    if (!strncmp(argument, format_prefix, sizeof format_prefix - 1)) {
        const char *level = argument + sizeof format_prefix - 1;
        char *end;
        unsigned long value = strtoul(level, &end, 0);
        if (level[0] < '0' || level[0] > '9' || *end || value > 2)
            error(c, (Loc){0}, "format warning level must be between 0 and 2");
        else
            c->warnings[WARNING_FORMAT].enabled = value ? DIAG_WARNING : DIAG_IGNORED;
        return true;
    }
    static const char fallthrough_prefix[] = "-Wimplicit-fallthrough=";
    if (!strncmp(argument, fallthrough_prefix, sizeof fallthrough_prefix - 1)) {
        const char *level = argument + sizeof fallthrough_prefix - 1;
        char *end;
        unsigned long value = strtoul(level, &end, 0);
        if (level[0] < '0' || level[0] > '9' || *end || value > 5)
            error(c, (Loc){0}, "implicit-fallthrough level must be between 0 and 5");
        else {
            c->fallthrough_level = (unsigned)value;
            c->warnings[WARNING_IMPLICIT_FALLTHROUGH].enabled = c->fallthrough_level ? DIAG_WARNING : DIAG_IGNORED;
        }
        return true;
    }
    if (equal(argument, "-w")) {
        c->suppress_warnings = true;
        return true;
    }
    if (equal(argument, "-Werror") || equal(argument, "-Wno-error")) {
        c->warnings_as_errors = equal(argument, "-Werror");
        return true;
    }
    static const char attribute_prefix[] = "-Wno-attributes=";
    if (!strncmp(argument, attribute_prefix, sizeof attribute_prefix - 1))
        return ignore_attribute(c, argument + sizeof attribute_prefix - 1);
    if (strncmp(argument, "-W", 2))
        return false;
    const char *name = argument + 2;
    bool disable = !strncmp(name, "no-", 3);
    if (disable)
        name += 3;
    bool severity = !strncmp(name, "error=", 6);
    if (severity)
        name += 6;
    WarningKind kind = warning_kind(name);
    if (kind == WARNING_NONE)
        return false;
    WarningOption *option = &c->warnings[kind];
    if (kind == WARNING_IMPLICIT_FALLTHROUGH) {
        if (!severity)
            c->fallthrough_level = disable ? 0u : 3u;
        else if (!disable && !c->fallthrough_level)
            c->fallthrough_level = 3;
    }
    if (severity) {
        option->severity = disable ? DIAG_WARNING : DIAG_ERROR;
        if (!disable)
            option->enabled = DIAG_WARNING;
    } else {
        option->enabled = disable ? DIAG_IGNORED : DIAG_WARNING;
    }
    return true;
}

static DiagnosticLevel warning_level(Context *c, WarningKind kind, Loc loc)
{
    if (c->suppress_warnings || suppress_system_diagnostic(c, loc))
        return DIAG_IGNORED;
    for (const DiagnosticState *state = loc.diagnostics; state; state = state->previous)
        if (state->warning == kind)
            return state->level;
    WarningOption option = c->warnings[kind];
    if (kind == WARNING_NONNULL) {
        WarningOption format = c->warnings[WARNING_FORMAT];
        if (!option.enabled)
            option.enabled = format.enabled;
        if (!option.severity && format.severity == DIAG_ERROR)
            option.severity = DIAG_ERROR;
    }
    if (kind == WARNING_MAYBE_UNINITIALIZED && !option.enabled) {
        WarningOption definite = c->warnings[WARNING_UNINITIALIZED];
        option.enabled = definite.enabled;
        if (!option.severity)
            option.severity = definite.severity;
    }
    bool pedantic_error = kind == WARNING_PEDANTIC && c->dialect.pedantic == PEDANTIC_ERROR;
    if (option.enabled == DIAG_IGNORED)
        return DIAG_IGNORED;
    if (!option.enabled && ((kind == WARNING_PEDANTIC && c->dialect.pedantic == PEDANTIC_OFF) ||
                           kind == WARNING_UNKNOWN_PRAGMAS ||
                           ((kind == WARNING_FRAME_ADDRESS || kind == WARNING_FORMAT || kind == WARNING_NONNULL ||
                             kind == WARNING_NONNULL_COMPARE) && !c->warn_all) ||
                           ((kind == WARNING_UNINITIALIZED || kind == WARNING_MAYBE_UNINITIALIZED) &&
                            !c->warn_all && !c->warn_extra) ||
                           (kind == WARNING_IMPLICIT_FALLTHROUGH && !c->warn_extra)))
        return DIAG_IGNORED;
    if (option.severity)
        return option.severity;
    return c->warnings_as_errors || pedantic_error ? DIAG_ERROR : DIAG_WARNING;
}

bool warning_enabled(Context *c, WarningKind kind, Loc loc)
{
    return warning_level(c, kind, loc) != DIAG_IGNORED;
}

static void emit_warning(Context *c, WarningKind kind, Loc loc, const char *message, va_list args)
{
    DiagnosticLevel level = warning_level(c, kind, loc);
    if (level == DIAG_IGNORED)
        return;
    if (level == DIAG_ERROR) {
        c->failed = true;
        if (++c->errors > 20)
            return;
    }
    const char *label = level == DIAG_ERROR ? "error" : "warning";
    if (loc.file)
        fprintf(stderr, "%s:%u:%u: %s: ", loc.file, loc.line, loc.column, label);
    else
        fprintf(stderr, "%s: %s: ", c->program ? c->program : "mcc", label);
    vfprintf(stderr, message, args);
    if (kind != WARNING_NONE)
        fprintf(stderr, " [-W%s%s]", level == DIAG_ERROR ? "error=" : "", warning_options[kind]);
    fputc('\n', stderr);
}

void warning_at(Context *c, WarningKind kind, Loc loc, const char *message, ...)
{
    va_list args;
    va_start(args, message);
    emit_warning(c, kind, loc, message, args);
    va_end(args);
}

void warning(Context *c, Loc loc, const char *message, ...)
{
    va_list args;
    va_start(args, message);
    emit_warning(c, WARNING_NONE, loc, message, args);
    va_end(args);
}

void note(Context *c, Loc loc, const char *message)
{
    if (loc.file)
        fprintf(stderr, "%s:%u:%u: note: %s\n", loc.file, loc.line, loc.column, message);
    else
        fprintf(stderr, "%s: note: %s\n", c->program ? c->program : "mcc", message);
}

void extension(Context *c, Loc loc, const char *feature)
{
    if (c->extension_depth || suppress_system_diagnostic(c, loc))
        return;
    if (!c->dialect.gnu)
        error(c, loc, "%s requires GNU mode", feature);
    else
        warning_at(c, WARNING_PEDANTIC, loc, "%s is a GNU extension", feature);
}

size_t align_to(size_t number, size_t alignment)
{
    return (number + alignment - 1) / alignment * alignment;
}

bool equal(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

bool aggregate(Type *t)
{
    return t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_UNION;
}
