#define _XOPEN_SOURCE 700
#include "compiler.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

struct Macro {
    Macro *next;
    const char *name;
    Strings params;
    Tokens body;
    bool function, variadic, undefined, system_only;
};

struct Hide {
    const Hide *next;
    const Macro *macro;
};

struct SavedMacro {
    SavedMacro *next;
    const char *name;
    Macro *definition, *system_definition;
    bool used;
};

struct PackSetting {
    PackSetting *previous;
    const char *label;
    unsigned char alignment;
};

typedef struct Work Work;
struct Work { Work *next; Token token; };
typedef struct {
    Tokens *data, *expanded;
    size_t len, cap;
    bool *ready;
    bool omitted_pack;
} Arguments;

typedef struct {
    bool parent, taken, active, otherwise;
    Loc loc;
} Branch;
typedef VECTOR(Branch) Branches;

static bool is(Token token, const char *text)
{
    return equal(token.text, text);
}

static bool poisoned(Context *c, const char *name)
{
    for (size_t i = 0; i < c->poisoned_identifiers.len; i++)
        if (equal(name, c->poisoned_identifiers.data[i]))
            return true;
    return false;
}

static bool check_poisoned(Context *c, Token token)
{
    if (!c->skip_poison_checks && token.kind == WORD && !token.hide &&
        !token.poison_checked && poisoned(c, token.text)) {
        error(c, token.loc, "attempt to use poisoned identifier '%s'", token.text);
        return false;
    }
    return true;
}

static bool check_poisoned_tokens(Context *c, Tokens tokens)
{
    for (size_t i = 0; i < tokens.len; i++)
        if (!check_poisoned(c, tokens.data[i]))
            return false;
    return true;
}

static Macro *lookup_macro(Context *c, const char *name, bool system_header)
{
    for (Macro *m = c->macros; m; m = m->next)
        if (equal(m->name, name) && (!m->system_only || system_header))
            return m->undefined ? NULL : m;
    return NULL;
}

static Macro *lookup(Context *c, const char *name)
{
    return lookup_macro(c, name, c->in_system_header);
}

static bool predefined(Context *c, const char *name)
{
    return equal(name, "__LINE__") || equal(name, "__FILE__") ||
           equal(name, "__DATE__") || equal(name, "__TIME__") ||
           equal(name, "__has_builtin") || equal(name, "__has_attribute") ||
           equal(name, "__has_c_attribute") ||
           equal(name, "__has_include") || equal(name, "__has_include_next") ||
           ((c->dialect.gnu || c->dialect.standard >= C2Y) && equal(name, "__COUNTER__"));
}

static bool defined(Context *c, const char *name)
{
    return lookup(c, name) || predefined(c, name);
}

static Macro *new_macro(Context *c, const char *name)
{
    Macro *m = alloc(c, sizeof *m);
    m->name = name;
    m->next = c->macros;
    c->macros = m;
    return m;
}

static Work *work(Context *c, Tokens input, Work *next)
{
    for (size_t i = input.len; i; i--) {
        Work *item = alloc(c, sizeof *item);
        *item = (Work){next, input.data[i - 1]};
        next = item;
    }
    return next;
}

static bool hidden(const Hide *h, const Macro *m)
{
    for (; h; h = h->next)
        if (h->macro == m)
            return true;
    return false;
}

static const Hide *hide(Context *c, const Hide *prior, const Macro *m)
{
    if (hidden(prior, m))
        return prior;
    Hide *h = alloc(c, sizeof *h);
    *h = (Hide){prior, m};
    return h;
}

static const Hide *intersection(Context *c, const Hide *a, const Hide *b)
{
    const Hide *result = NULL;
    for (; a; a = a->next)
        if (hidden(b, a->macro))
            result = hide(c, result, a->macro);
    return result;
}

static const char *quoted(Context *c, const char *str)
{
    char *buffer = alloc(c, 2 * strlen(str) + 3);
    char *out = buffer;
    *out++ = '"';
    for (; *str; str++) {
        if (*str == '\n') {
            *out++ = '\\';
            *out++ = 'n';
        } else {
            if (*str == '"' || *str == '\\')
                *out++ = '\\';
            *out++ = *str;
        }
    }
    *out++ = '"';
    *out = 0;
    return buffer;
}

static const char *joined(Context *c, Tokens input, bool preserve_space)
{
    size_t length = 1;
    for (size_t i = 0; i < input.len; i++)
        length += strlen(input.data[i].text) + 1;
    char *result = alloc(c, length);
    char *out = result;
    for (size_t i = 0; i < input.len; i++) {
        Token t = input.data[i];
        if (i && (!preserve_space || t.space))
            *out++ = ' ';
        size_t width = strlen(t.text);
        memcpy(out, t.text, width);
        out += width;
    }
    return result;
}

static size_t parameter(Macro *m, Token token)
{
    for (size_t i = 0; i < m->params.len; i++)
        if (is(token, m->params.data[i]))
            return i;
    return SIZE_MAX;
}

static bool arguments(Context *c, Macro *m, Work **rest, Arguments *args, Token *invocation)
{
    Work *p = (*rest)->next;
    Tokens current = {0};
    unsigned depth = 0;
    while (p) {
        Token t = p->token;
        if (!check_poisoned(c, t))
            return false;
        /* Argument spelling is read before its macro expansion can run a pragma. */
        t.poison_checked = true;
        if (is(t, ")") && !depth) {
            bool unnamed_empty = m->variadic && m->params.len == 1 && !current.len;
            if (current.len || args->len || m->params.len)
                PUSH(c, *args, current);
            *rest = p->next;
            invocation->hide = intersection(c, invocation->hide, t.hide);
            bool omitted = m->variadic && args->len + 1 == m->params.len;
            args->omitted_pack = omitted || unnamed_empty;
            if (omitted && (c->dialect.gnu || c->dialect.standard >= C23))
                PUSH(c, *args, (Tokens){0});
            if (args->len == m->params.len)
                return true;
            error(c, invocation->loc, "macro '%s' expects %zu argument(s), got %zu",
                  m->name, m->params.len, args->len);
            return false;
        }
        if (is(t, ",") && !depth &&
            (!m->variadic || args->len + 1 < m->params.len)) {
            PUSH(c, *args, current);
            current = (Tokens){0};
        } else {
            if (is(t, "("))
                depth++;
            if (is(t, ")"))
                depth--;
            PUSH(c, current, t);
        }
        p = p->next;
    }
    error(c, invocation->loc, "unterminated invocation of '%s'", m->name);
    return false;
}

static Tokens expand(Context *, Tokens, unsigned);
static bool at(Tokens, size_t, const char *);
static bool glibc_header(Context *, const char *);
static FILE *find_header(Context *, Tokens, Loc, bool, const char **, size_t *);
static void pragma(Context *, const char *, Tokens, Loc, unsigned, Tokens *, bool);
static void diagnostic_pragma(Context *, Tokens, Loc);
static Tokens pragma_arguments(Context *, Token);

static Token pasted(Context *c, Token a, Token b, Loc loc)
{
    const char *text = format(c, "%s%s", a.text, b.text);
    Tokens tokens = {0};
    scan(c, loc.file, text, strlen(text), &tokens, NULL);
    if (tokens.len != 2 || tokens.data[0].kind == NEWLINE) {
        error(c, loc, "'%s' and '%s' do not form one preprocessing token", a.text, b.text);
        return a;
    }
    Token t = tokens.data[0];
    t.loc = loc;
    t.space = a.space;
    check_poisoned(c, t);
    t.hide = intersection(c, a.hide, b.hide);
    return t;
}

static size_t va_opt_end(Tokens body, size_t start)
{
    if (!at(body, start + 1, "("))
        return SIZE_MAX;
    size_t depth = 0;
    for (size_t i = start + 2; i < body.len; i++) {
        if (at(body, i, "("))
            depth++;
        else if (at(body, i, ")")) {
            if (!depth)
                return i;
            depth--;
        }
    }
    return SIZE_MAX;
}

static Tokens expand_argument(Context *c, Arguments args, size_t param, unsigned depth)
{
    if (!args.ready[param]) {
        args.expanded[param] = expand(c, args.data[param], depth + 1);
        args.ready[param] = true;
    }
    return args.expanded[param];
}

