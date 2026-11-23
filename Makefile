BUILD := build
PROGRAM := $(BUILD)/mcc
ASSEMBLER := $(BUILD)/mas
LINKER := $(BUILD)/mld
RUNTIME := $(BUILD)/rt
SEED := $(RUNTIME)/mcc-seed
RT_ARCHS := aarch64 x86_64
RT_NATIVE := $(shell uname -m)
RT_FILES := $(foreach arch,$(RT_ARCHS),$(addprefix $(RUNTIME)/$(arch)/,crt-exe.o crt-shared.o libmccrt.a))
RT_EMBEDDED := $(RUNTIME)/embedded.o
RT_SEED := $(RUNTIME)/seed-quad.o $(RUNTIME)/seed-quad-complex.o $(RUNTIME)/seed-decimal.o $(RUNTIME)/seed-float-bits.o $(RUNTIME)/$(RT_NATIVE)/fp.o
CC_OBJECTS := $(addprefix $(BUILD)/cc/,driver.o cpp.o frontend.o ir.o data.o inline_asm.o aarch64.o x86_64.o parser.o scanner.o)
ELF_COMMON := $(addprefix $(BUILD)/,elf_tools.o core.o)
AS_OBJECTS := $(addprefix $(BUILD)/as/,as.o aarch64.o x86_64.o)
LD_OBJECTS := $(addprefix $(BUILD)/ld/,ld.o driver.o input.o reloc.o)
WARN := -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2 -Wformat-signedness \
	-Wconversion -Wsign-conversion -Wcast-qual -Wcast-align=strict -Wcast-function-type \
	-Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes \
	-Wshadow -Wundef -Wvla -Walloca -Walloc-zero -Wwrite-strings -Wdouble-promotion \
	-Wnull-dereference -Warray-bounds=2 -Warray-parameter=2 -Wstringop-overflow=4 \
	-Wstringop-overread \
	-fstrict-flex-arrays=3 -Wstrict-flex-arrays \
	-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wimplicit-fallthrough=3 \
	-Wjump-misses-init -Wstrict-overflow=5 -Wshift-overflow=2 -Wshift-negative-value \
	-Wstrict-aliasing=3 -Wpointer-arith -Wrestrict -Wsizeof-pointer-div \
	-Wsizeof-pointer-memaccess -Wuse-after-free=3 -Wdangling-pointer=2 -Wtrampolines \
	-Wframe-larger-than=65536 -Wstack-usage=65536
PARSER_WARN ?= $(if $(filter -Wall,$(WARN)),-Wno-unused-but-set-variable)

.PHONY: all mas mld clean check check-suite check-strict check-extended check-bootstrap check-gcc check-gnu-extensions
all: $(PROGRAM) $(ASSEMBLER) $(LINKER)

mas: $(ASSEMBLER)
mld: $(LINKER)

$(BUILD)/cc:
	mkdir -p $@

$(BUILD)/cc/parser.c $(BUILD)/cc/parser.h &: src/cc/parser.y | $(BUILD)/cc
	bison -Wall -Werror -d -o $(BUILD)/cc/parser.c $<

$(BUILD)/cc/scanner.c: src/cc/scanner.l | $(BUILD)/cc
	flex -o $@ $<

$(BUILD)/%.o: src/%.c src/compiler.h src/atomic.h src/int128.h src/decimal.h src/float_builtins.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc -I$(BUILD)/cc -c -o $@ $<

