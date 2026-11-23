#ifndef MCC_COMPILER_H
#define MCC_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "atomic.h"
#include "int128.h"
#include "decimal.h"
#include "float_builtins.h"

typedef struct DiagnosticState DiagnosticState;
typedef struct IgnoredAttribute IgnoredAttribute;

typedef struct {
    const char *file;
    unsigned line, column, end_line, end_column;
    bool system_header;
    unsigned char pack_alignment, visibility, fallthrough_comment;
    unsigned source_offset;
    const DiagnosticState *diagnostics;
    const IgnoredAttribute *ignored_attributes;
} Loc;

typedef struct Arena Arena;
typedef struct Context Context;
typedef struct Type Type;
typedef struct Node Node;
typedef struct Symbol Symbol;
typedef struct Decl Decl;
typedef struct Scope Scope;
typedef struct RecordBoundsScope RecordBoundsScope;
typedef struct Macro Macro;
typedef struct SavedMacro SavedMacro;
typedef struct SourceIdentity SourceIdentity;
typedef struct PackSetting PackSetting;
typedef struct Hide Hide;
typedef struct EnumDefinition EnumDefinition;

typedef enum { C90, C99, C11, C17, C23, C2Y } CStandard;
typedef enum { VIS_UNSPECIFIED, VIS_DEFAULT, VIS_INTERNAL, VIS_HIDDEN, VIS_PROTECTED } Visibility;
typedef enum { PEDANTIC_OFF, PEDANTIC_WARN, PEDANTIC_ERROR } Pedantic;
typedef enum {
    WARNING_NONE, WARNING_PEDANTIC, WARNING_CPP, WARNING_ATTRIBUTES, WARNING_ATTRIBUTE_ALIAS, WARNING_PRAGMAS,
    WARNING_UNKNOWN_PRAGMAS, WARNING_INVALID_MEMORY_MODEL, WARNING_STRINGOP_OVERFLOW, WARNING_STRINGOP_OVERREAD,
    WARNING_PRIORITY_CTOR_DTOR, WARNING_DEPRECATED, WARNING_DEPRECATED_DECLARATIONS, WARNING_DISCARDED_QUALIFIERS,
    WARNING_DESIGNATED_INIT, WARNING_FRAME_ADDRESS, WARNING_UNUSED_RESULT,
    WARNING_IMPLICIT_FALLTHROUGH, WARNING_ATTRIBUTE_WARNING, WARNING_FORMAT, WARNING_FORMAT_NONLITERAL, WARNING_FORMAT_SECURITY, WARNING_FORMAT_SIGNEDNESS, WARNING_FORMAT_Y2K,
    WARNING_NONNULL, WARNING_NONNULL_COMPARE, WARNING_UNINITIALIZED, WARNING_MAYBE_UNINITIALIZED, WARNING_CLOBBERED,
    WARNING_ALLOCA, WARNING_ALLOCA_LARGE, WARNING_VLA, WARNING_VLA_LARGE, WARNING_FRAME_LARGE, WARNING_STACK_USAGE, WARNING_UNDEF, WARNING_EXPANSION_DEFINED, WARNING_COUNT
} WarningKind;
typedef enum { DIAG_DEFAULT, DIAG_IGNORED, DIAG_WARNING, DIAG_ERROR } DiagnosticLevel;
typedef struct {
    DiagnosticLevel enabled, severity;
} WarningOption;

struct DiagnosticState {
    const DiagnosticState *previous;
    WarningKind warning;
    DiagnosticLevel level;
};

typedef struct {
    CStandard standard;
    Pedantic pedantic;
    bool gnu, gnu_inline, short_enums, round_float16_ops;
} Dialect;

