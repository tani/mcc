#include "elf_tools.h"
#include "float16.h"
#include "int128_float.h"
#include "int128_math.h"
#include "quad.h"
#include "quad_bits.h"

#include <float.h>
#include <inttypes.h>
#include <string.h>

uint64_t float_bits(long double value, unsigned format)
{
    if (format == FP_BINARY16 || format == FP_BFLOAT16) {
        Integer128 bits;
        unsigned flags = 0;
        memcpy(&bits, &value, sizeof bits);
        return float16_encode(bits, LDBL_MANT_DIG == 64, format, FP_NEAREST, &flags);
    }
    if (format == 4) {
        float converted = (float)value;
        uint32_t bits;
        memcpy(&bits, &converted, sizeof bits);
        return bits;
    }
    double converted = (double)value;
    uint64_t bits;
    memcpy(&bits, &converted, sizeof bits);
    return bits;
}

static long double float16_value(uint16_t value, unsigned format)
{
    uint32_t bits = float16_expand(value, format);
    float rounded;
    memcpy(&rounded, &bits, sizeof rounded);
    return (long double)rounded;
}

bool extended_float_bits(Context *c, long double value, uint64_t bits[2], Loc loc)
{
    if (sizeof value != 16 || (LDBL_MANT_DIG != 64 && LDBL_MANT_DIG != 113)) {
        error(c, loc, "unsupported host long double format");
        return false;
    }
    Integer128 raw;
    unsigned flags = 0;
    memcpy(&raw, &value, sizeof value);
    if (LDBL_MANT_DIG == 64) {
        raw.hi &= 0xffff;
        if (c->target == &aarch64_linux)
            raw = quad_from_x87(raw, &flags);
    } else if (c->target == &x86_64_linux) {
        raw = quad_to_x87(raw, FP_NEAREST, &flags);
    }
    bits[0] = raw.lo;
    bits[1] = raw.hi;
    return true;
}

static void padding(FILE *out, size_t size)
{
    if (size)
        fprintf(out, "\t.zero %zu\n", size);
}

static void bytes(FILE *out, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        fputs(i % 16 ? "," : "\t.byte ", out);
        fprintf(out, "%u", data[i]);
        if (i % 16 == 15 || i + 1 == len)
            fputc('\n', out);
    }
}

static bool static_address(Context *c, Node *n, Symbol **sym, uint64_t *add)
{
    if ((n->kind == N_SYMBOL || n->kind == N_COMPOUND) && !n->symbol->local &&
        !global_register(n->symbol) &&
        !n->symbol->thread_local_storage && !(n->type->kind == TY_FUNC && n->symbol->owner)) {
        *sym = n->symbol;
        return true;
    }
    if (n->kind == N_DEREF)
        return address_constant(c, n->left, sym, add);
    if (n->kind == N_MEMBER && !n->right && static_address(c, n->left, sym, add)) {
        *add += n->integer;
        return true;
    }
    return false;
}

bool address_constant(Context *c, Node *n, Symbol **sym, uint64_t *add)
{
    uint64_t value;
    if (n->kind == N_ADDR)
        return static_address(c, n->left, sym, add);
    if (n->kind == N_CAST)
        return address_constant(c, n->left, sym, add);
    if (constant_int(c, n, &value)) {
        *add += value;
        return true;
    }
    if (n->kind == N_BINARY && (n->op == '+' || n->op == '-') &&
        constant_int(c, n->right, &value) && address_constant(c, n->left, sym, add)) {
        *add += n->op == '+' ? value : 0 - value;
        return true;
    }
    return false;
}

static Symbol *label_symbol(Node *n)
{
    while (n->kind == N_CAST)
        n = n->left;
    if (n->kind != N_ADDR || n->left->kind != N_SYMBOL || !n->left->symbol->label_address_taken)
        return NULL;
    return n->left->symbol;
}

static bool label_difference(FILE *out, Type *t, Node *n)
{
    while (n->kind == N_CAST)
        n = n->left;
    if (n->kind != N_BINARY || n->op != '-')
        return false;
    Symbol *left = label_symbol(n->left), *right = label_symbol(n->right);
    if (!left || !right)
        return false;
    const char *directive = t->size == 1 ? ".byte" : t->size == 2 ? ".hword" :
                            t->size == 4 ? ".long" : ".quad";
    fprintf(out, "\t%s %s-%s\n", directive, left->assembly, right->assembly);
    return true;
}

