#define _POSIX_C_SOURCE 200809L
#include "as.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct AsmSymbol {
    const char *name;
    AsmSymbol *alias;
    Loc alias_loc;
    unsigned section;
    size_t offset, size;
    unsigned type, visibility;
    uint32_t index;
    bool global, weak, unique, weak_reference, referenced;
};

struct AsmDifference {
    AsmSymbol *left, *right;
    size_t offset;
    unsigned section, width;
    Loc loc;
};

static const SectionKind extra_sections[] = {
    {".tdata", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS},
    {".tbss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS},
    {".preinit_array", SHT_PREINIT_ARRAY, SHF_ALLOC | SHF_WRITE},
    {".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE},
    {".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE},
};

char *mas_trim(char *text)
{
    while (isspace((unsigned char)*text))
        text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = 0;
    return text;
}

bool mas_integer(const char *text, uint64_t *value)
{
    char *end;
    errno = 0;
    uint64_t n = strtoull(text, &end, 0);
    if (end == text || errno == ERANGE)
        return false;
    while (isspace((unsigned char)*end))
        end++;
    if (*end)
        return false;
    *value = n;
    return true;
}

static bool symbol_char(char ch, bool first)
{
    return isalpha((unsigned char)ch) || ch == '_' || ch == '.' || ch == '$' ||
           (!first && isdigit((unsigned char)ch));
}

static AsmSymbol *symbol(Assembler *a, const char *name)
{
    if (!symbol_char(*name, true))
        return NULL;
    for (const char *p = name + 1; *p; p++)
        if (!symbol_char(*p, false))
            return NULL;
    for (size_t i = 0; i < a->symbols.len; i++)
        if (equal(a->symbols.data[i]->name, name))
            return a->symbols.data[i];

    AsmSymbol *s = alloc(a->context, sizeof *s);
    s->name = copy(a->context, name, strlen(name));
    PUSH(a->context, a->symbols, s);
    return s;
}

bool mas_expression(Assembler *a, char *text, AsmExpr *expr)
{
    *expr = (AsmExpr){0};
    text = mas_trim(text);
    if (mas_integer(text, &expr->value))
        return true;
    char *split = strpbrk(text, "+-");
    if (split) {
        bool negative = *split == '-';
        if (!mas_integer(mas_trim(split + 1), &expr->value))
            return false;
        if (negative)
            expr->value = 0 - expr->value;
        *split = 0;
    }
    expr->symbol = symbol(a, mas_trim(text));
    return expr->symbol != NULL;
}

static void append(Assembler *a, const void *data, size_t size)
{
    AsmSection *section = &a->sections.data[a->section];
    bool nobits = section->kind.type == SHT_NOBITS;
    if (a->section == SECTION_NONE || (nobits && data)) {
        error(a->context, a->loc, "cannot emit initialized bytes in this section");
        return;
    }
    if (size > (size_t)PTRDIFF_MAX - section->size) {
        error(a->context, a->loc, "assembler section is too large");
        return;
    }
    if (!nobits)
        bytes_add(a->context, &section->bytes, data, size);
    section->size += size;
}

void mas_emit(Assembler *a, uint64_t value, unsigned width)
{
    unsigned char raw[8];
    for (unsigned i = 0; i < width; i++)
        raw[i] = (unsigned char)(value >> (8 * i));
    append(a, raw, width);
}

void mas_relocate(Assembler *a, uint32_t type, AsmExpr expr)
{
    AsmSection *section = &a->sections.data[a->section];
    if (!expr.symbol || a->section == SECTION_NONE || section->kind.type == SHT_NOBITS) {
        error(a->context, a->loc, "invalid symbolic relocation");
        return;
    }
    if ((a->machine == EM_X86_64 && type == R_X86_64_TLSGD) ||
        (a->machine == EM_AARCH64 && (type == R_AARCH64_TLSDESC_ADR_PAGE21 ||
                                     type == R_AARCH64_TLSDESC_LD64_LO12 ||
                                     type == R_AARCH64_TLSDESC_ADD_LO12 || type == R_AARCH64_TLSDESC_CALL)))
        expr.symbol->type = STT_TLS;
    expr.symbol->referenced = true;
    PUSH(a->context, section->relocations, ((AsmReloc){section->size, type, expr}));
}

static bool split_args(char *text, AsmArgs *args)
{
    text = mas_trim(text);
    if (!*text)
        return true;
    unsigned depth = 0;
    bool quoted = false;
    char *start = text;
    for (char *p = text;; p++) {
        char ch = *p;
        if (!ch || (!quoted && !depth && ch == ',')) {
            if (args->len == sizeof args->data / sizeof *args->data)
                return false;
            *p = 0;
            char *argument = mas_trim(start);
            if (!*argument)
                return false;
            args->data[args->len++] = argument;
            if (!ch)
                return !quoted && !depth;
            start = p + 1;
        } else if (ch == '"') {
            quoted = !quoted;
        } else if (quoted && ch == '\\' && p[1]) {
            p++;
        } else if (!quoted && (ch == '[' || ch == '(')) {
            depth++;
        } else if (!quoted && (ch == ']' || ch == ')')) {
            if (!depth)
                return false;
            depth--;
        }
    }
}

static bool define_label(Assembler *a, char *name)
{
    AsmSymbol *s = symbol(a, name);
    if (!s || a->section == SECTION_NONE || s->section || s->alias) {
        error(a->context, a->loc, "invalid or duplicate label '%s'", name);
        return false;
    }
    s->section = a->section;
    s->offset = a->sections.data[a->section].size;
    return true;
}

static bool align_section(Assembler *a, uint64_t alignment)
{
    if (!alignment || alignment > 65536 || (alignment & (alignment - 1)) || a->section == SECTION_NONE)
        return false;
    AsmSection *s = &a->sections.data[a->section];
    size_t boundary = (size_t)alignment;
    if (boundary > s->alignment)
        s->alignment = boundary;
    size_t padding = (0 - s->size) & (boundary - 1);
    bool code = (s->kind.flags & SHF_EXECINSTR) != 0;
    if (code && a->machine == EM_X86_64) {
        for (size_t i = 0; i < padding; i++)
            mas_emit(a, 0x90, 1);
    } else if (code && s->size % 4 == 0 && padding % 4 == 0) {
        for (size_t i = 0; i < padding; i += 4)
            mas_emit(a, 0xd503201f, 4);
    } else {
        append(a, NULL, padding);
    }
    return true;
}

static bool string_literal(Assembler *a, const char *text)
{
    if (*text++ != '"')
        return false;
    while (*text && *text != '"') {
        unsigned char ch = (unsigned char)*text++;
        if (ch == '\\') {
            ch = (unsigned char)*text++;
            switch (ch) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case '"': case '\\': break;
            default:
                if (ch < '0' || ch > '7')
                    return false;
                ch -= '0';
                for (unsigned i = 0; i < 2 && *text >= '0' && *text <= '7'; i++)
                    ch = (unsigned char)(ch * 8 + *text++ - '0');
                break;
            }
        }
        mas_emit(a, ch, 1);
    }
    return text[0] == '"' && !text[1];
}

static bool symbol_directive(Assembler *a, const char *op, AsmArgs *args)
{
    if (!equal(op, ".type") && !equal(op, ".size")) {
        for (size_t i = 0; i < args->len; i++) {
            AsmSymbol *s = symbol(a, args->data[i]);
            if (!s)
                return false;
            if (equal(op, ".hidden"))
                s->visibility = STV_HIDDEN;
            else if (equal(op, ".internal"))
                s->visibility = STV_INTERNAL;
            else if (equal(op, ".protected"))
                s->visibility = STV_PROTECTED;
            else if (equal(op, ".weak"))
                s->weak = true;
            else
                s->global = true;
        }
        return args->len != 0;
    }
    if (args->len != 2)
        return false;
    AsmSymbol *s = symbol(a, args->data[0]);
    if (!s)
        return false;
    const char *value = args->data[1];
    if (equal(op, ".type")) {
        if (*value == '@' || *value == '%')
            value++;
        if (equal(value, "function"))
            s->type = STT_FUNC;
        else if (equal(value, "gnu_indirect_function"))
            s->type = STT_GNU_IFUNC;
        else if (equal(value, "object"))
            s->type = STT_OBJECT;
        else if (equal(value, "gnu_unique_object")) {
            s->type = STT_OBJECT;
            s->unique = true;
            s->global = true;
        }
        else if (equal(value, "tls_object"))
            s->type = STT_TLS;
        else if (!equal(value, "notype"))
            return false;
        return true;
    }
    uint64_t size = 0;
    if (!strncmp(value, ".-", 2) && equal(value + 2, s->name) && s->section == a->section)
        size = a->sections.data[a->section].size - s->offset;
    else if (!mas_integer(value, &size) || size > (uint64_t)PTRDIFF_MAX)
        return false;
    s->size = (size_t)size;
    return true;
}

static bool symbol_alias(Assembler *a, AsmArgs *args, bool weak_reference)
{
    if (args->len != 2)
        return false;
    AsmSymbol *name = symbol(a, args->data[0]);
    AsmSymbol *target = symbol(a, args->data[1]);
    if (!name || !target || name->section ||
        (name->alias && (name->alias != target || name->weak_reference != weak_reference)))
        return false;
    name->alias = target;
    name->alias_loc = a->loc;
    name->weak_reference = weak_reference;
    return true;
}

static void resolve_aliases(Assembler *a)
{
    for (size_t i = 0; i < a->symbols.len && !a->context->failed; i++) {
        AsmSymbol *s = a->symbols.data[i];
        if (!s->alias || s->weak_reference)
            continue;
        AsmSymbol *target = s->alias;
        unsigned alias_type = target->type;
        size_t steps = 0;
        while (target->alias && ++steps < a->symbols.len) {
            target = target->alias;
            if (!alias_type)
                alias_type = target->type;
        }
        if (target->alias || !target->section) {
            error(a->context, s->alias_loc, "alias '%s' requires a defined target", s->name);
            return;
        }
        s->section = target->section;
        s->offset = target->offset;
        if (!s->type)
            s->type = alias_type;
        if (!s->size)
            s->size = target->size;
    }
}

static AsmSymbol *weak_referent(Assembler *a, AsmSymbol *alias)
{
    AsmSymbol *target = alias;
    size_t steps = 0;
    while (target->weak_reference && ++steps < a->symbols.len)
        target = target->alias;
    if (target->weak_reference)
        error(a->context, alias->alias_loc, "weak reference '%s' is part of a cycle", alias->name);
    return target;
}

static void resolve_weak_references(Assembler *a)
{
    for (size_t i = 0; i < a->symbols.len && !a->context->failed; i++)
        if (a->symbols.data[i]->weak_reference)
            weak_referent(a, a->symbols.data[i]);
    if (a->context->failed)
        return;
    for (size_t section = 1; section < a->sections.len; section++) {
        AsmSection *s = &a->sections.data[section];
        for (size_t i = 0; i < s->relocations.len; i++) {
            AsmExpr *e = &s->relocations.data[i].expr;
            if (!e->symbol->weak_reference)
                continue;
            AsmSymbol *target = weak_referent(a, e->symbol);
            if (!target->section && !target->referenced)
                target->weak = true;
            if (!target->type)
                target->type = e->symbol->type;
            target->referenced = true;
            e->symbol = target;
        }
    }
    for (size_t i = 0; i < a->differences.len; i++) {
        AsmDifference *d = &a->differences.data[i];
        d->left = weak_referent(a, d->left);
        d->right = weak_referent(a, d->right);
    }
}

static bool data_directive(Assembler *a, AsmArgs *args, unsigned width)
{
    uint32_t reloc = a->machine == EM_AARCH64 ?
                     (width == 8 ? R_AARCH64_ABS64 : R_AARCH64_ABS32) :
                     (width == 8 ? R_X86_64_64 : R_X86_64_32);
    for (size_t i = 0; i < args->len; i++) {
        char *text = args->data[i];
        char *minus = strchr(text, '-');
        if (minus && symbol_char(*mas_trim(minus + 1), true)) {
            *minus = 0;
            AsmSymbol *left = symbol(a, mas_trim(text));
            AsmSymbol *right = symbol(a, mas_trim(minus + 1));
            if (!left || !right)
                return false;
            AsmDifference difference = {left, right, a->sections.data[a->section].size,
                                        a->section, width, a->loc};
            PUSH(a->context, a->differences, difference);
            mas_emit(a, 0, width);
            continue;
        }
        AsmExpr expr;
        if (!mas_expression(a, text, &expr))
            return false;
        if (expr.symbol) {
            if (width < 4)
                return false;
            mas_relocate(a, reloc, expr);
        }
        mas_emit(a, expr.symbol ? 0 : expr.value, width);
    }
    return args->len != 0;
}

static void resolve_differences(Assembler *a)
{
    for (size_t i = 0; i < a->differences.len && !a->context->failed; i++) {
        AsmDifference diff = a->differences.data[i];
        if (!diff.left->section || diff.left->section != diff.right->section) {
            error(a->context, diff.loc, "symbol difference requires two definitions in the same section");
            return;
        }
        uint64_t value = (uint64_t)diff.left->offset - (uint64_t)diff.right->offset;
        unsigned char *out = a->sections.data[diff.section].bytes.data + diff.offset;
        for (unsigned b = 0; b < diff.width; b++)
            out[b] = (unsigned char)(value >> (8 * b));
    }
}

static bool section_type(const char *name, uint32_t *type)
{
    const struct { const char *name; uint32_t type; } types[] = {
        {"progbits", SHT_PROGBITS}, {"nobits", SHT_NOBITS}, {"note", SHT_NOTE},
        {"preinit_array", SHT_PREINIT_ARRAY}, {"init_array", SHT_INIT_ARRAY}, {"fini_array", SHT_FINI_ARRAY}
    };
    if (*name != '%' && *name != '@')
        return false;
    for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
        if (equal(name + 1, types[i].name)) {
            *type = types[i].type;
            return true;
        }
    }
    return false;
}

static bool section_flags(const char *text, uint32_t *flags)
{
    if (*text++ != '"')
        return false;
    *flags = 0;
    for (; *text && *text != '"'; text++) {
        switch (*text) {
        case 'a': *flags |= SHF_ALLOC; break;
        case 'w': *flags |= SHF_WRITE; break;
        case 'x': *flags |= SHF_EXECINSTR; break;
        case 'T': *flags |= SHF_TLS; break;
        case 'o': *flags |= SHF_LINK_ORDER; break;
        default: return false;
        }
    }
    return *text == '"' && !text[1];
}

static const char *section_name(Assembler *a, const char *text)
{
    if (*text != '"')
        return *text && !strpbrk(text, " \t\"\\") ? text : NULL;
    char *name = alloc(a->context, strlen(text));
    size_t len = 0;
    for (text++; *text && *text != '"'; text++) {
        if (*text == '\\' && *++text != '\\' && *text != '"')
            return NULL;
        name[len++] = *text;
    }
    name[len] = 0;
    return len && *text == '"' && !text[1] ? name : NULL;
}

static SectionKind section_defaults(const char *name)
{
    for (unsigned i = 1; i < SECTION_COUNT; i++)
        if (elf_section_family(name, elf_sections[i].name))
            return elf_sections[i];
    for (size_t i = 0; i < sizeof extra_sections / sizeof *extra_sections; i++)
        if (elf_section_family(name, extra_sections[i].name))
            return extra_sections[i];
    return (SectionKind){name, SHT_PROGBITS, 0};
}

static bool select_section(Assembler *a, const char *name, AsmArgs *args, bool explicit_section)
{
    uint32_t explicit_flags = 0;
    if (args->len > 4 || (args->len > 1 && !section_flags(args->data[1], &explicit_flags)) ||
        ((args->len == 4) != ((explicit_flags & SHF_LINK_ORDER) != 0)))
        return false;
    AsmSymbol *linked_to = NULL;
    if (args->len == 4 && !equal(args->data[3], "0")) {
        if (!symbol_char(args->data[3][0], true))
            return false;
        for (const char *p = args->data[3] + 1; *p; p++)
            if (!symbol_char(*p, false))
                return false;
        linked_to = symbol(a, args->data[3]);
    }
    size_t index = 1;
    for (; index < a->sections.len; index++) {
        const AsmSection *s = &a->sections.data[index];
        if (equal(name, s->kind.name) && linked_to == s->linked_to &&
            ((explicit_flags ^ s->kind.flags) & SHF_LINK_ORDER) == 0)
            break;
    }
    bool existing = index < a->sections.len;
    if (!existing && !explicit_section)
        return false;
    SectionKind kind = existing ? a->sections.data[index].kind : section_defaults(name);
    uint32_t flags = args->len > 1 ? explicit_flags : kind.flags, type = kind.type;
    if ((args->len >= 3 && !section_type(args->data[2], &type)) ||
        (existing && (type != kind.type || flags != kind.flags)))
        return false;
    if (!existing) {
        if (index >= SHN_LORESERVE)
            return false;
        kind.name = copy(a->context, name, strlen(name));
        kind.type = type;
        kind.flags = flags;
        unsigned alignment = a->machine == EM_AARCH64 && (flags & SHF_EXECINSTR) ? 4 : 1;
        PUSH(a->context, a->sections, ((AsmSection){.kind = kind, .alignment = alignment,
                                                   .linked_to = linked_to, .link_location = a->loc}));
    }
    a->section = (unsigned)index;
    return true;
}

static bool directive(Assembler *a, const char *op, AsmArgs *args)
{
    bool section = equal(op, ".section");
    const char *name = section && args->len ? section_name(a, args->data[0]) : op;
    if (!name || (section && !args->len))
        return false;
    if (section || args->len == 0) {
        if (equal(name, ".note.GNU-stack")) {
            if (args->len > 3 || (args->len > 1 && !equal(args->data[1], "\"\"")) ||
                (args->len == 3 && !equal(args->data[2], "%progbits") && !equal(args->data[2], "@progbits")))
                return false;
            a->section = SECTION_NONE;
            return true;
        }
        if (select_section(a, name, args, section))
            return true;
    }
    if (equal(op, ".tlsdesccall") && a->machine == EM_AARCH64 &&
        (a->sections.data[a->section].kind.flags & SHF_EXECINSTR)) {
        AsmExpr target;
        if (args->len != 1 || !mas_expression(a, args->data[0], &target) || !target.symbol || target.value)
            return false;
        mas_relocate(a, R_AARCH64_TLSDESC_CALL, target);
        return true;
    }
    if (equal(op, ".global") || equal(op, ".globl") || equal(op, ".hidden") || equal(op, ".internal") ||
        equal(op, ".protected") || equal(op, ".weak") || equal(op, ".type") || equal(op, ".size"))
        return symbol_directive(a, op, args);
    if (equal(op, ".set") || equal(op, ".weakref"))
        return symbol_alias(a, args, equal(op, ".weakref"));
    if (equal(op, ".byte"))
        return data_directive(a, args, 1);
    if (equal(op, ".short") || equal(op, ".hword") || equal(op, ".value"))
        return data_directive(a, args, 2);
    if (equal(op, ".word"))
        return data_directive(a, args, a->machine == EM_AARCH64 ? 4u : 2u);
    if (equal(op, ".long"))
        return data_directive(a, args, 4);
    if (equal(op, ".quad") || equal(op, ".xword"))
        return data_directive(a, args, 8);
    if (equal(op, ".ascii") || equal(op, ".asciz") || equal(op, ".string")) {
        for (size_t i = 0; i < args->len; i++) {
            if (!string_literal(a, args->data[i]))
                return false;
            if (!equal(op, ".ascii"))
                mas_emit(a, 0, 1);
        }
        return args->len != 0;
    }
    uint64_t value;
    if (args->len != 1 || !mas_integer(args->data[0], &value))
        return false;
    if (equal(op, ".zero") || equal(op, ".space")) {
        if (value > (uint64_t)PTRDIFF_MAX)
            return false;
        append(a, NULL, (size_t)value);
        return true;
    }
    if (equal(op, ".p2align") || (equal(op, ".align") && a->machine == EM_AARCH64))
        return value <= 16 && align_section(a, UINT64_C(1) << value);
    return (equal(op, ".align") || equal(op, ".balign")) && align_section(a, value);
}

static void strip_comment(Assembler *a, char *text)
{
    bool quoted = false;
    for (char *p = text; *p; p++) {
        if (quoted && *p == '\\' && p[1])
            p++;
        else if (*p == '"')
            quoted = !quoted;
        else if (!quoted &&
                 ((a->machine == EM_X86_64 && *p == '#') || (*p == '/' && p[1] == '/'))) {
            *p = 0;
            return;
        }
    }
}

static void statement(Assembler *a, char *text)
{
    strip_comment(a, text);
    text = mas_trim(text);
    if (!*text || *text == '#')
        return;
    for (;;) {
        char *end = text;
        while (symbol_char(*end, false))
            end++;
        if (*end != ':')
            break;
        *end = 0;
        if (!define_label(a, text))
            return;
        text = mas_trim(end + 1);
        if (!*text)
            return;
    }
    char *end = text;
    while (*end && !isspace((unsigned char)*end))
        end++;
    char *arguments = end;
    if (*end) {
        *end = 0;
        arguments++;
    }
    AsmArgs args = {0};
    if (!split_args(arguments, &args)) {
        error(a->context, a->loc, "invalid operands to '%s'", text);
        return;
    }
    bool ok = *text == '.' ? directive(a, text, &args) :
              !(a->sections.data[a->section].kind.flags & SHF_EXECINSTR) ? false :
              a->machine == EM_AARCH64 ? mas_aarch64(a, text, &args) : mas_x86_64(a, text, &args);
    if (!ok && !a->context->failed)
        error(a->context, a->loc, "unsupported or invalid assembly '%s'", text);
}

static void assemble_file(Assembler *a, const char *path)
{
    FILE *input = equal(path, "-") ? stdin : fopen(path, "r");
    if (!input) {
        error(a->context, (Loc){0}, "%s: %s", path, strerror(errno));
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    a->loc = (Loc){.file = input == stdin ? "<stdin>" : path, .column = 1};
    while (!a->context->failed) {
        ssize_t length = getline(&line, &capacity, input);
        if (length < 0)
            break;
        a->loc.line++;
        if (memchr(line, 0, (size_t)length))
            error(a->context, a->loc, "NUL in assembly input");
        else
            statement(a, line);
    }
    if (ferror(input))
        error(a->context, a->loc, "cannot read assembly: %s", strerror(errno));
    free(line);
    if (input != stdin)
        fclose(input);
}

static void put_section(Context *c, Bytes *object, Elf64_Shdr *section, const Bytes *bytes)
{
    bytes_align(c, object, (size_t)section->sh_addralign);
    section->sh_offset = object->len;
    section->sh_size = bytes->len;
    bytes_add(c, object, bytes->data, bytes->len);
}

static bool build_object(Assembler *a, ElfOptions *options)
{
    Context *c = a->context;
    size_t stack_note = a->sections.len;
    size_t symtab = stack_note + 1, strtab = symtab + 1, shstrtab = strtab + 1;
    size_t count = shstrtab + 1, section_count = count;
    for (size_t i = 1; i < a->sections.len; i++) {
        const AsmSection *section = &a->sections.data[i];
        if (section->linked_to && !section->linked_to->section) {
            error(c, section->link_location, "undefined section link symbol '%s'", section->linked_to->name);
            return false;
        }
        section_count += section->relocations.len != 0;
    }
    if (section_count >= SHN_LORESERVE) {
        error(c, (Loc){0}, "too many ELF sections");
        return false;
    }
    Elf64_Shdr *sections = alloc(c, section_count * sizeof *sections);
    Bytes names = {0}, strings = {0}, symbols = {0}, output = {0};
    bytes_add(c, &names, NULL, 1);
    bytes_add(c, &strings, NULL, 1);
    bytes_add(c, &symbols, NULL, sizeof(Elf64_Sym));
    bytes_add(c, &output, NULL, sizeof(Elf64_Ehdr));
    uint32_t index = 1, first_global = 1;
    const unsigned bindings[] = {STB_LOCAL, STB_GLOBAL, STB_WEAK, STB_GNU_UNIQUE};
    for (size_t pass = 0; pass < sizeof bindings / sizeof *bindings; pass++) {
        unsigned binding = bindings[pass];
        if (binding == STB_GLOBAL)
            first_global = index;
        for (size_t i = 0; i < a->symbols.len; i++) {
            AsmSymbol *s = a->symbols.data[i];
            if (s->weak_reference ||
                (!s->section && !s->referenced && (s->weak || (s->visibility && !s->global))))
                continue;
            if (!s->section && !strncmp(s->name, ".L", 2)) {
                error(c, (Loc){0}, "undefined local symbol '%s'", s->name);
                return false;
            }
            unsigned actual = s->unique && s->section ? STB_GNU_UNIQUE :
                              s->weak ? STB_WEAK : (s->global || !s->section ? STB_GLOBAL : STB_LOCAL);
            if (actual != binding)
                continue;
            unsigned type = a->sections.data[s->section].kind.flags & SHF_TLS ? STT_TLS : s->type;
            Elf64_Sym entry = {.st_name = bytes_string(c, &strings, s->name), .st_shndx = (uint16_t)s->section,
                               .st_info = (unsigned char)ELF64_ST_INFO(actual, type),
                               .st_other = (unsigned char)s->visibility,
                               .st_value = s->offset, .st_size = s->size};
            s->index = index++;
            bytes_add(c, &symbols, &entry, sizeof entry);
        }
    }

    for (size_t i = 1; i < a->sections.len; i++) {
        const AsmSection *section = &a->sections.data[i];
        const SectionKind *kind = &section->kind;
        Elf64_Shdr *header = &sections[i];
        *header = (Elf64_Shdr){.sh_name = bytes_string(c, &names, kind->name),
                              .sh_type = kind->type, .sh_flags = kind->flags,
                              .sh_link = section->linked_to ? section->linked_to->section : 0,
                              .sh_addralign = section->alignment};
        if (kind->type == SHT_PREINIT_ARRAY || kind->type == SHT_INIT_ARRAY || kind->type == SHT_FINI_ARRAY)
            header->sh_entsize = sizeof(Elf64_Addr);
        put_section(c, &output, header, &section->bytes);
        header->sh_size = section->size;
    }
    sections[stack_note] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".note.GNU-stack"),
                                       .sh_type = SHT_PROGBITS, .sh_addralign = 1,
                                       .sh_offset = output.len};
    sections[symtab] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".symtab"), .sh_type = SHT_SYMTAB,
                                    .sh_link = (uint32_t)strtab, .sh_info = first_global, .sh_addralign = 8,
                                    .sh_entsize = sizeof(Elf64_Sym)};
    sections[strtab] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".strtab"),
                                   .sh_type = SHT_STRTAB, .sh_addralign = 1};
    sections[shstrtab] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".shstrtab"),
                                     .sh_type = SHT_STRTAB, .sh_addralign = 1};
    put_section(c, &output, &sections[symtab], &symbols);
    put_section(c, &output, &sections[strtab], &strings);

    for (size_t i = 1; i < a->sections.len; i++) {
        const AsmSection *section = &a->sections.data[i];
        if (!section->relocations.len)
            continue;
        Bytes entries = {0};
        for (size_t j = 0; j < section->relocations.len; j++) {
            AsmReloc r = section->relocations.data[j];
            Elf64_Rela entry = {.r_offset = r.offset,
                                .r_info = ELF64_R_INFO(r.expr.symbol->index, r.type),
                                .r_addend = (int64_t)r.expr.value};
            bytes_add(c, &entries, &entry, sizeof entry);
        }
        const char *name = format(c, ".rela%s", section->kind.name);
        sections[count] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, name), .sh_type = SHT_RELA,
                                      .sh_link = (uint32_t)symtab, .sh_info = (uint32_t)i, .sh_addralign = 8,
                                      .sh_entsize = sizeof(Elf64_Rela)};
        put_section(c, &output, &sections[count++], &entries);
    }
    put_section(c, &output, &sections[shstrtab], &names);
    bytes_align(c, &output, 8);
    Elf64_Ehdr header = elf_header(options->machine, ET_REL);
    for (size_t i = 0; i < a->symbols.len; i++)
        if (a->symbols.data[i]->type == STT_GNU_IFUNC || a->symbols.data[i]->unique) {
            header.e_ident[EI_OSABI] = ELFOSABI_GNU;
            break;
        }
    header.e_shoff = output.len;
    header.e_shnum = (uint16_t)count;
    header.e_shstrndx = (uint16_t)shstrtab;
    bytes_add(c, &output, sections, sizeof *sections * count);
    memcpy(output.data, &header, sizeof header);
    return elf_write(c, options, &output, false);
}

bool mas_assemble(Context *c, ElfOptions *options)
{
    Assembler assembler = {.context = c, .machine = options->machine, .section = SECTION_TEXT};
    for (unsigned i = 0; i < SECTION_COUNT; i++)
        PUSH(c, assembler.sections, ((AsmSection){.kind = elf_sections[i], .alignment = 1}));
    if (options->machine == EM_AARCH64)
        assembler.sections.data[SECTION_TEXT].alignment = 4;
    for (size_t i = 0; i < options->inputs.len && !c->failed; i++)
        assemble_file(&assembler, options->inputs.data[i]);
    resolve_aliases(&assembler);
    resolve_weak_references(&assembler);
    resolve_differences(&assembler);
    return !c->failed && build_object(&assembler, options);
}
