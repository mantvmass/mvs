# กำหนดตัวแปรสำคัญ
CC = gcc               # คอมไพเลอร์ที่ใช้ (GCC)

CFLAGS = -Wall -Wextra -std=c11 -I src/  # ค่าคอมไพเลอร์แฟล็กเพื่อเปิดใช้งานคำเตือนและกำหนดมาตรฐาน C11

# กำหนดไดเรกทอรีสำหรับซอร์สโค้ด, ไฟล์ที่สร้างขึ้น, และโฟลเดอร์ที่ใช้เก็บไฟล์ object (.o)
SRC_DIR = src
BUILD_DIR = tests
OBJ_DIR = obj

# ตรวจสอบระบบปฏิบัติการที่ใช้งาน (Linux หรือ Windows)
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)  
    OS = linux                     # ถ้าเป็น Linux
    OUTPUT_EXT = out                # ไฟล์เอาต์พุตใช้ .out
else
    OS = windows                    # ถ้าเป็น Windows
    OUTPUT_EXT = exe                # ไฟล์เอาต์พุตใช้ .exe
endif

# กำหนดรายการไฟล์ object (.o) ที่ต้องใช้ในโปรแกรม
OBJS = $(OBJ_DIR)/lexer.o $(OBJ_DIR)/parser.o $(OBJ_DIR)/codegen.o $(OBJ_DIR)/main.o

# เป้าหมายหลัก: คอมไพล์ mvsc และทำการ build
all: $(BUILD_DIR)/mvsc.$(OUTPUT_EXT) build

# คอมไพล์ไฟล์ mvsc โดยรวมไฟล์ object ทุกไฟล์
$(BUILD_DIR)/mvsc.$(OUTPUT_EXT): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)  # คอมไพล์และลิงก์ไฟล์ทั้งหมด

# คอมไพล์ lexer.c เป็น lexer.o
$(OBJ_DIR)/lexer.o: $(SRC_DIR)/lexer.c $(SRC_DIR)/lexer.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# คอมไพล์ parser.c เป็น parser.o
$(OBJ_DIR)/parser.o: $(SRC_DIR)/parser.c $(SRC_DIR)/parser.h $(SRC_DIR)/lexer.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# คอมไพล์ codegen.c เป็น codegen.o
$(OBJ_DIR)/codegen.o: $(SRC_DIR)/codegen.c $(SRC_DIR)/codegen.h $(SRC_DIR)/parser.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# คอมไพล์ main.c เป็น main.o
$(OBJ_DIR)/main.o: $(SRC_DIR)/main.c $(SRC_DIR)/lexer.h $(SRC_DIR)/parser.h $(SRC_DIR)/codegen.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ขั้นตอนการ build: รัน mvsc เพื่อสร้าง output.c และทำการคอมไพล์แอสเซมบลี
build: $(BUILD_DIR)/mvsc.$(OUTPUT_EXT)
	./$(BUILD_DIR)/mvsc.$(OUTPUT_EXT) $(BUILD_DIR)/sample.mvs  # รัน mvsc เพื่อสร้าง output.c

# สร้างโฟลเดอร์ obj/ หากยังไม่มี
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# คำสั่ง clean เพื่อลบไฟล์ที่สร้างขึ้นทั้งหมด
clean:
	rm -rf $(OBJ_DIR) $(BUILD_DIR)/mvsc.$(OUTPUT_EXT) output.c output.$(OUTPUT_EXT)
