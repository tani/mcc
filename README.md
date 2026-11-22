# mcc

mcc is a compiler written in C11 for little-endian AArch64 Linux with musl. Flex scans preprocessing tokens, mcc expands macros and includes, and Bison parses the expanded source. The front end lowers typed expressions and statements to instructions with explicit branches, calls, and memory operations. The AArch64 backend emits GNU assembly. The musl toolchain assembles and links it.

## Build and use

On AArch64 Linux, enter the pinned development shell and build with its musl GCC.

```sh
nix develop path:.#musl
make CC="$CC"
printf '#include <stdio.h>\nint main(void) { puts("hello from mcc"); }\n' |
  build/mcc - -o hello
./hello
```

`build/mcc` defaults to `-std=c11`. Select `-std=gnu11` for the GNU features implemented so far. Use `-E` to inspect expanded source, `-fsyntax-only` to check a translation unit, `-S` for assembly, and `-c` for an ELF object. Multiple source and object inputs can be linked together. Link options include `-static`, `-shared`, `-L`, `-l`, and `-pthread`. The shell supplies `MCC_TOOLCHAIN`, `MCC_COMPILER_INCLUDE`, and `MCC_SYSTEM_INCLUDE`.

## Validation

```sh
make check
make check-strict
make check-suite
make check-extended
make check-bootstrap
```

`check` exercises the driver, owned preprocessor, generated AArch64 code, and interoperability with musl GCC. The `check-strict` and `check-suite` targets run the cloned `c-testsuite`. The pinned GCC reference passes 212 cases with `-std=c11 -pedantic-errors` and 220 with `-std=gnu11`.

`check-extended` compares C11 pointer constraints and execution tests with musl GCC. It checks required diagnostics, result types, and generated code. `check-bootstrap` builds mcc with GCC, rebuilds it twice in strict C11 mode, compares the self-hosted binaries byte for byte, and runs the tests with the final compiler. Both targets use temporary build or test directories and exit nonzero on failure.

### GCC safety checks

Run `make check-gcc` in the musl development shell on AArch64 Linux. Ordinary GCC builds also use the strict warning set in the Makefile and treat warnings as errors. Select individual profiles with, for example, `make check-gcc GCC_PROFILES='harden analyzer'`.

| Profile | Checks |
| --- | --- |
| `harden` | Optimized LTO build with Fortify 3, stack protection, PIE, RELRO, immediate binding, AArch64 branch protection, and GCC control-flow hardening. Checks ELF properties before running the tests. |
| `analyzer` | GCC `-fanalyzer` at `-O0`. Bison's generated parser keeps its uninitialized-value diagnostics visible as warnings. |
| `undefined` | musl build with trapping UBSan, strict bounds checks, and additional floating-point checks. A signed-overflow probe verifies that trapping is active. |
| `address` | Native GCC build with ASan, LeakSanitizer, UBSan, and invalid pointer-pair checks. |
| `thread` and `hwaddress` | Separate ThreadSanitizer and HWAddressSanitizer builds. |

The native sanitizer profiles use glibc for the instrumented mcc process because the pinned musl toolchain lacks those runtime libraries. Programs produced by mcc use musl in every profile. The runtime profiles run the smoke and extended tests and both upstream suites when the checkout is present. Kernel-only instrumentation and options that require an MTE-capable host or a ShadowCallStack runtime are outside this setup.

## Compatibility

The implementation covers integer and floating-point expressions, pointers, declarators, aggregates, aggregate calls and returns, scalar and aggregate variadic arguments, thread-local objects, generic selections, and one-dimensional automatic variable-length arrays. GNU support includes statement expressions, omitted conditional operands, case ranges, several preprocessor extensions, and selected attributes.

Full ISO C11 and GCC extension conformance are still in progress. Outstanding areas include C11 atomics, complex arithmetic, broader variably modified types, static bit-field initialization, extended assembly, and AArch64 intrinsics. OpenMP and OpenACC are outside the project scope. The [GNU extension index](docs/extensions.md) describes GCC's features.
