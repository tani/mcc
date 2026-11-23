"""Run the mcc, mas and mld pipeline and check object diagnostics."""

import os
from pathlib import Path
import re
from shutil import copy2
from struct import unpack_from
import subprocess
import sys
from tempfile import TemporaryDirectory


MAIN_ASSEMBLY = {
    "aarch64": (
        183, b"AArch64",
        b"mov x0, #20\nb calculate\n",
    ),
    "x86_64": (
        62, b"x86-64",
        b"movl $20, %edi\njmp calculate\n",
    ),
}

SOURCE = b"""
int counter;
int offset = 2;
int *pointer = &offset;
static int doubled(int value) { return value * 2; }
int calculate(int value) {
    const char *word = "mcc";
    double amount = 0.5 * (double)value;
    counter += word[0] == 'm';
    return counter == 1 && amount == 10.0 ? doubled(value) + *pointer : 1;
}
"""


def command(*args, source=None, status=0):
    # The tools can run with an empty PATH.
    result = subprocess.run(args, input=source, capture_output=True, timeout=15,
                            env={**os.environ, "PATH": ""})
    if result.returncode != status:
        raise RuntimeError(f"{args[0]} exited {result.returncode}: {result.stderr.decode()}")
    return result


def check_libraries(mcc, mld, root):
    root.mkdir()
    main = root / "main.o"
    first = root / "first.o"
    second = root / "second.o"
    last = root / "long_archive_member_name.o"
    command(mcc, "-c", "-", "-o", main, source=b"""
#include <stdio.h>
_Thread_local int count = 40;
extern int first(int);
int main(void) {
    puts("hosted");
    return first(count++);
}
""")
    command(mcc, "-c", "-", "-o", first, source=b"extern int second(int); int first(int n) { return second(n); }")
    command(mcc, "-c", "-", "-o", second, source=b"extern int last(int); int second(int n) { return last(n); }")
    command(mcc, "-c", "-", "-o", last, source=b"int last(int n) { return n + 2; }")
    subprocess.run(["ar", "rcs", root / "libfirst.a", first, last], check=True)
    subprocess.run(["ar", "rcs", root / "libsecond.a", second], check=True)
    (root / "libgroup.so").write_text("GROUP ( libfirst.a libsecond.a )\n")

    dynamic = root / "dynamic"
    command(mld, "-o", dynamic, main, "-L", root, "-lgroup")
    assert command(dynamic, status=42).stdout == b"hosted\n"

    static = root / "static"
    command(mcc, "-static", "-Wl,--start-group", main, "-L", root, "-lfirst", "-lsecond",
            "-Wl,--end-group", "-o", static)
    assert command(static, status=42).stdout == b"hosted\n"

    library = root / "libcallback.so"
    command(mcc, "-shared", "-", "-o", library,
            source=b"extern int callback(void); int call(void) { return callback(); }")
    command(mcc, "-", library, "-o", dynamic, source=b"""
extern int call(void);
int callback(void) { return 42; }
int main(void) { return call(); }
""")
    command(dynamic, status=42)

    # The bundled shared-library startup object finalizes only its own DSO.
    plugin = root / "libcleanup.so"
    command(mcc, "-shared", "-", "-o", plugin, source=b"""
extern void *__dso_handle;
extern int __cxa_atexit(void (*)(void *), void *, void *);
static void cleanup(void *p) { ++*(int *)p; }
int install_cleanup(int *count) { return __cxa_atexit(cleanup, count, __dso_handle); }
""")
    command(mcc, "-", "-o", dynamic, source=b"""
#include <dlfcn.h>
int main(int argc, char **argv) {
    int count = 0;
    void *module = argc == 2 ? dlopen(argv[1], RTLD_NOW) : 0;
    if (!module) return 1;
    union { void *address; int (*install)(int *); } symbol;
    symbol.address = dlsym(module, "install_cleanup");
    if (!symbol.address || symbol.install(&count) || count != 0 || dlclose(module)) return 2;
    return count != 1;
}
""")
    command(dynamic, plugin)


def check_runtime(mld, directory):
    source = Path(__file__).with_name("runtime.c")
    compiler = os.environ.get("MCC_TOOLCHAIN") or "gcc"
    obj = directory / "runtime.o"
    # GCC supplies an independent binary128 ABI caller and DWARF unwind data.
    subprocess.run([compiler, "-std=gnu11", "-O2", "-frounding-math", "-fexceptions",
                    "-funwind-tables", "-c", source, "-o", obj], check=True)
    output = directory / "runtime"
    command(mld, "-static", "-pthread", "-o", output, obj, "-lm")
    command(output)


