# Makefile สำหรับคอมไพเลอร์ MVS (เขียนด้วย C ล้วน ไม่พึ่ง LLVM/flex/bison)
#
# ต้องมีบน PATH: clang (build คอมไพเลอร์ + เป็น linker), nasm (ประกอบ .asm)
#   ถ้า clang ติดตั้งไว้คนละที่ ให้เพิ่มโฟลเดอร์ bin เข้า PATH ก่อน (mvs.exe จะแจ้งถ้าหาไม่เจอ)
#
# เป้าหมายที่ใช้บ่อย:
#   make            - build คอมไพเลอร์เป็น mvs.exe
#   make examples   - คอมไพล์ตัวอย่างทั้งหมดใน examples/ (ทุกกลุ่ม)
#   make framework  - build เฟรมเวิร์กตัวอย่าง vmass/
#   make clean      - ลบไฟล์ที่สร้างขึ้น

CC      = clang
# _CRT_SECURE_NO_WARNINGS: ปิดคำเตือน fopen/strcpy/strdup ของ MSVC runtime
# -Wextra: เปิดคำเตือนเพิ่ม (unused/dead code) เพื่อให้โค้ดสะอาด
CFLAGS  = -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -Isrc
TARGET  = mvs.exe

# ไฟล์ซอร์สของตัวคอมไพเลอร์
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

# ตัวอย่างที่คอมไพล์เป็น .exe ได้ (แบ่งเป็นกลุ่ม 01_language .. 08_stdlib)
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

# เป้าหมายปริยาย: build คอมไพเลอร์
$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)
	@echo "Built $(TARGET)"

# คอมไพล์ตัวอย่างทั้งหมด (ต้อง build คอมไพเลอร์ก่อน)
examples: $(TARGET)
	@for f in $(EXAMPLES); do ./$(TARGET) $$f.mvs || exit 1; done
	@# กลุ่มที่ต้องใช้แฟล็กพิเศษ (ผลิต .obj สำหรับ C interop / freestanding)
	./$(TARGET) examples/07_c_interop/export_lib.mvs -c
	./$(TARGET) examples/07_c_interop/use_c.mvs -c
	./$(TARGET) examples/07_c_interop/freestanding.mvs --nostd
	@echo "All examples compiled"

# build เฟรมเวิร์ก vmass (entry lib = vmass.mvs; โครงสร้าง modular: core/http/vmass + examples/)
framework: $(TARGET)
	./$(TARGET) vmass/examples/hello.mvs -o vmass/hello.exe
	./$(TARGET) vmass/examples/api.mvs -o vmass/api.exe
	@echo "Built vmass/hello.exe + vmass/api.exe (run one, then: curl http://127.0.0.1:8080/)"

# ลบไฟล์ที่สร้างขึ้นทั้งหมด (รวมโฟลเดอร์ย่อยของ examples และ vmass)
clean:
	rm -f $(TARGET) *.asm *.obj *.exe
	rm -f examples/*.asm examples/*.obj examples/*.exe
	rm -f examples/*/*.asm examples/*/*.obj examples/*/*.exe
	rm -f vmass/*.asm vmass/*.obj vmass/*.exe vmass/examples/*.asm vmass/examples/*.obj
	@echo "Cleaned"

.PHONY: examples framework clean