typedef enum {
    BUILTIN_NONE, BUILTIN_VA_ARG, BUILTIN_VA_START, BUILTIN_VA_END, BUILTIN_VA_COPY,
    BUILTIN_OFFSETOF, BUILTIN_EXPECT, BUILTIN_TYPES_COMPATIBLE, BUILTIN_CHOOSE_EXPR,
    BUILTIN_CONSTANT_P, BUILTIN_COMPLEX, BUILTIN_TRAP, BUILTIN_UNREACHABLE, BUILTIN_STATIC_CHAIN, BUILTIN_INTEGER,
    BUILTIN_ATOMIC, BUILTIN_OVERFLOW, BUILTIN_OBJECT_SIZE, BUILTIN_DYNAMIC_OBJECT_SIZE,
    BUILTIN_FORTIFIED, BUILTIN_ALLOCA, BUILTIN_ALLOCA_ALIGN, BUILTIN_ALLOCA_ALIGN_MAX, BUILTIN_ALLOCATOR,
    BUILTIN_EXEC,
    BUILTIN_VECTOR_CONVERT, BUILTIN_VECTOR_SHUFFLE, BUILTIN_VECTOR_SHUFFLE_CONSTANT, BUILTIN_TGMATH,
    BUILTIN_FLOAT, BUILTIN_EXPECT_PROBABILITY, BUILTIN_ASSUME_ALIGNED,
    BUILTIN_LINE, BUILTIN_FILE, BUILTIN_FUNCTION, BUILTIN_PREFETCH, BUILTIN_CLEAR_CACHE,
    BUILTIN_CLEAR_PADDING, BUILTIN_SETJMP, BUILTIN_LONGJMP,
    BUILTIN_FRAME_ADDRESS, BUILTIN_RETURN_ADDRESS, BUILTIN_EXTRACT_RETURN_ADDR,
    BUILTIN_FROB_RETURN_ADDR, BUILTIN_DWARF_CFA, BUILTIN_DWARF_SP_COLUMN, BUILTIN_EH_DATA_REGNO,
    BUILTIN_STACK_SAVE, BUILTIN_STACK_RESTORE, BUILTIN_DWARF_REG_SIZES, BUILTIN_EXTEND_POINTER,
    BUILTIN_CLASSIFY_TYPE, BUILTIN_SPECULATION_SAFE_VALUE, BUILTIN_SPECULATION_1,
    BUILTIN_SPECULATION_2, BUILTIN_SPECULATION_4, BUILTIN_SPECULATION_8, BUILTIN_SPECULATION_16,
    BUILTIN_SPECULATION_PTR, BUILTIN_SYSTEM_REGISTER, BUILTIN_HAS_ATTRIBUTE, BUILTIN_TERMINATING
} Builtin;
typedef enum { SYSREG_THREAD_POINTER, SYSREG_FPCR, SYSREG_FPSR, SYSREG_MXCSR } SystemRegister;
typedef enum {
    ATTR_NONE, ATTR_PACKED, ATTR_NOINLINE, ATTR_USED, ATTR_STDCALL, ATTR_GNU_INLINE, ATTR_ALIGNED,
    ATTR_WEAK, ATTR_ALIAS, ATTR_IFUNC, ATTR_WEAKREF, ATTR_VISIBILITY, ATTR_CONSTRUCTOR, ATTR_DESTRUCTOR,
    ATTR_ALLOC_SIZE, ATTR_ALLOC_ALIGN, ATTR_ASSUME_ALIGNED, ATTR_CLEANUP, ATTR_SECTION, ATTR_VECTOR_SIZE, ATTR_MODE, ATTR_DEPRECATED,
    ATTR_TRANSPARENT_UNION, ATTR_DESIGNATED_INIT, ATTR_NORETURN, ATTR_WARN_UNUSED_RESULT, ATTR_RETURNS_TWICE,
    ATTR_FALLTHROUGH, ATTR_ERROR, ATTR_WARNING, ATTR_SENTINEL, ATTR_NONNULL, ATTR_RETURNS_NONNULL,
    ATTR_NOPLT, ATTR_PATCHABLE_FUNCTION_ENTRY, ATTR_ACCESS, ATTR_LEAF, ATTR_HOT, ATTR_COLD, ATTR_FORMAT, ATTR_FORMAT_ARG
} Attribute;

typedef enum {
    WORD, NUMBER, STRING, CHARACTER, PUNCT, NEWLINE, ATTRIBUTE, DEFAULT_ALIGNMENT, SKIPPED,
    PRAGMA_PACK, PRAGMA_WEAK, PRAGMA_VISIBILITY, PRAGMA_DIAGNOSTIC, PRAGMA_MESSAGE, PRAGMA_UNKNOWN,
    DEFAULT_SENTINEL, DEFAULT_NONNULL,
    WEAKREF_ARGUMENT, DEFAULT_LIFECYCLE, ATTRIBUTE_IDENTIFIER, DEPRECATION_ARGUMENT
} TokenKind;
typedef struct {
    TokenKind kind;
    const char *text;
    Loc loc;
    bool space, poison_checked, macro_origin, macro_system_header;
    const Hide *hide;
} Token;

#define VECTOR(T) struct { T *data; size_t len, cap; }
#define PUSH(c, v, value) do { \
    if ((v).len == (v).cap) \
        (v).data = grow((c), (v).data, &(v).cap, sizeof *(v).data); \
    (v).data[(v).len++] = (value); \
} while (0)

typedef VECTOR(Token) Tokens;
typedef VECTOR(Loc) Locations;
typedef VECTOR(const char *) Strings;
typedef VECTOR(Node *) Nodes;
typedef VECTOR(Symbol *) Symbols;
typedef VECTOR(Decl *) Decls;
typedef struct {
    Attribute kind;
    const char *message;
    size_t length;
} CallDiagnostic;
typedef VECTOR(CallDiagnostic) CallDiagnostics;

typedef enum {
    TY_VOID, TY_BOOL, TY_INT, TY_FLOAT, TY_DECIMAL, TY_COMPLEX,
    TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_UNION, TY_VECTOR
} TypeKind;

