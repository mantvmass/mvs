/*
 * lower.c - AST to IR lowering pass
 *
 * Walks the AST once and produces three-address IR instructions (IRFunc).
 * This replaces the gen_expr/gen_stmt/gen_addr/gen_call functions that were
 * duplicated in every backend. Each backend now only translates IR to its
 * assembly syntax.
 *
 * Complex intrinsics (io.out, asm(), variadic MVS calls) are emitted as
 * high-level IR_IOOUT instructions that carry the AST node, so the backend
 * can use its existing handling code. They will be fully lowered later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lower.h"
#include "opt.h"
#include "diag.h"

/* ------------------------------------------------------------------ */
/*  forward declarations                                               */
/* ------------------------------------------------------------------ */

static int  lower_expr(Gen *g, IRFunc *f, Node *n);
static void lower_stmt(Gen *g, IRFunc *f, Node *n);
static int  lower_addr(Gen *g, IRFunc *f, Node *n);
static void lower_store_struct(Gen *g, IRFunc *f, int dst_addr, Node *value);
static int  lower_call_inner(Gen *g, IRFunc *f, Node *n, int sret_addr);

/* ------------------------------------------------------------------ */
/*  helpers                                                            */
/* ------------------------------------------------------------------ */

static IRInst *emit_ioout(Gen *g, IRFunc *f, int dst, Node *node) {
    IRInst *io = ir_emit(f, IR_IOOUT, dst, IR_NONE, IR_NONE);
    io->node = node;
    if (g->nvisible > 0) {
        io->vis = (int *)malloc(g->nvisible * sizeof(int));
        memcpy(io->vis, g->visible, g->nvisible * sizeof(int));
        io->nvis = g->nvisible;
    }
    return io;
}

static int is_unsigned_val(DataType base, int ptr) {
    if (ptr > 0) return 1;
    switch (base) {
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
        case TYPE_U128: case TYPE_USIZE: case TYPE_BOOL: case TYPE_CHAR:
            return 1;
        default:
            return 0;
    }
}

/* Emit an IR_IMM instruction */
static int emit_imm(IRFunc *f, long long val) {
    int dst = ir_vreg(f);
    IRInst *i = ir_emit(f, IR_IMM, dst, IR_NONE, IR_NONE);
    i->imm = val;
    return dst;
}

/* Emit IR_LOCAL_ADDR for a frame offset */
static int emit_local_addr(IRFunc *f, int offset) {
    int dst = ir_vreg(f);
    IRInst *i = ir_emit(f, IR_LOCAL_ADDR, dst, IR_NONE, IR_NONE);
    i->imm = offset;
    return dst;
}

/* Emit IR_GLOBAL_ADDR for a named global */
static int emit_global_addr(IRFunc *f, const char *name) {
    int dst = ir_vreg(f);
    IRInst *i = ir_emit(f, IR_GLOBAL_ADDR, dst, IR_NONE, IR_NONE);
    i->name = (char *)name;
    return dst;
}

/* Emit a typed load: dst = load(addr) */
static int emit_load(Gen *g, IRFunc *f, int addr, DataType base, int ptr, const char *sname) {
    int sz = type_size(g, base, ptr, sname);
    int dst = ir_vreg(f);
    IRInst *ld = ir_emit(f, IR_LOAD, dst, addr, IR_NONE);
    ld->type = ptr > 0 ? TYPE_USIZE : base;
    ld->ptr = ptr;
    ld->size = ptr > 0 ? 8 : sz;
    ld->is_signed = is_signed_type(base) && ptr == 0;
    ld->sname = (char *)sname;
    if (base == TYPE_F32 && ptr == 0) {
        int wide = ir_vreg(f);
        ir_emit(f, IR_F32_F64, wide, dst, IR_NONE);
        return wide;
    }
    return dst;
}

/* Emit a typed store: mem[addr] = val */
static void emit_store(Gen *g, IRFunc *f, int addr, int val,
                       DataType base, int ptr, const char *sname) {
    int sz = type_size(g, base, ptr, sname);
    IRInst *st = ir_emit(f, IR_STORE, IR_NONE, addr, val);
    st->type = ptr > 0 ? TYPE_USIZE : base;
    st->ptr = ptr;
    st->size = ptr > 0 ? 8 : sz;
    st->sname = (char *)sname;
}

/* Load a variable's value (scalar -> value, struct/i128/dyn -> address) */
static int load_var(Gen *g, IRFunc *f, Sym *s) {
    int addr;
    if (s->is_global)
        addr = emit_global_addr(f, s->name);
    else
        addr = emit_local_addr(f, s->offset);

    if (s->arr > 0)
        return addr;
    if ((s->type == TYPE_STRUCT && s->ptr == 0) || is_blob16(s->type, s->ptr))
        return addr;

    return emit_load(g, f, addr, s->type, s->ptr, s->sname);
}

/* Store a scalar value into a variable */
static void store_var(Gen *g, IRFunc *f, Sym *s, int val) {
    int addr;
    if (s->is_global)
        addr = emit_global_addr(f, s->name);
    else
        addr = emit_local_addr(f, s->offset);

    emit_store(g, f, addr, val, s->type, s->ptr, s->sname);
}

/* Emit a coercion between int and float if needed */
static int coerce_num(Gen *g, IRFunc *f, int val, ExprType from, ExprType to) {
    int from_f = is_float_type(from.base) && from.ptr == 0;
    int to_f = is_float_type(to.base) && to.ptr == 0;
    if (from_f == to_f) return val;
    if (!from_f && to_f) {
        int r = ir_vreg(f);
        if (is_unsigned_val(from.base, 0) && type_size(g, from.base, 0, NULL) >= 8)
            ir_emit(f, IR_UTOF, r, val, IR_NONE);
        else
            ir_emit(f, IR_ITOF, r, val, IR_NONE);
        return r;
    }
    /* float to int */
    int r = ir_vreg(f);
    ir_emit(f, IR_FTOI, r, val, IR_NONE);
    return r;
}

/* Map a token operator to an IR comparison sub-op */
static IRCmpOp cmp_op_for(TokenType op, int uns) {
    switch (op) {
        case TK_EQ:  return CMP_EQ;
        case TK_NEQ: return CMP_NE;
        case TK_LT:  return uns ? CMP_ULT : CMP_SLT;
        case TK_GT:  return uns ? CMP_UGT : CMP_SGT;
        case TK_LE:  return uns ? CMP_ULE : CMP_SLE;
        case TK_GE:  return uns ? CMP_UGE : CMP_SGE;
        default:     return CMP_EQ;
    }
}

static IRCmpOp fcmp_op_for(TokenType op) {
    switch (op) {
        case TK_EQ:  return CMP_FEQ;
        case TK_NEQ: return CMP_FNE;
        case TK_LT:  return CMP_FLT;
        case TK_GT:  return CMP_FGT;
        case TK_LE:  return CMP_FLE;
        case TK_GE:  return CMP_FGE;
        default:     return CMP_FEQ;
    }
}

/* Map a token operator to an IR integer binop */
static IROp binop_for(TokenType op, int uns) {
    switch (op) {
        case TK_PLUS:    return IR_ADD;
        case TK_MINUS:   return IR_SUB;
        case TK_STAR:    return IR_MUL;
        case TK_SLASH:   return uns ? IR_UDIV : IR_SDIV;
        case TK_PERCENT: return uns ? IR_UMOD : IR_SMOD;
        case TK_SHL:     return IR_SHL;
        case TK_SHR:     return uns ? IR_SHR : IR_SAR;
        case TK_AMP:     return IR_BAND;
        case TK_PIPE:    return IR_BOR;
        case TK_CARET:   return IR_BXOR;
        case TK_STARSTAR:return IR_MUL; /* integer power is lowered differently */
        default:         return IR_NOP;
    }
}

static IROp fbinop_for(TokenType op) {
    switch (op) {
        case TK_PLUS:  return IR_FADD;
        case TK_MINUS: return IR_FSUB;
        case TK_STAR:  return IR_FMUL;
        case TK_SLASH: return IR_FDIV;
        default:       return IR_NOP;
    }
}

static int is_cmp_op(TokenType op) {
    return op==TK_EQ||op==TK_NEQ||op==TK_LT||op==TK_GT||op==TK_LE||op==TK_GE;
}

static int is_arith_op(TokenType op) {
    return op==TK_PLUS||op==TK_MINUS||op==TK_STAR||op==TK_SLASH||
           op==TK_PERCENT||op==TK_SHL||op==TK_SHR||
           op==TK_AMP||op==TK_PIPE||op==TK_CARET;
}

