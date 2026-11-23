#include "compiler.h"
#include "parser.h"
#include "integer_bits.h"
#include "integer_overflow.h"
#include "int128_float.h"
#include "inline_asm.h"
#include "complex_ops.h"
#include "quad.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__aarch64__)
extern Float128 strtof128(const char *restrict, char **restrict);
#endif

unsigned floating_format(const Target *target, Type *t)
{
    if (t->bfloat)
        return FP_BFLOAT16;
    return t->size == 16 && target == &x86_64_linux && t->float_variant != FLOAT128 ? 10 : t->size;
}

bool binary128(const Target *target, Type *t)
{
    return t->kind == TY_FLOAT && floating_format(target, t) == 16;
}

unsigned builtin_float_format(const Target *target, Type *type)
{
    return type->kind == TY_DECIMAL ? FP_DECIMAL_FORMAT + type->size : floating_format(target, type);
}

typedef struct { const char *name; Type *type; } Tag;
struct Scope {
    Scope *parent;
    Symbols names, labels, bound_locals;
    VECTOR(Tag) tags;
    bool aligned_alloca, parameters;
};
typedef struct { Type *record; size_t count; } DeferredRecordBounds;

struct RecordBoundsScope {
    RecordBoundsScope *parent;
    VECTOR(DeferredRecordBounds) consumed;
};

struct EnumDefinition {
    EnumDefinition *previous;
    Type *type;
    Type *last_constant_type;
    Symbols constants;
    Loc loc;
    Integer128 next, largest, negative_magnitude;
    bool next_negative, next_overflow, has_wide_value, packed;
    const char *mode;
};

static Node *initialized_literal(Context *, Type *, Node *, Loc);
static Symbol *frame_local(Context *, Symbol *, const char *, Type *, Loc);
static Type *save_array_sizes(Context *, Type *, Nodes *, Loc);
static bool variably_modified(Type *);
static Node *keep_evaluation_precision(Context *, Node *, Type *);

static Type *type(Context *c, TypeKind kind, unsigned size, unsigned align)
{
    Type *t = alloc(c, sizeof *t);
    *t = (Type){.kind = kind, .size = size, .align = align, .complete = true};
    return t;
}

Type *type_int(Context *c, unsigned rank, bool uns)
{
    const Target *t = c->target;
    unsigned sizes[] = {1, t->char_size, t->short_size, t->int_size, t->long_size, t->long_long_size, 16};
    Type *result = type(c, rank ? TY_INT : TY_BOOL, sizes[rank], sizes[rank]);
    result->rank = rank;
    result->uns = uns || !rank;
    return result;
}

Type *type_float(Context *c, unsigned size)
{
    return type(c, TY_FLOAT, size, size);
}

Type *type_decimal(Context *c, unsigned size)
{
    return type(c, TY_DECIMAL, size, size);
}

Type *type_extended_float(Context *c, FloatVariant variant)
{
    unsigned size = variant == FLOAT32 ? 4 : variant == FLOAT64 || variant == FLOAT32X ? 8 : 16;
    Type *t = type_float(c, size);
    t->float_variant = variant;
    return t;
}

FloatVariant interchange_float_variant(const char *suffix)
{
    static const struct { const char *suffix; FloatVariant kind; } types[] = {
        {"32", FLOAT32}, {"64", FLOAT64}, {"32x", FLOAT32X}, {"64x", FLOAT64X}, {"128", FLOAT128},
    };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (equal(suffix, types[i].suffix))
            return types[i].kind;
    return FLOAT_DEFAULT;
}

Type *type_bfloat(Context *c)
{
    Type *t = type_float(c, 2);
    t->bfloat = true;
    return t;
}

static void floating_extension(Context *c, Loc loc, const char *name)
{
    if (c->extension_depth || suppress_system_diagnostic(c, loc))
        return;
    warning_at(c, WARNING_PEDANTIC, loc, "ISO C does not support %s", name);
}

void interchange_float_extension(Context *c, Loc loc, const char *name)
{
    if (c->dialect.standard < C23)
        floating_extension(c, loc, name);
}

Type *pointer(Context *c, Type *base)
{
    Type *t = type(c, TY_PTR, c->target->pointer_size, c->target->pointer_size);
    t->base = base;
    t->uns = true;
    return t;
}

static Type *array(Context *c, Type *base, size_t length, bool complete, Loc loc)
{
    if (base->kind == TY_VOID || base->kind == TY_FUNC || !base->complete)
        error(c, loc, "invalid array element type");
    if (base->size % base->align)
        error(c, loc, "array element size is not a multiple of its alignment");
    if (length && base->size > UINT_MAX / length) {
        error(c, loc, "array size is too large");
        length = 0;
    }
    Type *t = type(c, TY_ARRAY, (unsigned)(base->size * length), base->align);
    t->base = base;
    t->length = length;
    t->complete = complete;
    t->variable_size = base->variable_size;
    return t;
}

static Type *complex_type(Context *c, Type *base)
{
    Type *t = type(c, TY_COMPLEX, base->size * 2, base->align);
    t->base = unqualified_type(c, base);
    return t;
}

static Type *real_component(Type *t)
{
    return t->kind == TY_COMPLEX ? t->base : t;
}

bool scalar(Type *t)
{
    return integer_type(t) || (t && (t->kind == TY_PTR || t->kind == TY_FLOAT ||
                                    t->kind == TY_DECIMAL || t->kind == TY_COMPLEX));
}

static bool default_float_promotion(Type *t)
{
    return t->kind == TY_FLOAT &&
           (t->storage_float || (t->size == 4 && t->float_variant == FLOAT_DEFAULT));
}

static bool promoted_parameter(Type *t)
{
    return (integer_type(t) && t->rank < 3) || default_float_promotion(t);
}

static bool compatible_array_lengths(Type *a, Type *b)
{
    return !a->complete || !b->complete || a->variable_bound || b->variable_bound ||
           a->array_star || b->array_star || a->length == b->length;
}

static bool compatible(Type *, Type *, bool);

static bool transparent_parameter_matches(Type *type, Type *other)
{
    if (!type->transparent_union)
        return false;
    for (size_t i = 0; i < type->members.len; i++)
        if (compatible(type->members.data[i].type, other, false))
            return true;
    return false;
}

static bool transparent_parameter_conversion(Type *a, Type *b)
{
    return transparent_parameter_matches(a, b) || transparent_parameter_matches(b, a);
}

static bool transparent_function_conversion(Type *a, Type *b)
{
    if (a->kind != TY_FUNC || b->kind != TY_FUNC || !a->prototype || !b->prototype ||
        a->members.len != b->members.len)
        return false;
    for (size_t i = 0; i < a->members.len; i++) {
        Type *left = a->members.data[i].type;
        Type *right = b->members.data[i].type;
        if (!compatible(left, right, false) && transparent_parameter_conversion(left, right))
            return true;
    }
    return false;
}

static void warn_transparent_redeclaration(Context *c, Type *a, Type *b, Loc loc)
{
    if (!c->extension_depth && transparent_function_conversion(a, b))
        warning_at(c, WARNING_PEDANTIC, loc, "function types with transparent union parameters differ in ISO C");
}

static bool compatible(Type *a, Type *b, bool check_qualifiers)
{
    if (a == b)
        return true;
    if (!a || !b || a->kind != b->kind ||
        (check_qualifiers && a->qualifiers != b->qualifiers))
        return false;
    if (a->mode_enum || b->mode_enum)
        return a->mode_enum == b->mode_enum && a->rank == b->rank && a->uns == b->uns;
    if (a->enumeration && b->enumeration)
        return a->base == b->base;
    switch (a->kind) {
    case TY_PTR: return a->noreturn_pointer == b->noreturn_pointer && compatible(a->base, b->base, true);
    case TY_COMPLEX: return compatible(a->base, b->base, false);
    case TY_VECTOR: return a->size == b->size && compatible(a->base, b->base, false);
    case TY_ARRAY: return compatible_array_lengths(a, b) && compatible(a->base, b->base, true);
    case TY_FUNC:
        if (!compatible(a->base, b->base, true))
            return false;
        if (!a->prototype && !b->prototype)
            return true;
        if (!a->prototype || !b->prototype) {
            Type *prototype = a->prototype ? a : b;
            if (prototype->variadic)
                return false;
            for (size_t i = 0; i < prototype->members.len; i++)
                if (promoted_parameter(prototype->members.data[i].type))
                    return false;
            return true;
        }
        if (a->variadic != b->variadic || a->members.len != b->members.len)
            return false;
        for (size_t i = 0; i < a->members.len; i++) {
            Type *left = a->members.data[i].type;
            Type *right = b->members.data[i].type;
            if (compatible(left, right, false))
                continue;
            if (!transparent_parameter_conversion(left, right))
                return false;
        }
        return true;
    case TY_STRUCT: case TY_UNION: return a->base && a->base == b->base;
    default: return a->size == b->size && a->uns == b->uns && a->rank == b->rank &&
                    a->plain_char == b->plain_char && a->storage_float == b->storage_float &&
                    a->bfloat == b->bfloat && a->float_variant == b->float_variant;
    }
}

bool same_type(Type *a, Type *b)
{
    return compatible(a, b, false);
}

static void set_qualifiers(Type *t, unsigned qualifiers)
{
    if (t->qualifiers & S_ATOMIC)
        t->align = t->nonatomic_align;
    t->qualifiers = qualifiers;
    if (qualifiers & S_ATOMIC) {
        t->nonatomic_align = t->align;
        if (t->size && t->size <= 16 && !(t->size & (t->size - 1)) && t->size > t->align)
            t->align = t->size;
    }
}

static Type *vector_type(Context *c, Type *base, unsigned size, Loc loc)
{
    if ((base->kind != TY_INT && base->kind != TY_FLOAT) ||
        !base->size || size < base->size || size % base->size) {
        error(c, loc, "vector_size requires integer or floating elements and a whole number of lanes");
        return base;
    }
    Type *t = type(c, TY_VECTOR, size, size > 16 ? 16 : size);
    t->base = unqualified_type(c, base);
    t->length = size / base->size;
    set_qualifiers(t, base->qualifiers);
    return t;
}

static void set_typedef_alignment(Type *t, unsigned alignment)
{
    if (!alignment)
        return;
    unsigned qualifiers = t->qualifiers;
    set_qualifiers(t, qualifiers & ~(unsigned)S_ATOMIC);
    if (!t->typedef_alignment)
        t->natural_alignment = t->align;
    t->typedef_alignment = t->align = alignment;
    set_qualifiers(t, qualifiers);
}

static bool tagged_type(Type *t)
{
    return t->kind == TY_STRUCT || t->kind == TY_UNION || t->enumeration;
}

static Type *tagged_variant(Context *c, Type *definition, unsigned qualifiers, unsigned alignment)
{
    for (Type *t = definition; t; t = t->qualified_next)
        if (t->qualifiers == qualifiers && t->typedef_alignment == alignment)
            return t;
    Type *result = alloc(c, sizeof *result);
    *result = *definition;
    set_typedef_alignment(result, alignment);
    set_qualifiers(result, qualifiers);
    definition->qualified_next = result;
    return result;
}

static Type *qualified(Context *c, Type *t, unsigned qualifiers)
{
    if (t->kind == TY_ARRAY && qualifiers) {
        Type *array_type = alloc(c, sizeof *array_type);
        *array_type = *t;
        array_type->base = qualified(c, t->base, t->base->qualifiers | qualifiers);
        return array_type;
    }
    if (t->qualifiers == qualifiers)
        return t;
    if (tagged_type(t))
        return tagged_variant(c, t->base, qualifiers, t->typedef_alignment);
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    set_qualifiers(result, qualifiers);
    return result;
}

Type *aligned_type(Context *c, Type *t, unsigned alignment)
{
    if (!alignment || t->kind == TY_FUNC)
        return t;
    if (tagged_type(t))
        return tagged_variant(c, t->base, t->qualifiers, alignment);
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    set_typedef_alignment(result, alignment);
    return result;
}

static Type *without_type_alignment(Context *c, Type *t)
{
    if (!t->typedef_alignment)
        return t;
    if (tagged_type(t))
        return tagged_variant(c, t->base, t->qualifiers, 0);
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    set_qualifiers(result, t->qualifiers & ~(unsigned)S_ATOMIC);
    result->align = t->natural_alignment;
    result->typedef_alignment = result->natural_alignment = 0;
    set_qualifiers(result, t->qualifiers);
    return result;
}

Type *unqualified_type(Context *c, Type *t)
{
    if (t->kind != TY_ARRAY)
        return qualified(c, t, 0);
    Type *base = unqualified_type(c, t->base);
    if (base == t->base)
        return t;
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    result->base = base;
    return result;
}

static void update_qualified_types(Type *definition)
{
    for (Type *t = definition->qualified_next; t; t = t->qualified_next) {
        Type *next = t->qualified_next;
        unsigned qualifiers = t->qualifiers;
        unsigned alignment = t->typedef_alignment;
        *t = *definition;
        set_typedef_alignment(t, alignment);
        set_qualifiers(t, qualifiers);
        t->qualified_next = next;
    }
}

Type *atomic_type(Context *c, Type *t, Loc loc)
{
    if (t->qualifiers || t->variable_size || t->kind == TY_ARRAY || t->kind == TY_FUNC || t->kind == TY_VOID) {
        error(c, loc, "_Atomic requires an unqualified object type");
        return t;
    }
    return qualified(c, t, S_ATOMIC);
}

static void check_restrict(Context *c, Type *t, unsigned qualifiers, Loc loc)
{
    if ((qualifiers & S_RESTRICT) && (t->kind != TY_PTR || t->base->kind == TY_FUNC))
        error(c, loc, "restrict requires a pointer to an object or incomplete type");
}

static bool same_allocation_size(AllocationSize a, AllocationSize b)
{
    return a.first == b.first && a.second == b.second;
}

static bool same_sentinel(Sentinel a, Sentinel b)
{
    return a.explicit_position == b.explicit_position &&
           a.position.lo == b.position.lo && a.position.hi == b.position.hi;
}

static bool has_sentinel(Sentinels sentinels, Sentinel wanted)
{
    for (size_t i = 0; i < sentinels.len; i++)
        if (same_sentinel(sentinels.data[i], wanted))
            return true;
    return false;
}

static bool same_sentinels(Sentinels a, Sentinels b)
{
    if (a.len != b.len)
        return false;
    for (size_t i = 0; i < a.len; i++)
        if (!same_sentinel(a.data[i], b.data[i]))
            return false;
    return true;
}

Sentinels combine_sentinels(Context *c, Sentinels a, Sentinels b)
{
    if (!a.len || !b.len || same_sentinels(a, b))
        return a.len ? a : b;
    Sentinels result = {0};
    for (size_t i = 0; i < a.len; i++)
        PUSH(c, result, a.data[i]);
    for (size_t i = 0; i < b.len; i++)
        if (!has_sentinel(a, b.data[i]))
            PUSH(c, result, b.data[i]);
    return result;
}

static Type *with_function_type(Context *c, Type *t, Type *fn)
{
    if (t->kind == TY_FUNC)
        return fn;
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    result->base = fn;
    return result;
}

static bool same_nonnull_clause(ParameterIndices a, ParameterIndices b)
{
    return a.len == b.len && (!a.len || !memcmp(a.data, b.data, a.len * sizeof *a.data));
}

static bool same_nullability(FunctionNullability a, FunctionNullability b)
{
    if (a.returns_nonnull != b.returns_nonnull || a.nonnull.len != b.nonnull.len)
        return false;
    for (size_t i = 0; i < a.nonnull.len; i++)
        if (!same_nonnull_clause(a.nonnull.data[i], b.nonnull.data[i]))
            return false;
    return true;
}

FunctionNullability combine_nullability(Context *c, FunctionNullability a, FunctionNullability b)
{
    FunctionNullability result = {.returns_nonnull = a.returns_nonnull || b.returns_nonnull};
    if (!a.nonnull.len || !b.nonnull.len || same_nullability(a, b)) {
        result.nonnull = a.nonnull.len ? a.nonnull : b.nonnull;
        return result;
    }
    for (size_t i = 0; i < a.nonnull.len; i++)
        PUSH(c, result.nonnull, a.nonnull.data[i]);
    for (size_t i = 0; i < b.nonnull.len; i++) {
        ParameterIndices clause = b.nonnull.data[i];
        bool duplicate = false;
        for (size_t j = 0; j < a.nonnull.len; j++)
            duplicate |= same_nonnull_clause(a.nonnull.data[j], clause);
        if (!duplicate)
            PUSH(c, result.nonnull, clause);
    }
    return result;
}

static bool valid_nonnull_clause(Context *c, Type *fn, ParameterIndices clause, Loc loc)
{
    if (!fn->prototype) {
        if (clause.len)
            return true;
        error(c, loc, "'nonnull' attribute without arguments requires a prototype");
        return false;
    }
    for (size_t i = 0; i < clause.len; i++) {
        unsigned index = clause.data[i];
        if (index > fn->members.len) {
            warning_at(c, WARNING_ATTRIBUTES, loc,
                       "'nonnull' position %u exceeds the number of function parameters", index);
            return false;
        }
        if (fn->members.data[index - 1].type->kind != TY_PTR) {
            warning_at(c, WARNING_ATTRIBUTES, loc, "'nonnull' position %u requires a pointer parameter", index);
            return false;
        }
    }
    return true;
}

Type *nullability_type(Context *c, Type *t, FunctionNullability attributes, Loc loc)
{
    if (!attributes.nonnull.len && !attributes.returns_nonnull)
        return t;
    Type *fn = t->kind == TY_PTR ? t->base : t;
    if (fn->kind != TY_FUNC) {
        if (attributes.nonnull.len)
            warning_at(c, WARNING_ATTRIBUTES, loc, "'nonnull' attribute only applies to function types");
        if (attributes.returns_nonnull)
            warning_at(c, WARNING_ATTRIBUTES, loc, "'returns_nonnull' attribute only applies to function types");
        return t;
    }
    if (same_nullability(attributes, fn->nullability))
        return t;
    FunctionNullability accepted = {.returns_nonnull = attributes.returns_nonnull};
    if (accepted.returns_nonnull && fn->base->kind != TY_PTR) {
        error(c, loc, "'returns_nonnull' attribute requires a function returning a pointer");
        accepted.returns_nonnull = false;
    }
    for (size_t i = 0; i < attributes.nonnull.len; i++)
        if (valid_nonnull_clause(c, fn, attributes.nonnull.data[i], loc))
            PUSH(c, accepted.nonnull, attributes.nonnull.data[i]);
    FunctionNullability merged = combine_nullability(c, fn->nullability, accepted);
    if (same_nullability(merged, fn->nullability))
        return t;
    Type *annotated = alloc(c, sizeof *annotated);
    *annotated = *fn;
    annotated->nullability = merged;
    return with_function_type(c, t, annotated);
}

bool nonnull_parameter(Type *fn, size_t index)
{
    for (size_t i = 0; i < fn->nullability.nonnull.len; i++) {
        ParameterIndices clause = fn->nullability.nonnull.data[i];
        if (!clause.len)
            return true;
        for (size_t j = 0; j < clause.len; j++)
            if (clause.data[j] == index)
                return true;
    }
    return false;
}

static const char *const access_modes[] = {
    [ACCESS_NONE] = "none", [ACCESS_READ] = "read_only",
    [ACCESS_WRITE] = "write_only", [ACCESS_READ_WRITE] = "read_write"
};

static const AccessClause *find_access(const AccessClauses *clauses, unsigned reference)
{
    for (size_t i = 0; clauses && i < clauses->len; i++)
        if (clauses->data[i].reference == reference)
            return &clauses->data[i];
    return NULL;
}

const AccessClauses *combine_accesses(Context *c, const AccessClauses *prior, const AccessClauses *next,
                                     bool diagnose)
{
    if (!prior || !next || prior == next)
        return prior ? prior : next;
    AccessClauses *combined = NULL;
    for (size_t i = 0; i < next->len; i++) {
        AccessClause clause = next->data[i];
        const AccessClause *old = find_access(prior, clause.reference);
        if (old) {
            if (diagnose && (old->mode != clause.mode || old->size != clause.size) &&
                warning_enabled(c, WARNING_ATTRIBUTES, clause.loc)) {
                warning_at(c, WARNING_ATTRIBUTES, clause.loc, "conflicting 'access' attributes for argument %u",
                           clause.reference);
                note(c, old->loc, "previous 'access' attribute here");
            }
            continue;
        }
        if (!combined) {
            combined = alloc(c, sizeof *combined);
            for (size_t j = 0; j < prior->len; j++)
                PUSH(c, *combined, prior->data[j]);
        }
        PUSH(c, *combined, clause);
    }
    return combined ? combined : prior;
}

static bool access_position(Context *c, Node *arg, unsigned *index, Loc loc)
{
    Integer128 value;
    if (!constant_integer(c, arg, &value) || value.hi || !value.lo || value.lo > UINT_MAX) {
        error(c, loc, "'access' attribute positions must be positive integer parameter numbers");
        return false;
    }
    *index = (unsigned)value.lo;
    return true;
}

static bool make_access_clause(Context *c, const char *mode, Node *reference, Node *size, Loc loc, AccessClause *out)
{
    size_t length = mode ? strlen(mode) : 0;
    if (length > 4 && !strncmp(mode, "__", 2) && !strcmp(mode + length - 2, "__")) {
        mode += 2;
        length -= 4;
    }
    for (unsigned i = 0; i < sizeof access_modes / sizeof *access_modes; i++) {
        if (strlen(access_modes[i]) != length || strncmp(mode, access_modes[i], length))
            continue;
        *out = (AccessClause){.mode = (AccessMode)i, .loc = loc};
        return access_position(c, reference, &out->reference, loc) &&
               (!size || access_position(c, size, &out->size, loc));
    }
    error(c, loc, "'access' attribute mode must be read_only, write_only, read_write, or none");
    return false;
}

const AccessClauses *access_spec(Context *c, const char *mode, Node *reference, Node *size, Loc loc)
{
    AccessClause clause;
    if (!make_access_clause(c, mode, reference, size, loc, &clause))
        return NULL;
    AccessClauses *clauses = alloc(c, sizeof *clauses);
    PUSH(c, *clauses, clause);
    return clauses;
}

static bool valid_access_clause(Context *c, Type *fn, AccessClause clause)
{
    Loc loc = clause.loc;
    if (clause.reference > fn->members.len || clause.size > fn->members.len) {
        error(c, loc, "'access' attribute position exceeds the number of function parameters");
        return false;
    }
    Type *parameter = fn->members.data[clause.reference - 1].type;
    if (parameter->kind != TY_PTR || parameter->base->kind == TY_FUNC) {
        error(c, loc, "'access' attribute argument %u must have object pointer type", clause.reference);
        return false;
    }
    Type *element = parameter->base;
    while (element->kind == TY_ARRAY)
        element = element->base;
    if ((clause.mode == ACCESS_WRITE || clause.mode == ACCESS_READ_WRITE) && (element->qualifiers & S_CONST)) {
        error(c, loc, "'access' attribute cannot write through const-qualified argument %u", clause.reference);
        return false;
    }
    if (clause.size && !integer_type(fn->members.data[clause.size - 1].type)) {
        error(c, loc, "'access' attribute size argument %u must have integer type", clause.size);
        return false;
    }
    return true;
}

Type *access_type(Context *c, Type *t, const AccessClauses *clauses)
{
    if (!clauses)
        return t;
    Type *fn = t->kind == TY_PTR ? t->base : t;
    if (fn->kind != TY_FUNC) {
        warning_at(c, WARNING_ATTRIBUTES, clauses->data[0].loc, "'access' attribute only applies to function types");
        return t;
    }
    if (clauses == fn->accesses)
        return t;
    for (size_t i = 0; i < clauses->len; i++)
        if (!valid_access_clause(c, fn, clauses->data[i]))
            return t;
    const AccessClauses *merged = combine_accesses(c, fn->accesses, clauses, true);
    if (merged == fn->accesses)
        return t;
    Type *annotated = alloc(c, sizeof *annotated);
    *annotated = *fn;
    annotated->accesses = merged;
    return with_function_type(c, t, annotated);
}

Type *sentinel_type(Context *c, Type *t, Sentinels sentinels, Loc loc)
{
    if (!sentinels.len)
        return t;
    Type *fn = t->kind == TY_PTR ? t->base : t;
    if (fn->kind != TY_FUNC)
        warning_at(c, WARNING_ATTRIBUTES, loc, "'sentinel' attribute only applies to function types");
    else if (!fn->prototype)
        warning_at(c, WARNING_ATTRIBUTES, loc, "'sentinel' attribute requires a prototype with named arguments");
    else if (!fn->variadic)
        warning_at(c, WARNING_ATTRIBUTES, loc, "'sentinel' attribute only applies to variadic functions");
    else {
        Sentinels merged = combine_sentinels(c, sentinels, fn->sentinels);
        if (same_sentinels(merged, fn->sentinels))
            return t;
        Type *annotated = alloc(c, sizeof *annotated);
        *annotated = *fn;
        annotated->sentinels = merged;
        return with_function_type(c, t, annotated);
    }
    return t;
}

Type *allocation_size_type(Context *c, Type *t, AllocationSize allocation, Loc loc)
{
    if (!allocation.first)
        return t;
    Type *fn = t->kind == TY_PTR ? t->base : t;
    if (fn->kind != TY_FUNC || fn->base->kind != TY_PTR) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "alloc_size requires a function type returning a pointer");
        return t;
    }
    unsigned positions[] = {allocation.first, allocation.second};
    for (size_t i = 0; i < 2 && positions[i]; i++) {
        unsigned index = positions[i];
        if (fn->prototype && index > fn->members.len) {
            warning_at(c, WARNING_ATTRIBUTES, loc, "alloc_size argument %u exceeds the number of parameters", index);
            return t;
        }
        if (index <= fn->members.len && !integer_type(fn->members.data[index - 1].type)) {
            warning_at(c, WARNING_ATTRIBUTES, loc, "alloc_size argument %u requires an integer parameter", index);
            return t;
        }
    }
    if (same_allocation_size(fn->allocation, allocation))
        return t;
    Type *annotated = alloc(c, sizeof *annotated);
    *annotated = *fn;
    annotated->allocation = allocation;
    return with_function_type(c, t, annotated);
}

static Type *warn_unused_result_type(Context *c, Type *t, Loc loc)
{
    Type *fn = t->kind == TY_PTR ? t->base : t;
    if (fn->kind != TY_FUNC || fn->base->kind == TY_VOID) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "warn_unused_result requires a function returning a value");
        return t;
    }
    if (fn->warn_unused_result)
        return t;
    Type *annotated = alloc(c, sizeof *annotated);
    *annotated = *fn;
    annotated->warn_unused_result = true;
    return with_function_type(c, t, annotated);
}

static Type *composite_type(Context *c, Type *a, Type *b, bool expression)
{
    if (a == b)
        return a;
    if (a->typedef_alignment != b->typedef_alignment) {
        a = without_type_alignment(c, a);
        b = without_type_alignment(c, b);
    }
    switch (a->kind) {
    case TY_PTR: {
        Type *base = composite_type(c, a->base, b->base, expression);
        if (a->base == base)
            return a;
        Type *result = pointer(c, base);
        set_qualifiers(result, a->qualifiers);
        result->noreturn_pointer = a->noreturn_pointer;
        return aligned_type(c, result, a->typedef_alignment);
    }
    case TY_ARRAY: {
        Type *shape = a->complete ? a : b;
        Type *base = composite_type(c, a->base, b->base, expression);
        if (shape->base == base)
            return shape;
        Type *result = alloc(c, sizeof *result);
        *result = *shape;
        result->base = base;
        if (!shape->typedef_alignment)
            result->align = base->align;
        return result;
    }
    case TY_FUNC: {
        Type *signature = a->prototype ? a : b;
        Type *result = alloc(c, sizeof *result);
        *result = *signature;
        result->warn_unused_result = a->warn_unused_result || b->warn_unused_result;
        result->nullability = combine_nullability(c, a->nullability, b->nullability);
        result->accesses = expression ? combine_accesses(c, a->accesses, b->accesses, false) :
                                        combine_accesses(c, b->accesses, a->accesses, true);
        if (expression)
            result->nullability.returns_nonnull = a->nullability.returns_nonnull && b->nullability.returns_nonnull;
        /* Redeclarations preserve the established check. A conditional starts with its left operand. */
        result->sentinels = expression ? combine_sentinels(c, a->sentinels, b->sentinels) :
                                         combine_sentinels(c, b->sentinels, a->sentinels);
        if (expression && !same_allocation_size(a->allocation, b->allocation))
            result->allocation = (AllocationSize){0};
        else
            result->allocation = a->allocation.first ? a->allocation : b->allocation;
        result->base = composite_type(c, a->base, b->base, expression);
        if (a->prototype && b->prototype) {
            result->members = (Members){0};
            for (size_t i = 0; i < a->members.len; i++) {
                Member parameter = a->members.data[i];
                Type *other = b->members.data[i].type;
                if (compatible(parameter.type, other, false))
                    parameter.type = composite_type(c, parameter.type, other, expression);
                else if (transparent_parameter_matches(parameter.type, other))
                    parameter.type = other;
                PUSH(c, result->members, parameter);
            }
        }
        return result;
    }
    default:
        return a;
    }
}

Specs merge_specs(Context *c, Specs a, Specs b, Loc loc)
{
    unsigned builtin = (S_CONST - 1u) | S_INT128 | S_FLOAT16 | S_FP16;
    if ((a.type && (b.flags & builtin)) || (b.type && (a.flags & builtin)))
        error(c, loc, "invalid combination of type specifiers");
    if (b.type) {
        if (a.type)
            error(c, loc, "duplicate type specifier");
        a.type = b.type;
    }
    if (b.flags == S_LONG && (a.flags & S_LONG)) {
        if (a.flags & S_LLONG)
            error(c, loc, "too many 'long' specifiers");
        a.flags |= S_LLONG;
    } else {
        if (a.flags & b.flags)
            error(c, loc, "duplicate type specifier");
        a.flags |= b.flags;
    }
    return a;
}

Type *spec_type(Context *c, Specs s, Loc loc)
{
    unsigned f = s.flags & ((S_CONST - 1u) | S_INT128 | S_FLOAT16 | S_FP16);
    bool complex = (s.flags & S_COMPLEX) != 0;
    if (complex && !f && !s.type) {
        extension(c, loc, "_Complex without a base type");
        f = S_DOUBLE;
    }
    Type *t = s.type;
    if (s.flags & S_ENUM_FORWARD)
        error(c, loc, "a fixed enum forward declaration must stand alone");
    if (((s.flags & (S_TYPEDEF | S_EXTERN | S_STATIC | S_AUTO | S_REGISTER)) &
         ((s.flags & (S_TYPEDEF | S_EXTERN | S_STATIC | S_AUTO | S_REGISTER)) - 1)) != 0)
        error(c, loc, "conflicting storage classes");
    if (!t) {
        bool uns = f & S_UNSIGNED;
        bool bad = (f & S_SIGNED) && uns;
        if (f & S_INT128) {
            bad |= (f & ~(unsigned)(S_INT128 | S_SIGNED | S_UNSIGNED)) != 0;
            t = type_int(c, 6, uns);
        } else if (f & S_VOID) {
            bad |= f != S_VOID;
            t = type(c, TY_VOID, 0, 1);
        } else if (f & S_BOOL) {
            bad |= f != S_BOOL;
            t = type_int(c, 0, true);
        } else if (f & (S_FLOAT16 | S_FP16)) {
            bad |= f != S_FLOAT16 && f != S_FP16;
            t = type_float(c, 2);
            t->storage_float = f == S_FP16;
        } else if (f & (S_FLOAT | S_DOUBLE)) {
            unsigned width = f & S_LONG ? c->target->long_double_size : f == S_FLOAT ? 4 : 8;
            bad |= f != S_FLOAT && f != S_DOUBLE && f != (S_LONG | S_DOUBLE);
            t = type(c, TY_FLOAT, width, width);
        } else if (f & S_CHAR) {
            bad |= (f & ~(unsigned)(S_CHAR | S_SIGNED | S_UNSIGNED)) != 0;
            t = type_int(c, 1, uns || (!(f & S_SIGNED) && c->target->char_unsigned));
            t->plain_char = !(f & (S_SIGNED | S_UNSIGNED));
        } else if (f & S_SHORT) {
            bad |= (f & (S_LONG | S_LLONG)) != 0;
            t = type_int(c, 2, uns);
        } else {
            t = type_int(c, f & S_LLONG ? 5 : f & S_LONG ? 4 : 3, uns);
        }
        if (bad)
            error(c, loc, "invalid combination of type specifiers");
    }
    if (complex) {
        if (t->kind == TY_INT)
            extension(c, loc, "integer complex types");
        else if (t->kind != TY_FLOAT || t->storage_float) {
            error(c, loc, "invalid base type for _Complex");
            t = type(c, TY_FLOAT, 8, 8);
        }
        t = complex_type(c, t);
    }
    if (s.flags & S_PACKED) {
        if (t->enumeration)
            warning_at(c, WARNING_ATTRIBUTES, loc, "packed attribute ignored on this enum declaration");
        else
            record_attributes(c, t, true, 0, 0, loc);
    }
    unsigned qualifiers = s.flags & S_QUALIFIERS;
    check_restrict(c, t, qualifiers, loc);
    if ((qualifiers & S_ATOMIC) && (t->kind == TY_ARRAY || t->kind == TY_FUNC || t->kind == TY_VOID)) {
        error(c, loc, "_Atomic cannot qualify an array, function, or void");
        return t;
    }
    return qualified(c, t, t->qualifiers | qualifiers);
}

Decl *decl(Context *c, DeclKind kind, Decl *inner, Loc loc)
{
    Decl *d = alloc(c, sizeof *d);
    *d = (Decl){.kind = kind, .inner = inner, .loc = loc};
    return d;
}

Decl *decl_name(Decl *d)
{
    while (d && d->kind != D_NAME)
        d = d->inner;
    return d;
}

typedef struct {
    const char *name;
    unsigned rank, float_size;
    unsigned arm_lanes, x86_lanes;
} MachineMode;

static const MachineMode machine_modes[] = {
    {"QI", 1, 0, 8 | 16, 2 | 4 | 8 | 16 | 32 | 64 | 128},
    {"HI", 2, 0, 4 | 8 | 16, 2 | 4 | 8 | 16 | 32 | 64},
    {"SI", 3, 0, 2 | 4 | 8, 1 | 2 | 4 | 8 | 16 | 32 | 64},
    {"DI", 4, 0, 1 | 2 | 4 | 8, 1 | 2 | 4 | 8 | 16},
    {"TI", 6, 0, 2, 1 | 2 | 4 | 8},
    {"SD", 0, 4, 0, 0}, {"DD", 0, 8, 0, 0}, {"TD", 0, 16, 0, 0},
    {"BF", 0, 2, 4 | 8, 2 | 4 | 8 | 16 | 32 | 64},
    {"HF", 0, 2, 2 | 4 | 8, 2 | 4 | 8 | 16 | 32 | 64},
    {"SF", 0, 4, 2 | 4, 2 | 4 | 8 | 16 | 32 | 64},
    {"DF", 0, 8, 1 | 2, 2 | 4 | 8 | 16 | 32},
    {"XF", 0, 16, 0, 0},
    {"TF", 0, 16, 0, 2 | 4 | 8 | 16},
};

static Type *mode_type(Context *c, Type *t, const char *name, Loc loc)
{
    size_t length = strlen(name);
    if (length > 4 && !strncmp(name, "__", 2) && equal(name + length - 2, "__"))
        name = copy(c, name + 2, length - 4);
    const char *component = name;
    bool complex = false;
    unsigned long lanes = 0;
    if (name[0] == 'V' && isdigit((unsigned char)name[1])) {
        char *end;
        lanes = strtoul(name + 1, &end, 10);
        if (!lanes || lanes > 128 || (lanes & (lanes - 1)) || name[1] == '0') {
            error(c, loc, "unknown machine mode '%s'", name);
            return t;
        }
        component = end;
    } else if (name[0] == 'C' && strlen(name) == 3) {
        component = name + 1;
        complex = true;
    } else if (equal(name, "BC") || equal(name, "HC") || equal(name, "SC") || equal(name, "DC") ||
               equal(name, "XC") || equal(name, "TC")) {
        component = name[0] == 'B' ? "BF" : name[0] == 'H' ? "HF" : name[0] == 'S' ? "SF" :
                    name[0] == 'D' ? "DF" : name[0] == 'X' ? "XF" : "TF";
        complex = true;
    } else if (equal(name, "byte")) {
        component = "QI";
    } else if (equal(name, "word") || equal(name, "pointer")) {
        component = "DI";
    }
    bool x86 = c->target == &x86_64_linux;
    const MachineMode *m = NULL;
    for (size_t i = 0; i < sizeof machine_modes / sizeof *machine_modes; i++)
        if (equal(component, machine_modes[i].name)) {
            m = &machine_modes[i];
            break;
        }
    if (!m || (!x86 && equal(component, "XF")) ||
        (lanes && !(lanes & (x86 ? m->x86_lanes : m->arm_lanes))) ||
        (complex && name[0] == 'C' && !m->rank)) {
        error(c, loc, "unsupported machine mode '%s'", name);
        return t;
    }
    Type *source = complex && t->kind == TY_COMPLEX ? t->base : t;
    bool decimal = m->float_size && component[1] == 'D' && component[2] == 0;
    Type *base = m->rank ? type_int(c, m->rank, source->uns) :
                 decimal ? type_decimal(c, m->float_size) :
                 equal(component, "BF") ? type_bfloat(c) :
                 x86 && equal(component, "TF") ? type_extended_float(c, FLOAT128) : type_float(c, m->float_size);
    if (t->kind == TY_PTR && m->rank && !lanes && !complex) {
        if (base->size != t->size)
            error(c, loc, "invalid pointer mode '%s'", name);
        return t;
    }
    bool real_modes = (t->kind == TY_FLOAT || t->kind == TY_DECIMAL) &&
                      (base->kind == TY_FLOAT || base->kind == TY_DECIMAL);
    if (complex ? t->kind != TY_COMPLEX : (!real_modes && t->kind != base->kind) || (lanes && t->enumeration)) {
        error(c, loc, "mode '%s' applied to inappropriate type", name);
        return t;
    }
    Type *result = base;
    if (complex) {
        result = complex_type(c, base);
    } else if (lanes) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "vector machine modes are deprecated, use vector_size");
        result = vector_type(c, base, (unsigned)lanes * base->size, loc);
    } else if (t->enumeration) {
        result->enumeration = true;
        result->complete = t->complete;
        result->fixed_enum = t->fixed_enum;
        result->enum_defined = t->enum_defined;
        result->mode_enum = t->mode_enum ? t->mode_enum : t->base;
        result->base = result;
    }
    set_typedef_alignment(result, t->typedef_alignment);
    set_qualifiers(result, t->qualifiers);
    return result;
}

static Type *vector_sized_type(Context *c, Type *t, unsigned size, Loc loc)
{
    if (t->kind != TY_ARRAY && t->kind != TY_PTR && t->kind != TY_FUNC)
        return vector_type(c, t, size, loc);
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    result->base = vector_sized_type(c, t->base, size, loc);
    if (t->kind == TY_ARRAY) {
        if (t->length && result->base->size > UINT_MAX / t->length) {
            error(c, loc, "array size is too large");
            return t;
        }
        result->size = (unsigned)(result->base->size * t->length);
        if (!t->typedef_alignment)
            result->align = result->base->align;
    }
    return result;
}

static Type *apply_type_attributes(Context *c, Type *t, TypeAttributes attributes)
{
    for (size_t i = 0; i < attributes.len; i++) {
        TypeAttribute attr = attributes.data[i];
        t = attr.mode ? mode_type(c, t, attr.mode, attr.loc) :
                        vector_sized_type(c, t, attr.vector_size, attr.loc);
    }
    return t;
}

static Type *noreturn_pointer(Context *c, Type *t)
{
    if (t->noreturn_pointer)
        return t;
    Type *annotated = alloc(c, sizeof *annotated);
    *annotated = *t;
    annotated->noreturn_pointer = true;
    return annotated;
}

Type *apply_decl(Context *c, Type *base, Decl *d)
{
    if (!d)
        return base;
    Type *t = base;
    switch (d->kind) {
    case D_NAME: return base;
    case D_ALIGNED:
        base = apply_type_attributes(c, base, d->type_attributes);
        t = aligned_type(c, base, d->alignment);
        t = allocation_size_type(c, t, d->allocation, d->loc);
        t = sentinel_type(c, t, d->sentinels, d->loc);
        t = nullability_type(c, t, d->nullability, d->loc);
        t = access_type(c, t, d->accesses);
        if (d->attribute_markers & MARK_WARN_UNUSED_RESULT)
            t = warn_unused_result_type(c, t, d->loc);
        break;
    case D_POINTER:
        t = pointer(c, base);
        t = apply_type_attributes(c, t, d->type_attributes);
        set_qualifiers(t, d->qualifiers);
        check_restrict(c, t, t->qualifiers, d->loc);
        t = aligned_type(c, t, d->alignment);
        t = sentinel_type(c, t, d->sentinels, d->loc);
        t = nullability_type(c, t, d->nullability, d->loc);
        t = access_type(c, t, d->accesses);
        if (d->attribute_markers & MARK_NORETURN) {
            if (t->kind == TY_PTR && t->base->kind == TY_FUNC)
                t->noreturn_pointer = true;
            else
                warning_at(c, WARNING_ATTRIBUTES, d->loc, "noreturn attribute ignored on this pointer type");
        }
        if (d->attribute_markers & MARK_WARN_UNUSED_RESULT)
            t = warn_unused_result_type(c, t, d->loc);
        break;
    case D_ARRAY: {
        if (d->alignment || d->type_attributes.len)
            error(c, d->loc, "attributes inside array brackets require a parameter array declarator");
        uint64_t count = 0;
        bool variable = d->bound && !constant_int(c, d->bound, &count);
        if (variable && !integer_type(d->bound->type))
            error(c, d->loc, "array bound must be an integer expression");
        else if (!variable && d->bound && (int64_t)count <= 0) {
            if (count == 0)
                extension(c, d->loc, "zero-length array");
            else
                error(c, d->loc, "array bound must be positive");
            count = 0;
        }
        t = array(c, base, (size_t)count, d->bound || d->array_star, d->loc);
        if (d->array_star && !c->scope->parameters)
            error(c, d->loc, "[*] requires function prototype scope");
        t->array_star = d->array_star;
        t->variable_size |= d->array_star;
        if (variable) {
            t->variable_size = true;
            t->variable_bound = d->bound;
        }
        t->qualifiers = d->qualifiers;
        break;
    }
    case D_FUNCTION:
        if (base->kind == TY_ARRAY || base->kind == TY_FUNC)
            error(c, d->loc, "function cannot return an array or a function");
        t = type(c, TY_FUNC, 0, 1);
        t->base = base;
        t->prototype = d->prototype;
        t->variadic = d->variadic;
        t->members = d->params;
        t->parameter_scope = d->parameter_scope;
        if (t->members.len == 1 && t->members.data[0].type->kind == TY_VOID &&
            !t->members.data[0].name && !t->variadic)
            t->members.len = 0;
        for (size_t i = 0; i < t->members.len; i++)
            if (t->members.data[i].type->kind == TY_VOID)
                error(c, d->loc, "parameter cannot have void type");
        break;
    }
    return apply_decl(c, t, d->inner);
}

Type *declaration_type(Context *c, Specs s, Decl *d, Loc loc)
{
    Type *t = apply_decl(c, spec_type(c, s, loc), d);
    if (d)
        t = apply_type_attributes(c, t, d->member_type_attributes);
    t = apply_type_attributes(c, t, s.type_attributes);
    Sentinels sentinels = d ? combine_sentinels(c, d->member_sentinels, s.sentinels) : s.sentinels;
    t = sentinel_type(c, t, sentinels, loc);
    FunctionNullability attributes = d ? combine_nullability(c, d->member_nullability, s.nullability) : s.nullability;
    t = nullability_type(c, t, attributes, loc);
    if (d)
        t = access_type(c, t, d->member_accesses);
    t = access_type(c, t, s.accesses);
    if (s.attribute_markers & MARK_WARN_UNUSED_RESULT)
        t = warn_unused_result_type(c, t, loc);
    return t;
}

void enter_scope(Context *c)
{
    Scope *scope = alloc(c, sizeof *scope);
    scope->parent = c->scope;
    c->scope = scope;
}

void enter_parameter_scope(Context *c)
{
    enter_scope(c);
    c->scope->parameters = true;
}

void leave_scope(Context *c)
{
    for (size_t i = 0; i < c->scope->labels.len; i++) {
        Symbol *s = c->scope->labels.data[i];
        if (s->label_referenced && !s->defined)
            error(c, s->loc, "undefined label '%s'", s->name);
    }
    c->scope = c->scope->parent;
}

void finish_block_scope(Context *c, Node *block)
{
    if (c->scope->aligned_alloca)
        block->flags |= BLOCK_STACK_ALLOCATION;
    leave_scope(c);
}

static Symbol *find_in(Scope *scope, const char *name)
{
    for (size_t i = scope->names.len; i; i--)
        if (equal(scope->names.data[i - 1]->name, name))
            return scope->names.data[i - 1];
    return NULL;
}

static Symbol *find(Context *c, const char *name)
{
    for (Scope *s = c->scope; s; s = s->parent) {
        Symbol *result = find_in(s, name);
        if (result)
            return result;
    }
    return NULL;
}

static void warn_deprecated(Context *c, Loc loc, const char *name, const char *message)
{
    if (message)
        warning_at(c, WARNING_DEPRECATED_DECLARATIONS, loc, "'%s' is deprecated%s%s",
                   name, *message ? ": " : "", message);
}

void deprecated_type(Context *c, Type *t, const char *name, const char *message, Loc loc)
{
    if (!tagged_type(t))
        return;
    if (message)
        t->base->deprecated = message;
    else
        warn_deprecated(c, loc, name, t->base->deprecated);
}

Type *reference_typedef(Context *c, Token token, const PatchableEntries **patchable_entries)
{
    Symbol *symbol = find(c, token.text);
    if (!symbol || !symbol->is_typedef)
        return NULL;
    warn_deprecated(c, token.loc, token.text, symbol->deprecated);
    if (patchable_entries)
        *patchable_entries = symbol->patchable_entries;
    return symbol->type;
}

Type *find_typedef(Context *c, const char *name)
{
    Symbol *s = find(c, name);
    return s && s->is_typedef ? s->type : NULL;
}

static Symbol *symbol(Context *c, const char *name, Type *t, Loc loc)
{
    Symbol *s = alloc(c, sizeof *s);
    *s = (Symbol){.name = name, .assembly = name, .type = t, .loc = loc};
    return s;
}

Type *enum_type(Context *c, const char *name, Type *underlying, bool definition, Loc loc)
{
    Type *t = NULL;
    if (underlying && (!integer_type(underlying) || underlying->enumeration)) {
        error(c, loc, "invalid enum underlying type");
        underlying = type_int(c, 3, false);
    }
    if (name) {
        for (Scope *s = c->scope; s && !t; s = (definition || underlying) ? NULL : s->parent)
            for (size_t i = 0; i < s->tags.len; i++)
                if (equal(s->tags.data[i].name, name)) {
                    t = s->tags.data[i].type;
                    break;
                }
    }
    if (t && (!t->enumeration || (definition && t->enum_defined))) {
        error(c, loc, "conflicting declaration of enum '%s'", name);
        return underlying ? underlying : type_int(c, 3, false);
    }
    if (t && ((underlying && (!t->fixed_enum || !same_type(t, underlying))) ||
              (definition && !underlying && t->fixed_enum))) {
        error(c, loc, "conflicting underlying type for enum '%s'", name);
        return underlying ? underlying : type_int(c, 3, false);
    }
    if (!t && !definition && !underlying)
        extension(c, loc, "forward enum declaration");
    if (!t) {
        Type *base = underlying ? unqualified_type(c, underlying) : type_int(c, 3, true);
        t = type(c, base->kind, base->size, base->align);
        t->rank = base->rank;
        t->uns = base->uns;
        t->plain_char = base->plain_char;
        t->enumeration = true;
        t->fixed_enum = underlying != NULL;
        t->base = t;
        t->complete = t->fixed_enum;
        if (name) {
            Tag item = {name, t};
            PUSH(c, c->scope->tags, item);
        }
    }
    return t;
}

void begin_enum(Context *c, const char *name, Type *underlying, Specs attrs, Loc loc)
{
    EnumDefinition *enumeration = alloc(c, sizeof *enumeration);
    enumeration->previous = c->enumeration;
    enumeration->type = enum_type(c, name, underlying, true, loc);
    enumeration->loc = loc;
    if (attrs.deprecated)
        deprecated_type(c, enumeration->type, name, attrs.deprecated, loc);
    enumeration->packed = (attrs.flags & S_PACKED) != 0;
    if (attrs.type_attributes.len)
        enumeration->mode = attrs.type_attributes.data[attrs.type_attributes.len - 1].mode;
    c->enumeration = enumeration;
}

Type *end_enum(Context *c, Specs attrs)
{
    EnumDefinition *enumeration = c->enumeration;
    c->enumeration = enumeration->previous;
    Type *t = enumeration->type;
    if (!t->enumeration)
        return t;
    const char *mode = attrs.type_attributes.len ?
                       attrs.type_attributes.data[attrs.type_attributes.len - 1].mode : enumeration->mode;
    if (mode && t->fixed_enum)
        warning_at(c, WARNING_ATTRIBUTES, enumeration->loc, "mode attribute ignored on an enum with a fixed underlying type");
    if (!t->fixed_enum) {
        bool negative = wide_nonzero(enumeration->negative_magnitude);
        unsigned first = (attrs.flags & S_PACKED) || enumeration->packed || c->dialect.short_enums ? 1 : 3;
        Type *base = NULL;
        if (mode) {
            Type *integer = type_int(c, 3, !negative);
            base = mode_type(c, integer, mode, enumeration->loc);
            if (base->kind != TY_INT) {
                error(c, enumeration->loc, "mode '%s' is invalid for an enum", mode);
                base = integer;
            }
            first = base->rank;
        }
        for (unsigned rank = first; rank <= 6; rank++) {
            if (!mode)
                base = type_int(c, rank, !negative);
            unsigned width = base->size * CHAR_BIT;
            if (wide_compare(enumeration->largest, wide_limit(width, negative, false), true) <= 0 &&
                wide_compare(enumeration->negative_magnitude, wide_limit(width, negative, true), true) <= 0)
                break;
            if (mode) {
                error(c, enumeration->loc, "specified mode is too small for the enumerated values");
                break;
            }
            if (rank == 6)
                error(c, enumeration->loc, "enum values require an integer wider than 128 bits");
        }
        t->rank = base->rank;
        t->uns = base->uns;
        t->size = base->size;
        t->align = base->align;
        if (enumeration->has_wide_value)
            for (size_t i = 0; i < enumeration->constants.len; i++)
                enumeration->constants.data[i]->type = t;
    }
    t->complete = t->enum_defined = true;
    t->enum_packed = enumeration->packed || (attrs.flags & S_PACKED);
    if (attrs.deprecated)
        deprecated_type(c, t, NULL, attrs.deprecated, enumeration->loc);
    update_qualified_types(t);
    return t;
}

static void layout_record(Context *c, Type *t, Loc loc)
{
    bool is_union = t->kind == TY_UNION;
    bool forced = c->force_pack && t->record_pack;
    unsigned pack = forced ? 0 : t->record_pack;
    size_t cursor = 0, alignment = 1;
    for (size_t i = 0; i < t->members.len; i++) {
        Member *m = &t->members.data[i];
        bool zero_width = m->bit_field && !m->width;
        bool packed = t->record_packed || m->packed_attribute || forced;
        bool compact = packed || pack;
        unsigned limit = zero_width ? (c->force_pack ? 0 : c->default_pack_alignment) : pack;
        unsigned align = packed && !zero_width ? 1 : m->type->align;
        if (m->requested_alignment > align)
            align = m->requested_alignment;
        if (limit && limit < align)
            align = limit;
        m->alignment = align;
        m->offset = m->bit = 0;
        m->packed_bit_field = compact && m->bit_field && !zero_width;
        if ((!m->bit_field || m->name || c->target == &aarch64_linux) && align > alignment)
            alignment = align;
        if (zero_width) {
            if (!is_union)
                cursor = align_to(cursor, align * 8);
        } else if (m->bit_field) {
            if (is_union) {
                if (m->width > cursor)
                    cursor = m->width;
            } else {
                if (m->requested_alignment)
                    cursor = align_to(cursor, (pack && pack < m->requested_alignment ? pack
                                             : m->requested_alignment) * 8);
                size_t boundary = align * 8;
                size_t start = compact ? cursor / 8 * 8 : cursor / boundary * boundary;
                if (!compact && cursor - start + m->width > m->type->size * 8) {
                    cursor = align_to(cursor, boundary);
                    start = cursor;
                }
                m->offset = start / 8;
                m->bit = (unsigned)(cursor - start);
                cursor += m->width;
            }
        } else if (is_union) {
            if (m->type->size * (size_t)8 > cursor)
                cursor = m->type->size * (size_t)8;
        } else {
            m->offset = align_to((cursor + 7) / 8, align);
            cursor = (m->offset + m->type->size) * 8;
        }
    }
    t->member_alignment = (unsigned)alignment;
    if (t->record_alignment > alignment)
        alignment = t->record_alignment;
    size_t size = align_to((cursor + 7) / 8, alignment);
    if (size > UINT_MAX)
        error(c, loc, "aggregate size is too large");
    t->size = (unsigned)size;
    t->align = (unsigned)alignment;
    update_qualified_types(t);
}

Type *record_type(Context *c, bool is_union, const char *tag, Members *members, Loc loc)
{
    TypeKind kind = is_union ? TY_UNION : TY_STRUCT;
    Type *t = NULL;
    if (tag) {
        for (Scope *s = c->scope; s && !t; s = members ? NULL : s->parent)
            for (size_t i = 0; i < s->tags.len; i++)
                if (equal(s->tags.data[i].name, tag)) {
                    t = s->tags.data[i].type;
                    break;
                }
    }
    if (!t) {
        t = type(c, kind, 0, 1);
        t->base = t;
        t->complete = false;
        if (tag) {
            Tag item = {tag, t};
            PUSH(c, c->scope->tags, item);
        }
    } else if (t->kind != kind) {
        error(c, loc, "tag '%s' used with the wrong aggregate kind", tag);
        return t;
    }
    if (!members)
        return t;
    if (!members->len)
        extension(c, loc, "empty aggregate");
    if (t->complete) {
        error(c, loc, "redefinition of aggregate '%s'", tag);
        return t;
    }
    for (size_t i = 0; i < members->len; i++) {
        Member *m = &members->data[i];
        if (!m->name && !m->bit_field && m->type->kind != TY_STRUCT && m->type->kind != TY_UNION)
            error(c, loc, "unnamed member must be a structure or union");
        for (size_t j = 0; j < i; j++)
            if (m->name && members->data[j].name && equal(m->name, members->data[j].name))
                error(c, loc, "duplicate member '%s'", m->name);
        bool flexible = m->type->kind == TY_ARRAY && !m->type->complete && !is_union && i &&
                        i + 1 == members->len;
        if (m->type->kind == TY_VOID || m->type->kind == TY_FUNC ||
            (!m->type->complete && !flexible)) {
            error(c, loc, "member '%s' has an incomplete or invalid type", m->name);
            return t;
        }
    }
    t->members = *members;
    t->complete = true;
    t->record_pack = loc.pack_alignment;
    for (size_t i = 0; i < t->members.len; i++) {
        Type *member = t->members.data[i].type;
        t->variable_size |= member->variable_size;
        t->record_variably_modified |= variably_modified(member);
    }
    layout_record(c, t, loc);
    return t;
}

void record_attributes(Context *c, Type *t, bool packed, unsigned alignment, unsigned attributes, Loc loc)
{
    if ((t->kind != TY_STRUCT && t->kind != TY_UNION) || !t->complete) {
        error(c, loc, "record attribute requires a complete aggregate definition");
        return;
    }
    t = t->base;
    t->record_packed |= packed;
    t->record_alignment_attributes |= attributes;
    if (alignment > t->record_alignment)
        t->record_alignment = alignment;
    layout_record(c, t, loc);
}

Type *transparent_union_type(Context *c, Type *t, bool definition, Loc loc)
{
    if (t->kind != TY_UNION || !t->complete) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "transparent_union attribute ignored on this type");
        return t;
    }
    Type *first = t->members.len ? t->members.data[0].type : NULL;
    if (!first || t->record_variably_modified || !first->size || first->size != t->size ||
        t->members.data[0].bit_field ||
        !(integer_type(first) || first->kind == TY_PTR || aggregate(first))) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "union cannot be made transparent");
        return t;
    }
    if (definition) {
        t->base->transparent_union = true;
        update_qualified_types(t->base);
        return t;
    }
    /* Each attributed typedef has its own type identity and retains the union's layout. */
    Type *alias = alloc(c, sizeof *alias);
    *alias = *t;
    alias->base = alias;
    alias->qualified_next = NULL;
    alias->transparent_union = true;
    return alias;
}

void designated_init_type(Context *c, Type *t, bool definition, Loc loc)
{
    if (t->kind != TY_STRUCT) {
        error(c, loc, "designated_init attribute is only valid on structure types");
    } else if (definition) {
        t->base->designated_init = true;
        update_qualified_types(t->base);
    } else if (t->complete) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "designated_init attribute ignored after structure definition");
    }
}

static unsigned requested_alignment(Context *c, Specs s, Type *t, Loc loc)
{
    if (s.iso_alignment && s.iso_alignment < t->align)
        error(c, loc, "_Alignas cannot reduce the natural alignment of an object or member");
    return s.iso_alignment > s.gnu_alignment ? s.iso_alignment : s.gnu_alignment;
}

static const char *cleanup_name(Specs specs, Decl *d)
{
    const char *name = specs.cleanup;
    for (; !name && d; d = d->inner)
        name = d->cleanup;
    return name;
}

static Type *apply_noreturn(Context *c, Type *t, Decl *d, Specs *specs, bool parameter, Loc loc)
{
    bool is_typedef = (specs->flags & S_TYPEDEF) != 0;
    if (specs->noreturn_specifier) {
        if (t->kind != TY_FUNC || is_typedef || parameter)
            error(c, loc, "_Noreturn is only permitted on function declarations");
        else
            specs->attribute_markers |= MARK_NORETURN;
    }
    bool pointer_to_function = t->kind == TY_PTR && t->base->kind == TY_FUNC;
    if (pointer_to_function)
        for (Decl *part = d; part; part = part->inner)
            specs->attribute_markers |= part->attribute_markers & MARK_NORETURN;
    if (!(specs->attribute_markers & MARK_NORETURN))
        return t;
    if (pointer_to_function)
        return noreturn_pointer(c, t);
    if (t->kind == TY_FUNC && !is_typedef)
        return t;
    warning_at(c, WARNING_ATTRIBUTES, loc, "noreturn attribute ignored on this declaration");
    specs->attribute_markers &= ~(unsigned)MARK_NORETURN;
    return t;
}

static unsigned declarator_noplt(Specs specs, const Decl *d)
{
    unsigned marker = specs.attribute_markers & MARK_NOPLT;
    for (; d && !marker; d = d->inner)
        marker = d->attribute_markers & MARK_NOPLT;
    return marker;
}

void warn_invalid_noplt(Context *c, Specs specs, const Decl *d, Loc loc)
{
    if (declarator_noplt(specs, d))
        warning_at(c, WARNING_ATTRIBUTES, loc, "'noplt' attribute only applies to functions");
}

static const PatchableEntries *declarator_patchable_entries(Context *c, const PatchableEntries *entries, const Decl *d)
{
    for (; d; d = d->inner)
        entries = combine_patchable_entries(c, entries, d->patchable_entries);
    return entries;
}

void add_member(Context *c, Members *list, Specs s, Decl *d, bool parameter, Loc loc)
{
    warn_lifecycle_attributes(c, s, loc);
    warn_invalid_noplt(c, s, d, loc);
    warn_call_diagnostic_attributes(c, s.call_diagnostics, loc);
    if (s.visibility)
        warning_at(c, WARNING_ATTRIBUTES, loc, "visibility attribute ignored on %s", parameter ? "parameter" : "member");
    if (cleanup_name(s, d))
        warning_at(c, WARNING_ATTRIBUTES, loc, "cleanup attribute ignored on %s", parameter ? "parameter" : "member");
    if (s.section)
        warning_at(c, WARNING_ATTRIBUTES, loc, "section attribute ignored on %s", parameter ? "parameter" : "member");
    Decl *name = decl_name(d);
    Member m = {.name = name ? name->name : NULL, .packed_attribute =
                (s.flags & S_PACKED) || (d && d->packed_member)};
    s.flags &= ~(unsigned)S_PACKED;
    if (d && d->member_alignment > s.gnu_alignment)
        s.gnu_alignment = d->member_alignment;
    m.alignment_attributes = s.alignment_attributes | (d ? d->member_alignment_attributes : 0);
    if (parameter && d) {
        Decl *outer = d;
        while (outer->inner && outer->inner->kind != D_NAME)
            outer = outer->inner;
        if (outer->kind == D_ARRAY && (outer->alignment || outer->type_attributes.len)) {
            warning_at(c, WARNING_ATTRIBUTES, outer->loc, "attributes in parameter array declarator ignored");
            outer->alignment = 0;
            outer->type_attributes.len = 0;
        }
    }
    if (s.transparent_union)
        warning_at(c, WARNING_ATTRIBUTES, loc, "transparent_union attribute ignored on %s",
                   parameter ? "parameter" : "member");
    if (d)
        s.attribute_markers |= d->member_attribute_markers;
    m.type = declaration_type(c, s, d, loc);
    m.type = apply_noreturn(c, m.type, d, &s, parameter, loc);
    m.attribute_markers = s.attribute_markers & MARK_NORETURN;
    m.patchable_entries = declarator_patchable_entries(c, s.patchable_entries, d);
    if (s.designated_init || (d && d->designated_init))
        designated_init_type(c, m.type, false, loc);
    m.deprecated = d && d->deprecated ? d->deprecated : s.deprecated;
    AllocationSize allocation = d && d->member_allocation.first ? d->member_allocation : s.allocation;
    m.type = allocation_size_type(c, m.type, allocation, loc);
    if (d && d->bit_width) {
        uint64_t width = 0;
        m.bit_field = true;
        if (!integer_type(m.type) || (m.type->qualifiers & S_ATOMIC) ||
            !constant_int(c, d->bit_width, &width) ||
            width > (m.type->kind == TY_BOOL ? 1u : m.type->size * 8) || (!width && m.name))
            error(c, loc, "invalid bit-field width or type");
        else
            m.width = (unsigned)width;
    }
    if (parameter && (s.has_alignas || s.gnu_alignment))
        error(c, loc, "alignment cannot be specified for a parameter");
    else if (m.bit_field && s.has_alignas)
        error(c, loc, "_Alignas cannot be specified for a bit-field");
    else
        m.requested_alignment = requested_alignment(c, s, m.type, loc);
    PUSH(c, *list, m);
}

void add_parameter(Context *c, Decl *function, Specs specs, Decl *d, Loc loc)
{
    add_member(c, &function->params, specs, d, true, loc);
    Member *m = &function->params.data[function->params.len - 1];
    if (m->type->kind == TY_ARRAY) {
        m->parameter_array = m->type;
        m->type = pointer(c, m->parameter_array->base);
        m->type->qualifiers = m->parameter_array->qualifiers;
    } else if (m->type->kind == TY_FUNC) {
        m->type = pointer(c, m->type);
        m->type->noreturn_pointer = (m->attribute_markers & MARK_NORETURN) != 0;
    }
    if (!m->name)
        return;

    Symbol *prior = find_in(c->scope, m->name);
    if (prior) {
        if (!prior->forward_parameter) {
            error(c, loc, "duplicate parameter name '%s'", m->name);
            return;
        }
        if (!compatible(prior->type, m->type, true))
            error(c, loc, "incompatible forward declaration of parameter '%s'", m->name);
        prior->type = m->type;
        prior->forward_parameter = false;
        m->parameter = prior;
    } else {
        m->parameter = symbol(c, m->name, m->type, loc);
        PUSH(c, c->scope->names, m->parameter);
    }
    m->parameter->attribute_markers |= m->attribute_markers;
    m->parameter->patchable_entries = combine_patchable_entries(c, m->parameter->patchable_entries,
                                                              m->patchable_entries);
    if (m->deprecated)
        m->parameter->deprecated = m->deprecated;
}

void forward_parameters(Context *c, Decl *function, Loc loc)
{
    extension(c, loc, "forward parameter declaration");
    for (size_t i = 0; i < function->params.len; i++) {
        Member *m = &function->params.data[i];
        if (!m->parameter || m->type->kind == TY_VOID) {
            error(c, loc, "forward parameter declaration requires a named parameter");
            continue;
        }
        m->parameter->forward_parameter = true;
    }
    function->params.len = 0;
}

Decl *finish_parameters(Context *c, Decl *function, Loc loc)
{
    for (size_t i = 0; i < c->scope->names.len; i++) {
        Symbol *param = c->scope->names.data[i];
        if (param->forward_parameter)
            error(c, loc, "forward declaration of parameter '%s' has no matching declaration", param->name);
    }
    function->parameter_scope = c->scope;
    leave_scope(c);
    return function;
}

static const char *const builtins[] = {
    [BUILTIN_VA_ARG] = "__builtin_va_arg",
    [BUILTIN_VA_START] = "__builtin_va_start",
    [BUILTIN_VA_END] = "__builtin_va_end",
    [BUILTIN_VA_COPY] = "__builtin_va_copy",
    [BUILTIN_OFFSETOF] = "__builtin_offsetof",
    [BUILTIN_EXPECT] = "__builtin_expect",
    [BUILTIN_EXPECT_PROBABILITY] = "__builtin_expect_with_probability",
    [BUILTIN_ASSUME_ALIGNED] = "__builtin_assume_aligned",
    [BUILTIN_LINE] = "__builtin_LINE",
    [BUILTIN_FILE] = "__builtin_FILE",
    [BUILTIN_FUNCTION] = "__builtin_FUNCTION",
    [BUILTIN_PREFETCH] = "__builtin_prefetch",
    [BUILTIN_CLEAR_CACHE] = "__builtin___clear_cache",
    [BUILTIN_CLEAR_PADDING] = "__builtin_clear_padding",
    [BUILTIN_SETJMP] = "__builtin_setjmp",
    [BUILTIN_LONGJMP] = "__builtin_longjmp",
    [BUILTIN_FRAME_ADDRESS] = "__builtin_frame_address",
    [BUILTIN_RETURN_ADDRESS] = "__builtin_return_address",
    [BUILTIN_EXTRACT_RETURN_ADDR] = "__builtin_extract_return_addr",
    [BUILTIN_FROB_RETURN_ADDR] = "__builtin_frob_return_addr",
    [BUILTIN_DWARF_CFA] = "__builtin_dwarf_cfa",
    [BUILTIN_DWARF_SP_COLUMN] = "__builtin_dwarf_sp_column",
    [BUILTIN_EH_DATA_REGNO] = "__builtin_eh_return_data_regno",
    [BUILTIN_DWARF_REG_SIZES] = "__builtin_init_dwarf_reg_size_table",
    [BUILTIN_EXTEND_POINTER] = "__builtin_extend_pointer",
    [BUILTIN_CLASSIFY_TYPE] = "__builtin_classify_type",
    [BUILTIN_HAS_ATTRIBUTE] = "__builtin_has_attribute",
    [BUILTIN_SPECULATION_SAFE_VALUE] = "__builtin_speculation_safe_value",
    [BUILTIN_SPECULATION_1] = "__builtin_speculation_safe_value_1",
    [BUILTIN_SPECULATION_2] = "__builtin_speculation_safe_value_2",
    [BUILTIN_SPECULATION_4] = "__builtin_speculation_safe_value_4",
    [BUILTIN_SPECULATION_8] = "__builtin_speculation_safe_value_8",
    [BUILTIN_SPECULATION_16] = "__builtin_speculation_safe_value_16",
    [BUILTIN_SPECULATION_PTR] = "__builtin_speculation_safe_value_ptr",
    [BUILTIN_STACK_SAVE] = "__builtin_stack_save",
    [BUILTIN_STACK_RESTORE] = "__builtin_stack_restore",
    [BUILTIN_TYPES_COMPATIBLE] = "__builtin_types_compatible_p",
    [BUILTIN_CHOOSE_EXPR] = "__builtin_choose_expr",
    [BUILTIN_CONSTANT_P] = "__builtin_constant_p",
    [BUILTIN_COMPLEX] = "__builtin_complex",
    [BUILTIN_TGMATH] = "__builtin_tgmath",
    [BUILTIN_TRAP] = "__builtin_trap",
    [BUILTIN_UNREACHABLE] = "__builtin_unreachable",
    [BUILTIN_STATIC_CHAIN] = "__builtin_call_with_static_chain",
    [BUILTIN_OBJECT_SIZE] = "__builtin_object_size",
    [BUILTIN_DYNAMIC_OBJECT_SIZE] = "__builtin_dynamic_object_size",
    [BUILTIN_ALLOCA] = "__builtin_alloca",
    [BUILTIN_ALLOCA_ALIGN] = "__builtin_alloca_with_align",
    [BUILTIN_ALLOCA_ALIGN_MAX] = "__builtin_alloca_with_align_and_max",
    [BUILTIN_VECTOR_CONVERT] = "__builtin_convertvector",
    [BUILTIN_VECTOR_SHUFFLE] = "__builtin_shuffle",
    [BUILTIN_VECTOR_SHUFFLE_CONSTANT] = "__builtin_shufflevector",
};

typedef enum {
    FPT_GENERIC, FPT_DOUBLE, FPT_FLOAT, FPT_LONG_DOUBLE, FPT_FLOAT16, FPT_FLOAT32, FPT_FLOAT64,
    FPT_FLOAT32X, FPT_FLOAT64X, FPT_FLOAT128, FPT_QUAD_ALIAS, FPT_DECIMAL32, FPT_DECIMAL64, FPT_DECIMAL128
} FloatingBuiltinType;

typedef struct {
    const char *name;
    FloatingBuiltin op;
    FloatingBuiltinType type;
} FloatingFunction;

#define FP_STANDARD(name, op)                                                            \
    {"__builtin_" #name, op, FPT_DOUBLE}, {"__builtin_" #name "f", op, FPT_FLOAT},         \
    {"__builtin_" #name "l", op, FPT_LONG_DOUBLE}
#define FP_BINARY(name, op)                                                              \
    FP_STANDARD(name, op),                                                               \
    {"__builtin_" #name "f16", op, FPT_FLOAT16}, {"__builtin_" #name "f32", op, FPT_FLOAT32}, \
    {"__builtin_" #name "f64", op, FPT_FLOAT64}, {"__builtin_" #name "f128", op, FPT_FLOAT128}, \
    {"__builtin_" #name "f32x", op, FPT_FLOAT32X}, {"__builtin_" #name "f64x", op, FPT_FLOAT64X}, \
    {"__builtin_" #name "q", op, FPT_QUAD_ALIAS}
#define FP_DECIMAL(name, op)                                                              \
    {"__builtin_" #name "d32", op, FPT_DECIMAL32}, {"__builtin_" #name "d64", op, FPT_DECIMAL64}, \
    {"__builtin_" #name "d128", op, FPT_DECIMAL128}
#define FP_PREDICATE(name, op)                                                           \
    {"__builtin_" #name, op, FPT_GENERIC}, {"__builtin_" #name "f", op, FPT_FLOAT},        \
    {"__builtin_" #name "l", op, FPT_LONG_DOUBLE}, FP_DECIMAL(name, op)

static const FloatingFunction floating_builtins[] = {
    FP_BINARY(inf, FP_INFINITY), FP_DECIMAL(inf, FP_INFINITY), FP_BINARY(huge_val, FP_INFINITY),
    FP_BINARY(nan, FP_NAN), FP_DECIMAL(nan, FP_NAN),
    FP_BINARY(nans, FP_SNAN), FP_DECIMAL(nans, FP_SNAN),
    FP_BINARY(fabs, FP_ABS), FP_DECIMAL(fabs, FP_ABS), FP_BINARY(copysign, FP_COPYSIGN),
    FP_PREDICATE(signbit, FP_SIGNBIT), FP_PREDICATE(isinf, FP_ISINF),
    FP_PREDICATE(isnan, FP_ISNAN), FP_STANDARD(finite, FP_ISFINITE),
    {"__builtin_isfinite", FP_ISFINITE, FPT_GENERIC},
    {"__builtin_isnormal", FP_ISNORMAL, FPT_GENERIC},
    {"__builtin_issignaling", FP_ISSIGNALING, FPT_GENERIC},
    {"__builtin_isinf_sign", FP_ISINF_SIGN, FPT_GENERIC},
    {"__builtin_fpclassify", FP_CLASSIFY, FPT_GENERIC},
    {"__builtin_isunordered", FP_ISUNORDERED, FPT_GENERIC},
    {"__builtin_isgreater", FP_ISGREATER, FPT_GENERIC},
    {"__builtin_isgreaterequal", FP_ISGREATEREQUAL, FPT_GENERIC},
    {"__builtin_isless", FP_ISLESS, FPT_GENERIC},
    {"__builtin_islessequal", FP_ISLESSEQUAL, FPT_GENERIC},
    {"__builtin_islessgreater", FP_ISLESSGREATER, FPT_GENERIC},
};

#undef FP_STANDARD
#undef FP_BINARY
#undef FP_DECIMAL
#undef FP_PREDICATE

static const FloatingFunction *find_floating_builtin(const char *name)
{
    if (strncmp(name, "__builtin_", 10))
        return NULL;
    for (size_t i = 0; i < sizeof floating_builtins / sizeof *floating_builtins; i++)
        if (equal(floating_builtins[i].name, name))
            return &floating_builtins[i];
    return NULL;
}

static bool floating_builtin_available(Context *c, const FloatingFunction *builtin)
{
    return builtin->type != FPT_QUAD_ALIAS || c->target == &x86_64_linux;
}

static bool floating_library_symbol(const FloatingFunction *builtin)
{
    switch (builtin->op) {
    case FP_NAN: case FP_ABS: case FP_COPYSIGN: case FP_SIGNBIT: case FP_ISNAN: case FP_ISINF:
        return true;
    case FP_ISFINITE:
        return builtin->type != FPT_GENERIC;
    default:
        return false;
    }
}

static Type *floating_builtin_type(Context *c, FloatingBuiltinType kind)
{
    switch (kind) {
    case FPT_DOUBLE: return type_float(c, 8);
    case FPT_FLOAT: return type_float(c, 4);
    case FPT_LONG_DOUBLE: return type_float(c, c->target->long_double_size);
    case FPT_FLOAT16: return type_float(c, 2);
    case FPT_FLOAT32: return type_extended_float(c, FLOAT32);
    case FPT_FLOAT64: return type_extended_float(c, FLOAT64);
    case FPT_FLOAT32X: return type_extended_float(c, FLOAT32X);
    case FPT_FLOAT64X: return type_extended_float(c, FLOAT64X);
    case FPT_FLOAT128: case FPT_QUAD_ALIAS: return type_extended_float(c, FLOAT128);
    case FPT_DECIMAL32: return type_decimal(c, 4);
    case FPT_DECIMAL64: return type_decimal(c, 8);
    case FPT_DECIMAL128: return type_decimal(c, 16);
    default: return NULL;
    }
}

static Type *nan_string_type(Context *c)
{
    Type *character = type_int(c, 1, c->target->char_unsigned);
    character->plain_char = true;
    return pointer(c, qualified(c, character, S_CONST));
}

typedef struct { const char *name; IntegerBits operation; unsigned rank; } IntegerBuiltin;

static const IntegerBuiltin integer_builtins[] = {
    {"__builtin_bswap16", BITS_SWAP, 2}, {"__builtin_bswap32", BITS_SWAP, 3},
    {"__builtin_bswap64", BITS_SWAP, 4},
    {"__builtin_bswap128", BITS_SWAP, 6},
    {"__builtin_popcount", BITS_POPCOUNT, 3}, {"__builtin_popcountl", BITS_POPCOUNT, 4},
    {"__builtin_popcountll", BITS_POPCOUNT, 5},
    {"__builtin_parity", BITS_PARITY, 3}, {"__builtin_parityl", BITS_PARITY, 4},
    {"__builtin_parityll", BITS_PARITY, 5},
    {"__builtin_clz", BITS_CLZ, 3}, {"__builtin_clzl", BITS_CLZ, 4},
    {"__builtin_clzll", BITS_CLZ, 5},
    {"__builtin_ctz", BITS_CTZ, 3}, {"__builtin_ctzl", BITS_CTZ, 4},
    {"__builtin_ctzll", BITS_CTZ, 5},
    {"__builtin_ffs", BITS_FFS, 3}, {"__builtin_ffsl", BITS_FFS, 4},
    {"__builtin_ffsll", BITS_FFS, 5},
    {"__builtin_clrsb", BITS_CLRSB, 3}, {"__builtin_clrsbl", BITS_CLRSB, 4},
    {"__builtin_clrsbll", BITS_CLRSB, 5},
};

static const IntegerBuiltin *find_integer_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof integer_builtins / sizeof *integer_builtins; i++)
        if (equal(name, integer_builtins[i].name))
            return &integer_builtins[i];
    return NULL;
}

typedef enum { OVERFLOW_STORE, OVERFLOW_PREDICATE, OVERFLOW_CARRY } OverflowMode;
typedef struct {
    const char *name;
    int operation;
    OverflowMode mode;
    unsigned rank;
    bool uns;
} OverflowBuiltin;

#define OVERFLOW_BUILTINS(name, op) \
    {"__builtin_" #name "_overflow", op, OVERFLOW_STORE, 0, false}, \
    {"__builtin_" #name "_overflow_p", op, OVERFLOW_PREDICATE, 0, false}, \
    {"__builtin_s" #name "_overflow", op, OVERFLOW_STORE, 3, false}, \
    {"__builtin_s" #name "l_overflow", op, OVERFLOW_STORE, 4, false}, \
    {"__builtin_s" #name "ll_overflow", op, OVERFLOW_STORE, 5, false}, \
    {"__builtin_u" #name "_overflow", op, OVERFLOW_STORE, 3, true}, \
    {"__builtin_u" #name "l_overflow", op, OVERFLOW_STORE, 4, true}, \
    {"__builtin_u" #name "ll_overflow", op, OVERFLOW_STORE, 5, true}

static const OverflowBuiltin overflow_builtins[] = {
    OVERFLOW_BUILTINS(add, '+'), OVERFLOW_BUILTINS(sub, '-'), OVERFLOW_BUILTINS(mul, '*'),
    {"__builtin_addc", '+', OVERFLOW_CARRY, 3, true},
    {"__builtin_addcl", '+', OVERFLOW_CARRY, 4, true},
    {"__builtin_addcll", '+', OVERFLOW_CARRY, 5, true},
    {"__builtin_subc", '-', OVERFLOW_CARRY, 3, true},
    {"__builtin_subcl", '-', OVERFLOW_CARRY, 4, true},
    {"__builtin_subcll", '-', OVERFLOW_CARRY, 5, true},
};

#undef OVERFLOW_BUILTINS

static const OverflowBuiltin *find_overflow_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof overflow_builtins / sizeof *overflow_builtins; i++)
        if (equal(name, overflow_builtins[i].name))
            return &overflow_builtins[i];
    return NULL;
}

enum { AT_C11 = 2048, AT_ALWAYS = 4096, AT_LEGACY = 8192 };
typedef struct { const char *name; unsigned operation, arguments; } AtomicBuiltin;

static const AtomicBuiltin atomic_builtins[] = {
    {"__atomic_load_n", AT_LOAD, 2}, {"__atomic_store_n", AT_STORE, 3},
    {"__atomic_exchange_n", AT_EXCHANGE, 3}, {"__atomic_compare_exchange_n", AT_COMPARE, 6},
    {"__atomic_load", AT_LOAD | AT_POINTER_ARGUMENTS, 3},
    {"__atomic_store", AT_STORE | AT_POINTER_ARGUMENTS, 3},
    {"__atomic_exchange", AT_EXCHANGE | AT_POINTER_ARGUMENTS, 4},
    {"__atomic_compare_exchange", AT_COMPARE | AT_POINTER_ARGUMENTS, 6},
    {"__atomic_fetch_add", AT_ADD, 3}, {"__atomic_add_fetch", AT_ADD | AT_RETURN_NEW, 3},
    {"__atomic_fetch_sub", AT_SUB, 3}, {"__atomic_sub_fetch", AT_SUB | AT_RETURN_NEW, 3},
    {"__atomic_fetch_and", AT_AND, 3}, {"__atomic_and_fetch", AT_AND | AT_RETURN_NEW, 3},
    {"__atomic_fetch_or", AT_OR, 3}, {"__atomic_or_fetch", AT_OR | AT_RETURN_NEW, 3},
    {"__atomic_fetch_xor", AT_XOR, 3}, {"__atomic_xor_fetch", AT_XOR | AT_RETURN_NEW, 3},
    {"__atomic_fetch_nand", AT_NAND, 3}, {"__atomic_nand_fetch", AT_NAND | AT_RETURN_NEW, 3},
    {"__atomic_test_and_set", AT_TEST_AND_SET, 2}, {"__atomic_clear", AT_CLEAR, 2},
    {"__atomic_thread_fence", AT_THREAD_FENCE, 1}, {"__atomic_signal_fence", AT_SIGNAL_FENCE, 1},
    {"__atomic_is_lock_free", AT_LOCK_FREE, 2}, {"__atomic_always_lock_free", AT_LOCK_FREE | AT_ALWAYS, 2},
    {"__mcc_atomic_value_load", AT_LOAD | AT_C11, 2},
    {"__mcc_atomic_value_store", AT_STORE | AT_C11, 3},
    {"__mcc_atomic_value_exchange", AT_EXCHANGE | AT_C11, 3},
    {"__mcc_atomic_value_compare", AT_COMPARE | AT_C11, 6},
    {"__mcc_atomic_value_add", AT_ADD | AT_C11, 3}, {"__mcc_atomic_value_sub", AT_SUB | AT_C11, 3},
    {"__mcc_atomic_value_and", AT_AND | AT_C11, 3}, {"__mcc_atomic_value_or", AT_OR | AT_C11, 3},
    {"__mcc_atomic_value_xor", AT_XOR | AT_C11, 3},
    {"__mcc_atomic_object_lock_free", AT_LOCK_FREE | AT_C11, 1},
    {"__sync_fetch_and_add", AT_ADD | AT_LEGACY, 2},
    {"__sync_fetch_and_sub", AT_SUB | AT_LEGACY, 2},
    {"__sync_fetch_and_and", AT_AND | AT_LEGACY, 2},
    {"__sync_fetch_and_or", AT_OR | AT_LEGACY, 2},
    {"__sync_fetch_and_xor", AT_XOR | AT_LEGACY, 2},
    {"__sync_fetch_and_nand", AT_NAND | AT_LEGACY, 2},
    {"__sync_add_and_fetch", AT_ADD | AT_RETURN_NEW | AT_LEGACY, 2},
    {"__sync_sub_and_fetch", AT_SUB | AT_RETURN_NEW | AT_LEGACY, 2},
    {"__sync_and_and_fetch", AT_AND | AT_RETURN_NEW | AT_LEGACY, 2},
    {"__sync_or_and_fetch", AT_OR | AT_RETURN_NEW | AT_LEGACY, 2},
    {"__sync_xor_and_fetch", AT_XOR | AT_RETURN_NEW | AT_LEGACY, 2},
    {"__sync_nand_and_fetch", AT_NAND | AT_RETURN_NEW | AT_LEGACY, 2},
    {"__sync_bool_compare_and_swap", AT_COMPARE | AT_EXPECTED_VALUE | AT_LEGACY, 3},
    {"__sync_val_compare_and_swap", AT_COMPARE | AT_EXPECTED_VALUE | AT_RETURN_OBSERVED | AT_LEGACY, 3},
    {"__sync_synchronize", AT_THREAD_FENCE | AT_LEGACY, 0},
    {"__sync_lock_test_and_set", AT_EXCHANGE | AT_LEGACY, 2},
    {"__sync_lock_release", AT_UNLOCK | AT_LEGACY, 1},
};

static const AtomicBuiltin *find_atomic_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof atomic_builtins / sizeof *atomic_builtins; i++)
        if (equal(name, atomic_builtins[i].name))
            return &atomic_builtins[i];
    return NULL;
}

typedef enum {
    F_END, F_INT, F_SIZE, F_PTR, F_CONST_PTR, F_CHAR_PTR, F_CONST_CHAR_PTR, F_VA
} FortifiedType;
typedef struct {
    const char *name;
    FortifiedType result, arguments[6];
    bool variadic;
    unsigned count_arg, size_arg, nonnull;
} FortifiedBuiltin;

#define NONNULL_ARGUMENT(index) (1u << ((index) - 1))
#define MEMORY_CHECK(name) \
    {"__builtin___" #name "_chk", F_PTR, {F_PTR, F_CONST_PTR, F_SIZE, F_SIZE}, false, 3, 4, 0}
#define STRING_CHECK(name) \
    {"__builtin___" #name "_chk", F_CHAR_PTR, {F_CHAR_PTR, F_CONST_CHAR_PTR, F_SIZE}, false, 0, 3, UINT_MAX}

static const FortifiedBuiltin fortified_builtins[] = {
    MEMORY_CHECK(memcpy), MEMORY_CHECK(mempcpy), MEMORY_CHECK(memmove),
    {"__builtin___memset_chk", F_PTR, {F_PTR, F_INT, F_SIZE, F_SIZE}, false, 3, 4, 0},
    STRING_CHECK(strcpy), STRING_CHECK(stpcpy), STRING_CHECK(strcat),
    {"__builtin___strncpy_chk", F_CHAR_PTR, {F_CHAR_PTR, F_CONST_CHAR_PTR, F_SIZE, F_SIZE}, false, 3, 4, 0},
    {"__builtin___strncat_chk", F_CHAR_PTR, {F_CHAR_PTR, F_CONST_CHAR_PTR, F_SIZE, F_SIZE}, false, 0, 0,
     NONNULL_ARGUMENT(1)},
    {"__builtin___sprintf_chk", F_INT, {F_CHAR_PTR, F_INT, F_SIZE, F_CONST_CHAR_PTR}, true, 0, 0,
     NONNULL_ARGUMENT(1) | NONNULL_ARGUMENT(4)},
    {"__builtin___snprintf_chk", F_INT, {F_CHAR_PTR, F_SIZE, F_INT, F_SIZE, F_CONST_CHAR_PTR}, true, 2, 4,
     NONNULL_ARGUMENT(5)},
    {"__builtin___vsprintf_chk", F_INT, {F_CHAR_PTR, F_INT, F_SIZE, F_CONST_CHAR_PTR, F_VA}, false, 0, 0,
     NONNULL_ARGUMENT(1) | NONNULL_ARGUMENT(4)},
    {"__builtin___vsnprintf_chk", F_INT, {F_CHAR_PTR, F_SIZE, F_INT, F_SIZE, F_CONST_CHAR_PTR, F_VA}, false, 2, 4,
     NONNULL_ARGUMENT(5)},
    {"__builtin___printf_chk", F_INT, {F_INT, F_CONST_CHAR_PTR}, true, 0, 0, NONNULL_ARGUMENT(2)},
    {"__builtin___fprintf_chk", F_INT, {F_PTR, F_INT, F_CONST_CHAR_PTR}, true, 0, 0,
     NONNULL_ARGUMENT(1) | NONNULL_ARGUMENT(3)},
    {"__builtin___vprintf_chk", F_INT, {F_INT, F_CONST_CHAR_PTR, F_VA}, false, 0, 0, NONNULL_ARGUMENT(2)},
    {"__builtin___vfprintf_chk", F_INT, {F_PTR, F_INT, F_CONST_CHAR_PTR, F_VA}, false, 0, 0,
     NONNULL_ARGUMENT(1) | NONNULL_ARGUMENT(3)},
};

#undef MEMORY_CHECK
#undef STRING_CHECK

static const FortifiedBuiltin *find_fortified_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof fortified_builtins / sizeof *fortified_builtins; i++)
        if (equal(name, fortified_builtins[i].name))
            return &fortified_builtins[i];
    return NULL;
}

typedef struct {
    const char *name;
    AllocationSize allocation;
    unsigned arguments;
    bool pointer_argument;
    CStandard standard;
} AllocatorBuiltin;

static const AllocatorBuiltin allocator_builtins[] = {
    {"__builtin_malloc", {1, 0}, 1, false, C90},
    {"__builtin_calloc", {1, 2}, 2, false, C90},
    {"__builtin_realloc", {2, 0}, 2, true, C90},
    {"__builtin_aligned_alloc", {2, 0}, 2, false, C11},
    {"__builtin_free", {0, 0}, 1, true, C90},
};

static const AllocatorBuiltin *find_allocator_builtin(const char *name, bool explicit)
{
    const size_t prefix = sizeof "__builtin_" - 1;
    for (size_t i = 0; i < sizeof allocator_builtins / sizeof *allocator_builtins; i++) {
        const AllocatorBuiltin *b = &allocator_builtins[i];
        if (equal(name, b->name + (explicit ? 0 : prefix)))
            return b;
    }
    return NULL;
}

static bool library_builtin_enabled(Context *c, const Symbol *s)
{
    if (!s || s->type->kind != TY_FUNC || s->internal || s->explicit_asm_name || c->no_builtin)
        return false;
    for (size_t i = 0; i < c->disabled_builtins.len; i++)
        if (equal(s->name, c->disabled_builtins.data[i]))
            return false;
    return true;
}

typedef struct {
    const char *name;
    unsigned position;
} ExecBuiltin;

static const ExecBuiltin exec_builtins[] = {
    {"__builtin_execl", 0}, {"__builtin_execlp", 0}, {"__builtin_execle", 1},
};

static const ExecBuiltin *find_exec_builtin(const char *name, bool explicit)
{
    for (size_t i = 0; i < sizeof exec_builtins / sizeof *exec_builtins; i++) {
        const ExecBuiltin *builtin = &exec_builtins[i];
        if (equal(name, builtin->name + (explicit ? 0 : sizeof "__builtin_" - 1)))
            return builtin;
    }
    return NULL;
}

static void declare_exec_sentinel(Context *c, Symbol *s, Loc loc)
{
    if (!c->dialect.gnu || s->is_typedef || !library_builtin_enabled(c, s) || !s->type->prototype)
        return;
    const ExecBuiltin *builtin = find_exec_builtin(s->name, false);
    if (!builtin)
        return;
    Scope *outer = c->scope;
    while (outer->parent)
        outer = outer->parent;
    Symbol *known = find_in(outer, builtin->name);
    if (known && compatible(s->type, known->type, true))
        s->type = sentinel_type(c, s->type, known->type->sentinels, loc);
}

static const struct {
    const char *name;
    unsigned arguments;
    CStandard standard;
    bool gnu_only;
} terminating_builtins[] = {
    {"__builtin_abort", 0, C90, false}, {"__builtin_exit", 1, C90, false},
    {"__builtin__Exit", 1, C99, false}, {"__builtin__exit", 1, C90, true},
};

bool function_noreturn(Context *c, const Symbol *s)
{
    if (!s || s->type->kind != TY_FUNC)
        return false;
    if (s->attribute_markers & MARK_NORETURN)
        return true;
    Type *t = s->type;
    if (t->base->kind != TY_VOID || !library_builtin_enabled(c, s))
        return false;
    for (size_t i = 0; i < sizeof terminating_builtins / sizeof *terminating_builtins; i++) {
        if (!equal(s->name, terminating_builtins[i].name + 10))
            continue;
        if (!c->dialect.gnu && (terminating_builtins[i].gnu_only ||
                                c->dialect.standard < terminating_builtins[i].standard))
            return false;
        unsigned count = terminating_builtins[i].arguments;
        return !t->prototype || (!t->variadic && t->members.len == count &&
               (!count || compatible(t->members.data[0].type, type_int(c, 3, false), false)));
    }
    return false;
}

typedef struct {
    const char *name;
    SystemRegister reg;
    unsigned rank;
    bool write;
} SystemBuiltin;

static const SystemBuiltin system_builtins[] = {
    {"__builtin_thread_pointer", SYSREG_THREAD_POINTER, 0, false},
    {"__builtin_set_thread_pointer", SYSREG_THREAD_POINTER, 0, true},
    {"__builtin_aarch64_get_fpcr", SYSREG_FPCR, 3, false},
    {"__builtin_aarch64_set_fpcr", SYSREG_FPCR, 3, true},
    {"__builtin_aarch64_get_fpsr", SYSREG_FPSR, 3, false},
    {"__builtin_aarch64_set_fpsr", SYSREG_FPSR, 3, true},
    {"__builtin_aarch64_get_fpcr64", SYSREG_FPCR, 5, false},
    {"__builtin_aarch64_set_fpcr64", SYSREG_FPCR, 5, true},
    {"__builtin_aarch64_get_fpsr64", SYSREG_FPSR, 5, false},
    {"__builtin_aarch64_set_fpsr64", SYSREG_FPSR, 5, true},
    {"__builtin_ia32_stmxcsr", SYSREG_MXCSR, 3, false},
    {"__builtin_ia32_ldmxcsr", SYSREG_MXCSR, 3, true},
};

static const SystemBuiltin *find_system_builtin(const char *name)
{
    if (!strncmp(name, "__builtin_", 10))
        for (size_t i = 0; i < sizeof system_builtins / sizeof *system_builtins; i++)
            if (equal(name, system_builtins[i].name))
                return &system_builtins[i];
    return NULL;
}

static bool system_builtin_available(Context *c, const SystemBuiltin *builtin)
{
    switch (builtin->reg) {
    case SYSREG_THREAD_POINTER: return true;
    case SYSREG_FPCR: case SYSREG_FPSR: return c->target == &aarch64_linux;
    case SYSREG_MXCSR: return c->target == &x86_64_linux;
    }
    return false;
}

Builtin builtin_kind(const char *name)
{
    for (unsigned i = 1; i < sizeof builtins / sizeof *builtins; i++)
        if (builtins[i] && equal(name, builtins[i]))
            return (Builtin)i;
    if (find_integer_builtin(name))
        return BUILTIN_INTEGER;
    if (find_overflow_builtin(name))
        return BUILTIN_OVERFLOW;
    if (find_fortified_builtin(name))
        return BUILTIN_FORTIFIED;
    if (find_allocator_builtin(name, true))
        return BUILTIN_ALLOCATOR;
    if (find_exec_builtin(name, true))
        return BUILTIN_EXEC;
    if (find_floating_builtin(name))
        return BUILTIN_FLOAT;
    if (find_system_builtin(name))
        return BUILTIN_SYSTEM_REGISTER;
    if (!strncmp(name, "__builtin_", 10))
        for (size_t i = 0; i < sizeof terminating_builtins / sizeof *terminating_builtins; i++)
            if (equal(name, terminating_builtins[i].name))
                return BUILTIN_TERMINATING;
    return find_atomic_builtin(name) ? BUILTIN_ATOMIC : BUILTIN_NONE;
}

bool builtin_available(Context *c, const char *name)
{
    const FloatingFunction *floating = find_floating_builtin(name);
    if (floating)
        return floating_builtin_available(c, floating);
    const SystemBuiltin *system = find_system_builtin(name);
    return system ? system_builtin_available(c, system) : builtin_kind(name) != BUILTIN_NONE;
}

Attribute attribute_kind(Context *c, const char *name)
{
    size_t length = strlen(name);
    if (length > 4 && !strncmp(name, "__", 2) && equal(name + length - 2, "__"))
        name = copy(c, name + 2, length - 4);
    if (equal(name, "packed")) return ATTR_PACKED;
    if (equal(name, "aligned")) return ATTR_ALIGNED;
    if (equal(name, "weak")) return ATTR_WEAK;
    if (equal(name, "alias")) return ATTR_ALIAS;
    if (equal(name, "weakref")) return ATTR_WEAKREF;
    if (equal(name, "visibility")) return ATTR_VISIBILITY;
    if (equal(name, "constructor")) return ATTR_CONSTRUCTOR;
    if (equal(name, "destructor")) return ATTR_DESTRUCTOR;
    if (equal(name, "alloc_size")) return ATTR_ALLOC_SIZE;
    if (equal(name, "cleanup")) return ATTR_CLEANUP;
    if (equal(name, "section")) return ATTR_SECTION;
    if (equal(name, "vector_size")) return ATTR_VECTOR_SIZE;
    if (equal(name, "mode")) return ATTR_MODE;
    if (equal(name, "deprecated")) return ATTR_DEPRECATED;
    if (equal(name, "transparent_union")) return ATTR_TRANSPARENT_UNION;
    if (equal(name, "designated_init")) return ATTR_DESIGNATED_INIT;
    if (equal(name, "noinline")) return ATTR_NOINLINE;
    if (equal(name, "used")) return ATTR_USED;
    if (equal(name, "noreturn")) return ATTR_NORETURN;
    if (equal(name, "warn_unused_result")) return ATTR_WARN_UNUSED_RESULT;
    if (equal(name, "fallthrough")) return ATTR_FALLTHROUGH;
    if (equal(name, "error")) return ATTR_ERROR;
    if (equal(name, "warning")) return ATTR_WARNING;
    if (equal(name, "sentinel")) return ATTR_SENTINEL;
    if (equal(name, "nonnull")) return ATTR_NONNULL;
    if (equal(name, "returns_nonnull")) return ATTR_RETURNS_NONNULL;
    if (equal(name, "noplt")) return ATTR_NOPLT;
    if (equal(name, "patchable_function_entry")) return ATTR_PATCHABLE_FUNCTION_ENTRY;
    if (equal(name, "access")) return ATTR_ACCESS;
    if (equal(name, "stdcall")) return ATTR_STDCALL;
    if (equal(name, "gnu_inline")) return ATTR_GNU_INLINE;
    return ATTR_NONE;
}

Visibility visibility_kind(const char *name)
{
    if (equal(name, "default")) return VIS_DEFAULT;
    if (equal(name, "internal")) return VIS_INTERNAL;
    if (equal(name, "hidden")) return VIS_HIDDEN;
    if (equal(name, "protected")) return VIS_PROTECTED;
    return VIS_UNSPECIFIED;
}

static void setup_frame_builtins(Context *c, Type *nothing)
{
    Type *address = pointer(c, nothing);
    Type *integer = type_int(c, 3, false), *unsigned_integer = type_int(c, 3, true);
    const struct { Builtin builtin; Type *result, *parameter; } signatures[] = {
        {BUILTIN_FRAME_ADDRESS, address, unsigned_integer},
        {BUILTIN_RETURN_ADDRESS, address, unsigned_integer},
        {BUILTIN_EXTRACT_RETURN_ADDR, address, address},
        {BUILTIN_FROB_RETURN_ADDR, address, address},
        {BUILTIN_DWARF_CFA, address, NULL},
        {BUILTIN_DWARF_SP_COLUMN, unsigned_integer, NULL},
        {BUILTIN_EH_DATA_REGNO, integer, integer},
        {BUILTIN_DWARF_REG_SIZES, nothing, address},
        {BUILTIN_EXTEND_POINTER, type_int(c, 4, true), address},
        {BUILTIN_STACK_SAVE, address, NULL},
        {BUILTIN_STACK_RESTORE, nothing, address},
    };
    for (size_t i = 0; i < sizeof signatures / sizeof *signatures; i++) {
        Type *fn = type(c, TY_FUNC, 0, 1);
        fn->base = signatures[i].result;
        fn->prototype = true;
        if (signatures[i].parameter)
            PUSH(c, fn->members, ((Member){.type = signatures[i].parameter}));
        Symbol *s = symbol(c, builtins[signatures[i].builtin], fn, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
}

static bool is_speculation_builtin(Builtin builtin)
{
    return builtin >= BUILTIN_SPECULATION_SAFE_VALUE && builtin <= BUILTIN_SPECULATION_PTR;
}

static void setup_speculation_builtins(Context *c, Type *nothing)
{
    const unsigned ranks[] = {1, 2, 3, 4, 6};
    for (Builtin builtin = BUILTIN_SPECULATION_SAFE_VALUE; builtin <= BUILTIN_SPECULATION_PTR; builtin++) {
        Type *fn = type(c, TY_FUNC, 0, 1);
        if (builtin == BUILTIN_SPECULATION_SAFE_VALUE) {
            fn->base = nothing;
        } else {
            fn->base = builtin == BUILTIN_SPECULATION_PTR ? pointer(c, nothing) :
                       type_int(c, ranks[builtin - BUILTIN_SPECULATION_1], true);
            fn->prototype = fn->variadic = true;
            PUSH(c, fn->members, ((Member){.type = fn->base}));
        }
        Symbol *s = symbol(c, builtins[builtin], fn, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
}

static void setup_system_builtins(Context *c, Type *nothing)
{
    for (size_t i = 0; i < sizeof system_builtins / sizeof *system_builtins; i++) {
        const SystemBuiltin *builtin = &system_builtins[i];
        if (!system_builtin_available(c, builtin))
            continue;
        Type *reg = builtin->rank ? type_int(c, builtin->rank, true) : pointer(c, nothing);
        Type *fn = type(c, TY_FUNC, 0, 1);
        fn->prototype = true;
        fn->base = builtin->write ? nothing : reg;
        if (builtin->write)
            PUSH(c, fn->members, ((Member){.type = reg}));
        Symbol *s = symbol(c, builtin->name, fn, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
}

static void setup_terminating_builtins(Context *c, Type *nothing)
{
    for (size_t i = 0; i < sizeof terminating_builtins / sizeof *terminating_builtins; i++) {
        Type *fn = type(c, TY_FUNC, 0, 1);
        fn->base = nothing;
        fn->prototype = true;
        if (terminating_builtins[i].arguments)
            PUSH(c, fn->members, ((Member){.type = type_int(c, 3, false)}));
        const char *name = terminating_builtins[i].name;
        Symbol *s = symbol(c, name, fn, (Loc){0});
        s->assembly = name + 10;
        s->attribute_markers = MARK_NORETURN;
        PUSH(c, c->scope->names, s);
    }
}

void setup_types(Context *c)
{
    enter_scope(c);
    Members fields = {0};
    bool array_va_list = c->target == &x86_64_linux;
    Type *p = pointer(c, type(c, TY_VOID, 0, 1));
    Type *offset_type = type_int(c, 3, array_va_list);
    const Member aarch64_fields[] = {
        {.name="__stack",.type=p}, {.name="__gr_top",.type=p}, {.name="__vr_top",.type=p},
        {.name="__gr_offs",.type=offset_type}, {.name="__vr_offs",.type=offset_type}
    };
    const Member x86_64_fields[] = {
        {.name="gp_offset",.type=offset_type}, {.name="fp_offset",.type=offset_type},
        {.name="overflow_arg_area",.type=p}, {.name="reg_save_area",.type=p}
    };
    const Member *members = array_va_list ? x86_64_fields : aarch64_fields;
    size_t count = array_va_list ? sizeof x86_64_fields / sizeof *x86_64_fields :
                                   sizeof aarch64_fields / sizeof *aarch64_fields;
    for (size_t n = 0; n < count; n++)
        PUSH(c, fields, members[n]);
    Type *va_record = record_type(c, false, NULL, &fields, (Loc){0});
    c->va_list_type = array_va_list ? array(c, va_record, 1, true, (Loc){0}) : va_record;
    Symbol *s = symbol(c, "__builtin_va_list", c->va_list_type, (Loc){0});
    s->is_typedef = true;
    PUSH(c, c->scope->names, s);
    s = symbol(c, "__int128_t", type_int(c, 6, false), (Loc){0});
    s->is_typedef = true;
    PUSH(c, c->scope->names, s);
    s = symbol(c, "__uint128_t", type_int(c, 6, true), (Loc){0});
    s->is_typedef = true;
    PUSH(c, c->scope->names, s);
    Type *expect = type(c, TY_FUNC, 0, 1);
    expect->base = type_int(c, 4, false);
    expect->prototype = true;
    Member argument = {.type = expect->base};
    PUSH(c, expect->members, argument);
    PUSH(c, expect->members, argument);
    s = symbol(c, builtins[BUILTIN_EXPECT], expect, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *probability = type(c, TY_FUNC, 0, 1);
    probability->base = expect->base;
    probability->prototype = true;
    PUSH(c, probability->members, argument);
    PUSH(c, probability->members, argument);
    PUSH(c, probability->members, ((Member){.type = type_float(c, 8)}));
    s = symbol(c, builtins[BUILTIN_EXPECT_PROBABILITY], probability, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *aligned = type(c, TY_FUNC, 0, 1);
    Type *nothing = type(c, TY_VOID, 0, 1);
    setup_frame_builtins(c, nothing);
    setup_speculation_builtins(c, nothing);
    setup_system_builtins(c, nothing);
    setup_terminating_builtins(c, nothing);
    aligned->base = pointer(c, nothing);
    aligned->prototype = aligned->variadic = true;
    PUSH(c, aligned->members, ((Member){.type = pointer(c, qualified(c, nothing, S_CONST))}));
    PUSH(c, aligned->members, ((Member){.type = type_int(c, 4, true)}));
    s = symbol(c, builtins[BUILTIN_ASSUME_ALIGNED], aligned, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *line = type(c, TY_FUNC, 0, 1);
    line->base = type_int(c, 3, false);
    line->prototype = true;
    s = symbol(c, builtins[BUILTIN_LINE], line, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *source_name = type(c, TY_FUNC, 0, 1);
    Type *character = type_int(c, 1, c->target->char_unsigned);
    character->plain_char = true;
    source_name->base = pointer(c, qualified(c, character, S_CONST));
    source_name->prototype = true;
    s = symbol(c, builtins[BUILTIN_FILE], source_name, (Loc){0});
    PUSH(c, c->scope->names, s);
    s = symbol(c, builtins[BUILTIN_FUNCTION], source_name, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *classifier = type(c, TY_FUNC, 0, 1);
    classifier->base = line->base;
    classifier->variadic = true;
    s = symbol(c, builtins[BUILTIN_CLASSIFY_TYPE], classifier, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *setjmp_type = type(c, TY_FUNC, 0, 1);
    setjmp_type->base = line->base;
    setjmp_type->prototype = true;
    Member jump_buffer = {.type = pointer(c, nothing)};
    PUSH(c, setjmp_type->members, jump_buffer);
    s = symbol(c, builtins[BUILTIN_SETJMP], setjmp_type, (Loc){0});
    PUSH(c, c->scope->names, s);
    Type *longjmp_type = type(c, TY_FUNC, 0, 1);
    longjmp_type->base = nothing;
    longjmp_type->prototype = true;
    PUSH(c, longjmp_type->members, jump_buffer);
    PUSH(c, longjmp_type->members, ((Member){.type = line->base}));
    s = symbol(c, builtins[BUILTIN_LONGJMP], longjmp_type, (Loc){0});
    PUSH(c, c->scope->names, s);
    static const Builtin special_calls[] = {
        BUILTIN_VA_START, BUILTIN_VA_END, BUILTIN_VA_COPY,
        BUILTIN_OBJECT_SIZE, BUILTIN_DYNAMIC_OBJECT_SIZE,
        BUILTIN_ALLOCA, BUILTIN_ALLOCA_ALIGN, BUILTIN_ALLOCA_ALIGN_MAX,
        BUILTIN_VECTOR_SHUFFLE, BUILTIN_VECTOR_SHUFFLE_CONSTANT, BUILTIN_TGMATH,
        BUILTIN_PREFETCH, BUILTIN_CLEAR_CACHE, BUILTIN_CLEAR_PADDING
    };
    Type *builtin = type(c, TY_FUNC, 0, 1);
    builtin->base = nothing;
    for (size_t j = 0; j < sizeof special_calls / sizeof *special_calls; j++) {
        s = symbol(c, builtins[special_calls[j]], builtin, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
    for (size_t j = 0; j < sizeof floating_builtins / sizeof *floating_builtins; j++) {
        const FloatingFunction *function = &floating_builtins[j];
        if (!floating_builtin_available(c, function))
            continue;
        Type *fn = builtin;
        Type *fp = floating_builtin_type(c, function->type);
        bool library = floating_library_symbol(function);
        if (!fp && library)
            fp = type_float(c, 8);
        if (fp) {
            fn = type(c, TY_FUNC, 0, 1);
            fn->base = function->op < FP_SIGNBIT ? fp : type_int(c, 3, false);
            fn->prototype = true;
            if (function->op == FP_NAN || function->op == FP_SNAN) {
                PUSH(c, fn->members, ((Member){.type = nan_string_type(c)}));
            } else if (function->op != FP_INFINITY) {
                PUSH(c, fn->members, ((Member){.type = fp}));
                if (function->op == FP_COPYSIGN)
                    PUSH(c, fn->members, ((Member){.type = fp}));
            }
        }
        s = symbol(c, function->name, fn, (Loc){0});
        if (library)
            s->assembly = function->name + 10;
        PUSH(c, c->scope->names, s);
    }
    Type *no_arguments = type(c, TY_FUNC, 0, 1);
    no_arguments->base = builtin->base;
    no_arguments->prototype = true;
    s = symbol(c, builtins[BUILTIN_TRAP], no_arguments, (Loc){0});
    PUSH(c, c->scope->names, s);
    s = symbol(c, builtins[BUILTIN_UNREACHABLE], no_arguments, (Loc){0});
    PUSH(c, c->scope->names, s);
    for (size_t j = 0; j < sizeof integer_builtins / sizeof *integer_builtins; j++) {
        const IntegerBuiltin *b = &integer_builtins[j];
        Type *fn = type(c, TY_FUNC, 0, 1);
        Type *input = type_int(c, b->rank, b->operation != BITS_FFS && b->operation != BITS_CLRSB);
        fn->base = b->operation == BITS_SWAP ? input : type_int(c, 3, false);
        fn->prototype = true;
        PUSH(c, fn->members, ((Member){.type = input}));
        s = symbol(c, b->name, fn, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
    for (size_t j = 0; j < sizeof atomic_builtins / sizeof *atomic_builtins; j++) {
        s = symbol(c, atomic_builtins[j].name, builtin, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
    for (size_t j = 0; j < sizeof overflow_builtins / sizeof *overflow_builtins; j++) {
        s = symbol(c, overflow_builtins[j].name, builtin, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
    Type *ch = type_int(c, 1, c->target->char_unsigned);
    ch->plain_char = true;
    Type *fortified_types[] = {
        [F_INT] = type_int(c, 3, false),
        [F_SIZE] = type_int(c, 4, true),
        [F_PTR] = p,
        [F_CONST_PTR] = pointer(c, qualified(c, p->base, S_CONST)),
        [F_CHAR_PTR] = pointer(c, ch),
        [F_CONST_CHAR_PTR] = pointer(c, qualified(c, ch, S_CONST)),
        [F_VA] = array_va_list ? pointer(c, c->va_list_type->base) : c->va_list_type
    };
    for (size_t j = 0; j < sizeof fortified_builtins / sizeof *fortified_builtins; j++) {
        const FortifiedBuiltin *b = &fortified_builtins[j];
        Type *fn = type(c, TY_FUNC, 0, 1);
        fn->base = fortified_types[b->result];
        fn->prototype = true;
        fn->variadic = b->variadic;
        for (size_t i = 0; i < sizeof b->arguments / sizeof *b->arguments &&
             b->arguments[i] != F_END; i++)
            PUSH(c, fn->members, ((Member){.type = fortified_types[b->arguments[i]]}));
        if (b->nonnull) {
            ParameterIndices parameters = {0};
            if (b->nonnull != UINT_MAX)
                for (unsigned i = 1; i <= fn->members.len; i++)
                    if (b->nonnull & NONNULL_ARGUMENT(i))
                        PUSH(c, parameters, i);
            PUSH(c, fn->nullability.nonnull, parameters);
        }
        s = symbol(c, b->name, fn, (Loc){0});
        s->assembly = b->name + sizeof "__builtin_" - 1;
        PUSH(c, c->scope->names, s);
    }
    for (size_t j = 0; j < sizeof allocator_builtins / sizeof *allocator_builtins; j++) {
        const AllocatorBuiltin *b = &allocator_builtins[j];
        Type *fn = type(c, TY_FUNC, 0, 1);
        fn->base = b->allocation.first ? p : p->base;
        fn->prototype = true;
        fn->allocation = b->allocation;
        for (unsigned i = 0; i < b->arguments; i++) {
            Type *argument_type = !i && b->pointer_argument ? p : fortified_types[F_SIZE];
            PUSH(c, fn->members, ((Member){.type = argument_type}));
        }
        s = symbol(c, b->name, fn, (Loc){0});
        s->assembly = b->name + sizeof "__builtin_" - 1;
        PUSH(c, c->scope->names, s);
    }
    for (size_t i = 0; i < sizeof exec_builtins / sizeof *exec_builtins; i++) {
        const ExecBuiltin *exec = &exec_builtins[i];
        Type *fn = type(c, TY_FUNC, 0, 1);
        fn->base = type_int(c, 3, false);
        fn->prototype = fn->variadic = true;
        Member arg = {.type = fortified_types[F_CONST_CHAR_PTR]};
        PUSH(c, fn->members, arg);
        PUSH(c, fn->members, arg);
        Sentinel sentinel = {.position = {exec->position, 0}, .explicit_position = exec->position != 0};
        PUSH(c, fn->sentinels, sentinel);
        s = symbol(c, exec->name, fn, (Loc){0});
        s->assembly = exec->name + sizeof "__builtin_" - 1;
        PUSH(c, c->scope->names, s);
    }
}

#undef NONNULL_ARGUMENT

Node *node(Context *c, NodeKind kind, Loc loc, Node *left, Node *right)
{
    Node *n = alloc(c, sizeof *n);
    *n = (Node){.kind = kind, .loc = loc, .left = left, .right = right};
    return n;
}

Node *integer(Context *c, Loc loc, uint64_t value, Type *t)
{
    Node *n = node(c, N_INT, loc, NULL, NULL);
    n->type = t;
    n->integer = value;
    if (integer128(t) && !t->uns && value >> 63)
        n->integer_high = UINT64_MAX;
    return n;
}

static unsigned escape(Context *c, const char **text, Loc loc)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned value = *p++;
    if (value == '\\') {
        value = *p++;
        switch (value) {
        case '\\': case '\'': case '"': case '?': break;
        case 'a': value = '\a'; break;
        case 'b': value = '\b'; break;
        case 'f': value = '\f'; break;
        case 'n': value = '\n'; break;
        case 'r': value = '\r'; break;
        case 't': value = '\t'; break;
        case 'v': value = '\v'; break;
        case 'e': extension(c, loc, "\\e escape"); value = 27; break;
        case 'u': case 'U': {
            unsigned digits = value == 'u' ? 4 : 8;
            value = 0;
            for (unsigned i = 0; i < digits; i++) {
                if (!isxdigit(*p)) {
                    error(c, loc, "incomplete universal character name");
                    break;
                }
                unsigned digit = isdigit(*p) ? *p - '0' : tolower(*p) - 'a' + 10;
                value = value * 16 + digit;
                p++;
            }
            if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
                error(c, loc, "invalid universal character name");
            break;
        }
        case 'x':
            value = 0;
            if (!isxdigit(*p))
                error(c, loc, "expected hexadecimal escape digits");
            while (isxdigit(*p)) {
                unsigned digit = isdigit(*p) ? *p - '0' : tolower(*p) - 'a' + 10;
                value = value * 16 + digit;
                p++;
            }
            break;
        default:
            if (value >= '0' && value <= '7') {
                value -= '0';
                for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++)
                    value = value * 8 + *p++ - '0';
            } else {
                error(c, loc, "unsupported escape sequence");
            }
        }
    }
    *text = (const char *)p;
    return value;
}

static unsigned utf8(Context *c, const char **text, Loc loc)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned value = *p;
    unsigned width = value < 0x80 ? 1 : value >= 0xc2 && value <= 0xdf ? 2 :
                     value >= 0xe0 && value <= 0xef ? 3 :
                     value >= 0xf0 && value <= 0xf4 ? 4 : 0;
    if (!width) {
        error(c, loc, "invalid UTF-8 in literal");
        (*text)++;
        return 0;
    }
    if (width > 1) {
        value &= (1u << (7 - width)) - 1;
        for (unsigned i = 1; i < width; i++) {
            if ((p[i] & 0xc0) != 0x80) {
                error(c, loc, "invalid UTF-8 in literal");
                (*text)++;
                return 0;
            }
            value = (value << 6) | (p[i] & 0x3f);
        }
        if ((width == 3 && value < 0x800) || (width == 4 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            error(c, loc, "invalid UTF-8 in literal");
    }
    *text += width;
    return value;
}

Node *literal(Context *c, Token token)
{
    const char *s = token.text;
    if (token.kind == CHARACTER) {
        uint64_t value = 0;
        unsigned rank = 3;
        bool uns = false;
        bool wide = strchr("LuU", *s) != NULL;
        if (wide) {
            rank = *s == 'u' ? 2 : 3;
            uns = *s != 'L' || c->target->wchar_unsigned;
            s++;
        }
        if (*s != '\'' || s[1] == '\'')
            error(c, token.loc, "invalid or unsupported character constant");
        else if (wide) {
            const char *p = s + 1;
            value = *p == '\\' ? escape(c, &p, token.loc) : utf8(c, &p, token.loc);
            if (*p != '\'' || (rank == 2 && value > UINT16_MAX))
                error(c, token.loc, "invalid wide character constant");
        }
        else {
            const char *p = s + 1;
            while (*p && *p != '\'')
                value = (value << 8) | (escape(c, &p, token.loc) & 255);
        }
        return integer(c, token.loc, value, type_int(c, rank, uns));
    }
    const char *imaginary = strpbrk(s, "iIjJ");
    if (imaginary) {
        extension(c, token.loc, "imaginary constant");
        size_t index = (size_t)(imaginary - s);
        char *text = copy(c, s, strlen(s));
        memmove(text + index, imaginary + 1, strlen(imaginary));
        if (strpbrk(text, "iIjJ")) {
            error(c, token.loc, "invalid imaginary constant '%s'", s);
            return integer(c, token.loc, 0, type_int(c, 3, false));
        }
        token.text = text;
        Node *part = literal(c, token);
        if (part->type->kind == TY_DECIMAL) {
            error(c, token.loc, "decimal floating constants cannot be imaginary");
            return part;
        }
        Node *eval = excess_value(part);
        Node *zero = convert(c, integer(c, token.loc, 0, type_int(c, 3, false)), eval->type, token.loc);
        Node *result = node(c, N_COMPLEX, token.loc, zero, eval);
        result->type = complex_type(c, eval->type);
        return eval == part ? result : keep_evaluation_precision(c, result, complex_type(c, part->type));
    }
    bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (strchr(s, '.') || strpbrk(s, hex ? "pP" : "eE")) {
        char *end;
        long double real = strtold(s, &end);
        unsigned decimal_width = equal(end, "df") || equal(end, "DF") ? 4 :
                                 equal(end, "dd") || equal(end, "DD") ? 8 :
                                 equal(end, "dl") || equal(end, "DL") ? 16 : 0;
        if (decimal_width && end != s && !hex) {
            interchange_float_extension(c, token.loc, "decimal floating constants");
            Integer128 bits = decimal_literal(decimal_width, s, end);
            Node *n = node(c, N_FLOAT, token.loc, NULL, NULL);
            n->type = type_decimal(c, decimal_width);
            n->integer = bits.lo;
            n->integer_high = bits.hi;
            return n;
        }
        unsigned width = 8;
        Type *narrow = NULL;
        Type *extended = NULL;
        if (equal(end, "bf16") || equal(end, "BF16")) {
            floating_extension(c, token.loc, "the BF16 floating constant suffix");
            narrow = type_bfloat(c);
            end += 4;
        } else if (*end && (end[0] == 'f' || end[0] == 'F')) {
            FloatVariant variant = interchange_float_variant(end + 1);
            if (equal(end + 1, "16")) {
                interchange_float_extension(c, token.loc, "the F16 floating constant suffix");
                narrow = type_float(c, 2);
                end += 3;
            } else if (variant != FLOAT_DEFAULT) {
                interchange_float_extension(c, token.loc, format(c, "the %s floating constant suffix", end));
                extended = type_extended_float(c, variant);
                if (extended->size == 4)
                    real = strtof(s, NULL);
                else if (extended->size == 8)
                    real = strtod(s, NULL);
                end += strlen(end);
            } else {
                width = 4;
                real = strtof(s, NULL);
                end++;
            }
        }
        else if (*end && (end[0] == 'l' || end[0] == 'L')) {
            width = c->target->long_double_size;
            end++;
        } else if (*end && (end[0] == 'q' || end[0] == 'Q' ||
                   (c->target == &x86_64_linux && (end[0] == 'w' || end[0] == 'W')))) {
            floating_extension(c, token.loc, "the GNU extended floating constant suffix");
            bool quad = end[0] == 'q' || end[0] == 'Q';
            extended = type_extended_float(c, quad && c->target == &x86_64_linux ? FLOAT128 : FLOAT_DEFAULT);
            end++;
        } else {
            real = strtod(s, NULL);
        }
        if (end == s || *end)
            error(c, token.loc, "invalid floating constant '%s'", s);
        Node *n = node(c, N_FLOAT, token.loc, NULL, NULL);
        if (narrow && !c->dialect.round_float16_ops) {
            width = 4;
            real = strtof(s, NULL);
        }
        n->type = narrow && c->dialect.round_float16_ops ? narrow : extended ? extended : type_float(c, width);
        n->real = real;
        if (binary128(c->target, n->type)) {
#if defined(__aarch64__)
            Float128 quad = (Float128)real;
#else
            Float128 quad = strtof128(s, NULL);
#endif
            Integer128 bits;
            memcpy(&bits, &quad, sizeof bits);
            n->integer = bits.lo;
            n->integer_high = bits.hi;
            n->flags |= FLOAT_BITS_LITERAL;
        }
        return narrow ? keep_evaluation_precision(c, n, narrow) : n;
    }
    char *end;
    errno = 0;
    unsigned base = 0;
    const char *start = s;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        if (c->dialect.standard < C23)
            extension(c, token.loc, "binary literal");
        start += 2;
        base = 2;
    }
    uint64_t value = strtoull(start, &end, (int)base);
    if (end == start || errno == ERANGE)
        error(c, token.loc, "invalid integer constant '%s'", s);
    char suffix[4] = {0};
    size_t length = strlen(end);
    if (length < sizeof suffix)
        for (size_t i = 0; i < length; i++)
            suffix[i] = (char)tolower((unsigned char)end[i]);
    if (length >= sizeof suffix ||
        (*suffix && !equal(suffix,"u") && !equal(suffix,"l") && !equal(suffix,"ll") &&
         !equal(suffix,"ul") && !equal(suffix,"lu") && !equal(suffix,"ull") && !equal(suffix,"llu")))
        error(c, token.loc, "invalid integer suffix in '%s'", s);
    bool uns = strchr(suffix, 'u') != NULL;
    unsigned rank = strstr(suffix, "ll") ? 5 : strchr(suffix, 'l') ? 4 : 3;
    if (rank == 3 && value > (uns ? UINT32_MAX : INT32_MAX)) {
        if (s[0] == '0' && value <= UINT32_MAX)
            uns = true;
        else
            rank = 4;
    }
    if (value > INT64_MAX && !uns) {
        if (s[0] != '0')
            error(c, token.loc, "decimal constant is too large for a signed integer");
        uns = true;
    }
    return integer(c, token.loc, value, type_int(c, rank, uns));
}

static char string_prefix(const char *text)
{
    if (text[0] == 'u' && text[1] == '8')
        return '8';
    return strchr("LuU", text[0]) ? text[0] : 0;
}

static size_t decode_string(Context *c, Token token, unsigned width, unsigned char *bytes)
{
    char prefix = string_prefix(token.text);
    const char *s = token.text + (prefix == '8' ? 2 : prefix ? 1 : 0);
    bool raw = *s == 'R';
    s += raw ? 2 : 1;
    const char *stop = NULL;
    if (raw) {
        const char *opening = strchr(s, '(');
        size_t length = strlen(s);
        if (!opening || length < 2 * (size_t)(opening - s) + 3) {
            error(c, token.loc, "invalid raw string literal");
            return 0;
        }
        stop = s + length - (size_t)(opening - s) - 2;
        s = opening + 1;
    }
    size_t length = 0;
    while (raw ? s < stop : *s && *s != '"') {
        bool universal = !raw && s[0] == '\\' && (s[1] == 'u' || s[1] == 'U');
        unsigned cp;
        if (raw)
            cp = width == 1 ? (unsigned char)*s++ : utf8(c, &s, token.loc);
        else
            cp = width == 1 || *s == '\\' ? escape(c, &s, token.loc) : utf8(c, &s, token.loc);
        if (width == 1 && universal && cp > 0x7f) {
            unsigned n = cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
            bytes[length++] = (unsigned char)(((0xff << (8 - n)) & 0xff) | (cp >> (6 * (n - 1))));
            for (unsigned i = n - 1; i; i--)
                bytes[length++] = (unsigned char)(0x80 | ((cp >> (6 * (i - 1))) & 0x3f));
            continue;
        }
        if (width == 2 && cp > UINT16_MAX) {
            unsigned first = 0xd800 + ((cp - 0x10000) >> 10);
            bytes[length++] = (unsigned char)first;
            bytes[length++] = (unsigned char)(first >> 8);
            cp = 0xdc00 + ((cp - 0x10000) & 0x3ff);
        }
        for (unsigned i = 0; i < width; i++)
            bytes[length++] = (unsigned char)(cp >> (i * 8));
    }
    return length;
}

const char *decode_narrow_string(Context *c, Token token)
{
    unsigned char *bytes = alloc(c, strlen(token.text) + 1);
    bytes[decode_string(c, token, 1, bytes)] = 0;
    return (const char *)bytes;
}

static Node *string_literal(Context *c, Type *element, const unsigned char *bytes, size_t length, Loc loc)
{
    Type *t = array(c, element, length / element->size, true, loc);
    Symbol *sym = symbol(c, format(c, ".Lstr%u", c->unique++), t, loc);
    sym->defined = sym->internal = sym->read_only = true;
    sym->bytes = bytes;
    sym->byte_count = length;
    PUSH(c, c->globals, sym);
    Node *n = node(c, N_SYMBOL, loc, NULL, NULL);
    n->type = t;
    n->symbol = sym;
    return n;
}

Node *strings(Context *c, Tokens tokens)
{
    Loc loc = tokens.data[0].loc;
    char prefix = 0;
    size_t capacity = 1;
    for (size_t i = 0; i < tokens.len; i++) {
        Token token = tokens.data[i];
        char current = string_prefix(token.text);
        if (current && prefix && current != prefix)
            error(c, token.loc, "incompatible string literal prefixes");
        if (current)
            prefix = current;
        size_t length = strlen(token.text);
        if (length > SIZE_MAX / 4 - capacity) {
            error(c, token.loc, "string literal is too large");
            return integer(c, loc, 0, type_int(c, 3, false));
        }
        capacity += length;
    }
    unsigned width = prefix == 'u' ? 2 : prefix == 'U' || prefix == 'L' ? 4 : 1;
    bool uns = width == 1 ? c->target->char_unsigned : prefix != 'L' || c->target->wchar_unsigned;
    unsigned char *bytes = alloc(c, capacity * width);
    size_t length = 0;
    for (size_t i = 0; i < tokens.len; i++)
        length += decode_string(c, tokens.data[i], width, bytes + length);
    length += width;
    Type *element = type_int(c, width == 1 ? 1 : width == 2 ? 2 : 3, uns);
    element->plain_char = width == 1;
    return string_literal(c, element, bytes, length, loc);
}

static Symbol *predefined_function_name(Context *c, const char *name, Loc loc)
{
    static const char *const names[] = {"__func__", "__FUNCTION__", "__PRETTY_FUNCTION__"};
    if (!c->function)
        return NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (!equal(name, names[i]))
            continue;
        Symbol *s = c->function->function_names[i];
        if (!s) {
            Token token = {.kind = STRING, .text = format(c, "\"%s\"", c->function->name), .loc = loc};
            s = strings(c, (Tokens){.data = &token, .len = 1})->symbol;
            s->type = qualified(c, s->type, S_CONST);
            c->function->function_names[i] = s;
        }
        return s;
    }
    return NULL;
}

Node *reference(Context *c, const char *name, Loc loc)
{
    Symbol *s = find(c, name);
    if (!s)
        s = predefined_function_name(c, name, loc);
    if (!s || s->is_typedef) {
        error(c, loc, "use of undeclared identifier '%s'", name);
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    warn_deprecated(c, loc, name, s->deprecated);
    if (s->enum_constant) {
        Node *n = integer(c, loc, s->enum_value.lo, s->type);
        n->integer_high = s->enum_value.hi;
        return n;
    }
    if (s->local && c->function && s->owner && s->owner != c->function)
        s->captured_in_nested_function = true;
    Node *n = node(c, N_SYMBOL, loc, NULL, NULL);
    n->symbol = s;
    n->type = s->type;
    n->alignment = s->alignment ? symbol_alignment(s) : 0;
    if (s->type->kind == TY_FUNC) {
        unsigned minimum = c->target == &aarch64_linux ? 4 : 1;
        if (n->alignment < minimum)
            n->alignment = minimum;
    }
    return n;
}

static bool lvalue(Node *n)
{
    if (n->kind == N_MEMBER || n->kind == N_VECTOR_ELEMENT)
        return lvalue(n->left);
    return n->kind == N_SYMBOL || n->kind == N_DEREF ||
           (n->kind == N_COMPOUND && !(n->flags & UNION_CAST_LITERAL));
}

Symbol *explicit_register_object(Context *c, Node *n, size_t *displacement)
{
    size_t offset = 0;
    while (n->kind == N_MEMBER || n->kind == N_DEREF) {
        if (n->kind == N_MEMBER) {
            offset += n->integer;
            n = n->left;
            continue;
        }
        Node *sum = n->left;
        uint64_t index;
        /* An actual array subscript can name an element without exposing its address. */
        if (!(n->flags & SUBSCRIPT_ARRAY) || sum->kind != N_BINARY ||
            !constant_int(c, sum->right, &index))
            return NULL;
        Node *p = sum->left;
        if (p->kind != N_ADDR || p->left->type->kind != TY_ARRAY ||
            index > p->left->type->size || n->type->size > p->left->type->size - index)
            return NULL;
        offset += (size_t)index;
        n = p->left;
    }
    if (n->kind != N_SYMBOL || !n->symbol->register_binding)
        return NULL;
    if (displacement)
        *displacement = offset;
    return n->symbol;
}

static bool const_subobject(Type *t)
{
    if (t->qualifiers & S_CONST)
        return true;
    if (t->kind == TY_ARRAY)
        return const_subobject(t->base);
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        for (size_t i = 0; i < t->members.len; i++)
            if (const_subobject(t->members.data[i].type))
                return true;
    return false;
}

static Node *address(Context *c, Node *n, Type *t, Loc loc)
{
    if (n->kind == N_SYMBOL && n->type->kind == TY_FUNC) {
        const FloatingFunction *function = find_floating_builtin(n->symbol->name);
        if (function && !floating_library_symbol(function))
            error(c, loc, "cannot take the address of builtin function '%s'", function->name);
        else if (!strncmp(n->symbol->name, "__builtin_", 10)) {
            Builtin builtin = builtin_kind(n->symbol->name);
            if (is_speculation_builtin(builtin) || builtin == BUILTIN_SYSTEM_REGISTER)
                error(c, loc, "builtin function '%s' must be directly called", n->symbol->name);
        }
    }
    Node *result = node(c, N_ADDR, loc, n, NULL);
    result->type = pointer(c, t);
    if (t->kind == TY_FUNC) {
        if (n->kind == N_SYMBOL)
            result->type->noreturn_pointer = function_noreturn(c, n->symbol);
        else if (n->kind == N_DEREF)
            result->type->noreturn_pointer = n->left->type->noreturn_pointer;
    }
    return result;
}

static Symbol *find_label(Symbols labels, const char *name)
{
    for (size_t i = 0; i < labels.len; i++)
        if (equal(labels.data[i]->name, name))
            return labels.data[i];
    return NULL;
}

static Symbol *new_label_symbol(Context *c, const char *name, Loc loc)
{
    Symbol *s = symbol(c, name, type(c, TY_VOID, 0, 1), loc);
    s->assembly = format(c, ".Lmcc_label_%u", c->unique++);
    s->internal = true;
    s->owner = c->function;
    return s;
}

Node *local_label(Context *c, const char *name, Loc loc)
{
    extension(c, loc, "block-local label");
    if (find_label(c->scope->labels, name)) {
        error(c, loc, "duplicate local label declaration '%s'", name);
        return NULL;
    }
    Symbol *s = new_label_symbol(c, name, loc);
    PUSH(c, c->scope->labels, s);
    Node *declaration = node(c, N_LOCAL_LABEL, loc, NULL, NULL);
    declaration->symbol = s;
    return declaration;
}

static Symbol *resolve_label(Context *c, const char *name, Loc loc)
{
    for (Scope *scope = c->scope; scope; scope = scope->parent) {
        Symbol *s = find_label(scope->labels, name);
        if (s)
            return s;
    }
    Symbol *s = find_label(c->function->labels, name);
    if (s)
        return s;
    s = new_label_symbol(c, name, loc);
    PUSH(c, c->function->labels, s);
    return s;
}

Node *labeled_statement(Context *c, const char *name, Node *statement, bool definition, Loc loc)
{
    Symbol *s = resolve_label(c, name, loc);
    NodeKind kind = definition ? N_LABEL : N_GOTO;
    if (s->owner != c->function) {
        if (definition) {
            error(c, loc, "cannot define a label belonging to an enclosing function");
        } else {
            if (!s->nonlocal_state) {
                /* Space for the AArch64 saved registers, stack pointer, and return address. */
                Type *state = array(c, type_int(c, 4, true), 21, true, loc);
                s->nonlocal_state = frame_local(c, s->owner, "nonlocal goto state", state, loc);
                s->owner->has_nonlocal_labels = true;
            }
            kind = N_NONLOCAL_GOTO;
        }
    }
    if (definition) {
        if (s->defined)
            error(c, loc, "duplicate label '%s'", name);
        s->defined = true;
        Node *following = statement;
        while (following && following->kind == N_LABEL)
            following = following->left;
        if (following && (following->kind == N_CASE || following->kind == N_DEFAULT) &&
            loc.fallthrough_comment > following->loc.fallthrough_comment)
            following->loc.fallthrough_comment = loc.fallthrough_comment;
    } else {
        s->label_referenced = true;
    }
    Node *result = node(c, kind, loc, statement, NULL);
    result->symbol = s;
    return result;
}

Node *label_address(Context *c, const char *name, Loc loc)
{
    extension(c, loc, "label address");
    if (!c->function) {
        error(c, loc, "label address is only valid inside a function");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Symbol *s = resolve_label(c, name, loc);
    if (s->owner != c->function)
        error(c, loc, "cannot take the address of nonlocal label '%s'", name);
    s->label_referenced = s->label_address_taken = true;
    Node *label = node(c, N_SYMBOL, loc, NULL, NULL);
    label->type = s->type;
    label->symbol = s;
    return address(c, label, label->type, loc);
}

Node *indirect_goto(Context *c, Node *target, Loc loc)
{
    extension(c, loc, "computed goto");
    target = decay(c, target);
    if (target->type->kind != TY_PTR)
        error(c, loc, "computed goto requires a pointer");
    target = convert(c, target, pointer(c, type(c, TY_VOID, 0, 1)), loc);
    return node(c, N_INDIRECT_GOTO, loc, target, NULL);
}

const char *asm_string(Context *c, Node *literal, Loc loc)
{
    Symbol *s = literal->symbol;
    if (c->globals.len && c->globals.data[c->globals.len - 1] == s)
        c->globals.len--;
    if (s->type->base->size != 1 || !s->byte_count ||
        memchr(s->bytes, 0, s->byte_count - 1)) {
        error(c, loc, "asm requires a narrow string without embedded NUL bytes");
        return "";
    }
    return (const char *)s->bytes;
}

Node *asm_statement(Context *c, Node *literal, bool file_scope, Loc loc)
{
    const char *text = asm_string(c, literal, loc);
    if (file_scope) {
        Symbol *s = alloc(c, sizeof *s);
        s->asm_text = text;
        PUSH(c, c->globals, s);
        return NULL;
    }
    Node *n = node(c, N_ASM, loc, NULL, NULL);
    n->name = text;
    return n;
}

Node *asm_goto_label(Context *c, const char *name, Loc loc)
{
    Node *target = node(c, N_GOTO, loc, NULL, NULL);
    target->name = name;
    if (!c->function) {
        error(c, loc, "asm goto requires function scope");
        return target;
    }
    Symbol *s = resolve_label(c, name, loc);
    if (s->owner != c->function)
        error(c, loc, "asm goto label must belong to the current function");
    s->label_referenced = true;
    target->symbol = s;
    return target;
}

Node *extended_asm(Context *c, Node *literal, InlineAssembly *assembly, unsigned qualifiers, Loc loc)
{
    assembly->text = asm_string(c, literal, loc);
    if ((assembly->labels.len != 0) != ((qualifiers & ASM_GOTO) != 0))
        error(c, loc, "asm goto requires a list of destination labels");
    for (size_t i = 0; i < assembly->operands.len; i++) {
        AsmOperand *operand = &assembly->operands.data[i];
        Node *expression = operand->expression;
        Type *type = expression->type;
        bool output = i < assembly->outputs;
        if (output && expression->kind == N_SYMBOL && type->kind == TY_PTR)
            expression->symbol->pointer_size_untracked = true;
        if (output && (!lvalue(expression) || const_subobject(type) ||
                       type->kind == TY_FUNC || type->kind == TY_VOID))
            error(c, operand->loc, "asm output requires a modifiable lvalue");
        if (output && operand->constraint[0] != '=' && operand->constraint[0] != '+')
            error(c, operand->loc, "asm output constraint requires '=' or '+'");
        if (!output && (operand->constraint[0] == '=' || operand->constraint[0] == '+'))
            error(c, operand->loc, "asm input constraint cannot start with '=' or '+'");
        if (output && operand->constraint[0] == '+')
            assembly->extra_inputs++;
        operand->addressable = lvalue(expression) && !expression->width &&
                               type->kind != TY_VOID && type->kind != TY_FUNC &&
                               !explicit_register_object(c, expression, NULL);
        for (size_t j = 0; operand->name && j < i; j++) {
            const char *prior = assembly->operands.data[j].name;
            if (prior && equal(operand->name, prior))
                error(c, operand->loc, "duplicate asm operand name '%s'", operand->name);
        }
    }
    if (assembly->operands.len + assembly->extra_inputs + assembly->labels.len > ASM_MAX_OPERANDS)
        error(c, loc, "too many extended asm operands");
    if (!c->function) {
        if (assembly->outputs || assembly->clobbers.len || assembly->labels.len)
            error(c, loc, "file-scope extended asm requires immediate inputs and no clobbers or labels");
        if (!c->failed) {
            AsmPlan *plan = alloc(c, sizeof *plan);
            plan->assembly = assembly;
            plan->loc = loc;
            plan->serial = c->unique++;
            plan->file_scope = true;
            if (plan_inline_asm(c, plan)) {
                Symbol *definition = alloc(c, sizeof *definition);
                definition->asm_plan = plan;
                PUSH(c, c->globals, definition);
            }
        }
        return NULL;
    }
    Node *n = node(c, N_ASM, loc, NULL, NULL);
    n->inline_asm = assembly;
    return n;
}

Node *decay(Context *c, Node *n)
{
    if (n->type->kind == TY_ARRAY)
        return address(c, n, n->type->base, n->loc);
    if (n->type->kind == TY_FUNC)
        return address(c, n, n->type, n->loc);
    return n;
}

Node *convert(Context *c, Node *n, Type *to, Loc loc)
{
    n = decay(c, n);
    bool changed_attributes = to->kind == TY_PTR && to->base->kind == TY_FUNC &&
                              n->type->kind == TY_PTR && n->type->base->kind == TY_FUNC &&
                              (!same_allocation_size(n->type->base->allocation, to->base->allocation) ||
                               n->type->base->warn_unused_result != to->base->warn_unused_result ||
                               !same_sentinels(n->type->base->sentinels, to->base->sentinels) ||
                               !same_nullability(n->type->base->nullability, to->base->nullability));
    if ((!changed_attributes && compatible(n->type, to, true)) ||
        (aggregate(to) && same_type(n->type, to)))
        return n;
    if (scalar(to) && to->kind != TY_PTR)
        n = excess_value(n);
    if (to->kind != TY_VOID && (n->type->kind == TY_VECTOR || to->kind == TY_VECTOR)) {
        if (n->type->size != to->size ||
            !(n->type->kind == TY_VECTOR || integer_type(n->type) || n->type->kind == TY_FLOAT) ||
            !(to->kind == TY_VECTOR || integer_type(to) || to->kind == TY_FLOAT)) {
            error(c, loc, "vector cast requires equally sized vector or arithmetic types");
            return n;
        }
        Node *cast = node(c, N_CAST, loc, n, NULL);
        cast->type = to;
        return cast;
    }
    Type *from = n->type;
    bool from_noninteger = from->kind == TY_FLOAT || from->kind == TY_COMPLEX || from->kind == TY_DECIMAL;
    bool to_noninteger = to->kind == TY_FLOAT || to->kind == TY_COMPLEX || to->kind == TY_DECIMAL;
    if (to->kind != TY_VOID && (!scalar(n->type) || !scalar(to) ||
        (from_noninteger && to->kind == TY_PTR) || (n->type->kind == TY_PTR && to_noninteger))) {
        error(c, loc, "invalid conversion");
        return n;
    }
    Node *cast = node(c, N_CAST, loc, n, NULL);
    cast->type = to;
    return cast;
}

static bool object_type(Type *t)
{
    return t->kind != TY_VOID && t->kind != TY_FUNC;
}

static Node *with_type_evaluations(Context *c, Nodes evaluations, Node *expr, Loc loc)
{
    for (size_t i = evaluations.len; i > 0; i--)
        expr = comma_expression(c, evaluations.data[i - 1], expr, loc);
    return expr;
}

static Node *union_value(Context *c, Type *type, size_t member, Node *value, Loc loc)
{
    Node *init = node(c, N_INIT, loc, NULL, NULL);
    init->type = type;
    Node *item = node(c, N_DESIGNATE, loc, value, NULL);
    item->integer = member;
    PUSH(c, init->list, item);
    Node *literal = initialized_literal(c, type, init, loc);
    literal->flags |= UNION_CAST_LITERAL;
    return literal;
}

Node *cast_expression(Context *c, Node *n, Type *to, Loc loc)
{
    n = decay(c, n);
    if (n->type->kind == TY_PTR && to->kind == TY_PTR &&
        (n->type->base->kind == TY_FUNC) != (to->base->kind == TY_FUNC))
        extension(c, loc, "cast between function and object or void pointers");
    to = qualified(c, to, 0);
    if (to->kind == TY_UNION && !same_type(n->type, to) && !to->variable_size) {
        for (size_t i = 0; i < to->members.len; i++)
            if (same_type(n->type, to->members.data[i].type))
                return union_value(c, to, i, n, loc);
        error(c, loc, "union cast requires an operand matching a member type");
        return n;
    }
    Nodes evaluations = {0};
    if (c->function || c->scope->parameters)
        to = save_array_sizes(c, to, &evaluations, loc);
    Node *value = convert(c, n, to, loc);
    if (value == n && (lvalue(n) || excess_value(n) != n || (n->type != to && variably_modified(to)))) {
        value = node(c, N_CAST, loc, excess_value(n), NULL);
        value->type = to;
    }
    return with_type_evaluations(c, evaluations, value, loc);
}

static Node *promote(Context *c, Node *n)
{
    if (n->type->storage_float)
        return convert(c, n, type_float(c, 4), n->loc);
    return integer_type(n->type) && (n->type->rank < 3 ||
                                     (n->width && n->width < c->target->int_size * 8)) ?
        convert(c, n, type_int(c, 3, false), n->loc) : n;
}

static Type *evaluation_type(Context *c, Type *t)
{
    if (narrow_float(t))
        return type_float(c, 4);
    if (t->kind == TY_COMPLEX && narrow_float(t->base))
        return complex_type(c, type_float(c, 4));
    return t;
}

static Node *evaluation_operand(Context *c, Node *n, Type *semantic, Type *eval, Loc loc)
{
    if (semantic != eval && c->dialect.round_float16_ops)
        n = convert(c, n, semantic, loc);
    return convert(c, excess_value(n), eval, loc);
}

static Node *keep_evaluation_precision(Context *c, Node *n, Type *semantic)
{
    if (same_type(n->type, semantic))
        return n;
    Node *rounded = convert(c, n, semantic, n->loc);
    if (!c->dialect.round_float16_ops)
        rounded->flags |= CAST_EXCESS_PRECISION;
    return rounded;
}

Node *comma_expression(Context *c, Node *left, Node *right, Loc loc)
{
    Node *value = excess_value(right);
    Node *result = node(c, N_SEQUENCE, loc, left, value);
    result->type = value->type;
    return keep_evaluation_precision(c, result, right->type);
}

static Type *common_type(Context *c, Type *a, Type *b)
{
    if (a->kind == TY_DECIMAL || b->kind == TY_DECIMAL)
        return b->kind != TY_DECIMAL || (a->kind == TY_DECIMAL && a->size >= b->size) ? a : b;
    if (a->kind == TY_COMPLEX || b->kind == TY_COMPLEX)
        return complex_type(c, common_type(c, a->kind == TY_COMPLEX ? a->base : a,
                                          b->kind == TY_COMPLEX ? b->base : b));
    if (narrow_float(a) && narrow_float(b) && a->bfloat != b->bfloat)
        return a->bfloat ? b : a;
    if (a->kind == TY_FLOAT && b->kind == TY_FLOAT && a->size == b->size) {
        /* For equivalent formats, prefer interchange types, then the standard type. */
        static const unsigned rank[] = {
            [FLOAT32X] = 0, [FLOAT64X] = 0, [FLOAT_DEFAULT] = 1,
            [FLOAT32] = 2, [FLOAT64] = 2, [FLOAT128] = 2,
        };
        return rank[a->float_variant] >= rank[b->float_variant] ? a : b;
    }
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
        return b->kind != TY_FLOAT || (a->kind == TY_FLOAT && a->size >= b->size) ? a : b;
    if (a->rank < b->rank) {
        Type *tmp = a;
        a = b;
        b = tmp;
    }
    bool uns = a->uns || (b->uns && a->size <= b->size);
    return type_int(c, a->rank, uns);
}

static bool integer_constant_operands(Node *n)
{
    if (!integer_type(n->type))
        return false;
    switch (n->kind) {
    case N_INT:
        return true;
    case N_CAST:
        return n->left->kind == N_FLOAT || integer_constant_operands(n->left);
    case N_UNARY:
        return integer_constant_operands(n->left);
    case N_BINARY:
        return integer_constant_operands(n->left) && integer_constant_operands(n->right);
    case N_COND:
        return integer_constant_operands(n->left) && integer_constant_operands(n->right) &&
               integer_constant_operands(n->third);
    default:
        return false;
    }
}

static bool constant_pointer(Context *, Node *, uint64_t *);

static bool null_pointer_constant(Context *c, Node *n)
{
    if (n->kind == N_CAST && n->type->kind == TY_PTR &&
        n->type->base->kind == TY_VOID && !n->type->base->qualifiers)
        n = n->left;
    uint64_t value;
    return integer_constant_operands(n) && constant_int(c, n, &value) && value == 0;
}

static bool void_object_pair(Type *a, Type *b)
{
    return (a->kind == TY_VOID && object_type(b)) ||
           (b->kind == TY_VOID && object_type(a));
}

static bool void_function_pair(Type *a, Type *b)
{
    return (a->kind == TY_VOID && b->kind == TY_FUNC) ||
           (b->kind == TY_VOID && a->kind == TY_FUNC);
}

static unsigned pointed_to_qualifiers(Type *t)
{
    while (t->kind == TY_ARRAY)
        t = t->base;
    return t->qualifiers;
}

static bool compatible_pointer_targets(Type *a, Type *b)
{
    while (a->kind == TY_ARRAY && b->kind == TY_ARRAY) {
        if (!compatible_array_lengths(a, b))
            return false;
        a = a->base;
        b = b->base;
    }
    return same_type(a, b) && !((a->qualifiers ^ b->qualifiers) & S_ATOMIC);
}

static void diagnose_array_qualification(Context *c, Type *a, Type *b, Loc loc)
{
    if (a->kind == TY_ARRAY && b->kind == TY_ARRAY && c->dialect.standard < C23 &&
        pointed_to_qualifiers(a) != pointed_to_qualifiers(b))
        extension(c, loc, "pointer conversion between arrays with different qualifiers");
}

static Node *implicitly(Context *c, Node *n, Type *to, Loc loc)
{
    n = decay(c, n);
    if ((to->kind == TY_VECTOR || n->type->kind == TY_VECTOR) && !same_type(n->type, to)) {
        error(c, loc, "implicit conversion between incompatible vector or scalar types");
        return n;
    }
    if (to->kind == TY_PTR && !null_pointer_constant(c, n)) {
        if (n->type->kind == TY_PTR) {
            Type *dest = to->base, *source = n->type->base;
            if (void_function_pair(dest, source))
                extension(c, loc, "conversion between function and void pointers");
            else if (!compatible_pointer_targets(dest, source) && !void_object_pair(dest, source))
                error(c, loc, "incompatible pointer conversion");
            else {
                warn_transparent_redeclaration(c, dest, source, loc);
                diagnose_array_qualification(c, dest, source, loc);
                if ((pointed_to_qualifiers(source) & ~pointed_to_qualifiers(dest) & ~(unsigned)S_ATOMIC) &&
                    (!c->dialect.gnu || c->dialect.pedantic == PEDANTIC_ERROR))
                    error(c, loc, "pointer conversion discards qualifiers");
                if (to->noreturn_pointer && !n->type->noreturn_pointer)
                    warning_at(c, WARNING_DISCARDED_QUALIFIERS, loc,
                               "conversion to a noreturn function pointer from an unannotated pointer");
            }
        } else {
            error(c, loc, "expected a compatible pointer or null pointer constant");
        }
    } else if (integer_type(to) && to->kind != TY_BOOL && n->type->kind == TY_PTR) {
        error(c, loc, "implicit conversion from pointer to integer");
    }
    return convert(c, n, to, loc);
}

static Node *transparent_argument(Context *c, Node *arg, Type *to, Loc loc)
{
    if (same_type(arg->type, to))
        return arg;
    size_t fallback = SIZE_MAX;
    Type *source = arg->type->kind == TY_PTR ? arg->type->base : NULL;
    bool null = null_pointer_constant(c, arg);
    for (size_t i = 0; i < to->members.len; i++) {
        Type *member = to->members.data[i].type;
        if (same_type(arg->type, member)) {
            fallback = i;
            break;
        }
        if (member->kind != TY_PTR)
            continue;
        if (source && compatible_pointer_targets(member->base, source)) {
            if (!(pointed_to_qualifiers(source) & ~pointed_to_qualifiers(member->base))) {
                fallback = i;
                break;
            }
            if (fallback == SIZE_MAX)
                fallback = i;
        } else if (fallback == SIZE_MAX &&
                   (null || (source && (source->kind == TY_VOID || member->base->kind == TY_VOID)))) {
            fallback = i;
        }
    }
    if (fallback == SIZE_MAX) {
        error(c, loc, "argument does not match a member of the transparent union");
        return arg;
    }
    if (!c->extension_depth)
        warning_at(c, WARNING_PEDANTIC, loc, "ISO C does not support implicit conversion to a transparent union");
    Type *member = to->members.data[fallback].type;
    if (source && member->kind == TY_PTR &&
        (pointed_to_qualifiers(source) & ~pointed_to_qualifiers(member->base) & ~(unsigned)S_ATOMIC))
        warning_at(c, WARNING_DISCARDED_QUALIFIERS, loc, "transparent union argument discards pointer qualifiers");
    return union_value(c, to, fallback, convert(c, arg, member, loc), loc);
}

static Type *common_pointer(Context *c, Node *a, Node *b, bool allow_null_void, Loc loc)
{
    Type *left = a->type, *right = b->type;
    if (allow_null_void) {
        if (left->kind == TY_PTR && null_pointer_constant(c, b))
            return qualified(c, left, 0);
        if (right->kind == TY_PTR && null_pointer_constant(c, a))
            return qualified(c, right, 0);
    }
    if (left->kind != TY_PTR || right->kind != TY_PTR) {
        error(c, loc, "invalid pointer operands");
        return NULL;
    }
    Type *x = left->base, *y = right->base, *base;
    if (compatible_pointer_targets(x, y)) {
        diagnose_array_qualification(c, x, y, loc);
        if (!allow_null_void && !object_type(x))
            extension(c, loc, "ordered comparison of void or function pointers");
        base = composite_type(c, x, y, true);
    } else if (void_object_pair(x, y) || (allow_null_void && void_function_pair(x, y))) {
        if (!allow_null_void || void_function_pair(x, y))
            extension(c, loc, "comparison or conditional with incompatible pointer types");
        base = x->kind == TY_VOID ? x : y;
    } else {
        error(c, loc, "incompatible pointer operands");
        return NULL;
    }
    unsigned qualifiers = pointed_to_qualifiers(x) | pointed_to_qualifiers(y);
    if (base->kind == TY_VOID)
        qualifiers &= ~(unsigned)S_ATOMIC;
    Type *result = pointer(c, qualified(c, base, qualifiers));
    result->noreturn_pointer = left->noreturn_pointer && right->noreturn_pointer;
    return result;
}

Node *unary(Context *c, int op, Node *n, Loc loc)
{
    if (op == '&') {
        if (n->kind == N_SYMBOL)
            n->symbol->address_taken = true;
        if (n->kind == N_SYMBOL && n->type->kind == TY_PTR)
            n->symbol->pointer_size_untracked = true;
        if (n->kind == N_SYMBOL && n->type->kind == TY_FUNC &&
            (equal(n->symbol->name, builtins[BUILTIN_SETJMP]) ||
             equal(n->symbol->name, builtins[BUILTIN_LONGJMP]) ||
             equal(n->symbol->name, builtins[BUILTIN_DWARF_REG_SIZES]) ||
             equal(n->symbol->name, builtins[BUILTIN_EXTEND_POINTER]) ||
             equal(n->symbol->name, builtins[BUILTIN_CLASSIFY_TYPE])))
            error(c, loc, "builtin function '%s' must be directly called", n->symbol->name);
        if (n->width)
            error(c, loc, "cannot take address of bit-field");
        else if (explicit_register_object(c, n, NULL))
            error(c, loc, "cannot take address of explicit register variable");
        else if (!lvalue(n))
            error(c, loc, "operand of '&' is not an lvalue or function");
        return address(c, n, n->type, loc);
    }
    n = decay(c, n);
    if (op == '*') {
        Node *result = node(c, N_DEREF, loc, n, NULL);
        if (n->type->kind != TY_PTR) {
            error(c, loc, "operand of '*' is not a pointer");
            result->type = type_int(c, 3, false);
        } else {
            result->type = n->type->base;
        }
        return result;
    }
    if (n->type->kind == TY_VECTOR) {
        if (op != '+' && op != '-' && !(op == '~' && integer_type(n->type->base))) {
            error(c, loc, "invalid unary operator for vector");
            return n;
        }
        Node *result = node(c, N_UNARY, loc, n, NULL);
        result->op = op;
        result->type = qualified(c, n->type, 0);
        return result;
    }
    n = promote(c, n);
    if (op == '~' && n->type->kind == TY_COMPLEX)
        extension(c, loc, "complex conjugation with '~'");
    else if (!scalar(n->type) || (op == '~' && !integer_type(n->type)) ||
        (op != '!' && n->type->kind == TY_PTR))
        error(c, loc, "invalid unary operand");
    if (op == '+')
        return n;
    Node *result = node(c, N_UNARY, loc, excess_value(n), NULL);
    result->op = op;
    result->type = op == '!' ? type_int(c, 3, false) : result->left->type;
    return op == '!' ? result : keep_evaluation_precision(c, result, n->type);
}

Node *complex_component(Context *c, Node *n, bool imaginary, Loc loc)
{
    if (n->type->kind == TY_COMPLEX && excess_value(n) != n)
        return keep_evaluation_precision(c, complex_component(c, excess_value(n), imaginary, loc), n->type->base);
    if (n->type->kind != TY_COMPLEX) {
        if (n->type->kind != TY_FLOAT && n->type->kind != TY_DECIMAL && !integer_type(n->type))
            error(c, loc, "complex component access requires an arithmetic operand");
        if (!imaginary)
            return n;
        Node *result = node(c, N_SEQUENCE, loc, n, NULL);
        result->type = unqualified_type(c, n->type);
        result->right = convert(c, integer(c, loc, 0, type_int(c, 3, false)), result->type, loc);
        return result;
    }
    if (n->type->qualifiers & S_ATOMIC)
        error(c, loc, "a component of an atomic complex object cannot be accessed directly");
    Type *base = n->type->base;
    Node *result = node(c, N_MEMBER, loc, n, NULL);
    result->integer = imaginary ? base->size : 0;
    result->type = qualified(c, base, n->type->qualifiers & (S_CONST | S_VOLATILE));
    return result;
}

Node *complex_builtin(Context *c, Node *real, Node *imaginary, Loc loc)
{
    if (real->type->kind != TY_FLOAT || !same_type(real->type, imaginary->type)) {
        error(c, loc, "__builtin_complex requires two operands of the same real floating type");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Node *result = node(c, N_COMPLEX, loc, real, imaginary);
    result->type = complex_type(c, real->type);
    return result;
}

static Node *fold_float_complex(Context *c, Node *n, bool imaginary)
{
    Node *ar = constant_component(c, n->left, false);
    Node *ai = constant_component(c, n->left, true);
    Node *br = constant_component(c, n->right, false);
    Node *bi = constant_component(c, n->right, true);
    if (binary128(c->target, n->type->base)) {
        Integer128 a[2], b[2], result[2];
        if (!ar || !ai || !br || !bi || !quad_constant(c, ar, &a[0], false) ||
            !quad_constant(c, ai, &a[1], false) || !quad_constant(c, br, &b[0], false) ||
            !quad_constant(c, bi, &b[1], false))
            return NULL;
        __mcc_quad_complex_binary(result, a, b, n->op == '/');
        Node *part = node(c, N_FLOAT, n->loc, NULL, NULL);
        part->type = n->type->base;
        part->integer = result[imaginary].lo;
        part->integer_high = result[imaginary].hi;
        part->flags = FLOAT_BITS_LITERAL;
        return part;
    }
    ComplexNumber a, b;
    if (!ar || !ai || !br || !bi || !real_constant(c, ar, &a.real, false) ||
        !real_constant(c, ai, &a.imaginary, false) || !real_constant(c, br, &b.real, false) ||
        !real_constant(c, bi, &b.imaginary, false))
        return NULL;
    ComplexNumber result = n->op == '*' ? complex_product(a, b) : complex_quotient(a, b);
    Node *part = node(c, N_FLOAT, n->loc, NULL, NULL);
    part->type = n->type->base;
    part->real = imaginary ? result.imaginary : result.real;
    return part;
}

static Node *fold_integer_complex_division(Context *c, Node *n, bool imaginary)
{
    Node *a = constant_component(c, n->left, false);
    Node *b = constant_component(c, n->left, true);
    Node *x = constant_component(c, n->right, false);
    Node *y = constant_component(c, n->right, true);
    Integer128 abs_x, abs_y;
    if (!a || !b || !x || !y || !constant_integer(c, x, &abs_x) || !constant_integer(c, y, &abs_y))
        return NULL;
    if (!n->type->base->uns) {
        if (abs_x.hi >> 63)
            abs_x = wide_negate(abs_x);
        if (abs_y.hi >> 63)
            abs_y = wide_negate(abs_y);
    }
    bool swap = wide_compare(abs_x, abs_y, true) < 0;
    if (swap) {
        Node *old = x; x = y; y = old;
        old = a; a = b; b = old;
    }
    Loc loc = n->loc;
    Node *ratio = binary(c, '/', y, x, loc);
    Node *denominator = binary(c, '+', x, binary(c, '*', y, ratio, loc), loc);
    Node *cross_term = binary(c, '*', imaginary ? a : b, ratio, loc);
    Node *numerator = !imaginary ? binary(c, '+', a, cross_term, loc) :
                      swap ? binary(c, '-', cross_term, b, loc) :
                             binary(c, '-', b, cross_term, loc);
    return convert(c, binary(c, '/', numerator, denominator, loc), n->type->base, loc);
}

Node *constant_component(Context *c, Node *n, bool imaginary)
{
    if (n->type->kind != TY_COMPLEX)
        return imaginary ? convert(c, integer(c, n->loc, 0, type_int(c, 3, false)), n->type, n->loc)
                         : n;
    switch (n->kind) {
    case N_COMPLEX:
        return imaginary ? n->right : n->left;
    case N_CAST: {
        Node *part = constant_component(c, n->left, imaginary);
        return part ? convert(c, part, n->type->base, n->loc) : NULL;
    }
    case N_UNARY: {
        Node *part = constant_component(c, n->left, imaginary);
        if (part && (n->op == '-' || (n->op == '~' && imaginary)))
            return unary(c, '-', part, n->loc);
        return part;
    }
    case N_COND: {
        bool test;
        if (constant_truth(c, n->left, &test))
            return constant_component(c, test ? n->right : n->third, imaginary);
        return NULL;
    }
    case N_BINARY: {
        bool left_complex = n->left->type->kind == TY_COMPLEX;
        bool right_complex = n->right->type->kind == TY_COMPLEX;
        if (n->op == '+' || n->op == '-') {
            Node *a = constant_component(c, n->left, imaginary);
            Node *b = constant_component(c, n->right, imaginary);
            if (!a || !b)
                return NULL;
            if (imaginary && !right_complex)
                return a;
            if (imaginary && !left_complex)
                return n->op == '-' ? unary(c, '-', b, n->loc) : b;
            return binary(c, n->op, a, b, n->loc);
        }
        if (n->op == '/' && right_complex)
            return n->type->base->kind == TY_FLOAT ? fold_float_complex(c, n, imaginary) :
                                                    fold_integer_complex_division(c, n, imaginary);
        if ((n->op == '*' && (!left_complex || !right_complex)) || n->op == '/') {
            Node *a = constant_component(c, n->left, imaginary && left_complex);
            Node *b = constant_component(c, n->right, imaginary && right_complex);
            return a && b ? binary(c, n->op, a, b, n->loc) : NULL;
        }
        if (n->op == '*') {
            if (n->type->base->kind == TY_FLOAT)
                return fold_float_complex(c, n, imaginary);
            Node *a = constant_component(c, n->left, false);
            Node *b = constant_component(c, n->left, true);
            Node *x = constant_component(c, n->right, imaginary);
            Node *y = constant_component(c, n->right, !imaginary);
            if (!a || !b || !x || !y)
                return NULL;
            Node *first = binary(c, '*', a, x, n->loc);
            Node *second = binary(c, '*', b, y, n->loc);
            return binary(c, imaginary ? '+' : '-', first, second, n->loc);
        }
        return NULL;
    }
    default:
        return NULL;
    }
}

static Node *operation(Context *c, int op, Node *a, Node *b, Type *result, Loc loc)
{
    Node *n = node(c, N_BINARY, loc, a, b);
    n->op = op;
    n->type = result;
    return n;
}

static Node *size_expression(Context *c, Type *t, Loc loc)
{
    Type *size_type = type_int(c, 4, true);
    if (!t->variable_size)
        return integer(c, loc, t->size, size_type);
    if (t->runtime_size) {
        Node *n = node(c, N_SYMBOL, loc, NULL, NULL);
        n->symbol = t->runtime_size;
        n->type = size_type;
        return n;
    }
    if (t->kind != TY_ARRAY)
        return integer(c, loc, 0, size_type);
    Node *count = t->variable_bound ? convert(c, t->variable_bound, size_type, loc) :
                                     integer(c, loc, t->length, size_type);
    return operation(c, '*', count, size_expression(c, t->base, loc), size_type, loc);
}

static bool variably_modified(Type *t)
{
    while (t->kind == TY_PTR || t->kind == TY_ARRAY) {
        if (t->variable_size)
            return true;
        t = t->base;
    }
    return t->record_variably_modified;
}

Type *typeof_expression(Context *c, Node *expr, bool unqualified)
{
    Type *t = unqualified ? unqualified_type(c, expr->type) : expr->type;
    if (!variably_modified(t))
        return t;
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    result->typeof_operand = expr;
    return result;
}

static Node *save_layout_value(Context *c, Nodes *evaluations, const char *name, Node *value, Loc loc)
{
    Symbol *local;
    if (c->scope->parameters) {
        local = symbol(c, name, value->type, loc);
        local->local = true;
        PUSH(c, c->scope->bound_locals, local);
    } else {
        local = frame_local(c, c->function, name, value->type, loc);
    }
    Node *slot = node(c, N_SYMBOL, loc, NULL, NULL);
    slot->symbol = local;
    slot->type = local->type;
    PUSH(c, *evaluations, assign(c, '=', slot, value, false, loc));
    return slot;
}

void begin_unevaluated_operand(Context *c)
{
    RecordBoundsScope *scope = alloc(c, sizeof *scope);
    scope->parent = c->record_bounds_scope;
    c->record_bounds_scope = scope;
}

void finish_unevaluated_operand(Context *c, bool evaluated)
{
    RecordBoundsScope *scope = c->record_bounds_scope;
    c->record_bounds_scope = scope->parent;
    for (size_t i = 0; i < scope->consumed.len; i++) {
        DeferredRecordBounds consumed = scope->consumed.data[i];
        if (!evaluated)
            consumed.record->record_bounds.len = consumed.count;
        else if (scope->parent)
            PUSH(c, scope->parent->consumed, consumed);
    }
}

static void take_record_bounds(Context *c, Type *t, Nodes *evaluations)
{
    if (t->kind != TY_STRUCT && t->kind != TY_UNION)
        return;
    Type *definition = t->base;
    if (definition->record_bounds.len && c->record_bounds_scope)
        PUSH(c, c->record_bounds_scope->consumed,
             ((DeferredRecordBounds){definition, definition->record_bounds.len}));
    for (size_t i = 0; i < definition->record_bounds.len; i++)
        PUSH(c, *evaluations, definition->record_bounds.data[i]);
    definition->record_bounds.len = 0;
}

static Type *save_array_sizes(Context *c, Type *t, Nodes *evaluations, Loc loc)
{
    Type *base = t->base;
    if (t->kind == TY_PTR || t->kind == TY_ARRAY)
        base = save_array_sizes(c, base, evaluations, loc);
    else if (t->record_variably_modified)
        take_record_bounds(c, t, evaluations);
    if (t->typeof_operand || base != t->base) {
        if (t->typeof_operand)
            PUSH(c, *evaluations, decay(c, t->typeof_operand));
        Type *clean = alloc(c, sizeof *clean);
        *clean = *t;
        clean->base = base;
        clean->typeof_operand = NULL;
        t = clean;
    }
    if (t->kind == TY_ARRAY && t->complete && t->variable_size && !t->runtime_size && !t->array_star) {
        Node *slot = save_layout_value(c, evaluations, "array size", size_expression(c, t, loc), loc);
        t->runtime_size = slot->symbol;
    }
    return t;
}

static Node *layout_number(Context *c, Loc loc, size_t number)
{
    return integer(c, loc, number, type_int(c, 4, true));
}

static Node *layout_op(Context *c, int op, Node *a, Node *b, Loc loc)
{
    Node *expr = operation(c, op, a, b, a->type, loc);
    uint64_t value;
    return constant_int(c, expr, &value) ? layout_number(c, loc, value) : expr;
}

static Node *layout_align(Context *c, Node *offset, unsigned alignment, Loc loc)
{
    if (alignment <= 1)
        return offset;
    Node *sum = layout_op(c, '+', offset, layout_number(c, loc, alignment - 1), loc);
    return layout_op(c, '&', sum, layout_number(c, loc, 0 - (uint64_t)alignment), loc);
}

static Node *layout_bytes(Context *c, Node *offset, unsigned bytes, Loc loc)
{
    return bytes ? layout_op(c, '+', offset, layout_number(c, loc, bytes), loc) : offset;
}

static Node *keep_layout_value(Context *c, Nodes *evaluations, const char *name, Node *value, Loc loc)
{
    return value->kind == N_INT || value->kind == N_SYMBOL ? value :
           save_layout_value(c, evaluations, name, value, loc);
}

void finish_record_definition(Context *c, Type *t, Loc loc)
{
    if (!t->record_variably_modified || c->failed)
        return;
    extension(c, loc, "variably modified member type");
    if (!c->function && !c->scope->parameters) {
        error(c, loc, "variably modified members require block or prototype scope");
        return;
    }
    Nodes bounds = {0};
    Node *cursor = t->kind == TY_UNION ? layout_number(c, loc, 0) : NULL;
    unsigned bit = 0;
    for (size_t i = 0; i < t->members.len; i++) {
        Member *m = &t->members.data[i];
        m->type = save_array_sizes(c, m->type, &bounds, loc);
        if (!t->variable_size)
            continue;
        if (t->kind == TY_UNION) {
            Node *size = m->bit_field ? layout_number(c, loc, (m->width + 7) / 8) :
                                        size_expression(c, m->type, loc);
            Node *larger = binary(c, '>', size, cursor, loc);
            uint64_t known;
            if (constant_int(c, larger, &known))
                cursor = known ? size : cursor;
            else
                cursor = conditional(c, larger, size, cursor, loc);
        } else if (!cursor) {
            if (!m->type->variable_size)
                continue;
            Node *offset = layout_number(c, loc, m->offset);
            cursor = layout_op(c, '+', offset, size_expression(c, m->type, loc), loc);
        } else if (m->bit_field) {
            if (!m->width || m->requested_alignment) {
                cursor = layout_align(c, layout_bytes(c, cursor, bit != 0, loc), m->alignment, loc);
                bit = 0;
            }
            if (m->width) {
                m->runtime_offset = keep_layout_value(c, &bounds, "member offset", cursor, loc);
                m->offset = 0;
                m->bit = bit;
                m->packed_bit_field = true;
                cursor = layout_bytes(c, m->runtime_offset, (bit + m->width) / 8, loc);
                bit = (bit + m->width) % 8;
            }
        } else {
            cursor = layout_align(c, layout_bytes(c, cursor, bit != 0, loc), m->alignment, loc);
            m->runtime_offset = keep_layout_value(c, &bounds, "member offset", cursor, loc);
            m->offset = 0;
            cursor = layout_op(c, '+', m->runtime_offset, size_expression(c, m->type, loc), loc);
            bit = 0;
        }
        cursor = keep_layout_value(c, &bounds, "record extent", cursor, loc);
    }
    if (cursor && t->variable_size) {
        Node *size = layout_align(c, layout_bytes(c, cursor, bit != 0, loc), t->align, loc);
        t->runtime_size = keep_layout_value(c, &bounds, "record size", size, loc)->symbol;
    }
    t->record_bounds = bounds;
    update_qualified_types(t);
}

static bool vector_scalar_fits(Context *c, Type *element, Node *n)
{
    Type *from = n->type;
    if (integer_type(element)) {
        if (!integer_type(from))
            return false;
        if (from->size <= element->size)
            return true;
        uint64_t value;
        if (!constant_int(c, n, &value))
            return false;
        unsigned width = element->size * CHAR_BIT;
        if (element->uns)
            return value <= (UINT64_MAX >> (64 - width));
        int64_t max = (int64_t)(UINT64_MAX >> (65 - width));
        return (int64_t)value <= max && (int64_t)value >= -max - 1 &&
               (!from->uns || value <= (uint64_t)max);
    }
    if (!integer_type(from) && from->kind != TY_FLOAT)
        return false;
    if (from->kind == TY_FLOAT && from->size <= element->size)
        return true;
    unsigned precision = element->size == 4 ? 24 : element->size == 8 ? 53 :
                         c->target == &x86_64_linux ? 64 : 113;
    if (integer_type(from) && from->size * CHAR_BIT - !from->uns <= precision)
        return true;
    long double value;
    if (!real_constant(c, n, &value, false))
        return false;
    if (element->size == 4)
        return (long double)(float)value == value;
    if (element->size == 8)
        return (long double)(double)value == value;
    return true;
}

static Node *vector_binary(Context *c, int op, Node *a, Node *b, Loc loc)
{
    bool left = a->type->kind == TY_VECTOR, right = b->type->kind == TY_VECTOR;
    Type *vec = left ? a->type : b->type;
    Type *base = vec->base;
    bool comparison = op == EQ || op == NE || op == '<' || op == '>' || op == LE || op == GE;
    bool shift = op == SHL || op == SHR;
    bool integral = shift || op == '%' || op == '^' || op == '&' || op == '|';
    if (!base || (!comparison && !integral && op != '+' && op != '-' && op != '*' && op != '/') ||
        (integral && !integer_type(base))) {
        error(c, loc, "invalid binary operator for vector");
        return a;
    }
    if (left && right) {
        Type *other = b->type->base;
        if (!other || vec->size != b->type->size || base->size != other->size ||
            base->kind != other->kind) {
            error(c, loc, "incompatible vector operands");
            return a;
        }
        if (!shift && integer_type(base) &&
            ((!base->uns && other->uns) || (base->uns == other->uns && other->rank > base->rank)))
            base = other;
    } else {
        Node **value = left ? &b : &a;
        if (!vector_scalar_fits(c, base, *value)) {
            error(c, loc, "scalar conversion to vector element type loses precision");
            return a;
        }
        *value = convert(c, *value, base, loc);
    }
    Type *result = base;
    if (comparison) {
        unsigned rank = integer_type(base) ? base->rank : base->size == 2 ? 2 :
                        base->size == 4 ? 3 : base->size == 8 ? 4 : 6;
        result = type_int(c, rank, false);
    }
    Node *n = operation(c, op, a, b, vector_type(c, result, vec->size, loc), loc);
    n->operand_type = base;
    return n;
}

static bool mixed_decimal_types(Type *a, Type *b)
{
    if (a->kind != TY_DECIMAL) {
        Type *swap = a;
        a = b;
        b = swap;
    }
    return a->kind == TY_DECIMAL && (b->kind == TY_FLOAT || b->kind == TY_COMPLEX);
}

Node *binary(Context *c, int op, Node *a, Node *b, Loc loc)
{
    a = decay(c, a);
    b = decay(c, b);
    if (a->type->kind == TY_VECTOR || b->type->kind == TY_VECTOR)
        return vector_binary(c, op, a, b, loc);
    a = promote(c, a);
    b = promote(c, b);
    Type *i = type_int(c, 3, false), *wide = type_int(c, 4, false);
    bool comparison = op == EQ || op == NE || op == '<' || op == '>' || op == LE || op == GE;
    if ((!scalar(a->type) || !scalar(b->type))) {
        error(c, loc, "invalid binary operands");
        return integer(c, loc, 0, i);
    }
    if (op == AND || op == OR)
        return operation(c, op, a, b, i, loc);
    if (mixed_decimal_types(a->type, b->type)) {
        error(c, loc, "cannot mix decimal and binary or complex arithmetic operands");
        return integer(c, loc, 0, i);
    }
    if (a->type->kind == TY_COMPLEX || b->type->kind == TY_COMPLEX) {
        if (a->type->kind == TY_PTR || b->type->kind == TY_PTR ||
            (op != '+' && op != '-' && op != '*' && op != '/' && op != EQ && op != NE)) {
            error(c, loc, "invalid operator or operand for complex arithmetic");
            return integer(c, loc, 0, i);
        }
        Type *semantic = common_type(c, a->type, b->type), *t = evaluation_type(c, semantic);
        a = a->type->kind == TY_COMPLEX ? evaluation_operand(c, a, semantic, t, loc) :
                                         evaluation_operand(c, a, semantic->base, t->base, loc);
        b = b->type->kind == TY_COMPLEX ? evaluation_operand(c, b, semantic, t, loc) :
                                         evaluation_operand(c, b, semantic->base, t->base, loc);
        Node *result = operation(c, op, a, b, comparison ? i : t, loc);
        result->operand_type = t;
        return comparison ? result : keep_evaluation_precision(c, result, semantic);
    }
    if (a->type->kind == TY_PTR || b->type->kind == TY_PTR) {
        if (op == '+' && b->type->kind == TY_PTR && integer_type(a->type)) {
            Node *tmp = a; a = b; b = tmp;
        }
        if (comparison) {
            Type *common = common_pointer(c, a, b, op == EQ || op == NE, loc);
            if (!common)
                return integer(c, loc, 0, i);
            return operation(c, op, convert(c, a, common, loc), convert(c, b, common, loc),
                             i, loc);
        }
        Type *base = a->type->kind == TY_PTR ? a->type->base : NULL;
        if (base && (base->kind == TY_VOID || base->kind == TY_FUNC))
            extension(c, loc, "arithmetic on void or function pointers");
        else if (base && !base->complete)
            error(c, loc, "arithmetic requires a complete pointed-to type");
        Node *scale = base && base->variable_size ? convert(c, size_expression(c, base, loc), wide, loc) :
                      integer(c, loc, base && base->size ? base->size : 1, wide);
        if (a->type->kind == TY_PTR && b->type->kind == TY_PTR && op == '-' &&
            compatible_pointer_targets(base, b->type->base) && b->type->base->complete) {
            diagnose_array_qualification(c, base, b->type->base, loc);
            Node *difference = operation(c, '-', convert(c, a, wide, loc),
                                        convert(c, b, wide, loc), wide, loc);
            return scale->kind == N_INT && scale->integer == 1 ?
                   difference : operation(c, '/', difference, scale, wide, loc);
        }
        if (a->type->kind == TY_PTR && integer_type(b->type) && (op == '+' || op == '-')) {
            b = operation(c, '*', convert(c, b, wide, loc), scale, wide, loc);
            return operation(c, op, a, b, qualified(c, a->type, 0), loc);
        }
        error(c, loc, "invalid pointer operands");
        return integer(c, loc, 0, i);
    }
    if ((op == '%' || op == '&' || op == '|' || op == '^' || op == SHL || op == SHR) &&
        (!integer_type(a->type) || !integer_type(b->type))) {
        error(c, loc, "operator requires integer operands");
        return integer(c, loc, 0, i);
    }
    Type *semantic = op == SHL || op == SHR ? a->type : common_type(c, a->type, b->type);
    Type *t = evaluation_type(c, semantic);
    Node *result = operation(c, op, evaluation_operand(c, a, semantic, t, loc),
                             evaluation_operand(c, b, semantic, t, loc), comparison ? i : t, loc);
    return comparison ? result : keep_evaluation_precision(c, result, semantic);
}

Node *subscript(Context *c, Node *left, Node *right, Loc loc)
{
    if (left->type->kind == TY_VECTOR) {
        Node *index = promote(c, decay(c, right));
        if (!integer_type(index->type))
            error(c, loc, "vector subscript requires an integer index");
        Node *element = node(c, N_VECTOR_ELEMENT, loc, left, index);
        element->type = qualified(c, left->type->base, left->type->qualifiers);
        return element;
    }
    Node *n = unary(c, '*', binary(c, '+', left, right, loc), loc);
    if (left->type->kind != TY_ARRAY && right->type->kind != TY_ARRAY)
        return n;
    n->flags |= SUBSCRIPT_ARRAY;
    if (!c->extension_depth && explicit_register_object(c, n, NULL))
        warning_at(c, WARNING_PEDANTIC, loc, "subscripting a register array is a GNU extension");
    return n;
}

Node *assign(Context *c, int op, Node *lhs, Node *rhs, bool postfix, Loc loc)
{
    if (!lvalue(lhs) || const_subobject(lhs->type) || lhs->type->kind == TY_ARRAY ||
        lhs->type->kind == TY_FUNC || lhs->type->kind == TY_VOID)
        error(c, loc, "assignment requires a modifiable lvalue");
    Node *operand = rhs;
    if (op != '=')
        rhs = binary(c, op, lhs, rhs, loc);
    Node *n = node(c, op == '=' ? N_ASSIGN : N_UPDATE, loc, lhs, implicitly(c, rhs, lhs->type, loc));
    n->type = qualified(c, lhs->type, 0);
    n->third = operand;
    n->postfix = postfix;
    if (lhs->kind == N_SYMBOL && lhs->type->kind == TY_PTR && lhs->symbol->local) {
        Symbol *s = lhs->symbol;
        if (s->owner != c->function)
            s->pointer_size_untracked = true;
        else
            PUSH(c, s->pointer_assignments, op == '=' ? n->right : NULL);
    }
    return n;
}

Node *conditional(Context *c, Node *test, Node *yes, Node *no, Loc loc)
{
    test = decay(c, test);
    if (!scalar(test->type))
        error(c, loc, "condition must have scalar type");
    Node *a = yes ? decay(c, yes) : test;
    no = decay(c, no);
    Type *t;
    if (a->type->kind == TY_VOID || no->type->kind == TY_VOID) {
        if (a->type->kind != no->type->kind)
            extension(c, loc, "conditional expression mixing void and nonvoid arms");
        t = type(c, TY_VOID, 0, 1);
    } else if (a->type->kind == TY_PTR || no->type->kind == TY_PTR) {
        t = common_pointer(c, a, no, true, loc);
        if (!t)
            return integer(c, loc, 0, type_int(c, 3, false));
    } else if (mixed_decimal_types(a->type, no->type)) {
        error(c, loc, "cannot mix decimal and binary or complex conditional operands");
        return integer(c, loc, 0, type_int(c, 3, false));
    } else if (scalar(a->type) && scalar(no->type)) {
        t = qualified(c, common_type(c, promote(c, a)->type, promote(c, no)->type), 0);
    } else if (same_type(a->type, no->type)) {
        t = qualified(c, composite_type(c, a->type, no->type, true), 0);
    } else {
        error(c, loc, "incompatible conditional operands");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Type *eval = evaluation_type(c, t);
    Node *n = node(c, N_COND, loc, excess_value(test), evaluation_operand(c, a, t, eval, loc));
    n->third = evaluation_operand(c, no, t, eval, loc);
    n->type = eval;
    n->postfix = !yes;
    return keep_evaluation_precision(c, n, t);
}

static Node *va_object(Context *c, Node *n)
{
    Type *list_type = c->va_list_type;
    if (list_type->kind == TY_ARRAY) {
        if ((n->type->kind == TY_ARRAY || n->type->kind == TY_PTR) &&
            same_type(n->type->base, list_type->base))
            return unary(c, '*', decay(c, n), n->loc);
    } else if (lvalue(n) && same_type(n->type, list_type)) {
        return n;
    }
    return NULL;
}

static Node *integer_builtin(Context *c, const IntegerBuiltin *builtin, Type *fn, Nodes args, Loc loc)
{
    if (args.len != 1) {
        error(c, loc, "%s requires one argument", builtin->name);
        return integer(c, loc, 0, fn->base);
    }
    Type *input_type = fn->members.data[0].type;
    Node *input = implicitly(c, args.data[0], input_type, loc);
    unsigned width = input_type->size * CHAR_BIT;
    bool extended = integer128(input_type);
    Integer128 value;
    if (constant_integer(c, input, &value)) {
        if (!extended)
            return integer(c, loc, integer_bits(value.lo, builtin->operation, width), fn->base);
        Node *result = integer(c, loc, integer_bits(value.hi, BITS_SWAP, 64), fn->base);
        result->integer_high = integer_bits(value.lo, BITS_SWAP, 64);
        return result;
    }
    Type *wide = extended ? input_type : type_int(c, 4, true);
    Type *helper_type = type(c, TY_FUNC, 0, 1);
    helper_type->base = wide;
    helper_type->prototype = true;
    Node *callee = node(c, N_SYMBOL, loc, NULL, NULL);
    const char *helper = extended ? "__mcc_int128_swap" : "__mcc_integer_bits";
    callee->symbol = symbol(c, helper, helper_type, loc);
    callee->type = helper_type;
    Node *invoke = node(c, N_CALL, loc, decay(c, callee), NULL);
    invoke->type = wide;
    PUSH(c, invoke->list, convert(c, input, wide, loc));
    if (!extended) {
        Type *small = type_int(c, 3, true);
        PUSH(c, invoke->list, integer(c, loc, builtin->operation, small));
        PUSH(c, invoke->list, integer(c, loc, width, small));
    }
    return convert(c, invoke, fn->base, loc);
}

static Node *effect_sequence(Context *c, Node *a, Node *b, Loc loc)
{
    return a && b ? node(c, N_SEQUENCE, loc, a, b) : a ? a : b;
}

static Node *effect_condition(Context *c, Node *condition, Node *yes, Node *no)
{
    Node *n = node(c, N_COND, condition->loc, condition, yes);
    n->third = no;
    n->type = type(c, TY_VOID, 0, 1);
    return n;
}

/* Keep just observable access and side effects when a builtin discards an operand. */
static Node *discard_value(Context *c, Node *n, bool read)
{
    switch (n->kind) {
    case N_INT: case N_FLOAT:
        return NULL;
    case N_SYMBOL: case N_DEREF: case N_MEMBER:
        if (read && (n->type->qualifiers & (S_VOLATILE | S_ATOMIC)))
            return n;
        return n->left ? discard_value(c, n->left, n->kind == N_DEREF) : NULL;
    case N_ADDR:
        return discard_value(c, n->left, false);
    case N_CAST: case N_UNARY: case N_VECTOR_CONVERT:
        return discard_value(c, n->left, true);
    case N_VECTOR_ELEMENT: {
        if (read && (n->type->qualifiers & (S_VOLATILE | S_ATOMIC)))
            return n;
        Node *base = discard_value(c, n->left, false);
        Node *index = discard_value(c, n->right, true);
        return effect_sequence(c, base, index, n->loc);
    }
    case N_SEQUENCE: case N_BINARY: {
        if (n->kind == N_BINARY && (n->op == AND || n->op == OR)) {
            bool is_or = n->op == OR;
            bool test;
            if (constant_truth(c, n->left, &test))
                return test == is_or ? NULL : discard_value(c, n->right, true);
            Node *right = discard_value(c, n->right, true);
            if (right)
                return effect_condition(c, n->left, is_or ? NULL : right, is_or ? right : NULL);
            return discard_value(c, n->left, true);
        }
        Node *left = discard_value(c, n->left, true);
        Node *right = discard_value(c, n->right, true);
        return effect_sequence(c, left, right, n->loc);
    }
    case N_COND: {
        bool test;
        if (constant_truth(c, n->left, &test))
            return test && n->postfix ? NULL : discard_value(c, test ? n->right : n->third, true);
        Node *yes = n->postfix ? NULL : discard_value(c, n->right, true);
        Node *no = discard_value(c, n->third, true);
        return yes || no ? effect_condition(c, n->left, yes, no) : discard_value(c, n->left, true);
    }
    default:
        return n;
    }
}

bool expression_has_effect(Context *c, Node *n)
{
    return n && discard_value(c, n, true);
}

typedef struct ObjectOrigin {
    const struct ObjectOrigin *parent;
    const Symbol *symbol;
    size_t point;
} ObjectOrigin;

typedef struct {
    Context *context;
    Type *type;
    Loc loc;
    unsigned mode;
    bool dynamic, *defer;
    Symbol *access_caller;
    const ObjectOrigin *origin;
    size_t *pointer_search_budget;
    AccessRangeSource ranges;
} ObjectSizeQuery;

typedef struct {
    Node *expression;
    IntegerRange displacement[2];
    bool split;
} ObjectOffset;

static const Type displacement_type = {.kind = TY_INT, .size = 16};

static IntegerRange combine_integer_ranges(IntegerRange, IntegerRange, bool);
static IntegerRange arithmetic_range(IntegerRange, IntegerRange, const Type *, int);

static Node *unknown_object_size(const ObjectSizeQuery *q)
{
    return integer(q->context, q->loc, (q->mode & 2) ? 0 : UINT64_MAX, q->type);
}

static bool split_offset_expression(Context *c, Node *n, Node *arms[2])
{
    if (!integer_type(n->type))
        return false;
    if (n->kind == N_COND) {
        bool test;
        if (!constant_truth(c, n->left, &test)) {
            arms[0] = convert(c, n->postfix ? n->left : n->right, n->type, n->loc);
            arms[1] = convert(c, n->third, n->type, n->loc);
            return true;
        }
    }
    bool left = (n->kind == N_CAST || n->kind == N_UNARY || n->kind == N_BINARY) &&
                split_offset_expression(c, n->left, arms);
    if (!left && !((n->kind == N_BINARY || n->kind == N_SEQUENCE) && split_offset_expression(c, n->right, arms)))
        return false;
    for (unsigned i = 0; i < 2; i++) {
        Node *copy = node(c, n->kind, n->loc, NULL, NULL);
        *copy = *n;
        if (left)
            copy->left = arms[i];
        else
            copy->right = arms[i];
        arms[i] = copy;
    }
    return true;
}

static IntegerRange displacement_range(const ObjectSizeQuery *q, Node *expr)
{
    IntegerRange range = q->ranges.offset && q->ranges.point != SIZE_MAX ?
                         q->ranges.offset(q->ranges.data, expr, q->ranges.point) :
                         integer_expression_range(q->context, expr, NULL, NULL);
    return convert_integer_range(range, expr->type, &displacement_type);
}

static ObjectOffset object_offset(const ObjectSizeQuery *q, ObjectOffset offset, int op, Node *amount)
{
    if (q->access_caller) {
        /* Keep conditional alternatives apart so a gap at zero does not
           make a pointer before the object look like one at its start. */
        Node *arms[2];
        bool split = split_offset_expression(q->context, amount, arms);
        IntegerRange first = displacement_range(q, split ? arms[0] : amount);
        IntegerRange second = split ? displacement_range(q, arms[1]) : first;
        if (offset.split) {
            IntegerRange increment = combine_integer_ranges(first, second, false);
            offset.displacement[0] = arithmetic_range(offset.displacement[0], increment, &displacement_type, op);
            offset.displacement[1] = arithmetic_range(offset.displacement[1], increment, &displacement_type, op);
        } else {
            IntegerRange prior = offset.displacement[0];
            offset.displacement[0] = arithmetic_range(prior, first, &displacement_type, op);
            if (split) {
                offset.displacement[1] = arithmetic_range(prior, second, &displacement_type, op);
                offset.split = true;
            }
        }
        return offset;
    }
    amount = convert(q->context, amount, q->type, q->loc);
    if (!offset.expression && op == '+')
        return (ObjectOffset){.expression = amount};
    Node *prior = offset.expression ? offset.expression : integer(q->context, q->loc, 0, q->type);
    return (ObjectOffset){.expression = operation(q->context, op, prior, amount, q->type, q->loc)};
}

static Node *object_remaining(const ObjectSizeQuery *q, Node *size, ObjectOffset offset, Node *outer)
{
    Context *c = q->context;
    if (!q->access_caller && !offset.expression && !outer)
        return size;
    uint64_t extent = 0, distance = 0, limit = 0;
    bool fixed_size = constant_int(c, size, &extent);
    bool fixed_offset = !offset.expression || constant_int(c, offset.expression, &distance);
    if (q->access_caller) {
        IntegerRange range = convert_integer_range(offset.displacement[0], &displacement_type, q->type);
        distance = range.lower.lo;
        if (offset.split) {
            range = convert_integer_range(offset.displacement[1], &displacement_type, q->type);
            if (range.lower.lo < distance)
                distance = range.lower.lo;
        }
    }
    bool fixed_outer = !outer || constant_int(c, outer, &limit);
    if (fixed_size && fixed_offset && fixed_outer) {
        if (outer && limit < extent)
            extent = limit;
        if (distance <= extent)
            return integer(c, q->loc, extent - distance, q->type);
        return q->dynamic || q->access_caller ? integer(c, q->loc, 0, q->type) : NULL;
    }
    if (!q->dynamic)
        return NULL;
    Node *delta = offset.expression ? offset.expression : integer(c, q->loc, 0, q->type);
    Node *n = node(c, N_OBJECT_REMAINING, q->loc, size, delta);
    n->type = q->type;
    n->third = outer;
    n->integer = (q->mode & 2) ? 0 : UINT64_MAX;
    return n;
}

/* A static definition can have storage after a flexible array member. */
static size_t defined_object_size(Symbol *s)
{
    Type *t = s->type;
    Node *init = s->init;
    if (t->kind != TY_STRUCT || !t->members.len || !init || init->kind != N_INIT || !init->list.len)
        return t->size;
    size_t last = t->members.len - 1;
    Member tail = t->members.data[last];
    Node *item = init->list.data[init->list.len - 1];
    if (tail.type->kind != TY_ARRAY || tail.type->complete || item->integer != last ||
        !item->left->type || item->left->type->kind != TY_ARRAY)
        return t->size;
    size_t end = tail.offset + item->left->type->size;
    return end > t->size ? end : t->size;
}

static Node *pointer_object_size(const ObjectSizeQuery *, Node *, ObjectOffset);

Node *pointer_value_source(Node *n)
{
    while (n) {
        if (n->kind == N_CAST && n->left->type->kind == TY_PTR)
            n = n->left;
        else if (n->kind == N_SEQUENCE)
            n = n->right;
        else if (n->kind == N_ASSIGN && n->type->kind == TY_PTR)
            n = n->left;
        else
            break;
    }
    return n;
}

typedef struct { Node *first, *second; } AllocationArguments;

static AllocationArguments allocation_arguments(Node *n)
{
    if (n && n->kind == N_ALLOCA)
        return (AllocationArguments){n->left, NULL};
    if (!n || n->kind != N_CALL)
        return (AllocationArguments){0};
    AllocationSize size = n->left->type->base->allocation;
    if (!size.first || size.first > n->list.len || size.second > n->list.len ||
        !integer_type(n->list.data[size.first - 1]->type) ||
        (size.second && !integer_type(n->list.data[size.second - 1]->type)))
        return (AllocationArguments){0};
    return (AllocationArguments){n->list.data[size.first - 1],
                                 size.second ? n->list.data[size.second - 1] : NULL};
}

static bool constant_allocation_size(Context *c, AllocationArguments args, uint64_t *result)
{
    uint64_t count, element = 1;
    if (!constant_int(c, args.first, &count) || count > PTRDIFF_MAX ||
        (args.second && (!constant_int(c, args.second, &element) || element > PTRDIFF_MAX)) ||
        (element && count > PTRDIFF_MAX / element))
        return false;
    *result = count * element;
    return true;
}

static IntegerRange allocation_dimension(const ObjectSizeQuery *q, const Node *source, unsigned index, Node *count)
{
    return q->ranges.allocation ? q->ranges.allocation(q->ranges.data, source, index, count) :
                                  integer_expression_range(q->context, count, NULL, NULL);
}

static bool bounded_allocation_size(const ObjectSizeQuery *q, Node *source, uint64_t *result)
{
    AllocationArguments args = allocation_arguments(source);
    if (!args.first)
        return false;
    if (constant_allocation_size(q->context, args, result))
        return true;
    IntegerRange count = allocation_dimension(q, source, 0, args.first);
    IntegerRange element = args.second ? allocation_dimension(q, source, 1, args.second) :
                                          (IntegerRange){{1, 0}, {1, 0}};
    if ((!wide_nonzero(count.lower) && !wide_nonzero(count.upper)) ||
        (!wide_nonzero(element.lower) && !wide_nonzero(element.upper))) {
        *result = 0;
        return true;
    }
    if ((!args.first->type->uns && count.lower.hi >> 63) ||
        (args.second && !args.second->type->uns && element.lower.hi >> 63) ||
        count.upper.hi || element.upper.hi ||
        count.upper.lo > PTRDIFF_MAX || element.upper.lo > PTRDIFF_MAX ||
        (element.upper.lo && count.upper.lo > PTRDIFF_MAX / element.upper.lo))
        return false;
    *result = count.upper.lo * element.upper.lo;
    return true;
}

static void capture_allocation_size(Context *c, Node *source, Symbol *extent)
{
    AllocationArguments args = allocation_arguments(source);
    if (!args.first)
        return;
    source->symbol = extent;
    Type *size_type = extent->type;
    Loc loc = source->loc;
    uint64_t size;
    if (constant_allocation_size(c, args, &size)) {
        source->third = integer(c, loc, size, size_type);
        return;
    }

    Node *first = args.first, *second = args.second;
    Type *arg_type = type_int(c, integer128(first->type) || (second && integer128(second->type)) ? 6 : 4, true);
    Node *limit = integer(c, loc, PTRDIFF_MAX, arg_type);
    first = convert(c, first, arg_type, loc);
    Node *valid = binary(c, LE, first, limit, loc);
    Node *amount = convert(c, first, size_type, loc);
    if (second) {
        second = convert(c, second, arg_type, loc);
        valid = binary(c, AND, valid, binary(c, LE, second, limit, loc), loc);
        Node *divisor = convert(c, second, size_type, loc);
        Node *zero = integer(c, loc, 0, size_type);
        Node *product_limit = binary(c, '/', integer(c, loc, PTRDIFF_MAX, size_type), divisor, loc);
        Node *fits = binary(c, OR, binary(c, EQ, divisor, zero, loc),
                            binary(c, LE, amount, product_limit, loc), loc);
        valid = binary(c, AND, valid, fits, loc);
        amount = binary(c, '*', amount, divisor, loc);
    }
    source->third = conditional(c, valid, amount, integer(c, loc, UINT64_MAX, size_type), loc);
}

static bool tracks_pointer_size(Symbol *function, Symbol *s)
{
    return function && s->type->kind == TY_PTR && s->local && s->owner == function &&
           !s->register_binding && !s->pointer_size_untracked &&
           !(s->type->qualifiers & (S_VOLATILE | S_ATOMIC));
}

static Symbol *copied_pointer(Context *c, Node *n)
{
    return n && n->kind == N_SYMBOL && tracks_pointer_size(c->function, n->symbol) ? n->symbol : NULL;
}

/* Visit the initializer and each subsequent assignment using the same source rules. */
static Node *pointer_write_source(Symbol *s, size_t i)
{
    return pointer_value_source(i ? s->pointer_assignments.data[i - 1] : s->init);
}

static bool constant_pointer_extent(Context *c, Symbol *s, uint64_t *result);

static bool constant_source_extent(Context *c, Node *source, uint64_t *result)
{
    Symbol *copied = copied_pointer(c, source);
    return copied ? constant_pointer_extent(c, copied, result) :
                    constant_allocation_size(c, allocation_arguments(source), result);
}

static bool constant_pointer_extent(Context *c, Symbol *s, uint64_t *result)
{
    if (s->checking_extent)
        return false;
    if (s->checked_extent) {
        *result = s->extent_value;
        return s->constant_extent;
    }
    s->checking_extent = true;
    uint64_t extent = 0;
    bool fixed = constant_source_extent(c, pointer_write_source(s, 0), &extent);
    for (size_t i = 1; fixed && i <= s->pointer_assignments.len; i++) {
        Node *source = pointer_write_source(s, i);
        if (copied_pointer(c, source) == s)
            continue;
        uint64_t other;
        fixed = constant_source_extent(c, source, &other) && other == extent;
    }
    s->extent_value = *result = extent;
    s->constant_extent = fixed;
    s->checking_extent = false;
    s->checked_extent = true;
    return fixed;
}

static void prepare_pointer_extents(Context *c, Type *size_type, Symbol *root)
{
    Symbols copies = {0};
    PUSH(c, copies, root);
    for (size_t i = 0; i < copies.len; i++) {
        Symbol *s = copies.data[i];
        if (s->object_extent)
            continue;
        for (size_t j = 0; j <= s->pointer_assignments.len; j++) {
            Symbol *copied = copied_pointer(c, pointer_write_source(s, j));
            if (!copied || copied->object_extent)
                continue;
            size_t k = 0;
            while (k < copies.len && copies.data[k] != copied)
                k++;
            if (k == copies.len)
                PUSH(c, copies, copied);
        }
    }
    /* Start at allocations, then follow copies back to the queried pointer. */
    bool changed;
    do {
        changed = false;
        for (size_t i = copies.len; i; i--) {
            Symbol *s = copies.data[i - 1];
            if (s->object_extent)
                continue;
            bool has_extent = false;
            for (size_t j = 0; j <= s->pointer_assignments.len && !has_extent; j++) {
                Node *source = pointer_write_source(s, j);
                Symbol *copied = copied_pointer(c, source);
                has_extent = allocation_arguments(source).first || (copied && copied->object_extent);
            }
            if (!has_extent)
                continue;
            s->object_extent = frame_local(c, c->function, "allocated object size", size_type, s->loc);
            for (size_t j = 0; j <= s->pointer_assignments.len; j++)
                capture_allocation_size(c, pointer_write_source(s, j), s->object_extent);
            changed = true;
        }
    } while (changed);
}

static Node *access_pointer_size(const ObjectSizeQuery *q, Symbol *s, ObjectOffset offset)
{
    size_t point = q->ranges.point;
    if (!q->ranges.pointer_stores || point == SIZE_MAX || !tracks_pointer_size(q->access_caller, s) ||
        !*q->pointer_search_budget)
        return NULL;
    --*q->pointer_search_budget;
    for (const ObjectOrigin *origin = q->origin; origin; origin = origin->parent)
        if (origin->symbol == s && origin->point == point)
            return NULL;

    AccessPointerStore first;
    AccessPointerStores stores = {.data = &first, .cap = 1};
    if (!q->ranges.pointer_stores(q->ranges.data, s, point, &stores))
        return NULL;
    ObjectOrigin origin = {.parent = q->origin, .symbol = s, .point = point};
    ObjectSizeQuery before_store = *q;
    before_store.origin = &origin;
    Context *c = q->context;
    Node *limit = NULL;
    uint64_t largest = 0;
    for (size_t i = 0; i < stores.len; i++) {
        AccessPointerStore store = stores.data[i];
        /* An effectful RHS may change pointer or integer values before a
           subexpression reads them. Allocation arguments have their own sites. */
        before_store.ranges.point = expression_has_effect(c, store.value) ? SIZE_MAX : store.point;
        Node *size = pointer_object_size(&before_store, store.value, offset);
        uint64_t bytes;
        if (!constant_int(c, size, &bytes))
            return NULL;
        if (!limit || bytes > largest) {
            limit = size;
            largest = bytes;
        }
    }
    return limit;
}

static Node *allocated_object_size(const ObjectSizeQuery *q, Symbol *s, ObjectOffset offset)
{
    Context *c = q->context;
    if (q->access_caller)
        return access_pointer_size(q, s, offset);
    if (!tracks_pointer_size(c->function, s))
        return NULL;
    if (q->defer) {
        for (size_t i = 0; i <= s->pointer_assignments.len && !*q->defer; i++) {
            Node *source = pointer_write_source(s, i);
            *q->defer = allocation_arguments(source).first || copied_pointer(c, source);
        }
        return NULL;
    }
    if (c->function->labels.len || c->function->has_switch)
        return NULL;

    uint64_t size;
    if (constant_pointer_extent(c, s, &size))
        return object_remaining(q, integer(c, q->loc, size, q->type), offset, NULL);
    if (!q->dynamic)
        return NULL;
    if (!s->object_extent)
        prepare_pointer_extents(c, q->type, s);
    if (!s->object_extent)
        return NULL;
    Node *extent = node(c, N_SYMBOL, q->loc, NULL, NULL);
    extent->symbol = s->object_extent;
    extent->type = q->type;
    if (q->mode & 2) {
        Node *unknown = integer(c, q->loc, UINT64_MAX, q->type);
        Node *test = binary(c, EQ, extent, unknown, q->loc);
        extent = conditional(c, test, unknown_object_size(q), extent, q->loc);
    }
    return object_remaining(q, extent, offset, NULL);
}

static Node *lvalue_object_size(const ObjectSizeQuery *q, Node *n, ObjectOffset offset, bool array_decay)
{
    Context *c = q->context;
    Type *t = n->type;
    bool subobject = (q->mode & 1) != 0;
    if (n->kind == N_SYMBOL || n->kind == N_COMPOUND) {
        if (!t->complete || !object_type(t))
            return NULL;
        if (t->variable_size) {
            if (!q->dynamic || !t->runtime_size)
                return NULL;
            return object_remaining(q, size_expression(c, t, q->loc), offset, NULL);
        }
        Node *size = integer(c, q->loc, defined_object_size(n->symbol), q->type);
        return object_remaining(q, size, offset, NULL);
    }
    if (n->kind == N_MEMBER) {
        if (n->right && !q->dynamic)
            return NULL;
        Node *member_offset = integer(c, q->loc, n->integer, q->type);
        if (!subobject) {
            offset = object_offset(q, offset, '+', member_offset);
            if (n->right)
                offset = object_offset(q, offset, '+', n->right);
            return lvalue_object_size(q, n->left, offset, false);
        }
        ObjectSizeQuery whole = *q;
        whole.mode &= ~1u;
        ObjectOffset absolute = object_offset(q, (ObjectOffset){0}, '+', member_offset);
        if (n->right)
            absolute = object_offset(q, absolute, '+', n->right);
        Node *outer = lvalue_object_size(&whole, n->left, absolute, false);
        if (!outer)
            return NULL;
        if (!t->complete)
            return object_remaining(q, outer, offset, NULL);
        if (t->variable_size && !q->dynamic)
            return NULL;
        return object_remaining(q, size_expression(c, t, q->loc), offset, outer);
    }
    if (n->kind != N_DEREF)
        return NULL;
    if (subobject && array_decay && t->complete && !t->variable_size) {
        Node *outer = pointer_object_size(q, n->left, (ObjectOffset){0});
        if (!outer)
            return NULL;
        return object_remaining(q, integer(c, q->loc, t->size, q->type), offset, outer);
    }
    return pointer_object_size(q, n->left, offset);
}

static Node *pointer_object_size(const ObjectSizeQuery *q, Node *n, ObjectOffset offset)
{
    Context *c = q->context;
    if (n->type->kind != TY_PTR)
        return NULL;
    switch (n->kind) {
    case N_SYMBOL:
        return allocated_object_size(q, n->symbol, offset);
    case N_ADDR: {
        Type *object = n->left->type;
        bool array_decay = object->kind == TY_ARRAY && n->type->base != object;
        return lvalue_object_size(q, n->left, offset, array_decay);
    }
    case N_CAST:
        return pointer_object_size(q, n->left, offset);
    case N_SEQUENCE:
        return pointer_object_size(q, n->right, offset);
    case N_CALL: case N_ALLOCA: {
        uint64_t size;
        if (q->access_caller && bounded_allocation_size(q, n, &size))
            return object_remaining(q, integer(c, q->loc, size, q->type), offset, NULL);
        return NULL;
    }
    case N_ASSIGN:
        return q->access_caller ? pointer_object_size(q, n->right, offset) : NULL;
    case N_BINARY:
        if (n->op == '+' || n->op == '-')
            return pointer_object_size(q, n->left, object_offset(q, offset, n->op, n->right));
        return NULL;
    case N_COND: {
        bool test;
        if (constant_truth(c, n->left, &test))
            return pointer_object_size(q, test ? n->right : n->third, offset);
        Node *a = pointer_object_size(q, n->right, offset);
        Node *b = pointer_object_size(q, n->third, offset);
        uint64_t left, right;
        bool known = constant_int(c, a, &left) && constant_int(c, b, &right);
        if (q->dynamic) {
            if (!a && !b)
                return NULL;
            if (known && left == right)
                return a;
            return conditional(c, n->left, a ? a : unknown_object_size(q),
                               b ? b : unknown_object_size(q), q->loc);
        }
        if (!known)
            return NULL;
        return ((q->mode & 2) ? left < right : left > right) ? a : b;
    }
    default:
        return NULL;
    }
}

static Node *object_size_builtin(Context *c, Nodes args, bool dynamic, Loc loc)
{
    bool defer = false;
    ObjectSizeQuery q = {.context = c, .type = type_int(c, 4, true), .loc = loc,
                         .dynamic = dynamic, .defer = &defer};
    const char *name = dynamic ? "__builtin_dynamic_object_size" : "__builtin_object_size";
    if (args.len != 2) {
        error(c, loc, "%s requires two arguments", name);
        return unknown_object_size(&q);
    }
    uint64_t mode;
    if (!constant_int(c, args.data[1], &mode) || mode > 3) {
        error(c, loc, "%s requires a constant mode from 0 to 3", name);
        return unknown_object_size(&q);
    }
    q.mode = (unsigned)mode;
    Node *address = decay(c, args.data[0]);
    if (address->type->kind != TY_PTR && !null_pointer_constant(c, address)) {
        error(c, loc, "%s requires an object pointer", name);
        return unknown_object_size(&q);
    }
    Node *size = discard_value(c, address, true) ? NULL : pointer_object_size(&q, address, (ObjectOffset){0});
    if (defer) {
        Node *query = node(c, N_OBJECT_SIZE, loc, address, NULL);
        query->type = q.type;
        query->integer = q.mode;
        query->postfix = dynamic;
        PUSH(c, c->function->object_size_queries, query);
        return query;
    }
    return size ? size : unknown_object_size(&q);
}

static void resolve_object_sizes(Context *c)
{
    Nodes queries = c->function->object_size_queries;
    for (size_t i = 0; i < queries.len; i++) {
        Node *n = queries.data[i];
        ObjectSizeQuery q = {.context = c, .type = n->type, .loc = n->loc,
                             .mode = (unsigned)n->integer, .dynamic = n->postfix};
        Node *size = pointer_object_size(&q, n->left, (ObjectOffset){0});
        *n = *(size ? size : unknown_object_size(&q));
    }
    c->function->object_size_queries.len = 0;
}

static Node *overflow_builtin(Context *c, const OverflowBuiltin *builtin, Nodes args, Loc loc)
{
    Type *boolean = type_int(c, 0, true);
    bool carry = builtin->mode == OVERFLOW_CARRY;
    bool predicate = builtin->mode == OVERFLOW_PREDICATE;
    if (args.len != (carry ? 4u : 3u)) {
        error(c, loc, "%s requires %u arguments", builtin->name, carry ? 4u : 3u);
        return integer(c, loc, 0, boolean);
    }
    Type *fixed = builtin->rank ? type_int(c, builtin->rank, builtin->uns) : NULL;
    if (carry) {
        for (size_t i = 0; i < 3; i++)
            args.data[i] = implicitly(c, args.data[i], fixed, loc);
        args.data[3] = implicitly(c, args.data[3], pointer(c, fixed), loc);
        Node *n = node(c, N_CARRY, loc, NULL, NULL);
        n->type = fixed;
        n->op = builtin->operation;
        n->list = args;
        return n;
    }
    Node *a = args.data[0], *b = args.data[1], *out = args.data[2];
    if (fixed) {
        a = implicitly(c, a, fixed, loc);
        b = implicitly(c, b, fixed, loc);
        out = implicitly(c, out, pointer(c, fixed), loc);
    } else if (!integer_type(a->type) || !integer_type(b->type)) {
        error(c, loc, "%s requires integer operands", builtin->name);
        return integer(c, loc, 0, boolean);
    }
    if (!predicate)
        out = decay(c, out);
    Type *result = predicate ? out->type : out->type->kind == TY_PTR ? out->type->base : NULL;
    if (!result || result->kind != TY_INT || result->enumeration ||
        (!predicate && !fixed && (result->qualifiers & (S_CONST | S_ATOMIC)))) {
        error(c, loc, "%s requires %s", builtin->name, predicate ? "an integer type witness" :
              "a pointer to a writable non-atomic integer");
        return integer(c, loc, 0, boolean);
    }
    unsigned width = predicate && out->width ? out->width : result->size * CHAR_BIT;
    unsigned flags = (a->type->uns ? 0u : OVERFLOW_SIGNED_A) |
                     (b->type->uns ? 0u : OVERFLOW_SIGNED_B) |
                     (result->uns ? 0u : OVERFLOW_SIGNED_RESULT);
    bool extended = integer128(a->type) || integer128(b->type) || integer128(result);
    Type *wide = type_int(c, extended ? 6 : 4, true);
    a = convert(c, a, wide, loc);
    b = convert(c, b, wide, loc);
    if (predicate) {
        out = discard_value(c, out, true);
        Integer128 left, right;
        if (!out && constant_integer(c, a, &left) && constant_integer(c, b, &right)) {
            unsigned op = (unsigned)builtin->operation;
            bool overflow = extended ? integer128_overflow(left, right, op, width, flags) :
                                       integer_overflow(left.lo, right.lo, op, width, flags);
            return integer(c, loc, overflow, boolean);
        }
    }
    Node *n = node(c, N_OVERFLOW, loc, a, b);
    n->type = boolean;
    n->operand_type = predicate ? NULL : result;
    n->third = out;
    n->op = builtin->operation;
    n->integer = width;
    n->flags = flags;
    return n;
}

static void atomic_pointer_argument(Context *c, Nodes args, size_t index, Type *value,
                                    bool writable, Loc loc)
{
    Node *arg = args.data[index] = decay(c, args.data[index]);
    if (arg->type->kind != TY_PTR || !same_type(arg->type->base, value) ||
        (writable && (arg->type->base->qualifiers & S_CONST)))
        error(c, loc, "incompatible pointer argument in atomic operation");
}

static bool atomic_order(Context *c, Node **arg, unsigned operation, bool failure,
                         uint64_t *order, Loc loc)
{
    if (!integer_type((*arg)->type))
        error(c, loc, "atomic memory order must be an integer");
    *arg = convert(c, *arg, type_int(c, 3, false), loc);
    if (!constant_int(c, *arg, order))
        return false;
    bool load_only = operation == AT_LOAD || failure;
    bool store_only = operation == AT_STORE || operation == AT_CLEAR;
    if (*order > 5 || (load_only && (*order == 3 || *order == 4)) ||
        (store_only && (*order == 1 || *order == 2 || *order == 4)))
        warning_at(c, WARNING_INVALID_MEMORY_MODEL, loc, "invalid memory order for atomic operation");
    return true;
}

static Node *atomic_builtin(Context *c, const AtomicBuiltin *builtin, Nodes args, Loc loc)
{
    unsigned flags = builtin->operation;
    unsigned operation = flags & AT_OPERATION_MASK;
    bool legacy = (flags & AT_LEGACY) != 0;
    bool c11 = (flags & AT_C11) != 0, pointers = (flags & AT_POINTER_ARGUMENTS) != 0;
    if (args.len < builtin->arguments ||
        ((!legacy || operation == AT_THREAD_FENCE) && args.len != builtin->arguments)) {
        error(c, loc, "%s requires %u arguments", builtin->name, builtin->arguments);
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    /* The optional protected-variable list of the legacy builtins is ignored. */
    args.len = builtin->arguments;
    Type *nothing = type(c, TY_VOID, 0, 1);
    Type *boolean = type_int(c, 0, true);
    Node *node_value = node(c, N_ATOMIC, loc, NULL, NULL);
    node_value->op = (int)(flags & (AT_OPERATION_MASK | AT_POINTER_ARGUMENTS | AT_RETURN_NEW |
                                    AT_EXPECTED_VALUE | AT_RETURN_OBSERVED));
    node_value->type = nothing;
    node_value->list = args;
    if (operation == AT_THREAD_FENCE || operation == AT_SIGNAL_FENCE) {
        uint64_t order;
        if (!legacy)
            atomic_order(c, &args.data[0], operation, false, &order, loc);
        return node_value;
    }
    if (operation == AT_LOCK_FREE && !c11) {
        Type *size_type = type_int(c, 4, true);
        args.data[0] = implicitly(c, args.data[0], size_type, loc);
        Type *address_type = pointer(c, qualified(c, nothing, S_CONST | S_VOLATILE));
        Node *address = decay(c, args.data[1]);
        args.data[1] = implicitly(c, address, address_type, loc);
        if (flags & AT_ALWAYS) {
            uint64_t width;
            if (!constant_int(c, args.data[0], &width)) {
                error(c, loc, "__atomic_always_lock_free requires a constant size");
                return integer(c, loc, 0, boolean);
            }
            bool aligned = null_pointer_constant(c, address) ||
                           (address->type->kind == TY_PTR && address->type->base->align >= width);
            if (address->kind == N_CAST && address->left->type->kind == TY_PTR)
                aligned |= address->left->type->base->align >= width;
            return integer(c, loc, (width == 1 || width == 2 || width == 4 || width == 8) &&
                           aligned, boolean);
        }
        node_value->type = boolean;
        return node_value;
    }
    Node *object = args.data[0] = decay(c, args.data[0]);
    bool flag = operation == AT_CLEAR || operation == AT_TEST_AND_SET;
    if (object->type->kind != TY_PTR || !object->type->base ||
        (!flag && (!object_type(object->type->base) || !object->type->base->complete ||
                   object->type->base->variable_size || !object->type->base->size))) {
        error(c, loc, "%s requires a pointer to a complete object", builtin->name);
        return integer(c, loc, 0, boolean);
    }
    Type *qualified_value = object->type->base;
    if (c11 && !(qualified_value->qualifiers & S_ATOMIC))
        error(c, loc, "C11 atomic operation requires an atomic object");
    if (operation != AT_LOAD && operation != AT_LOCK_FREE && (qualified_value->qualifiers & S_CONST))
        error(c, loc, "atomic operation modifies a const object");
    Type *value = flag ? boolean : unqualified_type(c, qualified_value);
    if (!value) {
        error(c, loc, "invalid atomic object type");
        return integer(c, loc, 0, boolean);
    }
    node_value->operand_type = value;
    if (operation == AT_LOCK_FREE) {
        node_value->type = boolean;
        return node_value;
    }
    bool rmw = operation >= AT_ADD && operation <= AT_NAND;
    if ((rmw || (!pointers && !c11 && !flag)) &&
        (!integer_type(value) && value->kind != TY_PTR))
        error(c, loc, "%s requires an integer or pointer object", builtin->name);
    if (legacy && value->kind == TY_BOOL)
        error(c, loc, "unsupported operand type for legacy atomic operation");
    uint64_t order = 0;
    if (operation == AT_COMPARE) {
        if (flags & AT_EXPECTED_VALUE) {
            args.data[1] = implicitly(c, args.data[1], value, loc);
            args.data[2] = implicitly(c, args.data[2], value, loc);
            node_value->type = flags & AT_RETURN_OBSERVED ? value : boolean;
            return node_value;
        }
        atomic_pointer_argument(c, args, 1, value, true, loc);
        if (pointers)
            atomic_pointer_argument(c, args, 2, value, false, loc);
        else
            args.data[2] = implicitly(c, args.data[2], value, loc);
        args.data[3] = convert(c, args.data[3], boolean, loc);
        uint64_t success = 0;
        bool known = atomic_order(c, &args.data[4], operation, false, &success, loc);
        if (atomic_order(c, &args.data[5], operation, true, &order, loc) && known &&
            order <= 5 && success <= 5 &&
            (order > success || ((order == 1 || order == 2) && success == 3)))
            warning_at(c, WARNING_INVALID_MEMORY_MODEL, loc, "compare-exchange failure order is stronger than success order");
        node_value->type = boolean;
        return node_value;
    }
    if (!legacy)
        atomic_order(c, &args.data[args.len - 1], operation, false, &order, loc);
    if (pointers) {
        atomic_pointer_argument(c, args, 1, value, operation == AT_LOAD, loc);
        if (operation == AT_EXCHANGE)
            atomic_pointer_argument(c, args, 2, value, true, loc);
    } else if (operation == AT_STORE || operation == AT_EXCHANGE || rmw) {
        if (rmw && value->kind == TY_PTR) {
            Type *wide = type_int(c, 4, false);
            Node *amount = implicitly(c, args.data[1], wide, loc);
            if (c11) {
                if ((operation != AT_ADD && operation != AT_SUB) || !object_type(value->base) ||
                    !value->base->complete)
                    error(c, loc, "invalid C11 atomic pointer arithmetic");
                amount = binary(c, '*', amount, integer(c, loc, value->base->size, wide), loc);
            }
            args.data[1] = amount;
        } else {
            args.data[1] = implicitly(c, args.data[1], value, loc);
        }
    }
    if (operation == AT_TEST_AND_SET)
        node_value->type = boolean;
    else if (!pointers && (operation == AT_LOAD || operation == AT_EXCHANGE || rmw))
        node_value->type = value;
    return node_value;
}

static void check_fortified_size(Context *c, const FortifiedBuiltin *builtin, Nodes args, Loc loc)
{
    if (!builtin->size_arg || args.len < builtin->size_arg)
        return;
    uint64_t limit, count;
    if (!constant_int(c, args.data[builtin->size_arg - 1], &limit) || limit == UINT64_MAX)
        return;
    if (builtin->count_arg) {
        if (!constant_int(c, args.data[builtin->count_arg - 1], &count))
            return;
    } else {
        Node *source = args.data[1];
        while (source->kind == N_CAST)
            source = source->left;
        if (source->kind != N_ADDR || source->left->kind != N_SYMBOL ||
            !source->left->symbol->bytes || source->left->type->kind != TY_ARRAY ||
            source->left->type->base->size != 1)
            return;
        const Symbol *string = source->left->symbol;
        count = strlen((const char *)string->bytes) + 1;
    }
    if (count > limit)
        warning_at(c, WARNING_STRINGOP_OVERFLOW, loc, "%s requires %llu bytes in a destination with %llu bytes remaining",
                builtin->name, (unsigned long long)count, (unsigned long long)limit);
}

static Node *stack_allocation(Context *c, Builtin builtin, Nodes args, Loc loc)
{
    const char *name = builtins[builtin];
    bool aligned = builtin != BUILTIN_ALLOCA;
    unsigned expected = builtin == BUILTIN_ALLOCA_ALIGN_MAX ? 3 : aligned ? 2 : 1;
    Type *size_type = type_int(c, 4, true);
    Type *result = pointer(c, type(c, TY_VOID, 0, 1));
    if (!c->function || args.len != expected) {
        error(c, loc, "%s requires %u arguments and block scope", name, expected);
        return integer(c, loc, 0, result);
    }
    uint64_t alignment = 16 * CHAR_BIT;
    if (aligned &&
        (!constant_int(c, args.data[1], &alignment) || alignment < CHAR_BIT ||
         alignment > (UINT64_C(1) << 31) || (alignment & (alignment - 1)))) {
        error(c, loc, "%s requires a power-of-two alignment from 8 to 2147483648 bits", name);
        return integer(c, loc, 0, result);
    }
    if (expected == 3) {
        Node *maximum = implicitly(c, args.data[2], size_type, loc);
        uint64_t ignored;
        if (!constant_int(c, maximum, &ignored))
            error(c, loc, "%s requires a constant size bound", name);
    }
    Node *size = implicitly(c, args.data[0], size_type, loc);
    Node *n = node(c, N_ALLOCA, loc, size, NULL);
    n->type = result;
    n->integer = alignment / CHAR_BIT;
    n->flags = aligned ? BLOCK_STACK_ALLOCATION : 0;
    c->function->has_stack_alloca = true;
    c->scope->aligned_alloca |= aligned;
    return n;
}

static Type *library_allocator_type(Context *c, Symbol *s, Type *fn)
{
    if (!library_builtin_enabled(c, s) || fn->allocation.first || !fn->prototype)
        return fn;
    const AllocatorBuiltin *b = find_allocator_builtin(s->name, false);
    if (!b || !b->allocation.first || (!c->dialect.gnu && c->dialect.standard < b->standard))
        return fn;
    Scope *outer = c->scope;
    while (outer->parent)
        outer = outer->parent;
    Symbol *known = find_in(outer, b->name);
    if (!known || !compatible(fn, known->type, true))
        return fn;
    Type *result = nullability_type(c, known->type, fn->nullability, s->loc);
    result = access_type(c, result, fn->accesses);
    return fn->warn_unused_result ? warn_unused_result_type(c, result, s->loc) : result;
}

Node *vector_convert(Context *c, Node *source, Type *target, Loc loc)
{
    if (source->type->kind != TY_VECTOR || target->kind != TY_VECTOR ||
        source->type->length != target->length) {
        error(c, loc, "__builtin_convertvector requires vector types with the same number of elements");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Node *result = node(c, N_VECTOR_CONVERT, loc, source, NULL);
    result->type = target;
    return result;
}

static Node *vector_shuffle(Context *c, Nodes args, Loc loc)
{
    if (args.len == 2 || args.len == 3) {
        Node *first = args.data[0], *second = args.len == 3 ? args.data[1] : NULL;
        Node *mask = args.data[args.len - 1];
        Type *t = first->type, *m = mask->type;
        if (t->kind == TY_VECTOR && (!second || same_type(t, second->type)) &&
            m->kind == TY_VECTOR && integer_type(m->base) && t->length == m->length && t->size == m->size) {
            Node *result = node(c, N_VECTOR_SHUFFLE, loc, first, second);
            result->type = unqualified_type(c, t);
            result->third = mask;
            return result;
        }
    }
    error(c, loc, "__builtin_shuffle requires matching vectors and an integer mask of the same size and length");
    return integer(c, loc, 0, type_int(c, 3, false));
}

static Node *vector_shuffle_constant(Context *c, Nodes args, Loc loc)
{
    size_t count = args.len >= 2 ? args.len - 2 : 0;
    if (!count || (count & (count - 1))) {
        error(c, loc, "__builtin_shufflevector requires a power-of-two number of indices");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Node *first = args.data[0], *second = args.data[1];
    Type *a = first->type, *b = second->type;
    if (a->kind != TY_VECTOR || b->kind != TY_VECTOR || !same_type(a->base, b->base) ||
        count > UINT_MAX / a->base->size) {
        error(c, loc, "__builtin_shufflevector requires vectors with matching element types and a valid result size");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Node *result = node(c, N_VECTOR_SHUFFLE, loc, first, second);
    result->type = vector_type(c, a->base, (unsigned)(count * a->base->size), loc);
    Type *index_type = type_int(c, 4, true);
    for (size_t i = 0; i < count; i++) {
        Node *at = args.data[i + 2];
        Integer128 index;
        if (!constant_integer(c, at, &index) ||
            !((!index.hi && index.lo < a->length + b->length) ||
              (index.hi == UINT64_MAX && index.lo == UINT64_MAX))) {
            error(c, at->loc, "__builtin_shufflevector index must be -1 or a constant in range");
            index.lo = 0;
        }
        PUSH(c, result->list, integer(c, at->loc, index.lo == UINT64_MAX ? 0 : index.lo, index_type));
    }
    return result;
}

typedef struct { Type *function, *generic; } MathCandidate;

static Type *math_function(Node *n)
{
    Type *t = n->type;
    if (t->kind == TY_PTR)
        t = t->base;
    return t->kind == TY_FUNC && t->prototype && !t->variadic ? t : NULL;
}

/* Varying columns match either T or its real component in every candidate. */
static bool math_signature_column(const MathCandidate *candidates, size_t count, size_t column)
{
    bool matches = true, real_matches = true;
    for (size_t i = 0; i < count; i++) {
        Type *fn = candidates[i].function, *generic = candidates[i].generic;
        Type *t = column == fn->members.len ? fn->base : fn->members.data[column].type;
        matches &= compatible(t, generic, false);
        real_matches &= compatible(t, real_component(generic), false);
    }
    return matches || real_matches;
}

static bool math_format_contains(Context *c, Type *outer, Type *inner)
{
    if (outer->kind != inner->kind)
        return false;
    outer = real_component(outer);
    inner = real_component(inner);
    /* bfloat16 and binary16 are incomparable. Wider formats on these targets
       include both the range and precision of the smaller ones. */
    return outer->size > inner->size ||
           (outer->size == inner->size &&
            floating_format(c->target, outer) >= floating_format(c->target, inner) &&
            outer->bfloat == inner->bfloat);
}

static Node *math_error(Context *c, Loc loc, const char *problem)
{
    error(c, loc, "__builtin_tgmath %s", problem);
    return integer(c, loc, 0, type_int(c, 3, false));
}

static Node *type_generic_math(Context *c, Nodes args, Loc loc)
{
    const char *arity_error = "requires at least two prototyped, non-variadic functions "
                              "of the same arity and their call arguments";
    Type *first = args.len ? math_function(args.data[0]) : NULL;
    if (!first || args.len < first->members.len || args.len - first->members.len < 2)
        return math_error(c, loc, arity_error);
    size_t arity = first->members.len, count = args.len - arity;
    MathCandidate *candidates = alloc(c, count * sizeof *candidates);
    for (size_t i = 0; i < count; i++) {
        Type *fn = math_function(args.data[i]);
        if (!fn || fn->members.len != arity)
            return math_error(c, loc, arity_error);
        candidates[i].function = fn;
    }

    /* A varying column with a complex parameter identifies T even when an
       earlier varying column holds only the real components. */
    size_t discriminator = arity;
    bool complex_column = false;
    bool *varies = alloc(c, (arity + 1) * sizeof *varies);
    for (size_t column = 0; column <= arity; column++) {
        Type *lead = column == arity ? first->base : first->members.data[column].type;
        bool complex = lead->kind == TY_COMPLEX;
        varies[column] = false;
        for (size_t i = 1; i < count; i++) {
            Type *fn = candidates[i].function;
            Type *t = column == arity ? fn->base : fn->members.data[column].type;
            varies[column] |= !compatible(lead, t, false);
            complex |= t->kind == TY_COMPLEX;
        }
        if (column < arity && varies[column] && (discriminator == arity || (complex && !complex_column))) {
            discriminator = column;
            complex_column = complex;
        }
    }
    const char *signature_error = "requires distinct function types related by a floating-point parameter type";
    if (discriminator == arity)
        return math_error(c, loc, signature_error);
    bool real_candidate = false;
    for (size_t i = 0; i < count; i++) {
        Type *t = candidates[i].function->members.data[discriminator].type;
        if (real_component(t)->kind != TY_FLOAT)
            return math_error(c, loc, signature_error);
        for (size_t j = 0; j < i; j++)
            if (compatible(t, candidates[j].generic, false))
                return math_error(c, loc, signature_error);
        candidates[i].generic = t;
        real_candidate |= t->kind == TY_FLOAT;
    }
    for (size_t column = 0; column <= arity; column++)
        if (varies[column] && !math_signature_column(candidates, count, column))
            return math_error(c, loc, signature_error);

    bool complex = !real_candidate, integer_argument = false;
    Type *wanted = NULL;
    Nodes arguments = {.data = args.data + count, .len = arity};
    for (size_t i = 0; i < arity; i++) {
        if (!varies[i])
            continue;
        Type *arg = arguments.data[i]->type;
        Type *real = real_component(arg);
        complex |= arg->kind == TY_COMPLEX;
        if (real && real->kind == TY_FLOAT) {
            wanted = wanted ? common_type(c, wanted, real) : real;
        } else if (integer_type(real)) {
            integer_argument = true;
        } else {
            return math_error(c, loc, "requires an arithmetic argument for each varying parameter");
        }
    }
    if (!wanted || (integer_argument && wanted->size < 8))
        wanted = type_float(c, 8);
    if (complex)
        wanted = complex_type(c, wanted);

    size_t fallback = count;
    bool narrow = !varies[arity] && first->base->kind == TY_FLOAT;
    for (size_t i = 0; i < count; i++) {
        Type *generic = candidates[i].generic;
        if (compatible(generic, wanted, false))
            return call(c, args.data[i], arguments, loc);
        if (narrow && fallback == count && math_format_contains(c, generic, wanted))
            fallback = i;
    }
    if (fallback < count)
        return call(c, args.data[fallback], arguments, loc);
    return math_error(c, loc, "has no matching function for the argument types");
}

static Node *raw_float(Context *c, Loc loc, Type *type, Integer128 bits)
{
    Node *n = node(c, N_FLOAT, loc, NULL, NULL);
    n->type = type;
    n->flags = FLOAT_BITS_LITERAL;
    n->integer = bits.lo;
    n->integer_high = bits.hi;
    return n;
}

static bool real_floating(Type *t)
{
    return t->kind == TY_FLOAT || t->kind == TY_DECIMAL;
}

static bool floating_argument_bits(Context *c, Node *n, Integer128 *bits)
{
    return n->type->kind == TY_DECIMAL ? decimal_constant(c, n, bits, true) :
                                       binary_constant_bits(c, n, bits, true);
}

static Type *ordered_comparison_type(Context *c, Type *a, Type *b)
{
    if (mixed_decimal_types(a, b)) {
        Type *decimal = a->kind == TY_DECIMAL ? a : b;
        Type *binary = a->kind == TY_FLOAT ? a : b;
        return decimal->size > binary->size ? decimal : binary;
    }
    return common_type(c, a, b);
}

static Node *floating_call(Context *c, const FloatingFunction *function, Nodes args, Loc loc)
{
    FloatingBuiltin op = function->op;
    bool compare = op >= FP_ISUNORDERED;
    size_t count = op == FP_INFINITY ? 0 : op == FP_CLASSIFY ? 6 : compare || op == FP_COPYSIGN ? 2 : 1;
    Type *result = type_int(c, 3, false);
    if (args.len != count) {
        error(c, loc, "%s expects %zu argument(s)", function->name, count);
        return integer(c, loc, 0, result);
    }
    Type *fp = floating_builtin_type(c, function->type);
    if (op == FP_INFINITY)
        return raw_float(c, loc, fp, floating_special(builtin_float_format(c->target, fp), op, NULL));
    if (op == FP_NAN || op == FP_SNAN) {
        Node *arg = implicitly(c, args.data[0], nan_string_type(c), loc);
        Symbol *literal = NULL;
        uint64_t offset = 0;
        unsigned format = builtin_float_format(c->target, fp);
        if (address_constant(c, arg, &literal, &offset) && literal && literal->bytes && offset < literal->byte_count)
            return raw_float(c, loc, fp, floating_special(format, op, (const char *)literal->bytes + offset));
        Node *n = node(c, N_FLOAT_BUILTIN, loc, arg, NULL);
        n->type = n->operand_type = fp;
        n->op = op;
        return n;
    }

    Node *left = args.data[op == FP_CLASSIFY ? 5 : 0], *right = count == 2 ? args.data[1] : NULL;
    if (fp) {
        left = implicitly(c, left, fp, loc);
        if (right)
            right = implicitly(c, right, fp, loc);
    } else if (compare) {
        left = promote(c, decay(c, left));
        right = promote(c, decay(c, right));
        bool real_left = real_floating(left->type), real_right = real_floating(right->type);
        if ((!real_left && !integer_type(left->type)) || (!real_right && !integer_type(right->type)) ||
            (!real_left && !real_right)) {
            error(c, loc, "%s requires real arithmetic arguments, including a floating-point value", function->name);
            return integer(c, loc, 0, result);
        }
        fp = ordered_comparison_type(c, left->type, right->type);
        left = convert(c, left, fp, loc);
        right = convert(c, right, fp, loc);
    } else {
        fp = left->type;
        if (!real_floating(fp)) {
            error(c, loc, "%s requires a real floating-point argument", function->name);
            return integer(c, loc, 0, result);
        }
    }
    if (c->failed)
        return integer(c, loc, 0, result);
    if (op == FP_ABS || op == FP_COPYSIGN)
        result = fp;
    Node *n = node(c, N_FLOAT_BUILTIN, loc, left, right);
    n->type = result;
    n->operand_type = fp;
    n->op = op;
    if (op == FP_CLASSIFY) {
        for (size_t i = 0; i < 5; i++) {
            uint64_t code;
            if (!constant_int(c, args.data[i], &code)) {
                error(c, args.data[i]->loc, "%s requires five integer constant class codes", function->name);
                return integer(c, loc, 0, result);
            }
            PUSH(c, n->list, integer(c, args.data[i]->loc, code, result));
        }
    }
    Integer128 a, b = {0, 0};
    if (!floating_argument_bits(c, left, &a) || (right && !floating_argument_bits(c, right, &b)))
        return n;
    unsigned format = builtin_float_format(c->target, fp);
    if (op == FP_ABS || op == FP_COPYSIGN)
        return raw_float(c, loc, fp, floating_sign(format, a, b));
    int answer = floating_predicate(format, op, a, b);
    return op == FP_CLASSIFY ? n->list.data[answer] : integer(c, loc, (uint64_t)(int64_t)answer, result);
}

static Node *source_builtin(Context *c, Builtin builtin, Symbol *function, size_t count, Loc loc)
{
    Type *result = function->type->base;
    if (count)
        error(c, loc, "%s takes no arguments", function->name);
    if (builtin == BUILTIN_LINE)
        return integer(c, loc, loc.line, result);
    const char *name = builtin == BUILTIN_FILE ? loc.file : c->function ? c->function->name : "";
    if (!name)
        name = "";
    return decay(c, string_literal(c, result->base, (const unsigned char *)name, strlen(name) + 1, loc));
}

static Node *expect_builtin(Context *c, Builtin builtin, Type *fn, Nodes args, Loc loc)
{
    bool explicit_probability = builtin == BUILTIN_EXPECT_PROBABILITY;
    if (args.len != (explicit_probability ? 3u : 2u)) {
        error(c, loc, "incorrect number of arguments to %s", builtins[builtin]);
        return integer(c, loc, 0, fn->base);
    }
    if (explicit_probability) {
        Node *probability = implicitly(c, args.data[2], fn->members.data[2].type, loc);
        long double value;
        if (!real_constant(c, probability, &value, true) || !(value >= 0.0L && value <= 1.0L))
            error(c, probability->loc, "%s requires a constant probability between 0 and 1", builtins[builtin]);
    }
    Node *result = cast_expression(c, implicitly(c, args.data[0], fn->base, loc), fn->base, loc);
    Node *expected = implicitly(c, args.data[1], fn->base, loc);
    uint64_t constant;
    return constant_int(c, expected, &constant) ? result : comma_expression(c, expected, result, loc);
}

static Node *assume_aligned_builtin(Context *c, Type *fn, Nodes args, Loc loc)
{
    if (args.len < 2 || args.len > 3) {
        error(c, loc, "__builtin_assume_aligned requires two or three arguments");
        return convert(c, integer(c, loc, 0, type_int(c, 3, false)), fn->base, loc);
    }
    Node *ptr = implicitly(c, args.data[0], fn->members.data[0].type, loc);
    Node *alignment = implicitly(c, args.data[1], fn->members.data[1].type, loc);
    Node *result = convert(c, ptr, fn->base, loc);
    uint64_t constant;
    if (!constant_int(c, alignment, &constant))
        result = comma_expression(c, alignment, result, loc);
    if (args.len == 3) {
        Node *offset = args.data[2];
        if (!integer_type(offset->type))
            error(c, offset->loc, "offset to __builtin_assume_aligned must be an integer");
        else if (!constant_int(c, offset, &constant))
            result = comma_expression(c, promote(c, offset), result, loc);
    }
    return result;
}

static Node *cache_builtin(Context *c, Builtin builtin, Type *nothing, Nodes args, Loc loc)
{
    bool prefetch = builtin == BUILTIN_PREFETCH;
    Node *n = node(c, prefetch ? N_PREFETCH : N_CLEAR_CACHE, loc, NULL, NULL);
    n->type = nothing;
    if ((!prefetch && args.len != 2) || !args.len) {
        error(c, loc, "incorrect number of arguments to %s", builtins[builtin]);
        return n;
    }
    Type *address = pointer(c, prefetch ? qualified(c, nothing, S_CONST) : nothing);
    n->left = implicitly(c, args.data[0], address, loc);
    if (!prefetch) {
        n->right = implicitly(c, args.data[1], address, loc);
        return n;
    }

    unsigned hints[] = {0, 3};
    const unsigned limits[] = {2, 3};
    const char *const positions[] = {"second", "third"};
    for (size_t i = 1; i <= 2 && i < args.len; i++) {
        Node *arg = args.data[i];
        uint64_t hint;
        if (!constant_int(c, arg, &hint))
            error(c, arg->loc, "%s argument to __builtin_prefetch must be an integer constant", positions[i - 1]);
        else if (hint > limits[i - 1]) {
            warning(c, arg->loc, "invalid %s argument to __builtin_prefetch; using zero", positions[i - 1]);
            hints[i - 1] = 0;
        } else {
            hints[i - 1] = (unsigned)hint;
        }
    }
    n->op = hints[0] == 1;
    n->integer = hints[1];
    for (size_t i = 3; i < args.len; i++)
        PUSH(c, n->list, decay(c, args.data[i]));
    return n;
}

static bool valid_padding_type(Context *c, Type *t, Loc loc)
{
    if (t->kind == TY_ARRAY) {
        if (!t->complete) {
            error(c, loc, "__builtin_clear_padding cannot clear a flexible array member");
            return false;
        }
        return valid_padding_type(c, t->base, loc);
    }
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        if (t->variable_size) {
            error(c, loc, "__builtin_clear_padding does not support variable-length records");
            return false;
        }
        for (size_t i = 0; i < t->members.len; i++)
            if (!valid_padding_type(c, t->members.data[i].type, loc))
                return false;
    }
    return true;
}

static Node *clear_padding_builtin(Context *c, Type *nothing, Nodes args, Loc loc)
{
    Node *n = node(c, N_CLEAR_PADDING, loc, NULL, NULL);
    n->type = nothing;
    if (args.len != 1) {
        error(c, loc, "__builtin_clear_padding requires one argument");
        return n;
    }
    Node *arg = decay(c, args.data[0]);
    if (arg->type->kind != TY_PTR) {
        error(c, arg->loc, "__builtin_clear_padding requires a pointer");
        return n;
    }
    Type *t = arg->type->base;
    if ((t->kind != TY_FUNC && !t->complete) || t->kind == TY_VOID)
        error(c, arg->loc, "__builtin_clear_padding requires a pointer to a complete type");
    else if (t->qualifiers & (S_CONST | S_ATOMIC))
        error(c, arg->loc, "__builtin_clear_padding cannot clear a const or atomic object");
    else if (valid_padding_type(c, t, arg->loc)) {
        Nodes evaluations = {0};
        n->operand_type = save_array_sizes(c, t, &evaluations, loc);
        n->left = arg;
        return with_type_evaluations(c, evaluations, n, loc);
    }
    return n;
}

static Node *jump_builtin(Context *c, Builtin builtin, Type *fn, Nodes args, Loc loc)
{
    bool save = builtin == BUILTIN_SETJMP;
    Node *n = node(c, save ? N_SETJMP : N_LONGJMP, loc, NULL, NULL);
    n->type = fn->base;
    if (args.len != (save ? 1u : 2u)) {
        error(c, loc, "incorrect number of arguments to %s", builtins[builtin]);
        return n;
    }
    if (!c->function) {
        error(c, loc, "%s requires function scope", builtins[builtin]);
        return n;
    }
    n->left = implicitly(c, args.data[0], fn->members.data[0].type, loc);
    if (save) {
        c->function->has_builtin_setjmp = true;
    } else {
        Node *value = implicitly(c, args.data[1], fn->members.data[1].type, loc);
        uint64_t constant;
        if (!constant_int(c, value, &constant) || constant != 1)
            error(c, value->loc, "second argument to __builtin_longjmp must be 1");
    }
    return n;
}

static Node *frame_builtin(Context *c, Builtin builtin, Type *fn, Nodes args, Loc loc)
{
    const char *name = builtins[builtin];
    Node *n = node(c, N_FRAME_QUERY, loc, NULL, NULL);
    n->type = fn->base;
    n->op = builtin;
    n->name = name;
    if (args.len != fn->members.len) {
        error(c, loc, "incorrect number of arguments to %s", name);
        return n;
    }
    if (!args.len)
        return n;
    Type *parameter = fn->members.data[0].type;
    Node *arg = implicitly(c, args.data[0], parameter, loc);
    if (parameter->kind == TY_PTR) {
        n->left = arg;
    } else {
        uint64_t constant;
        if (!constant_int(c, arg, &constant))
            error(c, arg->loc, "%s requires a constant integer argument", name);
        else
            n->integer = (uint32_t)constant;
    }
    return n;
}

static Node *stack_builtin(Context *c, Builtin builtin, Type *fn, Nodes args, Loc loc)
{
    Node *n = node(c, builtin == BUILTIN_STACK_SAVE ? N_STACK_SAVE : N_STACK_RESTORE,
                   loc, NULL, NULL);
    n->type = fn->base;
    if (args.len != fn->members.len) {
        error(c, loc, "incorrect number of arguments to %s", builtins[builtin]);
        return n;
    }
    if (args.len)
        n->left = implicitly(c, args.data[0], fn->members.data[0].type, loc);
    if (c->function)
        c->function->has_explicit_stack = true;
    return n;
}

static uint64_t type_class(Type *t, bool expression)
{
    switch (t->kind) {
    case TY_VOID: return 0;
    case TY_BOOL: return expression ? 1 : 4;
    case TY_INT: return !expression && t->enumeration ? 3 : 1;
    case TY_PTR: return 5;
    case TY_FLOAT: case TY_DECIMAL: return 8;
    case TY_COMPLEX: return 9;
    case TY_FUNC: return expression ? 5 : 10;
    case TY_STRUCT: return 12;
    case TY_UNION: return 13;
    case TY_ARRAY: return expression ? 5 : 14;
    case TY_VECTOR: return 19;
    }
    return UINT64_MAX;
}

Node *classify_type_call(Context *c, Node *callee, Type *t, Loc loc)
{
    if (callee->kind != N_SYMBOL || callee->type->kind != TY_FUNC ||
        !equal(callee->symbol->name, builtins[BUILTIN_CLASSIFY_TYPE]))
        error(c, loc, "expected an expression as the function argument");
    return integer(c, loc, type_class(t, false), type_int(c, 3, false));
}

static Node *classify_expressions(Context *c, Nodes args, Loc loc)
{
    for (size_t i = 0; i < args.len; i++) {
        Node *arg = args.data[i];
        Type *type = arg->type;
        if (type->kind == TY_VOID ||
            (!type->complete && type->kind != TY_ARRAY && type->kind != TY_FUNC)) {
            error(c, arg->loc, "__builtin_classify_type requires a complete argument value");
            return integer(c, loc, 0, type_int(c, 3, false));
        }
    }
    uint64_t category = args.len ? type_class(args.data[0]->type, true) : UINT64_MAX;
    Node *result = integer(c, loc, category, type_int(c, 3, false));
    /* Multiple arguments follow the historical variadic call semantics. */
    if (args.len > 1)
        for (size_t i = args.len; i; i--) {
            Node *arg = decay(c, args.data[i - 1]);
            arg = default_float_promotion(arg->type) ? convert(c, arg, type_float(c, 8), loc) :
                                                        promote(c, arg);
            result = comma_expression(c, arg, result, loc);
        }
    return result;
}

static Node *speculation_safe_value(Context *c, Builtin builtin, Type *fn, Nodes args, Loc loc)
{
    const char *name = builtins[builtin];
    bool generic = builtin == BUILTIN_SPECULATION_SAFE_VALUE;
    if (!args.len || (generic && args.len > 2)) {
        error(c, loc, "incorrect number of arguments to %s", name);
        return integer(c, loc, 0, type_int(c, 3, false));
    }

    Node *first = decay(c, args.data[0]);
    Type *result = fn->base;
    if (generic) {
        Type *t = first->type;
        if (t->kind != TY_PTR && (t->kind != TY_INT || t->enumeration)) {
            error(c, first->loc, "%s requires a pointer or integer as its first argument", name);
            return first;
        }
        if (t->kind == TY_PTR) {
            if (t->base->kind == TY_FUNC)
                extension(c, first->loc, "conversion between function and void pointers");
            if (pointed_to_qualifiers(t->base) & (S_CONST | S_VOLATILE | S_RESTRICT))
                warning_at(c, WARNING_DISCARDED_QUALIFIERS, first->loc,
                           "%s discards qualifiers from the pointed-to type", name);
        }
        if (args.len == 2) {
            Node *fallback = decay(c, args.data[1]);
            Type *other = fallback->type;
            bool compatible_arguments = t->kind == TY_PTR ?
                other->kind == TY_PTR && (t->base->kind != TY_FUNC || other->base->kind == TY_FUNC) :
                !other->enumeration && compatible(t, other, false) && first->width == fallback->width;
            if (!compatible_arguments)
                error(c, fallback->loc, "%s requires compatible arguments", name);
        }
        result = unqualified_type(c, t);
    } else {
        first = implicitly(c, first, result, loc);
    }
    Node *n = node(c, N_SPECULATION_SAFE_VALUE, loc, first, NULL);
    n->type = result;
    for (size_t i = 1; i < args.len; i++) {
        Node *arg = decay(c, args.data[i]);
        if (!arg->type->complete || arg->type->kind == TY_VOID)
            error(c, arg->loc, "%s requires a complete argument value", name);
        PUSH(c, n->list, arg);
    }
    return n;
}

static Node *system_register_builtin(Context *c, const SystemBuiltin *builtin, Type *fn,
                                     Nodes args, Loc loc)
{
    Node *n = node(c, N_SYSTEM_REGISTER, loc, NULL, NULL);
    n->type = fn->base;
    n->op = builtin->reg;
    if (args.len != (builtin->write ? 1u : 0u))
        error(c, loc, "incorrect number of arguments to %s", builtin->name);
    else if (!system_builtin_available(c, builtin) ||
             (builtin->reg == SYSREG_THREAD_POINTER && builtin->write))
        error(c, loc, "'%s' is not supported on this target", builtin->name);
    else if (builtin->write)
        n->left = implicitly(c, args.data[0], type_int(c, builtin->rank, true), loc);
    return n;
}

static bool known_zero(Context *c, Node *arg)
{
    if (arg->type->kind == TY_PTR) {
        uint64_t address;
        if (constant_pointer(c, arg, &address))
            return address == 0;
    } else {
        Integer128 integer_value;
        if (constant_integer(c, arg, &integer_value))
            return !wide_nonzero(integer_value);
    }
    if (arg->kind == N_CAST && (arg->type->kind == TY_PTR || integer_type(arg->type)))
        return known_zero(c, arg->left);
    if (arg->kind == N_COND) {
        Node *yes = arg->postfix ? arg->left : arg->right;
        bool test;
        if (constant_truth(c, arg->left, &test))
            return known_zero(c, test ? yes : arg->third);
        return !expression_has_effect(c, arg->left) && known_zero(c, yes) && known_zero(c, arg->third);
    }
    return false;
}

bool known_null_pointer(Context *c, Node *arg)
{
    return arg->type->kind == TY_PTR && known_zero(c, arg);
}

bool call_returns_nonnull(Node *n)
{
    while (n->kind == N_CAST && n->type->kind == TY_PTR && n->left->type->kind == TY_PTR)
        n = n->left;
    return n->kind == N_CALL && n->left->type->base->nullability.returns_nonnull;
}

static void check_sentinel(Context *c, Type *fn, Nodes args, Loc loc)
{
    if (!fn->sentinels.len || args.len < fn->members.len || !warning_enabled(c, WARNING_FORMAT, loc))
        return;
    unsigned position = (unsigned)fn->sentinels.data[0].position.lo;
    if (position >= args.len - fn->members.len) {
        warning_at(c, WARNING_FORMAT, loc, "not enough variable arguments to fit a sentinel");
        return;
    }
    Node *arg = args.data[args.len - 1 - position];
    if (!known_null_pointer(c, arg))
        warning_at(c, WARNING_FORMAT, loc, "missing sentinel in function call");
}

static void check_nonnull_arguments(Context *c, Type *fn, Symbol *direct, Nodes args, Loc loc)
{
    if (!fn->nullability.nonnull.len || !warning_enabled(c, WARNING_NONNULL, loc))
        return;
    for (size_t i = 0; i < args.len; i++) {
        if (!nonnull_parameter(fn, i + 1) || !known_null_pointer(c, args.data[i]))
            continue;
        warning_at(c, WARNING_NONNULL, loc, "argument %zu null where non-null expected", i + 1);
        if (direct && direct->type->kind == TY_FUNC && direct->loc.file)
            note(c, direct->loc, format(c, "in a call to function '%s' declared 'nonnull'", direct->name));
    }
}

void check_access_argument(Context *c, Symbol *caller, const Symbol *direct, const Node *call,
                           AccessClause clause, IntegerRange counts, AccessRangeSource ranges)
{
    Nodes args = call->list;
    if (clause.reference > args.len || clause.size > args.len)
        return;
    Type *fn = call->left->type->base;
    Loc loc = call->loc;
    Node *address = args.data[clause.reference - 1];
    Type *element = fn->members.data[clause.reference - 1].type->base;
    if (!clause.size && clause.mode == ACCESS_NONE && element->kind == TY_VOID)
        return;
    bool uns = !clause.size || args.data[clause.size - 1]->type->uns;
    if (!uns && counts.upper.hi >> 63) {
        warning_at(c, WARNING_STRINGOP_OVERFLOW, loc, "size argument %u for 'access' is negative", clause.size);
        return;
    }
    Integer128 count = counts.lower;
    if (wide_compare(count, (Integer128){0, 0}, uns) <= 0)
        return;
    if (known_null_pointer(c, address)) {
        if (clause.size)
            warning_at(c, WARNING_NONNULL, loc, "argument %u is null but 'access' size argument %u is nonzero",
                       clause.reference, clause.size);
        return;
    }
    uint64_t width = element->kind == TY_VOID || !element->complete ? 1 : element->size;
    if (!width || element->variable_size)
        return;
    if (count.hi || count.lo > PTRDIFF_MAX / width) {
        warning_at(c, WARNING_STRINGOP_OVERFLOW, loc,
                   "argument %u of 'access' requires more than the maximum object size", clause.reference);
        return;
    }
    uint64_t available, bytes = count.lo * width;
    /* Leave very deep chains or highly branched searches undiagnosed. */
    size_t pointer_search_budget = 128;
    ObjectSizeQuery query = {.context = c, .type = type_int(c, 4, true), .loc = loc,
                             .mode = 1, .access_caller = caller, .ranges = ranges,
                             .pointer_search_budget = &pointer_search_budget};
    Node *limit = pointer_object_size(&query, address, (ObjectOffset){0});
    if (!constant_int(c, limit, &available) || bytes <= available)
        return;
    WarningKind warning = clause.mode == ACCESS_READ || clause.mode == ACCESS_NONE ?
                          WARNING_STRINGOP_OVERREAD : WARNING_STRINGOP_OVERFLOW;
    if (!warning_enabled(c, warning, loc))
        return;
    const char *action = clause.mode == ACCESS_READ ? "reads" : clause.mode == ACCESS_WRITE ? "writes" :
                         clause.mode == ACCESS_READ_WRITE ? "accesses" : "requires";
    const char *minimum = wide_compare(counts.lower, counts.upper, uns) ? "at least " : "";
    warning_at(c, warning, loc, "argument %u %s %s%llu byte%s in a region with at most %llu byte%s remaining",
               clause.reference, action, minimum, (unsigned long long)bytes, bytes == 1 ? "" : "s",
               (unsigned long long)available, available == 1 ? "" : "s");
    if (direct && direct->loc.file)
        note(c, clause.loc, format(c, "in a call to '%s' declared with attribute 'access(%s, ...)'",
                                  direct->name, access_modes[clause.mode]));
}

IntegerRange integer_type_range(const Type *type)
{
    if (type->kind == TY_BOOL)
        return (IntegerRange){{0, 0}, {1, 0}};
    unsigned width = type->size * CHAR_BIT;
    return (IntegerRange){wide_negate(wide_limit(width, !type->uns, true)), wide_limit(width, !type->uns, false)};
}

static Integer128 convert_integer_bound(Integer128 value, const Type *type)
{
    unsigned width = type->size * CHAR_BIT;
    value = wide_binary(value, wide_mask(width), WIDE_AND, true);
    if (!type->uns && wide_nonzero(wide_shift_right(value, width - 1, true)))
        value = wide_binary(value, wide_negate(wide_shift_left((Integer128){1, 0}, width - 1)), WIDE_OR, true);
    return value;
}

IntegerRange convert_integer_range(IntegerRange range, const Type *from, const Type *to)
{
    if (to->kind == TY_BOOL) {
        Integer128 zero = {0, 0}, one = {1, 0};
        if (wide_compare(range.lower, zero, from->uns) > 0 || wide_compare(range.upper, zero, from->uns) < 0)
            return (IntegerRange){one, one};
        if (!wide_nonzero(range.lower) && !wide_nonzero(range.upper))
            return (IntegerRange){zero, zero};
        return (IntegerRange){zero, one};
    }
    IntegerRange converted = {convert_integer_bound(range.lower, to), convert_integer_bound(range.upper, to)};
    /* An interval stays contiguous when conversion preserves its order and width. */
    if (wide_compare(converted.lower, converted.upper, to->uns) <= 0 &&
        !wide_compare(wide_subtract(range.upper, range.lower), wide_subtract(converted.upper, converted.lower), true))
        return converted;
    return integer_type_range(to);
}

static IntegerRange combine_integer_ranges(IntegerRange a, IntegerRange b, bool uns)
{
    return (IntegerRange){wide_compare(a.lower, b.lower, uns) <= 0 ? a.lower : b.lower,
                           wide_compare(a.upper, b.upper, uns) >= 0 ? a.upper : b.upper};
}

static IntegerRange arithmetic_range(IntegerRange a, IntegerRange b, const Type *type, int op)
{
    Integer128 x[] = {a.lower, a.upper}, y[] = {b.lower, b.upper};
    unsigned width = type->size * CHAR_BIT;
    unsigned flags = type->uns ? 0 : OVERFLOW_SIGNED_A | OVERFLOW_SIGNED_B | OVERFLOW_SIGNED_RESULT;
    WideOperation operation = op == '+' ? WIDE_ADD : op == '-' ? WIDE_SUB : WIDE_MUL;
    IntegerRange result = {0};
    size_t pairs = op == '*' ? 4 : 2;
    for (size_t i = 0; i < pairs; i++) {
        Integer128 left = op == '*' ? x[i / 2] : x[i];
        Integer128 right = op == '*' ? y[i % 2] : y[op == '-' ? 1 - i : i];
        if (integer128_overflow(left, right, (unsigned)op, width, flags))
            return integer_type_range(type);
        Integer128 value = wide_binary(left, right, operation, type->uns);
        IntegerRange point = {value, value};
        result = i ? combine_integer_ranges(result, point, type->uns) : point;
    }
    return result;
}

static IntegerRange binary_integer_range(Context *c, Node *n, IntegerRangeLookup lookup, void *data)
{
    Type *type = n->type;
    if (n->op == EQ || n->op == NE || n->op == '<' || n->op == '>' ||
        n->op == LE || n->op == GE || n->op == AND || n->op == OR)
        return (IntegerRange){{0, 0}, {1, 0}};
    if (!integer_type(n->left->type) || !integer_type(n->right->type))
        return integer_type_range(type);
    IntegerRange a = integer_expression_range(c, n->left, lookup, data);
    IntegerRange b = integer_expression_range(c, n->right, lookup, data);
    if (n->op == '+' || n->op == '-' || n->op == '*')
        return arithmetic_range(a, b, type, n->op);
    Integer128 zero = {0, 0};
    if (n->op == '&' && !wide_compare(a.lower, a.upper, type->uns)) {
        IntegerRange swap = a;
        a = b;
        b = swap;
    }
    if (wide_compare(b.lower, b.upper, n->right->type->uns))
        return integer_type_range(type);
    if (n->op == '&' && wide_compare(b.lower, zero, type->uns) >= 0)
        return (IntegerRange){zero, b.lower};
    if (n->op == '|' && (type->uns || (wide_compare(a.lower, zero, false) >= 0 && !(b.lower.hi >> 63))))
        return (IntegerRange){wide_compare(a.lower, b.lower, type->uns) > 0 ? a.lower : b.lower,
                               integer_type_range(type).upper};
    if (n->op == '%' && wide_compare(a.lower, zero, type->uns) >= 0 && wide_compare(b.lower, zero, type->uns) > 0) {
        Integer128 maximum = wide_subtract(b.lower, (Integer128){1, 0});
        return (IntegerRange){zero, wide_compare(a.upper, maximum, type->uns) < 0 ? a.upper : maximum};
    }
    if (n->op == SHR && !b.lower.hi && b.lower.lo < type->size * CHAR_BIT) {
        unsigned shift = (unsigned)b.lower.lo;
        return (IntegerRange){wide_shift_right(a.lower, shift, type->uns),
                               wide_shift_right(a.upper, shift, type->uns)};
    }
    if (n->op == SHL && !b.lower.hi && b.lower.lo < type->size * CHAR_BIT - !type->uns) {
        Integer128 scale = wide_shift_left((Integer128){1, 0}, (unsigned)b.lower.lo);
        return arithmetic_range(a, (IntegerRange){scale, scale}, type, '*');
    }
    return integer_type_range(type);
}

IntegerRange integer_expression_range(Context *c, Node *n, IntegerRangeLookup lookup, void *data)
{
    Integer128 exact;
    if (constant_integer(c, n, &exact))
        return (IntegerRange){exact, exact};
    switch (n->kind) {
    case N_SYMBOL:
        if (lookup)
            return lookup(data, n->symbol);
        break;
    case N_CAST:
        if (integer_type(n->left->type)) {
            IntegerRange source = integer_expression_range(c, n->left, lookup, data);
            return convert_integer_range(source, n->left->type, n->type);
        }
        break;
    case N_COND: {
        Node *yes = n->postfix ? n->left : n->right;
        bool test;
        if (constant_truth(c, n->left, &test)) {
            Node *selected = test ? yes : n->third;
            return convert_integer_range(integer_expression_range(c, selected, lookup, data), selected->type, n->type);
        }
        IntegerRange a = integer_expression_range(c, yes, lookup, data);
        IntegerRange b = integer_expression_range(c, n->third, lookup, data);
        return combine_integer_ranges(convert_integer_range(a, yes->type, n->type),
                                      convert_integer_range(b, n->third->type, n->type), n->type->uns);
    }
    case N_BINARY:
        return binary_integer_range(c, n, lookup, data);
    case N_UNARY:
        if (n->op == '!')
            return (IntegerRange){{0, 0}, {1, 0}};
        if (n->op == '-' && integer_type(n->left->type))
            return arithmetic_range((IntegerRange){0}, integer_expression_range(c, n->left, lookup, data), n->type, '-');
        break;
    case N_SEQUENCE: case N_ASSIGN:
        return integer_expression_range(c, n->right, lookup, data);
    default:
        break;
    }
    return integer_type_range(n->type);
}

Symbol *access_local_object(Context *c, Symbol *caller, const Node *address)
{
    while (address && address->type->kind == TY_PTR) {
        switch (address->kind) {
        case N_ADDR: {
            Node *object = address->left;
            while (object->kind == N_MEMBER)
                object = object->left;
            if (object->kind == N_DEREF) {
                address = object->left;
                break;
            }
            if (object->kind == N_SYMBOL && object->symbol->local && object->symbol->owner == caller)
                return object->symbol;
            return NULL;
        }
        case N_SYMBOL: {
            Symbol *copy = address->symbol;
            if (!tracks_pointer_size(caller, copy) || copy->checking_extent || copy->pointer_assignments.len)
                return NULL;
            copy->checking_extent = true;
            Symbol *object = access_local_object(c, caller, pointer_write_source(copy, 0));
            copy->checking_extent = false;
            return object;
        }
        case N_CAST:
            address = address->left;
            break;
        case N_SEQUENCE: case N_ASSIGN:
            address = address->right;
            break;
        case N_BINARY:
            if (address->op != '+' && address->op != '-')
                return NULL;
            address = address->left;
            break;
        case N_COND: {
            const Node *yes = address->postfix ? address->left : address->right;
            bool truth;
            if (constant_truth(c, address->left, &truth)) {
                address = truth ? yes : address->third;
                break;
            }
            Symbol *left = access_local_object(c, caller, yes);
            return left && left == access_local_object(c, caller, address->third) ? left : NULL;
        }
        default:
            return NULL;
        }
    }
    return NULL;
}

Node *call(Context *c, Node *callee, Nodes args, Loc loc)
{
    Symbol *direct = callee->kind == N_SYMBOL ? callee->symbol : NULL;
    const char *name = direct ? direct->name : "";
    Builtin builtin = direct && direct->type->kind == TY_FUNC ? builtin_kind(name) : BUILTIN_NONE;
    const AtomicBuiltin *atomic = builtin == BUILTIN_ATOMIC ? find_atomic_builtin(name) : NULL;
    if (atomic)
        return atomic_builtin(c, atomic, args, loc);
    if (builtin == BUILTIN_SYSTEM_REGISTER)
        return system_register_builtin(c, find_system_builtin(name), callee->type, args, loc);
    if (builtin == BUILTIN_INTEGER)
        return integer_builtin(c, find_integer_builtin(name), callee->type, args, loc);
    if (builtin == BUILTIN_FLOAT) {
        const FloatingFunction *function = find_floating_builtin(name);
        if (function && floating_builtin_available(c, function))
            return floating_call(c, function, args, loc);
    }
    const OverflowBuiltin *overflow = builtin == BUILTIN_OVERFLOW ? find_overflow_builtin(name) : NULL;
    if (overflow)
        return overflow_builtin(c, overflow, args, loc);
    if (builtin == BUILTIN_OBJECT_SIZE || builtin == BUILTIN_DYNAMIC_OBJECT_SIZE)
        return object_size_builtin(c, args, builtin == BUILTIN_DYNAMIC_OBJECT_SIZE, loc);
    if (builtin == BUILTIN_VECTOR_SHUFFLE)
        return vector_shuffle(c, args, loc);
    if (builtin == BUILTIN_VECTOR_SHUFFLE_CONSTANT)
        return vector_shuffle_constant(c, args, loc);
    if (builtin == BUILTIN_TGMATH)
        return type_generic_math(c, args, loc);
    if (builtin == BUILTIN_ALLOCA || builtin == BUILTIN_ALLOCA_ALIGN ||
        builtin == BUILTIN_ALLOCA_ALIGN_MAX)
        return stack_allocation(c, builtin, args, loc);
    if (builtin == BUILTIN_TRAP || builtin == BUILTIN_UNREACHABLE) {
        if (args.len)
            error(c, loc, "%s takes no arguments", name);
        Node *n = node(c, N_TRAP, loc, NULL, NULL);
        n->type = callee->type->base;
        return n;
    }
    if (builtin == BUILTIN_EXPECT || builtin == BUILTIN_EXPECT_PROBABILITY)
        return expect_builtin(c, builtin, callee->type, args, loc);
    if (builtin == BUILTIN_ASSUME_ALIGNED)
        return assume_aligned_builtin(c, callee->type, args, loc);
    if (builtin == BUILTIN_LINE || builtin == BUILTIN_FILE || builtin == BUILTIN_FUNCTION)
        return source_builtin(c, builtin, direct, args.len, callee->loc);
    if (builtin == BUILTIN_PREFETCH || builtin == BUILTIN_CLEAR_CACHE)
        return cache_builtin(c, builtin, callee->type->base, args, loc);
    if (builtin == BUILTIN_CLEAR_PADDING)
        return clear_padding_builtin(c, callee->type->base, args, loc);
    if (builtin == BUILTIN_SETJMP || builtin == BUILTIN_LONGJMP)
        return jump_builtin(c, builtin, callee->type, args, loc);
    if (builtin == BUILTIN_STACK_SAVE || builtin == BUILTIN_STACK_RESTORE)
        return stack_builtin(c, builtin, callee->type, args, loc);
    if (builtin == BUILTIN_CLASSIFY_TYPE)
        return classify_expressions(c, args, loc);
    if (is_speculation_builtin(builtin))
        return speculation_safe_value(c, builtin, callee->type, args, loc);
    switch (builtin) {
    case BUILTIN_FRAME_ADDRESS: case BUILTIN_RETURN_ADDRESS:
    case BUILTIN_EXTRACT_RETURN_ADDR: case BUILTIN_FROB_RETURN_ADDR:
    case BUILTIN_DWARF_CFA: case BUILTIN_DWARF_SP_COLUMN: case BUILTIN_EH_DATA_REGNO:
    case BUILTIN_DWARF_REG_SIZES: case BUILTIN_EXTEND_POINTER:
        return frame_builtin(c, builtin, callee->type, args, loc);
    default: break;
    }
    bool start = builtin == BUILTIN_VA_START, end = builtin == BUILTIN_VA_END;
    bool copy_list = builtin == BUILTIN_VA_COPY;
    if (start || end || copy_list) {
        size_t count = end ? 1 : 2;
        Type *void_type = type(c, TY_VOID, 0, 1);
        Node *list = args.len == count ? va_object(c, args.data[0]) : NULL;
        Node *copy_from = list && copy_list ? va_object(c, args.data[1]) : NULL;
        if (!list || (copy_list && !copy_from) ||
            (start && (!c->function || !c->function->type->variadic))) {
            error(c, loc, "invalid operands to '%s'", name);
            return integer(c, loc, 0, type_int(c, 3, false));
        }
        if (start && (!c->function->params.len || args.data[1]->kind != N_SYMBOL ||
            args.data[1]->symbol != c->function->params.data[c->function->params.len - 1]))
            error(c, loc, "va_start must name the last fixed parameter");
        if (copy_list)
            return convert(c, assign(c, '=', list, copy_from, false, loc), void_type, loc);
        Node *result = node(c, start ? N_VA_START : N_VA_END, loc, list, NULL);
        result->type = void_type;
        return result;
    }
    callee = decay(c, callee);
    Type *fn = callee->type->kind == TY_PTR ? callee->type->base : NULL;
    if (!fn || fn->kind != TY_FUNC) {
        error(c, loc, "called expression is not a function");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Type *allocation_type = library_allocator_type(c, direct, fn);
    if (allocation_type != fn) {
        fn = allocation_type;
        callee->type = pointer(c, fn);
    }
    if (fn->prototype && (args.len < fn->members.len || (!fn->variadic && args.len != fn->members.len)))
        error(c, loc, "incorrect number of function arguments");
    for (size_t i = 0; i < args.len; i++) {
        Node *arg = decay(c, args.data[i]);
        if (i < fn->members.len) {
            Type *parameter = fn->members.data[i].type;
            arg = parameter->transparent_union ? transparent_argument(c, arg, parameter, loc) :
                                                implicitly(c, arg, parameter, loc);
        } else if (default_float_promotion(arg->type))
            arg = convert(c, arg, type(c, TY_FLOAT, 8, 8), loc);
        else
            arg = promote(c, arg);
        args.data[i] = arg;
    }
    check_sentinel(c, fn, args, loc);
    check_nonnull_arguments(c, fn, direct, args, loc);
    if (c->function) {
        c->function->has_variable_temporary |= fn->base->variable_size;
        for (size_t i = 0; i < args.len; i++)
            c->function->has_variable_temporary |= args.data[i]->type->variable_size;
    }
    const FortifiedBuiltin *fortified = builtin == BUILTIN_FORTIFIED ? find_fortified_builtin(name) : NULL;
    Node *n = node(c, N_CALL, loc, callee, NULL);
    n->list = args;
    n->type = fn->base;
    if (fortified) {
        if (c->function && fortified->size_arg) {
            n->name = fortified->name;
            PUSH(c, c->function->fortified_calls, n);
        } else {
            check_fortified_size(c, fortified, args, loc);
        }
    }
    return n;
}

Node *call_with_static_chain(Context *c, Node *invoke, Node *chain, Loc loc)
{
    chain = decay(c, chain);
    if (invoke->kind != N_CALL || invoke->right)
        error(c, loc, "__builtin_call_with_static_chain requires a function call");
    else if (chain->type->kind != TY_PTR)
        error(c, loc, "the static chain must be a pointer");
    else
        invoke->right = chain;
    return invoke;
}

Node *va_argument(Context *c, Node *list, Type *t, Loc loc)
{
    Node *object = va_object(c, list);
    if (!object)
        error(c, loc, "va_arg requires a va_list object");
    if ((!scalar(t) && !(t->complete && (t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_VECTOR))) ||
        promoted_parameter(t))
        error(c, loc, "va_arg requires a supported type after default argument promotions");
    Node *result = node(c, N_VA_ARG, loc, object ? object : list, NULL);
    result->type = t;
    return result;
}

static bool find_member(Context *c, Type *t, const char *name, Member *match, const Member **declaration)
{
    if (t->kind != TY_STRUCT && t->kind != TY_UNION)
        return false;
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        if (m.name && equal(m.name, name)) {
            *match = m;
            if (declaration)
                *declaration = &t->members.data[i];
            return true;
        }
        if (!m.name && !m.bit_field && find_member(c, m.type, name, match, declaration)) {
            match->offset += m.offset;
            if (m.runtime_offset)
                match->runtime_offset = match->runtime_offset ?
                    layout_op(c, '+', m.runtime_offset, match->runtime_offset, m.runtime_offset->loc) :
                    m.runtime_offset;
            return true;
        }
    }
    return false;
}

Node *member_offset(Context *c, Type *t, Node *path, Loc loc)
{
    Node *offset = layout_number(c, loc, 0);
    Nodes evaluations = {0};
    take_record_bounds(c, t, &evaluations);
    for (; path; path = path->right) {
        if (path->name) {
            Member member;
            if (!find_member(c, t, path->name, &member, NULL) || member.bit_field) {
                error(c, path->loc, "invalid offsetof member '%s'", path->name);
                break;
            }
            if (member.runtime_offset)
                offset = layout_op(c, '+', offset, member.runtime_offset, loc);
            offset = layout_op(c, '+', offset, layout_number(c, loc, member.offset), loc);
            t = member.type;
        } else {
            if (t->kind != TY_ARRAY || !integer_type(path->left->type)) {
                error(c, path->loc, "offsetof requires an integer array subscript");
                break;
            }
            Node *index = convert(c, path->left, offset->type, loc);
            Node *stride = layout_op(c, '*', size_expression(c, t->base, loc), index, loc);
            offset = layout_op(c, '+', offset, stride, loc);
            t = t->base;
        }
    }
    return with_type_evaluations(c, evaluations, offset, loc);
}

Node *field(Context *c, Node *n, const char *name, bool indirect, Loc loc)
{
    if (indirect)
        n = unary(c, '*', n, loc);
    if (n->type->qualifiers & S_ATOMIC)
        error(c, loc, "a member of an atomic object cannot be accessed directly");
    Member m;
    const Member *member;
    if (find_member(c, n->type, name, &m, &member)) {
        warn_deprecated(c, loc, name, m.deprecated);
        Node *field_node = node(c, N_MEMBER, loc, n, m.runtime_offset);
        field_node->member = member;
        field_node->integer = m.offset;
        field_node->bit = m.bit;
        field_node->width = m.width;
        field_node->alignment = m.alignment;
        if (m.packed_bit_field)
            field_node->flags |= PACKED_BIT_FIELD;
        field_node->type = qualified(c, m.type, m.type->qualifiers |
                                     (n->type->qualifiers & (S_CONST | S_VOLATILE)));
        return field_node;
    }
    error(c, loc, "no member named '%s'", name);
    return integer(c, loc, 0, type_int(c, 3, false));
}

Node *measure(Context *c, Type *t, bool alignment, Loc loc)
{
    if (!t->complete)
        error(c, loc, "operand has incomplete type");
    if (t->kind == TY_VOID || t->kind == TY_FUNC)
        extension(c, loc, "size or alignment of void or function type");
    if (t->variable_size && !alignment)
        return size_expression(c, t, loc);
    unsigned value = alignment ? t->align : t->size;
    if (!alignment && (t->kind == TY_VOID || t->kind == TY_FUNC))
        value = 1;
    return integer(c, loc, value, type_int(c, 4, true));
}

Node *measure_expression(Context *c, Node *expr, Loc loc)
{
    Node *size = measure(c, expr->type, false, loc);
    return expr->type->variable_size ? comma_expression(c, expr, size, loc) : size;
}

Node *measure_type(Context *c, Type *t, Loc loc)
{
    Nodes evaluations = {0};
    if (t->variable_size && (c->function || c->scope->parameters))
        t = save_array_sizes(c, t, &evaluations, loc);
    return with_type_evaluations(c, evaluations, measure(c, t, false, loc), loc);
}

unsigned alignment_value(Context *c, Node *n, Loc loc)
{
    uint64_t value = 0;
    if (!integer_type(n->type) || !constant_int(c, n, &value) ||
        value > 65536 || (value & (value - 1))) {
        error(c, loc, "alignment must be zero or a supported power of two");
        return 0;
    }
    return (unsigned)value;
}

unsigned vector_size_value(Context *c, Node *n, Loc loc)
{
    uint64_t size;
    if (!constant_int(c, n, &size) || !size || size > UINT_MAX || (size & (size - 1))) {
        error(c, loc, "vector size must be a positive power of two in bytes");
        return 0;
    }
    return (unsigned)size;
}

AllocationSize allocation_size_spec(Context *c, Node *first, Node *second, Loc loc)
{
    uint64_t a, b = 0;
    if (!constant_int(c, first, &a) || !a || a > UINT_MAX ||
        (second && (!constant_int(c, second, &b) || !b || b > UINT_MAX))) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "alloc_size argument must be a positive integer parameter number");
        return (AllocationSize){0};
    }
    return (AllocationSize){(unsigned)a, (unsigned)b};
}

static bool patchable_entry_value(Context *c, Node *argument, Loc loc, unsigned *value)
{
    Integer128 number;
    if (!constant_integer(c, argument, &number) || number.hi || number.lo > MAX_PATCHABLE_ENTRY) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "'patchable_function_entry' arguments must be integers from 0 to %u",
                   (unsigned)MAX_PATCHABLE_ENTRY);
        return false;
    }
    *value = (unsigned)number.lo;
    return true;
}

const PatchableEntries *patchable_entry_spec(Context *c, Node *first, Node *second, Loc loc)
{
    PatchableEntry entry = {.explicit_before = second != NULL};
    if (!patchable_entry_value(c, first, loc, &entry.count) ||
        (second && !patchable_entry_value(c, second, loc, &entry.before)))
        return NULL;
    PatchableEntries *entries = alloc(c, sizeof *entries);
    PUSH(c, *entries, entry);
    return entries;
}

const PatchableEntries *combine_patchable_entries(Context *c, const PatchableEntries *a, const PatchableEntries *b)
{
    if (!a || !b || a == b)
        return a ? a : b;
    PatchableEntries *entries = alloc(c, sizeof *entries);
    for (size_t i = 0; i < a->len; i++)
        PUSH(c, *entries, a->data[i]);
    for (size_t i = 0; i < b->len; i++) {
        PatchableEntry entry = b->data[i];
        bool duplicate = false;
        for (size_t j = 0; j < a->len && !duplicate; j++) {
            PatchableEntry prior = a->data[j];
            duplicate = prior.count == entry.count && prior.before == entry.before &&
                        prior.explicit_before == entry.explicit_before;
        }
        if (!duplicate)
            PUSH(c, *entries, entry);
    }
    return entries;
}

static bool sentinel_position(Context *c, Node *argument, Loc loc, Sentinel *sentinel)
{
    *sentinel = (Sentinel){.explicit_position = argument != NULL};
    if (!argument)
        return true;
    if (!constant_integer(c, argument, &sentinel->position)) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "'sentinel' position is not an integer constant");
        return false;
    }
    if (!argument->type->uns && sentinel->position.hi >> 63) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "'sentinel' position must be nonnegative");
        return false;
    }
    return true;
}

FunctionNullability nonnull_spec(Context *c, Nodes args, Loc loc)
{
    ParameterIndices indices = {0};
    for (size_t i = 0; i < args.len; i++) {
        Integer128 value;
        if (!constant_integer(c, args.data[i], &value)) {
            warning_at(c, WARNING_ATTRIBUTES, loc, "'nonnull' position is not an integer constant");
            return (FunctionNullability){0};
        }
        if (value.hi || !value.lo || value.lo > UINT_MAX) {
            warning_at(c, WARNING_ATTRIBUTES, loc, "'nonnull' position must be a positive parameter number");
            return (FunctionNullability){0};
        }
        PUSH(c, indices, (unsigned)value.lo);
    }
    FunctionNullability result = {0};
    PUSH(c, result.nonnull, indices);
    return result;
}

Sentinels sentinel_spec(Context *c, Node *argument, Loc loc)
{
    Sentinels result = {0};
    Sentinel sentinel;
    if (sentinel_position(c, argument, loc, &sentinel))
        PUSH(c, result, sentinel);
    return result;
}

Specs lifecycle_spec(Context *c, Token attribute, Node *argument)
{
    bool constructor = attribute_kind(c, attribute.text) == ATTR_CONSTRUCTOR;
    const char *name = constructor ? "constructor" : "destructor";
    uint64_t priority = DEFAULT_LIFECYCLE_PRIORITY;
    if (argument && (!constant_int(c, argument, &priority) || priority > DEFAULT_LIFECYCLE_PRIORITY)) {
        error(c, attribute.loc, "%s priority must be an integer constant from 0 to %d",
              name, DEFAULT_LIFECYCLE_PRIORITY);
        return (Specs){0};
    }
    if (priority <= 100)
        warning_at(c, WARNING_PRIORITY_CTOR_DTOR, attribute.loc, "%s priorities 0 to 100 are reserved for the implementation", name);
    /* Store priority plus one so zero denotes an absent attribute. */
    unsigned stored = (unsigned)priority + 1;
    return constructor ?
        (Specs){.constructor_priority = stored, .attribute_markers = argument ? 0 : DEFAULT_CONSTRUCTOR_ATTRIBUTE} :
        (Specs){.destructor_priority = stored, .attribute_markers = argument ? 0 : DEFAULT_DESTRUCTOR_ATTRIBUTE};
}

static Node *attribute_subject(Context *c, Node *n)
{
    while (n) {
        if (n->kind == N_DEREF && n->left->kind == N_ADDR && same_type(n->type, n->left->left->type))
            n = n->left->left;
        else if (n->kind == N_CAST && same_type(n->type, n->left->type))
            n = n->left;
        else if (n->kind == N_SEQUENCE && n->left && !discard_value(c, n->left, true))
            n = n->right;
        else
            break;
    }
    return n;
}

static size_t attribute_query_arity(Attribute attribute)
{
    switch (attribute) {
    case ATTR_NONNULL:
        return SIZE_MAX;
    case ATTR_ACCESS:
        return 3;
    case ATTR_ALLOC_SIZE: case ATTR_PATCHABLE_FUNCTION_ENTRY:
        return 2;
    case ATTR_ALIGNED: case ATTR_ALIAS: case ATTR_WEAKREF: case ATTR_VISIBILITY:
    case ATTR_CONSTRUCTOR: case ATTR_DESTRUCTOR: case ATTR_CLEANUP: case ATTR_SECTION:
    case ATTR_VECTOR_SIZE: case ATTR_MODE: case ATTR_DEPRECATED: case ATTR_ERROR: case ATTR_WARNING: case ATTR_SENTINEL:
        return 1;
    default:
        return 0;
    }
}

static Symbol *call_diagnostic_literal(Context *c, Node *argument, Attribute kind, Loc loc)
{
    if (argument->kind != N_SYMBOL || !argument->symbol->bytes || argument->type->qualifiers) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "'%s' attribute requires a string literal",
                   kind == ATTR_ERROR ? "error" : "warning");
        return NULL;
    }
    Symbol *literal = argument->symbol;
    if (c->globals.len && c->globals.data[c->globals.len - 1] == literal)
        c->globals.len--;
    return literal;
}

Specs call_diagnostic_spec(Context *c, Token attribute, Node *argument)
{
    Specs specs = {0};
    Attribute kind = attribute_kind(c, attribute.text);
    Symbol *literal = call_diagnostic_literal(c, argument, kind, attribute.loc);
    if (literal)
        PUSH(c, specs.call_diagnostics,
             ((CallDiagnostic){kind, (const char *)literal->bytes, literal->byte_count}));
    return specs;
}

void warn_call_diagnostic_attributes(Context *c, CallDiagnostics diagnostics, Loc loc)
{
    for (size_t i = 0; i < diagnostics.len; i++)
        warning_at(c, WARNING_ATTRIBUTES, loc, "'%s' attribute applies to function declarations only",
                   diagnostics.data[i].kind == ATTR_ERROR ? "error" : "warning");
}

static const char *attribute_query_string(Context *c, Node *value)
{
    if (value->kind != N_SYMBOL || !value->symbol->bytes) {
        error(c, value->loc, "attribute query argument must be a string literal");
        return NULL;
    }
    return asm_string(c, value, value->loc);
}

static const WeakPragma *attribute_weak_pragma(Context *c, const Symbol *s, bool target)
{
    if (s && !s->local && !s->internal && !s->is_typedef && !s->register_binding &&
        !(s->inline_declarations & S_INLINE))
        for (size_t i = 0; i < c->weak_pragmas.len; i++) {
            const WeakPragma *pragma = &c->weak_pragmas.data[i];
            if ((!target || pragma->target) && equal(pragma->name, s->name))
                return pragma;
        }
    return NULL;
}

static bool attribute_query_matches(Context *c, Attribute attribute, Type *t, Node *subject,
                                    const char *name, Nodes args, Loc loc)
{
    const Symbol *s = subject && subject->kind == N_SYMBOL ? subject->symbol : NULL;
    const Member *member = subject && subject->kind == N_MEMBER ? subject->member : NULL;
    Node *arg = args.len ? args.data[0] : NULL;
    switch (attribute) {
    case ATTR_ALIGNED: {
        unsigned type_attributes = t->record_alignment_attributes ? t->record_alignment_attributes :
                                   t->typedef_alignment;
        unsigned attributes = type_attributes | (s ? s->alignment_attributes : 0) |
                              (member ? member->alignment_attributes : 0);
        return arg ? (attributes & alignment_value(c, arg, arg->loc)) != 0 : attributes != 0;
    }
    case ATTR_PACKED: return t->record_packed || t->enum_packed || (member && member->packed_attribute);
    case ATTR_TRANSPARENT_UNION: return t->transparent_union;
    case ATTR_DESIGNATED_INIT: return t->designated_init;
    case ATTR_VECTOR_SIZE:
        if (arg) {
            unsigned size = vector_size_value(c, arg, arg->loc);
            return t->kind == TY_VECTOR && t->size == size;
        }
        return t->kind == TY_VECTOR;
    case ATTR_MODE:
        if (arg && arg->kind == N_DESIGNATE)
            mode_type(c, t, arg->name, arg->loc);
        else if (arg)
            error(c, arg->loc, "mode attribute requires a machine mode name");
        warning_at(c, WARNING_ATTRIBUTES, loc, "mode attribute is unsupported in __builtin_has_attribute");
        return false;
    case ATTR_ALLOC_SIZE: {
        AllocationSize actual = t->kind == TY_FUNC ? t->allocation : (AllocationSize){0};
        if (!arg)
            return actual.first != 0;
        AllocationSize requested = allocation_size_spec(c, arg, args.len == 2 ? args.data[1] : NULL, loc);
        /* GNU C matches the first requested index against either recorded index. */
        return requested.first && (requested.first == actual.first || requested.first == actual.second);
    }
    case ATTR_WEAK: return (s && s->weak) || attribute_weak_pragma(c, s, false);
    case ATTR_WEAKREF:
        if (arg)
            attribute_query_string(c, arg);
        return !arg && s && s->weak_reference;
    case ATTR_ALIAS: case ATTR_SECTION: {
        const char *actual = s ? (attribute == ATTR_ALIAS ? s->alias : s->section) : NULL;
        if (!actual && attribute == ATTR_ALIAS) {
            const WeakPragma *pragma = attribute_weak_pragma(c, s, true);
            if (pragma)
                actual = pragma->target;
        }
        const char *requested = arg ? attribute_query_string(c, arg) : NULL;
        return actual && (!arg || (requested && equal(actual, requested)));
    }
    case ATTR_DEPRECATED: {
        const char *requested = arg ? attribute_query_string(c, arg) : NULL;
        const char *declared = s ? s->deprecated : member ? member->deprecated : NULL;
        return (declared && (!arg || (requested && equal(declared, requested)))) ||
               (t->deprecated && (!arg || (requested && equal(t->deprecated, requested))));
    }
    case ATTR_ERROR: case ATTR_WARNING: {
        Symbol *requested = arg ? call_diagnostic_literal(c, arg, attribute, arg->loc) : NULL;
        if (!s || (arg && (!requested || requested->type->base->size != 1)))
            return false;
        for (size_t i = 0; i < s->call_diagnostics.len; i++) {
            CallDiagnostic item = s->call_diagnostics.data[i];
            if (item.kind == attribute && (!requested || (item.length == requested->byte_count &&
                !memcmp(item.message, requested->bytes, item.length))))
                return true;
        }
        return false;
    }
    case ATTR_VISIBILITY: {
        const char *requested = arg ? attribute_query_string(c, arg) : NULL;
        Visibility visibility = requested ? visibility_kind(requested) : VIS_UNSPECIFIED;
        if (requested && !visibility)
            error(c, arg->loc, "invalid symbol visibility '%s'", requested);
        return s && s->explicit_visibility && (!arg || (visibility && s->visibility == visibility));
    }
    case ATTR_CONSTRUCTOR: case ATTR_DESTRUCTOR: {
        bool ctor = attribute == ATTR_CONSTRUCTOR;
        unsigned priority = s ? (ctor ? s->constructor_priority : s->destructor_priority) : 0;
        if (!arg)
            return priority != 0;
        Specs requested = lifecycle_spec(c, (Token){.text = name, .loc = loc}, arg);
        unsigned expected = ctor ? requested.constructor_priority : requested.destructor_priority;
        unsigned implicit = ctor ? DEFAULT_CONSTRUCTOR_ATTRIBUTE : DEFAULT_DESTRUCTOR_ATTRIBUTE;
        return s && priority && !(s->attribute_markers & implicit) && priority == expected;
    }
    case ATTR_CLEANUP:
        if (arg && (arg->kind != N_SYMBOL || arg->type->kind != TY_FUNC)) {
            error(c, arg->loc, "cleanup argument must name a function");
            return false;
        }
        if (!s || !s->cleanup || !arg)
            return s && s->cleanup;
        if (s->cleanup->kind == N_CALL && s->cleanup->left->kind == N_ADDR) {
            Node *function = s->cleanup->left->left;
            return function->kind == N_SYMBOL && function->symbol == arg->symbol;
        }
        return false;
    case ATTR_GNU_INLINE: return s && (s->inline_declarations & S_GNU_INLINE);
    case ATTR_NOINLINE: return s && (s->attribute_markers & MARK_NOINLINE);
    case ATTR_NOPLT: return s && (s->attribute_markers & MARK_NOPLT);
    case ATTR_ACCESS: {
        if (!args.len)
            return t->kind == TY_FUNC && t->accesses;
        if (args.len < 2) {
            error(c, loc, "'access' attribute requires a mode and pointer argument position");
            return false;
        }
        AccessClause clause;
        if (make_access_clause(c, arg->kind == N_DESIGNATE ? arg->name : NULL,
                               args.data[1], args.len == 3 ? args.data[2] : NULL, loc, &clause) &&
            t->kind == TY_FUNC && valid_access_clause(c, t, clause)) {
            AccessClauses requested = {.data = &clause, .len = 1};
            combine_accesses(c, t->accesses, &requested, true);
        }
        /* GNU queries recognize the attribute by name; its arguments use a normalized representation. */
        return false;
    }
    case ATTR_PATCHABLE_FUNCTION_ENTRY: {
        const PatchableEntries *entries = member ? member->patchable_entries : s ? s->patchable_entries : NULL;
        if (!arg)
            return entries != NULL;
        unsigned requested, before;
        if (!patchable_entry_value(c, arg, loc, &requested))
            return false;
        if (args.len == 2)
            patchable_entry_value(c, args.data[1], loc, &before);
        /* Attribute queries match the patch area size. */
        for (size_t i = 0; entries && i < entries->len; i++)
            if (entries->data[i].count == requested)
                return true;
        return false;
    }
    case ATTR_USED: return s && (s->attribute_markers & MARK_USED);
    case ATTR_NORETURN:
        return (s && (s->attribute_markers & MARK_NORETURN)) || function_noreturn(c, s) ||
               (member && (member->attribute_markers & MARK_NORETURN));
    case ATTR_WARN_UNUSED_RESULT: return t->kind == TY_FUNC && t->warn_unused_result;
    case ATTR_SENTINEL: {
        Sentinel requested;
        if (!sentinel_position(c, arg, loc, &requested) || t->kind != TY_FUNC)
            return false;
        return arg ? has_sentinel(t->sentinels, requested) : t->sentinels.len != 0;
    }
    case ATTR_NONNULL: {
        if (t->kind != TY_FUNC)
            return false;
        if (!args.len) {
            for (size_t i = 0; i < t->nullability.nonnull.len; i++)
                if (!t->nullability.nonnull.data[i].len)
                    return true;
            return false;
        }
        FunctionNullability requested = nonnull_spec(c, args, loc);
        if (!requested.nonnull.len)
            return false;
        ParameterIndices indices = requested.nonnull.data[0];
        if (!valid_nonnull_clause(c, t, indices, loc))
            return false;
        for (size_t i = 0; i < indices.len; i++)
            if (nonnull_parameter(t, indices.data[i]))
                return true;
        return false;
    }
    case ATTR_RETURNS_NONNULL: return t->kind == TY_FUNC && t->nullability.returns_nonnull;
    case ATTR_STDCALL: case ATTR_FALLTHROUGH: case ATTR_NONE: return false;
    }
    return false;
}

Node *has_attribute(Context *c, Type *t, Node *subject, const char *name, Nodes args, Loc loc)
{
    Attribute attribute = attribute_kind(c, name);
    bool result = false;
    if (attribute == ATTR_NONE)
        error(c, loc, "unknown attribute '%s' in __builtin_has_attribute", name);
    else if (args.len > attribute_query_arity(attribute))
        error(c, loc, "too many arguments for attribute '%s'", name);
    else
        result = attribute_query_matches(c, attribute, t, attribute_subject(c, subject), name, args, loc);
    return integer(c, loc, result, type_int(c, 0, true));
}

void warn_lifecycle_attributes(Context *c, Specs specs, Loc loc)
{
    if (specs.constructor_priority)
        warning_at(c, WARNING_ATTRIBUTES, loc, "constructor attribute applies to functions only");
    if (specs.destructor_priority)
        warning_at(c, WARNING_ATTRIBUTES, loc, "destructor attribute applies to functions only");
}

Node *array_count(Context *c, Type *t, Loc loc)
{
    Type *size_type = type_int(c, 4, true);
    if (t->kind != TY_ARRAY || !t->complete) {
        error(c, loc, "_Countof requires a complete array type");
        return integer(c, loc, 0, size_type);
    }
    if (!t->variable_bound)
        return integer(c, loc, t->length, size_type);
    if (!t->runtime_size)
        return convert(c, t->variable_bound, size_type, loc);
    return binary(c, '/', size_expression(c, t, loc), size_expression(c, t->base, loc), loc);
}

Node *type_limit(Context *c, Type *t, bool maximum, Loc loc)
{
    if (!integer_type(t) || !t->complete) {
        error(c, loc, "integer limit query requires a complete integer type");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    if (t->kind == TY_BOOL)
        return integer(c, loc, maximum, unqualified_type(c, t));
    if (integer128(t)) {
        Node *n = integer(c, loc, maximum ? UINT64_MAX : 0, unqualified_type(c, t));
        n->integer_high = maximum ? (t->uns ? UINT64_MAX : INT64_MAX) :
                                    t->uns ? 0 : UINT64_C(1) << 63;
        return n;
    }
    unsigned width = t->size * CHAR_BIT;
    uint64_t sign = UINT64_C(1) << (width - 1);
    uint64_t value = maximum ? (t->uns ? sign + (sign - 1) : sign - 1) : t->uns ? 0 : 0 - sign;
    return integer(c, loc, value, unqualified_type(c, t));
}

Node *choose_expression(Context *c, Node *condition, Node *yes, Node *no, Loc loc)
{
    bool value = false;
    if (!constant_truth(c, condition, &value))
        error(c, loc, "__builtin_choose_expr requires an integer constant expression");
    return value ? yes : no;
}

static bool constant_complex(Context *c, Node *expression, long double *real, long double *imaginary,
                             bool reject_zero_divisor)
{
    Node *a = constant_component(c, expression, false);
    Node *b = constant_component(c, expression, true);
    return a && b && real_constant(c, a, real, reject_zero_divisor) &&
           real_constant(c, b, imaginary, reject_zero_divisor);
}

Node *constant_query(Context *c, Node *expression, Loc loc)
{
    Integer128 integer_value;
    long double real_value, imaginary;
    bool constant = constant_integer(c, expression, &integer_value) ||
                    (expression->type->kind == TY_DECIMAL && decimal_constant(c, expression, &integer_value, true)) ||
                    (expression->type->kind == TY_FLOAT && real_constant(c, expression, &real_value, true)) ||
                    (expression->type->kind == TY_COMPLEX &&
                     constant_complex(c, expression, &real_value, &imaginary, true)) ||
                    (expression->kind == N_SYMBOL && expression->symbol->bytes != NULL);
    return integer(c, loc, constant, type_int(c, 3, false));
}

static uint64_t normalize(Type *t, uint64_t n)
{
    if (t->kind == TY_BOOL)
        return n != 0;
    unsigned bits = t->size * 8;
    if (!integer_type(t) || bits >= 64)
        return n;
    uint64_t sign = UINT64_C(1) << (bits - 1);
    n &= (sign << 1) - 1;
    return t->uns ? n : (n ^ sign) - sign;
}

static bool constant_object_offset(Context *c, Node *n, uint64_t *value)
{
    if (n->kind == N_DEREF)
        return constant_pointer(c, n->left, value);
    if (n->kind == N_MEMBER && !n->right && constant_object_offset(c, n->left, value)) {
        *value += n->integer;
        return true;
    }
    return false;
}

static bool constant_pointer(Context *c, Node *n, uint64_t *value)
{
    if (n->kind == N_CAST)
        return n->left->type->kind == TY_PTR ? constant_pointer(c, n->left, value) :
               constant_int(c, n->left, value) && *value == 0;
    if (n->kind == N_ADDR)
        return constant_object_offset(c, n->left, value);
    uint64_t delta;
    if (n->kind == N_BINARY && (n->op == '+' || n->op == '-') &&
        constant_pointer(c, n->left, value) && constant_int(c, n->right, &delta)) {
        *value += n->op == '+' ? delta : 0 - delta;
        return true;
    }
    return false;
}

bool wide_operation(int op, WideOperation *operation)
{
    switch (op) {
    case '+': *operation = WIDE_ADD; break;
    case '-': *operation = WIDE_SUB; break;
    case '*': *operation = WIDE_MUL; break;
    case '/': *operation = WIDE_DIV; break;
    case '%': *operation = WIDE_REM; break;
    case '&': *operation = WIDE_AND; break;
    case '|': *operation = WIDE_OR; break;
    case '^': *operation = WIDE_XOR; break;
    case SHL: *operation = WIDE_SHL; break;
    case SHR: *operation = WIDE_SHR; break;
    case '<': *operation = WIDE_LT; break;
    case LE: *operation = WIDE_LE; break;
    case '>': *operation = WIDE_GT; break;
    case GE: *operation = WIDE_GE; break;
    case EQ: *operation = WIDE_EQ; break;
    case NE: *operation = WIDE_NE; break;
    default: return false;
    }
    return true;
}

static bool constant_wide_binary(Context *c, Node *n, Integer128 *result)
{
    Integer128 a, b;
    WideOperation op;
    if (!wide_operation(n->op, &op) || !constant_integer(c, n->left, &a) ||
        !constant_integer(c, n->right, &b))
        return false;
    if ((op == WIDE_DIV || op == WIDE_REM) && !wide_nonzero(b))
        return false;
    if ((op == WIDE_SHL || op == WIDE_SHR) && (b.hi || b.lo >= 128))
        return false;
    *result = wide_binary(a, b, op, n->left->type->uns);
    return true;
}

static bool quad_integer_constant(Context *c, Node *n, Integer128 *result)
{
    Integer128 bits;
    if (!quad_constant(c, n->left, &bits, false))
        return false;
    if (n->type->kind == TY_BOOL) {
        bits.hi &= UINT64_MAX >> 1;
        *result = (Integer128){wide_nonzero(bits), 0};
        return true;
    }
    if (!wide_integer_bits(bits, 16, n->type->uns, result))
        return false;
    if (integer128(n->type))
        return true;
    bool negative = !n->type->uns && (result->hi >> 63);
    unsigned width = n->type->size * CHAR_BIT;
    Integer128 magnitude = negative ? wide_negate(*result) : *result;
    return wide_compare(magnitude, wide_limit(width, !n->type->uns, negative), true) <= 0;
}

bool decimal_operation(int op, DecimalOperation *operation)
{
    switch (op) {
    case '+': *operation = DEC_ADD; break;
    case '-': *operation = DEC_SUB; break;
    case '*': *operation = DEC_MUL; break;
    case '/': *operation = DEC_DIV; break;
    case '<': *operation = DEC_LT; break;
    case LE: *operation = DEC_LE; break;
    case '>': *operation = DEC_GT; break;
    case GE: *operation = DEC_GE; break;
    case EQ: *operation = DEC_EQ; break;
    case NE: *operation = DEC_NE; break;
    default: return false;
    }
    return true;
}

bool decimal_constant(Context *c, Node *n, Integer128 *result, bool reject_zero_divisor)
{
    unsigned width = n->type->size;
    Integer128 a, b;
    switch (n->kind) {
    case N_FLOAT:
        *result = (Integer128){n->integer, n->integer_high};
        return true;
    case N_CAST: {
        Type *source = n->left->type;
        if (source->kind == TY_DECIMAL) {
            if (!decimal_constant(c, n->left, &a, reject_zero_divisor))
                return false;
            *result = decimal_convert(width, source->size, a);
        } else if (source->kind == TY_FLOAT) {
            if (!binary_constant_bits(c, n->left, &a, reject_zero_divisor))
                return false;
            *result = decimal_from_binary(width, floating_format(c->target, source), a);
        } else if (source->kind == TY_COMPLEX) {
            Node *real = constant_component(c, n->left, false);
            return real && decimal_constant(c, convert(c, real, n->type, n->loc), result, reject_zero_divisor);
        } else {
            if (!constant_integer(c, n->left, &a))
                return false;
            *result = decimal_from_integer(width, a, source->uns);
        }
        return true;
    }
    case N_UNARY:
        if (n->op != '-' || !decimal_constant(c, n->left, &a, reject_zero_divisor))
            return false;
        *result = decimal_negate(width, a);
        return true;
    case N_BINARY: {
        DecimalOperation op;
        if (!decimal_operation(n->op, &op) || op >= DEC_LT ||
            !decimal_constant(c, n->left, &a, reject_zero_divisor) ||
            !decimal_constant(c, n->right, &b, reject_zero_divisor) ||
            (reject_zero_divisor && op == DEC_DIV && !decimal_nonzero(width, b)))
            return false;
        *result = decimal_binary(width, op, a, b);
        return true;
    }
    case N_COND: {
        bool test;
        return constant_truth(c, n->left, &test) &&
               decimal_constant(c, test ? n->right : n->third, result, reject_zero_divisor);
    }
    default: return false;
    }
}

static bool decimal_integer_constant(Context *c, Node *n, Integer128 *result)
{
    Integer128 bits;
    if (!decimal_constant(c, n->left, &bits, false))
        return false;
    unsigned width = n->left->type->size;
    if (n->type->kind == TY_BOOL) {
        *result = (Integer128){decimal_nonzero(width, bits), 0};
        return true;
    }
    return decimal_to_integer(width, bits, n->type->size * CHAR_BIT, n->type->uns, result);
}

bool constant_integer(Context *c, Node *n, Integer128 *result)
{
    if (!n || !integer_type(n->type))
        return false;
    if (!integer128(n->type)) {
        uint64_t low;
        if (!constant_int(c, n, &low))
            return false;
        *result = (Integer128){low, !n->type->uns && (low >> 63) ? UINT64_MAX : 0};
        return true;
    }
    switch (n->kind) {
    case N_INT:
        *result = (Integer128){n->integer, n->integer_high};
        return true;
    case N_CAST:
        if (n->left->type->kind == TY_DECIMAL)
            return decimal_integer_constant(c, n, result);
        if (n->left->type->kind == TY_PTR) {
            result->hi = 0;
            return constant_pointer(c, n->left, &result->lo);
        }
        if (n->left->type->kind == TY_FLOAT) {
            if (binary128(c->target, n->left->type))
                return quad_integer_constant(c, n, result);
            long double value;
            Type *source = n->left->type;
            unsigned format = floating_format(c->target, source);
            if (!real_constant(c, n->left, &value, false))
                return false;
            uint64_t words[2] = {0};
            if (source->size == 16) {
                if (!extended_float_bits(c, value, words, n->loc))
                    return false;
            } else {
                words[0] = float_bits(value, format);
            }
            return wide_integer_bits((Integer128){words[0], words[1]},
                                     format, n->type->uns, result);
        }
        return constant_integer(c, n->left, result);
    case N_COND: {
        bool test;
        return constant_truth(c, n->left, &test) &&
               constant_integer(c, test ? n->right : n->third, result);
    }
    case N_UNARY:
        if (!constant_integer(c, n->left, result))
            return false;
        *result = n->op == '-' ? wide_negate(*result) : (Integer128){~result->lo, ~result->hi};
        return true;
    case N_BINARY:
        return constant_wide_binary(c, n, result);
    default:
        return false;
    }
}

bool constant_truth(Context *c, Node *n, bool *truth)
{
    n = excess_value(n);
    if (n->type->kind == TY_DECIMAL) {
        Integer128 bits;
        if (!decimal_constant(c, n, &bits, false))
            return false;
        *truth = decimal_nonzero(n->type->size, bits);
        return true;
    }
    if (binary128(c->target, n->type)) {
        Integer128 bits;
        if (!quad_constant(c, n, &bits, false))
            return false;
        bits.hi &= UINT64_MAX >> 1;
        *truth = wide_nonzero(bits);
        return true;
    }
    if (n->type->kind == TY_FLOAT) {
        long double real;
        if (!real_constant(c, n, &real, false))
            return false;
        *truth = real != 0;
        return true;
    }
    if (n->type->kind == TY_COMPLEX) {
        long double a, b;
        if (!constant_complex(c, n, &a, &b, false))
            return false;
        *truth = a != 0 || b != 0;
        return true;
    }
    Integer128 value;
    if (!constant_integer(c, n, &value))
        return false;
    *truth = wide_nonzero(value);
    return true;
}

bool constant_int(Context *c, Node *n, uint64_t *value)
{
    if (!n || !integer_type(n->type))
        return false;
    if (integer128(n->type)) {
        Integer128 v;
        if (!constant_integer(c, n, &v) ||
            v.hi != (!n->type->uns && (v.lo >> 63) ? UINT64_MAX : 0))
            return false;
        *value = v.lo;
        return true;
    }
    uint64_t a = 0, b = 0, result;
    switch (n->kind) {
    case N_INT: result = n->integer; break;
    case N_MEMBER: {
        if (n->left->type->kind != TY_COMPLEX)
            return false;
        Node *part = constant_component(c, n->left, n->integer != 0);
        return part && constant_int(c, part, value);
    }
    case N_CAST:
        if (n->left->type->kind == TY_COMPLEX) {
            if (n->type->kind == TY_BOOL) {
                bool truth;
                if (!constant_truth(c, n->left, &truth))
                    return false;
                result = truth;
            } else {
                Node *part = constant_component(c, n->left, false);
                return part && constant_int(c, convert(c, part, n->type, n->loc), value);
            }
        } else if (integer128(n->left->type)) {
            Integer128 wide;
            if (!constant_integer(c, n->left, &wide))
                return false;
            result = n->type->kind == TY_BOOL ? wide_nonzero(wide) : wide.lo;
        } else if (n->left->type->kind == TY_PTR) {
            if (!constant_pointer(c, n->left, &result))
                return false;
        } else if (n->left->type->kind == TY_DECIMAL) {
            Integer128 decimal_integer;
            if (!decimal_integer_constant(c, n, &decimal_integer))
                return false;
            result = decimal_integer.lo;
        } else if (binary128(c->target, n->left->type)) {
            Integer128 quad_integer;
            if (!quad_integer_constant(c, n, &quad_integer))
                return false;
            result = quad_integer.lo;
        } else if (n->left->type->kind == TY_FLOAT) {
            long double real;
            if (!real_constant(c, n->left, &real, false))
                return false;
            if (n->type->kind == TY_BOOL) {
                result = real != 0;
            } else {
                unsigned bits = n->type->size * CHAR_BIT;
                unsigned exponent = bits - !n->type->uns;
                long double upper = exponent == 64 ? 0x1p64L :
                                    (long double)(UINT64_C(1) << exponent);
                long double lower = n->type->uns ? -1 : -upper - 1;
                if (!(real > lower && real < upper))
                    return false;
                result = n->type->uns ? (uint64_t)real : (uint64_t)(int64_t)real;
            }
        } else if (!constant_int(c, n->left, &result)) {
            return false;
        }
        break;
    case N_COND: {
        bool test;
        if (!constant_truth(c, n->left, &test))
            return false;
        return constant_int(c, test ? n->right : n->third, value);
    }
    case N_UNARY:
        if (n->op == '!') {
            bool test;
            if (!constant_truth(c, n->left, &test))
                return false;
            result = !test;
            break;
        }
        if (!constant_int(c, n->left, &a))
            return false;
        result = n->op == '-' ? 0 - a : ~a;
        break;
    case N_BINARY: {
        if (n->op == AND || n->op == OR) {
            bool test;
            if (!constant_truth(c, n->left, &test))
                return false;
            if (test != (n->op == OR) && !constant_truth(c, n->right, &test))
                return false;
            result = test;
            break;
        }
        if (integer128(n->left->type)) {
            Integer128 wide;
            if (!constant_wide_binary(c, n, &wide))
                return false;
            result = wide.lo;
            break;
        }
        if (n->left->type->kind == TY_DECIMAL) {
            Integer128 raw_a, raw_b;
            DecimalOperation op;
            if (!decimal_operation(n->op, &op) || op < DEC_LT ||
                !decimal_constant(c, n->left, &raw_a, false) || !decimal_constant(c, n->right, &raw_b, false))
                return false;
            result = decimal_relation(n->left->type->size, op, raw_a, raw_b);
            break;
        }
        if (binary128(c->target, n->left->type)) {
            Integer128 raw_a, raw_b;
            if (!quad_constant(c, n->left, &raw_a, false) || !quad_constant(c, n->right, &raw_b, false))
                return false;
            Float128 left, right;
            memcpy(&left, &raw_a, sizeof left);
            memcpy(&right, &raw_b, sizeof right);
            switch (n->op) {
            case EQ: result = left == right; break;
            case NE: result = left != right; break;
            case '<': result = left < right; break;
            case LE: result = left <= right; break;
            case '>': result = left > right; break;
            case GE: result = left >= right; break;
            default: return false;
            }
            break;
        }
        if (!constant_int(c, n->left, &a))
            return false;
        if (!constant_int(c, n->right, &b))
            return false;
        bool uns = n->left->type->uns;
        int64_t sa = (int64_t)a, sb = (int64_t)b;
        switch (n->op) {
        case '+': result = a+b; break; case '-': result = a-b; break;
        case '*': result = a*b; break; case '&': result = a&b; break;
        case '|': result = a|b; break; case '^': result = a^b; break;
        case EQ: result = a==b; break; case NE: result = a!=b; break;
        case '<': result = uns ? a<b : sa<sb; break;
        case '>': result = uns ? a>b : sa>sb; break;
        case LE: result = uns ? a<=b : sa<=sb; break;
        case GE: result = uns ? a>=b : sa>=sb; break;
        case SHL: case SHR:
            if (b >= n->left->type->size * 8)
                return false;
            result = n->op == SHL ? a << b : uns ? a >> b : (uint64_t)(sa >> b);
            break;
        case '/': case '%':
            if (!b || (!uns && sa == INT64_MIN && sb == -1))
                return false;
            result = n->op == '/' ? (uns ? a/b : (uint64_t)(sa/sb))
                                   : (uns ? a%b : (uint64_t)(sa%sb));
            break;
        default: return false;
        }
        break;
    }
    default: return false;
    }
    *value = normalize(n->type, result);
    return true;
}

static Type *implicit_enumerator_type(Context *c, Type *prior, bool negative, Integer128 magnitude)
{
    bool uns = prior && prior->uns;
    unsigned first_rank = prior ? prior->rank : 3;
    for (unsigned rank = first_rank; rank <= 6; rank++) {
        Type *candidate = type_int(c, rank, uns);
        if (wide_compare(magnitude, wide_limit(candidate->size * CHAR_BIT, !uns, negative), true) <= 0)
            return candidate;
    }
    return type_int(c, 6, !negative);
}

void define_enumerator(Context *c, const char *name, Node *expr, const char *deprecated, Loc loc)
{
    EnumDefinition *enumeration = c->enumeration;
    Type *t = enumeration->type;
    Integer128 value = enumeration->next;
    bool negative = enumeration->next_negative;
    if (expr) {
        if (!constant_integer(c, expr, &value))
            error(c, loc, "enumerator requires an integer constant expression");
        negative = !expr->type->uns && (value.hi >> 63);
    } else if (enumeration->next_overflow) {
        error(c, loc, "enumerator value exceeds the largest supported integer");
    }
    Type *constant_type = t;
    Integer128 magnitude = negative ? wide_negate(value) : value;
    if (t->fixed_enum) {
        unsigned width = t->kind == TY_BOOL ? 1u : t->size * CHAR_BIT;
        if (wide_compare(magnitude, wide_limit(width, !t->uns, negative), true) > 0)
            error(c, loc, "enumerator value is outside the range of the enum underlying type");
    } else {
        Integer128 *maximum = negative ? &enumeration->negative_magnitude : &enumeration->largest;
        if (wide_compare(magnitude, *maximum, true) > 0)
            *maximum = magnitude;
        bool wide = magnitude.hi || magnitude.lo > (uint64_t)INT32_MAX + negative;
        if (wide && c->dialect.standard < C23)
            extension(c, loc, "enumerator outside int range");
        enumeration->has_wide_value |= wide;
        if (!wide)
            constant_type = type_int(c, 3, false);
        else if (expr && expr->type->rank >= 3)
            constant_type = unqualified_type(c, expr->type);
        else
            constant_type = implicit_enumerator_type(c, enumeration->last_constant_type, negative,
                                                     magnitude);
    }
    if (find_in(c->scope, name))
        error(c, loc, "redeclaration of '%s'", name);
    Symbol *s = symbol(c, name, constant_type, loc);
    s->enum_constant = true;
    s->deprecated = deprecated;
    s->enum_value = value;
    PUSH(c, c->scope->names, s);
    PUSH(c, enumeration->constants, s);
    enumeration->last_constant_type = constant_type;
    enumeration->next = wide_binary(value, (Integer128){1, 0}, WIDE_ADD, true);
    enumeration->next_negative = negative && wide_nonzero(enumeration->next);
    enumeration->next_overflow = !negative && !wide_nonzero(enumeration->next);
}

static Symbol *global(Context *c, const char *name, Type *t, unsigned flags,
                      unsigned alignment, Loc loc)
{
    Scope *outer = c->scope;
    while (outer->parent)
        outer = outer->parent;
    Symbol *s = find_in(outer, name);
    if (!s) {
        s = symbol(c, name, t, loc);
        s->alignment = alignment;
        s->internal = flags & S_STATIC;
        s->thread_local_storage = (flags & S_THREAD) != 0;
        PUSH(c, outer->names, s);
        PUSH(c, c->globals, s);
    } else if (s->is_typedef || s->enum_constant || !compatible(s->type, t, true) ||
               s->thread_local_storage != ((flags & S_THREAD) != 0)) {
        error(c, loc, "conflicting declaration of '%s'", name);
    } else {
        warn_transparent_redeclaration(c, t, s->type, loc);
        unsigned prior = symbol_alignment(s);
        unsigned current = alignment ? alignment : t->align;
        bool explicit_alignment = alignment || s->alignment ||
                                  t->typedef_alignment || s->type->typedef_alignment;
        if (t->complete || s->type->complete)
            s->type = composite_type(c, t->complete ? t : s->type, t->complete ? s->type : t, false);
        if (explicit_alignment) {
            unsigned strongest = prior > current ? prior : current;
            s->alignment = strongest == s->type->align ? 0 : strongest;
        }
    }
    if (c->scope != outer && !find_in(c->scope, name))
        PUSH(c, c->scope->names, s);
    return s;
}

static Symbol *frame_local(Context *c, Symbol *function, const char *name, Type *t, Loc loc)
{
    Symbol *s = symbol(c, name, t, loc);
    s->local = true;
    s->owner = function;
    PUSH(c, function->locals, s);
    return s;
}

static Symbol *nested_function(Context *c, const char *name, Type *t, Loc loc)
{
    Symbol *s = find_in(c->scope, name);
    if (s) {
        if (s->owner != c->function || s->type->kind != TY_FUNC || !same_type(s->type, t))
            error(c, loc, "conflicting declaration of nested function '%s'", name);
        else {
            warn_transparent_redeclaration(c, t, s->type, loc);
            if (transparent_function_conversion(t, s->type))
                s->type = composite_type(c, t, s->type, false);
            else {
                if (t->warn_unused_result)
                    s->type = warn_unused_result_type(c, s->type, loc);
                Sentinels sentinels = combine_sentinels(c, s->type->sentinels, t->sentinels);
                s->type = sentinel_type(c, s->type, sentinels, loc);
                s->type = nullability_type(c, s->type, t->nullability, loc);
                s->type = access_type(c, s->type, t->accesses);
            }
        }
        return s;
    }
    s = symbol(c, name, t, loc);
    s->assembly = format(c, ".Lmcc_nested_%u", c->unique++);
    s->internal = true;
    s->owner = c->function;
    PUSH(c, c->scope->names, s);
    PUSH(c, c->globals, s);
    return s;
}

static void declare_inline(Context *c, Symbol *s, unsigned flags, bool file_scope, Loc loc)
{
    unsigned specifiers = flags & (S_INLINE | S_GNU_INLINE);
    if (file_scope && (!(specifiers & S_INLINE) || (flags & S_EXTERN)))
        s->iso_external = true;
    if (specifiers == S_GNU_INLINE) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "gnu_inline attribute requires inline on '%s'", s->name);
        return;
    }
    if (specifiers) {
        if (s->inline_declarations && s->inline_declarations != specifiers)
            error(c, loc, "conflicting gnu_inline attributes on '%s'", s->name);
        s->inline_declarations = specifiers;
    }
}

static void mark_weak(Context *c, Symbol *s, Loc loc)
{
    if (s->is_typedef)
        warning_at(c, WARNING_ATTRIBUTES, loc, "weak attribute ignored on typedef '%s'", s->name);
    else if (s->local || s->internal || global_register(s))
        error(c, loc, "weak declaration of '%s' requires external linkage", s->name);
    else if (s->inline_declarations & S_INLINE)
        warning(c, loc, "inline function '%s' declared weak", s->name);
    else
        s->weak = true;
}

static void declare_alias(Context *c, Symbol *s, const char *target, Loc loc)
{
    if (s->body || s->defined)
        error(c, loc, "'%s' is both defined and declared as an alias", s->name);
    else if (s->alias && !equal(s->alias, target))
        error(c, loc, "conflicting alias targets for '%s'", s->name);
    else {
        s->alias = target;
        s->tentative = false;
    }
}

static void declare_visibility(Context *c, Symbol *s, Visibility attribute, Loc loc)
{
    if (s->internal || s->local || s->is_typedef || global_register(s)) {
        if (attribute)
            warning_at(c, WARNING_ATTRIBUTES, loc, "visibility attribute ignored on '%s'", s->name);
    } else if (!s->visibility) {
        s->visibility = attribute ? attribute : (Visibility)loc.visibility;
        s->explicit_visibility = attribute != VIS_UNSPECIFIED;
    } else if (attribute && attribute != s->visibility) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "conflicting visibility on redeclaration of '%s'", s->name);
    } else if (attribute) {
        s->explicit_visibility = true;
    }
}

static void declare_lifecycle(Context *c, Symbol *s, Specs specs, Loc loc)
{
    if (s->is_typedef || s->type->kind != TY_FUNC) {
        warn_lifecycle_attributes(c, specs, loc);
        return;
    }
    /* A redeclaration keeps the established priority. After a definition,
       a newly added attribute uses the default priority. */
    unsigned late_priority = s->body ? DEFAULT_LIFECYCLE_PRIORITY + 1 : 0;
    if (specs.constructor_priority && !s->constructor_priority) {
        s->constructor_priority = late_priority ? late_priority : specs.constructor_priority;
        s->attribute_markers |= specs.attribute_markers & DEFAULT_CONSTRUCTOR_ATTRIBUTE;
    }
    if (specs.destructor_priority && !s->destructor_priority) {
        s->destructor_priority = late_priority ? late_priority : specs.destructor_priority;
        s->attribute_markers |= specs.attribute_markers & DEFAULT_DESTRUCTOR_ATTRIBUTE;
    }
}

static void declare_query_attributes(Symbol *s, Specs specs)
{
    if (s->is_typedef)
        return;
    s->alignment_attributes |= specs.alignment_attributes;
    s->attribute_markers |= specs.attribute_markers & MARK_NORETURN;
    if (!s->local)
        s->attribute_markers |= specs.attribute_markers & MARK_USED;
    if (s->type->kind == TY_FUNC)
        s->attribute_markers |= specs.attribute_markers & (MARK_NOINLINE | MARK_NOPLT);
}

static void declare_patchable_entry(Context *c, Symbol *s, Specs specs, const Decl *d)
{
    const PatchableEntries *entries = declarator_patchable_entries(c, specs.patchable_entries, d);
    if (s->is_typedef || s->type->kind == TY_FUNC)
        entries = combine_patchable_entries(c, specs.inherited_patchable_entries, entries);
    s->patchable_entries = combine_patchable_entries(c, s->patchable_entries, entries);
}

static void declare_call_diagnostics(Context *c, Symbol *s, CallDiagnostics diagnostics, Loc loc)
{
    if (s->is_typedef || s->type->kind != TY_FUNC) {
        warn_call_diagnostic_attributes(c, diagnostics, loc);
        return;
    }
    for (size_t i = 0; i < diagnostics.len; i++)
        PUSH(c, s->call_diagnostics, diagnostics.data[i]);
}

static void declare_cleanup(Context *c, Symbol *s, const char *name, unsigned flags, Loc loc)
{
    if (!name)
        return;
    if (!s->local) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "cleanup attribute applies to automatic variables only");
        return;
    }
    if (flags & S_REGISTER) {
        error(c, loc, "cleanup requires the address of register variable '%s'", s->name);
        return;
    }
    Symbol *function = find(c, name);
    if (!function || function->is_typedef || function->type->kind != TY_FUNC) {
        error(c, loc, "cleanup argument '%s' does not name a function", name);
        return;
    }
    Node *variable = node(c, N_SYMBOL, loc, NULL, NULL);
    variable->type = s->type;
    variable->symbol = s;
    Nodes args = {0};
    PUSH(c, args, address(c, variable, s->type, loc));
    s->cleanup = call(c, reference(c, name, loc), args, loc);
}

static void declare_section(Context *c, Symbol *s, const char *name, Loc loc)
{
    if (!name)
        return;
    if (s->is_typedef || global_register(s))
        warning_at(c, WARNING_ATTRIBUTES, loc, "section attribute ignored on '%s'", s->name);
    else if (s->local)
        error(c, loc, "section attribute requires static or thread storage");
    else if (s->section && !equal(s->section, name))
        warning(c, loc, "conflicting section on redeclaration of '%s'", s->name);
    else
        s->section = name;
}

static void declare_global_register(Context *c, Symbol *s, const char *name, Loc loc)
{
    if (!c->extension_depth)
        warning_at(c, WARNING_PEDANTIC, loc, "file-scope register declaration of '%s' is a GNU extension", s->name);
    if (!name) {
        error(c, loc, "global register variable '%s' requires an assembler register name", s->name);
        return;
    }
    if (s->defined || s->explicit_asm_name || (s->type->qualifiers & S_ATOMIC)) {
        error(c, loc, "invalid declaration of global register variable '%s'", s->name);
        return;
    }
    unsigned binding = asm_register_binding(c, name, s->type, true, loc);
    if (!binding)
        return;
    if (s->register_binding) {
        if (s->register_binding != binding)
            error(c, loc, "conflicting registers for '%s'", s->name);
        return;
    }
    uint64_t bit = UINT64_C(1) << (binding - 1);
    if (c->global_registers & bit)
        warning(c, loc, "register '%s' is used for multiple global register variables", name);
    bool call_clobbered = asm_call_clobbered(c, binding);
    if (call_clobbered)
        warning(c, loc, "call-clobbered register used for global register variable '%s'", s->name);
    if (call_clobbered || s->type->size > 8)
        c->global_register_spills |= bit;
    c->global_registers |= bit;
    s->register_binding = binding;
    s->tentative = false;
}

static Node *type_bound_statements(Context *c, Nodes evaluations, Loc loc)
{
    if (!evaluations.len)
        return NULL;
    Node *result = node(c, N_SEQUENCE, loc, NULL, NULL);
    for (size_t i = 0; i < evaluations.len; i++)
        PUSH(c, result->list, node(c, N_EXPR, loc, evaluations.data[i], NULL));
    return result;
}

Node *type_declaration(Context *c, Specs specs, Loc loc)
{
    warn_call_diagnostic_attributes(c, specs.call_diagnostics, loc);
    warn_invalid_noplt(c, specs, NULL, loc);
    if (specs.noreturn_specifier)
        error(c, loc, "_Noreturn is not permitted in an empty declaration");
    Type *t = declaration_type(c, specs, NULL, loc);
    if (specs.designated_init)
        designated_init_type(c, t, false, loc);
    Nodes evaluations = {0};
    if (t->record_variably_modified)
        take_record_bounds(c, t, &evaluations);
    return type_bound_statements(c, evaluations, loc);
}

Node *declare(Context *c, Specs specs, Decl *d, Node *init, Loc loc)
{
    Decl *name = decl_name(d);
    const char *id = name->name;
    if (specs.flags & S_PACKED) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "packed attribute ignored on declaration of '%s'", id);
        specs.flags &= ~(unsigned)S_PACKED;
    }
    Type *t = declaration_type(c, specs, d, loc);
    t = apply_noreturn(c, t, d, &specs, false, loc);
    t = allocation_size_type(c, t, specs.allocation, loc);
    if (specs.designated_init)
        designated_init_type(c, t, false, loc);
    bool is_typedef = specs.flags & S_TYPEDEF;
    if (specs.transparent_union) {
        if (is_typedef)
            t = transparent_union_type(c, t, false, loc);
        else
            warning_at(c, WARNING_ATTRIBUTES, loc, "transparent_union applies to union types only");
    }
    bool file_scope = c->scope->parent == NULL;
    if ((specs.flags & S_WEAKREF) && (!file_scope || is_typedef)) {
        warning_at(c, WARNING_ATTRIBUTES, loc, "weakref attribute ignored on '%s'", id);
        specs.flags &= ~(unsigned)S_WEAKREF;
        specs.alias = NULL;
    }
    bool register_global = file_scope && (specs.flags & S_REGISTER);
    bool local = !file_scope && t->kind != TY_FUNC && !(specs.flags & S_EXTERN);
    bool tls = (specs.flags & S_THREAD) != 0;
    Symbol *s = NULL;
    if (specs.has_alignas && (is_typedef || t->kind == TY_FUNC || (specs.flags & S_REGISTER)))
        error(c, loc, "_Alignas cannot be used with a typedef, function, or register declaration");
    unsigned alignment = requested_alignment(c, specs, t, loc);
    if (is_typedef && specs.gnu_alignment) {
        t = aligned_type(c, t, specs.gnu_alignment);
        alignment = 0;
    }
    if ((specs.flags & S_INLINE) && (is_typedef || t->kind != TY_FUNC))
        error(c, loc, "inline applies to a function declaration");
    if ((specs.flags & S_GNU_INLINE) && (is_typedef || t->kind != TY_FUNC))
        warning_at(c, WARNING_ATTRIBUTES, loc, "gnu_inline attribute applies to functions only");
    if (t->kind == TY_VOID && !is_typedef)
        error(c, loc, "object '%s' cannot have void type", id);
    if (tls && (is_typedef || t->kind == TY_FUNC ||
        (!file_scope && !(specs.flags & (S_STATIC | S_EXTERN))) ||
        (specs.flags & (S_AUTO | S_REGISTER))))
        error(c, loc, "invalid thread-local declaration of '%s'", id);
    bool modified = variably_modified(t);
    if (modified && (!local || !c->function))
        error(c, loc, "variably modified declaration requires block or prototype scope and no linkage");
    if (t->variable_size && !is_typedef && (specs.flags & (S_STATIC | S_THREAD)))
        error(c, loc, "variable-sized object requires automatic storage");
    Nodes evaluations = {0};
    if (specs.type)
        take_record_bounds(c, specs.type, &evaluations);
    if (modified && local && c->function)
        t = save_array_sizes(c, t, &evaluations, loc);
    if (is_typedef || local) {
        Symbol *prior = find_in(c->scope, id);
        if (prior && !(is_typedef && prior->is_typedef && !modified && compatible(prior->type, t, true)))
            error(c, loc, "redeclaration of '%s'", id);
        s = symbol(c, id, t, loc);
        s->alignment = alignment;
        s->is_typedef = is_typedef;
        if (is_typedef && prior && prior->is_typedef)
            s->deprecated = prior->deprecated;
        PUSH(c, c->scope->names, s);
        if (!is_typedef) {
            if (specs.flags & S_STATIC) {
                s->assembly = format(c, ".Lstatic%u", c->unique++);
                s->internal = s->defined = true;
                s->thread_local_storage = tls;
                s->owner = c->function;
                PUSH(c, c->globals, s);
            } else {
                s->local = true;
                s->owner = c->function;
                PUSH(c, c->function->locals, s);
            }
        }
    } else if (!file_scope && t->kind == TY_FUNC && (specs.flags & S_AUTO)) {
        extension(c, loc, "nested function declaration");
        s = nested_function(c, id, t, loc);
        if (alignment > s->alignment)
            s->alignment = alignment;
    } else {
        s = global(c, id, t, specs.flags, alignment, loc);
        if (t->kind == TY_FUNC)
            declare_inline(c, s, specs.flags, file_scope, loc);
        else if (file_scope && !(specs.flags & S_EXTERN) && !register_global && !global_register(s) &&
                 !specs.alias && !s->alias)
            s->tentative = true;
    }
    if (register_global) {
        declare_global_register(c, s, name->asm_name, loc);
    } else if (name->asm_name) {
        const char *assembly = name->asm_name;
        if (s->local && (specs.flags & S_REGISTER)) {
            s->register_binding = asm_register_binding(c, assembly, t, false, loc);
        } else if (global_register(s)) {
            error(c, loc, "global register variable '%s' cannot have an assembler symbol name", id);
        } else if (s->local || s->is_typedef) {
            error(c, loc, "asm names require a symbol with static storage or linkage");
        } else if (!*assembly || strspn(assembly, "_.$abcdefghijklmnopqrstuvwxyz"
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != strlen(assembly) ||
                   isdigit((unsigned char)*assembly)) {
            error(c, loc, "invalid assembler symbol name for '%s'", id);
        } else if (s->explicit_asm_name && !equal(s->assembly, assembly)) {
            error(c, loc, "conflicting asm names for '%s'", id);
        } else {
            s->assembly = assembly;
            s->explicit_asm_name = true;
        }
    }
    if (specs.deprecated)
        s->deprecated = specs.deprecated;
    declare_exec_sentinel(c, s, loc);
    if (s->type->kind == TY_FUNC && !s->is_typedef)
        specs.attribute_markers |= declarator_noplt(specs, d);
    else
        warn_invalid_noplt(c, specs, d, loc);
    declare_query_attributes(s, specs);
    declare_patchable_entry(c, s, specs, d);
    declare_call_diagnostics(c, s, specs.call_diagnostics, loc);
    declare_visibility(c, s, specs.visibility, loc);
    declare_lifecycle(c, s, specs, loc);
    declare_cleanup(c, s, cleanup_name(specs, d), specs.flags, loc);
    declare_section(c, s, specs.section, loc);
    if (specs.flags & S_WEAK)
        mark_weak(c, s, loc);
    s->weak_reference |= (specs.flags & S_WEAKREF) != 0;
    if (specs.alias) {
        if (is_typedef)
            warning_at(c, WARNING_ATTRIBUTES, loc, "alias attribute ignored on typedef '%s'", id);
        else if (!file_scope || global_register(s))
            error(c, loc, "alias '%s' requires a file-scope symbol", id);
        else if (t->kind != TY_FUNC && !(specs.flags & (S_EXTERN | S_STATIC)))
            error(c, loc, "'%s' is both defined and declared as an alias", id);
        else
            declare_alias(c, s, specs.alias, loc);
    }
    Node *n = node(c, N_DECL, loc, NULL, NULL);
    n->symbol = s;
    if (modified && local && c->function)
        n->flags |= DECL_VARIABLY_MODIFIED;
    n->list = evaluations;
    if (init)
        set_initializer(c, n, init);
    return n;
}

static bool string_initializer(Type *t, Node *n)
{
    return t->kind == TY_ARRAY && n->kind == N_SYMBOL && n->symbol->bytes &&
           t->base->kind == TY_INT && t->base->size == n->type->base->size;
}

static Type *subobject(Type *t, size_t index)
{
    return indexed_elements(t) ? t->base : t->members.data[index].type;
}

static size_t elements(Type *t)
{
    if (indexed_elements(t))
        return t->complete ? t->length : SIZE_MAX;
    if (t->kind == TY_UNION)
        return t->members.len ? 1 : 0;
    return t->members.len;
}

static Node *initializer(Context *, Type **, Node *);

static Node *init_slot(Context *c, Node *list, size_t index, Loc loc)
{
    size_t n = 0, end = list->list.len;
    while (n < end) {
        size_t middle = n + (end - n) / 2;
        if (list->list.data[middle]->integer < index)
            n = middle + 1;
        else
            end = middle;
    }
    if (n < list->list.len && list->list.data[n]->integer == index)
        return list->list.data[n];
    Node *item = node(c, N_DESIGNATE, loc, NULL, NULL);
    item->integer = index;
    PUSH(c, list->list, item);
    memmove(list->list.data + n + 1, list->list.data + n,
            (list->list.len - n - 1) * sizeof *list->list.data);
    list->list.data[n] = item;
    return item;
}

static bool choose_member(Context *c, Type *t, const char *name, size_t *index, bool *anonymous)
{
    for (size_t i = 0; i < t->members.len; i++) {
        Member member = t->members.data[i];
        Member nested;
        if ((member.name && equal(member.name, name)) ||
            (!member.name && !member.bit_field && find_member(c, member.type, name, &nested, NULL))) {
            *index = i;
            *anonymous = !member.name;
            return true;
        }
    }
    return false;
}

typedef struct { Type *type; Node *init; size_t index; } InitPosition;
typedef VECTOR(InitPosition) InitPositions;

static bool elides_initializer_braces(Type *t, Node *init)
{
    return (aggregate(t) || t->kind == TY_VECTOR) && init->kind != N_INIT &&
           !string_initializer(t, init) && !same_type(t, init->type);
}

static size_t next_initializable(Type *t, size_t index, size_t limit)
{
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        while (index < limit && t->members.data[index].bit_field && !t->members.data[index].name)
            index++;
    return index;
}

static Node *range_initializer(Context *c, Type *t, Node *init)
{
    if (init->kind == N_RANGE_VALUE)
        return init;
    if (init->kind != N_INIT) {
        Node *once = node(c, N_RANGE_VALUE, init->loc, init, NULL);
        once->type = t;
        return once;
    }

    /* Share expressions and give each destination its own slots for overrides. */
    Node *copy = node(c, N_INIT, init->loc, NULL, NULL);
    copy->type = init->type;
    for (size_t i = 0; i < init->list.len; i++) {
        Node *item = init->list.data[i];
        Type *child = subobject(t, item->integer);
        if (child->kind == TY_ARRAY && !child->complete)
            child = item->left->type;
        Node *value = range_initializer(c, child, item->left);
        Node *slot = node(c, N_DESIGNATE, item->loc, value, NULL);
        slot->integer = item->integer;
        PUSH(c, copy->list, slot);
    }
    return copy;
}

static size_t designate(Context *, Type *, Node *, Node *, Node *, InitPositions *, Node **);

static Node *nested_initializer(Context *c, Type *t, Node *previous, Loc loc)
{
    if (previous && previous->kind == N_INIT)
        return previous;

    Node *init = node(c, N_INIT, loc, NULL, NULL);
    init->type = t;
    if (!previous)
        return init;
    if (previous->kind == N_RANGE_VALUE)
        previous = previous->left;
    if (string_initializer(t, previous)) {
        Symbol *literal = previous->symbol;
        size_t width = t->base->size;
        size_t count = literal->byte_count / width;
        if (t->complete && count > t->length)
            count = t->length;
        for (size_t i = 0; i < count; i++) {
            uint64_t value = 0;
            for (size_t b = 0; b < width; b++)
                value |= (uint64_t)(unsigned char)literal->bytes[i * width + b] << (8 * b);
            if (value)
                init_slot(c, init, i, loc)->left = integer(c, loc, value, t->base);
        }
    } else if (t->kind != TY_STRUCT && t->kind != TY_UNION) {
        error(c, loc, "nested designator cannot override this initializer");
    }
    return init;
}

static void designate_element(Context *c, Type *t, Node *list, size_t index, Node *next,
                              Node *value, Loc loc, InitPositions *continuation, Node **shared)
{
    if (t->kind == TY_UNION && list->list.len && list->list.data[0]->integer != index)
        list->list.len = 0;
    Node *slot = init_slot(c, list, index, loc);
    Type *child = subobject(t, index);
    if (next) {
        slot->left = nested_initializer(c, child, slot->left, loc);
        if (c->failed)
            return;
        size_t following = designate(c, child, slot->left, next, value, continuation, shared) + 1;
        if (continuation)
            PUSH(c, *continuation, ((InitPosition){child, slot->left, following}));
    } else if (elides_initializer_braces(child, value)) {
        size_t limit = child->kind == TY_UNION ? child->members.len : elements(child);
        size_t first = next_initializable(child, 0, limit);
        if (first == limit) {
            error(c, loc, "cannot initialize an empty aggregate without braces");
            return;
        }
        slot->left = nested_initializer(c, child, slot->left, loc);
        if (c->failed)
            return;
        designate_element(c, child, slot->left, first, NULL, value, loc, continuation, shared);
        if (continuation)
            PUSH(c, *continuation, ((InitPosition){child, slot->left, first + 1}));
    } else if (shared) {
        if (!*shared) {
            Node *init = initializer(c, &child, value);
            if (c->failed)
                return;
            *shared = range_initializer(c, child, init);
        }
        slot->left = range_initializer(c, child, *shared);
    } else {
        slot->left = initializer(c, &child, value);
    }
}

static size_t designate(Context *c, Type *t, Node *list, Node *selector, Node *value,
                        InitPositions *continuation, Node **shared)
{
    size_t index;
    Node *next = selector->right;
    if (selector->name) {
        bool anonymous;
        if ((t->kind != TY_STRUCT && t->kind != TY_UNION) ||
            !choose_member(c, t, selector->name, &index, &anonymous)) {
            error(c, selector->loc, "unknown member designator '%s'", selector->name);
            return 0;
        }
        if (anonymous)
            next = selector;
    } else {
        uint64_t n;
        if (t->kind != TY_ARRAY || !constant_int(c, selector->left, &n) ||
            (int64_t)n < 0 || n >= elements(t) || n >= UINT_MAX) {
            error(c, selector->loc, "invalid array designator");
            return 0;
        }
        index = (size_t)n;
    }
    if (selector->third) {
        uint64_t high = 0;
        if (!constant_int(c, selector->third, &high) || high < index ||
            high >= elements(t) || high >= UINT_MAX || high - index > 65535) {
            error(c, selector->loc, "unsupported or invalid initializer range");
            return index;
        }
        Node *once = NULL;
        for (size_t i = index; i <= high && !c->failed; i++)
            designate_element(c, t, list, i, next, value, selector->loc,
                              i == high ? continuation : NULL, shared ? shared : &once);
        return (size_t)high;
    }
    designate_element(c, t, list, index, next, value, selector->loc, continuation, shared);
    return index;
}

static Node *aggregate_init(Context *, Type **, Nodes, size_t *, bool, Loc);

static void warn_positional_initializer(Context *c, Type *t, Loc loc)
{
    if (t->designated_init)
        warning_at(c, WARNING_DESIGNATED_INIT, loc,
                   "positional initialization of a structure field with the designated_init attribute");
}

static bool positional_initializer(Context *c, Type *t, Node *result, size_t *index,
                                   size_t limit, Nodes entries, size_t *cursor, bool warn)
{
    *index = next_initializable(t, *index, limit);
    if (*index >= limit)
        return false;
    Node *item = entries.data[*cursor];
    Type *child = subobject(t, *index);
    Node *value;
    if (elides_initializer_braces(child, item)) {
        size_t previous = *cursor;
        value = aggregate_init(c, &child, entries, cursor, false, item->loc);
        if (*cursor == previous) {
            error(c, item->loc, "cannot initialize an empty aggregate without braces");
            return false;
        }
    } else {
        if (warn)
            warn_positional_initializer(c, t, item->loc);
        (*cursor)++;
        value = initializer(c, &child, item);
    }
    init_slot(c, result, (*index)++, item->loc)->left = value;
    return true;
}

static size_t continue_initializer(Context *c, InitPositions positions, Nodes entries, size_t *cursor)
{
    size_t diagnosed = SIZE_MAX;
    for (size_t i = 0; i < positions.len; i++) {
        InitPosition position = positions.data[i];
        size_t limit = position.type->kind == TY_UNION ? 0 : elements(position.type);
        while (*cursor < entries.len && !c->failed && entries.data[*cursor]->kind != N_DESIGNATE) {
            bool warn = diagnosed != *cursor;
            if (positional_initializer(c, position.type, position.init, &position.index,
                                       limit, entries, cursor, warn))
                continue;
            /* GNU checks the current designator level before returning to its parent. */
            if (warn && !c->failed) {
                warn_positional_initializer(c, position.type, entries.data[*cursor]->loc);
                diagnosed = *cursor;
            }
            break;
        }
    }
    return diagnosed;
}

static Type *complete_array_initializer(Context *c, Type *t, Node *init)
{
    size_t length = init->list.len ? init->list.data[init->list.len - 1]->integer + 1 : 0;
    return array(c, t->base, length, true, init->loc);
}

static Node *aggregate_init(Context *c, Type **destination, Nodes entries, size_t *cursor,
                            bool explicit_braces, Loc loc)
{
    Type *t = *destination;
    Node *result = node(c, N_INIT, loc, NULL, NULL);
    size_t index = 0, limit = elements(t), diagnosed = SIZE_MAX;
    if (t->kind == TY_UNION && limit) {
        index = next_initializable(t, 0, t->members.len);
        limit = index < t->members.len ? index + 1 : index;
    }
    while (*cursor < entries.len && !c->failed) {
        Node *item = entries.data[*cursor];
        if (item->kind == N_DESIGNATE) {
            if (!explicit_braces)
                break;
            InitPositions continuation = {0};
            index = designate(c, t, result, item->left, item->right, &continuation, NULL) + 1;
            (*cursor)++;
            diagnosed = continue_initializer(c, continuation, entries, cursor);
            continue;
        }
        if (!positional_initializer(c, t, result, &index, limit, entries, cursor, diagnosed != *cursor)) {
            if (explicit_braces && !c->failed)
                error(c, item->loc, "too many initializer elements");
            break;
        }
    }
    if (t->kind == TY_ARRAY && !t->complete) {
        *destination = t = complete_array_initializer(c, t, result);
    } else if (t->kind == TY_STRUCT && result->list.len && !c->failed) {
        Node *last = result->list.data[result->list.len - 1]->left;
        if (last->kind == N_INIT && last->type->kind == TY_ARRAY && !last->type->complete)
            last->type = complete_array_initializer(c, last->type, last);
    }
    result->type = t;
    return result;
}

static Node *initializer(Context *c, Type **destination, Node *init)
{
    Type *t = *destination;
    if (t->kind == TY_ARRAY && init->kind == N_INIT && init->list.len == 1 &&
        string_initializer(t, init->list.data[0]))
        init = init->list.data[0];
    if (string_initializer(t, init)) {
        if (!t->complete)
            *destination = array(c, t->base, init->type->length, true, init->loc);
        else if (t->length + 1 < init->type->length)
            error(c, init->loc, "string is too long for array initializer");
        return init;
    }
    if (init->kind == N_COMPOUND && !init->symbol->local && (aggregate(t) || t->kind == TY_VECTOR) &&
        same_type(t, init->type) && c->dialect.gnu) {
        return init->symbol->init;
    }
    if (init->kind != N_INIT) {
        if (t->kind == TY_ARRAY)
            error(c, init->loc, "array initializer requires braces or a string");
        return implicitly(c, init, t, init->loc);
    }
    if (!aggregate(t) && t->kind != TY_VECTOR) {
        if (init->list.len != 1 || init->list.data[0]->kind == N_DESIGNATE) {
            if (!init->list.len && (c->dialect.gnu || c->dialect.standard >= C23 ||
                c->extension_depth) && scalar(t))
                return convert(c, integer(c, init->loc, 0, type_int(c, 3, false)), t, init->loc);
            error(c, init->loc, "scalar initializer requires one element");
            return init;
        }
        return initializer(c, destination, init->list.data[0]);
    }
    size_t start = 0;
    return aggregate_init(c, destination, init->list, &start, true, init->loc);
}

void set_initializer(Context *c, Node *declaration, Node *init)
{
    Symbol *s = declaration->symbol;
    if (global_register(s)) {
        error(c, init->loc, "global register variable '%s' cannot be initialized", s->name);
        return;
    }
    if (s->is_typedef || s->alias || s->type->kind == TY_FUNC || (!s->local && s->init)) {
        error(c, init->loc, "invalid or duplicate initializer for '%s'", s->name);
        return;
    }
    if (s->type->variable_size) {
        if (init->kind != N_INIT || init->list.len) {
            error(c, init->loc, "variable-sized object requires an empty initializer");
            return;
        }
        if (c->dialect.standard < C23)
            extension(c, init->loc, "empty initializer for a variable-sized object");
        s->init = init;
        s->defined = true;
        return;
    }
    s->init = initializer(c, &s->type, init);
    if (s->type->kind == TY_STRUCT && s->type->members.len && s->init->kind == N_INIT &&
        s->init->list.len) {
        size_t last = s->type->members.len - 1;
        Type *tail = s->type->members.data[last].type;
        if (tail->kind == TY_ARRAY && !tail->complete &&
            s->init->list.data[s->init->list.len - 1]->integer == last) {
            extension(c, init->loc, "flexible array member initializer");
            if (s->local)
                error(c, init->loc, "flexible array initializer requires static storage");
        }
    }
    if (!s->type->complete)
        error(c, init->loc, "cannot determine initializer size");
    s->defined = true;
}

Node *compound_literal(Context *c, Type *t, Nodes entries, Loc loc)
{
    if (t->variable_size) {
        Node *n = node(c, N_COMPOUND, loc, NULL, NULL);
        if (!c->function || entries.len) {
            error(c, loc, "variable-sized compound literal requires block scope and an empty initializer");
        } else {
            n->type = save_array_sizes(c, t, &n->list, loc);
            c->function->has_stack_alloca = c->scope->aligned_alloca = true;
        }
        if (!n->type)
            n->type = t;
        n->symbol = symbol(c, "compound literal", n->type, loc);
        n->symbol->owner = c->function;
        n->symbol->local = c->function != NULL;
        return n;
    }
    Node *raw = node(c, N_INIT, loc, NULL, NULL);
    raw->list = entries;
    Node *init = initializer(c, &t, raw);
    return initialized_literal(c, t, init, loc);
}

static Node *initialized_literal(Context *c, Type *t, Node *init, Loc loc)
{
    Symbol *s = symbol(c, format(c, ".Lliteral%u", c->unique++), t, loc);
    s->internal = s->defined = true;
    s->local = c->function != NULL;
    s->owner = c->function;
    s->init = init;
    if (s->local)
        PUSH(c, c->function->locals, s);
    else
        PUSH(c, c->globals, s);
    Node *n = node(c, N_COMPOUND, loc, NULL, NULL);
    n->symbol = s;
    n->type = t;
    return n;
}

Node *generic_selection(Context *c, Node *control, Nodes entries, Loc loc)
{
    Type type = *decay(c, control)->type;
    type.qualifiers = 0;
    Node *selected = NULL, *fallback = NULL;
    for (size_t i = 0; i < entries.len; i++) {
        Node *entry = entries.data[i];
        if (!entry->type) {
            if (fallback)
                error(c, entry->loc, "duplicate generic default association");
            fallback = entry->left;
            continue;
        }
        for (size_t j = 0; j < i; j++)
            if (entries.data[j]->type && compatible(entry->type, entries.data[j]->type, true))
                error(c, entry->loc, "duplicate compatible generic association");
        if (compatible(&type, entry->type, true))
            selected = entry->left;
    }
    if (selected || fallback)
        return selected ? selected : fallback;
    error(c, loc, "no matching generic association");
    return integer(c, loc, 0, type_int(c, 3, false));
}

Node *statement_expression(Context *c, Node *block, Loc loc)
{
    extension(c, loc, "statement expression");
    if (!c->function)
        error(c, loc, "statement expressions require function scope");
    Node *last = block->list.len ? block->list.data[block->list.len - 1] : NULL;
    Node *result = last && last->kind == N_EXPR && last->left ? decay(c, last->left) : NULL;
    if (result)
        block->list.len--;
    Node *n = node(c, N_STATEMENT_EXPR, loc, block, result);
    n->type = result ? result->type : type(c, TY_VOID, 0, 1);
    if (c->function)
        c->function->has_variable_temporary |= n->type->variable_size;
    return n;
}

static void discard_function_objects(Context *c, Symbol *function)
{
    for (size_t i = 0; i < c->globals.len; i++) {
        Symbol *s = c->globals.data[i];
        if (s->owner == function) {
            s->body = NULL;
            s->defined = false;
        }
    }
    function->locals.len = 0;
    function->params.len = 0;
    function->parameter_bounds.len = 0;
    function->labels.len = 0;
    function->trampolines = NULL;
    function->has_nonlocal_labels = false;
    function->has_builtin_setjmp = false;
    function->has_explicit_stack = false;
    function->has_stack_alloca = false;
    function->has_variable_temporary = false;
    function->has_switch = false;
}

Node *begin_function(Context *c, Specs specs, Decl *d, Loc loc)
{
    if (specs.transparent_union)
        warning_at(c, WARNING_ATTRIBUTES, loc, "transparent_union applies to union types only");
    Type *t = declaration_type(c, specs, d, loc);
    t = apply_noreturn(c, t, d, &specs, false, loc);
    if (specs.designated_init)
        designated_init_type(c, t, false, loc);
    t = allocation_size_type(c, t, specs.allocation, loc);
    const char *name = decl_name(d)->name;
    if (specs.has_alignas)
        error(c, loc, "_Alignas cannot be used with a function definition");
    if (t->kind != TY_FUNC || (specs.flags & (S_TYPEDEF | S_THREAD))) {
        error(c, loc, "invalid function definition");
        t = type(c, TY_FUNC, 0, 1);
        t->base = type_int(c, 3, false);
    }
    Symbol *outer = c->function;
    Nodes evaluations = {0};
    if (outer)
        t->base = save_array_sizes(c, t->base, &evaluations, loc);
    Node *bounds_statement = type_bound_statements(c, evaluations, loc);
    Symbol *s;
    InlineDefinition definition = INLINE_ORDINARY;
    if (outer) {
        extension(c, loc, "nested function");
        if (specs.flags & (S_STATIC | S_EXTERN | S_REGISTER))
            error(c, loc, "invalid storage class for nested function '%s'", name);
        s = nested_function(c, name, t, loc);
        Type *frame_pointer = pointer(c, type(c, TY_VOID, 0, 1));
        s->static_chain = frame_local(c, s, "static chain", frame_pointer, loc);
        if (!outer->trampolines)
            outer->trampolines = frame_local(c, outer, "trampoline list", frame_pointer, loc);
    } else {
        s = global(c, name, t, specs.flags, specs.gnu_alignment, loc);
        declare_inline(c, s, specs.flags, true, loc);
        if (!s->internal && (specs.flags & S_INLINE)) {
            if (c->dialect.gnu_inline || (specs.flags & S_GNU_INLINE)) {
                if (specs.flags & S_EXTERN)
                    definition = INLINE_GNU_EXTERN;
            } else {
                definition = INLINE_ISO;
            }
        }
    }
    if (s->defined) {
        if (s->inline_definition == INLINE_GNU_EXTERN && definition == INLINE_ORDINARY)
            discard_function_objects(c, s);
        else
            error(c, loc, "redefinition of function '%s'", name);
    }
    s->inline_definition = definition;
    if (specs.gnu_alignment > s->alignment)
        s->alignment = specs.gnu_alignment;
    Type *declared = s->type;
    if (!transparent_function_conversion(t, declared))
        declared = t;
    if (s->type->warn_unused_result)
        declared = warn_unused_result_type(c, declared, loc);
    declared = sentinel_type(c, declared, s->type->sentinels, loc);
    declared = nullability_type(c, declared, s->type->nullability, loc);
    declared = access_type(c, declared, s->type->accesses);
    s->type = allocation_size_type(c, declared, s->type->allocation, loc);
    declare_exec_sentinel(c, s, loc);
    if (specs.deprecated)
        s->deprecated = specs.deprecated;
    specs.attribute_markers |= declarator_noplt(specs, d);
    declare_query_attributes(s, specs);
    declare_patchable_entry(c, s, specs, d);
    declare_call_diagnostics(c, s, specs.call_diagnostics, loc);
    declare_visibility(c, s, specs.visibility, loc);
    declare_lifecycle(c, s, specs, loc);
    declare_cleanup(c, s, cleanup_name(specs, d), specs.flags, loc);
    declare_section(c, s, specs.section, loc);
    if (s->alias || specs.alias)
        error(c, loc, "'%s' is both defined and declared as an alias", name);
    if (specs.flags & S_WEAK)
        mark_weak(c, s, loc);
    s->weak_reference |= (specs.flags & S_WEAKREF) != 0;
    s->defined = true;
    c->function = s;
    if (t->parameter_scope) {
        c->scope = t->parameter_scope;
        c->scope->parameters = false;
    } else {
        enter_scope(c);
    }
    Symbols bounds = c->scope->bound_locals;
    for (size_t i = 0; i < bounds.len; i++) {
        bounds.data[i]->owner = s;
        PUSH(c, s->locals, bounds.data[i]);
    }
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        Symbol *param = m.parameter;
        if (!param) {
            error(c, loc, "missing parameter name");
            continue;
        }
        param->local = true;
        param->owner = s;
        PUSH(c, s->locals, param);
        PUSH(c, s->params, param);
    }
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        Type *shape = m.parameter_array ? m.parameter_array : m.type;
        for (Type *part = shape; part->kind == TY_PTR || part->kind == TY_ARRAY; part = part->base)
            if (part->array_star)
                error(c, loc, "[*] is not permitted in a function definition");
        if (!c->failed) {
            Type *evaluated = save_array_sizes(c, shape, &s->parameter_bounds, loc);
            if (evaluated == shape || !m.parameter)
                continue;
            if (m.parameter_array) {
                Type *adjusted = pointer(c, evaluated->base);
                adjusted->qualifiers = m.type->qualifiers;
                m.parameter->type = adjusted;
            } else {
                m.parameter->type = evaluated;
            }
        }
    }
    c->function_body = true;
    return bounds_statement;
}

void end_function(Context *c, Node *body)
{
    for (size_t i = 0; i < c->function->labels.len; i++) {
        Symbol *s = c->function->labels.data[i];
        if (s->label_referenced && !s->defined)
            error(c, s->loc, "undefined label '%s'", s->name);
    }
    if (!c->failed) {
        resolve_object_sizes(c);
        Nodes calls = c->function->fortified_calls;
        for (size_t i = 0; i < calls.len; i++) {
            Node *call = calls.data[i];
            check_fortified_size(c, find_fortified_builtin(call->name), call->list, call->loc);
        }
    }
    c->function->fortified_calls.len = 0;
    c->function->body = body;
    c->function = c->function->owner;
}

static Symbol *find_assembly_symbol(Context *c, const char *name)
{
    for (size_t i = 0; i < c->globals.len; i++) {
        Symbol *s = c->globals.data[i];
        if (s->assembly && equal(s->assembly, name))
            return s;
    }
    return NULL;
}

static void apply_weak_pragmas(Context *c)
{
    for (size_t i = 0; i < c->weak_pragmas.len; i++) {
        WeakPragma pragma = c->weak_pragmas.data[i];
        Symbol *s = find_in(c->scope, pragma.name);
        if (!s && pragma.target) {
            s = symbol(c, pragma.name, NULL, pragma.loc);
            s->visibility = VIS_DEFAULT;
            PUSH(c, c->scope->names, s);
            PUSH(c, c->globals, s);
        }
        if (!s)
            continue;
        mark_weak(c, s, pragma.loc);
        if (pragma.target && s->weak)
            declare_alias(c, s, pragma.target, pragma.loc);
    }
}

static void resolve_symbol_aliases(Context *c)
{
    for (size_t i = 0; i < c->globals.len && !c->failed; i++) {
        Symbol *s = c->globals.data[i];
        if (s->weak_reference && !s->alias) {
            warning_at(c, WARNING_ATTRIBUTES, s->loc, "weakref attribute requires an alias target");
            s->weak_reference = false;
        }
        if (!s->alias)
            continue;
        if (s->weak_reference && !s->internal) {
            error(c, s->loc, "weakref '%s' requires static linkage", s->name);
            return;
        }
        Symbol *target = find_assembly_symbol(c, s->alias);
        size_t steps = 0;
        while (target && target->alias && ++steps < c->globals.len)
            target = find_assembly_symbol(c, target->alias);
        if (s->weak_reference) {
            if (target && target->alias)
                error(c, s->loc, "weakref '%s' is part of an alias cycle", s->name);
            continue;
        }
        if (!target || target->alias || (target->type->kind == TY_FUNC ? !target->body :
                                        (!target->defined && !target->tentative))) {
            error(c, s->loc, "alias '%s' requires a defined target '%s'", s->name, s->alias);
            return;
        }
        if (!s->type) {
            s->type = target->type;
            s->thread_local_storage = target->thread_local_storage;
        } else if ((s->type->kind == TY_FUNC) != (target->type->kind == TY_FUNC) ||
                   s->thread_local_storage != target->thread_local_storage) {
            error(c, s->loc, "alias '%s' and '%s' have different symbol kinds", s->name, s->alias);
        }
    }
}

void finish_translation_unit(Context *c)
{
    for (size_t i = 0; i < c->globals.len; i++) {
        Symbol *s = c->globals.data[i];
        if (s->owner && !s->owner->body) {
            s->body = NULL;
            s->defined = false;
        } else if (s->inline_definition == INLINE_GNU_EXTERN ||
                   (s->inline_definition == INLINE_ISO && !s->iso_external)) {
            s->body = NULL;
        }
    }
    apply_weak_pragmas(c);
    resolve_symbol_aliases(c);
    if (c->failed || !c->default_visibility)
        return;
    for (size_t i = 0; i < c->globals.len; i++) {
        Symbol *s = c->globals.data[i];
        if (s->type && !s->internal && !s->visibility && !s->weak_reference &&
            (s->alias || (s->type->kind == TY_FUNC ? s->body != NULL : s->defined || s->tentative)))
            s->visibility = c->default_visibility;
    }
}

Node *check_return(Context *c, Node *n, Loc loc)
{
    if (c->function->attribute_markers & MARK_NORETURN)
        warning(c, loc, "function declared 'noreturn' has a 'return' statement");
    Type *t = c->function->type->base;
    if (!n || t->kind == TY_VOID) {
        if (!!n != (t->kind != TY_VOID))
            error(c, loc, "return value does not match function return type");
        return n;
    }
    return implicitly(c, n, t, loc);
}