typedef struct { unsigned count, before; bool explicit_before; } PatchableEntry;
typedef VECTOR(PatchableEntry) PatchableEntries;
enum { MAX_PATCHABLE_ENTRY = 65535 };
typedef enum { ACCESS_NONE, ACCESS_READ, ACCESS_WRITE, ACCESS_READ_WRITE } AccessMode;
typedef struct { AccessMode mode; unsigned reference, size; Loc loc; } AccessClause;
typedef VECTOR(AccessClause) AccessClauses;
typedef struct { Integer128 lower, upper; } IntegerRange;
typedef IntegerRange (*IntegerRangeLookup)(void *, const Symbol *);
typedef struct { Node *value; size_t point; } AccessPointerStore;
typedef VECTOR(AccessPointerStore) AccessPointerStores;
typedef bool (*AccessPointerLookup)(void *, const Symbol *, size_t, AccessPointerStores *);
typedef IntegerRange (*AccessOffsetRange)(void *, Node *, size_t);
typedef IntegerRange (*AccessAllocationRange)(void *, const Node *, unsigned, Node *);
typedef bool (*AccessLayoutRange)(void *, const Symbol *, size_t, IntegerRange *);
typedef struct {
    AccessPointerLookup pointer_stores;
    AccessOffsetRange offset;
    AccessAllocationRange allocation;
    AccessLayoutRange layout;
    void *data;
    size_t point;
} AccessRangeSource;

typedef struct {
    const char *name;
    Type *type, *parameter_array;
    const char *deprecated;
    Symbol *parameter;
    const PatchableEntries *patchable_entries;
    size_t offset;
    Node *runtime_offset;
    unsigned bit, width, alignment, requested_alignment, alignment_attributes, attribute_markers;
    bool bit_field, packed_bit_field, packed_attribute;
} Member;

typedef VECTOR(Member) Members;
typedef enum { FLOAT_DEFAULT, FLOAT32, FLOAT64, FLOAT32X, FLOAT64X, FLOAT128 } FloatVariant;
typedef struct { unsigned first, second; } AllocationSize;
typedef struct { uint64_t alignment, offset; } AssumedAlignment;
typedef struct FormatSpec FormatSpec;
struct FormatSpec { const char *style; unsigned index, first; Loc loc; const FormatSpec *next; };
typedef struct { Integer128 position; bool explicit_position; } Sentinel;
typedef VECTOR(Sentinel) Sentinels;
typedef VECTOR(unsigned) ParameterIndices;
typedef VECTOR(ParameterIndices) NonnullClauses;
typedef struct {
    bool returns_nonnull;
    NonnullClauses nonnull;
} FunctionNullability;
typedef struct {
    const char *mode;
    unsigned vector_size;
    Loc loc;
} TypeAttribute;
typedef VECTOR(TypeAttribute) TypeAttributes;

struct Type {
    TypeKind kind;
    unsigned size, align, rank, qualifiers, nonatomic_align;
    bool uns, variadic, prototype, complete, noreturn_pointer, warn_unused_result;
    bool plain_char, storage_float, bfloat, variable_size, array_star;
    FloatVariant float_variant;
    unsigned char record_pack;
    bool record_packed, record_variably_modified, transparent_union, designated_init;
    unsigned record_alignment, member_alignment, typedef_alignment, natural_alignment;
    unsigned record_alignment_attributes;
    bool enumeration, fixed_enum, enum_defined, enum_packed;
    Type *mode_enum;
    Type *base;
    Type *qualified_next;
    Node *variable_bound, *typeof_operand;
    Symbol *runtime_size;
    Scope *parameter_scope;
    const char *deprecated;
    Nodes record_bounds;
    size_t length;
    AllocationSize allocation;
    unsigned allocation_alignment;
    const AssumedAlignment *assumed_alignment;
    Sentinels sentinels;
    const FormatSpec *format_spec;
    const ParameterIndices *format_args;
    FunctionNullability nullability;
    const AccessClauses *accesses;
    Members members;
};

static inline Type *parameter_abi_type(Type *t)
{
    return t->transparent_union ? t->members.data[0].type : t;
}

static inline bool integer_type(const Type *t)
{
    return t && (t->kind == TY_INT || t->kind == TY_BOOL);
}

static inline bool integer128(Type *t)
{
    return t->kind == TY_INT && t->size == 16;
}

static inline unsigned abi_alignment(const Type *type)
{
    return type->typedef_alignment ? type->natural_alignment : type->align;
}

typedef struct {
    const char *arch, *display_name;
    bool (*emit)(Context *, FILE *);
    unsigned char_size, short_size, int_size, long_size, long_long_size, pointer_size;
    unsigned long_double_size;
    bool char_unsigned, wchar_unsigned;
} Target;

extern const Target aarch64_linux;
extern const Target x86_64_linux;

unsigned floating_format(const Target *, Type *);
bool binary128(const Target *, Type *);
unsigned builtin_float_format(const Target *, Type *);

