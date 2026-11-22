#ifndef MCC_COMPILER_H
#define MCC_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *file;
    unsigned line, column, end_line, end_column;
} Loc;

typedef struct Arena Arena;
typedef struct Context Context;
typedef struct Type Type;
typedef struct Node Node;
typedef struct Symbol Symbol;
typedef struct Decl Decl;
typedef struct Scope Scope;
typedef struct Macro Macro;
typedef struct SavedMacro SavedMacro;
typedef struct Hide Hide;

typedef enum { WORD, NUMBER, STRING, CHARACTER, PUNCT, NEWLINE } TokenKind;
typedef struct {
    TokenKind kind;
    const char *text;
    Loc loc;
    bool space;
    const Hide *hide;
} Token;

#define VECTOR(T) struct { T *data; size_t len, cap; }
#define PUSH(c, v, value) do { \
    if ((v).len == (v).cap) \
        (v).data = grow((c), (v).data, &(v).cap, sizeof *(v).data); \
    (v).data[(v).len++] = (value); \
} while (0)

typedef VECTOR(Token) Tokens;
typedef VECTOR(const char *) Strings;
typedef VECTOR(Node *) Nodes;
typedef VECTOR(Symbol *) Symbols;
typedef VECTOR(Decl *) Decls;

typedef enum {
    TY_VOID, TY_BOOL, TY_INT, TY_FLOAT, TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_UNION
} TypeKind;

typedef struct {
    const char *name;
    Type *type;
    size_t offset;
    unsigned bit, width;
    bool bit_field;
} Member;

typedef VECTOR(Member) Members;

struct Type {
    TypeKind kind;
    unsigned size, align, rank, qualifiers;
    bool uns, variadic, prototype, complete;
    bool plain_char;
    bool enumeration;
    Type *base;
    Type *qualified_next;
    Node *variable_bound;
    Symbol *runtime_size;
    size_t length;
    Members members;
};

typedef struct {
    unsigned char_size, short_size, int_size, long_size, long_long_size, pointer_size;
    unsigned long_double_size;
    bool char_unsigned;
} Target;

extern const Target aarch64_linux;

typedef enum { D_NAME, D_POINTER, D_ARRAY, D_FUNCTION } DeclKind;
struct Decl {
    DeclKind kind;
    Decl *inner;
    const char *name;
    Loc loc;
    Node *bound;
    Node *bit_width;
    Members params;
    bool variadic, prototype;
    unsigned qualifiers;
};

enum {
    S_VOID = 1u << 0, S_BOOL = 1u << 1, S_CHAR = 1u << 2, S_SHORT = 1u << 3,
    S_INT = 1u << 4, S_LONG = 1u << 5, S_LLONG = 1u << 6, S_FLOAT = 1u << 7,
    S_DOUBLE = 1u << 8, S_SIGNED = 1u << 9, S_UNSIGNED = 1u << 10,
    S_CONST = 1u << 11, S_TYPEDEF = 1u << 12, S_EXTERN = 1u << 13,
    S_STATIC = 1u << 14, S_AUTO = 1u << 15, S_REGISTER = 1u << 16, S_THREAD = 1u << 17,
    S_PACKED = 1u << 18, S_VOLATILE = 1u << 19, S_RESTRICT = 1u << 20,
    S_QUALIFIERS = S_CONST | S_VOLATILE | S_RESTRICT
};

typedef struct { unsigned flags; Type *type; } Specs;

typedef enum {
    N_INT, N_FLOAT, N_SYMBOL, N_ADDR, N_DEREF, N_MEMBER, N_CAST,
    N_UNARY, N_BINARY, N_ASSIGN, N_UPDATE, N_COND, N_CALL, N_SEQUENCE, N_INIT,
    N_DESIGNATE, N_RANGE_VALUE, N_COMPOUND, N_STATEMENT_EXPR, N_VA_START, N_VA_ARG, N_VA_END,
    N_BLOCK, N_EXPR, N_DECL, N_IF, N_LOOP, N_DO, N_RETURN, N_BREAK, N_CONTINUE,
    N_SWITCH, N_CASE, N_DEFAULT, N_GOTO, N_LABEL
} NodeKind;

struct Node {
    NodeKind kind;
    Loc loc;
    Type *type;
    Node *left, *right, *third, *fourth;
    Nodes list;
    Symbol *symbol;
    const char *name;
    uint64_t integer;
    long double real;
    int op;
    bool postfix;
    unsigned bit, width;
};

struct Symbol {
    const char *name;
    const char *assembly;
    Loc loc;
    Type *type;
    bool local, internal, defined, tentative, is_typedef, enum_constant, read_only;
    bool thread_local_storage;
    uint64_t enum_value;
    size_t slot;
    const unsigned char *bytes;
    size_t byte_count;
    Node *init, *body;
    Symbols locals, params;
};

