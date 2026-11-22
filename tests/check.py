import argparse
import concurrent.futures
import os
from pathlib import Path
import subprocess
import tempfile

STRICT_EXCEPTIONS = {
    "00095.c": "conversion between a function pointer and void pointer",
    "00144.c": "discarded const qualifier",
    "00170.c": "forward enum declaration",
    "00209.c": "forward enum declaration",
    "00210.c": "conversion between a function pointer and void pointer",
    "00213.c": "statement expression",
    "00214.c": "statement expression",
    "00216.c": "empty aggregate",
}


def command(args, *, source=None):
    return subprocess.run(args, input=source, capture_output=True, timeout=15)


def succeeded(args, *, source=None):
    result = command(args, source=source)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout


def x86_abi(compiler, gcc, root):
    declarations = br"""
#include <stdarg.h>
struct Mixed { double fp; long gp; };
struct Large { long a, b, c; };
struct Tiny { unsigned char bytes[3]; };
struct Extended { long double value; };
struct Mixed mcc_mix(struct Mixed, int, int, int, int, int, int, struct Mixed);
struct Large mcc_large(struct Large);
struct Extended mcc_extended(struct Extended);
long double mcc_variadic(int, ...);
int gcc_check(void);
int gcc_variadic(int, ...);
"""
    first, second, binary = (root / name for name in ("abi-mcc.o", "abi-gcc.o", "abi"))
    succeeded([compiler, "-c", "-", "-o", first], source=declarations + br"""
struct Mixed mcc_mix(struct Mixed a, int b, int c, int d, int e, int f, int g, struct Mixed z) {
    return (struct Mixed){a.fp + z.fp, a.gp + b + c + d + e + f + g + z.gp};
}
struct Large mcc_large(struct Large x) { return (struct Large){x.b, x.c, x.a}; }
struct Extended mcc_extended(struct Extended x) {
    return (struct Extended){(x.value - 3.L) / 2.L};
}
static int consume(va_list args) { return va_arg(args, int); }
long double mcc_variadic(int count, ...) {
    va_list args, saved;
    va_start(args, count);
    va_copy(saved, args);
    struct Mixed mix = va_arg(args, struct Mixed);
    long double extended = va_arg(args, long double);
    struct Tiny tiny = va_arg(args, struct Tiny);
    int first = consume(args);
    int second = va_arg(args, int);
    struct Mixed again = va_arg(saved, struct Mixed);
    va_end(saved);
    va_end(args);
    return count + mix.fp + mix.gp + extended + tiny.bytes[2] + first + second + again.gp;
}
int main(void) {
    volatile unsigned long maximum = 18446744073709551615UL;
    volatile double zero = 0.0;
    double nan = zero / zero;
    if ((unsigned long)(long double)maximum != maximum || nan == nan || nan <= 0.0 || !nan)
        return 1;
    return gcc_check() || gcc_variadic(7, (struct Mixed){4.0, 3}, 2.L, 1) != 17;
}
""")
    succeeded([gcc, "-x", "c", "-c", "-", "-o", second], source=declarations + br"""
int gcc_check(void) {
    struct Mixed mix = mcc_mix((struct Mixed){1.5, 5}, 1,2,3,4,5,6, (struct Mixed){2.0, 7});
    struct Large large = mcc_large((struct Large){1, 2, 3});
    struct Extended ext = mcc_extended((struct Extended){13.L});
    long double sum = mcc_variadic(5, (struct Mixed){2.0, 4}, 3.L, (struct Tiny){{1, 2, 6}}, 7, 8);
    return mix.fp != 3.5 || mix.gp != 33 || large.a != 2 || large.b != 3 || large.c != 1 ||
           ext.value != 5.L || sum != 39.L;
}
int gcc_variadic(int n, ...) {
    va_list args;
    va_start(args, n);
    struct Mixed mix = va_arg(args, struct Mixed);
    long double extended = va_arg(args, long double);
    int i = va_arg(args, int);
    va_end(args);
    return n + mix.fp + mix.gp + extended + i;
}
""")
    succeeded([compiler, first, second, "-o", binary])
    succeeded([binary])


