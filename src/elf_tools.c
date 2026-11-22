#define _POSIX_C_SOURCE 200809L
#include "elf_tools.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const SectionKind elf_sections[SECTION_COUNT] = {
    [SECTION_TEXT] = {".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR},
    [SECTION_RODATA] = {".rodata", SHT_PROGBITS, SHF_ALLOC},
    [SECTION_DATA] = {".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE},
    [SECTION_BSS] = {".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE},
};

uint16_t elf_machine(const char *target)
{
    if (target) {
        if (equal(target, "aarch64-linux-gnu"))
            return EM_AARCH64;
        if (equal(target, "x86_64-linux-gnu"))
            return EM_X86_64;
        return EM_NONE;
    }
#if defined(__x86_64__)
    return EM_X86_64;
#elif defined(__aarch64__)
    return EM_AARCH64;
#else
    return EM_NONE;
#endif
}

Elf64_Ehdr elf_header(uint16_t machine, uint16_t type)
{
    Elf64_Ehdr header = {.e_type = type, .e_machine = machine, .e_version = EV_CURRENT,
                        .e_ehsize = sizeof(Elf64_Ehdr), .e_shentsize = sizeof(Elf64_Shdr)};
    memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    return header;
}

void bytes_add(Context *c, Bytes *buffer, const void *data, size_t size)
{
    if (!size || c->failed)
        return;
    if (size > (size_t)PTRDIFF_MAX - buffer->len) {
        error(c, (Loc){0}, "ELF output is too large");
        return;
    }
    size_t end = buffer->len + size;
    if (end > buffer->cap) {
        size_t cap = buffer->cap <= (size_t)PTRDIFF_MAX / 2 ? buffer->cap * 2 : buffer->cap;
        if (cap < end)
            cap = end;
        unsigned char *memory = alloc(c, cap);
        if (buffer->len)
            memcpy(memory, buffer->data, buffer->len);
        buffer->data = memory;
        buffer->cap = cap;
    }
    unsigned char *dest = buffer->data + buffer->len;
    if (data)
        memcpy(dest, data, size);
    else
        memset(dest, 0, size);
    buffer->len = end;
}

void bytes_int(Context *c, Bytes *buffer, uint64_t value, unsigned width)
{
    unsigned char encoded[8];
    for (unsigned i = 0; i < width; i++)
        encoded[i] = (unsigned char)(value >> (i * 8));
    bytes_add(c, buffer, encoded, width);
}

void bytes_align(Context *c, Bytes *buffer, size_t alignment)
{
    bytes_add(c, buffer, NULL, (0 - buffer->len) & (alignment - 1));
}

uint32_t bytes_string(Context *c, Bytes *buffer, const char *name)
{
    size_t start = buffer->len;
    if (start > UINT32_MAX) {
        error(c, (Loc){0}, "ELF string table is too large");
        return 0;
    }
    bytes_add(c, buffer, name, strlen(name) + 1);
    return (uint32_t)start;
}

static bool aliases_input(Context *c, ElfOptions *options)
{
    struct stat output;
    if (stat(options->output, &output) < 0)
        return false;
    if (!S_ISREG(output.st_mode)) {
        error(c, (Loc){0}, "%s: output is not a regular file", options->output);
        return true;
    }
    for (size_t i = 0; i < options->inputs.len; i++) {
        struct stat input;
        if (!stat(options->inputs.data[i], &input) && input.st_dev == output.st_dev &&
            input.st_ino == output.st_ino) {
            error(c, (Loc){0}, "%s: output aliases an input", options->output);
            return true;
        }
    }
    return false;
}

bool elf_write(Context *c, ElfOptions *options, const Bytes *bytes, bool executable)
{
    if (c->failed)
        return false;
    if (equal(options->output, "-")) {
        if (fwrite(bytes->data, 1, bytes->len, stdout) == bytes->len && fflush(stdout) == 0)
            return true;
        error(c, (Loc){0}, "cannot write ELF to stdout: %s", strerror(errno));
        return false;
    }
    if (aliases_input(c, options))
        return false;
    char *path = format(c, "%s.tmp.XXXXXX", options->output);
    int fd = mkstemp(path);
    if (fd < 0) {
        error(c, (Loc){0}, "%s: %s", options->output, strerror(errno));
        return false;
    }
    mode_t mask = umask(0);
    umask(mask);
    bool ok = fchmod(fd, (executable ? 0777u : 0666u) & ~mask) == 0;
    FILE *file = fdopen(fd, "wb");
    if (file) {
        ok = ok && fwrite(bytes->data, 1, bytes->len, file) == bytes->len;
        ok = fclose(file) == 0 && ok;
    } else {
        close(fd);
        ok = false;
    }
    if (ok && rename(path, options->output) == 0)
        return true;
    error(c, (Loc){0}, "%s: cannot write ELF output: %s", options->output, strerror(errno));
    unlink(path);
    return false;
}

bool fits_signed(int64_t value, unsigned bits)
{
    int64_t limit = INT64_C(1) << (bits - 1);
    return value >= -limit && value < limit;
}