static Tokens substitute_span(Context *c, Macro *m, Tokens body, Arguments args,
                              Token invocation, unsigned depth)
{
    Tokens result = {0};
    bool paste = false, has_left = false;
    for (size_t i = 0; i < body.len; i++) {
        Token t = body.data[i];
        if (is(t, "##")) {
            paste = true;
            continue;
        }
        size_t param = parameter(m, t);
        Tokens part = {0};
        bool quote = m->function && is(t, "#");
        size_t operand = i + quote;
        if (m->variadic && at(body, operand, "__VA_OPT__")) {
            size_t end = va_opt_end(body, operand);
            if (end == SIZE_MAX)
                return result;
            if (expand_argument(c, args, args.len - 1, depth).len) {
                Tokens contents = {.data = body.data + operand + 2, .len = end - operand - 2};
                part = substitute_span(c, m, contents, args, invocation, depth);
            }
            i = end;
            if (quote) {
                t.kind = STRING;
                t.text = quoted(c, joined(c, part, true));
                part = (Tokens){0};
                PUSH(c, part, t);
            }
        } else if (quote && operand < body.len &&
                   (param = parameter(m, body.data[operand])) != SIZE_MAX) {
            t.kind = STRING;
            t.text = quoted(c, joined(c, args.data[param], true));
            i++;
            PUSH(c, part, t);
        } else if (param != SIZE_MAX) {
            bool raw = paste || at(body, i + 1, "##");
            part = raw ? args.data[param] : expand_argument(c, args, param, depth);
        } else {
            PUSH(c, part, t);
        }
        bool comma_pack = c->dialect.gnu && paste && m->variadic && param != SIZE_MAX &&
                          param + 1 == m->params.len && has_left &&
                          is(result.data[result.len - 1], ",");
        size_t first = 0;
        if (comma_pack && args.omitted_pack) {
            result.len--;
        } else if (paste && has_left && part.len && !comma_pack) {
            result.data[result.len - 1] = pasted(c, result.data[result.len - 1], part.data[0],
                                                 invocation.loc);
            first = 1;
        }
        for (size_t j = first; j < part.len; j++) {
            Token item = part.data[j];
            if (j == 0)
                item.space = t.space;
            PUSH(c, result, item);
        }
        has_left = part.len || (paste && has_left && !(comma_pack && args.omitted_pack));
        paste = false;
    }
    return result;
}

static Tokens substitute(Context *c, Macro *m, Arguments args, Token invocation, unsigned depth)
{
    if (args.len) {
        args.expanded = alloc(c, args.len * sizeof *args.expanded);
        args.ready = alloc(c, args.len * sizeof *args.ready);
    }
    Tokens result = substitute_span(c, m, m->body, args, invocation, depth);
    for (size_t i = 0; i < result.len; i++) {
        Token *t = &result.data[i];
        t->hide = hide(c, t->hide, m);
        for (const Hide *h = invocation.hide; h; h = h->next)
            t->hide = hide(c, t->hide, h->macro);
        t->loc = invocation.loc;
        t->loc.fallthrough_comment = 0;
    }
    if (result.len)
        result.data[0].space = invocation.space;
    return result;
}

static bool parenthesized(Context *c, Token t, Work **rest, Tokens *operand)
{
    if (!*rest || !is((*rest)->token, "(")) {
        error(c, t.loc, "%s requires a parenthesized operand", t.text);
        return false;
    }
    Work *p = (*rest)->next;
    size_t nested = 0;
    for (; p; p = p->next) {
        Token item = p->token;
        if (is(item, "("))
            nested++;
        else if (is(item, ")")) {
            if (!nested)
                break;
            nested--;
        }
        PUSH(c, *operand, item);
    }
    if (!p) {
        error(c, t.loc, "unterminated %s operand", t.text);
        return false;
    }
    *rest = p->next;
    return true;
}

static void pragma_operator(Context *c, Token op, Work **rest, unsigned depth, Tokens *result)
{
    Tokens operand = {0};
    if (!parenthesized(c, op, rest, &operand))
        return;
    operand = expand(c, operand, depth + 1);
    const char *start = operand.len == 1 && operand.data[0].kind == STRING ?
                        strchr(operand.data[0].text, '"') : NULL;
    if (!start) {
        error(c, op.loc, "_Pragma requires a string literal");
        return;
    }
    start++;
    char *buffer = alloc(c, strlen(start));
    size_t length = 0;
    for (const char *p = start; *p && p[1]; p++) {
        if (*p == '\\' && (p[1] == '\\' || p[1] == '"'))
            p++;
        buffer[length++] = *p;
    }
    Tokens directive = {0};
    if (scan(c, op.loc.file, buffer, length, &directive, NULL) && directive.len) {
        directive.len--;
        for (size_t i = 0; i < directive.len; i++)
            directive.data[i].loc = op.loc;
        pragma(c, c->include_path, directive, op.loc, op.loc.source_offset, result, true);
    }
}

static bool feature_query(Context *c, Token t, Work **rest, unsigned depth)
{
    Tokens operand = {0};
    if (!parenthesized(c, t, rest, &operand))
        return false;
    bool next = is(t, "__has_include_next");
    if (next || is(t, "__has_include")) {
        if (!c->pp_condition) {
            error(c, t.loc, "%s is only valid in a preprocessing conditional", t.text);
            return false;
        }
        if (!operand.len || (!is(operand.data[0], "<") && operand.data[0].kind != STRING))
            operand = expand(c, operand, depth + 1);
        const char *path;
        size_t index;
        FILE *file = find_header(c, operand, t.loc, next, &path, &index);
        if (!file)
            return false;
        fclose(file);
        return true;
    }
    operand = expand(c, operand, depth + 1);
    if (is(t, "__has_c_attribute")) {
        if (operand.len == 4 && operand.data[0].kind == WORD &&
            at(operand, 1, ":") && at(operand, 2, ":") && operand.data[3].kind == WORD) {
            if (!at(operand, 0, "gnu") && !at(operand, 0, "__gnu__"))
                return false;
            Attribute kind = attribute_kind(c, operand.data[3].text);
            return kind != ATTR_NONE && kind != ATTR_STDCALL;
        }
        if (operand.len != 1 || operand.data[0].kind != WORD)
            error(c, t.loc, "__has_c_attribute requires an attribute name");
        return false;
    }
    if (operand.len != 1 || operand.data[0].kind != WORD) {
        error(c, t.loc, "%s requires an identifier", t.text);
        return false;
    }
    if (is(t, "__has_builtin"))
        return builtin_available(c, operand.data[0].text);
    Attribute kind = attribute_kind(c, operand.data[0].text);
    return kind != ATTR_NONE && kind != ATTR_STDCALL;
}

static bool glibc_gnu_prereq(Context *c, Arguments args)
{
    if (args.len != 2 || args.data[0].len != 1 || args.data[1].len != 1)
        return false;
    Tokens major = args.data[0], minor = args.data[1];
    if (at(major, 0, "7") && at(minor, 0, "0"))
        return glibc_header(c, "/bits/floatn.h") || glibc_header(c, "/bits/floatn-common.h");
    /* These header branches use the built-in for GNU11 calls as well as later dialects. */
    return ((at(major, 0, "2") && at(minor, 0, "7")) ||
            ((at(major, 0, "8") || at(major, 0, "13")) && at(minor, 0, "0"))) &&
           glibc_header(c, "/tgmath.h");
}

static bool glibc_declaration_attribute(Context *c, Token keyword, Macro *macro, Arguments args,
                                    Tokens *result)
{
    if (c->pp_condition || !macro->system_only || !is(keyword, "__attribute__") || args.len != 1)
        return false;
    Tokens attribute = args.data[0];
    if (attribute.len < 3 || !at(attribute, 0, "(") || !at(attribute, attribute.len - 1, ")"))
        return false;
    Attribute kind = attribute_kind(c, attribute.data[1].text);
    bool marker = attribute.len == 3 && (kind == ATTR_NORETURN || kind == ATTR_WARN_UNUSED_RESULT ||
                                         kind == ATTR_SENTINEL || kind == ATTR_NONNULL ||
                                         kind == ATTR_RETURNS_NONNULL || kind == ATTR_NOPLT);
    bool parameterized = attribute.len >= 5 && (kind == ATTR_ERROR || kind == ATTR_WARNING ||
                                               kind == ATTR_SENTINEL || kind == ATTR_NONNULL ||
                                               kind == ATTR_RETURNS_NONNULL || kind == ATTR_NOPLT) &&
                         at(attribute, 2, "(") && at(attribute, attribute.len - 2, ")");
    if (!marker && !parameterized)
        return false;
    /* Keep the fallback macro visible to conditionals while forwarding this declaration attribute. */
    PUSH(c, *result, keyword);
    PUSH(c, *result, attribute.data[0]);
    for (size_t i = 0; i < attribute.len; i++)
        PUSH(c, *result, attribute.data[i]);
    PUSH(c, *result, attribute.data[attribute.len - 1]);
    return true;
}

