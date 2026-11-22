BUILD := build
PROGRAM := $(BUILD)/mcc
ASSEMBLER := $(BUILD)/mas
LINKER := $(BUILD)/mld
RUNTIME := $(BUILD)/rt
SEED := $(RUNTIME)/mcc-seed
RT_ARCHS := aarch64 x86_64
RT_FILES := $(foreach arch,$(RT_ARCHS),$(addprefix $(RUNTIME)/$(arch)/,crt-exe.o crt-shared.o libmccrt.a))
RT_EMBEDDED := $(RUNTIME)/embedded.o
CC_OBJECTS := $(addprefix $(BUILD)/cc/,driver.o cpp.o frontend.o ir.o data.o aarch64.o x86_64.o parser.o scanner.o)
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

.PHONY: all mas mld clean check check-suite check-strict check-extended check-bootstrap check-gcc
all: $(PROGRAM) $(ASSEMBLER) $(LINKER)

mas: $(ASSEMBLER)
mld: $(LINKER)

$(BUILD)/cc:
	mkdir -p $@

$(BUILD)/cc/parser.c $(BUILD)/cc/parser.h &: src/cc/parser.y | $(BUILD)/cc
	bison -Wall -Werror -d -o $(BUILD)/cc/parser.c $<

$(BUILD)/cc/scanner.c: src/cc/scanner.l | $(BUILD)/cc
	flex -o $@ $<

$(BUILD)/%.o: src/%.c src/compiler.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc -I$(BUILD)/cc -c -o $@ $<

$(BUILD)/cc/driver.o: override CPPFLAGS += -DMCC_BUILTIN_INCLUDE='"$(CURDIR)/include"'
$(BUILD)/cc/driver.o: src/as/as.h src/ld/ld.h src/elf_tools.h
$(addprefix $(BUILD)/cc/,frontend.o ir.o aarch64.o x86_64.o): $(BUILD)/cc/parser.h
$(BUILD)/as/main.o $(AS_OBJECTS): src/as/as.h src/elf_tools.h
$(BUILD)/ld/main.o $(BUILD)/ld/runtime.o $(LD_OBJECTS): src/ld/ld.h src/elf_tools.h
$(BUILD)/elf_tools.o: src/elf_tools.h

$(BUILD)/cc/%.o: $(BUILD)/cc/%.c src/compiler.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(GENERATED_WARN) -Isrc -I$(BUILD)/cc -c -o $@ $<

$(BUILD)/cc/parser.o: GENERATED_WARN = $(PARSER_WARN)

$(PROGRAM): $(CC_OBJECTS) $(AS_OBJECTS) $(LD_OBJECTS) $(ELF_COMMON) $(BUILD)/ld/runtime.o $(RT_EMBEDDED)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ASSEMBLER): $(BUILD)/as/main.o $(AS_OBJECTS) $(ELF_COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(LINKER): $(BUILD)/ld/main.o $(LD_OBJECTS) $(ELF_COMMON) $(BUILD)/ld/runtime.o $(RT_EMBEDDED)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(RUNTIME)/seed.o: src/ld/runtime.c src/ld/ld.h src/elf_tools.h src/compiler.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DMCC_RUNTIME_SEED -Isrc -c -o $@ $<

$(SEED): $(CC_OBJECTS) $(AS_OBJECTS) $(LD_OBJECTS) $(ELF_COMMON) $(RUNTIME)/seed.o
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
	awk '{ print } $$$$1 == ".global" { print ".hidden " $$$$2 }' $$(@:.o=.s) > $$(@:.o=.hidden.s)
	$(ASSEMBLER) --target=$(1)-linux-gnu -o $$@ $$(@:.o=.hidden.s)
endef
$(foreach arch,$(RT_ARCHS),$(foreach part,quad unwind integer,$(eval $(call rt_c,$(arch),$(part)))))

$(RUNTIME)/x86_64/quad-abi.o: src/rt/quad-x86_64.awk $(ASSEMBLER)
	@mkdir -p $(@D)
	awk -f $< > $(@:.o=.s)
	$(ASSEMBLER) --target=x86_64-linux-gnu -o $@ $(@:.o=.s)

$(RUNTIME)/%/frame.o: src/rt/frame-%.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/%/fp.o: src/rt/fp-%.s $(ASSEMBLER)
	@mkdir -p $(@D)
	$(ASSEMBLER) --target=$*-linux-gnu -o $@ $<

$(RUNTIME)/aarch64/libmccrt.a: $(RUNTIME)/aarch64/atomics.o
$(RUNTIME)/x86_64/libmccrt.a: $(RUNTIME)/x86_64/quad-abi.o
$(RUNTIME)/%/libmccrt.a: $(addprefix $(RUNTIME)/%/,quad.o fp.o integer.o unwind.o frame.o)
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

check-bootstrap:
	python3 c-testsuite-extended/bootstrap.py

check-gcc:
	python3 tests/check_gcc.py $(GCC_PROFILES)

clean:
	rm -rf $(BUILD)