static IR128Op i128_op_for(TokenType op) {
    switch (op) {
        case TK_PLUS:    return I128_ADD;
        case TK_MINUS:   return I128_SUB;
        case TK_STAR:    return I128_MUL;
        case TK_SLASH:   return I128_SDIV;
        case TK_PERCENT: return I128_SMOD;
        case TK_SHL:     return I128_SHL;
        case TK_SHR:     return I128_SHR;
        case TK_AMP:     return I128_AND;
        case TK_PIPE:    return I128_OR;
        case TK_CARET:   return I128_XOR;
        default:         return I128_ADD;
    }
}

static int lower_i128_operand(Gen *g, IRFunc *f, Node *expr) {
    int val = lower_expr(g, f, expr);
    ExprType t = type_of(g, expr);
    if (is_i128(t.base, t.ptr)) return val;
    int tidx = f->n_i128_temps++;
    int dst = ir_vreg(f);
    IRInst *fi = ir_emit(f, IR_I128_FROM, dst, val, IR_NONE);
    fi->imm = tidx;
    fi->is_signed = is_signed_type(t.base) && t.ptr == 0;
    return dst;
}

static void emit_i128_store(Gen *g, IRFunc *f, int dst_addr, Node *src_expr) {
    int val = lower_expr(g, f, src_expr);
    ExprType et = type_of(g, src_expr);
    IRInst *st = ir_emit(f, IR_I128_STORE, IR_NONE, dst_addr, val);
    st->is_signed = is_signed_type(et.base) && et.ptr == 0;
    st->type = et.base;
    st->ptr = et.ptr;
}

/* ------------------------------------------------------------------ */
/*  lower_addr: compute the address of an lvalue                       */
/* ------------------------------------------------------------------ */

static int lower_addr(Gen *g, IRFunc *f, Node *n) {
    switch (n->kind) {
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (!s) {
                fprintf(stderr, "lower error: undefined variable '%s'\n", n->name);
                g->had_error = 1;
                return IR_NONE;
            }
            if (s->is_global)
                return emit_global_addr(f, s->name);
            return emit_local_addr(f, s->offset);
        }
        case ND_FRAMEREF:
            return emit_local_addr(f, (int)n->int_val);

        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            StructInfo *si = find_struct(g, bt.sname);
            if (!si) {
                fprintf(stderr, "lower error: member access on non-struct\n");
                g->had_error = 1;
                return IR_NONE;
            }
            Field *fi = find_field(si, n->name);
            if (!fi) {
                fprintf(stderr, "lower error: no field '%s' in struct '%s'\n", n->name, si->name);
                g->had_error = 1;
                return IR_NONE;
            }
            int base;
            if (bt.ptr > 0)
                base = lower_expr(g, f, n->operand);
            else
                base = lower_addr(g, f, n->operand);
            if (fi->offset == 0) return base;
            int dst = ir_vreg(f);
            IRInst *i = ir_emit(f, IR_OFFSET, dst, base, IR_NONE);
            i->imm = fi->offset;
            return dst;
        }
        case ND_INDEX: {
            ExprType bt = type_of(g, n->lhs);
            int esz;
            int base;
            if (bt.arr > 0) {
                esz = type_size(g, bt.base, bt.ptr, bt.sname);
                if (bt.arr2 > 0) esz *= bt.arr2;
                base = lower_addr(g, f, n->lhs);
            } else if (bt.ptr > 0) {
                esz = type_size(g, bt.base, bt.ptr - 1, bt.sname);
                base = lower_expr(g, f, n->lhs);
            } else {
                fprintf(stderr, "lower error: cannot index a non-array, non-pointer value\n");
                g->had_error = 1;
                return IR_NONE;
            }
            int idx = lower_expr(g, f, n->rhs);
            /* bounds check for [T; N] arrays with non-constant index */
            if (mvs_bounds_checks && bt.arr > 0 && n->rhs->kind != ND_INT) {
                int lok = ir_label(f);
                int vlen = emit_imm(f, bt.arr);
                int check = ir_vreg(f);
                IRInst *ci = ir_emit(f, IR_CMP, check, idx, vlen);
                ci->cmp = CMP_ULT;
                IRInst *br = ir_emit(f, IR_BR, IR_NONE, check, IR_NONE);
                br->label = lok;
                /* the false branch: emit a trap (ud2 on x86, brk on arm64).
                 * The emitter handles this; we emit a JMP to a non-existent label
                 * that the emitter interprets as a trap. Or better: use IR_NOP with
                 * a special marker. For now, emit the out-of-bounds label inline
                 * and let the emitter handle the trap from the branch-false path. */
                int ltrap = ir_label(f);
                br->label2 = ltrap;
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = ltrap;
                /* IR_NOP with imm=1 signals "trap here" to the emitter */
                ir_emit(f, IR_NOP, IR_NONE, IR_NONE, IR_NONE)->imm = 1;
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lok;
            }
            int dst = ir_vreg(f);
            IRInst *ix = ir_emit(f, IR_INDEX, dst, base, idx);
            ix->imm = esz;
            return dst;
        }
        case ND_SLICE: {
            ExprType bt = type_of(g, n->lhs);
            int esz, base;
            if (bt.arr > 0) {
                esz = type_size(g, bt.base, bt.ptr, bt.sname);
                if (bt.arr2 > 0) esz *= bt.arr2;
                base = lower_addr(g, f, n->lhs);
            } else {
                esz = type_size(g, bt.base, bt.ptr - 1, bt.sname);
                base = lower_expr(g, f, n->lhs);
            }
            int idx = lower_expr(g, f, n->rhs);
            int sd = ir_vreg(f);
            IRInst *si = ir_emit(f, IR_INDEX, sd, base, idx);
            si->imm = esz;
            return sd;
        }
        case ND_STRUCT_LIT:
            if (n->int_val) {
                int addr = emit_local_addr(f, (int)n->int_val);
                lower_store_struct(g, f, addr, n);
                return emit_local_addr(f, (int)n->int_val);
            }
            fprintf(stderr, "lower error: struct literal has no destination\n");
            g->had_error = 1;
            return IR_NONE;

        case ND_UNARY:
            if (n->op == TK_STAR)
                return lower_expr(g, f, n->operand);
            fprintf(stderr, "lower error: expression is not an lvalue\n");
            g->had_error = 1;
            return IR_NONE;

        case ND_CALL: {
            ExprType rt = type_of(g, n);
            if ((rt.base == TYPE_STRUCT || is_blob16(rt.base, rt.ptr)) &&
                rt.ptr == 0 && n->int_val) {
                int sret = emit_local_addr(f, (int)n->int_val);
                lower_call_inner(g, f, n, sret);
                return emit_local_addr(f, (int)n->int_val);
            }
            fprintf(stderr, "lower error: cannot take address of this call result\n");
            g->had_error = 1;
            return IR_NONE;
        }
        default:
            fprintf(stderr, "lower error: expression is not an lvalue (kind %d)\n", n->kind);
            g->had_error = 1;
            return IR_NONE;
    }
}

/* ------------------------------------------------------------------ */
/*  lower_store_struct: write a struct value to a destination address   */
/* ------------------------------------------------------------------ */

static void lower_store_struct(Gen *g, IRFunc *f, int dst_addr, Node *value) {
    if (!value) return;

    if (value->kind == ND_STRUCT_LIT) {
        /* struct literal: store each field */
        StructInfo *si = find_struct(g, value->name);
        if (!si) { g->had_error = 1; return; }
        for (int i = 0; i < value->nitems; i++) {
            Node *init = value->items[i];
            if (!init || init->kind != ND_ASSIGN) continue;
            Field *fi = find_field(si, init->lhs ? init->lhs->name : init->name);
            if (!fi) continue;
            int field_addr = dst_addr;
            if (fi->offset) {
                field_addr = ir_vreg(f);
                IRInst *off = ir_emit(f, IR_OFFSET, field_addr, dst_addr, IR_NONE);
                off->imm = fi->offset;
            }
            Node *rhs = init->rhs ? init->rhs : init->operand;
            if (!rhs) continue;
            if (fi->arr > 0) {
                if (rhs->kind != ND_ARRAY_LIT) {
                    fprintf(stderr, "lower error: array field '%s' needs an array literal\n", fi->name);
                    g->had_error = 1; continue;
                }
                int esz = fi->size / fi->arr;
                for (int j = 0; j < rhs->nitems && j < fi->arr; j++) {
                    Node *el = rhs->items[j];
                    int elem_addr;
                    if (fi->offset + j * esz == 0)
                        elem_addr = dst_addr;
                    else {
                        elem_addr = ir_vreg(f);
                        IRInst *eoff = ir_emit(f, IR_OFFSET, elem_addr, dst_addr, IR_NONE);
                        eoff->imm = fi->offset + j * esz;
                    }
                    if (fi->type == TYPE_STRUCT && fi->ptr == 0) {
                        lower_store_struct(g, f, elem_addr, el);
                    } else if (is_i128(fi->type, fi->ptr)) {
                        emit_i128_store(g, f, elem_addr, el);
                    } else if (is_dyn(fi->type, fi->ptr)) {
                        IRInst *io = emit_ioout(g, f, IR_NONE, value);
                        io->a = dst_addr;
                        return;
                    } else {
                        int val = lower_expr(g, f, el);
                        ExprType vt2 = type_of(g, el);
                        ExprType dt2 = { fi->type, fi->ptr, fi->sname, fi->sig, 0, 0 };
                        val = coerce_num(g, f, val, vt2, dt2);
                        emit_store(g, f, elem_addr, val, fi->type, fi->ptr, fi->sname);
                    }
                }
            } else if (fi->type == TYPE_STRUCT && fi->ptr == 0) {
                lower_store_struct(g, f, field_addr, rhs);
            } else if (is_i128(fi->type, fi->ptr)) {
                emit_i128_store(g, f, field_addr, rhs);
            } else if (is_dyn(fi->type, fi->ptr)) {
                IRInst *io = emit_ioout(g, f, IR_NONE, value);
                io->a = dst_addr;
                return;
            } else {
                int val = lower_expr(g, f, rhs);
                ExprType vt = type_of(g, rhs);
                ExprType dt = { fi->type, fi->ptr, fi->sname, fi->sig, 0, 0 };
                val = coerce_num(g, f, val, vt, dt);
                emit_store(g, f, field_addr, val, fi->type, fi->ptr, fi->sname);
            }
        }
    } else if (value->kind == ND_CALL) {
        /* struct-returning call: pass destination as the sret pointer */
        lower_call_inner(g, f, value, dst_addr);
    } else {
        /* copy from another struct (variable, member access, etc.) */
        int src = lower_expr(g, f, value);
        ExprType vt = type_of(g, value);
        int sz = type_size(g, vt.base, vt.ptr, vt.sname);
        IRInst *cp = ir_emit(f, IR_COPY, IR_NONE, dst_addr, src);
        cp->imm = sz;
    }
}

