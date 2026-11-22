#define _XOPEN_SOURCE 700
#include "compiler.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Macro {
    Macro *next;
    const char *name;
    Strings params;
    Tokens body;
    bool function, variadic, undefined;
};

struct Hide {
    const Hide *next;
    const Macro *macro;
};

struct SavedMacro {
    SavedMacro *next;
    const char *name;
    Macro *definition;
    bool used;
};

typedef struct Work Work;
struct Work { Work *next; Token token; };
typedef VECTOR(Tokens) Arguments;

typedef struct {
    bool parent, taken, active, otherwise;
    Loc loc;
} Branch;
typedef VECTOR(Branch) Branches;

static bool is(Token token, const char *text)
{
    return equal(token.text, text);
}

static Macro *lookup(Context *c, const char *name)
{
    for (Macro *m = c->macros; m; m = m->next)
        if (equal(m->name, name))
            return m->undefined ? NULL : m;
    return NULL;
}

static bool predefined(Context *c, const char *name)
{
    return equal(name, "__LINE__") || equal(name, "__FILE__") ||
           equal(name, "__DATE__") || equal(name, "__TIME__") ||
           (c->gnu && equal(name, "__COUNTER__"));
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
        if (is(t, ")") && !depth) {
            if (current.len || args->len || m->params.len)
                PUSH(c, *args, current);
            *rest = p->next;
            invocation->hide = intersection(c, invocation->hide, t.hide);
            bool omitted = m->variadic && args->len + 1 == m->params.len;
            if (omitted && c->gnu)
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

static Token pasted(Context *c, Token a, Token b, Loc loc)
{
    const char *text = format(c, "%s%s", a.text, b.text);
    Tokens tokens = {0};
    scan(c, loc.file, text, strlen(text), &tokens);
    if (tokens.len != 2 || tokens.data[0].kind == NEWLINE) {
        error(c, loc, "'%s' and '%s' do not form one preprocessing token", a.text, b.text);
        return a;
    }
    Token t = tokens.data[0];
    t.loc = loc;
    t.space = a.space;
    t.hide = intersection(c, a.hide, b.hide);
    return t;
}

static Tokens substitute(Context *c, Macro *m, Arguments args, Token invocation, unsigned depth)
{
    Tokens result = {0};
    bool paste = false, has_left = false;
    for (size_t i = 0; i < m->body.len; i++) {
        Token t = m->body.data[i];
        if (is(t, "##")) {
            paste = true;
            continue;
        }
        size_t param = parameter(m, t);
        Tokens part = {0};
        if (m->function && is(t, "#") && i + 1 < m->body.len &&
            (param = parameter(m, m->body.data[i + 1])) != SIZE_MAX) {
            t.kind = STRING;
            t.text = quoted(c, joined(c, args.data[param], true));
            i++;
            PUSH(c, part, t);
        } else if (param != SIZE_MAX) {
            bool raw = paste || (i + 1 < m->body.len && is(m->body.data[i + 1], "##"));
            part = raw ? args.data[param] : expand(c, args.data[param], depth + 1);
        } else {
            PUSH(c, part, t);
        }
        bool comma_pack = c->gnu && paste && m->variadic && param != SIZE_MAX &&
                          param + 1 == m->params.len && has_left &&
                          is(result.data[result.len - 1], ",");
        size_t first = 0;
        if (comma_pack && !part.len) {
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
        has_left = part.len || (paste && has_left && !comma_pack);
        paste = false;
    }
    for (size_t i = 0; i < result.len; i++) {
        Token *t = &result.data[i];
        t->hide = hide(c, t->hide, m);
        for (const Hide *h = invocation.hide; h; h = h->next)
            t->hide = hide(c, t->hide, h->macro);
        t->loc = invocation.loc;
    }
    if (result.len)
        result.data[0].space = invocation.space;
    return result;
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
        if (t.kind == WORD) {
            Macro *m = lookup(c, t.text);
            if (m && !hidden(t.hide, m) && (!m->function || (p && is(p->token, "(")))) {
                Arguments args = {0};
                if (m->function && !arguments(c, m, &p, &args, &t))
                    break;
                p = work(c, substitute(c, m, args, t, depth), p);
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
            } else if (c->gnu && is(t, "__COUNTER__")) {
                t.kind = NUMBER;
                t.text = format(c, "%u", c->unique++);
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
                if (c->gnu && at(line, i, "...")) {
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
    if (at(m->body, 0, "##") || (m->body.len && at(m->body, m->body.len - 1, "##")))
        error(c, loc, "'##' cannot appear at either end of a macro replacement");
    for (size_t j = 0; m->function && j < m->body.len; j++) {
        if (at(m->body, j, "#") && (j + 1 == m->body.len ||
            parameter(m, m->body.data[j + 1]) == SIZE_MAX))
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
        return 0;
    if (t.kind == CHARACTER) {
        Node *n = literal(p->context, t);
        return n ? n->integer : 0;
    }
    if (t.kind == NUMBER) {
        char *end;
        errno = 0;
        uint64_t value = strtoull(t.text, &end, 0);
        if (end != t.text && !errno && end[strspn(end, "uUlL")] == 0)
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
            bool paren = at(args, i + 1, "(");
            i += paren ? 2 : 1;
            if (i >= args.len || args.data[i].kind != WORD) {
                error(c, t.loc, "expected identifier after 'defined'");
                return false;
            }
            t.kind = NUMBER;
            t.text = defined(c, args.data[i].text) ? "1" : "0";
            if (paren && !at(args, ++i, ")")) {
                error(c, t.loc, "expected ')' after 'defined'");
                return false;
            }
        }
        PUSH(c, input, t);
    }
    IfParser p = {c, expand(c, input, 0), 0, loc};
    uint64_t output = if_expr(&p, 1, true);
    if (p.index != p.tokens.len)
        error(c, p.tokens.data[p.index].loc, "unexpected token in preprocessing expression");
    return output != 0;
}

static bool read_source(Context *c, const char *path, FILE *file, Tokens *result, unsigned depth);

static bool include_file(Context *c, const char *path, Tokens args, Loc loc, Tokens *result,
                         unsigned depth, bool next)
{
    if (!at(args, 0, "<") && !(args.len && args.data[0].kind == STRING))
        args = expand(c, args, 0);
    bool local = args.len == 1 && args.data[0].kind == STRING;
    const char *name = NULL;
    if (local) {
        const char *text = args.data[0].text;
        name = copy(c, text + 1, strlen(text) - 2);
    } else if (args.len >= 3 && at(args, 0, "<") && at(args, args.len - 1, ">")) {
        Tokens inside = tail(args, 1);
        inside.len--;
        name = joined(c, inside, true);
    }
    if (!name || !*name) {
        error(c, loc, "expected a quoted or bracketed header name");
        return false;
    }
    const char *slash = strrchr(path, '/');
    const char *directory = slash ? copy(c, path, (size_t)(slash - path)) : ".";
    bool skip = next;
    for (size_t i = 0; i <= c->includes.len + c->system_includes.len; i++) {
        if (i == 0 && (!local || next))
            continue;
        const char *dir = i == 0 ? directory : i <= c->includes.len ? c->includes.data[i - 1]
            : c->system_includes.data[i - c->includes.len - 1];
        if (skip) {
            size_t length = strlen(dir);
            if (!strncmp(path, dir, length) && path[length] == '/' &&
                equal(path + length + 1, name))
                skip = false;
            continue;
        }
        const char *candidate = name[0] == '/' ? name : format(c, "%s/%s", dir, name);
        FILE *file = fopen(candidate, "rb");
        if (file) {
            char *canonical = realpath(candidate, NULL);
            if (canonical) {
                candidate = copy(c, canonical, strlen(canonical));
                free(canonical);
            }
            bool ok = read_source(c, candidate, file, result, depth + 1);
            fclose(file);
            return ok;
        }
    }
    error(c, loc, "header '%s' not found", name);
    return false;
}

static void flush(Context *c, Tokens *pending, Tokens *out)
{
    Tokens result = expand(c, *pending, 0);
    for (size_t i = 0; i < result.len; i++)
        PUSH(c, *out, result.data[i]);
    pending->len = 0;
}

static void pragma(Context *c, const char *path, Tokens args, Loc loc)
{
    if (at(args, 0, "once")) {
        PUSH(c, c->once, path);
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
            *saved = (SavedMacro){c->saved_macros, name, lookup(c, name), false};
            c->saved_macros = saved;
        } else {
            for (SavedMacro *saved = c->saved_macros; saved; saved = saved->next)
                if (!saved->used && equal(saved->name, name)) {
                    Macro *restored = new_macro(c, name);
                    if (saved->definition) {
                        Macro *next = restored->next;
                        *restored = *saved->definition;
                        restored->next = next;
                    } else {
                        restored->undefined = true;
                    }
                    saved->used = true;
                    break;
                }
        }
    } else if (at(args, 0, "GCC") || at(args, 0, "pack") || at(args, 0, "weak") ||
               at(args, 0, "STDC")) {
        error(c, loc, "unsupported pragma %s", joined(c, args, false));
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
    if (ferror(file))
        error(c, (Loc){.file = path}, "error reading source");
    else
        scan(c, path, bytes, length, &raw);
    free(bytes);
    Branches branches = {0};
    const char *logical_file = path;
    int64_t line_delta = 0;
    for (size_t start = 0; start < raw.len && !c->failed;) {
        size_t end = start;
        while (end < raw.len && raw.data[end].kind != NEWLINE)
            end++;
        Tokens line = {.data = raw.data + start, .len = end - start};
        start = end + 1;
        if (!line.len)
            continue;
        unsigned physical_line = raw.data[end].loc.line;
        for (size_t i = 0; i < line.len; i++) {
            Loc *loc = &line.data[i].loc;
            loc->file = logical_file;
            loc->line = (unsigned)(loc->line + line_delta);
            loc->end_line = (unsigned)(loc->end_line + line_delta);
        }
        Branch *branch = branches.len ? &branches.data[branches.len - 1] : NULL;
        bool active = !branch || branch->active;
        if (!at(line, 0, "#")) {
            if (active) {
                line.data[0].space = true;
                for (size_t i = 0; i < line.len; i++)
                    PUSH(c, pending, line.data[i]);
            }
            continue;
        }
        flush(c, &pending, result);
        if (line.len == 1)
            continue;
        Token directive = line.data[1];
        Loc loc = directive.loc;
        Tokens args = tail(line, 2);
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
        } else if (is(directive, "elif") || is(directive, "else") || is(directive, "endif")) {
            if (!branch) {
                error(c, loc, "unexpected #%s", directive.text);
            } else if (is(directive, "endif")) {
                branches.len--;
            } else if (branch->otherwise) {
                error(c, loc, "#%s after #else", directive.text);
            } else {
                branch->otherwise = is(directive, "else");
                branch->active = branch->parent && !branch->taken &&
                                 (branch->otherwise || test_condition(c, args, loc));
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
                include_file(c, path, args, loc, result, depth, next);
        } else if (is(directive, "error")) {
            error(c, loc, "#error %s", joined(c, args, false));
        } else if (is(directive, "pragma")) {
            pragma(c, path, args, loc);
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
            }
        } else {
            error(c, loc, "unsupported preprocessing directive #%s", directive.text);
        }
    }
    if (branches.len && !c->failed)
        error(c, branches.data[branches.len - 1].loc, "unterminated conditional directive");
    flush(c, &pending, result);
    return !c->failed;
}

static void command_define(Context *c, const char *spec)
{
    const char *sep = strchr(spec, '=');
    const char *line = sep ? format(c, "%.*s %s", (int)(sep - spec), spec, sep + 1)
                           : format(c, "%s 1", spec);
    Tokens tokens = {0};
    if (scan(c, "<command-line>", line, strlen(line), &tokens)) {
        tokens.len--;
        define(c, tokens, (Loc){.file = "<command-line>", .line = 1, .column = 1});
    }
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
        "__STDC__=1", "__STDC_VERSION__=201112L", "__STDC_HOSTED__=1",
        "__STDC_NO_ATOMICS__=1", "__STDC_NO_VLA__=1", "__STDC_NO_COMPLEX__=1",
        "__linux__=1", "__unix__=1", "__LP64__=1", "__SIZEOF_POINTER__=8"
    };
    for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
        command_define(c, builtins[i]);
    if (c->target == &aarch64_linux) {
        command_define(c, "__aarch64__=1");
        command_define(c, "__AARCH64EL__=1");
    } else {
        command_define(c, "__x86_64__=1");
        command_define(c, "__amd64__=1");
    }
    if (c->target->char_unsigned)
        command_define(c, "__CHAR_UNSIGNED__=1");
    command_define(c, c->target->wchar_unsigned ? "__WCHAR_TYPE__=unsigned int" : "__WCHAR_TYPE__=int");
    if (!c->gnu)
        command_define(c, "__STRICT_ANSI__=1");
    for (size_t i = 0; i < c->macro_options.len; i++) {
        const char *option = c->macro_options.data[i];
        if (option[0] == 'D')
            command_define(c, option + 1);
        else
            new_macro(c, option + 1)->undefined = true;
    }
    return !c->failed && read_source(c, path, input, result, 0);
}

void write_tokens(FILE *out, Tokens tokens)
{
    const char *file = NULL;
    unsigned line = 0;
    for (size_t i = 0; i < tokens.len; i++) {
        Token t = tokens.data[i];
        if (i && (t.loc.line != line || !equal(file, t.loc.file)))
            fputc('\n', out);
        else if (i)
            fputc(' ', out);
        fputs(t.text, out);
        file = t.loc.file;
        line = t.loc.line;
    }
    if (tokens.len)
        fputc('\n', out);
}
