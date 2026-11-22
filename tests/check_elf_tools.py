"""Run the mcc, mas and mld pipeline and check object diagnostics."""

import os
from pathlib import Path
from struct import unpack_from
import subprocess
import sys
from tempfile import TemporaryDirectory


STARTUP = {
    "aarch64": (
        183, b"AArch64",
        b"mov x0, #20\nbl calculate\nmov x8, #93\nsvc #0\n",
    ),
    "x86_64": (
        62, b"x86-64",
        b"movl $20, %edi\ncall calculate\nmovl %eax, %edi\nmovl $60, %eax\nsyscall\n",
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
    # These phases are usable with just the three project binaries.
    result = subprocess.run(args, input=source, capture_output=True, timeout=15,
                            env={**os.environ, "PATH": ""})
    if result.returncode != status:
        raise RuntimeError(f"{args[0]} exited {result.returncode}: {result.stderr.decode()}")
    return result


def check_hosted(mcc, mld, root):
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
    command(mld, "--hosted", "-o", dynamic, main, "-L", root, "-lgroup")
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


def check(mcc, mas, mld, directory):
    version = command(mcc, "--version").stdout
    objects = {}
    for arch, (machine, name, startup) in STARTUP.items():
        root = directory / arch
        root.mkdir()
        flag = f"--target={arch}-linux-gnu"
        start, body, output = (root / filename for filename in ("start.o", "body.o", "program"))
        start_source = root / "start.s"
        start_source.write_bytes(b".global start\nstart:\n" + startup)
        assembly = command(mcc, flag, "-S", "-o", "-", "-", source=SOURCE).stdout
        obj = command(mas, flag, "-o", "-", "-", source=assembly).stdout
        assert obj[:6] == b"\x7fELF\x02\x01" and unpack_from("<HH", obj, 16) == (1, machine)
        body.write_bytes(obj)
        command(mas, flag, "-o", start, start_source)
        command(mld, flag, "-e", "start", "-o", output, start, "-", source=obj)
        program = output.read_bytes()
        assert unpack_from("<HH", program, 16) == (2, machine)
        assert os.access(output, os.X_OK)
        driven = root / "driven"
        command(mcc, flag, "-nostdlib", "-Wl,-e,start", "-o", driven, start_source, "-", source=SOURCE)
        assert driven.read_bytes() == program
        if name in version:
            command(output, status=42)
        assert b"undefined symbol 'calculate'" in command(
            mld, flag, "-e", "start", "-o", output, start, status=1
        ).stderr
        assert b"multiple definitions" in command(
            mld, flag, "-e", "start", "-o", output, start, body, body, status=1
        ).stderr
        bad = command(mas, flag, "-o", start, "-", source=b"invalid_instruction\n", status=1)
        assert b"<stdin>:1:1:" in bad.stderr
        assert output.read_bytes() == program and start.read_bytes().startswith(b"\x7fELF")
        objects[arch] = body

    failed = command(mld, "--target=aarch64-linux-gnu", "-o", directory / "mixed",
                     objects["x86_64"], status=1)
    assert b"does not match the selected target" in failed.stderr
    broken = directory / "truncated.o"
    broken.write_bytes(b"\x7fELF")
    failed = command(mld, "-o", directory / "broken", broken, status=1)
    assert b"truncated ELF" in failed.stderr
    assert not (directory / "mixed").exists() and not (directory / "broken").exists()
    check_hosted(mcc, mld, directory / "hosted")
    print("mas and mld ELF tests passed")


if __name__ == "__main__":
    with TemporaryDirectory(prefix="mcc-elf-") as temporary:
        check(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(temporary))
