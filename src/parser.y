%code requires {
#include "compiler.h"
#define YYLLOC_DEFAULT(Current, Rhs, N) do { \
    (Current) = YYRHSLOC(Rhs, (N) ? 1 : 0); \
    if (N) { \
        (Current).end_line = YYRHSLOC(Rhs, N).end_line; \
        (Current).end_column = YYRHSLOC(Rhs, N).end_column; \
    } \
} while (0)
}
%code {
#include <string.h>

static int yylex(YYSTYPE *, Loc *, Context *);
static void yyerror(Loc *loc, Context *c, const char *message)
{
    error(c, *loc, "%s", message);
}
}
%define api.pure full
%define api.location.type {Loc}
%define parse.error detailed
%define parse.lac full
%locations
%param { Context *c }
%initial-action { @$ = (Loc){c->input, 1, 1, 1, 1}; }

%union {
    Token token;
    const char *name;
    unsigned flags;
    int op;
    Node *node;
    Nodes nodes;
    Type *type;
    Decl *decl;
    Specs specs;
    Members members;
    Decls decls;
    bool flag;
}

%token <token> IDENT TYPE_NAME TYPE_LABEL NUMBER_TOKEN STRING_TOKEN CHAR_TOKEN
%token VOID BOOL CHAR SHORT INT LONG FLOAT DOUBLE SIGNED UNSIGNED
%token TYPEDEF EXTERN STATIC AUTO REGISTER CONST VOLATILE RESTRICT INLINE NORETURN THREAD_LOCAL
%token STRUCT UNION ENUM
%token PACKED_ATTR
%token IF ELSE WHILE DO FOR RETURN BREAK CONTINUE SWITCH CASE DEFAULT GOTO
%token SIZEOF ALIGNOF STATIC_ASSERT GENERIC VA_ARG TYPEOF OFFSETOF
%token INC DEC ARROW AND OR SHL SHR EQ NE LE GE ELLIPSIS
%token ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token SHL_ASSIGN SHR_ASSIGN AND_ASSIGN XOR_ASSIGN OR_ASSIGN
%precedence TYPE_NAME
%precedence PREFER_TYPE
%precedence IF_WITHOUT_ELSE
%precedence ELSE

%type <name> name label_name tag_opt
%type <flags> mods mod builtin builtin_list
%type <specs> specs declaration_head
%type <decl> declarator direct_declarator pointers abstract_opt abstract_declarator direct_abstract
%type <decl> brackets parameters parameter_list member_declarator
%type <decls> member_declarators
%type <type> record enum_spec type_name typeof_spec
%type <flags> record_kind record_intro
%type <flags> quals
%type <members> member_list member_declaration
%type <node> compound block_item declaration init_declarators initialized declared initializer
%type <node> statement expression expression_opt assignment conditional logical_or logical_and
%type <node> bit_or bit_xor bit_and equality relation shift additive multiplicative cast unary
%type <node> postfix primary string_list for_init
%type <node> initializer_item designator designators generic_association offset_path offset_tail
%type <nodes> block_items arguments arguments_opt initializer_list braced_list generic_associations
%type <op> assignment_op unary_op

%%

translation_unit:
    %empty
  | translation_unit external
  ;

external:
    declaration
  | declaration_head declarator { begin_function(c, $1, $2, @2); }
    compound { end_function(c, $4); }
  | static_assertion
  ;

static_assertion:
    STATIC_ASSERT '(' conditional ',' string_list ')' ';' {
      uint64_t value;
      if (!constant_int(c, $3, &value))
        error(c, @3, "static assertion requires an integer constant expression");
      else if (!value)
        error(c, @3, "static assertion failed");
    }
  ;

declaration_head:
    specs { c->declaration = $1; $$ = $1; }
  ;

declaration:
    declaration_head init_declarators ';' { $$ = $2; }
  | declaration_head ';' { spec_type(c, $1, @1); $$ = node(c, N_SEQUENCE, @1, NULL, NULL); }
  ;

init_declarators:
    initialized { $$ = node(c, N_SEQUENCE, @1, NULL, NULL); PUSH(c, $$->list, $1); }
  | init_declarators ',' initialized { $$ = $1; PUSH(c, $$->list, $3); }
  ;

