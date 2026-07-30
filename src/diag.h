/*
 * diag.h - Rust-style diagnostic output
 *
 * Every diagnostic shows where it happened and what the offending line looks like:
 *
 *   examples/foo.mvs:12:5: error: cannot assign to constant 'MAX'
 *      12 |     MAX = 20;
 *         |     ^
 *   help: declare 'MAX' with 'let' to make it mutable
 *
 * The source of every parsed file is registered here so any later pass
 * (typecheck, codegen) can quote it by (file, line).
 */
#ifndef MVS_DIAG_H
#define MVS_DIAG_H

/* Register a file's source text (takes ownership of src; never freed until exit).
 * Returns a stable copy of the name that AST nodes can point at safely. */
const char *diag_register_source(const char *name, char *src);

/* Mark the entry file: warnings are only emitted for code in this file
 * (imported modules stay quiet, like Rust warning only for the current crate) */
void diag_set_primary(const char *name);
int  diag_is_primary(const char *name);

/* Print one diagnostic: "file:line:col: <kind>: <message>" plus the quoted source
 * line and a caret under col (col <= 0 omits the caret). kind is "error",
 * "warning", or "syntax error". */
void diag_print(const char *file, int line, int col, const char *kind, const char *fmt, ...);

/* Print a help note attached to the previous diagnostic */
void diag_help(const char *fmt, ...);

#endif /* MVS_DIAG_H */
