#include "compiler.h"

#include <float.h>
#include <inttypes.h>
#include <string.h>

uint64_t float_bits(long double value, unsigned size)
{
    if (size == 4) {
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

bool extended_float_bits(Context *c, long double value, uint64_t bits[2], Loc loc)
{
    if (sizeof value != 16 || (LDBL_MANT_DIG != 64 && LDBL_MANT_DIG != 113)) {
        error(c, loc, "unsupported host long double format");
        return false;
    }
    memcpy(bits, &value, sizeof value);
    /* Both formats use the same exponent bias. x87 stores the leading bit explicitly. */
    if (LDBL_MANT_DIG == 64) {
        uint64_t sign_exponent = bits[1] & 0xffff;
        bits[1] = sign_exponent;
        if (c->target == &aarch64_linux) {
            uint64_t fraction = bits[0] & UINT64_C(0x7fffffffffffffff);
            bits[1] = (sign_exponent << 48) | (fraction >> 15);
            bits[0] = fraction << 49;
        }
    } else if (c->target == &x86_64_linux) {
        const uint64_t integer_bit = UINT64_C(1) << 63;
        uint64_t sign = (bits[1] >> 63) << 15;
        uint64_t exponent = (bits[1] >> 48) & 0x7fff;
        uint64_t fraction = ((bits[1] & UINT64_C(0xffffffffffff)) << 15) | (bits[0] >> 49);
        uint64_t tail = bits[0] & ((UINT64_C(1) << 49) - 1);
        uint64_t significand = fraction | (exponent ? integer_bit : 0);
        if (exponent == 0x7fff) {
            if (tail && !fraction)
                significand |= UINT64_C(1) << 62;
        } else {
            const uint64_t halfway = UINT64_C(1) << 48;
            if (tail > halfway || (tail == halfway && (significand & 1))) {
                if (++significand == 0) {
                    significand = integer_bit;
                    exponent++;
                } else if (!exponent && significand == integer_bit) {
                    exponent = 1;
                }
            }
        }
        bits[0] = significand;
        bits[1] = sign | exponent;
    }
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

static bool address_constant(Context *, Node *, Symbol **, uint64_t *);

static bool static_address(Context *c, Node *n, Symbol **sym, uint64_t *add)
{
    if ((n->kind == N_SYMBOL || n->kind == N_COMPOUND) && !n->symbol->local &&
        !n->symbol->thread_local_storage) {
        *sym = n->symbol;
        return true;
    }
    if (n->kind == N_DEREF)
        return address_constant(c, n->left, sym, add);
    if (n->kind == N_MEMBER && static_address(c, n->left, sym, add)) {
        *add += n->integer;
        return true;
    }
    return false;
}

static bool address_constant(Context *c, Node *n, Symbol **sym, uint64_t *add)
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

static bool real_constant(Context *c, Node *n, long double *result)
{
    uint64_t value;
    long double a, b;
    if (constant_int(c, n, &value)) {
        *result = n->type->uns ? (long double)value : (long double)(int64_t)value;
    } else if (n->kind == N_FLOAT) {
        *result = n->real;
    } else if (n->kind == N_CAST && real_constant(c, n->left, &a)) {
        *result = a;
    } else if (n->kind == N_UNARY && n->op == '-' && real_constant(c, n->left, &a)) {
        *result = -a;
    } else if (n->kind == N_BINARY && real_constant(c, n->left, &a) &&
               real_constant(c, n->right, &b)) {
        switch (n->op) {
        case '+': *result = a + b; break;
        case '-': *result = a - b; break;
        case '*': *result = a * b; break;
        case '/': *result = a / b; break;
        default: return false;
        }
    } else {
        return false;
    }
    if (n->type->kind == TY_FLOAT && n->type->size == 4)
        *result = (float)*result;
    else if (n->type->kind == TY_FLOAT && n->type->size == 8)
        *result = (double)*result;
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
    if (n->kind == N_INIT) {
        size_t end = 0;
        for (size_t i = 0; i < n->list.len; i++) {
            Node *item = n->list.data[i];
            size_t index = item->integer;
            Member m = t->kind == TY_ARRAY ? (Member){.type=t->base, .offset=index * t->base->size}
                                          : t->members.data[index];
            if (m.bit_field) {
                error(c, item->loc, "static bit-field initializers are not supported");
                return;
            }
            Type *element = m.type;
            if (element->kind == TY_ARRAY && !element->complete &&
                item->left->type && item->left->type->kind == TY_ARRAY)
                element = item->left->type;
            padding(out, m.offset - end);
            data_initializer(c, out, element, item->left);
            end = m.offset + element->size;
        }
        if (t->size > end)
            padding(out, t->size - end);
        return;
    }
    if (t->kind == TY_ARRAY && n->kind == N_SYMBOL && n->symbol->bytes) {
        size_t length = n->symbol->byte_count < t->size ? n->symbol->byte_count : t->size;
        bytes(out, n->symbol->bytes, length);
        padding(out, t->size - length);
        return;
    }
    uint64_t value = 0;
    Symbol *sym = NULL;
    long double real;
    bool valid = t->kind == TY_PTR ? address_constant(c, n, &sym, &value) :
                 t->kind != TY_FLOAT ? constant_int(c, n, &value) :
                 real_constant(c, n, &real);
    if (!valid) {
        error(c, n->loc, "unsupported or nonconstant static initializer");
        return;
    }
    if (t->kind == TY_FLOAT && t->size == 16) {
        uint64_t bits[2];
        if (extended_float_bits(c, real, bits, n->loc))
            fprintf(out, "\t.quad %" PRIu64 ", %" PRIu64 "\n", bits[0], bits[1]);
        return;
    }
    if (t->kind == TY_FLOAT)
        value = float_bits(real, t->size);
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
    fputs(s->thread_local_storage ? (s->init ? "\t.section .tdata,\"awT\",%progbits\n"
                                            : "\t.section .tbss,\"awT\",%nobits\n") :
          s->read_only ? "\t.section .rodata\n" :
          s->init ? "\t.data\n" : "\t.bss\n", out);
    if (!s->internal)
        fprintf(out, "\t.global %s\n", s->assembly);
    fprintf(out, "\t.balign %u\n\t.type %s, %%%s\n%s:\n", t->align, s->assembly,
            s->thread_local_storage ? "tls_object" : "object", s->assembly);
    if (s->bytes)
        bytes(out, s->bytes, s->byte_count);
    else
        data_initializer(c, out, t, s->init);
    fprintf(out, "\t.size %s, .-%s\n", s->assembly, s->assembly);
}
