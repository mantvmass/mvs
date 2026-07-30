# Makefile for the MVS compiler (written in pure C, no LLVM/flex/bison)
#
# Required on PATH: clang (builds the compiler + acts as linker), nasm (assembles .asm)
#   If clang is installed elsewhere, add its bin folder to PATH first (mvs.exe reports if not found)
#
# Common targets:
#   make            - build the compiler as mvs.exe
#   make examples   - compile all examples in examples/ (every group)
#   make test       - run the full test suite (golden output + compile-fail)
#   make clean      - remove generated files

CC      = clang
# _CRT_SECURE_NO_WARNINGS: silence MSVC runtime warnings for fopen/strcpy/strdup
# -Wextra: enable extra warnings (unused/dead code) to keep the code clean
CFLAGS  = -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc
TARGET  = mvs.exe

# source files of the compiler itself
SRC = src/main.c \
      src/lexer.c \
      src/ast.c \
      src/parser.c \
      src/module.c \
      src/generic.c \
      src/codegen.c \
      src/arch/common.c \
      src/arch/x86_64/win.c

HDR = src/token.h src/lexer.h src/ast.h src/parser.h src/module.h src/generic.h src/codegen.h \
      src/arch/common.h src/arch/x86_64/win.h

# examples that compile to .exe (organized into groups 01_language .. 08_stdlib)
EXAMPLES = examples/demo \
           examples/01_language/hello examples/01_language/types examples/01_language/operators \
           examples/01_language/casts examples/01_language/control examples/01_language/bitwise \
           examples/01_language/args \
           examples/02_functions/generics examples/02_functions/overload examples/02_functions/recursion \
           examples/02_functions/funcptr \
           examples/03_structs/structs examples/03_structs/methods examples/03_structs/pointers \
           examples/04_traits/traits examples/04_traits/display \
           examples/05_strings/strings \
           examples/06_modules/use_import \
           examples/07_c_interop/extern_c \
           examples/08_stdlib/io_demo examples/08_stdlib/floats examples/08_stdlib/files \
           examples/08_stdlib/net_client examples/08_stdlib/net_server

# default target: build the compiler
$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)
	@echo "Built $(TARGET)"

# compile all examples (the compiler must be built first)
examples: $(TARGET)
	@for f in $(EXAMPLES); do ./$(TARGET) $$f.mvs || exit 1; done
	@# groups that need special flags (produce .obj for C interop / freestanding)
	./$(TARGET) examples/07_c_interop/export_lib.mvs -c
	./$(TARGET) examples/07_c_interop/use_c.mvs -c
	./$(TARGET) examples/07_c_interop/freestanding.mvs --nostd
	@echo "All examples compiled"

# run the test suite: golden output tests + compile-only + compile-fail (expected errors)
test: $(TARGET)
	powershell -ExecutionPolicy Bypass -File tests/run.ps1

# remove all generated files (including subfolders of examples)
clean:
	rm -f $(TARGET) *.asm *.obj *.exe
	rm -f examples/*.asm examples/*.obj examples/*.exe
	rm -f examples/*/*.asm examples/*/*.obj examples/*/*.exe
	@echo "Cleaned"

.PHONY: examples test clean