static Tokens expand(Context *c, Tokens input, unsigned depth)
{
    Tokens result = {0};
    if (depth > 128) {
        error(c, input.len ? input.data[0].loc : (Loc){0}, "macro nesting exceeds 128 levels");
        return result;
    }
    for (Work *p = work(c, input, NULL); p && !c->failed;) {
        Token t = p->token;
        p = p->next;
        t.loc.diagnostics = c->diagnostics;
        t.loc.ignored_attributes = c->ignored_attributes;
        if (c->in_system_header)
            t.loc.system_header = true;
        if (t.kind == WORD) {
            if (poisoned(c, t.text)) {
                if (!check_poisoned(c, t))
                    break;
                PUSH(c, result, t);
                continue;
            }
            Macro *m = lookup(c, t.text);
            if (m && !hidden(t.hide, m) && (!m->function || (p && is(p->token, "(")))) {
                Arguments args = {0};
                if (m->function && !arguments(c, m, &p, &args, &t))
                    break;
                if (glibc_declaration_attribute(c, t, m, args, &result))
                    continue;
                if (c->pp_condition && is(t, "__GNUC_PREREQ") && glibc_gnu_prereq(c, args)) {
                    t.kind = NUMBER;
                    t.text = "1";
                    PUSH(c, result, t);
                } else {
                    p = work(c, substitute(c, m, args, t, depth), p);
                }
                continue;
            }
            if (is(t, "_Pragma")) {
                pragma_operator(c, t, &p, depth, &result);
                continue;
            }
            if (is(t, "__LINE__")) {
                t.kind = NUMBER;
                t.text = format(c, "%u", t.loc.line);
            } else if (is(t, "__FILE__")) {
                t.kind = STRING;
                t.text = quoted(c, t.loc.file);
            } else if (is(t, "__DATE__") || is(t, "__TIME__")) {
                t.kind = STRING;
                t.text = is(t, "__DATE__") ? c->date_macro : c->time_macro;
            } else if ((c->dialect.gnu || c->dialect.standard >= C2Y) && is(t, "__COUNTER__")) {
                t.kind = NUMBER;
                t.text = format(c, "%u", c->unique++);
            } else if (is(t, "__has_builtin") || is(t, "__has_attribute") ||
                       is(t, "__has_c_attribute") ||
                       is(t, "__has_include") || is(t, "__has_include_next")) {
                bool supported = feature_query(c, t, &p, depth);
                t.kind = NUMBER;
                t.text = supported ? "1" : "0";
            }
        }
        PUSH(c, result, t);
    }
    return result;
}

static Tokens tail(Tokens line, size_t start)
{
    if (start > line.len)
        start = line.len;
    return (Tokens){.data = line.data + start, .len = line.len - start};
}

static bool at(Tokens line, size_t i, const char *value)
{
    return i < line.len && is(line.data[i], value);
}

static bool glibc_header(Context *c, const char *suffix)
{
    if (!c->in_system_header || !lookup(c, "__GLIBC__"))
        return false;
    size_t length = strlen(c->include_path), end = strlen(suffix);
    return length >= end && equal(c->include_path + length - end, suffix);
}

static bool glibc_keyword_fallback(Context *c, const Macro *macro)
{
    if (!c->in_system_header || macro->body.len)
        return false;
    bool keyword = macro->function ?
        equal(macro->name, "__attribute__") && macro->params.len == 1 && !macro->variadic :
        equal(macro->name, "__extension__");
    return keyword && glibc_header(c, "/sys/cdefs.h");
}

static void define(Context *c, Tokens line, Loc loc)
{
    if (!line.len || line.data[0].kind != WORD) {
        error(c, loc, "expected a macro name");
        return;
    }
    if (predefined(c, line.data[0].text)) {
        error(c, loc, "cannot redefine '%s'", line.data[0].text);
        return;
    }
    Macro *m = new_macro(c, line.data[0].text);
    size_t i = 1;
    if (at(line, i, "(") && !line.data[i].space) {
        m->function = true;
        i++;
        if (!at(line, i, ")")) {
            for (;;) {
                if (at(line, i, "...")) {
                    m->variadic = true;
                    PUSH(c, m->params, "__VA_ARGS__");
                    i++;
                    break;
                }
                if (i == line.len || line.data[i].kind != WORD) {
                    error(c, loc, "expected a macro parameter");
                    return;
                }
                Token param = line.data[i++];
                if (parameter(m, param) != SIZE_MAX)
                    error(c, param.loc, "duplicate macro parameter");
                PUSH(c, m->params, param.text);
                if (c->dialect.gnu && at(line, i, "...")) {
                    m->variadic = true;
                    i++;
                    break;
                }
                if (!at(line, i, ","))
                    break;
                i++;
            }
        }
        if (!at(line, i++, ")")) {
            error(c, loc, "expected ')' in macro parameter list");
            return;
        }
    }
    m->body = tail(line, i);
    /* glibc's x86 header checks compiler versions before exposing its f128 API. */
    if (c->target == &x86_64_linux && equal(m->name, "__HAVE_FLOAT128") &&
        m->body.len == 1 && at(m->body, 0, "0") && glibc_header(c, "/bits/floatn.h"))
        m->body.data[0].text = "1";
    /* Keep glibc's compatibility macros available to system headers while
       exposing the language keywords in application code. */
    m->system_only = glibc_keyword_fallback(c, m);
    if (at(m->body, 0, "##") || (m->body.len && at(m->body, m->body.len - 1, "##")))
        error(c, loc, "'##' cannot appear at either end of a macro replacement");
    size_t opt_end = 0;
    for (size_t j = 0; j < m->body.len; j++) {
        if (at(m->body, j, "__VA_OPT__")) {
            if (!m->variadic || j < opt_end) {
                error(c, m->body.data[j].loc, "invalid use of __VA_OPT__");
                return;
            }
            if (c->dialect.standard < C23)
                extension(c, m->body.data[j].loc, "__VA_OPT__");
            opt_end = va_opt_end(m->body, j);
            if (opt_end == SIZE_MAX || at(m->body, j + 2, "##") || at(m->body, opt_end - 1, "##")) {
                error(c, m->body.data[j].loc, "invalid __VA_OPT__ replacement");
                return;
            }
        }
        if (m->function && at(m->body, j, "#") && (j + 1 == m->body.len ||
            (!at(m->body, j + 1, "__VA_OPT__") && parameter(m, m->body.data[j + 1]) == SIZE_MAX)))
            error(c, m->body.data[j].loc, "'#' must precede a macro parameter");
    }
}

typedef struct { Context *context; Tokens tokens; size_t index; Loc loc; } IfParser;

static uint64_t if_expr(IfParser *p, unsigned min, bool evaluate);

static bool eat(IfParser *p, const char *str)
{
    if (!at(p->tokens, p->index, str))
        return false;
    p->index++;
    return true;
}

static uint64_t if_primary(IfParser *p, bool evaluate)
{
    if (eat(p, "(")) {
        uint64_t value = if_expr(p, 1, evaluate);
        if (!eat(p, ")"))
            error(p->context, p->loc, "expected ')' in preprocessing expression");
        return value;
    }
    if (eat(p, "!")) return !if_primary(p, evaluate);
    if (eat(p, "~")) return ~if_primary(p, evaluate);
    if (eat(p, "+")) return if_primary(p, evaluate);
    if (eat(p, "-")) return 0 - if_primary(p, evaluate);
    if (p->index == p->tokens.len) {
        error(p->context, p->loc, "incomplete preprocessing expression");
        return 0;
    }
    Token t = p->tokens.data[p->index++];
    if (t.kind == WORD)
        return p->context->dialect.standard >= C23 && is(t, "true");
    if (t.kind == CHARACTER) {
        Node *n = literal(p->context, t);
        return n ? n->integer : 0;
    }
    if (t.kind == NUMBER) {
        char *end;
        errno = 0;
        bool binary_literal = t.text[0] == '0' && (t.text[1] == 'b' || t.text[1] == 'B');
        const char *start = t.text + (binary_literal ? 2 : 0);
        if (binary_literal && p->context->dialect.standard < C23)
            extension(p->context, t.loc, "binary literal");
        uint64_t value = strtoull(start, &end, binary_literal ? 2 : 0);
        if (end != start && !errno && end[strspn(end, "uUlL")] == 0)
            return value;
    }
    error(p->context, t.loc, "invalid token '%s' in preprocessing expression", t.text);
    return 0;
}

