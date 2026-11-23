"""Check GNU C features with original inputs and a GCC binary as a reference."""

import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
from tempfile import TemporaryDirectory


def invoke(args, source=None):
    return subprocess.run([str(arg) for arg in args], input=source, text=True,
                          capture_output=True, timeout=15)


def require(result, description, success=True):
    assert (result.returncode == 0) == success, (
        f"{description}: exit {result.returncode}\n{result.stdout}{result.stderr}"
    )


def dialects(mcc, gcc, later_dialects):
    modes = [("c90", 0), ("gnu89", 0), ("c99", 199901), ("c11", 201112), ("gnu11", 201112)]
    if later_dialects:
        modes += [("c18", 201710), ("gnu23", 202311), ("c2y", 202500)]
    for mode, version in modes:
        strict = int(mode.startswith("c"))
        source = f"""
#if __STDC_VERSION__ != {version}L
#error wrong language version
#endif
#if defined(__STRICT_ANSI__) != {strict}
#error wrong GNU mode
#endif
#if {version}L >= 202311L && !true
#error wrong C23 boolean preprocessing token
#endif
"""
        for cc in (mcc, gcc):
            require(invoke([cc, f"-std={mode}", "-E", "-x", "c", "-"] if cc == gcc
                           else [cc, f"-std={mode}", "-E", "-"], source), f"{cc} {mode}")


def preprocess(mcc, gcc, root):
    first, second = root / "first", root / "second"
    first.mkdir()
    second.mkdir()
    (first / "wrapper.h").write_text("""
#if !__has_include_next(<actual.h>)
#error missing next header
#endif
#include_next <actual.h>
""")
    (first / "actual.h").write_text("#error picked the wrapper directory\n")
    (second / "actual.h").write_text("found_header\n")
    source = """
#if !__has_builtin(__builtin_choose_expr) || __has_builtin(__builtin_unknown_for_test)
#error incorrect builtin query
#endif
#if !__has_attribute(__packed__) || __has_attribute(unknown_for_test)
#error incorrect attribute query
#endif
#if !__has_c_attribute(gnu::packed) || __has_c_attribute(unknown::packed)
#error incorrect namespaced attribute query
#endif
#define HEADER <wrapper.h>
#if __has_include(HEADER) && !__has_include("unknown-for-test.h")
#include HEADER
#endif
#define JOIN(a, ...) a , ##__VA_ARGS__
JOIN(left)
JOIN(left, )
JOIN(left, right)
#define OPTIONAL(...) x __VA_OPT__(, __VA_ARGS__)
#define NOTHING
OPTIONAL() semicolon
OPTIONAL(NOTHING) semicolon
OPTIONAL(1, 2) semicolon
#define DUPLICATE(n) n n
DUPLICATE(__COUNTER__) __COUNTER__
#define VALUE restored
#define SAVE "push_macro(\\"VALUE\\")"
_Pragma(SAVE)
#undef VALUE
#define VALUE changed
VALUE
_Pragma("pop_macro(\\"VALUE\\")")
VALUE
"""
    expected = "found_header left left , left , right x semicolon x semicolon x , 1 , 2 semicolon 0 0 1 changed restored"
    for cc in (mcc, gcc):
        flags = ["-x", "c", "-P"] if cc == gcc else []
        result = invoke([cc, *flags, "-std=gnu11", "-E", "-I", first, "-I", second, "-"], source)
        require(result, f"{cc} preprocessing extensions")
        assert re.findall(r"\w+|,", result.stdout) == expected.split(), result.stdout


def compile_and_run(mcc, gcc, root, target, runner, later_dialects):
    cases = [
        ("gnu11", """
static int calls;
static int effect(void) { ++calls; return 9; }
static int example(int x) {
    __auto_type outer = x + 2;
    {
        __auto_type outer = outer + 3;
        int selected = 0;
        __builtin_choose_expr(1, selected, x) = outer;
        return __builtin_constant_p(effect()) || calls ||
               selected != 8 || __alignof__(selected) != __alignof__(int) ||
               sizeof __func__ != 8 || __FUNCTION__[0] != 'e' ||
               __PRETTY_FUNCTION__[6] != 'e';
    }
}
_Static_assert(__builtin_types_compatible_p(volatile long, long), "top qualifier");
_Static_assert(!__builtin_types_compatible_p(volatile long *, long *), "nested qualifier");
_Static_assert(__builtin_constant_p(2 + 3), "constant");
_Static_assert(_Minof(signed char) == -128 && _Maxof(unsigned char) == 255, "limits");
int main(void) { return example(3); }
"""),
        ("gnu11", r"""
asm(".data\n.p2align 2\n.globl gnu_probe_value\ngnu_probe_value:\n.long 17\n.text");
extern int from_asm asm("gnu_probe_" "value");
static int renamed(void) __asm__("gnu_probe_function");
static int renamed(void) {
    asm volatile ("nop");
    return from_asm;
}
int main(void) { return renamed() != 17; }
"""),
        ("gnu11", """
_Static_assert(__builtin_bswap16(0x1234) == 0x3412, "swap");
_Static_assert(__builtin_popcountll(0xf00000000ULL) == 4, "count");
_Static_assert(__builtin_clrsb(-1) == 31, "sign");
int main(void) {
    volatile unsigned long x = 0x0123456789abcdefUL;
    volatile unsigned int y = 0x40000108U;
    volatile int z = -2;
    int calls = 0;
    return __builtin_bswap64(x) != 0xefcdab8967452301UL ||
           __builtin_clz(y) != 1 || __builtin_ctz(y) != 3 ||
           __builtin_ffs(y) != 4 || __builtin_clrsb(z) != 30 ||
           __builtin_popcount(y + calls++) != 3 || calls != 1 ||
           __builtin_parity(y) != 1;
}
"""),
        ("gnu11", """
static int dispatch(int select, int count) {
    static void *targets[] = { &&first, &&second };
    {
        __label__ first;
        int scratch[count];
        scratch[0] = 17;
        if (scratch[0] != 17)
            goto first;
        goto *targets[select];
    first:
        __builtin_trap();
    }
first:
    return 11;
second:
    return 22;
}
static int relative(int select) {
    static const int offsets[] = { &&second - &&first, &&first - &&first };
    goto *(&&first + offsets[select]);
first:
    return 33;
second:
    return 44;
}
int main(void) {
    if (dispatch(0, 9) != 11 || dispatch(1, 10) != 22)
        return 1;
    if (relative(0) != 44 || relative(1) != 33)
        return 2;
    if (0)
        __builtin_unreachable();
    return 0;
}
"""),
        ("c23", """
static_assert(0b11 == 3);
static_assert(_Generic(true, _Bool: 1, default: 0));
static_assert(__builtin_types_compatible_p(typeof(1), int));
static_assert(_Generic((typeof_unqual(const volatile int))0, int: 1, default: 0));
struct [[gnu::packed]] record { char a; int b; };
static_assert(sizeof(struct record) == 5);
int main(void) { int zero = {}; return zero || false; }
"""),
        ("c2y", """
static_assert(_Countof(int[4][2]) == 4);
static int check(int n) {
    int array[n++];
    return _Countof(array) == 4 && n == 5 && _Countof(int[n++][2]) == 5 && n == 6;
}
int main(void) { return !check(4); }
"""),
        ("c11", """
__extension__ enum { number = 0b101 };
int main(void) {
    __extension__ int value = ({ 2 + 3; });
    return __extension__ ({ value != number; });
}
"""),
    ]
    cases.append(("gnu11", r"""
#if defined(__aarch64__)
asm(".text\n.globl read_chain\nread_chain:\nldr w0, [x18]\nret");
#else
asm(".text\n.globl read_chain\nread_chain:\nmovl (%r10), %eax\nret");
#endif
extern int read_chain(void);
int main(void) {
    int data[] = {3, 7};
    int (*volatile indirect)(void) = read_chain;
    int index = 0;
    return __builtin_call_with_static_chain(indirect(), &data[index++]) != 3 ||
           __builtin_call_with_static_chain(indirect(), &data[index++]) != 7 ||
           index != 2;
}
"""))
    cases.append(("gnu11", """
#include <pthread.h>
#include <stdarg.h>
extern void qsort(void *, unsigned long, unsigned long, int (*)(const void *, const void *));
typedef int (*reader)(void);
static int activate(int value, reader previous) {
    int read(void) { return value; }
    reader current = read;
    if (value == 1)
        return activate(2, current) || current() != value;
    return current == previous || previous() != 1 || current() != 2;
}
static int enclosing(int bias, int length) {
    int array[length];
    array[0] = 5;
    int sum = 0;
    auto int step(int);
    int recurse(int n) { return n ? step(n - 1) : sum; }
    int step(int n) {
        int visit(int x) { sum += x + bias + array[0]; return recurse(n); }
        return visit(1);
    }
    int compare(const void *a, const void *b) {
        return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
    }
    double variadic(int n, ...) {
        va_list ap;
        va_start(ap, n);
        double result = va_arg(ap, double) + bias + n;
        va_end(ap);
        return result;
    }
    int values[] = {8, 2, 5};
    double (*callback)(int, ...) = variadic;
    qsort(values, 3, sizeof values[0], compare);
    return step(2) != 27 || values[0] != 2 || values[2] != 8 ||
           callback(1, 4.5) != 8.5 || callback != &variadic;
}
static int escape(int length) {
    int live[length];
    live[0] = 19;
    {
        __label__ done;
        int called = 0;
        int later[length];
        int escape_from_callback(const void *x, const void *y) {
            called += *(const int *)x + *(const int *)y;
            goto done;
        }
        int sort[] = {9, 2};
        later[0] = 31;
        qsort(sort, 2, sizeof sort[0], escape_from_callback);
        return 1;
    done:
        return live[0] != 19 || later[0] != 31 || called != 11;
    }
}
static int parallel(int value) {
    int read(void) { return value; }
    void *prepare(void *result) {
        *(reader *)result = read;
        return 0;
    }
    pthread_t first, second;
    reader results[2];
    if (pthread_create(&first, 0, prepare, &results[0]))
        return 1;
    int status = pthread_create(&second, 0, prepare, &results[1]);
    if (pthread_join(first, 0) || status || pthread_join(second, 0))
        return 1;
    return results[0] != results[1] || results[0]() != value;
}
int main(void) { return enclosing(3, 10) || activate(1, 0) || escape(6) || parallel(73); }
"""))
    for mode, source in cases:
        if mode in ("c23", "c2y") and not later_dialects:
            continue
        target_flags = [f"--target={target}"] if target else []
        for cc in (mcc, gcc):
            flags = ["-x", "c"] if cc == gcc else target_flags.copy()
            if mode != "gnu11":
                flags += ["-pedantic-errors"]
            binary = root / "extension-probe"
            require(invoke([cc, *flags, f"-std={mode}", "-ftrampoline-impl=heap", "-pthread", "-", "-o", binary],
                           source), f"{cc} {mode}")
            prefix = runner if cc == mcc else []
            require(invoke([*prefix, binary]), f"{cc} {mode} executable")
            if cc == mcc:
                elf = invoke(["readelf", "-W", "-l", "-d", binary])
                require(elf, f"{cc} {mode} ELF information")
                stack = next(line.split() for line in elf.stdout.splitlines() if "GNU_STACK" in line)
                assert stack[-2] == "RW", stack
                assert "libgcc_s.so" not in elf.stdout, elf.stdout
        require(invoke([mcc, *target_flags, f"-std={mode}", "-c", "-o",
                        root / "extension-probe.o", "-"], source), f"mcc {mode} object")


def diagnostics(mcc):
    source = "int main(void) { return ({ 0; }); }\n"
    warning = invoke([mcc, "-std=gnu11", "-Wpedantic", "-fsyntax-only", "-"], source)
    require(warning, "pedantic warning")
    assert "warning:" in warning.stderr and "statement expression" in warning.stderr
    require(invoke([mcc, "-std=gnu11", "-pedantic-errors", "-fsyntax-only", "-"], source),
            "pedantic rejection", success=False)
    for invalid in ("int x = _Countof(int);", "int x = _Maxof(double);",
                    "int f(int n) { return __builtin_choose_expr(n, 1, 2); }",
                    "__auto_type x = 1, y = 2;",
                    "int f(void) { goto *&&missing; }",
                    "void *target = &&nowhere;",
                    'int f(void) { int x asm("invalid for automatic storage"); return x; }',
                    "int f(void) { int n; return __builtin_call_with_static_chain(n, &n); }",
                    "#define BAD(...) __VA_OPT__(__VA_OPT__(1))\n"):
        require(invoke([mcc, "-std=gnu11", "-fsyntax-only", "-"], invalid + "\n"),
                invalid, success=False)
    nested = "int f(void) { int g(void) { return 1; } return g(); }"
    require(invoke([mcc, "-std=c11", "-fsyntax-only", "-"], nested),
            "nested function rejected in strict mode", success=False)
    require(invoke([mcc, "-std=gnu11", "-ftrampoline-impl=stack", "-fsyntax-only", "-"], nested),
            "executable stack request rejected", success=False)


def diagnostic_pragmas(mcc, gcc, root, target):
    flags = [f"--target={target}"] if target else []
    source = root / "diagnostic-test.c"
    source.write_text(r"""
#define APPLY_(x) _Pragma(#x)
#define APPLY(x) APPLY_(x)
#define SILENCE(x) APPLY(GCC diagnostic push) \
                   APPLY(GCC diagnostic ignored "-Wdeprecated-declarations") \
                   x APPLY(GCC diagnostic pop)
#define ID(x) x

extern int old __attribute__((deprecated("old object")));
int old_api(void) __attribute__((deprecated("old function")));
typedef int Old __attribute__((deprecated("old typedef")));
typedef int Old;
struct __attribute__((deprecated("old record"))) Tagged {
    int field __attribute__((deprecated("old member")));
};
enum __attribute__((deprecated("old enum"))) Named {
    OLD_WORD __attribute__((deprecated("old enumerator"))) = 3
};
#define TAKE_ADDRESS() (&old)
SILENCE(int quiet(void) { return *TAKE_ADDRESS() + old_api(); })

#pragma GCC diagnostic warning "-Wdeprecated-declarations"
ID(APPLY(GCC diagnostic push)
   APPLY(GCC diagnostic ignored "-Wdeprecated-declarations")
   int quiet_argument(void) { return *TAKE_ADDRESS() + old_api(); }
   APPLY(GCC diagnostic pop)
   int visible_argument(void) { return *TAKE_ADDRESS(); })

int visible(struct Tagged *p, Old n, enum Named x) { return p->field + n + x + OLD_WORD + old_api(); }
#pragma GCC diagnostic pop
int rejected(void) { return old_api(); }
""")
    expected_messages = ("old object", "old function", "old typedef", "old record",
                         "old member", "old enum", "old enumerator")
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        result = invoke([*prefix, "-std=gnu11", "-Werror", "-fsyntax-only", source])
        require(result, f"{cc} restore -Werror after an unmatched diagnostic pop", success=False)
        complaints = [line for line in result.stderr.splitlines()
                      if re.search(r": (warning|error):", line)]
        warnings = [line for line in complaints if ": warning:" in line]
        errors = [line for line in complaints if ": error:" in line]
        assert len(warnings) == len(expected_messages) and len(errors) == 1, result.stderr
        for message in expected_messages:
            assert sum(re.search(re.escape(message) + r"(?: \[|$)", line) is not None
                       for line in warnings) == 1, result.stderr
        assert "old function" in errors[0], result.stderr

        cases = (
            ('#pragma GCC diagnostic ignored "-Wcpp"\n#warning quiet\n'
             '#pragma GCC diagnostic pop\n#warning visible\n',
             ["-Werror=cpp"], "error", "visible"),
            ('#pragma GCC diagnostic warning "-Wpedantic"\nint a[0];\n'
             '#pragma GCC diagnostic pop\n',
             ["-pedantic-errors"], "warning", "pedantic"),
            ('#pragma GCC diagnostic ignored "-Wpedantic"\nint a[0];\n',
             ["-pedantic-errors"], "", ""),
            ('#pragma not_defined_for_this_test\n', [], "", ""),
            ('#pragma not_defined_for_this_test\n',
             ["-Werror=unknown-pragmas"], "error", "unknown-pragmas"),
            ('#pragma GCC diagnostic ignored "-Wno_such_warning_for_test"\n',
             ["-Werror", "-Wno-error=pragmas"], "warning", "pragmas"),
            ('#pragma GCC diagnostic ignored "-Wpragmas"\n'
             '#pragma GCC diagnostic ignored "-Wno_such_warning_for_test"\n',
             ["-Werror"], "", ""),
        )
        stdin = [*prefix, "-std=gnu11", *(["-x", "c"] if cc == gcc else [])]
        for text, options, level, fragment in cases:
            result = invoke([*stdin, *options, "-fsyntax-only", "-"], text)
            require(result, f"{cc} apply {options} to {text!r}", success=level != "error")
            diagnostics = [line for line in result.stderr.splitlines()
                           if re.search(r": (warning|error):", line)]
            assert len(diagnostics) == bool(level), result.stderr
            if level:
                assert f": {level}:" in diagnostics[0] and fragment in diagnostics[0], result.stderr

        unknown = invoke([*stdin, "-Werror=unknown-pragmas", "-E", "-P", "-"],
                         "#pragma not_defined_for_this_test\n")
        require(unknown, f"{cc} preserve unknown pragmas during preprocessing")
        assert "#pragma not_defined_for_this_test" in unknown.stdout and not unknown.stderr, unknown

        source_text = r"""
#define TEXT "from a macro"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma message (TEXT " and a literal")
_Pragma("message \"from an operator\"")
int zero[0];
"""
        pp = invoke([*stdin, "-E", "-P", "-"], source_text)
        require(pp, f"{cc} preprocess #pragma message")
        assert "note:" not in pp.stderr and "#pragma GCC diagnostic" in pp.stdout, pp
        for phrase in ("from a macro", "and a literal", "from an operator"):
            assert phrase in pp.stdout, pp.stdout
        replay = invoke([*stdin, "-Wpedantic", "-Werror", "-fsyntax-only", "-"], pp.stdout)
        require(replay, f"{cc} replay diagnostic and message pragmas")
        assert all(f"note: " in replay.stderr and text in replay.stderr
                   for text in ("from a macro and a literal", "from an operator")), replay.stderr

    initial = root / "diag-first.c"
    next_file = root / "diag-next.c"
    initial.write_text('#pragma GCC diagnostic ignored "-Wdeprecated-declarations"\n'
                       'extern int old __attribute__((deprecated("muted earlier")));\n'
                       'int first(void) { return old; }\n')
    next_file.write_text('extern int old __attribute__((deprecated("new unit")));\n'
                         'int second(void) { return old; }\n')
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        result = invoke([*prefix, "-std=gnu11", "-Werror=deprecated-declarations",
                         "-fsyntax-only", initial, next_file])
        require(result, f"{cc} reset diagnostic settings for each translation unit", success=False)
        assert "muted earlier" not in result.stderr and "new unit" in result.stderr, result.stderr

    header = root / "diag-system.h"
    header.write_text("""
#pragma GCC system_header
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wdeprecated-declarations"
extern int hidden __attribute__((deprecated("system diagnostic")));
static int get(void) { return hidden; }
#pragma GCC diagnostic pop
""")
    source.write_text('#include "diag-system.h"\nint go(void) { return get(); }\n')
    scoped = root / "scoped-diagnostics.c"
    scoped.write_text("""
#pragma GCC diagnostic ignored_attributes "vendor::first"
[[vendor::first]] int a;
[[vendor::second]] int b;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored_attributes "vendor::"
#pragma GCC diagnostic pop
[[vendor::second]] int c;
[[commandline::quiet(1, (2))]] int d;
""")
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else []), "-std=gnu11", "-fsyntax-only"]
        default = invoke([*prefix, source])
        require(default, f"{cc} suppress system header diagnostics")
        assert "system diagnostic" not in default.stderr, default.stderr
        shown = invoke([*prefix, "-Wsystem-headers", source])
        require(shown, f"{cc} enable system header diagnostics", success=False)
        assert ": error:" in shown.stderr and "system diagnostic" in shown.stderr, shown.stderr
        attributes = invoke([*prefix, "-Wno-attributes=commandline::", scoped])
        require(attributes, f"{cc} filter scoped attributes")
        complaints = [line for line in attributes.stderr.splitlines() if ": warning:" in line]
        assert len(complaints) == 1 and "vendor::second" in complaints[0], attributes.stderr


def atomics(mcc, gcc, root, target, runner):
    source = """
#include <pthread.h>
#include <stdatomic.h>
#ifdef __STDC_NO_ATOMICS__
#error atomic support is required
#endif
struct bytes4 { char data[4]; };
struct triple { unsigned long a, b, c; };
_Static_assert(_Alignof(_Atomic(struct bytes4)) == 4, "atomic alignment");
static atomic_int count = ATOMIC_VAR_INIT(0);
static atomic_int calls;
static atomic_flag flag = ATOMIC_FLAG_INIT;
static int operand(void) { ++calls; return 2; }
static void *worker(void *unused) {
    (void)unused;
    for (int i = 0; i < 1000; i++) {
        ++count;
        count += operand();
        atomic_fetch_sub_explicit(&count, 1, memory_order_relaxed);
    }
    return 0;
}
int main(void) {
    if (!__atomic_always_lock_free(4, 0) || __atomic_always_lock_free(17, 0))
        return 17;
    pthread_t thread;
    if (pthread_create(&thread, 0, worker, 0))
        return 1;
    worker(0);
    if (pthread_join(thread, 0) || atomic_load(&count) != 4000 || calls != 2000)
        return 2;
    if (!atomic_is_lock_free(&count) || !__atomic_is_lock_free(sizeof count, &count))
        return 3;
    if (atomic_flag_test_and_set(&flag) || !atomic_flag_test_and_set(&flag))
        return 4;
    atomic_flag_clear_explicit(&flag, memory_order_release);
    if (atomic_flag_test_and_set_explicit(&flag, memory_order_acquire))
        return 5;
    int values[4] = {0};
    _Atomic(int *) cursor = values;
    if (cursor++ != values || atomic_load(&cursor) != values + 1)
        return 6;
#ifdef __MCC__
    if (atomic_fetch_add(&cursor, 2) != values + 1 || cursor != values + 3)
        return 7;
#endif
    _Atomic(float) number = 1.25f;
    if (++number != 2.25f || atomic_exchange(&number, 9.5f) != 2.25f || number != 9.5f)
        return 8;
    _Atomic(struct triple) big;
    struct triple a = {1, 2, 3}, b = {4, 5, 6}, wrong = {7, 8, 9}, snapshot;
    atomic_init(&big, a);
    if (atomic_is_lock_free(&big))
        return 9;
    if (atomic_compare_exchange_strong(&big, &wrong, b) || wrong.b != 2 ||
        !atomic_compare_exchange_strong_explicit(&big, &wrong, b,
                                               memory_order_release, memory_order_relaxed))
        return 10;
    snapshot = atomic_exchange(&big, a);
    if (snapshot.a != 4 || snapshot.c != 6)
        return 11;
    snapshot = big;
    if (snapshot.a != 1 || snapshot.c != 3)
        return 12;
    big = b;
    snapshot = atomic_load_explicit(&big, memory_order_acquire);
    if (snapshot.a != 4 || snapshot.c != 6)
        return 13;
    unsigned char byte = 0x91, old = 0x99;
    if (__atomic_compare_exchange_n(&byte, &old, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
        || old != 0x91)
        return 14;
    if (__atomic_fetch_nand(&byte, 0xf0, __ATOMIC_RELAXED) != 0x91 || byte != 0x6f)
        return 15;
    unsigned word = 4, value = 9, out = 0, expected = 4;
    __atomic_exchange(&word, &value, &out, __ATOMIC_SEQ_CST);
    __atomic_load(&word, &value, __ATOMIC_ACQUIRE);
    if (out != 4 || value != 9 ||
        __atomic_compare_exchange(&word, &expected, &out, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)
        || expected != 9)
        return 16;
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_acquire);
    __sync_synchronize();
    unsigned legacy = 11;
    if (__sync_fetch_and_add(&legacy, 7) != 11 || __sync_sub_and_fetch(&legacy, 2) != 16)
        return 18;
    if (__sync_val_compare_and_swap(&legacy, 11, 3) != 16 ||
        !__sync_bool_compare_and_swap(&legacy, 16, 3) ||
        __sync_lock_test_and_set(&legacy, 1) != 3)
        return 19;
    __sync_lock_release(&legacy);
    if (legacy != 0)
        return 20;
    return 0;
}
"""
    flags = [f"--target={target}"] if target else []
    binary = root / "atomic-probe"
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-pthread", "-", "-o", binary],
                   source), "atomic mcc build")
    require(invoke([*runner, binary]), "atomic mcc execution")
    require(invoke([gcc, "-std=c11", "-pedantic-errors", "-pthread", "-x", "c", "-", "-latomic",
                    "-o", binary], source), "atomic GCC build")
    require(invoke([binary]), "atomic GCC execution")
    if target is None:
        atomic_abi(mcc, gcc, root)
    for invalid in (
        "_Atomic(const int) value;",
        "typedef int array[3]; _Atomic array value;",
        "struct invalid { _Atomic int bits : 2; };",
        "int f(void) { return __atomic_load_n(1, __ATOMIC_SEQ_CST); }",
        "void f(void) { const int n = 0; __atomic_store_n(&n, 1, __ATOMIC_SEQ_CST); }",
    ):
        require(invoke([mcc, "-std=c11", "-fsyntax-only", "-"], invalid),
                invalid, success=False)


def atomic_abi(mcc, gcc, root):
    header = """
#include <stdatomic.h>
extern atomic_uint counter;
unsigned reference_add(unsigned);
"""
    own = header + """
atomic_uint counter = 7;
int main(void) {
    if (reference_add(4) != 7 || atomic_load(&counter) != 11)
        return 1;
    if (atomic_fetch_add_explicit(&counter, 5, memory_order_release) != 11)
        return 2;
    return reference_add(3) != 16 || counter != 19;
}
"""
    reference = header + """
unsigned reference_add(unsigned amount) {
    return atomic_fetch_add_explicit(&counter, amount, memory_order_acq_rel);
}
"""
    own_obj = root / "atomic-own.o"
    ref_obj = root / "atomic-reference.o"
    binary = root / "atomic-mixed"
    require(invoke([mcc, "-std=c11", "-c", "-", "-o", own_obj], own), "mcc atomic ABI object")
    require(invoke([gcc, "-std=c11", "-x", "c", "-c", "-", "-o", ref_obj], reference),
            "GCC atomic ABI object")
    require(invoke([mcc, own_obj, ref_obj, "-o", binary]), "mixed atomic ABI link")
    require(invoke([binary]), "mixed atomic ABI execution")
    elf = invoke(["readelf", "-dW", binary])
    require(elf, "mixed atomic ABI dynamic section")
    assert "libatomic.so" not in elf.stdout and "libgcc_s.so" not in elf.stdout


def inline_linkage(mcc, gcc, root, target, runner):
    source = """
#if defined(__GNUC_GNU_INLINE__) == defined(__GNUC_STDC_INLINE__)
#error expected one inline semantics macro
#endif
#if __GNUC_GNU_INLINE__ != EXPECT_GNU_INLINE
#error wrong inline semantics
#endif
#if EXPECT_GNU_INLINE
#define HEADER_INLINE extern __inline__
#define EMITTED_INLINE __inline__
extern __inline__ int replaced(void) { return 90; }
int replaced(void) { return 9; }
#else
#define HEADER_INLINE __inline__
#define EMITTED_INLINE extern __inline__
int replaced(void) { return 9; }
#endif
HEADER_INLINE int shared(void) { return 70; }
EMITTED_INLINE int emitted(void) { return 3; }
__inline__ int declared_later(void) { return 4; }
extern int declared_later(void);
extern __inline__ int attribute_shared(void) __attribute__((gnu_inline));
extern __inline__ __attribute__((gnu_inline)) int attribute_shared(void) { return 80; }
static __inline__ int local_value(void) { return 5; }
int main(void) {
    int (*shared_address)(void) = shared;
    return shared_address() != 7 || attribute_shared() != 8 || emitted() != 3
           || declared_later() != 4 || local_value() != 5 || replaced() != 9;
}
"""
    fallback = """
int shared(void) { return 7; }
int attribute_shared(void) { return 8; }
"""
    modes = [
        ("c90", [], 1),
        ("gnu89", [], 1),
        ("c11", [], 0),
        ("gnu11", ["-fgnu89-inline"], 1),
        ("gnu11", ["-fgnu89-inline", "-fno-gnu89-inline"], 0),
    ]
    for cc in (mcc, gcc):
        if cc == gcc:
            link_flags, source_flags, prefix = [], ["-x", "c"], []
        else:
            link_flags = [f"--target={target}"] if target else []
            source_flags, prefix = link_flags, runner
        header_obj, fallback_obj = root / "inline-header.o", root / "inline-fallback.o"
        binary = root / "inline-linked"
        require(invoke([cc, *source_flags, "-c", "-", "-o", fallback_obj], fallback), "inline fallback")
        for mode, options, expected in modes:
            label = f"{cc} {mode} {' '.join(options)} inline linkage"
            require(invoke([cc, *source_flags, f"-std={mode}", *options, "-O0",
                            f"-DEXPECT_GNU_INLINE={expected}", "-c", "-", "-o", header_obj],
                           source), label)
            require(invoke([cc, *link_flags, header_obj, fallback_obj, "-o", binary]), label)
            require(invoke([*prefix, binary]), label)
    mismatch = """
inline int example(void);
inline __attribute__((gnu_inline)) int example(void) { return 3; }
"""
    require(invoke([mcc, "-std=gnu11", "-fsyntax-only", "-"], mismatch),
            "inconsistent gnu_inline declarations", success=False)


def raw_strings(mcc, gcc, root, target, runner):
    source = r'''
#define SPELL(x) #x
#define PASTE(a,b) a ## b
#define RAW R"abcdefghijklmnop(a/* # // " )wrong")abcdefghijklmnop"
static const char text[] = R"tag(backslash: \n, quotes: "word", close: )other"
one \@spaces@
two
)tag" "!";
static const char expected[] = "backslash: \\n, quotes: \"word\", close: )other\"\none \\@spaces@\ntwo\n!";
static const unsigned short wide16[] = "a" uR"-(🙂)-" "\xff";
static const unsigned wide32[] = UR"(雪)" "!";
static const char utf8[] = u8R"(é)" "!";
enum { before = __LINE__ };
static const char layout[] = R"(a
b
c)";
enum { after = __LINE__ };
_Static_assert(after - before == 4, "physical line tracking");
static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
int main(void) {
    if (!same(text, expected) || !same(layout, "a\nb\nc"))
        return 1;
    if (!same(RAW, "a/* # // \" )wrong\"") ||
        !same(PASTE(R, "(cat)"), "cat") ||
        !same(SPELL(R"tag(\q)tag"), "R\"tag(\\q)tag\""))
        return 2;
    if (wide16[0] != 'a' || wide16[1] != 0xd83d || wide16[2] != 0xde42 ||
        wide16[3] != 0xff || wide16[4] ||
        wide32[0] != 0x96ea || wide32[1] != '!' || wide32[2])
        return 3;
    return (unsigned char)utf8[0] != 0xc3 || (unsigned char)utf8[1] != 0xa9
           || utf8[2] != '!' || utf8[3];
}
'''.replace("\n", "\r\n")
    binary = root / "raw-string"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        text = source.replace("@spaces@", "  " if cc == mcc else "")
        result = invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", "-", "-o", binary], text)
        require(result, f"{cc} GNU raw literals")
        if cc == mcc:
            assert "backslash and newline separated" not in result.stderr, result.stderr
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} raw literal execution")
        for mode in ("c11", "gnu89"):
            require(invoke([cc, *flags, f"-std={mode}", "-fsyntax-only", "-"],
                           'const char *text = R"(content)";\n'),
                    f"{cc} {mode} raw literal rejection", success=False)
        for spelling in ('R"bad delimiter(x)bad delimiter"', 'R"abcdefghijklmnopq(x)abcdefghijklmnopq"',
                         'R"once(unterminated"'):
            require(invoke([cc, *flags, "-std=gnu11", "-fsyntax-only", "-"],
                           f"const char *text = {spelling};\n"),
                    f"{cc} malformed raw literal", success=False)
    simple = 'static const char text[] = R\x5c\n"(phase one)";\n'
    require(invoke([mcc, "-std=gnu99", "-fsyntax-only", "-"], simple),
            "gnu99 spliced raw literal prefix")


def checked_arithmetic(mcc, gcc, root, target, runner):
    source = """
#include <limits.h>
#if !__has_builtin(__builtin_mul_overflow_p) || !__has_builtin(__builtin_subcll)
#error missing checked arithmetic
#endif
struct fields { signed int small : 3; unsigned int positive : 4; };
_Static_assert(__builtin_add_overflow_p(3, 1, ((struct fields *)0)->small), "signed field overflow");
_Static_assert(!__builtin_sub_overflow_p(0, 4, ((struct fields *)0)->small), "signed field minimum");
_Static_assert(__builtin_sub_overflow_p(0, 1, ((struct fields *)0)->positive), "unsigned field minimum");
_Static_assert(!__builtin_mul_overflow_p(3, 5, ((struct fields *)0)->positive), "unsigned field maximum");
_Static_assert(__builtin_add_overflow_p(127, 1, *(signed char *)0), "ignored type witness");
_Static_assert(!__builtin_add_overflow_p(ULONG_MAX, LONG_MIN, 0L), "mixed signedness");
_Static_assert(__builtin_mul_overflow_p(ULONG_MAX, ULONG_MAX, 0UL), "wide mathematical product");
enum { chosen = __builtin_choose_expr(__builtin_add_overflow_p(INT_MAX, 1, 0), 4, 8) };
static volatile long negative = LONG_MIN;
static volatile unsigned long maximum = ULONG_MAX;
static int touched;
static unsigned destination;
static unsigned *place(void) { ++touched; return &destination; }
static signed char witness(void) { ++touched; return 17; }
int main(void) {
    long s;
    if (chosen != 4 || __builtin_add_overflow(maximum, negative, &s) || s != LONG_MAX)
        return 1;
    if (!__builtin_sub_overflow(maximum, LONG_MAX, &s) || s != LONG_MIN)
        return 2;
    if (!__builtin_smull_overflow(negative, -1, &s) || s != LONG_MIN)
        return 3;
    signed char small;
    if (__builtin_add_overflow((signed char)-2, (unsigned char)15, &small) || small != 13)
        return 4;
    if (!__builtin_mul_overflow(-1L, maximum, place()) || destination != 1 || touched != 1)
        return 5;
    if (!__builtin_add_overflow_p(127, 1, (touched++, (signed char)0)) || touched != 2)
        return 6;
    volatile int pick = 0;
    if (!__builtin_add_overflow_p(127, 1, (signed char)(pick ? witness() : *(signed char *)0))
        || touched != 2)
        return 7;
    pick = 1;
    if (__builtin_sub_overflow_p(127, 1, (signed char)(pick ? witness() : *(signed char *)0))
        || touched != 3)
        return 8;
    int converted;
    if (__builtin_sadd_overflow(3.75, 4, &converted) || converted != 7 ||
        sizeof(__builtin_sadd_overflow(3, 4, &converted)) != sizeof(_Bool))
        return 9;
    unsigned carry;
    if (__builtin_addc(UINT_MAX, UINT_MAX, UINT_MAX, &carry) != UINT_MAX - 2 || carry != 1)
        return 10;
    if (__builtin_subc(0, UINT_MAX, UINT_MAX, &carry) != 2 || carry != 1)
        return 11;
    unsigned long wide_carry;
    if (__builtin_addcl(0, 0, 1UL << 40, &wide_carry) != (1UL << 40) || wide_carry)
        return 12;
    unsigned long long borrow;
    if (__builtin_subcll(5, 3, 3, &borrow) != ULLONG_MAX || borrow != 1)
        return 13;
    return 0;
}
"""
    binary = root / "checked-arithmetic"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        require(invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", "-", "-o", binary],
                       source), f"{cc} checked arithmetic")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} checked arithmetic execution")
    for source in (
        "int f(int *p) { return __builtin_add_overflow(1., 2, p); }",
        "int f(void) { _Bool b; return __builtin_add_overflow(1, 2, &b); }",
        "int f(void) { enum E { A } e; return __builtin_sub_overflow(1, 2, &e); }",
        "int f(_Atomic int *p) { return __builtin_mul_overflow(1, 2, p); }",
        "int f(void) { return __builtin_mul_overflow_p(1, 2, (_Bool)0); }",
        "int f(void) { return __builtin_sadd_overflow(1, 2); }",
    ):
        require(invoke([mcc, "-std=gnu11", "-fsyntax-only", "-"], source),
                source, success=False)


def source_and_cache_builtins(mcc, gcc, root, target, runner):
    source = root / "source-and-cache-builtins.c"
    source.write_text("""
#include <string.h>
#include <sys/mman.h>

#if !__has_builtin(__builtin_LINE) || !__has_builtin(__builtin_FILE) || \\
    !__has_builtin(__builtin_FUNCTION) || !__has_builtin(__builtin_prefetch) || \\
    !__has_builtin(__builtin___clear_cache) || !__has_builtin(__builtin_expect_with_probability) || \\
    !__has_builtin(__builtin_assume_aligned)
#error missing builtin
#endif

#line 100 "source-builtins-virtual.c"
enum { split = __builtin_LINE
() };
static const char *file = __builtin_FILE();
static const char *outside = __builtin_FUNCTION();
#define HERE() __builtin_LINE()
_Static_assert(split == 100 && HERE() == __LINE__, "source line");
_Static_assert(_Generic(__builtin_LINE(), int: 1, default: 0), "line type");
_Static_assert(_Generic(__builtin_FILE(), const char *: 1, default: 0), "file type");

static int exercise_hints(void)
{
    _Alignas(32) char data[32] = {0};
    int reads=0, align=31, offset=0, after=0, expected=0, actual=3, unused=0;
    void *pointer = __builtin_assume_aligned(data + ++reads, ++align, ++offset);
    long first = __builtin_expect(++actual, expected++);
    long second = __builtin_expect_with_probability(++actual, expected++, 3.0 / 4.0);
    __builtin_prefetch(data + ++after);
    __builtin_prefetch(pointer, 0, 0);
    __builtin_prefetch(pointer, 0, 1);
    __builtin_prefetch(pointer, 0, 2);
    __builtin_prefetch(pointer, 1, 3, ++unused);
    __builtin_prefetch((const void *)0, 1, 1);
    return strcmp(__builtin_FUNCTION(), "exercise_hints") || pointer!=data+1 ||
           reads!=1 || align!=32 || offset!=1 || after!=1 || expected!=2 ||
           actual!=5 || first!=4 || second!=5 || unused!=1;
}

static int exercise_cache(void)
{
    unsigned char *page = mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return 1;
    unsigned char *start = page + 60;
    unsigned first = 0, last = 0;
    for (unsigned i = 11; i < 13; i++) {
#if defined(__aarch64__)
        unsigned code[] = {0x52800000u | (i << 5), 0xd65f03c0u};
#else
        unsigned char code[] = {0xb8, i, 0, 0, 0, 0xc3};
#endif
        memcpy(start, code, sizeof code);
        __builtin___clear_cache(start + first++, start + sizeof code + last++);
        __builtin___clear_cache(start, start);
        __builtin___clear_cache(start + 1, start);
        if (mprotect(page, 4096, PROT_READ | PROT_EXEC) ||
            ((unsigned (*)(void))start)() != i ||
            mprotect(page, 4096, PROT_READ | PROT_WRITE))
            return 1;
    }
    return munmap(page, 4096) || first!=2 || last!=2;
}

int main(void)
{
    return strcmp(file, "source-builtins-virtual.c") || *outside ||
           exercise_hints() || exercise_cache();
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "source-and-cache-builtins"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]),
                f"{cc} build source, branch, alignment, and cache builtins")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run source, branch, alignment, and cache builtins")

        invalid = [*prefix, *(["-x", "c"] if cc == gcc else []), "-std=gnu11",
                   "-c", "-o", root / "invalid-cache.o", "-"]
        for expression in ("__builtin_prefetch(p, hint, 3)",
                           "__builtin_prefetch(p, 0, hint)",
                           "__builtin___clear_cache(p)",
                           "__builtin_assume_aligned(p, 32, 1.25)",
                           "__builtin_FILE(1)",
                           "__builtin_expect_with_probability(value, 1, 0.5) = 2"):
            declaration = f"void f(void *p, int hint, long value) {{ (void)({expression}); }}"
            require(invoke(invalid, declaration), f"{cc} reject {expression}", success=False)
        for probability in ("1.25", "dynamic"):
            branch = f"""
extern void unlikely_path(void);
int f(int hint, double dynamic) {{
    if (__builtin_expect_with_probability(hint, 1, {probability}))
        unlikely_path();
    return hint;
}}
"""
            options = [*prefix, "-O2", *invalid[len(prefix):]] if cc == gcc else invalid
            require(invoke(options, branch),
                    f"{cc} reject invalid probability {probability}", success=False)

    assembly = root / "source-and-cache-builtins.s"
    require(invoke([mcc, *flags, "-std=gnu11", "-S", source, "-o", assembly]),
            "emit cache instructions")
    instructions = assembly.read_text()
    x86 = target.startswith("x86_64") if target else "prefetcht0" in instructions
    if x86:
        assert all(op in instructions for op in ("prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2"))
        assert "__clear_cache" not in instructions
    else:
        assert all(op in instructions for op in ("pldl1strm", "pldl3keep", "pldl2keep", "pstl1keep"))
        assert re.search(r"\bbl\s+__clear_cache\b", instructions)


def clear_padding(mcc, gcc, root, target, runner):
    source = root / "clear-padding.c"
    source.write_text("""
#include <stddef.h>
#include <string.h>

#if !__has_builtin(__builtin_clear_padding)
#error missing padding builtin
#endif

struct Element { unsigned char lead; unsigned value; unsigned char tail; };
struct Alternate { unsigned char lead, extra; unsigned value; unsigned char tail; };
union Overlay { struct Element elements[3]; struct Alternate overlap[3]; unsigned prefix; };
struct Bits {
    unsigned a:1; unsigned :1; unsigned b:1; unsigned :1;
    unsigned c:1; unsigned :1; unsigned d:1; unsigned :1; unsigned e:3;
};
struct __attribute__((packed)) Packed {
    unsigned :3; unsigned middle:10; unsigned :2; unsigned last:1;
};
union Mixed {
    struct __attribute__((packed)) { unsigned low:3; unsigned :5; } low;
    struct __attribute__((packed)) { unsigned :2; unsigned middle:3; } middle;
};
union Direct { unsigned low:3; unsigned wide:9; unsigned :25; };
static struct Element large[70000];

static int element_matches(const unsigned char *p, size_t extra)
{
    for (size_t i=0; i<sizeof(struct Element); i++) {
        size_t start = offsetof(struct Element, value);
        int keep = i<extra || i==offsetof(struct Element, lead) ||
                   (i>=start && i<start+sizeof(unsigned)) || i==offsetof(struct Element, tail);
        if (p[i] != (keep ? 255 : 0))
            return 0;
    }
    return 1;
}

static int variable_array(int rows, int cols, struct Element (*array)[rows][cols])
{
    memset(array, 255, sizeof *array);
    __builtin_clear_padding(array);
    for (int i=0; i<rows; i++)
        for (int j=0; j<cols; j++)
            if (!element_matches((unsigned char *)&(*array)[i][j], 0))
                return 1;
    return 0;
}

static int arrays(int rows, int cols)
{
    struct Element matrix[rows][cols];
    memset(large, 255, sizeof large);
    __builtin_clear_padding(&large);
    return variable_array(rows, cols, &matrix) ||
           !element_matches((unsigned char *)&large[0], 0) ||
           !element_matches((unsigned char *)&large[69999], 0);
}

static int overlapping_members(void)
{
    union Overlay overlay;
    union Mixed mixed;
    union Direct direct;
    struct Bits bits;
    struct Packed packed;
    memset(&overlay, 255, sizeof overlay);
    memset(&mixed, 255, sizeof mixed);
    memset(&direct, 255, sizeof direct);
    memset(&bits, 255, sizeof bits);
    memset(&packed, 255, sizeof packed);
    __builtin_clear_padding(&overlay);
    __builtin_clear_padding(&mixed);
    __builtin_clear_padding(&direct);
    __builtin_clear_padding((volatile struct Bits *)&bits);
    __builtin_clear_padding(&packed);
    if (!element_matches((unsigned char *)&overlay, sizeof(unsigned)))
        return 1;
    for (size_t i=1; i<3; i++)
        if (!element_matches((unsigned char *)&overlay + i * sizeof(struct Element), 2))
            return 1;
    unsigned char *b = (unsigned char *)&bits;
    unsigned char *p = (unsigned char *)&packed;
    unsigned char *m = (unsigned char *)&mixed;
    if (b[0]!=0x55 || b[1]!=0x07 || p[0]!=0xf8 || p[1]!=0x9f || m[0]!=0x1f)
        return 1;
    for (size_t i=2; i<sizeof bits; i++)
        if (b[i])
            return 1;
    for (size_t i=1; i<sizeof mixed; i++)
        if (m[i])
            return 1;
    unsigned char *d = (unsigned char *)&direct;
    for (size_t i=0; i<sizeof direct; i++)
        if (d[i] != (i<2 ? 255 : 0))
            return 1;
    return 0;
}

static int extended_precision(void)
{
    _Complex long double value = __builtin_complex(3.25L, -5.75L);
    unsigned char reference[sizeof value];
    unsigned char *bytes = (unsigned char *)&value;
    memcpy(reference, bytes, sizeof value);
#if defined(__x86_64__)
    for (size_t i=0; i<sizeof value; i++)
        if (i % sizeof(long double) >= 10) {
            bytes[i] = 255;
            reference[i] = 0;
        }
#endif
    __builtin_clear_padding(&value);
    return memcmp(reference, bytes, sizeof value);
}

int main(void)
{
    unsigned plain[] = {1, 2};
    unsigned *p = plain;
    __builtin_clear_padding(p++);
    return p!=plain+1 || plain[0]!=1 || plain[1]!=2 ||
           arrays(3, 2) || overlapping_members() || extended_precision();
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "clear-padding"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]), f"{cc} build clear padding")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} clear only padding bits")

        invalid = [*prefix, *(["-x", "c"] if cc == gcc else []), "-std=gnu11",
                   "-c", "-o", root / "invalid-padding.o", "-"]
        for declaration in ("void f(void *p) { __builtin_clear_padding(p); }",
                            "void f(int p) { __builtin_clear_padding(p); }",
                            "void f(const int *p) { __builtin_clear_padding(p); }",
                            "void f(_Atomic int *p) { __builtin_clear_padding(p); }",
                            "struct S; void f(struct S *p) { __builtin_clear_padding(p); }",
                            "struct S {int n; char tail[];}; "
                            "void f(struct S *p) { __builtin_clear_padding(p); }",
                            "void f(int n) { struct S {char c; int a[n];} s; __builtin_clear_padding(&s); }"):
            require(invoke(invalid, declaration), f"{cc} reject invalid object for clear padding", success=False)

    assembly = root / "large-padding-union.s"
    large_union = """
struct Element { char lead; unsigned value; char tail; };
union U { struct Element elements[70000]; unsigned prefix; };
void clear(union U *p) { __builtin_clear_padding(p); }
"""
    require(invoke([mcc, *flags, "-std=gnu11", "-S", "-o", assembly, "-"], large_union),
            "emit compact code for padding in a large union")
    assert assembly.stat().st_size < 16384, assembly.stat().st_size


def nonlocal_builtins(mcc, gcc, root, target, runner):
    sources = {
        "main": """
#include <stdint.h>
int receive(int width);

int main(void)
{
    uintptr_t before, after;
#if defined(__aarch64__)
    uint64_t fp_before, fp_after;
    __asm__ volatile ("mov %0, x19" : "=r"(before));
    __asm__ volatile ("str d8, %0" : "=m"(fp_before));
#else
    __asm__ volatile ("movq %%r12, %0" : "=r"(before));
#endif
    int result = receive(77);
#if defined(__aarch64__)
    __asm__ volatile ("mov %0, x19" : "=r"(after));
    __asm__ volatile ("str d8, %0" : "=m"(fp_after));
    if (fp_before != fp_after)
        return 1;
#else
    __asm__ volatile ("movq %%r12, %0" : "=r"(after));
#endif
    return result || before!=after;
}
""",
        "save": """
#include <stdint.h>

#if !__has_builtin(__builtin_setjmp) || !__has_builtin(__builtin_longjmp)
#error missing nonlocal jump builtins
#endif

#if defined(__aarch64__)
#define GET_STACK(output) __asm__ volatile ("mov %0, sp" : "=r"(output))
#else
#define GET_STACK(output) __asm__ volatile ("movq %%rsp, %0" : "=r"(output))
#endif

extern void escape(intptr_t *env, int depth);
static unsigned cleanups;
static void clean(unsigned *value) { cleanups += *value; }

static int variable_array(int width)
{
    struct { intptr_t before, env[5], after; } guard = {.before=31, .after=53};
    volatile unsigned char *keep = __builtin_alloca(47);
    keep[32] = 67;
    uintptr_t outside;
    GET_STACK(outside);
    for (unsigned i=0; i<32; i++) {
        {
            volatile unsigned char local[width];
            local[width - 1] = (unsigned char)i;
            uintptr_t saved;
            GET_STACK(saved);
            int resumed = __builtin_setjmp(guard.env);
            if (!resumed) {
                unsigned state __attribute__((cleanup(clean))) = 1;
                volatile unsigned char *lost = __builtin_alloca(width + i);
                lost[width - 1] = 71;
                escape(guard.env, 3);
                return 1;
            }
            uintptr_t restored;
            GET_STACK(restored);
            if (resumed!=1 || restored!=saved || local[width - 1]!=i ||
                keep[32]!=67 || cleanups || guard.before!=31 || guard.after!=53)
                return 2;
        }
#ifdef MCC_CHECK_VLA_RECLAIM
        uintptr_t restored;
        GET_STACK(restored);
        if (restored!=outside)
            return 3;
#endif
    }
    return 0;
}

static int multiple_buffers(void)
{
    intptr_t env[2][5];
    volatile uintptr_t saved[2];
    volatile unsigned char *volatile live[2];
    for (unsigned i=0; i<2; i++) {
        live[i] = __builtin_alloca(33+i);
        live[i][0] = (unsigned char)(73+i);
        uintptr_t stack;
        GET_STACK(stack);
        saved[i] = stack;
        if (__builtin_setjmp(env[i])) {
            GET_STACK(stack);
            return stack!=saved[0] || *live[0]!=73;
        }
    }
    escape(env[0], 0);
    return 1;
}

int receive(int width) { return variable_array(width) || multiple_buffers(); }
""",
        "jump": """
#include <stdint.h>

void escape(intptr_t *env, int depth)
{
    volatile unsigned char scratch[37+depth];
    scratch[depth] = (unsigned char)depth;
#if defined(__aarch64__)
    uint64_t fp_sentinel = 501;
    __asm__ volatile ("mov x19, #501" ::: "x19");
    __asm__ volatile ("ldr d8, %0" :: "m"(fp_sentinel) : "d8");
#else
    __asm__ volatile ("movq $501, %%r12" ::: "r12");
#endif
    if (depth > 0)
        escape(env, depth-1);
    if (scratch[depth] != depth)
        __builtin_trap();
    __builtin_longjmp(env, 1.0);
}
""",
    }
    flags = [f"--target={target}"] if target else []
    object_files = {}
    for part, content in sources.items():
        source = root / f"builtin-jump-{part}.c"
        source.write_text(content)
        for label, cc in (("mcc", mcc), ("gcc", gcc)):
            obj = root / f"builtin-jump-{part}-{label}.o"
            options = [*flags, "-DMCC_CHECK_VLA_RECLAIM"] if cc == mcc else []
            require(invoke([cc, *options, "-std=gnu11", "-c", source, "-o", obj]),
                    f"{cc} compile nonlocal jump {part}")
            object_files[part, label] = obj

    combinations = [("mcc", "mcc"), ("gcc", "gcc")]
    if not target:
        combinations.extend((("mcc", "gcc"), ("gcc", "mcc")))
    binary = root / "builtin-jump"
    for saver, jumper in combinations:
        cc = gcc if saver == jumper == "gcc" else mcc
        prefix = [cc, *(flags if cc == mcc else [])]
        objects = [object_files["main", saver], object_files["save", saver], object_files["jump", jumper]]
        require(invoke([*prefix, *objects, "-o", binary]), f"link {saver} setjmp with {jumper} longjmp")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"run {saver} setjmp with {jumper} longjmp")

    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else []), *(["-x", "c"] if cc == gcc else [])]
        invalid = [*prefix, "-std=gnu11", "-Werror", "-c", "-o", root / "invalid-jump.o", "-"]
        for expression in ("__builtin_setjmp()", "__builtin_setjmp(5)",
                           "__builtin_longjmp(env)", "__builtin_longjmp(env, 0)",
                           "__builtin_longjmp(env, 2)", "__builtin_longjmp(env, n)",
                           "&__builtin_setjmp", "&__builtin_longjmp"):
            source = f"void f(long *env, int n) {{ (void)({expression}); }}"
            require(invoke(invalid, source), f"{cc} reject {expression}", success=False)


def frame_queries(mcc, gcc, root, target, runner):
    header = root / "frame-query.h"
    header.write_text("""
#include <stdint.h>
struct Frames {
    void *main_frame, *middle_frame, *leaf_frame, *caller, *grandparent;
    void *middle_return, *leaf_return, *caller_return;
    uintptr_t cfa, caller_sp, stored_return;
};
void sample_middle(struct Frames *);
void sample_leaf(struct Frames *);
""")
    sources = {
        "leaf": """
#include "frame-query.h"
void sample_leaf(struct Frames *s)
{
    volatile unsigned char *scratch = __builtin_alloca(37);
    scratch[36] = 19;
    s->leaf_frame = __builtin_frame_address(0);
    s->caller = __builtin_frame_address(1);
    s->grandparent = __builtin_frame_address(1 + 1);
    s->cfa = (uintptr_t)__builtin_dwarf_cfa();
    s->leaf_return = __builtin_extract_return_addr(__builtin_return_address(0));
    s->stored_return = ((const uintptr_t *)s->leaf_frame)[1];
    s->caller_return = __builtin_frob_return_addr(__builtin_return_address(1));
    if (scratch[36] != 19)
        __builtin_trap();
}
""",
        "middle": """
#include "frame-query.h"
void sample_middle(struct Frames *s)
{
    s->middle_frame = __builtin_frame_address(0);
    s->middle_return = __builtin_return_address(0);
#if defined(__aarch64__)
    __asm__ volatile ("mov %0, sp" : "=r"(s->caller_sp));
#else
    __asm__ volatile ("movq %%rsp, %0" : "=r"(s->caller_sp));
#endif
    sample_leaf(s);
}
""",
        "main": """
#include "frame-query.h"

#if !__has_builtin(__builtin_frame_address) || !__has_builtin(__builtin_return_address) || \\
    !__has_builtin(__builtin_extract_return_addr) || !__has_builtin(__builtin_frob_return_addr) || \\
    !__has_builtin(__builtin_dwarf_cfa) || !__has_builtin(__builtin_dwarf_sp_column) || \\
    !__has_builtin(__builtin_eh_return_data_regno)
#error missing frame builtins
#endif

_Static_assert(_Generic(__builtin_frame_address(0), void *:1, default:0), "frame pointer type");
_Static_assert(_Generic(__builtin_dwarf_sp_column(), unsigned int:1, default:0), "SP column type");
_Static_assert(_Generic(__builtin_eh_return_data_regno(0), int:1, default:0), "EH register type");

int main(void)
{
    struct Frames s = {.main_frame=__builtin_frame_address(0)};
    sample_middle(&s);
    if (!s.leaf_frame || s.caller!=s.middle_frame || s.grandparent!=s.main_frame ||
        s.cfa!=s.caller_sp ||
        !s.leaf_return || (uintptr_t)s.leaf_return!=s.stored_return)
        return 1;
#if defined(__aarch64__) && !defined(EXPECT_ANCESTOR_RETURN)
    if (s.caller_return)
        return 2;
#else
    if (!s.caller_return || s.caller_return!=s.middle_return)
        return 2;
#endif
    void *pointers[] = {&s, 0}, **p = pointers;
    if (__builtin_frob_return_addr(__builtin_extract_return_addr(*p++)) != &s || p!=pointers+1)
        return 3;
#if defined(__aarch64__)
    unsigned sp = 31, last = 3;
#else
    unsigned sp = 7, last = 1;
#endif
    if (__builtin_dwarf_sp_column()!=sp ||
        __builtin_eh_return_data_regno(0)!=0 || __builtin_eh_return_data_regno(1)!=1 ||
        __builtin_eh_return_data_regno(4)!=-1 || __builtin_eh_return_data_regno(-1)!=-1)
        return 4;
    return __builtin_eh_return_data_regno(2)!=(last>=2 ? 2 : -1) ||
           __builtin_eh_return_data_regno(3)!=(last>=3 ? 3 : -1);
}
""",
    }
    paths = []
    for part, content in sources.items():
        source = root / f"frame-query-{part}.c"
        source.write_text(content)
        paths.append(source)
    flags = [f"--target={target}"] if target else []
    binary = root / "frame-query"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        options = ["-DEXPECT_ANCESTOR_RETURN"] if cc == mcc else [
            "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls"
        ]
        require(invoke([*prefix, *options, "-std=gnu11", *paths, "-o", binary]),
                f"{cc} build frame queries")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run frame queries")

        input_options = [] if cc == mcc else ["-x", "c"]
        compile_source = [*prefix, *input_options, "-std=gnu11", "-S", "-o", root / "frame-query.s"]
        for expression in ("__builtin_frame_address(n)", "__builtin_return_address(n)",
                           "__builtin_eh_return_data_regno(n)", "__builtin_dwarf_cfa(1)"):
            declaration = f"void f(int n) {{ (void)({expression}); }}"
            require(invoke([*compile_source, "-"], declaration), f"{cc} reject {expression}", success=False)

        deeper = "void *f(void) { return __builtin_frame_address(1); }"
        for enabled, warning_options in ((False, []), (True, ["-Wframe-address"]), (True, ["-Wall"]),
                                         (False, ["-Wall", "-Wno-frame-address"])):
            result = invoke([*compile_source, *warning_options, "-"], deeper)
            require(result, f"{cc} compile frame query with warning options")
            assert ("[-Wframe-address]" in result.stderr) == enabled, result.stderr
        result = invoke([*compile_source, "-Werror=frame-address", "-"], deeper)
        require(result, f"{cc} promote frame-address warning", success=False)
        assert "[-Werror=frame-address]" in result.stderr, result.stderr
        require(invoke([*prefix, *input_options, "-std=gnu11", "-fsyntax-only",
                        "-Werror=frame-address", "-"], deeper),
                f"{cc} check syntax without code generation diagnostics")
        muted = '#pragma GCC diagnostic ignored "-Wframe-address"\n' + deeper
        require(invoke([*compile_source, "-Wall", "-Werror", "-"], muted),
                f"{cc} suppress frame-address warning with pragma")

    huge = "void *f(void) { return __builtin_frame_address(4294967295u); }"
    assembly = root / "deep-frame-query.s"
    require(invoke([mcc, *flags, "-std=gnu11", "-S", "-o", assembly, "-"], huge),
            "lower deep frame queries without unrolling")
    assert assembly.stat().st_size < 8192, assembly.stat().st_size


def explicit_stack_builtins(mcc, gcc, root, target, runner):
    source = root / "explicit-stack.c"
    source.write_text("""
#include <stdint.h>

#if !__has_builtin(__builtin_stack_save) || !__has_builtin(__builtin_stack_restore)
#error missing stack builtins
#endif
#if defined(__aarch64__)
#define STACK(out) __asm__ volatile ("mov %0, sp" : "=r"(out))
#else
#define STACK(out) __asm__ volatile ("movq %%rsp, %0" : "=r"(out))
#endif

static int dynamic_allocations(unsigned width)
{
    uintptr_t raw;
    void *initial = __builtin_stack_save();
    STACK(raw);
    if (raw!=(uintptr_t)initial)
        return 1;
    volatile unsigned char *keep = __builtin_alloca_with_align(width, 512);
    keep[0] = 47;
    keep[width-1] = 53;
    void *outer = __builtin_stack_save();
    if ((uintptr_t)outer>=(uintptr_t)initial || (uintptr_t)keep%64)
        return 2;

    for (unsigned i=0; i<32; i++) {
        {
            volatile unsigned char local[width];
            local[width-1] = (unsigned char)i;
            void *inner = __builtin_stack_save();
            volatile unsigned char *temp = __builtin_alloca(262144 + i);
            temp[262143] = 59;
            STACK(raw);
            if (raw>=(uintptr_t)inner)
                return 3;

            void *positions[] = {inner, outer}, **position = positions;
            __builtin_stack_restore(*position++);
            STACK(raw);
            if (raw!=(uintptr_t)inner || position!=positions+1 || local[width-1]!=i ||
                keep[0]!=47 || keep[width-1]!=53)
                return 4;
        }
#ifdef MCC_CHECK_EXPLICIT_RECLAIM
        STACK(raw);
        if (raw!=(uintptr_t)outer)
            return 5;
#endif
        __builtin_stack_restore(outer);
    }
    __builtin_stack_restore(initial);
    STACK(raw);
    return raw!=(uintptr_t)initial;
}

static int discarded_scopes(unsigned width)
{
    void *point = __builtin_stack_save();
    uintptr_t raw;
    {
        volatile unsigned char outer[width];
        outer[width-1] = 61;
        {
            volatile unsigned char inner[width + 1];
            inner[width] = 67;
            __builtin_stack_restore(point);
        }
#ifdef MCC_CHECK_EXPLICIT_RECLAIM
        STACK(raw);
        if (raw!=(uintptr_t)point)
            return 1;
#endif
    }
    STACK(raw);
    return raw!=(uintptr_t)point;
}

static __attribute__((noinline)) void escape(intptr_t *buffer)
{
    __builtin_longjmp(buffer, 1);
}

static int mixed_checkpoints(unsigned width)
{
    intptr_t buffer[5];
    void *initial = __builtin_stack_save();
    volatile unsigned char *keep = __builtin_alloca(width);
    keep[width-1] = 71;
    void *saved = __builtin_stack_save();
    if (!__builtin_setjmp(buffer)) {
        volatile unsigned char *temp = __builtin_alloca(width + 32);
        temp[width+31] = 73;
        void *inner = __builtin_stack_save();
        volatile unsigned char *extra = __builtin_alloca(width + 64);
        extra[width+63] = 79;
        __builtin_stack_restore(inner);
        if (temp[width+31] != 73)
            return 2;
        escape(buffer);
    }
    uintptr_t raw;
    STACK(raw);
    if (raw!=(uintptr_t)saved || keep[width-1]!=71)
        return 1;
    __builtin_stack_restore(initial);
    STACK(raw);
    return raw!=(uintptr_t)initial;
}

int main(void)
{
    volatile unsigned width = 77;
    int code = dynamic_allocations(width);
    if (code)
        return code;
    if (discarded_scopes(width + 6))
        return 6;
    return mixed_checkpoints(width + 12) ? 7 : 0;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "explicit-stack"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        options = ["-DMCC_CHECK_EXPLICIT_RECLAIM"] if cc == mcc else ["-fno-optimize-sibling-calls"]
        require(invoke([*prefix, *options, "-std=gnu11", source, "-o", binary]),
                f"{cc} build explicit stack checkpoints")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} restore explicit stack checkpoints")
        input_options = [] if cc == mcc else ["-x", "c"]
        invalid = [*prefix, *input_options, "-std=gnu11", "-Werror", "-fsyntax-only", "-"]
        for expression in ("__builtin_stack_save(0)", "__builtin_stack_restore()",
                           "__builtin_stack_restore(7)"):
            require(invoke(invalid, f"void f(void) {{ (void)({expression}); }}"),
                    f"{cc} reject {expression}", success=False)


def dwarf_register_sizes(mcc, gcc, root, target, runner):
    source = root / "dwarf-register-sizes.c"
    source.write_text("""
#include <stdint.h>
#include <string.h>

#if !__has_builtin(__builtin_init_dwarf_reg_size_table) || !__has_builtin(__builtin_extend_pointer)
#error missing unwind builtins
#endif

_Static_assert(__builtin_types_compatible_p(
    __typeof__(__builtin_init_dwarf_reg_size_table((void *)0)), void), "table result");
_Static_assert(__builtin_types_compatible_p(
    __typeof__(__builtin_extend_pointer((void *)0)), unsigned long), "pointer result");

static unsigned register_size(unsigned index, unsigned untouched)
{
#if defined(__aarch64__)
    if (index<32 || index==46 || (index>=72 && index<80) || index==96)
        return 8;
    if (index>=48 && index<96)
        return 0;
#else
    if (index<17)
        return 8;
#endif
    return untouched;
}

static int check(unsigned char untouched)
{
    _Alignas(16) unsigned char storage[258];
    unsigned char *unaligned = storage + 1;
    memset(storage, untouched, sizeof storage);
    unsigned char *arguments[] = {unaligned, storage}, **next = arguments;
    __builtin_init_dwarf_reg_size_table(*next++);
    if (next!=arguments+1 || storage[0]!=untouched || storage[257]!=untouched)
        return 1;
    for (unsigned i=0; i<256; i++)
        if (unaligned[i] != register_size(i, untouched))
            return 2;
    return __builtin_extend_pointer(*next++)!=(uintptr_t)storage ||
           next!=arguments+2 || __builtin_extend_pointer((void *)0)!=0;
}

int main(void) { return check(0xa5) || check(0x6f); }
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "dwarf-register-sizes"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]),
                f"{cc} build DWARF register-size test")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} populate only the designated DWARF register sizes")
        input_options = [] if cc == mcc else ["-x", "c"]
        invalid = [*prefix, *input_options, "-std=gnu11", "-Werror", "-fsyntax-only", "-"]
        for expression in ("__builtin_init_dwarf_reg_size_table()",
                           "__builtin_init_dwarf_reg_size_table(1)",
                           "__builtin_init_dwarf_reg_size_table(p, p)",
                           "__builtin_extend_pointer()", "__builtin_extend_pointer(1)",
                           "&__builtin_init_dwarf_reg_size_table"):
            require(invoke(invalid, f"void f(void *p) {{ (void)({expression}); }}"),
                    f"{cc} reject {expression}", success=False)


def classify_types(mcc, gcc, root, target, runner):
    source = root / "classify-type.c"
    source.write_text("""
#if !__has_builtin(__builtin_classify_type)
#error missing classify_type
#endif

enum E { A, B };
struct S { int number; };
union U { int number; };
typedef int Vector __attribute__((vector_size(16)));
typedef int Function(void);
static int function(void) { return 1; }
static int array[2];
static int effects;

_Static_assert(__builtin_classify_type() == -1, "empty argument list");
_Static_assert(__builtin_classify_type((enum E)0) == 1, "promoted enum");
_Static_assert(__builtin_classify_type((_Bool)0) == 1, "promoted bool");
_Static_assert(__builtin_classify_type((char)0) == 1, "promoted char");
_Static_assert(__builtin_classify_type((double)0) == 8, "real expression");
_Static_assert(__builtin_classify_type((__complex__ int)0) == 9, "complex expression");
_Static_assert(__builtin_classify_type((Vector){0}) == 19, "vector expression");
_Static_assert(__builtin_classify_type((struct S){0}) == 12, "structure expression");
_Static_assert(__builtin_classify_type((union U){0}) == 13, "union expression");
_Static_assert((__builtin_classify_type)(array) == 5, "array decay");
_Static_assert(__builtin_classify_type(function) == 5, "function decay");
_Static_assert(__builtin_classify_type(effects++) == 1, "constant type classification");

_Static_assert(__builtin_classify_type(void) == 0, "void type");
_Static_assert(__builtin_classify_type(enum E) == 3, "enum type");
_Static_assert(__builtin_classify_type(_Bool) == 4, "bool type");
_Static_assert(__builtin_classify_type(int *) == 5, "pointer type");
_Static_assert(__builtin_classify_type(Function) == 10, "function type");
_Static_assert(__builtin_classify_type(int [3]) == 14, "array type");
_Static_assert(__builtin_classify_type(__typeof__(array)) == 14, "typeof array");
_Static_assert(__builtin_classify_type(Vector) == 19, "vector type");

int main(void)
{
    int n=1, m=2;
    if (__builtin_classify_type(*(int (*)[++n])0) != 5 ||
        __builtin_classify_type(int [++m]) != 14 || n!=1 || m!=2 || effects)
        return 1;
    if (__builtin_classify_type(n++, m++) != 1)
        return 2;
    return n!=2 || m!=3;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "classify-type"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]), f"{cc} compile type classes")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} classify types and expressions")
        input_options = [] if cc == mcc else ["-x", "c"]
        invalid = [*prefix, *input_options, "-std=gnu11", "-c", "-o", root / "invalid-class.o", "-"]
        for declaration in ("int i=__builtin_classify_type((void)0);",
                            "struct S; int f(struct S *x) { return __builtin_classify_type(*x); }",
                            "int f(void); int g(void) { return f(int); }",
                            "int i=__builtin_classify_type(1, 2);"):
            require(invoke(invalid, declaration), f"{cc} reject invalid type classification", success=False)


def attribute_queries(mcc, gcc, root, target, runner):
    source = root / "attribute-queries.c"
    source.write_text("""
#if !__has_builtin(__builtin_has_attribute)
#error missing attribute query
#endif
#define HAS(subject, ...) __builtin_has_attribute(subject, __VA_ARGS__)
#define CHECK(test) _Static_assert(test, #test)

typedef int Align16 __attribute__((aligned(16)));
typedef int AlignDefault __attribute__((aligned));
typedef Align16 Align32 __attribute__((aligned(32)));
static Align16 layered __attribute__((aligned(32)));
static int default_aligned __attribute__((aligned));
_Alignas(16) static int iso_aligned;
extern int redeclared __attribute__((aligned(8)));
extern int redeclared __attribute__((aligned(32)));
static int unannotated;
static int elements[2] __attribute__((aligned(32)));

CHECK(!HAS(int, aligned));
CHECK(_Generic(HAS(layered, aligned), _Bool: 1, default: 0));
CHECK(HAS(Align16, aligned(16)));
CHECK(HAS(AlignDefault, aligned(16)));
CHECK(HAS(Align32, aligned(32)) && !HAS(Align32, aligned(16)));
CHECK(HAS(layered, __aligned__(16)) && HAS(layered, aligned(32)));
CHECK(!HAS(layered, aligned(64)));
CHECK(HAS(default_aligned, aligned) && !HAS(default_aligned, aligned(16)));
CHECK(HAS(iso_aligned, aligned(16)) && !HAS(__typeof__(iso_aligned), aligned));
CHECK(HAS(redeclared, aligned(8)) && HAS(redeclared, aligned(32)));
CHECK(HAS(*&layered, aligned) && HAS((0, layered), aligned()));
CHECK(!HAS(&layered, aligned) && !HAS(layered + 0, aligned));
CHECK(!HAS(unannotated, aligned) && !HAS(elements[0], aligned));

struct __attribute__((packed, aligned(8))) Packet { int value; };
typedef struct Packet AlignedPacket __attribute__((aligned(32)));
struct Fields {
    char lead;
    int a __attribute__((packed));
    int b __attribute__((aligned(16)));
};
#pragma pack(push, 1)
struct PragmaPacked { char lead; int value; };
#pragma pack(pop)
struct AnonymousField { struct { int value __attribute__((aligned(16))); }; };
enum __attribute__((packed)) Digit { DigitZero, DigitOne };
static struct Packet packet;
static struct Fields fields;
extern struct AnonymousField anonymous;
union __attribute__((transparent_union)) U { int a; unsigned int b; };
struct __attribute__((designated_init)) OnlyDesignated { int value; };
typedef int Vec __attribute__((vector_size(16)));
static Vec vectors;

CHECK(HAS(struct Packet, packed) && HAS(packet, aligned(8)));
CHECK(HAS(AlignedPacket, aligned(8)) && !HAS(AlignedPacket, aligned(32)));
CHECK(!HAS(packet.value, packed) && HAS(fields.a, packed));
CHECK(HAS(fields.b, aligned(16)) && !HAS(fields.b, aligned(8)));
CHECK(!HAS(struct PragmaPacked, packed) && HAS(anonymous.value, aligned(16)));
CHECK(HAS(enum Digit, packed));
CHECK(HAS(union U, transparent_union) && HAS(struct OnlyDesignated, designated_init));
CHECK(HAS(Vec, vector_size) && HAS(vectors, vector_size(16)) && !HAS(Vec, vector_size(8)));

int reference_object;
extern int aliased __attribute__((alias("reference_object")));
extern int weak_object __attribute__((weak));
extern int pragma_weak;
CHECK(!HAS(pragma_weak, weak));
#pragma weak pragma_weak
CHECK(HAS(pragma_weak, weak));
static int weak_reference __attribute__((weakref("reference_object")));
extern int hidden __attribute__((visibility("hidden")));
#pragma GCC visibility push(hidden)
extern int implicit_hidden;
#pragma GCC visibility pop
static int kept __attribute__((used, section(".kept")));
static int old_object __attribute__((deprecated("old " "message")));
static __attribute__((noinline)) int out_of_line(void) { return 1; }
extern inline __attribute__((gnu_inline)) int old_inline(void) { return 2; }
static void start(void) __attribute__((constructor));
static void finish(void) __attribute__((destructor(200)));
static void start(void) {}
static void finish(void) {}
extern void *reserve(int, int) __attribute__((alloc_size(1, 2)));

CHECK(HAS(aliased, alias("reference_object")) && !HAS(aliased, alias("other")));
CHECK(HAS(weak_object, weak) && HAS(weak_reference, weakref));
CHECK(HAS(hidden, visibility("hidden")) && !HAS(hidden, visibility("default")));
CHECK(!HAS(implicit_hidden, visibility));
CHECK(HAS(kept, used) && HAS(kept, section(".kept")));
CHECK(HAS(old_object, deprecated("old message")) && !HAS(old_object, deprecated("new")));
CHECK(HAS(out_of_line, noinline) && HAS(old_inline, gnu_inline));
CHECK(HAS(start, constructor) && !HAS(start, constructor(65535)));
CHECK(HAS(finish, destructor(200)) && !HAS(finish, destructor(201)));
CHECK(HAS(reserve, alloc_size) && HAS(reserve, alloc_size(2)) && HAS(reserve, alloc_size(1, 2)));
CHECK(!HAS(&reserve, alloc_size));

typedef int MachineByte __attribute__((mode(QI)));
CHECK(!HAS(MachineByte, mode(QI)));
static void release(int *p) { *p = 0; }
static void other_release(int *p) { *p = 1; }
extern int never_called(void);
int main(void)
{
    int local __attribute__((cleanup(release))) = 7;
    int side = 1;
    CHECK(HAS(local, cleanup(release)) && !HAS(local, cleanup(other_release)));
    CHECK(!HAS((++side, local), cleanup));
    CHECK(!HAS((int (*)[++side])0, aligned));
    CHECK(!HAS(int [++side], aligned));
    return HAS(never_called(), aligned) || side != 1 || local != 7;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "attribute-queries"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=c11", "-pedantic-errors", "-Wno-deprecated-declarations",
                        source, "-o", binary]), f"{cc} build compile-time attribute queries")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} attribute operands are unevaluated")
        input_flags = [] if cc == mcc else ["-x", "c"]
        invalid = [*prefix, *input_flags, "-std=gnu11", "-Werror", "-fsyntax-only", "-"]
        for code in ("int x = __builtin_has_attribute(int, just_an_unknown_attribute);",
                     "int x = __builtin_has_attribute(int, packed(1));",
                     "int x = __builtin_has_attribute(int, aligned(3));",
                     "int x = __builtin_has_attribute(int, visibility(1));",
                     "int x = __builtin_has_attribute(int, mode(BAD_MACHINE_MODE));"):
            require(invoke(invalid, code), f"{cc} reject {code}", success=False)


def speculation_safe_values(mcc, gcc, root, target, runner):
    source = root / "speculation-safe-value.c"
    source.write_text("""
#if !__HAVE_SPECULATION_SAFE_VALUE || !__has_builtin(__builtin_speculation_safe_value)
#error missing speculation-safe value
#endif
#if !__has_builtin(__builtin_speculation_safe_value_16)
#error missing width-specific builtin
#endif

#define SAFE __builtin_speculation_safe_value
__extension__ typedef unsigned __int128 Wide;
static int values[4] = {3, 5, 7, 11};
static volatile unsigned first_calls, fallback_calls;
static unsigned first(void) { ++first_calls; return 2; }
static unsigned fallback(void) { ++fallback_calls; return 0; }

_Static_assert(_Generic(SAFE((short)0), short: 1, default: 0), "short result type");
_Static_assert(_Generic(SAFE(values), int *: 1, default: 0), "array conversion");
_Static_assert(_Generic(SAFE((Wide)0), Wide: 1, default: 0), "wide result type");
_Static_assert(_Generic(__builtin_speculation_safe_value_1(2), unsigned char: 1, default: 0),
               "one-byte builtin");
_Static_assert(_Generic(__builtin_speculation_safe_value_2(2), unsigned short: 1, default: 0),
               "two-byte builtin");
_Static_assert(_Generic(__builtin_speculation_safe_value_4(2), unsigned int: 1, default: 0),
               "four-byte builtin");
_Static_assert(_Generic(__builtin_speculation_safe_value_8(2), unsigned long: 1, default: 0),
               "eight-byte builtin");
_Static_assert(_Generic(__builtin_speculation_safe_value_16(2), Wide: 1, default: 0),
               "sixteen-byte builtin");
_Static_assert(_Generic(__builtin_speculation_safe_value_ptr(values), void *: 1, default: 0),
               "pointer builtin");

int bounded(unsigned index)
{
    if (index >= 4)
        return -1;
    return values[SAFE(index, 0u)];
}

int main(void)
{
    unsigned index = SAFE(first(), fallback());
    if (index != 2 || first_calls != 1 || fallback_calls != 1)
        return 1;
    if (bounded(index) != 7 || bounded(4) != -1 ||
        *SAFE(values + index, (void *)0) != 7 || SAFE(values) != values)
        return 2;
    Wide wide = ((Wide)1 << 111) + index;
    if (SAFE(wide) != wide || __builtin_speculation_safe_value_16(wide) != wide)
        return 3;
    if (__builtin_speculation_safe_value_4(9, fallback(), fallback()) != 9 || fallback_calls != 3)
        return 4;
    volatile signed char small = -23;
    return SAFE(small) != -23;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "speculation-safe-value"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=c11", "-pedantic-errors", "-ffreestanding", "-fno-builtin",
                        source, "-o", binary]), f"{cc} build speculation-safe values in freestanding C11")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} speculation-safe value semantics")
        input_flags = [] if cc == mcc else ["-x", "c"]
        invalid = [*prefix, *input_flags, "-std=gnu11", "-Werror", "-c", "-o", root / "invalid-safe.o", "-"]
        for code in ("int f(void) { return __builtin_speculation_safe_value(); }",
                     "int f(void) { return __builtin_speculation_safe_value(0, 0, 0); }",
                     "int f(void) { return __builtin_speculation_safe_value(0.0); }",
                     "int f(_Bool b) { return __builtin_speculation_safe_value(b); }",
                     "int f(short x) { return __builtin_speculation_safe_value(x, 0); }",
                     "int *f(int *p) { return __builtin_speculation_safe_value(p, 0); }",
                     "int n = __builtin_speculation_safe_value(1);",
                     "void *f(void) { return (void *)&__builtin_speculation_safe_value; }"):
            require(invoke(invalid, code), f"{cc} reject {code}", success=False)

    assembly = root / "speculation-safe-value.s"
    require(invoke([mcc, *flags, "-std=gnu11", "-S", source, "-o", assembly]),
            "emit a speculation barrier")
    instructions = assembly.read_text()
    bounded = instructions.split("\nbounded:\n", 1)[1].split("\t.size bounded,", 1)[0]
    if (target or os.uname().machine).startswith("x86_64"):
        pattern = r"\tje .*\n[\s\S]*\n\tlfence\n"
    else:
        pattern = r"\tcbz .*\n[\s\S]*\n\tdsb sy\n\tisb\n"
    assert re.search(pattern, bounded), bounded


def noreturn_functions(mcc, gcc, root, target, runner):
    source = root / "noreturn-functions.c"
    source.write_text("""
#include <stdint.h>
#include <stdnoreturn.h>

#if !__has_attribute(__noreturn__) || !__has_builtin(__builtin_exit)
#error missing noreturn support
#endif
#define HAS(subject) __builtin_has_attribute(subject, __noreturn__)

static intptr_t continuation[5];
static unsigned visits;
static noreturn void finish(void);
static void finish(void)
{
    ++visits;
    __builtin_longjmp(continuation, 1);
}
static void stop(void) __attribute__((__noreturn__()));
static void stop(void) { finish(); }

typedef void (*Callback)(void) __attribute__((__noreturn__));
static Callback inherited = finish;
static void (*declared)(void) __attribute__((__noreturn__)) = finish;
static void (* __attribute__((__noreturn__)) placed)(void) = finish;
struct Handler { void (*callback)(void) __attribute__((__noreturn__)); };
static struct Handler handler = {finish};

_Static_assert(HAS(finish) && HAS(declared) && HAS(placed) && HAS(stop), "declaration attributes");
_Static_assert(!HAS(Callback) && !HAS(inherited) && !HAS(&finish), "type and declaration distinction");
_Static_assert(HAS(handler.callback), "member attribute");
_Static_assert(__builtin_types_compatible_p(__typeof__(&finish), Callback), "qualified function pointer");
_Static_assert(!__builtin_types_compatible_p(void (*)(void), Callback), "distinct pointer qualification");

static noreturn void invoke(void (*callback)(void) __attribute__((__noreturn__)))
{
    _Static_assert(HAS(callback), "parameter attribute");
    (*callback)();
}

static noreturn void route(unsigned option)
{
    switch (option) {
    case 0: invoke(declared);
    case 1: (*inherited)();
    case 2: handler.callback();
    default: placed();
    }
}

noreturn void cycle(void)
{
    while (2 > 1) {
        if (0)
            break;
    }
}

noreturn void known_switch(void)
{
    switch (2) {
    case 2: finish();
    default: break;
    }
}

noreturn void known_logical(void) { (void)(0 || (finish(), 1)); }
noreturn void known_exit(void) { __builtin_exit(0); }

int main(void)
{
    for (unsigned i = 0; i < 4; i++) {
        if (!__builtin_setjmp(continuation))
            route(i);
    }
    return visits != 4;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "noreturn-functions"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else ["-O0", "-Wno-cpp"])]
        require(invoke([*prefix, "-std=c11", "-pedantic-errors", "-Werror", source, "-o", binary]),
                f"{cc} compile noreturn declarations")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} noreturn callback and nonlocal jump")
        input_flags = [] if cc == mcc else ["-x", "c"]
        syntax = [*prefix, *input_flags, "-fsyntax-only", "-Werror"]
        for code in ("_Noreturn int invalid;",
                     "_Noreturn struct Empty;",
                     "_Noreturn typedef void Invalid(void);",
                     "_Noreturn void (*invalid)(void);",
                     "void outer(_Noreturn void (*invalid)(void));",
                     "_Noreturn void invalid(void) { if (0) return; __builtin_trap(); }",
                     """typedef void (*Callback)(void) __attribute__((noreturn));
                        void ordinary(void);
                        Callback invalid = ordinary;"""):
            require(invoke([*syntax, "-std=c11", "-"], code), f"{cc} reject {code}", success=False)
        require(invoke([*syntax, "-std=c99", "-pedantic-errors", "-"], "_Noreturn void stop(void);"),
                f"{cc} reject the C11 specifier in strict C99", success=False)
        compile_command = [*prefix, *input_flags, "-std=c11", "-Werror", "-S", "-o", root / "noreturn.s"]
        for code in ("_Noreturn void invalid(void) {}",
                     "_Noreturn void invalid(int x) { if (x) __builtin_trap(); }",
                     "void invalid(void) {} _Noreturn void invalid(void);"):
            require(invoke([*compile_command, "-"], code), f"{cc} detect a reachable return in {code}",
                    success=False)
        hosted = "void abort(void); _Noreturn void stop(void) { abort(); }"
        require(invoke([*compile_command, "-"], hosted), f"{cc} recognize hosted abort")
        require(invoke([*compile_command, "-ffreestanding", "-"], hosted),
                f"{cc} respect freestanding compilation", success=False)
        require(invoke([*compile_command, "-fno-builtin-abort", "-"], hosted),
                f"{cc} respect per-function builtin disabling", success=False)
        posix = "void _exit(int); _Noreturn void stop(void) { _exit(42); }"
        require(invoke([*compile_command, "-"], posix), f"{cc} require POSIX _exit declaration attributes",
                success=False)
        require(invoke([*compile_command, "-std=gnu11", "-"], posix), f"{cc} recognize GNU mode _exit")
        declarations = """
#include <setjmp.h>
#include <stdlib.h>
_Noreturn void from_system_header(jmp_buf state) { longjmp(state, 1); }
_Noreturn void exit_from_system_header(void) { quick_exit(3); }
"""
        require(invoke([*compile_command, "-fno-builtin", "-"], declarations),
                f"{cc} honor explicit noreturn attributes from system declarations")


def warn_unused_result(mcc, gcc, root, target):
    source = root / "warn-unused-result.c"
    source.write_text("""
#if !__has_attribute(__warn_unused_result__)
#error missing warn_unused_result attribute
#endif
#include <stdio.h>
#include <status-check.h>

typedef int Plain(void);
typedef int Required(void) __attribute__((warn_unused_result));
typedef int (*Pointer)(void) __attribute__((warn_unused_result));
extern int checked(void) __attribute__((__warn_unused_result__()));
extern int unchecked(void);
extern Required from_type;
extern Pointer callback;
extern int (* __attribute__((warn_unused_result)) placed)(void);
extern int (*merged)(void);
extern Pointer merged;
struct Handler { int (*callback)(void) __attribute__((warn_unused_result)); };
extern struct Handler handler;
extern int sink(int);
static volatile int output;

#define HAS(declaration) __builtin_has_attribute(declaration, warn_unused_result)
_Static_assert(HAS(checked) && HAS(Required) && HAS(from_type) && HAS(header_checked), "function type");
_Static_assert(!HAS(Plain) && !HAS(Pointer) && !HAS(callback) && !HAS(placed), "pointer type");
_Static_assert(__builtin_types_compatible_p(Plain, Required), "compatible function types");

extern int later(void);
void before(void) { later(); }
extern int later(void) __attribute__((warn_unused_result));
int later(void) { return 3; }
_Static_assert(HAS(later), "redeclaration and definition");

void discard(int choice)
{
    checked();                                            /* warn 1 */
    (void)checked();                                      /* warn 1 */
    (long)checked();                                      /* warn 1 */
    later();                                              /* warn 1 */
    from_type();                                          /* warn 1 */
    callback();                                           /* warn 1 */
    (*placed)();                                          /* warn 1 */
    merged();                                             /* warn 1 */
    handler.callback();                                   /* warn 1 */
    header_checked();                                     /* warn 1 */
    (choice ? checked : unchecked)();                     /* warn 1 */
    ((Plain *)checked)();
    ((Required *)unchecked)();                            /* warn 1 */
    output = (checked(), checked());                      /* warn 1 */
    (void)(checked(), checked());                         /* warn 2 */
    choice ? checked() : checked();
    (void)(choice ? checked() : checked());               /* warn 2 */
    (1 ? checked() : unchecked());                        /* warn 1 */
    (1 ? unchecked() : (checked(), 1));
    choice && (checked(), choice);                        /* warn 1 */
    0 && (checked(), choice);
    choice && checked();
    if (0) checked();                                     /* warn 1 */
    (void)sizeof(checked());
    (void)_Generic(checked(), int: 0, default: checked());
    (void)__builtin_choose_expr(0, checked(), 0);
    ({ checked(); checked(); });                          /* warn 1 */
    (void)({ checked(); });                               /* reference warning varies */
    output = ({ checked(); checked(); });                 /* warn 1 */
    for (checked(); 0; checked()) {}                     /* warn 2 */
    checked() + 1;
    sink(checked());
    output = checked();
    if (checked()) {}
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
void suppressed(void) { (void)checked(); }
#pragma GCC diagnostic pop
void restored(void) { (void)checked(); }                   /* warn 1 */
""")
    (root / "status-check.h").write_text("extern int header_checked(void) __attribute__((__warn_unused_result__));\n")
    expected, reference_varies = [], set()
    for i, line in enumerate(source.read_text().splitlines(), 1):
        match = re.search(r"/\* warn (\d+) \*/", line)
        if match:
            expected.extend([i] * int(match[1]))
        if "/* reference warning varies */" in line:
            reference_varies.add(i)
    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else ["-O0", "-Wno-cpp"]), "-std=gnu11", "-isystem", root]
        command = [*prefix, "-S", source, "-o", root / "unused-result.s"]
        result = invoke(command)
        require(result, f"{cc} warn about discarded results")
        diagnostics = re.findall(r"^.+?:(\d+):\d+: warning: .*\[-Wunused-result\]$", result.stderr, re.M)
        lines = sorted(map(int, diagnostics))
        if cc == gcc:
            lines = [i for i in lines if i not in reference_varies]
        assert lines == expected, result.stderr
        result = invoke([*command, "-Werror", "-Wno-unused-result"])
        require(result, f"{cc} disable unused-result diagnostics")
        assert "[-Wunused-result]" not in result.stderr, result.stderr
        require(invoke([*command, "-Werror=unused-result"]), f"{cc} promote the warning to an error",
                success=False)
        result = invoke([*prefix, "-fsyntax-only", "-Werror", source])
        require(result, f"{cc} check syntax before code generation")
        assert "[-Wunused-result]" not in result.stderr, result.stderr
        input_flags = [] if cc == mcc else ["-x", "c"]
        for invalid in ("void invalid(void) __attribute__((warn_unused_result));",
                        "int invalid __attribute__((warn_unused_result));"):
            require(invoke([*prefix, *input_flags, "-fsyntax-only", "-Werror=attributes", "-"], invalid),
                    f"{cc} diagnose {invalid}", success=False)


def switch_fallthrough(mcc, gcc, root, target):
    source = root / "switch-fallthrough.c"
    source.write_text("""
#if !__has_attribute(__fallthrough__)
#error missing fallthrough attribute
#endif
extern void effect(int);
extern void stop(void) __attribute__((noreturn));
#define NEXT() __attribute__((__fallthrough__()))

void flow(int selector, int condition)
{
    switch (selector) {
    case 0:
        effect(0);                          /* warn */
    case 1:                                 /* note */
    case 2:
        if (condition) break;
        effect(2);
        NEXT();
    case 3:
        if (condition) stop();
        else return;
    case 4:
        goto named;
    named: case 5:
        effect(5);
        { __attribute__((fallthrough)); }
    case 6:
        effect(6);
        __attribute__((fallthrough));
    default:
        effect(7);
    }
    switch (selector) {
    case 0:
        effect(8);
    case 1:
        break;
    }
    switch (selector) {
    case 0:
        if (condition) stop();
        else effect(9);                     /* warn */
    case 1:                                 /* note */
        effect(10);
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
void hidden(int x) { switch (x) { case 0: effect(11); case 1: effect(12); } }
#pragma GCC diagnostic pop
""")
    comments = root / "fallthrough-comments.c"
    spellings = [
        ("/* about to run the next case */", 1),
        ("/* FallThrough */", 2),
        ("/* falls through - same handler */", 3),
        ("/* FALLTHROUGH */", 4),
        ("/*@fallthrough@*/", 4),
        ("/*-fallthrough*/", 4),
        ("/*lint -fallthrough  */", 4),
        ("// FALLTHRU", 4),
        ("/* FALLTHROUGH */ /* same handler */", 4),
        ("/*\n * FALLTHROUGH\n */", 2),
        ("", 0),
    ]
    comment_lines = {}
    lines = ["extern void effect(int);"]
    for i, (text, level) in enumerate(spellings):
        comment_lines[len(lines) + 1] = level
        lines.extend(f"void comment_{i}(int x) {{ switch (x) {{ case 0: effect(0); {text}".splitlines())
        lines.append("case 1: effect(1); } }")
    comments.write_text("\n".join(lines) + "\n")
    expected_warnings = [i for i, line in enumerate(source.read_text().splitlines(), 1) if "/* warn */" in line]
    expected_notes = [i for i, line in enumerate(source.read_text().splitlines(), 1) if "/* note */" in line]
    invalid = root / "fallthrough-invalid.c"
    output = root / "switch-fallthrough.s"

    def warning_lines(result):
        return [int(line) for line in re.findall(
            r"^.+?:(\d+):\d+: warning: .*\[-Wimplicit-fallthrough=?\]$", result.stderr, re.M)]

    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        command = [cc, *(flags if cc == mcc else ["-O0"]), "-std=gnu11"]
        compile_source = [*command, "-S", source, "-o", output]
        result = invoke([*compile_source, "-Wall"])
        require(result, f"{cc} keep fallthrough warning disabled by default")
        assert not warning_lines(result), result.stderr
        for option in ("-Wextra", "-Wimplicit-fallthrough", "-Wimplicit-fallthrough=03",
                       "-Wimplicit-fallthrough=5"):
            result = invoke([*compile_source, option])
            require(result, f"{cc} track switch flow with {option}")
            assert warning_lines(result) == expected_warnings, result.stderr
            notes = [int(line) for line in re.findall(r"^.+?:(\d+):\d+: note: here$", result.stderr, re.M)]
            assert notes == expected_notes, result.stderr
        require(invoke([*compile_source, "-Werror=implicit-fallthrough"]), f"{cc} promote fallthrough warning",
                success=False)
        require(invoke([*compile_source, "-Wextra", "-Wno-implicit-fallthrough", "-Werror"]),
                f"{cc} disable fallthrough warning")
        require(invoke([*command, "-std=c11", "-pedantic-errors", "-fsyntax-only",
                        "-Werror=implicit-fallthrough", source]), f"{cc} accept GNU spelling in C11")
        for level in range(6):
            result = invoke([*command, "-S", comments, "-o", output, f"-Wimplicit-fallthrough={level}"])
            require(result, f"{cc} classify fallthrough comments at level {level}")
            assert warning_lines(result) == [line for line, threshold in comment_lines.items()
                                             if level and threshold < level], result.stderr
        invalid.write_text("void outside(void) { __attribute__((fallthrough)); }\n")
        require(invoke([*command, "-S", invalid, "-o", output]), f"{cc} reject fallthrough outside a switch",
                success=False)
        invalid.write_text("void last(int x) { switch (x) { case 0: __attribute__((fallthrough)); } }\n")
        result = invoke([*command, "-Werror", "-S", invalid, "-o", output])
        require(result, f"{cc} diagnose a fallthrough annotation with no next case", success=False)
        assert "not preceding a case label" in result.stderr, result.stderr


def call_diagnostic_attributes(mcc, gcc, root, target, runner):
    source = root / "call-diagnostics.c"
    source.write_text(r"""
#if !__has_attribute(__error__) || !__has_attribute(warning)
#error missing call diagnostic attributes
#endif
#include <stdio.h>
#include <call-diagnostic.h>

extern int impossible(void) __attribute__((__error__("eliminated call")));
extern void undefined_argument(void);
extern int impossible_with_arg(void (*)(void)) __attribute__((error("eliminated call and argument")));
extern int notice(int) __attribute__((warning("initial message")));
extern int notice(int) __attribute__((__warning__(("latest " "message"))));
extern int declared_late(int);
int before_declaration(int x) { return declared_late(x); }  /* declared later */
extern int declared_late(int) __attribute__((warning(u"declared later")));

_Static_assert(__builtin_has_attribute(notice, warning), "function attribute");
_Static_assert(__builtin_has_attribute(notice, warning("initial message")), "older declaration");
_Static_assert(__builtin_has_attribute(notice, warning("latest message")), "latest declaration");
_Static_assert(!__builtin_has_attribute(notice, warning("unrelated")), "different message");
_Static_assert(!__builtin_has_attribute(__typeof__(notice), warning), "not a type attribute");
_Static_assert(__builtin_has_attribute(impossible, error("eliminated call")), "error attribute");
_Static_assert(__builtin_has_attribute(from_header, warning("header message")), "header attribute");

int notice(int x) { return x + 1; }
int declared_late(int x) { return x + 2; }
int from_header(int x) { return x + 3; }
int (*address_only(void))(int) { return notice; }
int indirect(int x, int (*callback)(int)) { return callback(x); }

int dead(int x)
{
    if (0) impossible();
    if (0) impossible_with_arg(undefined_argument);
    while (0) impossible();
    (void)(x && (0 && impossible()));
    (void)sizeof(impossible());
    switch (2) {
    case 1: impossible(); break;
    case 2: break;
    }
    if (x) goto done;
    goto done;
    impossible();
done:
    return 1 ? x : impossible();
}

int after_trap(void) { __builtin_trap(); return impossible(); }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-warning"
int quiet(int x) { return notice(x); }
#pragma GCC diagnostic pop

int main(void)
{
    int direct = (&notice)(3);                             /* latest message */
    int selected = (1 ? notice : declared_late)(4);         /* latest message */
    int header = from_header(5);                            /* header message */
    return !(direct == 4 && selected == 5 && header == 8 && dead(7) == 7 &&
             before_declaration(7) == 9 && quiet(8) == 9 && indirect(9, address_only()) == 10);
}
""")
    (root / "call-diagnostic.h").write_text("""
#define MESSAGE(text) __attribute__((__warning__(text)))
extern int from_header(int) MESSAGE("header message");
#undef MESSAGE
""")
    invalid_call = root / "diagnose-call.c"
    invalid_call.write_text("""
extern void disabled(void) __attribute__((warning("warn on live call"), error("reject live call")));
void invoke(int enabled)
{
    if (enabled) disabled();
}
""")
    misplaced = root / "misplaced-call-diagnostic.c"
    misplaced.write_text("""
typedef void Fn(void) __attribute__((warning("typedef")));
extern void (*pointer)(void) __attribute__((error("object")));
struct Members { void (*callback)(void) __attribute__((warning("member"))); };
extern void invalid(void) __attribute__((warning(42)));
""")
    expected = {"declared later": 1, "latest message": 2, "header message": 1}
    flags = [f"--target={target}"] if target else []
    output = root / "call-diagnostics.s"
    executable = root / "call-diagnostics"
    arity = root / "call-diagnostic-arity.c"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else ["-O0", "-Wno-cpp"]), "-std=gnu11", "-isystem", root]
        compile_source = [*prefix, "-S", source, "-o", output]
        result = invoke(compile_source)
        require(result, f"{cc} diagnose reachable annotated calls")
        messages = re.findall(r"warning: call to .*?: (.*) \[-Wattribute-warning\]$", result.stderr, re.M)
        assert len(messages) == sum(expected.values()), result.stderr
        assert all(messages.count(message) == count for message, count in expected.items()), result.stderr
        require(invoke([*compile_source, "-Werror=attribute-warning"]),
                f"{cc} promote attribute-warning", success=False)
        require(invoke([*prefix, "-std=c11", "-pedantic-errors", "-Werror=attribute-warning",
                        "-fsyntax-only", source]), f"{cc} check attributed declarations in C11")
        require(invoke([*prefix, "-Werror", "-Wno-attribute-warning", source, "-o", executable]),
                f"{cc} link after eliminating unreachable calls")
        require(invoke([*(runner if cc == mcc else []), executable]), f"{cc} retain live call behavior")
        require(invoke([*prefix, "-fsyntax-only", invalid_call]), f"{cc} defer call error until code generation")
        result = invoke([*prefix, "-S", invalid_call, "-o", output])
        require(result, f"{cc} reject reachable calls to an error declaration", success=False)
        assert "reject live call" in result.stderr and "warn on live call [-Wattribute-warning]" in result.stderr
        result = invoke([*prefix, "-Wno-attribute-warning", "-S", invalid_call, "-o", output])
        require(result, f"{cc} keep error diagnosis when warnings are disabled", success=False)
        assert "reject live call" in result.stderr and "warn on live call" not in result.stderr
        result = invoke([*prefix, "-fsyntax-only", misplaced])
        require(result, f"{cc} diagnose attributes on an invalid subject or argument")
        assert result.stderr.count("[-Wattributes]") == 4, result.stderr
        for attribute in ("warning", "error()", 'warning("one", "two")'):
            arity.write_text(f"extern void bad(void) __attribute__(({attribute}));\n")
            require(invoke([*prefix, "-fsyntax-only", arity]), f"{cc} require one message", success=False)

    rounding = root / "call-diagnostic-rounding.c"
    rounding.write_text("""
#define __STDC_WANT_IEC_60559_DFP_EXT__ 1
#include <fenv.h>
extern int marked(void) __attribute__((warning("rounding-dependent call")));
int marked(void) { return 41; }
static int probe(void)
{
    if (1e-398DD / 2.0DD)
        return marked();
    return 0;
}
int main(void)
{
    if (fe_dec_setround(FE_DEC_TONEAREST) || probe() != 0) return 1;
    if (fe_dec_setround(FE_DEC_UPWARD) || probe() != 41) return 2;
    return 0;
}
""")
    result = invoke([mcc, *flags, "-std=gnu11", rounding, "-lm", "-o", executable])
    require(result, "diagnose a call whose reachability depends on runtime decimal rounding")
    assert "rounding-dependent call [-Wattribute-warning]" in result.stderr, result.stderr
    require(invoke([*runner, executable]), "preserve calls reachable under a dynamic rounding mode")


def sentinel_attributes(mcc, gcc, root, target, runner):
    calls = root / "sentinel-calls.c"
    calls.write_text("""
extern void last(int kind, ...) __attribute__((__sentinel__));
extern void earlier(int kind, ...) __attribute__((sentinel(1)));
extern void earlier(int kind, ...) __attribute__((sentinel(0)));
extern void earlier(int kind, ...);
extern void listed(int kind, ...) __attribute__((sentinel(0), sentinel(1)));
typedef void Tail(int kind, ...) __attribute__((sentinel()));
typedef void Plain(int kind, ...);
extern Tail *typed;
extern void (* __attribute__((sentinel)) placed)(int, ...);
extern void (*abbreviated)(int, ...) __attribute__((sentinel(1)));
struct Dispatch { void (*callback)(int, ...) __attribute__((sentinel)); };
extern struct Dispatch destination;

void check(int condition, void *pointer, Plain *unmarked)
{
    last(0, (void *)0);
    last(0, (const char *)0);
    last(0, (void (*)(void))0);
    last(0, (const char *)(1 - 1));
    last(0, condition ? (void *)0 : (void *)0);
    last(0, 0);                                /* missing */
    last(0, 0UL);                              /* missing */
    last(0, pointer);                          /* missing */
    last(0);                                   /* short */
    last(0, (char *)1);                        /* missing */
    earlier(0, (void *)0, 4);
    earlier(0, 4, (void *)0);                   /* missing */
    earlier(0, (void *)0);                      /* short */
    listed(0, (void *)0, 4);
    listed(0, 4, (void *)0);                    /* missing */
    (*typed)(0, 2);                             /* missing */
    placed(0, 2);                               /* missing */
    abbreviated(0, (void *)0);                  /* short */
    destination.callback(0, 2);                 /* missing */
    ((Plain *)typed)(0, 2);
    ((Tail *)unmarked)(0, 2);                    /* missing */
    (condition ? unmarked : typed)(0, 2);       /* missing */
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
void suppressed(void) { last(0, 0); }
#pragma GCC diagnostic pop
""")
    queries = root / "sentinel-queries.c"
    queries.write_text("""
#if !__has_attribute(__sentinel__) || !__has_builtin(__builtin_execle)
#error missing sentinel attribute or built-in
#endif
extern void declared(int, ...) __attribute__((sentinel));
extern void repeated(int, ...) __attribute__((sentinel(1)));
extern void repeated(int, ...) __attribute__((sentinel(0)));
typedef void Function(int, ...) __attribute__((sentinel(1)));
extern Function *pointer;
struct Dispatch { void (*callback)(int, ...) __attribute__((sentinel)); };
extern struct Dispatch destination;
extern int execl(const char *, const char *, ...);
extern int execle(const char *, const char *, ...);
#define HAS(target, attr) __builtin_has_attribute(target, attr)
_Static_assert(HAS(declared, sentinel) && !HAS(declared, sentinel(0)), "default spelling");
_Static_assert(HAS(repeated, sentinel(0)) && HAS(repeated, sentinel(1)), "redeclarations");
_Static_assert(HAS(Function, sentinel(1)) && !HAS(Function, sentinel(0)), "position");
_Static_assert(!HAS(pointer, sentinel) && HAS(*pointer, sentinel(1)), "function type");
_Static_assert(HAS(*destination.callback, sentinel), "record member");
_Static_assert(HAS(__builtin_execl, sentinel) && !HAS(__builtin_execl, sentinel(0)), "builtin default");
_Static_assert(HAS(__builtin_execle, sentinel(1)), "builtin position");
#ifdef __STRICT_ANSI__
_Static_assert(!HAS(execl, sentinel) && !HAS(execle, sentinel), "strict library declarations");
#else
_Static_assert(HAS(execl, sentinel) && HAS(execle, sentinel(1)), "hosted GNU library declarations");
#endif
""")
    builtins = root / "sentinel-builtins.c"
    builtins.write_text("""
extern int execl(const char *, const char *, ...);
extern int execlp(const char *, const char *, ...);
extern int execle(const char *, const char *, ...);
extern __typeof__(execl) *callback;
void calls(char *const *environment)
{
    execl("path", "arg", 0);                               /* execl */
    (&execl)("path", "arg", 0);                            /* execl */
    callback("path", "arg", 0);                            /* execl */
    execlp("path", "arg", 0);                              /* library */
    execle("path", "arg", 0, environment);                 /* library */
    __builtin_execl("path", "arg", 0);                     /* builtin */
    __builtin_execlp("path", "arg", 0);                    /* builtin */
    __builtin_execle("path", "arg", 0, environment);       /* builtin */
    __builtin_execle("path", "arg", (char *)0, environment);
}
""")
    invalid = root / "sentinel-invalid.c"
    invalid.write_text("""
extern void old_style() __attribute__((sentinel));
extern void nonvariadic(int) __attribute__((sentinel));
extern int object __attribute__((sentinel));
extern void negative(int, ...) __attribute__((sentinel(-1)));
extern void noninteger(int, ...) __attribute__((sentinel(0.5)));
""")
    runtime = root / "sentinel-runtime.c"
    runtime.write_text("""
#include <stdarg.h>
static int sum(int first, ...) __attribute__((sentinel));
static int sum(int first, ...)
{
    va_list values;
    va_start(values, first);
    const int *value;
    while ((value = va_arg(values, const int *)))
        first += *value;
    va_end(values);
    return first;
}
int main(void)
{
    const int left = 3, right = 5;
    char *const environment[] = {0};
    return sum(2, &left, &right, (const int *)0) != 10 ||
           __builtin_execl("", "", (char *)0) != -1 ||
           __builtin_execle("", "", (char *)0, environment) != -1;
}
""")
    (root / "sentinel-header.h").write_text("""
extern void header_last(int, ...) __attribute__((__sentinel__));
extern void header_earlier(int, ...) __attribute__((__sentinel__(1)));
""")
    header_calls = root / "sentinel-header-calls.c"
    header_calls.write_text("""
#include <stdio.h>
#include <sentinel-header.h>
void use_header(void)
{
    header_last(0, 3);
    header_earlier(0, (void *)0);
}
""")
    expected = {}
    for i, line in enumerate(calls.read_text().splitlines(), 1):
        if "/* missing */" in line:
            expected[i] = "missing"
        elif "/* short */" in line:
            expected[i] = "not enough"
    builtin_lines = {"execl": [], "library": [], "builtin": []}
    for i, line in enumerate(builtins.read_text().splitlines(), 1):
        match = re.search(r"/\* (execl|library|builtin) \*/", line)
        if match:
            builtin_lines[match[1]].append(i)

    def diagnostics(result):
        return {int(line): message for line, message in re.findall(
            r"^.+?:(\d+):\d+: warning: (missing|not enough).*\[-Wformat=?\]$", result.stderr, re.M)}

    flags = [f"--target={target}"] if target else []
    binary = root / "sentinel-runtime"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else []), "-std=gnu11"]
        syntax = [*prefix, "-fsyntax-only"]
        for option in ("-Wformat", "-Wformat=2", "-Wall"):
            result = invoke([*syntax, option, calls])
            require(result, f"{cc} check sentinel arguments with {option}")
            assert diagnostics(result) == expected, result.stderr
        if cc == mcc:
            require(invoke([*syntax, "-Werror", calls]), "keep format checking disabled by default")
        for option in ("-Wno-format", "-Wformat=0"):
            flags_without_format = [option] if cc == mcc else [option, "-Wno-format-security"]
            require(invoke([*syntax, "-Wall", *flags_without_format, "-Werror", calls]),
                    f"{cc} disable format checks")
        require(invoke([*syntax, "-Werror=format", calls]), f"{cc} promote format warnings", success=False)
        for options, scopes in [
            ([], ("execl", "library", "builtin")),
            (["-fno-builtin-execl"], ("library", "builtin")),
            (["-fno-builtin"], ("builtin",)),
            (["-ffreestanding"], ("builtin",)),
            (["-std=c11"], ("builtin",)),
        ]:
            result = invoke([*syntax, *options, "-Wall", builtins])
            require(result, f"{cc} check builtin sentinel options {options}")
            assert diagnostics(result) == {line: "missing" for scope in scopes for line in builtin_lines[scope]}, (
                result.stderr
            )
        for dialect in ("gnu11", "c11"):
            require(invoke([*syntax, f"-std={dialect}", "-pedantic-errors", queries]),
                    f"{cc} query sentinel metadata in {dialect}")
        result = invoke([*syntax, invalid])
        require(result, f"{cc} diagnose inapplicable sentinel attributes")
        assert result.stderr.count("[-Wattributes]") == 5, result.stderr
        result = invoke([*syntax, "-Wformat", "-isystem", root, header_calls])
        require(result, f"{cc} retain sentinel attributes from system headers")
        assert list(diagnostics(result).values()) == ["missing", "not enough"], result.stderr
        require(invoke([*prefix, "-Werror=format", runtime, "-o", binary]),
                f"{cc} compile correct variadic calls with sentinel checking")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run variadic and builtin calls")


def nullability_attributes(mcc, gcc, root, target, runner):
    calls = root / "nonnull-calls.c"
    calls.write_text("""
extern void all(void *pointer, int kind, ...) __attribute__((__nonnull__));
extern void selected(void *, int, void *) __attribute__((nonnull(1)));
extern void selected(void *, int, void *) __attribute__((nonnull(3)));
extern void invalid(int, void *, void *) __attribute__((nonnull(1,2), nonnull(3)));
typedef void Callback(void *, void *) __attribute__((nonnull(2)));
extern Callback *callback;
extern void (* __attribute__((nonnull(1))) placed)(void *, void *);
struct Dispatch { void (*call)(void *, void *) __attribute__((nonnull(2))); };
extern struct Dispatch dispatch;
extern void explicit_position() __attribute__((nonnull(1)));

void test(void *p)
{
    all(0, 7, 0, (void *)0);                 /* null 1 4 */
    all(p, 7, 0);
    selected((char *)0, 0, (void *)0);       /* null 1 3 */
    invalid(0, 0, 0);                       /* null 3 */
    callback(0, 0);                         /* null 2 */
    placed(0, 0);                           /* null 1 */
    dispatch.call(0, 0);                    /* null 2 */
    explicit_position((void *)0);           /* null 1 */
    explicit_position(0);
    (void)__builtin___strcpy_chk(p, 0, (unsigned long)-1);  /* null 2 */
    (void)__builtin___snprintf_chk(0, 0, 0, 0, "");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull"
void suppressed(void) { selected(0, 0, 0); }
#pragma GCC diagnostic pop
""")
    comparisons = root / "nonnull-compare.c"
    comparisons.write_text("""
extern void effect(void);
extern void escape(void **);
#define NONNULL __attribute__((nonnull(1)))
NONNULL int dynamic(void *p, void *other, int choice)
{
    if (!p) effect();                        /* compare */
    if ((char *)p == (char *)0) effect();    /* compare */
    if (p) effect();                         /* compare */
    if (p == other) effect();
    if (choice) {
        p = other;
        return !p;
    }
    return (_Bool)p;                         /* compare */
}
NONNULL int join(void *p, void *other, int choice)
{
    if (choice) p = other;
    return p != 0;
}
NONNULL int escaped(void *p)
{
    escape(&p);
    return !p;
}
NONNULL int unreachable(void *p)
{
    if (0) return !p;
    return 0;
}
int late(void *p) { return !p; }             /* compare */
extern int late(void *p) NONNULL;
""")
    queries = root / "nullability-queries.c"
    queries.write_text("""
#if !__has_attribute(__nonnull__) || !__has_attribute(__returns_nonnull__)
#error missing pointer attributes
#endif
extern void all(void *, void *) __attribute__((nonnull));
extern void some(void *, void *) __attribute__((nonnull(2)));
typedef void Typed(void *, void *) __attribute__((nonnull(1)));
extern Typed *typed;
typedef void *Factory(void) __attribute__((returns_nonnull));
extern Factory *factory;
#define HAS(item, attr) __builtin_has_attribute(item, attr)
_Static_assert(HAS(all, nonnull) && HAS(all, nonnull(1,2)), "all pointer parameters");
_Static_assert(!HAS(some, nonnull) && !HAS(some, nonnull(1)) && HAS(some, nonnull(1,2)), "positions");
_Static_assert(HAS(Typed, nonnull(1)) && !HAS(typed, nonnull(1)) && HAS(*typed, nonnull(1)), "type");
_Static_assert(HAS(Factory, returns_nonnull) && HAS(*factory, __returns_nonnull__), "result type");
_Static_assert(HAS(__builtin___strcpy_chk, nonnull(1,2)), "checked string copy");
""")
    runtime = root / "returns-nonnull.c"
    runtime.write_text("""
extern void *provide(void) __attribute__((returns_nonnull));
extern void unreachable(void) __attribute__((error("explicit null check remains")));
int effects;
int main(void)
{
    if (provide() == 0) unreachable();
    if (!provide()) unreachable();
    if (!(_Bool)provide()) unreachable();
    return effects != 3;
}
""")
    provider = root / "returns-nonnull-provider.c"
    provider.write_text("""
extern int effects;
static int value;
void *provide(void) { ++effects; return &value; }
""")
    invalid = root / "nullability-invalid.c"
    expected_args = [(i, int(arg)) for i, line in enumerate(calls.read_text().splitlines(), 1)
                     for match in re.findall(r"/\* null ([0-9 ]+) \*/", line) for arg in match.split()]
    expected_comparisons = [i for i, line in enumerate(comparisons.read_text().splitlines(), 1)
                            if "/* compare */" in line]
    flags = [f"--target={target}"] if target else []
    output = root / "nullability.s"
    binary = root / "returns-nonnull"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else ["-O0"]), "-std=gnu11"]
        for warning in ("-Wnonnull", "-Wformat", "-Wall"):
            result = invoke([*prefix, warning, "-fsyntax-only", calls])
            require(result, f"{cc} check nonnull inputs during parsing")
            found = [(int(line), int(arg)) for line, arg in re.findall(
                r"^.+?:(\d+):\d+: warning: argument (\d+) null .*\[-Wnonnull\]$", result.stderr, re.M)]
            assert found == expected_args, result.stderr
            assert result.stderr.count("[-Wattributes]") == 1, result.stderr
        result = invoke([*prefix, "-Wall", "-Wno-nonnull", "-fsyntax-only", calls])
        require(result, f"{cc} disable null argument warnings")
        assert "[-Wnonnull]" not in result.stderr, result.stderr
        without_format = ["-Wno-format"] if cc == mcc else ["-Wno-format", "-Wno-format-security"]
        result = invoke([*prefix, "-Wall", *without_format, "-fsyntax-only", calls])
        require(result, f"{cc} disable implied nonnull warnings")
        assert "[-Wnonnull]" not in result.stderr, result.stderr
        result = invoke([*prefix, "-Werror=format", "-fsyntax-only", calls])
        require(result, f"{cc} promote implied nonnull warnings", success=False)
        assert "[-Werror=nonnull]" in result.stderr, result.stderr
        result = invoke([*prefix, "-Wnonnull-compare", "-fsyntax-only", comparisons])
        require(result, f"{cc} postpone nonnull parameter comparisons")
        assert "[-Wnonnull-compare]" not in result.stderr, result.stderr
        result = invoke([*prefix, "-Wall", "-S", comparisons, "-o", output])
        require(result, f"{cc} diagnose comparisons of original nonnull arguments")
        found = re.findall(r"^.+?:(\d+):\d+: warning: .*\[-Wnonnull-compare\]$", result.stderr, re.M)
        assert sorted(map(int, found)) == expected_comparisons, result.stderr
        require(invoke([*prefix, "-Wall", "-Wno-nonnull-compare", "-Werror",
                        "-S", comparisons, "-o", output]), f"{cc} disable nonnull comparison warnings")
        require(invoke([*prefix, "-std=c11", "-pedantic-errors", "-fsyntax-only", queries]),
                f"{cc} query pointer attributes in C11")
        for text in ("extern void old_style() __attribute__((nonnull));",
                     "extern int result(void) __attribute__((returns_nonnull));"):
            invalid.write_text(text + "\n")
            require(invoke([*prefix, "-fsyntax-only", invalid]), f"{cc} reject invalid pointer contracts",
                    success=False)
        require(invoke([*prefix, runtime, provider, "-o", binary]), f"{cc} simplify nonnull result comparisons")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} preserve call side effects")
        result = invoke([*prefix, "-fno-delete-null-pointer-checks", "-S", runtime, "-o", output])
        require(result, f"{cc} retain explicit null checks when requested", success=False)
        assert "explicit null check remains" in result.stderr, result.stderr

    rounding = root / "nonnull-rounding.c"
    rounding.write_text("""
#include <fenv.h>
static void *nonzero_address(void) __attribute__((returns_nonnull));
static void *nonzero_address(void) { return (void *)1; }
static int agrees(void)
{
    return nonzero_address() == (void *)(long)(0x1.fffffffffffffp-1 + 0x1p-55);
}
int main(void)
{
    if (fesetround(FE_TONEAREST) || agrees()) return 1;
    if (fesetround(FE_UPWARD) || !agrees()) return 2;
    return 0;
}
""")
    require(invoke([mcc, *flags, "-std=gnu11", rounding, "-lm", "-o", binary]),
            "compile a nonnull result comparison with runtime pointer conversion")
    require(invoke([*runner, binary]), "preserve result comparisons sensitive to runtime rounding")


def noplt_calls(mcc, gcc, root, target, runner):
    source = root / "noplt-calls.c"
    source.write_text("""
#if !__has_attribute(__noplt__)
#error missing noplt attribute
#endif
extern int decorated(int) __attribute__((__noplt__()));
extern int decorated(int);
extern int ordinary(int);
extern int renamed(int) __asm__("renamed_target") __attribute__((noplt));
extern int hidden(int) __attribute__((noplt, visibility("hidden")));
extern int protected(int) __attribute__((noplt, visibility("protected")));
extern int ( __attribute__((noplt)) grouped)(int);
extern int (* __attribute__((noplt)) result_pointer(int))(int);
extern int late(int);
int before(int x) { return late(x); }
extern int late(int) __attribute__((noplt));
int same_unit(int) __attribute__((noplt));
int same_unit(int x) { return x + 1; }
static int local(int) __attribute__((noplt));
static int local(int x) { return x + 2; }
extern _Thread_local int thread_value;
int read_thread_value(void) { return thread_value; }

typedef int Plain(int);
#define HAS(item) __builtin_has_attribute(item, noplt)
_Static_assert(HAS(decorated) && HAS(grouped) && HAS(result_pointer) && HAS(same_unit), "named functions");
_Static_assert(!HAS(ordinary) && !HAS(Plain) && !HAS(&decorated), "unannotated subjects");

int use(int x)
{
    return decorated(x) + ordinary(x) + renamed(x) + hidden(x) + protected(x)
        + grouped(x) + result_pointer(x)(x) + same_unit(x) + local(x);
}
""")
    invalid = root / "noplt-invalid.c"
    invalid.write_text("""
extern int (*pointer)(int) __attribute__((noplt));
typedef int Function(int) __attribute__((noplt));
struct Host { int (*member)(int) __attribute__((noplt)); };
void takes(int (* __attribute__((noplt)) param)(int));
""")
    argument = root / "noplt-argument.c"
    argument.write_text("extern int invalid(int) __attribute__((noplt(1)));\n")
    provider = root / "noplt-provider.c"
    provider.write_text("""
#include <stdarg.h>
_Thread_local int thread_value = 5;
int decorated(int x) { return x + 3; }
int ordinary(int x) { return x + 5; }
int variadic_call(int x, ...) {
    va_list values;
    va_start(values, x);
    double value = va_arg(values, double);
    va_end(values);
    return x + (int)value;
}
""")
    client = root / "noplt-client.c"
    client.write_text("""
int decorated(int) __attribute__((noplt));
int ordinary(int);
int variadic_call(int, ...) __attribute__((noplt));
int same_unit(int) __attribute__((noplt));
int same_unit(int x) { return x + 99; }
static int optional(int) __attribute__((weakref("noplt_unavailable"), noplt));
extern _Thread_local int thread_value;

int run_client(int x) {
    return decorated(x) + ordinary(x) + variadic_call(x, 2.0) + same_unit(x) + thread_value
           + (optional ? optional(x) : 11);
}
""")
    main = root / "noplt-main.c"
    main.write_text("""
int run_client(int);
int same_unit(int x) { return x + 7; }
int main(void) { return run_client(4) != 49; }
""")
    flags = [f"--target={target}"] if target else []
    decorated = ("decorated", "renamed_target", "grouped", "result_pointer", "late", "same_unit")
    object_file = root / "noplt.o"

    def relocations(path):
        result = invoke(["readelf", "-Wr", path])
        require(result, f"inspect relocations in {path}")
        relocations_by_symbol = {}
        for kind, name in re.findall(r"\b(R_\w+)\s+[a-fA-F0-9]+\s+(\S+)", result.stdout):
            relocations_by_symbol.setdefault(name, []).append(kind)
        return relocations_by_symbol

    def got(kinds):
        return any("GOTPCREL" in kind or "_ADR_GOT_PAGE" in kind for kind in kinds)

    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else ["-O0"]), "-std=gnu11", "-fPIC"]
        for options, ordinary_got in [((), False), (("-fno-plt",), True), (("-fno-plt", "-fplt"), False)]:
            require(invoke([*prefix, *options, "-Werror=attributes", "-c", source, "-o", object_file]),
                    f"{cc} compile PLT relocation variants")
            relocs = relocations(object_file)
            for symbol in decorated:
                assert got(relocs[symbol]), (symbol, relocs)
            assert got(relocs["ordinary"]) == ordinary_got, relocs
            if not ordinary_got:
                assert any(kind.endswith(("PLT32", "CALL26")) for kind in relocs["ordinary"]), relocs
            for symbol in ("hidden", "protected"):
                assert relocs[symbol] and not got(relocs[symbol]), relocs
            if any(kind.startswith("R_X86_64_") for kind in relocs["ordinary"]):
                assert got(relocs["__tls_get_addr"]) == ordinary_got, relocs
        result = invoke([*prefix, "-std=c11", "-pedantic-errors", "-fsyntax-only", invalid])
        require(result, f"{cc} warn when noplt is applied outside a function declaration")
        assert result.stderr.count("[-Wattributes]") == 4, result.stderr
        require(invoke([*prefix, "-fsyntax-only", argument]), f"{cc} reject arguments to noplt", success=False)

        provider_so = root / "libnoplt-provider.so"
        client_so = root / "libnoplt-client.so"
        executable = root / "noplt-runtime"
        paths = [f"-L{root}", f"-Wl,-rpath,{root}"]
        require(invoke([*prefix, "-shared", provider, "-Wl,-soname,libnoplt-provider.so", "-o", provider_so]),
                f"{cc} link a provider for external calls")
        for options in ((), ("-fno-plt",)):
            require(invoke([*prefix, *options, "-shared", client, *paths, "-lnoplt-provider", "-o", client_so]),
                    f"{cc} link a shared caller")
            dynamic = relocations(client_so)
            for name in ("decorated", "variadic_call", "same_unit", "noplt_unavailable"):
                assert any(kind.endswith("GLOB_DAT") for kind in dynamic[name]), dynamic
            if options:
                assert any(kind.endswith("GLOB_DAT") for kind in dynamic["ordinary"]), dynamic
            require(invoke([*prefix, *options, main, *paths, "-lnoplt-client", "-lnoplt-provider", "-o", executable]),
                    f"{cc} link an executable overriding a shared definition")
            require(invoke([*(runner if cc == mcc else []), executable]),
                    f"{cc} preserve interposition, weak symbols, and variadic calls")


def patchable_entries(mcc, gcc, root, target, runner):
    source = root / "patchable-calls.c"
    source.write_text("""
#include <stdint.h>

#if !__has_attribute(__patchable_function_entry__)
#error missing patchable function entry attribute
#endif
#ifdef __aarch64__
typedef uint32_t instruction;
#define NOP UINT32_C(0xd503201f)
#else
typedef uint8_t instruction;
#define NOP UINT8_C(0x90)
#endif

__attribute__((section(".isolated.text"), patchable_function_entry(3)))
static int isolated(int x) { return x + 1; }
static int explicit(int x) __attribute__((patchable_function_entry(6, 3)));
static int explicit(int x) __attribute__((patchable_function_entry(2 + 2, 2)));
static int explicit(int x) __attribute__((patchable_function_entry(6, 3)));
static int explicit(int x) { return x + 2; }
static int ordinary(int x) { return x + 3; }
static int disabled(int x) __attribute__((patchable_function_entry(0)));
static int disabled(int x) { return x + 4; }
typedef int Callable(int) __attribute__((patchable_function_entry(1, 1)));
static Callable inherited;
static int inherited(int x) { return x + 5; }
struct Record { int field __attribute__((patchable_function_entry(3))); };
extern int variable __attribute__((patchable_function_entry(3)));

_Static_assert(__builtin_has_attribute(explicit, patchable_function_entry), "explicit attribute");
_Static_assert(__builtin_has_attribute(explicit, patchable_function_entry(4, 1)), "match the size");
_Static_assert(__builtin_has_attribute(explicit, patchable_function_entry(6)), "previous declaration");
_Static_assert(!__builtin_has_attribute(explicit, patchable_function_entry(5)), "different size");
_Static_assert(!__builtin_has_attribute(ordinary, patchable_function_entry), "option alone");
_Static_assert(__builtin_has_attribute(disabled, patchable_function_entry(0)), "zero override");
_Static_assert(__builtin_has_attribute(inherited, patchable_function_entry(1)), "inherited attribute");
_Static_assert(__builtin_has_attribute(variable, patchable_function_entry), "object attribute");
_Static_assert(__builtin_has_attribute(((struct Record){0}).field, patchable_function_entry),
               "member attribute");

extern const uintptr_t __start___patchable_function_entries[];
extern const uintptr_t __stop___patchable_function_entries[];

__attribute__((patchable_function_entry(0))) int main(void) {
    const struct { int (*function)(int); unsigned count, before; } functions[] = {
        {isolated, 3, 0}, {explicit, 4, 2}, {ordinary, 5, 1}, {inherited, 1, 1}
    };
    const uintptr_t *start = __start___patchable_function_entries;
    if ((uintptr_t)__stop___patchable_function_entries - (uintptr_t)start != sizeof(*start) * 4)
        return 1;
    for (unsigned i = 1; i < 4; i++)
        if (start[i] <= start[i - 1])
            return 2;
    for (unsigned i = 0; i < 4; i++) {
        uintptr_t address = (uintptr_t)functions[i].function - functions[i].before * sizeof(instruction);
        unsigned found = 0;
        for (unsigned j = 0; j < 4; j++)
            found += start[j] == address;
        if (found != 1)
            return 3;
        const volatile instruction *area = (const volatile instruction *)address;
        for (unsigned j = 0; j < functions[i].count; j++)
            if (area[j] != NOP)
                return 4;
        if (area[functions[i].count] == NOP || functions[i].function(7) != 8 + (int)i + (i == 3))
            return 5;
    }
    return disabled(7) != 11;
}
""")
    disabled = root / "patchable-disabled.c"
    disabled.write_text("int ordinary(void) { return 0; }\n")
    malformed = root / "patchable-invalid.c"
    malformed.write_text("int broken(void) __attribute__((patchable_function_entry()));\n")
    flags = [f"--target={target}"] if target else []
    objects = {}

    def records(path):
        result = invoke(["readelf", "-SW", path])
        require(result, f"inspect the patchable entry sections in {path}")
        names = dict(re.findall(r"^\s*\[\s*(\d+)\]\s+(\S+)\s", result.stdout, re.M))
        return [(int(size, 16), names[link]) for size, link in re.findall(
            r"__patchable_function_entries\s+PROGBITS\s+\S+\s+\S+\s+(\S+)\s+00\s+WAL\s+(\d+)\s+0\s+8\s*$",
            result.stdout, re.M
        )]

    for label, cc, options, command in (("mcc", mcc, flags, runner), ("gcc", gcc, ["-O0"], [])):
        prefix = [cc, *options, "-std=gnu11"]
        obj = objects[label] = root / f"patchable-{label}.o"
        executable = root / f"patchable-{label}"
        require(invoke([*prefix, "-Werror=attributes", "-fpatchable-function-entry=5,1", "-c", source,
                        "-o", obj]), f"{cc} compile patchable function entries")
        assert sorted(records(obj)) == [(8, ".isolated.text")] + [(8, ".text")] * 3, records(obj)
        require(invoke([*prefix, obj, "-o", executable]), f"{cc} link patchable function entries")
        linked_records = records(executable)
        assert linked_records == [(32, ".text")] or (label == "gcc" and linked_records == [(32, ".isolated.text")]), (
            linked_records
        )
        require(invoke([*command, executable]), f"{cc} inspect NOPs at the recorded addresses")
        require(invoke([*prefix, "-fpatchable-function-entry=5", "-fpatchable-function-entry=0", "-c",
                        disabled, "-o", root / "patchable-disabled.o"]), f"{cc} cancel entry padding")
        result = invoke(["readelf", "-SW", root / "patchable-disabled.o"])
        require(result, "inspect disabled entry padding")
        assert "__patchable_function_entries" not in result.stdout, result.stdout
        for invalid in ("", "5,", "-1", "65536", "3,4"):
            require(invoke([*prefix, f"-fpatchable-function-entry={invalid}", "-S", disabled, "-o", "/dev/null"]),
                    f"{cc} reject invalid patchable entry option {invalid!r}", success=False)
        require(invoke([*prefix, "-fsyntax-only", malformed]), f"{cc} require a patch size", success=False)

    if not target:
        executable = root / "patchable-mixed"
        for linker, obj in ((mcc, objects["gcc"]), (gcc, objects["mcc"])):
            require(invoke([linker, obj, "-o", executable]), f"{linker} link patch records from {obj.name}")
            require(invoke([executable]), f"run patchable entries from {obj.name} linked by {linker}")

        discarded = root / "patchable-unused.c"
        # Distinct code sections let GNU ld discard the unused entry with its patch record.
        discarded.write_text("""
__attribute__((patchable_function_entry(3), section(".unused.text"))) int unused(void) { return 0; }
__attribute__((patchable_function_entry(2))) int used(void) { return 7; }
int main(void) { return used() != 7; }
""")
        require(invoke([mcc, "-std=gnu11", "-c", discarded, "-o", objects["mcc"]]),
                "assemble patch metadata for section collection")
        require(invoke([gcc, objects["mcc"], "-Wl,--gc-sections", "-o", executable]),
                "GNU ld collects the patch record for an unused code section")
        assert records(executable) == [(8, ".text")], records(executable)
        require(invoke([executable]), "run functions remaining after section collection")


def access_attributes(mcc, gcc, root, target, runner):
    source = root / "access-calls.c"
    source.write_text("""
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#if !__has_attribute(__access__)
#error missing access attribute
#endif
#define ACCESS(...) __attribute__((__access__(__VA_ARGS__)))
void read(const int *, long) ACCESS(__read_only__, 1, 1 + 1);
void read(const int *, long);
void write(int *, int) ACCESS(write_only, 1, 2);
void change(int *, int) ACCESS(read_write, 1, 2);
void size_only(const int *, int) ACCESS(none, 1, 2);
void raw_size(const void *) ACCESS(none, 1);
void byte_write(void *, size_t) ACCESS(write_only, 1, 2);
void read_one(const int *) ACCESS(read_only, 1);
void move(int *, const int *, int) ACCESS(write_only, 1, 3) ACCESS(read_only, 2, 3);
typedef void Writer(int *, int) ACCESS(write_only, 1, 2);
extern Writer *indirect;
struct Record { int item[2]; int rest[4]; };
struct Callbacks { void (*writer)(int *, int) ACCESS(write_only, 1, 2); };
struct Incomplete;
void incomplete(struct Incomplete *, int) ACCESS(read_only, 1, 2);
_Static_assert(__builtin_has_attribute(read, access), "declaration merge");
_Static_assert(__builtin_has_attribute(Writer, access), "function type");
_Static_assert(__builtin_has_attribute(*indirect, access), "function pointer target");
_Static_assert(!__builtin_has_attribute(indirect, access), "pointer type");
_Static_assert(!__builtin_has_attribute(read, access(read_only, 1, 2)), "normalized arguments");

void cases(struct Callbacks callbacks, int branch, void *unknown) {
    int a[2] = {0}, b[4] = {0};
    struct Record record = {0};
    int *copy = a;
    int *allocated = malloc(sizeof(int));
    read(a, 3);                         /* overread */
    write(a, 3);                        /* overflow */
    change(a, 3);                       /* overflow */
    size_only(a, 3);                    /* overread */
    byte_write(a, sizeof(a) + 1);        /* overflow */
    read_one(a + 2);                     /* overread */
    read(record.item, 3);                /* overread */
    move(b, a, 3);                       /* overread */
    indirect(a, 3);                      /* overflow */
    callbacks.writer(a, 3);              /* overflow */
    read(copy, 3);                       /* overread */
    read(allocated, 2);                  /* overread */
    read(malloc(sizeof(int)), 2);        /* overread */
    incomplete((struct Incomplete *)a, 9); /* overread */
    read(branch ? a : b, 5);             /* overread */
    read(a, -1);                         /* overflow */
    read(unknown, LONG_MAX);              /* overflow */
    read(0, 1);                           /* nonnull */
    write(0, 1);                          /* nonnull */
    size_only(0, 1);                      /* nonnull */
    read(a, 2);
    read(branch ? a : b, 3);
    write(a + 2, 0);
    read(0, 0);
    read_one(0);
    raw_size(a + 2);
    if (0) read(a, 3);
    (void)sizeof((read(a, 3), 1));
    free(allocated);
}
""")
    cases = {"overread": "stringop-overread", "overflow": "stringop-overflow", "nonnull": "nonnull"}
    expected = {line: cases[marker] for line, text in enumerate(source.read_text().splitlines(), 1)
                for marker in re.findall(r"/\* (\w+) \*/", text)}
    invalid = root / "access-invalid.c"
    declarations = (
        "void f(int *) ACCESS(missing, 1);",
        'void f(int *) ACCESS("read_only", 1);',
        "void f(int *) ACCESS(read_only);",
        "void f(int *) ACCESS(read_only, 0);",
        "void f(int *) ACCESS(read_only, 2);",
        "void f(int) ACCESS(read_only, 1);",
        "void f(const int *) ACCESS(write_only, 1);",
        "void f(void (*)(void)) ACCESS(read_only, 1);",
        "void f(int *, float) ACCESS(read_only, 1, 2);",
        "void f(int *, ...) ACCESS(read_only, 1, 2);",
        "void f() ACCESS(read_only, 1);",
    )
    conflicting = root / "access-redeclare.c"
    conflicting.write_text("""
__attribute__((access(read_only, 1, 2))) void read(int *, int);
__attribute__((access(write_only, 1, 2))) void read(int *, int);
""")
    runtime = root / "access-runtime.c"
    runtime.write_text("""
typedef void Callback(int*, unsigned) __attribute__((access(write_only, 1, 2)));
static Callback fill;
static void fill(int *out, unsigned length) {
    for (unsigned i = 0; i < length; i++)
        out[i] = (int)i + 3;
}
int main(void) {
    int array[2];
    Callback *action = fill;
    action(array, 2);
    action(0, 0);
    return (array[0] != 3 || array[1] != 4);
}
""")
    flags = [f"--target={target}"] if target else []
    for cc, options, command in ((mcc, flags, runner), (gcc, ["-O0", "-Wno-format-security"], [])):
        prefix = [cc, *options, "-std=c11", "-pedantic-errors"]
        output = root / "access-calls.s"
        for warnings, suppressed in (
            (["-Wnonnull"], None),
            (["-Wnonnull", "-Wno-stringop-overread"], "stringop-overread"),
            (["-Wall", "-Wno-nonnull"], "nonnull"),
            (["-Wno-format"], "nonnull"),
        ):
            result = invoke([*prefix, *warnings, "-S", source, "-o", output])
            require(result, f"{cc} compile calls to access-annotated functions")
            actual = {(int(line), group) for line, group in re.findall(
                r"access-calls\.c:(\d+):\d+: warning:[^\n]*\[-W([\w-]+)=?\]", result.stderr
            )}
            actual = {(line, group) for line, group in actual if group in cases.values()}
            assert actual == {(line, group) for line, group in expected.items() if group != suppressed}, (
                cc, warnings, result.stderr, expected
            )
        for declaration in declarations:
            invalid.write_text("#define ACCESS(...) __attribute__((access(__VA_ARGS__)))\n" + declaration + "\n")
            require(invoke([*prefix, "-fsyntax-only", invalid]), f"{cc} reject {declaration}", success=False)
        require(invoke([*prefix, "-Werror=attributes", "-fsyntax-only", conflicting]),
                f"{cc} diagnose conflicting access attributes", success=False)
        executable = root / "access-runtime"
        require(invoke([*prefix, "-Werror", runtime, "-o", executable]), f"{cc} compile an annotated callback")
        require(invoke([*command, executable]), f"{cc} run an annotated callback")


def access_initialization(mcc, gcc, root, target):
    source = root / "access-initialization.c"
    source.write_text("""
#define ACCESS(...) __attribute__((access(__VA_ARGS__)))
void input(const int *, unsigned) ACCESS(read_only, 1, 2);
void output(int *, unsigned) ACCESS(write_only, 1, 2);
void update(int *, unsigned) ACCESS(read_write, 1, 2);
void observe(const int *, unsigned) ACCESS(none, 1, 2);
void unknown_output(int *);
void volatile_input(const volatile int *) ACCESS(read_only, 1);
typedef void Reader(const int *) ACCESS(read_only, 1);
extern Reader *indirect;
struct Pair { int first, second; };
int *escaped;
int statically_initialized[2];

void scalar(void) {
    int value;
    input(&value, 1);                 /* uninitialized */
}
void array(unsigned n) {
    int values[3];
    input(values + 1, n);            /* uninitialized */
}
void copy(void) {
    int values[2];
    const int *p = values;
    input(p, 2);                     /* uninitialized */
    input(p, 2);
}
void before_store(void) {
    int value;
    input(&value, 1);                /* uninitialized */
    value = 2;
}
void other_member(void) {
    struct Pair pair;
    pair.first = 1;
    input(&pair.second, 1);          /* uninitialized */
}
void callback(void) {
    int value;
    indirect(&value);                /* uninitialized */
}
void variable_array(int n) {
    int values[n];
    input(values, 1);                /* uninitialized */
}
void after_dead_store(void) {
    int value;
    if (0) value = 7;
    input(&value, 1);                /* uninitialized */
}
void write_then_read(void) {
    int values[2];
    output(values, 2);
    input(values, 2);
}
void read_and_write(void) {
    int values[2];
    update(values, 2);               /* maybe-uninitialized */
    input(values, 2);
}
void by_alias(void) {
    int values[2], *p = values;
    p[1] = 3;
    input(values + 1, 1);
}
void ambient_write(void) {
    int value;
    escaped = &value;
    *escaped = 10;
    input(&value, 1);
}
void escape_before_read(void) {
    int value;
    escaped = &value;
    input(&value, 1);                /* uninitialized */
}
void uncertain_write(int flag) {
    int value;
    if (flag) value = 1;
    input(&value, 1);
}
void initialized(int value) {
    int array[2] = {0};
    struct Pair pair;
    pair.second = 9;
    input(array, 2);
    input(statically_initialized, 2);
    input(&pair.second, 1);
    input(&value, 1);
}
void untouched(void) {
    int a[2], b[2], c[2];
    if (0) input(a, 2);
    (void)sizeof((input(a, 2), 1));
    input(a + 2, 1);
    input(a, 0);
    observe(b, 2);
    output(c, 2);
}
void maybe_written(void) {
    int value;
    unknown_output(&value);
    input(&value, 1);
}
void volatile_object(void) {
    volatile int value;
    volatile_input(&value);          /* uninitialized */
}
""")
    expected = {(line, kind) for line, text in enumerate(source.read_text().splitlines(), 1)
                for kind in re.findall(r"/\* ((?:maybe-)?uninitialized) \*/", text)}
    modes = [
        (["-Wall"], {"uninitialized", "maybe-uninitialized"}),
        (["-Wextra"], {"uninitialized", "maybe-uninitialized"}),
        (["-Wuninitialized"], {"uninitialized", "maybe-uninitialized"}),
        (["-Wmaybe-uninitialized"], {"maybe-uninitialized"}),
        (["-Wall", "-Wno-maybe-uninitialized"], {"uninitialized"}),
        ([], set()),
    ]
    flags = [f"--target={target}"] if target else []
    for cc, options in ((mcc, flags), (gcc, ["-O0"])):
        prefix = [cc, *options, "-std=c11", "-pedantic-errors", "-Wno-stringop-overread"]
        output = root / "access-initialization.s"
        for warnings, enabled in modes:
            result = invoke([*prefix, *warnings, "-S", source, "-o", output])
            require(result, f"{cc} analyze initialization before access-annotated calls")
            actual = [(int(line), kind) for line, kind in re.findall(
                r"access-initialization\.c:(\d+):\d+: warning:[^\n]*\[-W((?:maybe-)?uninitialized)\]",
                result.stderr
            )]
            assert sorted(actual) == sorted((line, kind) for line, kind in expected if kind in enabled), (
                cc, warnings, result.stderr
            )


def access_size_ranges(mcc, gcc, root, target):
    source = root / "access-size-ranges.c"
    source.write_text("""
#include <limits.h>
#define ACCESS(...) __attribute__((access(__VA_ARGS__)))
void read_array(const int *, long) ACCESS(read_only, 1, 2);
void write_array(int *, unsigned) ACCESS(write_only, 1, 2);
void update_count(int *);

void expressions(int flag, const int *unknown) {
    int array[2] = {0};
    read_array(array, flag ? 1 : 3);
    read_array(array, flag ? 3 : 5);             /* overread */
    read_array(array, flag ? -1 : 1);
    read_array(array, flag ? -5 : -1);           /* overflow */
    read_array(array, flag ? 0 : 3);
    read_array(0, flag ? 0 : 3);
    read_array(0, flag ? 1 : 3);                 /* nonnull */
    read_array(unknown, flag ? LONG_MAX : 1);
    read_array(unknown, flag ? LONG_MAX : LONG_MAX - 1); /* overflow */
    write_array(array, ((unsigned)flag & 1) + 3); /* overflow */
    write_array(array, ((unsigned)flag & 1) + 2);
    read_array(array, (unsigned char)(flag ? 256 : 257));
    read_array(array, (short)(flag ? 258 : 259)); /* overread */
}

void paths(int flag, int count, int escaped) {
    int array[2] = {0};
    if (count > 2 && count < 10)
        write_array(array, (unsigned)count);      /* overflow */
    if (3 == count)
        write_array(array, (unsigned)count);      /* overflow */
    if (count > 2) {
        count = 1;
        read_array(array, count);
    }
    if (count > 2)
        read_array(array, (count = 1, count));
    if (escaped > 2) {
        update_count(&escaped);
        read_array(array, escaped);
    }
    int size;
    if (flag) size = 3; else size = 5;
    read_array(array, size);                     /* overread */
    if (flag) size = 3; else size = 1;
    read_array(array, size);
    unsigned masked = ((unsigned)flag & 1) + 3;
    write_array(array, masked);                  /* overflow */
    if (size <= 2 || size >= 10) return;
    write_array(array, (unsigned)size);           /* overflow */
}
""")
    groups = {"overread": "stringop-overread", "overflow": "stringop-overflow", "nonnull": "nonnull"}
    expected = {(line, groups[marker]) for line, text in enumerate(source.read_text().splitlines(), 1)
                for marker in re.findall(r"/\* (\w+) \*/", text)}
    flags = [f"--target={target}"] if target else []
    output = root / "access-size-ranges.s"
    for cc, options in ((mcc, flags), (gcc, ["-O0"])):
        result = invoke([cc, *options, "-std=c11", "-pedantic-errors", "-Wnonnull", "-S", source, "-o", output])
        require(result, f"{cc} compile ranged sizes in access-annotated calls")
        actual = {(int(line), group) for line, group in re.findall(
            r"access-size-ranges\.c:(\d+):\d+: warning:[^\n]*\[-W([\w-]+)=?\]", result.stderr
        )}
        assert actual == expected, (cc, result.stderr, expected)
        if cc == mcc:
            assert "reads at least 12 bytes" in result.stderr, result.stderr

    wide = root / "access-wide-size-ranges.c"
    wide.write_text("""
__extension__ typedef unsigned __int128 Count;
void read_array(const int *, Count) __attribute__((access(read_only, 1, 2)));
void cases(int flag) {
    int array[2] = {0};
    read_array(array, flag ? (Count)1 << 80 : (Count)1 << 64); /* overflow */
    read_array(array, flag ? (Count)1 << 80 : 1);
}
""")
    result = invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-S", wide, "-o", output])
    require(result, "mcc compile 128-bit ranges for access sizes")
    assert result.stderr.count("[-Wstringop-overflow]") == 1 and "maximum object size" in result.stderr, result.stderr


def check_access_warning_lines(mcc, gcc, source, target, remaining):
    groups = {"overread": "stringop-overread", "overflow": "stringop-overflow"}
    required, additional = set(), set()
    for line, text in enumerate(source.read_text().splitlines(), 1):
        for mcc_only, marker in re.findall(r"/\* (mcc-)?(\w+) \*/", text):
            (additional if mcc_only else required).add((line, groups[marker]))
    flags = [f"--target={target}"] if target else []
    diagnostics = rf"{re.escape(source.name)}:(\d+):\d+: warning:[^\n]*\[-W([\w-]+)=?\]"
    for cc, options in ((mcc, flags), (gcc, ["-O0"])):
        result = invoke([cc, *options, "-std=c11", "-pedantic-errors", "-S", source,
                         "-o", source.with_suffix(".s")])
        require(result, f"{cc} compile {source.name}")
        actual = {(int(line), group) for line, group in re.findall(diagnostics, result.stderr)}
        expected = required | additional
        assert required <= actual <= expected, (cc, actual, result.stderr)
        if cc == mcc:
            assert actual == expected and f"at most {remaining} bytes remaining" in result.stderr, (actual, result.stderr)


def access_pointer_offsets(mcc, gcc, root, target):
    source = root / "access-pointer-offsets.c"
    source.write_text("""
#include <stdlib.h>
#define ACCESS(...) __attribute__((access(__VA_ARGS__)))
void read_array(const int *, int) ACCESS(read_only, 1, 2);
void write_array(int *, int) ACCESS(write_only, 1, 2);
void read_bytes(const void *, int) ACCESS(read_only, 1, 2);
struct Record { int head[4]; int tail[8]; };

void expressions(int choose, int index, int *unknown) {
    int array[4] = {0};
    struct Record record = {0};
    read_array(array + (choose ? 2 : 3), 3);                 /* overread */
    read_array(array + (choose ? 1 : 3), 3);
    read_array(array + (choose ? 4 : 5), 1);                 /* overread */
    read_array(array + (choose ? -1 : 2), 3);                /* overread */
    read_array(array + (choose ? -2 : -1), 1);               /* overread */
    read_array(array + (((unsigned)index & 1) + 2), 3);      /* overread */
    read_array((array + 4) - (choose ? 1 : 2), 3);           /* overread */
    read_array((array + 4) - (choose ? 1 : 3), 3);
    write_array(record.head + (choose ? 2 : 3), 3);          /* overflow */
    read_bytes((char *)record.head + (choose ? 12 : 13), 6); /* overread */
    if (index > 1 && index < 4)
        read_array(array + index, 3);                      /* overread */
    if (index > 1 && index < 4)
        read_array(array + index, 2);
    if (index > 1 && index < 4)
        read_array(index + array, 3);                       /* overread */
    if (index > 1 && index < 4)
        read_array(choose ? array + index : unknown, 3);
    if (index > 1 && index < 4)
        read_array(array + (index = 0, index), 3);
}

void saved_pointers(int index, int choose) {
    int array[4] = {0};
    if (index > 1 && index < 4) {
        int *pointer = array + index;
        index = 0;
        read_array(pointer, 3);                            /* overread */
    }
    int *pointer = array + (choose ? 2 : 3);
    int *copy = pointer;
    read_array(copy, 3);                                   /* overread */
    int *earlier = array + index;
    if (index > 1 && index < 4)
        read_array(earlier, 3);
    int displacement = 0;
    int *safe = array + displacement;
    displacement = 3;
    read_array(safe, 3);
    int *near_end = array + displacement;
    displacement = 0;
    read_array(near_end, 3);                               /* overread */
}

void buffers(int choose) {
    int a[4] = {0}, b[6] = {0};
    int matrix[2][4] = {{0}};
    int *heap = malloc(4 * sizeof(int));
    read_array(heap + (choose ? 2 : 3), 3);                 /* overread */
    free(heap);
    read_array(matrix[(unsigned)choose & 1] + (choose ? 2 : 3), 3); /* mcc-overread */
    read_array(choose ? a + 2 : b + 4, 3);                  /* mcc-overread */
    read_array(choose ? a + 2 : b + 3, 3);
}
""")
    check_access_warning_lines(mcc, gcc, source, target, 8)


def access_allocation_ranges(mcc, gcc, root, target):
    source = root / "access-allocation-ranges.c"
    source.write_text("""
#include <stdlib.h>
#define ACCESS(...) __attribute__((access(__VA_ARGS__)))
void read_bytes(const void *, size_t) ACCESS(read_only, 1, 2);
void write_bytes(void *, size_t) ACCESS(write_only, 1, 2);
void *custom(size_t) __attribute__((alloc_size(1)));
void *product(size_t, size_t) __attribute__((alloc_size(1, 2)));
void *swapped(size_t, size_t, size_t) __attribute__((alloc_size(3, 1)));
typedef void *Factory(size_t) __attribute__((alloc_size(1)));
extern Factory *factory;

void direct(int choose, size_t count) {
    write_bytes(malloc(choose ? 4 : 8), 9);                       /* overflow */
    write_bytes(malloc(choose ? 4 : 8), 5);
    write_bytes(calloc(choose ? 2 : 3, 4), 13);                   /* overflow */
    write_bytes(product(choose ? 2 : 3, choose ? 4 : 5), 16);     /* overflow */
    write_bytes(swapped(choose ? 2 : 3, count, 4), 13);           /* overflow */
    write_bytes(custom((count & 3) + 4), 8);                      /* overflow */
    write_bytes(factory((count & 3) + 4), 8);                     /* overflow */
    write_bytes(__builtin_alloca(choose ? 4 : 8), 9);             /* overflow */
    write_bytes(__builtin_alloca_with_align(choose ? 4 : 8, 64), 9); /* overflow */
    write_bytes(__builtin_alloca_with_align_and_max(choose ? 4 : 8, 64, 16), 9); /* mcc-overflow */
    write_bytes(malloc(count), 17);
    write_bytes(malloc(choose ? 4 : count), 9);
    if (count > 0 && count <= 8)
        write_bytes(malloc(count), 9);                           /* overflow */
    if (count > 0 && count <= 8)
        write_bytes(malloc(count), 8);
}

void saved(size_t count, int choose) {
    if (count > 0 && count < 9) {
        char *small = malloc(count);
        count = 32;
        write_bytes(small, 9);                                   /* overflow */
        free(small);
    }
    void *earlier = malloc(count);
    if (count == 4)
        write_bytes(earlier, 5);
    free(earlier);
    char *buffer = malloc(choose ? 8 : 12);
    char *alias = buffer;
    read_bytes(alias + (choose ? 4 : 6), 9);                     /* overread */
    read_bytes(alias + (choose ? 4 : 6), 8);
    free(buffer);
}

void limits(int choose, size_t count) {
    write_bytes(calloc(choose ? 0 : 0, count), 1);                /* overflow */
    write_bytes(calloc(0, choose ? 4 : 8), 1);                   /* overflow */
    write_bytes(calloc(choose ? 4 : count, 2), 13);
    write_bytes(realloc(0, choose ? 4 : 8), 9);                  /* overflow */
    write_bytes(aligned_alloc(8, choose ? 8 : 16), 17);          /* overflow */
}
""")
    check_access_warning_lines(mcc, gcc, source, target, 12)
    flags = [f"--target={target}"] if target else []

    builtin = root / "access-allocation-builtins.c"
    builtin.write_text("""
void *malloc(unsigned long);
void write_bytes(void *, unsigned long) __attribute__((access(write_only, 1, 2)));
__extension__ typedef unsigned __int128 Wide;
void *wide_allocate(Wide) __attribute__((alloc_size(1)));
void examples(int choose) {
    write_bytes(malloc(choose ? 4 : 8), 9);
    write_bytes(__builtin_malloc(choose ? 4 : 8), 9);
    write_bytes(wide_allocate(choose ? 4 : 8), 9);
    write_bytes(wide_allocate(choose ? (Wide)1 << 64 : 8), 9);
}
""")
    for options, warnings in (([], 3), (["-fno-builtin-malloc"], 2)):
        result = invoke([mcc, *flags, *options, "-std=c11", "-pedantic-errors", "-S", builtin,
                         "-o", root / "access-allocation-builtins.s"])
        require(result, "mcc honor allocation builtin settings and wide allocation dimensions")
        assert result.stderr.count("[-Wstringop-overflow]") == warnings, result.stderr


def access_pointer_assignments(mcc, gcc, root, target):
    source = root / "access-pointer-assignments.c"
    source.write_text("""
#include <stdlib.h>
void read_bytes(const void *, size_t) __attribute__((access(read_only, 1, 2)));
void write_bytes(void *, size_t) __attribute__((access(write_only, 1, 2)));

void sequential(char *unknown) {
    char small[4] = {0}, large[16] = {0};
    char *p = large;
    read_bytes(p, 5);
    p = small;
    read_bytes(p, 5);                     /* overread */
    p = large;
    read_bytes(p, 5);
    p = unknown;
    read_bytes(p, 5);
}

void branches(int flag, char *unknown) {
    char a[4] = {0}, b[5] = {0}, large[16] = {0};
    char *p;
    if (flag)
        p = a;
    else
        p = b;
    read_bytes(p, 6);                     /* overread */
    read_bytes(p, 5);
    p = large;
    if (flag)
        p = a;
    read_bytes(p, 6);
    if (flag)
        p = b;
    else
        p = unknown;
    read_bytes(p, 6);
}

void allocations(int flag, size_t amount) {
    char *p = malloc(16);
    free(p);
    p = malloc(flag ? 4 : 8);
    write_bytes(p, 9);                    /* overflow */
    write_bytes(p, 8);
    free(p);
    p = malloc(amount);
    write_bytes(p, 9);
    free(p);
}

void snapshots(char *unknown, unsigned offset) {
    char small[8] = {0}, large[16] = {0};
    char *p = unknown;
    p = small;
    char *saved = p;
    p = large;
    read_bytes(saved, 9);                 /* overread */
    read_bytes(p, 9);
    if (offset >= 4 && offset <= 6) {
        p = small + offset;
        offset = 0;
        read_bytes(p, 5);                 /* mcc-overread */
        read_bytes(p, 4);
    }
    p = small;
    p += 2;
    read_bytes(p, 7);                     /* overread */
    read_bytes(p, 6);
}

void loops(unsigned iterations) {
    char storage[30];
    char *p = storage + 26;
    for (unsigned i = 0; i < (iterations & 3); ++i)
        p -= 4;
    write_bytes(p, 10);
    p = storage;
    do {
        p = storage + 26;
    } while (iterations--);
    write_bytes(p, 5);                    /* mcc-overflow */
}

void jumps(int flag, char *unknown) {
    char small[4] = {0}, large[16] = {0};
    char *p = unknown;
    if (flag)
        goto small_path;
    p = large;
    goto use;
small_path:
    p = small;
use:
    read_bytes(p, 5);
    p = small;
    read_bytes(p, 5);                     /* overread */
}

void replace_pointer(char **);
void escaped(void) {
    char small[4] = {0};
    char *p = small;
    replace_pointer(&p);
    read_bytes(p, 5);
}
""")
    check_access_warning_lines(mcc, gcc, source, target, 5)
    flags = [f"--target={target}"] if target else []

    long_chain = root / "access-long-pointer-chain.c"
    long_chain.write_text("""
void write_bytes(void *, unsigned long) __attribute__((access(write_only, 1, 2)));
void test(void) {
    char buffer[4];
    char *p = buffer;
""" + "    p += 0;\n" * 12000 + "    write_bytes(p, 5);\n}\n")
    require(invoke([mcc, *flags, "-std=c11", "-S", long_chain, "-o", long_chain.with_suffix(".s")]),
            "mcc compile a long chain of pointer assignments")


def system_register_builtins(mcc, gcc, root, target, runner):
    source = root / "system-register-builtins.c"
    source.write_text("""
#include <pthread.h>
#include <stdint.h>

#if !__has_builtin(__builtin_thread_pointer)
#error missing thread pointer builtin
#endif
_Static_assert(_Generic(__builtin_thread_pointer(), void *: 1, default: 0), "thread pointer type");

static _Thread_local unsigned local = 42;

struct observations {
    void *thread;
    uintptr_t displacement;
    int valid;
};

static void *observe(void *opaque)
{
    struct observations *out = opaque;
    void *actual = __builtin_thread_pointer();
    void *expected;
#if defined(__aarch64__)
    __asm__("mrs %0, tpidr_el0" : "=r"(expected));
#else
    __asm__("movq %%fs:0, %0" : "=r"(expected));
#endif
    out->thread = actual;
    out->displacement = (uintptr_t)&local - (uintptr_t)actual;
    out->valid = actual != 0 && actual == expected && local == 42;
    ++local;
    return 0;
}

#if defined(__aarch64__)
#if __has_builtin(__builtin_ia32_stmxcsr) || !__has_builtin(__builtin_aarch64_get_fpsr64)
#error incorrect AArch64 register availability
#endif
#define FPCR __builtin_aarch64_get_fpcr
#define FPSR __builtin_aarch64_get_fpsr
#define FPCR64 __builtin_aarch64_get_fpcr64
#define FPSR64 __builtin_aarch64_get_fpsr64
#define SET_FPCR __builtin_aarch64_set_fpcr
#define SET_FPSR __builtin_aarch64_set_fpsr
#define SET_FPCR64 __builtin_aarch64_set_fpcr64
#define SET_FPSR64 __builtin_aarch64_set_fpsr64
_Static_assert(_Generic(FPCR(), unsigned: 1, default: 0), "FPCR type");
_Static_assert(_Generic(FPSR(), unsigned: 1, default: 0), "FPSR type");
_Static_assert(_Generic(FPCR64(), unsigned long long: 1, default: 0), "wide FPCR type");
_Static_assert(_Generic(FPSR64(), unsigned long long: 1, default: 0), "wide FPSR type");

static int floating_environment(void)
{
    const unsigned round = 1u << 22, round_mask = 3u << 22, inexact = 1u << 4;
    unsigned long long old_control = FPCR64(), old_status = FPSR64();
    unsigned next = (FPCR() & ~round_mask) | round;
    SET_FPCR(next);
    SET_FPSR(FPSR() & ~inexact);
    volatile float one = 1.0f, half_ulp = 0x1p-24f;
    volatile float rounded = one + half_ulp;
    unsigned long long updated_control = FPCR64(), updated_status = FPSR64();
    SET_FPCR64(old_control);
    SET_FPSR64(old_status);
    return rounded == 0x1.000002p0f && next == updated_control &&
           (updated_status & inexact) && FPCR64() == old_control && FPSR64() == old_status;
}
#else
#if __has_builtin(__builtin_aarch64_get_fpcr) || !__has_builtin(__builtin_ia32_stmxcsr)
#error incorrect x86-64 register availability
#endif
#define MXCSR __builtin_ia32_stmxcsr
#define SET_MXCSR __builtin_ia32_ldmxcsr
_Static_assert(_Generic(MXCSR(), unsigned: 1, default: 0), "MXCSR type");

static int floating_environment(void)
{
    const unsigned round = 2u << 13, round_mask = 3u << 13, inexact = 1u << 5;
    unsigned original = MXCSR();
    SET_MXCSR((original & ~(round_mask | inexact)) | round);
    volatile float one = 1.0f, half_ulp = 0x1p-24f;
    volatile float rounded = one + half_ulp;
    unsigned updated = MXCSR();
    SET_MXCSR(original);
    return rounded == 0x1.000002p0f && (updated & round_mask) == round &&
           (updated & inexact) && MXCSR() == original;
}
#endif

int main(void)
{
    pthread_t worker;
    struct observations current, other;
    observe(&current);
    if (pthread_create(&worker, 0, observe, &other) || pthread_join(worker, 0))
        return 1;
    if (!current.valid || !other.valid || current.thread == other.thread ||
        current.displacement != other.displacement || local != 43)
        return 2;
    return floating_environment() ? 0 : 3;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "system-register-builtins"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        rounding = [] if cc == mcc else ["-frounding-math"]
        require(invoke([*prefix, *rounding, "-std=c11", "-pedantic-errors", "-fno-builtin", "-pthread", source,
                        "-o", binary]), f"{cc} compile thread pointer and floating-point register test")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} thread pointer and FP register semantics")
        input_flags = [] if cc == mcc else ["-x", "c"]
        invalid = [*prefix, *input_flags, "-std=gnu11", "-Werror", "-c", "-o", root / "invalid-sysreg.o", "-"]
        for code in ("void *f(void) { return __builtin_thread_pointer(1); }",
                     "void *f(void) { return (void *)&__builtin_thread_pointer; }",
                     "void *p = __builtin_thread_pointer();",
                     "void f(void *p) { __builtin_set_thread_pointer(p); }",
                     """#if defined(__aarch64__)
                         void f(void) { __builtin_aarch64_set_fpcr(); }
                         #else
                         void f(void) { __builtin_ia32_ldmxcsr(); }
                         #endif"""):
            require(invoke(invalid, code), f"{cc} reject {code}", success=False)


def object_sizes(mcc, gcc, root, target, runner):
    source = """
#include <stddef.h>
#if !__has_builtin(__builtin_object_size) || !__has_builtin(__builtin_dynamic_object_size)
#error missing object-size builtins
#endif
#define BOS __builtin_object_size
#define DOS __builtin_dynamic_object_size
struct buffer { char left[10]; int word; char right[10]; };
static struct buffer s;
static int rows[2][4];
__extension__ static struct packet { int count; char data[]; } packet = {3, {1, 2, 3}};
static volatile unsigned calls;
extern char *never_called(void);
static int probe(unsigned i, int choose, char *unknown) {
    char a[8], b[16];
    if (BOS(s.left + 1, 0) != sizeof s - 1 || BOS(&s.left[1], 1) != sizeof s.left - 1)
        return 1;
    if (BOS(&s.word, 2) != sizeof s - offsetof(struct buffer, word) ||
        BOS(&s.word, 3) != sizeof s.word || BOS(&s.left[10], 1) != 0)
        return 2;
    if (BOS(rows[0], 0) != sizeof rows ||
        BOS(&rows[0][2], 1) != 2 * sizeof (int) || BOS("abc" + 2, 0) != 2)
        return 3;
    if (BOS(&packet, 0) != offsetof(struct packet, data) + 3 ||
        BOS(packet.data + 1, 1) != 2)
        return 4;
    if (BOS(a + calls++, 0) != (size_t)-1 || DOS(b + ++calls, 2) != 0 ||
        BOS(never_called(), 1) != (size_t)-1 || DOS(never_called(), 3) != 0 || calls)
        return 5;
    if (DOS(s.right + i, 0) != sizeof s - offsetof(struct buffer, right) - i)
        return 6;
    unsigned count = i + 4;
    long vla[count];
    count += 13;
    if (DOS(vla, 0) != (i + 4) * sizeof (long) || DOS(vla + i, 1) != 4 * sizeof (long))
        return 7;
    if (DOS(choose ? a + i : b + i, 0) != (choose ? sizeof a : sizeof b) - i)
        return 8;
#ifdef __MCC__
    if (DOS(s.right + i, 1) != sizeof s.right - i)
        return 6;
    if (BOS(rows[0], 1) != sizeof rows[0] || DOS(rows[i - 2] + i, 3) != 2 * sizeof (int))
        return 11;
    if (BOS(choose ? a + 1 : b + 2, 0) != 14 ||
        BOS(choose ? a + 1 : b + 2, 2) != 7 ||
        BOS((choose ? a + 1 : b + 2) + 2, 1) != 12)
        return 9;
    if (BOS(unknown, 0) != (size_t)-1 || BOS(unknown, 2) != 0 ||
        DOS(choose ? unknown : s.right, 1) != (choose ? (size_t)-1 : sizeof s.right))
        return 10;
    if (DOS((choose ? (struct buffer *)unknown : &s)->left, 1) !=
        (choose ? (size_t)-1 : sizeof s.left))
        return 12;
#endif
    return 0;
}
int main(void) {
    int first = probe(2, 0, 0);
    return first ? first : probe(2, 1, 0);
}
"""
    binary = root / "object-sizes"
    for cc in (mcc, gcc):
        flags = ["-x", "c", "-O2"] if cc == gcc else [f"--target={target}"] if target else []
        require(invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", "-", "-o", binary],
                       source), f"{cc} object-size build")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} object-size execution")
    for source in (
        "int f(void *p, int mode) { return __builtin_object_size(p, mode); }",
        "int f(void *p) { return __builtin_object_size(p, 4); }",
        "int f(void *p) { return __builtin_dynamic_object_size(p, -1); }",
        "int f(void) { return __builtin_object_size(12, 0); }",
        "int f(void *p) { return __builtin_dynamic_object_size(p); }",
    ):
        require(invoke([mcc, "-std=gnu11", "-fsyntax-only", "-"], source), source, success=False)


def allocation_sizes(mcc, gcc, root, target, runner):
    provider = root / "allocation-provider.c"
    provider.write_text("""
#include <stddef.h>
static char memory[256];
unsigned calls;
char *allocate(size_t n) { calls++; return n <= sizeof memory ? memory : 0; }
char *allocate_array(size_t n, size_t width) {
    calls++;
    return !width || n <= sizeof memory / width ? memory : 0;
}
char *(*factory(void))(size_t) { return allocate; }
char *unknown_allocation(void) { calls++; return memory; }
int next_size(void) { calls++; return 13; }
void overwrite(char **p) { *p = memory; }
""")
    source = root / "allocation-size.c"
    source.write_text("""
#include <stddef.h>
#if !__has_attribute(alloc_size)
#error alloc_size attribute query
#endif
#define BOS __builtin_object_size
#define DOS __builtin_dynamic_object_size
extern unsigned calls;
char *allocate(size_t) __attribute__((__alloc_size__(1)));
char *allocate(size_t);
char *allocate_array(size_t, size_t) __attribute__((alloc_size(1, 2)));
char *unknown_allocation(void);
int next_size(void);
void overwrite(char **);
enum { amount = 1 };
typedef __attribute__((alloc_size(amount))) char *(*Allocator)(size_t);
Allocator factory(void);
struct allocator { char *(*function)(size_t) __attribute__((alloc_size(1))); };
static __attribute__((noinline)) int check(Allocator cb, size_t n, int pick) {
    unsigned start = calls;
    if (BOS(allocate(8), 0) != (size_t)-1 || DOS(allocate_array(n++, 2), 2) != 0 || calls != start)
        return 1;
    char *fixed = allocate(21);
    if (BOS(fixed, 0) != 21 || BOS(fixed + 3, 3) != 18 || DOS(fixed, 0) != 21)
        return 2;
    char *product = allocate_array(8, 5);
    if (BOS(product, 1) != 40 || BOS(product + 39, 2) != 1)
        return 3;
    char *side_effect = allocate(n++);
    if (DOS(side_effect + 2, 1) != n - 3 || DOS(side_effect, 3) != n - 1)
        return 4;
    char *from_call = allocate((size_t)next_size());
    if (DOS(from_call, 0) != 13 || DOS(from_call + 3, 1) != 10 || calls != start + 5)
        return 5;
    char *indirect = cb(n);
    if (DOS(indirect, 0) != n)
        return 6;
    struct allocator a = {cb};
    char *through_member = a.function(n + 1);
    if (DOS(through_member, 0) != n + 1)
        return 7;
    char *zero = allocate_array(n, 0);
    if (DOS(zero, 0) != 0)
        return 11;
    char *assigned;
    if (pick) assigned = allocate(n);
    else assigned = allocate_array(n, 2);
    size_t len = pick ? n : n * 2;
    if (DOS(assigned + 1, 0) != len - 1)
        return 8;
    assigned = unknown_allocation();
    if (DOS(assigned, 0) != (size_t)-1 || DOS(assigned + 1, 3) != 0)
        return 9;
    char *escaped = allocate(n);
    overwrite(&escaped);
    if (DOS(escaped, 1) != (size_t)-1)
        return 10;
    return 0;
}
#ifdef __MCC__
static size_t replace_parameter(char *initial, size_t n, int pick) {
    if (pick) initial = allocate(n);
    return DOS(initial, 0);
}
#endif
int main(int argc, char **argv) {
    (void)argv;
    Allocator callback = factory();
    size_t n = (size_t)argc + 4;
#ifdef __MCC__
    if (replace_parameter(unknown_allocation(), n, 0) != (size_t)-1 ||
        replace_parameter(unknown_allocation(), n, 1) != n)
        return 12;
#endif
    int error = check(callback, n, 0);
    return error ? error : check(callback, n + 2, 1);
}
""")
    boundary = root / "allocation-boundary.c"
    boundary.write_text("""
#include <stddef.h>
char *allocate(size_t) __attribute__((alloc_size(1)));
int main(int argc, char **argv) {
    (void)argv;
    size_t n = (size_t)argc + 2;
    char *p = allocate(n);
    __builtin___memset_chk(p, 0, n + (argc > 1),
                          __builtin_dynamic_object_size(p, 0));
    return 0;
}
""")
    binary = root / "allocation-size"
    for cc in (mcc, gcc):
        flags = ["-O2"] if cc == gcc else [f"--target={target}"] if target else []
        options = [cc, *flags, "-std=c11", "-pedantic-errors"]
        run = runner if cc == mcc else []
        require(invoke([*options, source, provider, "-o", binary]), f"{cc} alloc_size build")
        require(invoke([*run, binary]), f"{cc} alloc_size constant and dynamic execution")
        require(invoke([*options, boundary, provider, "-o", binary]), f"{cc} alloc_size bounds checking")
        require(invoke([*run, binary]), f"{cc} allow an in-bounds checked operation")
        result = invoke([*run, binary, "excess"])
        require(result, f"{cc} reject a checked operation larger than the allocation", success=False)
        assert result.returncode == -signal.SIGABRT, result

    flags = [f"--target={target}"] if target else []
    source = """
#include <stddef.h>
char *allocate(size_t) __attribute__((alloc_size(1)));
void diagnose(void) {
    char *p = allocate(4);
    __builtin___memset_chk(p, 0, 5, __builtin_object_size(p, 0));
}
"""
    result = invoke([mcc, *flags, "-std=c11", "-fsyntax-only", "-"], source)
    require(result, "diagnose a constant checked allocation overrun")
    assert "requires 5 bytes in a destination with 4 bytes remaining" in result.stderr, result.stderr
    for invalid in ("alloc_size", "alloc_size()", "alloc_size(1, 2, 3)"):
        code = f"void *alloc(unsigned) __attribute__(({invalid}));\n"
        require(invoke([mcc, *flags, "-std=c11", "-fsyntax-only", "-"], code),
                f"reject {invalid}", success=False)
    for prototype, attr in (("void *f(unsigned)", "alloc_size(0)"),
                            ("void *f(unsigned)", "alloc_size(2)"),
                            ("void *f(double)", "alloc_size(1)"),
                            ("int f(unsigned)", "alloc_size(1)")):
        result = invoke([mcc, *flags, "-std=c11", "-fsyntax-only", "-"],
                        f"{prototype} __attribute__(({attr}));\n")
        require(result, f"ignore inapplicable {attr} on {prototype}")
        assert "warning:" in result.stderr and "alloc_size" in result.stderr, result.stderr


def library_allocation_sizes(mcc, gcc, root, target, runner):
    source = root / "library-allocation-size.c"
    source.write_text("""
#include <stdlib.h>
#if !__has_builtin(__builtin_malloc) || !__has_builtin(__builtin_calloc) || \
    !__has_builtin(__builtin_realloc) || !__has_builtin(__builtin_aligned_alloc) || \
    !__has_builtin(__builtin_free)
#error allocator builtin queries
#endif
#define BOS __builtin_object_size
#define DOS __builtin_dynamic_object_size
static __attribute__((noinline)) int check(size_t n) {
    char *constant = malloc(19);
    char *constant_copy = constant;
    if (BOS(constant_copy, 0) != 19 || BOS(constant_copy + 4, 3) != 15)
        return 1;
    free(constant_copy);

    char *p = __builtin_malloc(n++);
    char *q = (char *)(void *)p;
    p = __builtin_calloc(n, 2);
    if (DOS(q, 0) != n - 1 || DOS(p + 1, 2) != n * 2 - 1)
        return 2;
    __builtin_free(q);

    char *replaced = __builtin_realloc(p, n + 4);
    if (!replaced || DOS(replaced, 0) != n + 4)
        return 3;
    __builtin_free(replaced);

    void *aligned = aligned_alloc(16, 32);
    void *explicit_aligned = __builtin_aligned_alloc(16, 48);
    if (BOS(aligned, 1) != 32 || BOS(explicit_aligned, 0) != 48)
        return 4;
    free(aligned);
    __builtin_free(explicit_aligned);

    char *stack = __builtin_alloca(n++);
    char *stack_copy = stack;
    if (DOS(stack_copy + 1, 1) != n - 2)
        return 5;

    char *first, *second;
    first = second = __builtin_malloc(n);
    second = first;
    if (DOS(second + 1, 2) != n - 1)
        return 6;
    __builtin_free(second);
    return 0;
}
int main(int argc, char **argv) { (void)argv; return check((size_t)argc + 4); }
""")
    options_source = root / "allocator-builtin-options.c"
    options_source.write_text("""
#include <stddef.h>
#if __STDC_HOSTED__ != EXPECT_HOSTED
#error unexpected compilation environment
#endif
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void *annotated(size_t) __asm__("malloc") __attribute__((alloc_size(1)));
int main(void) {
    void *normal = malloc(17);
    void *zeroed = calloc(3, 8);
    void *resized = realloc(0, 23);
    void *explicit_builtin = __builtin_malloc(21);
    void *explicit_attr = annotated(29);
    int ok = __builtin_object_size(normal, 0) == (size_t)EXPECT_MALLOC &&
             __builtin_object_size(zeroed, 0) == (size_t)EXPECT_CALLOC &&
             __builtin_object_size(resized, 0) == (size_t)EXPECT_REALLOC &&
             __builtin_object_size(explicit_builtin, 0) == 21 &&
             __builtin_object_size(explicit_attr, 0) == 29;
    free(normal);
    free(zeroed);
    free(resized);
    free(explicit_builtin);
    free(explicit_attr);
    return !ok;
}
""")
    binary = root / "library-allocation-size"
    for cc in (mcc, gcc):
        flags = ["-O2"] if cc == gcc else [f"--target={target}"] if target else []
        prefix = [cc, *flags, "-std=c11", "-pedantic-errors"]
        run = runner if cc == mcc else []
        require(invoke([*prefix, source, "-o", binary]), f"{cc} builtin allocator sizes")
        require(invoke([*run, binary]), f"{cc} builtin allocations and copied extents")
        for modes, hosted, size, count, resized in (
            ([], 1, 17, 24, 23),
            (["-fno-builtin-malloc"], 1, -1, 24, 23),
            (["-fno-builtin"], 1, -1, -1, -1),
            (["-ffreestanding"], 0, -1, -1, -1),
            (["-ffreestanding", "-fbuiltin"], 0, 17, 24, 23),
            (["-ffreestanding", "-fhosted"], 1, 17, 24, 23),
        ):
            require(invoke([*prefix, *modes, f"-DEXPECT_HOSTED={hosted}", f"-DEXPECT_MALLOC={size}",
                            f"-DEXPECT_CALLOC={count}", f"-DEXPECT_REALLOC={resized}",
                            options_source, "-o", binary]), f"{cc} allocator options {modes}")
            require(invoke([*run, binary]), f"{cc} honor allocator options {modes}")
        for invalid in ("-fno-builtin-", "-fno-builtin-__builtin_malloc"):
            require(invoke([*prefix, invalid, "-fsyntax-only", source]), f"{cc} reject {invalid}", success=False)


def fortified_calls(mcc, gcc, root, target, runner):
    source = r"""
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#if !__has_builtin(__builtin___memcpy_chk) || !__has_builtin(__builtin___vsnprintf_chk)
#error missing fortified calls
#endif
static int emit(char *output, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = __builtin___vsnprintf_chk(output, size, 1, size, fmt, ap);
    va_end(ap);
    return count;
}
int main(void) {
    char output[32];
    size_t capacity = __builtin_object_size(output, 0);
    char *where = __builtin___mempcpy_chk(output, "abcd", 5, capacity);
    if (where != output + 5 || strcmp(output, "abcd"))
        return 1;
    __builtin___memmove_chk(output + 1, output, 5, capacity - 1);
    if (strcmp(output + 1, "abcd") || __builtin___memset_chk(output, 'x', 2, capacity) != output)
        return 2;
    if (__builtin___stpcpy_chk(output, "hello", capacity) != output + 5)
        return 3;
    __builtin___strncat_chk(output, "!!ignored", 2, capacity);
    if (strcmp(output, "hello!!"))
        return 4;
    if (__builtin___snprintf_chk(output, capacity, 1, capacity, "%s:%d", "ready", 42) != 8 ||
        strcmp(output, "ready:42"))
        return 5;
    if (emit(output, capacity, "%g/%ld", 1.5, 7L) != 5 || strcmp(output, "1.5/7"))
        return 6;
    return __builtin___fprintf_chk(stdout, 1, "%s %d\n", "ok", 42) != 6;
}
"""
    failure = """
#include <stddef.h>
int main(void) {
    char physical[16];
    volatile size_t count = 5;
    __builtin___memcpy_chk(physical, "123456", count, 4);
    return physical[0] != '1';
}
"""
    binary = root / "fortified"
    for cc in (mcc, gcc):
        flags = ["-x", "c", "-O2"] if cc == gcc else [f"--target={target}"] if target else []
        prefix = [] if cc == gcc else runner
        command = [cc, *flags, "-std=gnu11", "-pedantic-errors", "-", "-o", binary]
        require(invoke(command, source), f"{cc} fortified calls")
        result = invoke([*prefix, binary])
        require(result, f"{cc} fortified calls execution")
        assert result.stdout == "ok 42\n", result.stdout
        require(invoke(command, failure), f"{cc} fortified rejection build")
        result = subprocess.run([*prefix, str(binary)], cwd=root, capture_output=True, timeout=15)
        assert result.returncode == -signal.SIGABRT, (result.returncode, result.stderr)
    diagnostic = 'void f(void) { char a[4]; __builtin___strcpy_chk(a, "abcd", sizeof a); }'
    result = invoke([mcc, "-std=gnu11", "-fsyntax-only", "-"], diagnostic)
    require(result, "detected fortified overflow")
    assert "requires 5 bytes in a destination with 4 bytes remaining" in result.stderr, result.stderr


def variably_modified_arrays(mcc, gcc, root, target, runner):
    source = root / "variably-modified-arrays.c"
    source.write_text("""
#include <stddef.h>

static int outer_count, inner_count;
static int outer(int n) { ++outer_count; return n; }
static int inner(int n) { ++inner_count; return n; }

static int check(int [*][*], int, int);
static int check(int cols; int rows; int a[const static outer(rows)][inner(cols)], int rows, int cols)
{
    int (*p)[cols] = a;
    int (*const end)[cols] = p + rows;
    cols = rows = 1;
    if (sizeof *p != 4 * sizeof(int) || end - p != 3 || a[2][3] != 73)
        return 1;
    if (sizeof *p++ != 4 * sizeof(int) || p != a + 1)
        return 2;
    return 0;
}

static int standard_parameters(int rows, int cols, const int (*a)[cols])
{
    return rows == 3 && sizeof *a == 4 * sizeof(int) && a[2][3] == 73;
}

static int retained(const int count)
{
    int n = count;
    typedef int Row[inner(n)];
    Row a[2], b[3];
    const Row *p = &a[1];
    static Row *empty;
    n = 1;
    a[1][count - 1] = 29;
    return empty == 0 && sizeof (Row) == count * sizeof(int) &&
           sizeof(a) == 2 * sizeof(Row) && sizeof(b) == 3 * sizeof(Row) &&
           sizeof *p == sizeof(Row) && (*p)[count - 1] == 29;
}

static int expression_bounds(void)
{
    int n = 2;
    int values[3][n];
    int (*p)[n] = values;
    __typeof__(*p++) row;
    if (p != values + 1 || sizeof row != 2 * sizeof(int))
        return 0;
    if (sizeof(__typeof__(*p++)) != 2 * sizeof(int) || p != values + 2)
        return 0;
    if (_Alignof(__typeof__(*p++)) != _Alignof(int) || p != values + 2)
        return 0;
    typedef __typeof__(*p++) Row;
    Row copy;
    if (sizeof copy != 2 * sizeof(int) || p != values + 3)
        return 0;
    __typeof__(int[++n]) a, b;
    if (n != 3 || sizeof a != sizeof b || sizeof a != 3 * sizeof(int))
        return 0;
    if (sizeof((int (*)[++n])values) != sizeof p || n != 3)
        return 0;
    if (sizeof *((int (*)[++n])values) != 4 * sizeof(int) || n != 4)
        return 0;
    return 1;
}

int main(void)
{
    int rows = 3, cols = 4;
    int a[outer(rows)][inner(cols)];
    a[2][3] = 73;
    if (outer_count != 1 || inner_count != 1 || sizeof a != 12 * sizeof(int))
        return 1;
    if (sizeof(int[rows][cols]) != sizeof a || !standard_parameters(rows, cols, a))
        return 2;
    if (check(a, rows, cols) || outer_count != 2 || inner_count != 2)
        return 3;
    if (!retained(5) || inner_count != 3)
        return 4;
    if (!expression_bounds())
        return 5;
    return 0;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "variably-modified-arrays"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]),
                f"{cc} compile variably modified arrays")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run variably modified arrays")

    strict = root / "c11-variable-array-parameters.c"
    strict.write_text("""
static int rows(void *storage, int n, int a[static 1][sizeof(*(int (*)[n++])storage) / sizeof(int)])
{
    return n == 5 && sizeof *a == 4 * sizeof(int) && a[1][3] == 11;
}
int main(void)
{
    int table[2][4] = {{0}, {0, 0, 0, 11}};
    return !rows(table, 4, table);
}
""")
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=c11", "-pedantic-errors", strict, "-o", binary]),
                f"{cc} compile C11 array bounds in parameter scope")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run C11 array bounds in parameter scope")

    invalid = (
        ("int f(int n; int a[n]);", "-std=gnu11"),
        ("int f(int n; int a[n], long n);", "-std=gnu11"),
        ("int f(int n; int a[n], const int n);", "-std=gnu11"),
        ("int f(int n; int a[n], int n);", "-std=c11"),
        ("int f(int a[*]) { return *a; }", "-std=gnu11"),
        ("void f(int n) { static int a[2][n]; }", "-std=gnu11"),
        ("void f(int n) { typedef int A[n]; typedef A A; }", "-std=c11"),
        ("void f(int n) { goto in; typedef int A[n]; in: ; }", "-std=gnu11"),
        ("void f(int n) { goto in; int (*p)[n]; in: ; }", "-std=gnu11"),
        ("void f(int n) { switch (n) { int (*p)[n]; case 1: ; } }", "-std=gnu11"),
    )
    for src, mode in invalid:
        for cc in (mcc, gcc):
            prefix = [cc, *flags] if cc == mcc else [cc, "-x", "c"]
            require(invoke([*prefix, mode, "-pedantic-errors", "-S", "-o", os.devnull, "-"], src),
                    f"{cc} reject {src}", success=False)


def variable_length_members(mcc, gcc, root, target, runner):
    source = root / "variable-length-members.c"
    source.write_text("""
#include <stddef.h>
#include <stdarg.h>

static int layout(int n)
{
    int evaluated = 0;
    struct Item {
        char bytes[n + (++evaluated)];
        unsigned first:2, second:31, third:3;
        short tail;
    };
    union Storage { struct Item item; _Alignas(16) char raw[17]; };
    struct Pair { union Storage storage; int last; };
    int length = n + 1;
    size_t tail = ((size_t)length + 6) & ~(size_t)1;
    size_t item = (tail + 5) & ~(size_t)3;
    size_t storage = ((item > 17 ? item : 17) + 15) & ~(size_t)15;
    n = 1;
    struct Pair p[2] = {};
    if (evaluated != 1 || offsetof(struct Item, tail) != tail || sizeof(struct Item) != item ||
        offsetof(struct Pair, last) != storage || sizeof p != 2 * (storage + 16))
        return 1;
    p[1].storage.item.first = 3;
    p[1].storage.item.second = 0x72456289;
    p[1].storage.item.third = 6;
    p[1].storage.item.tail = 17;
    int index = length - 1;
    p[1].storage.item.bytes[index] = 91;
    p[1].last = 29;
    p[0] = p[1];
    if (offsetof(struct Pair, storage.item.bytes[index]) != (size_t)index ||
        p[0].storage.item.first != 3 || p[0].storage.item.second != 0x72456289 ||
        p[0].storage.item.third != 6 || p[0].storage.item.tail != 17 ||
        p[0].storage.item.bytes[index] != 91 || p[0].last != 29)
        return 2;
    return 0;
}

static int local_types(int n)
{
    int initial = n;
    size_t alignment = _Alignof(struct Alignment { int x[n++]; });
    if (alignment != _Alignof(int) || n != initial)
        return 1;
    size_t bytes = sizeof(struct Alignment);
    if (n != initial + 1 || bytes != initial * sizeof(int))
        return 1;
    int backing[16];
    size_t pointer_size = sizeof((struct Discarded { int a[n++]; } *)backing);
    if (pointer_size != sizeof(void *) || n != initial + 1)
        return 2;
    bytes = sizeof(struct Discarded);
    if (n != initial + 2 || bytes != (initial + 1) * sizeof(int))
        return 2;
    struct Descriptor { int (*data)[n++]; } descriptor;
    int values[3][initial + 2] = {};
    descriptor.data = values;
    descriptor.data[2][initial] = 29;
    if (n != initial + 3 || sizeof *descriptor.data != (initial + 2) * sizeof(int) ||
        values[2][initial] != 29)
        return 2;
    struct { char prefix; struct Alignment member; long suffix; } anonymous = {};
    anonymous.member.x[initial - 1] = 19;
    anonymous.suffix = 3;
    if (anonymous.member.x[initial - 1] + anonymous.suffix != 22)
        return 3;
    int before = n;
    struct Result { int items[n++]; short result; } make(void) {
        struct Result value = {};
        value.result = 17;
        return value;
    }
    if (n != before + 1 || make().result != 17)
        return 4;
    return 0;
}

static int passing(int n, int iterations)
{
    struct Payload { int elements[n]; long serial; };
    struct Payload bump(struct Payload x, int delta) {
        x.serial += delta;
        x.elements[n - 1] += delta * 2;
        return x;
    }
    long spill(int a, int b, int c, int d, int e, int f, int g, int h, struct Payload x) {
        return a + b + c + d + e + f + g + h + x.serial;
    }
    long varied(int count, ...) {
        va_list ap;
        va_start(ap, count);
        struct Payload x;
        x = va_arg(ap, struct Payload);
        long other = va_arg(ap, long);
        va_end(ap);
        return x.serial + x.elements[n - 1] + other;
    }
    struct Payload in = {}, out;
#if defined(__MCC__) || __GNUC__ >= 16
    struct Payload *zero = &(struct Payload){};
#else
    struct Payload zero_value = {};
    struct Payload *zero = &zero_value;
#endif
    in.serial = 5;
    in.elements[n - 1] = 7;
    for (int i = 0; i < iterations; i++) {
        out = bump(in, i);
        if (out.serial != 5L + i || out.elements[n - 1] != 7 + 2 * i)
            return 1;
    }
    long sum = spill(1, 2, 3, 4, 5, 6, 7, 8, in);
    if (sum != 41 || varied(2, in, 63L) != 75 || in.serial != 5)
        return 2;
    out = ({ struct Payload block; block = in; block.serial += 13; block; });
    if (out.serial != 18 || zero->serial || zero->elements[n - 1])
        return 3;
    return 0;
}

int main(void)
{
    for (int n = 1; n < 18; n++)
        if (layout(n))
            return 1;
    if (local_types(5))
        return 2;
    if (passing(2500, 1000))
        return 3;
    return 0;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "variable-length-members"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]),
                f"{cc} compile variable-length members")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run variable-length members")

    invalid = (
        ("void f(int n) { struct S { int v[n]; }; }", "-std=c11", "-pedantic-errors"),
        ("int n; struct S { char v[n]; };", "-std=gnu11"),
        ("void f(int n) { struct S { int v[n]; }; static struct S s; }", "-std=gnu11"),
        ("void f(int n) { struct S { int v[n]; }; extern struct S s; }", "-std=gnu11"),
        ("void f(int n) { struct S { int v[n]; }; struct S s = {{0}}; }", "-std=gnu11"),
    )
    for src, *options in invalid:
        for cc in (mcc, gcc):
            prefix = [cc, *flags] if cc == mcc else [cc, "-x", "c"]
            require(invoke([*prefix, *options, "-S", "-o", os.devnull, "-"], src),
                    f"{cc} reject {src}", success=False)


def stack_allocations(mcc, gcc, root, target, runner):
    source = r"""
#include <alloca.h>
#include <stddef.h>
#include <stdint.h>
#if !__has_builtin(__builtin_alloca) || !__has_builtin(__builtin_alloca_with_align_and_max)
#error missing stack allocators
#endif
static int pinned(unsigned n) {
    unsigned char *saved[8];
    for (unsigned i = 0; i < 8; ++i) {
        volatile unsigned char vla[n + i];
        vla[n + i - 1] = (unsigned char)i;
        saved[i] = alloca(n + i);
        saved[i][0] = (unsigned char)(vla[n + i - 1] + 16);
        saved[i][n + i - 1] = (unsigned char)(i + 64);
    }
    for (unsigned i = 0; i < 8; ++i) {
        if ((uintptr_t)saved[i] % __BIGGEST_ALIGNMENT__ ||
            saved[i][0] != i + 16 || saved[i][n + i - 1] != i + 64)
            return 1;
    }
    return 0;
}
static int aligned(unsigned n, unsigned loops) {
    unsigned visits = 0;
again:
    if (visits % 2)
        goto inside;
    {
inside: ;
        volatile unsigned char *p = __builtin_alloca_with_align_and_max(n, 4096, 8);
        if ((uintptr_t)p % 512)
            return 2;
        p[0] = 40;
        p[n - 1] = 2;
        if (p[0] + p[n - 1] != 42)
            return 3;
        if (++visits < loops)
            goto again;
    }
    return visits != loops;
}
static int cases(int choice) {
    int result = 0;
    switch (choice) {
        case 0: result = 10;
        case 1: ;
            volatile unsigned char *p = __builtin_alloca_with_align(545, 512);
            if ((uintptr_t)p % 64)
                return 4;
            p[0] = 5;
            result += p[0];
            break;
        default: return 5;
    }
    return result != (choice ? 5 : 15);
}
static int expr(unsigned n) {
    return ({
        volatile unsigned char *p = __builtin_alloca_with_align(n, 256);
        p[n - 1] = 31;
        p[n - 1];
    }) != 31;
}
static int transfers(unsigned n) {
    unsigned jumps = 0;
    void *to = &&inside;
    if (jumps == 0)
        goto *to;
    {
inside: ;
        volatile unsigned char vla[n];
        volatile unsigned char *p = __builtin_alloca_with_align(n, 256);
        p[0] = vla[0] = 17;
        jumps += p[0];
        to = &&outside;
        goto *to;
    }
outside:
    return jumps != 17;
}
static int overwrite(unsigned n) {
    volatile unsigned char *scratch = __builtin_alloca(n);
    for (unsigned i = 0; i < n; i++)
        scratch[i] = 0;
    return scratch[n - 1];
}
static int nonlocal(unsigned n) {
    __label__ done;
    volatile unsigned char *saved;
    void go(void) { goto done; }
    {
        volatile unsigned char vla[n];
        vla[0] = 53;
        saved = __builtin_alloca(n);
        saved[0] = vla[0];
        saved[n - 1] = 9;
        go();
    }
done:
    return overwrite(2 * n) || saved[0] != 53 || saved[n - 1] != 9;
}
int main(void) {
    if (pinned(120) || aligned(1311, 3) || cases(0) || cases(1) || expr(154) || transfers(65))
        return 1;
#ifdef __MCC__
    if (aligned(33000, 600))
        return 2;
#endif
    if (nonlocal(191))
        return 3;
    return 0;
}
"""
    binary = root / "stack-allocations"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        require(invoke([cc, *flags, "-std=gnu11", "-O0", "-", "-o", binary], source),
                f"{cc} stack allocation build")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} stack allocation execution")
    for source in (
        "void *p = __builtin_alloca(16);",
        "void f(unsigned bits) { (void)__builtin_alloca_with_align(7, bits); }",
        "void f(void) { (void)__builtin_alloca_with_align(7, 24); }",
        "void f(void) { (void)__builtin_alloca_with_align(7, 4294967296ULL); }",
        "void f(unsigned max) { (void)__builtin_alloca_with_align_and_max(7, 128, max); }",
        "void f(void) { (void)__builtin_alloca(); }",
        "void f(int n) { goto bad; { int a[n]; bad: (void)a; } }",
    ):
        require(invoke([mcc, "-std=gnu11", "-fsyntax-only", "-"], source),
                source, success=False)


def enum_types(mcc, gcc, root, target, runner):
    source = r"""
#include <limits.h>
#include <stddef.h>
#define COMPATIBLE(a, b) __builtin_types_compatible_p(a, b)
__extension__ enum wire : unsigned char;
struct packet { enum wire kind; char payload[3]; };
static enum wire before;
_Static_assert(sizeof before == 1 && sizeof(struct packet) == 4, "opaque type is complete");
__extension__ enum wire : unsigned char { idle, full = 255, width = sizeof(enum wire) };
__extension__ enum wire : unsigned char;
_Static_assert(sizeof full == 1 && width == 1, "fixed enumerator type");
_Static_assert(COMPATIBLE(enum wire, unsigned char) && COMPATIBLE(__typeof__(full), enum wire),
               "underlying type compatibility");
typedef unsigned short word;
__extension__ enum : const word { anonymous = 65535 } token;
__extension__ enum signed_code : signed char { minimum = -128, next, maximum = 127 };
__extension__ enum qualified_code : unsigned const short { qualified_max = 65535 };
__extension__ enum flag : _Bool { clear, set };
_Static_assert(sizeof token == 2 && anonymous == 65535 && sizeof set == 1, "fixed types");
_Static_assert(next == -127 && (enum signed_code)maximum == 127, "signed range");
_Static_assert(sizeof(enum qualified_code) == 2, "interleaved enum qualifiers");
struct bits { enum wire : 3; enum wire value : 5; };
__extension__ enum sequence {
    start = INT_MAX, over, after,
    provisional_long = COMPATIBLE(__typeof__(over), long)
};
__extension__ enum huge { small = 0, large = ULONG_MAX };
__extension__ enum mixed { negative = -2147483649L, positive = 2147483648UL };
enum nested { first = 2, inner = sizeof(enum { child = 41 }), last };
_Static_assert(over == 2147483648U && after == 2147483649U, "wider implicit enumerators");
_Static_assert(provisional_long == 1, "implicit enumerator type during the definition");
_Static_assert(COMPATIBLE(__typeof__(start), unsigned int), "wide enum constants");
_Static_assert(COMPATIBLE(__typeof__(small), unsigned long) && large == ULONG_MAX, "unsigned range");
_Static_assert(COMPATIBLE(__typeof__(positive), long) && negative == -2147483649L, "mixed sign range");
_Static_assert(last == (EXPECT_LAYOUT == 2 ? 1 : sizeof(int)) + 1 && child == 41,
               "nested enum state");
enum __attribute__((packed)) packed_s { low = -129, high = 127 };
enum packed_u { cap = 255 } __attribute__((packed));
__extension__ enum __attribute__((packed)) fixed_wide : unsigned long { big = 1 };
_Static_assert(sizeof(enum packed_s) == 2 && sizeof(enum packed_u) == 1, "packed enum sizes");
_Static_assert(sizeof(enum fixed_wide) == sizeof(unsigned long), "fixed size wins");
enum layout { layout_min = 0, layout_max = 256 };
_Static_assert(sizeof(enum layout) == EXPECT_LAYOUT, "short-enum option");
static enum wire exchange(enum wire x) {
    enum wire previous = before;
    before = x;
    return previous;
}
static int classify(enum huge value) {
    switch (value) {
    case large: return 8;
    case small: return 2;
    default: return -1;
    }
}
static int shadow(void) {
    __extension__ enum wire : signed long;
    enum wire local;
    __extension__ enum wire : signed long { local_value = -55 };
    local = local_value;
    return sizeof local != sizeof(long) || local != -55;
}
int main(void) {
    struct bits bits = {0};
    bits.value = 15;
    return exchange(full) != idle || exchange(width) != full || bits.value != 15 ||
           shadow() || classify(large) != 8 || classify(small) != 2;
}
"""
    binary = root / "enum-types"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        for opts, size in [([], 4), (["-fshort-enums"], 2),
                           (["-fshort-enums", "-fno-short-enums"], 4)]:
            require(invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", *opts,
                            f"-DEXPECT_LAYOUT={size}", "-", "-o", binary], source),
                    f"{cc} enum representation {opts}")
            require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} enum execution {opts}")
    invalid = (
        "enum a : unsigned char; enum a : signed char;",
        "enum a : unsigned char; enum a { first };",
        "enum a; enum a : unsigned char { first };",
        "enum a : unsigned char { first }; enum a : unsigned char { second };",
        "enum a : unsigned char value;",
        "enum a : int { first = 2147483648UL };",
        "enum a : unsigned char { first = 255, overflow };",
        "enum a : long { first = 9223372036854775807L, overflow };",
        "enum a : _Bool { first = 2 };",
        "enum a : double { first };",
        "enum a { first }; enum b : enum a { second };",
    )
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else []
        for source in invalid:
            require(invoke([cc, *flags, "-std=gnu11", "-fsyntax-only", "-"], source),
                    f"{cc} {source}", success=False)
        require(invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", "-fsyntax-only", "-"],
                       "enum no_extension : int { e };"),
                f"{cc} fixed enum pedantic diagnostic", success=False)
        require(invoke([cc, *flags, "-std=gnu11", "-fsyntax-only", "-"], """
struct holder { enum nested_tag : unsigned char; int count; };
enum nested_tag : unsigned char { tag_value = 17 };
_Static_assert(sizeof(struct holder) == sizeof(int), "tag-only member declaration");
"""), f"{cc} tag declaration in a structure")
    if target is None:
        enum_abi(mcc, gcc, root)


def enum_abi(mcc, gcc, root):
    common = """
__extension__ enum wire_code : unsigned short;
__extension__ enum sample : signed long { low = -0x123456789L, high = 0x23456789aL };
enum __attribute__((packed)) state { off, on = 240 };
struct record { enum wire_code code; enum state state; enum sample value; };
extern struct record step(struct record, enum wire_code, enum sample, enum state);
__extension__ enum wire_code : unsigned short { initial = 63, changed = 65534 };
"""
    callee = common + """
struct record step(struct record item, enum wire_code code, enum sample value, enum state state) {
    if (item.code == initial && item.state == off && item.value == low) {
        item.code = code;
        item.state = state;
        item.value = value;
    }
    return item;
}
"""
    caller = common + """
int main(void) {
    struct record item = {initial, off, low};
    item = step(item, changed, high, on);
    return item.code != changed || item.state != on || item.value != high;
}
"""
    obj1, obj2 = root / "enum-callee.o", root / "enum-caller.o"
    binary = root / "enum-abi"
    for producer, consumer in ((mcc, gcc), (gcc, mcc)):
        for cc, code, obj in ((producer, callee, obj1), (consumer, caller, obj2)):
            flags = ["-x", "c"] if cc == gcc else []
            require(invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", "-c", "-", "-o", obj],
                           code), f"{cc} enum ABI object")
        require(invoke([mcc, obj1, obj2, "-o", binary]), "mixed enum ABI link")
        require(invoke([binary]), "mixed enum ABI execution")


def wide_integers(mcc, gcc, root, target, runner):
    source = r"""
#include <stdarg.h>
#include <stdatomic.h>
__extension__ typedef unsigned __int128 uint128;
__extension__ typedef __int128 int128;
#if __SIZEOF_INT128__ != 16
#error wrong int128 feature macro
#endif
#define ONE ((uint128)1)
#define WIDE ((ONE << 100) + 91)
#define LIMIT ((ONE << 127) - 1)
_Static_assert(sizeof(uint128) == 16 && _Alignof(int128) == 16, "integer layout");
_Static_assert(__builtin_types_compatible_p(__uint128_t, uint128) &&
               __builtin_types_compatible_p(__int128_t, int128), "builtin typedefs");
_Static_assert((ONE << 127) && (_Bool)(ONE << 100), "full width truth");
_Static_assert(__builtin_bswap128(ONE) == ONE << 120, "compile-time byte swap");
_Static_assert(!__builtin_mul_overflow_p(ONE << 100, (uint128)4, (uint128)0), "exact wide product");
_Static_assert(__builtin_add_overflow_p(LIMIT, 1, (int128)0), "signed upper limit");
__extension__ enum wide_enum : uint128 { enumerator = WIDE, following };
__extension__ enum wide_inferred { negative = -2, beyond64 = 18446744073709551615UL, inferred_next };
_Static_assert(sizeof(enum wide_inferred) == 16 && inferred_next == ONE << 64, "wide inferred enum");
_Static_assert(sizeof(following) == 16 && following == WIDE + 1, "fixed wide enum");
static uint128 initial = (WIDE * 37) | (ONE << 120);
static int128 negative_float = (int128)-0x1p113L;
static double rounded = (double)((ONE << 90) + (ONE << 37) + 1);
static _Atomic(uint128) atomic;
struct fields { uint128 prefix : 17; int128 middle : 82; uint128 tail : 29; };
static int classify(uint128 value) {
    switch (value) {
    case WIDE: return 10;
    case WIDE + 1: return 20;
    default: return 30;
    }
}
static uint128 arithmetic(uint128 a, uint128 b) {
    return (a / b) * b + a % b;
}
static uint128 stack_args(int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint128 first = va_arg(ap, uint128);
    for (int i = 0; i < n; i++)
        if (va_arg(ap, int) != i)
            return 0;
    uint128 second = va_arg(ap, uint128);
    va_end(ap);
    return first ^ second;
}
static double to_double(uint128 x) { return (double)x; }
static float to_single(uint128 x) { return (float)x; }
static long double to_extended(int128 x) { return (long double)x; }
static int128 from_double(double x) { return (int128)x; }
static int128 from_extended(long double x) { return (int128)x; }
int main(void) {
    if (initial != ((WIDE * 37) | (ONE << 120)) || !initial || !(ONE << 120))
        return 1;
    if (arithmetic(initial, WIDE - 10) != initial || arithmetic(initial, 65537) != initial)
        return 2;
    int128 signed_value = -(int128)WIDE;
    if (signed_value / 7 != -(int128)(WIDE / 7) || signed_value % 7 != -(int128)(WIDE % 7) ||
        (signed_value >> 111) != -1 || ((uint128)signed_value >> 127) != 1)
        return 3;
    if (stack_args(7, WIDE, 0, 1, 2, 3, 4, 5, 6, initial) != (WIDE ^ initial))
        return 4;
    if (classify(WIDE) != 10 || classify(WIDE + 1) != 20 || classify(WIDE + 2) != 30)
        return 5;
    struct fields bits = {0};
    bits.prefix = 32769;
    bits.middle = -((int128)1 << 78);
    bits.tail = 31337;
    if (bits.prefix != 32769 || bits.middle != -((int128)1 << 78) || bits.tail != 31337)
        return 6;
    uint128 overflow;
    if (!__builtin_mul_overflow((uint128)-1, WIDE, &overflow) || overflow != -WIDE ||
        __builtin_add_overflow((int128)-31, (uint128)9, &signed_value) || signed_value != -22)
        return 7;
    unsigned narrow;
    if (!__builtin_add_overflow(WIDE, -1, &narrow) || narrow != 90)
        return 8;
    atomic_init(&atomic, WIDE);
    if (atomic_fetch_add_explicit(&atomic, ONE << 80, memory_order_relaxed) != WIDE ||
        atomic_load(&atomic) != WIDE + (ONE << 80))
        return 9;
    if (rounded != 0x1.0000000000001p90 ||
        to_double((ONE << 90) + (ONE << 37) + 1) != rounded ||
        to_single((ONE << 90) + (ONE << 66) + 1) != 0x1.000002p90f)
        return 10;
    if (negative_float != -(int128)(ONE << 113) ||
        to_extended(negative_float) != -0x1p113L ||
        from_extended(-0x1p113L) != negative_float ||
        from_double(-0x1p88) != -(int128)(ONE << 88) ||
        from_double(-42.75) != -42)
        return 11;
    if (__builtin_bswap128(__builtin_bswap128(initial)) != initial)
        return 12;
    return 0;
}
"""
    binary = root / "wide-integers"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        libraries = ["-latomic"] if cc == gcc else []
        require(invoke([cc, *flags, "-std=gnu11", "-pedantic-errors", "-", *libraries,
                        "-o", binary], source), f"{cc} int128 build")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} int128 execution")
        if cc == mcc:
            dynamic = invoke(["readelf", "-dW", binary])
            require(dynamic, "int128 dynamic dependencies")
            assert "libgcc_s.so" not in dynamic.stdout and "libatomic.so" not in dynamic.stdout
    if target is None:
        wide_integer_abi(mcc, gcc, root)


def wide_integer_abi(mcc, gcc, root):
    common = """
#include <stdarg.h>
__extension__ typedef unsigned __int128 u128;
__extension__ typedef __int128 i128;
#define A (((u128)1 << 105) + 8701)
#define B (((u128)1 << 71) + 13)
u128 abi_divide(unsigned, u128, int, long, int, int, u128, unsigned);
i128 abi_signed(i128, int, i128);
u128 abi_args(int, ...);
"""
    producer = common + """
u128 abi_divide(unsigned marker, u128 a, int p, long q, int r, int s, u128 b, unsigned end) {
    return marker == 1 && p == 2 && q == 3 && r == 4 && s == 5 && end == 6 ?
           a/b + a%b : 0;
}
i128 abi_signed(i128 a, int p, i128 b) {
    return a/b + a%b + p;
}
u128 abi_args(int count, ...) {
    va_list ap;
    va_start(ap, count);
    u128 first = va_arg(ap, u128);
    for (int i = 0; i < count; i++)
        if (va_arg(ap, int) != i)
            return 0;
    u128 second = va_arg(ap, u128);
    va_end(ap);
    return first ^ second;
}
"""
    consumer = common + """
int main(void) {
    return abi_divide(1,A,2,3,4,5,B,6) != A/B + A%B ||
           abi_signed(-(i128)A, 2, (i128)B) != 2 - (i128)(A/B + A%B) ||
           abi_args(7, A, 0,1,2,3,4,5,6,B) != (A^B);
}
"""
    binary = root / "wide-integer-mixed"
    lib, main = root / "wide-callee.o", root / "wide-caller.o"
    for library_cc, main_cc in ((mcc, gcc), (gcc, mcc)):
        for cc, text, obj in ((library_cc, producer, lib), (main_cc, consumer, main)):
            options = ["-x", "c"] if cc == gcc else []
            require(invoke([cc, *options, "-std=c11", "-pedantic-errors", "-c", "-", "-o", obj],
                           text), f"{cc} int128 ABI object")
        require(invoke([mcc, lib, main, "-o", binary]), "int128 ABI interoperation link")
        require(invoke([binary]), "int128 ABI interoperation execution")
        dynamic = invoke(["readelf", "-dW", binary])
        require(dynamic, "int128 mixed executable dependencies")
        assert "libgcc_s.so" not in dynamic.stdout


def extended_assembly(mcc, gcc, root, target, runner):
    source = r"""
#if defined(__x86_64__)
#define ADD "addq %[input], %[out]"
#define MOVE "movq %1, %0"
#define LOAD_MEMORY "movq %1, %0"
#define INC_MEMORY "addq $4, %0"
#define ADDFP "addsd %1, %0"
#define FP "x"
#define FLAGS "=@cce"
#define COMPARE "cmpq %2, %1"
#define BRANCH "cmpq %1, %0\nje %l3\naddq $5, %0"
#define TOUCH_SAVE "movq $123, %%rbx"
#define SAVED "rbx"
#define WRITE_BITS "movl $7, %0"
#define HARD_REGISTER "r12"
#define READ_HARD "movq %%r12, %0"
#define WRITE_HARD "movq $42, %%r12"
#define TIED_HARD "addq $5, %0"
#define VERIFY ".text\n.globl verify_save\nverify_save:\n" \
  "pushq %rbx\nmovq $29,%rbx\ncall touch_saved\ncmpq $29,%rbx\n" \
  "sete %al\nmovzbl %al,%eax\npopq %rbx\nret"
#else
#define ADD "add %x[out], %x[out], %x[input]"
#define MOVE "mov %x0, %x1"
#define LOAD_MEMORY "ldr %x0, %1"
#define INC_MEMORY "ldr x10, %0\nadd x10, x10, #4\nstr x10, %0"
#define ADDFP "fadd %d0, %d0, %d1"
#define FP "w"
#define FLAGS "=@cceq"
#define COMPARE "cmp %x1, %2"
#define BRANCH "cmp %x0, %x1\nb.eq %l3\nadd %x0, %x0, #5"
#define TOUCH_SAVE "mov x19, #123"
#define SAVED "x19"
#define WRITE_BITS "mov %w0, #7"
#define HARD_REGISTER "x20"
#define READ_HARD "mov %x0, x20"
#define WRITE_HARD "mov x20, #42"
#define TIED_HARD "add %x0, %x0, #5"
#define MOVE_WIDE_ZERO "mov %0, %x1"
#define VERIFY ".text\n.globl verify_save\nverify_save:\n" \
  "stp x29,x30,[sp,#-16]!\nmov x29,sp\nstp x19,xzr,[sp,#-16]!\n" \
  "mov x19,#29\nbl touch_saved\ncmp x19,#29\ncset w0,eq\n" \
  "ldp x19,xzr,[sp],#16\nldp x29,x30,[sp],#16\nret"
#endif
__asm__(VERIFY);
__asm__(".data\n.global asm_constant\n.balign 8\nasm_constant:\n.quad %c0\n.text"
        :: "i"(23 * 2));
extern long asm_constant;
int verify_save(void);
void touch_saved(void) {
    __asm__ volatile(TOUCH_SAVE ::: SAVED);
}
static long sum(long a, long b) {
    long value;
    __asm__ inline (ADD : [out] "=&r"(value) : "[out]"(a), [input] "r"(b) : "cc");
    return value;
}
static long transferred(long x) {
    long count = x;
    {
        volatile char vla[10 + x];
        vla[0] = 7;
        __asm__ goto (BRANCH : "+r"(count) : "r"(0L) : "cc", "memory" : zero);
        return count + vla[0];
    }
zero:
    return count - 10;
}
static long memory_operand(long *p) {
    long loaded;
    __asm__ volatile(LOAD_MEMORY : "=r"(loaded) : "m"(*p) : "memory");
#if defined(__x86_64__)
    __asm__ volatile(INC_MEMORY : "+m"(*p++) :: "cc", "memory");
#else
    __asm__ volatile(INC_MEMORY : "+m"(*p++) :: "x10", "memory");
#endif
    return loaded + p[-1];
}
static long register_values(long n) {
    register long bound __asm__(HARD_REGISTER) = n;
    long observed, advanced;
    __asm__(READ_HARD : "=&r"(observed) : "r"(bound));
    __asm__(TIED_HARD : "=r"(advanced) : "0"(bound));
    __asm__(WRITE_HARD : "=r"(bound));
    return observed + advanced + bound;
}
int main(void) {
    long copied;
    __asm__(MOVE : "=r"(copied) : "i"(7 + 2));
    if (copied != 9 || asm_constant != 46 || sum(19, 23) != 42 || !verify_save())
        return 1;
    long storage = 10;
    if (memory_operand(&storage) != 24 || storage != 14)
        return 2;
    if (transferred(0) != -10 || transferred(12) != 24)
        return 3;
    double fp = 2.5;
    __asm__(ADDFP : "+" FP(fp) : FP(1.75));
    unsigned condition;
    __asm__(COMPARE : FLAGS(condition) : "r"(12L), "i"(12));
    if (fp != 4.25 || condition != 1)
        return 4;
    struct { unsigned member : 3; } b = {0};
    __asm__(WRITE_BITS : "=r"(b.member));
    if (b.member != 7)
        return 5;
    if (register_values(123) != 123 + 128 + 42)
        return 7;
#if defined(__aarch64__)
    unsigned narrow;
    __asm__(MOVE_WIDE_ZERO : "=r"(narrow) : "i"(0));
    if (narrow != 0)
        return 8;
#endif
#if defined(__x86_64__)
    __asm__(MOVE : "=a,r"(copied) : "r,r"(storage) : "rax");
    __asm__("{addq $7,%0|add %0,7}" : "+r"(copied));
    if (copied != 21)
        return 6;
#endif
    __asm__ volatile(".Ltest%=:\nnop" ::: "memory");
    __asm__ volatile(".Ltest%=:\nnop" ::: "memory");
    return 0;
}
"""
    binary = root / "extended-assembly"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        require(invoke([cc, *flags, "-std=c11", "-pedantic-errors", "-", "-o", binary], source),
                f"{cc} extended asm compilation")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} extended asm execution")
    register = "rax" if (target or os.uname().machine).startswith("x86_64") else "x0"
    flags = [f"--target={target}"] if target else []
    for source in (
        'void f(void) { __asm__("" : "=r"(1)); }',
        'void f(int n) { __asm__("" : "r"(n)); }',
        'void f(int n) { __asm__("" : "=r"(n) : "i"(n)); }',
        'void f(int n) { __asm__("bad %4" :: "r"(n)); }',
        'void f(void) { __asm__("" ::: "invalid_register"); }',
        'void f(int n) { __asm__("" : [x] "=r"(n) : [x] "r"(n)); }',
        'void f(int n) { __asm__("" : "=r,r"(n) : "r"(n)); }',
        'void f(void) { __asm__ goto ("" :::: missing); }',
        'void f(int n) { __asm__ goto ("" :::: inside); { int v[n]; inside: (void)v; } }',
        'void f(void) { register int n __asm__("invalid_register"); }',
        f'void f(void) {{ register int n __asm__("{register}"); (void)&n; }}',
        f'void f(void) {{ register int n __asm__("{register}"); __asm__("" :: "m"(n)); }}',
    ):
        require(invoke([mcc, *flags, "-std=c11", "-fsyntax-only", "-"], source), source, success=False)


def global_register_variables(mcc, gcc, root, target, runner):
    bindings = """
#if defined(__x86_64__)
#define WORD_REGISTER "r12"
#define BYTE_REGISTER "r13"
#define POINTER_REGISTER "r14"
#define ADD_FOUR "addq $4, %0"
#define DOUBLE "movq %1, %0\\naddq %2, %0"
#define ADD_ONE "addq $1, %0"
#else
#define WORD_REGISTER "x19"
#define BYTE_REGISTER "x20"
#define POINTER_REGISTER "x21"
#define ADD_FOUR "add %0, %0, #4"
#define DOUBLE "add %0, %1, %2"
#define ADD_ONE "add %0, %0, #1"
#endif
long word;
__extension__ register long word __asm__(WORD_REGISTER);
"""
    main_source = bindings + """
extern long word;
__extension__ register signed char byte __asm__(BYTE_REGISTER);
__extension__ register long *ptr __asm__(POINTER_REGISTER);
#if defined(__aarch64__)
__extension__ register double fp __asm__("d8");
#endif
extern long alter(long);
extern long strtol(const char *, char **, int);
extern void _Exit(int);
static int check(void) {
    long array[] = {12, 34};
    word = 3;
    byte = -4;
    ptr = array;
#if defined(__aarch64__)
    fp = 1.75;
#endif
    long before = word++;
    long changed = alter(5);
    long parsed = strtol("17", (char **)0, 10);
    if (before != 3 || changed != 9 || parsed != 17 || word != 9)
        return 1;
    word *= 3;
    __asm__(ADD_FOUR : "+r"(word) : : "cc");
    long twice, next;
    __asm__(DOUBLE : "=&r"(twice) : "r"(word), "r"(word) : "cc");
    __asm__(ADD_ONE : "=r"(next) : "0"(word) : "cc");
    if (twice != 62 || next != 32 || word != 31)
        return 2;
    if (++byte != -3 || *ptr++ != 12 || *ptr != 34)
        return 3;
#if defined(__aarch64__)
    __asm__("fadd %d0, %d0, %d1" : "+w"(fp) : "w"(2.5));
    if (fp != 4.25)
        return 4;
#endif
    return 0;
}
int main(void) { _Exit(check()); }
"""
    primary = root / "global-register-main.c"
    secondary = root / "global-register-other.c"
    binary = root / "global-register"
    primary.write_text(main_source)
    secondary.write_text(bindings + "long alter(long n) { return word += n; }")
    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        require(invoke([cc, *([] if cc == gcc else flags), "-std=c11", "-pedantic-errors",
                        primary, secondary, "-o", binary]), f"{cc} global register compilation")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} global register execution")

    register = "r12" if (target or os.uname().machine).startswith("x86_64") else "x19"
    declaration = f'register long state __asm__("{register}")'
    for source, dialect in (
        ("register long state;", ["-std=gnu11"]),
        (declaration + ";", ["-std=c11", "-pedantic-errors"]),
        (declaration + " = 1;", ["-std=gnu11"]),
        (declaration + "; long *address(void) { return &state; }", ["-std=gnu11"]),
        ("extern long state; long *address(void) { return &state; } " + declaration + ";",
         ["-std=gnu11"]),
        (declaration + '; void f(void) { __asm__("" :: "m"(state)); }', ["-std=gnu11"]),
    ):
        require(invoke([mcc, *flags, *dialect, "-fsyntax-only", "-"], source), source, success=False)


def call_clobbered_global_registers(mcc, gcc, root, target, runner):
    declarations = r"""
#if defined(__x86_64__)
#define WORD_REGISTER "r10"
#define FRAME_REGISTER "r9"
#define FP_REGISTER "xmm10"
#define FP_CONSTRAINT "x"
#define ADD_AND_JUMP "addq $3, %0\njmp %l2"
#define ADD_FP "addsd %1, %0"
#else
#define WORD_REGISTER "x14"
#define FRAME_REGISTER "x15"
#define FP_REGISTER "d16"
#define FP_CONSTRAINT "w"
#define ADD_AND_JUMP "add %0, %0, #3\nb %l2"
#define ADD_FP "fadd %d0, %d0, %d1"
#endif
__extension__ register long word __asm__(WORD_REGISTER);
__extension__ register long frame_word __asm__(FRAME_REGISTER);
__extension__ register double fp __asm__(FP_REGISTER);
"""
    main_source = r"""
extern long update(long, double);
extern long use_large_frame(void);
extern void _Exit(int);
static int check(void) {
    word = 11;
    frame_word = 19;
    fp = 4.25;
    if (use_large_frame() != 24)
        return 1;
    long value = update(7, 0.5);
    if (value != 18 || word != 18 || fp != 4.75)
        return 2;
    __asm__(ADD_FP : "+" FP_CONSTRAINT(fp) : FP_CONSTRAINT(2.0));
    __asm__ goto (ADD_AND_JUMP : "+r"(word) : : "cc" : jumped);
    return 3;
jumped:
    return word != 21 || fp != 6.75 ? 4 : 0;
}
int main(void) { _Exit(check()); }
"""
    other_source = r"""
long update(long bump, double delta) {
    fp += delta;
    word += bump;
    return word;
}
long use_large_frame(void) {
    volatile unsigned char bytes[5000];
    bytes[4095] = 5;
    return frame_word + bytes[4095];
}
"""
    main_path = root / "clobbered-register-main.c"
    other_path = root / "clobbered-register-other.c"
    binary = root / "clobbered-register"
    main_path.write_text(declarations + main_source)
    other_path.write_text(declarations + other_source)
    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        cc_flags = ["-fzero-call-used-regs=skip"] if cc == gcc else flags
        require(invoke([cc, *cc_flags, "-std=c11", "-pedantic-errors", main_path, other_path,
                        "-o", binary]), f"{cc} call-clobbered global register compilation")
        require(invoke([*([] if cc == gcc else runner), binary]),
                f"{cc} call-clobbered global register execution")

    source = r"""
#if defined(__x86_64__)
#define ARG_REGISTER "rdi"
#define RESULT_REGISTER "rax"
#define FP_RESULT_REGISTER "xmm0"
#define RAW_CALL_BODY "addq $4,%rax\naddsd %xmm0,%xmm0\nret"
#define ADD_INPUT "addq %1,%0"
__extension__ register long arg_model __asm__(ARG_REGISTER);
#else
#define RESULT_REGISTER "x0"
#define FP_RESULT_REGISTER "d0"
#define RAW_CALL_BODY "add x0,x0,#4\nfadd d0,d0,d0\nret"
#define ADD_INPUT "add %0,%0,%1"
#endif
__extension__ register long result_model __asm__(RESULT_REGISTER);
__extension__ register double fp_model __asm__(FP_RESULT_REGISTER);
__asm__(".text\n.globl raw_call\nraw_call:\n" RAW_CALL_BODY);
extern void raw_call(void);
typedef struct { long n; double d; } mixed;
extern void _Exit(int);
_Thread_local long local;
static mixed argument_and_return(long n, double d) {
#if defined(__x86_64__)
    if (arg_model != n)
        return (mixed){0, 0};
#else
    if (result_model != n)
        return (mixed){0, 0};
#endif
    if (fp_model != d)
        return (mixed){0, 0};
    result_model = 91;
    fp_model = 1.0;
    return (mixed){n + 3, d + 4};
}
static long take_double(int tag, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, tag);
    double value = __builtin_va_arg(args, double);
    __builtin_va_end(args);
    return tag + (long)value;
}
static int check(void) {
    result_model = 95;
    fp_model = 21;
    mixed (*volatile fn)(long, double) = argument_and_return;
    mixed value = fn(30, 1.5);
    if (value.n != 33 || value.d != 5.5 || result_model != 33)
        return 1;
#if defined(__x86_64__)
    if (fp_model != 5.5)
        return 2;
#endif
    if (take_double(9, 18.0) != 27 || result_model != 27)
        return 3;
    result_model = 30;
    fp_model = 6.25;
    raw_call();
    if (result_model != 34 || fp_model != 12.5)
        return 4;
    __asm__(ADD_INPUT : "=r"(result_model) : "r"(11L), "0"(23L) : "cc");
    if (result_model != 34)
        return 7;
    local = 73;
    if (local != 73)
        return 5;
#if defined(__aarch64__)
    __extension__ long double expected = 13.5L;
    extern long double wide;
    __asm__("ldr %q0, %1" : "=w"(wide) : "m"(expected));
    long double actual = wide;
    if (actual != expected)
        return 6;
#endif
    return 0;
}
#if defined(__aarch64__)
__extension__ register long double wide __asm__("d8");
#endif
int main(void) { _Exit(check()); }
"""
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-", "-o", binary], source),
            "global register ABI compilation")
    require(invoke([*runner, binary]), "global register ABI execution")


def aggregate_register_variables(mcc, gcc, root, target, runner):
    declarations = r"""
typedef struct { int first, second; } Pair;
typedef union { Pair pair; unsigned long bits; } Both;
typedef struct { Pair pair; long number; } Wide;
typedef struct { signed first:5; unsigned other:3; } Bits;
#if defined(__x86_64__)
#define PAIR_REG "r12"
#define ARRAY_REG "r13"
#define UNION_REG "r14"
#define SIMD_REG "xmm11"
#define LOCAL_REG "r15"
#define BITS_REG "rbx"
#define COPY_WORD "movq %1,%0"
#define ADD_MEMBER "addl $5,%0"
#define COPY_SIMD "movaps %1,%0"
#define SIMD_CONSTRAINT "x"
#else
#define PAIR_REG "x19"
#define ARRAY_REG "x20"
#define UNION_REG "x21"
#define SIMD_REG "d8"
#define LOCAL_REG "x22"
#define BITS_REG "x23"
#define COPY_WORD "mov %x0,%x1"
#define ADD_MEMBER "add %w0,%w0,#5"
#define COPY_SIMD "ldr %q0,%1"
#define SIMD_CONSTRAINT "m"
#endif
__extension__ register Pair pair __asm__(PAIR_REG);
__extension__ register int elements[2] __asm__(ARRAY_REG);
__extension__ register Both both __asm__(UNION_REG);
__extension__ register Wide wide __asm__(SIMD_REG);
__extension__ register Bits bits __asm__(BITS_REG);
"""
    primary = root / "aggregate-register-main.c"
    secondary = root / "aggregate-register-other.c"
    primary.write_text(declarations + r"""
extern void _Exit(int);
extern int set_second(int);
extern Pair read_pair(void);
static int check(void) {
    pair = (Pair){-3, 10};
    if (pair.second++ != 10)
        return 1;
    pair.first = set_second(64);
    Pair copy = read_pair();
    if (copy.first != -7 || copy.second != 64)
        return 2;
    __asm__(ADD_MEMBER : "+r"(pair.first) : : "cc");
    __asm__(COPY_WORD : "=r"(elements) : "r"((10UL << 32) | 9));
    __extension__ (elements[1] += 2);
    int array_ok = __extension__ (elements[0] == 9 && elements[1] == 12);
    if (pair.first != -2 || pair.second != 64 || !array_ok)
        return 3;
    both.bits = 1;
    both.pair.second = -5;
    if (both.pair.first != 1 || both.pair.second != -5)
        return 4;
    bits = (Bits){-2, 6};
    if (bits.first++ != -2 || bits.first != -1 || bits.other != 6)
        return 8;
    wide = (Wide){{3, -4}, 0x102030405060708L};
    wide.pair.first += 2;
    Wide snapshot = wide;
    if (snapshot.pair.first != 5 || snapshot.pair.second != -4 ||
        snapshot.number != 0x102030405060708L)
        return 5;
    Wide input = {{7, 11}, -2};
#if defined(__x86_64__)
    __asm__(COPY_SIMD : "=x"(wide) : SIMD_CONSTRAINT(input));
#else
    __asm__(COPY_SIMD : "=w"(wide) : SIMD_CONSTRAINT(input));
#endif
    if (wide.pair.second != 11 || wide.number != -2)
        return 6;
    register Pair local __asm__(LOCAL_REG) = {101, 202};
    __asm__(COPY_WORD : "=r"(local) : "r"(pair));
    if (local.first != -2 || local.second != 64)
        return 7;
    int bytes[2];
    __asm__(COPY_WORD : "=r"(bytes) : "r"((5UL << 32) | 3));
    return bytes[0] != 3 || bytes[1] != 5 ? 9 : 0;
}
int main(void) { _Exit(check()); }
""")
    secondary.write_text(declarations + r"""
int set_second(int value) { pair.second = value; return -7; }
Pair read_pair(void) { return pair; }
""")
    binary = root / "aggregate-register"
    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        require(invoke([cc, *([] if cc == gcc else flags), "-std=c11", "-pedantic-errors",
                        primary, secondary, "-o", binary]), f"{cc} aggregate register compilation")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} aggregate register execution")

    register = "r12" if (target or os.uname().machine).startswith("x86_64") else "x19"
    for bad in (
        f'register struct {{ int n; }} v __asm__("{register}"); int *f(void) {{ return &v.n; }}',
        f'register int v[2] __asm__("{register}"); int *f(void) {{ return v; }}',
        f'register int v[2] __asm__("{register}"); int f(int i) {{ return v[i]; }}',
        f'register int v[2] __asm__("{register}"); int f(void) {{ return (v + 1)[0]; }}',
        f'register int v[2] __asm__("{register}"); int *f(void) {{ return &v[1]; }}',
        f'register struct {{ long a, b; }} v __asm__("{register}");',
        f'register struct {{ int n; }} v __asm__("{register}"); void f(void) '
        '{ __asm__("" : "+m"(v.n)); }',
        'void f(void) { int data[16] = {0}; __asm__("" : "+r"(data)); }',
    ):
        require(invoke([mcc, *flags, "-std=gnu11", "-fsyntax-only", "-"], bad), bad, success=False)
    strict = f'__extension__ register int v[2] __asm__("{register}"); int f(void) {{ return v[1]; }}'
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"], strict),
            "register array strict diagnostics", success=False)


def complex_numbers(mcc, gcc, root, target, runner):
    common = """
#include <stdarg.h>
#include <complex.h>
typedef float _Complex single;
typedef double _Complex pair;
typedef long double _Complex extended;
typedef struct { single a, b; } four;
single adjust(single, int);
pair variadic(int, ...);
extended subtract(extended, extended);
pair stack_args(double, double, double, double, double, double, double, pair);
four group(four);
"""
    library = common + """
single adjust(single z, int count) { return z + count; }
pair variadic(int tag, ...) {
    va_list args;
    va_start(args, tag);
    single a = va_arg(args, single);
    pair b = va_arg(args, pair);
    extended c = va_arg(args, extended);
    va_end(args);
    return (pair)a + b - (pair)c + tag;
}
extended subtract(extended a, extended b) { return a - b; }
pair stack_args(double a, double b, double c, double d, double e, double f, double g, pair z) {
    return z + (a+b+c+d+e+f+g);
}
four group(four x) { return (four){x.b, -x.a}; }
"""
    consumer = common + """
static const pair origin = __extension__ (2.25 + 3.5i);
static const extended initial = CMPLXL(5.25L, -2.75L);
static double coordinate = __imag__ (__extension__ (3.0 + 7.5i));
static single sample(void) {
    static int calls;
    return __builtin_complex(1.0f + calls++, -2.0f);
}
int main(void) {
    _Static_assert(sizeof(single) == 2 * sizeof(float), "size");
    _Static_assert(sizeof(extended) == 2 * sizeof(long double), "size");
    _Static_assert(_Alignof(pair) == _Alignof(double), "alignment");
    _Static_assert(_Generic((pair)0, double _Complex: 1, default: 0), "type");
    if (__real__ origin != 2.25 || __imag__ origin != 3.5 || coordinate != 7.5)
        return 1;
    single s = adjust(sample(), 3);
    if (__real__ s != 4.0f || __imag__ s != -2.0f || __real__ sample() != 2.0f)
        return 2;
    pair z = 5.0 - origin;
    __imag__ z += 0.5;
    double *component = &__real__ z;
    *component += 1;
    pair expected = CMPLX(3.75, -3.0);
    if (z != expected || (pair)0 || !z || !(z && 1) || (double)z != 3.75)
        return 3;
    if ((I ? z : origin) != z || (z * 2) / 2 != z)
        return 4;
    if ((__extension__ (~z)) != __builtin_complex(3.75, 3.0))
        return 5;
    extended x = subtract(initial, __builtin_complex(2.5L, 4.0L));
    if (__real__ x != 2.75L || __imag__ x != -6.75L)
        return 6;
    pair stacked = stack_args(1, 2, 3, 4, 5, 6, 7, z);
    if (__real__ stacked != 31.75 || __imag__ stacked != -3.0)
        return 7;
    pair args = variadic(2, s, origin, initial);
    if (__real__ args != 3.0 || __imag__ args != 4.25)
        return 8;
    four hfa = group((four){s, (single)origin});
    if (hfa.a != (single)origin || hfa.b != -s)
        return 9;
    union { float value; unsigned bits; } signed_zero = {__imag__ __builtin_complex(0.0f, -0.0f)};
    if (signed_zero.bits != 0x80000000u)
        return 10;
    double (*volatile real_part)(pair) = creal;
    long double (*volatile imag_part)(extended) = cimagl;
    extended (*volatile conjugate)(extended) = conjl;
    if (real_part(z) != 3.75 || imag_part(x) != -6.75L ||
        conjugate(x) != CMPLXL(2.75L, 6.75L))
        return 11;
    return 0;
}
"""
    main_path = root / "complex-main.c"
    library_path = root / "complex-library.c"
    binary = root / "complex"
    main_path.write_text(consumer)
    library_path.write_text(library)
    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        cc_flags = [] if cc == gcc else flags
        require(invoke([cc, *cc_flags, "-std=c11", "-pedantic-errors", main_path, library_path,
                        "-lm", "-o", binary]), f"{cc} complex build")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} complex execution")
    if target is None:
        main_object, library_object = root / "complex-main.o", root / "complex-library.o"
        for caller, callee in ((mcc, gcc), (gcc, mcc)):
            require(invoke([caller, "-std=c11", "-c", main_path, "-o", main_object]),
                    "complex ABI caller")
            require(invoke([callee, "-std=c11", "-c", library_path, "-o", library_object]),
                    "complex ABI callee")
            require(invoke([mcc, main_object, library_object, "-lm", "-o", binary]), "complex ABI link")
            require(invoke([binary]), "complex ABI interoperation")

    source = """
static _Complex int initial = 3 + 4i;
static _Complex int product = (3 + 4i) * (2 - 3j);
static _Complex int quotient = (3 + 4i) / (1 - 2j);
_Static_assert(__builtin_constant_p(2.0i), "imaginary constant");
static _Complex int multiply(_Complex int a, _Complex int b) { return a * b; }
static _Complex int divide(_Complex int a, _Complex int b) { return a / b; }
int main(void) {
    _Complex int value = multiply(initial, 2 - 3j);
    if (__real__ value != 18 || __imag__ value != -1 || value != product)
        return 1;
    __imag__ value = 9;
    _Complex int other = ~value;
    if (__real__ other != 18 || __imag__ other != -9 || !other)
        return 2;
    other = divide(initial, 1 - 2i);
    if (other != quotient || __real__ other != -2 || __imag__ other != 1)
        return 3;
    other = divide(initial, 2 - 1i);
    if (__real__ other != 1 || __imag__ other != 2)
        return 4;
    return 0;
}
"""
    for cc in (mcc, gcc):
        cc_flags = ["-x", "c"] if cc == gcc else flags
        require(invoke([cc, *cc_flags, "-std=gnu11", "-", "-o", binary], source),
                f"{cc} integer complex build")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} integer complex execution")
    for bad in (
        "double _Complex f(double _Complex z) { return z < 2; }",
        "_Complex _Bool invalid;",
        "double _Complex f(void) { return __builtin_complex(1.0f, 2.0); }",
        "double _Complex f(void) { return 1.0i; }",
    ):
        require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"], bad),
                bad, success=False)


def complex_multiplication_and_division(mcc, gcc, root, target, runner):
    source = """
#include <complex.h>
#include <float.h>
#ifdef __STDC_NO_COMPLEX__
#error complex arithmetic is enabled
#endif
typedef float _Complex single;
typedef double _Complex pair;
typedef long double _Complex extended;
static const pair product = CMPLX(3,5) * CMPLX(7,-1);
static const extended quotient = CMPLXL(26,32) / CMPLXL(7,-1);
static unsigned float_zero_sign(float x) {
    union { float value; unsigned bits; } bits = {x};
    return (bits.bits >> 31) != 0;
}
static unsigned double_zero_sign(double x) {
    union { double value; unsigned long bits; } bits = {x};
    return (unsigned)(bits.bits >> 63);
}
int main(void) {
    volatile single a = CMPLXF(3,5), b = CMPLXF(7,-1);
    volatile pair c = CMPLX(26,32), d = CMPLX(7,-1);
    volatile extended e = CMPLXL(3,5), f = CMPLXL(7,-1);
    if (a * b != CMPLXF(26,32) || c / d != CMPLX(3,5) ||
        e * f != CMPLXL(26,32) || (e * f) / f != CMPLXL(3,5))
        return 1;
    if (product != c || quotient != e)
        return 2;
    c = CMPLX(3,4);
    d = CMPLX(2,-1);
    if (10.0 / d != CMPLX(4,2) || c * d != CMPLX(10,5))
        return 3;
    a = CMPLXF(-0.0f,0.0f);
    b = CMPLXF(1.0f,1.0f);
    single zeros = a * b;
    if (__real__ zeros != 0.0f || __imag__ zeros != 0.0f ||
        !float_zero_sign(__real__ zeros) || float_zero_sign(__imag__ zeros))
        return 4;
    volatile double zero = 0.0;
    double infinity = 1.0 / zero, nan = zero / zero;
    c = CMPLX(infinity,infinity);
    d = CMPLX(1,1);
    pair nonfinite = c * d;
    if (!(__imag__ nonfinite > DBL_MAX))
        return 5;
    c = CMPLX(-2,-3);
    d = CMPLX(infinity,0);
    nonfinite = c / d;
    if (__real__ nonfinite != 0.0 || __imag__ nonfinite != 0.0 ||
        !double_zero_sign(__real__ nonfinite) || !double_zero_sign(__imag__ nonfinite))
        return 6;
    c = CMPLX(-1,2);
    d = CMPLX(-0.0,0.0);
    nonfinite = c / d;
    if (!(__real__ nonfinite > DBL_MAX) || !(__imag__ nonfinite < -DBL_MAX))
        return 7;
    c = CMPLX(nan,nan);
    d = CMPLX(1,2);
    nonfinite = c / d;
    if (__real__ nonfinite == __real__ nonfinite || __imag__ nonfinite == __imag__ nonfinite)
        return 8;
    c = CMPLX(1,1);
    d = CMPLX(0x1p1020,0x1p1020);
    if (c / d != CMPLX(0x1p-1020,0))
        return 9;
#ifdef __MCC__
    c = CMPLX(0x1p1023,0x1p-1023);
    d = CMPLX(0x1p677,0x1p-677);
    if (c / d != CMPLX(0x1p346,-0x1p-1008))
        return 10;
    e = CMPLXL(0x1p16300L,0x1p-16300L);
    f = CMPLXL(0x1p10800L,0x1p-10800L);
    if (e / f != CMPLXL(0x1p5500L,-0x1p-16100L))
        return 11;
    e = CMPLXL(0x1p12000L,0x1p12000L);
    f = CMPLXL(0x1p5000L,0x1p5000L);
    extended canceled = e * f;
    if (__real__ canceled != 0 || !(__imag__ canceled > LDBL_MAX))
        return 12;
    e = CMPLXL(0x1p16380L,0x1p-16360L);
    f = CMPLXL(0x1p100L,0);
    extended wide = e * f;
    if (!(__real__ wide > LDBL_MAX) || __imag__ wide != 0x1p-16260L)
        return 13;
    c = CMPLX(DBL_TRUE_MIN,DBL_TRUE_MIN);
    d = c;
    if (c / d != CMPLX(1,0))
        return 14;
#endif
    return 0;
}
"""
    binary = root / "complex-arithmetic"
    for cc in (mcc, gcc):
        flags = ["-x", "c"] if cc == gcc else [f"--target={target}"] if target else []
        require(invoke([cc, *flags, "-std=c11", "-pedantic-errors", "-", "-o", binary], source),
                f"{cc} complex arithmetic compilation")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} complex arithmetic execution")
        if cc == mcc:
            dynamic = invoke(["readelf", "-dW", binary])
            require(dynamic, "complex arithmetic dynamic dependencies")
            assert "libgcc" not in dynamic.stdout and "libm.so" not in dynamic.stdout


def bit_field_initialization(mcc, gcc, root, target, runner):
    source = """
struct bits {
    unsigned low:3;
    unsigned :2;
    signed high:3;
    unsigned char after;
};
struct record {
    struct bits before;
    unsigned lead:5;
    unsigned :0;
    signed middle:13;
    unsigned :3;
    unsigned remainder:4;
    const char *name;
    _Bool ready:1;
};
union selection { unsigned :0; unsigned number:10; unsigned other; };
static const char name[] = "static";
static struct record value = {
    .ready = 1, .before.high = -2, .before.after = 146,
    .name = name, .before.low = 5, .middle = -953, .lead = 10.25 + 8.5, .remainder = 13
};
static _Thread_local struct bits tls = {.high = -4, .low = 3, .after = 11};
static const union selection selection = {511};
static const struct bits array[] = {{3, -3, 9}, [3] = {.after = 17, .high = -1}};
static int test_block_static(void) {
    static struct bits nearby = {.low = 5, .high = -2, .after = 146};
    nearby.low++;
    return nearby.low == 6 && nearby.high == -2 && nearby.after == 146;
}
int main(void) {
    const unsigned char *bytes = (const unsigned char *)&value.before;
    if (sizeof(struct bits) != 4 || bytes[0] != 0xc5 || bytes[1] != 146 ||
        bytes[2] != 0 || bytes[3] != 0)
        return 1;
    if (value.lead != 18 || value.middle != -953 || value.remainder != 13 ||
        value.name != name || value.name[2] != 'a' || !value.ready)
        return 2;
    if (tls.low != 3 || tls.high != -4 || tls.after != 11 || selection.number != 511)
        return 3;
    if (array[0].low != 3 || array[0].high != -3 || array[1].high != 0 ||
        array[2].after != 0 || array[3].high != -1 || array[3].after != 17)
        return 4;
    if (!test_block_static())
        return 5;
    return 0;
}
"""
    packed = """
#include <stdio.h>
__extension__ typedef unsigned __int128 wide;
#define PACK __attribute__((packed))
struct PACK wire {
    unsigned char start;
    unsigned prefix:3;
    unsigned body:32;
    signed end:5;
    unsigned char marker;
};
struct PACK barrier {
    unsigned first:3;
    unsigned :0;
    unsigned second:5;
    unsigned char marker;
};
union PACK small { unsigned bits:19; unsigned short value; };
struct PACK full { unsigned low:3; wide whole:128; unsigned high:5; };
static struct wire wire = {0x44, 6, 0x12345678u, -7, 0x55};
static struct barrier barrier = {5, 11, 0x33};
static union small small = {.bits = 0x3abcd};
static struct full data = {5, ((wide)1 << 124) | ((wide)1 << 66) | 59, 27};
int main(void) {
    const unsigned char *p = (const unsigned char *)&wire;
    if (sizeof wire != 7 || p[0] != 0x44 || p[1] != 0xc6 || p[2] != 0xb3 ||
        p[3] != 0xa2 || p[4] != 0x91 || p[5] != 0xc8 || p[6] != 0x55)
        return 1;
#if defined(__aarch64__)
    if (sizeof barrier != 8 || _Alignof(struct barrier) != 4)
        return 2;
#else
    if (sizeof barrier != 6 || _Alignof(struct barrier) != 1)
        return 2;
#endif
    const unsigned char *q = (const unsigned char *)&barrier;
    if (q[0] != 5 || q[1] != 0 || q[4] != 11 || q[5] != 0x33)
        return 3;
    if (sizeof small != 3 || small.bits != 0x3abcd || small.value != 0xabcd)
        return 4;
    struct wire local = {1, 5, 0xaabbccdd, -3, 0x77};
    volatile struct wire *v = &local;
    v->prefix++;
    v->body ^= 0x00ff00ff;
    v->end -= 3;
    if (local.start != 1 || local.prefix != 6 || local.body != 0xaa44cc22 ||
        local.end != -6 || local.marker != 0x77)
        return 5;
    wide original = ((wide)1 << 124) | ((wide)1 << 66) | 59;
    if (sizeof data != 17 || data.low != 5 || data.whole != original || data.high != 27)
        return 6;
    data.whole ^= ((wide)1 << 127) | 7;
    if (data.low != 5 || data.whole != (original ^ (((wide)1 << 127) | 7)) || data.high != 27)
        return 7;
    return 0;
}
"""
    binary = root / "bit-field-initialization"
    flags = [f"--target={target}"] if target else []
    for dialect, test in (("c11", source), ("gnu11", packed)):
        for cc in (mcc, gcc):
            cc_flags = ["-x", "c"] if cc == gcc else flags
            require(invoke([cc, *cc_flags, f"-std={dialect}", "-pedantic-errors", "-", "-o", binary],
                           test), f"{cc} {dialect} bit-field initialization compilation")
            require(invoke([*([] if cc == gcc else runner), binary]),
                    f"{cc} {dialect} bit-field initialization execution")
    bad = """
int value(void);
static struct { unsigned bits:3; } not_constant = {value()};
"""
    require(invoke([mcc, *flags, "-std=c11", "-fsyntax-only", "-"], bad),
            "static bit-field initializer requires a constant", success=False)


def glibc_keyword_compatibility(mcc, gcc, root, target, runner):
    headers = root / "keyword-system-headers"
    headers.mkdir()
    (headers / "parent.h").write_text('#include "child.h"\n')
    (headers / "after.h").write_text('#include "child.h"\n')
    (headers / "child.h").write_text("""
#if defined(__MCC__) && defined(__GLIBC__)
#if !defined(__attribute__) || !defined(__extension__)
#error compatibility macros must remain available to system headers
#endif
#endif
#define PACKED_RECORD __attribute__((packed))
#define GNU_EXPRESSION __extension__
""")
    primary = root / "glibc-keywords.c"
    primary.write_text("""
#include <stdio.h>
#include <string.h>
#include <parent.h>
#if defined(__attribute__) || defined(__extension__)
#error compatibility macros must stay in system headers
#endif
struct PACKED_RECORD record { unsigned field:3; };
_Static_assert(sizeof(struct record) == 1, "packed attribute is active");
static struct record initialized = {5};
static int from_gnu_expression(void) {
    return GNU_EXPRESSION ({ int value = initialized.field; value + 2; });
}
#pragma push_macro("__attribute__")
#define __attribute__(argument)
struct PACKED_RECORD unmodified { unsigned field:3; };
_Static_assert(sizeof(struct unmodified) == sizeof(unsigned), "explicit macro takes effect");
#pragma pop_macro("__attribute__")
#include <after.h>
struct PACKED_RECORD packed_again { unsigned field:3; };
_Static_assert(sizeof(struct packed_again) == 1, "restored GNU attribute");
int main(void) { return from_gnu_expression() != 7 || strlen("gnu") != 3; }
""")
    binary = root / "glibc-keywords"
    flags = [f"--target={target}"] if target else []
    options = ["-std=c11", "-pedantic-errors", "-isystem", headers]
    for cc in (mcc, gcc):
        require(invoke([cc, *([] if cc == gcc else flags), *options, primary, "-o", binary]),
                f"{cc} GNU keywords after system headers")
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} GNU keyword execution")
    preprocessed = invoke([mcc, *flags, *options, "-E", primary])
    require(preprocessed, "preprocess GNU keywords after system headers")
    expanded = root / "glibc-keywords.i"
    expanded.write_text(preprocessed.stdout)
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", expanded, "-o", binary]),
            "compile preprocessed GNU keywords")
    require(invoke([*runner, binary]), "execute code compiled from preprocessed GNU keywords")


def system_header_pragmas(mcc, gcc, root, target, runner):
    headers = root / "system-pragma-headers"
    headers.mkdir()
    (headers / "marked.h").write_text("""
#ifdef BEFORE_MARKER
static int before_marker(void) { return ({ int n = 9; n; }); }
#endif
#pragma GCC system_header
#warning retained message
#include "nested.h"
static int header_value(void) { return ({ int n = nested_value(); n + 2; }); }
""")
    (headers / "nested.h").write_text("""
static int nested_value(void) { return ({ int n = 5; n; }); }
""")
    (headers / "operator.h").write_text("""
_Pragma("GCC system_header")
static int operator_value(void) { return ({ int n = 3; n; }); }
""")
    after = root / "include-after"
    after.mkdir()
    (after / "stddef.h").write_text("#error -idirafter took precedence over a standard header\n")
    (after / "unique.h").write_text("static int trailing_value(void) { return ({ 11; }); }\n")
    primary = root / "system-pragmas.c"
    primary.write_text("""
#include <marked.h>
#include <operator.h>
#include <stddef.h>
#include <unique.h>
#ifdef AFTER_HEADER
static int after_header(void) { return ({ int n = 1; n; }); }
#endif
int main(void) { return header_value() + operator_value() + trailing_value() != 21; }
""")
    binary = root / "system-pragmas"
    flags = [f"--target={target}"] if target else []
    options = ["-std=c11", "-pedantic-errors", "-I", headers, "-idirafter", after]
    for cc in (mcc, gcc):
        cc_flags = [] if cc == gcc else flags
        compile_result = invoke([cc, *cc_flags, *options, primary, "-o", binary])
        require(compile_result, f"{cc} recognize system header pragmas")
        assert "retained message" in compile_result.stderr, compile_result.stderr
        require(invoke([*([] if cc == gcc else runner), binary]), f"{cc} run code from system headers")
        for macro in ("BEFORE_MARKER", "AFTER_HEADER"):
            require(invoke([cc, *cc_flags, *options, f"-D{macro}", "-fsyntax-only", primary]),
                    f"{cc} report extension outside system headers", success=False)
        warnings = invoke([cc, *cc_flags, "-std=gnu11", "-Wpedantic", "-Wsystem-headers",
                           "-I", headers, "-idirafter", after, "-fsyntax-only", primary])
        require(warnings, f"{cc} enable system header diagnostics")
        assert "operator.h:" in warnings.stderr, warnings.stderr

    processed = invoke([mcc, *flags, *options, "-E", primary])
    require(processed, "retain system header provenance when preprocessing")
    assert re.search(r'^# [0-9]+ ".*operator\.h" 3$', processed.stdout, re.M)
    without_markers = invoke([mcc, *flags, *options, "-E", "-P", primary])
    require(without_markers, "preprocess without line markers")
    assert not re.search(r"^# [0-9]+ ", without_markers.stdout, re.M)
    expanded = root / "system-pragmas.i"
    expanded.write_text(processed.stdout)
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", expanded, "-o", binary]),
            "compile preprocessed system headers")
    require(invoke([*runner, binary]), "execute preprocessed system header test")

    main_pragma = '#pragma GCC system_header\nint f(void) { return ({ 7; }); }\n'
    result = invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"], main_pragma)
    require(result, "system_header pragma has no effect in primary source", success=False)
    assert "ignored outside an include file" in result.stderr, result.stderr


def system_header_splices(mcc, gcc, root, target):
    headers = root / "system-splice-headers"
    headers.mkdir()
    splice = "\\ \n"
    (headers / "outer.h").write_text(
        f"enum {{ before = {splice}1 }};\n"
        "#pragma GCC system_header\n"
        '#line 90 "logical-outer.h"\n'
        f"enum {{ after = {splice}2 }};\n"
        '#include "nested.h"\n'
    )
    (headers / "nested.h").write_text(f"enum {{ nested = {splice}3 }};\n")
    (headers / "operator.h").write_text(
        '#define MARK_SYSTEM _Pragma("GCC system_header")\n'
        f"enum {{ before_op = {splice}4 }};\n"
        "MARK_SYSTEM\n"
        f"enum {{ after_op = {splice}5 }};\n"
    )
    primary = headers / "splice-main.c"
    primary.write_text(
        '#include "outer.h"\n'
        '#include "operator.h"\n'
        f"int value = {splice}6;\n"
    )
    user_locations = [("operator.h", "2"), ("outer.h", "1"), ("splice-main.c", "3")]
    system_locations = [("logical-outer.h", "90"), ("nested.h", "1"), ("operator.h", "5")]
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        for warnings, expected in [
            ([], user_locations),
            (["-Wsystem-headers"], user_locations + system_locations),
        ]:
            result = invoke([cc, *flags, "-std=gnu11", *warnings, "-fsyntax-only", primary])
            require(result, f"{cc} diagnose splices according to system header status")
            locations = re.findall(r"([^/\n]+):(\d+):\d+: warning: backslash and newline separated by",
                                   result.stderr)
            assert sorted(locations) == sorted(expected), result.stderr


def preprocessor_pragma_messages(mcc, gcc, root, target):
    headers = root / "dependency-headers"
    headers.mkdir()
    newer = headers / "newer.h"
    older = headers / "older.h"
    checked = root / "checked.h"
    for path, timestamp in [(newer, 300), (older, 100)]:
        path.write_text("#error dependency header was included\n")
        os.utime(path, (timestamp, timestamp))
    checked.write_text(
        '#pragma GCC dependency <newer.h> regenerate checked header\n'
        '#pragma GCC dependency <older.h> stale warning should be absent\n'
        '_Pragma("GCC system_header")\n'
        '#pragma GCC warning "system pragma warning"\n'
    )
    os.utime(checked, (200, 200))
    source = root / "pragma-messages.c"
    source.write_text(
        '#include "checked.h"\n'
        r'#pragma GCC warning "value \101 and \"quotes\" %d"' "\n"
        '#define WARN _Pragma("GCC warning \\"macro pragma warning\\"")\n'
        'WARN\n'
        'int value;\n'
    )
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        command = [cc, *flags, "-std=c11", "-pedantic-errors", "-I", headers, "-E"]
        result = invoke([*command, source])
        require(result, f"{cc} handle preprocessing pragma warnings and dependencies")
        for message in ("current file is older than newer.h", "regenerate checked header",
                        'value A and "quotes" %d', "macro pragma warning"):
            assert message in result.stderr, result.stderr
        assert "stale warning should be absent" not in result.stderr, result.stderr
        assert "system pragma warning" not in result.stderr, result.stderr
        result = invoke([*command, "-Wsystem-headers", source])
        require(result, f"{cc} enable pragma warnings from system headers")
        assert "system pragma warning" in result.stderr, result.stderr
        raw = invoke([cc, *flags, "-std=gnu11", "-E", "-"],
                     '#pragma GCC warning R"(raw \\n)"\n')
        require(raw, f"{cc} accept raw string pragma message")
        assert r"raw \n" in raw.stderr, raw.stderr

        for text, message in [
            ('#pragma GCC error "direct pragma error"\n', "direct pragma error"),
            ('#define FAIL _Pragma("GCC error \\"macro pragma error\\"")\nFAIL\n',
             "macro pragma error"),
            ('#define TEXT "replacement"\n#pragma GCC warning TEXT\n', "pragma GCC warning"),
            ('#pragma GCC dependency "absent.h"\n', "absent.h"),
        ]:
            result = invoke([*command, "-"], text)
            require(result, f"{cc} reject failing preprocessing pragma", success=False)
            assert message in result.stderr, result.stderr


def preprocessor_poison(mcc, gcc, root, target):
    (root / "poison-guard.h").write_text("#pragma GCC poison legacy obsolete\n")
    (root / "obsolete.h").write_text("int guarded_header;\n")
    valid = """
enum { legacy = 7 };
#define legacy 42
#define SELECT legacy
#include "poison-guard.h"
#include <obsolete.h>
#pragma GCC poison legacy
#if 0
int obsolete;
#elif obsolete
#error disabled
#endif
int selected = SELECT;
"""
    failures = [
        ('#include "poison-guard.h"\nint obsolete;\n', "obsolete"),
        ('#pragma GCC poison obsolete\n#define NEW obsolete\n', "obsolete"),
        ('#pragma GCC poison obsolete\n#if defined(obsolete)\n#endif\n', "obsolete"),
        ('#define DISCARD(x) 0\n#pragma GCC poison obsolete\nint value = DISCARD(obsolete);\n',
         "obsolete"),
        ('#define MAKE(a, b) a##b\n#pragma GCC poison obsolete\nint MAKE(obso, lete);\n',
         "obsolete"),
        ('#define MARK _Pragma("GCC poison obsolete")\nMARK int obsolete;\n', "obsolete"),
        ('#pragma GCC poison 42\n', "invalid"),
    ]
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        command = [cc, *flags, "-std=gnu11", "-I", root, "-E", "-"]
        result = invoke(command, valid)
        require(result, f"{cc} process existing macro references and header names after GCC poison")
        assert "poisoning existing macro" in result.stderr, result.stderr
        assert re.search(r"int selected\s*=\s*legacy\s*;", result.stdout), result.stdout
        assert "guarded_header" in result.stdout, result.stdout
        for code, message in failures:
            result = invoke(command, code)
            require(result, f"{cc} reject invalid use of GCC poison", success=False)
            assert message in result.stderr and "poison" in result.stderr, result.stderr

        result = invoke(command, """
#define MARK _Pragma("GCC poison obsolete")
#define MAKE(a, b) a int b;
MAKE(MARK, obsolete)
""")
        require(result, f"{cc} accept argument read before macro poisoning")
        assert re.search(r"int\s+obsolete\s*;", result.stdout), result.stdout


def structure_packing(mcc, gcc, root, target, runner):
    (root / "push-pack.h").write_text("""
#pragma pack(push, from_header, 1)
struct header_record { char tag; int value; };
""")
    source = root / "structure-packing.c"
    source.write_text("""
#define OFFSET(type, field) __builtin_offsetof(type, field)
#define CHECK(type, size, alignment) \\
    _Static_assert(sizeof(type) == (size) && _Alignof(type) == (alignment), #type)
#define PACK(directive) _Pragma(#directive)
struct ordinary { char tag; int value; };
#include "push-pack.h"
struct after_header { char tag; int value; };
#pragma pack(pop, from_header)
struct restored { char tag; int value; };
#pragma pack(2)
struct limited { char tag; int value; char rest[]; };
#pragma pack(push, outer, 1)
struct bits { unsigned int small : 5; unsigned int wide : 32; char tail; };
#pragma pack(push, inner, 16)
struct cap16 { char tag; long value; };
#pragma pack(pop, outer)
union packed_union { char byte; unsigned int bits : 20; };
struct nested { char tag; struct ordinary inner; };
#pragma pack(push)
#pragma pack(4)
struct cap4 { char tag; long value; };
#pragma pack(8)
struct cap8 { char tag; long value; };
#pragma pack(pop)
struct named_restore { char tag; long value; };
struct local_change {
    char tag;
    int value;
    PACK(pack(1))
};
struct barrier { char tag; unsigned int : 0; char value; };
struct __attribute__((packed)) attribute_record { char tag; int value; };
#pragma pack(0)
struct reset { char tag; int value; };
CHECK(struct ordinary, 8, 4);
CHECK(struct header_record, 5, 1);
CHECK(struct after_header, 5, 1);
CHECK(struct restored, 8, 4);
CHECK(struct limited, 6, 2);
CHECK(struct bits, 6, 1);
CHECK(struct cap16, 16, 8);
CHECK(union packed_union, 4, 2);
CHECK(struct nested, 10, 2);
CHECK(struct cap4, 12, 4);
CHECK(struct cap8, 16, 8);
CHECK(struct named_restore, 10, 2);
CHECK(struct local_change, 5, 1);
CHECK(struct attribute_record, 5, 1);
CHECK(struct reset, 8, 4);
#ifdef __aarch64__
CHECK(struct barrier, 8, 4);
#else
CHECK(struct barrier, 5, 1);
#endif
_Static_assert(OFFSET(struct limited, value) == 2 && OFFSET(struct limited, rest) == 6, "cap");
_Static_assert(OFFSET(struct bits, tail) == 5, "cross-container bit-field");
_Static_assert(OFFSET(struct barrier, value) == 4, "zero-width bit-field");
_Static_assert(__alignof__(((struct limited *)0)->value) == 2, "member alignment");
_Static_assert(__alignof__(((struct nested *)0)->inner) == 2, "aggregate member alignment");
_Static_assert(__alignof__(((struct nested *)0)->inner.value) == 4, "nested member alignment");

static struct bits initial = { .small = 7, .wide = 0xf1234567u, .tail = 'z' };
static struct bits change(struct bits value, unsigned int wide)
{
    value.wide = wide;
    return value;
}
int main(void)
{
    struct bits local = { .small = 11, .wide = initial.wide, .tail = 'a' };
    struct bits updated = change(local, 0xc3218765u);
    union packed_union value = { .bits = 0xabcdeu };
    return initial.small != 7 || initial.wide != 0xf1234567u || initial.tail != 'z' ||
           local.small != 11 || local.wide != 0xf1234567u || local.tail != 'a' ||
           updated.small != 11 || updated.wide != 0xc3218765u || updated.tail != 'a' ||
           value.bits != 0xabcdeu;
}
""")
    binary = root / "structure-packing"
    cc_flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        flags = cc_flags if cc == mcc else ["-x", "c"]
        command = [cc, *flags, "-std=c11", "-pedantic-errors"]
        require(invoke([*command, source, "-o", binary]), f"{cc} compile packed records")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run packed records")

        options = """
struct initial { char tag; int value; };
#pragma pack(1)
struct tighter { char tag; int value; };
#pragma pack()
struct reset { char tag; int value; };
#pragma pack(0)
struct natural { char tag; int value; };
_Static_assert(sizeof(struct initial) == INITIAL, "initial alignment");
_Static_assert(sizeof(struct tighter) == TIGHTER, "pragma alignment");
_Static_assert(sizeof(struct reset) == INITIAL, "reset alignment");
_Static_assert(sizeof(struct natural) == NATURAL, "explicit natural alignment");
"""
        for options_flags, sizes in [
            (["-fpack-struct=2"], (6, 5, 8)),
            (["-fpack-struct"], (5, 5, 5)),
            (["-fpack-struct", "-fno-pack-struct"], (8, 5, 8)),
        ]:
            initial, tighter, natural = sizes
            result = invoke([*command, *options_flags, f"-DINITIAL={initial}", f"-DTIGHTER={tighter}",
                             f"-DNATURAL={natural}", "-fsyntax-only", "-"], options)
            require(result, f"{cc} apply {options_flags} with #pragma pack")
            if options_flags == ["-fpack-struct"]:
                assert "no effect" in result.stderr, result.stderr

        invalid = """
#pragma pack(2)
#pragma pack(3)
#pragma pack(pop)
struct retained { char tag; int value; };
_Static_assert(sizeof(struct retained) == 6, "invalid pragmas preserve alignment");
"""
        result = invoke([*command, "-fsyntax-only", "-"], invalid)
        require(result, f"{cc} reject invalid pack settings with warnings")
        assert "alignment must be" in result.stderr and "without" in result.stderr, result.stderr

    for flags in ([], ["-P"]):
        preprocessed = invoke([mcc, *cc_flags, "-std=c11", "-E", *flags, source])
        require(preprocessed, "preprocess packed records")
        assert "#pragma pack(1)" in preprocessed.stdout and "#pragma pack(0)" in preprocessed.stdout
        expanded = root / "structure-packing.i"
        expanded.write_text(preprocessed.stdout)
        require(invoke([mcc, *cc_flags, "-std=c11", "-pedantic-errors", expanded, "-o", binary]),
                "compile preprocessed packed records")
        require(invoke([*runner, binary]), "run preprocessed packed records")


def mixed_abi(mcc, gcc, provider, client):
    library, caller, binary = provider.with_suffix(".o"), client.with_suffix(".o"), client.with_suffix("")
    for driver, other in [(gcc, mcc), (mcc, gcc)]:
        require(invoke([driver, "-std=c11", "-c", provider, "-o", library]), "compile ABI provider")
        require(invoke([other, "-std=c11", "-c", client, "-o", caller]), "compile ABI caller")
        require(invoke([mcc, library, caller, "-o", binary]), "link mixed ABI objects")
        require(invoke([binary]), f"call {driver} ABI provider from {other}")


def packed_argument_abi(mcc, gcc, root):
    (root / "packed-abi.h").write_text("""
#pragma pack(push, 1)
struct packet { char tag; unsigned int length; short id; };
struct floats { float left; unsigned int : 0; float right; };
struct padded { float left; unsigned long : 0; float right; };
union single { float value; unsigned int : 0; };
union pair { double values[2]; unsigned long : 0; };
#pragma pack(pop)
struct packet packet(struct packet value);
struct floats floats(struct floats value);
struct padded padded(struct padded value);
union single single(union single value);
union pair pair(union pair value);
""")
    provider = root / "packed-abi-provider.c"
    provider.write_text("""
#include "packed-abi.h"
struct packet packet(struct packet value) { value.length += 3; return value; }
struct floats floats(struct floats value) { value.left += 0.5f; return value; }
struct padded padded(struct padded value) { value.left += 0.5f; return value; }
union single single(union single value) { value.value += 0.5f; return value; }
union pair pair(union pair value) { value.values[0] += 0.5; return value; }
""")
    client = root / "packed-abi-client.c"
    client.write_text("""
#include "packed-abi.h"
int main(void)
{
    struct packet p = packet((struct packet){'p', 17, 42});
    struct floats f = floats((struct floats){1.0f, 3.0f});
    struct padded b = padded((struct padded){2.0f, 4.0f});
    union single s = single((union single){5.0f});
    union pair d = pair((union pair){{6.0, 7.0}});
    if (p.tag != 'p' || p.length != 20 || p.id != 42)
        return 1;
    if (f.left != 1.5f || f.right != 3.0f)
        return 2;
    if (b.left != 2.5f || b.right != 4.0f)
        return 3;
    if (s.value != 5.5f)
        return 4;
    if (d.values[0] != 6.5 || d.values[1] != 7.0)
        return 5;
    return 0;
}
""")
    mixed_abi(mcc, gcc, provider, client)


def transparent_unions(mcc, gcc, root, target, runner):
    header = root / "transparent-union.h"
    header.write_text("""
union PointerUnion { int *i; const long *l; };
typedef union PointerUnion Pointer __attribute__((__transparent_union__));
typedef union PointerUnion OtherPointer __attribute__((__transparent_union__));
_Static_assert(!__builtin_types_compatible_p(Pointer, union PointerUnion), "original union");
_Static_assert(!__builtin_types_compatible_p(Pointer, OtherPointer), "separate typedef");
union __attribute__((transparent_union)) Word { unsigned u; short s; float f; };
typedef struct { float a, b; } FloatPair;
typedef struct { float x, y; } OtherPair;
union Pair { FloatPair first; OtherPair second; } __attribute__((transparent_union));
typedef struct { long a, b, c; } Large;
union __attribute__((transparent_union)) LargeValue { Large data; long array[3]; };
int pointed(Pointer value, int selector);
unsigned word(union Word value);
float pair(float before, const union Pair value, float after);
long large(long before, union LargeValue value, long after);
int stacked(int a, int b, int c, int d, int e, int f, int g, int h, Pointer value);
int varied(Pointer value, ...);
int previous(int *);
""")
    provider = root / "transparent-provider.c"
    provider.write_text("""
#include <stdarg.h>
#include "transparent-union.h"
int pointed(Pointer value, int selector)
{
    return selector ? (int)*value.l : value.i ? *value.i : -1;
}
unsigned word(union Word value) { return value.u; }
float pair(float before, const union Pair value, float after)
{
    return before + value.first.a + 2 * value.first.b + after;
}
long large(long before, union LargeValue value, long after)
{
    value.data.b *= 2;
    return before + value.data.a + value.data.b + value.data.c + after;
}
int stacked(int a, int b, int c, int d, int e, int f, int g, int h, Pointer value)
{
    return a + b + c + d + e + f + g + h + *value.i;
}
int varied(Pointer value, ...)
{
    va_list ap;
    va_start(ap, value);
    int result = *value.i + va_arg(ap, int);
    va_end(ap);
    return result;
}
int previous(Pointer value) { return *value.i; }
""")
    client = root / "transparent-client.c"
    client.write_text("""
#include "transparent-union.h"
union CastValue { int number; double other; };
static union CastValue stored = (union CastValue)41;
static int called;
static int base = 18;
static int *address(void) { called++; return &base; }

int main(void)
{
    Pointer boxed = { .i = &base };
    long other = 23;
    void *generic = &base;
    if (pointed(address(), 0) != 18 || called != 1 || pointed(&other, 1) != 23 ||
        pointed(generic, 0) != 18 || pointed(0, 0) != -1 || pointed(boxed, 0) != 18)
        return 1;
    int (*take_i)(int *, int) = pointed;
    int (*take_l)(const long *, int) = pointed;
    if (take_i(&base, 0) != 18 || take_l(&other, 1) != 23 || previous(&base) != 18)
        return 2;
    volatile short narrow = -11;
    if (word(narrow) != 0x0000fff5U || word(3.0f) != 0x40400000U ||
        word((union Word){ .u = 31 }) != 31U)
        return 3;
    FloatPair fields = {2, 3};
    OtherPair alt = {4, 5};
    Large big = {6, 7, 8};
    if (pair(8, fields, 9) != 25 || pair(6, alt, 7) != 27 || large(10, big, 9) != 47 ||
        big.b != 7 || stacked(1, 2, 3, 4, 5, 6, 7, 8, &base) != 54 ||
        varied(&base, 12) != 30)
        return 4;
    int x = 7;
    if (stored.number != 41 || ((union CastValue)(x++)).number != 7 || x != 8)
        return 5;
    return 0;
}
""")
    flags = [f"--target={target}"] if target else []
    objects = {}
    binary = root / "transparent-test"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        name = "mcc" if cc == mcc else "gcc"
        objects[cc] = []
        for src in (provider, client):
            obj = root / f"{name}-{src.stem}.o"
            require(invoke([*prefix, "-std=gnu11", "-c", src, "-o", obj]),
                    f"{cc} compile {src.name}")
            objects[cc].append(obj)
        require(invoke([*prefix, *objects[cc], "-o", binary]), f"{cc} link transparent union test")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run transparent union test")

    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, objects[provider_cc][0], objects[client_cc][1], "-o", binary]),
                    f"mixed transparent union link with {provider_cc} provider")
            require(invoke([binary]), f"mixed transparent union run with {provider_cc} provider")

    declarations = ('union Plain { int *i; long *l; }; '
                    'typedef union Plain T __attribute__((transparent_union)); int f(T); ')
    rejections = (
        (declarations + "int g(union Plain p) { return f(p); }", "-std=gnu11"),
        (declarations + "int g(short *p) { return f(p); }", "-std=gnu11"),
        (declarations + "union Plain x; void g(int *p) { x = p; }", "-std=gnu11"),
        (declarations + "int f(union Plain);", "-std=gnu11"),
        (declarations + "int g(int *p) { return f(p); }", "-pedantic-errors"),
        (declarations + "int g(int *p) { return ((union Plain)p = (union Plain){.i=p}).i != 0; }",
         "-std=gnu11"),
        ("union U { int x; long y; }; union U cast(double n) { return (union U)n; }", "-std=gnu11"),
    )
    for cc in (mcc, gcc):
        prefix = [mcc, *flags] if cc == mcc else [gcc, "-x", "c"]
        for source, extra in rejections:
            require(invoke([*prefix, "-std=gnu11", extra, "-fsyntax-only", "-"], source),
                    f"{cc} reject {source}", success=False)
        expected = invoke([*prefix, "-std=gnu11", "-Werror=discarded-qualifiers", "-fsyntax-only", "-"],
                          declarations + "int g(const int *p) { return f(p); }")
        require(expected, f"{cc} diagnose discarded qualifiers", success=False)
        assert "[-Werror=discarded-qualifiers]" in expected.stderr, expected.stderr
        rejected_type = ("union __attribute__((transparent_union)) U { float f; int i; }; "
                         "int safe(union U value) { return value.i; }")
        ignored = invoke([*prefix, "-std=gnu11", "-fsyntax-only", "-"], rejected_type)
        require(ignored, f"{cc} retain unsupported transparent representation as ordinary union")
        assert "cannot be made transparent" in ignored.stderr, ignored.stderr


def designated_initializers(mcc, gcc, root, target, runner):
    source = root / "designated-initializer.c"
    source.write_text("""
#if !__has_attribute(designated_init)
#error missing designated_init attribute
#endif
struct __attribute__((__designated_init__)) Inner { int x, y; };
struct Outer { struct Inner a; int z; };
struct Parent { struct Outer row[2]; int last; };
struct Tail { int a,b,c; } __attribute__((designated_init));
typedef const struct Inner Qualified;

static struct Outer resumed = { .a.x = 11, 12, 13 };
static struct Parent deep = { .row[0].a.y = 21, 22, {{.x=23,.y=24},25}, 26 };
static struct Outer skipped = { .a.x = 31, .z = 32 };
static Qualified configured = { .y = 33 };
static struct Tail fixed = { .b = 34, .c = 35 };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdesignated-init"
static struct Inner quiet = { 40, 41 };
#pragma GCC diagnostic pop

int main(void)
{
    struct Outer runtime = { .a.x = 44, 45, 46 };
    struct Inner copy = quiet;
    return resumed.a.x!=11 || resumed.a.y!=12 || resumed.z!=13 ||
           deep.row[0].a.x!=0 || deep.row[0].a.y!=21 || deep.row[0].z!=22 ||
           deep.row[1].a.x!=23 || deep.row[1].a.y!=24 || deep.row[1].z!=25 || deep.last!=26 ||
           skipped.a.x!=31 || skipped.a.y!=0 || skipped.z!=32 ||
           configured.y!=33 || fixed.a!=0 || fixed.b!=34 || fixed.c!=35 ||
           runtime.a.x!=44 || runtime.a.y!=45 || runtime.z!=46 ||
           copy.x!=40 || copy.y!=41;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "designated-initializer"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        result = invoke([*prefix, "-std=gnu11", source, "-o", binary])
        require(result, f"{cc} build designated initializer test")
        assert sum("[-Wdesignated-init]" in line for line in result.stderr.splitlines()) == 5, result.stderr
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run designated initializer test")

        muted = invoke([*prefix, "-std=gnu11", "-Wno-designated-init", "-Werror",
                        "-fsyntax-only", source])
        require(muted, f"{cc} suppress designated-init warnings")
        assert not muted.stderr, muted.stderr
        rejected = invoke([*prefix, "-std=gnu11", "-Werror=designated-init",
                           "-fsyntax-only", source])
        require(rejected, f"{cc} promote designated-init warning", success=False)
        assert "[-Werror=designated-init]" in rejected.stderr, rejected.stderr

        strict = [*prefix, *(["-x", "c"] if cc == gcc else []), "-std=c11", "-pedantic-errors",
                  "-Werror=designated-init", "-fsyntax-only", "-"]
        require(invoke(strict, "struct __attribute__((designated_init)) S { int x; };"
                               "struct S x = {.x=1};"), f"{cc} designated initialization in strict C11")
        require(invoke(strict, "union __attribute__((designated_init)) U { int x; };"),
                f"{cc} reject designated_init on a union", success=False)
        misplaced = invoke(strict, "struct S { int x; };"
                                    "typedef struct S T __attribute__((designated_init));"
                                    "T t = {1};")
        require(misplaced, f"{cc} report attributes applied after the definition")
        assert "[-Wattributes]" in misplaced.stderr and "-Werror=designated-init" not in misplaced.stderr


def initializer_ranges(mcc, gcc, root, target, runner):
    source = root / "initializer-ranges.c"
    source.write_text("""
struct Field { int x; unsigned y:4; };
union Either { struct Field fields; long wide; };
static struct Field fields[] = {
    [0 ... 2] = {.x=7, .y=3}, [1].x=9, [4 ... 5].y=4
};
static union Either alternatives[2] = {
    [0 ... 1].fields = {.x=4, .y=2}, [1].wide=8
};
static int grid[3][3] = {[0 ... 1][0 ... 1] = 5, 6, 7};
static struct Field elided[] = {[0 ... 1] = 5, 6, 7};
static unsigned short words[][4] = {[0 ... 1] = u"αβ", [1][0] = 3};
static struct Flex { unsigned length; int data[]; } flex = {.length=4, .data[0 ... 2]=5, 6};
static struct Field legacy = {x:10, y:5};
static int old[] = {[1] 12, [3 ... 4] 15};

int main(void)
{
    int x=0, y=0, bit=0;
    for (int i=1; i<=2; i++) {
        struct Field a[] = {
            [0 ... 2] = {.x=++x, .y=++y}, [1].x=8, [4 ... 6].y=++bit
        };
        int copied=0;
        struct Field copies[] = {[0 ... 2] = (copied++, a[2]), [1].x=9};
        char text[][16] = {[0 ... 2] = "cd", [1][0]='e'};
        if (x!=i || y!=i || bit!=i || sizeof a/sizeof *a!=7 ||
            a[0].x!=i || a[1].x!=8 || a[2].x!=i || a[2].y!=i ||
            a[3].x || a[3].y || a[4].y!=i || a[5].y!=i || a[6].y!=i ||
            copied!=1 || copies[0].x!=i || copies[1].x!=9 || copies[1].y || copies[2].y!=i ||
            text[1][0]!='e' || text[1][1]!='d' || text[2][15] || text[2][0]!='c')
            return 1;
    }
    return sizeof fields/sizeof *fields!=6 || fields[1].x!=9 || fields[1].y!=3 ||
           fields[3].x || fields[3].y || fields[5].y!=4 ||
           alternatives[0].fields.x!=4 || alternatives[0].fields.y!=2 || alternatives[1].wide!=8 ||
           grid[0][0]!=5 || grid[0][1]!=5 || grid[0][2] ||
           grid[1][0]!=5 || grid[1][1]!=5 || grid[1][2]!=6 || grid[2][0]!=7 ||
           elided[0].x!=5 || elided[0].y || elided[1].x!=5 || elided[1].y!=6 || elided[2].x!=7 ||
           words[0][0]!=0x3b1 || words[1][0]!=3 || words[1][1]!=0x3b2 || words[1][2] ||
           __builtin_object_size(flex.data, 1)!=4*sizeof(int) || flex.data[2]!=5 || flex.data[3]!=6 ||
           legacy.x!=10 || legacy.y!=5 ||
           sizeof old/sizeof *old!=5 || old[0] || old[1]!=12 || old[2] || old[4]!=15;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "initializer-ranges"
    for cc in (mcc, gcc):
        prefix = [cc, *(flags if cc == mcc else [])]
        require(invoke([*prefix, "-std=gnu11", source, "-o", binary]),
                f"{cc} build aggregate ranges and legacy initializers")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run aggregate ranges and legacy initializers")

        strict = [*prefix, *(["-x", "c"] if cc == gcc else []),
                  "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"]
        require(invoke(strict, "int a[2][2] = {[0] = 1, 2, 3};"),
                f"{cc} accept brace elision after a designator in C11")
        for declaration in ("int a[] = {[0 ... 1] = 2};", "int a[] = {[0] 1};",
                            "struct T { int x; } t = {x:1};"):
            require(invoke(strict, declaration), f"{cc} reject GNU initializer in C11", success=False)


def alignment_attributes(mcc, gcc, root, target, runner):
    source = root / "alignment-attributes.c"
    source.write_text("""
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#if !__has_attribute(__aligned__) || !__alignas_is_defined || !__alignof_is_defined
#error alignment support query failed
#endif
_Alignas(64) extern int shared;
int shared = 7;
static alignas(64) _Alignas(128) int bss;
static _Thread_local _Alignas(128) int tls = 3;
static int enhanced __attribute__((__aligned__(sizeof(long) * sizeof(long)))) = 5;
static int maximum __attribute__((aligned()));
typedef int lower __attribute__((aligned(1), packed));
typedef struct natural { int value; } higher __attribute__((aligned(64)));
typedef struct delayed delayed_alias __attribute__((aligned(64)));
struct delayed { int value; };
struct aligned_member { char tag; _Alignas(64) int value; };
struct __attribute__((packed, aligned(32))) packed_type { char tag; int value; };
struct suffix { int value; } __attribute__((aligned(32)));
struct __attribute__((packed)) explicit_member { char tag; alignas(8) int value; };
struct field { char tag; int value __attribute__((packed, aligned(2))); };
struct bit { char tag; unsigned value : 5 __attribute__((aligned(8))); char tail; };
#pragma pack(push, 2)
struct capped { char tag; alignas(8) int iso; int gnu __attribute__((aligned(16))); };
#pragma pack(pop)
_Static_assert(alignof(struct aligned_member) == 64, "member updates type alignment");
_Static_assert(sizeof(struct aligned_member) == 128, "member updates type stride");
_Static_assert(offsetof(struct aligned_member, value) == 64, "member offset");
_Static_assert(sizeof(struct packed_type) == 32 && offsetof(struct packed_type, value) == 1,
               "packed and aligned type");
_Static_assert(sizeof(struct suffix) == 32, "trailing type attribute");
_Static_assert(offsetof(struct explicit_member, value) == 8, "member in packed type");
_Static_assert(offsetof(struct field, value) == 2 && alignof(struct field) == 2, "packed member");
_Static_assert(offsetof(struct bit, tail) == 9 && alignof(struct bit) == 8, "aligned bit-field");
_Static_assert(offsetof(struct capped, iso) == 2 && offsetof(struct capped, gnu) == 6 &&
               alignof(struct capped) == 2, "pragma pack caps explicit member alignment");
_Static_assert(alignof(const lower) == 1 && sizeof(lower) == sizeof(int), "unaligned typedef");
_Static_assert(alignof(int __attribute__((aligned(8)))) == 8, "aligned type name");
_Static_assert(alignof(const higher) == 64 && sizeof(higher) == sizeof(int) &&
               alignof(struct natural) == alignof(int), "aligned typedef preserves base type");
_Static_assert(alignof(delayed_alias) == 64 && sizeof(delayed_alias) == sizeof(int),
               "aligned typedef of forward declaration");
_Static_assert(__alignof__(shared) == 64 && __alignof__(enhanced) == 64 &&
               __alignof__(maximum) == __BIGGEST_ALIGNMENT__, "object alignment queries");
_Static_assert(sizeof(enhanced) == sizeof(int) && alignof(__typeof__(enhanced)) == alignof(int),
               "object alignment preserves type");

static int compute(int value) __attribute__((aligned(128)));
static int compute(int value) { return value + 3; }
static int normal_function(void);
#ifdef __aarch64__
_Static_assert(__alignof__(normal_function) == 4, "AArch64 function minimum");
#else
_Static_assert(__alignof__(normal_function) == 1, "x86-64 function minimum");
#endif
_Static_assert(__alignof__(compute) == 128, "explicit function alignment");
static struct aligned_member adjust(int x, struct aligned_member value, int y)
{
    value.value += x + y;
    return value;
}
static int collect(int count, ...)
{
    va_list args;
    va_start(args, count);
    int first = va_arg(args, int);
    struct aligned_member middle = va_arg(args, struct aligned_member);
    int last = va_arg(args, int);
    va_end(args);
    return first + middle.value + last;
}
int main(void)
{
    _Alignas(0) _Alignas(long double) int local = 1;
    int other __attribute__((aligned(256))) = 2;
    int length = 5;
    _Alignas(128) int dynamic[length];
    higher copied = { 11 };
    static _Alignas(256) int kept;
    struct aligned_member updated = adjust(local, (struct aligned_member){ 'a', 13 }, other);
    dynamic[3] = 9;
    if (updated.tag != 'a' || updated.value != 16 || collect(3, local, updated, other) != 19 ||
        dynamic[3] != 9 || compute(copied.value) != 14 || shared != 7 || enhanced != 5 || tls != 3)
        return 1;
    return ((uintptr_t)&shared & 63) || ((uintptr_t)&bss & 127) ||
           ((uintptr_t)&tls & 127) || ((uintptr_t)&enhanced & 63) ||
           ((uintptr_t)&local & (alignof(long double) - 1)) || ((uintptr_t)&other & 255) ||
           ((uintptr_t)dynamic & 127) || ((uintptr_t)&copied & 63) ||
           ((uintptr_t)&kept & 255) || ((uintptr_t)&updated.value & 63) ||
           ((uintptr_t)compute & 127);
}
""")
    binary = root / "alignment-attributes"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if target and cc == mcc else []
        options = [cc, *flags, "-std=c11", "-pedantic-errors"]
        require(invoke([*options, source, "-o", binary]), f"{cc} compile C11 and GNU alignments")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run aligned objects and calls")
        for invalid in [
            "_Alignas(2) int reduced;",
            "_Alignas(3) int bad;",
            "_Alignas(-8) int bad;",
            "int changing; _Alignas(changing) int bad;",
            "_Alignas(8) typedef int bad;",
            "_Alignas(8) int function(void);",
            "void test(_Alignas(8) int parameter);",
            "void test(void) { _Alignas(8) register int bad; }",
            "struct test { _Alignas(16) unsigned int bad : 3; };",
            "int bad __attribute__((aligned(3)));",
            "typedef int element __attribute__((aligned(32))); element bad[2];",
        ]:
            syntax_flags = ["-x", "c"] if cc == gcc else []
            require(invoke([*options, *syntax_flags, "-fsyntax-only", "-"], invalid + "\n"),
                    f"{cc} diagnose invalid alignment {invalid}", success=False)


def aligned_argument_abi(mcc, gcc, root):
    (root / "aligned-abi.h").write_text("""
struct large { char tag; _Alignas(64) int value; };
struct __attribute__((aligned(16))) whole { int value; };
typedef int wide __attribute__((aligned(64)));
typedef int narrow __attribute__((aligned(1)));
struct unaligned_member { char tag; narrow value; };
struct large round_trip(int a, int b, int c, int d, int e, int f, int g,
                        struct large payload, int tail);
int whole_record(int before, struct whole payload, int after);
int narrow_member(int before, struct unaligned_member payload, int after);
int varargs(int count, ...);
""")
    provider = root / "aligned-abi-provider.c"
    provider.write_text("""
#include "aligned-abi.h"
struct large round_trip(int a, int b, int c, int d, int e, int f, int g,
                        struct large payload, int tail)
{
    payload.value += a + b + c + d + e + f + g + tail;
    return payload;
}
int whole_record(int before, struct whole payload, int after)
{
    return before + 3 * payload.value + 5 * after;
}
int narrow_member(int before, struct unaligned_member payload, int after)
{
    return payload.tag == 'u' ? before + 3 * payload.value + 5 * after : -1;
}
int varargs(int count, ...)
{
    __builtin_va_list list;
    __builtin_va_start(list, count);
    for (int i = 1; i <= count; ++i)
        if (__builtin_va_arg(list, int) != i)
            return 0;
    wide scalar = __builtin_va_arg(list, wide);
    struct large payload = __builtin_va_arg(list, struct large);
    int tail = __builtin_va_arg(list, int);
    __builtin_va_end(list);
    return payload.tag == 'v' ? scalar + payload.value + tail : -1;
}
""")
    client = root / "aligned-abi-client.c"
    client.write_text("""
#include "aligned-abi.h"
int main(void)
{
    wide scalar = 17;
    struct large v = round_trip(1, 2, 3, 4, 5, 6, 7, (struct large){'o', 9}, 10);
    if (v.tag != 'o' || v.value != 47)
        return 1;
    if (whole_record(3, (struct whole){5}, 7) != 53)
        return 2;
    if (narrow_member(3, (struct unaligned_member){'u', 5}, 7) != 53)
        return 3;
    if (varargs(8, 1, 2, 3, 4, 5, 6, 7, 8, scalar, (struct large){'v', 9}, 13) != 39)
        return 4;
    return 0;
}
""")
    mixed_abi(mcc, gcc, provider, client)


def declarator_alignment(mcc, gcc, root, target, runner):
    source = root / "declarator-alignment.c"
    source.write_text("""
#include <stdint.h>
#define A(n) __attribute__((aligned(n)))
static int value = 19;
static int * A(32) direct = &value, *plain = &value;
static int * A(32) * A(64) indirect = &direct;
static int * A(64) A(32) repeated = &value;
static int * A(64) explicit_storage A(32) = &value;
static int * A(1) small = &value;
static int * A(64) merged = &value;
extern int * A(32) merged;
static int * A(32) *merged_inner = &direct;
extern int **merged_inner;
typedef int * A(32) pointer;
static int (A(64) *array_pointer)[4];
struct member { char tag; int * A(32) value; };
static int result(void) { return value; }
static int (* A(128) callback)(void) = result;
_Static_assert(__alignof__(direct) == 32 && _Alignof(__typeof__(direct)) == 32, "pointer type");
_Static_assert(__alignof__(plain) == 8, "adjacent declarator");
_Static_assert(_Alignof(__typeof__(indirect)) == 64 &&
               _Alignof(__typeof__(*indirect)) == 32, "two levels of pointer alignment");
_Static_assert(_Alignof(__typeof__(repeated)) == 32, "later pointer annotation");
_Static_assert(__alignof__(explicit_storage) == 32 &&
               _Alignof(__typeof__(explicit_storage)) == 64, "declaration and type alignment");
_Static_assert(_Alignof(__typeof__(small)) == 1 && sizeof(small) == sizeof(void *),
               "reduced pointer type alignment");
_Static_assert(__alignof__(merged) == 64 && _Alignof(__typeof__(merged)) == 8,
               "retain object alignment when combining declarations");
_Static_assert(_Alignof(__typeof__(*merged_inner)) == 8, "combine nested pointer types");
_Static_assert(_Alignof(pointer) == 32 && _Alignof(int * A(32)) == 32, "abstract declarator");
_Static_assert(_Alignof(int * A(32) *) == 8, "inner abstract pointer");
_Static_assert(_Alignof(__typeof__(*array_pointer)) == 64 &&
               _Alignof(__typeof__(array_pointer)) == 8, "parenthesized declarator");
_Static_assert(_Alignof(struct member) == 32 &&
               __builtin_offsetof(struct member, value) == 32, "member pointer type");
_Static_assert(__alignof__(callback) == 128, "function pointer");
static int read(int *const A(64) volatile param)
{
    _Static_assert(_Alignof(__typeof__(param)) == 64, "qualified pointer parameter");
    return ((uintptr_t)&param & 63) ? -1 : *param;
}
static int first(int a[static A(32) 2]) { return a[0]; }
int main(void)
{
    int data[2] = {42, 15};
    return ((uintptr_t)&direct & 31) || ((uintptr_t)&indirect & 63) ||
           ((uintptr_t)&explicit_storage & 31) || ((uintptr_t)&callback & 127) ||
           **indirect != 19 || read(plain) != 19 || callback() != 19 || first(data) != 42;
}
""")
    binary = root / "declarator-alignment"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if target and cc == mcc else []
        options = [cc, *flags, "-std=c11", "-pedantic-errors"]
        result = invoke([*options, source, "-o", binary])
        require(result, f"{cc} compile aligned nested declarators")
        assert "attributes in parameter array declarator ignored" in result.stderr, result.stderr
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run aligned nested declarators")
        for invalid in ("int * __attribute__((aligned(32))) unsupported[2];",
                        "int unsupported[__attribute__((aligned(32))) 2];"):
            require(invoke([*options, *(["-x", "c"] if cc == gcc else []),
                            "-fsyntax-only", "-"], invalid + "\n"),
                    f"{cc} diagnose invalid array alignment", success=False)


def pack_placement(mcc, gcc, root, target, runner):
    source = root / "pack-placement.c"
    source.write_text("""
#define WRAP(op) struct natural { char tag; int value; }; op \\
                 struct packed { char tag; int value; };
#define TWICE(op) op op
WRAP(_Pragma("pack(push, 1)"))
_Static_assert(sizeof(struct natural) == 8 && sizeof(struct packed) == 5, "macro order");
#pragma pack(pop)
TWICE(_Pragma("pack(push, 2)"))
#pragma pack(pop)
struct remaining { char tag; int value; };
_Static_assert(sizeof(struct remaining) == 6, "repeated macro operand");
#pragma pack(pop)
static int f(int yes)
{
    if (yes)
#pragma pack()
        return 7;
    return 8;
}
int main(void)
{
    return f(0) != 8 || f(1) != 7;
}
""")
    binary = root / "pack-placement"
    arch_flags = [f"--target={target}"] if target else []
    invalid = [
        "struct s { int v; }\n#pragma pack(1)\n;",
        "struct s { int\n#pragma pack(1)\nv; };",
        "int f(void) { return\n#pragma pack()\n0; }",
        "int f(int x) { if (x) return 1;\n#pragma pack()\nelse return 0; }",
        "enum e { a,\n#pragma pack(1)\nb };",
    ]
    for cc in (mcc, gcc):
        flags = arch_flags if cc == mcc else []
        command = [cc, *flags, "-std=c11", "-pedantic-errors"]
        require(invoke([*command, source, "-o", binary]), f"{cc} compile ordered pack pragmas")
        require(invoke([*(runner if cc == mcc else []), binary]),
                f"{cc} run ordered pack pragmas")
        for code in invalid:
            require(invoke([*command, *(["-x", "c"] if cc == gcc else []),
                            "-fsyntax-only", "-"], code + "\n"),
                    f"{cc} diagnose misplaced pack pragma", success=False)

    for markers in ([], ["-P"]):
        output = invoke([mcc, *arch_flags, "-std=c11", "-E", *markers, source])
        require(output, "preprocess ordered pack pragmas")
        assert output.stdout.count("#pragma pack(push, 2)") == 2, output.stdout
        expanded = root / "pack-placement.i"
        expanded.write_text(output.stdout)
        require(invoke([mcc, *arch_flags, "-std=c11", "-pedantic-errors", expanded, "-o", binary]),
                "compile ordered pack pragmas from preprocessed source")
        require(invoke([*runner, binary]), "run ordered pack pragmas from preprocessed source")


def elf_symbols(path, option="-sW"):
    result = invoke(["readelf", option, path])
    require(result, f"inspect symbols in {path.name}")
    return {fields[-1]: fields[1:-1] for line in result.stdout.splitlines()
            if len(fields := line.split()) == 8 and fields[0].endswith(":") and fields[2].isdigit()}


def weak_symbols(mcc, gcc, root, target, runner):
    provider = root / "weak-provider.c"
    provider.write_text("""
#if !__has_attribute(weak) || !__has_attribute(alias) || !__has_attribute(weakref)
#error weak and alias attribute queries
#endif
__attribute__((weak)) int choice = 17;
int uninitialized __attribute__((weak));
__attribute__((weak)) int fallback(void) { return 7; }
int implementation(void) { return 5; }
int mirror(void) __attribute__((alias("implementation")));
#pragma weak alternate = implementation
static int backing __asm__("backing_elf") = 23;
extern int view __attribute__((alias("backing_elf")));
extern int view_chain __attribute__((alias("view")));
int late = 29;
_Pragma("weak late")
int observed(void) { return choice + fallback(); }
""")
    strong = root / "strong-provider.c"
    strong.write_text("""
int choice = 31, late = 43, uninitialized = 37;
int fallback(void) { return 11; }
int alternate(void) { return 13; }
""")
    optional = root / "optional-provider.c"
    optional.write_text("int missing_data = 41; int missing_fn(void) { return 39; }\n")
    client = root / "weak-client.c"
    client.write_text("""
extern int choice, uninitialized, late, view, view_chain;
extern int fallback(void), implementation(void), alternate(void), mirror(void);
extern int observed(void);
extern int missing_data __attribute__((weak));
static int indirect_data __attribute__((weakref("missing_data")));
static int indirect_fn(void) __attribute__((weakref));
static int indirect_fn(void) __attribute__((alias("missing_fn")));
static int chained_fn(void) __attribute__((weakref("indirect_fn")));
extern int missing_fn(void);
static int (*saved_fn)(void) = missing_fn;
static int *saved_data = &indirect_data;
static int *volatile saved_view = &view;
static int *volatile saved_chain = &view_chain;
#pragma weak missing_fn
int main(void)
{
#ifdef OVERRIDE
    if (choice != 31 || fallback() != 11 || alternate() != 13 || late != 43 ||
        uninitialized != 37 || observed() != 42)
        return 1;
#else
    if (choice != 17 || fallback() != 7 || alternate() != 5 || late != 29 ||
        uninitialized != 0 || observed() != 24)
        return 2;
#endif
    if (implementation() != 5 || mirror() != 5 || view != 23 || saved_chain != saved_view)
        return 3;
#ifdef PRESENT
    if (!saved_data || *saved_data != 41 || !saved_fn || saved_fn() != 39 ||
        chained_fn() != 39 || &missing_data != saved_data)
        return 4;
#else
    if (saved_data || saved_fn || &missing_data || missing_fn || chained_fn)
        return 5;
#endif
    return 0;
}
""")
    required = root / "required-weakref.c"
    required.write_text("""
extern int missing_data;
static int shortcut __attribute__((weakref("missing_data")));
int *direct = &missing_data;
int *via_alias = &shortcut;
int main(void) { return direct != via_alias; }
""")
    binary = root / "weak-program"
    provider_obj, strong_obj, optional_obj, required_obj, archive = [
        root / name for name in ("weak-provider.o", "strong-provider.o", "optional-provider.o",
                                "required-weakref.o", "liboptional-weak.a")
    ]

    def symbols(path, option="-sW"):
        return {name: (value, int(size), kind, binding, section)
                for name, (value, size, kind, binding, _, section) in elf_symbols(path, option).items()}

    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if target and cc == mcc else []
        prefix = [cc, *flags]
        options = [*prefix, "-std=c11", "-pedantic-errors"]
        run = runner if cc == mcc else []
        for source, output in [(provider, provider_obj), (strong, strong_obj),
                               (optional, optional_obj), (required, required_obj)]:
            require(invoke([*options, "-fPIC", "-c", source, "-o", output]), f"{cc} compile {source.name}")
        exported = symbols(provider_obj)
        for name in ("choice", "uninitialized", "fallback", "alternate", "late"):
            assert exported[name][3] == "WEAK" and exported[name][4] != "UND", exported
        for alias, original in (("mirror", "implementation"), ("alternate", "implementation"),
                                ("view", "backing_elf"), ("view_chain", "backing_elf")):
            assert exported[alias][:3] == exported[original][:3], exported
            assert exported[alias][4] == exported[original][4], exported
        assert symbols(required_obj)["missing_data"][3:] == ("GLOBAL", "UND")
        require(invoke([*prefix, required_obj, "-o", binary]),
                f"{cc} reject missing target when there is a strong reference", success=False)
        require(invoke([*prefix, required_obj, optional_obj, "-o", binary]),
                f"{cc} resolve direct and weak references to one object")
        require(invoke([*run, binary]), f"{cc} share the direct reference and the weakref target")

        require(invoke(["ar", "rcs", archive, optional_obj]), "archive the optional provider")
        for definitions, linked in [
            ([], [provider_obj]), (["-static"], [provider_obj]),
            ([], [provider_obj, archive]), (["-DPRESENT"], [provider_obj, optional_obj]),
            (["-DOVERRIDE"], [provider_obj, strong_obj]),
            (["-DOVERRIDE"], [strong_obj, provider_obj]),
        ]:
            require(invoke([*options, *definitions, client, *linked, "-o", binary]),
                    f"{cc} link weak declarations with {definitions} and {linked}")
            require(invoke([*run, binary]), f"{cc} execute linked weak declarations")

        shared = root / "libweak-provider.so"
        require(invoke([*prefix, "-shared", provider_obj, "-o", shared]),
                f"{cc} export weak definitions from a DSO")
        assert symbols(shared, "--dyn-syms")["fallback"][3] == "WEAK"
        require(invoke([*options, "-fPIC", client, f"-L{root}", "-lweak-provider",
                        f"-Wl,-rpath,{root}", "-o", binary]),
                f"{cc} link against the shared weak definitions")
        require(invoke([*run, binary]), f"{cc} run against shared weak definitions")
        require(invoke([*options, "-fPIC", "-DOVERRIDE", client, strong_obj,
                        f"-L{root}", "-lweak-provider", f"-Wl,-rpath,{root}", "-o", binary]),
                f"{cc} link executable definitions that override shared weak symbols")
        require(invoke([*run, binary]), f"{cc} preempt the weak definitions inside the DSO")

        optional_shared = root / "libweak-optional.so"
        require(invoke([*prefix, "-shared", optional_obj, "-o", optional_shared]),
                f"{cc} build the DSO for optional references")
        for mode, definition in [("--as-needed", []), ("--no-as-needed", ["-DPRESENT"])]:
            require(invoke([*options, *definition, client, provider_obj, f"-L{root}",
                            f"-Wl,{mode}", "-lweak-optional", f"-Wl,-rpath,{root}", "-o", binary]),
                    f"{cc} link optional references with {mode}")
            require(invoke([*run, binary]), f"{cc} resolve optional DSO references with {mode}")
            dynamic = invoke(["readelf", "-d", binary])
            require(dynamic, "inspect optional DSO dependencies")
            assert ("[libweak-optional.so]" in dynamic.stdout) == bool(definition), dynamic.stdout

        for invalid in [
            "static int hidden __attribute__((weak));",
            "extern int bad __attribute__((weakref(\"target\")));",
            "extern int absent __attribute__((alias(\"never_defined\")));",
            "#pragma weak absent = never_defined",
            "static int a __attribute__((weakref(\"b\"))); "
            "static int b __attribute__((weakref(\"a\"))); int *use = &b;",
        ]:
            stdin = ["-x", "c"] if cc == gcc else []
            require(invoke([*options, *stdin, "-c", "-", "-o", required_obj], invalid + "\n"),
                    f"{cc} diagnose invalid weak declaration {invalid}", success=False)

    expanded = root / "weak-provider.i"
    flags = [f"--target={target}"] if target else []
    result = invoke([mcc, *flags, "-E", "-P", provider])
    require(result, "preserve weak pragmas in preprocessed source")
    assert "#pragma weak alternate = implementation" in result.stdout
    expanded.write_text(result.stdout)
    require(invoke([mcc, *flags, "-c", expanded, "-o", provider_obj]),
            "assemble weak declarations from preprocessed source")
    assert symbols(provider_obj)["alternate"][3] == "WEAK"


def symbol_visibility(mcc, gcc, root, target, runner):
    provider = root / "visibility-provider.c"
    provider.write_text("""
#if !__has_attribute(visibility)
#error visibility attribute unavailable
#endif
#define API __attribute__((__visibility__("default")))
#define PUSH_HIDDEN _Pragma("GCC visibility push(hidden)")
#define POP_VISIBILITY _Pragma("GCC visibility pop")
extern int external_value;
extern int merged_value __attribute__((visibility("protected")));
API int replaceable(void) { return 1; }
int private_default = 2;
PUSH_HIDDEN
extern int hidden_value;
extern int merged_value __attribute__((visibility("hidden")));
int private_fn(void) { return 3; }
API int exported_alias(void) __attribute__((alias("private_fn")));
#pragma GCC visibility push(protected)
int protected_fn(void) { return 4; }
int protected_value = 5;
#pragma GCC visibility pop
int hidden_value = 6;
POP_VISIBILITY
#pragma GCC visibility push(default)
int pragma_public = 7;
#pragma GCC visibility pop
__attribute__((visibility("internal"))) int internal_fn(void) { return 8; }
extern int weak_hidden __attribute__((weak, visibility("hidden")));
extern int unused_hidden __attribute__((visibility("hidden")));
API int *protected_address(void) { return &protected_value; }
API int check_provider(void) {
    return replaceable() + private_default + private_fn() + protected_fn() +
           protected_value + hidden_value + pragma_public + internal_fn() +
           external_value + merged_value + (&weak_hidden ? weak_hidden : 0);
}
""")
    consumer = root / "visibility-consumer.c"
    consumer.write_text("""
int replaceable(void) { return 11; }
int private_default = 12;
int private_fn(void) { return 13; }
int protected_fn(void) { return 14; }
int hidden_value = 16;
int internal_fn(void) { return 18; }
int external_value = 19;
int merged_value = 20;
int weak_hidden = 21;
extern int pragma_public, protected_value;
extern int check_provider(void), exported_alias(void);
extern int *protected_address(void);
int main(void) {
    return check_provider() != 75 || exported_alias() != 3 || pragma_public != 7 ||
           protected_value != 5 || protected_address() != &protected_value;
}
""")
    definition = root / "visibility-definition.c"
    definition.write_text("int merged_value = 10;\n")
    decl = root / "visibility-declaration.c"
    decl.write_text("""
extern int protected_value __attribute__((visibility("hidden")));
int read_local(void) { return protected_value; }
""")
    archived = root / "visibility-archive.c"
    archived.write_text("int protected_value = 31;\n")
    read_local = root / "visibility-local-main.c"
    read_local.write_text("int read_local(void); int main(void) { return read_local() != 31; }\n")
    obj, definitions = root / "visibility.o", root / "visibility-definitions.o"
    library, binary = root / "libvisibility.so", root / "visibility-program"
    local_obj, archived_obj = root / "visibility-local.o", root / "visibility-archive.o"
    archive = root / "libvisibility-local.a"
    foreign = root / "libvisibility-foreign.so"
    expected = {
        "replaceable": "DEFAULT", "external_value": "DEFAULT", "private_default": "HIDDEN",
        "private_fn": "HIDDEN", "exported_alias": "DEFAULT", "protected_fn": "PROTECTED",
        "protected_value": "PROTECTED", "hidden_value": "HIDDEN", "pragma_public": "DEFAULT",
        "internal_fn": "INTERNAL", "weak_hidden": "HIDDEN", "merged_value": "PROTECTED",
    }
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if target and cc == mcc else []
        prefix = [cc, *flags]
        options = [*prefix, "-std=c11", "-pedantic-errors", "-fPIC"]
        run = runner if cc == mcc else []
        result = invoke([*options, "-fvisibility=hidden", "-c", provider, "-o", obj])
        require(result, f"{cc} compile visibility declarations")
        assert "warning:" in result.stderr and "merged_value" in result.stderr, result.stderr
        symbols = elf_symbols(obj)
        assert {name: symbols[name][4] for name in expected} == expected, symbols
        assert symbols["external_value"][-1] == "UND", symbols
        assert "unused_hidden" not in symbols, symbols
        for source, output in [(definition, definitions), (decl, local_obj), (archived, archived_obj)]:
            require(invoke([*options, "-c", source, "-o", output]), f"{cc} compile {source.name}")
        require(invoke([*prefix, "-shared", obj, definitions, "-o", library]), f"{cc} link visibility DSO")
        exports = elf_symbols(library, "--dyn-syms")
        for name in ("replaceable", "exported_alias", "check_provider", "protected_address", "pragma_public"):
            assert exports[name][4] == "DEFAULT", exports
        for name in ("protected_fn", "protected_value", "merged_value"):
            assert exports[name][4] == "PROTECTED", exports
        for name in ("private_default", "private_fn", "hidden_value", "internal_fn", "weak_hidden"):
            assert name not in exports, exports
        linked = elf_symbols(library)
        for name in ("private_default", "private_fn", "hidden_value", "internal_fn"):
            assert linked[name][3] == "LOCAL", linked
        require(invoke([*options, consumer, library, "-o", binary]), f"{cc} link visibility consumer")
        require(invoke([*run, binary]), f"{cc} enforce interposition and protected addresses")

        require(invoke([*prefix, "-shared", archived_obj, "-o", foreign]),
                f"{cc} create DSO with the matching external name")
        require(invoke([*options, read_local, local_obj, foreign, "-o", binary]),
                f"{cc} reject a DSO as the definition for a hidden reference", success=False)
        require(invoke(["ar", "rcs", archive, archived_obj]), "archive the private definition")
        require(invoke([*options, read_local, local_obj, foreign, archive, "-o", binary]),
                f"{cc} satisfy a hidden reference from an archive after the DSO")
        require(invoke([*run, binary]), f"{cc} use the archived private definition")
        assert elf_symbols(binary)["protected_value"][3] == "LOCAL"

        for mode in ("default", "internal", "protected"):
            require(invoke([*options, f"-fvisibility={mode}", "-c", definition, "-o", obj]),
                    f"{cc} compile with visibility mode {mode}")
            assert elf_symbols(obj)["merged_value"][4] == mode.upper()

        require(invoke([*options, "-fvisibility=invalid", "-c", definition, "-o", obj]),
                f"{cc} reject an invalid visibility option", success=False)
        invalid = 'int wrong __attribute__((visibility("invalid")));\n'
        require(invoke([*options, "-x", "c", "-c", "-", "-o", obj], invalid),
                f"{cc} reject an invalid visibility attribute", success=False)

    flags = [f"--target={target}"] if target else []
    expanded = root / "visibility-provider.i"
    result = invoke([mcc, *flags, "-E", provider])
    require(result, "preserve visibility pragmas in preprocessed source")
    assert "#pragma GCC visibility push(hidden)" in result.stdout
    expanded.write_text(result.stdout)
    require(invoke([mcc, *flags, "-fvisibility=hidden", "-c", expanded, "-o", obj]),
            "compile visibility from preprocessed source")
    symbols = elf_symbols(obj)
    assert {name: symbols[name][4] for name in expected} == expected, symbols


def lifecycle_attributes(mcc, gcc, root, target, runner):
    late = root / "lifecycle-late.c"
    late.write_text("""
#if !__has_attribute(constructor) || !__has_attribute(destructor)
#error lifecycle attribute queries
#endif
int putchar(int);
__attribute__((constructor(450))) static void init_middle(void) { putchar('B'); }
__attribute__((destructor(450))) static void fini_middle(void) { putchar('b'); }
__attribute__((constructor(777))) static void init_first_tie(void) { putchar('D'); }
__attribute__((destructor(777))) static void fini_first_tie(void) { putchar('d'); }
__attribute__((constructor())) static void init_default(void) { putchar('C'); }
__attribute__((destructor(65535))) static void fini_default(void) { putchar('c'); }
""")
    early = root / "lifecycle-early.c"
    early.write_text("""
int putchar(int);
enum { early = 100 + 1 };
static void init_early(void) __attribute__((__constructor__(early)));
static void fini_early(void) __attribute__((__destructor__(early)));
static void init_early(void) { putchar('A'); }
static void fini_early(void) { putchar('a'); }
__attribute__((constructor(777))) static void init_second_tie(void) { putchar('E'); }
__attribute__((destructor(777))) static void fini_second_tie(void) { putchar('e'); }
void raw_init(void) { putchar('N'); }
void raw_fini(void) { putchar('n'); }
int lifecycle_anchor(void) { return 1; }
""")
    unpadded = root / "lifecycle-unpadded.s"
    unpadded.write_text("""
.section .init_array.99,"aw"
.p2align 3
.quad raw_init
.section .fini_array.99,"aw"
.p2align 3
.quad raw_fini
.section .note.GNU-stack,""
""")
    program = root / "lifecycle-main.c"
    program.write_text("""
int putchar(int);
int lifecycle_anchor(void);
int main(void) {
    putchar('M');
    return !lifecycle_anchor();
}
""")
    loader = root / "lifecycle-load.c"
    loader.write_text("""
#include <dlfcn.h>
int putchar(int);
int main(int argc, char **argv) {
    putchar('[');
    void *module = argc == 2 ? dlopen(argv[1], RTLD_NOW) : 0;
    if (!module) return 1;
    putchar('M');
    if (dlclose(module)) return 2;
    putchar(']');
    return 0;
}
""")
    library = root / "liblifecycle.so"
    binary = root / "lifecycle"
    providers = (late, early, unpadded)
    products = {}
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if target and cc == mcc else []
        prefix = [cc, *flags]
        options = [*prefix, "-std=c11", "-pedantic-errors", "-fPIC"]
        run = runner if cc == mcc else []
        tool = "mcc" if cc == mcc else "gcc"
        objects = [root / f"{source.stem}-{tool}.o" for source in providers]
        products[cc] = objects
        for source, output in zip(providers, objects):
            compile_flags = prefix if source.suffix == ".s" else options
            require(invoke([*compile_flags, "-c", source, "-o", output]), f"{cc} compile {source.name}")
        result = invoke(["readelf", "-SW", objects[0]])
        require(result, "inspect lifecycle input sections")
        for array, kind in (("init", "INIT_ARRAY"), ("fini", "FINI_ARRAY")):
            for suffix in (".00450", ".00777", ""):
                assert re.search(rf"\s\.{array}_array{re.escape(suffix)}\s+{kind}\s", result.stdout), result.stdout
        for mode in ([], ["-no-pie"], ["-static"]):
            require(invoke([*options, *mode, program, *objects, "-o", binary]), f"{cc} lifecycle {mode}")
            result = invoke([*run, binary])
            require(result, f"{cc} run lifecycle {mode}")
            assert result.stdout == "NABDECMcedban", result.stdout
        require(invoke([*prefix, "-shared", *objects, "-o", library]), f"{cc} build lifecycle DSO")
        require(invoke([*options, program, library, "-o", binary]), f"{cc} link lifecycle DSO")
        result = invoke([*run, binary])
        require(result, f"{cc} run with lifecycle DSO")
        assert result.stdout == "NABDECMcedban", result.stdout
        require(invoke([*options, loader, "-ldl", "-o", binary]), f"{cc} build lifecycle loader")
        result = invoke([*run, binary, library])
        require(result, f"{cc} dlopen and dlclose lifecycle DSO")
        assert result.stdout == "[NABDECMcedban]", result.stdout

        for attr, argument in (("constructor", "-1"), ("destructor", "65536"),
                               ("constructor", "1.5"), ("destructor", "variable")):
            source = f"int variable; __attribute__(({attr}({argument}))) void f(void) {{}}\n"
            require(invoke([*options, "-x", "c", "-c", "-", "-o", root / "invalid-lifecycle.o"], source),
                    f"{cc} reject {attr} priority {argument}", success=False)

    if not target:
        for cc, other in ((mcc, gcc), (gcc, mcc)):
            # Use both assemblers' unsuffixed and numbered input sections with each linker.
            left, right = products[cc], products[other]
            require(invoke([cc, "-std=c11", "-pedantic-errors", program, left[0], right[1], left[2],
                            "-o", binary]), f"{cc} link mixed lifecycle objects")
            result = invoke([binary])
            require(result, f"{cc} run mixed lifecycle objects")
            assert result.stdout == "NABDECMcedban", result.stdout


def cleanup_attributes(mcc, gcc, root, target, runner):
    source = root / "cleanup.c"
    source.write_text("""
#if !__has_attribute(cleanup)
#error missing cleanup attribute
#endif
#define CLEAN(f) __attribute__((__cleanup__(f)))
int putchar(int);
static int record(int *p) { putchar(*p); return *p = 0; }
static void mark(void *p) { (void)p; putchar('x'); }
struct packet { int words[12]; };
static void wipe(struct packet *p) { putchar(p->words[0]); p->words[0] = 0; }
static void vla_done(void *p) { putchar(*(int *)p); }
static void block(void) {
    int a CLEAN(record) = 'a', b CLEAN(record) = 'b';
    { int c CLEAN(record) = 'c'; putchar('N'); }
}
static void loop(void) {
    for (int guard CLEAN(record) = 'L', i = 0; i < 4; ++i) {
        int each CLEAN(record) = '1' + i;
        if (i == 0) continue;
        if (i == 1) break;
    }
}
static struct packet return_packet(void) {
    int local CLEAN(record) = 'd';
    struct packet p CLEAN(wipe) = {{'e'}};
    putchar('R');
    return p;
}
static void jump(void) {
    int iteration = 0;
again: ;
    int value CLEAN(record) = 'u' + iteration;
    if (iteration++ == 0) goto again;
    { int inner CLEAN(record) = 'g'; goto out; }
out:
    putchar('G');
}
static void enter(int key) {
    goto inside;
    {
        int skipped CLEAN(mark) = (putchar('!'), 0);
inside:
        putchar('J');
    }
    switch (key) {
    default: ;
        int skipped CLEAN(mark) = (putchar('!'), 0);
    case 1:
        putchar('S');
        break;
    }
}
static void vla(int n) {
    int outer CLEAN(record) = 'W';
    {
        int array[n] CLEAN(vla_done);
        array[0] = 'V';
        goto end;
    }
end:
    putchar('Y');
}
static void declarators(void) {
    CLEAN(record) int prefix CLEAN(mark) = 'P';
    int (CLEAN(record) parenthesized) = 'Q';
    int * CLEAN(mark) pointer = 0;
}
int main(void) {
    block(); putchar('|');
    loop(); putchar('|');
    if (return_packet().words[0] != 'e') return 1;
    putchar('|');
    jump(); putchar('|');
    enter(1); putchar('|');
    vla(3); putchar('|');
    declarators();
    return 0;
}
""")
    gnu = root / "cleanup-gnu.c"
    gnu.write_text("""
#define CLEAN(f) __attribute__((cleanup(f)))
#if defined(__aarch64__)
# define JUMP "b "
#else
# define JUMP "jmp "
#endif
int putchar(int);
static void record(int *value) { putchar(*value); }
static void indirect(int choose) {
    void *target = choose ? &&outside : &&inside;
    {
        int x CLEAN(record) = 'C';
        goto *target;
inside:
        putchar('I');
    }
outside:
    putchar('O');
}
static void nonlocal(void) {
    __label__ target;
    int a CLEAN(record) = 'A';
    {
        int b CLEAN(record) = 'B';
        void leave(void) {
            int c CLEAN(record) = 'C';
            goto target;
        }
        leave();
    }
target:
    putchar('T');
}
static void nested(void) {
    int base = 'D';
    void print(int *p) { putchar(base + *p); }
    int x CLEAN(print) = 2;
}
struct packet { int words[12]; };
static void wipe(struct packet *p) { putchar(p->words[0]); p->words[0] = 0; }
static int expression(void) {
    struct packet p = ({ struct packet inner CLEAN(wipe) = {{'E'}}; inner; });
    return p.words[0] != 'E';
}
static void assembly_jump(void) {
    {
        int x CLEAN(record) = 'U';
        __asm__ goto(JUMP "%l0" : : : : end);
    }
end:
    putchar('Z');
}
int main(void) {
    indirect(1); indirect(0); putchar('|');
    nonlocal(); putchar('|');
    nested(); putchar('|');
    if (expression()) return 1;
    putchar('|'); assembly_jump();
    return 0;
}
""")
    binary = root / "cleanup"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        run = runner if cc == mcc else []
        for file, dialect, expected in (
            (source, "-std=c11", "Ncba|12L|Red|ugGv|JxSx|VYW|xQP"),
            (gnu, "-std=gnu11", "OICO|TA|F|E|UZ"),
        ):
            options = ["-pedantic-errors"] if file == source else []
            require(invoke([cc, *flags, dialect, *options, file, "-o", binary]),
                    f"{cc} compile {file.name}")
            result = invoke([*run, binary])
            require(result, f"{cc} cleanup execution in {dialect}")
            assert result.stdout == expected, (cc, file, result.stdout)

        syntax = ["-x", "c"] if cc == gcc else []
        for declarations, callback in (
            ("", "undeclared"),
            ("void callback(void);", "callback"),
            ("void callback(double *);", "callback"),
            ("void (*callback)(int *);", "callback"),
            ("void callback(int *);", "&callback"),
            ("void callback(int *);", ""),
        ):
            code = f"{declarations} void f(void) {{ int x __attribute__((cleanup({callback}))); }}"
            require(invoke([cc, *flags, *syntax, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"], code),
                    f"{cc} reject invalid cleanup declaration {code}", success=False)
        register_source = """
void callback(int *);
void f(void) { register int x __attribute__((cleanup(callback))); }
"""
        require(invoke([cc, *flags, *syntax, "-std=c11", "-fsyntax-only", "-"], register_source),
                f"{cc} reject cleanup on a register variable", success=False)
        ignored = """
void callback(int *);
int global __attribute__((cleanup(callback)));
struct record { int field __attribute__((cleanup(callback))); };
void f(int parameter __attribute__((cleanup(callback)))) {
    static int local __attribute__((cleanup(callback)));
    typedef int T __attribute__((cleanup(callback)));
}
"""
        result = invoke([cc, *flags, *syntax, "-std=c11", "-fsyntax-only", "-"], ignored)
        require(result, f"{cc} diagnose inapplicable cleanup attributes")
        assert "warning:" in result.stderr and "cleanup" in result.stderr, result.stderr


def named_sections(mcc, gcc, root, target, runner):
    provider = root / "section-provider.c"
    provider.write_text("""
#if !__has_attribute(section)
#error missing section attribute
#endif
#define SEC(n) __attribute__((__section__(n)))
extern int shared SEC("custom_data");
int shared = 12;
const unsigned long first SEC("registry") = 27;
extern const unsigned long __start_registry[], __stop_registry[];
int registered_count(void) { return (int)(__stop_registry - __start_registry); }
int zero SEC("custom_zero");
int bss SEC(".bss.extended");
_Thread_local int tls SEC("custom_tls") = 18;
_Thread_local int tls_zero SEC(".tbss.extended");
const int constant SEC("custom_ro") = 8;
const int before SEC("custom_mixed") = 2;
const int *const address SEC("custom_ptr") = &constant;
int touched;
static void mark(void) { touched = 4; }
void (*startup)(void) SEC(".init_array.00160") = mark;
int read_custom(void) SEC("custom_code");
int read_custom(void) {
    static int visits SEC("custom_visits") = 2;
    return shared + zero + bss + tls + tls_zero + *address + visits++;
}
""")
    second = root / "section-second.c"
    second.write_text('__attribute__((section("registry"))) const unsigned long second = 31;\n'
                      'const int other __attribute__((section("custom_ro"))) = 6;\n'
                      'int after __attribute__((section("custom_mixed"))) = 7;\n')
    raw = root / "section-zero.s"
    raw.write_text("""
.section custom_nobits,"aw",%nobits
.global raw_zero
.type raw_zero,%object
raw_zero:
.zero 4
.size raw_zero, 4
.section .note.GNU-stack,""
""")
    raw_tls = root / "section-tls-zero.s"
    raw_tls.write_text("""
.section custom_tls_nobits,"awT",%nobits
.global raw_tls
.type raw_tls,%tls_object
.balign 32
raw_tls:
.zero 4
.size raw_tls, 4
.section .note.GNU-stack,""
""")
    main_c = root / "section-main.c"
    main_c.write_text("""
extern const unsigned long __start_registry[], __stop_registry[];
extern const unsigned long first, second;
extern const int constant, other;
extern const int *const address;
extern int shared, zero, bss, touched;
extern _Thread_local int tls, tls_zero;
extern unsigned char raw_zero[];
#ifdef CHECK_CUSTOM_TLS_NOBITS
extern _Thread_local unsigned char raw_tls[];
#endif
extern const int before;
extern int after;
int read_custom(void);
int registered_count(void);
int main(void) {
    if (registered_count() != 2 || __stop_registry - __start_registry != 2 || __start_registry[0] != first ||
        __start_registry[1] != second || first != 27 || second != 31) return 1;
    if (constant != 8 || other != 6 || address != &constant || touched != 4) return 2;
    if (zero || bss || tls_zero || tls != 18 || read_custom() != 40) return 3;
    if (raw_zero[0] || before != 2 || after != 7) return 4;
    raw_zero[2] = 3; after++;
#ifdef CHECK_CUSTOM_TLS_NOBITS
    if (raw_tls[0]) return 5;
    raw_tls[2] = 4;
#endif
    zero = 1; bss = 2; tls = 3; tls_zero = 4;
#ifdef CHECK_CUSTOM_TLS_NOBITS
    if (raw_tls[2] != 4) return 6;
#endif
    return shared != 12 || read_custom() != 33 || raw_zero[2] + after != 11;
}
""")
    expected = {"custom_code": ("PROGBITS", "AX"), "custom_data": ("PROGBITS", "WA"),
                "custom_ro": ("PROGBITS", "A"), "custom_zero": ("PROGBITS", "WA"),
                "custom_tls": ("PROGBITS", "WAT"), "registry": ("PROGBITS", "A")}

    def sections(path):
        result = invoke(["readelf", "-SW", path])
        require(result, f"inspect sections in {path.name}")
        layout = {}
        for line in result.stdout.splitlines():
            match = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+([WAXT]+)\s", line)
            if match:
                index, name, kind, flags = match.groups()
                layout[name] = (index, kind, flags)
        return layout

    binary = root / "named-sections"
    lib = root / "libnamed-sections.so"
    all_objects = {}
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        prefix = [cc, *flags]
        options = [*prefix, "-std=c11", "-pedantic-errors", "-fPIC"]
        run = runner if cc == mcc else []
        tool = "mcc" if cc == mcc else "gcc"
        objs = [root / f"section-{part}-{tool}.o" for part in ("provider", "second", "zero")]
        all_objects[cc] = objs
        for c, obj in zip((provider, second), objs):
            require(invoke([*options, "-c", c, "-o", obj]), f"{cc} compile {c.name}")
        require(invoke([*prefix, "-c", raw, "-o", objs[2]]), f"{cc} assemble custom NOBITS")
        extra = []
        definitions = []
        if cc == mcc:
            # GNU ld on the reference host can overlap custom TLS NOBITS and .tbss.
            extra = [root / "section-tls-zero.o"]
            definitions = ["-DCHECK_CUSTOM_TLS_NOBITS"]
            require(invoke([*prefix, "-c", raw_tls, "-o", extra[0]]), "assemble custom TLS NOBITS")
        layout = sections(objs[0])
        for name, kind_flags in expected.items():
            assert layout[name][1:] == kind_flags, layout
        assert layout[".bss.extended"][1:] == ("NOBITS", "WA"), layout
        assert layout[".tbss.extended"][1:] == ("NOBITS", "WAT"), layout
        assert layout[".init_array.00160"][1:] == ("INIT_ARRAY", "WA"), layout
        assert elf_symbols(objs[0])["read_custom"][-1] == layout["custom_code"][0]
        for mode in ([], ["-static"]):
            require(invoke([*options, *mode, *definitions, main_c, *objs, *extra, "-o", binary]),
                    f"{cc} link named sections {mode}")
            require(invoke([*run, binary]), f"{cc} run named sections {mode}")
            layout = sections(binary)
            for name, kind_flags in expected.items():
                assert layout[name][1:] == kind_flags, layout
            assert layout["custom_mixed"][1:] == ("PROGBITS", "WA"), layout
            assert elf_symbols(binary)["before"][-1] == elf_symbols(binary)["after"][-1], layout
            assert layout["custom_nobits"][1:] == ("NOBITS", "WA"), layout
            if cc == mcc:
                assert layout["custom_tls_nobits"][1:] == ("NOBITS", "WAT"), layout
            for name in ("__start_registry", "__stop_registry", "first", "second"):
                assert elf_symbols(binary)[name][-1] == layout["registry"][0], layout
        require(invoke([*prefix, "-shared", *objs, *extra, "-o", lib]), f"{cc} link named-section DSO")
        require(invoke([*options, *definitions, main_c, lib, "-o", binary]),
                f"{cc} link consumer of custom-section DSO")
        require(invoke([*run, binary]), f"{cc} run consumer of custom-section DSO")

    if not target:
        for cc, first_cc, second_cc in ((mcc, gcc, mcc), (gcc, mcc, gcc)):
            require(invoke([cc, "-std=c11", "-pedantic-errors", "-fPIC", main_c,
                            all_objects[first_cc][0], *all_objects[second_cc][1:], "-o", binary]),
                    f"{cc} link mixed named sections")
            require(invoke([binary]), f"{cc} run mixed named sections")

    flags = [f"--target={target}"] if target else []
    quoted = 'int x __attribute__((section("quoted section, elF"))) = 1; int main(void) { return x != 1; }'
    require(invoke([mcc, *flags, "-std=c11", "-", "-o", binary], quoted), "link a quoted section name")
    require(invoke([*runner, binary]), "load a quoted section name")
    result = invoke(["readelf", "-SW", binary])
    require(result, "inspect the quoted output section")
    assert "quoted section, elF" in result.stdout
    for declaration in (
        'void f(void) { int x __attribute__((section("invalid"))); }',
        'int x __attribute__((section("")));',
        'int y __attribute__((section(12)));',
    ):
        require(invoke([mcc, *flags, "-std=c11", "-fsyntax-only", "-"], declaration),
                f"reject invalid section attribute {declaration}", success=False)


def vector_types(mcc, gcc, root, target, runner):
    header = root / "vectors.h"
    header.write_text("""
#if !__has_attribute(vector_size)
#error missing vector_size attribute
#endif
#if !__has_builtin(__builtin_convertvector) || !__has_builtin(__builtin_shuffle) || \
    !__has_builtin(__builtin_shufflevector)
#error missing vector builtins
#endif
#define VECTOR(bytes) __attribute__((__vector_size__(bytes)))
typedef int i4 VECTOR(16);
typedef unsigned u4 VECTOR(16);
typedef unsigned char b4 VECTOR(4);
typedef unsigned short h4 VECTOR(8);
typedef float f1 VECTOR(4);
typedef float f4 VECTOR(16);
typedef double d4 VECTOR(32);
typedef int i2 VECTOR(8);
typedef int i8 VECTOR(32);
typedef long double ld1 VECTOR(16);
struct pair { f4 first, second; };
struct mixed { double real; h4 vector; };
i4 combine(i4 a, i4 b, int unused);
b4 transform(b4 a);
h4 transform_short(h4 a);
f1 narrow(f1 a, int scalar);
struct pair pair_sum(struct pair values, f4 increment);
struct mixed mixed_sum(struct mixed values, h4 increment);
i8 wide_sum(int lead, i8 a, i8 b);
i4 pick(int ignored, ...);
i4 spilled(i4 a, i4 b, i4 c, i4 d, i4 e, i4 f, i4 g, i4 h, i4 ninth);
""")
    provider = root / "vector-provider.c"
    provider.write_text("""
#include "vectors.h"
i4 combine(i4 a, i4 b, int unused) { (void)unused; return (a - b) * 2 + 1; }
b4 transform(b4 a) { return a + 1; }
h4 transform_short(h4 a) { return a << 1; }
f1 narrow(f1 a, int scalar) { return a + (float)scalar; }
struct pair pair_sum(struct pair values, f4 increment) {
    values.first += increment; values.second -= increment; return values;
}
struct mixed mixed_sum(struct mixed values, h4 increment) {
    values.real += 0.5; values.vector += increment; return values;
}
i8 wide_sum(int lead, i8 a, i8 b) { return a + b + lead; }
i4 pick(int ignored, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, ignored);
    f4 v = __builtin_va_arg(args, f4);
    i4 result = __builtin_va_arg(args, i4);
    __builtin_va_end(args);
    return result + (int)v[3];
}
i4 spilled(i4 a, i4 b, i4 c, i4 d, i4 e, i4 f, i4 g, i4 h, i4 ninth) {
    return a + b + c + d + e + f + g + h + ninth;
}
""")
    main_c = root / "vector-main.c"
    main_c.write_text("""
#include "vectors.h"
_Static_assert(sizeof(i4) == 16 && _Alignof(i4) == 16, "short vector");
_Static_assert(sizeof(i8) == 32 && _Alignof(i8) == 16, "wide vector");
_Static_assert(sizeof(b4) == 4 && _Alignof(b4) == 4, "narrow vector");
static int samples[2] VECTOR(16) = {{10, 20, 30, 40}, {5, 6}};
static struct { unsigned vector VECTOR(16); } table = {{3, 7}};
static int vector_calls, mask_calls;
static i4 once(i4 v) { ++vector_calls; return v; }
static i4 indices(int first) { ++mask_calls; return (i4){first, 0, 10, 5}; }
#ifdef __MCC__
static ld1 quad_sum(ld1 a, ld1 b) { return a + b; }
#endif
int main(int argc, char **argv) {
    (void)argv;
    i4 a = samples[0], b = {1, 2, 3, 4};
    int *middle = &samples[0][2];
    a[argc] += 4;
    *middle = 98;
    i4 v = combine(a, b, 99);
    if (v[0] != 19 || v[1] != 45 || v[2] != 55 || v[3] != 73 || samples[0][2] != 98)
        return 1;
    if (samples[1][3] || table.vector[1] != 7 || table.vector[2]) return 2;
    const i4 p = {-2, 4, 6, 17}, q = {-3, 4, 8, 1};
    i4 compare = p >= q;
    i4 bits = ~(v & 3) ^ -(v < 20);
    if (compare[0] != -1 || compare[1] != -1 || compare[2] || compare[3] != -1 ||
        bits[0] != -3 || bits[1] != -2 || bits[2] != -4 || bits[3] != -2) return 3;
    u4 cast = (u4)p;
    if (cast[0] != (unsigned)-2 || ((i4)cast)[3] != 17) return 4;
    i4 old = v++;
    if (old[0] != 19 || v[0] != 20 || v[3] != 74 || (0 ? old : v)[0] != 20) return 5;
    b4 byte = transform((b4){255, 0, 1, 2});
    h4 half = transform_short((h4){1, 300, 3, 4});
    if (byte[0] || byte[2] != 2 || half[1] != 600) return 6;
    f1 single = narrow((f1){1.5f}, argc);
    f4 f = {1, 2, 3, 4}, g = {0.5f, -2, 8, 1};
    struct pair result = pair_sum((struct pair){f, g}, g);
    struct mixed mixed = mixed_sum((struct mixed){2.5, {1, 2}}, half);
    if (single[0] != 2.5f || result.first[1] || result.first[2] != 11 ||
        result.second[1] || mixed.real != 3 || mixed.vector[1] != 602) return 7;
#ifdef __MCC__
    ld1 extended = quad_sum((ld1){1.25L}, (ld1){1.25L});
    if (extended[0] != 2.5L) return 8;
#endif
    i8 wide = wide_sum(argc, (i8){1,2,3,4,5,6,7,8}, (i8){10,20,30,40,50,60,70,80});
    if (wide[0] != 12 || wide[4] != 56 || wide[7] != 89) return 9;
    i4 arg = pick(0, f, a);
    i4 nine = spilled(b, b, b, b, b, b, b, b, a);
    if (arg[0] != 14 || arg[3] != 44 || nine[0] != 18 || nine[3] != 72) return 10;
    d4 precise = __builtin_convertvector(g, d4);
    i4 converted = __builtin_convertvector(precise, i4);
    f4 from_short = __builtin_convertvector(half, f4);
    if (precise[0] != 0.5 || converted[0] || converted[1] != -2 ||
        converted[2] != 8 || from_short[1] != 600.0f) return 11;
    i4 one = __builtin_shuffle(once(a), indices(-argc));
    i4 two = __builtin_shuffle(once(a), once(b), indices(-argc));
    i2 compact = __builtin_shufflevector(a, wide, 4 + 4, 1);
    i4 ignore = __builtin_shufflevector(once(a), once(b), -1, 3, 5, 7);
    if (one[0] != 40 || one[2] != 30 || one[3] != 24 ||
        two[0] != 4 || two[1] != 10 || two[2] != 30 || two[3] != 2 ||
        compact[0] != 56 || compact[1] != 24 || ignore[1] != 40 ||
        ignore[2] != 2 || ignore[3] != 4 || vector_calls != 5 || mask_calls != 2) return 12;
    return 0;
}
""")
    products = {}
    binary = root / "vectors"
    for cc in (mcc, gcc):
        tool = "mcc" if cc == mcc else "gcc"
        flags = [f"--target={target}"] if cc == mcc and target else []
        prefix = [cc, *flags, "-std=c11", "-pedantic-errors"]
        objs = [root / f"vectors-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objs
        for source, obj in zip((provider, main_c), objs):
            require(invoke([*prefix, "-c", source, "-o", obj]), f"{cc} compile {source.name}")
        require(invoke([cc, *flags, *objs, "-o", binary]), f"{cc} link vector ABI")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} execute vector operations and ABI")
    if not target:
        for cc, other in ((mcc, gcc), (gcc, mcc)):
            require(invoke([cc, products[cc][0], products[other][1], "-o", binary]),
                    f"{cc} link mixed vector ABI")
            require(invoke([binary]), f"{cc} run mixed vector ABI")
    flags = [f"--target={target}"] if target else []
    for invalid in (
        "typedef _Bool wrong VECTOR(16);",
        "typedef int wrong VECTOR(12);",
        "typedef long wrong VECTOR(4);",
        "i4 wrong(i4 v, long s) { return v + s; }",
        "i4 wrong(i4 v, h4 w) { return v + w; }",
        "i4 wrong(i4 v) { return __builtin_convertvector(v, i2); }",
        "i4 wrong(i4 v, f4 m) { return __builtin_shuffle(v, m); }",
        "i4 wrong(i4 v, int i) { return __builtin_shufflevector(v, v, 0, 1, i, 3); }",
        "i4 wrong(i4 v) { return __builtin_shufflevector(v, v, 0, 1, 2, 8); }",
        "i4 wrong(i4 v) { return __builtin_shufflevector(v, v, 0, 1, 2); }",
    ):
        source = '#include "vectors.h"\n' + invalid
        require(invoke([mcc, *flags, "-std=gnu11", "-I", root, "-fsyntax-only", "-"], source),
                f"reject invalid vector declaration or operation {invalid}", success=False)


def machine_modes(mcc, gcc, root, target, runner):
    header = root / "machine-modes.h"
    header.write_text("""
#if !__has_attribute(mode)
#error missing mode attribute
#endif
#define MODE(name) __attribute__((__mode__(name)))
typedef signed int s8 MODE(__byte__);
typedef unsigned int u16 MODE(HI);
typedef long i32 MODE(SI);
typedef unsigned short u64 MODE(__word__);
typedef unsigned int u128 MODE(TI);
typedef int *intptr MODE(__pointer__);
typedef int (*callback)(int);
typedef callback MODE(pointer) callback_mode;
typedef float scalar MODE(DF);
#ifdef __x86_64__
typedef float extended MODE(XF);
#else
typedef float extended MODE(TF);
#endif
typedef float _Complex cplx MODE(DC);
__extension__ typedef double _Complex pair MODE(CHI);
typedef unsigned MODE(V4SI) u4;
enum compact { low = 0, high = 239 } MODE(QI);
enum MODE(HI) signed_enum { bottom = -107, top = 500 };
typedef enum compact compact_word MODE(SI);
typedef enum compact compact_word_again MODE(SI);
struct payload {
    s8 tiny;
    unsigned half MODE(HI);
    enum signed_enum member MODE(SI);
    unsigned limit : 9 MODE(HI);
};
struct payload change(struct payload value, int extra MODE(HI));
u128 step(u128 x, u64 y);
cplx combine(cplx x, scalar y);
extended sum(extended a, extended b);
u4 reverse_pairs(u4 a);
""")
    provider = root / "machine-modes-provider.c"
    provider.write_text("""
#include "machine-modes.h"
struct payload change(struct payload value, int extra MODE(HI)) {
    value.tiny += extra;
    value.half += 100;
    value.member -= 2;
    value.limit += 3;
    return value;
}
u128 step(u128 x, u64 y) { return (x << 13) | y; }
cplx combine(cplx x, scalar y) { return x + y; }
extended sum(extended a, extended b) { return a + b; }
u4 reverse_pairs(u4 a) { return (u4){a[1], a[0], a[3], a[2]}; }
""")
    main_c = root / "machine-modes-main.c"
    main_c.write_text("""
#include "machine-modes.h"
typedef int explicit_byte MODE(QI);
typedef unsigned int second_word MODE(DI);
typedef int (__attribute__((mode(HI))) *short_pointer);
typedef int * MODE(pointer) pointer_level;
typedef int * __attribute__((vector_size(16))) pointer_level_vector;
typedef int __attribute__((vector_size(8))) short_vector MODE(HI);
typedef int short_vector_again __attribute__((mode(HI), vector_size(8)));
typedef int *vector_pointer MODE(pointer) __attribute__((vector_size(16)));
typedef const int small_const MODE(QI);
static small_const tiny = -7;
_Static_assert(sizeof(explicit_byte) == 1 && sizeof(u16) == 2 && sizeof(i32) == 4, "integer widths");
_Static_assert(sizeof(u64) == 8 && sizeof(u128) == 16, "wide modes");
_Static_assert(__builtin_types_compatible_p(second_word, unsigned long), "canonical DI");
_Static_assert(__builtin_types_compatible_p(explicit_byte, signed char), "canonical QI");
_Static_assert(__builtin_types_compatible_p(scalar, double), "canonical DF");
_Static_assert(__builtin_types_compatible_p(extended, long double), "canonical extended");
_Static_assert(__builtin_types_compatible_p(cplx, double _Complex) && sizeof(pair) == 4, "complex modes");
_Static_assert(__builtin_types_compatible_p(intptr, int *) &&
               __builtin_types_compatible_p(pointer_level, int *) &&
               __builtin_types_compatible_p(callback_mode, callback), "pointer modes");
_Static_assert(sizeof(*(short_pointer)0) == 2 && sizeof(*(vector_pointer)0) == 16, "nested attributes");
_Static_assert(__builtin_types_compatible_p(pointer_level_vector, vector_pointer), "pointer level vector");
_Static_assert(sizeof(short_vector) == 8 && sizeof(((short_vector){0})[0]) == 2, "mode before vector_size");
_Static_assert(__builtin_types_compatible_p(short_vector, short_vector_again), "attribute order");
_Static_assert(sizeof(int MODE(HI)) == 2, "abstract type");
_Static_assert(sizeof(enum compact) == 1 && sizeof(enum signed_enum) == 2 && sizeof(high) == 4,
               "enum definition attributes");
_Static_assert(__builtin_types_compatible_p(enum compact, unsigned char), "unsigned enum");
_Static_assert(__builtin_types_compatible_p(enum signed_enum, short), "signed enum");
_Static_assert(sizeof(compact_word) == 4 &&
               __builtin_types_compatible_p(compact_word, compact_word_again) &&
               !__builtin_types_compatible_p(compact_word, enum compact) &&
               !__builtin_types_compatible_p(compact_word, unsigned int), "enum mode variant");
int main(void) {
    struct payload out = change((struct payload){-61, 800, top, 137}, 12);
    if (out.tiny != -49 || out.half != 900 || out.member != 498 || out.limit != 140 || tiny != -7)
        return 1;
    if (step(((u128)1 << 100) | 3, 25) != (((u128)1 << 113) | (3 << 13) | 25))
        return 2;
    cplx z = combine(__builtin_complex(1.5, -3.0), 2.25);
    if (__real__ z != 3.75 || __imag__ z != -3.0 || sum(2.5L, -1.25L) != 1.25L)
        return 3;
    u4 r = reverse_pairs((u4){29, 84, 300, 18});
    return r[0] != 84 || r[1] != 29 || r[2] != 18 || r[3] != 300;
}
""")
    products = {}
    binary = root / "machine-modes"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        tool = "mcc" if cc == mcc else "gcc"
        objs = [root / f"modes-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objs
        for c_file, obj in zip((provider, main_c), objs):
            require(invoke([cc, *flags, "-std=c11", "-pedantic-errors", "-c", c_file, "-o", obj]),
                    f"{cc} compile {c_file.name}")
        require(invoke([cc, *flags, *objs, "-o", binary]), f"{cc} link mode attributes")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run mode attributes")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, products[provider_cc][0], products[client_cc][1], "-o", binary]),
                    f"link modes with {provider_cc} provider")
            require(invoke([binary]), f"run modes with {provider_cc} provider")
    for ignored in ('int x __attribute__((mode(1 + 2)));', 'int x __attribute__((mode("HI")));'):
        source = ignored + ' _Static_assert(sizeof(x) == sizeof(int), "ignored mode");'
        for cc in (mcc, gcc):
            flags = (["-x", "c"] if cc == gcc else [f"--target={target}"] if target else [])
            result = invoke([cc, *flags, "-std=gnu11", "-fsyntax-only", "-"], source)
            require(result, f"{cc} ignores {ignored}")
            assert "warning:" in result.stderr
    for invalid in (
        "int x __attribute__((mode));",
        "int x __attribute__((mode(HI, SI)));",
        "int x __attribute__((mode(no_such_mode)));",
        "float x __attribute__((mode(HI)));",
        "_Bool x __attribute__((mode(QI)));",
        "int *p __attribute__((mode(HI)));",
        "int x[4] __attribute__((mode(HI)));",
        "__attribute__((mode(HI))) int x(void);",
        "struct __attribute__((mode(SI))) s { int value; };",
        "enum __attribute__((mode(QI))) e { A = 256 };",
        "int v __attribute__((vector_size(16), mode(SI)));",
        "int __attribute__((mode(HI))) v __attribute__((vector_size(16)));",
    ):
        for cc in (mcc, gcc):
            flags = (["-x", "c"] if cc == gcc else [f"--target={target}"] if target else [])
            require(invoke([cc, *flags, "-std=gnu11", "-fsyntax-only", "-"], invalid),
                    f"{cc} rejects {invalid}", success=False)


def half_precision(mcc, gcc, root, target, runner):
    header = root / "half.h"
    header.write_text("""
#if __FLT16_MANT_DIG__ != 11 || __FLT16_MIN_EXP__ != -13 || __FLT16_HAS_DENORM__ != 1
#error wrong binary16 macros
#endif
typedef _Float16 half;
typedef float hf __attribute__((mode(HF)));
typedef _Complex _Float16 complex_half;
typedef float _Complex complex_hf __attribute__((mode(HC)));
typedef half half1 __attribute__((vector_size(2)));
typedef half half2 __attribute__((vector_size(4)));
typedef half half4 __attribute__((vector_size(8)));
typedef float mode_half4 __attribute__((mode(V4HF)));
typedef short mask4 __attribute__((vector_size(8)));
struct half3 { half a, b, c; };
struct mixed_half { half a; int b; };
half expression(half a, half b, half c);
half explicit_round(half a, half b, half c);
half conditional_round(int choose, half a, half b, half c);
half comma_round(half a, half b, half c);
half long_double_input(long double a);
long double long_double_output(half a);
struct half3 homogeneous(struct half3 a);
struct mixed_half mixed_half(struct mixed_half a);
complex_half half_complex(complex_half a, complex_half b);
half4 half_vector(half4 a, half b);
half1 narrowest_half(half1 a);
half2 narrow_half(half2 a);
half ninth_half(half a, half b, half c, half d, half e, half f, half g, half h, half i);
half get_half(int ignored, ...);
#ifdef __aarch64__
__fp16 storage_sum(__fp16 a, __fp16 b);
double storage_va(int ignored, ...);
#endif
""")
    provider = root / "half-provider.c"
    provider.write_text("""
#include "half.h"
half expression(half a, half b, half c) { return a + b + c; }
half explicit_round(half a, half b, half c) { return (half)(a + b) + c; }
half conditional_round(int choose, half a, half b, half c) { return (choose ? a + b : a) + c; }
half comma_round(half a, half b, half c) { return ((void)0, a + b) + c; }
half long_double_input(long double a) { return (half)a; }
long double long_double_output(half a) { return a; }
struct half3 homogeneous(struct half3 a) { return (struct half3){a.b, a.c, a.a}; }
struct mixed_half mixed_half(struct mixed_half a) { a.a -= 0.5F16; a.b += 2; return a; }
complex_half half_complex(complex_half a, complex_half b) { return a + b; }
half4 half_vector(half4 a, half b) { return a + b; }
half1 narrowest_half(half1 a) { return a; }
half2 narrow_half(half2 a) { return a; }
half ninth_half(half a, half b, half c, half d, half e, half f, half g, half h, half i) {
    return a + b + c + d + e + f + g + h + i;
}
half get_half(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    half a = __builtin_va_arg(ap, half);
    double b = __builtin_va_arg(ap, double);
    half c = __builtin_va_arg(ap, half);
    __builtin_va_end(ap);
    return a + (half)b + c;
}
#ifdef __aarch64__
__fp16 storage_sum(__fp16 a, __fp16 b) { return a + b; }
double storage_va(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    double a = __builtin_va_arg(ap, double);
    __builtin_va_end(ap);
    return a;
}
#endif
""")
    main_c = root / "half-main.c"
    main_c.write_text("""
#include "half.h"
typedef unsigned int wide __attribute__((mode(TI)));
typedef union { half value; unsigned short bits; } representation;
static half table[] = {
    0.333251953125F16, -0.0f16, __FLT16_DENORM_MIN__, __FLT16_MAX__,
    0x1.002p0f16, 0x1.006p0F16, (half)65520.0L, (half)(((wide)1) << 100),
    2048.0F16 + 1.0F16 - 2048.0F16
};
static int rounded_integer = (int)(half)255.97;
static wide converted_wide = (wide)533.75F16;
static wide narrowed_wide = (wide)(half)533.75F16;
_Static_assert(sizeof(half) == 2 && _Alignof(half) == 2, "binary16 layout");
_Static_assert(__builtin_types_compatible_p(hf, half), "HF mode");
_Static_assert(__builtin_types_compatible_p(complex_hf, complex_half), "HC mode");
_Static_assert(__builtin_types_compatible_p(mode_half4, half4), "HF vector mode");
_Static_assert(__builtin_types_compatible_p(__typeof__((half)0 + (half)1), half), "semantic half type");
_Static_assert(__builtin_types_compatible_p(__typeof__((half)0 * 3.0f), float), "mixed type");
#ifdef __aarch64__
_Static_assert(!__builtin_types_compatible_p(__fp16, half) && sizeof(__fp16) == 2, "Arm storage type");
_Static_assert(__builtin_types_compatible_p(__typeof__((__fp16)0 + (half)1), float), "Arm promotion");
#endif
static unsigned short bits(half value) { return ((representation){.value = value}).bits; }
int main(void) {
    if (bits(table[0]) != 0x3555 || bits(table[1]) != 0x8000 || bits(table[2]) != 1 ||
        bits(table[3]) != 0x7bff || bits(table[4]) != 0x3c00 || bits(table[5]) != 0x3c02 ||
        bits(table[6]) != 0x7c00 || bits(table[7]) != 0x7c00 || table[8] != 1 ||
        rounded_integer != 256 || converted_wide != 533 || narrowed_wide != 534)
        return 1;
    if (expression(2048, 1, -2048) != 1 || explicit_round(2048, 1, -2048) != 0 ||
        conditional_round(1, 2048, 1, -2048) != 1 || conditional_round(0, 2048, 1, -2048) ||
        comma_round(2048, 1, -2048) != 1)
        return 2;
    if (bits(long_double_input(0x1.002000000000001p0L)) != 0x3c01 ||
        bits(long_double_input(0x1.002p0L)) != 0x3c00 ||
        long_double_output(table[2]) != 0x1p-24L ||
        bits(long_double_input(-0x1p-26L)) != 0x8000)
        return 3;
    struct half3 h = homogeneous((struct half3){2.0F16, -3.0F16, 5.0F16});
    struct mixed_half m = mixed_half((struct mixed_half){9.5F16, 77});
    if (h.a != -3 || h.b != 5 || h.c != 2 || m.a != 9 || m.b != 79)
        return 4;
    complex_half z = half_complex(__builtin_complex(2.0F16, -3.0F16), __builtin_complex(1.5F16, 2.0F16));
    half4 v = half_vector((half4){10, -2, 0, 4}, (half)1.5f);
    half1 narrowest = narrowest_half((half1){-2.75F16});
    half2 narrow = narrow_half((half2){12.5F16, -6.5F16});
    mask4 mask = v > (half)0;
    if (__real__ z != 3.5 || __imag__ z != -1 || v[1] != -0.5 || v[2] != 1.5 ||
        mask[0] != -1 || mask[1] != 0 || mask[3] != -1 ||
        narrowest[0] != -2.75 || narrow[0] != 12.5 || narrow[1] != -6.5)
        return 5;
    if (ninth_half(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45 ||
        get_half(0, (half)1.25, 0.5, (half)-0.25) != 1.5)
        return 6;
    half nan = ((representation){.bits = 0x7e01}).value;
    half small = table[2];
    if (nan == nan || !(small * small) || (half)(small * small) || bits(-table[1]) != 0)
        return 7;
#ifdef __aarch64__
    if (storage_sum((__fp16)2.0, (__fp16)0.5) != 2.5 || storage_va(0, (__fp16)0.75) != 0.75)
        return 8;
#endif
    return 0;
}
""")
    products = {}
    binary = root / "half-precision"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        tool = "mcc" if cc == mcc else "gcc"
        objs = [root / f"half-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objs
        for c_file, obj in zip((provider, main_c), objs):
            require(invoke([cc, *flags, "-std=gnu11", "-c", c_file, "-o", obj]),
                    f"{cc} compile {c_file.name}")
        require(invoke([cc, *flags, *objs, "-o", binary]), f"{cc} link half precision")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run half precision")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, products[provider_cc][0], products[client_cc][1], "-o", binary]),
                    f"link half precision with {provider_cc} provider")
            require(invoke([binary]), f"run half precision with {provider_cc} provider")
    rounding = root / "half-rounding.c"
    rounding.write_text("""
#include <fenv.h>
typedef float half __attribute__((mode(HF)));
typedef union { half value; unsigned short bits; } representation;
int main(void) {
    volatile long double positive = 0x1.002p0L, negative = -0x1.002p0L;
    volatile long double subnormal = 0x1p-26L, large = 65536.0L;
    if (fesetround(FE_UPWARD) || feclearexcept(FE_ALL_EXCEPT)) return 1;
    representation a = {.value = (half)positive}, b = {.value = (half)subnormal};
    if (a.bits != 0x3c01 || b.bits != 1 || !fetestexcept(FE_INEXACT) ||
        !fetestexcept(FE_UNDERFLOW)) return 2;
    if (fesetround(FE_DOWNWARD) || feclearexcept(FE_ALL_EXCEPT)) return 3;
    a.value = (half)negative;
    b.value = (half)large;
    return a.bits != 0xbc01 || b.bits != 0x7bff || !fetestexcept(FE_OVERFLOW) ||
           !fetestexcept(FE_INEXACT);
}
""")
    flags = [f"--target={target}"] if target else []
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", rounding, "-lm", "-o", binary]),
            "link binary16 rounding and exception tests")
    require(invoke([*runner, binary]), "run binary16 rounding and exception tests")
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "_Float16 value = 1.0F16;"), "diagnose an unsuppressed C11 extension", success=False)
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "__extension__ _Float16 value = 1.0F16;"), "allow a suppressed C11 extension")


def bfloat16(mcc, gcc, root, target, runner):
    header = root / "bfloat.h"
    header.write_text("""
#if __BFLT16_MANT_DIG__ != 8 || __BFLT16_MAX_EXP__ != 128 || __BFLT16_HAS_DENORM__ != 1
#error wrong bfloat16 macros
#endif
typedef __bf16 bf;
typedef float bm __attribute__((mode(BF)));
typedef _Float16 hf;
typedef unsigned int wide __attribute__((mode(TI)));
typedef float _Complex bc __attribute__((mode(BC)));
typedef float bv __attribute__((mode(V4BF)));
typedef bf bf2 __attribute__((vector_size(4)));
struct bf3 { bf a, b, c; };
struct mixed16 { bf a; hf b; };
struct complex_box { bc value; };
bf add_bf(bf a, bf b, bf c);
bf round_bf(bf a, bf b, bf c);
bf convert_float(float a);
bf convert_double(double a);
bf convert_ext(long double a);
bf convert_half(hf a);
hf convert_back(bf a);
bf convert_wide(wide a);
wide restore_wide(bf a);
struct bf3 rotate_bf(struct bf3 a);
struct mixed16 mixed_formats(struct mixed16 a);
bc complex_bf(bc a, int increment);
struct complex_box boxed_bf(struct complex_box a);
bv vector_bf(bv a, bf b);
bf2 vector_pair(bf2 a);
bf va_bf(int ignored, ...);
""")
    provider = root / "bfloat-provider.c"
    provider.write_text("""
#include "bfloat.h"
bf add_bf(bf a, bf b, bf c) { return (a + b) + c; }
bf round_bf(bf a, bf b, bf c) { return (bf)(a + b) + c; }
bf convert_float(float a) { return a; }
bf convert_double(double a) { return a; }
bf convert_ext(long double a) { return a; }
bf convert_half(hf a) { return a; }
hf convert_back(bf a) { return a; }
bf convert_wide(wide a) { return a; }
wide restore_wide(bf a) { return a; }
struct bf3 rotate_bf(struct bf3 a) { return (struct bf3){a.b, a.c, a.a}; }
struct mixed16 mixed_formats(struct mixed16 a) {
    return (struct mixed16){(bf)(a.a - 1.5f), (hf)(a.b + 2.0f)};
}
bc complex_bf(bc a, int increment) { return a + (bc)increment; }
struct complex_box boxed_bf(struct complex_box a) {
    return (struct complex_box){a.value + (bc)1};
}
bv vector_bf(bv a, bf b) { return a + b; }
bf2 vector_pair(bf2 a) { return a; }
bf va_bf(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    bf a = __builtin_va_arg(ap, bf);
    double b = __builtin_va_arg(ap, double);
    bf c = __builtin_va_arg(ap, bf);
    __builtin_va_end(ap);
    return a + (bf)b + c;
}
""")
    main_c = root / "bfloat-main.c"
    main_c.write_text("""
#include "bfloat.h"
typedef union { bf value; unsigned short bits; } representation;
static unsigned short bits(bf a) { return ((representation){.value = a}).bits; }
static bf constants[] = {
    -0.0bf16, 0x1.01p0BF16, 0x1.03p0bf16, __BFLT16_MIN__,
    __BFLT16_DENORM_MIN__, __BFLT16_MAX__, (__bf16)0x1.ffp127L,
    (__bf16)(((wide)1 << 100) + ((wide)1 << 92) + 1), (__bf16)~(wide)0
};
static float literal_excess = 0x1.01p0bf16;
static float literal_rounded = (bf)0x1.01p0bf16;
static bc imaginary = 1.5bf16i;
#ifdef __MCC__
static bc add_complex(bc a, bc b) { return a + b; }
static bc va_complex(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    bc a = __builtin_va_arg(ap, bc);
    bc b = __builtin_va_arg(ap, bc);
    __builtin_va_end(ap);
    return a + b;
}
#endif
_Static_assert(sizeof(bf) == 2 && _Alignof(bf) == 2, "bfloat16 layout");
_Static_assert(__builtin_types_compatible_p(bm, bf), "BF mode");
_Static_assert(!__builtin_types_compatible_p(bf, hf), "distinct encodings");
_Static_assert(__builtin_types_compatible_p(__typeof__((bf)1 + 1), bf), "bfloat16 integer arithmetic");
_Static_assert(__builtin_types_compatible_p(__typeof__((bf)1 + (hf)1), hf) &&
               __builtin_types_compatible_p(__typeof__((hf)1 + (bf)1), hf), "mixed ordering");
_Static_assert(__builtin_types_compatible_p(__typeof__((bf)1 / 2.0f), float), "single precision");
int main(void) {
    unsigned short expected[] = {0x8000, 0x3f80, 0x3f82, 0x0080, 1, 0x7f7f, 0x7f80, 0x7181, 0x7f80};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; ++i)
        if (bits(constants[i]) != expected[i]) return 1;
    if (literal_excess != 0x1.01p0f || literal_rounded != 1.0f ||
        add_bf(256, 1, -256) != 1 || round_bf(256, 1, -256))
        return 2;
    if (bits(convert_float(0x1.010002p0f)) != 0x3f81 ||
        bits(convert_double(0x1.0100000000001p0)) != 0x3f81 ||
        bits(convert_ext(0x1.010000000000001p0L)) != 0x3f81 ||
        bits(convert_half(1.0078125F16)) != 0x3f81 ||
        convert_back(1.0078125BF16) != 1.0078125F16)
        return 3;
    wide wide_input = ((wide)1 << 100) + ((wide)1 << 92) + 1;
    if (bits(convert_wide(wide_input)) != 0x7181 ||
        restore_wide(0x1.02p96BF16) != (((wide)1 << 96) + ((wide)1 << 89)))
        return 4;
    struct bf3 h = rotate_bf((struct bf3){2, -3, 5});
    struct mixed16 mixed = mixed_formats((struct mixed16){8.5bf16, -9.25f16});
    if (h.a != -3 || h.b != 5 || h.c != 2 || mixed.a != 7 || mixed.b != -7.25)
        return 5;
    bc z = complex_bf(imaginary, 2);
    struct complex_box box = boxed_bf((struct complex_box){z});
    bv v = vector_bf((bv){12, -5.5bf16, 0, 3}, (bf)1.5f);
    bf2 pair = vector_pair((bf2){2.5bf16, -7.5bf16});
    if (__real__ z != 2 || __imag__ z != 1.5 ||
        __real__ box.value != 3 || __imag__ box.value != 1.5 ||
        v[0] != 13.5 || v[1] != -4 || v[2] != 1.5 ||
        pair[0] != 2.5 || pair[1] != -7.5)
        return 6;
#ifdef __MCC__
    bc multiple = add_complex((bc)2, imaginary);
    bc variadic = va_complex(0, (bc)2, imaginary);
    if (__real__ multiple != 2 || __imag__ multiple != 1.5 ||
        __real__ variadic != 2 || __imag__ variadic != 1.5)
        return 7;
#endif
    bf nan = ((representation){.bits = 0x7fc1}).value;
    return va_bf(0, (bf)1.25, 2.0, (bf)-0.75) != 2.5 || nan == nan ||
           bits(-constants[0]) != 0 || constants[4] == 0;
}
""")
    products = {}
    binary = root / "bfloat-test"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        tool = "mcc" if cc == mcc else "gcc"
        objs = [root / f"bfloat-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objs
        for c_file, obj in zip((provider, main_c), objs):
            require(invoke([cc, *flags, "-std=gnu11", "-c", c_file, "-o", obj]),
                    f"{cc} compile {c_file.name}")
        require(invoke([cc, *flags, *objs, "-o", binary]), f"{cc} link bfloat16")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run bfloat16")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, products[provider_cc][0], products[client_cc][1], "-o", binary]),
                    f"link bfloat16 with {provider_cc} provider")
            require(invoke([binary]), f"run bfloat16 with {provider_cc} provider")
    flags = [f"--target={target}"] if target else []
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "__bf16 value = 1.0;"), "recognize the reserved bfloat16 type")
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "__bf16 value = 1.0bf16;"), "diagnose a bfloat16 literal in strict C11", success=False)
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "__extension__ __bf16 value = 1.0bf16;"), "allow a suppressed C11 extension")

    precision = root / "float16-excess.c"
    precision.write_text("""
typedef __bf16 bf;
typedef _Float16 hf;
volatile bf b = 256, one_bf = 1;
volatile hf h = 2048, one_hf = 1;
volatile float bf_literal = 0x1.01p0bf16, hf_literal = 0x1.002p0f16;
int main(void) {
    return b + one_bf - b != 0 || h + one_hf - h != 0 ||
           b - 257 != 0 || h - 2049 != 0 || bf_literal != 1 || hf_literal != 1;
}
""")
    for cc in (mcc, gcc):
        settings = flags if cc == mcc else []
        require(invoke([cc, *settings, "-std=gnu11", "-fexcess-precision=16", precision, "-o", binary]),
                f"{cc} compile exact-precision bfloat16 and binary16")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run with -fexcess-precision=16")
    rounding = root / "bfloat-rounding.c"
    rounding.write_text("""
#include <fenv.h>
typedef unsigned int wide __attribute__((mode(TI)));
typedef union { __bf16 value; unsigned short bits; } representation;
int main(void) {
    volatile long double positive = 0x1.01p0L, negative = -0x1.01p0L;
    volatile long double tiny = 0x1p-135L, large = 0x1p128L;
    volatile wide midpoint = ((wide)1 << 100) + ((wide)1 << 92);
    if (fesetround(FE_UPWARD) || feclearexcept(FE_ALL_EXCEPT)) return 1;
    representation a = {.value = (__bf16)positive}, b = {.value = (__bf16)tiny};
    representation c = {.value = (__bf16)midpoint};
    if (a.bits != 0x3f81 || b.bits != 1 || c.bits != 0x7181 ||
        !fetestexcept(FE_UNDERFLOW) || !fetestexcept(FE_INEXACT)) return 2;
    if (fesetround(FE_DOWNWARD) || feclearexcept(FE_ALL_EXCEPT)) return 3;
    a.value = (__bf16)negative;
    b.value = (__bf16)large;
    c.value = (__bf16)midpoint;
    return a.bits != 0xbf81 || b.bits != 0x7f7f || c.bits != 0x7180 ||
           !fetestexcept(FE_OVERFLOW) || !fetestexcept(FE_INEXACT);
}
""")
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", rounding, "-lm", "-o", binary]),
            "link bfloat16 rounding tests")
    require(invoke([*runner, binary]), "run bfloat16 rounding and exception tests")


def binary128(mcc, gcc, root, target, runner):
    header = root / "binary128.h"
    header.write_text("""
#if __FLT128_MANT_DIG__ != 113 || __FLT128_MAX_EXP__ != 16384
#error wrong binary128 format macros
#endif
typedef _Float128 quad;
typedef float tf __attribute__((mode(TF)));
typedef _Float64x lx;
typedef _Complex _Float128 cq;
typedef float _Complex tc __attribute__((mode(TC)));
typedef unsigned int wide __attribute__((mode(TI)));
typedef quad q1 __attribute__((vector_size(16)));
typedef quad q2 __attribute__((vector_size(32)));
struct one { quad value; };
struct pair { quad a, b; };
quad add(quad a, quad b);
quad work(quad a, quad b);
int relation(quad a, quad b);
quad extended_to_quad(long double a);
long double quad_to_extended(quad a);
quad wide_to_quad(wide a);
wide quad_to_wide(quad a);
double quad_to_double(quad a);
_Float16 quad_to_half(quad a);
__bf16 quad_to_brain(quad a);
struct one increment(struct one a);
struct pair swap(struct pair a);
cq complex_product(cq a, cq b);
cq complex_divide(cq a, cq b);
q2 vector_add(q2 a, q2 b);
q1 vector_single(q1 a, int increment);
quad spill(quad a, quad b, quad c, quad d, quad e, quad f, quad g, quad h, quad i);
quad get_quad(int ignored, ...);
""")
    provider = root / "binary128-provider.c"
    provider.write_text("""
#include "binary128.h"
quad add(quad a, quad b) { return a + b; }
quad work(quad a, quad b) { return -(a * b - (quad)1) / b; }
int relation(quad a, quad b) {
    return (a<b) + 2*(a<=b) + 4*(a>b) + 8*(a>=b) + 16*(a==b) + 32*(a!=b) + 64*(!a);
}
quad extended_to_quad(long double a) { return a; }
long double quad_to_extended(quad a) { return a; }
quad wide_to_quad(wide a) { return a; }
wide quad_to_wide(quad a) { return a; }
double quad_to_double(quad a) { return a; }
_Float16 quad_to_half(quad a) { return a; }
__bf16 quad_to_brain(quad a) { return a; }
struct one increment(struct one a) { return (struct one){a.value + 1.0f128}; }
struct pair swap(struct pair a) { return (struct pair){a.b, a.a}; }
cq complex_product(cq a, cq b) { return a * b; }
cq complex_divide(cq a, cq b) { return a / b; }
q2 vector_add(q2 a, q2 b) { return a + b; }
q1 vector_single(q1 a, int increment) { return (q1){a[0] + (quad)increment}; }
quad spill(quad a, quad b, quad c, quad d, quad e, quad f, quad g, quad h, quad i) {
    return a+b+c+d+e+f+g+h+i;
}
quad get_quad(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    quad a = __builtin_va_arg(ap, quad);
    double b = __builtin_va_arg(ap, double);
    quad c = __builtin_va_arg(ap, quad);
    __builtin_va_end(ap);
    return a + b + c;
}
""")
    main_c = root / "binary128-main.c"
    main_c.write_text("""
#include "binary128.h"
typedef union { quad value; struct { unsigned long lo, hi; } words; } encoding;
static quad decimal = 1.0000000000000000000000000000000002F128;
static quad exact = 0x1.0000000000000000000000000001p0F128;
static quad folded = ((1.0F128 + __FLT128_EPSILON__) - 1.0F128) * 2.0F128;
static quad extreme[] = {__FLT128_DENORM_MIN__, __FLT128_MAX__, -0.0F128};
static quad cast_wide = (quad)(((wide)1 << 110) + 7);
static wide cast_back = (wide)((quad)(((wide)1 << 110) + 7));
static double cast_double = (double)0x1.0000000000000800000000000001p0F128;
static _Float16 cast_half = (_Float16)0x1.0020000000000000000000000001p0F128;
static __bf16 cast_brain = (__bf16)0x1.0100000000000000000000000001p0F128;
static cq folded_complex = __builtin_complex(1.0F128, 2.0F128) *
                           __builtin_complex(3.0F128, 4.0F128);
static int constant_comparison = 0x1.0000000000000000000000000001p0F128 > 1.0F128;
static unsigned long low(quad value) { return ((encoding){.value = value}).words.lo; }
static unsigned long high(quad value) { return ((encoding){.value = value}).words.hi; }
#ifdef __MCC__
static q1 get_quad_vector(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    q1 value = __builtin_va_arg(ap, q1);
    __builtin_va_end(ap);
    return value;
}
#endif
_Static_assert(sizeof(quad) == 16 && _Alignof(quad) == 16 && sizeof(cq) == 32, "quad layout");
_Static_assert(!__builtin_types_compatible_p(quad, long double) &&
               !__builtin_types_compatible_p(lx, long double) &&
               !__builtin_types_compatible_p(lx, quad), "distinct source types");
_Static_assert(__builtin_types_compatible_p(__typeof__(1.0F64x), lx), "extended literal");
_Static_assert(__builtin_types_compatible_p(__typeof__(1.0L + 1.0F128), quad), "quad rank");
#if defined(__x86_64__)
_Static_assert(__FLT64X_MANT_DIG__ == 64, "x87 precision");
_Static_assert(__builtin_types_compatible_p(tf, quad) && __builtin_types_compatible_p(tc, cq),
               "x86 mode(TF) and mode(TC)");
_Static_assert(__builtin_types_compatible_p(__float128, quad) &&
               __builtin_types_compatible_p(__float80, long double), "x86 GNU aliases");
_Static_assert(__builtin_types_compatible_p(__typeof__(1.0Q), quad) &&
               __builtin_types_compatible_p(__typeof__(1.0W), long double), "x86 GNU suffixes");
typedef float v2tf __attribute__((mode(V2TF)));
_Static_assert(__builtin_types_compatible_p(q2, v2tf), "x86 vector machine mode");
#else
_Static_assert(__FLT64X_MANT_DIG__ == 113, "AArch64 precision");
_Static_assert(__builtin_types_compatible_p(tf, long double) &&
               __builtin_types_compatible_p(tc, long double _Complex), "AArch64 modes");
_Static_assert(__builtin_types_compatible_p(__typeof__(1.0Q), long double), "AArch64 GNU suffix");
#endif
int main(void) {
    if (decimal != exact || low(exact) != 1 || high(exact) != 0x3fff000000000000UL ||
        folded != 0x1p-111f128 || low(extreme[0]) != 1 || high(extreme[0]) ||
        low(extreme[1]) != ~0UL || high(extreme[1]) != 0x7ffeffffffffffffUL ||
        low(extreme[2]) || high(extreme[2]) != 0x8000000000000000UL ||
        !constant_comparison)
        return 1;
    if (add(1, __FLT128_EPSILON__) != exact || work(6, 2) != -5.5F128 ||
        add(extreme[0], extreme[0]) != 0x2p-16494F128 ||
        relation(0, 0) != 90 || relation(1, 2) != 35)
        return 2;
    quad nan = ((encoding){.words = {3, 0x7fff800000000000UL}}).value;
    if (relation(nan, 0) != 32 || relation(0, nan) != 96)
        return 3;
    wide input = ((wide)1 << 110) + 7;
    if (cast_wide != wide_to_quad(input) || cast_back != input || quad_to_wide(cast_wide) != input ||
        cast_double != 0x1.0000000000001p0 || quad_to_double(0x1.0000000000000800000000000001p0F128) != cast_double ||
        cast_half != 0x1.004p0F16 || quad_to_half(0x1.0020000000000000000000000001p0F128) != cast_half ||
        cast_brain != 0x1.02p0BF16 || quad_to_brain(0x1.0100000000000000000000000001p0F128) != cast_brain)
        return 4;
    if (extended_to_quad(1.25L) != 1.25F128 || quad_to_extended(1.125F128) != 1.125L)
        return 5;
#if defined(__x86_64__)
    if (quad_to_extended(0x1.0000000000000001000000000001p0F128) != 0x1.0000000000000002p0L ||
        quad_to_extended(0x1.0000000000000001000000000000p0F128) != 1.0L)
        return 6;
#endif
    struct one o = increment((struct one){__FLT128_EPSILON__});
    struct pair p = swap((struct pair){decimal, extreme[0]});
    cq c = complex_product(__builtin_complex(1.0F128, 2.0F128), __builtin_complex(3.0F128, 4.0F128));
    cq d = complex_divide(c, __builtin_complex(3.0F128, 4.0F128));
    if (o.value != exact || p.a != extreme[0] || p.b != exact ||
        __real__ c != -5 || __imag__ c != 10 || __real__ d != 1 || __imag__ d != 2 ||
        __real__ folded_complex != -5 || __imag__ folded_complex != 10)
        return 7;
    q2 v = vector_add((q2){1, 2}, (q2){__FLT128_EPSILON__, 3});
    q1 single = vector_single((q1){__FLT128_EPSILON__}, 1);
#ifdef __MCC__
    if (get_quad_vector(0, single)[0] != exact) return 8;
#endif
    return v[0] != exact || v[1] != 5 ||
           single[0] != exact || spill(1,2,3,4,5,6,7,8,9) != 45 ||
           get_quad(0, (quad)1, -1.0, __FLT128_EPSILON__) != __FLT128_EPSILON__;
}
""")
    products = {}
    binary = root / "quad-precision"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        tool = "mcc" if cc == mcc else "gcc"
        objs = [root / f"quad-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objs
        for c_file, obj in zip((provider, main_c), objs):
            require(invoke([cc, *flags, "-std=gnu11", "-c", c_file, "-o", obj]),
                    f"{cc} compile {c_file.name}")
        require(invoke([cc, *flags, *objs, "-o", binary]), f"{cc} link binary128")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run binary128")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, products[provider_cc][0], products[client_cc][1], "-o", binary]),
                    f"link binary128 with {provider_cc} provider")
            require(invoke([binary]), f"run binary128 with {provider_cc} provider")
    flags = [f"--target={target}"] if target else []
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "_Float128 value = 1.0F128;"), "diagnose a binary128 extension in strict C11", success=False)
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"],
                   "__extension__ _Float128 value = 1.0F128;"), "allow a suppressed C11 extension")
    rounding = root / "quad-rounding.c"
    rounding.write_text("""
#define __STDC_WANT_IEC_60559_TYPES_EXT__ 1
#include <fenv.h>
#include <math.h>
#include <stdlib.h>
typedef _Float128 quad;
typedef unsigned int wide __attribute__((mode(TI)));
int main(void) {
    volatile quad one = 1.0F128, halfway = 0x1p-113F128, x87_half = 0x1.0000000000000001p0F128;
    volatile wide integer_halfway = ((wide)1 << 120) + ((wide)1 << 7);
    if (strtof128("1.0000000000000000000000000000000002", 0) !=
        0x1.0000000000000000000000000001p0F128 || sqrtf128(4.0F128) != 2.0F128)
        return 1;
    if (fesetround(FE_UPWARD) || feclearexcept(FE_ALL_EXCEPT)) return 2;
    if (one + halfway != 0x1.0000000000000000000000000001p0F128 ||
        (quad)integer_halfway != 0x1.0000000000000000000000000001p120F128 ||
        !fetestexcept(FE_INEXACT))
        return 3;
#ifdef __x86_64__
    if ((long double)x87_half != 0x1.0000000000000002p0L) return 4;
#endif
    if (fesetround(FE_DOWNWARD) || feclearexcept(FE_ALL_EXCEPT)) return 5;
    if (one + halfway != 1.0F128 || (quad)integer_halfway != 0x1p120F128 ||
        !fetestexcept(FE_INEXACT))
        return 6;
#ifdef __x86_64__
    if ((long double)x87_half != 1.0L) return 7;
#endif
    return 0;
}
""")
    require(invoke([mcc, *flags, "-std=gnu11", rounding, "-lm", "-o", binary]),
            "link glibc binary128 APIs and runtime rounding")
    require(invoke([*runner, binary]), "run glibc binary128 APIs and runtime rounding")


def interchange_float(mcc, gcc, root, target, runner):
    header = root / "interchange.h"
    header.write_text("""
typedef _Float32 f32;
typedef _Float64 f64;
typedef _Float32x f32x;
typedef _Complex _Float32 c32;
typedef _Complex _Float64 c64;
typedef _Complex _Float32x c32x;
typedef f32 v4f32 __attribute__((vector_size(16)));
struct doubles { f64 d; f32x x; };
f32 sum(int count, ...);
f32 from_float(float a);
f32x from_double(double a);
struct doubles mixed(struct doubles a);
c32 prod(c32 a, c32 b);
c64 quot(c64 a, c32x b);
v4f32 vector_sum(v4f32 a, v4f32 b);
""")
    provider = root / "interchange-provider.c"
    provider.write_text("""
#include "interchange.h"
#include <stdarg.h>
f32 sum(int count, ...) {
    va_list ap;
    va_start(ap, count);
    f32 a = va_arg(ap, f32);
    double b = va_arg(ap, double);
    f64 c = va_arg(ap, f64);
    f32x d = va_arg(ap, f32x);
    for (int i = 0; i < count; ++i)
        b += va_arg(ap, double);
    a += va_arg(ap, f32);
    va_end(ap);
    return a + b + c + d;
}
f32 from_float(float a) { return a; }
f32x from_double(double a) { return a; }
struct doubles mixed(struct doubles a) { return (struct doubles){a.x + 1.0F64, a.d - 1.0F32x}; }
c32 prod(c32 a, c32 b) { return a * b; }
c64 quot(c64 a, c32x b) { return a / b; }
v4f32 vector_sum(v4f32 a, v4f32 b) { return a + b; }
""")
    client = root / "interchange-main.c"
    client.write_text("""
#define __STDC_WANT_IEC_60559_TYPES_EXT__ 1
#include "interchange.h"
#include <complex.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#define COMP(a,b) __builtin_types_compatible_p(a,b)
#define TYPE(e,t) COMP(__typeof__(e),t)
_Static_assert(!COMP(f32,float) && !COMP(f64,double) && !COMP(f32x,double) &&
               !COMP(f32x,f64) && !COMP(c32,float _Complex), "distinct types");
_Static_assert(sizeof(f32) == sizeof(float) && _Alignof(f32x) == _Alignof(double) &&
               sizeof(c32x) == sizeof(double _Complex), "corresponding representations");
_Static_assert(TYPE(1.0F32 + 1.0f, f32) && TYPE(1.0 + 1.0f32x, double) &&
               TYPE(1.0f64 + 1.0f32x, f64) && TYPE(1.0F64 + 1.0, f64) &&
               TYPE(1.0L + 1.0F64x, long double) && TYPE(1.0F64x + 1.0F128, _Float128),
               "arithmetic conversion precedence");
_Static_assert(TYPE(1.0iF32, c32) && TYPE(0 ? 1.0F32x : 1.0F64, f64), "expression type");
_Static_assert(COMP(int (), int (f32)) && !COMP(int (), int (float)), "default promotions");
_Static_assert(FLT32_MANT_DIG == 24 && FLT64_MANT_DIG == 53 && FLT32X_MANT_DIG == 53 &&
               TYPE(FLT32_MAX, f32) && TYPE(FLT64_MIN, f64) && TYPE(FLT32X_TRUE_MIN, f32x) &&
               TYPE(FLT64X_EPSILON, _Float64x), "type-specific limits");
_Static_assert(TYPE(strtof32("1", 0), f32) && TYPE(sqrtf64(4.0F64), f64) &&
               TYPE(strtof32x("1", 0), f32x), "glibc function types");
_Static_assert(TYPE(CMPLXF32(0, 0), c32) && TYPE(csqrtf32x(CMPLXF32X(0, 0)), c32x),
               "interchange complex interfaces");
static f32 round32 = 1.000000059604644775390625000001F32;
static f64 round64 = 1.0000000000000001110223024625156540423631668090820312500001F64;
static f32x round32x = 0x1.000000000000080000000001p0F32x;
static f32x subnormal = FLT32X_TRUE_MIN;
static c32 c = 1.0F32 + 2.0jF32;
int main(void) {
    if (round32 != 1.0F32 + FLT32_EPSILON || round64 != 1.0F64 + FLT64_EPSILON ||
        round32x != 1.0F32x + FLT32X_EPSILON || subnormal != 0x1p-1074F32x)
        return 1;
    if (sum(5, 1.0F32, 2.0f, 3.0F64, 4.0F32x, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0F32) != 55.0F32)
        return 2;
    struct doubles s = mixed((struct doubles){1.5F64, 5.0F32x});
    v4f32 v = vector_sum((v4f32){1, 2, 3, 4}, (v4f32){4, 3, 2, 1});
    c32 p = prod(c, 3.0F32 + 4.0F32i);
    c64 q = quot(p, 3.0F32x + 4.0jF32x);
    if (s.d != 6 || s.x != 0.5F32x || v[0] != 5 || v[3] != 5 ||
        __real__ p != -5 || __imag__ p != 10 || __real__ q != 1 || __imag__ q != 2)
        return 3;
    volatile f64 square = 16.0F64;
    c32x z = csqrtf32x(CMPLXF32X(-square, 0));
    if (strtof32("1.5", 0) != 1.5F32 || sqrtf64(square) != 4.0F64 ||
        strtof32x("1.25", 0) != 1.25F32x || __real__ z != 0 || __imag__ z != 4.0F32x)
        return 4;
    union { float value; unsigned bits; } f = {.bits = 0x7f800123U};
    union { f32 value; unsigned bits; } f_out;
    union { double value; unsigned long bits; } d = {.bits = 0x7ff0000000000123UL};
    union { f32x value; unsigned long bits; } d_out;
    if (feclearexcept(FE_ALL_EXCEPT)) return 5;
    f_out.value = from_float(f.value);
    d_out.value = from_double(d.value);
    return f_out.bits != f.bits || d_out.bits != d.bits || fetestexcept(FE_INVALID);
}
""")
    objects = {}
    binary = root / "interchange"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        tool = "mcc" if cc == mcc else "gcc"
        objs = [root / f"interchange-{tool}-{part}.o" for part in ("provider", "main")]
        objects[cc] = objs
        for source, obj in zip((provider, client), objs):
            require(invoke([cc, *flags, "-std=gnu11", "-c", source, "-o", obj]),
                    f"{cc} compile {source.name}")
        require(invoke([cc, *flags, *objs, "-lm", "-o", binary]), f"{cc} link interchange floats")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run interchange floats")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, objects[provider_cc][0], objects[client_cc][1], "-lm", "-o", binary]),
                    f"link interchange floats with {provider_cc} provider")
            require(invoke([binary]), f"run interchange floats with {provider_cc} provider")
    flags = [f"--target={target}"] if target else []
    for source in ("_Float32 value;", "double value = 1.0F64;", "double value = 1.0F32x;"):
        command = [mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"]
        require(invoke(command, source), f"diagnose C11 extension in {source}", success=False)
        require(invoke(command, "__extension__ " + source), f"allow suppressed extension in {source}")


def type_generic_math(mcc, gcc, root, target, runner):
    direct = root / "tgmath-builtin.c"
    direct.write_text("""
#if defined(__MCC__) && !__has_builtin(__builtin_tgmath)
#error missing builtin feature query
#endif
typedef _Complex float cf;
typedef _Complex double cd;
static unsigned calls;
static float one(float x) { calls += 1; return x + 1; }
static double two(double x) { calls += 2; return x + 2; }
static double (*doubles(void))(double) { calls += 4; return two; }
static cf conjugate_f(float x, cf y) { return __builtin_complex(x, -__imag__ y); }
static cd conjugate_d(double x, cd y) { return __builtin_complex(x, -__imag__ y); }
static int tagged_f32(_Float32 x, _Float32 y) { return 32; }
static int tagged_f32x(_Float32x x, _Float32x y) { return 132; }
static int tagged_double(double x, double y) { return 2; }
static int fixedf(float value, int tag) { return tag; }
static int fixedd(double value, int tag) { return -tag; }
static float narrow_quad(_Float128 x) { return 128; }
static float narrow_double(double x) { return 64; }
static float narrow_f64(_Float64 x) { return 164; }
#define TYPE(e,t) __builtin_types_compatible_p(__typeof__(e),t)
#define TAG(a,b) __builtin_tgmath(tagged_f32,tagged_f32x,tagged_double,a,b)
_Static_assert(TYPE(__builtin_tgmath(one,two,1.0f), float), "float result");
_Static_assert(TYPE(__builtin_tgmath(one,two,1), double), "integer selects double");
_Static_assert(TYPE(__builtin_tgmath(conjugate_f,conjugate_d,1.0f,2.0f), cf), "complex-only family");
int main(void) {
    volatile float x = 1.0f;
    if (__builtin_tgmath(one, doubles(), x++) != 2 || x != 2 || calls != 1) return 1;
    if (__builtin_tgmath(one, doubles(), 1) != 3 || calls != 7) return 2;
    if (__builtin_tgmath(fixedf, fixedd, 1.0f, 3) != 3) return 3;
    cf z = __builtin_tgmath(conjugate_f, conjugate_d, 3.0f, 4.0fi);
    if (__real__ z != 3 || __imag__ z != -4) return 4;
    if (TAG(1.0F32, 2.0f) != 32 || TAG(1.0F32, 2) != 2 || TAG(1.0F32x, 2) != 132)
        return 5;
    if (__builtin_tgmath(narrow_quad,narrow_double,narrow_f64,1.0) != 64 ||
        __builtin_tgmath(narrow_quad,narrow_double,narrow_f64,1.0F64) != 164 ||
        __builtin_tgmath(narrow_quad,narrow_double,narrow_f64,1.0f) != 128 ||
        __builtin_tgmath(narrow_double,narrow_quad,narrow_f64,1.0f) != 64)
        return 6;
    return 0;
}
""")
    strict = root / "tgmath-std.c"
    strict.write_text("""
#include <tgmath.h>
#define TYPE(e,t) __builtin_types_compatible_p(__typeof__(e),t)
_Static_assert(TYPE(pow(2.0f, 3.0f), float), "real float");
_Static_assert(TYPE(pow(2.0f, 3), double), "integer mixed with float");
_Static_assert(TYPE(pow(CMPLXF(2, 0), 3), double _Complex), "integer mixed with complex float");
_Static_assert(TYPE(creal(2.0f), float), "complex-only overload");
_Static_assert(TYPE(nexttoward(2.0f, 1.0L), float), "only the first parameter is generic");
int main(void) {
    volatile float x = 2.0f;
    double y = pow(x++, 3);
    float z = sqrt(sqrt(16.0f));
    if (x != 3 || y != 8 || z != 2) return 1;
    return fabs(CMPLX(-3, 4)) != 5 || frexp(1.0, &(int){0}) != 0.5;
}
""")
    extended = root / "tgmath-iec.c"
    extended.write_text("""
#define __STDC_WANT_IEC_60559_TYPES_EXT__ 1
#define __STDC_WANT_IEC_60559_BFP_EXT__ 1
#include <tgmath.h>
#define TYPE(e,t) __builtin_types_compatible_p(__typeof__(e),t)
_Static_assert(TYPE(sqrt(4.0F32), _Float32) && TYPE(sqrt(4.0F64), _Float64) &&
               TYPE(sqrt(4.0F32x), _Float32x), "interchange binary32 and binary64");
_Static_assert(TYPE(sqrt(4.0F64x), _Float64x) && TYPE(sqrt(4.0F128), _Float128), "extended precision");
_Static_assert(TYPE(pow(2.0F32, 3), double) && TYPE(pow(2.0F32x, 3), _Float32x), "mixed integers");
_Static_assert(TYPE(pow(CMPLXF32(2, 0), 3.0F32), _Float32 _Complex) &&
               TYPE(fabs(CMPLXF128(3, 4)), _Float128), "complex interchange generics");
_Static_assert(TYPE(fsqrt(4.0), float) && TYPE(f32xsqrt(4), _Float32x), "narrowing macros");
int main(void) {
    volatile _Float32 x = 9.0F32;
    volatile _Float128 y = 16.0F128;
    return sqrt(x) != 3.0F32 || sqrt(y) != 4.0F128 ||
           fabs(CMPLXF128(3, 4)) != 5.0F128 || f32xsqrt(y) != 4.0F32x;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "tgmath"
    for cc in (mcc, gcc):
        for source, mode in ((direct, ["-std=gnu11"]), (strict, ["-std=c11", "-pedantic-errors"]),
                             (extended, ["-std=gnu11"])):
            require(invoke([cc, *(flags if cc == mcc else []), *mode, source, "-lm", "-o", binary]),
                    f"{cc} compile {source.name}")
            require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run {source.name}")
    signatures = """
float f(float); double d(double); long double l2(long double,long double);
float variadic(float,...); float noproto();
int mismatch(double); float only_result(float);
"""
    for arguments in ("f,1.0f", "f,f,1.0f", "f,l2,1.0f", "f,variadic,1.0f",
                      "noproto,d,1.0f", "f,mismatch,1.0f", "f,only_result,1.0f",
                      "f,d,1.0L", "f,d,1.0fi", "f,d,(void *)0"):
        source = signatures + f"void test(void) {{ __builtin_tgmath({arguments}); }}"
        for cc in (mcc, gcc):
            options = flags if cc == mcc else ["-x", "c"]
            require(invoke([cc, *options, "-std=gnu11", "-fsyntax-only", "-"], source),
                    f"{cc} diagnose invalid type-generic call {arguments}", success=False)


def decimal_floating(mcc, gcc, root, target, runner):
    header = root / "decimal.h"
    header.write_text("""
typedef _Decimal32 d32;
typedef _Decimal64 d64;
typedef _Decimal128 d128;
typedef unsigned __int128 uint128;
typedef float quad __attribute__((mode(TF)));
struct hom { d64 a, b; };
struct mixed { d32 a; float b; };
struct one { d128 x; };
d32 compute(d32 a, int b, d32 c);
d128 calculate(d128 a, d128 b, int op);
int compare(d64 a, d64 b);
struct hom swap(struct hom x);
struct mixed flip(struct mixed x);
struct one grow(struct one x);
d128 var(int ignored, ...);
d128 from_wide(uint128);
uint128 to_wide(d128);
#define CAST_DECLS(type, tag)                  \
    type bin_##tag##_32(d32);                  \
    type bin_##tag##_64(d64);                  \
    type bin_##tag##_128(d128);                \
    d32 dec_##tag##_32(type);                  \
    d64 dec_##tag##_64(type);                  \
    d128 dec_##tag##_128(type)
CAST_DECLS(_Float16, hf);
CAST_DECLS(__bf16, bf);
CAST_DECLS(float, sf);
CAST_DECLS(double, df);
CAST_DECLS(long double, ld);
CAST_DECLS(quad, tf);
""")
    provider = root / "decimal-provider.c"
    provider.write_text("""
#include "decimal.h"
d32 compute(d32 a, int b, d32 c) { return -(a + b) * c; }
d128 calculate(d128 a, d128 b, int op) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    default: return a / b;
    }
}
int compare(d64 a, d64 b) {
    return (a<b) + 2*(a<=b) + 4*(a>b) + 8*(a>=b) + 16*(a==b) + 32*(a!=b) + 64*(!a);
}
struct hom swap(struct hom x) { return (struct hom){x.b, x.a}; }
struct mixed flip(struct mixed x) { return (struct mixed){-x.a, -x.b}; }
struct one grow(struct one x) { return (struct one){x.x + 1.0DL}; }
d128 var(int ignored, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, ignored);
    d32 a = __builtin_va_arg(ap, d32);
    double b = __builtin_va_arg(ap, double);
    d64 c = __builtin_va_arg(ap, d64);
    d128 d = __builtin_va_arg(ap, d128);
    for (int i = 0; i < 4; ++i) d += __builtin_va_arg(ap, d128);
    d32 e = __builtin_va_arg(ap, d32);
    __builtin_va_end(ap);
    return b == 2.0 ? a + c + d + e : 0;
}
d128 from_wide(uint128 x) { return x; }
uint128 to_wide(d128 x) { return (uint128)x; }
#define CASTS(type, tag)                                \
    type bin_##tag##_32(d32 x) { return x; }             \
    type bin_##tag##_64(d64 x) { return x; }             \
    type bin_##tag##_128(d128 x) { return x; }           \
    d32 dec_##tag##_32(type x) { return x; }             \
    d64 dec_##tag##_64(type x) { return x; }             \
    d128 dec_##tag##_128(type x) { return x; }
CASTS(_Float16, hf)
#ifndef TEST_NO_BFLOAT_RUNTIME
CASTS(__bf16, bf)
#endif
CASTS(float, sf)
CASTS(double, df)
CASTS(long double, ld)
CASTS(quad, tf)
""")
    client = root / "decimal-main.c"
    client.write_text("""
#define __STDC_WANT_DEC_FP__ 1
#include <float.h>
#include "decimal.h"
#define TYPE(e,T) __builtin_types_compatible_p(__typeof__(e),T)
#if __DECIMAL_BID_FORMAT__ != 1 || DEC32_MANT_DIG != 7 || DEC64_MANT_DIG != 16 || DEC128_MANT_DIG != 34
#error decimal BID limits
#endif
typedef float sd __attribute__((mode(SD)));
typedef float dd __attribute__((mode(DD)));
typedef float td __attribute__((mode(TD)));
_Static_assert(TYPE(1.0df,sd) && TYPE(1.0DD,dd) && TYPE(1.0dl,td), "literal and mode types");
_Static_assert(TYPE(1.0DF + 0,d32) && TYPE(1.0DF + 1.0DD,d64) &&
               TYPE(1 ? 1.0DF : 1.0DL,d128), "common types");
_Static_assert(sizeof(d32)==4 && sizeof(d64)==8 && sizeof(d128)==16 && _Alignof(d128)==16, "ABI");
typedef union { d32 x; unsigned bits; } u32;
typedef union { d64 x; unsigned long bits; } u64;
typedef union { d128 x; unsigned long bits[2]; } u128;
typedef union { double x; unsigned long bits; } f64_bits;
typedef union { quad x; unsigned long bits[2]; } f128_bits;
static d32 sum32 = (1.0DF + 2.00DF) * 20.5DF - 61.0DF;
static d64 third = 1.0DD / 3.0DD;
static d128 fine = 1.000000000000000000000000000000001DL;
static unsigned long large_integer = (unsigned long)1.23456789e10DD;
static int negative = (int)-2.5DF;
static d32 rounded32 = (d32)9.9999995DD;
static d32 zero32 = -0.00DF;
static int choose = (1.e-6176DL < 1.e-6143DL ? 7 : 9);
static float above_float_tie = 1.000000059604644775390625000000001DL;
static _Float16 above_half_tie = 1.000488281250000000000000000000001DL;
static __bf16 above_bfloat_tie = 1.003906250000000000000000000000001DL;
static double above_double_tie = 1.000000000000000111022302462515655DL;
static d128 from_min_quad = 0x1p-16494F128;
static quad to_min_quad = 6.475175119438025110924438958227647e-4966DL;
static d64 from_single = 0.1f;
static d64 project = (d64)(1.25 + 2.5i);
static double _Complex expand = (double _Complex)1.25DD;
#define CHECK_CASTS(type, tag) (                                      \
    bin_##tag##_32(1.25DF) == (type)1.25 &&                           \
    bin_##tag##_64(-1.25DD) == (type)-1.25 &&                         \
    bin_##tag##_128(2.5DL) == (type)2.5 &&                            \
    dec_##tag##_32((type)3.125) == 3.125DF &&                        \
    dec_##tag##_64((type)0.125) == 0.125DD &&                        \
    dec_##tag##_128((type)-0.25) == -0.25DL)
int main(void) {
    if (((u32){.x=sum32}).bits != 0x310001f4 ||
        ((u64){.x=third}).bits != 0x2fcbd7a625405555UL ||
        ((u128){.x=fine}).bits[0] != 0x38c15b0a00000001UL ||
        ((u128){.x=fine}).bits[1] != 0x2ffe314dc6448d93UL ||
        large_integer != 12345678900UL || negative != -2 || choose != 7 ||
        ((u32){.x=rounded32}).bits != 0x300f4240 || ((u32){.x=zero32}).bits != 0xb1800000)
        return 1;
    if (compute(3.0DF, 5, 2.0DF) != -16.0DF || !(!zero32) || !(!(_Bool)zero32))
        return 2;
    d64 nan = ((u64){.bits=0x7c00000000000000UL}).x;
    if (compare(0,0) != 90 || compare(nan,0)!=32 || compare(0,nan)!=96 || compare(1,2)!=35)
        return 3;
    d128 maximum = DEC128_MAX;
    d128 threshold = DEC128_SUBNORMAL_MIN;
    if (calculate(fine, 1, 1) != DEC128_EPSILON || calculate(1, 3, 3) !=
        0.3333333333333333333333333333333333DL || calculate(maximum,-threshold,0)!=maximum ||
        ((u128){.x=calculate(maximum,10,2)}).bits[1] != 0x7800000000000000UL ||
        calculate(threshold,2,3) != 0.DL || calculate(threshold,0.5DL,3)!=2e-6176DL)
        return 4;
    struct hom ab = swap((struct hom){1, 2});
    struct mixed mixed = flip((struct mixed){3, 4});
    struct one one = grow((struct one){fine});
    if (ab.a!=2 || ab.b!=1 || mixed.a!=-3 || mixed.b!=-4 || one.x-2!=DEC128_EPSILON ||
        var(0,1.DF,2.0,3.DD,4.DL,5.DL,6.DL,7.DL,8.DL,9.DF)!=43)
        return 5;
    uint128 wide = ((uint128)1 << 91) + 13;
    if (to_wide(from_wide(wide)) != wide)
        return 6;
    volatile d64 a = 1.25DD;
    if (a++ != 1.25DD || (a += 2) != 4.25DD)
        return 7;
    if (!CHECK_CASTS(_Float16,hf) || !CHECK_CASTS(float,sf) ||
        !CHECK_CASTS(double,df) || !CHECK_CASTS(long double,ld) || !CHECK_CASTS(quad,tf))
        return 8;
    if (above_float_tie != 0x1.000002p0f ||
        above_float_tie != bin_sf_128(1.000000059604644775390625000000001DL) ||
        above_double_tie != 0x1.0000000000001p0 ||
        above_double_tie != bin_df_128(1.000000000000000111022302462515655DL) ||
        from_single != 0.1000000014901161DD || from_single != dec_sf_64(0.1f) ||
        project != 1.25DD || __real__ expand != 1.25 || __imag__ expand != 0.0)
        return 9;
    if (from_min_quad != 6.475175119438025110924438958227647e-4966DL ||
        from_min_quad != dec_tf_128(0x1p-16494F128) ||
        to_min_quad != 0x1p-16494F128 || to_min_quad != bin_tf_128(from_min_quad) ||
        ((f128_bits){.x=bin_tf_128(-1e-6000DL)}).bits[1] != 0x8000000000000000UL ||
        ((f128_bits){.x=bin_tf_128(maximum)}).bits[1] != 0x7fff000000000000UL)
        return 10;
    if (above_half_tie != 0x1.004p0F16 || above_bfloat_tie != 0x1.02p0BF16)
        return 11;
#ifndef TEST_NO_BFLOAT_RUNTIME
    if (!CHECK_CASTS(__bf16,bf))
        return 11;
#endif
#ifndef TEST_REFERENCE_RUNTIME
    if (above_half_tie != bin_hf_128(1.000488281250000000000000000000001DL) ||
        above_bfloat_tie != bin_bf_128(1.003906250000000000000000000000001DL))
        return 11;
#endif
    f64_bits snan = {.bits=0xfff0123456789abcUL};
    d64 payload = dec_df_64(snan.x);
    if (((u64){.x=payload}).bits != 0xfc00091a2b3c4d5eUL ||
        ((f64_bits){.x=bin_df_64(payload)}).bits != 0xfff8123456789abcUL ||
        ((u32){.x=dec_df_32(-0.0)}).bits != 0xb2800000U)
        return 12;
    return 0;
}
""")
    products = {}
    binary = root / "decimal"
    for cc in (mcc, gcc):
        flags = [f"--target={target}"] if cc == mcc and target else []
        tool = "mcc" if cc == mcc else "gcc"
        objects = [root / f"decimal-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objects
        for source, obj in zip((provider, client), objects):
            require(invoke([cc, *flags, "-std=gnu11", "-c", source, "-o", obj]), f"{cc} compile {source.name}")
        if cc == gcc:
            # GCC 16 rounds some half-precision midpoint cases differently at runtime.
            reference_main = root / "decimal-reference-main.o"
            reference_flags = ["-std=gnu11", "-DTEST_REFERENCE_RUNTIME"]
            require(invoke([cc, *reference_flags, "-c", client, "-o", reference_main]),
                    "compile standalone GCC decimal reference client")
            linked = invoke([cc, objects[0], reference_main, "-o", binary])
            missing_bfloat = ("undefined reference" in linked.stderr and "__bid_truncsdbf" in linked.stderr)
            if linked.returncode and missing_bfloat:
                # Keep the original objects for mixed links. Some GCC runtimes lack their bfloat16 helpers.
                reference = [root / f"decimal-reference-{part}.o" for part in ("provider", "main")]
                for source, obj in zip((provider, client), reference):
                    require(invoke([cc, *reference_flags, "-DTEST_NO_BFLOAT_RUNTIME", "-c", source, "-o", obj]),
                            f"{cc} compile decimal reference {source.name}")
                linked = invoke([cc, *reference, "-o", binary])
        else:
            linked = invoke([cc, *flags, *objects, "-o", binary])
        require(linked, f"{cc} link decimal floating point")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run decimal floating point")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, products[provider_cc][0], products[client_cc][1], "-o", binary]),
                    f"link decimal tests with {provider_cc} provider")
            require(invoke([binary]), f"run decimal tests with {provider_cc} provider")
    flags = [f"--target={target}"] if target else []
    command = [mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"]
    require(invoke(command, "_Decimal32 value = 0.1DF;"), "diagnose C11 decimal type", success=False)
    require(invoke(command, "__extension__ _Decimal32 value = 0.1DF;"), "suppress C11 decimal diagnostic")
    for source in ("_Decimal32 x = 0x1p0DF;", "_Decimal64 x = 1.0Dd;", "_Decimal128 x = 1.0DLi;",
                   "_Decimal32 f(_Decimal32 a) { return a+1.0; }",
                   "_Decimal32 f(_Decimal32 a) { return a%2; }"):
        require(invoke([mcc, *flags, "-std=gnu11", "-fsyntax-only", "-"], source), source, success=False)


def floating_format_builtins(mcc, gcc, root, target, runner):
    source = root / "floating-format-builtins.c"
    source.write_text("""
#define TYPE(e,t) __builtin_types_compatible_p(__typeof__(e), t)
typedef union { _Decimal32 value; unsigned bits; } d32;
typedef union { _Decimal64 value; unsigned long bits; } d64;
typedef union { _Decimal128 value; unsigned long words[2]; } d128;
typedef union { double value; unsigned long bits; } f64;
typedef union { _Float128 value; unsigned long words[2]; } f128;
static d32 positive_infinity = {__builtin_infd32()};
static d64 negative_infinity = {-__builtin_infd64()};
static d128 decimal_nan = {__builtin_nand128("0x123")};
static d32 decimal_snan = {__builtin_nansd32("12")};
static f64 binary_nan = {__builtin_nan("0x123")};
static f64 binary_snan = {__builtin_nans("021")};
static f128 wide_nan = {__builtin_nanf128("0x123456789abcdef001")};
static double signed_zero = __builtin_copysign(0., -1.);
static int constant_class = __builtin_fpclassify(-3, 5, 19, 27, -1, 1.e-398DD);
static double (*abs_function)(double) = __builtin_fabs;
#ifdef __MCC__
static int (*decimal_infinity_function)(_Decimal32) = __builtin_isinfd32;
#endif
_Static_assert(TYPE(__builtin_infd32(), _Decimal32) && TYPE(__builtin_nansd64(""), _Decimal64),
               "decimal builtin types");
_Static_assert(TYPE(__builtin_fabsd128(2.), _Decimal128) && TYPE(__builtin_huge_valf32x(), _Float32x),
               "types of fabs and infinity");
_Static_assert(TYPE(__builtin_nanf32(""), _Float32) && TYPE(__builtin_nansf16(""), _Float16) &&
               TYPE(__builtin_copysignf128(1, 2), _Float128), "binary builtin types");
_Static_assert(__builtin_constant_p((_Decimal64)0.125) && __builtin_constant_p(__builtin_infd128()),
               "decimal constant queries");
_Static_assert(!__builtin_constant_p(1.0DF / 0.0DF), "division by zero is not a constant query");
#ifdef __x86_64__
_Static_assert(TYPE(__builtin_infq(), _Float128) && TYPE(__builtin_fabsq(1.0), _Float128), "x86 aliases");
#endif

static int classify(_Decimal128 value) {
    return __builtin_fpclassify(-7, 15, 25, 35, 0, value);
}
static int is_signaling(double value) { return __builtin_issignaling(value); }
int main(void) {
    if (positive_infinity.bits != 0x78000000U || negative_infinity.bits != 0xf800000000000000UL ||
        decimal_nan.words[1] != 0x7c00000000000000UL || decimal_nan.words[0] ||
        decimal_snan.bits != 0x7e000000U || binary_nan.bits != 0x7ff8000000000123UL ||
        binary_snan.bits != 0x7ff0000000000011UL || !__builtin_signbit(signed_zero) ||
        wide_nan.words[1] != 0x7fff800000000012UL || wide_nan.words[0] != 0x3456789abcdef001UL ||
        constant_class != 27)
        return 1;
    if (!__builtin_isinfd32(positive_infinity.value) || !__builtin_isinfd64(negative_infinity.value) ||
        __builtin_isinf_sign(negative_infinity.value) != -1 || !__builtin_signbitd64(negative_infinity.value) ||
        !__builtin_isnand128(decimal_nan.value) || __builtin_issignaling(decimal_nan.value) ||
        !__builtin_issignaling(decimal_snan.value) || !is_signaling(binary_snan.value) ||
        __builtin_issignaling(binary_nan.value))
        return 2;
    volatile _Decimal128 zero = -0.DL, tiny = 1.e-6176DL, normal = 1.e-6143DL;
    if (classify(zero) || classify(tiny) != 35 || classify(normal) != 25 ||
        classify(negative_infinity.value) != 15 || classify(decimal_nan.value) != -7 ||
        !__builtin_signbit(zero) || !__builtin_isfinite(tiny) || __builtin_isnormal(tiny) ||
        !__builtin_isnormal(normal) || __builtin_isfinite(negative_infinity.value))
        return 3;
    if (__builtin_signbit(__builtin_fabsd128(zero)) ||
        __builtin_signbit(__builtin_fabs(-2.0)) ||
        abs_function(-3.0) != 3.0 ||
        !__builtin_signbit(__builtin_copysignf32(-1.0F32, signed_zero)))
        return 4;
    int side_effects = 0;
    if (!__builtin_isless((side_effects++, tiny), normal) || side_effects != 1 ||
        !__builtin_islessequal(normal, normal) || !__builtin_isgreaterequal(normal, normal) ||
        !__builtin_isunordered(decimal_nan.value, 1.0DF) ||
        __builtin_islessgreater(decimal_nan.value, normal) ||
        __builtin_isgreater(0.1DF, 0.1f) ||
        !__builtin_isgreater(0.1f, 0.1DD) ||
        __builtin_isgreater(1.0DF, 2))
        return 5;
    volatile double b = 0x1p-1074;
    if (__builtin_fpclassify(1, 2, 3, 4, 5, b) != 4 || !__builtin_finite(b) ||
        __builtin_isnormal(b) || __builtin_isless(binary_nan.value, b) ||
        !__builtin_isunordered(binary_nan.value, b))
        return 6;
#ifdef __MCC__
    const char *payload = "0x123";
    f64 parsed = {__builtin_nan((++side_effects, payload))};
    if (parsed.bits != binary_nan.bits || side_effects != 2)
        return 7;
    d64 dynamic = {__builtin_nansd64((++side_effects, payload))};
    if (dynamic.bits != 0x7e00000000000000UL || side_effects != 3 ||
        !__builtin_constant_p(3.0DF+5.0DF) || !decimal_infinity_function(positive_infinity.value))
        return 8;
#endif
    return 0;
}
""")
    binary = root / "floating-format-builtins"
    flags = [f"--target={target}"] if target else []
    for cc in (mcc, gcc):
        options = flags if cc == mcc else ["-fsignaling-nans"]
        require(invoke([cc, *options, "-std=gnu11", source, "-lm", "-o", binary]),
                f"{cc} compile floating-format builtins")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run floating-format builtins")
    for source in ("int f(void) { return __builtin_isinf(1); }",
                   "int f(void) { return __builtin_isgreater(1, 2); }",
                   "int f(int x) { return __builtin_fpclassify(1,2,3,4,x,0.0); }",
                   "void *p = (void *)&__builtin_infd32;",
                   "_Decimal32 f(void) { return __builtin_nand32(1); }",
                   "_Decimal64 f(void) { return __builtin_infd64(1); }"):
        require(invoke([mcc, *flags, "-std=gnu11", "-fsyntax-only", "-"], source), source, success=False)


def decimal_environment(mcc, gcc, root, target, runner):
    provider = root / "decimal-environment-provider.c"
    provider.write_text("""
_Decimal32 dadd(_Decimal32 a, _Decimal32 b) { return a + b; }
_Decimal32 dmul(_Decimal32 a, _Decimal32 b) { return a * b; }
_Decimal32 ddiv(_Decimal32 a, _Decimal32 b) { return a / b; }
_Decimal32 dnarrow(_Decimal64 a) { return a; }
_Decimal64 dwiden(_Decimal32 a) { return a; }
_Decimal32 dfrom_int(int a) { return a; }
int dto_int(_Decimal32 a) { return (int)a; }
_Decimal32 dfrom_double(double a) { return a; }
double dto_double(_Decimal128 a) { return a; }
int dequal(_Decimal32 a, _Decimal32 b) { return a == b; }
int dunordered(_Decimal32 a, _Decimal32 b) { return __builtin_isunordered(a, b); }
""")
    client = root / "decimal-environment-main.c"
    client.write_text("""
#define __STDC_WANT_IEC_60559_DFP_EXT__ 1
#include <fenv.h>
#include <float.h>
#include <pthread.h>
extern void __dfp_set_round(int);
extern int __dfp_get_round(void);
extern void __dfp_clear_except(void);
extern int __dfp_test_except(int);
extern void __dfp_raise_except(int);
_Decimal32 dadd(_Decimal32, _Decimal32);
_Decimal32 dmul(_Decimal32, _Decimal32);
_Decimal32 ddiv(_Decimal32, _Decimal32);
_Decimal32 dnarrow(_Decimal64);
_Decimal64 dwiden(_Decimal32);
_Decimal32 dfrom_int(int);
int dto_int(_Decimal32);
_Decimal32 dfrom_double(double);
double dto_double(_Decimal128);
int dequal(_Decimal32, _Decimal32);
int dunordered(_Decimal32, _Decimal32);

typedef union { _Decimal32 value; unsigned bits; } d32;
typedef union { _Decimal64 value; unsigned long bits; } d64;
typedef union { double value; unsigned long bits; } f64;
static _Decimal32 constant = 1.000000DF + 0.0000005DF;
static int exceptions(int expected) { return __dfp_test_except(FE_ALL_EXCEPT) == expected; }

#ifdef __MCC__
static void *thread(void *result) {
    int *ok = result;
    *ok = fe_dec_getround() == FE_DEC_TONEAREST && exceptions(0) &&
          fe_dec_setround(FE_DEC_DOWNWARD) == 0;
    __dfp_raise_except(FE_INEXACT);
    *ok &= fe_dec_getround() == FE_DEC_DOWNWARD && exceptions(FE_INEXACT);
    return 0;
}
#endif

int main(void) {
    static const _Decimal32 pos[] = {1.DF,1.DF,1.000001DF,1.DF,1.000001DF};
    static const _Decimal32 neg[] = {-1.DF,-1.000001DF,-1.DF,-1.DF,-1.000001DF};
    if (!exceptions(0) || __dfp_get_round() != 0 || constant != 1.DF)
        return 1;
    if (fesetround(FE_UPWARD) || feclearexcept(FE_ALL_EXCEPT))
        return 2;
    for (int mode = 0; mode < 5; mode++) {
        __dfp_set_round(mode);
        __dfp_clear_except();
        _Decimal32 above = dadd(1.000000DF,0.0000005DF);
        _Decimal32 below = dadd(-1.000000DF,-0.0000005DF);
        if (__dfp_get_round() != mode || above != pos[mode] || below != neg[mode] ||
            !exceptions(FE_INEXACT) || constant != 1.DF)
            return 3;
        __dfp_clear_except();
        d32 cancel = {.value=dadd(1.DF,-1.DF)};
        if ((cancel.bits >> 31) != (unsigned)(mode == 1) || !exceptions(0))
            return 4;
        __dfp_clear_except();
        _Decimal32 over = dmul(DEC32_MAX,2.DF);
        if (over != ((mode == 1 || mode == 3) ? DEC32_MAX : __builtin_infd32()) ||
            !exceptions(FE_OVERFLOW | FE_INEXACT))
            return 5;
        __dfp_clear_except();
        _Decimal32 under = dnarrow(1.23e-110DD);
        if (under != (mode == 2 ? DEC32_TRUE_MIN : 0.DF) ||
            !exceptions(FE_UNDERFLOW | FE_INEXACT))
            return 6;
        __dfp_clear_except();
        _Decimal32 rounded = dnarrow(9.999995e-96DD);
        if (rounded != ((mode == 1 || mode == 3) ? 9.99999e-96DF : DEC32_MIN) ||
            !exceptions(FE_UNDERFLOW | FE_INEXACT))
            return 7;
        __dfp_clear_except();
        _Decimal32 integer = dfrom_int(12345665);
        if (integer != ((mode == 2 || mode == 4) ? 12345670.DF : 12345660.DF) ||
            !exceptions(FE_INEXACT))
            return 8;
        __dfp_clear_except();
        int truncate = dto_int(-1.75DF);
        if (truncate != -1 || !exceptions(FE_INEXACT))
            return 9;
        __dfp_clear_except();
        _Decimal32 from_binary = dfrom_double(0.1);
        double to_binary = dto_double(1.000000000000000111022302462515655DL);
        if (from_binary != (mode == 2 ? 0.1000001DF : 0.1DF) ||
            to_binary != ((mode == 1 || mode == 3) ? 1. : 0x1.0000000000001p0) ||
            !exceptions(FE_INEXACT))
            return 10;
    }
    __dfp_clear_except();
    _Decimal32 exact = dnarrow(1e-101DD);
    if (exact != DEC32_TRUE_MIN || !exceptions(0))
        return 11;
    __dfp_clear_except();
    _Decimal32 infinity = ddiv(-1.DF,0.DF);
    if (infinity != -__builtin_infd32() || !exceptions(FE_DIVBYZERO))
        return 12;
    _Decimal32 nan = ddiv(0.DF,0.DF);
    if (!__builtin_isnand32(nan) || !exceptions(FE_DIVBYZERO | FE_INVALID) ||
        __dfp_test_except(FE_OVERFLOW) != 0 || __dfp_test_except(FE_INVALID) != FE_INVALID ||
        fetestexcept(FE_ALL_EXCEPT))
        return 13;
    d32 signaling = {.bits=0x7e000123U};
    __dfp_clear_except();
    d64 quiet = {.value=dwiden(signaling.value)};
    if (quiet.bits != 0x7c000043c0f39e00UL || !exceptions(FE_INVALID))
        return 14;
    __dfp_clear_except();
    if (dequal(nan, nan) || !exceptions(0) || !dunordered(signaling.value, nan) ||
        !exceptions(FE_INVALID))
        return 15;
    __dfp_clear_except();
    if (__builtin_issignaling(signaling.value) != 1 || !exceptions(0))
        return 16;
    __dfp_raise_except(FE_INVALID | FE_UNDERFLOW);
    if (!exceptions(FE_INVALID | FE_UNDERFLOW))
        return 17;
#ifdef __MCC__
    if (fe_dec_getround() != FE_DEC_TONEARESTFROMZERO ||
        fe_dec_setround(42) == 0 || fe_dec_getround() != FE_DEC_TONEARESTFROMZERO ||
        fe_dec_setround(FE_DEC_UPWARD))
        return 18;
    pthread_t worker;
    int ok = 0;
    if (pthread_create(&worker, 0, thread, &ok) || pthread_join(worker, 0) || !ok ||
        fe_dec_getround() != FE_DEC_UPWARD || !exceptions(FE_INVALID | FE_UNDERFLOW))
        return 19;
#endif
    return 0;
}
""")
    flags = [f"--target={target}"] if target else []
    binary = root / "decimal-environment"
    products = {}
    for cc in (mcc, gcc):
        options = flags if cc == mcc else ["-O0", "-frounding-math", "-fsignaling-nans"]
        tool = "mcc" if cc == mcc else "gcc"
        objects = [root / f"decimal-environment-{tool}-{part}.o" for part in ("provider", "main")]
        products[cc] = objects
        for source, obj in zip((provider, client), objects):
            require(invoke([cc, *options, "-std=gnu11", "-pthread", "-c", source, "-o", obj]),
                    f"{cc} compile {source.name}")
        require(invoke([cc, *options, *objects, "-pthread", "-lm", "-o", binary]),
                f"{cc} link decimal environment")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run decimal environment")

        provider_dso = root / f"libdecimal-environment-{tool}.so"
        require(invoke([cc, *options, "-std=gnu11", "-shared", "-fPIC", provider, "-o", provider_dso]),
                f"{cc} build decimal environment DSO")
        require(invoke([cc, *options, objects[1], f"-L{root}", f"-ldecimal-environment-{tool}",
                        f"-Wl,-rpath,{root}", "-pthread", "-lm", "-o", binary]),
                f"{cc} link decimal environment DSO")
        require(invoke([*(runner if cc == mcc else []), binary]), f"{cc} run decimal environment DSO")
    if not target:
        for provider_cc, client_cc in ((mcc, gcc), (gcc, mcc)):
            require(invoke([mcc, products[provider_cc][0], products[client_cc][1], "-pthread", "-lm", "-o", binary]),
                    f"link decimal environment with {provider_cc} provider")
            require(invoke([binary]), f"run decimal environment with {provider_cc} provider")
    strict = "#define __STDC_WANT_IEC_60559_DFP_EXT__ 1\n#include <fenv.h>\n" \
             "int mode(void) { return fe_dec_getround() + FE_DEC_TONEAREST; }\n"
    require(invoke([mcc, *flags, "-std=c11", "-pedantic-errors", "-fsyntax-only", "-"], strict),
            "use the opt-in decimal environment header from strict C11")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mcc", type=Path)
    parser.add_argument("--reference", default=os.environ.get("MCC_TOOLCHAIN") or "gcc")
    parser.add_argument("--target")
    parser.add_argument("--runner", default="", help="command prefix for target executables")
    parser.add_argument("--later-dialects", action="store_true")
    args = parser.parse_args()
    with TemporaryDirectory(prefix="mcc-gnu-extensions-") as directory:
        mcc, gcc, root = args.mcc.resolve(), args.reference, Path(directory)
        dialects(mcc, gcc, args.later_dialects)
        preprocess(mcc, gcc, root)
        compile_and_run(mcc, gcc, root, args.target, shlex.split(args.runner), args.later_dialects)
        diagnostics(mcc)
        diagnostic_pragmas(mcc, gcc, root, args.target)
        atomics(mcc, gcc, root, args.target, shlex.split(args.runner))
        inline_linkage(mcc, gcc, root, args.target, shlex.split(args.runner))
        raw_strings(mcc, gcc, root, args.target, shlex.split(args.runner))
        checked_arithmetic(mcc, gcc, root, args.target, shlex.split(args.runner))
        source_and_cache_builtins(mcc, gcc, root, args.target, shlex.split(args.runner))
        clear_padding(mcc, gcc, root, args.target, shlex.split(args.runner))
        nonlocal_builtins(mcc, gcc, root, args.target, shlex.split(args.runner))
        frame_queries(mcc, gcc, root, args.target, shlex.split(args.runner))
        explicit_stack_builtins(mcc, gcc, root, args.target, shlex.split(args.runner))
        dwarf_register_sizes(mcc, gcc, root, args.target, shlex.split(args.runner))
        classify_types(mcc, gcc, root, args.target, shlex.split(args.runner))
        attribute_queries(mcc, gcc, root, args.target, shlex.split(args.runner))
        speculation_safe_values(mcc, gcc, root, args.target, shlex.split(args.runner))
        noreturn_functions(mcc, gcc, root, args.target, shlex.split(args.runner))
        warn_unused_result(mcc, gcc, root, args.target)
        switch_fallthrough(mcc, gcc, root, args.target)
        call_diagnostic_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        sentinel_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        nullability_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        noplt_calls(mcc, gcc, root, args.target, shlex.split(args.runner))
        patchable_entries(mcc, gcc, root, args.target, shlex.split(args.runner))
        access_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        access_initialization(mcc, gcc, root, args.target)
        access_size_ranges(mcc, gcc, root, args.target)
        access_pointer_offsets(mcc, gcc, root, args.target)
        access_allocation_ranges(mcc, gcc, root, args.target)
        access_pointer_assignments(mcc, gcc, root, args.target)
        system_register_builtins(mcc, gcc, root, args.target, shlex.split(args.runner))
        object_sizes(mcc, gcc, root, args.target, shlex.split(args.runner))
        allocation_sizes(mcc, gcc, root, args.target, shlex.split(args.runner))
        library_allocation_sizes(mcc, gcc, root, args.target, shlex.split(args.runner))
        fortified_calls(mcc, gcc, root, args.target, shlex.split(args.runner))
        variably_modified_arrays(mcc, gcc, root, args.target, shlex.split(args.runner))
        variable_length_members(mcc, gcc, root, args.target, shlex.split(args.runner))
        stack_allocations(mcc, gcc, root, args.target, shlex.split(args.runner))
        enum_types(mcc, gcc, root, args.target, shlex.split(args.runner))
        wide_integers(mcc, gcc, root, args.target, shlex.split(args.runner))
        extended_assembly(mcc, gcc, root, args.target, shlex.split(args.runner))
        global_register_variables(mcc, gcc, root, args.target, shlex.split(args.runner))
        call_clobbered_global_registers(mcc, gcc, root, args.target, shlex.split(args.runner))
        aggregate_register_variables(mcc, gcc, root, args.target, shlex.split(args.runner))
        complex_numbers(mcc, gcc, root, args.target, shlex.split(args.runner))
        complex_multiplication_and_division(mcc, gcc, root, args.target, shlex.split(args.runner))
        bit_field_initialization(mcc, gcc, root, args.target, shlex.split(args.runner))
        glibc_keyword_compatibility(mcc, gcc, root, args.target, shlex.split(args.runner))
        system_header_pragmas(mcc, gcc, root, args.target, shlex.split(args.runner))
        system_header_splices(mcc, gcc, root, args.target)
        preprocessor_pragma_messages(mcc, gcc, root, args.target)
        preprocessor_poison(mcc, gcc, root, args.target)
        structure_packing(mcc, gcc, root, args.target, shlex.split(args.runner))
        if not args.target:
            packed_argument_abi(mcc, gcc, root)
            aligned_argument_abi(mcc, gcc, root)
        transparent_unions(mcc, gcc, root, args.target, shlex.split(args.runner))
        designated_initializers(mcc, gcc, root, args.target, shlex.split(args.runner))
        initializer_ranges(mcc, gcc, root, args.target, shlex.split(args.runner))
        alignment_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        declarator_alignment(mcc, gcc, root, args.target, shlex.split(args.runner))
        pack_placement(mcc, gcc, root, args.target, shlex.split(args.runner))
        weak_symbols(mcc, gcc, root, args.target, shlex.split(args.runner))
        symbol_visibility(mcc, gcc, root, args.target, shlex.split(args.runner))
        lifecycle_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        cleanup_attributes(mcc, gcc, root, args.target, shlex.split(args.runner))
        named_sections(mcc, gcc, root, args.target, shlex.split(args.runner))
        vector_types(mcc, gcc, root, args.target, shlex.split(args.runner))
        machine_modes(mcc, gcc, root, args.target, shlex.split(args.runner))
        half_precision(mcc, gcc, root, args.target, shlex.split(args.runner))
        bfloat16(mcc, gcc, root, args.target, shlex.split(args.runner))
        binary128(mcc, gcc, root, args.target, shlex.split(args.runner))
        interchange_float(mcc, gcc, root, args.target, shlex.split(args.runner))
        type_generic_math(mcc, gcc, root, args.target, shlex.split(args.runner))
        decimal_floating(mcc, gcc, root, args.target, shlex.split(args.runner))
        floating_format_builtins(mcc, gcc, root, args.target, shlex.split(args.runner))
        decimal_environment(mcc, gcc, root, args.target, shlex.split(args.runner))
    print("GNU extension tests passed")


if __name__ == "__main__":
    main()