$(BUILD)/cc/driver.o: override CPPFLAGS += -DMCC_BUILTIN_INCLUDE='"$(CURDIR)/include"'
$(BUILD)/cc/driver.o: src/as/as.h src/ld/ld.h src/elf_tools.h
$(addprefix $(BUILD)/cc/,frontend.o ir.o aarch64.o x86_64.o): $(BUILD)/cc/parser.h
$(BUILD)/cc/frontend.o: src/integer_bits.h src/integer_overflow.h src/complex_ops.h
$(addprefix $(BUILD)/cc/,frontend.o data.o): src/int128_float.h
$(addprefix $(BUILD)/cc/,frontend.o data.o ir.o): src/int128_math.h
$(addprefix $(BUILD)/cc/,frontend.o data.o): src/quad.h
$(BUILD)/cc/data.o: src/float16.h src/quad_bits.h
$(addprefix $(BUILD)/cc/,frontend.o inline_asm.o ir.o aarch64.o x86_64.o): src/cc/inline_asm.h
$(BUILD)/as/main.o $(AS_OBJECTS): src/as/as.h src/elf_tools.h
$(BUILD)/ld/main.o $(BUILD)/ld/runtime.o $(LD_OBJECTS): src/ld/ld.h src/elf_tools.h
$(BUILD)/elf_tools.o: src/elf_tools.h

$(BUILD)/cc/%.o: $(BUILD)/cc/%.c src/compiler.h src/atomic.h src/int128.h src/decimal.h src/float_builtins.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(GENERATED_WARN) -Isrc -I$(BUILD)/cc -c -o $@ $<

$(BUILD)/cc/parser.o: GENERATED_WARN = $(PARSER_WARN)
$(BUILD)/cc/scanner.o: GENERATED_WARN = $(SCANNER_WARN)

$(PROGRAM): $(CC_OBJECTS) $(AS_OBJECTS) $(LD_OBJECTS) $(ELF_COMMON) $(BUILD)/ld/runtime.o $(RT_EMBEDDED) $(RUNTIME)/$(RT_NATIVE)/libmccrt.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ASSEMBLER): $(BUILD)/as/main.o $(AS_OBJECTS) $(ELF_COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(LINKER): $(BUILD)/ld/main.o $(LD_OBJECTS) $(ELF_COMMON) $(BUILD)/ld/runtime.o $(RT_EMBEDDED)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(RUNTIME)/seed.o: src/ld/runtime.c src/ld/ld.h src/elf_tools.h src/compiler.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DMCC_RUNTIME_SEED -Isrc -c -o $@ $<

$(RUNTIME)/seed-quad.o: src/rt/quad.c src/rt/types.h src/rt/fp-round.h src/quad_bits.h src/int128_float.h src/int128_math.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(RUNTIME)/seed-quad-complex.o: src/rt/quad-complex.c src/rt/types.h src/complex_ops.h src/quad.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(RUNTIME)/seed-decimal.o: src/rt/decimal.c src/rt/types.h src/rt/decimal-internal.h src/decimal.h src/float_builtins.h src/int128_math.h src/int128_float.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(RUNTIME)/seed-float-bits.o: src/rt/float-bits.c src/rt/types.h src/float_builtins.h src/decimal.h src/int128_math.h src/int128_float.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c -o $@ $<

$(SEED): $(CC_OBJECTS) $(AS_OBJECTS) $(LD_OBJECTS) $(ELF_COMMON) $(RUNTIME)/seed.o $(RT_SEED)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(RUNTIME)/%/crt-exe.o: src/rt/crt-exe.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/%/crt-shared.o: src/rt/crt-shared-%.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/aarch64/atomics.o: src/rt/atomics.awk $(ASSEMBLER)
	@mkdir -p $(@D)
	awk -f $< > $(@:.o=.s)
	$(ASSEMBLER) --target=aarch64-linux-gnu -o $@ $(@:.o=.s)

define rt_c
$(RUNTIME)/$(1)/$(2).o: src/rt/$(2).c src/rt/types.h $(SEED) $(ASSEMBLER)
	@mkdir -p $$(@D)
	$(SEED) --target=$(1)-linux-gnu -std=c11 -pedantic-errors -S -o $$(@:.o=.s) $$<
	awk '{ print } $$$$1 == ".global" && $$$$2 != "__mcc_decimal_environment" { print ".hidden " $$$$2 }' $$(@:.o=.s) > $$(@:.o=.hidden.s)
	$(ASSEMBLER) --target=$(1)-linux-gnu -o $$@ $$(@:.o=.hidden.s)
endef
$(foreach arch,$(RT_ARCHS),$(foreach part,quad quad-complex half unwind integer atomic complex decimal decimal-runtime decimal-abi float-bits,$(eval $(call rt_c,$(arch),$(part)))))
$(foreach arch,$(RT_ARCHS),$(eval $(call rt_c,$(arch),trampoline)))
$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/integer.o): src/integer_bits.h src/integer_overflow.h src/int128.h src/int128_math.h src/int128_float.h
$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/atomic.o): src/atomic.h
$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/complex.o): src/complex_ops.h include/float.h
$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/half.o): src/float16.h src/int128_math.h include/float.h

