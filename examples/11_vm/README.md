# minivm: a small language, compiled to bytecode and run, written in MVS

The second dogfood program in this repository, after
[10_json](../10_json/README.md). About 1400 lines of MVS across five modules,
implementing the whole pipeline for a toy language called *mini*: tokenizer,
recursive-descent parser, an arena syntax tree, a compiler that resolves names
to numbered slots and emits bytecode, and a stack machine that runs it.

It exists to find compiler bugs that a test suite does not, by being a program
someone would actually write. It runs byte-identically on all three targets
(x86-64 Windows, x86-64 Linux, AArch64 Linux) and is clean under
AddressSanitizer with leak detection ON: every allocation it makes it frees.

## Run it

```sh
mvs examples/11_vm/main.mvs
./examples/11_vm/main                 # the demo suite

./examples/11_vm/main run 'let i = 1; let s = 0; while (i <= 10) { s = s + i; i = i + 1; } print s;'
./examples/11_vm/main dis 'let x = 1; if (x == 1) { print x; } else { print 0; }'
./examples/11_vm/main stats 'func fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print fib(10);'
```

## The mini language

```txt
let x = 2 + 3 * 4;              // integers only, C precedence
x = x - 1;                      // assignment to a declared name
print x;

if (x < 10) { print 1; }        // else if chains
else if (x < 20) { print 2; }
else { print 3; }

while (x > 0) { x = x - 1; }    // the only loop

func add(a, b) { return a + b; }   // functions, recursion, forward calls
print add(2, 3);
// comments run to the end of the line
```

Operators: `+ - * / %`, unary `-`, and `< <= > >= == !=` which yield 1 or 0.
Everything is `i64`. A function without an explicit `return` gives 0.

## The modules

| File | What it does |
|------|--------------|
| [lexer.mvs](lexer.mvs) | text to a `Vec<Token>`; keywords, numbers, two-character operators, comments |
| [ast.mvs](ast.mvs) | the arena: nodes in one vector, child lists in another, referenced by index |
| [parser.mvs](parser.mvs) | recursive descent with precedence climbing; the first error stops the parse |
| [compiler.mvs](compiler.mvs) | two passes: collect functions, then emit bytecode with patched jumps |
| [vm.mvs](vm.mvs) | the stack machine, plus a disassembler |
| [main.mvs](main.mvs) | the CLI and the demo suite that the golden test runs |

## What it exercises in the compiler

Enums with struct payloads inside a `Vec`, `HashMap<str, i64>` for scope and
function tables, `Option` returned from a map lookup and matched, methods on
structs held behind pointers, recursion through several modules, `match` as a
statement and as the body of a function that returns from every arm, and a
program whose own error paths must be reported rather than crashed on.

Every runtime failure the VM can hit (stack underflow, bad slot, division by
zero, a jump out of range, exceeding the step limit) stops the machine with a
message. A VM that trusts its bytecode is not a VM.
