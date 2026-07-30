/*
 * generic.h - monomorphization of generic functions
 *
 * Turns `func max<T>(a: T, b: T) -> T` into type-specific copies for the types actually called
 * (e.g. max__i32, max__f64), then rewrites the call sites to point at those copies.
 * Runs as an AST pass before codegen, so codegen only ever sees concrete (single-type) functions.
 */
#ifndef MVS_GENERIC_H
#define MVS_GENERIC_H

#include "ast.h"

/* Replace generic calls with type-specific instances and append those instances to the program.
 * Returns the number of trait bound violations (`<T: Trait>` whose concrete type lacks the impl); 0 = pass */
/* Desugar Rust-style enums and match statements into tagged structs +
 * associated constructors + if-chains (runs FIRST, right after module load;
 * later passes never see ND_ENUM_DECL/ND_MATCH). Returns the error count. */
int desugar_enums(Node *program);

int monomorphize(Node *program);

/* Resolve overloaded functions (same name, different parameter types): rename each definition by its
 * signature, then resolve call sites to the definition whose argument types match */
int resolve_overloads(Node *program);   /* returns the number of unresolved calls */

/* Compile-time type checking: catches invalid type mixes (e.g. 50 + "50", u8 = str).
 * Returns the number of errors found (0 = pass). Call after monomorphize + resolve_overloads */
int typecheck(Node *program);

/* Check top-level duplicate names (structs/traits/functions with the same ns+name+signature).
 * Call before monomorphize. Returns the number of errors (0 = pass) */
int check_duplicates(Node *program);

/* Fill omitted trailing arguments with the parameters' declared default values
 * (func f(a: i32, b: i32 = 5) called as f(1) becomes f(1, 5)).
 * Call after check_duplicates and before monomorphize */
void fill_default_args(Node *program);

#endif /* MVS_GENERIC_H */
