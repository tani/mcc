BUILD := build
PROGRAM := $(BUILD)/mcc
SOURCES := driver core cpp frontend ir aarch64
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SOURCES))) $(BUILD)/parser.o $(BUILD)/scanner.o
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

.PHONY: all clean check check-suite check-strict check-extended check-bootstrap check-gcc
all: $(PROGRAM)

$(BUILD):
	mkdir -p $@

$(BUILD)/parser.c $(BUILD)/parser.h &: src/parser.y | $(BUILD)
	bison -Wall -Werror -d -o $(BUILD)/parser.c $<

$(BUILD)/scanner.c: src/scanner.l | $(BUILD)
	flex -o $@ $<

$(BUILD)/%.o: src/%.c src/compiler.h $(BUILD)/parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc -I$(BUILD) -c -o $@ $<

$(BUILD)/driver.o: override CPPFLAGS += -DMCC_BUILTIN_INCLUDE='"$(CURDIR)/include"'

$(BUILD)/%.o: $(BUILD)/%.c src/compiler.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(GENERATED_WARN) -Isrc -I$(BUILD) -c -o $@ $<

$(BUILD)/parser.o: GENERATED_WARN = $(PARSER_WARN)

$(PROGRAM): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

check: $(PROGRAM)
	python3 tests/check.py $(PROGRAM)

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
