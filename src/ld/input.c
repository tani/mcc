#include "ld.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct { const char *name; size_t offset; } ArchiveSymbol;
typedef struct { size_t offset; bool selected; } ArchiveMember;
typedef struct Input Input;
typedef VECTOR(Input *) Inputs;

struct Input {
    const char *path, *link_name;
    Bytes file, names;
    VECTOR(ArchiveSymbol) symbols;
    VECTOR(ArchiveMember) members;
    Inputs children;
    enum { INPUT_OBJECT, INPUT_SHARED, INPUT_ARCHIVE, INPUT_SEQUENCE, INPUT_GROUP } kind;
    bool selected, as_needed;
};

typedef struct {
    Context *context;
    Linker *linker;
    LinkOptions *options;
    Strings files;
    size_t selected;
    bool static_link, as_needed;
} Reader;

static bool read_stream(Context *c, FILE *in, const char *path, Bytes *file)
{
    unsigned char buffer[8192];
    size_t count;
    while (!c->failed && (count = fread(buffer, 1, sizeof buffer, in)) != 0)
        bytes_add(c, file, buffer, count);
    if (ferror(in))
        error(c, (Loc){0}, "%s: cannot read linker input: %s", path, strerror(errno));
    return !c->failed;
}

static bool read_file(Context *c, const char *path, Bytes *file)
{
    if (equal(path, "-"))
        return read_stream(c, stdin, path, file);
    FILE *in = fopen(path, "rb");
    if (!in) {
        error(c, (Loc){0}, "%s: %s", path, strerror(errno));
        return false;
    }
    bool ok = read_stream(c, in, path, file);
    fclose(in);
    return ok;
}

static uint64_t big_endian(const unsigned char *p, size_t width)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; i++)
        value = value << 8 | p[i];
    return value;
}

static bool member_header(Bytes file, size_t offset, size_t *size)
{
    if (offset > file.len || file.len - offset < 60)
        return false;
    const unsigned char *p = file.data + offset;
    if (memcmp(p + 58, "`\n", 2))
        return false;
    uint64_t length = 0;
    size_t i = 48;
    for (; i < 58 && p[i] >= '0' && p[i] <= '9'; i++)
        length = length * 10 + (unsigned)(p[i] - '0');
    if (i == 48 || length > file.len - offset - 60)
        return false;
    for (; i < 58; i++)
        if (p[i] != ' ')
            return false;
    *size = (size_t)length;
    return true;
}

static bool archive_index(Context *c, Input *input, const unsigned char *data, size_t size, size_t width)
{
    if (size < width)
        return false;
    uint64_t count = big_endian(data, width);
    if (count > (size - width) / width)
        return false;
    const unsigned char *name = data + width + count * width;
    const unsigned char *end = data + size;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *next = memchr(name, 0, (size_t)(end - name));
        uint64_t offset = big_endian(data + width + i * width, width);
        if (!next || offset < 8 || offset >= input->file.len)
            return false;
        PUSH(c, input->symbols, ((ArchiveSymbol){(const char *)name, (size_t)offset}));
        name = next + 1;
    }
    return true;
}

static void read_archive(Context *c, Input *input)
{
    bool indexed = false;
    for (size_t offset = 8; offset < input->file.len;) {
        size_t size;
        if (!member_header(input->file, offset, &size)) {
            error(c, (Loc){0}, "%s: malformed archive member", input->path);
            return;
        }
        unsigned char *header = input->file.data + offset;
        unsigned char *data = header + 60;
        if (!memcmp(header, "/ ", 2) || !memcmp(header, "/SYM64/ ", 8)) {
            size_t width = header[1] == ' ' ? 4 : 8;
            if (indexed || !archive_index(c, input, data, size, width)) {
                error(c, (Loc){0}, "%s: malformed archive symbol index", input->path);
                return;
            }
            indexed = true;
        } else if (!memcmp(header, "//", 2)) {
            input->names = (Bytes){.data = data, .len = size};
        } else {
            PUSH(c, input->members, ((ArchiveMember){.offset = offset}));
        }
        offset += 60 + size + (size & 1);
    }
    if (!indexed && input->members.len)
        error(c, (Loc){0}, "%s: archive has no GNU symbol index", input->path);
}

static const char *member_name(Context *c, Input *input, const unsigned char *header)
{
    const unsigned char *name = header;
    size_t length = 16;
    if (header[0] == '/') {
        size_t offset = 0;
        for (size_t i = 1; i < 16 && header[i] >= '0' && header[i] <= '9'; i++)
            offset = offset * 10 + (size_t)(header[i] - '0');
        if (offset >= input->names.len)
            return NULL;
        name = input->names.data + offset;
        length = input->names.len - offset;
    }
    const unsigned char *end = memchr(name, '/', length);
    if (end)
        length = (size_t)(end - name);
    return copy(c, (const char *)name, length);
}