$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/quad.o): src/quad_bits.h src/int128_float.h src/int128_math.h
$(foreach arch,$(RT_ARCHS),$(addprefix $(RUNTIME)/$(arch)/,decimal.o decimal-runtime.o)): src/rt/decimal-internal.h
$(foreach arch,$(RT_ARCHS),$(addprefix $(RUNTIME)/$(arch)/,decimal.o decimal-runtime.o decimal-abi.o)): src/decimal.h src/int128_math.h src/int128_float.h
$(foreach arch,$(RT_ARCHS),$(addprefix $(RUNTIME)/$(arch)/,decimal.o decimal-runtime.o decimal-abi.o float-bits.o)): src/float_builtins.h
$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/float-bits.o): src/decimal.h src/int128_math.h src/int128_float.h
$(foreach arch,$(RT_ARCHS),$(addprefix $(RUNTIME)/$(arch)/,quad.o half.o integer.o)): src/rt/fp-round.h
$(foreach arch,$(RT_ARCHS),$(RUNTIME)/$(arch)/quad-complex.o): src/complex_ops.h src/quad.h include/float.h

$(RUNTIME)/%/frame.o: src/rt/frame-%.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/%/fp.o: src/rt/fp-%.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/%/atomic-arch.o: src/rt/atomic-%.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/aarch64/libmccrt.a: $(RUNTIME)/aarch64/atomics.o
$(RUNTIME)/%/libmccrt.a: $(addprefix $(RUNTIME)/%/,quad.o quad-complex.o half.o fp.o integer.o unwind.o frame.o trampoline.o atomic.o atomic-arch.o complex.o decimal.o decimal-runtime.o decimal-abi.o float-bits.o)
	@mkdir -p $(@D)
	rm -f $@.tmp
	$(AR) rcs $@.tmp $^
	mv $@.tmp $@

$(RT_EMBEDDED): $(RT_FILES) src/rt/embed.awk $(ASSEMBLER)
	@set -e; for arch in $(RT_ARCHS); do \
		for entry in exe:crt-exe.o shared:crt-shared.o archive:libmccrt.a; do \
			od -An -v -tu1 $(RUNTIME)/$$arch/$${entry#*:} | \
				awk -v name=mcc_$${arch}_$${entry%%:*} -f src/rt/embed.awk; \
		done; \
	done > $(@:.o=.s)
	$(ASSEMBLER) -o $@ $(@:.o=.s)

check: all
	python3 tests/check.py $(PROGRAM)
	python3 tests/check_elf_tools.py $(PROGRAM) $(ASSEMBLER) $(LINKER)

check-suite: $(PROGRAM)
	python3 tests/check.py $(PROGRAM) --suite c-testsuite/tests/single-exec

check-strict: $(PROGRAM)
	python3 tests/check.py $(PROGRAM) --suite c-testsuite/tests/single-exec --mode c11

check-extended: $(PROGRAM)
	python3 c-testsuite-extended/check.py $(PROGRAM)

check-gnu-extensions: $(PROGRAM)
	python3 tests/check_gnu_extensions.py $(PROGRAM)

check-bootstrap:
	python3 c-testsuite-extended/bootstrap.py

check-gcc:
	python3 tests/check_gcc.py $(GCC_PROFILES)

clean:
	rm -rf $(BUILD)