typedef enum { D_NAME, D_POINTER, D_ARRAY, D_FUNCTION, D_ALIGNED } DeclKind;
struct Decl {
    DeclKind kind;
    Decl *inner;
    const char *name;
    const char *asm_name;
    const char *cleanup, *deprecated;
    Loc loc;
    Node *bound;
    Node *bit_width;
    Members params;
    Scope *parameter_scope;
    AllocationSize allocation;
    unsigned allocation_alignment;
    const AssumedAlignment *assumed_alignment;
    const PatchableEntries *patchable_entries;
    Sentinels sentinels, member_sentinels;
    const FormatSpec *format_spec, *member_format_spec;
    const ParameterIndices *format_args, *member_format_args;
    FunctionNullability nullability, member_nullability;
    const AccessClauses *accesses, *member_accesses;
    TypeAttributes type_attributes, member_type_attributes;
    bool variadic, prototype, array_star;
    unsigned qualifiers, alignment;
    unsigned member_alignment, member_alignment_attributes, attribute_markers, member_attribute_markers;
    AllocationSize member_allocation;
    unsigned member_allocation_alignment;
    const AssumedAlignment *member_assumed_alignment;
    bool packed_member, designated_init;
};

enum {
    S_VOID = 1u << 0, S_BOOL = 1u << 1, S_CHAR = 1u << 2, S_SHORT = 1u << 3,
    S_INT = 1u << 4, S_LONG = 1u << 5, S_LLONG = 1u << 6, S_FLOAT = 1u << 7,
    S_DOUBLE = 1u << 8, S_SIGNED = 1u << 9, S_UNSIGNED = 1u << 10,
    S_CONST = 1u << 11, S_TYPEDEF = 1u << 12, S_EXTERN = 1u << 13,
    S_STATIC = 1u << 14, S_AUTO = 1u << 15, S_REGISTER = 1u << 16, S_THREAD = 1u << 17,
    S_PACKED = 1u << 18, S_VOLATILE = 1u << 19, S_RESTRICT = 1u << 20,
    S_ATOMIC = 1u << 21, S_INLINE = 1u << 22, S_GNU_INLINE = 1u << 23,
    S_ENUM_FORWARD = 1u << 24, S_INT128 = 1u << 25, S_COMPLEX = 1u << 26,
    S_WEAK = 1u << 27, S_WEAKREF = 1u << 28, S_FLOAT16 = 1u << 29, S_FP16 = 1u << 30,
    S_QUALIFIERS = S_CONST | S_VOLATILE | S_RESTRICT | S_ATOMIC
};

typedef struct {
    unsigned flags;
    Type *type;
    const char *alias, *ifunc;
    const char *cleanup;
    const char *section, *deprecated;
    unsigned iso_alignment, gnu_alignment, alignment_attributes;
    unsigned constructor_priority, destructor_priority;
    unsigned attribute_markers;
    AllocationSize allocation;
    unsigned allocation_alignment;
    const AssumedAlignment *assumed_alignment;
    const PatchableEntries *patchable_entries, *inherited_patchable_entries;
    Sentinels sentinels;
    const FormatSpec *format_spec;
    const ParameterIndices *format_args;
    FunctionNullability nullability;
    const AccessClauses *accesses;
    TypeAttributes type_attributes;
    CallDiagnostics call_diagnostics;
    Visibility visibility;
    bool has_alignas, transparent_union, designated_init, noreturn_specifier;
} Specs;

enum {
    /* Explicit alignment values each occupy one bit in the attribute mask. */
    DEFAULT_ALIGNED_ATTRIBUTE = 1u << 17,
    MARK_NOINLINE = 1u << 0, MARK_USED = 1u << 1,
    DEFAULT_CONSTRUCTOR_ATTRIBUTE = 1u << 2, DEFAULT_DESTRUCTOR_ATTRIBUTE = 1u << 3,
    MARK_NORETURN = 1u << 4, MARK_WARN_UNUSED_RESULT = 1u << 5, MARK_NOPLT = 1u << 6,
    MARK_LEAF = 1u << 7, MARK_HOT = 1u << 8, MARK_COLD = 1u << 9, MARK_RETURNS_TWICE = 1u << 10
};

typedef enum {
    N_INT, N_FLOAT, N_COMPLEX, N_SYMBOL, N_ADDR, N_DEREF, N_MEMBER, N_VECTOR_ELEMENT, N_CAST,
    N_UNARY, N_BINARY, N_ASSIGN, N_UPDATE, N_COND, N_CALL, N_SEQUENCE, N_INIT,
    N_DESIGNATE, N_RANGE_VALUE, N_COMPOUND, N_STATEMENT_EXPR, N_VA_START, N_VA_ARG, N_VA_END,
    N_BLOCK, N_EXPR, N_DECL, N_IF, N_LOOP, N_DO, N_RETURN, N_BREAK, N_CONTINUE,
    N_SWITCH, N_CASE, N_DEFAULT, N_GOTO, N_LABEL, N_INDIRECT_GOTO, N_TRAP, N_FALLTHROUGH,
    N_PREFETCH, N_CLEAR_CACHE, N_CLEAR_PADDING, N_SETJMP, N_LONGJMP, N_FRAME_QUERY,
    N_STACK_SAVE, N_STACK_RESTORE, N_SPECULATION_SAFE_VALUE, N_SYSTEM_REGISTER,
    N_LOCAL_LABEL, N_NONLOCAL_GOTO, N_ATOMIC, N_OVERFLOW, N_CARRY, N_OBJECT_SIZE, N_OBJECT_REMAINING, N_ALLOCA,
    N_VECTOR_CONVERT, N_VECTOR_SHUFFLE, N_FLOAT_BUILTIN
} NodeKind;

