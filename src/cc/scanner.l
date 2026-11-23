%top{
#define _POSIX_C_SOURCE 200809L
#include "compiler.h"
#include <limits.h>
#include <regex.h>
#include <string.h>

typedef struct { unsigned line, column, offset; } Position;
typedef VECTOR(Position) Positions;
typedef struct {
    Context *context;
    Tokens *tokens;
    Locations *splice_warnings;
    const char *file, *input;
    char *source;
    size_t size;
    Position *positions;
    Positions splices;
    size_t next_splice;
    size_t offset, comment_start;
    Loc loc;
    size_t raw_end;
    const char *raw_text;
    Loc raw_start;
    bool space;
    unsigned char fallthrough_comment;
} Scanner;

static void matched(Scanner *, size_t);
static void record_token(Scanner *, TokenKind, const char *);
static void token(Scanner *, TokenKind, const char *, int);
static bool start_raw_string(Scanner *, const char *, size_t);
static void comment(Scanner *, const char *);
static void block_comment(Scanner *, size_t);
#define YY_USER_ACTION matched(yyextra, (size_t)yyleng);
}

%option reentrant noyywrap nounput noinput nodefault prefix="pplex"
%option extra-type="Scanner *"
%x COMMENT RAW_STRING

ID      [a-zA-Z_$\200-\377][a-zA-Z0-9_$\200-\377]*
NUM     ([0-9]|\.[0-9])([a-zA-Z0-9_.]|[eEpP][+-])*

%%
[ \t\v\f]+                     { yyextra->space = true; }
\n                             { token(yyextra, NEWLINE, yytext, yyleng); }
"//"[^\n]*                     { comment(yyextra, yytext + 2); }
"/*"                           { yyextra->comment_start = yyextra->offset; BEGIN(COMMENT); }
<COMMENT>"*/"                  { block_comment(yyextra, (size_t)yyleng); BEGIN(INITIAL); }
<COMMENT>\n                    { token(yyextra, NEWLINE, yytext, yyleng); }
<COMMENT>[^*\n]+                { }
<COMMENT>"*"                   { }
<COMMENT><<EOF>>                { error(yyextra->context, yyextra->loc, "unterminated comment"); return 0; }
(u8|[LuU])?R\"                 {
                                  Context *c = yyextra->context;
                                  if (c->dialect.gnu && c->dialect.standard >= C99) {
                                      if (!start_raw_string(yyextra, yytext, (size_t)yyleng))
                                          return 0;
                                      BEGIN(RAW_STRING);
                                  } else {
                                      int prefix = yyleng - 1;
                                      yyextra->offset -= (size_t)yyleng;
                                      yyless(prefix);
                                      matched(yyextra, (size_t)prefix);
                                      token(yyextra, WORD, yytext, prefix);
                                  }
                               }
<RAW_STRING>\"                 {
                                  if (yyextra->positions[yyextra->offset - 1].offset == yyextra->raw_end) {
                                      yyextra->loc.line = yyextra->raw_start.line;
                                      yyextra->loc.column = yyextra->raw_start.column;
                                      yyextra->loc.source_offset = yyextra->raw_start.source_offset;
                                      record_token(yyextra, STRING, yyextra->raw_text);
                                      yyextra->raw_end = 0;
                                      BEGIN(INITIAL);
                                  }
                               }
<RAW_STRING>[^"\n]+|\n         { }
<RAW_STRING><<EOF>>            { error(yyextra->context, yyextra->raw_start, "unterminated raw string"); return 0; }
(u8|[LuU])?\"([^"\\\n]|\\.)*\"   { token(yyextra, STRING, yytext, yyleng); }
[LuU]?\'([^'\\\n]|\\.)*\'       { token(yyextra, CHARACTER, yytext, yyleng); }
{ID}                           { token(yyextra, WORD, yytext, yyleng); }
{NUM}                          { token(yyextra, NUMBER, yytext, yyleng); }
"%:%:"|"..."|"<<="|">>="|"##"|"++"|"--"|"->"|"&&"|"||"|"<<"|">>"|"=="|"!="|"<="|">="|"+="|"-="|"*="|"/="|"%="|"&="|"|="|"^="|"<:"|":>"|"<%"|"%>"|"%:" {
                                  token(yyextra, PUNCT, yytext, yyleng);
                               }