/* ------------------------------------------------------------------ */
/*  lower_power: a ** b using a loop (replaces the backend's inline    */
/*  assembly power sequence)                                           */
/* ------------------------------------------------------------------ */

static int lower_power(Gen *g, IRFunc *f, Node *n, int fop) {
    int base_v = lower_expr(g, f, n->lhs);
    int exp_v  = lower_expr(g, f, n->rhs);
    int uns    = !fop && is_unsigned_val(type_of(g, n->lhs).base, 0);

    int result   = ir_vreg(f);
    int abs_exp  = ir_vreg(f);
    int orig_exp = ir_vreg(f);
    int zero     = emit_imm(f, 0);
    int ione     = emit_imm(f, 1);
    int one_val;
    if (fop) {
        one_val = ir_vreg(f);
        ir_emit(f, IR_IMM, one_val, IR_NONE, IR_NONE)->imm = 0x3FF0000000000000LL;
    } else {
        one_val = ione;
    }

    int labs  = ir_label(f);
    int lneg  = ir_label(f);
    int ltop  = ir_label(f);
    int lbody = ir_label(f);
    int lend  = ir_label(f);
    int ldiv  = ir_label(f);
    int ldone = ir_label(f);

    ir_emit(f, IR_MOV, orig_exp, exp_v, IR_NONE);
    ir_emit(f, IR_MOV, abs_exp, exp_v, IR_NONE);

    int cmp1 = ir_vreg(f);
    IRInst *c1 = ir_emit(f, IR_CMP, cmp1, exp_v, zero);
    c1->cmp = CMP_SGE;
    IRInst *br1 = ir_emit(f, IR_BR, IR_NONE, cmp1, IR_NONE);
    br1->label = labs; br1->label2 = lneg;

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lneg;
    ir_emit(f, IR_NEG, abs_exp, abs_exp, IR_NONE);
    ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = labs;

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = labs;
    ir_emit(f, IR_MOV, result, one_val, IR_NONE);

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = ltop;
    int cmp2 = ir_vreg(f);
    IRInst *c2 = ir_emit(f, IR_CMP, cmp2, abs_exp, zero);
    c2->cmp = CMP_SLE;
    IRInst *br2 = ir_emit(f, IR_BR, IR_NONE, cmp2, IR_NONE);
    br2->label = lend; br2->label2 = lbody;

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbody;
    if (fop)
        ir_emit(f, IR_FMUL, result, result, base_v);
    else
        ir_emit(f, IR_MUL, result, result, base_v);
    ir_emit(f, IR_SUB, abs_exp, abs_exp, ione);
    ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = ltop;

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lend;
    int cmp3 = ir_vreg(f);
    IRInst *c3 = ir_emit(f, IR_CMP, cmp3, orig_exp, zero);
    c3->cmp = CMP_SGE;
    IRInst *br3 = ir_emit(f, IR_BR, IR_NONE, cmp3, IR_NONE);
    br3->label = ldone; br3->label2 = ldiv;

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = ldiv;
    if (fop)
        ir_emit(f, IR_FDIV, result, one_val, result);
    else if (uns)
        ir_emit(f, IR_UDIV, result, one_val, result);
    else
        ir_emit(f, IR_SDIV, result, one_val, result);

    ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = ldone;
    return result;
}

/* ------------------------------------------------------------------ */
/*  lower_call: lower a function call to IR                            */
/* ------------------------------------------------------------------ */