enum { BLOCK_STACK_ALLOCATION = 1 };
enum { DECL_VARIABLY_MODIFIED = 1 };
enum { UNION_CAST_LITERAL = 1 };
enum { SUBSCRIPT_ARRAY = 1 };
enum { CAST_EXCESS_PRECISION = 1 };
enum { FLOAT_BITS_LITERAL = 1 };
enum { PACKED_BIT_FIELD = 1 };
enum { DEFAULT_LIFECYCLE_PRIORITY = 65535 };

struct Node {
    NodeKind kind;
    Loc loc;
    Type *type, *operand_type;
    Node *left, *right, *third, *fourth;
    Node *object_subsize, *object_displacement;
    Nodes list;
    Symbol *symbol;
    Symbol *extent_source;
    const Member *member;
    const char *name;
    uint64_t integer, integer_high;
    long double real;
    int op;
    bool postfix;
    unsigned bit, width, flags, alignment;
};

static inline Node *excess_value(Node *n)
{
    return n->kind == N_CAST && (n->flags & CAST_EXCESS_PRECISION) ? n->left : n;
}

static inline bool narrow_float(Type *t)
{
    return t->kind == TY_FLOAT && t->size == 2;
}

typedef enum { INLINE_ORDINARY, INLINE_ISO, INLINE_GNU_EXTERN } InlineDefinition;

struct Symbol {
    const char *name;
    const char *assembly;
    const char *alias, *ifunc;
    const char *section, *deprecated;
    CallDiagnostics call_diagnostics;
    const PatchableEntries *patchable_entries;
    Loc loc, function_definition_loc, function_end_loc;
    Type *type;
    bool local, internal, defined, tentative, is_typedef, enum_constant, read_only;
    bool thread_local_storage, forward_parameter;
    bool label_referenced, label_address_taken;
    bool explicit_asm_name;
    bool has_nonlocal_labels, has_builtin_setjmp, has_returns_twice, has_explicit_stack;
    bool captured_in_nested_function, address_taken;
    bool has_stack_alloca, has_variable_temporary;
    bool iso_external;
    bool weak, weak_reference, explicit_visibility;
    bool pointer_size_untracked, has_switch, array_layout_size;
    bool checking_extent, checked_extent, constant_extent;
    unsigned inline_declarations;
    unsigned register_binding;
    unsigned alignment, alignment_attributes, attribute_markers;
    unsigned constructor_priority, destructor_priority;
    Visibility visibility;
    InlineDefinition inline_definition;
    Integer128 enum_value;
    size_t slot, frame_offset;
    const unsigned char *bytes;
    size_t byte_count;
    Node *init, *body, *cleanup;
    Symbol *owner, *static_chain, *trampolines;
    Symbol *nonlocal_state;
    Symbol *object_extent, *object_subextent, *object_base;
    uint64_t extent_value;
    Symbols locals, params, labels;
    Nodes object_size_queries, pointer_assignments, fortified_calls, parameter_bounds;
    Symbol *function_names[3];
};

static inline bool has_stack_checkpoints(const Symbol *s)
{
    return s->has_builtin_setjmp || s->has_explicit_stack;
}

static inline unsigned symbol_alignment(const Symbol *s)
{
    return s->alignment ? s->alignment : s->type->align;
}

static inline bool global_register(Symbol *s)
{
    return s->register_binding && !s->local;
}

typedef struct { const char *name, *target; Loc loc; } WeakPragma;

