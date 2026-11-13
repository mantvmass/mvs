/*
 * lower.h - AST to IR lowering pass
 *
 * lower_func() converts one AST function into an IRFunc. The caller
 * must have populated Gen (struct layouts, function table, globals,
 * string pool) before calling. The returned IRFunc is owned by the
 * caller and must be freed with ir_func_free().
 */
#ifndef MVS_LOWER_H
#define MVS_LOWER_H

#include "ir.h"
#include "arch/common.h"

/* Lower one function from AST to IR.
 * gen must be fully populated (structs laid out, functions registered,
 * globals collected). The per-function state (locals, visibility, frame)
 * is set up inside this call, matching what each backend's emit_ir_func expects.
 * program is needed for main's global initializers. */
IRFunc *lower_func(Gen *g, Node *fn, Node *program, int nregpool);

#endif /* MVS_LOWER_H */
