/*
 * opt.h - IR optimization passes
 *
 * All passes operate on a single IRFunc. They are pure IR-to-IR
 * transforms that never touch the AST or Gen state. Called by
 * lower_func() after lowering and before the backend emitter.
 */
#ifndef MVS_OPT_H
#define MVS_OPT_H

#include "ir.h"

void ir_optimize(IRFunc *f);

void ir_opt_const_fold(IRFunc *f);
void ir_opt_strength_reduce(IRFunc *f);
void ir_opt_cse_addr(IRFunc *f);
void ir_opt_copy_prop(IRFunc *f);
void ir_opt_store_forward(IRFunc *f);
void ir_opt_dce(IRFunc *f);

#endif /* MVS_OPT_H */
