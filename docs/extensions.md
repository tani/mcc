# GNU C extensions

GCC 16.2 defaults to `-std=gnu23`. Select an ISO baseline with `-std=c23 -Wpedantic`, or use `-std=c17 -Wpedantic` to spot features added after C17. Use `-pedantic-errors` to make required diagnostics errors. [GNU C chapter](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/C-Extensions.html) · [C dialect options](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/C-Dialect-Options.html)

## Expressions and control flow

- **Statement expressions** `({ int n = f(); n * n; })` use a block as an expression. The final expression supplies its value. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Statement-Exprs.html)
- **Block-local labels** `__label__ done;` declare a label local to a block, including one inside a macro's statement expression. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Local-Labels.html)
- **Computed `goto`** uses `&&label` to take the address of a label and `goto *ptr;` to branch to it within the same function. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Labels-as-Values.html)
- **Nested functions** can be defined inside functions and can refer to variables in their enclosing scope. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Nested-Functions.html)
- **Omitted middle operand** `x ?: fallback` returns `x` when it is nonzero and otherwise evaluates `fallback`. It evaluates `x` once. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Conditionals.html)
- **Case ranges** `case 1 ... 4:` match a closed interval. GCC also supports them in its experimental C2y mode. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Case-Ranges.html) · [C2y support](https://gcc.gnu.org/projects/c-status.html)

## Types and declarations

- **Type queries and inference** `typeof(expr)` names a type and `__auto_type n = expr;` infers a variable's type from its initializer. C23 standardized `typeof`, `typeof_unqual`, and `auto` type inference. `__typeof__` is GCC's alternate spelling for `typeof`. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Typeof.html) · [C23 support](https://gcc.gnu.org/projects/c-status.html)
- **Array element counts** `_Countof(array)` gives the outermost dimension of a complete array. GCC 16 also implements it in experimental C2y mode. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/_005fCountof.html) · [C2y support](https://gcc.gnu.org/projects/c-status.html)
- **Integer type limits** `_Minof(short)` and `_Maxof(int)` give the minimum and maximum representable values of the given integer types. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/_005fMaxof-and-_005fMinof.html)
- **Alignment queries** `__alignof__` can take a type, object, or function. ISO C11 added `_Alignof(type)`, and C23 provides the `alignof` keyword. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Alignment.html)
- **Member offsets** `__builtin_offsetof(T, member)` implements the standard `offsetof` macro and accepts nested member names and array subscripts. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Offsetof.html)
- **Enumeration declarations** GCC accepts fixed underlying types such as `enum color : unsigned char` in older C modes as an extension. C23 standardized the syntax. GCC also accepts incomplete `enum` forward declarations without a fixed underlying type. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Enum-Extensions.html)
- **Thread-local storage** `__thread` gives each thread its own instance of a variable on supported targets. The ISO C11 spelling is `_Thread_local`, and C23 provides the `thread_local` keyword. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Thread-Local.html)
- **Alternate keywords** include `__asm__`, `__inline__`, and `__typeof__`. `__extension__` suppresses pedantic diagnostics for the following expression or declaration. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Alternate-Keywords.html)

## Numeric and vector types

- **128-bit integers** `__int128` and `unsigned __int128` are available on targets with an integer mode wide enough to represent them. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/_005f_005fint128.html)
- **Complex arithmetic** includes complex integer types, `__real__` and `__imag__` component access, and literal suffixes such as `3i`. Floating-point `_Complex` types are part of C99. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Complex.html)
- **Additional floating-point types** include `_Float16`, `_Float128`, and target-specific types such as Arm's `__fp16`. Availability depends on the target. [Floating types](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Floating-Types.html) · [Half precision](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Half-Precision.html)
- **Decimal and fixed-point types** include `_Decimal32`, `_Decimal64`, `_Decimal128`, `_Fract`, and `_Accum` on supported targets. GCC documents its decimal implementation against an earlier technical-report draft and notes gaps in C23 support. [Decimal types](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Decimal-Float.html) · [Fixed point](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Fixed-Point.html)
- **Vector types** can be declared with `__attribute__((vector_size(16)))` on a suitable scalar base type. GCC supports elementwise operations, indexing, and built-ins for shuffles and conversions. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Vector-Extensions.html)

