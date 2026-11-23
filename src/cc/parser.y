%code top {
/* Keep the initial parser stack bounded; Bison grows it for deep input. */
#define YYINITDEPTH 128
}
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

static FloatVariant interchange_type(const char *name)
{
    return strncmp(name, "_Float", 6) ? FLOAT_DEFAULT : interchange_float_variant(name + 6);
}

static unsigned decimal_type_width(const char *name)
{
    if (equal(name, "_Decimal32")) return 4;
    if (equal(name, "_Decimal64")) return 8;
    if (equal(name, "_Decimal128")) return 16;
    return 0;
}

static TypeAttributes combine_type_attributes(Context *c, TypeAttributes a, TypeAttributes b)
{
    if (!a.len || !b.len)
        return a.len ? a : b;
    TypeAttributes result = {0};
    for (size_t i = 0; i < a.len; i++)
        PUSH(c, result, a.data[i]);
    for (size_t i = 0; i < b.len; i++)
        PUSH(c, result, b.data[i]);
    return result;
}

static CallDiagnostics combine_call_diagnostics(Context *c, CallDiagnostics a, CallDiagnostics b)
{
    if (!a.len || !b.len)
        return a.len ? a : b;
    CallDiagnostics result = {0};
    for (size_t i = 0; i < a.len; i++)
        PUSH(c, result, a.data[i]);
    for (size_t i = 0; i < b.len; i++)
        PUSH(c, result, b.data[i]);
    return result;
}

static unsigned combine_markers(Context *c, unsigned previous, unsigned added, Loc loc)
{
    if (((previous & MARK_HOT) && (added & MARK_COLD)) ||
        ((previous & MARK_COLD) && (added & MARK_HOT))) {
        bool cold = (previous & MARK_HOT) != 0;
        warning_at(c, WARNING_ATTRIBUTES, loc, "ignoring '%s' attribute because it conflicts with '%s'",
                   cold ? "cold" : "hot", cold ? "hot" : "cold");
        added &= ~(unsigned)(cold ? MARK_COLD : MARK_HOT);
    }
    return previous | added;
}

static Specs combine_specs(Context *c, Specs a, Specs b, Loc loc)
{
    if (a.visibility && b.visibility && a.visibility != b.visibility)
        error(c, loc, "conflicting visibility attributes in declaration");
    if (a.assumed_alignment && b.assumed_alignment &&
        (a.assumed_alignment->alignment != b.assumed_alignment->alignment ||
         a.assumed_alignment->offset != b.assumed_alignment->offset))
        warning_at(c, WARNING_ATTRIBUTES, loc, "conflicting assume_aligned attributes; keeping the first");
    if (a.allocation_alignment && b.allocation_alignment && a.allocation_alignment != b.allocation_alignment)
        warning_at(c, WARNING_ATTRIBUTES, loc, "conflicting alloc_align attributes; keeping the first");
    return (Specs){
        .flags = a.flags | b.flags,
        .type = a.type ? a.type : b.type,
        .alias = b.alias ? b.alias : a.alias,
        .ifunc = b.ifunc ? b.ifunc : a.ifunc,
        .cleanup = b.cleanup ? b.cleanup : a.cleanup,
        .section = b.section ? b.section : a.section,
        .deprecated = b.deprecated ? b.deprecated : a.deprecated,
        .visibility = a.visibility ? a.visibility : b.visibility,
        .iso_alignment = a.iso_alignment > b.iso_alignment ? a.iso_alignment : b.iso_alignment,
        .gnu_alignment = a.gnu_alignment > b.gnu_alignment ? a.gnu_alignment : b.gnu_alignment,
        .alignment_attributes = a.alignment_attributes | b.alignment_attributes,
        .type_attributes = combine_type_attributes(c, a.type_attributes, b.type_attributes),
        .call_diagnostics = combine_call_diagnostics(c, a.call_diagnostics, b.call_diagnostics),
        .sentinels = combine_sentinels(c, b.sentinels, a.sentinels),
        .format_spec = combine_format_specs(c, a.format_spec, b.format_spec),
        .format_args = combine_format_arguments(c, a.format_args, b.format_args),
        .nullability = combine_nullability(c, a.nullability, b.nullability),
        .accesses = combine_accesses(c, a.accesses, b.accesses, true),
        .constructor_priority = b.constructor_priority ? b.constructor_priority : a.constructor_priority,
        .destructor_priority = b.destructor_priority ? b.destructor_priority : a.destructor_priority,
        .attribute_markers = combine_markers(c, a.attribute_markers, b.attribute_markers, loc),
        .allocation = b.allocation.first ? b.allocation : a.allocation,
        .allocation_alignment = a.allocation_alignment ? a.allocation_alignment : b.allocation_alignment,
        .assumed_alignment = a.assumed_alignment ? a.assumed_alignment : b.assumed_alignment,
        .patchable_entries = combine_patchable_entries(c, a.patchable_entries, b.patchable_entries),
        .inherited_patchable_entries = a.type ? a.inherited_patchable_entries : b.inherited_patchable_entries,
        .has_alignas = a.has_alignas || b.has_alignas,
        .noreturn_specifier = a.noreturn_specifier || b.noreturn_specifier,
        .transparent_union = a.transparent_union || b.transparent_union,
        .designated_init = a.designated_init || b.designated_init
    };
}

static Specs declarator_specs(Context *c, Specs prefix, Specs suffix, Loc loc)
{
    /* A declarator's type attributes apply before the shared specifiers. */
    TypeAttributes attributes = prefix.type_attributes;
    prefix.type_attributes = suffix.type_attributes;
    suffix.type_attributes = attributes;
    /* Declarator frequency attributes also take precedence over specifiers. */
    unsigned frequency = MARK_HOT | MARK_COLD;
    unsigned prior = prefix.attribute_markers & frequency;
    prefix.attribute_markers = (prefix.attribute_markers & ~frequency) | (suffix.attribute_markers & frequency);
    suffix.attribute_markers = (suffix.attribute_markers & ~frequency) | prior;
    return combine_specs(c, prefix, suffix, loc);
}

static Decl *with_member_attributes(Context *c, Decl *d, Specs spec)
{
    if (spec.transparent_union)
        warning_at(c, WARNING_ATTRIBUTES, d->loc, "transparent_union attribute ignored on member");
    d->deprecated = spec.deprecated;
    d->designated_init = spec.designated_init;
    if (spec.visibility)
        warning_at(c, WARNING_ATTRIBUTES, d->loc, "visibility attribute ignored on member");
    warn_lifecycle_attributes(c, spec, d->loc);
    d->patchable_entries = combine_patchable_entries(c, d->patchable_entries, spec.patchable_entries);
    if (spec.cleanup)
        d->cleanup = spec.cleanup;
    if (spec.section)
        warning_at(c, WARNING_ATTRIBUTES, d->loc, "section attribute ignored on member");
    d->packed_member = (spec.flags & S_PACKED) != 0;
    d->member_alignment = spec.gnu_alignment;
    d->member_alignment_attributes = spec.alignment_attributes;
    d->member_attribute_markers = spec.attribute_markers &
                                  (MARK_NORETURN | MARK_WARN_UNUSED_RESULT | MARK_LEAF | MARK_HOT | MARK_COLD | MARK_RETURNS_TWICE);
    d->member_type_attributes = spec.type_attributes;
    d->member_allocation = spec.allocation;
    d->member_allocation_alignment = spec.allocation_alignment;
    d->member_assumed_alignment = spec.assumed_alignment;
    d->member_sentinels = spec.sentinels;
    d->member_format_spec = spec.format_spec;
    d->member_format_args = spec.format_args;
    d->member_nullability = spec.nullability;
    d->member_accesses = spec.accesses;
    return d;
}
}
%define api.pure full
%define api.location.type {Loc}
%define parse.error detailed
%define parse.lac full
%locations
%param { Context *c }
%initial-action { @$ = (Loc){.file = c->input, .line = 1, .column = 1,
                             .end_line = 1, .end_column = 1}; }

%union {
    Token token;
    const char *name;
    unsigned flags;
    int op;
    Node *node;
    Nodes nodes;
    Tokens tokens;
    Type *type;
    Decl *decl;
    Specs specs;
    Members members;
    Decls decls;
    bool flag;
}

%token <token> IDENT TYPE_NAME TYPE_LABEL NUMBER_TOKEN STRING_TOKEN CHAR_TOKEN ATTRIBUTE_NAME
%token DEPRECATED_ATTR DEPRECATED_MESSAGE TRANSPARENT_UNION_ATTR DESIGNATED_INIT_ATTR FALLTHROUGH_ATTR
%token DEFAULT_SENTINEL_ATTR
%token VOID BOOL CHAR SHORT INT LONG FLOAT DOUBLE SIGNED UNSIGNED INT128 COMPLEX
%token FLOAT16 FP16 BF16
%token <token> EXTENDED_FLOAT
%token REAL_PART IMAG_PART COMPLEX_BUILTIN
%token TYPEDEF EXTERN STATIC AUTO REGISTER CONST VOLATILE RESTRICT INLINE NORETURN THREAD_LOCAL
%token ATOMIC_TYPE ATOMIC_QUALIFIER
%token STRUCT UNION ENUM ENUM_BASE_COLON
%token PACKED_ATTR GNU_INLINE_ATTR ALIGNED_ATTR DEFAULT_ALIGNED_ATTR WEAK_ATTR ALIAS_ATTR IFUNC_ATTR
%token WEAKREF_ATTR TARGETED_WEAKREF_ATTR
%token PACK_PRAGMA "#pragma pack"
%token WEAK_PRAGMA "#pragma weak"
%token VISIBILITY_PRAGMA "#pragma GCC visibility"
%token IF ELSE WHILE DO FOR RETURN BREAK CONTINUE SWITCH CASE DEFAULT GOTO
%token SIZEOF ALIGNAS ALIGNOF STATIC_ASSERT GENERIC VA_ARG TYPEOF OFFSETOF
%token GNU_ALIGNOF TYPEOF_UNQUAL COUNT_OF MIN_OF MAX_OF EXTENSION
%token TYPES_COMPATIBLE CHOOSE_EXPR CONSTANT_P HAS_ATTRIBUTE
%token STATIC_CHAIN_CALL VECTOR_CONVERT
%token TRUE_LITERAL FALSE_LITERAL
%token GNU_AUTO_TYPE
%token LOCAL_LABEL
%token ASM
%token INC DEC ARROW AND OR SHL SHR EQ NE LE GE ELLIPSIS
%token ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token SHL_ASSIGN SHR_ASSIGN AND_ASSIGN XOR_ASSIGN OR_ASSIGN
%precedence TYPE_NAME
%precedence PREFER_TYPE
%precedence ENUM_BASE_COMPLETE
%precedence CONST VOLATILE RESTRICT ATOMIC_QUALIFIER PACKED_ATTR ALIGNED_ATTR DEFAULT_ALIGNED_ATTR
%precedence VISIBILITY_ATTR
%precedence <token> LIFECYCLE_ATTR DEFAULT_LIFECYCLE_ATTR MARKER_ATTR
%precedence <token> CALL_DIAGNOSTIC_ATTR
%precedence ALLOC_SIZE_ATTR ALLOC_ALIGN_ATTR ASSUME_ALIGNED_ATTR PATCHABLE_ENTRY_ATTR ACCESS_ATTR SENTINEL_ATTR DEFAULT_SENTINEL_ATTR
%precedence NONNULL_ATTR DEFAULT_NONNULL_ATTR RETURNS_NONNULL_ATTR
%precedence FORMAT_ATTR FORMAT_ARG_ATTR CLEANUP_ATTR
%precedence SECTION_ATTR
%precedence DEPRECATED_ATTR DEPRECATED_MESSAGE TRANSPARENT_UNION_ATTR DESIGNATED_INIT_ATTR
%precedence VECTOR_SIZE_ATTR MODE_ATTR
%precedence IF_WITHOUT_ELSE
%precedence ELSE