declared:
    declarator { $$ = declare(c, c->declaration, $1, NULL, @1); }
  ;

initialized:
    declared
  | declared '=' initializer { $$ = $1; set_initializer(c, $$, $3); }
  ;

initializer:
    assignment
  | braced_list { $$ = node(c, N_INIT, @1, NULL, NULL); $$->list = $1; }
  ;

braced_list:
    '{' initializer_list '}' { $$ = $2; }
  | '{' initializer_list ',' '}' { $$ = $2; }
  | '{' '}' { extension(c, @1, "empty initializer"); $$ = (Nodes){0}; }
  ;

initializer_list:
    initializer_item { $$ = (Nodes){0}; PUSH(c, $$, $1); }
  | initializer_list ',' initializer_item { $$ = $1; PUSH(c, $$, $3); }
  ;

initializer_item:
    initializer
  | designators '=' initializer { $$ = node(c, N_DESIGNATE, @1, $1, $3); }
  ;

designators:
    designator
  | designator designators { $$ = $1; $$->right = $2; }
  ;

designator:
    '.' name { $$ = node(c, N_DESIGNATE, @1, NULL, NULL); $$->name = $2; }
  | '[' conditional ']' { $$ = node(c, N_DESIGNATE, @1, $2, NULL); }
  | '[' conditional ELLIPSIS conditional ']' {
      extension(c, @3, "initializer range");
      $$ = node(c, N_DESIGNATE, @1, $2, NULL); $$->third = $4;
    }
  ;

specs:
    mods builtin_list { $$ = (Specs){$1 | $2, NULL}; }
  | mods TYPE_NAME mods { $$ = (Specs){$1 | $3, find_typedef(c, $2.text)}; }
  | mods record mods { $$ = (Specs){$1 | $3, $2}; }
  | mods enum_spec mods { $$ = (Specs){$1 | $3, $2}; }
  | mods typeof_spec mods { $$ = (Specs){$1 | $3, $2}; }
  ;

typeof_spec:
    TYPEOF '(' type_name ')' { $$ = $3; }
  | TYPEOF '(' expression ')' { $$ = $3->type; }
  ;

mods:
    %empty %prec PREFER_TYPE { $$ = 0; }
  | mods mod { $$ = $1 | $2; }
  ;

mod:
    CONST { $$ = S_CONST; }
  | VOLATILE { $$ = S_VOLATILE; }
  | RESTRICT { $$ = S_RESTRICT; }
  | INLINE { $$ = 0; }
  | NORETURN { $$ = 0; }
  | TYPEDEF { $$ = S_TYPEDEF; }
  | STATIC { $$ = S_STATIC; }
  | EXTERN { $$ = S_EXTERN; }
  | AUTO { $$ = S_AUTO; }
  | REGISTER { $$ = S_REGISTER; }
  | THREAD_LOCAL { $$ = S_THREAD; }
  | PACKED_ATTR { $$ = S_PACKED; }
  ;

builtin_list:
    builtin { $$ = $1; }
  | builtin_list builtin { $$ = merge_specs(c, (Specs){$1, NULL}, (Specs){$2, NULL}, @2).flags; }
  | builtin_list mod { $$ = $1 | $2; }
  ;

builtin:
    VOID { $$ = S_VOID; }
  | BOOL { $$ = S_BOOL; }
  | CHAR { $$ = S_CHAR; }
  | SHORT { $$ = S_SHORT; }
  | INT { $$ = S_INT; }
  | LONG { $$ = S_LONG; }
  | FLOAT { $$ = S_FLOAT; }
  | DOUBLE { $$ = S_DOUBLE; }
  | SIGNED { $$ = S_SIGNED; }
  | UNSIGNED { $$ = S_UNSIGNED; }
  ;

name:
    IDENT { $$ = $1.text; }
  | TYPE_NAME { $$ = $1.text; }
  | TYPE_LABEL { $$ = $1.text; }
  ;

label_name:
    IDENT { $$ = $1.text; }
  | TYPE_LABEL { $$ = $1.text; }
  ;

record_kind:
    STRUCT { $$ = 0; }
  | UNION { $$ = 1; }
  ;

record_intro:
    record_kind
  | record_kind PACKED_ATTR { $$ = $1 | S_PACKED; }
  ;

