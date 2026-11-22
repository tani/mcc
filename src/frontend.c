#include "compiler.h"
#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; Type *type; } Tag;
struct Scope { Scope *parent; Symbols names; VECTOR(Tag) tags; };

static Type *type(Context *c, TypeKind kind, unsigned size, unsigned align)
{
    Type *t = alloc(c, sizeof *t);
    *t = (Type){.kind = kind, .size = size, .align = align, .complete = true};
    return t;
}

Type *type_int(Context *c, unsigned rank, bool uns)
{
    const Target *t = c->target;
    unsigned sizes[] = {1, t->char_size, t->short_size, t->int_size, t->long_size, t->long_long_size};
    Type *result = type(c, rank ? TY_INT : TY_BOOL, sizes[rank], sizes[rank]);
    result->rank = rank;
    result->uns = uns || !rank;
    return result;
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
    if (base->variable_bound)
        error(c, loc, "multidimensional variable-length arrays are not supported");
    if (length && base->size > UINT_MAX / length) {
        error(c, loc, "array size is too large");
        length = 0;
    }
    Type *t = type(c, TY_ARRAY, (unsigned)(base->size * length), base->align);
    t->base = base;
    t->length = length;
    t->complete = complete;
    return t;
}

static bool integer_type(Type *t)
{
    return t && (t->kind == TY_INT || t->kind == TY_BOOL);
}

bool scalar(Type *t)
{
    return integer_type(t) || (t && (t->kind == TY_PTR || t->kind == TY_FLOAT));
}

static bool promoted_parameter(Type *t)
{
    return (integer_type(t) && t->rank < 3) || (t->kind == TY_FLOAT && t->size == 4);
}

static bool compatible(Type *a, Type *b, bool check_qualifiers)
{
    if (a == b)
        return true;
    if (!a || !b || a->kind != b->kind ||
        (check_qualifiers && a->qualifiers != b->qualifiers))
        return false;
    if (a->enumeration && b->enumeration)
        return a->base == b->base;
    switch (a->kind) {
    case TY_PTR: return compatible(a->base, b->base, true);
    case TY_ARRAY: return (!a->complete || !b->complete || a->length == b->length) &&
                          compatible(a->base, b->base, true);
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
        for (size_t i = 0; i < a->members.len; i++)
            if (!compatible(a->members.data[i].type, b->members.data[i].type, false))
                return false;
        return true;
    case TY_STRUCT: case TY_UNION: return a->base && a->base == b->base;
    default: return a->size == b->size && a->uns == b->uns && a->rank == b->rank &&
                    a->plain_char == b->plain_char;
    }
}

bool same_type(Type *a, Type *b)
{
    return compatible(a, b, false);
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
    if (t->kind == TY_STRUCT || t->kind == TY_UNION || t->enumeration) {
        Type *definition = t->base;
        for (Type *version = definition; version; version = version->qualified_next)
            if (version->qualifiers == qualifiers)
                return version;
        Type *result = alloc(c, sizeof *result);
        *result = *definition;
        result->qualifiers = qualifiers;
        definition->qualified_next = result;
        return result;
    }
    Type *result = alloc(c, sizeof *result);
    *result = *t;
    result->qualifiers = qualifiers;
    return result;
}

static void update_qualified_types(Type *definition)
{
    for (Type *t = definition->qualified_next; t; t = t->qualified_next) {
        Type *next = t->qualified_next;
        unsigned qualifiers = t->qualifiers;
        *t = *definition;
        t->qualifiers = qualifiers;
        t->qualified_next = next;
    }
}

static void check_restrict(Context *c, Type *t, unsigned qualifiers, Loc loc)
{
    if ((qualifiers & S_RESTRICT) && (t->kind != TY_PTR || t->base->kind == TY_FUNC))
        error(c, loc, "restrict requires a pointer to an object or incomplete type");
}

