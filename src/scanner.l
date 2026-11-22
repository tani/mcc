%top{
#define _POSIX_C_SOURCE 200809L
#include "compiler.h"
#include <limits.h>
#include <string.h>

typedef struct { unsigned line, column; } Position;
typedef struct {
    Context *context;
    Tokens *tokens;
    const char *file;
    Position *positions;
    size_t offset;
    Loc loc;
    bool space;
} Scanner;

static void matched(Scanner *, size_t);
static void token(Scanner *, TokenKind, const char *, size_t);
#define YY_USER_ACTION matched(yyextra, (size_t)yyleng);
}

%option reentrant noyywrap nounput noinput nodefault prefix="pplex"
%option extra-type="Scanner *"
%x COMMENT

ID      [a-zA-Z_$\200-\377][a-zA-Z0-9_$\200-\377]*
NUM     ([0-9]|\.[0-9])([a-zA-Z0-9_.]|[eEpP][+-])*

%%
[ \t\v\f]+                     { yyextra->space = true; }
\n                             { token(yyextra, NEWLINE, yytext, yyleng); }
"//"[^\n]*                     { yyextra->space = true; }
"/*"                           { yyextra->space = true; BEGIN(COMMENT); }
<COMMENT>"*/"                  { yyextra->space = true; BEGIN(INITIAL); }
<COMMENT>\n                    { token(yyextra, NEWLINE, yytext, yyleng); }
<COMMENT>[^*\n]+                { }
<COMMENT>"*"                   { }
<COMMENT><<EOF>>                { error(yyextra->context, yyextra->loc, "unterminated comment"); return 0; }
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
    s->loc = (Loc){s->file, start.line, start.column, end.line, end.column};
    s->offset += len;
}

static void token(Scanner *s, TokenKind kind, const char *text, size_t len)
{
    Token value = {kind, copy(s->context, text, len), s->loc, s->space, NULL};
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

bool scan(Context *c, const char *file, const char *input, size_t size, Tokens *result)
{
    if (size >= INT_MAX) {
        error(c, (Loc){.file = file}, "source is too large");
        return false;
    }
    char *source = alloc(c, size + 2);
    Position *positions = alloc(c, (size + 2) * sizeof *positions);
    unsigned line = 1, column = 1;
    size_t used = 0;
    for (size_t i = 0; i < size;) {
        unsigned char ch = input[i];
        size_t width = 1;
        if (!c->gnu && ch == '?' && size - i >= 3 && input[i + 1] == '?') {
            static const char keys[] = "=/\'()!<>-";
            static const char replacements[] = "#\\^[]|{}~";
            const char *key = input[i + 2] ? strchr(keys, input[i + 2]) : NULL;
            if (key) {
                ch = replacements[key - keys];
                width = 3;
            }
        }
        if (ch == '\\' && i + width < size) {
            size_t next = i + width;
            if (input[next] == '\r')
                next++;
            if (next < size && input[next] == '\n') {
                i = next + 1;
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
            error(c, (Loc){file, line, column, line, column}, "NUL in source");
            return false;
        }
        positions[used] = (Position){line, column};
        source[used++] = (char)ch;
        i += width;
        if (ch == '\n') {
            line++;
            column = 1;
        } else {
            column += (unsigned)width;
        }
    }
    if (!used || source[used - 1] != '\n') {
        positions[used] = (Position){line, column};
        source[used++] = '\n';
    }
    Scanner state = {.context = c, .tokens = result, .file = file, .positions = positions};
    yyscan_t scanner;
    if (yylex_init_extra(&state, &scanner)) {
        error(c, (Loc){.file = file}, "cannot initialize lexer");
        return false;
    }
    YY_BUFFER_STATE buffer = yy_scan_bytes(source, (int)used, scanner);
    yylex(scanner);
    yy_delete_buffer(buffer, scanner);
    yylex_destroy(scanner);
    return !c->failed;
}