tag_opt:
    %empty { $$ = NULL; }
  | name
  ;

record:
    record_intro name {
      $$ = record_type(c, $1 & 1, $2, NULL, @2);
      if ($1 & S_PACKED) pack_record(c, $$, @1);
    }
  | record_intro tag_opt '{' { record_type(c, $1 & 1, $2, NULL, @2); }
    member_list '}' {
      $$ = record_type(c, $1 & 1, $2, &$5, @1);
      if ($1 & S_PACKED) pack_record(c, $$, @1);
    }
  ;

member_list:
    %empty { $$ = (Members){0}; }
  | member_list member_declaration {
      $$ = $1;
      for (size_t i = 0; i < $2.len; i++)
        PUSH(c, $$, $2.data[i]);
    }
  ;

member_declaration:
    specs member_declarators ';' {
      $$ = (Members){0};
      for (size_t i = 0; i < $2.len; i++)
        add_member(c, &$$, $1, $2.data[i], @2);
    }
  | specs ';' { $$ = (Members){0}; add_member(c, &$$, $1, NULL, @1); }
  ;

member_declarators:
    member_declarator { $$ = (Decls){0}; PUSH(c, $$, $1); }
  | member_declarators ',' member_declarator { $$ = $1; PUSH(c, $$, $3); }
  ;

member_declarator:
    declarator
  | declarator ':' conditional { $$ = $1; $$->bit_width = $3; }
  | ':' conditional { $$ = decl(c, D_NAME, NULL, @1); $$->bit_width = $2; }
  ;

enum_spec:
    ENUM name { $$ = enum_type(c, $2, false, @2); }
  | ENUM tag_opt '{' { c->next_enum = 0; c->enum_negative = false; }
    enumerators optional_comma '}' { $$ = enum_type(c, $2, true, @1); }
  ;

optional_comma:
    %empty
  | ','
  ;

enumerators:
    enumerator
  | enumerators ',' enumerator
  ;

enumerator:
    name { define_enum(c, $1, NULL, &c->next_enum, @1); }
  | name '=' conditional { define_enum(c, $1, $3, &c->next_enum, @1); }
  ;

pointers:
    '*' quals { $$ = decl(c, D_POINTER, NULL, @1); $$->qualifiers = $2; }
  | '*' quals pointers { $$ = decl(c, D_POINTER, $3, @1); $$->qualifiers = $2; }
  ;

quals:
    %empty { $$ = 0; }
  | quals CONST { $$ = $1 | S_CONST; }
  | quals VOLATILE { $$ = $1 | S_VOLATILE; }
  | quals RESTRICT { $$ = $1 | S_RESTRICT; }
  ;

declarator:
    direct_declarator
  | pointers direct_declarator {
      $$ = $1;
      Decl *end = $$;
      while (end->inner) end = end->inner;
      end->inner = $2;
    }
  ;

direct_declarator:
    name { $$ = decl(c, D_NAME, NULL, @1); $$->name = $1; }
  | '(' declarator ')' { $$ = $2; }
  | direct_declarator brackets { $$ = $2; $$->inner = $1; }
  | direct_declarator '(' parameters ')' { $$ = $3; $$->inner = $1; }
  ;

parameters:
    %empty { $$ = decl(c, D_FUNCTION, NULL, @$); }
  | parameter_list
  | parameter_list ',' ELLIPSIS { $$ = $1; $$->variadic = true; }
  ;

parameter_list:
    specs declarator {
      $$ = decl(c, D_FUNCTION, NULL, @1); $$->prototype = true;
      add_member(c, &$$->params, $1, $2, @2);
    }
  | specs abstract_opt {
      $$ = decl(c, D_FUNCTION, NULL, @1); $$->prototype = true;
      add_member(c, &$$->params, $1, $2, @2);
    }
  | parameter_list ',' specs declarator { $$ = $1; add_member(c, &$$->params, $3, $4, @4); }
  | parameter_list ',' specs abstract_opt { $$ = $1; add_member(c, &$$->params, $3, $4, @4); }
  ;

abstract_opt:
    %empty { $$ = NULL; }
  | abstract_declarator
  ;

