#!/usr/bin/env python3
"""Generated C11 pointer constraints and execution regressions for mcc."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from itertools import product
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


@dataclass(frozen=True)
class Case:
    name: str
    source: str
    diagnostic: str = ""
    mode: str = "c11"
    executable: bool = False

    @property
    def flags(self):
        return [f"-std={self.mode}"] + (["-pedantic-errors"] if self.mode == "c11" else [])


def types():
    qualifiers = [(), ("const",), ("volatile",), ("const", "volatile")]
    for left, right in product(qualifiers, repeat=2):
        def spelling(parts, base="int"):
            return " ".join((*parts, base, "*"))

        def label(parts):
            return "-".join(parts) or "plain"

        key = f"{label(left)}-{label(right)}"
        a, b = spelling(left), spelling(right)
        common = spelling(sorted(set(left + right)))
        for op, name in (("==", "eq"), ("!=", "ne"), ("<", "lt"), ("<=", "le"),
                         (">", "gt"), (">=", "ge"), ("-", "subtract")):
            yield Case(f"{name}-{key}", f"long f({a} x, {b} y) {{ return x {op} y; }}")
        yield Case(f"conditional-{key}", f"int f(int c, {a} x, {b} y) {{ "
                   f"_Static_assert(_Generic(c ? x : y, {common}: 1, default: 0), \"type\"); "
                   "return 0; }")
        keep = set(right) <= set(left)
        yield Case(f"assignment-{key}", f"void f({a} x, {b} y) {{ x = y; }}",
                   "" if keep else "qualifier")
        v = spelling(right, "void")
        common = spelling(sorted(set(left + right)), "void")
        for order in ("object-first", "void-first"):
            yes, no = ("x", "y") if order == "object-first" else ("y", "x")
            yield Case(f"void-conditional-{key}-{order}", f"int f(int c, {a} x, {v} y) {{ "
                       f"_Static_assert(_Generic(c ? {yes} : {no}, {common}: 1, default: 0), \"type\"); "
                       "return x == y; }")


def nulls():
    for spelling, name in (("0", "integer"), ("(9 - 9)", "expression"), ("(void *)0", "void-cast"),
                           ("(int)0.0", "float-cast-to-int")):
        for kind, signature in (("object", "const char *"), ("function", "FP")):
            for reverse in (False, True):
                yes, no = (spelling, "p") if reverse else ("p", spelling)
                yield Case(f"null-{name}-{kind}-{reverse}",
                           f"typedef int (*FP)(void); int f(int c, {signature} p) {{ "
                           f"_Static_assert(_Generic(c ? {yes} : {no}, {signature}:1, default:0), \"type\"); "
                           f"{signature} q = {spelling}; return ({yes} == {no}) && q == p; }}")


def constraints():
    for name, source, diagnostic in [
        ("return-const", "char *f(const char *p) { return p; }", "qualifier"),
        ("argument-volatile", "void g(char *); void f(volatile char *p) { g(p); }", "qualifier"),
        ("deep-assignment", "void f(char **x, const char **y) { y = x; }", "incompatible pointer"),
        ("deep-equality", "int f(char **x, const char **y) { return x == y; }", "pointer"),
        ("deep-conditional", "void f(int c, char **x, const char **y) { (void)(c ? x : y); }", "pointer"),
        ("different-equality", "int f(int *x, char *y) { return x == y; }", "pointer"),
        ("nonzero-equality", "int f(int *x) { return x == 1; }", "pointer"),
        ("variable-zero", "int f(int *x) { int z = 0; return x == z; }", "pointer"),
        ("float-zero", "int f(int *x) { return x == 0.0; }", "pointer"),
        ("null-order", "int f(int *x) { return x < 0; }", "pointer"),
        ("void-order", "int f(int *x, void *p) { return x < p; }", "pointer"),
        ("function-order", "int f(int (*x)(void), int (*y)(void)) { return x < y; }", "pointer"),
        ("function-void-assignment", "void *f(int (*p)(void)) { return p; }", "pointer"),
        ("function-void-equality", "int f(int (*x)(void), void *y) { return x == y; }", "pointer"),
        ("function-void-conditional", "void f(int c, int (*x)(void), void *y) { (void)(c ? x : y); }", "pointer"),
        ("function-void-cast", "void *f(int (*p)(void)) { return (void *)p; }", "pointer"),
        ("qualified-void-not-null", "int f(int (*x)(void)) { return x == (const void *)0; }", "pointer"),
        ("typed-null-not-constant", "int f(char *x) { return x == (int *)0; }", "pointer"),
        ("integer-cast-pointer-not-null", "int f(int *x) { return x == (long)(void *)0; }", "pointer"),
        ("unevaluated-call-not-null", "int g(void); int f(int *x) { return x == (0 && g()); }", "pointer"),
        ("unrelated-function-prototypes", "typedef void F(const int *); typedef void G(int *); int f(F *a,G *b){return a==b;}", "pointer"),
        ("distinct-enums", "enum A { A }; enum B { B }; int f(enum A *x, enum B *y) { return x == y; }", "pointer"),
        ("incompatible-subtraction", "long f(char **x, const char **y) { return x - y; }", "pointer"),
        ("incompatible-redeclaration", "extern int *p; extern const int *p;", "conflicting declaration"),
        ("incompatible-prototypes", "void f(int *); void f(const int *);", "conflicting declaration"),
        ("unprototyped-promotions", "int f(); int f(float);", "conflicting declaration"),
        ("unprototyped-variadic", "int f(); int f(int, ...);", "conflicting declaration"),
        ("restrict-nonpointer", "typedef int Value; restrict Value value;", "restrict"),
        ("restrict-function-pointer", "typedef int (*FP)(void); FP restrict value;", "restrict"),
        ("const-subobject", "struct S { const int value; }; void f(struct S *p, struct S x) { *p = x; }", "modifiable lvalue"),
        ("const-array-subobject", "struct S { const int value[2]; }; void f(struct S *p, struct S x) { *p = x; }", "modifiable lvalue"),
        ("conditional-non-lvalue", "struct S { int v; }; void f(int c, struct S a, struct S b) { (void)&((c ? a : b).v); }", "not an lvalue"),
        ("cast-non-lvalue", "void f(int *p) { (void)&((int *)p); }", "not an lvalue"),
    ]:
        yield Case(name, source, diagnostic)


def specialized():
    yield Case("offsetof-zero", """
        #include <stddef.h>
        struct S { int first; };
        int f(int *a) { return a == offsetof(struct S, first); }
    """)
    yield Case("qualified-forward-tag", """
        struct S;
        static const volatile struct S *p;
        struct S { int value; };
        int f(void) {
            _Static_assert(sizeof *p == sizeof(struct S), "complete after definition");
            _Static_assert(_Generic(&p->value, const volatile int *:1, default:0), "type");
            return p->value;
        }
    """)
    yield Case("enum-underlying", """
        enum E { E0 };
        int f(enum E *a, unsigned int *b) { return a == b; }
    """)
    yield Case("pointee-restrict-union", """
        int f(int c, int *restrict *a, int *const *b) {
            _Static_assert(_Generic(c ? a : b, int *const restrict *:1, default:0), "type");
            return a == b;
        }
    """)
    yield Case("array-composite", """
        int f(int c, int (*x)[], int (*y)[4]) {
            _Static_assert(sizeof *(c ? x : y) == sizeof(int[4]), "complete array");
            _Static_assert(sizeof *(c ? y : x) == sizeof(int[4]), "complete array");
            return 0;
        }
    """)
    yield Case("function-composite", """
        int f(int c, int (*old)(), int (*proto)(int)) { return (c ? old : proto)(1); }
    """)
    yield Case("function-composite-arity", """
        int f(int c, int (*old)(), int (*proto)(int)) { return (c ? old : proto)(); }
    """, "number of function arguments")
    yield Case("member-qualifiers", """
        struct S { int x; char text[2]; };
        int f(const volatile struct S *s) {
            _Static_assert(_Generic(&s->x, const volatile int *:1, default:0), "member");
            _Static_assert(_Generic(s->text, const volatile char *:1, default:0), "array member");
            return 0;
        }
    """)
    yield Case("qualified-array-typedef", """
        typedef int Arr[3];
        int f(const volatile Arr a) {
            _Static_assert(_Generic(a, const volatile int *:1, default:0), "array typedef");
            return 0;
        }
    """)
    yield Case("gnu-pointer-extensions", """
        typedef int (*FP)(void);
        void *f(FP p) { return p; }
        FP g(void *p) { return (FP)p; }
        int h(FP p, void *q) { return p == q; }
    """, mode="gnu11")
    yield Case("gnu-restrict-discarded", """
        int f(int *restrict *p) { int **q = p; return *q != 0; }
    """, mode="gnu11")
    yield Case("runtime", (Path(__file__).parent / "pointer-runtime.c").read_text(), executable=True)
    yield Case("gnu-omitted-runtime", """
        static int calls;
        static int value;
        static int *get(int ok) { calls++; return ok ? &value : 0; }
        int main(void) {
            const volatile int *fallback = 0;
            _Static_assert(_Generic(get(1) ?: fallback, const volatile int *:1, default:0), "type");
            return (get(1) ?: fallback) != &value || calls != 1 ||
                   (get(0) ?: fallback) != fallback || calls != 2;
        }
    """, executable=True, mode="gnu11")


def run(argv, source=None):
    return subprocess.run(argv, input=source, text=True, capture_output=True, timeout=30)


def verify(case, compiler, reference, directory):
    def compile_with(tool, oracle=False):
        command = [tool, *case.flags]
        if oracle:
            command += ["-x", "c"]
        output = directory / (case.name + ("-gcc" if oracle else ""))
        command += ["-", "-o", output] if case.executable else ["-fsyntax-only", "-"]
        result = run(command, case.source)
        if result.returncode < 0:
            return f"compiler terminated by signal {-result.returncode}", None
        if case.diagnostic:
            if not result.returncode:
                return "invalid program accepted", None
            if result.returncode != 1:
                return f"compiler exit {result.returncode}: {result.stderr.strip()}", None
            if not oracle and case.diagnostic not in result.stderr:
                return f"wrong diagnostic: {result.stderr.strip()}", None
            return "", None
        if result.returncode:
            return result.stderr.strip() or f"compiler exit {result.returncode}", None
        if case.executable:
            result = run([output])
            if result.returncode:
                return f"program exit {result.returncode}: {result.stderr.strip()}", None
            return "", result.stdout
        return "", None

    try:
        if reference:
            error, expected = compile_with(reference, oracle=True)
            if error:
                return f"GCC reference: {error}"
        error, actual = compile_with(compiler)
        if not error and case.executable and reference and expected != actual:
            return f"stdout differs: got {actual!r}, expected {expected!r}"
        return error
    except (OSError, subprocess.TimeoutExpired) as error:
        return str(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--reference", default=os.environ.get("MCC_TOOLCHAIN"))
    args = parser.parse_args()
    cases = [*types(), *nulls(), *constraints(), *specialized()]
    failures = []
    with TemporaryDirectory(prefix="mcc-extended-") as directory, ThreadPoolExecutor(8) as pool:
        for case, error in zip(cases, pool.map(
            lambda case: verify(case, args.compiler.resolve(), args.reference, Path(directory)), cases
        )):
            if error:
                print(f"{case.name}: {error}")
                failures.append(case.name)
    print(f"c-testsuite-extended: {len(cases) - len(failures)}/{len(cases)} passed")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