static int lower_call_inner(Gen *g, IRFunc *f, Node *n, int sret_addr) {
    Node *callee = n->operand;

    /* asm() intrinsic */
    if (callee->kind == ND_IDENT && strcmp(callee->name, "asm") == 0) {
        emit_ioout(g, f, IR_NONE, n);
        return IR_NONE;
    }

    /* __atomic intrinsic: pass through to the backend like asm() */
    if (callee->kind == ND_IDENT && strcmp(callee->name, "__atomic") == 0) {
        emit_ioout(g, f, IR_NONE, n);
        return IR_NONE;
    }

    /* io.out intrinsic: lower to IR_CALL printf */
    if (callee->kind == ND_MEMBER && callee->operand->kind == ND_IDENT &&
        strcmp(callee->operand->name, "io") == 0 &&
        strcmp(callee->name, "out") == 0) {
        if (!g->io_imported) {
            fprintf(stderr, "codegen error: io is not imported (use: import { io } from \"std\";)\n");
            g->had_error = 1;
            return IR_NONE;
        }
        if (n->nitems < 1 || n->items[0]->kind != ND_STR) {
            fprintf(stderr, "codegen error: io.out: first argument must be a string literal\n");
            g->had_error = 1;
            return IR_NONE;
        }
        int flen, nph, nv;
        Node *vals[256];
        char *cf = build_c_format(g, n, n->items[0]->str_val, &flen, &nph, vals, &nv, 256);
        int nvals = n->nitems - 1;
        if (nph != nvals) {
            fprintf(stderr, "codegen error: io.out: %d placeholder(s) but %d argument(s)\n", nph, nvals);
            g->had_error = 1;
            free(cf);
            return IR_NONE;
        }
        int sidx = intern_string(g, cf, flen);
        free(cf);

        /* pre-materialize struct-returning calls so all fields read from the temp */
        for (int k = 1; k < n->nitems; k++) {
            Node *a = n->items[k];
            if (a->kind == ND_CALL && a->int_val) {
                ExprType at = type_of(g, a);
                if (at.base == TYPE_STRUCT && at.ptr == 0)
                    lower_expr(g, f, a);
            }
        }

        /* lower every expanded value to a vreg; format string is arg 0 */
        struct { int vreg; DataType type; int ptr; } pargs[258];
        int np = 0;
        int fmt_v = ir_vreg(f);
        ir_emit(f, IR_STR, fmt_v, IR_NONE, IR_NONE)->imm = sidx;
        pargs[np++] = (typeof(pargs[0])){ fmt_v, TYPE_STR, 1 };

        for (int i = 0; i < nv; i++) {
            ExprType vt = type_of(g, vals[i]);
            if (is_i128(vt.base, vt.ptr)) {
                g->need_i128 = 1;
                int addr = lower_expr(g, f, vals[i]);
                int as = ir_arg_start(f);
                ir_arg(f, addr, TYPE_USIZE, 1, NULL, 8);
                int sv = ir_vreg(f);
                IRInst *hc = ir_emit(f, IR_CALL, sv, IR_NONE, IR_NONE);
                hc->name = strdup(vt.base == TYPE_U128 ? "mvs_u128_str" : "mvs_i128_str");
                hc->arg_start = as;
                hc->nargs = 1;
                pargs[np++] = (typeof(pargs[0])){ sv, TYPE_STR, 1 };
            } else {
                int v = lower_expr(g, f, vals[i]);
                pargs[np++] = (typeof(pargs[0])){ v, vt.base, vt.ptr };
            }
        }

        /* emit IR_CALL to printf */
        int as = ir_arg_start(f);
        for (int i = 0; i < np; i++)
            ir_arg(f, pargs[i].vreg, pargs[i].type, pargs[i].ptr, NULL, 8);
        static Node printf_sig = {0};
        printf_sig.is_extern = 1;
        printf_sig.type = TYPE_I64;
        IRInst *pc = ir_emit(f, IR_CALL, IR_NONE, IR_NONE, IR_NONE);
        pc->name = strdup("printf");
        pc->arg_start = as;
        pc->nargs = np;
        pc->sig = &printf_sig;
        return IR_NONE;
    }

    /* dyn dispatch: delegate to backend */
    if (callee->kind == ND_MEMBER) {
        ExprType bt0 = type_of(g, callee->operand);
        if (is_dyn(bt0.base, bt0.ptr) && bt0.sname) {
            int dst = sret_addr != IR_NONE ? IR_NONE : ir_vreg(f);
            IRInst *io = emit_ioout(g, f, dst, n);
            if (sret_addr != IR_NONE) io->a = sret_addr;
            return dst;
        }
    }

    /* resolve the target */
    Node *target = NULL;
    Node *self_expr = NULL;
    int self_is_ptr = 0;
    Node *sig = expr_func_sig(g, callee);
    int indirect = (sig != NULL);

    if (!indirect) {
        if (callee->kind == ND_IDENT) {
            target = find_func(g, g->cur_ns ? g->cur_ns : "", callee->name);
            if (!target) target = find_func(g, "", callee->name);
            if (!target) {
                fprintf(stderr, "lower error: undefined function '%s'\n", callee->name);
                g->had_error = 1;
                return IR_NONE;
            }
        } else if (callee->kind == ND_MEMBER) {
            ExprType bt = type_of(g, callee->operand);
            if (bt.base == TYPE_STRUCT && bt.sname) {
                target = find_func(g, bt.sname, callee->name);
                if (target) { self_expr = callee->operand; self_is_ptr = (bt.ptr > 0); }
            }
            if (!target && bt.ptr == 0 && bt.base != TYPE_STRUCT && bt.base != TYPE_DYN &&
                bt.base != TYPE_FUNC && bt.base != TYPE_VOID && bt.base != TYPE_UNKNOWN &&
                (callee->operand->kind != ND_IDENT || find_var(g, callee->operand->name))) {
                target = find_func(g, datatype_name(bt.base), callee->name);
                if (target) { self_expr = callee->operand; self_is_ptr = 0; }
            }
            if (!target && callee->operand->kind == ND_IDENT)
                target = find_func(g, callee->operand->name, callee->name);
            if (!target) {
                fprintf(stderr, "lower error: undefined function/method '%s'\n", callee->name);
                g->had_error = 1;
                return IR_NONE;
            }
        } else {
            fprintf(stderr, "lower error: unsupported call target\n");
            g->had_error = 1;
            return IR_NONE;
        }
        sig = target;
    }

    /* variadic MVS calls: delegate to backend (complex packing logic) */
    if (!indirect && target && target->variadic && !target->is_extern && !self_expr) {
        int dst = ir_vreg(f);
        IRInst *io = emit_ioout(g, f, dst, n);
        if (sret_addr != IR_NONE) io->a = sret_addr;
        return dst;
    }

    int returns_struct = ((sig->type == TYPE_STRUCT || is_blob16(sig->type, sig->ptr)) && sig->ptr == 0);

    /* Pre-lower all argument expressions BEFORE touching the arg pool.
     * Nested calls (e.g. max(lo, min(x, hi))) also push to the pool,
     * so we must finish all lowering first, then batch-push. */
    int poff = self_expr ? 1 : 0;

    struct { int vreg; DataType type; int ptr; char *sname; int size; } pre[128];
    int npre = 0;

    if (returns_struct && sret_addr != IR_NONE) {
        pre[npre++] = (typeof(pre[0])){ sret_addr, TYPE_USIZE, 1, NULL, 8 };
    }

    if (self_expr) {
        int sv;
        if (self_is_ptr)
            sv = lower_expr(g, f, self_expr);
        else
            sv = lower_addr(g, f, self_expr);
        ExprType st = type_of(g, self_expr);
        pre[npre++] = (typeof(pre[0])){ sv, st.base, st.ptr > 0 ? st.ptr : 1, st.sname,
                                          type_size(g, st.base, 1, st.sname) };
    }

    for (int i = 0; i < n->nitems; i++) {
        int av = lower_expr(g, f, n->items[i]);
        ExprType at = type_of(g, n->items[i]);
        if (i + poff < sig->nitems) {
            Node *pp = sig->items[i + poff];
            ExprType pt = { pp->type, pp->ptr, pp->type_name, pp->sig, 0, 0 };
            if (is_dyn(pp->type, pp->ptr) && !is_dyn(at.base, at.ptr)) {
                IRInst *io = emit_ioout(g, f, ir_vreg(f), n);
                if (sret_addr != IR_NONE) io->a = sret_addr;
                return f->insts[f->ninsts - 1].dst;
            }
            av = coerce_num(g, f, av, at, pt);
            if (!indirect && target && (target->is_export || target->is_extern) &&
                pp->type == TYPE_F32 && pp->ptr == 0) {
                int nv = ir_vreg(f);
                ir_emit(f, IR_F64_F32, nv, av, IR_NONE);
                av = nv;
            }
        }
        pre[npre++] = (typeof(pre[0])){ av, at.base, at.ptr, at.sname,
                                          type_size(g, at.base, at.ptr, at.sname) };
    }

    int faddr = IR_NONE;
    if (indirect)
        faddr = lower_expr(g, f, callee);

    /* NOW push args to the pool (no nested lowering can interleave) */
    int astart = ir_arg_start(f);
    for (int i = 0; i < npre; i++)
        ir_arg(f, pre[i].vreg, pre[i].type, pre[i].ptr, pre[i].sname, pre[i].size);

    int dst = ir_vreg(f);
    IRInst *ci;
    if (indirect) {
        ci = ir_emit(f, IR_ICALL, dst, faddr, IR_NONE);
    } else {
        ci = ir_emit(f, IR_CALL, dst, IR_NONE, IR_NONE);
        char lbl[LABEL_MAX];
        func_label_of(target, lbl);
        ci->name = strdup(lbl);
    }
    ci->arg_start = astart;
    ci->nargs = npre;
    ci->sig = sig;
    ci->type = sig->type;
    ci->ptr = sig->ptr;
    ci->sname = sig->type_name;

    return dst;
}

/* ------------------------------------------------------------------ */
/*  lower_expr: lower an expression, returns vreg holding result       */
/* ------------------------------------------------------------------ */