## Arrays, structures, and unions

- **Variable-length array extensions** include VLA members in local structures and unions, plus forward declarations of function parameters for use in an array bound. Automatic VLAs were introduced in ISO C99. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Variable-Length.html)
- **Zero-length arrays** such as `char data[0]` are accepted as trailing members. GCC also supports some extensions to standard C99 flexible members (`data[]`), including static initialization of a top-level object. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Zero-Length.html)
- **Empty structures** may have no members and have size zero in GNU C. GCC also allows flexible array members in unions and as the sole member of a structure. [Empty structures](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Empty-Structures.html) · [Flexible members in unions](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Flexible-Array-Members-in-Unions.html) · [Flexible members alone in structures](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Flexible-Array-Members-alone-in-Structures.html)
- **Unnamed members** expose members of an anonymous structure or union through the containing object. C11 standardized tagless forms. `-fms-extensions` and `-fplan9-extensions` enable additional forms. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Unnamed-Fields.html)
- **Casts to union types** such as `(union value)42` create a union value if the operand's type matches a member type. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Cast-to-Union.html)
- **Range designators** such as `[0 ... 3] = 1` initialize multiple array elements. Single-index `[i] =` and field `.name =` designators are part of C99. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Designated-Inits.html)
- **Compound literals in static initializers** can initialize objects of static storage duration from a matching compound literal with constant elements. Ordinary compound literals were introduced in C99. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Compound-Literals.html)
- **Other C99 features accepted in C90 mode** include subscripting an array member of a returned structure and using runtime expressions in automatic aggregate initializers. [Array subscripting](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Subscripting.html) · [Aggregate initializers](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Initializers.html)

## Language semantics and linkage

- **`void *` and function-pointer arithmetic** treats `void` and function types as size one for pointer arithmetic and `sizeof`. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Pointer-Arith.html)
- **Variadic pointer arguments** can be retrieved with `va_arg(ap, void *)` regardless of the pointer type passed, under the POSIX XSI extension implemented by GCC. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Variadic-Pointer-Args.html)
- **Pointers to qualified arrays** permit conversions such as `int (*)[5]` to `const int (*)[5]`. C23 standardized the underlying array qualification rules. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Pointers-to-Arrays.html) · [C23 support](https://gcc.gnu.org/projects/c-status.html)
- **Old-style function definitions** can inherit an argument type from a prior prototype in the cases described by GCC, including some mismatches caused by default argument promotions. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Function-Prototypes.html)
- **Qualified function types** interpret `const` on a function typedef like GCC's `const` function attribute and `volatile` like `noreturn`. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Const-and-Volatile-Functions.html)
- **GNU89 `inline` linkage** is selected by `-std=gnu89`, `-fgnu89-inline`, or the `gnu_inline` attribute. Later C modes normally use ISO C99 inline semantics. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Inline.html)
- **Volatile accesses** follow GCC's documented rules, including what a discarded read does and how volatile and ordinary accesses are ordered. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Volatiles.html)

## Tokens and preprocessing

