# mcc

mcc is a compiler written in C11 for x86-64 and little-endian AArch64 Linux with glibc. Flex scans preprocessing tokens, mcc expands macros and includes, and Bison parses the expanded source. The front end lowers typed expressions and statements to instructions with explicit branches, calls, and memory operations. The target backend emits GNU assembly. The driver calls the built-in mas assembler and mld linker in the same process.

## Build and use

On x86-64 and AArch64 Linux, the default development shell supplies GCC, glibc headers and libraries, Bison, Flex, and the test tools.

```sh
nix develop path:.
make
printf '#include <stdio.h>\nint main(void) { puts("hello from mcc"); }\n' |
  build/mcc - -o hello
./hello
```

`build/mcc` defaults to `-std=c11`. Select `-std=gnu11` for the GNU features implemented so far. Use `-E` to inspect expanded source, `-fsyntax-only` to check a translation unit, `-S` for assembly, and `-c` for an ELF object. Multiple source and object inputs can be linked together. Link options include `-static`, `-shared`, `-L`, `-l`, and `-pthread`. Use `-lm` for programs that call the math library and `-nostdlib` for a freestanding link with an entry point supplied by the program.

The driver targets the architecture of the running mcc binary by default. Use `--target=x86_64-linux-gnu` or `--target=aarch64-linux-gnu` to select a target explicitly. For hosted links, mld reads the target's startup objects and libraries and writes the ELF output itself. GCC can build mcc itself and serve as the reference compiler in the tests.

Set `MCC_SYSROOT` to locate a target's headers and glibc libraries under a common root. `MCC_SYSTEM_INCLUDE` and `MCC_SYSTEM_LIB` override the glibc header and library directories. Set `MCC_STATIC_LIB` when static glibc archives live in a separate directory. `MCC_RUNTIME_LIB` overrides the directory for compiler runtime objects and archives such as `libgcc.a`. Nix sets these directory overrides for its layout. Use `MCC_COMPILER_INCLUDE` to relocate mcc's own headers.

Cross-compilation parses and folds `long double` constants using the compiler host's `long double` format before encoding them for the target.

## Assembling and linking ELF objects

`make` also builds the standalone tools `build/mas` and `build/mld`. You can build them separately with `make mas mld`. Both accept the same `--target` values as mcc and default to their native architecture.

- `mas [-o FILE] INPUT...` assembles GNU-style AArch64 or x86-64 AT&T syntax into an ELF64 relocatable object. It handles common instructions and directives emitted by mcc. Use `-` to read assembly from stdin. The default output is `a.o`.
- `mld [-e SYMBOL] [-o FILE] INPUT...` resolves symbols and relocations in ELF64 objects and writes a freestanding static executable. Its default entry is `_start`, and its default output is `a.out`. Inputs can come from mas or GNU tools.
- `mld --hosted [--target=TARGET] [OPTIONS] INPUT...` links with glibc. It supports PIE by default, `-no-pie`, `-static`, `-shared`, `-L`, `-l`, `-pthread`, `-rpath`, and `-soname`. It reads indexed GNU archives and the `INPUT`, `GROUP`, `AS_NEEDED`, and `OUTPUT_FORMAT` directives used by glibc library scripts.

For example, these commands build a freestanding x86-64 executable that exits with status 42.

```sh
printf 'int twice(int x) { return 2 * x; }\n' |
  build/mcc --target=x86_64-linux-gnu -S -o twice.s -
build/mas --target=x86_64-linux-gnu -o twice.o twice.s
build/mas --target=x86_64-linux-gnu -o start.o - <<'ASM'
.global _start
_start:
    movl $21, %edi
    call twice
    movl %eax, %edi
    movl $60, %eax
    syscall
ASM
build/mld --target=x86_64-linux-gnu -o twice start.o twice.o
```

## Validation

```sh
make check
make check-strict
make check-suite
make check-extended
make check-bootstrap
```

`check` exercises the driver, preprocessor, native code generation, interoperability with GCC, and the mcc–mas–mld pipeline. The `check-strict` and `check-suite` targets run the cloned `c-testsuite`. The pinned glibc GCC reference passes 212 cases with `-std=c11 -pedantic-errors` and 220 with `-std=gnu11`.

`check-extended` compares C11 pointer constraints and execution tests with glibc GCC. It checks required diagnostics, result types, and generated code. `check-bootstrap` builds mcc with GCC, rebuilds it twice in strict C11 mode, compares the self-hosted binaries byte for byte, and runs the tests with the final compiler. Both targets use temporary build or test directories and exit nonzero on failure.

### GCC safety checks

Run `make check-gcc` with the pinned GCC 16 from the default development shell on AArch64 Linux. Ordinary GCC builds also use the strict warning set in the Makefile and treat warnings as errors. Select individual profiles with, for example, `make check-gcc GCC_PROFILES='harden analyzer'`.

| Profile | Checks |
| --- | --- |
| `harden` | Optimized LTO build with Fortify 3, stack protection, PIE, RELRO, immediate binding, AArch64 branch protection, and GCC control-flow hardening. Checks ELF properties before running the tests. |
| `analyzer` | GCC `-fanalyzer` at `-O0`. Bison's generated parser keeps its uninitialized-value diagnostics visible as warnings. |
| `undefined` | Trapping UBSan, strict bounds checks, and additional floating-point checks. A signed-overflow probe verifies that trapping is active. |
| `address` | Native GCC build with ASan, LeakSanitizer, UBSan, and invalid pointer-pair checks. |
| `thread` and `hwaddress` | Separate ThreadSanitizer and HWAddressSanitizer builds. |

All profiles use the same native GCC and glibc toolchain. The runtime profiles run the smoke and extended tests and both upstream suites when the checkout is present. Kernel-only instrumentation and options that require an MTE-capable host or a ShadowCallStack runtime are outside this setup.

## Compatibility

The implementation covers integer and floating-point expressions, pointers, declarators, aggregates, aggregate calls and returns, scalar and aggregate variadic arguments, thread-local objects, generic selections, and one-dimensional automatic variable-length arrays. GNU support includes statement expressions, omitted conditional operands, case ranges, several preprocessor extensions, and selected attributes.

Full ISO C11 and GCC extension conformance are still in progress. Outstanding areas include C11 atomics, complex arithmetic, broader variably modified types, static bit-field initialization, extended assembly, and AArch64 intrinsics. OpenMP and OpenACC are outside the project scope. The [GNU extension index](docs/extensions.md) describes GCC's features.
