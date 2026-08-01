# jsontool: a real program written in MVS

A complete JSON library and command-line tool, written entirely in MVS. It
exists to answer a question no test suite can: what does it feel like to write
a real program in this language, and what breaks when you do?

| File | Lines | What it is |
|------|-------|------------|
| `lexer.mvs` | 291 | tokenizer: strings with escapes (including `\uXXXX` to UTF-8), integers kept apart from floats, byte offsets for error reporting |
| `value.mvs` | 202 | the value tree: a `Json` enum plus an arena `Doc` that owns every node and string, with `get` and dotted `path` lookup returning `Option<i64>` |
| `parser.mvs` | 224 | recursive descent over the token stream, errors as `Result<i64, str>`, a depth cap so hostile input cannot exhaust the stack |
| `writer.mvs` | 144 | serialization back to text, compact and pretty, re-escaping strings so output re-parses |
| `main.mvs` | 223 | the CLI: `pretty`, `compact`, `get <path>`, `stats`, and a demo suite when run with no arguments |
| **total** | **1084** | |

## Running it

```sh
mvs examples/10_json/main.mvs
./examples/10_json/main                       # the demo suite

./main pretty  '{"a":[1,{"b":true}]}'
./main compact '{ "x" : [ 1 , 2 ] }'
./main get     '{"user":{"roles":["admin","dev"]}}' user.roles.1
./main stats   '{"a":[1,2,{"b":"c"}]}'
```

It leans on most of the language at once: enums with payloads and `match`,
`Vec`/`HashMap`/`Option`/`Result`, methods and associated functions,
recursion, string handling through `core`, manual memory with a single arena
owner, and a CLI reading `argv`.

## What it found

Writing it surfaced four real gaps in the compiler, all fixed in the same
change:

1. A trailing comma in a struct literal, array literal, or call was a syntax
   error. It is now allowed, as in Rust.
2. `from` was a hard keyword, so it could not be used as a parameter or field
   name. It is now contextual: only an `import` statement gives it meaning.
3. A struct literal could not be passed as an argument (`v.push(Span { .. })`)
   and had to go through a temporary. It now materializes into a compiler
   temp, like a struct returned from a function.
4. Comparing two `str` values with `==` silently compared addresses. That is
   still what the operator does, but the compiler now warns and points at
   `cstr.eq`.

The program runs identically on all three targets and is clean under
AddressSanitizer with leak detection on: every `Doc` and `String` it creates
is released.

## Limits

The parser accepts the JSON grammar but is not a validator: it does not reject
duplicate keys, does not enforce the number grammar strictly (`1e` is taken as
`1`), and `\u` escapes above the BMP are encoded as three-byte UTF-8 rather
than surrogate pairs. Those are properties of this example, not of MVS.
