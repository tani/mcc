BUILD := build
PROGRAM := $(BUILD)/mcc
SOURCES := driver core cpp frontend ir aarch64
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SOURCES))) $(BUILD)/parser.o $(BUILD)/scanner.o
WARN := -std=c11 -Wall -Wextra -Werror

.PHONY: all clean check check-suite check-strict
all: $(PROGRAM)

$(BUILD):
	mkdir -p $@

$(BUILD)/parser.c $(BUILD)/parser.h &: src/parser.y | $(BUILD)
	bison -Wall -Werror -d -o $(BUILD)/parser.c $<

$(BUILD)/scanner.c: src/scanner.l | $(BUILD)
	flex -o $@ $<

$(BUILD)/%.o: src/%.c src/compiler.h $(BUILD)/parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc -I$(BUILD) -c -o $@ $<

$(BUILD)/%.o: $(BUILD)/%.c src/compiler.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc -I$(BUILD) -c -o $@ $<

$(BUILD)/parser.o: WARN += -Wno-unused-but-set-variable

$(PROGRAM): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

check: $(PROGRAM)
	python3 tests/check.py $(PROGRAM)

check-suite: $(PROGRAM)
	python3 tests/check.py $(PROGRAM) --suite c-testsuite/tests/single-exec

check-strict: $(PROGRAM)
	python3 tests/check.py $(PROGRAM) --suite c-testsuite/tests/single-exec --mode c11

clean:
	rm -rf $(BUILD)