abstract_declarator:
    direct_abstract
  | pointers
  | pointers direct_abstract {
      $$ = $1;
      Decl *end = $$;
      while (end->inner) end = end->inner;
      end->inner = $2;
    }
  ;

direct_abstract:
    '(' abstract_declarator ')' { $$ = $2; }
  | brackets
  | direct_abstract brackets { $$ = $2; $$->inner = $1; }
  | '(' parameters ')' { $$ = $2; }
  | direct_abstract '(' parameters ')' { $$ = $3; $$->inner = $1; }
  ;

brackets:
    '[' quals expression_opt ']' {
      $$ = decl(c, D_ARRAY, NULL, @1); $$->bound = $3; $$->qualifiers = $2;
    }
  | '[' STATIC quals assignment ']' {
      $$ = decl(c, D_ARRAY, NULL, @1); $$->bound = $4; $$->qualifiers = $3;
    }
  | '[' quals '*' ']' { $$ = decl(c, D_ARRAY, NULL, @1); $$->qualifiers = $2; }
  ;

type_name:
    specs abstract_opt { $$ = apply_decl(c, spec_type(c, $1, @1), $2); }
  ;

compound:
    '{' { if (c->function_body) c->function_body = false; else enter_scope(c); } block_items '}' {
      $$ = node(c, N_BLOCK, @1, NULL, NULL); $$->list = $3; leave_scope(c);
    }
  ;

block_items:
    %empty { $$ = (Nodes){0}; }
  | block_items block_item { $$ = $1; if ($2) PUSH(c, $$, $2); }
  ;

block_item:
    declaration
  | statement
  | static_assertion { $$ = NULL; }
  ;

for_init:
    expression_opt ';' { $$ = node(c, N_EXPR, @1, $1, NULL); }
  | declaration
  ;

statement:
    compound
  | expression_opt ';' { $$ = node(c, N_EXPR, @1, $1, NULL); }
  | RETURN expression_opt ';' { $$ = node(c, N_RETURN, @1, check_return(c, $2, @1), NULL); }
  | IF '(' expression ')' statement %prec IF_WITHOUT_ELSE {
      $$ = node(c, N_IF, @1, $3, $5);
    }
  | IF '(' expression ')' statement ELSE statement {
      $$ = node(c, N_IF, @1, $3, $5); $$->third = $7;
    }
  | WHILE '(' expression ')' statement { $$ = node(c, N_LOOP, @1, $3, $5); }
  | DO statement WHILE '(' expression ')' ';' { $$ = node(c, N_DO, @1, $5, $2); }
  | FOR '(' { enter_scope(c); } for_init expression_opt ';' expression_opt ')' statement {
      $$ = node(c, N_LOOP, @1, $5, $9); $$->third = $7; $$->fourth = $4; leave_scope(c);
    }
  | SWITCH '(' expression ')' statement { $$ = node(c, N_SWITCH, @1, $3, $5); }
  | CASE conditional ':' statement { $$ = node(c, N_CASE, @1, $2, $4); }
  | CASE conditional ELLIPSIS conditional ':' statement {
      extension(c, @3, "case range");
      $$ = node(c, N_CASE, @1, $2, $6); $$->third = $4;
    }
  | DEFAULT ':' statement { $$ = node(c, N_DEFAULT, @1, NULL, $3); }
  | BREAK ';' { $$ = node(c, N_BREAK, @1, NULL, NULL); }
  | CONTINUE ';' { $$ = node(c, N_CONTINUE, @1, NULL, NULL); }
  | GOTO name ';' { $$ = node(c, N_GOTO, @1, NULL, NULL); $$->name = $2; }
  | label_name ':' statement { $$ = node(c, N_LABEL, @1, $3, NULL); $$->name = $1; }
  ;

expression_opt:
    %empty { $$ = NULL; }
  | expression
  ;

expression:
    assignment
  | expression ',' assignment { $$ = node(c, N_SEQUENCE, @2, $1, $3); $$->type = $3->type; }
  ;

assignment:
    conditional
  | unary assignment_op assignment { $$ = assign(c, $2, $1, $3, false, @2); }
  ;

