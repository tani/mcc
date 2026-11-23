# mcc

mcc is a C11 compiler for x86-64 and little-endian AArch64 Linux. It emits GNU-style assembly and uses the built-in `mas` assembler and `mld` linker in the same process. Both tools also run as standalone commands. The linker uses the target's glibc.

## Build and use

Build on x86-64 or AArch64 Linux with GCC, GNU Make, Bison, Flex, GNU binutils, and glibc development files. The tests also use Python 3. The Nix development shell supplies these dependencies and configures the glibc paths. Enter it with `nix develop path:.` if you use Nix.

```sh
make
printf '#include <stdio.h>\nint main(void) { puts("hello from mcc"); }\n' |
  build/mcc - -o hello
./hello
```

`build/mcc` defaults to `-std=c11`. Select `-std=gnu11` for the supported GNU extensions. C input can come from a `.c` or `.i` file or from stdin (`-`). Use `-o FILE` to name the output.

| Option | Result |
| --- | --- |
| `-E` | Write preprocessed C to stdout by default. Use `-P` to omit line markers. |
| `-fsyntax-only` | Check C source without producing an output file. |
| `-S` | Compile C to GNU-style assembly (`.s`). |
| `-c` | Compile C or assemble a `.s` file into an ELF64 object (`.o`). |
| No stage option | Link C, assembly, objects, archives (`.a`), or shared libraries (`.so`) into `a.out` by default. |

Executables are position-independent (PIE) by default. Use `-no-pie`, `-static`, or `-shared` to select a different output. Use `-L` and `-l` for library search paths and names, `-lm` for the math library, and `-pthread` to link pthreads. Pass linker options such as `-Wl,-rpath,/path/to/lib` through the driver.

### Targets and system files

`mcc`, `mas`, and `mld` default to the architecture they were built for. Select `--target=x86_64-linux-gnu` or `--target=aarch64-linux-gnu` to build for either architecture. `mcc` uses the target's glibc headers. Both `mcc` and `mld` link with the target's glibc startup objects and libraries.

Use `-isystem DIR` to add a system header search directory or `-idirafter DIR`
to search a directory after the standard headers. `-Wsystem-headers` enables
warnings from system headers.

| Environment variable | Purpose |
| --- | --- |
| `MCC_SYSROOT` | Root for target header and library searches. |
| `MCC_SYSTEM_INCLUDE` | Override the glibc header directory. |
| `MCC_SYSTEM_LIB` | Override the glibc library and startup object directory. |
| `MCC_STATIC_LIB` | Add a search directory for static glibc archives. |
| `MCC_COMPILER_INCLUDE` | Override the path to mcc's bundled headers, which defaults to `include/` in the source checkout used to build it. |

Cross-compilation parses and folds `long double` constants using the compiler host's `long double` format before encoding them for the target.

`mcc` and `mld` embed their startup objects and `libmccrt.a` for both targets. The bundled runtime supplies binary128 arithmetic and conversions, the AArch64 out-of-line atomic ABI, and the C unwinding routines used by static glibc. The linker adds these objects and glibc without looking up `libgcc` or GCC startup files. Static linking uses the `_dl_find_object` interface from glibc 2.35 or later. A dynamically linked glibc can load its configured unwinder when the program uses backtraces or thread cancellation.

## Assembling and linking ELF objects

`make` builds `build/mcc`, `build/mas`, and `build/mld`. Use `make mas mld` to build just the standalone assembler and linker.

- `mas [-o FILE] INPUT...` assembles GNU-style AArch64 or x86-64 AT&T syntax into an ELF64 relocatable object. It handles common instructions and directives emitted by mcc. Use `-` to read assembly from stdin. The default output is `a.o`.
- `mld [--target=TARGET] [OPTIONS] INPUT...` links ELF64 objects with the target's glibc. Use `-` to read an object from stdin. The default output is `a.out`, and the default entry for an executable is `_start` from glibc's startup objects. `mld` supports `-no-pie`, `-static`, `-shared`, `-L`, `-l`, `-pthread`, `-rpath`, and `-soname`. It reads indexed GNU archives and the `INPUT`, `GROUP`, `AS_NEEDED`, and `OUTPUT_FORMAT` directives used by glibc library scripts.

Use the three tools directly to see the assembly and ELF object produced along the way.

```sh
printf '#include <stdio.h>\nint main(void) { puts("hello from mld"); }\n' |
  build/mcc -S -o hello.s -
build/mas -o hello.o hello.s
build/mld -o hello hello.o
./hello
```

## Validation

```sh
make check
make check-extended
make check-bootstrap
```

`check` exercises the driver, preprocessor, native code generation, interoperability with GCC, and the mcc–mas–mld pipeline. `check-extended` compares C11 pointer constraints and execution tests with glibc GCC. It checks required diagnostics, result types, and generated code. `check-bootstrap` builds mcc with GCC, rebuilds it twice in strict C11 mode, compares the self-hosted binaries byte for byte, and runs the tests with the final compiler. Both targets use temporary build or test directories and exit nonzero on failure.

`check-strict` and `check-suite` run the upstream cases in strict C11 and GNU11 mode, respectively. Clone the suite from the repository root before running either target.

```sh
git clone https://github.com/c-testsuite/c-testsuite.git c-testsuite
make check-strict
make check-suite
```

The pinned glibc GCC reference passes 212 cases with `-std=c11 -pedantic-errors` and 220 with `-std=gnu11`.

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

## Source layout

- [`src/cc/`](src/cc/) contains the driver, Flex scanner, preprocessor, Bison parser, type checking, intermediate representation, and assembly backends.
- [`src/as/`](src/as/) contains the assembler and the instruction encoders for AArch64 and x86-64.
- [`src/ld/`](src/ld/) contains the linker, ELF and archive input handling, relocations, and startup and library selection.
- [`src/rt/`](src/rt/) contains the runtime and startup objects built into the compiler and linker.
- [`include/`](include/) contains the compiler's bundled headers. [`tests/`](tests/) and [`c-testsuite-extended/`](c-testsuite-extended/) contain the local validation scripts and cases.

## Compatibility

The implementation covers integer and floating-point expressions, pointers, declarators, aggregates, aggregate calls and returns, scalar and aggregate variadic arguments, thread-local objects, generic selections, and one-dimensional automatic variable-length arrays. GNU support includes statement expressions, omitted conditional operands, case ranges, several preprocessor extensions, and selected attributes.

Full ISO C11 and GCC extension conformance are still in progress. Outstanding areas include broader variably modified types, additional numeric types, target-specific asm constraints, and AArch64 intrinsics. OpenMP and OpenACC are outside the project scope. The [GNU extension index](docs/extensions.md) tracks current support and its limitations.