struct Context {
    Arena *arena;
    const Target *target;
    const char *program;
    Scope *scope;
    RecordBoundsScope *record_bounds_scope;
    Macro *macros;
    SavedMacro *saved_macros;
    SourceIdentity *source_identities;
    PackSetting *pack_stack;
    const DiagnosticState *diagnostics;
    const IgnoredAttribute *ignored_attributes;
    VECTOR(const DiagnosticState *) diagnostic_stack;
    WarningOption warnings[WARNING_COUNT];
    bool warnings_as_errors, suppress_warnings, warn_all, warn_extra, syntax_only;
    FILE *stack_usage_report;
    bool no_delete_null_pointer_checks;
    bool no_plt;
    PatchableEntry patchable_entry;
    unsigned fallthrough_level, format_level;
    uint64_t alloca_warning_limit, vla_warning_limit, frame_warning_limit, stack_warning_limit;
    bool alloca_warning_limit_set, vla_warning_limit_set, frame_warning_limit_set, stack_warning_limit_set;
    VECTOR(WeakPragma) weak_pragmas;
    VECTOR(Visibility) visibility_stack;
    Visibility default_visibility, pragma_visibility;
    Dialect dialect;
    unsigned extension_depth;
    bool failed;
    unsigned errors, unique;
    Strings includes, system_includes, macro_options;
    Strings disabled_builtins;
    const char *include_path;
    size_t include_index;
    bool in_system_header, warn_system_headers;
    unsigned system_header_offset;
    bool pp_condition, skip_poison_checks;
    bool freestanding, no_builtin;
    bool force_pack;
    unsigned char default_pack_alignment, pack_alignment;
    Strings poisoned_identifiers;
    Tokens tokens;
    size_t current;
    unsigned enum_tag_context;
    size_t enum_attribute_end;
    uint64_t global_registers, global_register_spills;
    Symbols globals;
    Symbol *function;
    EnumDefinition *enumeration;
    Type *va_list_type;
    const char *date_macro, *time_macro;
    bool function_body;
    Specs declaration;
    const char *input;
};

static inline bool valid_pack_alignment(unsigned long long alignment)
{
    return alignment <= 16 && !(alignment & (alignment - 1));
}

static inline bool suppress_system_diagnostic(Context *c, Loc loc)
{
    return loc.system_header && !c->warn_system_headers;
}

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
#if defined(__GNUC__)
__attribute__((__format__(__printf__, 3, 4)))
#endif
void warning(Context *, Loc, const char *, ...);
#if defined(__GNUC__)
__attribute__((__format__(__printf__, 4, 5)))
#endif
void warning_at(Context *, WarningKind, Loc, const char *, ...);
void note(Context *, Loc, const char *);
bool warning_enabled(Context *, WarningKind, Loc);
WarningKind warning_kind(const char *);
bool warning_argument(Context *, const char *);
bool ignore_attribute(Context *, const char *);
bool attribute_ignored(Loc, const char *, const char *);
size_t align_to(size_t, size_t);
bool equal(const char *, const char *);

bool scan(Context *, const char *, const char *, size_t, Tokens *, Locations *);
bool preprocess(Context *, const char *, FILE *, Tokens *);
void apply_pack_pragma(Context *, Token);
void apply_weak_pragma(Context *, Token);
void apply_visibility_pragma(Context *, Token);
void write_tokens(Context *, FILE *, Tokens, bool);
bool parse(Context *, Tokens);
void finish_translation_unit(Context *);
void setup_types(Context *);
Builtin builtin_kind(const char *);
bool builtin_available(Context *, const char *);
Attribute attribute_kind(Context *, const char *);
Visibility visibility_kind(const char *);
Specs lifecycle_spec(Context *, Token, Node *);
void warn_lifecycle_attributes(Context *, Specs, Loc);
void warn_invalid_noplt(Context *, Specs, const Decl *, Loc);
const PatchableEntries *patchable_entry_spec(Context *, Node *, Node *, Loc);
const PatchableEntries *combine_patchable_entries(Context *, const PatchableEntries *, const PatchableEntries *);
AllocationSize allocation_size_spec(Context *, Node *, Node *, Loc);
Type *allocation_size_type(Context *, Type *, AllocationSize, Loc);
unsigned allocation_alignment_spec(Context *, Node *, Loc);
Type *allocation_alignment_type(Context *, Type *, unsigned, Loc);
const AssumedAlignment *assumed_alignment_spec(Context *, Node *, Node *, Loc);
Type *assumed_alignment_type(Context *, Type *, const AssumedAlignment *, Loc);
Sentinels combine_sentinels(Context *, Sentinels, Sentinels);
Sentinels sentinel_spec(Context *, Node *, Loc);
const FormatSpec *format_attribute_spec(Context *, const char *, Node *, Node *, Loc);
Type *format_attribute_type(Context *, Type *, const FormatSpec *);
const FormatSpec *combine_format_specs(Context *, const FormatSpec *, const FormatSpec *);
const ParameterIndices *format_argument_spec(Context *, Node *, Loc);
const ParameterIndices *combine_format_arguments(Context *, const ParameterIndices *, const ParameterIndices *);
Type *format_argument_type(Context *, Type *, const ParameterIndices *, Loc);
Type *sentinel_type(Context *, Type *, Sentinels, Loc);
FunctionNullability combine_nullability(Context *, FunctionNullability, FunctionNullability);
FunctionNullability nonnull_spec(Context *, Nodes, Loc);
Type *nullability_type(Context *, Type *, FunctionNullability, Loc);
const AccessClauses *access_spec(Context *, const char *, Node *, Node *, Loc);
const AccessClauses *combine_accesses(Context *, const AccessClauses *, const AccessClauses *, bool);
Type *access_type(Context *, Type *, const AccessClauses *);
void check_access_argument(Context *, Symbol *, const Symbol *, const Node *, AccessClause, IntegerRange,
                           AccessRangeSource);