static void select_archive(Reader *r, Input *input)
{
    Context *c = r->context;
    size_t previous;
    do {
        previous = r->selected;
        for (size_t i = 0; i < input->symbols.len && !c->failed; i++) {
            ArchiveSymbol symbol = input->symbols.data[i];
            if (!ld_needs(r->linker, symbol.name))
                continue;
            size_t lo = 0, hi = input->members.len;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (input->members.data[mid].offset < symbol.offset)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == input->members.len || input->members.data[lo].offset != symbol.offset) {
                error(c, (Loc){0}, "%s: archive index points outside its members", input->path);
                return;
            }
            ArchiveMember *member = &input->members.data[lo];
            if (member->selected)
                continue;
            const unsigned char *header = input->file.data + member->offset;
            size_t size;
            const char *name = member_name(c, input, header);
            if (!name || !member_header(input->file, member->offset, &size)) {
                error(c, (Loc){0}, "%s: invalid archive member", input->path);
                return;
            }
            member->selected = true;
            r->selected++;
            ld_take_object(r->linker, format(c, "%s(%s)", input->path, name),
                           (Bytes){.data = input->file.data + member->offset + 60, .len = size});
        }
    } while (previous != r->selected && !c->failed);
}

static const char *search_library(Reader *r, const char *name)
{
    const char *extensions[] = {".so", ".a"};
    for (size_t i = 0; i < r->options->libraries.len; i++) {
        const char *dir = r->options->libraries.data[i];
        if (dir[0] == '=')
            dir = format(r->context, "%s%s", r->options->root, dir + 1);
        for (size_t j = r->static_link ? 1 : 0; j < 2; j++) {
            const char *path = name[0] == ':' ? format(r->context, "%s/%s", dir, name + 1) :
                               format(r->context, "%s/lib%s%s", dir, name, extensions[j]);
            if (access(path, R_OK) == 0)
                return path;
        }
    }
    error(r->context, (Loc){0}, "cannot find library -l%s", name);
    return NULL;
}

static Input *read_input(Reader *, const char *, bool, bool, unsigned);

static const char *script_token(Context *c, const char **cursor)
{
    const char *p = *cursor;
    for (;;) {
        while (isspace((unsigned char)*p))
            p++;
        if (p[0] != '/' || p[1] != '*')
            break;
        const char *end = strstr(p + 2, "*/");
        if (!end)
            return NULL;
        p = end + 2;
    }
    if (!*p)
        return NULL;
    const char *start = p++;
    if (!strchr("(),;", *start))
        while (*p && !isspace((unsigned char)*p) && !strchr("(),;", *p))
            p++;
    *cursor = p;
    return copy(c, start, (size_t)(p - start));
}

static const char *script_path(Reader *r, const char *script, const char *name)
{
    Context *c = r->context;
    if (!strncmp(name, "-l", 2))
        return search_library(r, name + 2);
    if (name[0] == '/' || name[0] == '=') {
        const char *root = r->options->root;
        if (name[0] == '=' || *root)
            return format(c, "%s%s", root, name + (name[0] == '='));
        return name;
    }
    const char *slash = strrchr(script, '/');
    if (slash) {
        const char *candidate = format(c, "%.*s/%s", (int)(slash - script), script, name);
        if (access(candidate, R_OK) == 0)
            return candidate;
    }
    return name;
}

static bool script_list(Reader *r, Input *input, const char **cursor, bool as_needed, unsigned depth)
{
    Context *c = r->context;
    if (depth > 32)
        return false;
    const char *token = script_token(c, cursor);
    if (!token || !equal(token, "("))
        return false;
    while ((token = script_token(c, cursor)) && !c->failed) {
        if (equal(token, ")"))
            return true;
        if (equal(token, ","))
            continue;
        if (equal(token, "AS_NEEDED")) {
            if (!script_list(r, input, cursor, true, depth + 1))
                return false;
        } else {
            const char *path = script_path(r, input->path, token);
            if (path)
                PUSH(c, input->children, read_input(r, path, !strncmp(token, "-l", 2), as_needed, depth + 1));
        }
    }
    return false;
}