- **Variadic macro extensions** include named packs (`args...`) and GNU comma deletion with `, ##__VA_ARGS__` when the variable argument is omitted. ISO C99 introduced variadic macros, and C23 added `__VA_OPT__` and permits omitting the variable argument. [GNU CPP manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/cpp/Variadic-Macros.html)
- **Source spelling extensions** include `$` in identifiers on most targets, whitespace between a backslash and a spliced newline, the `\e` escape for ESC, and raw strings such as `R"(C:\tmp)"` in `gnu99` and later GNU modes. [Identifiers](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Dollar-Signs.html) · [Newlines](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Escaped-Newlines.html) · [ESC](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Character-Escapes.html) · [Raw strings](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Raw-String-Literals.html)
- **Function-name identifiers** `__FUNCTION__` and `__PRETTY_FUNCTION__` give the function name in GNU C. ISO C99 added `__func__`. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Function-Names.html)
- **Features accepted before their ISO modes** include `long long`, `_Bool`, `//` comments, mixed declarations and statements, and hexadecimal floating constants such as `0x1.8p+1` (C99). Binary constants such as `0b1010` and labels before declarations or at the end of blocks are standard since C23. [Integers](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Long-Long.html) · [Boolean type](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Boolean-Type.html) · [Comments](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/C_002b_002b-Comments.html) · [Declarations and labels](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Mixed-Labels-and-Declarations.html) · [Hex floats](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Hex-Floats.html) · [Binary constants](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Binary-constants.html)
- **Wrapper headers** can use `#include_next` to resume include lookup after the directory where the current header was found. [GNU CPP manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/cpp/Wrapper-Headers.html)
- **Predefined macros and feature checks** include `__GNUC__`, `__COUNTER__`, `__has_builtin(...)`, and `__has_attribute(...)`. [Predefined macros](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/cpp/Common-Predefined-Macros.html) · [Built-in checks](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/cpp/_005f_005fhas_005fbuiltin.html) · [Attribute checks](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/cpp/_005f_005fhas_005fattribute.html)

## Attributes, pragmas, and target features

- **GNU attributes** use `__attribute__((...))` or `[[gnu::...]]` syntax. Examples include `format`, `nonnull`, `cleanup`, `packed`, `aligned`, `section`, `weak`, and `visibility`. [Common attributes](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Common-Attributes.html) · [Target-specific attributes](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Target-Specific-Attributes.html)
- **Pragmas** include `#pragma GCC diagnostic`, `#pragma GCC target`, `#pragma GCC optimize`, `#pragma GCC unroll`, `#pragma pack`, `#pragma weak`, and `#pragma push_macro` / `pop_macro`. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Pragmas.html)
- **Named address spaces** qualify types on supported targets, for example `__flash` on AVR or `__seg_fs` on x86. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Named-Address-Spaces.html)
- **Inline assembly** includes basic `asm`, extended `asm` with operands and clobbers, `asm goto`, custom assembler names, and explicit register variables. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Using-Assembly-Language-with-C.html)
- **OpenMP and OpenACC** directives are enabled with `-fopenmp` and `-fopenacc`, respectively. [OpenMP](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/OpenMP.html) · [OpenACC](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/OpenACC.html)

## Compiler built-ins

- **Type and compile-time queries** include `__builtin_types_compatible_p`, `__builtin_choose_expr`, and `__builtin_constant_p`. [GCC manual](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Other-Builtins.html)
- **Integer operations** include checked arithmetic such as `__builtin_add_overflow`, bit operations such as `__builtin_popcount` and `__builtin_clz`, and byte swaps such as `__builtin_bswap32`. [Overflow checks](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Integer-Overflow-Builtins.html) · [Bit operations](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Bit-Operation-Builtins.html) · [Byte swaps](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Byte-Swapping-Builtins.html)
- **Memory and control flow** include `__builtin_alloca`, `__builtin_object_size`, `__builtin_expect`, and `__builtin_unreachable`. [Stack allocation](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Stack-Allocation.html) · [Object sizes](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Object-Size-Checking.html) · [Other built-ins](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Other-Builtins.html)
- **Atomics and target intrinsics** include the `__atomic_*` family, the older `__sync_*` family, and machine-specific built-ins. [Atomic operations](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/_005f_005fatomic-Builtins.html) · [Legacy atomics](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/_005f_005fsync-Builtins.html) · [Target intrinsics](https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/Target-Builtins.html)