def check(mcc, mas, mld, directory):
    # Test the compiler in a directory containing only its own executable.
    installed = directory / "standalone"
    installed.mkdir()
    mcc = copy2(mcc, installed / "mcc")
    version = command(mcc, "--version").stdout
    native = next(arch for arch, (_, name, _) in MAIN_ASSEMBLY.items() if name in version)
    objects = {}
    for arch, (machine, _, _) in MAIN_ASSEMBLY.items():
        root = directory / arch
        root.mkdir()
        flag = f"--target={arch}-linux-gnu"
        assembly = command(mcc, flag, "-S", "-o", "-", "-", source=SOURCE).stdout
        obj = command(mas, flag, "-o", "-", "-", source=assembly).stdout
        assert obj[:6] == b"\x7fELF\x02\x01" and unpack_from("<HH", obj, 16) == (1, machine)
        body = root / "body.o"
        body.write_bytes(obj)
        bad = command(mas, flag, "-o", body, "-", source=b"invalid_instruction\n", status=1)
        assert b"<stdin>:1:1:" in bad.stderr
        assert body.read_bytes() == obj
        objects[arch] = body

    root = directory / native
    main, body, output = root / "main.o", objects[native], root / "program"
    main_source = root / "main.s"
    machine, _, assembly = MAIN_ASSEMBLY[native]
    main_source.write_bytes(b".global main\nmain:\n" + assembly)
    command(mas, "-o", main, main_source)
    command(mld, "-o", output, main, "-", source=body.read_bytes())
    program = output.read_bytes()
    assert unpack_from("<HH", program, 16) == (3, machine)
    assert os.access(output, os.X_OK)
    command(output, status=42)
    driven = root / "driven"
    command(mcc, "-o", driven, main_source, "-", source=SOURCE)
    assert driven.read_bytes() == program
    assert b"undefined symbol 'calculate'" in command(mld, "-o", output, main, status=1).stderr
    assert b"multiple definitions" in command(mld, "-o", output, main, body, body, status=1).stderr
    assert output.read_bytes() == program

    unique = root / "unique.o"
    unique_copy = root / "unique-copy.o"
    unique_source = b"""
.data
.global unique_value
.type unique_value, %gnu_unique_object
unique_value: .quad 17
.size unique_value, 8
"""
    command(mas, "-o", unique, "-", source=unique_source)
    unique_copy.write_bytes(unique.read_bytes())
    undefined_unique = root / "undefined-unique.o"
    command(mas, "-o", undefined_unique, "-", source=b"""
.type missing_unique, %gnu_unique_object
.data
.quad missing_unique
""")
    symbols = subprocess.run(["readelf", "-sW", undefined_unique], capture_output=True,
                             text=True, check=True).stdout
    assert re.search(r"OBJECT\s+GLOBAL\s+DEFAULT\s+UND\s+missing_unique", symbols), symbols
    unique_shared = root / "unique.so"
    command(mld, "-shared", "-o", unique_shared, unique)
    failed = command(mld, "-shared", "-o", unique_shared, unique, unique_copy, status=1)
    assert b"multiple definitions" in failed.stderr
    common = root / "common.o"
    compiler = os.environ.get("MCC_TOOLCHAIN") or "gcc"
    subprocess.run([compiler, "-fcommon", "-x", "c", "-c", "-", "-o", common],
                   input=b"unsigned long unique_value;\n", check=True)
    for operands in ((common, unique), (unique, common)):
        command(mld, "-shared", "-o", unique_shared, *operands)
    for artifact in (unique, unique_shared):
        symbols = subprocess.run(["readelf", "-sW", artifact], capture_output=True,
                                 text=True, check=True).stdout
        assert re.search(r"OBJECT\s+(?:UNIQUE|<OS specific>:\s*10)\s+DEFAULT\s+\S+\s+unique_value",
                         symbols), symbols
    unique_executable = root / "unique-program"
    command(mld, "-o", unique_executable, main, body, unique)
    symbols = subprocess.run(["readelf", "--dyn-syms", "-W", unique_executable],
                             capture_output=True, text=True, check=True).stdout
    assert "unique_value" not in symbols, symbols
    command(mld, "-E", "-o", unique_executable, main, body, unique)
    symbols = subprocess.run(["readelf", "--dyn-syms", "-W", unique_executable],
                             capture_output=True, text=True, check=True).stdout
    assert re.search(r"OBJECT\s+(?:UNIQUE|<OS specific>:\s*10)\s+DEFAULT\s+\S+\s+unique_value",
                     symbols), symbols

    foreign = next(obj for arch, obj in objects.items() if arch != native)
    failed = command(mld, "-o", directory / "mixed", foreign, status=1)
    assert b"does not match the selected target" in failed.stderr
    broken = directory / "truncated.o"
    broken.write_bytes(b"\x7fELF")
    failed = command(mld, "-o", directory / "broken", broken, status=1)
    assert b"truncated ELF" in failed.stderr
    assert not (directory / "mixed").exists() and not (directory / "broken").exists()
    check_libraries(mcc, mld, directory / "libraries")
    check_runtime(mld, directory)
    print("mas and mld ELF tests passed")


if __name__ == "__main__":
    with TemporaryDirectory(prefix="mcc-elf-") as temporary:
        check(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(temporary))