struct Context {
    Arena *arena;
    const Target *target;
    Scope *scope;
    Macro *macros;
    SavedMacro *saved_macros;
    bool gnu, pedantic, failed;
    unsigned errors, unique;
    Strings includes, system_includes, macro_options;
    Strings once;
    Tokens tokens;
    size_t current;
    Symbols globals;
    Symbol *function;
    uint64_t next_enum;
    bool enum_negative;
    Type *va_list_type;
    const char *date_macro, *time_macro;
    bool function_body;
    Specs declaration;
    const char *input;
};

void *alloc(Context *, size_t);
void *grow(Context *, const void *, size_t *, size_t);
char *copy(Context *, const char *, size_t);
#if defined(__GNUC__)
__attribute__((__format__(__printf__, 2, 3)))
#endif
char *format(Context *, const char *, ...);
void release(Context *);
#if defined(__GNUC__)
__attribute__((__format__(__printf__, 3, 4)))
#endif
void error(Context *, Loc, const char *, ...);
void extension(Context *, Loc, const char *);
size_t align_to(size_t, size_t);
bool equal(const char *, const char *);

bool scan(Context *, const char *, const char *, size_t, Tokens *);
bool preprocess(Context *, const char *, FILE *, Tokens *);
void write_tokens(FILE *, Tokens);
bool parse(Context *, Tokens);
void setup_types(Context *);
Type *type_int(Context *, unsigned rank, bool uns);
Type *pointer(Context *, Type *);
Type *spec_type(Context *, Specs, Loc);
Type *apply_decl(Context *, Type *, Decl *);
Specs merge_specs(Context *, Specs, Specs, Loc);
Decl *decl(Context *, DeclKind, Decl *, Loc);
Decl *decl_name(Decl *);
void enter_scope(Context *);
void leave_scope(Context *);
Type *find_typedef(Context *, const char *);
Node *node(Context *, NodeKind, Loc, Node *, Node *);
Node *integer(Context *, Loc, uint64_t, Type *);
Node *literal(Context *, Token);
Node *strings(Context *, Node *, Token);
Node *reference(Context *, const char *, Loc);
Node *unary(Context *, int, Node *, Loc);
Node *binary(Context *, int, Node *, Node *, Loc);
Node *assign(Context *, int, Node *, Node *, bool, Loc);
Node *convert(Context *, Node *, Type *, Loc);
Node *cast_expression(Context *, Node *, Type *, Loc);
Node *conditional(Context *, Node *, Node *, Node *, Loc);
Node *call(Context *, Node *, Nodes, Loc);
Node *field(Context *, Node *, const char *, bool, Loc);
Node *measure(Context *, Type *, bool, Loc);
Node *decay(Context *, Node *);
Node *check_return(Context *, Node *, Loc);
bool constant_int(Context *, Node *, uint64_t *);
Type *record_type(Context *, bool, const char *, Members *, Loc);
void pack_record(Context *, Type *, Loc);
void add_member(Context *, Members *, Specs, Decl *, Loc);
Node *declare(Context *, Specs, Decl *, Node *, Loc);
void set_initializer(Context *, Node *, Node *);
Node *compound_literal(Context *, Type *, Nodes, Loc);
Node *generic_selection(Context *, Node *, Nodes, Loc);
Node *statement_expression(Context *, Node *, Loc);
Node *va_argument(Context *, Node *, Type *, Loc);
Node *member_offset(Context *, Type *, Node *, Loc);
void begin_function(Context *, Specs, Decl *, Loc);
void end_function(Context *, Node *);
void define_enum(Context *, const char *, Node *, uint64_t *, Loc);
Type *enum_type(Context *, const char *, bool, Loc);
bool scalar(Type *);
bool aggregate(Type *);
bool same_type(Type *, Type *);

typedef enum {
    I_IMMEDIATE, I_REAL, I_ADDRESS, I_LOAD, I_STORE, I_COPY, I_CAST, I_UNARY, I_BINARY,
    I_LABEL, I_JUMP, I_ZERO, I_CALL, I_RETURN, I_MEMCOPY, I_MEMZERO, I_VA_START, I_VA_ARG,
    I_BITLOAD, I_BITSTORE, I_STACK_ALLOC, I_STACK_RESTORE
} Opcode;
typedef VECTOR(size_t) Values;
typedef VECTOR(Type *) Types;
typedef struct {
    Opcode code;
    Loc loc;
    size_t dest, a, b, label;
    int op;
    uint64_t immediate;
    long double real;
    unsigned bit, width;
    Type *type, *source_type;
    Symbol *symbol;
    Values args;
    Types arg_types;
} Inst;
typedef VECTOR(Inst) Instructions;
typedef struct {
    Symbol *function;
    Types slots;
    Instructions instructions;
} FunctionIR;

bool lower(Context *, Symbol *, FunctionIR *);
bool emit_aarch64(Context *, FILE *);

#endif