[][{}(),.;:~!?+*/%&|^<>=#-]      { token(yyextra, PUNCT, yytext, yyleng); }
.                              { error(yyextra->context, yyextra->loc, "invalid preprocessing character '%.*s'", yyleng, yytext); }
%%

static void matched(Scanner *s, size_t len)
{
    Position start = s->positions[s->offset];
    Position end = s->positions[s->offset + len - 1];
    while (s->next_splice < s->splices.len && s->splices.data[s->next_splice].offset < end.offset) {
        Position splice = s->splices.data[s->next_splice++];
        if (!s->raw_end && s->splice_warnings) {
            Loc loc = {.file = s->file, .line = splice.line, .column = splice.column,
                       .end_line = splice.line, .end_column = splice.column,
                       .system_header = s->context->in_system_header, .source_offset = splice.offset};
            PUSH(s->context, *s->splice_warnings, loc);
        }
    }
    s->loc = (Loc){.file = s->file, .line = start.line, .column = start.column,
                   .end_line = end.line, .end_column = end.column,
                   .system_header = s->context->in_system_header, .source_offset = start.offset};
    s->offset += len;
}

static void record_token(Scanner *s, TokenKind kind, const char *text)
{
    Token value = {.kind = kind, .text = text, .loc = s->loc, .space = s->space};
    if (kind != NEWLINE) {
        value.loc.fallthrough_comment = s->fallthrough_comment;
        s->fallthrough_comment = 0;
    }
    if (kind == PUNCT) {
        static const char *const from[] = {"<:", ":>", "<%", "%>", "%:", "%:%:"};
        static const char *const to[] = {"[", "]", "{", "}", "#", "##"};
        for (size_t i = 0; i < sizeof from / sizeof *from; i++)
            if (equal(value.text, from[i]))
                value.text = to[i];
    }
    PUSH(s->context, *s->tokens, value);
    s->space = kind == NEWLINE;
}

static void token(Scanner *s, TokenKind kind, const char *text, int len)
{
    record_token(s, kind, copy(s->context, text, (size_t)len));
}

static unsigned char comment_level(const char *text)
{
    static regex_t permissive, conventional, uppercase;
    static bool ready;
    if (!ready) {
        /* Spelling conventions for levels 2 through 4 of -Wimplicit-fallthrough. */
        regcomp(&permissive, "falls?[ \t-]*thr(ough|u)", REG_EXTENDED | REG_NOSUB | REG_ICASE);
        regcomp(&conventional,
                "^[ \t.!]*((ELSE,? |INTENTIONAL(LY)? )?FALL(S |[ -])?THR(OUGH|U)"
                "|(Else,? |Intentional(ly)? )?Fall(thr(ough|u)|(s |[ -])[Tt]hr(ough|u))"
                "|([Ee]lse,? |[Ii]ntentional(ly)? )?fall(s |[ -])?thr(ough|u))"
                "[ \t.!]*(-[^\n\r]*)?$", REG_EXTENDED | REG_NOSUB);
        regcomp(&uppercase, "^[ \t]*FALLTHR(OUGH|U)[ \t]*$", REG_EXTENDED | REG_NOSUB);
        ready = true;
    }
    const char *lint = "lint -fallthrough";
    if (equal(text, "-fallthrough") || equal(text, "@fallthrough@") ||
        (!strncmp(text, lint, strlen(lint)) && !text[strlen(lint) + strspn(text + strlen(lint), " \t")]) ||
        !regexec(&uppercase, text, 0, NULL, 0))
        return 4;
    if (!regexec(&conventional, text, 0, NULL, 0))
        return 3;
    return regexec(&permissive, text, 0, NULL, 0) ? 1 : 2;
}

static void comment(Scanner *s, const char *text)
{
    unsigned char level = comment_level(text);
    if (level > s->fallthrough_comment)
        s->fallthrough_comment = level;
    s->space = true;
}

static void block_comment(Scanner *s, size_t close_length)
{
    char *end = s->source + s->offset - close_length;
    char saved = *end;
    *end = 0;
    comment(s, s->source + s->comment_start);
    *end = saved;
}

static bool start_raw_string(Scanner *s, const char *prefix, size_t prefix_length)
{
    static const char delimiter_chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_{}[]#<>%:;.?*+-/^&|~!=,\"'";
    const char *delimiter = s->input + s->positions[s->offset - 1].offset + 1;
    const char *end = s->input + s->size;
    const char *body = delimiter;
    while (body < end && *body != '(') {
        if (body - delimiter == 16 || !*body || !strchr(delimiter_chars, *body)) {
            error(s->context, s->loc, "invalid raw string delimiter");
            return false;
        }
        body++;
    }
    if (body == end) {
        error(s->context, s->loc, "unterminated raw string");
        return false;
    }
    size_t length = (size_t)(body - delimiter);
    for (const char *close = body + 1; (size_t)(end - close) >= length + 2; close++) {
        if (*close != ')' || close[length + 1] != '"' || memcmp(close + 1, delimiter, length))
            continue;
        s->raw_end = (size_t)(close + length + 1 - s->input);
        s->raw_start = s->loc;
        /* Restore physical spelling after the opening quote, including spliced newlines. */
        const char *stop = close + length + 2;
        char *text = alloc(s->context, prefix_length + (size_t)(stop - delimiter) + 1);
        memcpy(text, prefix, prefix_length);
        char *out = text + prefix_length;
        for (const char *p = delimiter; p < stop; p++) {
            if (*p == '\r') {
                *out++ = '\n';
                if (p + 1 < stop && p[1] == '\n')
                    p++;
            } else {
                *out++ = *p;
            }
        }
        *out = 0;
        s->raw_text = text;
        return true;
    }
    error(s->context, s->loc, "unterminated raw string");
    return false;
}

bool scan(Context *c, const char *file, const char *input, size_t size, Tokens *result,
          Locations *splice_warnings)
{
    if (size > INT_MAX - 3) {
        error(c, (Loc){.file = file}, "source is too large");
        return false;
    }
    char *source = alloc(c, size + 3);
    Position *positions = alloc(c, (size + 2) * sizeof *positions);
    Positions splices = {0};
    unsigned line = 1, column = 1;
    size_t used = 0;
    for (size_t i = 0; i < size;) {
        unsigned char ch = (unsigned char)input[i];
        size_t width = 1;
        if (!c->dialect.gnu && c->dialect.standard < C23 &&
            ch == '?' && size - i >= 3 && input[i + 1] == '?') {
            static const char keys[] = "=/\'()!<>-";
            static const char replacements[] = "#\\^[]|{}~";
            const char *key = input[i + 2] ? strchr(keys, input[i + 2]) : NULL;
            if (key) {
                ch = (unsigned char)replacements[key - keys];
                width = 3;
            }
        }
        if (ch == '\\' && i + width < size) {
            size_t next = i + width;
            while (next < size && (input[next] == ' ' || input[next] == '\t' ||
                   input[next] == '\v' || input[next] == '\f'))
                next++;
            if (next < size && (input[next] == '\r' || input[next] == '\n')) {
                if (splice_warnings && next != i + width)
                    PUSH(c, splices, ((Position){line, column, (unsigned)i}));
                i = next + 1;
                if (input[next] == '\r' && i < size && input[i] == '\n')
                    i++;
                line++;
                column = 1;
                continue;
            }
        }
        if (ch == '\r') {
            ch = '\n';
            if (i + 1 < size && input[i + 1] == '\n')
                width++;
        }
        if (!ch) {
            error(c, (Loc){.file = file, .line = line, .column = column},
                  "NUL in source");
            return false;
        }
        positions[used] = (Position){line, column, (unsigned)i};
        source[used++] = (char)ch;
        i += width;
        if (ch == '\n') {
            line++;
            column = 1;
        } else {
            column += (unsigned)width;
        }
    }
    if (!used || source[used - 1] != '\n' || positions[used - 1].line + 1 < line) {
        positions[used] = (Position){line, column, (unsigned)size};
        source[used++] = '\n';
    }
    Scanner state = {.context = c, .tokens = result, .file = file, .input = input, .source = source, .size = size,
                     .positions = positions, .splices = splices, .splice_warnings = splice_warnings};
    yyscan_t scanner;
    if (yylex_init_extra(&state, &scanner)) {
        error(c, (Loc){.file = file}, "cannot initialize lexer");
        return false;
    }
    source[used] = source[used + 1] = 0;
    YY_BUFFER_STATE buffer = yy_scan_buffer(source, used + 2, scanner);
    if (!buffer) {
        error(c, (Loc){.file = file}, "cannot initialize lexer buffer");
        yylex_destroy(scanner);
        return false;
    }
    yylex(scanner);
    yy_delete_buffer(buffer, scanner);
    yylex_destroy(scanner);
    return !c->failed;
}