assignment_op:
    '=' { $$ = '='; } | ADD_ASSIGN { $$ = '+'; } | SUB_ASSIGN { $$ = '-'; }
  | MUL_ASSIGN { $$ = '*'; } | DIV_ASSIGN { $$ = '/'; } | MOD_ASSIGN { $$ = '%'; }
  | SHL_ASSIGN { $$ = SHL; } | SHR_ASSIGN { $$ = SHR; } | AND_ASSIGN { $$ = '&'; }
  | XOR_ASSIGN { $$ = '^'; } | OR_ASSIGN { $$ = '|'; }
  ;

conditional:
    logical_or
  | logical_or '?' expression ':' conditional { $$ = conditional(c, $1, $3, $5, @2); }
  | logical_or '?' ':' conditional {
      extension(c, @2, "omitted middle conditional operand");
      $$ = conditional(c, $1, NULL, $4, @2);
    }
  ;

logical_or:
    logical_and
  | logical_or OR logical_and { $$ = binary(c, OR, $1, $3, @2); }
  ;

logical_and:
    bit_or
  | logical_and AND bit_or { $$ = binary(c, AND, $1, $3, @2); }
  ;

bit_or:
    bit_xor
  | bit_or '|' bit_xor { $$ = binary(c, '|', $1, $3, @2); }
  ;

bit_xor:
    bit_and
  | bit_xor '^' bit_and { $$ = binary(c, '^', $1, $3, @2); }
  ;

bit_and:
    equality
  | bit_and '&' equality { $$ = binary(c, '&', $1, $3, @2); }
  ;

equality:
    relation
  | equality EQ relation { $$ = binary(c, EQ, $1, $3, @2); }
  | equality NE relation { $$ = binary(c, NE, $1, $3, @2); }
  ;

relation:
    shift
  | relation '<' shift { $$ = binary(c, '<', $1, $3, @2); }
  | relation '>' shift { $$ = binary(c, '>', $1, $3, @2); }
  | relation LE shift { $$ = binary(c, LE, $1, $3, @2); }
  | relation GE shift { $$ = binary(c, GE, $1, $3, @2); }
  ;

shift:
    additive
  | shift SHL additive { $$ = binary(c, SHL, $1, $3, @2); }
  | shift SHR additive { $$ = binary(c, SHR, $1, $3, @2); }
  ;

additive:
    multiplicative
  | additive '+' multiplicative { $$ = binary(c, '+', $1, $3, @2); }
  | additive '-' multiplicative { $$ = binary(c, '-', $1, $3, @2); }
  ;

multiplicative:
    cast
  | multiplicative '*' cast { $$ = binary(c, '*', $1, $3, @2); }
  | multiplicative '/' cast { $$ = binary(c, '/', $1, $3, @2); }
  | multiplicative '%' cast { $$ = binary(c, '%', $1, $3, @2); }
  ;

cast:
    unary
  | '(' type_name ')' cast {
      if ($2->kind == TY_STRUCT || $2->kind == TY_UNION)
        extension(c, @1, "cast to an aggregate type");
      $$ = cast_expression(c, $4, $2, @1);
    }
  ;

unary:
    postfix
  | INC unary { $$ = assign(c, '+', $2, integer(c, @1, 1, type_int(c, 3, false)), false, @1); }
  | DEC unary { $$ = assign(c, '-', $2, integer(c, @1, 1, type_int(c, 3, false)), false, @1); }
  | unary_op cast { $$ = unary(c, $1, $2, @1); }
  | SIZEOF unary {
      if ($2->width) error(c, @1, "sizeof cannot be applied to a bit-field");
      $$ = measure(c, $2->type, false, @1);
    }
  | SIZEOF '(' type_name ')' { $$ = measure(c, $3, false, @1); }
  | ALIGNOF '(' type_name ')' { $$ = measure(c, $3, true, @1); }
  ;

unary_op:
    '&' { $$ = '&'; } | '*' { $$ = '*'; } | '+' { $$ = '+'; }
  | '-' { $$ = '-'; } | '~' { $$ = '~'; } | '!' { $$ = '!'; }
  ;