static int lower_expr(Gen *g, IRFunc *f, Node *n) {
    if (!n) return IR_NONE;

    switch (n->kind) {
        case ND_INT:
        case ND_FLOAT:
        case ND_CHAR:
        case ND_BOOL:
            return emit_imm(f, n->int_val);

        case ND_STR: {
            int idx = intern_string(g, n->str_val, n->str_len);
            int dst = ir_vreg(f);
            IRInst *i = ir_emit(f, IR_STR, dst, IR_NONE, IR_NONE);
            i->imm = idx;
            return dst;
        }

        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (s) return load_var(g, f, s);
            Node *fn = find_func(g, g->cur_ns ? g->cur_ns : "", n->name);
            if (!fn) fn = find_func(g, "", n->name);
            if (fn) {
                char lbl[LABEL_MAX]; func_label_of(fn, lbl);
                int dst = ir_vreg(f);
                IRInst *i = ir_emit(f, IR_FUNC_ADDR, dst, IR_NONE, IR_NONE);
                i->name = strdup(lbl);
                return dst;
            }
            fprintf(stderr, "lower error: undefined variable '%s'\n", n->name);
            g->had_error = 1;
            return IR_NONE;
        }

        case ND_BINARY: {
            /* short-circuit && || */
            if (n->op == TK_AND || n->op == TK_OR) {
                int lend = ir_label(f);
                int leval = ir_label(f);
                int rb = ir_vreg(f);
                int lhs = lower_expr(g, f, n->lhs);
                ir_emit(f, IR_MOV, rb, lhs, IR_NONE);
                IRInst *br = ir_emit(f, IR_BR, IR_NONE, lhs, IR_NONE);
                if (n->op == TK_AND) { br->label = leval; br->label2 = lend; }
                else                 { br->label = lend;  br->label2 = leval; }
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = leval;
                int rhs = lower_expr(g, f, n->rhs);
                ir_emit(f, IR_BOOL, rb, rhs, IR_NONE);
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lend;
                return rb;
            }

            ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);

            /* 128-bit operations */
            if (is_i128(lt.base, lt.ptr) || is_i128(rt.base, rt.ptr)) {
                int a = lower_i128_operand(g, f, n->lhs);
                int b = lower_i128_operand(g, f, n->rhs);
                int uns = (lt.base == TYPE_U128 || rt.base == TYPE_U128 ||
                           (is_unsigned_val(lt.base, lt.ptr) && is_unsigned_val(rt.base, rt.ptr)));
                if (is_cmp_op(n->op)) {
                    int tidx = f->n_i128_temps;
                    f->n_i128_temps += 2;
                    int dst = ir_vreg(f);
                    IRInst *ci = ir_emit(f, IR_I128_CMP, dst, a, b);
                    ci->cmp = cmp_op_for(n->op, uns);
                    ci->is_signed = !uns;
                    ci->imm = tidx;
                    return dst;
                }
                IR128Op iop = i128_op_for(n->op);
                if (uns && (n->op == TK_SLASH)) iop = I128_UDIV;
                if (uns && (n->op == TK_PERCENT)) iop = I128_UMOD;
                int tidx = f->n_i128_temps;
                f->n_i128_temps += 3;
                int dst = ir_vreg(f);
                IRInst *bi = ir_emit(f, IR_I128_BINOP, dst, a, b);
                bi->i128_op = iop;
                bi->imm = tidx;
                bi->is_signed = !uns;
                return dst;
            }

            /* float path */
            int fop = (is_float_type(lt.base) && lt.ptr == 0) ||
                      (is_float_type(rt.base) && rt.ptr == 0);
            if (fop && n->op == TK_STARSTAR)
                return lower_power(g, f, n, 1);
            if (fop && (is_cmp_op(n->op) || is_arith_op(n->op))) {
                int lhs = lower_expr(g, f, n->lhs);
                int rhs = lower_expr(g, f, n->rhs);
                /* convert int operand to float if needed */
                if (!is_float_type(lt.base) || lt.ptr > 0) {
                    int c = ir_vreg(f);
                    if (is_unsigned_val(lt.base, 0) && type_size(g, lt.base, 0, NULL) >= 8)
                        ir_emit(f, IR_UTOF, c, lhs, IR_NONE);
                    else
                        ir_emit(f, IR_ITOF, c, lhs, IR_NONE);
                    lhs = c;
                }
                if (!is_float_type(rt.base) || rt.ptr > 0) {
                    int c = ir_vreg(f);
                    if (is_unsigned_val(rt.base, 0) && type_size(g, rt.base, 0, NULL) >= 8)
                        ir_emit(f, IR_UTOF, c, rhs, IR_NONE);
                    else
                        ir_emit(f, IR_ITOF, c, rhs, IR_NONE);
                    rhs = c;
                }
                if (is_cmp_op(n->op)) {
                    int dst = ir_vreg(f);
                    IRInst *ci = ir_emit(f, IR_FCMP, dst, lhs, rhs);
                    ci->cmp = fcmp_op_for(n->op);
                    return dst;
                } else {
                    IROp fbo = fbinop_for(n->op);
                    int dst = ir_vreg(f);
                    ir_emit(f, fbo, dst, lhs, rhs);
                    return dst;
                }
            }

            if (n->op == TK_STARSTAR)
                return lower_power(g, f, n, 0);

            /* generic integer binary */
            int lhs = lower_expr(g, f, n->lhs);
            int rhs = lower_expr(g, f, n->rhs);

            /* pointer arithmetic: scale */
            if (n->op == TK_PLUS || n->op == TK_MINUS) {
                if (lt.ptr > 0 && rt.ptr > 0 && n->op == TK_MINUS) {
                    /* ptr - ptr : subtract then divide by element size */
                    int diff = ir_vreg(f);
                    ir_emit(f, IR_SUB, diff, lhs, rhs);
                    int sc = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                    if (sc > 1) {
                        int scv = emit_imm(f, sc);
                        int res = ir_vreg(f);
                        ir_emit(f, IR_SDIV, res, diff, scv);
                        return res;
                    }
                    return diff;
                }
                if (lt.ptr > 0 && rt.ptr == 0) {
                    int sc = type_size(g, lt.base, lt.ptr - 1, lt.sname);
                    if (sc != 1) {
                        int scv = emit_imm(f, sc);
                        int scaled = ir_vreg(f);
                        ir_emit(f, IR_MUL, scaled, rhs, scv);
                        rhs = scaled;
                    }
                }
                if (n->op == TK_PLUS && rt.ptr > 0 && lt.ptr == 0) {
                    int sc = type_size(g, rt.base, rt.ptr - 1, rt.sname);
                    if (sc != 1) {
                        int scv = emit_imm(f, sc);
                        int scaled = ir_vreg(f);
                        ir_emit(f, IR_MUL, scaled, lhs, scv);
                        lhs = scaled;
                    }
                }
            }

            /* signedness */
            int uns;
            if (n->op==TK_SHL || n->op==TK_SHR || n->op==TK_SLASH || n->op==TK_PERCENT)
                uns = is_unsigned_val(lt.base, lt.ptr);
            else
                uns = is_unsigned_val(lt.base, lt.ptr) || is_unsigned_val(rt.base, rt.ptr);

            if (is_cmp_op(n->op)) {
                int dst = ir_vreg(f);
                IRInst *ci = ir_emit(f, IR_CMP, dst, lhs, rhs);
                ci->cmp = cmp_op_for(n->op, uns);
                return dst;
            }

            IROp bop = binop_for(n->op, uns);
            if (bop == IR_NOP) {
                fprintf(stderr, "lower error: unsupported binary op %d\n", n->op);
                g->had_error = 1;
                return IR_NONE;
            }
            int dst = ir_vreg(f);
            IRInst *ci = ir_emit(f, bop, dst, lhs, rhs);
            ci->is_signed = !uns;
            ci->size = type_size(g, lt.base, 0, NULL);
            return dst;
        }

        case ND_UNARY: {
            ExprType ot = type_of(g, n->operand);

            /* i128 unary */
            if ((n->op == TK_MINUS || n->op == TK_TILDE) && is_i128(ot.base, ot.ptr)) {
                int a = lower_expr(g, f, n->operand);
                int tidx = f->n_i128_temps;
                f->n_i128_temps += 2;
                int dst = ir_vreg(f);
                IRInst *ui = ir_emit(f, IR_I128_UNOP, dst, a, IR_NONE);
                ui->i128_op = n->op == TK_MINUS ? I128_NEG : I128_NOT;
                ui->imm = tidx;
                return dst;
            }

            if (n->op == TK_MINUS) {
                int val = lower_expr(g, f, n->operand);
                int dst = ir_vreg(f);
                if (is_float_type(ot.base) && ot.ptr == 0)
                    ir_emit(f, IR_FNEG, dst, val, IR_NONE);
                else
                    ir_emit(f, IR_NEG, dst, val, IR_NONE);
                return dst;
            }
            if (n->op == TK_NOT) {
                int val = lower_expr(g, f, n->operand);
                int zero = emit_imm(f, 0);
                int dst = ir_vreg(f);
                IRInst *ci = ir_emit(f, IR_CMP, dst, val, zero);
                ci->cmp = CMP_EQ;
                return dst;
            }
            if (n->op == TK_TILDE) {
                int val = lower_expr(g, f, n->operand);
                int dst = ir_vreg(f);
                ir_emit(f, IR_BNOT, dst, val, IR_NONE);
                /* truncate to type width */
                if (ot.ptr == 0) {
                    int sz = type_size(g, ot.base, 0, ot.sname);
                    if (sz < 8) {
                        int t = ir_vreg(f);
                        IRInst *tr = ir_emit(f, is_signed_type(ot.base) ? IR_SEXT : IR_ZEXT, t, dst, IR_NONE);
                        tr->size = sz;
                        tr->imm = 8;
                        dst = t;
                    }
                }
                return dst;
            }
            if (n->op == TK_AMP) {
                return lower_addr(g, f, n->operand);
            }
            if (n->op == TK_STAR) {
                int ptr = lower_expr(g, f, n->operand);
                ExprType pt = type_of(g, n->operand);
                int pptr = pt.ptr > 0 ? pt.ptr - 1 : 0;
                if (pt.base == TYPE_STRUCT && pptr == 0) return ptr;
                if (is_blob16(pt.base, pptr)) return ptr;
                return emit_load(g, f, ptr, pt.base, pptr, pt.sname);
            }
            if (n->op == TK_PLUSPLUS || n->op == TK_MINUSMINUS) {
                if (n->operand->kind != ND_IDENT) {
                    fprintf(stderr, "lower error: ++/-- requires a variable\n");
                    g->had_error = 1;
                    return IR_NONE;
                }
                Sym *s = find_var(g, n->operand->name);
                if (!s) {
                    fprintf(stderr, "lower error: undefined variable '%s'\n", n->operand->name);
                    g->had_error = 1;
                    return IR_NONE;
                }
                int old = load_var(g, f, s);
                int inc = (s->ptr > 0) ? type_size(g, s->type, s->ptr - 1, s->sname) : 1;
                int incv = emit_imm(f, inc);
                int nv = ir_vreg(f);
                ir_emit(f, n->op == TK_PLUSPLUS ? IR_ADD : IR_SUB, nv, old, incv);
                store_var(g, f, s, nv);
                return old;
            }
            break;
        }

        case ND_CAST: {
            ExprType st = type_of(g, n->operand);

            /* cast TO i128 */
            if (is_i128(n->type, n->ptr) && !is_i128(st.base, st.ptr)) {
                int val = lower_expr(g, f, n->operand);
                int tidx = f->n_i128_temps++;
                int dst = ir_vreg(f);
                IRInst *fi = ir_emit(f, IR_I128_FROM, dst, val, IR_NONE);
                fi->imm = tidx;
                fi->is_signed = is_signed_type(st.base) && st.ptr == 0;
                return dst;
            }
            /* cast FROM i128 */
            if (!is_i128(n->type, n->ptr) && is_i128(st.base, st.ptr)) {
                int addr = lower_expr(g, f, n->operand);
                int lo = ir_vreg(f);
                IRInst *ld = ir_emit(f, IR_LOAD, lo, addr, IR_NONE);
                ld->type = TYPE_USIZE; ld->size = 8;
                if (n->type == TYPE_BOOL && n->ptr == 0) {
                    int ha = ir_vreg(f);
                    ir_emit(f, IR_OFFSET, ha, addr, IR_NONE)->imm = 8;
                    int hi = ir_vreg(f);
                    IRInst *ldh = ir_emit(f, IR_LOAD, hi, ha, IR_NONE);
                    ldh->type = TYPE_USIZE; ldh->size = 8;
                    int orv = ir_vreg(f);
                    ir_emit(f, IR_BOR, orv, lo, hi);
                    int dst = ir_vreg(f);
                    ir_emit(f, IR_BOOL, dst, orv, IR_NONE);
                    return dst;
                }
                int sz = type_size(g, n->type, n->ptr, n->type_name);
                if (sz < 8 && n->ptr == 0) {
                    int dst = ir_vreg(f);
                    IRInst *ext = ir_emit(f, is_signed_type(n->type) ? IR_SEXT : IR_ZEXT,
                                          dst, lo, IR_NONE);
                    ext->size = sz;
                    return dst;
                }
                return lo;
            }

            int val = lower_expr(g, f, n->operand);
            int src_f = is_float_type(st.base) && st.ptr == 0;
            int dst_f = is_float_type(n->type) && n->ptr == 0;

            /* int -> float */
            if (!src_f && dst_f) {
                int dst = ir_vreg(f);
                if (is_unsigned_val(st.base, 0) && type_size(g, st.base, 0, NULL) >= 8)
                    ir_emit(f, IR_UTOF, dst, val, IR_NONE);
                else
                    ir_emit(f, IR_ITOF, dst, val, IR_NONE);
                return dst;
            }
            /* float -> float: same domain */
            if (src_f && dst_f) return val;
            /* float -> int */
            if (src_f && !dst_f) {
                int dst = ir_vreg(f);
                ir_emit(f, IR_FTOI, dst, val, IR_NONE);
                val = dst;
                /* fall through to width adjustment */
            }
            /* bool cast */
            if (n->ptr == 0 && n->type == TYPE_BOOL) {
                int dst = ir_vreg(f);
                ir_emit(f, IR_BOOL, dst, val, IR_NONE);
                return dst;
            }
            /* integer width adjustment */
            if (n->ptr == 0 && !dst_f &&
                n->type != TYPE_STR && n->type != TYPE_VOID && n->type != TYPE_STRUCT) {
                int sz = type_size(g, n->type, 0, NULL);
                if (sz < 8) {
                    int dst = ir_vreg(f);
                    IRInst *tr = ir_emit(f, is_signed_type(n->type) ? IR_SEXT : IR_ZEXT,
                                         dst, val, IR_NONE);
                    tr->size = sz;
                    tr->imm = 8;
                    return dst;
                }
            }
            return val;
        }

        case ND_ASSIGN: {
            Node *target = n->lhs;
            ExprType tt = type_of(g, target);

            /* array assignment: error */
            if (tt.arr > 0) {
                fprintf(stderr, "lower error: whole-array assignment is not supported\n");
                g->had_error = 1;
                return IR_NONE;
            }

            /* i128 simple assignment */
            if (is_i128(tt.base, tt.ptr) && n->op == TK_ASSIGN) {
                int addr = lower_addr(g, f, target);
                emit_i128_store(g, f, addr, n->rhs);
                return addr;
            }
            /* dyn assignment: delegate */
            if (is_dyn(tt.base, tt.ptr)) {
                int dst = ir_vreg(f);
                emit_ioout(g, f, dst, n);
                return dst;
            }

            /* whole-struct assignment */
            if (tt.base == TYPE_STRUCT && tt.ptr == 0 && n->op == TK_ASSIGN) {
                int addr = lower_addr(g, f, target);
                lower_store_struct(g, f, addr, n->rhs);
                return addr;
            }

            if (n->op == TK_ASSIGN) {
                int val = lower_expr(g, f, n->rhs);
                ExprType rvt = type_of(g, n->rhs);
                val = coerce_num(g, f, val, rvt, tt);
                int addr = lower_addr(g, f, target);
                emit_store(g, f, addr, val, tt.base, tt.ptr, tt.sname);
                return val;
            }

            /* compound assignment: x op= y */
            TokenType bop = n->op == TK_PLUS_ASSIGN ? TK_PLUS :
                            n->op == TK_MINUS_ASSIGN ? TK_MINUS :
                            n->op == TK_STAR_ASSIGN ? TK_STAR : TK_SLASH;
            int addr = lower_addr(g, f, target);
            int rhs = lower_expr(g, f, n->rhs);
            int old = emit_load(g, f, addr, tt.base, tt.ptr, tt.sname);

            if (is_float_type(tt.base) && tt.ptr == 0) {
                ExprType rt2 = type_of(g, n->rhs);
                if (!is_float_type(rt2.base) || rt2.ptr > 0) {
                    int c = ir_vreg(f);
                    if (is_unsigned_val(rt2.base, 0) && type_size(g, rt2.base, 0, NULL) >= 8)
                        ir_emit(f, IR_UTOF, c, rhs, IR_NONE);
                    else
                        ir_emit(f, IR_ITOF, c, rhs, IR_NONE);
                    rhs = c;
                }
                IROp fbo = fbinop_for(bop);
                int res = ir_vreg(f);
                ir_emit(f, fbo, res, old, rhs);
                emit_store(g, f, addr, res, tt.base, tt.ptr, tt.sname);
                return res;
            }

            /* pointer += / -= : scale rhs */
            if (tt.ptr > 0 && (bop == TK_PLUS || bop == TK_MINUS)) {
                int sc = type_size(g, tt.base, tt.ptr - 1, tt.sname);
                if (sc != 1) {
                    int scv = emit_imm(f, sc);
                    int scaled = ir_vreg(f);
                    ir_emit(f, IR_MUL, scaled, rhs, scv);
                    rhs = scaled;
                }
            }

            int uns = is_unsigned_val(tt.base, tt.ptr);
            IROp iop = binop_for(bop, uns);
            int res = ir_vreg(f);
            IRInst *ci = ir_emit(f, iop, res, old, rhs);
            ci->is_signed = !uns;
            ci->size = type_size(g, tt.base, 0, NULL);
            emit_store(g, f, addr, res, tt.base, tt.ptr, tt.sname);
            return res;
        }

        case ND_CALL: {
            /* struct-returning call as rvalue */
            ExprType rt = type_of(g, n);
            if ((rt.base == TYPE_STRUCT || is_blob16(rt.base, rt.ptr)) &&
                rt.ptr == 0 && n->int_val) {
                int sret = emit_local_addr(f, (int)n->int_val);
                lower_call_inner(g, f, n, sret);
                return emit_local_addr(f, (int)n->int_val);
            }
            return lower_call_inner(g, f, n, IR_NONE);
        }

        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            /* arr.len: compile-time constant */
            if (bt.arr > 0 && n->name && strcmp(n->name, "len") == 0)
                return emit_imm(f, bt.arr);

            ExprType et = type_of(g, n);
            int addr = lower_addr(g, f, n);
            if (et.base == TYPE_STRUCT && et.ptr == 0) return addr;
            if (et.arr > 0) return addr;
            if (is_blob16(et.base, et.ptr)) return addr;
            return emit_load(g, f, addr, et.base, et.ptr, et.sname);
        }

        case ND_INDEX: {
            ExprType et = type_of(g, n);
            int addr = lower_addr(g, f, n);
            if (et.base == TYPE_STRUCT && et.ptr == 0) return addr;
            if (is_blob16(et.base, et.ptr)) return addr;
            return emit_load(g, f, addr, et.base, et.ptr, et.sname);
        }

        case ND_SLICE:
            return lower_addr(g, f, n);

        case ND_ARRAY_LIT:
            fprintf(stderr, "lower error: array literal can only initialize a variable\n");
            g->had_error = 1;
            return IR_NONE;

        case ND_FRAMEREF:
            return lower_addr(g, f, n);

        case ND_STRUCT_LIT:
            if (n->int_val) {
                int addr = emit_local_addr(f, (int)n->int_val);
                lower_store_struct(g, f, addr, n);
                return emit_local_addr(f, (int)n->int_val);
            }
            fprintf(stderr, "lower error: struct literal has no destination\n");
            g->had_error = 1;
            return IR_NONE;

        default:
            fprintf(stderr, "lower error: unsupported expression (kind %d)\n", n->kind);
            g->had_error = 1;
            return IR_NONE;
    }
    return IR_NONE;
}

