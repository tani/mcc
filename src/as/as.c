#define _POSIX_C_SOURCE 200809L
#include "as.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct AsmSymbol {
    const char *name;
    unsigned section;
    size_t offset, size;
    unsigned type, visibility;
    uint32_t index;
    bool global;
};

static const SectionKind *section_kind(unsigned index)
{
    static const SectionKind extra[] = {
        {".tdata", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS},
        {".tbss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS},
        {".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE},
    };
    return index < SECTION_COUNT ? &elf_sections[index] : &extra[index - SECTION_COUNT];
}

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
    AsmSection *section = &a->sections[a->section];
    bool nobits = section_kind(a->section)->type == SHT_NOBITS;
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
    AsmSection *section = &a->sections[a->section];
    if (!expr.symbol || a->section == SECTION_NONE || section_kind(a->section)->type == SHT_NOBITS) {
        error(a->context, a->loc, "invalid symbolic relocation");
        return;
    }
    if ((a->machine == EM_X86_64 && type == R_X86_64_TLSGD) ||
        (a->machine == EM_AARCH64 && (type == R_AARCH64_TLSDESC_ADR_PAGE21 ||
                                     type == R_AARCH64_TLSDESC_LD64_LO12 ||
                                     type == R_AARCH64_TLSDESC_ADD_LO12 || type == R_AARCH64_TLSDESC_CALL)))
        expr.symbol->type = STT_TLS;
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
    if (!s || a->section == SECTION_NONE || s->section) {
        error(a->context, a->loc, "invalid or duplicate label '%s'", name);
        return false;
    }
    s->section = a->section;
    s->offset = a->sections[a->section].size;
    return true;
}

static bool align_section(Assembler *a, uint64_t alignment)
{
    if (!alignment || alignment > 65536 || (alignment & (alignment - 1)) || a->section == SECTION_NONE)
        return false;
    AsmSection *s = &a->sections[a->section];
    size_t boundary = (size_t)alignment;
    if (boundary > s->alignment)
        s->alignment = boundary;
    size_t padding = (0 - s->size) & (boundary - 1);
    if (a->section == SECTION_TEXT && a->machine == EM_X86_64) {
        for (size_t i = 0; i < padding; i++)
            mas_emit(a, 0x90, 1);
    } else if (a->section == SECTION_TEXT && s->size % 4 == 0 && padding % 4 == 0) {
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
        else if (equal(value, "object"))
            s->type = STT_OBJECT;
        else if (equal(value, "tls_object"))
            s->type = STT_TLS;
        else if (!equal(value, "notype"))
            return false;
        return true;
    }
    uint64_t size = 0;
    if (!strncmp(value, ".-", 2) && equal(value + 2, s->name) && s->section == a->section)
        size = a->sections[a->section].size - s->offset;
    else if (!mas_integer(value, &size) || size > (uint64_t)PTRDIFF_MAX)
        return false;
    s->size = (size_t)size;
    return true;
}

static bool data_directive(Assembler *a, AsmArgs *args, unsigned width)
{
    uint32_t reloc = a->machine == EM_AARCH64 ?
                     (width == 8 ? R_AARCH64_ABS64 : R_AARCH64_ABS32) :
                     (width == 8 ? R_X86_64_64 : R_X86_64_32);
    for (size_t i = 0; i < args->len; i++) {
        AsmExpr expr;
        if (!mas_expression(a, args->data[i], &expr))
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

static bool directive(Assembler *a, const char *op, AsmArgs *args)
{
    bool section = equal(op, ".section");
    const char *name = section && args->len ? args->data[0] : op;
    if (section || args->len == 0) {
        for (unsigned i = 1; i < ASM_SECTION_COUNT; i++) {
            const SectionKind *kind = section_kind(i);
            if (!equal(name, kind->name))
                continue;
            const char *flags = i == SECTION_TEXT ? "\"ax\"" : i == SECTION_RODATA ? "\"a\"" :
                                kind->flags & SHF_TLS ? "\"awT\"" : "\"aw\"";
            const char *type = kind->type == SHT_NOBITS ? "nobits" :
                               kind->type == SHT_FINI_ARRAY ? "fini_array" : "progbits";
            if (args->len > 3 || (args->len > 1 && !equal(args->data[1], flags)) ||
                (args->len == 3 && ((*args->data[2] != '%' && *args->data[2] != '@') ||
                                   !equal(args->data[2] + 1, type))))
                return false;
            a->section = i;
            a->sections[i].present = true;
            return true;
        }
        if (equal(name, ".note.GNU-stack")) {
            if (args->len > 3 || (args->len > 1 && !equal(args->data[1], "\"\"")) ||
                (args->len == 3 && !equal(args->data[2], "%progbits") && !equal(args->data[2], "@progbits")))
                return false;
            a->section = SECTION_NONE;
            return true;
        }
    }
    if (equal(op, ".tlsdesccall") && a->machine == EM_AARCH64 && a->section == SECTION_TEXT) {
        AsmExpr target;
        if (args->len != 1 || !mas_expression(a, args->data[0], &target) || !target.symbol || target.value)
            return false;
        mas_relocate(a, R_AARCH64_TLSDESC_CALL, target);
        return true;
    }
    if (equal(op, ".global") || equal(op, ".globl") || equal(op, ".hidden") ||
        equal(op, ".type") || equal(op, ".size"))
        return symbol_directive(a, op, args);
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
              a->section != SECTION_TEXT ? false :
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
    Elf64_Shdr sections[2 * ASM_SECTION_COUNT + 4] = {{0}};
    uint16_t section_index[ASM_SECTION_COUNT] = {0}, count = 1;
    for (unsigned i = 1; i < ASM_SECTION_COUNT; i++)
        if (i < SECTION_COUNT || a->sections[i].present)
            section_index[i] = count++;
    uint16_t stack_note = count++, symtab = count++, strtab = count++, shstrtab = count++;
    Bytes names = {0}, strings = {0}, symbols = {0}, output = {0};
    bytes_add(c, &names, NULL, 1);
    bytes_add(c, &strings, NULL, 1);
    bytes_add(c, &symbols, NULL, sizeof(Elf64_Sym));
    bytes_add(c, &output, NULL, sizeof(Elf64_Ehdr));
    uint32_t index = 1, first_global = 1;
    for (unsigned binding = STB_LOCAL; binding <= STB_GLOBAL; binding++) {
        if (binding == STB_GLOBAL)
            first_global = index;
        for (size_t i = 0; i < a->symbols.len; i++) {
            AsmSymbol *s = a->symbols.data[i];
            if (!s->section && !strncmp(s->name, ".L", 2)) {
                error(c, (Loc){0}, "undefined local symbol '%s'", s->name);
                return false;
            }
            unsigned actual = s->global || !s->section ? STB_GLOBAL : STB_LOCAL;
            if (actual != binding)
                continue;
            unsigned type = section_kind(s->section)->flags & SHF_TLS ? STT_TLS : s->type;
            Elf64_Sym entry = {.st_name = bytes_string(c, &strings, s->name), .st_shndx = section_index[s->section],
                               .st_info = (unsigned char)ELF64_ST_INFO(actual, type),
                               .st_other = (unsigned char)s->visibility,
                               .st_value = s->offset, .st_size = s->size};
            s->index = index++;
            bytes_add(c, &symbols, &entry, sizeof entry);
        }
    }

    for (unsigned i = 1; i < ASM_SECTION_COUNT; i++) {
        if (!section_index[i])
            continue;
        const AsmSection *section = &a->sections[i];
        const SectionKind *kind = section_kind(i);
        Elf64_Shdr *header = &sections[section_index[i]];
        *header = (Elf64_Shdr){.sh_name = bytes_string(c, &names, kind->name),
                              .sh_type = kind->type, .sh_flags = kind->flags,
                              .sh_addralign = section->alignment};
        put_section(c, &output, header, &section->bytes);
        header->sh_size = section->size;
    }
    sections[stack_note] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".note.GNU-stack"),
                                       .sh_type = SHT_PROGBITS, .sh_addralign = 1,
                                       .sh_offset = output.len};
    sections[symtab] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".symtab"), .sh_type = SHT_SYMTAB,
                                    .sh_link = strtab, .sh_info = first_global, .sh_addralign = 8,
                                    .sh_entsize = sizeof(Elf64_Sym)};
    sections[strtab] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".strtab"),
                                   .sh_type = SHT_STRTAB, .sh_addralign = 1};
    sections[shstrtab] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".shstrtab"),
                                     .sh_type = SHT_STRTAB, .sh_addralign = 1};
    put_section(c, &output, &sections[symtab], &symbols);
    put_section(c, &output, &sections[strtab], &strings);

    for (unsigned i = 1; i < ASM_SECTION_COUNT; i++) {
        const AsmSection *section = &a->sections[i];
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
        const char *name = format(c, ".rela%s", section_kind(i)->name);
        sections[count] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, name), .sh_type = SHT_RELA,
                                      .sh_link = symtab, .sh_info = section_index[i], .sh_addralign = 8,
                                      .sh_entsize = sizeof(Elf64_Rela)};
        put_section(c, &output, &sections[count++], &entries);
    }
    put_section(c, &output, &sections[shstrtab], &names);
    bytes_align(c, &output, 8);
    Elf64_Ehdr header = elf_header(options->machine, ET_REL);
    header.e_shoff = output.len;
    header.e_shnum = count;
    header.e_shstrndx = shstrtab;
    bytes_add(c, &output, sections, sizeof *sections * count);
    memcpy(output.data, &header, sizeof header);
    return elf_write(c, options, &output, false);
}

bool mas_assemble(Context *c, ElfOptions *options)
{
    Assembler assembler = {.context = c, .machine = options->machine, .section = SECTION_TEXT};
    for (unsigned i = 1; i < ASM_SECTION_COUNT; i++)
        assembler.sections[i].alignment = 1;
    if (options->machine == EM_AARCH64)
        assembler.sections[SECTION_TEXT].alignment = 4;
    for (size_t i = 0; i < options->inputs.len && !c->failed; i++)
        assemble_file(&assembler, options->inputs.data[i]);
    return !c->failed && build_object(&assembler, options);
}