static Float128 quad_value(Integer128 bits)
{
    Float128 value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static bool decimal_to_binary_constant(Context *c, Node *cast, Integer128 *bits, bool reject_zero_divisor)
{
    Integer128 source;
    if (!decimal_constant(c, cast->left, &source, reject_zero_divisor))
        return false;
    *bits = decimal_to_binary(floating_format(c->target, cast->type), cast->left->type->size, source);
    return true;
}

bool quad_constant(Context *c, Node *n, Integer128 *result, bool reject_zero_divisor)
{
    Float128 value;
    if (!binary128(c->target, n->type)) {
        if (n->type->kind == TY_INT || n->type->kind == TY_BOOL) {
            Integer128 integer;
            if (!constant_integer(c, n, &integer))
                return false;
            *result = wide_float_bits(integer, 16, n->type->uns);
            return true;
        }
        long double real;
        if (!real_constant(c, n, &real, reject_zero_divisor))
            return false;
        value = (Float128)real;
    } else if (n->kind == N_FLOAT) {
        if (n->flags & FLOAT_BITS_LITERAL) {
            *result = (Integer128){n->integer, n->integer_high};
            return true;
        }
        value = (Float128)n->real;
    } else if (n->kind == N_CAST && n->left->type->kind == TY_DECIMAL) {
        return decimal_to_binary_constant(c, n, result, reject_zero_divisor);
    } else if ((n->kind == N_CAST || n->kind == N_MEMBER) && n->left->type->kind == TY_COMPLEX) {
        Node *part = constant_component(c, n->left, n->kind == N_MEMBER && n->integer != 0);
        return part && quad_constant(c, part, result, reject_zero_divisor);
    } else if (n->kind == N_CAST) {
        return quad_constant(c, n->left, result, reject_zero_divisor);
    } else if (n->kind == N_COND) {
        bool test;
        return constant_truth(c, n->left, &test) &&
               quad_constant(c, test ? n->right : n->third, result, reject_zero_divisor);
    } else if (n->kind == N_UNARY && n->op == '-') {
        if (!quad_constant(c, n->left, result, reject_zero_divisor))
            return false;
        result->hi ^= UINT64_C(1) << 63;
        return true;
    } else if (n->kind == N_BINARY) {
        Integer128 left, right;
        if (!quad_constant(c, n->left, &left, reject_zero_divisor) ||
            !quad_constant(c, n->right, &right, reject_zero_divisor))
            return false;
        Float128 a = quad_value(left), b = quad_value(right);
        switch (n->op) {
        case '+': value = a + b; break;
        case '-': value = a - b; break;
        case '*': value = a * b; break;
        case '/':
            if (reject_zero_divisor && b == (Float128)0)
                return false;
            value = a / b;
            break;
        default: return false;
        }
    } else {
        return false;
    }
    memcpy(result, &value, sizeof value);
    return true;
}

static long double quad_as_real(Integer128 bits, unsigned format)
{
    if (format == FP_BINARY16 || format == FP_BFLOAT16) {
        unsigned flags = 0;
        return float16_value(float16_encode(bits, false, format, FP_NEAREST, &flags), format);
    }
    Float128 quad = quad_value(bits);
    if (format == 4)
        return (long double)(float)quad;
    if (format == 8)
        return (long double)(double)quad;
    if (format == 10 && LDBL_MANT_DIG != 64) {
        unsigned flags = 0;
        bits = quad_from_x87(quad_to_x87(bits, FP_NEAREST, &flags), &flags);
        quad = quad_value(bits);
    }
    return (long double)quad;
}

static long double binary_as_real(Integer128 bits, unsigned format)
{
    if (format == 4) {
        float value;
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    if (format == 8) {
        double value;
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    if (format == 10) {
        if (LDBL_MANT_DIG == 64) {
            long double value;
            memcpy(&value, &bits, sizeof value);
            return value;
        }
        unsigned flags = 0;
        bits = quad_from_x87(bits, &flags);
    }
    if (format >= 10)
        return quad_as_real(bits, 16);
    return float16_value((uint16_t)bits.lo, format);
}

bool real_constant(Context *c, Node *n, long double *result, bool reject_zero_divisor)
{
    if (binary128(c->target, n->type)) {
        Integer128 bits;
        if (!quad_constant(c, n, &bits, reject_zero_divisor))
            return false;
        *result = quad_as_real(bits, 16);
        return true;
    }
    if (n->kind == N_CAST && n->type->kind == TY_FLOAT && n->left->type->kind == TY_DECIMAL) {
        Integer128 bits;
        if (!decimal_to_binary_constant(c, n, &bits, reject_zero_divisor))
            return false;
        *result = binary_as_real(bits, floating_format(c->target, n->type));
        return true;
    }
    if (n->kind == N_CAST && n->type->kind == TY_FLOAT && binary128(c->target, n->left->type)) {
        Integer128 bits;
        if (!quad_constant(c, n->left, &bits, reject_zero_divisor))
            return false;
        *result = quad_as_real(bits, floating_format(c->target, n->type));
        return true;
    }
    uint64_t value;
    long double a, b;
    Node *wide = n->kind == N_CAST && n->type->kind == TY_FLOAT && integer128(n->left->type) ?
                 n->left : integer128(n->type) ? n : NULL;
    if (wide) {
        Integer128 integer;
        if (!constant_integer(c, wide, &integer))
            return false;
        unsigned format = n->type->kind == TY_FLOAT && n->type->size < 16 ?
                          floating_format(c->target, n->type) : LDBL_MANT_DIG == 64 ? 10 : 16;
        Integer128 bits = wide_float_bits(integer, format, wide->type->uns);
        *result = binary_as_real(bits, format);
        return true;
    }
    if (constant_int(c, n, &value)) {
        *result = n->type->uns ? (long double)value : (long double)(int64_t)value;
    } else if ((n->kind == N_MEMBER || n->kind == N_CAST) && n->left->type->kind == TY_COMPLEX) {
        Node *part = constant_component(c, n->left, n->kind == N_MEMBER && n->integer != 0);
        if (!part || !real_constant(c, part, result, reject_zero_divisor))
            return false;
    } else if (n->kind == N_FLOAT) {
        *result = n->flags & FLOAT_BITS_LITERAL ?
                  binary_as_real((Integer128){n->integer, n->integer_high}, floating_format(c->target, n->type)) :
                  n->real;
    } else if (n->kind == N_CAST && real_constant(c, n->left, &a, reject_zero_divisor)) {
        *result = a;
    } else if (n->kind == N_UNARY && n->op == '-' && real_constant(c, n->left, &a, reject_zero_divisor)) {
        *result = -a;
    } else if (n->kind == N_COND) {
        bool test;
        if (!constant_truth(c, n->left, &test) ||
            !real_constant(c, test ? n->right : n->third, result, reject_zero_divisor))
            return false;
    } else if (n->kind == N_BINARY && real_constant(c, n->left, &a, reject_zero_divisor) &&
               real_constant(c, n->right, &b, reject_zero_divisor)) {
        switch (n->op) {
        case '+': *result = a + b; break;
        case '-': *result = a - b; break;
        case '*': *result = a * b; break;
        case '/':
            if (reject_zero_divisor && b == 0)
                return false;
            *result = a / b;
            break;
        default: return false;
        }
    } else {
        return false;
    }
    if (narrow_float(n->type)) {
        unsigned format = floating_format(c->target, n->type);
        *result = float16_value((uint16_t)float_bits(*result, format), format);
    } else if (n->type->kind == TY_FLOAT && n->type->size == 4)
        *result = (float)*result;
    else if (n->type->kind == TY_FLOAT && n->type->size == 8)
        *result = (double)*result;
    return true;
}

bool binary_constant_bits(Context *c, Node *n, Integer128 *bits, bool reject_zero_divisor)
{
    Type *t = n->type;
    if (n->kind == N_FLOAT && (n->flags & FLOAT_BITS_LITERAL)) {
        *bits = (Integer128){n->integer, n->integer_high};
        return true;
    }
    if (n->kind == N_CAST && n->left->type->kind == TY_DECIMAL)
        return decimal_to_binary_constant(c, n, bits, reject_zero_divisor);
    if (binary128(c->target, t))
        return quad_constant(c, n, bits, reject_zero_divisor);
    unsigned format = floating_format(c->target, t);
    if (n->kind == N_UNARY && n->op == '-') {
        if (!binary_constant_bits(c, n->left, bits, reject_zero_divisor))
            return false;
        Integer128 sign = wide_shift_left((Integer128){1, 0}, wide_fp_bytes(format) * 8 - 1);
        bits->lo ^= sign.lo;
        bits->hi ^= sign.hi;
        return true;
    }
    if (n->kind == N_CAST && n->left->type->kind == TY_FLOAT &&
        floating_format(c->target, n->left->type) == format)
        return binary_constant_bits(c, n->left, bits, reject_zero_divisor);
    if (n->kind == N_COND) {
        bool test;
        return constant_truth(c, n->left, &test) &&
               binary_constant_bits(c, test ? n->right : n->third, bits, reject_zero_divisor);
    }
    if (n->kind == N_CAST && integer128(n->left->type)) {
        Integer128 integer;
        if (!constant_integer(c, n->left, &integer))
            return false;
        *bits = wide_float_bits(integer, format, n->left->type->uns);
        return true;
    }
    long double real;
    if (!real_constant(c, n, &real, reject_zero_divisor))
        return false;
    if (t->size == 16) {
        uint64_t words[2];
        if (!extended_float_bits(c, real, words, n->loc))
            return false;
        *bits = (Integer128){words[0], words[1]};
    } else {
        *bits = (Integer128){float_bits(real, format), 0};
    }
    return true;
}

typedef struct {
    size_t offset;
    unsigned partial;
    bool pending;
} DataCursor;

static void flush_data_byte(FILE *out, DataCursor *cursor)
{
    if (cursor->pending) {
        fprintf(out, "\t.byte %u\n", cursor->partial);
        cursor->offset++;
        cursor->partial = 0;
        cursor->pending = false;
    }
}

static void seek_data(FILE *out, DataCursor *cursor, size_t offset)
{
    if (offset != cursor->offset) {
        flush_data_byte(out, cursor);
        padding(out, offset - cursor->offset);
        cursor->offset = offset;
    }
}

static bool data_bit_field(Context *c, FILE *out, DataCursor *cursor, Member member, Node *init)
{
    if (init->kind == N_RANGE_VALUE)
        init = init->left;
    Node converted, floating;
    if (init->kind == N_CAST && init->left->type->kind == TY_FLOAT && init->left->kind != N_FLOAT) {
        long double real;
        if (!real_constant(c, init->left, &real, false)) {
            error(c, init->loc, "bit-field initializer is not an arithmetic constant");
            return false;
        }
        floating = (Node){.kind = N_FLOAT, .loc = init->left->loc, .type = init->left->type, .real = real};
        converted = *init;
        converted.left = &floating;
        init = &converted;
    }
    Integer128 value;
    if (!constant_integer(c, init, &value)) {
        error(c, init->loc, "bit-field initializer is not an integer constant");
        return false;
    }
    /* Layout uses low-to-high bits on both supported little-endian targets. */
    for (unsigned done = 0; done < member.width;) {
        unsigned at = member.bit + done;
        unsigned position = at % 8;
        unsigned count = 8 - position;
        if (count > member.width - done)
            count = member.width - done;
        seek_data(out, cursor, member.offset + at / 8);
        unsigned part = (unsigned)wide_shift_right(value, done, true).lo & ((1u << count) - 1);
        cursor->partial |= part << position;
        cursor->pending = true;
        done += count;
    }
    return true;
}

static void data_initializer(Context *c, FILE *out, Type *t, Node *n)
{
    if (!n) {
        padding(out, t->size);
        return;
    }
    if (n->kind == N_RANGE_VALUE)
        n = n->left;
    if (t->kind == TY_COMPLEX) {
        Node *real = constant_component(c, n, false);
        Node *imaginary = constant_component(c, n, true);
        if (!real || !imaginary) {
            error(c, n->loc, "unsupported or nonconstant complex initializer");
            return;
        }
        data_initializer(c, out, t->base, real);
        data_initializer(c, out, t->base, imaginary);
        return;
    }
    if (n->kind == N_INIT) {
        DataCursor cursor = {0};
        for (size_t i = 0; i < n->list.len; i++) {
            Node *item = n->list.data[i];
            size_t index = item->integer;
            Member m = indexed_elements(t) ? (Member){.type=t->base, .offset=index * t->base->size}
                                          : t->members.data[index];
            if (m.bit_field) {
                if (!data_bit_field(c, out, &cursor, m, item->left))
                    return;
                continue;
            }
            Type *element = m.type;
            if (element->kind == TY_ARRAY && !element->complete &&
                item->left->type && item->left->type->kind == TY_ARRAY)
                element = item->left->type;
            seek_data(out, &cursor, m.offset);
            data_initializer(c, out, element, item->left);
            cursor.offset = m.offset + element->size;
        }
        flush_data_byte(out, &cursor);
        if (t->size > cursor.offset)
            padding(out, t->size - cursor.offset);
        return;
    }
    if (t->kind == TY_ARRAY && n->kind == N_SYMBOL && n->symbol->bytes) {
        size_t length = n->symbol->byte_count < t->size ? n->symbol->byte_count : t->size;
        bytes(out, n->symbol->bytes, length);
        padding(out, t->size - length);
        return;
    }
    if (t->kind == TY_INT && label_difference(out, t, n))
        return;
    if (integer128(t)) {
        Integer128 value;
        if (!constant_integer(c, n, &value))
            error(c, n->loc, "128-bit initializer is not an integer constant");
        else
            fprintf(out, "\t.quad %" PRIu64 ", %" PRIu64 "\n", value.lo, value.hi);
        return;
    }
    if (t->kind == TY_DECIMAL) {
        Integer128 bits;
        if (!decimal_constant(c, n, &bits, false))
            error(c, n->loc, "unsupported or nonconstant decimal initializer");
        else if (t->size == 16)
            fprintf(out, "\t.quad %" PRIu64 ", %" PRIu64 "\n", bits.lo, bits.hi);
        else
            fprintf(out, "\t%s %" PRIu64 "\n", t->size == 4 ? ".long" : ".quad", bits.lo);
        return;
    }
    if (t->kind == TY_FLOAT) {
        Integer128 bits;
        if (!binary_constant_bits(c, n, &bits, false))
            error(c, n->loc, "unsupported or nonconstant floating-point initializer");
        else if (t->size == 16)
            fprintf(out, "\t.quad %" PRIu64 ", %" PRIu64 "\n", bits.lo, bits.hi);
        else
            fprintf(out, "\t%s %" PRIu64 "\n",
                    t->size == 2 ? ".hword" : t->size == 4 ? ".long" : ".quad", bits.lo);
        return;
    }
    uint64_t value = 0;
    Symbol *sym = NULL;
    bool valid = t->kind == TY_PTR ? address_constant(c, n, &sym, &value) : constant_int(c, n, &value);
    if (!valid) {
        error(c, n->loc, "unsupported or nonconstant static initializer");
        return;
    }
    if (sym) {
        fprintf(out, "\t.quad %s%+" PRId64 "\n", sym->assembly, (int64_t)value);
        return;
    }
    const char *directive = t->size == 1 ? ".byte" : t->size == 2 ? ".hword" :
                            t->size == 4 ? ".long" : ".quad";
    if (t->size < 8)
        value &= (UINT64_C(1) << (8 * t->size)) - 1;
    fprintf(out, "\t%s %" PRIu64 "\n", directive, value);
}

bool symbol_may_be_preempted(const Symbol *s)
{
    return s->weak_reference || (!s->internal &&
           (s->visibility == VIS_UNSPECIFIED || s->visibility == VIS_DEFAULT));
}

bool call_through_got(Context *c, const Symbol *s)
{
    return s && (c->no_plt || (s->attribute_markers & MARK_NOPLT)) && symbol_may_be_preempted(s);
}

void emit_symbol_linkage(FILE *out, Symbol *s)
{
    if (s->weak_reference) {
        fprintf(out, "\t.weakref %s, %s\n", s->assembly, s->alias);
        return;
    }
    static const char *const visibility_directives[] = {
        [VIS_UNSPECIFIED] = NULL, [VIS_DEFAULT] = NULL,
        [VIS_INTERNAL] = ".internal", [VIS_HIDDEN] = ".hidden", [VIS_PROTECTED] = ".protected"
    };
    const char *directive = visibility_directives[s->visibility];
    if (directive)
        fprintf(out, "\t%s %s\n", directive, s->assembly);
    if (s->weak)
        fprintf(out, "\t.weak %s\n", s->assembly);
    if (s->alias) {
        if (!s->weak && !s->internal)
            fprintf(out, "\t.global %s\n", s->assembly);
        fprintf(out, "\t.set %s, %s\n", s->assembly, s->alias);
    }
}

static void emit_lifecycle_entry(FILE *out, const char *symbol, const char *array, char type,
                                 unsigned priority)
{
    if (!priority)
        return;
    fprintf(out, "\t.section .%s", array);
    if (--priority != DEFAULT_LIFECYCLE_PRIORITY)
        fprintf(out, ".%05u", priority);
    fprintf(out, ",\"aw\",%c%s\n\t.p2align 3\n\t.quad %s\n", type, array, symbol);
}

void emit_lifecycle_functions(Context *c, FILE *out)
{
    char type = c->target == &x86_64_linux ? '@' : '%';
    for (size_t i = 0; i < c->globals.len; i++) {
        Symbol *s = c->globals.data[i];
        if (s->body) {
            emit_lifecycle_entry(out, s->assembly, "init_array", type, s->constructor_priority);
            emit_lifecycle_entry(out, s->assembly, "fini_array", type, s->destructor_priority);
        }
    }
}

static void emit_named_section(FILE *out, const char *name, const char *flags)
{
    fputs("\t.section \"", out);
    for (const char *p = name; *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', out);
        fputc(*p, out);
    }
    fprintf(out, "\",\"%s\"\n", flags);
}

void emit_function_entry(Context *c, FILE *out, Symbol *s, unsigned id)
{
    bool arm = c->target == &aarch64_linux;
    PatchableEntry entry = c->patchable_entry;
    const PatchableEntries *attributes = s->patchable_entries;
    if (attributes)
        entry = attributes->data[attributes->len - 1];
    if (entry.count && entry.before > entry.count) {
        warning_at(c, WARNING_ATTRIBUTES, s->loc, "patchable function entry %u exceeds size %u",
                   entry.before, entry.count);
        entry.before = 0;
    }
    if (entry.count)
        fprintf(out, "\t.section __patchable_function_entries,\"awo\",%cprogbits,.Lpatchable%u\n"
                     "\t.p2align 3\n\t.quad .Lpatchable%u\n", arm ? '%' : '@', id, id);
    if (s->section)
        emit_named_section(out, s->section, "ax");
    else
        fputs("\t.text\n", out);
    unsigned alignment = arm ? 4 : 16;
    fprintf(out, "\t.balign %u\n", s->alignment > alignment ? s->alignment : alignment);
    if (entry.count) {
        fprintf(out, ".Lpatchable%u:\n", id);
        for (unsigned i = 0; i < entry.before; i++)
            fputs("\tnop\n", out);
    }
    if (!s->internal && !s->weak)
        fprintf(out, "\t.%s %s\n", arm ? "global" : "globl", s->assembly);
    fprintf(out, "\t.type %s, %cfunction\n%s:\n", s->assembly, arm ? '%' : '@', s->assembly);
    for (unsigned i = entry.before; i < entry.count; i++)
        fputs("\tnop\n", out);
}

static bool has_address_initializer(Node *n)
{
    if (!n)
        return false;
    if (n->kind == N_ADDR || has_address_initializer(n->left) ||
        has_address_initializer(n->right) || has_address_initializer(n->third))
        return true;
    for (size_t i = 0; i < n->list.len; i++)
        if (has_address_initializer(n->list.data[i]))
            return true;
    return false;
}

static const char *named_data_flags(Symbol *s)
{
    if (s->thread_local_storage)
        return "awT";
    if (elf_section_family(s->section, ".data") || elf_section_family(s->section, ".bss") ||
        elf_section_family(s->section, ".preinit_array") ||
        elf_section_family(s->section, ".init_array") || elf_section_family(s->section, ".fini_array"))
        return "aw";
    for (Type *t = s->type;; t = t->base) {
        if ((t->qualifiers & (S_CONST | S_VOLATILE)) == S_CONST && !has_address_initializer(s->init))
            return "a";
        if (t->kind != TY_ARRAY)
            return "aw";
    }
}

void emit_static_data(Context *c, FILE *out, Symbol *s)
{
    Type *t = s->type;
    if (!t->complete) {
        if (t->kind == TY_ARRAY && s->tentative) {
            t->length = 1;
            t->complete = true;
            t->size = t->base->size;
        } else {
            error(c, s->loc, "object '%s' has incomplete type", s->name);
            return;
        }
    }
    if (s->section)
        emit_named_section(out, s->section, named_data_flags(s));
    else
        fputs(s->thread_local_storage ? (s->init ? "\t.section .tdata,\"awT\",%progbits\n"
                                            : "\t.section .tbss,\"awT\",%nobits\n") :
          s->read_only ? "\t.section .rodata\n" :
          s->init ? "\t.data\n" : "\t.bss\n", out);
    if (!s->internal && !s->weak)
        fprintf(out, "\t.global %s\n", s->assembly);
    fprintf(out, "\t.balign %u\n\t.type %s, %%%s\n%s:\n", symbol_alignment(s), s->assembly,
            s->thread_local_storage ? "tls_object" : "object", s->assembly);
    if (s->bytes)
        bytes(out, s->bytes, s->byte_count);
    else
        data_initializer(c, out, t, s->init);
    fprintf(out, "\t.size %s, .-%s\n", s->assembly, s->assembly);
}