IntegerRange integer_type_range(const Type *);
IntegerRange convert_integer_range(IntegerRange, const Type *, const Type *);
IntegerRange integer_expression_range(Context *, Node *, IntegerRangeLookup, void *);
Symbol *access_local_object(Context *, Symbol *, const Node *);
bool nonnull_parameter(Type *, size_t);
bool known_null_pointer(Context *, Node *);
bool call_returns_nonnull(Node *);
Node *pointer_value_source(Node *);
Type *type_int(Context *, unsigned rank, bool uns);
Type *type_float(Context *, unsigned);
Type *type_extended_float(Context *, FloatVariant);
Type *type_decimal(Context *, unsigned);
FloatVariant interchange_float_variant(const char *suffix);
Type *type_bfloat(Context *);
void interchange_float_extension(Context *, Loc, const char *);
Type *pointer(Context *, Type *);
Type *spec_type(Context *, Specs, Loc);
Type *declaration_type(Context *, Specs, Decl *, Loc);
Type *atomic_type(Context *, Type *, Loc);
Type *apply_decl(Context *, Type *, Decl *);
Specs merge_specs(Context *, Specs, Specs, Loc);
Decl *decl(Context *, DeclKind, Decl *, Loc);
Decl *decl_name(Decl *);
void enter_scope(Context *);
void leave_scope(Context *);
void finish_block_scope(Context *, Node *);
void enter_parameter_scope(Context *);
Decl *finish_parameters(Context *, Decl *, Loc);
void add_parameter(Context *, Decl *, Specs, Decl *, Loc);
void forward_parameters(Context *, Decl *, Loc);
Type *find_typedef(Context *, const char *);
Type *reference_typedef(Context *, Token, const PatchableEntries **);
void deprecated_type(Context *, Type *, const char *, const char *, Loc);
Node *node(Context *, NodeKind, Loc, Node *, Node *);
Node *integer(Context *, Loc, uint64_t, Type *);
Node *literal(Context *, Token);
Node *strings(Context *, Tokens);
const char *decode_narrow_string(Context *, Token);
Node *reference(Context *, const char *, Loc);
Node *unary(Context *, int, Node *, Loc);
Node *complex_component(Context *, Node *, bool, Loc);
Node *complex_builtin(Context *, Node *, Node *, Loc);
Node *vector_convert(Context *, Node *, Type *, Loc);
Node *constant_component(Context *, Node *, bool);
Node *subscript(Context *, Node *, Node *, Loc);
Node *binary(Context *, int, Node *, Node *, Loc);
Node *assign(Context *, int, Node *, Node *, bool, Loc);
Node *convert(Context *, Node *, Type *, Loc);
Node *cast_expression(Context *, Node *, Type *, Loc);
Node *conditional(Context *, Node *, Node *, Node *, Loc);
Node *comma_expression(Context *, Node *, Node *, Loc);
Node *call(Context *, Node *, Nodes, Loc);
Node *classify_type_call(Context *, Node *, Type *, Loc);
Node *call_with_static_chain(Context *, Node *, Node *, Loc);
Node *field(Context *, Node *, const char *, bool, Loc);
Node *measure(Context *, Type *, bool, Loc);
Node *measure_expression(Context *, Node *, Loc);
Node *measure_type(Context *, Type *, Loc);
void begin_unevaluated_operand(Context *);
void finish_unevaluated_operand(Context *, bool);
Type *typeof_expression(Context *, Node *, bool);
Node *decay(Context *, Node *);
Node *check_return(Context *, Node *, Loc);
bool constant_int(Context *, Node *, uint64_t *);
bool constant_integer(Context *, Node *, Integer128 *);
bool constant_truth(Context *, Node *, bool *);
bool expression_has_effect(Context *, Node *);
Specs call_diagnostic_spec(Context *, Token, Node *);
void warn_call_diagnostic_attributes(Context *, CallDiagnostics, Loc);
bool wide_operation(int, WideOperation *);
bool decimal_operation(int, DecimalOperation *);
Type *record_type(Context *, bool, const char *, Members *, Loc);
void finish_record_definition(Context *, Type *, Loc);
Node *type_declaration(Context *, Specs, Loc);
void record_attributes(Context *, Type *, bool packed, unsigned alignment, unsigned attributes, Loc);
Type *transparent_union_type(Context *, Type *, bool, Loc);
void designated_init_type(Context *, Type *, bool, Loc);
void add_member(Context *, Members *, Specs, Decl *, bool parameter, Loc);
unsigned alignment_value(Context *, Node *, Loc);
unsigned vector_size_value(Context *, Node *, Loc);
Type *aligned_type(Context *, Type *, unsigned alignment);
Node *has_attribute(Context *, Type *, Node *, const char *, Nodes, Loc);
Node *declare(Context *, Specs, Decl *, Node *, Loc);
void set_initializer(Context *, Node *, Node *);
Node *compound_literal(Context *, Type *, Nodes, Loc);
Node *generic_selection(Context *, Node *, Nodes, Loc);
Node *statement_expression(Context *, Node *, Loc);
Node *va_argument(Context *, Node *, Type *, Loc);
Node *member_offset(Context *, Type *, Node *, Loc);
Node *choose_expression(Context *, Node *, Node *, Node *, Loc);
Node *constant_query(Context *, Node *, Loc);
Type *unqualified_type(Context *, Type *);
Node *type_limit(Context *, Type *, bool, Loc);
Node *array_count(Context *, Type *, Loc);
Node *local_label(Context *, const char *, Loc);
Node *labeled_statement(Context *, const char *, Node *, bool, Loc);
Node *label_address(Context *, const char *, Loc);
Node *indirect_goto(Context *, Node *, Loc);
const char *asm_string(Context *, Node *, Loc);
Symbol *explicit_register_object(Context *, Node *, size_t *);
bool address_constant(Context *, Node *, Symbol **, uint64_t *);
Node *begin_function(Context *, Specs, Decl *, Loc);
void end_function(Context *, Node *);
void define_enumerator(Context *, const char *, Node *, const char *, Loc);
Type *enum_type(Context *, const char *, Type *, bool, Loc);
void begin_enum(Context *, const char *, Type *, Specs, Loc);
Type *end_enum(Context *, Specs);
bool scalar(Type *);
bool aggregate(Type *);
static inline bool indexed_elements(Type *t)
{
    return t->kind == TY_ARRAY || t->kind == TY_VECTOR;
}

