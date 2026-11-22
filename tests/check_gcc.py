"""Build and exercise mcc with the pinned GCC hardening and diagnostic profiles."""
import argparse
import os
from pathlib import Path
import platform
import resource
import signal
import subprocess
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parent.parent
HARDEN = """
-O2 -g -flto -D_FORTIFY_SOURCE=3 -fPIE
-fstack-protector-strong -fstack-clash-protection -ftrivial-auto-var-init=zero
-fharden-compares -fharden-conditional-branches
-fharden-control-flow-redundancy -mbranch-protection=standard
-fzero-call-used-regs=used-gpr
""".split()
UBSAN = "undefined,bounds-strict,float-divide-by-zero,float-cast-overflow"
DEBUG = ["-O1", "-g", "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls"]
STOP_ON_UB = "-fno-sanitize-recover=all"
PROFILES = {
    "harden": ("MCC_TOOLCHAIN", HARDEN, [
        "-Werror", "-pie", "-Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code,--fatal-warnings",
    ]),
    "analyzer": ("MCC_TOOLCHAIN", ["-O0", "-g", "-fanalyzer"], []),
    "undefined": ("MCC_TOOLCHAIN", [
        *DEBUG, "-fstrict-overflow", f"-fsanitize={UBSAN}", "-fsanitize-trap=all", STOP_ON_UB,
    ], []),
    "address": ("MCC_GCC_HOST", [
        *DEBUG, "-fstrict-overflow",
        f"-fsanitize=address,leak,pointer-compare,pointer-subtract,{UBSAN}", STOP_ON_UB,
    ], []),
    "thread": ("MCC_GCC_HOST", [*DEBUG, "-fsanitize=thread", STOP_ON_UB], []),
    "hwaddress": ("MCC_GCC_HOST", [*DEBUG, "-fsanitize=hwaddress", STOP_ON_UB], []),
}


def environment(profile):
    env = {key: value for key, value in os.environ.items()
           if key not in {"MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES"}}
    if profile == "address":
        env["ASAN_OPTIONS"] = (
            "detect_leaks=1:detect_invalid_pointer_pairs=2:detect_stack_use_after_return=1:"
            "strict_string_checks=1:halt_on_error=1:exitcode=99"
        )
        env["LSAN_OPTIONS"] = "exitcode=99"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1:exitcode=99"
    if profile == "thread":
        env["TSAN_OPTIONS"] = "halt_on_error=1:exitcode=99"
    if profile == "hwaddress":
        env["HWASAN_OPTIONS"] = "halt_on_error=1:exitcode=99"
    return env


def verify_elf(binary):
    info = subprocess.check_output(
        ["readelf", "-W", "-h", "-l", "-d", "--dyn-syms", binary],
        cwd=ROOT, text=True,
    )
    stack = next((line.split() for line in info.splitlines() if "GNU_STACK" in line), [])
    required = {
        "PIE": any("DYN" in line for line in info.splitlines() if "Type:" in line),
        "GNU_RELRO": "GNU_RELRO" in info,
        "BIND_NOW": "BIND_NOW" in info,
        "non-executable stack": bool(stack) and stack[-2] == "RW",
        "stack protector": "__stack_chk_fail" in info,
    }
    missing = [flag for flag, present in required.items() if not present]
    if missing:
        raise RuntimeError("missing ELF hardening: " + ", ".join(missing))


def verify_overflow_trap(compiler, flags, build, env):
    probe = build / "overflow-probe"
    source = """
#include <limits.h>
volatile int value = INT_MAX;
int main(void) {
    volatile int sum = value + 1;
    return sum == 0;
}
"""
    subprocess.run([compiler, *flags, "-x", "c", "-", "-o", probe],
                   input=source, text=True, env=env, check=True)
    result = subprocess.run([probe], env=env, timeout=10)
    if result.returncode != -signal.SIGTRAP:
        raise RuntimeError(f"UBSan overflow probe exited {result.returncode}, expected SIGTRAP")


def check(profile, build, jobs):
    compiler_variable, flags, extra_linker = PROFILES[profile]
    compiler = os.environ[compiler_variable]
    env = environment(profile)
    make = [
        "make", "-s", f"-j{jobs}", f"BUILD={build}", f"CC={compiler}",
        f"CFLAGS={' '.join(flags)}",
        f"LDFLAGS={' '.join(flags + extra_linker)}",
    ]
    if profile == "analyzer":
        # Bison's table-driven stack relocation and empty reduction confuse GCC's analyzer.
        make.append("PARSER_WARN=-Wno-unused-but-set-variable "
                    "-Wno-error=analyzer-use-of-uninitialized-value -fdiagnostics-path-format=none")
    print(f"GCC check: {profile}", flush=True)
    subprocess.run([*make, "all"], cwd=ROOT, env=env, check=True)
    if profile == "analyzer":
        return
    if profile == "harden":
        verify_elf(build / "mcc")
    if profile == "undefined":
        verify_overflow_trap(compiler, flags, build, env)
    goals = ["check", "check-extended"]
    if (ROOT / "c-testsuite/tests/single-exec").is_dir():
        goals += ["check-strict", "check-suite"]
    else:
        print("Upstream c-testsuite checkout absent. Running local tests.", flush=True)
    subprocess.run([*make, *goals], cwd=ROOT, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profiles", nargs="*", metavar="PROFILE",
                        help="available: " + ", ".join(PROFILES))
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if platform.machine() != "aarch64":
        parser.error("these checks run AArch64 Linux programs and need an AArch64 host")
    _, core_limit = resource.getrlimit(resource.RLIMIT_CORE)
    resource.setrlimit(resource.RLIMIT_CORE, (0, core_limit))
    profiles = args.profiles or list(PROFILES)
    for profile in profiles:
        if profile not in PROFILES:
            parser.error(f"unknown profile: {profile}")
        name = PROFILES[profile][0]
        if not os.environ.get(name):
            parser.error(f"{name} is required. Run this in the musl development shell.")

    failed = []
    with TemporaryDirectory(prefix="mcc-gcc-") as directory:
        for profile in profiles:
            try:
                check(profile, Path(directory) / profile, args.jobs)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                print(f"GCC check failed: {profile}: {error}", flush=True)
                failed.append(profile)
    print(f"GCC checks: {len(profiles) - len(failed)}/{len(profiles)} profiles passed")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
