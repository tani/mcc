#include "ld.h"

#include <inttypes.h>
#include <string.h>

enum {
    OUT_INIT = SECTION_COUNT, OUT_FINI, OUT_EH_FRAME, OUT_IPLT_RELA, OUT_GOT, OUT_PLT,
    OUT_PREINIT_ARRAY, OUT_INIT_ARRAY, OUT_FINI_ARRAY, OUT_TDATA, OUT_TBSS,
    OUT_INTERP, OUT_DYNSTR, OUT_DYNSYM, OUT_HASH, OUT_DYN_RELA, OUT_DYNAMIC,
    OUT_VERSYM, OUT_VERNEED, OUT_EH_FRAME_HDR, OUT_COUNT
};

static const SectionKind link_sections[] = {
    [OUT_INIT - SECTION_COUNT] = {".init", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR},
    [OUT_FINI - SECTION_COUNT] = {".fini", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR},
    [OUT_EH_FRAME - SECTION_COUNT] = {".eh_frame", SHT_PROGBITS, SHF_ALLOC},
    [OUT_IPLT_RELA - SECTION_COUNT] = {".rela.iplt", SHT_RELA, SHF_ALLOC},
    [OUT_GOT - SECTION_COUNT] = {".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE},
    [OUT_PLT - SECTION_COUNT] = {".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR},
    [OUT_PREINIT_ARRAY - SECTION_COUNT] = {".preinit_array", SHT_PREINIT_ARRAY, SHF_ALLOC | SHF_WRITE},
    [OUT_INIT_ARRAY - SECTION_COUNT] = {".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE},
    [OUT_FINI_ARRAY - SECTION_COUNT] = {".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE},
    [OUT_TDATA - SECTION_COUNT] = {".tdata", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS},
    [OUT_TBSS - SECTION_COUNT] = {".tbss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE | SHF_TLS},
    [OUT_INTERP - SECTION_COUNT] = {".interp", SHT_PROGBITS, SHF_ALLOC},
    [OUT_DYNSTR - SECTION_COUNT] = {".dynstr", SHT_STRTAB, SHF_ALLOC},
    [OUT_DYNSYM - SECTION_COUNT] = {".dynsym", SHT_DYNSYM, SHF_ALLOC},
    [OUT_HASH - SECTION_COUNT] = {".hash", SHT_HASH, SHF_ALLOC},
    [OUT_DYN_RELA - SECTION_COUNT] = {".rela.dyn", SHT_RELA, SHF_ALLOC},
    [OUT_DYNAMIC - SECTION_COUNT] = {".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE},
    [OUT_VERSYM - SECTION_COUNT] = {".gnu.version", SHT_GNU_versym, SHF_ALLOC},
    [OUT_VERNEED - SECTION_COUNT] = {".gnu.version_r", SHT_GNU_verneed, SHF_ALLOC},
    [OUT_EH_FRAME_HDR - SECTION_COUNT] = {".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC},
};

typedef struct { unsigned output; size_t offset; } Placement;
typedef struct {
    const char *path;
    Bytes file;
    Elf64_Ehdr header;
    Elf64_Shdr *sections;
    Placement *locations;
    bool *discarded;
    size_t section_names;
    size_t symbol_section, symbol_count;
} Object;

typedef struct { const char *name; uint32_t hash; uint16_t index; } Version;
typedef VECTOR(Version) Versions;
typedef struct { const char *name; Versions versions; Strings references; bool needed; } Library;
typedef struct Global Global;
struct Global {
    Global *next;
    const char *name;
    Object *object;
    Elf64_Sym symbol;
    Library *library;
    Elf64_Sym imported;
    Version version;
    size_t bss_offset;
    uint32_t dynamic_index;
    bool referenced, shared_reference, copy;
};

typedef struct { Object *object; Global *global; Elf64_Sym symbol; size_t index; const char *name; } LinkSymbol;
typedef struct { Bytes contents; size_t size, alignment, offset; uint64_t address; } OutputSection;
typedef enum { GOT_ADDRESS, GOT_TLS_INDEX, GOT_TLS_OFFSET, GOT_TLS_DESC } SlotKind;
typedef struct { LinkSymbol symbol; SlotKind kind; size_t offset, stub; } Slot;

struct Linker {
    Context *context;
    LinkOptions *options;
    uint16_t machine;
    uint64_t base;
    bool dynamic, text_relocations, static_tls;
    OutputSection sections[OUT_COUNT];
    VECTOR(Object *) objects;
    VECTOR(Global *) globals;
    VECTOR(Global *) dynamic_symbols;
    VECTOR(Library *) libraries;
    VECTOR(Slot) slots;
    VECTOR(Elf64_Rela) relas;
    VECTOR(Elf64_Rela) ifuncs;
    Strings comdats;
    Global **buckets;
    size_t bucket_count;
    size_t load_end, memory_end;
    size_t tls_resolver;
    uint32_t version_libraries;
};

enum { PAGE = 65536, BASE_ADDRESS = 0x400000 };

static const SectionKind *section_kind(unsigned id)
{
    return id < SECTION_COUNT ? &elf_sections[id] : &link_sections[id - SECTION_COUNT];
}

static bool within(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

static const char *table_string(Object *obj, size_t section, uint64_t index)
{
    const Elf64_Shdr *s = &obj->sections[section];
    if (index >= s->sh_size)
        return NULL;
    const char *name = (const char *)obj->file.data + s->sh_offset + index;
    return memchr(name, 0, (size_t)(s->sh_size - index)) ? name : NULL;
}

static bool load_elf(Linker *l, Object *obj, uint16_t type)
{
    Context *c = l->context;
    if (obj->file.len < sizeof(Elf64_Ehdr)) {
        error(c, (Loc){0}, "%s: truncated ELF file", obj->path);
        return false;
    }
    memcpy(&obj->header, obj->file.data, sizeof obj->header);
    const Elf64_Ehdr *h = &obj->header;
    size_t table_size = (size_t)h->e_shnum * sizeof(Elf64_Shdr);
    if (memcmp(h->e_ident, ELFMAG, SELFMAG) || h->e_ident[EI_CLASS] != ELFCLASS64 ||
        h->e_ident[EI_DATA] != ELFDATA2LSB || h->e_ident[EI_VERSION] != EV_CURRENT ||
        h->e_version != EV_CURRENT || h->e_type != type || h->e_ehsize != sizeof *h ||
        h->e_shentsize != sizeof(Elf64_Shdr) || !h->e_shnum ||
        !within(obj->file.len, h->e_shoff, table_size)) {
        error(c, (Loc){0}, "%s: expected a little-endian ELF64 %s", obj->path,
              type == ET_REL ? "relocatable object" : "shared library");
        return false;
    }
    if (h->e_machine != l->machine) {
        error(c, (Loc){0}, "%s: object machine does not match the selected target", obj->path);
        return false;
    }
    obj->sections = alloc(c, table_size);
    memcpy(obj->sections, obj->file.data + h->e_shoff, table_size);
    for (size_t i = 1; i < h->e_shnum; i++) {
        const Elf64_Shdr *s = &obj->sections[i];
        if (s->sh_type != SHT_NOBITS && !within(obj->file.len, s->sh_offset, s->sh_size)) {
            error(c, (Loc){0}, "%s: section %zu lies outside the object", obj->path, i);
            return false;
        }
        if (s->sh_type != (type == ET_REL ? SHT_SYMTAB : SHT_DYNSYM))
            continue;
        if (obj->symbol_section || s->sh_link >= h->e_shnum ||
            obj->sections[s->sh_link].sh_type != SHT_STRTAB ||
            s->sh_entsize != sizeof(Elf64_Sym) || s->sh_size % sizeof(Elf64_Sym)) {
            error(c, (Loc){0}, "%s: invalid ELF symbol table", obj->path);
            return false;
        }
        obj->symbol_section = i;
        obj->symbol_count = (size_t)(s->sh_size / sizeof(Elf64_Sym));
    }
    if (h->e_shstrndx != SHN_UNDEF) {
        if (h->e_shstrndx >= h->e_shnum || obj->sections[h->e_shstrndx].sh_type != SHT_STRTAB) {
            error(c, (Loc){0}, "%s: invalid ELF section name table", obj->path);
            return false;
        }
        obj->section_names = h->e_shstrndx;
    }
    return true;
}

static bool read_symbol(Linker *l, Object *obj, size_t index, Elf64_Sym *s, const char **name)
{
    if (index >= obj->symbol_count) {
        error(l->context, (Loc){0}, "%s: invalid symbol index %zu", obj->path, index);
        return false;
    }
    const Elf64_Shdr *table = &obj->sections[obj->symbol_section];
    memcpy(s, obj->file.data + table->sh_offset + index * sizeof *s, sizeof *s);
    *name = table_string(obj, table->sh_link, s->st_name);
    if (!*name) {
        error(l->context, (Loc){0}, "%s: invalid symbol name", obj->path);
        return false;
    }
    return true;
}

static uint32_t symbol_hash(const char *name)
{
    uint32_t hash = 0;
    for (; *name; name++) {
        hash = (hash << 4) + (unsigned char)*name;
        hash ^= (hash & 0xf0000000u) >> 24;
        hash &= 0x0fffffffu;
    }
    return hash;
}

static Global *global_symbol(Linker *l, const char *name, bool create)
{
    uint32_t hash = symbol_hash(name);
    for (Global *g = l->buckets[hash % l->bucket_count]; g; g = g->next)
        if (equal(g->name, name))
            return g;
    if (!create)
        return NULL;
    if (l->globals.len >= l->bucket_count) {
        l->bucket_count *= 2;
        l->buckets = alloc(l->context, l->bucket_count * sizeof *l->buckets);
        for (size_t i = 0; i < l->globals.len; i++) {
            Global *g = l->globals.data[i];
            size_t bucket = symbol_hash(g->name) % l->bucket_count;
            g->next = l->buckets[bucket];
            l->buckets[bucket] = g;
        }
    }
    Global *g = alloc(l->context, sizeof *g);
    size_t bucket = hash % l->bucket_count;
    g->name = name;
    g->next = l->buckets[bucket];
    l->buckets[bucket] = g;
    PUSH(l->context, l->globals, g);
    return g;
}

bool ld_needs(Linker *l, const char *name)
{
    Global *g = global_symbol(l, name, false);
    return g && g->referenced && !g->object && !g->library;
}

static void discard_comdats(Linker *l, Object *obj)
{
    Context *c = l->context;
    for (size_t i = 1; i < obj->header.e_shnum && !c->failed; i++) {
        Elf64_Shdr *s = &obj->sections[i];
        if (s->sh_type != SHT_GROUP)
            continue;
        Elf64_Sym symbol;
        const char *name;
        if (s->sh_size < 4 || s->sh_size % 4 || s->sh_link != obj->symbol_section ||
            !read_symbol(l, obj, s->sh_info, &symbol, &name)) {
            error(c, (Loc){0}, "%s: invalid ELF section group", obj->path);
            return;
        }
        const unsigned char *data = obj->file.data + s->sh_offset;
        uint32_t flags;
        memcpy(&flags, data, sizeof flags);
        if (!(flags & GRP_COMDAT))
            continue;
        bool known = false;
        for (size_t j = 0; j < l->comdats.len && !known; j++)
            known = equal(name, l->comdats.data[j]);
        if (!known) {
            PUSH(c, l->comdats, name);
            continue;
        }
        for (size_t j = 4; j < s->sh_size; j += 4) {
            uint32_t member;
            memcpy(&member, data + j, sizeof member);
            if (!member || member >= obj->header.e_shnum) {
                error(c, (Loc){0}, "%s: invalid ELF section group member", obj->path);
                return;
            }
            obj->discarded[member] = true;
        }
    }
}

static void collect_definitions(Linker *l, Object *obj)
{
    for (size_t i = 1; i < obj->symbol_count && !l->context->failed; i++) {
        Elf64_Sym s;
        const char *name;
        if (!read_symbol(l, obj, i, &s, &name))
            return;
        unsigned binding = ELF64_ST_BIND(s.st_info);
        if (binding != STB_GLOBAL && binding != STB_WEAK)
            continue;
        if (s.st_shndx != SHN_ABS && s.st_shndx != SHN_COMMON && s.st_shndx >= obj->header.e_shnum) {
            error(l->context, (Loc){0}, "%s: invalid section for symbol '%s'", obj->path, name);
            return;
        }
        if (s.st_shndx < obj->header.e_shnum && obj->discarded[s.st_shndx])
            continue;
        Global *g = global_symbol(l, name, true);
        if (s.st_shndx == SHN_UNDEF) {
            g->referenced |= binding == STB_GLOBAL;
        } else if (s.st_shndx == SHN_COMMON && g->object && g->symbol.st_shndx == SHN_COMMON) {
            if (s.st_size > g->symbol.st_size)
                g->symbol.st_size = s.st_size;
            if (s.st_value > g->symbol.st_value)
                g->symbol.st_value = s.st_value;
        } else if (s.st_shndx == SHN_COMMON && g->object && ELF64_ST_BIND(g->symbol.st_info) == STB_GLOBAL) {
            continue;
        } else if (!g->object || (binding == STB_GLOBAL &&
                   (ELF64_ST_BIND(g->symbol.st_info) == STB_WEAK || g->symbol.st_shndx == SHN_COMMON))) {
            g->object = obj;
            g->symbol = s;
        } else if (binding == STB_GLOBAL) {
            error(l->context, (Loc){0}, "%s: multiple definitions of '%s' (also in %s)",
                  obj->path, name, g->object->path);
        }
    }
}

void ld_take_object(Linker *l, const char *path, Bytes file)
{
    Context *c = l->context;
    Object *obj = alloc(c, sizeof *obj);
    obj->path = path;
    obj->file = file;
    if (!load_elf(l, obj, ET_REL))
        return;
    obj->locations = alloc(c, (size_t)obj->header.e_shnum * sizeof *obj->locations);
    obj->discarded = alloc(c, (size_t)obj->header.e_shnum * sizeof *obj->discarded);
    discard_comdats(l, obj);
    collect_definitions(l, obj);
    PUSH(c, l->objects, obj);
}

static bool read_versions(Linker *l, Object *obj, const Elf64_Shdr *table, Versions *versions)
{
    size_t offset = 0;
    while (offset < table->sh_size) {
        Elf64_Verdef def;
        Elf64_Verdaux aux;
        if (!within((size_t)table->sh_size, offset, sizeof def) ||
            table->sh_link >= obj->header.e_shnum || obj->sections[table->sh_link].sh_type != SHT_STRTAB)
            return false;
        const unsigned char *data = obj->file.data + table->sh_offset + offset;
        memcpy(&def, data, sizeof def);
        if (def.vd_version != VER_DEF_CURRENT ||
            !within((size_t)table->sh_size - offset, def.vd_aux, sizeof aux))
            return false;
        memcpy(&aux, data + def.vd_aux, sizeof aux);
        const char *name = table_string(obj, table->sh_link, aux.vda_name);
        if (!name)
            return false;
        PUSH(l->context, *versions, ((Version){name, def.vd_hash, def.vd_ndx}));
        if (!def.vd_next)
            return true;
        if (def.vd_next < sizeof def || def.vd_next >= table->sh_size - offset)
            return false;
        offset += def.vd_next;
    }
    return false;
}

void ld_take_shared(Linker *l, const char *path, const char *link_name, Bytes file, bool as_needed)
{
    Context *c = l->context;
    if (!l->dynamic) {
        error(c, (Loc){0}, "%s: shared library in a static link", path);
        return;
    }
    Object obj = {.path = path, .file = file};
    if (!load_elf(l, &obj, ET_DYN))
        return;
    const char *name = link_name;
    const Elf64_Shdr *versions = NULL;
    Versions definitions = {0};
    for (size_t i = 1; i < obj.header.e_shnum; i++) {
        const Elf64_Shdr *s = &obj.sections[i];
        if (s->sh_type == SHT_GNU_versym && s->sh_link == obj.symbol_section && s->sh_size / 2 >= obj.symbol_count)
            versions = s;
        if (s->sh_type == SHT_GNU_verdef && !read_versions(l, &obj, s, &definitions)) {
            error(c, (Loc){0}, "%s: invalid ELF version definitions", path);
            return;
        }
        if (s->sh_type != SHT_DYNAMIC)
            continue;
        if (s->sh_link >= obj.header.e_shnum || obj.sections[s->sh_link].sh_type != SHT_STRTAB ||
            s->sh_size % sizeof(Elf64_Dyn)) {
            error(c, (Loc){0}, "%s: invalid ELF dynamic table", path);
            return;
        }
        for (size_t j = 0; j < s->sh_size; j += sizeof(Elf64_Dyn)) {
            Elf64_Dyn entry;
            memcpy(&entry, file.data + s->sh_offset + j, sizeof entry);
            if (entry.d_tag == DT_NULL)
                break;
            if (entry.d_tag == DT_SONAME) {
                name = table_string(&obj, s->sh_link, entry.d_un.d_val);
                if (!name) {
                    error(c, (Loc){0}, "%s: invalid ELF SONAME", path);
                    return;
                }
            }
        }
    }
    for (size_t i = 0; i < l->libraries.len; i++) {
        Library *lib = l->libraries.data[i];
        if (equal(lib->name, name)) {
            lib->needed |= !as_needed;
            return;
        }
    }
    Library *lib = alloc(c, sizeof *lib);
    *lib = (Library){.name = name, .needed = !as_needed};
    PUSH(c, l->libraries, lib);
    for (size_t i = 1; i < obj.symbol_count && !c->failed; i++) {
        Elf64_Sym s;
        const char *symbol;
        if (!read_symbol(l, &obj, i, &s, &symbol))
            return;
        unsigned binding = ELF64_ST_BIND(s.st_info), visibility = ELF64_ST_VISIBILITY(s.st_other);
        uint16_t version = 0;
        if (versions)
            memcpy(&version, file.data + versions->sh_offset + i * sizeof version, sizeof version);
        if (binding == STB_LOCAL || visibility == STV_HIDDEN || visibility == STV_INTERNAL || !*symbol)
            continue;
        if (s.st_shndx == SHN_UNDEF) {
            PUSH(c, lib->references, symbol);
            continue;
        }
        if (version & 0x8000)
            continue;
        Global *g = global_symbol(l, symbol, true);
        if (!g->library) {
            g->library = lib;
            g->imported = s;
            if (version > VER_NDX_GLOBAL) {
                size_t j = 0;
                while (j < definitions.len && definitions.data[j].index != version)
                    j++;
                if (j == definitions.len) {
                    error(c, (Loc){0}, "%s: undefined version index for '%s'", path, symbol);
                    return;
                }
                g->version = definitions.data[j];
            }
        }
    }
}

static size_t frame_records(const unsigned char *data, size_t size)
{
    size_t offset = 0;
    while (size - offset >= 4) {
        uint32_t length;
        memcpy(&length, data + offset, sizeof length);
        if (!length)
            return offset;
        unsigned header = 4;
        uint64_t content = length;
        if (length == UINT32_MAX) {
            header = 12;
            if (size - offset < header)
                break;
            memcpy(&content, data + offset + 4, sizeof content);
        }
        if (content > size - offset - header)
            break;
        offset += header + (size_t)content;
    }
    return size;
}

static void place_sections(Linker *l, Object *obj)
{
    for (size_t i = 1; i < obj->header.e_shnum && !l->context->failed; i++) {
        const Elf64_Shdr *s = &obj->sections[i];
        if (!(s->sh_flags & SHF_ALLOC) || obj->discarded[i])
            continue;
        bool array = s->sh_type == SHT_PREINIT_ARRAY || s->sh_type == SHT_INIT_ARRAY ||
                     s->sh_type == SHT_FINI_ARRAY;
        if (((s->sh_flags & SHF_WRITE) && (s->sh_flags & SHF_EXECINSTR)) ||
            (!array && s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS && s->sh_type != SHT_NOTE &&
             !(s->sh_type == SHT_X86_64_UNWIND && l->machine == EM_X86_64))) {
            error(l->context, (Loc){0}, "%s: unsupported allocated section %zu", obj->path, i);
            return;
        }
        if (s->sh_type == SHT_NOBITS && (!(s->sh_flags & SHF_WRITE) || (s->sh_flags & SHF_EXECINSTR))) {
            error(l->context, (Loc){0}, "%s: unsupported zero-fill section %zu", obj->path, i);
            return;
        }
        unsigned id = s->sh_type == SHT_NOBITS ? SECTION_BSS : s->sh_flags & SHF_EXECINSTR ? SECTION_TEXT :
                      s->sh_flags & SHF_WRITE ? SECTION_DATA : SECTION_RODATA;
        const char *name = obj->section_names ? table_string(obj, obj->section_names, s->sh_name) : "";
        if (!name) {
            error(l->context, (Loc){0}, "%s: invalid section name", obj->path);
            return;
        }
        if (s->sh_flags & SHF_TLS)
            id = s->sh_type == SHT_NOBITS ? OUT_TBSS : OUT_TDATA;
        else if (s->sh_type == SHT_PREINIT_ARRAY)
            id = OUT_PREINIT_ARRAY;
        else if (s->sh_type == SHT_INIT_ARRAY)
            id = OUT_INIT_ARRAY;
        else if (s->sh_type == SHT_FINI_ARRAY)
            id = OUT_FINI_ARRAY;
        else if (equal(name, ".init"))
            id = OUT_INIT;
        else if (equal(name, ".fini"))
            id = OUT_FINI;
        else if (equal(name, ".eh_frame"))
            id = OUT_EH_FRAME;
        OutputSection *out = &l->sections[id];
        uint64_t alignment = s->sh_addralign ? s->sh_addralign : 1;
        if (alignment > PAGE || (alignment & (alignment - 1)) || out->size > (size_t)PTRDIFF_MAX - PAGE ||
            s->sh_size > (uint64_t)PTRDIFF_MAX - PAGE - out->size) {
            error(l->context, (Loc){0}, "%s: unsupported section size or alignment", obj->path);
            return;
        }
        size_t offset = id == OUT_EH_FRAME ? out->size : align_to(out->size, (size_t)alignment);
        size_t size = (size_t)s->sh_size;
        if (s->sh_type != SHT_NOBITS) {
            const unsigned char *data = obj->file.data + s->sh_offset;
            if (id == OUT_EH_FRAME)
                size = frame_records(data, size);
            else
                bytes_align(l->context, &out->contents, (size_t)alignment);
            bytes_add(l->context, &out->contents, data, size);
        }
        out->size = offset + size;
        if (alignment > out->alignment)
            out->alignment = (size_t)alignment;
        obj->locations[i] = (Placement){id, offset};
    }
}

static bool symbol_address(Linker *l, Object *obj, Elf64_Sym s, const char *name, uint64_t *value)
{
    if (s.st_shndx == SHN_ABS) {
        *value = s.st_value;
        return true;
    }
    if (s.st_shndx >= obj->header.e_shnum ||
        !obj->locations[s.st_shndx].output || s.st_value > obj->sections[s.st_shndx].sh_size) {
        error(l->context, (Loc){0}, "%s: symbol '%s' is outside allocated sections", obj->path, name);
        return false;
    }
    Placement p = obj->locations[s.st_shndx];
    *value = l->sections[p.output].address + p.offset + s.st_value;
    return true;
}

static bool lookup(Linker *l, Object *obj, size_t index, LinkSymbol *result)
{
    if (!index) {
        *result = (LinkSymbol){.symbol = {.st_shndx = SHN_ABS}, .name = ""};
        return true;
    }
    Elf64_Sym s;
    const char *name;
    if (!read_symbol(l, obj, index, &s, &name))
        return false;
    Global *g = ELF64_ST_BIND(s.st_info) == STB_LOCAL ? NULL : global_symbol(l, name, true);
    if (g && g->object) {
        obj = g->object;
        s = g->symbol;
    }
    *result = (LinkSymbol){.object = obj, .global = g, .symbol = s, .index = index, .name = name};
    return true;
}

static bool synthetic(Linker *l, const char *name, uint64_t *value)
{
    if (!l->dynamic && equal(name, "_DYNAMIC"))
        return false;
    if (equal(name, "__ehdr_start")) {
        *value = l->base;
        return true;
    }
    if (equal(name, "__tls_get_addr") && l->tls_resolver && !l->dynamic && l->machine == EM_X86_64) {
        *value = l->sections[OUT_PLT].address + l->tls_resolver - 1;
        return true;
    }
    const struct { const char *name; unsigned section; bool end; } symbols[] = {
        {"_GLOBAL_OFFSET_TABLE_", OUT_GOT, false}, {"_DYNAMIC", OUT_DYNAMIC, false},
        {"__preinit_array_start", OUT_PREINIT_ARRAY, false}, {"__preinit_array_end", OUT_PREINIT_ARRAY, true},
        {"__init_array_start", OUT_INIT_ARRAY, false}, {"__init_array_end", OUT_INIT_ARRAY, true},
        {"__fini_array_start", OUT_FINI_ARRAY, false}, {"__fini_array_end", OUT_FINI_ARRAY, true},
        {"__rela_iplt_start", OUT_IPLT_RELA, false}, {"__rela_iplt_end", OUT_IPLT_RELA, true},
        {"__bss_start", SECTION_BSS, false}, {"_end", SECTION_BSS, true}, {"end", SECTION_BSS, true},
        {"_edata", SECTION_DATA, true}, {"edata", SECTION_DATA, true},
        {"_etext", OUT_PLT, true}, {"etext", OUT_PLT, true},
    };
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++) {
        if (equal(name, symbols[i].name)) {
            OutputSection *s = &l->sections[symbols[i].section];
            *value = s->address + (symbols[i].end ? s->size : 0);
            return true;
        }
    }
    return false;
}

static bool imported(Linker *l, LinkSymbol s)
{
    uint64_t unused;
    return s.symbol.st_shndx == SHN_UNDEF && s.global && !s.global->copy &&
           l->dynamic && !synthetic(l, s.name, &unused) &&
           (s.global->library || l->options->shared || ELF64_ST_BIND(s.symbol.st_info) == STB_WEAK) &&
           ELF64_ST_VISIBILITY(s.symbol.st_other) == STV_DEFAULT;
}

static bool preemptible(Linker *l, LinkSymbol s)
{
    return imported(l, s) ||
           (l->options->shared && s.global && s.symbol.st_shndx != SHN_UNDEF &&
            ELF64_ST_VISIBILITY(s.symbol.st_other) == STV_DEFAULT);
}

static bool resolve(Linker *l, LinkSymbol s, uint64_t *value)
{
    if (s.global && (s.global->copy || s.symbol.st_shndx == SHN_COMMON)) {
        *value = l->sections[SECTION_BSS].address + s.global->bss_offset;
        return true;
    }
    if (s.symbol.st_shndx != SHN_UNDEF)
        return symbol_address(l, s.object, s.symbol, s.name, value);
    if (synthetic(l, s.name, value))
        return true;
    if (ELF64_ST_BIND(s.symbol.st_info) == STB_WEAK) {
        *value = 0;
        return true;
    }
    error(l->context, (Loc){0}, "%s: undefined symbol '%s'", s.object->path, s.name);
    return false;
}

static bool load_relative(Linker *l, LinkSymbol s)
{
    uint64_t unused;
    return (l->options->pie || l->options->shared) && s.symbol.st_shndx != SHN_ABS &&
           (s.symbol.st_shndx != SHN_UNDEF || (s.global && s.global->copy) || synthetic(l, s.name, &unused));
}

static bool got_relocation(uint16_t machine, uint32_t type, SlotKind *kind)
{
    *kind = GOT_ADDRESS;
    if (machine == EM_AARCH64) {
        if (type >= R_AARCH64_TLSDESC_LD_PREL19 && type <= R_AARCH64_TLSDESC_ADD_LO12) {
            *kind = GOT_TLS_DESC;
            return true;
        }
        if (type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 || type == R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC ||
            type == R_AARCH64_TLSIE_LD_GOTTPREL_PREL19) {
            *kind = GOT_TLS_OFFSET;
            return true;
        }
        return type == R_AARCH64_ADR_GOT_PAGE || type == R_AARCH64_LD64_GOT_LO12_NC ||
               type == R_AARCH64_GOT_LD_PREL19;
    }
    if (type == R_X86_64_TLSGD) {
        *kind = GOT_TLS_INDEX;
        return true;
    }
    if (type == R_X86_64_GOTTPOFF) {
        *kind = GOT_TLS_OFFSET;
        return true;
    }
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX ||
           type == R_X86_64_GOTPCREL64 || type == R_X86_64_GOT64;
}

static bool relocation_section(Linker *l, Object *obj, const Elf64_Shdr *s)
{
    if (s->sh_info >= obj->header.e_shnum ||
        (s->sh_type != SHT_RELA && obj->locations[s->sh_info].output) ||
        (s->sh_type == SHT_RELA && (s->sh_link != obj->symbol_section || !obj->symbol_section ||
                                  s->sh_entsize != sizeof(Elf64_Rela) || s->sh_size % sizeof(Elf64_Rela)))) {
        error(l->context, (Loc){0}, "%s: invalid or unsupported relocation table", obj->path);
        return false;
    }
    return true;
}

static bool branch_relocation(uint16_t machine, uint32_t type)
{
    return machine == EM_AARCH64 ? type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26 :
           type == R_X86_64_PLT32 || type == R_X86_64_PLTOFF64;
}

static bool direct_data_relocation(uint16_t machine, uint32_t type)
{
    if (machine == EM_X86_64)
        return type == R_X86_64_PC32 || type == R_X86_64_PC64 || type == R_X86_64_32 ||
               type == R_X86_64_32S || type == R_X86_64_GOTOFF64;
    return type == R_AARCH64_PREL32 || type == R_AARCH64_PREL64 || type == R_AARCH64_ABS32 ||
           type == R_AARCH64_ADR_PREL_LO21 || type == R_AARCH64_ADR_PREL_PG_HI21 ||
           type == R_AARCH64_ADR_PREL_PG_HI21_NC || type == R_AARCH64_ADD_ABS_LO12_NC ||
           (type >= R_AARCH64_MOVW_UABS_G0 && type <= R_AARCH64_MOVW_UABS_G3);
}

static void allocate_bss(Linker *l, Global *g, uint64_t size, uint64_t alignment)
{
    OutputSection *bss = &l->sections[SECTION_BSS];
    if (!alignment || alignment > PAGE || (alignment & (alignment - 1)) ||
        bss->size > (size_t)PTRDIFF_MAX - PAGE || size > (uint64_t)PTRDIFF_MAX - PAGE - bss->size) {
        error(l->context, (Loc){0}, "invalid BSS size or alignment for '%s'", g->name);
        return;
    }
    g->bss_offset = align_to(bss->size, (size_t)alignment);
    bss->size = g->bss_offset + (size_t)size;
    if (alignment > bss->alignment)
        bss->alignment = (size_t)alignment;
}

static void reserve_copy(Linker *l, LinkSymbol s)
{
    Global *g = s.global;
    uint64_t address = g->imported.st_value;
    uint64_t alignment = address & (0 - address);
    if (alignment > PAGE)
        alignment = PAGE;
    g->copy = true;
    allocate_bss(l, g, g->imported.st_size, alignment ? alignment : 1);
}

static Slot *find_slot(Linker *l, LinkSymbol s, SlotKind kind)
{
    for (size_t i = 0; i < l->slots.len; i++) {
        Slot *slot = &l->slots.data[i];
        LinkSymbol other = slot->symbol;
        if (slot->kind == kind && s.global == other.global &&
            (s.global || (s.object == other.object && s.index == other.index)))
            return slot;
    }
    return NULL;
}

static void reserve(OutputSection *s, Context *c, size_t size, size_t alignment)
{
    bytes_align(c, &s->contents, alignment);
    bytes_add(c, &s->contents, NULL, size);
    s->size = s->contents.len;
    if (alignment > s->alignment)
        s->alignment = alignment;
}

static Slot *reserve_slot(Linker *l, LinkSymbol symbol, SlotKind kind, bool stub)
{
    Slot *slot = find_slot(l, symbol, kind);
    if (!slot) {
        OutputSection *got = &l->sections[OUT_GOT];
        size_t offset = got->size;
        reserve(got, l->context, kind == GOT_TLS_INDEX || kind == GOT_TLS_DESC ? 16 : 8, 8);
        PUSH(l->context, l->slots, ((Slot){.symbol = symbol, .kind = kind, .offset = offset}));
        slot = &l->slots.data[l->slots.len - 1];
        if (!l->dynamic && ELF64_ST_TYPE(symbol.symbol.st_info) == STT_GNU_IFUNC)
            reserve(&l->sections[OUT_IPLT_RELA], l->context, sizeof(Elf64_Rela), 8);
    }
    if (stub && !slot->stub) {
        OutputSection *plt = &l->sections[OUT_PLT];
        slot->stub = plt->size + 1;
        reserve(plt, l->context, l->machine == EM_AARCH64 ? 12 : 8, 4);
    }
    return slot;
}

static void prepare_relocations(Linker *l, Object *obj)
{
    for (size_t j = 1; j < obj->header.e_shnum && !l->context->failed; j++) {
        const Elf64_Shdr *s = &obj->sections[j];
        if (obj->discarded[j] || (s->sh_type != SHT_RELA && s->sh_type != SHT_REL))
            continue;
        if (!relocation_section(l, obj, s))
            return;
        if (!obj->locations[s->sh_info].output)
            continue;
        for (size_t offset = 0; offset < s->sh_size; offset += sizeof(Elf64_Rela)) {
            Elf64_Rela rel;
            memcpy(&rel, obj->file.data + s->sh_offset + offset, sizeof rel);
            uint32_t type = ELF64_R_TYPE(rel.r_info);
            SlotKind kind;
            bool got = got_relocation(l->machine, type, &kind);
            LinkSymbol symbol;
            if (!lookup(l, obj, ELF64_R_SYM(rel.r_info), &symbol))
                return;
            if (l->dynamic && !l->options->shared && direct_data_relocation(l->machine, type) &&
                imported(l, symbol) && symbol.global->library &&
                ELF64_ST_TYPE(symbol.global->imported.st_info) == STT_OBJECT) {
                reserve_copy(l, symbol);
            }
            bool stub = !got && (ELF64_ST_TYPE(symbol.symbol.st_info) == STT_GNU_IFUNC ||
                                (branch_relocation(l->machine, type) && preemptible(l, symbol)));
            if (got || stub)
                reserve_slot(l, symbol, kind, stub);
            if (!l->dynamic && !l->tls_resolver && (kind == GOT_TLS_DESC || kind == GOT_TLS_INDEX)) {
                OutputSection *plt = &l->sections[OUT_PLT];
                l->tls_resolver = plt->size + 1;
                reserve(plt, l->context, l->machine == EM_AARCH64 ? 8 : 16, 4);
            }
        }
    }
}

static unsigned reloc_type(Linker *l, unsigned arm, unsigned x86)
{
    return l->machine == EM_AARCH64 ? arm : x86;
}

static uint32_t dynamic_symbol(Linker *l, LinkSymbol s)
{
    Global *g = s.global;
    if (!g)
        return 0;
    if (!g->dynamic_index) {
        PUSH(l->context, l->dynamic_symbols, g);
        g->dynamic_index = (uint32_t)l->dynamic_symbols.len;
    }
    if (!g->object && g->library)
        g->library->needed = true;
    return g->dynamic_index;
}

static void dynamic_relocation(Linker *l, uint64_t address, unsigned type, unsigned symbol, uint64_t addend)
{
    PUSH(l->context, l->relas, ((Elf64_Rela){address, ELF64_R_INFO(symbol, type), (int64_t)addend}));
}

static size_t tls_alignment(Linker *l)
{
    size_t initialized = l->sections[OUT_TDATA].alignment, zero = l->sections[OUT_TBSS].alignment;
    return initialized > zero ? initialized : zero;
}

static size_t tls_size(Linker *l)
{
    OutputSection *data = &l->sections[OUT_TDATA], *bss = &l->sections[OUT_TBSS];
    return bss->offset + bss->size - data->offset;
}

static uint64_t thread_offset(Linker *l, uint64_t module_offset)
{
    size_t alignment = tls_alignment(l);
    uint64_t adjustment = l->machine == EM_AARCH64 ? align_to(16, alignment) : 0 - align_to(tls_size(l), alignment);
    return module_offset + adjustment;
}

static bool write_slots(Linker *l)
{
    OutputSection *got = &l->sections[OUT_GOT], *plt = &l->sections[OUT_PLT];
    bool arm = l->machine == EM_AARCH64;
    uint64_t resolver = l->tls_resolver ? plt->address + l->tls_resolver - 1 : 0;
    for (size_t i = 0; i < l->globals.len; i++) {
        Global *g = l->globals.data[i];
        if (g->copy) {
            uint64_t address = l->sections[SECTION_BSS].address + g->bss_offset;
            dynamic_relocation(l, address, reloc_type(l, R_AARCH64_COPY, R_X86_64_COPY),
                               dynamic_symbol(l, (LinkSymbol){.global = g}), 0);
        }
    }
    if (resolver) {
        const uint32_t arm_code[] = {0xf9400400, 0xd65f03c0}; /* ldr x0, [x0, #8]; ret */
        const unsigned char x86_code[] = {
            0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0, /* mov %fs:0, %rax */
            0x48, 0x03, 0x47, 0x08, 0xc3,              /* add 8(%rdi), %rax; ret */
        };
        unsigned char *out = plt->contents.data + l->tls_resolver - 1;
        if (arm)
            memcpy(out, arm_code, sizeof arm_code);
        else
            memcpy(out, x86_code, sizeof x86_code);
    }
    for (size_t i = 0; i < l->slots.len; i++) {
        Slot *slot = &l->slots.data[i];
        LinkSymbol s = slot->symbol;
        uint64_t address = got->address + slot->offset, values[2] = {0};
        bool external = preemptible(l, s);
        bool ifunc = ELF64_ST_TYPE(s.symbol.st_info) == STT_GNU_IFUNC;
        bool tls = slot->kind != GOT_ADDRESS;
        uint64_t value = 0;
        if (!external && !resolve(l, s, &value))
            return false;
        unsigned index = external ? dynamic_symbol(l, s) : 0;
        if (tls && !external && s.symbol.st_shndx != SHN_UNDEF)
            value -= l->sections[OUT_TDATA].address;
        if (slot->kind == GOT_ADDRESS) {
            if (external) {
                dynamic_relocation(l, address, reloc_type(l, R_AARCH64_GLOB_DAT, R_X86_64_GLOB_DAT), index, 0);
            } else if (ifunc) {
                unsigned type = reloc_type(l, R_AARCH64_IRELATIVE, R_X86_64_IRELATIVE);
                PUSH(l->context, l->ifuncs, ((Elf64_Rela){address, ELF64_R_INFO(0, type), (int64_t)value}));
            } else if (load_relative(l, s)) {
                dynamic_relocation(l, address, reloc_type(l, R_AARCH64_RELATIVE, R_X86_64_RELATIVE), 0, value);
            } else {
                values[0] = value;
            }
        } else if (slot->kind == GOT_TLS_DESC) {
            if (l->dynamic) {
                dynamic_relocation(l, address, R_AARCH64_TLSDESC, index, value);
            } else {
                values[0] = resolver;
                values[1] = thread_offset(l, value);
            }
        } else if (slot->kind == GOT_TLS_INDEX) {
            if (l->dynamic) {
                dynamic_relocation(l, address, R_X86_64_DTPMOD64, index, 0);
                if (external)
                    dynamic_relocation(l, address + 8, R_X86_64_DTPOFF64, index, 0);
                else
                    values[1] = value;
            } else {
                values[0] = 1;
                values[1] = thread_offset(l, value);
            }
        } else if (l->dynamic && (external || l->options->shared)) {
            dynamic_relocation(l, address, reloc_type(l, R_AARCH64_TLS_TPREL, R_X86_64_TPOFF64), index, value);
            l->static_tls = true;
        } else {
            values[0] = thread_offset(l, value);
        }
        size_t width = slot->kind == GOT_TLS_DESC || slot->kind == GOT_TLS_INDEX ? 16 : 8;
        memcpy(got->contents.data + slot->offset, values, width);
        if (!slot->stub)
            continue;
        uint64_t place = plt->address + slot->stub - 1;
        unsigned char *code = plt->contents.data + slot->stub - 1;
        PatchResult status;
        if (arm) {
            const uint32_t instructions[] = {0x90000010, 0xf9400211, 0xd61f0220};
            memcpy(code, instructions, sizeof instructions);
            status = ld_patch(l->machine, code, 4, R_AARCH64_ADR_GOT_PAGE, address, place);
            if (status == PATCH_OK)
                status = ld_patch(l->machine, code + 4, 4, R_AARCH64_LD64_GOT_LO12_NC, address, place + 4);
        } else {
            const unsigned char instructions[] = {0xff, 0x25, 0, 0, 0, 0, 0x66, 0x90};
            memcpy(code, instructions, sizeof instructions);
            status = ld_patch(l->machine, code + 2, 4, R_X86_64_PC32, address - 4, place + 2);
        }
        if (status != PATCH_OK) {
            error(l->context, (Loc){0}, "GOT entry for '%s' is out of PLT range", s.name);
            return false;
        }
    }
    return !l->context->failed;
}

static void apply_relocations(Linker *l, Object *obj)
{
    for (size_t i = 1; i < obj->header.e_shnum && !l->context->failed; i++) {
        const Elf64_Shdr *s = &obj->sections[i];
        if (s->sh_type != SHT_RELA || obj->discarded[i])
            continue;
        Placement p = obj->locations[s->sh_info];
        if (!p.output)
            continue;
        OutputSection *target = &l->sections[p.output];
        uint64_t section_size = obj->sections[s->sh_info].sh_size;
        for (size_t offset = 0; offset < s->sh_size; offset += sizeof(Elf64_Rela)) {
            Elf64_Rela rel;
            memcpy(&rel, obj->file.data + s->sh_offset + offset, sizeof rel);
            uint32_t type = (uint32_t)ELF64_R_TYPE(rel.r_info);
            if (!type || (l->machine == EM_AARCH64 && type >= R_AARCH64_TLSDESC_LDR && type <= R_AARCH64_TLSDESC_CALL))
                continue;
            if (p.output == SECTION_BSS || p.output == OUT_TBSS || rel.r_offset >= section_size) {
                error(l->context, (Loc){0}, "%s: relocation offset is outside initialized sections", obj->path);
                return;
            }
            LinkSymbol sref;
            if (!lookup(l, obj, ELF64_R_SYM(rel.r_info), &sref))
                return;
            SlotKind kind;
            bool got = got_relocation(l->machine, type, &kind);
            bool branch = branch_relocation(l->machine, type) ||
                          ELF64_ST_TYPE(sref.symbol.st_info) == STT_GNU_IFUNC;
            Slot *slot = got || branch ? find_slot(l, sref, kind) : NULL;
            size_t at = p.offset + (size_t)rel.r_offset;
            uint64_t place = target->address + at;
            size_t available = (size_t)(section_size - rel.r_offset);
            unsigned char *out = target->contents.data + at;
            uint64_t symbol = 0;
            unsigned abs64 = reloc_type(l, R_AARCH64_ABS64, R_X86_64_64);
            if (got && slot) {
                if (l->machine == EM_AARCH64 && rel.r_addend) {
                    error(l->context, (Loc){0}, "%s: nonzero addend in AArch64 GOT relocation", obj->path);
                    return;
                }
                symbol = l->sections[OUT_GOT].address + slot->offset;
            } else if (branch && slot && slot->stub) {
                symbol = l->sections[OUT_PLT].address + slot->stub - 1;
            } else if (type == abs64 && available >= 8 && preemptible(l, sref)) {
                dynamic_relocation(l, place, abs64, dynamic_symbol(l, sref), (uint64_t)rel.r_addend);
                l->text_relocations |= !(section_kind(p.output)->flags & SHF_WRITE);
                continue;
            } else if (!resolve(l, sref, &symbol)) {
                return;
            }
            uint64_t value = symbol + (uint64_t)rel.r_addend;
            if (type == abs64 && available >= 8 && load_relative(l, sref)) {
                dynamic_relocation(l, place, reloc_type(l, R_AARCH64_RELATIVE, R_X86_64_RELATIVE), 0, value);
                l->text_relocations |= !(section_kind(p.output)->flags & SHF_WRITE);
            }
            if (l->machine == EM_X86_64) {
                if (type == R_X86_64_TPOFF32 || type == R_X86_64_TPOFF64) {
                    if (sref.symbol.st_shndx != SHN_UNDEF)
                        value = thread_offset(l, value - l->sections[OUT_TDATA].address);
                } else if (type == R_X86_64_DTPOFF32 || type == R_X86_64_DTPOFF64) {
                    if (sref.symbol.st_shndx != SHN_UNDEF)
                        value -= l->sections[OUT_TDATA].address;
                } else if (type == R_X86_64_GOTOFF64 || type == R_X86_64_PLTOFF64 || type == R_X86_64_GOT64) {
                    value -= l->sections[OUT_GOT].address;
                }
            } else if ((type >= R_AARCH64_TLSLE_MOVW_TPREL_G2 && type <= R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC) ||
                       type == R_AARCH64_TLSLE_LDST128_TPREL_LO12 || type == R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC) {
                if (sref.symbol.st_shndx != SHN_UNDEF)
                    value = thread_offset(l, value - l->sections[OUT_TDATA].address);
            }
            PatchResult status = ld_patch(l->machine, out, available, type, value, place);
            if (status != PATCH_OK) {
                const char *reason = status == PATCH_UNSUPPORTED ? "unsupported" :
                                     status == PATCH_RANGE ? "out of range" : "truncated";
                error(l->context, (Loc){0}, "%s: ELF relocation %u at offset %#" PRIx64 " is %s",
                      obj->path, type, rel.r_offset, reason);
                return;
            }
        }
    }
}

static Elf64_Phdr segment(unsigned type, size_t offset, uint64_t address,
                           size_t file_size, size_t memory_size, unsigned flags, size_t alignment)
{
    return (Elf64_Phdr){.p_type = type, .p_flags = flags, .p_offset = offset,
                        .p_vaddr = address, .p_paddr = address, .p_filesz = file_size,
                        .p_memsz = memory_size, .p_align = alignment};
}

static Elf64_Phdr section_segment(Linker *l, unsigned type, unsigned id, size_t memory_size, unsigned flags)
{
    OutputSection *s = &l->sections[id];
    return segment(type, s->offset, s->address, s->size, memory_size, flags, s->alignment);
}

enum { MAX_PROGRAM_HEADERS = 12 };

static size_t place_output(Linker *l, unsigned id, size_t offset, uint64_t bias)
{
    OutputSection *s = &l->sections[id];
    s->offset = align_to(offset, s->alignment);
    s->address = bias + s->offset;
    return s->offset + s->size;
}

static bool layout(Linker *l, Elf64_Phdr programs[MAX_PROGRAM_HEADERS], uint16_t *count)
{
    const unsigned code[] = {OUT_INTERP, OUT_INIT, SECTION_TEXT, OUT_FINI, OUT_PLT};
    const unsigned read_only[] = {SECTION_RODATA, OUT_EH_FRAME, OUT_EH_FRAME_HDR, OUT_IPLT_RELA};
    const unsigned writeable[] = {OUT_DYNAMIC, OUT_GOT, OUT_PREINIT_ARRAY, OUT_INIT_ARRAY,
                                  OUT_FINI_ARRAY, SECTION_DATA, OUT_TDATA, OUT_TBSS, SECTION_BSS};
    bool ro = false, rw = false;
    for (size_t i = 0; i < sizeof read_only / sizeof *read_only; i++)
        ro |= l->sections[read_only[i]].size != 0;
    for (size_t i = 0; i < sizeof writeable / sizeof *writeable; i++)
        rw |= l->sections[writeable[i]].size != 0;
    bool tls = l->sections[OUT_TDATA].size || l->sections[OUT_TBSS].size;
    bool interp = l->options->interpreter != NULL;
    bool unwind = l->sections[OUT_EH_FRAME_HDR].size != 0;
    *count = (uint16_t)(3 + ro + rw + tls + interp + unwind + l->dynamic * 3);

    size_t sizes = 8 * PAGE;
    for (size_t i = 1; i < OUT_COUNT; i++) {
        if (l->sections[i].size > (size_t)PTRDIFF_MAX - sizes) {
            error(l->context, (Loc){0}, "linked ELF image is too large");
            return false;
        }
        sizes += l->sections[i].size;
    }
    size_t phdr_size = *count * sizeof(Elf64_Phdr);
    unsigned i = 0;
    programs[i++] = segment(PT_PHDR, sizeof(Elf64_Ehdr), l->base + sizeof(Elf64_Ehdr),
                            phdr_size, phdr_size, PF_R, 8);
    size_t end = sizeof(Elf64_Ehdr) + phdr_size;
    for (size_t j = 0; j < sizeof code / sizeof *code; j++)
        end = place_output(l, code[j], end, l->base);
    if (interp)
        programs[i++] = section_segment(l, PT_INTERP, OUT_INTERP, l->sections[OUT_INTERP].size, PF_R);
    programs[i++] = segment(PT_LOAD, 0, l->base, end, end, PF_R | PF_X, PAGE);
    size_t start = align_to(end, PAGE), next = start;
    for (size_t j = 0; j < sizeof read_only / sizeof *read_only; j++)
        next = place_output(l, read_only[j], next, l->base);
    if (ro) {
        programs[i++] = segment(PT_LOAD, start, l->base + start, next - start, next - start, PF_R, PAGE);
        end = next;
    }
    start = next = align_to(end, PAGE);
    l->sections[OUT_TDATA].alignment = tls_alignment(l);
    for (size_t j = 0; j < sizeof writeable / sizeof *writeable; j++) {
        if (l->dynamic && writeable[j] == SECTION_DATA)
            next = align_to(next, PAGE);
        next = place_output(l, writeable[j], next, l->base);
    }
    l->load_end = l->memory_end = end;
    if (rw) {
        size_t files = l->sections[OUT_TDATA].offset + l->sections[OUT_TDATA].size;
        programs[i++] = segment(PT_LOAD, start, l->base + start, files - start, next - start, PF_R | PF_W, PAGE);
        l->load_end = files;
        l->memory_end = next;
    }
    if (tls) {
        programs[i++] = section_segment(l, PT_TLS, OUT_TDATA, tls_size(l), PF_R);
    }
    if (unwind) {
        OutputSection *hdr = &l->sections[OUT_EH_FRAME_HDR];
        int64_t offset = (int64_t)(l->sections[OUT_EH_FRAME].address - hdr->address - 4);
        if (!fits_signed(offset, 32)) {
            error(l->context, (Loc){0}, ".eh_frame is out of range of .eh_frame_hdr");
            return false;
        }
        int32_t displacement = (int32_t)offset;
        memcpy(hdr->contents.data + 4, &displacement, sizeof displacement);
        programs[i++] = section_segment(l, PT_GNU_EH_FRAME, OUT_EH_FRAME_HDR, hdr->size, PF_R);
    }
    programs[i++] = segment(PT_GNU_STACK, 0, 0, 0, 0, PF_R | PF_W, 16);
    return true;
}

static bool output_symbol(Linker *l, Global *g, Elf64_Sym *symbol)
{
    *symbol = g->copy ? g->imported : g->symbol;
    if (g->copy || symbol->st_shndx == SHN_COMMON) {
        symbol->st_value = l->sections[SECTION_BSS].address + g->bss_offset;
        symbol->st_shndx = SECTION_BSS;
        return true;
    }
    if (!symbol_address(l, g->object, *symbol, g->name, &symbol->st_value))
        return false;
    if (symbol->st_shndx != SHN_ABS)
        symbol->st_shndx = (uint16_t)g->object->locations[symbol->st_shndx].output;
    if (ELF64_ST_TYPE(symbol->st_info) == STT_TLS)
        symbol->st_value -= l->sections[OUT_TDATA].address;
    return true;
}

static bool has_output_symbol(Global *g)
{
    if (g->copy)
        return true;
    unsigned section = g->symbol.st_shndx;
    return g->object && (section == SHN_ABS || section == SHN_COMMON || g->object->locations[section].output);
}

static void dynamic_tag(Linker *l, int64_t tag, uint64_t value)
{
    Elf64_Dyn entry = {.d_tag = tag, .d_un.d_val = value};
    bytes_add(l->context, &l->sections[OUT_DYNAMIC].contents, &entry, sizeof entry);
}

static void dynamic_tags(Linker *l)
{
    const struct { unsigned id; int64_t address, size; } tags[] = {
        {OUT_HASH, DT_HASH, DT_NULL}, {OUT_DYNSTR, DT_STRTAB, DT_STRSZ}, {OUT_DYNSYM, DT_SYMTAB, DT_NULL},
        {OUT_DYN_RELA, DT_RELA, DT_RELASZ}, {OUT_INIT, DT_INIT, DT_NULL}, {OUT_FINI, DT_FINI, DT_NULL},
        {OUT_VERSYM, DT_VERSYM, DT_NULL}, {OUT_VERNEED, DT_VERNEED, DT_NULL},
        {OUT_PREINIT_ARRAY, DT_PREINIT_ARRAY, DT_PREINIT_ARRAYSZ},
        {OUT_INIT_ARRAY, DT_INIT_ARRAY, DT_INIT_ARRAYSZ}, {OUT_FINI_ARRAY, DT_FINI_ARRAY, DT_FINI_ARRAYSZ},
    };
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
        OutputSection *s = &l->sections[tags[i].id];
        if (s->contents.len) {
            dynamic_tag(l, tags[i].address, s->address);
            if (tags[i].size != DT_NULL)
                dynamic_tag(l, tags[i].size, s->contents.len);
        }
    }
    dynamic_tag(l, DT_SYMENT, sizeof(Elf64_Sym));
    if (l->version_libraries)
        dynamic_tag(l, DT_VERNEEDNUM, l->version_libraries);
    if (l->sections[OUT_DYN_RELA].contents.len)
        dynamic_tag(l, DT_RELAENT, sizeof(Elf64_Rela));
    if (!l->options->shared)
        dynamic_tag(l, DT_DEBUG, 0);
    if (l->text_relocations)
        dynamic_tag(l, DT_TEXTREL, 0);
    dynamic_tag(l, DT_FLAGS, DF_BIND_NOW | (l->text_relocations ? DF_TEXTREL : 0) |
                            (l->static_tls ? DF_STATIC_TLS : 0));
    dynamic_tag(l, DT_FLAGS_1, DF_1_NOW | (l->options->pie && !l->options->shared ? DF_1_PIE : 0));
    dynamic_tag(l, DT_NULL, 0);
}

static void dynamic_versions(Linker *l, Bytes *strings)
{
    Context *c = l->context;
    Bytes *indices = &l->sections[OUT_VERSYM].contents;
    Bytes *requirements = &l->sections[OUT_VERNEED].contents;
    uint16_t next = VER_NDX_GLOBAL + 1;
    bytes_int(c, indices, VER_NDX_LOCAL, 2);
    for (size_t i = 0; i < l->dynamic_symbols.len && !c->failed; i++) {
        Global *g = l->dynamic_symbols.data[i];
        uint16_t index = VER_NDX_GLOBAL;
        if (!g->object && g->version.name) {
            Versions *versions = &g->library->versions;
            size_t j = 0;
            while (j < versions->len && !equal(versions->data[j].name, g->version.name))
                j++;
            if (j == versions->len) {
                if (next == 0x8000) {
                    error(c, (Loc){0}, "too many ELF symbol version requirements");
                    return;
                }
                if (!j)
                    l->version_libraries++;
                Version version = g->version;
                version.index = next++;
                PUSH(c, *versions, version);
            }
            index = versions->data[j].index;
        }
        bytes_int(c, indices, index, 2);
    }
    if (!l->version_libraries) {
        indices->len = 0;
        return;
    }
    uint32_t remaining = l->version_libraries;
    for (size_t i = 0; i < l->libraries.len; i++) {
        Library *lib = l->libraries.data[i];
        if (!lib->versions.len)
            continue;
        Elf64_Verneed need = {.vn_version = VER_NEED_CURRENT, .vn_cnt = (uint16_t)lib->versions.len,
                              .vn_file = bytes_string(c, strings, lib->name), .vn_aux = sizeof need,
                              .vn_next = --remaining ? (uint32_t)(sizeof need + lib->versions.len * sizeof(Elf64_Vernaux)) : 0};
        bytes_add(c, requirements, &need, sizeof need);
        for (size_t j = 0; j < lib->versions.len; j++) {
            Version v = lib->versions.data[j];
            Elf64_Vernaux aux = {.vna_hash = v.hash, .vna_other = v.index, .vna_name = bytes_string(c, strings, v.name),
                                .vna_next = j + 1 == lib->versions.len ? 0 : sizeof aux};
            bytes_add(c, requirements, &aux, sizeof aux);
        }
    }
}

static bool dynamic_image(Linker *l, Elf64_Phdr programs[MAX_PROGRAM_HEADERS], uint16_t count)
{
    Context *c = l->context;
    OutputSection *dyn = &l->sections[OUT_DYNAMIC];
    Bytes *strings = &l->sections[OUT_DYNSTR].contents, *symbols = &l->sections[OUT_DYNSYM].contents;
    Bytes *hash = &l->sections[OUT_HASH].contents, *relas = &l->sections[OUT_DYN_RELA].contents;
    bytes_int(c, strings, 0, 1);
    bytes_add(c, symbols, NULL, sizeof(Elf64_Sym));
    for (size_t i = 0; i < l->libraries.len; i++) {
        Library *lib = l->libraries.data[i];
        if (!lib->needed)
            continue;
        for (size_t j = 0; j < lib->references.len; j++) {
            Global *g = global_symbol(l, lib->references.data[j], false);
            if (g)
                g->shared_reference = true;
        }
    }
    for (size_t i = 0; i < l->globals.len; i++) {
        Global *g = l->globals.data[i];
        if (!(l->options->shared || l->options->export_all || g->shared_reference) || !has_output_symbol(g))
            continue;
        unsigned visibility = ELF64_ST_VISIBILITY(g->symbol.st_other);
        if (visibility == STV_DEFAULT || visibility == STV_PROTECTED)
            dynamic_symbol(l, (LinkSymbol){.global = g});
    }
    size_t count_symbols = l->dynamic_symbols.len + 1;
    size_t buckets = count_symbols / 2 + 1;
    uint32_t *table = alloc(c, (buckets + count_symbols) * sizeof *table);
    uint32_t *chains = table + buckets;
    for (size_t i = 1; i < count_symbols && !c->failed; i++) {
        Global *g = l->dynamic_symbols.data[i - 1];
        unsigned type = ELF64_ST_TYPE(g->imported.st_info);
        if (type == STT_GNU_IFUNC)
            type = STT_FUNC;
        Elf64_Sym s = {.st_info = (unsigned char)ELF64_ST_INFO(g->referenced ? (unsigned)STB_GLOBAL : (unsigned)STB_WEAK, type)};
        if (has_output_symbol(g) && !output_symbol(l, g, &s))
            return false;
        s.st_name = bytes_string(c, strings, g->name);
        bytes_add(c, symbols, &s, sizeof s);
        size_t bucket = symbol_hash(g->name) % buckets;
        chains[i] = table[bucket];
        table[bucket] = (uint32_t)i;
    }
    bytes_int(c, hash, buckets, 4);
    bytes_int(c, hash, count_symbols, 4);
    bytes_add(c, hash, table, (buckets + count_symbols) * sizeof *table);
    dynamic_versions(l, strings);
    bytes_add(c, relas, l->relas.data, l->relas.len * sizeof(Elf64_Rela));
    bytes_add(c, relas, l->ifuncs.data, l->ifuncs.len * sizeof(Elf64_Rela));
    for (size_t i = 0; i < l->libraries.len; i++) {
        Library *lib = l->libraries.data[i];
        if (lib->needed)
            dynamic_tag(l, DT_NEEDED, bytes_string(c, strings, lib->name));
    }
    if (l->options->soname)
        dynamic_tag(l, DT_SONAME, bytes_string(c, strings, l->options->soname));
    if (l->options->runpaths.len) {
        size_t offset = strings->len;
        for (size_t i = 0; i < l->options->runpaths.len; i++) {
            const char *path = l->options->runpaths.data[i];
            if (i)
                bytes_int(c, strings, ':', 1);
            bytes_add(c, strings, path, strlen(path));
        }
        bytes_int(c, strings, 0, 1);
        dynamic_tag(l, DT_RUNPATH, offset);
    }
    size_t start = align_to(l->load_end, PAGE), end = start;
    uint64_t address = l->base + align_to(l->memory_end, PAGE);
    uint64_t bias = address - start;
    const unsigned tables[] = {OUT_DYNSTR, OUT_DYNSYM, OUT_HASH, OUT_DYN_RELA, OUT_VERSYM, OUT_VERNEED};
    for (size_t i = 0; i < sizeof tables / sizeof *tables; i++) {
        OutputSection *s = &l->sections[tables[i]];
        s->size = s->contents.len;
        s->alignment = tables[i] == OUT_DYNSTR ? 1 : tables[i] == OUT_VERSYM ? 2 :
                       tables[i] == OUT_HASH || tables[i] == OUT_VERNEED ? 4 : 8;
        end = place_output(l, tables[i], end, bias);
    }
    dynamic_tags(l);
    if (dyn->contents.len > dyn->size) {
        error(c, (Loc){0}, "ELF dynamic table exceeded its allocated size");
        return false;
    }
    bytes_add(c, &dyn->contents, NULL, dyn->size - dyn->contents.len);
    size_t size = end - start;
    size_t relro = l->sections[SECTION_DATA].offset - dyn->offset;
    programs[count - 3] = segment(PT_LOAD, start, address, size, size, PF_R, PAGE);
    programs[count - 2] = section_segment(l, PT_DYNAMIC, OUT_DYNAMIC, dyn->size, PF_R | PF_W);
    programs[count - 1] = segment(PT_GNU_RELRO, dyn->offset, dyn->address, relro, relro, PF_R, 1);
    return !c->failed;
}

static bool executable(Linker *l, Elf64_Phdr programs[MAX_PROGRAM_HEADERS], uint16_t count)
{
    Context *c = l->context;
    uint64_t address = 0;
    if (!l->options->shared) {
        const char *name = l->options->entry;
        Global *entry = global_symbol(l, name, false);
        if (!entry || !entry->object || entry->symbol.st_shndx >= entry->object->header.e_shnum ||
            !(section_kind(entry->object->locations[entry->symbol.st_shndx].output)->flags & SHF_EXECINSTR) ||
            entry->symbol.st_value >= entry->object->sections[entry->symbol.st_shndx].sh_size) {
            error(c, (Loc){0}, "entry symbol '%s' must be defined in executable code", name);
            return false;
        }
        if (!symbol_address(l, entry->object, entry->symbol, name, &address))
            return false;
    }
    Elf64_Ehdr h = elf_header(l->machine, l->base ? ET_EXEC : ET_DYN);
    h.e_entry = address;
    h.e_phoff = sizeof h;
    h.e_phentsize = sizeof(Elf64_Phdr);
    h.e_phnum = count;
    Bytes out = {0}, names = {0}, symbols = {0}, strings = {0};
    bytes_add(c, &out, NULL, sizeof h);
    bytes_add(c, &out, programs, sizeof *programs * count);
    bytes_add(c, &names, NULL, 1);
    enum { EXEC_SYMTAB = OUT_COUNT, EXEC_STRTAB, EXEC_SHSTRTAB, EXEC_SECTION_COUNT };
    Elf64_Shdr sections[EXEC_SECTION_COUNT] = {{0}};
    size_t file_size = out.len;
    for (unsigned i = 1; i < OUT_COUNT; i++) {
        OutputSection *s = &l->sections[i];
        const SectionKind *kind = section_kind(i);
        sections[i] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, kind->name),
                                   .sh_type = kind->type, .sh_flags = kind->flags,
                                   .sh_size = s->size, .sh_addralign = s->alignment,
                                   .sh_offset = s->offset, .sh_addr = s->address};
        if (s->contents.len && s->offset + s->contents.len > file_size)
            file_size = s->offset + s->contents.len;
    }
    sections[OUT_DYNSYM].sh_entsize = sizeof(Elf64_Sym);
    sections[OUT_DYNSYM].sh_link = OUT_DYNSTR;
    sections[OUT_DYNSYM].sh_info = 1;
    sections[OUT_HASH].sh_entsize = 4;
    sections[OUT_HASH].sh_link = OUT_DYNSYM;
    sections[OUT_DYNAMIC].sh_entsize = sizeof(Elf64_Dyn);
    sections[OUT_DYNAMIC].sh_link = OUT_DYNSTR;
    sections[OUT_DYN_RELA].sh_entsize = sizeof(Elf64_Rela);
    sections[OUT_DYN_RELA].sh_link = OUT_DYNSYM;
    sections[OUT_VERSYM].sh_entsize = 2;
    sections[OUT_VERSYM].sh_link = OUT_DYNSYM;
    sections[OUT_VERNEED].sh_link = OUT_DYNSTR;
    sections[OUT_VERNEED].sh_info = l->version_libraries;
    sections[OUT_IPLT_RELA].sh_entsize = sizeof(Elf64_Rela);
    bytes_add(c, &out, NULL, file_size - out.len);
    if (c->failed)
        return false;
    for (unsigned i = 1; i < OUT_COUNT; i++) {
        OutputSection *s = &l->sections[i];
        if (s->contents.len)
            memcpy(out.data + s->offset, s->contents.data, s->contents.len);
    }
    bytes_add(c, &symbols, NULL, sizeof(Elf64_Sym));
    bytes_add(c, &strings, NULL, 1);
    for (size_t i = 0; i < l->globals.len; i++) {
        Global *g = l->globals.data[i];
        if (!has_output_symbol(g))
            continue;
        Elf64_Sym s;
        if (!output_symbol(l, g, &s))
            return false;
        s.st_name = bytes_string(c, &strings, g->name);
        bytes_add(c, &symbols, &s, sizeof s);
    }
    bytes_align(c, &out, 8);
    sections[EXEC_SYMTAB] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".symtab"),
                                         .sh_type = SHT_SYMTAB, .sh_offset = out.len,
                                         .sh_size = symbols.len, .sh_addralign = 8,
                                         .sh_entsize = sizeof(Elf64_Sym), .sh_link = EXEC_STRTAB,
                                         .sh_info = 1};
    bytes_add(c, &out, symbols.data, symbols.len);
    sections[EXEC_STRTAB] = (Elf64_Shdr){.sh_name = bytes_string(c, &names, ".strtab"),
                                         .sh_type = SHT_STRTAB, .sh_offset = out.len,
                                         .sh_size = strings.len, .sh_addralign = 1};
    bytes_add(c, &out, strings.data, strings.len);
    uint32_t names_offset = bytes_string(c, &names, ".shstrtab");
    sections[EXEC_SHSTRTAB] = (Elf64_Shdr){.sh_name = names_offset, .sh_type = SHT_STRTAB,
                                           .sh_offset = out.len, .sh_size = names.len,
                                           .sh_addralign = 1};
    bytes_add(c, &out, names.data, names.len);
    bytes_align(c, &out, 8);
    h.e_shoff = out.len;
    h.e_shnum = EXEC_SECTION_COUNT;
    h.e_shstrndx = EXEC_SHSTRTAB;
    bytes_add(c, &out, sections, sizeof sections);
    memcpy(out.data, &h, sizeof h);
    return elf_write(c, &l->options->elf, &out, true);
}

bool link_elf(Context *c, LinkOptions *options)
{
    Linker l = {.context = c, .options = options, .machine = options->elf.machine, .bucket_count = 64,
                .base = options->pie || options->shared ? 0 : BASE_ADDRESS,
                .dynamic = options->shared || options->interpreter != NULL};
    l.buckets = alloc(c, l.bucket_count * sizeof *l.buckets);
    for (size_t i = 1; i < OUT_COUNT; i++)
        l.sections[i].alignment = 1;
    if (!ld_inputs(c, &l, options))
        return false;
    for (size_t i = 0; i < l.objects.len && !c->failed; i++)
        place_sections(&l, l.objects.data[i]);
    for (size_t i = 0; i < l.globals.len && !c->failed; i++) {
        Global *g = l.globals.data[i];
        if (g->object && g->symbol.st_shndx == SHN_COMMON)
            allocate_bss(&l, g, g->symbol.st_size, g->symbol.st_value);
    }
    for (size_t i = 0; i < l.objects.len && !c->failed; i++)
        prepare_relocations(&l, l.objects.data[i]);
    if (l.dynamic) {
        OutputSection *dynamic = &l.sections[OUT_DYNAMIC];
        reserve(dynamic, c, (l.libraries.len + 32) * sizeof(Elf64_Dyn), 8);
        dynamic->contents.len = 0;
    }
    if (l.sections[OUT_EH_FRAME].size) {
        OutputSection *frame = &l.sections[OUT_EH_FRAME], *hdr = &l.sections[OUT_EH_FRAME_HDR];
        reserve(frame, c, 4, 4);
        reserve(hdr, c, 8, 4);
        /* Version 1, PC-relative signed 32-bit frame pointer, linear FDE search. */
        const unsigned char encoding[] = {1, 0x1b, 0xff, 0xff};
        if (!c->failed)
            memcpy(hdr->contents.data, encoding, sizeof encoding);
    }
    if (options->interpreter) {
        OutputSection *interp = &l.sections[OUT_INTERP];
        bytes_string(c, &interp->contents, options->interpreter);
        interp->size = interp->contents.len;
    }
    Elf64_Phdr programs[MAX_PROGRAM_HEADERS] = {{0}};
    uint16_t count;
    if (c->failed || !layout(&l, programs, &count) || !write_slots(&l))
        return false;
    for (size_t i = 0; i < l.objects.len && !c->failed; i++)
        apply_relocations(&l, l.objects.data[i]);
    if (c->failed)
        return false;
    if (l.dynamic) {
        if (!dynamic_image(&l, programs, count))
            return false;
    } else if (l.ifuncs.len) {
        memcpy(l.sections[OUT_IPLT_RELA].contents.data, l.ifuncs.data, l.ifuncs.len * sizeof(Elf64_Rela));
    }
    return executable(&l, programs, count);
}
