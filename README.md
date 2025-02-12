# MVS Language

> เพื่อศึกษาการออกแบบและพัฒนา compiler

### แผนงาน  
ขั้นที่ 1: เขียน Compiler ตัวแรก (Stage 1) ใน C
- แปลง MVS → LLVM IR หรือ Assembly
- สนับสนุนเฉพาะ Subset ของ MVS เพื่อให้เขียน Compiler ตัวที่สองได้  

ขั้นที่ 2: เขียน Compiler ตัวที่สอง (Stage 2) ใน MVS
- Compiler ตัวแรก Compile Compiler ตัวที่สอง
- Compiler ตัวที่สองต้องสามารถ Compile ตัวเองได้

ขั้นที่ 3: Self-hosting และ Optimization
- Compiler ตัวที่สอง Compile ตัวเองและสร้าง Executable โดยตรง
- ใช้ LLVM หรือ Backend อื่นเพื่อ Optimize

### Project Structure สำหรับ Self-hosting Compiler ของ MVS
เพื่อให้พัฒนา Bootstrapping Compiler ของ MVS ได้อย่างเป็นระบบ นี่คือโครงสร้างโครงการที่ชัดเจน รองรับทั้ง Compiler ตัวแรก (C) และ Compiler ตัวที่สอง (MVS) ที่สามารถ Compile ตัวเองได้

```
mvsc/
│── stage1/                     # Compiler ตัวแรก (เขียนด้วย C)
│   ├── include/                 # Header files
│   │   ├── lexer.h              # Header file สำหรับ Lexer
│   │   ├── parser.h             # Header file สำหรับ Parser
│   │   ├── codegen.h            # Header file สำหรับ Code Generation
│   ├── src/                     # Source code
│   │   ├── main.c               # Entry point ของ Compiler
│   │   ├── lexer.c              # แปลง Source Code เป็น Tokens
│   │   ├── parser.c             # แปลง Tokens เป็น AST
│   │   ├── codegen.c            # สร้าง LLVM IR หรือ Assembly
│   ├── test/                    # ไฟล์สำหรับทดสอบ Stage 1
│   │   ├── example.mvs          # ตัวอย่างไฟล์ Source Code ภาษา MVS
│   ├── build.sh                 # สคริปต์คอมไพล์ Stage 1
│
│── stage2/                     # Compiler ตัวที่สอง (เขียนด้วย MVS)
│   ├── src/                     # Source Code ของ Compiler ตัวที่สอง
│   │   ├── main.mvs             # Entry point ของ Compiler ตัวที่สอง
│   │   ├── lexer.mvs            # Lexer ใน MVS
│   │   ├── parser.mvs           # Parser ใน MVS
│   │   ├── codegen.mvs          # Code Generation ใน MVS
│   ├── test/                    # ทดสอบ Compiler ตัวที่สอง
│   ├── build.mvs                # สคริปต์คอมไพล์ Stage 2
│
│── runtime/                     # Runtime Library สำหรับ MVS
│   ├── stdlib.mvs               # Built-in functions (print, math, io)
│
│── docs/                        # เอกสาร
│   ├── spec.md                  # รายละเอียดไวยากรณ์ของภาษา
│   ├── roadmap.md               # แผนพัฒนา Compiler
│
│── examples/                    # ตัวอย่างโค้ด MVS
│   ├── hello_world.mvs          # โค้ด Hello World
│   ├── math_test.mvs            # การทดสอบคำนวณ
│
│── tools/                       # Utility scripts
│   ├── test_all.sh              # ทดสอบทุกเคส
│
│── .gitignore                    # ไฟล์ที่ไม่ต้อง commit
│── README.md                     # คำอธิบายโปรเจค

```