def smoke(compiler):
    with tempfile.TemporaryDirectory(prefix="mcc-smoke-") as tmp:
        root = Path(tmp)
        binary = root / "program"
        header = root / "header.h"
        header.write_text("#define CAT(a, b) a ## b\n#define STR(x) #x\n")
        source = br"""
#include "header.h"
#if defined(OFF) || (!defined(ON) && 42 / 0)
#error unexpected branch
#elif ON != 3
#error wrong value
#endif
STR(two words) CAT(12,34)
"""
        output = succeeded([compiler, "-E", "-I", tmp, "-DON=1", "-UON", "-DON=3", "-"],
                           source=source)
        assert output.split() == [b'"two', b'words"', b'1234'], output

        first_source = root / "first.c"
        second_source = root / "second.c"
        first_source.write_text("int first;\n")
        second_source.write_text("int second;\n")
        combined = root / "combined.i"
        succeeded([compiler, "-E", first_source, second_source, "-o", combined])
        assert combined.read_text().split() == ["int", "first", ";", "int", "second", ";"]

        duplicate = root / "nested" / first_source.name
        duplicate.parent.mkdir()
        duplicate.write_text("int duplicate;\n")
        for mode, suffix in (("-c", ".o"), ("-S", ".s")):
            result = subprocess.run([compiler, mode, first_source, duplicate], cwd=root,
                                    capture_output=True, timeout=15)
            assert result.returncode == 1 and b"multiple inputs map" in result.stderr
            assert first_source.with_suffix(suffix).exists()

        source = br"""
#include <stdio.h>
typedef int Index;
static int data[] = {1, 2, 3, 4};
int accumulate(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
    return a+b+c+d+e+f+g+h+i+j;
}
int main(void) {
    int sum = 0;
    for (Index i = 0; i < 4; i++)
        sum += *(data + i);
    if (accumulate(1,2,3,4,5,6,7,8,9,10) != 55 || sum != 10)
        return 1;
    double x = 1.5;
    if (x + 0.5 != 2.0)
        return 2;
    puts("mcc");
    return 0;
}
"""
        succeeded([compiler, "-", "-o", binary], source=source)
        assert succeeded([binary]) == b"mcc\n"
        gcc = os.environ.get("MCC_TOOLCHAIN") or "gcc"
        target = succeeded([gcc, "-dumpmachine"]).split(b"-")[0]
        machine, loader = {
            b"aarch64": (b"AArch64", b"ld-linux-aarch64.so.1"),
            b"x86_64": (b"X86-64", b"ld-linux-x86-64.so.2"),
        }[target]
        elf = succeeded(["readelf", "-h", "-l", binary])
        assert machine in elf and loader in elf
        for arch, instruction in (("aarch64", b"movz"), ("x86_64", b"movabsq")):
            flag = f"--target={arch}-linux-gnu"
            source = f"#ifndef __{arch}__\n#error incorrect target\n#endif\nint f(void) {{ return 42; }}".encode()
            assert instruction in succeeded([compiler, flag, "-S", "-o", "-", "-"], source=source)

        first = root / "mcc.o"
        second = root / "gcc.o"
        succeeded([compiler, "-c", "-", "-o", first], source=br"""
extern long from_gcc(long,long,long,long,long,long,long,long,long);
long from_mcc(long a, long b, long c, long d, long e, long f, long g, long h, long i) {
    return a + i;
}
int main(void) { return from_gcc(8,2,3,4,5,6,7,8,5) != 26; }
""")
        succeeded([gcc, "-x", "c", "-c", "-", "-o", second], source=br"""
extern long from_mcc(long,long,long,long,long,long,long,long,long);
long from_gcc(long a, long b, long c, long d, long e, long f, long g, long h, long i) {
    return 2 * from_mcc(a,b,c,d,e,f,g,h,i);
}
""")
        succeeded([compiler, first, second, "-static", "-o", binary])
        succeeded([binary])
        if target == b"x86_64":
            x86_abi(compiler, gcc, root)

        original = binary.read_bytes()
        failed = command([compiler, "-", "-o", binary], source=b"int main(void) { return missing; }")
        assert failed.returncode == 1 and b"<stdin>:1:" in failed.stderr
        assert binary.read_bytes() == original
        gnu = br"""
int main(void) {
    int value = 0b11;
    switch (value) { case 1 ... 4: return (value++ ?: 9) != 3 || value != 4; }
    return 1;
}
"""
        assert command([compiler, "-fsyntax-only", "-"], source=gnu).returncode == 1
        succeeded([compiler, "-std=gnu11", "-", "-o", binary], source=gnu)
        succeeded([binary])

        succeeded([compiler, "-pthread", "-", "-o", binary], source=br"""
#include <pthread.h>
#include <stdarg.h>
_Thread_local int number = 7;
static void *worker(void *arg) {
    int *result = arg;
    *result = number++;
    return 0;
}
static long double sum(int count, ...) {
    va_list list;
    va_start(list, count);
    long double value = 0.0L;
    for (int i = 0; i < count; i++)
        value += va_arg(list, long double);
    va_end(list);
    return value;
}
int main(void) {
    int result = 0;
    number = 11;
    pthread_t thread;
    if (pthread_create(&thread, 0, worker, &result)
        || pthread_join(thread, 0) || result != 7 || number != 11)
        return 1;
    return sum(10,1.L,2.L,3.L,4.L,5.L,6.L,7.L,8.L,9.L,10.L) != 55.L;
}
""")
        succeeded([binary])

        succeeded([compiler, "-", "-o", binary], source=br"""
struct Bits { unsigned number: 5; signed delta: 6; };
int main(void) {
    int count = 4;
    int times = 0;
again:
    ;
    int data[count];
    if (sizeof data != 4 * sizeof(int))
        return 1;
    struct Bits bits = { 12, -5 };
    data[3] = (bits.number += 3) + bits.delta;
    if (data[3] != 10)
        return 2;
    if (++times < 300000)
        goto again;
    return 0;
}
""")
        succeeded([binary])

        failed = command([compiler, "-fsyntax-only", "-"], source=b"""
int main(int argc, char **argv) {
    goto after;
    int array[argc];
after:
    return 0;
}
""")
        assert failed.returncode == 1 and b"jump enters the scope" in failed.stderr

        # Exercise Bison stack relocation and large Flex input under the sanitizers.
        expression = b"(" * 400 + b"0" + b")" * 400
        source = b"/*" + b"x" * 20000 + b"*/ int main(void) { return " + expression + b"; }"
        succeeded([compiler, "-", "-o", binary], source=source)
        succeeded([binary])

        library = root / "libsample.so"
        succeeded([compiler, "-shared", "-", "-o", library], source=br"""
_Thread_local long current = 9;
long step(void) { return current++; }
""")
        succeeded([compiler, "-", "-L", tmp, "-lsample", "-Wl,-rpath,$ORIGIN", "-o", binary],
                  source=br"""
#include <stddef.h>
struct S { int first; long values[2]; };
_Static_assert(offsetof(struct S, values[1]) == 16, "wrong nested offset");
extern long step(void);
int main(void) { return step() != 9 || step() != 10; }
""")
        succeeded([binary])

        succeeded([compiler, "-std=gnu11", "-", "-o", binary], source=br"""
int main(void) {
    long calls = 0;
    typeof(calls) values[] = { [1 ... 3] = ++calls, [2] = 8 };
    return calls != 1 || values[0] || values[1] != 1 || values[2] != 8 || values[3] != 1;
}
""")
        succeeded([binary])
    print("mcc smoke tests passed")


