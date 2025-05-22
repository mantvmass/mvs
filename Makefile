# Makefile for MVS Compiler
# Assumes LLVM 18 is installed, adjust LLVM_CONFIG if using a different version
CC = gcc
FLEX = flex
BISON = bison
LLVM_CONFIG = llvm-config-18
CFLAGS = -Wall `$(LLVM_CONFIG) --cflags`
LDFLAGS = `$(LLVM_CONFIG) --ldflags` `$(LLVM_CONFIG) --libs all` `$(LLVM_CONFIG) --system-libs`

# Source files
SOURCES = mvs.tab.c lex.yy.c codegen.c
OBJECTS = $(SOURCES:.c=.o)
EXEC = mvs
TEST_DIR = tests
TEST_OUTPUT = mvs_program
OBJ_FILE = output.o
TEST_SCRIPT = run_tests.sh

# Default target
all: build

# Build the compiler
build: $(EXEC)

# Build the program from a single test file
program: $(EXEC)
	@echo "Running compiler on $(TEST_INPUT)..."
	./$(EXEC) < $(TEST_INPUT)
	@if [ -f $(TEST_OUTPUT) ]; then \
		echo "$(TEST_OUTPUT) created successfully"; \
	else \
		echo "Error: $(TEST_OUTPUT) was not created"; \
		exit 1; \
	fi

$(EXEC): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Compiler $(EXEC) built successfully"

mvs.tab.c mvs.tab.h: mvs.y
	$(BISON) -d $<
	@echo "Generated mvs.tab.c and mvs.tab.h"

lex.yy.c: mvs.l mvs.tab.h
	$(FLEX) $<
	@echo "Generated lex.yy.c"

%.o: %.c ast.h mvs.tab.h
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled $< to $@"

# Test all test cases using the test script
test: build $(TEST_SCRIPT)
	@echo "Running test script..."
	./$(TEST_SCRIPT)

# Clean generated files
clean:
	rm -f lex.yy.c mvs.tab.c mvs.tab.h *.o $(EXEC) $(TEST_OUTPUT) $(OBJ_FILE)
	@echo "Cleaned all generated files"

.PHONY: all build program test clean
