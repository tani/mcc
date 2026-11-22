BUILD := build
PROGRAM := $(BUILD)/mcc
ASSEMBLER := $(BUILD)/mas
LINKER := $(BUILD)/mld
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
$(BUILD)/ld/main.o $(LD_OBJECTS): src/ld/ld.h src/elf_tools.h
$(BUILD)/elf_tools.o: src/elf_tools.h

$(BUILD)/cc/%.o: $(BUILD)/cc/%.c src/compiler.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(GENERATED_WARN) -Isrc -I$(BUILD)/cc -c -o $@ $<

$(BUILD)/cc/parser.o: GENERATED_WARN = $(PARSER_WARN)

$(PROGRAM): $(CC_OBJECTS) $(AS_OBJECTS) $(LD_OBJECTS) $(ELF_COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ASSEMBLER): $(BUILD)/as/main.o $(AS_OBJECTS) $(ELF_COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(LINKER): $(BUILD)/ld/main.o $(LD_OBJECTS) $(ELF_COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

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