static Type *composite_type(Context *c, Type *a, Type *b)
{
    if (a == b)
        return a;
    switch (a->kind) {
    case TY_PTR: {
        Type *base = composite_type(c, a->base, b->base);
        if (a->base == base)
            return a;
        Type *result = pointer(c, base);
        result->qualifiers = a->qualifiers;
        return result;
    }
    case TY_ARRAY: {
        Type *shape = a->complete ? a : b;
        Type *base = composite_type(c, a->base, b->base);
        if (shape->base == base)
            return shape;
        Type *result = alloc(c, sizeof *result);
        *result = *shape;
        result->base = base;
        return result;
    }
    case TY_FUNC: {
        Type *signature = a->prototype ? a : b;
        Type *result = alloc(c, sizeof *result);
        *result = *signature;
        result->base = composite_type(c, a->base, b->base);
        if (a->prototype && b->prototype) {
            result->members = (Members){0};
            for (size_t i = 0; i < a->members.len; i++) {
                Member parameter = a->members.data[i];
                parameter.type = composite_type(c, parameter.type, b->members.data[i].type);
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
    unsigned f = s.flags & (S_CONST - 1);
    Type *t = s.type;
    if (((s.flags & (S_TYPEDEF | S_EXTERN | S_STATIC | S_AUTO | S_REGISTER)) &
         ((s.flags & (S_TYPEDEF | S_EXTERN | S_STATIC | S_AUTO | S_REGISTER)) - 1)) != 0)
        error(c, loc, "conflicting storage classes");
    if (!t) {
        bool uns = f & S_UNSIGNED;
        bool bad = (f & S_SIGNED) && uns;
        if (f & S_VOID) {
            bad |= f != S_VOID;
            t = type(c, TY_VOID, 0, 1);
        } else if (f & S_BOOL) {
            bad |= f != S_BOOL;
            t = type_int(c, 0, true);
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
    if (s.flags & S_PACKED)
        pack_record(c, t, loc);
    unsigned qualifiers = s.flags & S_QUALIFIERS;
    check_restrict(c, t, qualifiers, loc);
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

Type *apply_decl(Context *c, Type *base, Decl *d)
{
    if (!d)
        return base;
    Type *t = base;
    switch (d->kind) {
    case D_NAME: return base;
    case D_POINTER:
        if (base->variable_bound)
            error(c, d->loc, "declarators for pointers to variable-length arrays are not supported");
        t = pointer(c, base);
        t->qualifiers = d->qualifiers;
        check_restrict(c, t, t->qualifiers, d->loc);
        break;
    case D_ARRAY: {
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
        t = array(c, base, (size_t)count, d->bound != NULL, d->loc);
        if (variable)
            t->variable_bound = d->bound;
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
        if (t->members.len == 1 && t->members.data[0].type->kind == TY_VOID &&
            !t->members.data[0].name && !t->variadic)
            t->members.len = 0;
        for (size_t i = 0; i < t->members.len; i++) {
            Member *p = &t->members.data[i];
            if (p->type->kind == TY_ARRAY) {
                Type *array_type = p->type;
                p->type = pointer(c, array_type->base);
                p->type->qualifiers = array_type->qualifiers;
            }
            else if (p->type->kind == TY_FUNC)
                p->type = pointer(c, p->type);
            else if (p->type->kind == TY_VOID)
                error(c, d->loc, "parameter cannot have void type");
        }
        break;
    }
    return apply_decl(c, t, d->inner);
}

void enter_scope(Context *c)
{
    Scope *scope = alloc(c, sizeof *scope);
    scope->parent = c->scope;
    c->scope = scope;
}

void leave_scope(Context *c)
{
    c->scope = c->scope->parent;
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

Type *enum_type(Context *c, const char *name, bool definition, Loc loc)
{
    Type *t = NULL;
    if (name) {
        for (Scope *s = c->scope; s && !t; s = definition ? NULL : s->parent)
            for (size_t i = 0; i < s->tags.len; i++)
                if (equal(s->tags.data[i].name, name)) {
                    t = s->tags.data[i].type;
                    break;
                }
    }
    if (t && (!t->enumeration || (definition && t->complete))) {
        error(c, loc, "conflicting declaration of enum '%s'", name);
        return type_int(c, 3, false);
    }
    if (!t && !definition)
        extension(c, loc, "forward enum declaration");
    if (!t) {
        t = type_int(c, 3, true);
        t->enumeration = true;
        t->base = t;
        t->complete = false;
        if (name) {
            Tag item = {name, t};
            PUSH(c, c->scope->tags, item);
        }
    }
    if (definition) {
        t->complete = true;
        t->uns = !c->enum_negative;
        update_qualified_types(t);
    }
    return t;
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
    size_t cursor = 0, alignment = 1;
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
        if ((!m->bit_field || m->name) && m->type->align > alignment)
            alignment = m->type->align;
        if (m->bit_field) {
            size_t boundary = m->type->align * 8;
            if (!m->width) {
                if (!is_union)
                    cursor = align_to(cursor, boundary);
                continue;
            }
            if (!is_union) {
                size_t start = cursor / boundary * boundary;
                if (cursor - start + m->width > m->type->size * 8) {
                    cursor = align_to(cursor, boundary);
                    start = cursor;
                }
                m->offset = start / 8;
                m->bit = (unsigned)(cursor - start);
                cursor += m->width;
            } else if (m->width > cursor) {
                cursor = m->width;
            }
        } else if (is_union) {
            if (m->type->size * (size_t)8 > cursor)
                cursor = m->type->size * (size_t)8;
        } else {
            m->offset = align_to((cursor + 7) / 8, m->type->align);
            cursor = (m->offset + m->type->size) * 8;
        }
    }
    size_t size = (cursor + 7) / 8;
    if (align_to(size, alignment) > UINT_MAX)
        error(c, loc, "aggregate size is too large");
    t->size = (unsigned)align_to(size, alignment);
    t->align = (unsigned)alignment;
    t->members = *members;
    t->complete = true;
    update_qualified_types(t);
    return t;
}

void pack_record(Context *c, Type *t, Loc loc)
{
    if ((t->kind != TY_STRUCT && t->kind != TY_UNION) || !t->complete) {
        error(c, loc, "packed attribute requires a complete aggregate definition");
        return;
    }
    t = t->base;
    size_t size = 0;
    for (size_t i = 0; i < t->members.len; i++) {
        Member *m = &t->members.data[i];
        if (m->bit_field) {
            error(c, loc, "packed bit-fields are not supported");
            return;
        }
        if (t->kind == TY_STRUCT) {
            m->offset = size;
            size += m->type->size;
        } else if (m->type->size > size) {
            size = m->type->size;
        }
    }
    t->align = 1;
    t->size = (unsigned)size;
    update_qualified_types(t);
}

void add_member(Context *c, Members *list, Specs s, Decl *d, Loc loc)
{
    Decl *name = decl_name(d);
    Member m = {.name = name ? name->name : NULL, .type = apply_decl(c, spec_type(c, s, loc), d)};
    if (m.type->variable_bound)
        error(c, loc, "variable bounds in members and parameter declarators are not supported");
    if (d && d->bit_width) {
        uint64_t width = 0;
        m.bit_field = true;
        if (!integer_type(m.type) || !constant_int(c, d->bit_width, &width) ||
            width > (m.type->kind == TY_BOOL ? 1u : m.type->size * 8) || (!width && m.name))
            error(c, loc, "invalid bit-field width or type");
        else
            m.width = (unsigned)width;
    }
    PUSH(c, *list, m);
}

void setup_types(Context *c)
{
    enter_scope(c);
    Members fields = {0};
    Type *p = pointer(c, type(c, TY_VOID, 0, 1));
    Type *i = type_int(c, 3, false);
    Member field[] = {
        {.name="__stack",.type=p}, {.name="__gr_top",.type=p}, {.name="__vr_top",.type=p},
        {.name="__gr_offs",.type=i}, {.name="__vr_offs",.type=i}
    };
    for (size_t n = 0; n < sizeof field / sizeof *field; n++)
        PUSH(c, fields, field[n]);
    Type *va = record_type(c, false, NULL, &fields, (Loc){0});
    c->va_list_type = va;
    Symbol *s = symbol(c, "__builtin_va_list", va, (Loc){0});
    s->is_typedef = true;
    PUSH(c, c->scope->names, s);
    Type *expect = type(c, TY_FUNC, 0, 1);
    expect->base = type_int(c, 4, false);
    expect->prototype = true;
    Member argument = {.type = expect->base};
    PUSH(c, expect->members, argument);
    PUSH(c, expect->members, argument);
    s = symbol(c, "__builtin_expect", expect, (Loc){0});
    PUSH(c, c->scope->names, s);
    static const char *const varargs[] = {"__builtin_va_start", "__builtin_va_end", "__builtin_va_copy"};
    Type *builtin = type(c, TY_FUNC, 0, 1);
    builtin->base = type(c, TY_VOID, 0, 1);
    for (size_t j = 0; j < sizeof varargs / sizeof *varargs; j++) {
        s = symbol(c, varargs[j], builtin, (Loc){0});
        PUSH(c, c->scope->names, s);
    }
}

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
        if (strchr("LuU", *s)) {
            rank = *s == 'u' ? 2 : 3;
            uns = true;
            s++;
        }
        if (*s != '\'' || s[1] == '\'')
            error(c, token.loc, "invalid or unsupported character constant");
        else if (uns) {
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
    bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (strchr(s, '.') || strpbrk(s, hex ? "pP" : "eE")) {
        char *end;
        long double real = strtold(s, &end);
        unsigned width = 8;
        if (*end && (end[0] == 'f' || end[0] == 'F')) {
            width = 4;
            real = strtof(s, NULL);
            end++;
        }
        else if (*end && (end[0] == 'l' || end[0] == 'L')) {
            width = c->target->long_double_size;
            end++;
        } else {
            real = strtod(s, NULL);
        }
        if (end == s || *end)
            error(c, token.loc, "invalid floating constant '%s'", s);
        Node *n = node(c, N_FLOAT, token.loc, NULL, NULL);
        n->type = type(c, TY_FLOAT, width, width);
        n->real = real;
        return n;
    }
    char *end;
    errno = 0;
    unsigned base = 0;
    const char *start = s;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
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

Node *strings(Context *c, Node *previous, Token token)
{
    const char *s = token.text;
    unsigned width = 1;
    if (!strncmp(s, "u8", 2))
        s += 2;
    else if (strchr("LuU", *s)) {
        width = *s == 'u' ? 2 : 4;
        s++;
    }
    if (*s++ != '"') {
        error(c, token.loc, "wide string literals are not supported");
        return integer(c, token.loc, 0, type_int(c, 3, false));
    }
    if (previous && previous->kind != N_SYMBOL)
        return previous;
    if (previous && previous->type->base->size != width) {
        error(c, token.loc, "mixed-width string concatenation is not supported");
        return previous;
    }
    size_t prefix = previous ? previous->symbol->byte_count - width : 0;
    unsigned char *bytes = alloc(c, prefix + (strlen(s) + 1) * width);
    if (prefix)
        memcpy(bytes, previous->symbol->bytes, prefix);
    size_t length = prefix;
    while (*s && *s != '"') {
        bool universal = s[0] == '\\' && (s[1] == 'u' || s[1] == 'U');
        unsigned cp = width == 1 || *s == '\\' ? escape(c, &s, token.loc) : utf8(c, &s, token.loc);
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
    length += width;
    Symbol *sym;
    if (previous) {
        sym = previous->symbol;
    } else {
        sym = symbol(c, format(c, ".Lstr%u", c->unique++), NULL, token.loc);
        sym->defined = sym->internal = sym->read_only = true;
        PUSH(c, c->globals, sym);
    }
    Type *element = type_int(c, width == 1 ? 1 : width == 2 ? 2 : 3,
                             width != 1 || c->target->char_unsigned);
    element->plain_char = width == 1;
    sym->type = array(c, element, length / width, true, token.loc);
    sym->bytes = bytes;
    sym->byte_count = length;
    Node *n = previous ? previous : node(c, N_SYMBOL, token.loc, NULL, NULL);
    n->type = sym->type;
    n->symbol = sym;
    return n;
}

Node *reference(Context *c, const char *name, Loc loc)
{
    Symbol *s = find(c, name);
    if (!s || s->is_typedef) {
        error(c, loc, "use of undeclared identifier '%s'", name);
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    if (s->enum_constant)
        return integer(c, loc, s->enum_value, s->type);
    Node *n = node(c, N_SYMBOL, loc, NULL, NULL);
    n->symbol = s;
    n->type = s->type;
    return n;
}

static bool lvalue(Node *n)
{
    if (n->kind == N_MEMBER)
        return lvalue(n->left);
    return n->kind == N_SYMBOL || n->kind == N_DEREF || n->kind == N_COMPOUND;
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
    Node *result = node(c, N_ADDR, loc, n, NULL);
    result->type = pointer(c, t);
    return result;
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
    if (compatible(n->type, to, true) || (aggregate(to) && same_type(n->type, to)))
        return n;
    if (to->kind != TY_VOID && (!scalar(n->type) || !scalar(to) ||
        (n->type->kind == TY_FLOAT && to->kind == TY_PTR) ||
        (n->type->kind == TY_PTR && to->kind == TY_FLOAT))) {
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

Node *cast_expression(Context *c, Node *n, Type *to, Loc loc)
{
    n = decay(c, n);
    if (n->type->kind == TY_PTR && to->kind == TY_PTR &&
        (n->type->base->kind == TY_FUNC) != (to->base->kind == TY_FUNC))
        extension(c, loc, "cast between function and object or void pointers");
    to = qualified(c, to, 0);
    Node *value = convert(c, n, to, loc);
    if (value == n && lvalue(n)) {
        value = node(c, N_CAST, loc, n, NULL);
        value->type = to;
    }
    return value;
}

static Node *promote(Context *c, Node *n)
{
    return integer_type(n->type) && (n->type->rank < 3 ||
                                     (n->width && n->width < c->target->int_size * 8)) ?
        convert(c, n, type_int(c, 3, false), n->loc) : n;
}

static Type *common_type(Context *c, Type *a, Type *b)
{
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

static Node *implicitly(Context *c, Node *n, Type *to, Loc loc)
{
    n = decay(c, n);
    if (to->kind == TY_PTR && !null_pointer_constant(c, n)) {
        if (n->type->kind == TY_PTR) {
            Type *dest = to->base, *source = n->type->base;
            if (void_function_pair(dest, source))
                extension(c, loc, "conversion between function and void pointers");
            else if (!same_type(dest, source) && !void_object_pair(dest, source))
                error(c, loc, "incompatible pointer conversion");
            else if ((source->qualifiers & ~dest->qualifiers) && (!c->gnu || c->pedantic))
                error(c, loc, "pointer conversion discards qualifiers");
        } else {
            error(c, loc, "expected a compatible pointer or null pointer constant");
        }
    } else if (integer_type(to) && to->kind != TY_BOOL && n->type->kind == TY_PTR) {
        error(c, loc, "implicit conversion from pointer to integer");
    }
    return convert(c, n, to, loc);
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
    if (same_type(x, y)) {
        if (!allow_null_void && !object_type(x))
            extension(c, loc, "ordered comparison of void or function pointers");
        base = composite_type(c, x, y);
    } else if (void_object_pair(x, y) || (allow_null_void && void_function_pair(x, y))) {
        if (!allow_null_void || void_function_pair(x, y))
            extension(c, loc, "comparison or conditional with incompatible pointer types");
        base = x->kind == TY_VOID ? x : y;
    } else {
        error(c, loc, "incompatible pointer operands");
        return NULL;
    }
    return pointer(c, qualified(c, base, x->qualifiers | y->qualifiers));
}

Node *unary(Context *c, int op, Node *n, Loc loc)
{
    if (op == '&') {
        if (n->width)
            error(c, loc, "cannot take address of bit-field");
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
    n = promote(c, n);
    if (!scalar(n->type) || (op == '~' && !integer_type(n->type)) ||
        (op != '!' && n->type->kind == TY_PTR))
        error(c, loc, "invalid unary operand");
    if (op == '+')
        return n;
    Node *result = node(c, N_UNARY, loc, n, NULL);
    result->op = op;
    result->type = op == '!' ? type_int(c, 3, false) : n->type;
    return result;
}

static Node *operation(Context *c, int op, Node *a, Node *b, Type *result, Loc loc)
{
    Node *n = node(c, N_BINARY, loc, a, b);
    n->op = op;
    n->type = result;
    return n;
}

static Node *vla_size(Context *c, Type *t, Loc loc)
{
    Type *integer_type = type_int(c, 4, true);
    if (t->runtime_size) {
        Node *n = node(c, N_SYMBOL, loc, NULL, NULL);
        n->symbol = t->runtime_size;
        n->type = integer_type;
        return n;
    }
    return operation(c, '*', convert(c, t->variable_bound, integer_type, loc),
                     integer(c, loc, t->base->size, integer_type), integer_type, loc);
}

Node *binary(Context *c, int op, Node *a, Node *b, Loc loc)
{
    a = promote(c, decay(c,a));
    b = promote(c, decay(c,b));
    Type *i = type_int(c, 3, false), *wide = type_int(c, 4, false);
    bool comparison = op == EQ || op == NE || op == '<' || op == '>' || op == LE || op == GE;
    if ((!scalar(a->type) || !scalar(b->type))) {
        error(c, loc, "invalid binary operands");
        return integer(c, loc, 0, i);
    }
    if (op == AND || op == OR)
        return operation(c, op, a, b, i, loc);
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
        Node *scale = base && base->variable_bound ? convert(c, vla_size(c, base, loc), wide, loc) :
                      integer(c, loc, base && base->size ? base->size : 1, wide);
        if (a->type->kind == TY_PTR && b->type->kind == TY_PTR && op == '-' &&
            same_type(base, b->type->base) && b->type->base->complete) {
            Node *difference = operation(c, '-', convert(c, a, wide, loc),
                                        convert(c, b, wide, loc), wide, loc);
            return operation(c, '/', difference, scale, wide, loc);
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
    Type *t = op == SHL || op == SHR ? a->type : common_type(c, a->type, b->type);
    return operation(c, op, convert(c, a, t, loc), convert(c, b, t, loc),
                     comparison ? i : t, loc);
}

Node *assign(Context *c, int op, Node *lhs, Node *rhs, bool postfix, Loc loc)
{
    if (!lvalue(lhs) || const_subobject(lhs->type) || lhs->type->kind == TY_ARRAY ||
        lhs->type->kind == TY_FUNC || lhs->type->kind == TY_VOID)
        error(c, loc, "assignment requires a modifiable lvalue");
    if (op != '=')
        rhs = binary(c, op, lhs, rhs, loc);
    Node *n = node(c, op == '=' ? N_ASSIGN : N_UPDATE, loc, lhs, implicitly(c, rhs, lhs->type, loc));
    n->type = qualified(c, lhs->type, 0);
    n->postfix = postfix;
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
    } else if (scalar(a->type) && scalar(no->type)) {
        t = qualified(c, common_type(c, promote(c, a)->type, promote(c, no)->type), 0);
    } else if (same_type(a->type, no->type)) {
        t = qualified(c, composite_type(c, a->type, no->type), 0);
    } else {
        error(c, loc, "incompatible conditional operands");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    Node *n = node(c, N_COND, loc, test, convert(c, a, t, loc));
    n->third = convert(c, no, t, loc);
    n->type = t;
    n->postfix = !yes;
    return n;
}

Node *call(Context *c, Node *callee, Nodes args, Loc loc)
{
    const char *name = callee->kind == N_SYMBOL ? callee->symbol->name : "";
    if (equal(name, "__builtin_expect")) {
        uint64_t expected;
        if (args.len != 2 || !constant_int(c, args.data[1], &expected)) {
            error(c, loc, "__builtin_expect requires a value and an integer constant");
            return integer(c, loc, 0, type_int(c, 4, false));
        }
        return implicitly(c, args.data[0], type_int(c, 4, false), loc);
    }
    bool start = equal(name, "__builtin_va_start"), end = equal(name, "__builtin_va_end");
    bool copy_list = equal(name, "__builtin_va_copy");
    if (start || end || copy_list) {
        size_t count = end ? 1 : 2;
        Type *void_type = type(c, TY_VOID, 0, 1);
        if (args.len != count || !lvalue(args.data[0]) ||
            !same_type(args.data[0]->type, c->va_list_type) ||
            (copy_list && !same_type(args.data[1]->type, c->va_list_type)) ||
            (start && (!c->function || !c->function->type->variadic))) {
            error(c, loc, "invalid operands to '%s'", name);
            return integer(c, loc, 0, type_int(c, 3, false));
        }
        if (start && (!c->function->params.len || args.data[1]->kind != N_SYMBOL ||
            args.data[1]->symbol != c->function->params.data[c->function->params.len - 1]))
            error(c, loc, "va_start must name the last fixed parameter");
        if (copy_list)
            return convert(c, assign(c, '=', args.data[0], args.data[1], false, loc), void_type, loc);
        Node *result = node(c, start ? N_VA_START : N_VA_END, loc, args.data[0], NULL);
        result->type = void_type;
        return result;
    }
    callee = decay(c, callee);
    Type *fn = callee->type->kind == TY_PTR ? callee->type->base : NULL;
    if (!fn || fn->kind != TY_FUNC) {
        error(c, loc, "called expression is not a function");
        return integer(c, loc, 0, type_int(c, 3, false));
    }
    if (fn->prototype && (args.len < fn->members.len || (!fn->variadic && args.len != fn->members.len)))
        error(c, loc, "incorrect number of function arguments");
    for (size_t i = 0; i < args.len; i++) {
        Node *arg = decay(c, args.data[i]);
        if (i < fn->members.len)
            arg = implicitly(c, arg, fn->members.data[i].type, loc);
        else if (arg->type->kind == TY_FLOAT && arg->type->size == 4)
            arg = convert(c, arg, type(c, TY_FLOAT, 8, 8), loc);
        else
            arg = promote(c, arg);
        args.data[i] = arg;
    }
    Node *n = node(c, N_CALL, loc, callee, NULL);
    n->list = args;
    n->type = fn->base;
    return n;
}

Node *va_argument(Context *c, Node *list, Type *t, Loc loc)
{
    if (!lvalue(list) || !same_type(list->type, c->va_list_type))
        error(c, loc, "va_arg requires a va_list object");
    if ((!scalar(t) && !(t->complete && (t->kind == TY_STRUCT || t->kind == TY_UNION))) ||
        (integer_type(t) && t->rank < 3) ||
        (t->kind == TY_FLOAT && t->size == 4))
        error(c, loc, "va_arg requires a supported type after default argument promotions");
    Node *result = node(c, N_VA_ARG, loc, list, NULL);
    result->type = t;
    return result;
}

static bool find_member(Type *t, const char *name, Member *match)
{
    if (t->kind != TY_STRUCT && t->kind != TY_UNION)
        return false;
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        if (m.name && equal(m.name, name)) {
            *match = m;
            return true;
        }
        if (!m.name && !m.bit_field && find_member(m.type, name, match)) {
            match->offset += m.offset;
            return true;
        }
    }
    return false;
}

Node *member_offset(Context *c, Type *t, Node *path, Loc loc)
{
    uint64_t offset = 0;
    for (; path; path = path->right) {
        if (path->name) {
            Member member;
            if (!find_member(t, path->name, &member) || member.bit_field) {
                error(c, path->loc, "invalid offsetof member '%s'", path->name);
                break;
            }
            offset += member.offset;
            t = member.type;
        } else {
            uint64_t index;
            if (t->kind != TY_ARRAY || !constant_int(c, path->left, &index)) {
                error(c, path->loc, "offsetof requires a constant array subscript");
                break;
            }
            offset += t->base->size * index;
            t = t->base;
        }
    }
    return integer(c, loc, offset, type_int(c, 4, true));
}

Node *field(Context *c, Node *n, const char *name, bool indirect, Loc loc)
{
    if (indirect)
        n = unary(c, '*', n, loc);
    Member m;
    if (find_member(n->type, name, &m)) {
        Node *field_node = node(c, N_MEMBER, loc, n, NULL);
        field_node->integer = m.offset;
        field_node->bit = m.bit;
        field_node->width = m.width;
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
    if (t->variable_bound && !alignment)
        return vla_size(c, t, loc);
    unsigned value = alignment ? t->align : t->size;
    if (!alignment && (t->kind == TY_VOID || t->kind == TY_FUNC))
        value = 1;
    return integer(c, loc, value, type_int(c, 4, true));
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

static bool constant_pointer(Context *, Node *, uint64_t *);

static bool constant_object_offset(Context *c, Node *n, uint64_t *value)
{
    if (n->kind == N_DEREF)
        return constant_pointer(c, n->left, value);
    if (n->kind == N_MEMBER && constant_object_offset(c, n->left, value)) {
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

bool constant_int(Context *c, Node *n, uint64_t *value)
{
    if (!n || !integer_type(n->type))
        return false;
    uint64_t a = 0, b = 0, result;
    switch (n->kind) {
    case N_INT: result = n->integer; break;
    case N_CAST:
        if (n->left->type->kind == TY_PTR) {
            if (!constant_pointer(c, n->left, &result))
                return false;
        } else if (n->left->kind == N_FLOAT) {
            long double real = n->left->real;
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
    case N_COND:
        if (!constant_int(c, n->left, &a))
            return false;
        return constant_int(c, a ? n->right : n->third, value);
    case N_UNARY:
        if (!constant_int(c, n->left, &a))
            return false;
        result = n->op == '-' ? 0 - a : n->op == '~' ? ~a : !a;
        break;
    case N_BINARY: {
        if (!constant_int(c, n->left, &a))
            return false;
        if ((n->op == AND && !a) || (n->op == OR && a)) {
            *value = n->op == OR;
            return true;
        }
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
        case AND: result = a&&b; break; case OR: result = a||b; break;
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

void define_enum(Context *c, const char *name, Node *expr, uint64_t *next, Loc loc)
{
    uint64_t value = *next;
    if (expr && !constant_int(c, expr, &value))
        error(c, loc, "enumerator requires an integer constant expression");
    if ((int64_t)value < INT32_MIN || (int64_t)value > INT32_MAX)
        extension(c, loc, "enumerator outside int range");
    c->enum_negative |= (int64_t)value < 0;
    if (find_in(c->scope, name))
        error(c, loc, "redeclaration of '%s'", name);
    Symbol *s = symbol(c, name, type_int(c, 3, false), loc);
    s->enum_constant = true;
    s->enum_value = value;
    PUSH(c, c->scope->names, s);
    *next = value + 1;
}

static Symbol *global(Context *c, const char *name, Type *t, unsigned flags, Loc loc)
{
    Scope *outer = c->scope;
    while (outer->parent)
        outer = outer->parent;
    Symbol *s = find_in(outer, name);
    if (!s) {
        s = symbol(c, name, t, loc);
        s->internal = flags & S_STATIC;
        s->thread_local_storage = (flags & S_THREAD) != 0;
        PUSH(c, outer->names, s);
        PUSH(c, c->globals, s);
    } else if (s->is_typedef || s->enum_constant || !compatible(s->type, t, true) ||
               s->thread_local_storage != ((flags & S_THREAD) != 0)) {
        error(c, loc, "conflicting declaration of '%s'", name);
    } else if (t->complete && (t->kind != TY_FUNC || t->prototype)) {
        s->type = t;
    }
    if (c->scope != outer && !find_in(c->scope, name))
        PUSH(c, c->scope->names, s);
    return s;
}

Node *declare(Context *c, Specs specs, Decl *d, Node *init, Loc loc)
{
    Decl *name = decl_name(d);
    const char *id = name->name;
    Type *t = apply_decl(c, spec_type(c, specs, loc), d);
    bool is_typedef = specs.flags & S_TYPEDEF;
    bool file_scope = c->scope->parent == NULL;
    bool local = !file_scope && t->kind != TY_FUNC && !(specs.flags & S_EXTERN);
    bool tls = (specs.flags & S_THREAD) != 0;
    Symbol *s = NULL;
    if (t->kind == TY_VOID && !is_typedef)
        error(c, loc, "object '%s' cannot have void type", id);
    if (tls && (is_typedef || t->kind == TY_FUNC ||
        (!file_scope && !(specs.flags & (S_STATIC | S_EXTERN))) ||
        (specs.flags & (S_AUTO | S_REGISTER))))
        error(c, loc, "invalid thread-local declaration of '%s'", id);
    if (t->variable_bound && (!local || is_typedef || (specs.flags & S_STATIC)))
        error(c, loc, "variable-length array requires automatic storage");
    if (is_typedef || local) {
        Symbol *prior = find_in(c->scope, id);
        if (prior && !(is_typedef && prior->is_typedef && compatible(prior->type, t, true)))
            error(c, loc, "redeclaration of '%s'", id);
        s = symbol(c, id, t, loc);
        s->is_typedef = is_typedef;
        PUSH(c, c->scope->names, s);
        if (!is_typedef) {
            if (specs.flags & S_STATIC) {
                s->assembly = format(c, ".Lstatic%u", c->unique++);
                s->internal = s->defined = true;
                s->thread_local_storage = tls;
                PUSH(c, c->globals, s);
            } else {
                s->local = true;
                PUSH(c, c->function->locals, s);
                if (t->variable_bound) {
                    Symbol *size = symbol(c, id, type_int(c, 4, true), loc);
                    size->local = true;
                    t->runtime_size = size;
                    PUSH(c, c->function->locals, size);
                }
            }
        }
    } else {
        s = global(c, id, t, specs.flags, loc);
        if (file_scope && t->kind != TY_FUNC && !(specs.flags & S_EXTERN))
            s->tentative = true;
    }
    Node *n = node(c, N_DECL, loc, NULL, NULL);
    n->symbol = s;
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
    return t->kind == TY_ARRAY ? t->base : t->members.data[index].type;
}

static size_t elements(Type *t)
{
    if (t->kind == TY_ARRAY)
        return t->complete ? t->length : SIZE_MAX;
    if (t->kind == TY_UNION)
        return t->members.len ? 1 : 0;
    return t->members.len;
}

static Node *initializer(Context *, Type **, Node *);

static Node *init_slot(Context *c, Node *list, size_t index, Loc loc)
{
    size_t n = 0;
    while (n < list->list.len && list->list.data[n]->integer < index)
        n++;
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

static bool choose_member(Type *t, const char *name, size_t *index, bool *anonymous)
{
    for (size_t i = 0; i < t->members.len; i++) {
        Member member = t->members.data[i];
        Member nested;
        if ((member.name && equal(member.name, name)) ||
            (!member.name && !member.bit_field && find_member(member.type, name, &nested))) {
            *index = i;
            *anonymous = !member.name;
            return true;
        }
    }
    return false;
}

static size_t designate(Context *c, Type *t, Node *list, Node *selector, Node *value)
{
    size_t index;
    Node *next = selector->right;
    if (selector->name) {
        bool anonymous;
        if ((t->kind != TY_STRUCT && t->kind != TY_UNION) ||
            !choose_member(t, selector->name, &index, &anonymous)) {
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
        if (next || t->kind != TY_ARRAY || !scalar(t->base) ||
            !constant_int(c, selector->third, &high) || high < index ||
            high >= elements(t) || high >= UINT_MAX || high - index > 65535) {
            error(c, selector->loc, "unsupported or invalid initializer range");
            return index;
        }
        Type *base = t->base;
        Node *once = node(c, N_RANGE_VALUE, selector->loc, initializer(c, &base, value), NULL);
        once->type = base;
        for (size_t i = index; i <= high; i++)
            init_slot(c, list, i, selector->loc)->left = once;
        return (size_t)high;
    }
    if (t->kind == TY_UNION && list->list.len && list->list.data[0]->integer != index)
        list->list.len = 0;
    Node *slot = init_slot(c, list, index, selector->loc);
    Type *child = subobject(t, index);
    if (next) {
        if (!slot->left) {
            slot->left = node(c, N_INIT, selector->loc, NULL, NULL);
            slot->left->type = child;
        } else if (slot->left->kind != N_INIT) {
            error(c, selector->loc, "nested designator cannot override this initializer");
            return index;
        }
        designate(c, child, slot->left, next, value);
    } else {
        slot->left = initializer(c, &child, value);
    }
    return index;
}

static Node *aggregate_init(Context *c, Type **destination, Nodes entries, size_t *cursor,
                            bool explicit_braces, Loc loc)
{
    Type *t = *destination;
    Node *result = node(c, N_INIT, loc, NULL, NULL);
    size_t index = 0, limit = elements(t);
    while (*cursor < entries.len && !c->failed) {
        Node *item = entries.data[*cursor];
        if (item->kind == N_DESIGNATE) {
            if (!explicit_braces)
                break;
            index = designate(c, t, result, item->left, item->right) + 1;
            (*cursor)++;
            if (item->left->right && *cursor < entries.len &&
                entries.data[*cursor]->kind != N_DESIGNATE)
                error(c, item->loc, "positional continuation of nested designator is not supported");
            continue;
        }
        while (t->kind == TY_STRUCT && index < limit && t->members.data[index].bit_field &&
               !t->members.data[index].name)
            index++;
        if (index >= limit) {
            if (explicit_braces)
                error(c, item->loc, "too many initializer elements");
            break;
        }
        Type *child = subobject(t, index);
        Node *value;
        if (aggregate(child) && item->kind != N_INIT && !string_initializer(child, item) &&
            !same_type(child, item->type)) {
            size_t previous = *cursor;
            value = aggregate_init(c, &child, entries, cursor, false, item->loc);
            if (*cursor == previous) {
                error(c, item->loc, "cannot initialize an empty aggregate without braces");
                break;
            }
        } else {
            (*cursor)++;
            value = initializer(c, &child, item);
        }
        init_slot(c, result, index++, item->loc)->left = value;
    }
    if (t->kind == TY_ARRAY && !t->complete) {
        size_t length = result->list.len ? result->list.data[result->list.len - 1]->integer + 1 : 0;
        *destination = t = array(c, t->base, length, true, loc);
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
    if (init->kind == N_COMPOUND && !init->symbol->local && aggregate(t) &&
        same_type(t, init->type) && c->gnu) {
        return init->symbol->init;
    }
    if (init->kind != N_INIT) {
        if (t->kind == TY_ARRAY)
            error(c, init->loc, "array initializer requires braces or a string");
        return implicitly(c, init, t, init->loc);
    }
    if (!aggregate(t)) {
        if (init->list.len != 1 || init->list.data[0]->kind == N_DESIGNATE) {
            if (!init->list.len && c->gnu && scalar(t))
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
    if (s->type->variable_bound) {
        error(c, init->loc, "variable-length array cannot be initialized");
        return;
    }
    if (s->is_typedef || s->type->kind == TY_FUNC || (!s->local && s->init)) {
        error(c, init->loc, "invalid or duplicate initializer for '%s'", s->name);
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
    Symbol *s = symbol(c, format(c, ".Lliteral%u", c->unique++), t, loc);
    s->internal = s->defined = true;
    s->local = c->function != NULL;
    if (s->local)
        PUSH(c, c->function->locals, s);
    else
        PUSH(c, c->globals, s);
    Node *raw = node(c, N_INIT, loc, NULL, NULL);
    raw->list = entries;
    s->init = initializer(c, &s->type, raw);
    Node *n = node(c, N_COMPOUND, loc, NULL, NULL);
    n->symbol = s;
    n->type = s->type;
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
    return n;
}

void begin_function(Context *c, Specs specs, Decl *d, Loc loc)
{
    Type *t = apply_decl(c, spec_type(c, specs, loc), d);
    const char *name = decl_name(d)->name;
    if (t->kind != TY_FUNC || (specs.flags & (S_TYPEDEF | S_THREAD))) {
        error(c, loc, "invalid function definition");
        t = type(c, TY_FUNC, 0, 1);
        t->base = type_int(c, 3, false);
    }
    Symbol *s = global(c, name, t, specs.flags, loc);
    if (s->defined)
        error(c, loc, "redefinition of function '%s'", name);
    s->type = t;
    s->defined = true;
    c->function = s;
    enter_scope(c);
    for (size_t i = 0; i < t->members.len; i++) {
        Member m = t->members.data[i];
        if (!m.name || find_in(c->scope, m.name)) {
            error(c, loc, "missing or duplicate parameter name");
            continue;
        }
        Symbol *param = symbol(c, m.name, m.type, loc);
        param->local = true;
        PUSH(c, c->scope->names, param);
        PUSH(c, s->params, param);
        PUSH(c, s->locals, param);
    }
    c->function_body = true;
}

void end_function(Context *c, Node *body)
{
    c->function->body = body;
    c->function = NULL;
}

Node *check_return(Context *c, Node *n, Loc loc)
{
    Type *t = c->function->type->base;
    if (!n || t->kind == TY_VOID) {
        if (!!n != (t->kind != TY_VOID))
            error(c, loc, "return value does not match function return type");
        return n;
    }
    return implicitly(c, n, t, loc);
}