%type <name> name label_name tag_opt asm_name
%type <name> queried_attribute
%type <flags> mod_flags builtin enum_scalar
%type <specs> specs declaration_head enum_spec mods mod builtin_list extended_float alignas_spec aligned_spec pointer_quals alias_spec
%type <specs> nested_attributes allocation_size_attribute sentinel_attribute nullability_attribute format_attribute format_argument_attribute
%type <specs> cleanup_spec
%type <specs> section_spec
%type <specs> vector_size_spec mode_spec
%type <specs> record_intro record_attrs declarator_attributes
%type <specs> visibility_spec
%type <specs> lifecycle_attribute deprecated_attribute enumerator_attrs
%type <specs> marker_attribute call_diagnostic_attribute patchable_entry_attribute access_attribute
%type <decl> declarator direct_declarator pointers abstract_opt abstract_declarator direct_abstract
%type <decl> brackets parameters parameter_list parameter_forwards function_parameters member_declarator
%type <decls> member_declarators
%type <type> record enum_base enum_storage_type type_name typeof_spec atomic_spec
%type <flags> record_kind
%type <flags> quals qualifier
%type <specs> enum_keyword enum_attributes
%type <members> member_list member_declaration
%type <node> compound block_item declaration init_declarators initialized declared initializer function_definition
%type <node> statement statement_body expression expression_opt assignment conditional logical_or logical_and
%type <node> bit_or bit_xor bit_and equality relation shift additive multiplicative cast unary
%type <node> postfix primary string_list for_init
%type <tokens> string_tokens
%type <node> initializer_item designator designators array_designator
%type <node> generic_association generic_control offset_path offset_tail
%type <nodes> block_items arguments arguments_opt initializer_list braced_list generic_associations
%type <nodes> attribute_query_arguments
%type <nodes> local_label_declarations local_label_list
%type <op> assignment_op unary_op

%%

translation_unit:
    %empty
  | translation_unit external
  ;

external:
    declaration
  | function_definition
  | static_assertion
  | declaration_pragma
  | ASM { error(c, @1, "inline assembly is not supported"); YYERROR; }
  | extension_prefix external { c->extension_depth--; }
  ;

function_definition:
    declaration_head declarator { $<node>$ = begin_function(c, $1, $2, @2); }
    compound { end_function(c, $4); $$ = $<node>3; }
  ;

extension_prefix:
    EXTENSION { c->extension_depth++; }
  ;

static_assertion:
    STATIC_ASSERT '(' conditional assertion_message ')' ';' {
      bool value;
      if (!constant_truth(c, $3, &value))
        error(c, @3, "static assertion requires an integer constant expression");
      else if (!value)
        error(c, @3, "static assertion failed");
    }
  ;

assertion_message:
    %empty {
      if (c->dialect.standard < C23)
        extension(c, @$, "static assertion without a message");
    }
  | ',' string_list
  ;

declaration_head:
    specs { c->declaration = $1; $$ = $1; }
  ;

declaration:
    declaration_head init_declarators ';' { $$ = $2; }
  | declaration_head ';' {
      Specs specs = $1;
      specs.flags &= ~(unsigned)S_ENUM_FORWARD;
      if (specs.has_alignas)
        warning(c, @1, "_Alignas has no effect in an empty declaration");
      $$ = type_declaration(c, specs, @1);
    }
  | mods GNU_AUTO_TYPE name '=' assignment ';' {
      extension(c, @2, "__auto_type");
      if ($1.flags & (S_TYPEDEF | S_EXTERN))
        error(c, @2, "invalid storage class for __auto_type");
      if ($5->width)
        error(c, @5, "__auto_type cannot infer a type from a bit-field");
      Node *value = decay(c, $5);
      Decl *d = decl(c, D_NAME, NULL, @3);
      d->name = $3;
      Specs specs = $1;
      specs.type = unqualified_type(c, value->type);
      $$ = declare(c, specs, d, NULL, @3);
      set_initializer(c, $$, value);
    }
  ;

init_declarators:
    initialized { $$ = node(c, N_SEQUENCE, @1, NULL, NULL); PUSH(c, $$->list, $1); }
  | init_declarators ',' initialized { $$ = $1; PUSH(c, $$->list, $3); }
  ;

declared:
    declarator asm_name declarator_attributes {
      Decl *id = decl_name($1);
      id->asm_name = $2;
      Specs spec = declarator_specs(c, c->declaration, $3, @3);
      if (c->declaration.cleanup) spec.cleanup = c->declaration.cleanup;
      $$ = declare(c, spec, $1, NULL, @1);
    }
  ;