postfix:
    primary
  | '(' type_name ')' braced_list { $$ = compound_literal(c, $2, $4, @1); }
  | postfix '[' expression ']' { $$ = unary(c, '*', binary(c, '+', $1, $3, @2), @2); }
  | postfix '(' arguments_opt ')' { $$ = call(c, $1, $3, @2); }
  | postfix '.' name { $$ = field(c, $1, $3, false, @3); }
  | postfix ARROW name { $$ = field(c, $1, $3, true, @3); }
  | postfix INC { $$ = assign(c, '+', $1, integer(c, @2, 1, type_int(c, 3, false)), true, @2); }
  | postfix DEC { $$ = assign(c, '-', $1, integer(c, @2, 1, type_int(c, 3, false)), true, @2); }
  ;

arguments_opt:
    %empty { $$ = (Nodes){0}; }
  | arguments
  ;

arguments:
    assignment { $$ = (Nodes){0}; PUSH(c, $$, $1); }
  | arguments ',' assignment { $$ = $1; PUSH(c, $$, $3); }
  ;

primary:
    IDENT { $$ = reference(c, $1.text, @1); }
  | NUMBER_TOKEN { $$ = literal(c, $1); }
  | CHAR_TOKEN { $$ = literal(c, $1); }
  | string_list
  | '(' expression ')' { $$ = $2; }
  | '(' compound ')' { $$ = statement_expression(c, $2, @1); }
  | GENERIC '(' assignment ',' generic_associations ')' { $$ = generic_selection(c, $3, $5, @1); }
  | VA_ARG '(' assignment ',' type_name ')' { $$ = va_argument(c, $3, $5, @1); }
  | OFFSETOF '(' type_name ',' offset_path ')' { $$ = member_offset(c, $3, $5, @1); }
  ;

offset_path:
    name offset_tail {
      $$ = node(c, N_DESIGNATE, @1, NULL, $2);
      $$->name = $1;
    }
  ;

offset_tail:
    %empty { $$ = NULL; }
  | '.' offset_path { $$ = $2; }
  | '[' conditional ']' offset_tail { $$ = node(c, N_DESIGNATE, @1, $2, $4); }
  ;

generic_associations:
    generic_association { $$ = (Nodes){0}; PUSH(c, $$, $1); }
  | generic_associations ',' generic_association { $$ = $1; PUSH(c, $$, $3); }
  ;

generic_association:
    type_name ':' assignment { $$ = node(c, N_EXPR, @1, $3, NULL); $$->type = $1; }
  | DEFAULT ':' assignment { $$ = node(c, N_EXPR, @1, $3, NULL); }
  ;

string_list:
    STRING_TOKEN { $$ = strings(c, NULL, $1); }
  | string_list STRING_TOKEN { $$ = strings(c, $1, $2); }
  ;

%%
static bool next_token(Context *c, const char *text)
{
    if (c->current < c->tokens.len && equal(c->tokens.data[c->current].text, text)) {
        c->current++;
        return true;
    }
    return false;
}

static int attributes(Context *c, Loc loc)
{
    if (!next_token(c, "(") || !next_token(c, "(")) {
        error(c, loc, "expected double parentheses after __attribute__");
        return YYerror;
    }
    bool packed = false;
    while (c->current < c->tokens.len && !next_token(c, ")")) {
        if (next_token(c, ","))
            continue;
        Token attr = c->tokens.data[c->current++];
        const char *name = attr.text;
        size_t len = strlen(name);
        if (len > 4 && name[0] == '_' && name[1] == '_' &&
            name[len - 2] == '_' && name[len - 1] == '_')
            name = copy(c, name + 2, len - 4);
        if (equal(name, "packed"))
            packed = true;
        else if (equal(name, "stdcall"))
            fprintf(stderr, "%s:%u:%u: warning: stdcall is ignored on %s\n",
                    attr.loc.file, attr.loc.line, attr.loc.column, c->target->display_name);
        else if (!equal(name, "noinline") && !equal(name, "used")) {
            error(c, attr.loc, "unsupported attribute '%s'", attr.text);
            return YYerror;
        }
        if (c->current < c->tokens.len && equal(c->tokens.data[c->current].text, "(")) {
            error(c, attr.loc, "attribute '%s' does not take arguments", attr.text);
            return YYerror;
        }
    }
    if (!next_token(c, ")")) {
        error(c, loc, "expected ')' after attribute list");
        return YYerror;
    }
    return packed ? PACKED_ATTR : 0;
}

