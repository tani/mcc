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

`build/mcc` defaults to `-std=c11`. Select `-std=gnu11` for the GNU features implemented so far. Use `-E` to inspect expanded source, `-fsyntax-only` to check a translation unit, `-S` for assembly, and `-c` for an ELF object. Multiple source and object inputs can be linked together. Link options include `-static`, `-shared`, `-L`, `-l`, and `-pthread`. The shell supplies `MCC_TOOLCHAIN` and `MCC_SYSTEM_INCLUDE`.

## Validation

```sh
make check
make check-strict
make check-suite
```

`check` exercises the driver, owned preprocessor, generated AArch64 code, and interoperability with musl GCC. The other targets execute the cloned `c-testsuite` from temporary directories. The pinned GCC reference passes 212 cases with `-std=c11 -pedantic-errors` and 220 with `-std=gnu11`. A suite target exits nonzero on any failure.

## Compatibility

The implementation covers integer and floating-point expressions, pointers, declarators, aggregates, aggregate calls and returns, scalar and aggregate variadic arguments, thread-local objects, generic selections, and one-dimensional automatic variable-length arrays. GNU support includes statement expressions, omitted conditional operands, case ranges, several preprocessor extensions, and selected attributes.

Full ISO C11 and GCC extension conformance are still in progress. Outstanding areas include C11 atomics, complex arithmetic, broader variably modified types, static bit-field initialization, extended assembly, and AArch64 intrinsics. OpenMP and OpenACC are outside the project scope. The [GNU extension index](docs/extensions.md) describes GCC's features.