static bool read_script(Reader *r, Input *input, unsigned depth)
{
    Context *c = r->context;
    const char *p = (const char *)input->file.data;
    const char *token;
    input->kind = INPUT_SEQUENCE;
    while ((token = script_token(c, &p)) && !c->failed) {
        if (equal(token, "GROUP") || equal(token, "INPUT")) {
            if (equal(token, "GROUP"))
                input->kind = INPUT_GROUP;
            if (!script_list(r, input, &p, input->as_needed, depth))
                return false;
        } else if (equal(token, "OUTPUT_FORMAT")) {
            token = script_token(c, &p);
            if (!token || !equal(token, "("))
                return false;
            while ((token = script_token(c, &p)) && !equal(token, ")"))
                ;
            if (!token)
                return false;
        } else if (!equal(token, ";")) {
            return false;
        }
    }
    return input->children.len != 0;
}

static Input *read_input(Reader *r, const char *path, bool searched, bool as_needed, unsigned depth)
{
    Context *c = r->context;
    Input *input = alloc(c, sizeof *input);
    input->path = path;
    const char *slash = searched ? strrchr(path, '/') : NULL;
    input->link_name = slash ? slash + 1 : path;
    input->as_needed = as_needed;
    if (depth > 32) {
        error(c, (Loc){0}, "%s: linker scripts nested too deeply", path);
        return input;
    }
    if (!read_file(c, path, &input->file))
        return input;
    PUSH(c, r->files, path);
    Bytes file = input->file;
    if (file.len >= SELFMAG && !memcmp(file.data, ELFMAG, SELFMAG)) {
        Elf64_Ehdr h = {0};
        if (file.len >= sizeof h)
            memcpy(&h, file.data, sizeof h);
        if (h.e_type == ET_DYN) {
            input->kind = INPUT_SHARED;
            if (r->static_link)
                error(c, (Loc){0}, "%s: shared library in a static link", path);
        }
    } else if (file.len >= 8 && !memcmp(file.data, "!<arch>\n", 8)) {
        input->kind = INPUT_ARCHIVE;
        read_archive(c, input);
    } else if (r->options->hosted && file.len && !memchr(file.data, 0, file.len)) {
        bytes_add(c, &input->file, NULL, 1);
        if (!read_script(r, input, depth) && !c->failed)
            error(c, (Loc){0}, "%s: unsupported linker script", path);
    }
    return input;
}

static void select_input(Reader *r, Input *input)
{
    if (r->context->failed)
        return;
    if (input->kind == INPUT_ARCHIVE) {
        select_archive(r, input);
    } else if (input->kind == INPUT_GROUP || input->kind == INPUT_SEQUENCE) {
        size_t previous;
        do {
            previous = r->selected;
            for (size_t i = 0; i < input->children.len && !r->context->failed; i++)
                select_input(r, input->children.data[i]);
        } while (input->kind == INPUT_GROUP && previous != r->selected && !r->context->failed);
    } else if (!input->selected) {
        input->selected = true;
        r->selected++;
        if (input->kind == INPUT_OBJECT)
            ld_take_object(r->linker, input->path, input->file);
        else
            ld_take_shared(r->linker, input->path, input->link_name, input->file, input->as_needed);
    }
}

static bool read_inputs(Reader *r, Inputs *inputs, size_t *i, unsigned depth)
{
    Context *c = r->context;
    Strings files = r->options->elf.inputs;
    for (; *i < files.len && !c->failed; ++*i) {
        const char *arg = files.data[*i];
        bool searched = false;
        if (r->options->hosted) {
            if (equal(arg, "--end-group"))
                return depth != 0;
            if (equal(arg, "--start-group")) {
                Input *group = alloc(c, sizeof *group);
                group->kind = INPUT_GROUP;
                if (depth == 32)
                    return false;
                ++*i;
                if (!read_inputs(r, &group->children, i, depth + 1))
                    return false;
                PUSH(c, *inputs, group);
                continue;
            }
            if (equal(arg, "-Bdynamic") || equal(arg, "-Bstatic")) {
                r->static_link = equal(arg, "-Bstatic");
                continue;
            }
            if (equal(arg, "--as-needed") || equal(arg, "--no-as-needed")) {
                r->as_needed = equal(arg, "--as-needed");
                continue;
            }
            if (!strncmp(arg, "-l", 2)) {
                searched = true;
                arg = search_library(r, arg + 2);
                if (!arg)
                    return false;
            }
        }
        PUSH(c, *inputs, read_input(r, arg, searched, r->as_needed, depth));
    }
    return depth == 0;
}

bool ld_inputs(Context *c, Linker *l, LinkOptions *options)
{
    Reader r = {.context = c, .linker = l, .options = options};
    Inputs inputs = {0};
    size_t i = 0;
    if (!read_inputs(&r, &inputs, &i, 0) && !c->failed)
        error(c, (Loc){0}, "unmatched or excessively nested archive groups");
    for (i = 0; i < inputs.len && !c->failed; i++)
        select_input(&r, inputs.data[i]);
    options->elf.inputs = r.files;
    return !c->failed;
}