static int yylex(YYSTYPE *value, Loc *loc, Context *c)
{
    Token t;
    for (;;) {
        if (c->current >= c->tokens.len)
            return 0;
        t = c->tokens.data[c->current++];
        if (!equal(t.text, "__attribute__") && !equal(t.text, "__attribute"))
            break;
        int attribute = attributes(c, t.loc);
        if (attribute) {
            *loc = t.loc;
            return attribute;
        }
    }
    *loc = t.loc;
    value->token = t;
    if (t.kind == NUMBER) return NUMBER_TOKEN;
    if (t.kind == STRING) return STRING_TOKEN;
    if (t.kind == CHARACTER) return CHAR_TOKEN;
    static const struct { const char *name; int code; } words[] = {
        {"void", VOID}, {"_Bool", BOOL}, {"char", CHAR}, {"short", SHORT},
        {"int", INT}, {"long", LONG}, {"float", FLOAT}, {"double", DOUBLE},
        {"signed", SIGNED}, {"unsigned", UNSIGNED}, {"typedef", TYPEDEF},
        {"extern", EXTERN}, {"static", STATIC}, {"auto", AUTO}, {"register", REGISTER},
        {"const", CONST}, {"volatile", VOLATILE}, {"restrict", RESTRICT},
        {"__restrict", RESTRICT}, {"__restrict__", RESTRICT}, {"inline", INLINE},
        {"_Noreturn", NORETURN}, {"_Thread_local", THREAD_LOCAL},
        {"__thread", THREAD_LOCAL}, {"struct", STRUCT}, {"union", UNION}, {"enum", ENUM},
        {"if", IF}, {"else", ELSE}, {"while", WHILE}, {"do", DO}, {"for", FOR},
        {"return", RETURN}, {"break", BREAK}, {"continue", CONTINUE},
        {"switch", SWITCH}, {"case", CASE}, {"default", DEFAULT}, {"goto", GOTO},
        {"sizeof", SIZEOF}, {"_Alignof", ALIGNOF}, {"_Static_assert", STATIC_ASSERT},
        {"_Generic", GENERIC}, {"__builtin_va_arg", VA_ARG}, {"__builtin_offsetof", OFFSETOF},
        {"typeof", TYPEOF}, {"__typeof__", TYPEOF}, {"__typeof", TYPEOF},
        {"++", INC}, {"--", DEC}, {"->", ARROW}, {"&&", AND}, {"||", OR},
        {"<<", SHL}, {">>", SHR}, {"==", EQ}, {"!=", NE}, {"<=", LE}, {">=", GE},
        {"...", ELLIPSIS}, {"+=", ADD_ASSIGN}, {"-=", SUB_ASSIGN}, {"*=", MUL_ASSIGN},
        {"/=", DIV_ASSIGN}, {"%=", MOD_ASSIGN}, {"<<=", SHL_ASSIGN}, {">>=", SHR_ASSIGN},
        {"&=", AND_ASSIGN}, {"^=", XOR_ASSIGN}, {"|=", OR_ASSIGN},
    };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (equal(t.text, words[i].name)) {
            if (equal(t.text, "typeof") && !c->gnu)
                return find_typedef(c, t.text) ? TYPE_NAME : IDENT;
            if (equal(t.text, "typeof") && c->pedantic)
                extension(c, t.loc, "typeof");
            if (equal(t.text, "__thread"))
                extension(c, t.loc, "__thread");
            return words[i].code;
        }
    if (t.kind == WORD) {
        if (!find_typedef(c, t.text))
            return IDENT;
        if (c->current < c->tokens.len && equal(c->tokens.data[c->current].text, ":")) {
            const char *previous = c->current >= 2 ? c->tokens.data[c->current - 2].text : "";
            if (equal(previous, ";") || equal(previous, "{") || equal(previous, "}") ||
                equal(previous, ":") || equal(previous, ")") || equal(previous, "else") ||
                equal(previous, "do"))
                return TYPE_LABEL;
        }
        return TYPE_NAME;
    }
    if (t.kind == PUNCT && !t.text[1])
        return (unsigned char)t.text[0];
    error(c, t.loc, "unexpected token '%s'", t.text);
    return YYerror;
}

bool parse(Context *c, Tokens tokens)
{
    c->tokens = tokens;
    setup_types(c);
    return yyparse(c) == 0 && !c->failed;
}