static inline bool address_value(Type *t)
{
    return t->kind == TY_COMPLEX || t->kind == TY_VECTOR || aggregate(t);
}
bool same_type(Type *, Type *);
bool function_noreturn(Context *, const Symbol *);
bool function_returns_twice(Context *, const Symbol *);

typedef enum {
    I_IMMEDIATE, I_REAL, I_ADDRESS, I_LOAD, I_STORE, I_COPY, I_CAST, I_UNARY, I_BINARY,
    I_LABEL, I_JUMP, I_ZERO, I_CALL, I_RETURN, I_MEMCOPY, I_MEMZERO, I_VA_START, I_VA_ARG,
    I_BITLOAD, I_BITSTORE, I_STACK_SAVE, I_STACK_ALLOC, I_STACK_RESTORE, I_INDIRECT_JUMP, I_TRAP,
    I_SPECULATION_BARRIER, I_SYSTEM_REGISTER,
    I_PREFETCH, I_SETJMP, I_LONGJMP,
    I_FRAME, I_FRAME_RETURN, I_REGISTER_READ, I_REGISTER_WRITE,
    I_FLOW_STATEMENT, I_FALLTHROUGH, I_CASE_BOUNDARY, I_NOP, I_NONNULL_COMPARE, I_NONNULL_WRITE
} Opcode;
typedef VECTOR(size_t) Values;
typedef VECTOR(Type *) Types;
typedef struct {
    Opcode code;
    Loc loc;
    size_t dest, a, b, label;
    int op;
    uint64_t immediate, immediate_high;
    long double real;
    unsigned bit, width;
    Type *type, *source_type;
    Symbol *symbol;
    union { const Node *access_call; Node *branch_condition, *stored_value; };
    size_t *access_argument_starts;
    Values args;
    Types arg_types;
} Inst;
typedef VECTOR(Inst) Instructions;
typedef struct { Type *type; unsigned alignment; } FrameSlot;
typedef struct {
    Symbol *function;
    VECTOR(FrameSlot) slots;
    Instructions instructions;
    uint64_t dynamic_stack_bound;
    bool has_dynamic_stack, dynamic_stack_unbounded;
} FunctionIR;

bool lower(Context *, Symbol *, FunctionIR *);
size_t layout_frame_slot(size_t *, FrameSlot);
void check_frame_size(Context *, const Symbol *, size_t);
void check_stack_usage(Context *, const FunctionIR *, size_t, size_t, bool);
bool emit_aarch64(Context *, FILE *);
bool emit_x86_64(Context *, FILE *);
uint64_t float_bits(long double, unsigned format);
bool real_constant(Context *, Node *, long double *, bool);
bool quad_constant(Context *, Node *, Integer128 *, bool);
bool binary_constant_bits(Context *, Node *, Integer128 *, bool);
bool decimal_constant(Context *, Node *, Integer128 *, bool);
bool extended_float_bits(Context *, long double, uint64_t bits[2], Loc);
void emit_static_data(Context *, FILE *, Symbol *);
void emit_symbol_linkage(FILE *, Symbol *);
bool symbol_may_be_preempted(const Symbol *);
bool call_through_got(Context *, const Symbol *);
void emit_function_entry(Context *, FILE *, Symbol *, unsigned);
void emit_lifecycle_functions(Context *, FILE *);

#endif