def suite(compiler, directory, mode):
    def case(source):
        try:
            with tempfile.TemporaryDirectory(prefix="mcc-case-") as tmp:
                binary = Path(tmp) / "program"
                flags = ["-std=" + mode]
                if mode == "c11":
                    flags.append("-pedantic-errors")
                result = command([compiler, *flags, source, "-lm", "-o", binary])
                if result.returncode:
                    detail = result.stderr.decode(errors="replace").splitlines()
                    return False, "\n".join(detail[:4]) if detail else "compiler exited unsuccessfully"
                program = subprocess.run([binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         timeout=5)
                expected = source.with_suffix(".c.expected").read_bytes()
                if program.returncode or program.stdout != expected:
                    return False, (
                        f"exit {program.returncode}, expected stdout/stderr {expected[:100]!r}, "
                        f"got {program.stdout[:100]!r}"
                    )
        except subprocess.TimeoutExpired:
            return False, "timed out"
        return True, ""

    cases = sorted(directory.glob("*.c"))
    if not cases:
        raise RuntimeError(f"no cases in {directory}")
    if mode == "c11":
        cases = [source for source in cases if source.name not in STRICT_EXCEPTIONS]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(case, cases))
    for source, (passed, detail) in zip(cases, results):
        if not passed:
            print(f"{source.name}: {detail}")
    total = sum(passed for passed, _ in results)
    print(f"{mode}: {total}/{len(cases)} cases passed")
    return total == len(cases)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--suite", type=Path)
    parser.add_argument("--mode", choices=["c11", "gnu11"], default="gnu11")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    if args.suite:
        return not suite(compiler, args.suite.resolve(), args.mode)
    smoke(compiler)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