declarator_attributes:
    %empty { $$ = (Specs){0}; }
  | declarator_attributes GNU_INLINE_ATTR { $$ = $1; $$.flags |= S_GNU_INLINE; }
  | declarator_attributes WEAK_ATTR { $$ = $1; $$.flags |= S_WEAK; }
  | declarator_attributes WEAKREF_ATTR { $$ = $1; $$.flags |= S_WEAKREF; }
  | declarator_attributes PACKED_ATTR { $$ = $1; $$.flags |= S_PACKED; }
  | declarator_attributes aligned_spec { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes alias_spec { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes visibility_spec { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes lifecycle_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes allocation_size_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes patchable_entry_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes access_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes sentinel_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes format_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes format_argument_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes nullability_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes deprecated_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes call_diagnostic_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes marker_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes FALLTHROUGH_ATTR {
      warning_at(c, WARNING_ATTRIBUTES, @2, "'fallthrough' attribute is only valid on a null statement");
      $$ = $1;
    }
  | declarator_attributes TRANSPARENT_UNION_ATTR { $$ = $1; $$.transparent_union = true; }
  | declarator_attributes DESIGNATED_INIT_ATTR { $$ = $1; $$.designated_init = true; }
  | declarator_attributes cleanup_spec { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes section_spec { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes vector_size_spec { $$ = combine_specs(c, $1, $2, @2); }
  | declarator_attributes mode_spec { $$ = combine_specs(c, $1, $2, @2); }
  ;

asm_name:
    %empty { $$ = NULL; }
  | ASM '(' string_list ')' { $$ = asm_string(c, $3, @1); }
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
  | '{' '}' {
      if (c->dialect.standard < C23)
        extension(c, @1, "empty initializer");
      $$ = (Nodes){0};
    }
  ;

initializer_list:
    initializer_item { $$ = (Nodes){0}; PUSH(c, $$, $1); }
  | initializer_list ',' initializer_item { $$ = $1; PUSH(c, $$, $3); }
  ;

initializer_item:
    initializer
  | designators '=' initializer { $$ = node(c, N_DESIGNATE, @1, $1, $3); }
  | array_designator initializer {
      extension(c, @2, "obsolete initializer designator without '='");
      $$ = node(c, N_DESIGNATE, @1, $1, $2);
    }
  | name ':' initializer {
      extension(c, @2, "obsolete initializer designator with ':'");
      Node *selector = node(c, N_DESIGNATE, @1, NULL, NULL);
      selector->name = $1;
      $$ = node(c, N_DESIGNATE, @1, selector, $3);
    }
  ;

designators:
    designator
  | designator designators { $$ = $1; $$->right = $2; }
  ;

designator:
    '.' name { $$ = node(c, N_DESIGNATE, @1, NULL, NULL); $$->name = $2; }
  | array_designator
  ;

array_designator:
    '[' conditional ']' { $$ = node(c, N_DESIGNATE, @1, $2, NULL); }
  | '[' conditional ELLIPSIS conditional ']' {
      extension(c, @3, "initializer range");
      $$ = node(c, N_DESIGNATE, @1, $2, NULL); $$->third = $4;
    }
  ;

specs:
    mods builtin_list { $$ = combine_specs(c, $1, $2, @2); }
  | mods BF16 mods { $$ = combine_specs(c, $1, $3, @3); $$.type = type_bfloat(c); }
  | mods TYPE_NAME mods {
      $$ = combine_specs(c, $1, $3, @3);
      $$.type = reference_typedef(c, $2, &$$.inherited_patchable_entries);
    }
  | mods record mods { $$ = combine_specs(c, $1, $3, @3); $$.type = $2; }
  | mods enum_spec mods { $$ = combine_specs(c, combine_specs(c, $1, $2, @2), $3, @3); }
  | mods typeof_spec mods { $$ = combine_specs(c, $1, $3, @3); $$.type = $2; }
  | mods atomic_spec mods { $$ = combine_specs(c, $1, $3, @3); $$.type = $2; }
  ;

atomic_spec:
    ATOMIC_TYPE '(' type_name ')' { $$ = atomic_type(c, $3, @1); }
  ;

typeof_spec:
    TYPEOF unevaluated_operand '(' type_name ')' {
      $$ = $4;
      finish_unevaluated_operand(c, true);
    }
  | TYPEOF unevaluated_operand '(' expression ')' {
      $$ = typeof_expression(c, $4, false);
      finish_unevaluated_operand(c, $$->typeof_operand != NULL);
    }
  | TYPEOF_UNQUAL unevaluated_operand '(' type_name ')' {
      $$ = unqualified_type(c, $4);
      finish_unevaluated_operand(c, true);
    }
  | TYPEOF_UNQUAL unevaluated_operand '(' expression ')' {
      $$ = typeof_expression(c, $4, true);
      finish_unevaluated_operand(c, $$->typeof_operand != NULL);
    }
  ;

unevaluated_operand:
    %empty { begin_unevaluated_operand(c); }
  ;

mods:
    %empty %prec PREFER_TYPE { $$ = (Specs){0}; }
  | mods mod { $$ = combine_specs(c, $1, $2, @2); }
  ;

mod:
    mod_flags { $$ = (Specs){.flags = $1}; }
  | alignas_spec
  | aligned_spec
  | alias_spec
  | visibility_spec
  | lifecycle_attribute
  | marker_attribute
  | NORETURN {
      if (c->dialect.standard < C11)
        extension(c, @1, "_Noreturn");
      $$ = (Specs){.noreturn_specifier = true};
    }
  | allocation_size_attribute
  | patchable_entry_attribute
  | access_attribute
  | sentinel_attribute
  | format_attribute
  | format_argument_attribute
  | nullability_attribute
  | cleanup_spec
  | deprecated_attribute
  | call_diagnostic_attribute
  | TRANSPARENT_UNION_ATTR { $$ = (Specs){.transparent_union = true}; }
  | DESIGNATED_INIT_ATTR { $$ = (Specs){.designated_init = true}; }
  | section_spec
  | vector_size_spec
  | mode_spec
  ;

mode_spec:
    MODE_ATTR '(' ATTRIBUTE_NAME ')' {
      $$ = (Specs){0};
      PUSH(c, $$.type_attributes, ((TypeAttribute){.mode = $3.text, .loc = @1}));
    }
  | MODE_ATTR '(' assignment ')' {
      $$ = (Specs){0};
      warning_at(c, WARNING_ATTRIBUTES, @1, "mode attribute ignored because its argument is not a mode name");
    }
  ;

vector_size_spec:
    VECTOR_SIZE_ATTR '(' conditional ')' {
      $$ = (Specs){0};
      unsigned size = vector_size_value(c, $3, @3);
      if (size) PUSH(c, $$.type_attributes, ((TypeAttribute){.vector_size = size, .loc = @1}));
    }
  ;

section_spec:
    SECTION_ATTR '(' string_list ')' {
      const char *name = asm_string(c, $3, @1);
      bool valid = *name != 0;
      for (const unsigned char *ch = (const unsigned char *)name; *ch; ch++)
        if (*ch < 32 || *ch == 127)
          valid = false;
      if (!valid && !c->failed)
        error(c, @1, "section attribute requires a nonempty name without control characters");
      $$ = (Specs){.section = valid ? name : NULL};
    }
  ;

cleanup_spec:
    CLEANUP_ATTR '(' name ')' { $$ = (Specs){.cleanup = $3}; }
  ;

deprecated_attribute:
    DEPRECATED_ATTR { $$ = (Specs){.deprecated = ""}; }
  | DEPRECATED_MESSAGE '(' string_list ')' {
      $$ = (Specs){.deprecated = asm_string(c, $3, @1)};
    }
  ;

call_diagnostic_attribute:
    CALL_DIAGNOSTIC_ATTR '(' unevaluated_operand assignment ')' {
      finish_unevaluated_operand(c, false);
      $$ = call_diagnostic_spec(c, $1, $4);
    }
  ;

ignored_call_diagnostic:
    call_diagnostic_attribute { warn_call_diagnostic_attributes(c, $1.call_diagnostics, @1); }
  ;

allocation_size_attribute:
    ASSUME_ALIGNED_ATTR '(' conditional ')' {
      $$ = (Specs){.assumed_alignment = assumed_alignment_spec(c, $3, NULL, @1)};
    }
  | ASSUME_ALIGNED_ATTR '(' conditional ',' conditional ')' {
      $$ = (Specs){.assumed_alignment = assumed_alignment_spec(c, $3, $5, @1)};
    }
  | ALLOC_ALIGN_ATTR '(' conditional ')' {
      $$ = (Specs){.allocation_alignment = allocation_alignment_spec(c, $3, @1)};
    }
  | ALLOC_SIZE_ATTR '(' conditional ')' {
      $$ = (Specs){.allocation = allocation_size_spec(c, $3, NULL, @1)};
    }
  | ALLOC_SIZE_ATTR '(' conditional ',' conditional ')' {
      $$ = (Specs){.allocation = allocation_size_spec(c, $3, $5, @1)};
    }
  ;

access_attribute:
    ACCESS_ATTR '(' ATTRIBUTE_NAME ',' conditional ')' {
      $$ = (Specs){.accesses = access_spec(c, $3.text, $5, NULL, @1)};
    }
  | ACCESS_ATTR '(' ATTRIBUTE_NAME ',' conditional ',' conditional ')' {
      $$ = (Specs){.accesses = access_spec(c, $3.text, $5, $7, @1)};
    }
  ;

ignored_access:
    access_attribute { access_type(c, type_int(c, 3, false), $1.accesses); }
  ;

patchable_entry_attribute:
    PATCHABLE_ENTRY_ATTR '(' conditional ')' {
      $$ = (Specs){.patchable_entries = patchable_entry_spec(c, $3, NULL, @1)};
    }
  | PATCHABLE_ENTRY_ATTR '(' conditional ',' conditional ')' {
      $$ = (Specs){.patchable_entries = patchable_entry_spec(c, $3, $5, @1)};
    }
  ;

format_argument_attribute:
    FORMAT_ARG_ATTR '(' conditional ')' {
      $$ = (Specs){.format_args = format_argument_spec(c, $3, @1)};
    }
  ;

format_attribute:
    FORMAT_ATTR '(' ATTRIBUTE_NAME ',' conditional ',' conditional ')' {
      $$ = (Specs){.format_spec = format_attribute_spec(c, $3.text, $5, $7, @1)};
    }
  ;

sentinel_attribute:
    DEFAULT_SENTINEL_ATTR { $$ = (Specs){.sentinels = sentinel_spec(c, NULL, @1)}; }
  | SENTINEL_ATTR '(' conditional ')' { $$ = (Specs){.sentinels = sentinel_spec(c, $3, @1)}; }
  ;

nullability_attribute:
    DEFAULT_NONNULL_ATTR { $$ = (Specs){.nullability = nonnull_spec(c, (Nodes){0}, @1)}; }
  | NONNULL_ATTR '(' arguments_opt ')' { $$ = (Specs){.nullability = nonnull_spec(c, $3, @1)}; }
  | RETURNS_NONNULL_ATTR { $$ = (Specs){.nullability.returns_nonnull = true}; }
  ;

ignored_nullability:
    nullability_attribute { nullability_type(c, type_int(c, 3, false), $1.nullability, @1); }
  ;

ignored_sentinel:
    sentinel_attribute {
      if ($1.sentinels.len)
        warning_at(c, WARNING_ATTRIBUTES, @1, "'sentinel' attribute only applies to function types");
    }
  ;

lifecycle_attribute:
    DEFAULT_LIFECYCLE_ATTR { $$ = lifecycle_spec(c, $1, NULL); }
  | LIFECYCLE_ATTR '(' conditional ')' { $$ = lifecycle_spec(c, $1, $3); }
  ;

marker_attribute:
    MARKER_ATTR {
      Attribute kind = attribute_kind(c, $1.text);
      unsigned marker = kind == ATTR_USED ? MARK_USED :
                        kind == ATTR_NORETURN ? MARK_NORETURN :
                        kind == ATTR_RETURNS_TWICE ? MARK_RETURNS_TWICE :
                        kind == ATTR_NOPLT ? MARK_NOPLT :
                        kind == ATTR_LEAF ? MARK_LEAF :
                        kind == ATTR_HOT ? MARK_HOT :
                        kind == ATTR_COLD ? MARK_COLD :
                        kind == ATTR_WARN_UNUSED_RESULT ? MARK_WARN_UNUSED_RESULT : MARK_NOINLINE;
      $$ = (Specs){.attribute_markers = marker};
    }
  ;

visibility_spec:
    VISIBILITY_ATTR '(' string_list ')' {
      const char *name = asm_string(c, $3, @1);
      Visibility visibility = visibility_kind(name);
      if (!visibility && !c->failed)
        error(c, @1, "invalid symbol visibility '%s'", name);
      $$ = (Specs){.visibility = visibility};
    }
  ;

alias_spec:
    ALIAS_ATTR '(' string_list ')' { $$ = (Specs){.alias = asm_string(c, $3, @1)}; }
  | IFUNC_ATTR '(' string_list ')' { $$ = (Specs){.ifunc = asm_string(c, $3, @1)}; }
  | TARGETED_WEAKREF_ATTR '(' string_list ')' {
      $$ = (Specs){.flags = S_WEAKREF, .alias = asm_string(c, $3, @1)};
    }
  ;

aligned_spec:
    DEFAULT_ALIGNED_ATTR {
      $$ = (Specs){.gnu_alignment = 16, .alignment_attributes = DEFAULT_ALIGNED_ATTRIBUTE};
    }
  | ALIGNED_ATTR '(' conditional ')' {
      unsigned alignment = alignment_value(c, $3, @3);
      if (!alignment && !c->failed)
        warning_at(c, WARNING_ATTRIBUTES, @3, "aligned attribute requires a positive alignment");
      $$ = (Specs){.gnu_alignment = alignment, .alignment_attributes = alignment};
    }
  ;

alignas_spec:
    ALIGNAS '(' type_name ')' {
      if (c->dialect.standard < C11)
        extension(c, @1, "_Alignas");
      $$ = (Specs){.has_alignas = true, .iso_alignment =
                    alignment_value(c, measure(c, $3, true, @3), @3)};
      $$.alignment_attributes = $$.iso_alignment;
    }
  | ALIGNAS '(' conditional ')' {
      if (c->dialect.standard < C11)
        extension(c, @1, "_Alignas");
      $$ = (Specs){.has_alignas = true, .iso_alignment = alignment_value(c, $3, @3)};
      $$.alignment_attributes = $$.iso_alignment;
    }
  ;

mod_flags:
    CONST { $$ = S_CONST; }
  | ATOMIC_QUALIFIER { $$ = S_ATOMIC; }
  | VOLATILE { $$ = S_VOLATILE; }
  | RESTRICT { $$ = S_RESTRICT; }
  | INLINE { $$ = S_INLINE; }
  | TYPEDEF { $$ = S_TYPEDEF; }
  | STATIC { $$ = S_STATIC; }
  | EXTERN { $$ = S_EXTERN; }
  | AUTO { $$ = S_AUTO; }
  | REGISTER { $$ = S_REGISTER; }
  | THREAD_LOCAL { $$ = S_THREAD; }
  | PACKED_ATTR { $$ = S_PACKED; }
  | GNU_INLINE_ATTR { $$ = S_GNU_INLINE; }
  | WEAK_ATTR { $$ = S_WEAK; }
  | WEAKREF_ATTR { $$ = S_WEAKREF; }
  ;

builtin_list:
    builtin { $$ = (Specs){.flags = $1}; }
  | extended_float
  | builtin_list builtin { $$ = merge_specs(c, $1, (Specs){.flags = $2}, @2); }
  | builtin_list extended_float { $$ = merge_specs(c, $1, $2, @2); }
  | builtin_list mod { $$ = combine_specs(c, $1, $2, @2); }
  ;

extended_float:
    EXTENDED_FLOAT {
      unsigned width = decimal_type_width($1.text);
      FloatVariant variant = interchange_type($1.text);
      if (width || variant != FLOAT_DEFAULT)
        interchange_float_extension(c, @1, $1.text);
      else if (equal($1.text, "__float128"))
        variant = FLOAT128;
      $$ = (Specs){.type = width ? type_decimal(c, width) : type_extended_float(c, variant)};
    }
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
  | FLOAT16 { interchange_float_extension(c, @1, "_Float16"); $$ = S_FLOAT16; }
  | FP16 { $$ = S_FP16; }
  | COMPLEX { $$ = S_COMPLEX; }
  | SIGNED { $$ = S_SIGNED; }
  | UNSIGNED { $$ = S_UNSIGNED; }
  | INT128 { extension(c, @1, "__int128"); $$ = S_INT128; }
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
    record_kind record_attrs { $$ = $2; $$.flags |= $1; }
  ;

record_attrs:
    %empty { $$ = (Specs){0}; }
  | record_attrs PACKED_ATTR { $$ = $1; $$.flags |= S_PACKED; }
  | record_attrs aligned_spec { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs visibility_spec { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs lifecycle_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs allocation_size_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs patchable_entry_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs sentinel_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs nullability_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs access_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs deprecated_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs ignored_call_diagnostic { $$ = $1; }
  | record_attrs TRANSPARENT_UNION_ATTR { $$ = $1; $$.transparent_union = true; }
  | record_attrs DESIGNATED_INIT_ATTR { $$ = $1; $$.designated_init = true; }
  | record_attrs marker_attribute {
      $$ = $1;
      warn_invalid_noplt(c, $2, NULL, @2);
      if ($2.attribute_markers & MARK_LEAF)
        warning_at(c, WARNING_ATTRIBUTES, @2, "'leaf' attribute ignored");
      if ($2.attribute_markers & (MARK_HOT | MARK_COLD))
        warning_at(c, WARNING_ATTRIBUTES, @2, "'%s' attribute ignored",
                   $2.attribute_markers & MARK_HOT ? "hot" : "cold");
      $$.attribute_markers |= $2.attribute_markers & (MARK_NORETURN | MARK_WARN_UNUSED_RESULT);
    }
  | record_attrs cleanup_spec { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs section_spec { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs vector_size_spec { $$ = combine_specs(c, $1, $2, @2); }
  | record_attrs mode_spec { $$ = combine_specs(c, $1, $2, @2); }
  ;

tag_opt:
    %empty { $$ = NULL; }
  | name
  ;

record:
    record_intro name {
      if ($1.visibility) warning_at(c, WARNING_ATTRIBUTES, @1, "visibility attribute ignored on type");
      if ($1.attribute_markers & MARK_RETURNS_TWICE)
        warning_at(c, WARNING_ATTRIBUTES, @1, "returns_twice attribute ignored on record type");
      if ($1.attribute_markers & MARK_NORETURN)
        warning_at(c, WARNING_ATTRIBUTES, @1, "noreturn attribute ignored on record type");
      if ($1.attribute_markers & MARK_WARN_UNUSED_RESULT)
        warning_at(c, WARNING_ATTRIBUTES, @1, "warn_unused_result attribute ignored on record type");
      warn_lifecycle_attributes(c, $1, @1);
      if ($1.cleanup) warning_at(c, WARNING_ATTRIBUTES, @1, "cleanup attribute ignored on type");
      if ($1.section) warning_at(c, WARNING_ATTRIBUTES, @1, "section attribute ignored on type");
      if ($1.type_attributes.len) error(c, @1, "numeric type attribute cannot be applied to a structure or union type");
      $$ = record_type(c, $1.flags & 1, $2, NULL, @2);
      sentinel_type(c, $$, $1.sentinels, @1);
      nullability_type(c, $$, $1.nullability, @1);
      access_type(c, $$, $1.accesses);
      deprecated_type(c, $$, $2, $1.deprecated, @2);
      allocation_size_type(c, $$, $1.allocation, @1);
      allocation_alignment_type(c, $$, $1.allocation_alignment, @1);
      $$ = assumed_alignment_type(c, $$, $1.assumed_alignment, @1);
      if ($1.flags & S_PACKED) record_attributes(c, $$, true, 0, 0, @1);
      if ($1.transparent_union)
        $$ = transparent_union_type(c, $$, true, @1);
      if ($1.designated_init && $$->kind != TY_STRUCT)
        designated_init_type(c, $$, false, @1);
    }
  | record_intro tag_opt '{' { record_type(c, $1.flags & 1, $2, NULL, @2); }
    member_list '}' record_attrs %prec ENUM_BASE_COMPLETE {
      Loc loc = @1;
      loc.pack_alignment = @6.pack_alignment;
      $$ = record_type(c, $1.flags & 1, $2, &$5, loc);
      Specs attrs = combine_specs(c, $1, $7, @7);
      sentinel_type(c, $$, attrs.sentinels, loc);
      nullability_type(c, $$, attrs.nullability, loc);
      access_type(c, $$, attrs.accesses);
      if (attrs.visibility) warning_at(c, WARNING_ATTRIBUTES, loc, "visibility attribute ignored on type");
      if (attrs.attribute_markers & MARK_RETURNS_TWICE)
        warning_at(c, WARNING_ATTRIBUTES, loc, "returns_twice attribute ignored on record type");
      if (attrs.attribute_markers & MARK_NORETURN)
        warning_at(c, WARNING_ATTRIBUTES, loc, "noreturn attribute ignored on record type");
      if (attrs.attribute_markers & MARK_WARN_UNUSED_RESULT)
        warning_at(c, WARNING_ATTRIBUTES, loc, "warn_unused_result attribute ignored on record type");
      warn_lifecycle_attributes(c, attrs, loc);
      if (attrs.cleanup) warning_at(c, WARNING_ATTRIBUTES, loc, "cleanup attribute ignored on type");
      if (attrs.section) warning_at(c, WARNING_ATTRIBUTES, loc, "section attribute ignored on type");
      if (attrs.type_attributes.len) error(c, loc, "numeric type attribute cannot be applied to a structure or union type");
      allocation_size_type(c, $$, attrs.allocation, loc);
      allocation_alignment_type(c, $$, attrs.allocation_alignment, loc);
      $$ = assumed_alignment_type(c, $$, attrs.assumed_alignment, loc);
      if ((attrs.flags & S_PACKED) || attrs.gnu_alignment)
        record_attributes(c, $$, (attrs.flags & S_PACKED) != 0, attrs.gnu_alignment,
                          attrs.alignment_attributes, loc);
      if (attrs.deprecated)
        deprecated_type(c, $$, $2, attrs.deprecated, loc);
      finish_record_definition(c, $$, loc);
      if (attrs.transparent_union)
        $$ = transparent_union_type(c, $$, true, loc);
      if (attrs.designated_init)
        designated_init_type(c, $$, true, loc);
    }
  ;

member_list:
    %empty { $$ = (Members){0}; }
  | member_list declaration_pragma { $$ = $1; }
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
        add_member(c, &$$, $1, $2.data[i], false, @2);
    }
  | specs ';' {
      $$ = (Members){0};
      if ($1.type && $1.type->enumeration) {
        if ((!c->dialect.gnu || c->dialect.pedantic == PEDANTIC_ERROR) &&
            !suppress_system_diagnostic(c, @1))
          error(c, @1, "enum declaration does not declare a member");
        else
          warning(c, @1, "enum declaration does not declare a member");
      } else {
        add_member(c, &$$, $1, NULL, false, @1);
      }
    }
  ;

member_declarators:
    member_declarator { $$ = (Decls){0}; PUSH(c, $$, $1); }
  | member_declarators ',' member_declarator { $$ = $1; PUSH(c, $$, $3); }
  ;

member_declarator:
    declarator record_attrs { $$ = with_member_attributes(c, $1, $2); }
  | declarator ':' conditional record_attrs { $$ = with_member_attributes(c, $1, $4); $$->bit_width = $3; }
  | ':' conditional record_attrs {
      $$ = with_member_attributes(c, decl(c, D_NAME, NULL, @1), $3);
      $$->bit_width = $2;
    }
  ;

enum_spec:
    enum_keyword name {
      $$ = (Specs){.type = enum_type(c, $2, NULL, false, @2)};
      deprecated_type(c, $$.type, $2, $1.deprecated, @2);
    }
  | enum_keyword name enum_base {
      $$ = (Specs){.flags = S_ENUM_FORWARD, .type = enum_type(c, $2, $3, false, @2)};
      deprecated_type(c, $$.type, $2, $1.deprecated, @2);
    }
  | enum_definition_head enumerators optional_comma '}' enum_attributes %prec ENUM_BASE_COMPLETE {
      $$ = (Specs){.type = end_enum(c, $5)};
    }
  ;

enum_keyword:
    ENUM { $$ = (Specs){0}; }
  | enum_keyword PACKED_ATTR { $$ = $1; $$.flags |= S_PACKED; }
  | enum_keyword mode_spec { $$ = combine_specs(c, $1, $2, @2); }
  | enum_keyword aligned_spec { $$ = $1; }
  | enum_keyword visibility_spec { $$ = $1; warning_at(c, WARNING_ATTRIBUTES, @2, "visibility attribute ignored on type"); }
  | enum_keyword lifecycle_attribute { $$ = $1; warn_lifecycle_attributes(c, $2, @2); }
  | enum_keyword marker_attribute {
      $$ = $1; warn_invalid_noplt(c, $2, NULL, @2);
      if ($2.attribute_markers & MARK_LEAF)
        warning_at(c, WARNING_ATTRIBUTES, @2, "'leaf' attribute ignored");
      if ($2.attribute_markers & (MARK_HOT | MARK_COLD))
        warning_at(c, WARNING_ATTRIBUTES, @2, "'%s' attribute ignored",
                   $2.attribute_markers & MARK_HOT ? "hot" : "cold");
    }
  | enum_keyword patchable_entry_attribute { $$ = $1; }
  | enum_keyword deprecated_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | enum_keyword ignored_call_diagnostic { $$ = $1; }
  | enum_keyword ignored_sentinel { $$ = $1; }
  | enum_keyword ignored_nullability { $$ = $1; }
  | enum_keyword ignored_access { $$ = $1; }
  | enum_keyword DESIGNATED_INIT_ATTR {
      $$ = $1;
      error(c, @2, "designated_init attribute is only valid on structure types");
    }
  | enum_keyword TRANSPARENT_UNION_ATTR {
      $$ = $1;
      warning_at(c, WARNING_ATTRIBUTES, @2, "transparent_union attribute ignored on enum");
    }
  | enum_keyword cleanup_spec { $$ = $1; warning_at(c, WARNING_ATTRIBUTES, @2, "cleanup attribute ignored on type"); }
  | enum_keyword section_spec { $$ = $1; warning_at(c, WARNING_ATTRIBUTES, @2, "section attribute ignored on type"); }
  | enum_keyword allocation_size_attribute {
      $$ = $1;
      if ($2.allocation.first) warning(c, @2, "alloc_size applies to function types only");
      if ($2.allocation_alignment) warning(c, @2, "alloc_align applies to function types only");
      if ($2.assumed_alignment) warning(c, @2, "assume_aligned applies to function types only");
    }
  ;

enum_attributes:
    %empty { $$ = (Specs){0}; }
  | enum_attributes PACKED_ATTR { $$ = $1; $$.flags |= S_PACKED; }
  | enum_attributes mode_spec { $$ = combine_specs(c, $1, $2, @2); }
  | enum_attributes aligned_spec { $$ = $1; }
  | enum_attributes visibility_spec { $$ = $1; warning_at(c, WARNING_ATTRIBUTES, @2, "visibility attribute ignored on type"); }
  | enum_attributes lifecycle_attribute { $$ = $1; warn_lifecycle_attributes(c, $2, @2); }
  | enum_attributes marker_attribute {
      $$ = $1; warn_invalid_noplt(c, $2, NULL, @2);
      if ($2.attribute_markers & MARK_LEAF)
        warning_at(c, WARNING_ATTRIBUTES, @2, "'leaf' attribute ignored");
      if ($2.attribute_markers & (MARK_HOT | MARK_COLD))
        warning_at(c, WARNING_ATTRIBUTES, @2, "'%s' attribute ignored",
                   $2.attribute_markers & MARK_HOT ? "hot" : "cold");
    }
  | enum_attributes patchable_entry_attribute { $$ = $1; }
  | enum_attributes deprecated_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | enum_attributes ignored_call_diagnostic { $$ = $1; }
  | enum_attributes ignored_sentinel { $$ = $1; }
  | enum_attributes ignored_nullability { $$ = $1; }
  | enum_attributes ignored_access { $$ = $1; }
  | enum_attributes DESIGNATED_INIT_ATTR {
      $$ = $1;
      error(c, @2, "designated_init attribute is only valid on structure types");
    }
  | enum_attributes TRANSPARENT_UNION_ATTR {
      $$ = $1;
      warning_at(c, WARNING_ATTRIBUTES, @2, "transparent_union attribute ignored on enum");
    }
  | enum_attributes cleanup_spec { $$ = $1; warning_at(c, WARNING_ATTRIBUTES, @2, "cleanup attribute ignored on type"); }
  | enum_attributes section_spec { $$ = $1; warning_at(c, WARNING_ATTRIBUTES, @2, "section attribute ignored on type"); }
  | enum_attributes allocation_size_attribute {
      $$ = $1;
      if ($2.allocation.first) warning(c, @2, "alloc_size applies to function types only");
      if ($2.allocation_alignment) warning(c, @2, "alloc_align applies to function types only");
      if ($2.assumed_alignment) warning(c, @2, "assume_aligned applies to function types only");
    }
  ;

enum_definition_head:
    enum_keyword '{' { begin_enum(c, NULL, NULL, $1, @1); }
  | enum_keyword name '{' { begin_enum(c, $2, NULL, $1, @1); }
  | enum_keyword enum_base '{' { begin_enum(c, NULL, $2, $1, @1); }
  | enum_keyword name enum_base '{' { begin_enum(c, $2, $3, $1, @1); }
  ;

enum_base:
    ENUM_BASE_COLON quals enum_storage_type quals %prec ENUM_BASE_COMPLETE {
      if (c->dialect.standard < C23)
        extension(c, @1, "fixed enum underlying type");
      $$ = spec_type(c, (Specs){.flags = $2 | $4, .type = $3}, @3);
    }
  ;

enum_storage_type:
    enum_scalar %prec ENUM_BASE_COMPLETE { $$ = spec_type(c, (Specs){.flags = $1}, @1); }
  | BF16 { $$ = type_bfloat(c); }
  | extended_float { $$ = $1.type; }
  | TYPE_NAME { $$ = reference_typedef(c, $1, NULL); }
  | typeof_spec
  | atomic_spec
  | ENUM name { $$ = enum_type(c, $2, NULL, false, @2); }
  ;

enum_scalar:
    builtin { $$ = $1; }
  | enum_scalar builtin { $$ = merge_specs(c, (Specs){.flags = $1}, (Specs){.flags = $2}, @2).flags; }
  | enum_scalar qualifier { $$ = $1 | $2; }
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
    name enumerator_attrs { define_enumerator(c, $1, NULL, $2.deprecated, @1); }
  | name enumerator_attrs '=' conditional { define_enumerator(c, $1, $4, $2.deprecated, @1); }
  ;

enumerator_attrs:
    %empty { $$ = (Specs){0}; }
  | enumerator_attrs deprecated_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | enumerator_attrs ignored_call_diagnostic { $$ = $1; }
  | enumerator_attrs ignored_sentinel { $$ = $1; }
  | enumerator_attrs ignored_nullability { $$ = $1; }
  | enumerator_attrs ignored_access { $$ = $1; }
  | enumerator_attrs marker_attribute {
      $$ = $1; warn_invalid_noplt(c, $2, NULL, @2);
      if ($2.attribute_markers & MARK_LEAF)
        warning_at(c, WARNING_ATTRIBUTES, @2, "'leaf' attribute ignored");
      if ($2.attribute_markers & (MARK_HOT | MARK_COLD))
        warning_at(c, WARNING_ATTRIBUTES, @2, "'%s' attribute ignored",
                   $2.attribute_markers & MARK_HOT ? "hot" : "cold");
    }
  | enumerator_attrs patchable_entry_attribute { $$ = $1; }
  ;

pointers:
    '*' pointer_quals {
      $$ = decl(c, D_POINTER, NULL, @1);
      $$->qualifiers = $2.flags;
      $$->alignment = $2.gnu_alignment;
      $$->allocation = $2.allocation;
      $$->allocation_alignment = $2.allocation_alignment;
      $$->assumed_alignment = $2.assumed_alignment;
      $$->attribute_markers = $2.attribute_markers & (MARK_NORETURN | MARK_WARN_UNUSED_RESULT | MARK_NOPLT | MARK_RETURNS_TWICE);
      $$->patchable_entries = $2.patchable_entries;
      $$->sentinels = $2.sentinels;
      $$->format_spec = $2.format_spec;
      $$->format_args = $2.format_args;
      $$->nullability = $2.nullability;
      $$->accesses = $2.accesses;
      $$->cleanup = $2.cleanup;
      $$->type_attributes = $2.type_attributes;
    }
  | '*' pointer_quals pointers {
      $$ = decl(c, D_POINTER, $3, @1);
      $$->qualifiers = $2.flags;
      $$->alignment = $2.gnu_alignment;
      $$->allocation = $2.allocation;
      $$->allocation_alignment = $2.allocation_alignment;
      $$->assumed_alignment = $2.assumed_alignment;
      $$->attribute_markers = $2.attribute_markers & (MARK_NORETURN | MARK_WARN_UNUSED_RESULT | MARK_NOPLT | MARK_RETURNS_TWICE);
      $$->patchable_entries = $2.patchable_entries;
      $$->sentinels = $2.sentinels;
      $$->format_spec = $2.format_spec;
      $$->format_args = $2.format_args;
      $$->nullability = $2.nullability;
      $$->accesses = $2.accesses;
      $$->cleanup = $2.cleanup;
      $$->type_attributes = $2.type_attributes;
    }
  ;

pointer_quals:
    %empty { $$ = (Specs){0}; }
  | pointer_quals qualifier { $$ = $1; $$.flags |= $2; }
  | pointer_quals aligned_spec {
      $$ = $1;
      if ($2.gnu_alignment)
        $$.gnu_alignment = $2.gnu_alignment;
    }
  | pointer_quals cleanup_spec { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals allocation_size_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals mode_spec { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals vector_size_spec { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals ignored_call_diagnostic { $$ = $1; }
  | pointer_quals patchable_entry_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals sentinel_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals format_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals format_argument_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals nullability_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals access_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | pointer_quals marker_attribute {
      $$ = $1;
      $$.attribute_markers = combine_markers(c, $1.attribute_markers,
          $2.attribute_markers & (MARK_NORETURN | MARK_WARN_UNUSED_RESULT | MARK_NOPLT | MARK_LEAF | MARK_HOT | MARK_COLD | MARK_RETURNS_TWICE), @2);
    }
  ;

quals:
    %empty { $$ = 0; }
  | quals qualifier { $$ = $1 | $2; }
  ;

qualifier:
    CONST { $$ = S_CONST; }
  | VOLATILE { $$ = S_VOLATILE; }
  | RESTRICT { $$ = S_RESTRICT; }
  | ATOMIC_QUALIFIER { $$ = S_ATOMIC; }
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
  | '(' nested_attributes declarator ')' {
      $$ = decl(c, D_ALIGNED, $3, @2);
      $$->alignment = $2.gnu_alignment;
      $$->allocation = $2.allocation;
      $$->allocation_alignment = $2.allocation_alignment;
      $$->assumed_alignment = $2.assumed_alignment;
      $$->sentinels = $2.sentinels;
      $$->format_spec = $2.format_spec;
      $$->format_args = $2.format_args;
      $$->nullability = $2.nullability;
      $$->accesses = $2.accesses;
      $$->cleanup = $2.cleanup;
      $$->type_attributes = $2.type_attributes;
      $$->attribute_markers = $2.attribute_markers & (MARK_WARN_UNUSED_RESULT | MARK_NOPLT | MARK_LEAF | MARK_HOT | MARK_COLD | MARK_RETURNS_TWICE);
      $$->patchable_entries = $2.patchable_entries;
    }
  | direct_declarator brackets { $$ = $2; $$->inner = $1; }
  | direct_declarator function_parameters { $$ = $2; $$->inner = $1; }
  ;

nested_attributes:
    aligned_spec
  | allocation_size_attribute
  | patchable_entry_attribute
  | sentinel_attribute
  | format_attribute
  | format_argument_attribute
  | nullability_attribute
  | access_attribute
  | cleanup_spec
  | mode_spec
  | vector_size_spec
  | marker_attribute
  | ignored_call_diagnostic { $$ = (Specs){0}; }
  | nested_attributes aligned_spec {
      $$ = combine_specs(c, $1, $2, @2);
      $$.gnu_alignment = $2.gnu_alignment ? $2.gnu_alignment : $1.gnu_alignment;
    }
  | nested_attributes allocation_size_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes patchable_entry_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes sentinel_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes format_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes format_argument_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes nullability_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes access_attribute { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes cleanup_spec { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes mode_spec { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes vector_size_spec { $$ = combine_specs(c, $1, $2, @2); }
  | nested_attributes marker_attribute {
      $$ = $1;
      $$.attribute_markers = combine_markers(c, $1.attribute_markers,
          $2.attribute_markers & (MARK_WARN_UNUSED_RESULT | MARK_NOPLT | MARK_LEAF | MARK_HOT | MARK_COLD | MARK_RETURNS_TWICE), @2);
    }
  | nested_attributes ignored_call_diagnostic { $$ = $1; }
  ;

function_parameters:
    '(' parameter_scope parameters ')' { $$ = finish_parameters(c, $3, @4); }
  ;

parameter_scope:
    %empty %prec PREFER_TYPE { enter_parameter_scope(c); }
  ;

parameters:
    %empty { $$ = decl(c, D_FUNCTION, NULL, @$); }
  | parameter_list
  | parameter_list ',' ELLIPSIS { $$ = $1; $$->variadic = true; }
  ;

parameter_list:
    specs declarator declarator_attributes {
      $$ = decl(c, D_FUNCTION, NULL, @1); $$->prototype = true;
      add_parameter(c, $$, declarator_specs(c, $1, $3, @3), $2, @2);
    }
  | specs abstract_opt {
      $$ = decl(c, D_FUNCTION, NULL, @1); $$->prototype = true;
      add_parameter(c, $$, $1, $2, @2);
    }
  | parameter_list ',' specs declarator declarator_attributes {
      $$ = $1;
      add_parameter(c, $$, declarator_specs(c, $3, $5, @5), $4, @4);
    }
  | parameter_list ',' specs abstract_opt { $$ = $1; add_parameter(c, $$, $3, $4, @4); }
  | parameter_forwards specs declarator declarator_attributes {
      $$ = $1;
      add_parameter(c, $$, declarator_specs(c, $2, $4, @4), $3, @3);
    }
  | parameter_forwards specs abstract_opt { $$ = $1; add_parameter(c, $$, $2, $3, @3); }
  ;

parameter_forwards:
    parameter_list ';' { $$ = $1; forward_parameters(c, $$, @2); }
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
  | '(' nested_attributes abstract_declarator ')' {
      $$ = decl(c, D_ALIGNED, $3, @2);
      $$->alignment = $2.gnu_alignment;
      $$->allocation = $2.allocation;
      $$->allocation_alignment = $2.allocation_alignment;
      $$->assumed_alignment = $2.assumed_alignment;
      $$->sentinels = $2.sentinels;
      $$->format_spec = $2.format_spec;
      $$->format_args = $2.format_args;
      $$->nullability = $2.nullability;
      $$->accesses = $2.accesses;
      $$->cleanup = $2.cleanup;
      $$->type_attributes = $2.type_attributes;
      $$->attribute_markers = $2.attribute_markers & (MARK_WARN_UNUSED_RESULT | MARK_NOPLT);
      $$->patchable_entries = $2.patchable_entries;
    }
  | brackets
  | direct_abstract brackets { $$ = $2; $$->inner = $1; }
  | function_parameters
  | direct_abstract function_parameters { $$ = $2; $$->inner = $1; }
  ;

brackets:
    '[' pointer_quals expression_opt ']' {
      $$ = decl(c, D_ARRAY, NULL, @1);
      $$->bound = $3; $$->qualifiers = $2.flags; $$->alignment = $2.gnu_alignment;
      $$->type_attributes = $2.type_attributes;
    }
  | '[' pointer_quals STATIC pointer_quals assignment ']' {
      if ($2.flags && $4.flags)
        error(c, @3, "array parameter qualifiers must be on one side of static");
      Specs specs = combine_specs(c, $2, $4, @4);
      $$ = decl(c, D_ARRAY, NULL, @1);
      $$->bound = $5; $$->qualifiers = specs.flags; $$->alignment = specs.gnu_alignment;
      $$->type_attributes = specs.type_attributes;
    }
  | '[' pointer_quals '*' ']' {
      $$ = decl(c, D_ARRAY, NULL, @1); $$->array_star = true;
      $$->qualifiers = $2.flags; $$->alignment = $2.gnu_alignment;
      $$->type_attributes = $2.type_attributes;
    }
  ;

type_name:
    specs abstract_opt {
      warn_call_diagnostic_attributes(c, $1.call_diagnostics, @1);
      warn_invalid_noplt(c, $1, $2, @1);
      if ($1.has_alignas)
        error(c, @1, "_Alignas is not permitted in a type name");
      if ($1.noreturn_specifier)
        error(c, @1, "_Noreturn is not permitted in a type name");
      if ($1.cleanup) warning_at(c, WARNING_ATTRIBUTES, @1, "cleanup attribute ignored on type");
      $$ = declaration_type(c, $1, $2, @1);
      $$ = allocation_size_type(c, $$, $1.allocation, @1);
      $$ = allocation_alignment_type(c, $$, $1.allocation_alignment, @1);
      $$ = assumed_alignment_type(c, $$, $1.assumed_alignment, @1);
      $$ = aligned_type(c, $$, $1.gnu_alignment);
    }
  ;

compound:
    '{' { $<flag>$ = c->function_body; if (c->function_body) c->function_body = false; else enter_scope(c); }
    local_label_declarations block_items '}' {
      if ($<flag>2) c->function->function_end_loc = @5;
      Nodes items = $3;
      if (items.len) {
        for (size_t i = 0; i < $4.len; i++)
          PUSH(c, items, $4.data[i]);
      } else {
        items = $4;
      }
      $$ = node(c, N_BLOCK, @1, NULL, NULL); $$->list = items; finish_block_scope(c, $$);
    }
  ;

local_label_declarations:
    %empty { $$ = (Nodes){0}; }
  | local_label_declarations LOCAL_LABEL local_label_list ';' {
      $$ = $1;
      for (size_t i = 0; i < $3.len; i++)
        PUSH(c, $$, $3.data[i]);
    }
  ;

local_label_list:
    name { $$ = (Nodes){0}; PUSH(c, $$, local_label(c, $1, @1)); }
  | local_label_list ',' name { $$ = $1; PUSH(c, $$, local_label(c, $3, @3)); }
  ;

block_items:
    %empty { $$ = (Nodes){0}; }
  | block_items block_item { $$ = $1; if ($2) PUSH(c, $$, $2); }
  ;

block_item:
    declaration
  | function_definition
  | extension_prefix declaration { c->extension_depth--; $$ = $2; }
  | statement_body
  | declaration_pragma { $$ = NULL; }
  | static_assertion { $$ = NULL; }
  ;

for_init:
    expression_opt ';' { $$ = node(c, N_EXPR, @1, $1, NULL); }
  | declaration
  ;

statement:
    statement_body
  | declaration_pragma statement { $$ = $2; }
  ;

declaration_pragma:
    PACK_PRAGMA
  | WEAK_PRAGMA
  | VISIBILITY_PRAGMA
  ;

statement_body:
    compound
  | expression_opt ';' { $$ = node(c, N_EXPR, @1, $1, NULL); }
  | FALLTHROUGH_ATTR ';' { $$ = node(c, N_FALLTHROUGH, @1, NULL, NULL); }
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
      $$ = node(c, N_LOOP, @1, $5, $9); $$->third = $7; $$->fourth = $4;
      finish_block_scope(c, $$);
    }
  | SWITCH '(' expression ')' statement {
      if (c->function) c->function->has_switch = true;
      $$ = node(c, N_SWITCH, @1, $3, $5);
    }
  | CASE conditional ':' statement { $$ = node(c, N_CASE, @1, $2, $4); }
  | CASE conditional ELLIPSIS conditional ':' statement {
      extension(c, @3, "case range");
      $$ = node(c, N_CASE, @1, $2, $6); $$->third = $4;
    }
  | DEFAULT ':' statement { $$ = node(c, N_DEFAULT, @1, NULL, $3); }
  | BREAK ';' { $$ = node(c, N_BREAK, @1, NULL, NULL); }
  | CONTINUE ';' { $$ = node(c, N_CONTINUE, @1, NULL, NULL); }
  | GOTO name ';' { $$ = labeled_statement(c, $2, NULL, false, @2); }
  | GOTO '*' expression ';' { $$ = indirect_goto(c, $3, @1); }
  | ASM { error(c, @1, "inline assembly is not supported"); YYERROR; }
  | label_name ':' statement { $$ = labeled_statement(c, $1, $3, true, @1); }
  ;

expression_opt:
    %empty { $$ = NULL; }
  | expression
  ;

expression:
    assignment
  | expression ',' assignment { $$ = comma_expression(c, $1, $3, @2); }
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
  | extension_prefix cast { c->extension_depth--; $$ = $2; }
  | '(' type_name ')' cast {
      if ($2->kind == TY_STRUCT || $2->kind == TY_UNION)
        extension(c, @1, "cast to an aggregate type");
      $$ = cast_expression(c, $4, $2, @1);
    }
  ;

unary:
    postfix
  | AND name { $$ = label_address(c, $2, @1); }
  | INC unary { $$ = assign(c, '+', $2, integer(c, @1, 1, type_int(c, 3, false)), false, @1); }
  | DEC unary { $$ = assign(c, '-', $2, integer(c, @1, 1, type_int(c, 3, false)), false, @1); }
  | unary_op cast { $$ = unary(c, $1, $2, @1); }
  | REAL_PART cast { $$ = complex_component(c, $2, false, @1); }
  | IMAG_PART cast { $$ = complex_component(c, $2, true, @1); }
  | SIZEOF unevaluated_operand unary {
      if ($3->width) error(c, @1, "sizeof cannot be applied to a bit-field");
      $$ = measure_expression(c, $3, @1);
      finish_unevaluated_operand(c, $3->type->variable_size);
    }
  | SIZEOF unevaluated_operand '(' type_name ')' {
      $$ = measure_type(c, $4, @1);
      finish_unevaluated_operand(c, $4->variable_size);
    }
  | ALIGNOF unevaluated_operand '(' type_name ')' {
      $$ = measure(c, $4, true, @1);
      finish_unevaluated_operand(c, false);
    }
  | GNU_ALIGNOF unevaluated_operand '(' type_name ')' {
      $$ = measure(c, $4, true, @1);
      finish_unevaluated_operand(c, false);
    }
  | GNU_ALIGNOF unevaluated_operand unary {
      if ($3->width) error(c, @1, "alignment cannot be queried for a bit-field");
      $$ = $3->alignment ? integer(c, @1, $3->alignment, type_int(c, 4, true))
                         : measure(c, $3->type, true, @1);
      finish_unevaluated_operand(c, false);
    }
  | COUNT_OF '(' type_name ')' { $$ = array_count(c, $3, @1); }
  | COUNT_OF unary { $$ = array_count(c, $2->type, @1); }
  | MIN_OF '(' type_name ')' { $$ = type_limit(c, $3, false, @1); }
  | MAX_OF '(' type_name ')' { $$ = type_limit(c, $3, true, @1); }
  ;

unary_op:
    '&' { $$ = '&'; } | '*' { $$ = '*'; } | '+' { $$ = '+'; }
  | '-' { $$ = '-'; } | '~' { $$ = '~'; } | '!' { $$ = '!'; }
  ;

postfix:
    primary
  | '(' type_name ')' braced_list { $$ = compound_literal(c, $2, $4, @1); }
  | postfix '[' expression ']' { $$ = subscript(c, $1, $3, @2); }
  | postfix '(' arguments_opt ')' { $$ = call(c, $1, $3, @2); }
  | postfix '(' type_name ')' { $$ = classify_type_call(c, $1, $3, @2); }
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
  | TRUE_LITERAL { $$ = integer(c, @1, 1, spec_type(c, (Specs){.flags = S_BOOL}, @1)); }
  | FALSE_LITERAL { $$ = integer(c, @1, 0, spec_type(c, (Specs){.flags = S_BOOL}, @1)); }
  | NUMBER_TOKEN { $$ = literal(c, $1); }
  | CHAR_TOKEN { $$ = literal(c, $1); }
  | string_list
  | '(' expression ')' { $$ = $2; }
  | '(' compound ')' { $$ = statement_expression(c, $2, @1); }
  | GENERIC '(' generic_control ',' generic_associations ')' { $$ = generic_selection(c, $3, $5, @1); }
  | VA_ARG '(' assignment ',' type_name ')' { $$ = va_argument(c, $3, $5, @1); }
  | OFFSETOF '(' type_name ',' offset_path ')' { $$ = member_offset(c, $3, $5, @1); }
  | TYPES_COMPATIBLE '(' type_name ',' type_name ')' {
      $$ = integer(c, @1, same_type($3, $5), type_int(c, 3, false));
    }
  | HAS_ATTRIBUTE '(' unevaluated_operand type_name ',' queried_attribute attribute_query_arguments ')' {
      $$ = has_attribute(c, $4, NULL, $6, $7, @6);
      finish_unevaluated_operand(c, false);
    }
  | HAS_ATTRIBUTE '(' unevaluated_operand assignment ',' queried_attribute attribute_query_arguments ')' {
      $$ = has_attribute(c, $4->type, $4, $6, $7, @6);
      finish_unevaluated_operand(c, false);
    }
  | CHOOSE_EXPR '(' assignment ',' assignment ',' assignment ')' {
      $$ = choose_expression(c, $3, $5, $7, @1);
    }
  | CONSTANT_P '(' assignment ')' { $$ = constant_query(c, $3, @1); }
  | COMPLEX_BUILTIN '(' assignment ',' assignment ')' { $$ = complex_builtin(c, $3, $5, @1); }
  | VECTOR_CONVERT '(' assignment ',' type_name ')' { $$ = vector_convert(c, $3, $5, @1); }
  | STATIC_CHAIN_CALL '(' assignment ',' assignment ')' { $$ = call_with_static_chain(c, $3, $5, @1); }
  ;

queried_attribute:
    name {
      $$ = $1;
      Attribute kind = attribute_kind(c, $1);
      if (kind == ATTR_MODE || kind == ATTR_ACCESS || kind == ATTR_FORMAT) {
        size_t i = c->current;
        if (i && equal(c->tokens.data[i - 1].text, "("))
          i--;
        if (i + 2 < c->tokens.len && equal(c->tokens.data[i].text, "(") &&
            c->tokens.data[i + 1].kind == WORD && (kind == ATTR_ACCESS || kind == ATTR_FORMAT || equal(c->tokens.data[i + 2].text, ")")))
          c->tokens.data[i + 1].kind = ATTRIBUTE_IDENTIFIER;
      }
    }
  ;

attribute_query_arguments:
    %empty { $$ = (Nodes){0}; }
  | '(' arguments_opt ')' { $$ = $2; }
  | '(' ATTRIBUTE_NAME ')' {
      Node *argument = node(c, N_DESIGNATE, @2, NULL, NULL);
      argument->name = $2.text;
      $$ = (Nodes){0};
      PUSH(c, $$, argument);
    }
  | '(' ATTRIBUTE_NAME ',' arguments ')' {
      Node *argument = node(c, N_DESIGNATE, @2, NULL, NULL);
      argument->name = $2.text;
      $$ = (Nodes){0};
      PUSH(c, $$, argument);
      for (size_t i = 0; i < $4.len; i++)
        PUSH(c, $$, $4.data[i]);
    }
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

generic_control:
    unevaluated_operand assignment { $$ = $2; finish_unevaluated_operand(c, false); }
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
    STRING_TOKEN { $$ = strings(c, (Tokens){.data = &$1, .len = 1}); }
  | string_tokens { $$ = strings(c, $1); }
  ;

string_tokens:
    STRING_TOKEN STRING_TOKEN { $$ = (Tokens){0}; PUSH(c, $$, $1); PUSH(c, $$, $2); }
  | string_tokens STRING_TOKEN { $$ = $1; PUSH(c, $$, $2); }
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

static bool enum_base_colon(Context *c)
{
    size_t i = c->current;
    if (!c->enum_tag_context || i >= c->tokens.len || c->tokens.data[i].kind != WORD)
        return false;
    const char *word = c->tokens.data[i].text;
    if (find_typedef(c, word) || interchange_type(word) != FLOAT_DEFAULT || decimal_type_width(word))
        return true;
    static const char *const type_words[] = {
        "void", "_Bool", "char", "short", "int", "long", "signed", "unsigned", "float", "double", "__int128",
        "_Float16", "__fp16", "__bf16", "__float80", "__float128",
        "const", "__const", "__const__", "volatile", "__volatile", "__volatile__",
        "restrict", "__restrict", "__restrict__", "_Atomic", "enum", "struct", "union",
        "typeof", "__typeof", "__typeof__", "typeof_unqual", "__typeof_unqual", "__typeof_unqual__"
    };
    for (size_t j = 0; j < sizeof type_words / sizeof *type_words; j++)
        if (equal(word, type_words[j]))
            return true;
    return false;
}

static bool attribute_arguments(Context *c)
{
    unsigned depth = 1;
    while (depth && c->current < c->tokens.len) {
        const char *text = c->tokens.data[c->current++].text;
        if (equal(text, "("))
            depth++;
        else if (equal(text, ")"))
            depth--;
    }
    return !depth;
}

static bool attributes(Context *c, Loc loc, bool bracketed)
{
    size_t start = c->current - (bracketed ? 2 : 1), output = start;
    if (!bracketed && (!next_token(c, "(") || !next_token(c, "("))) {
        error(c, loc, "expected double parentheses after __attribute__");
        return false;
    }
    const char *close = bracketed ? "]" : ")";
    Token fallthrough = {0};
    while (c->current < c->tokens.len && !next_token(c, close)) {
        if (next_token(c, ","))
            continue;
        Token attr = c->tokens.data[c->current++];
        const char *namespace = NULL;
        if (bracketed && next_token(c, ":")) {
            if (!next_token(c, ":") || c->current >= c->tokens.len ||
                c->tokens.data[c->current].kind != WORD) {
                error(c, attr.loc, "expected a scoped attribute name");
                return false;
            }
            namespace = attr.text;
            attr = c->tokens.data[c->current++];
        }
        if (attr.kind != WORD) {
            error(c, attr.loc, "expected an attribute name");
            return false;
        }
        const char *name = attr.text;
        bool gnu = !bracketed || (namespace && (equal(namespace, "gnu") || equal(namespace, "__gnu__")));
        Attribute kind = gnu ? attribute_kind(c, name) : ATTR_NONE;
        bool arguments = next_token(c, "(");
        if (kind == ATTR_FALLTHROUGH) {
            if (arguments && !next_token(c, ")")) {
                if (!attribute_arguments(c)) {
                    error(c, attr.loc, "unterminated attribute '%s'", name);
                    return false;
                }
                warning_at(c, WARNING_ATTRIBUTES, attr.loc, "'fallthrough' attribute takes no arguments");
            }
            if (!fallthrough.text)
                fallthrough = attr;
            else
                warning_at(c, WARNING_ATTRIBUTES, attr.loc, "'fallthrough' attribute specified multiple times");
            continue;
        }
        if (kind == ATTR_NONE) {
            if (!attribute_ignored(attr.loc, namespace, name)) {
                const char *printed = namespace ? format(c, "%s::%s", namespace, name) : name;
                warning_at(c, WARNING_ATTRIBUTES, attr.loc, "unknown attribute '%s' ignored", printed);
            }
            if (arguments && !attribute_arguments(c)) {
                error(c, attr.loc, "unterminated attribute '%s'", name);
                return false;
            }
            continue;
        }
        if (kind == ATTR_STDCALL)
            warning_at(c, WARNING_ATTRIBUTES, attr.loc, "stdcall is ignored on %s", c->target->display_name);
        bool lifecycle = kind == ATTR_CONSTRUCTOR || kind == ATTR_DESTRUCTOR;
        bool call_diagnostic = kind == ATTR_ERROR || kind == ATTR_WARNING;
        if (kind == ATTR_ALIGNED || kind == ATTR_ALIAS || kind == ATTR_IFUNC || kind == ATTR_WEAKREF || kind == ATTR_VISIBILITY ||
            kind == ATTR_ALLOC_SIZE || kind == ATTR_ALLOC_ALIGN || kind == ATTR_ASSUME_ALIGNED || kind == ATTR_PATCHABLE_FUNCTION_ENTRY || kind == ATTR_ACCESS || kind == ATTR_FORMAT || kind == ATTR_FORMAT_ARG ||
            kind == ATTR_SENTINEL || kind == ATTR_NONNULL ||
            kind == ATTR_CLEANUP || kind == ATTR_SECTION ||
            kind == ATTR_VECTOR_SIZE || kind == ATTR_MODE || kind == ATTR_DEPRECATED || lifecycle ||
            call_diagnostic) {
            attr.kind = ATTRIBUTE;
            if (!arguments || next_token(c, ")")) {
                if (kind == ATTR_ALIAS || kind == ATTR_IFUNC || kind == ATTR_VISIBILITY || kind == ATTR_ALLOC_SIZE || kind == ATTR_ALLOC_ALIGN || kind == ATTR_ASSUME_ALIGNED ||
                    kind == ATTR_PATCHABLE_FUNCTION_ENTRY || kind == ATTR_ACCESS || kind == ATTR_FORMAT || kind == ATTR_FORMAT_ARG ||
                    kind == ATTR_CLEANUP || kind == ATTR_SECTION || kind == ATTR_VECTOR_SIZE ||
                    kind == ATTR_MODE || call_diagnostic) {
                    error(c, attr.loc, "%s attribute requires an argument", name);
                    return false;
                }
                if (kind == ATTR_ALIGNED)
                    attr.kind = DEFAULT_ALIGNMENT;
                if (kind == ATTR_SENTINEL)
                    attr.kind = DEFAULT_SENTINEL;
                if (kind == ATTR_NONNULL)
                    attr.kind = DEFAULT_NONNULL;
                if (lifecycle)
                    attr.kind = DEFAULT_LIFECYCLE;
                c->tokens.data[output++] = attr;
            } else {
                size_t first = c->current - 1;
                if (!attribute_arguments(c)) {
                    error(c, attr.loc, "unterminated attribute '%s'", name);
                    return false;
                }
                if (kind == ATTR_WEAKREF)
                    attr.kind = WEAKREF_ARGUMENT;
                if (kind == ATTR_DEPRECATED)
                    attr.kind = DEPRECATION_ARGUMENT;
                if ((kind == ATTR_ACCESS || kind == ATTR_FORMAT || (kind == ATTR_MODE && c->current - first == 3)) &&
                    c->tokens.data[first + 1].kind == WORD)
                    c->tokens.data[first + 1].kind = ATTRIBUTE_IDENTIFIER;
                if (call_diagnostic) {
                    /* The declaration attribute interprets message literals as diagnostic text. */
                    for (size_t i = first + 1; i + 1 < c->current; i++) {
                        Token *message = &c->tokens.data[i];
                        if (message->kind == STRING && strchr("LuU", message->text[0]))
                            message->text += message->text[0] == 'u' && message->text[1] == '8' ? 2 : 1;
                    }
                }
                c->tokens.data[output++] = attr;
                size_t count = c->current - first;
                memmove(c->tokens.data + output, c->tokens.data + first, count * sizeof(Token));
                output += count;
            }
        } else if (arguments && !((kind == ATTR_NORETURN || kind == ATTR_WARN_UNUSED_RESULT ||
                                    kind == ATTR_RETURNS_NONNULL || kind == ATTR_RETURNS_TWICE || kind == ATTR_NOPLT || kind == ATTR_LEAF ||
                                    kind == ATTR_HOT || kind == ATTR_COLD) &&
                                  next_token(c, ")"))) {
            error(c, attr.loc, "attribute '%s' does not take arguments", attr.text);
            return false;
        } else if (kind == ATTR_PACKED || kind == ATTR_GNU_INLINE || kind == ATTR_WEAK ||
                   kind == ATTR_NOINLINE || kind == ATTR_USED || kind == ATTR_NORETURN ||
                   kind == ATTR_WARN_UNUSED_RESULT || kind == ATTR_RETURNS_NONNULL || kind == ATTR_RETURNS_TWICE || kind == ATTR_NOPLT ||
                   kind == ATTR_LEAF || kind == ATTR_HOT || kind == ATTR_COLD ||
                   kind == ATTR_TRANSPARENT_UNION || kind == ATTR_DESIGNATED_INIT) {
            attr.kind = ATTRIBUTE;
            c->tokens.data[output++] = attr;
        }
    }
    if (!next_token(c, close)) {
        error(c, loc, "expected ')' after attribute list");
        return false;
    }
    if (fallthrough.text) {
        if (output != start || c->current >= c->tokens.len || !equal(c->tokens.data[c->current].text, ";")) {
            warning_at(c, WARNING_ATTRIBUTES, fallthrough.loc, "'fallthrough' attribute is only valid on a null statement");
        } else {
            fallthrough.kind = ATTRIBUTE;
            fallthrough.loc = loc;
            c->tokens.data[output++] = fallthrough;
        }
    }
    /* Retain attribute arguments as ordinary C tokens for the expression parser. */
    while (output < c->current)
        c->tokens.data[output++].kind = SKIPPED;
    if (c->enum_tag_context == 1)
        c->enum_attribute_end = c->current;
    c->current = start;
    return true;
}

static int read_token(YYSTYPE *value, Loc *loc, Context *c)
{
    Token t;
    for (;;) {
        if (c->current >= c->tokens.len)
            return 0;
        t = c->tokens.data[c->current++];
        if (t.kind == SKIPPED)
            continue;
        if (t.kind == PRAGMA_UNKNOWN) {
            warning_at(c, WARNING_UNKNOWN_PRAGMAS, t.loc, "ignoring #pragma %s", t.text);
            continue;
        }
        if (t.kind == PRAGMA_MESSAGE) {
            note(c, t.loc, format(c, "#pragma message: %s", t.text));
            continue;
        }
        bool bracketed = equal(t.text, "[") && next_token(c, "[");
        if (!bracketed && !equal(t.text, "__attribute__") && !equal(t.text, "__attribute"))
            break;
        if (bracketed && c->dialect.standard < C23)
            extension(c, t.loc, "bracketed attribute");
        if (!attributes(c, t.loc, bracketed)) {
            *loc = t.loc;
            return YYerror;
        }
    }
    *loc = t.loc;
    loc->pack_alignment = c->pack_alignment;
    loc->visibility = (unsigned char)c->pragma_visibility;
    value->token = t;
    if (t.kind == PRAGMA_PACK) {
        apply_pack_pragma(c, t);
        return PACK_PRAGMA;
    }
    if (t.kind == PRAGMA_WEAK) {
        apply_weak_pragma(c, t);
        return WEAK_PRAGMA;
    }
    if (t.kind == PRAGMA_VISIBILITY) {
        apply_visibility_pragma(c, t);
        return VISIBILITY_PRAGMA;
    }
    if (t.kind == DEFAULT_ALIGNMENT) return DEFAULT_ALIGNED_ATTR;
    if (t.kind == DEFAULT_LIFECYCLE) return DEFAULT_LIFECYCLE_ATTR;
    if (t.kind == DEFAULT_SENTINEL) return DEFAULT_SENTINEL_ATTR;
    if (t.kind == DEFAULT_NONNULL) return DEFAULT_NONNULL_ATTR;
    if (t.kind == WEAKREF_ARGUMENT) return TARGETED_WEAKREF_ATTR;
    if (t.kind == ATTRIBUTE_IDENTIFIER) return ATTRIBUTE_NAME;
    if (t.kind == DEPRECATION_ARGUMENT) return DEPRECATED_MESSAGE;
    if (t.kind == ATTRIBUTE) {
        Attribute kind = attribute_kind(c, t.text);
        switch (kind) {
        case ATTR_PACKED: return PACKED_ATTR;
        case ATTR_NOINLINE: case ATTR_USED: case ATTR_NORETURN: case ATTR_WARN_UNUSED_RESULT: case ATTR_NOPLT:
        case ATTR_LEAF: case ATTR_HOT: case ATTR_COLD: case ATTR_RETURNS_TWICE:
            return MARKER_ATTR;
        case ATTR_GNU_INLINE: return GNU_INLINE_ATTR;
        case ATTR_WEAK: return WEAK_ATTR;
        case ATTR_WEAKREF: return WEAKREF_ATTR;
        case ATTR_ALIAS: return ALIAS_ATTR;
        case ATTR_IFUNC: return IFUNC_ATTR;
        case ATTR_FORMAT: return FORMAT_ATTR;
        case ATTR_FORMAT_ARG: return FORMAT_ARG_ATTR;
        case ATTR_VISIBILITY: return VISIBILITY_ATTR;
        case ATTR_ALLOC_SIZE: return ALLOC_SIZE_ATTR;
        case ATTR_ALLOC_ALIGN: return ALLOC_ALIGN_ATTR;
        case ATTR_ASSUME_ALIGNED: return ASSUME_ALIGNED_ATTR;
        case ATTR_PATCHABLE_FUNCTION_ENTRY: return PATCHABLE_ENTRY_ATTR;
        case ATTR_ACCESS: return ACCESS_ATTR;
        case ATTR_SENTINEL: return SENTINEL_ATTR;
        case ATTR_NONNULL: return NONNULL_ATTR;
        case ATTR_RETURNS_NONNULL: return RETURNS_NONNULL_ATTR;
        case ATTR_CLEANUP: return CLEANUP_ATTR;
        case ATTR_SECTION: return SECTION_ATTR;
        case ATTR_VECTOR_SIZE: return VECTOR_SIZE_ATTR;
        case ATTR_MODE: return MODE_ATTR;
        case ATTR_DEPRECATED: return DEPRECATED_ATTR;
        case ATTR_TRANSPARENT_UNION: return TRANSPARENT_UNION_ATTR;
        case ATTR_DESIGNATED_INIT: return DESIGNATED_INIT_ATTR;
        case ATTR_FALLTHROUGH: return FALLTHROUGH_ATTR;
        case ATTR_ERROR: case ATTR_WARNING: return CALL_DIAGNOSTIC_ATTR;
        case ATTR_CONSTRUCTOR: case ATTR_DESTRUCTOR: return LIFECYCLE_ATTR;
        default: return ALIGNED_ATTR;
        }
    }
    if (t.kind == NUMBER) return NUMBER_TOKEN;
    if (t.kind == STRING) return STRING_TOKEN;
    if (t.kind == CHARACTER) return CHAR_TOKEN;
    if (t.kind == WORD) {
        if (c->target == &aarch64_linux && equal(t.text, "__fp16"))
            return FP16;
        if (interchange_type(t.text) != FLOAT_DEFAULT || decimal_type_width(t.text) ||
            (c->target == &x86_64_linux && (equal(t.text, "__float128") || equal(t.text, "__float80"))))
            return EXTENDED_FLOAT;
        if (equal(t.text, "_Atomic"))
            return c->current < c->tokens.len && equal(c->tokens.data[c->current].text, "(") ?
                   ATOMIC_TYPE : ATOMIC_QUALIFIER;
        switch (builtin_kind(t.text)) {
        case BUILTIN_VA_ARG: return VA_ARG;
        case BUILTIN_OFFSETOF: return OFFSETOF;
        case BUILTIN_TYPES_COMPATIBLE: return TYPES_COMPATIBLE;
        case BUILTIN_HAS_ATTRIBUTE: return HAS_ATTRIBUTE;
        case BUILTIN_CHOOSE_EXPR: return CHOOSE_EXPR;
        case BUILTIN_CONSTANT_P: return CONSTANT_P;
        case BUILTIN_COMPLEX: return COMPLEX_BUILTIN;
        case BUILTIN_VECTOR_CONVERT: return VECTOR_CONVERT;
        case BUILTIN_STATIC_CHAIN: return STATIC_CHAIN_CALL;
        default: break;
        }
    }
    if (c->dialect.standard >= C23) {
        if (equal(t.text, "bool")) return BOOL;
        if (equal(t.text, "alignof")) return ALIGNOF;
        if (equal(t.text, "static_assert")) return STATIC_ASSERT;
        if (equal(t.text, "thread_local")) return THREAD_LOCAL;
        if (equal(t.text, "typeof_unqual")) return TYPEOF_UNQUAL;
        if (equal(t.text, "true")) return TRUE_LITERAL;
        if (equal(t.text, "false")) return FALSE_LITERAL;
    }
    static const struct { const char *name; int code; } words[] = {
        {"void", VOID}, {"_Bool", BOOL}, {"char", CHAR}, {"short", SHORT},
        {"int", INT}, {"long", LONG}, {"float", FLOAT}, {"double", DOUBLE},
        {"_Float16", FLOAT16}, {"__bf16", BF16},
        {"_Complex", COMPLEX}, {"__complex__", COMPLEX}, {"__complex", COMPLEX},
        {"__real__", REAL_PART}, {"__real", REAL_PART},
        {"__imag__", IMAG_PART}, {"__imag", IMAG_PART},
        {"signed", SIGNED}, {"unsigned", UNSIGNED}, {"typedef", TYPEDEF},
        {"__int128", INT128},
        {"extern", EXTERN}, {"static", STATIC}, {"auto", AUTO}, {"register", REGISTER},
        {"const", CONST}, {"volatile", VOLATILE}, {"restrict", RESTRICT},
        {"__restrict", RESTRICT}, {"__restrict__", RESTRICT}, {"inline", INLINE},
        {"_Noreturn", NORETURN}, {"_Thread_local", THREAD_LOCAL},
        {"__thread", THREAD_LOCAL}, {"struct", STRUCT}, {"union", UNION}, {"enum", ENUM},
        {"if", IF}, {"else", ELSE}, {"while", WHILE}, {"do", DO}, {"for", FOR},
        {"return", RETURN}, {"break", BREAK}, {"continue", CONTINUE},
        {"switch", SWITCH}, {"case", CASE}, {"default", DEFAULT}, {"goto", GOTO},
        {"sizeof", SIZEOF}, {"_Alignas", ALIGNAS}, {"_Alignof", ALIGNOF}, {"_Static_assert", STATIC_ASSERT},
        {"_Generic", GENERIC},
        {"typeof", TYPEOF}, {"__typeof__", TYPEOF}, {"__typeof", TYPEOF},
        {"__typeof_unqual__", TYPEOF_UNQUAL}, {"__typeof_unqual", TYPEOF_UNQUAL},
        {"__alignof__", GNU_ALIGNOF}, {"__alignof", GNU_ALIGNOF},
        {"__inline__", INLINE}, {"__inline", INLINE},
        {"__const__", CONST}, {"__const", CONST},
        {"__volatile__", VOLATILE}, {"__volatile", VOLATILE},
        {"_Countof", COUNT_OF}, {"_Minof", MIN_OF}, {"_Maxof", MAX_OF},
        {"__extension__", EXTENSION},
        {"__auto_type", GNU_AUTO_TYPE},
        {"__label__", LOCAL_LABEL},
        {"asm", ASM}, {"__asm__", ASM}, {"__asm", ASM},
        {"++", INC}, {"--", DEC}, {"->", ARROW}, {"&&", AND}, {"||", OR},
        {"<<", SHL}, {">>", SHR}, {"==", EQ}, {"!=", NE}, {"<=", LE}, {">=", GE},
        {"...", ELLIPSIS}, {"+=", ADD_ASSIGN}, {"-=", SUB_ASSIGN}, {"*=", MUL_ASSIGN},
        {"/=", DIV_ASSIGN}, {"%=", MOD_ASSIGN}, {"<<=", SHL_ASSIGN}, {">>=", SHR_ASSIGN},
        {"&=", AND_ASSIGN}, {"^=", XOR_ASSIGN}, {"|=", OR_ASSIGN},
    };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (equal(t.text, words[i].name)) {
            if (equal(t.text, "inline") && c->dialect.standard == C90 && !c->dialect.gnu)
                break;
            if (equal(t.text, "typeof") && c->dialect.standard < C23 && !c->dialect.gnu)
                return find_typedef(c, t.text) ? TYPE_NAME : IDENT;
            if (equal(t.text, "typeof") && c->dialect.standard < C23)
                extension(c, t.loc, "typeof");
            if (equal(t.text, "asm")) {
                if (!c->dialect.gnu)
                    return find_typedef(c, t.text) ? TYPE_NAME : IDENT;
                extension(c, t.loc, "asm");
            }
            if (words[i].code == COUNT_OF && c->dialect.standard < C2Y)
                extension(c, t.loc, "_Countof");
            if (words[i].code == MIN_OF || words[i].code == MAX_OF)
                extension(c, t.loc, t.text);
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
    if (equal(t.text, ":") && enum_base_colon(c))
        return ENUM_BASE_COLON;
    if (t.kind == PUNCT && !t.text[1])
        return (unsigned char)t.text[0];
    error(c, t.loc, "unexpected token '%s'", t.text);
    return YYerror;
}

static int yylex(YYSTYPE *value, Loc *loc, Context *c)
{
    int code = read_token(value, loc, c);
    if (c->enum_tag_context == 1 && c->current <= c->enum_attribute_end)
        return code;
    if (code == ENUM)
        c->enum_tag_context = 1;
    else if (c->enum_tag_context == 1 && (code == IDENT || code == TYPE_NAME || code == TYPE_LABEL))
        c->enum_tag_context = 2;
    else if (c->enum_tag_context != 1 || code != PACKED_ATTR)
        c->enum_tag_context = 0;
    return code;
}

bool parse(Context *c, Tokens tokens)
{
    size_t count = 0;
    for (size_t i = 0; i < tokens.len; i++)
        if (tokens.data[i].kind != PRAGMA_DIAGNOSTIC)
            tokens.data[count++] = tokens.data[i];
    tokens.len = count;
    c->tokens = tokens;
    c->pack_alignment = c->default_pack_alignment;
    setup_types(c);
    if (yyparse(c) != 0 || c->failed)
        return false;
    finish_translation_unit(c);
    return !c->failed;
}