/* ------------------------------------------------------------------ */
/*  lower_stmt: lower a statement to IR                                */
/* ------------------------------------------------------------------ */

static void lower_stmt(Gen *g, IRFunc *f, Node *n) {
    if (!n) return;

    /* debug line info */
    if (mvs_debug_lines && n->line > 0) {
        IRInst *li = ir_emit(f, IR_LINE, IR_NONE, IR_NONE, IR_NONE);
        li->line = n->line;
        li->file = n->file;
    }

    switch (n->kind) {
        case ND_VAR_DECL: {
            Sym *s = &g->locals[n->int_val];
            if (!n->operand || !s) {
                g->visible[g->nvisible++] = (int)n->int_val;
                break;
            }

            if (s->arr > 0) {
                /* array variable: store each element */
                if (n->operand->kind != ND_ARRAY_LIT) {
                    fprintf(stderr, "lower error: array variable needs an array literal initializer\n");
                    g->had_error = 1; break;
                }
                int esz = type_size(g, s->type, s->ptr, s->sname);
                for (int i = 0; i < n->operand->nitems && i < s->arr; i++) {
                    Node *el = n->operand->items[i];
                    int eoff = s->offset - i * esz;
                    if (s->type == TYPE_STRUCT && s->ptr == 0) {
                        int eaddr = emit_local_addr(f, eoff);
                        lower_store_struct(g, f, eaddr, el);
                    } else if (is_i128(s->type, s->ptr)) {
                        int eaddr = emit_local_addr(f, eoff);
                        emit_i128_store(g, f, eaddr, el);
                    } else if (is_dyn(s->type, s->ptr)) {
                        emit_ioout(g, f, IR_NONE, n);
                        g->visible[g->nvisible++] = (int)n->int_val;
                        return;
                    } else {
                        int val = lower_expr(g, f, el);
                        ExprType vt2 = type_of(g, el);
                        ExprType dt2 = { s->type, s->ptr, s->sname, s->sig, 0, 0 };
                        val = coerce_num(g, f, val, vt2, dt2);
                        int eaddr = emit_local_addr(f, eoff);
                        emit_store(g, f, eaddr, val, s->type, s->ptr, s->sname);
                    }
                }
            } else if (s->type == TYPE_STRUCT && s->ptr == 0) {
                int addr;
                if (s->is_global) addr = emit_global_addr(f, s->name);
                else              addr = emit_local_addr(f, s->offset);
                lower_store_struct(g, f, addr, n->operand);
            } else if (is_i128(s->type, s->ptr)) {
                int addr;
                if (s->is_global) addr = emit_global_addr(f, s->name);
                else              addr = emit_local_addr(f, s->offset);
                emit_i128_store(g, f, addr, n->operand);
            } else if (is_dyn(s->type, s->ptr)) {
                emit_ioout(g, f, IR_NONE, n);
            } else {
                int val = lower_expr(g, f, n->operand);
                ExprType vt = type_of(g, n->operand);
                ExprType dt = { s->type, s->ptr, s->sname, s->sig, s->arr, 0 };
                val = coerce_num(g, f, val, vt, dt);
                store_var(g, f, s, val);
            }
            g->visible[g->nvisible++] = (int)n->int_val;
            break;
        }

        case ND_EXPR_STMT:
            lower_expr(g, f, n->operand);
            break;

        case ND_RETURN: {
            int returns_struct = (g->sret_off != 0);
            if (returns_struct && n->operand) {
                if (g->cur_ret_i128) {
                    int sret = emit_local_addr(f, g->sret_off);
                    int sptr = emit_load(g, f, sret, TYPE_USIZE, 1, NULL);
                    emit_i128_store(g, f, sptr, n->operand);
                    ir_emit(f, IR_RET, IR_NONE, IR_NONE, IR_NONE);
                    break;
                }
                if (g->cur_ret_dyn) {
                    emit_ioout(g, f, IR_NONE, n);
                    ir_emit(f, IR_RET, IR_NONE, IR_NONE, IR_NONE);
                    break;
                }
                /* struct return: write value through sret pointer */
                int sret = emit_local_addr(f, g->sret_off);
                int sptr = emit_load(g, f, sret, TYPE_USIZE, 1, NULL);
                lower_store_struct(g, f, sptr, n->operand);
                int rval = emit_local_addr(f, g->sret_off);
                int rv = emit_load(g, f, rval, TYPE_USIZE, 1, NULL);
                ir_emit(f, IR_RET, IR_NONE, rv, IR_NONE);
            } else if (n->operand) {
                int val = lower_expr(g, f, n->operand);
                ExprType vt = type_of(g, n->operand);
                int vf = is_float_type(vt.base) && vt.ptr == 0;
                /* coerce int<->float for return value */
                if (g->cur_ret_float && !vf) {
                    int c = ir_vreg(f);
                    if (is_unsigned_val(vt.base, 0) && type_size(g, vt.base, 0, NULL) >= 8)
                        ir_emit(f, IR_UTOF, c, val, IR_NONE);
                    else
                        ir_emit(f, IR_ITOF, c, val, IR_NONE);
                    val = c;
                } else if (!g->cur_ret_float && vf) {
                    int c = ir_vreg(f);
                    ir_emit(f, IR_FTOI, c, val, IR_NONE);
                    val = c;
                }
                IRInst *ret = ir_emit(f, IR_RET, IR_NONE, val, IR_NONE);
                ret->type = vt.base;
                ret->ptr = vt.ptr;
            } else {
                int zero = emit_imm(f, 0);
                ir_emit(f, IR_RET, IR_NONE, zero, IR_NONE);
            }
            break;
        }

        case ND_BLOCK: {
            int mark = g->nvisible;
            for (int i = 0; i < n->nitems; i++)
                lower_stmt(g, f, n->items[i]);
            g->nvisible = mark;
            break;
        }

        case ND_IF: {
            int lelse = ir_label(f);
            int lend = ir_label(f);
            int cond = lower_expr(g, f, n->cond);
            IRInst *br = ir_emit(f, IR_BR, IR_NONE, cond, IR_NONE);
            int lthen = ir_label(f);
            br->label = lthen;
            br->label2 = lelse;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lthen;
            lower_stmt(g, f, n->then_branch);
            ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = lend;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lelse;
            if (n->else_branch)
                lower_stmt(g, f, n->else_branch);
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lend;
            break;
        }

        case ND_WHILE: {
            int lcont = ir_label(f);
            int lbrk = ir_label(f);
            if (g->nloops >= MAX_LOOP) {
                fprintf(stderr, "lower error: too many nested loops (max %d)\n", MAX_LOOP);
                g->had_error = 1; break;
            }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = lcont;
            g->nloops++;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lcont;
            int cond = lower_expr(g, f, n->cond);
            int lbody = ir_label(f);
            IRInst *br = ir_emit(f, IR_BR, IR_NONE, cond, IR_NONE);
            br->label = lbody;
            br->label2 = lbrk;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbody;
            lower_stmt(g, f, n->body);
            ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = lcont;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbrk;
            g->nloops--;
            break;
        }

        case ND_FOR: {
            int lcond = ir_label(f);
            int lcont = ir_label(f);
            int lbrk = ir_label(f);
            int mark = g->nvisible;
            if (n->init) {
                if (n->init->kind == ND_VAR_DECL) lower_stmt(g, f, n->init);
                else lower_expr(g, f, n->init);
            }
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lcond;
            if (n->cond) {
                int cond = lower_expr(g, f, n->cond);
                int lbody = ir_label(f);
                IRInst *br = ir_emit(f, IR_BR, IR_NONE, cond, IR_NONE);
                br->label = lbody;
                br->label2 = lbrk;
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbody;
            }
            if (g->nloops >= MAX_LOOP) {
                fprintf(stderr, "lower error: too many nested loops (max %d)\n", MAX_LOOP);
                g->had_error = 1; break;
            }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = lcont;
            g->nloops++;
            lower_stmt(g, f, n->body);
            g->nloops--;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lcont;
            if (n->step) lower_expr(g, f, n->step);
            ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = lcond;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbrk;
            g->nvisible = mark;
            break;
        }

        case ND_DOWHILE: {
            int lstart = ir_label(f);
            int lcont = ir_label(f);
            int lbrk = ir_label(f);
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lstart;
            if (g->nloops >= MAX_LOOP) {
                fprintf(stderr, "lower error: too many nested loops (max %d)\n", MAX_LOOP);
                g->had_error = 1; break;
            }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = lcont;
            g->nloops++;
            lower_stmt(g, f, n->body);
            g->nloops--;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lcont;
            int cond = lower_expr(g, f, n->cond);
            IRInst *br = ir_emit(f, IR_BR, IR_NONE, cond, IR_NONE);
            br->label = lstart;
            br->label2 = lbrk;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbrk;
            break;
        }

        case ND_SWITCH: {
            int lbrk = ir_label(f);
            int val = lower_expr(g, f, n->cond);
            int ncases = n->nitems;
            int *caselbl = (int *)malloc(sizeof(int) * (ncases > 0 ? ncases : 1));
            int ldefault = -1;
            for (int i = 0; i < ncases; i++) {
                caselbl[i] = ir_label(f);
                Node *c = n->items[i];
                if (!c->operand) { ldefault = caselbl[i]; continue; }
                int cv = lower_expr(g, f, c->operand);
                int eq = ir_vreg(f);
                IRInst *ci = ir_emit(f, IR_CMP, eq, val, cv);
                ci->cmp = CMP_EQ;
                int lnext = ir_label(f);
                IRInst *br = ir_emit(f, IR_BR, IR_NONE, eq, IR_NONE);
                br->label = caselbl[i];
                br->label2 = lnext;
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lnext;
            }
            if (ldefault >= 0)
                ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = ldefault;
            else
                ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = lbrk;
            if (g->nloops >= MAX_LOOP) {
                fprintf(stderr, "lower error: too many nested loops/switch (max %d)\n", MAX_LOOP);
                g->had_error = 1; free(caselbl); break;
            }
            g->loops[g->nloops].brk = lbrk;
            g->loops[g->nloops].cont = -1;
            g->nloops++;
            for (int i = 0; i < ncases; i++) {
                ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = caselbl[i];
                for (int j = 0; j < n->items[i]->nitems; j++)
                    lower_stmt(g, f, n->items[i]->items[j]);
            }
            g->nloops--;
            ir_emit(f, IR_LABEL, IR_NONE, IR_NONE, IR_NONE)->label = lbrk;
            free(caselbl);
            break;
        }

        case ND_BREAK:
            if (g->nloops == 0) {
                fprintf(stderr, "lower error: 'break' outside loop/switch\n");
                g->had_error = 1; break;
            }
            ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = g->loops[g->nloops - 1].brk;
            break;

        case ND_CONTINUE: {
            int target = -1;
            for (int i = g->nloops - 1; i >= 0; i--)
                if (g->loops[i].cont >= 0) { target = g->loops[i].cont; break; }
            if (target < 0) {
                fprintf(stderr, "lower error: 'continue' outside loop\n");
                g->had_error = 1; break;
            }
            ir_emit(f, IR_JMP, IR_NONE, IR_NONE, IR_NONE)->label = target;
            break;
        }

        default:
            fprintf(stderr, "lower error: unsupported statement (kind %d)\n", n->kind);
            g->had_error = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  lower_func: the main entry point                                   */
/* ------------------------------------------------------------------ */

IRFunc *lower_func(Gen *g, Node *fn, Node *program, int nregpool) {
    char lbl[LABEL_MAX];
    func_label_of(fn, lbl);

    IRFunc *f = ir_func_new(lbl, fn);

    /* per-function state setup (mirrors the top of gen_func in each backend) */
    g->cur_ns = fn->mod ? fn->mod : "";
    g->nlocals = 0;
    g->nvisible = 0;
    int frame = 0;
    g->sret_off = 0;

    /* register allocation plan */
    regplan_build(g, fn, nregpool);
    g->regsave_off = 0;
    if (g->plan.n > 0) {
        frame += g->plan.n * 8;
        g->regsave_off = frame;
    }

    /* struct/i128/dyn-returning function: reserve sret slot */
    int returns_struct = ((fn->type == TYPE_STRUCT || is_blob16(fn->type, fn->ptr)) &&
                          fn->ptr == 0 && !fn->is_extern);
    if (returns_struct)
        g->sret_off = add_local(g, "$sret", TYPE_USIZE, 0, 0, 0, NULL, NULL, &frame);

    g->cur_ret_float = is_float_type(fn->type) && fn->ptr == 0;
    g->cur_ret_f32c = fn->is_export && fn->type == TYPE_F32 && fn->ptr == 0;
    g->cur_ret_i128 = is_i128(fn->type, fn->ptr);
    g->cur_ret_dyn = is_dyn(fn->type, fn->ptr) ? fn->type_name : NULL;

    /* reserve parameter slots */
    int param_start = g->nlocals;
    for (int i = 0; i < fn->nitems; i++)
        add_local(g, fn->items[i]->name, fn->items[i]->type, fn->items[i]->ptr,
                  0, 0, fn->items[i]->type_name, fn->items[i]->sig, &frame);

    /* reserve all local variable slots */
    collect_locals(g, fn->body, &frame);

    /* reserve temp slots for struct-returning calls */
    int saved_vis = g->nvisible;
    for (int i = 0; i < fn->nitems; i++)
        g->visible[g->nvisible++] = param_start + i;
    collect_struct_temps(g, fn->body, &frame);
    if ((fn->ns == NULL || fn->ns[0] == 0) && strcmp(fn->name, "main") == 0)
        for (int i = 0; i < program->nitems; i++)
            if (program->items[i]->kind == ND_VAR_DECL && program->items[i]->operand)
                collect_struct_temps(g, program->items[i]->operand, &frame);
    g->nvisible = saved_vis;

    /* parameters become visible */
    for (int i = 0; i < fn->nitems; i++)
        g->visible[g->nvisible++] = param_start + i;

    /* if this is main, lower global initializers */
    if ((fn->ns == NULL || fn->ns[0] == 0) && strcmp(fn->name, "main") == 0) {
        for (int i = 0; i < program->nitems; i++) {
            Node *d = program->items[i];
            if (d->kind == ND_VAR_DECL && d->operand) {
                Sym *s = find_var(g, d->name);
                if (!s) continue;
                if (s->arr > 0) {
                    if (d->operand->kind != ND_ARRAY_LIT) {
                        fprintf(stderr, "lower error: global array needs an array literal initializer\n");
                        g->had_error = 1; continue;
                    }
                    int esz = type_size(g, s->type, s->ptr, s->sname);
                    for (int j = 0; j < d->operand->nitems && j < s->arr; j++) {
                        Node *el = d->operand->items[j];
                        int eaddr = ir_vreg(f);
                        IRInst *ga = ir_emit(f, IR_GLOBAL_ADDR, eaddr, IR_NONE, IR_NONE);
                        ga->name = s->name;
                        if (j != 0) {
                            int off = ir_vreg(f);
                            IRInst *oi = ir_emit(f, IR_OFFSET, off, eaddr, IR_NONE);
                            oi->imm = j * esz;
                            eaddr = off;
                        }
                        if (s->type == TYPE_STRUCT && s->ptr == 0) {
                            lower_store_struct(g, f, eaddr, el);
                        } else if (is_i128(s->type, s->ptr)) {
                            emit_i128_store(g, f, eaddr, el);
                        } else if (is_dyn(s->type, s->ptr)) {
                            emit_ioout(g, f, IR_NONE, d);
                            break;
                        } else {
                            int val = lower_expr(g, f, el);
                            ExprType vt2 = type_of(g, el);
                            ExprType dt2 = { s->type, s->ptr, s->sname, s->sig, 0, 0 };
                            val = coerce_num(g, f, val, vt2, dt2);
                            emit_store(g, f, eaddr, val, s->type, s->ptr, s->sname);
                        }
                    }
                } else if (s->type == TYPE_STRUCT && s->ptr == 0) {
                    int addr = emit_global_addr(f, s->name);
                    lower_store_struct(g, f, addr, d->operand);
                } else if (is_i128(s->type, s->ptr)) {
                    int addr = emit_global_addr(f, s->name);
                    emit_i128_store(g, f, addr, d->operand);
                } else if (is_dyn(s->type, s->ptr)) {
                    emit_ioout(g, f, IR_NONE, d);
                } else {
                    int val = lower_expr(g, f, d->operand);
                    ExprType vt = type_of(g, d->operand);
                    ExprType dt = { s->type, s->ptr, s->sname, s->sig, s->arr, 0 };
                    val = coerce_num(g, f, val, vt, dt);
                    store_var(g, f, s, val);
                }
            }
        }
    }

    /* lower the function body */
    lower_stmt(g, f, fn->body);

    /* store metadata the emitter needs */
    f->frame_size = frame;
    f->nlocals_snap = g->nlocals;
    f->param_start = param_start;
    f->nparams = fn->nitems;

    ir_optimize(f);
    return f;
}
