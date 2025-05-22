# MVS Compiler

MVS is a simple programming language designed for basic arithmetic operations, supporting operators like `+`, `-`, `*`, `/`, `%`, `^`, and parentheses. The compiler generates LLVM IR and produces executable programs that return an 8-bit integer (`i8`) as an exit code.

This document provides instructions for setting up the development environment for the MVS compiler on Linux and Windows.

## Prerequisites

### Linux (Ubuntu/Debian-based)

#### GCC: C Compiler for Building the Compiler
```bash
sudo apt update
sudo apt install gcc
```

#### Flex: Lexical Analyzer Generator for `mvs.l`
```bash
sudo apt install flex
```

#### Bison: Parser Generator for `mvs.y`
```bash
sudo apt install bison
```

#### LLVM 18: For Code Generation and IR Manipulation
```bash
sudo apt install llvm-18 llvm-18-dev
```

Verify installation:
```bash
llvm-config-18 --version
```
Should return `18.x.x`.

#### Make: For Building the Project Using Makefile
```bash
sudo apt install make
```

### Windows

Windows requires a Unix-like environment (e.g., MSYS2 or WSL). MSYS2 is recommended for native Windows development.

#### Using MSYS2

1. **Install MSYS2**:
   Download and install MSYS2 from [https://www.msys2.org/](https://www.msys2.org/).

2. **Update MSYS2**:
   ```bash
   pacman -Syu
   ```

3. **Install GCC**:
   ```bash
   pacman -S mingw-w64-x86_64-gcc
   ```

4. **Install Flex**:
   ```bash
   pacman -S flex
   ```

5. **Install Bison**:
   ```bash
   pacman -S bison
   ```

6. **Install LLVM 18**:

   LLVM 18 may not be available directly in MSYS2. You can build it from source or use a prebuilt binary:

   - Download LLVM 18 from [https://releases.llvm.org/](https://releases.llvm.org/) or use a package manager.
   - Alternatively, install via Chocolatey (if available):
     ```bash
     choco install llvm --version 18.1.8
     ```
   
   Ensure `llvm-config` is in your `PATH`.

   If building from source:
   ```bash
   pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
   git clone https://github.com/llvm/llvm-project.git
   cd llvm-project
   git checkout llvmorg-18.1.8
   mkdir build
   cd build
   cmake -G Ninja -DLLVM_ENABLE_PROJECTS="clang;lld" ../llvm
   ninja
   ```
   Add `build/bin` to your `PATH`.

7. **Install Make**:
   ```bash
   pacman -S make
   ```

#### Using WSL (Windows Subsystem for Linux)

1. **Install WSL2 and an Ubuntu Distribution**:
   ```bash
   wsl --install
   ```

2. **Follow the Linux (Ubuntu/Debian-based) Instructions Above** within WSL.

## Building the Compiler

1. **Clone the Repository (if using Git)**:
   ```bash
   git clone <repository-url>
   cd mvs
   ```

2. **Build the Compiler**:
   ```bash
   make clean
   make build
   ```

3. **Run Tests**:
   ```bash
   make test
   ```

## Project Structure

- `mvs.l`: Lexical analyzer specification.
- `mvs.y`: Parser specification.
- `codegen.c`: Code generation using LLVM.
- `ast.h`: Abstract Syntax Tree definitions.
- `tests/`: Test cases for arithmetic operations.
- `run_tests.sh`: Shell script for running tests.
- `Makefile`: Build script.

## Notes

- Ensure `llvm-config-18` is in your `PATH`. Adjust `LLVM_CONFIG` in `Makefile` if using a different LLVM version.
- On Windows, MSYS2 is preferred for compatibility with `flex`, `bison`, and `make`.
- For issues with LLVM installation, check the official LLVM documentation: [https://llvm.org/docs/](https://llvm.org/docs/).