static const char *const operators[][4] = {
    {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="},
    {"<", ">", "<=", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}
};

static uint64_t pp_binary(IfParser *p, const char *op, uint64_t a, uint64_t b)
{
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    if (equal(op, "||")) return a || b;
    if (equal(op, "&&")) return a && b;
    if (equal(op, "|")) return a | b;
    if (equal(op, "^")) return a ^ b;
    if (equal(op, "&")) return a & b;
    if (equal(op, "==")) return a == b;
    if (equal(op, "!=")) return a != b;
    if (equal(op, "<")) return sa < sb;
    if (equal(op, ">")) return sa > sb;
    if (equal(op, "<=")) return sa <= sb;
    if (equal(op, ">=")) return sa >= sb;
    if (equal(op, "<<")) return b < 64 ? a << b : 0;
    if (equal(op, ">>")) return b < 64 ? (uint64_t)(sa >> b) : 0;
    if (equal(op, "+")) return a + b;
    if (equal(op, "-")) return a - b;
    if (equal(op, "*")) return a * b;
    if (!b || (sa == INT64_MIN && sb == -1)) {
        error(p->context, p->loc, "invalid division in preprocessing expression");
        return 0;
    }
    return (uint64_t)(equal(op, "/") ? sa / sb : sa % sb);
}

static uint64_t if_expr(IfParser *p, unsigned min, bool evaluate)
{
    uint64_t left = if_primary(p, evaluate);
    while (p->index < p->tokens.len) {
        const char *op = p->tokens.data[p->index].text;
        unsigned precedence = 0;
        for (unsigned i = 0; i < sizeof operators / sizeof *operators; i++)
            for (unsigned j = 0; j < 4 && operators[i][j]; j++)
                if (equal(op, operators[i][j]))
                    precedence = i + 2;
        if (precedence < min)
            break;
        p->index++;
        bool use_right = evaluate && !(equal(op, "&&") && !left) && !(equal(op, "||") && left);
        uint64_t right = if_expr(p, precedence + 1, use_right);
        if (evaluate)
            left = pp_binary(p, op, left, right);
    }
    if (min == 1 && eat(p, "?")) {
        uint64_t yes = if_expr(p, 1, evaluate && left);
        if (!eat(p, ":"))
            error(p->context, p->loc, "expected ':' in preprocessing expression");
        uint64_t no = if_expr(p, 1, evaluate && !left);
        left = left ? yes : no;
    }
    return left;
}

static bool test_condition(Context *c, Tokens args, Loc loc)
{
    Tokens input = {0};
    for (size_t i = 0; i < args.len; i++) {
        Token t = args.data[i];
        if (is(t, "defined")) {
            if (!check_poisoned(c, t))
                return false;
            bool paren = at(args, i + 1, "(");
            i += paren ? 2 : 1;
            if (i >= args.len || args.data[i].kind != WORD) {
                error(c, t.loc, "expected identifier after 'defined'");
                return false;
            }
            if (!check_poisoned(c, args.data[i]))
                return false;
            t.kind = NUMBER;
            t.text = defined(c, args.data[i].text) ? "1" : "0";
            if (paren && !at(args, ++i, ")")) {
                error(c, t.loc, "expected ')' after 'defined'");
                return false;
            }
        }
        PUSH(c, input, t);
    }
    c->pp_condition = true;
    Tokens expanded = expand(c, input, 0);
    c->pp_condition = false;
    if (c->failed)
        return false;
    IfParser p = {c, expanded, 0, loc};
    uint64_t output = if_expr(&p, 1, true);
    if (p.index != p.tokens.len)
        error(c, p.tokens.data[p.index].loc, "unexpected token in preprocessing expression");
    return output != 0;
}

static bool read_source(Context *c, const char *path, FILE *file, Tokens *result, unsigned depth);

static const char *header_name(Context *c, Tokens args, bool *local)
{
    *local = args.len == 1 && args.data[0].kind == STRING && args.data[0].text[0] == '"';
    if (*local) {
        const char *text = args.data[0].text;
        return copy(c, text + 1, strlen(text) - 2);
    }
    if (args.len >= 3 && at(args, 0, "<") && at(args, args.len - 1, ">")) {
        Tokens inside = tail(args, 1);
        inside.len--;
        return joined(c, inside, true);
    }
    return NULL;
}

static FILE *open_header(Context *c, const char *name, bool local, bool next,
                         const char **resolved, size_t *index)
{
    *resolved = name;
    const char *path = c->include_path;
    const char *slash = strrchr(path, '/');
    const char *directory = slash ? copy(c, path, (size_t)(slash - path)) : ".";
    size_t start = next ? (c->include_index == SIZE_MAX ? 1 : c->include_index + 1) : !local;
    for (size_t i = start; i <= c->includes.len + c->system_includes.len; i++) {
        const char *dir = i == 0 ? directory : i <= c->includes.len ? c->includes.data[i - 1]
            : c->system_includes.data[i - c->includes.len - 1];
        const char *candidate = name[0] == '/' ? name : format(c, "%s/%s", dir, name);
        FILE *file = fopen(candidate, "rb");
        if (file) {
            *resolved = candidate;
            *index = i;
            return file;
        }
    }
    return NULL;
}

static FILE *find_header(Context *c, Tokens args, Loc loc, bool next, const char **resolved,
                         size_t *index)
{
    bool local;
    const char *name = header_name(c, args, &local);
    if (!name || !*name) {
        error(c, loc, "expected a quoted or bracketed header name");
        return NULL;
    }
    return open_header(c, name, local, next, resolved, index);
}

static bool include_file(Context *c, Tokens args, Loc loc, Tokens *result, unsigned depth, bool next)
{
    if (!at(args, 0, "<") && !(args.len && args.data[0].kind == STRING))
        args = expand(c, args, 0);
    const char *path = NULL;
    size_t index;
    FILE *file = find_header(c, args, loc, next, &path, &index);
    if (!file) {
        if (!c->failed)
            error(c, loc, "header '%s' not found", path);
        return false;
    }
    char *canonical = realpath(path, NULL);
    if (canonical) {
        path = copy(c, canonical, strlen(canonical));
        free(canonical);
    }
    const char *saved_path = c->include_path;
    size_t saved_index = c->include_index;
    bool saved_system_header = c->in_system_header;
    unsigned saved_system_header_offset = c->system_header_offset;
    c->include_path = path;
    c->include_index = index;
    c->in_system_header = index ? index > c->includes.len : saved_system_header;
    bool ok = read_source(c, path, file, result, depth + 1);
    c->include_path = saved_path;
    c->include_index = saved_index;
    c->in_system_header = saved_system_header;
    c->system_header_offset = saved_system_header_offset;
    fclose(file);
    return ok;
}

static void flush(Context *c, Tokens *pending, Tokens *out)
{
    Tokens result = expand(c, *pending, 0);
    for (size_t i = 0; i < result.len; i++) {
        Token token = result.data[i];
        token.loc.diagnostics = c->diagnostics;
        token.loc.ignored_attributes = c->ignored_attributes;
        if (token.kind == PRAGMA_DIAGNOSTIC) {
            /* Macro arguments may be expanded before their tokens are placed in the result. */
            Tokens args = pragma_arguments(c, token);
            diagnostic_pragma(c, tail(args, 1), token.loc);
        }
        PUSH(c, *out, token);
    }
    pending->len = 0;
}

static void restore_macro(Context *c, const char *name, const Macro *definition, bool system_only)
{
    Macro *restored = new_macro(c, name);
    if (definition) {
        Macro *next = restored->next;
        *restored = *definition;
        restored->next = next;
    } else {
        restored->undefined = true;
    }
    restored->system_only = system_only;
}

static void pragma_dependency(Context *c, const char *path, Tokens args, Loc loc)
{
    Tokens header = {.data = args.data, .len = args.len != 0};
    if (at(args, 0, "<")) {
        while (header.len < args.len && !at(header, header.len - 1, ">"))
            header.len++;
    }
    bool local;
    const char *name = header_name(c, header, &local);
    if (!name || !*name) {
        error(c, loc, "#pragma GCC dependency expects a quoted or bracketed file name");
        return;
    }
    Tokens message = tail(args, header.len);
    if (!check_poisoned_tokens(c, message))
        return;
    const char *resolved;
    size_t index;
    FILE *file = open_header(c, name, local, false, &resolved, &index);
    if (!file) {
        error(c, loc, "dependency '%s' not found", name);
        return;
    }
    struct stat source, dependency;
    bool newer = !fstat(fileno(file), &dependency) && !stat(path, &source) &&
                 dependency.st_mtime > source.st_mtime;
    fclose(file);
    if (newer) {
        warning(c, loc, "current file is older than %s", name);
        if (message.len)
            warning(c, loc, "%s", joined(c, message, true));
    }
}

static void pragma_pack(Context *c, Tokens args, Loc loc)
{
    if (!at(args, 0, "(")) {
        warning_at(c, WARNING_PRAGMAS, loc, "missing '(' after #pragma pack");
        return;
    }
    size_t close = 1;
    while (close < args.len && !at(args, close, ")"))
        close++;
    if (close == args.len) {
        warning_at(c, WARNING_PRAGMAS, loc, "missing ')' in #pragma pack");
        return;
    }
    if (close + 1 < args.len)
        warning_at(c, WARNING_PRAGMAS, loc, "extra tokens after #pragma pack");

    Tokens arguments = {.data = args.data + 1, .len = close - 1};
    bool push = at(arguments, 0, "push"), pop = at(arguments, 0, "pop");
    if (arguments.len && arguments.data[0].kind == WORD && !push && !pop) {
        warning_at(c, WARNING_PRAGMAS, loc, "unknown action '%s' for #pragma pack", arguments.data[0].text);
        return;
    }
    size_t i = push || pop;
    const char *label = NULL;
    bool has_alignment = arguments.len && !push && !pop;
    if ((push || pop) && at(arguments, i, ",")) {
        i++;
        if (i < arguments.len && arguments.data[i].kind == WORD) {
            label = arguments.data[i++].text;
            if (push && at(arguments, i, ",")) {
                i++;
                has_alignment = true;
            }
        } else if (push) {
            has_alignment = true;
        } else {
            warning_at(c, WARNING_PRAGMAS, loc, "expected an identifier after #pragma pack(pop,");
            return;
        }
    }
    if (i + has_alignment != arguments.len ||
        (has_alignment && arguments.data[i].kind != NUMBER)) {
        warning_at(c, WARNING_PRAGMAS, loc, "malformed #pragma pack");
        return;
    }

    uint64_t alignment = c->default_pack_alignment;
    if (has_alignment) {
        Node *value = literal(c, arguments.data[i]);
        if (c->failed)
            return;
        if (value->kind != N_INT || value->integer_high) {
            warning_at(c, WARNING_PRAGMAS, loc, "#pragma pack requires a small integer alignment");
            return;
        }
        alignment = value->integer;
    }
    if (c->force_pack) {
        warning_at(c, WARNING_PRAGMAS, loc, "#pragma pack has no effect with -fpack-struct");
        return;
    }
    if (!valid_pack_alignment(alignment)) {
        warning_at(c, WARNING_PRAGMAS, loc, "#pragma pack alignment must be 0, 1, 2, 4, 8, or 16");
        return;
    }
    if (pop) {
        PackSetting *saved = c->pack_stack;
        if (label) {
            while (saved && (!saved->label || !equal(saved->label, label)))
                saved = saved->previous;
            if (!saved) {
                warning_at(c, WARNING_PRAGMAS, loc, "no matching #pragma pack(push, %s)", label);
                saved = c->pack_stack;
            }
        }
        if (saved) {
            c->pack_alignment = saved->alignment;
            c->pack_stack = saved->previous;
        } else if (!label) {
            warning_at(c, WARNING_PRAGMAS, loc, "#pragma pack(pop) without a matching #pragma pack(push)");
        }
    } else {
        if (push) {
            PackSetting *saved = alloc(c, sizeof *saved);
            *saved = (PackSetting){c->pack_stack, label, c->pack_alignment};
            c->pack_stack = saved;
        }
        if (!push || has_alignment)
            c->pack_alignment = (unsigned char)alignment;
    }
}

static Tokens pragma_arguments(Context *c, Token token)
{
    Tokens args = {0};
    if (!scan(c, token.loc.file, token.text, strlen(token.text), &args, NULL) || !args.len)
        return (Tokens){0};
    args.len--;
    for (size_t i = 0; i < args.len; i++)
        args.data[i].loc = token.loc;
    return tail(args, 1);
}

void apply_pack_pragma(Context *c, Token token)
{
    Tokens args = pragma_arguments(c, token);
    if (!c->failed)
        pragma_pack(c, args, token.loc);
}

void apply_weak_pragma(Context *c, Token token)
{
    Tokens args = pragma_arguments(c, token);
    if ((args.len != 1 && args.len != 3) || args.data[0].kind != WORD ||
        (args.len == 3 && (!at(args, 1, "=") || args.data[2].kind != WORD))) {
        warning_at(c, WARNING_PRAGMAS, token.loc, "malformed #pragma weak");
        return;
    }
    const char *target = args.len == 3 ? args.data[2].text : NULL;
    PUSH(c, c->weak_pragmas, ((WeakPragma){args.data[0].text, target, token.loc}));
}

void apply_visibility_pragma(Context *c, Token token)
{
    Tokens args = tail(pragma_arguments(c, token), 1);
    if (at(args, 0, "push")) {
        Visibility visibility = VIS_UNSPECIFIED;
        if (args.len >= 4 && at(args, 1, "(") && args.data[2].kind == WORD && at(args, 3, ")"))
            visibility = visibility_kind(args.data[2].text);
        if (!visibility) {
            warning_at(c, WARNING_PRAGMAS, token.loc, "#pragma GCC visibility push requires default, internal, hidden or protected");
            return;
        }
        PUSH(c, c->visibility_stack, c->pragma_visibility);
        c->pragma_visibility = visibility;
    } else if (at(args, 0, "pop")) {
        if (c->visibility_stack.len)
            c->pragma_visibility = c->visibility_stack.data[--c->visibility_stack.len];
        else
            warning_at(c, WARNING_PRAGMAS, token.loc, "no matching push for #pragma GCC visibility pop");
    } else {
        warning_at(c, WARNING_PRAGMAS, token.loc, "#pragma GCC visibility requires push or pop");
    }
}

static void diagnostic_pragma(Context *c, Tokens args, Loc loc)
{
    if (at(args, 0, "push")) {
        PUSH(c, c->diagnostic_stack, c->diagnostics);
        return;
    }
    if (at(args, 0, "pop")) {
        c->diagnostics = c->diagnostic_stack.len ?
                         c->diagnostic_stack.data[--c->diagnostic_stack.len] : NULL;
        return;
    }
    if (at(args, 0, "ignored_attributes")) {
        if (args.len < 2 || args.data[1].kind != STRING || args.data[1].text[0] != '"' ||
            !ignore_attribute(c, decode_narrow_string(c, args.data[1])))
            warning_at(c, WARNING_PRAGMAS, loc, "#pragma GCC diagnostic ignored_attributes requires a quoted scoped attribute");
        return;
    }
    DiagnosticLevel level = at(args, 0, "ignored") ? DIAG_IGNORED :
                            at(args, 0, "warning") ? DIAG_WARNING :
                            at(args, 0, "error") ? DIAG_ERROR : DIAG_DEFAULT;
    if (!level) {
        warning_at(c, WARNING_PRAGMAS, loc, "expected ignored, warning, error, push, or pop after #pragma GCC diagnostic");
        return;
    }
    if (args.len < 2 || args.data[1].kind != STRING || args.data[1].text[0] != '"') {
        warning_at(c, WARNING_PRAGMAS, loc, "expected a quoted warning option in #pragma GCC diagnostic");
        return;
    }
    const char *name = decode_narrow_string(c, args.data[1]);
    WarningKind kind = !strncmp(name, "-W", 2) ? warning_kind(name + 2) : WARNING_NONE;
    if (kind == WARNING_NONE) {
        warning_at(c, WARNING_PRAGMAS, loc, "unknown warning option '%s' in #pragma GCC diagnostic", name);
        return;
    }
    DiagnosticState *state = alloc(c, sizeof *state);
    *state = (DiagnosticState){.previous = c->diagnostics, .warning = kind, .level = level};
    c->diagnostics = state;
}

static void message_pragma(Context *c, Tokens args, Loc loc, Tokens *result)
{
    args = expand(c, args, 0);
    if (at(args, 0, "(") && at(args, args.len - 1, ")")) {
        args = tail(args, 1);
        args.len--;
    }
    if (!args.len) {
        warning_at(c, WARNING_PRAGMAS, loc, "#pragma message expects a string literal");
        return;
    }
    size_t length = 0;
    for (size_t i = 0; i < args.len; i++) {
        Token token = args.data[i];
        if (token.kind != STRING || token.text[0] != '"') {
            warning_at(c, WARNING_PRAGMAS, loc, "#pragma message expects a string literal");
            return;
        }
        length += strlen(token.text);
    }
    char *message = alloc(c, length + 1);
    size_t used = 0;
    for (size_t i = 0; i < args.len; i++) {
        const char *part = decode_narrow_string(c, args.data[i]);
        size_t size = strlen(part);
        memcpy(message + used, part, size);
        used += size;
    }
    message[used] = '\0';
    PUSH(c, *result, ((Token){.kind = PRAGMA_MESSAGE, .loc = loc, .text = message}));
}

static void pragma(Context *c, const char *path, Tokens args, Loc loc, unsigned next_offset,
                   Tokens *result, bool expanding)
{
    bool poison = at(args, 0, "GCC") && at(args, 1, "poison");
    bool dependency = at(args, 0, "GCC") && at(args, 1, "dependency");
    Tokens words = {.data = args.data, .len = poison || dependency ? 2 : args.len};
    if (!check_poisoned_tokens(c, words))
        return;
    if (at(args, 0, "once")) {
        PUSH(c, c->once, path);
    } else if (at(args, 0, "GCC") && at(args, 1, "system_header")) {
        if (c->include_index == SIZE_MAX) {
            warning_at(c, WARNING_PRAGMAS, loc, "#pragma GCC system_header ignored outside an include file");
        } else {
            if (args.len > 2)
                warning_at(c, WARNING_PRAGMAS, loc, "extra tokens after #pragma GCC system_header");
            c->in_system_header = true;
            if (next_offset < c->system_header_offset)
                c->system_header_offset = next_offset;
        }
    } else if (at(args, 0, "GCC") && (at(args, 1, "warning") || at(args, 1, "error"))) {
        bool fatal = at(args, 1, "error");
        const char *message = args.len >= 3 && args.data[2].kind == STRING ? args.data[2].text : "";
        if (message[0] != '"' && !(message[0] == 'R' && message[1] == '"')) {
            error(c, loc, "invalid #pragma GCC %s directive", args.data[1].text);
        } else if (fatal || !suppress_system_diagnostic(c, loc)) {
            message = decode_narrow_string(c, args.data[2]);
            if (!c->failed) {
                if (fatal)
                    error(c, loc, "%s", message);
                else
                    warning(c, loc, "%s", message);
            }
        }
    } else if (at(args, 0, "GCC") && at(args, 1, "diagnostic")) {
        if (!expanding)
            diagnostic_pragma(c, tail(args, 2), loc);
        PUSH(c, *result, ((Token){.kind = PRAGMA_DIAGNOSTIC, .loc = loc, .text = joined(c, args, true)}));
    } else if (at(args, 0, "message")) {
        message_pragma(c, tail(args, 1), loc, result);
    } else if (dependency) {
        pragma_dependency(c, path, tail(args, 2), loc);
    } else if (poison) {
        for (size_t i = 2; i < args.len; i++) {
            Token token = args.data[i];
            if (token.kind != WORD) {
                error(c, token.loc, "invalid #pragma GCC poison directive");
                return;
            }
            if (poisoned(c, token.text))
                continue;
            if (defined(c, token.text)) {
                warning(c, token.loc, "poisoning existing macro '%s'", token.text);
                new_macro(c, token.text)->undefined = true;
            }
            PUSH(c, c->poisoned_identifiers, token.text);
        }
    } else if (at(args, 0, "push_macro") || at(args, 0, "pop_macro")) {
        if (args.len != 4 || !at(args, 1, "(") || args.data[2].kind != STRING ||
            !at(args, 3, ")") || args.data[2].text[0] != '"') {
            error(c, loc, "expected a quoted macro name in pragma");
            return;
        }
        const char *quoted = args.data[2].text;
        const char *name = copy(c, quoted + 1, strlen(quoted) - 2);
        if (at(args, 0, "push_macro")) {
            SavedMacro *saved = alloc(c, sizeof *saved);
            *saved = (SavedMacro){c->saved_macros, name, lookup_macro(c, name, false),
                                  lookup_macro(c, name, true), false};
            c->saved_macros = saved;
        } else {
            for (SavedMacro *saved = c->saved_macros; saved; saved = saved->next)
                if (!saved->used && equal(saved->name, name)) {
                    restore_macro(c, name, saved->definition, false);
                    if (saved->system_definition != saved->definition)
                        restore_macro(c, name, saved->system_definition, true);
                    saved->used = true;
                    break;
                }
        }
    } else if (at(args, 0, "pack") || at(args, 0, "weak") ||
               (at(args, 0, "GCC") && at(args, 1, "visibility"))) {
        TokenKind kind = at(args, 0, "GCC") ? PRAGMA_VISIBILITY :
                         at(args, 0, "pack") ? PRAGMA_PACK : PRAGMA_WEAK;
        PUSH(c, *result, ((Token){.kind = kind, .loc = loc, .text = joined(c, args, true)}));
    } else if (args.len) {
        PUSH(c, *result, ((Token){.kind = PRAGMA_UNKNOWN, .loc = loc, .text = joined(c, args, true)}));
    }
}

static bool read_source(Context *c, const char *path, FILE *file, Tokens *result, unsigned depth)
{
    for (size_t i = 0; i < c->once.len; i++)
        if (equal(c->once.data[i], path))
            return true;
    if (depth > 128) {
        error(c, (Loc){.file = path}, "include nesting exceeds 128 levels");
        return false;
    }
    size_t length = 0, capacity = 8192;
    char *bytes = malloc(capacity);
    if (!bytes) {
        error(c, (Loc){.file = path}, "out of memory reading source");
        return false;
    }
    for (;;) {
        length += fread(bytes + length, 1, capacity - length, file);
        if (length < capacity)
            break;
        char *larger = capacity <= SIZE_MAX / 2 ? realloc(bytes, capacity * 2) : NULL;
        if (!larger) {
            error(c, (Loc){.file = path}, "out of memory reading source");
            free(bytes);
            return false;
        }
        bytes = larger;
        capacity *= 2;
    }
    Tokens raw = {0}, pending = {0};
    Locations splice_warnings = {0};
    c->system_header_offset = c->in_system_header ? 0 : UINT_MAX;
    if (ferror(file))
        error(c, (Loc){.file = path}, "error reading source");
    else
        scan(c, path, bytes, length, &raw, &splice_warnings);
    free(bytes);
    Branches branches = {0};
    const char *logical_file = path;
    bool line_marker_system = false;
    int64_t line_delta = 0;
    size_t next_warning = 0;
    for (size_t start = 0; start < raw.len && !c->failed;) {
        size_t end = start;
        while (end < raw.len && raw.data[end].kind != NEWLINE)
            end++;
        Tokens line = {.data = raw.data + start, .len = end - start};
        start = end + 1;
        unsigned physical_line = raw.data[end].loc.line;
        while (next_warning < splice_warnings.len &&
               splice_warnings.data[next_warning].line <= physical_line) {
            Loc *loc = &splice_warnings.data[next_warning++];
            loc->file = logical_file;
            loc->line = loc->end_line = (unsigned)(loc->line + line_delta);
            loc->system_header |= line_marker_system;
        }
        if (!line.len)
            continue;
        bool directive_line = at(line, 0, "#");
        if (directive_line)
            flush(c, &pending, result);
        for (size_t i = 0; i < line.len; i++) {
            Loc *loc = &line.data[i].loc;
            loc->file = logical_file;
            loc->line = (unsigned)(loc->line + line_delta);
            loc->end_line = (unsigned)(loc->end_line + line_delta);
            loc->system_header = c->in_system_header || line_marker_system;
            loc->diagnostics = c->diagnostics;
            loc->ignored_attributes = c->ignored_attributes;
        }
        Branch *branch = branches.len ? &branches.data[branches.len - 1] : NULL;
        bool active = !branch || branch->active;
        if (!directive_line) {
            if (active) {
                line.data[0].space = true;
                for (size_t i = 0; i < line.len; i++)
                    PUSH(c, pending, line.data[i]);
            }
            continue;
        }
        if (line.len == 1)
            continue;
        Token directive = line.data[1];
        Loc loc = directive.loc;
        Tokens args = tail(line, 2);
        if (active && !check_poisoned(c, directive))
            break;
        if (active && (is(directive, "define") || is(directive, "undef") || is(directive, "ifdef") ||
                       is(directive, "ifndef") || is(directive, "error") || is(directive, "warning")) &&
            !check_poisoned_tokens(c, args))
            break;
        if (is(directive, "if") || is(directive, "ifdef") || is(directive, "ifndef")) {
            bool value = false;
            if (active) {
                if (is(directive, "if"))
                    value = test_condition(c, args, loc);
                else if (args.len == 1 && args.data[0].kind == WORD)
                    value = defined(c, args.data[0].text) == is(directive, "ifdef");
                else
                    error(c, loc, "expected an identifier after #%s", directive.text);
            }
            Branch item = {active, value, active && value, false, loc};
            PUSH(c, branches, item);
        } else if (is(directive, "elif") || is(directive, "elifdef") || is(directive, "elifndef") ||
                   is(directive, "else") || is(directive, "endif")) {
            if (!branch) {
                error(c, loc, "unexpected #%s", directive.text);
            } else if (is(directive, "endif")) {
                branches.len--;
            } else if (branch->otherwise) {
                error(c, loc, "#%s after #else", directive.text);
            } else {
                branch->otherwise = is(directive, "else");
                bool eligible = branch->parent && !branch->taken;
                if (!eligible || branch->otherwise) {
                    branch->active = eligible;
                } else if (is(directive, "elif")) {
                    /* The condition is read while skipping the preceding branch. */
                    c->skip_poison_checks = true;
                    branch->active = test_condition(c, args, loc);
                    c->skip_poison_checks = false;
                } else {
                    if (c->dialect.standard < C23)
                        extension(c, loc, directive.text);
                    if (args.len != 1 || args.data[0].kind != WORD)
                        error(c, loc, "expected an identifier after #%s", directive.text);
                    else
                        branch->active = defined(c, args.data[0].text) == is(directive, "elifdef");
                }
                branch->taken |= branch->active;
            }
        } else if (!active) {
            continue;
        } else if (is(directive, "define")) {
            define(c, args, loc);
        } else if (is(directive, "undef")) {
            if (args.len != 1 || args.data[0].kind != WORD)
                error(c, loc, "expected an identifier after #undef");
            else if (predefined(c, args.data[0].text))
                error(c, loc, "cannot undefine '%s'", args.data[0].text);
            else
                new_macro(c, args.data[0].text)->undefined = true;
        } else if (is(directive, "include") || is(directive, "include_next")) {
            bool next = is(directive, "include_next");
            if (next)
                extension(c, loc, "#include_next");
            if (!c->failed)
                include_file(c, args, loc, result, depth, next);
        } else if (is(directive, "error")) {
            error(c, loc, "#error %s", joined(c, args, false));
        } else if (is(directive, "warning")) {
            if (c->dialect.standard < C23)
                extension(c, loc, "#warning");
            loc.system_header = false;
            if (!c->failed)
                warning_at(c, WARNING_CPP, loc, "#warning %s", joined(c, args, false));
        } else if (is(directive, "pragma")) {
            pragma(c, path, args, loc, raw.data[end].loc.source_offset + 1, result, false);
        } else if (is(directive, "line") || directive.kind == NUMBER) {
            if (directive.kind == NUMBER)
                args = tail(line, 1);
            else
                args = expand(c, args, 0);
            char *finish = NULL;
            unsigned long n = args.len ? strtoul(args.data[0].text, &finish, 10) : 0;
            if (!n || n > INT32_MAX || !finish || *finish) {
                error(c, loc, "invalid #line directive");
            } else {
                line_delta = (int64_t)n - physical_line - 1;
                if (args.len > 1 && args.data[1].kind == STRING) {
                    const char *t = args.data[1].text;
                    logical_file = copy(c, t + 1, strlen(t) - 2);
                }
                if (directive.kind == NUMBER) {
                    line_marker_system = false;
                    for (size_t i = 2; i < args.len; i++)
                        if (at(args, i, "3"))
                            line_marker_system = true;
                }
            }
        } else {
            error(c, loc, "unsupported preprocessing directive #%s", directive.text);
        }
    }
    if (branches.len && !c->failed)
        error(c, branches.data[branches.len - 1].loc, "unterminated conditional directive");
    flush(c, &pending, result);
    /* Macro expansion can execute _Pragma after scanning the whole file. */
    for (size_t i = 0; i < splice_warnings.len; i++) {
        Loc loc = splice_warnings.data[i];
        loc.system_header |= loc.source_offset >= c->system_header_offset;
        warning(c, loc, "backslash and newline separated by whitespace");
    }
    return !c->failed;
}

static void command_define(Context *c, const char *spec)
{
    const char *sep = strchr(spec, '=');
    const char *line = sep ? format(c, "%.*s %s", (int)(sep - spec), spec, sep + 1)
                           : format(c, "%s 1", spec);
    Tokens tokens = {0};
    if (scan(c, "<command-line>", line, strlen(line), &tokens, NULL)) {
        tokens.len--;
        define(c, tokens, (Loc){.file = "<command-line>", .line = 1, .column = 1});
    }
}

static void interchange_format_macros(Context *c, const char *name, const char *suffix, bool single)
{
    static const struct { const char *key, *binary32, *binary64; bool literal; } limits[] = {
        {"MANT_DIG", "24", "53", false}, {"DIG", "6", "15", false}, {"DECIMAL_DIG", "9", "17", false},
        {"MIN_EXP", "(-125)", "(-1021)", false}, {"MAX_EXP", "128", "1024", false},
        {"MIN_10_EXP", "(-37)", "(-307)", false}, {"MAX_10_EXP", "38", "308", false},
        {"MIN", "0x1p-126", "0x1p-1022", true},
        {"DENORM_MIN", "0x1p-149", "0x1p-1074", true},
        {"MAX", "0x1.fffffep127", "0x1.fffffffffffffp1023", true},
        {"NORM_MAX", "0x1.fffffep127", "0x1.fffffffffffffp1023", true},
        {"EPSILON", "0x1p-23", "0x1p-52", true},
        {"HAS_DENORM", "1", "1", false}, {"HAS_INFINITY", "1", "1", false},
        {"HAS_QUIET_NAN", "1", "1", false}, {"IS_IEC_60559", "1", "1", false},
    };
    for (size_t i = 0; i < sizeof limits / sizeof *limits; i++)
        command_define(c, format(c, "__FLT%s_%s__=%s%s", name, limits[i].key,
                                 single ? limits[i].binary32 : limits[i].binary64,
                                 limits[i].literal ? suffix : ""));
}

bool preprocess(Context *c, const char *path, FILE *input, Tokens *result)
{
    time_t now = time(NULL);
    struct tm date;
    char date_buffer[32] = "??? ?? ????", time_buffer[16] = "??:??:??";
    if (localtime_r(&now, &date)) {
        static const char *const months[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        snprintf(date_buffer, sizeof date_buffer, "%s %2d %04d", months[date.tm_mon],
                 date.tm_mday, date.tm_year + 1900);
        strftime(time_buffer, sizeof time_buffer, "%H:%M:%S", &date);
    }
    c->date_macro = quoted(c, date_buffer);
    c->time_macro = quoted(c, time_buffer);
    static const char *const builtins[] = {
        "__STDC__=1", "__MCC__=1",
        "__STDC_NO_VLA__=1",
        "__HAVE_SPECULATION_SAFE_VALUE=1",
        "__ATOMIC_RELAXED=0", "__ATOMIC_CONSUME=1", "__ATOMIC_ACQUIRE=2",
        "__ATOMIC_RELEASE=3", "__ATOMIC_ACQ_REL=4", "__ATOMIC_SEQ_CST=5",
        "__linux__=1", "__unix__=1", "__LP64__=1", "__SIZEOF_POINTER__=8", "__BIGGEST_ALIGNMENT__=16",
        "__SIZEOF_INT128__=16",
        "__DECIMAL_BID_FORMAT__=1", "__DEC_EVAL_METHOD__=0",
        "__DEC32_MANT_DIG__=7", "__DEC32_MIN_EXP__=(-94)", "__DEC32_MAX_EXP__=97",
        "__DEC32_MIN__=1E-95DF", "__DEC32_MAX__=9.999999E96DF",
        "__DEC32_EPSILON__=1E-6DF", "__DEC32_SUBNORMAL_MIN__=1E-101DF",
        "__DEC64_MANT_DIG__=16", "__DEC64_MIN_EXP__=(-382)", "__DEC64_MAX_EXP__=385",
        "__DEC64_MIN__=1E-383DD", "__DEC64_MAX__=9.999999999999999E384DD",
        "__DEC64_EPSILON__=1E-15DD", "__DEC64_SUBNORMAL_MIN__=1E-398DD",
        "__DEC128_MANT_DIG__=34", "__DEC128_MIN_EXP__=(-6142)", "__DEC128_MAX_EXP__=6145",
        "__DEC128_MIN__=1E-6143DL", "__DEC128_MAX__=9.999999999999999999999999999999999E6144DL",
        "__DEC128_EPSILON__=1E-33DL", "__DEC128_SUBNORMAL_MIN__=1E-6176DL",
        "__FLT16_MANT_DIG__=11", "__FLT16_DIG__=3", "__FLT16_DECIMAL_DIG__=5",
        "__FLT16_MIN_EXP__=(-13)", "__FLT16_MAX_EXP__=16",
        "__FLT16_MIN_10_EXP__=(-4)", "__FLT16_MAX_10_EXP__=4",
        "__FLT16_MIN__=0x1p-14F16", "__FLT16_DENORM_MIN__=0x1p-24F16",
        "__FLT16_MAX__=0x1.ffcp15F16", "__FLT16_NORM_MAX__=0x1.ffcp15F16",
        "__FLT16_EPSILON__=0x1p-10F16", "__FLT16_HAS_DENORM__=1",
        "__FLT16_HAS_INFINITY__=1", "__FLT16_HAS_QUIET_NAN__=1", "__FLT16_IS_IEC_60559__=1",
        "__BFLT16_MANT_DIG__=8", "__BFLT16_DIG__=2", "__BFLT16_DECIMAL_DIG__=4",
        "__BFLT16_MIN_EXP__=(-125)", "__BFLT16_MAX_EXP__=128",
        "__BFLT16_MIN_10_EXP__=(-37)", "__BFLT16_MAX_10_EXP__=38",
        "__BFLT16_MIN__=0x1p-126BF16", "__BFLT16_DENORM_MIN__=0x1p-133BF16",
        "__BFLT16_MAX__=0x1.fep127BF16", "__BFLT16_NORM_MAX__=0x1.fep127BF16",
        "__BFLT16_EPSILON__=0x1p-7BF16", "__BFLT16_HAS_DENORM__=1",
        "__BFLT16_HAS_INFINITY__=1", "__BFLT16_HAS_QUIET_NAN__=1", "__BFLT16_IS_IEC_60559__=0",
        "__FLT128_MANT_DIG__=113", "__FLT128_DIG__=33", "__FLT128_DECIMAL_DIG__=36",
        "__FLT128_MIN_EXP__=(-16381)", "__FLT128_MAX_EXP__=16384",
        "__FLT128_MIN_10_EXP__=(-4931)", "__FLT128_MAX_10_EXP__=4932",
        "__FLT128_MIN__=0x1p-16382F128", "__FLT128_DENORM_MIN__=0x1p-16494F128",
        "__FLT128_MAX__=0x1.ffffffffffffffffffffffffffffp16383F128",
        "__FLT128_NORM_MAX__=__FLT128_MAX__", "__FLT128_EPSILON__=0x1p-112F128",
        "__FLT128_HAS_DENORM__=1", "__FLT128_HAS_INFINITY__=1",
        "__FLT128_HAS_QUIET_NAN__=1", "__FLT128_IS_IEC_60559__=1",
        "__FLT64X_MIN_EXP__=(-16381)", "__FLT64X_MAX_EXP__=16384",
        "__FLT64X_MIN_10_EXP__=(-4931)", "__FLT64X_MAX_10_EXP__=4932",
        "__FLT64X_MIN__=0x1p-16382F64x", "__FLT64X_HAS_DENORM__=1",
        "__FLT64X_HAS_INFINITY__=1", "__FLT64X_HAS_QUIET_NAN__=1", "__FLT64X_IS_IEC_60559__=1"
    };
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
        command_define(c, builtins[i]);
    interchange_format_macros(c, "32", "F32", true);
    interchange_format_macros(c, "64", "F64", false);
    interchange_format_macros(c, "32X", "F32x", false);
    command_define(c, c->freestanding ? "__STDC_HOSTED__=0" : "__STDC_HOSTED__=1");
    static const char *const versions[] = {
        [C99] = "199901L", [C11] = "201112L", [C17] = "201710L",
        [C23] = "202311L", [C2Y] = "202500L",
    };
    if (c->dialect.standard != C90)
        command_define(c, format(c, "__STDC_VERSION__=%s", versions[c->dialect.standard]));
    command_define(c, c->dialect.gnu_inline ? "__GNUC_GNU_INLINE__=1" : "__GNUC_STDC_INLINE__=1");
    if (c->target == &aarch64_linux) {
        command_define(c, "__aarch64__=1");
        command_define(c, "__AARCH64EL__=1");
        command_define(c, "__ARM_FP16_FORMAT_IEEE=1");
        command_define(c, "__ARM_FP16_ARGS=1");
        command_define(c, "__LDBL_MANT_DIG__=113");
        command_define(c, "__FLT64X_MANT_DIG__=113");
        command_define(c, "__FLT64X_DIG__=33");
        command_define(c, "__FLT64X_DECIMAL_DIG__=36");
        command_define(c, "__FLT64X_MAX__=0x1.ffffffffffffffffffffffffffffp16383F64x");
        command_define(c, "__FLT64X_NORM_MAX__=__FLT64X_MAX__");
        command_define(c, "__FLT64X_EPSILON__=0x1p-112F64x");
        command_define(c, "__FLT64X_DENORM_MIN__=0x1p-16494F64x");
    } else {
        command_define(c, "__x86_64__=1");
        command_define(c, "__amd64__=1");
        command_define(c, "__SIZEOF_FLOAT128__=16");
        command_define(c, "__SIZEOF_FLOAT80__=16");
        command_define(c, "__LDBL_MANT_DIG__=64");
        command_define(c, "__FLT64X_MANT_DIG__=64");
        command_define(c, "__FLT64X_DIG__=18");
        command_define(c, "__FLT64X_DECIMAL_DIG__=21");
        command_define(c, "__FLT64X_MAX__=0x1.fffffffffffffffep16383F64x");
        command_define(c, "__FLT64X_NORM_MAX__=__FLT64X_MAX__");
        command_define(c, "__FLT64X_EPSILON__=0x1p-63F64x");
        command_define(c, "__FLT64X_DENORM_MIN__=0x1p-16445F64x");
    }
    if (c->target->char_unsigned)
        command_define(c, "__CHAR_UNSIGNED__=1");
    command_define(c, c->target->wchar_unsigned ? "__WCHAR_TYPE__=unsigned int" : "__WCHAR_TYPE__=int");
    if (!c->dialect.gnu)
        command_define(c, "__STRICT_ANSI__=1");
    for (size_t i = 0; i < c->macro_options.len; i++) {
        const char *option = c->macro_options.data[i];
        if (option[0] == 'D')
            command_define(c, option + 1);
        else
            new_macro(c, option + 1)->undefined = true;
    }
    c->include_path = path;
    c->include_index = SIZE_MAX;
    return !c->failed && read_source(c, path, input, result, 0);
}

void write_tokens(Context *c, FILE *out, Tokens tokens, bool markers)
{
    const char *file = NULL;
    unsigned line = 0;
    bool system = false, after_pragma = false, line_start = true;
    for (size_t i = 0; i < tokens.len; i++) {
        Token t = tokens.data[i];
        bool pragma = t.kind == PRAGMA_PACK || t.kind == PRAGMA_WEAK || t.kind == PRAGMA_VISIBILITY ||
                      t.kind == PRAGMA_DIAGNOSTIC || t.kind == PRAGMA_MESSAGE || t.kind == PRAGMA_UNKNOWN;
        bool same_file = i && equal(file, t.loc.file);
        bool marker = markers && (pragma || after_pragma ||
                                   ((system || t.loc.system_header) &&
                                    (!same_file || system != t.loc.system_header ||
                                     (t.loc.line != line && t.loc.line != line + 1))));
        if (marker) {
            if (!line_start)
                fputc('\n', out);
            fprintf(out, "# %u %s%s\n", t.loc.line, quoted(c, t.loc.file),
                    t.loc.system_header ? " 3" : "");
        } else if (!line_start) {
            fputc(pragma || t.loc.line != line || !same_file ? '\n' : ' ', out);
        }
        if (pragma) {
            if (t.kind == PRAGMA_MESSAGE)
                fprintf(out, "#pragma message %s\n", quoted(c, t.text));
            else
                fprintf(out, "#pragma %s\n", t.text);
            line_start = true;
        } else {
            fputs(t.text, out);
            line_start = false;
        }
        file = t.loc.file;
        line = t.loc.line;
        system = t.loc.system_header;
        after_pragma = pragma;
    }
    if (!line_start)
        fputc('\n', out);
}
