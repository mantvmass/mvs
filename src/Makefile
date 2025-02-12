CC = gcc
NASM = nasm
CFLAGS = -Wall -Wextra -std=c11

all: mvsc build

mvsc: lexer.o parser.o codegen.o main.o
	$(CC) $(CFLAGS) -o mvsc lexer.o parser.o codegen.o main.o

lexer.o: lexer.c lexer.h
	$(CC) $(CFLAGS) -c lexer.c

parser.o: parser.c parser.h lexer.h
	$(CC) $(CFLAGS) -c parser.c

codegen.o: codegen.c codegen.h parser.h
	$(CC) $(CFLAGS) -c codegen.c

main.o: main.c lexer.h parser.h codegen.h
	$(CC) $(CFLAGS) -c main.c

build:
	./mvsc test.mvs
	$(NASM) -f win64 output.asm -o output.obj
	gcc output.obj -o output.exe -nostartfiles

clean:
	rm -f *.o mvsc output.obj output.asm output.exe