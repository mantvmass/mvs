/*
 * ir.c - IR construction helpers and debug printing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"

/* initial capacities (grown as needed) */
#define INIT_INSTS 256
#define INIT_ARGS  128

IRFunc *ir_func_new(const char *name, Node *fn) {
    IRFunc *f = (IRFunc *)calloc(1, sizeof(IRFunc));
    f->name = strdup(name);
    f->fn = fn;
    f->insts = (IRInst *)calloc(INIT_INSTS, sizeof(IRInst));
    f->cap = INIT_INSTS;
    f->args = (IRArg *)calloc(INIT_ARGS, sizeof(IRArg));
    f->args_cap = INIT_ARGS;
    return f;
}

void ir_func_free(IRFunc *f) {
    if (!f) return;
    free(f->name);
    free(f->insts);
    free(f->args);
    free(f);
}

int ir_vreg(IRFunc *f) {
    return f->next_vreg++;
}

int ir_label(IRFunc *f) {
    return f->next_label++;
}

IRInst *ir_emit(IRFunc *f, IROp op, int dst, int a, int b) {
    if (f->ninsts >= f->cap) {
        f->cap *= 2;
        f->insts = (IRInst *)realloc(f->insts, f->cap * sizeof(IRInst));
    }
    IRInst *i = &f->insts[f->ninsts++];
    memset(i, 0, sizeof(IRInst));
    i->op = op;
    i->dst = dst;
    i->a = a;
    i->b = b;
    return i;
}

int ir_arg_start(IRFunc *f) {
    return f->nargs;
}

int ir_arg(IRFunc *f, int vreg, DataType type, int ptr, char *sname, int size) {
    if (f->nargs >= f->args_cap) {
        f->args_cap *= 2;
        f->args = (IRArg *)realloc(f->args, f->args_cap * sizeof(IRArg));
    }
    int idx = f->nargs++;
    IRArg *a = &f->args[idx];
    memset(a, 0, sizeof(IRArg));
    a->vreg = vreg;
    a->type = type;
    a->ptr = ptr;
    a->sname = sname;
    a->size = size;
    return idx;
}

/* --- debug printing --- */

const char *ir_op_name(IROp op) {
    switch (op) {
        case IR_IMM:        return "imm";
        case IR_STR:        return "str";
        case IR_FUNC_ADDR:  return "func_addr";
        case IR_LOCAL_ADDR: return "local_addr";
        case IR_GLOBAL_ADDR:return "global_addr";
        case IR_LOAD:       return "load";
        case IR_STORE:      return "store";
        case IR_COPY:       return "copy";
        case IR_OFFSET:     return "offset";
        case IR_INDEX:      return "index";
        case IR_ADD:        return "add";
        case IR_SUB:        return "sub";
        case IR_MUL:        return "mul";
        case IR_SDIV:       return "sdiv";
        case IR_UDIV:       return "udiv";
        case IR_SMOD:       return "smod";
        case IR_UMOD:       return "umod";
        case IR_SHL:        return "shl";
        case IR_SAR:        return "sar";
        case IR_SHR:        return "shr";
        case IR_BAND:       return "and";
        case IR_BOR:        return "or";
        case IR_BXOR:       return "xor";
        case IR_NEG:        return "neg";
        case IR_BNOT:       return "not";
        case IR_CMP:        return "cmp";
        case IR_FADD:       return "fadd";
        case IR_FSUB:       return "fsub";
        case IR_FMUL:       return "fmul";
        case IR_FDIV:       return "fdiv";
        case IR_FNEG:       return "fneg";
        case IR_FPOW:       return "fpow";
        case IR_FCMP:       return "fcmp";
        case IR_ITOF:       return "itof";
        case IR_UTOF:       return "utof";
        case IR_FTOI:       return "ftoi";
        case IR_SEXT:       return "sext";
        case IR_ZEXT:       return "zext";
        case IR_TRUNC:      return "trunc";
        case IR_F32_F64:    return "f32_f64";
        case IR_F64_F32:    return "f64_f32";
        case IR_BOOL:       return "bool";
        case IR_LABEL:      return "label";
        case IR_BR:         return "br";
        case IR_JMP:        return "jmp";
        case IR_RET:        return "ret";
        case IR_CALL:       return "call";
        case IR_ICALL:      return "icall";
        case IR_ASM:        return "asm";
        case IR_ATOMIC:     return "atomic";
        case IR_I128_BINOP: return "i128_binop";
        case IR_I128_UNOP:  return "i128_unop";
        case IR_I128_CMP:   return "i128_cmp";
        case IR_I128_FROM:  return "i128_from";
        case IR_I128_STORE: return "i128_store";
        case IR_I128_LOAD:  return "i128_load";
        case IR_DYN_WRAP:   return "dyn_wrap";
        case IR_DYN_STORE:  return "dyn_store";
        case IR_DYN_CALL:   return "dyn_call";
        case IR_LINE:       return "line";
        case IR_MOV:        return "mov";
        case IR_NOP:        return "nop";
    }
    return "???";
}

static const char *cmp_name(IRCmpOp c) {
    switch (c) {
        case CMP_EQ:  return "eq";  case CMP_NE:  return "ne";
        case CMP_SLT: return "slt"; case CMP_SGT: return "sgt";
        case CMP_SLE: return "sle"; case CMP_SGE: return "sge";
        case CMP_ULT: return "ult"; case CMP_UGT: return "ugt";
        case CMP_ULE: return "ule"; case CMP_UGE: return "uge";
        case CMP_FLT: return "flt"; case CMP_FGT: return "fgt";
        case CMP_FLE: return "fle"; case CMP_FGE: return "fge";
        case CMP_FEQ: return "feq"; case CMP_FNE: return "fne";
    }
    return "?";
}

static void print_vreg(int v) {
    if (v == IR_NONE) fprintf(stderr, "_");
    else              fprintf(stderr, "%%%d", v);
}

void ir_dump(IRFunc *f) {
    fprintf(stderr, "--- IR: %s (%d insts, %d vregs) ---\n",
            f->name ? f->name : "<anon>", f->ninsts, f->next_vreg);
    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];
        if (inst->op == IR_LABEL) {
            fprintf(stderr, ".L%d:\n", inst->label);
            continue;
        }
        if (inst->op == IR_LINE) {
            fprintf(stderr, "  ; line %d\n", inst->line);
            continue;
        }
        fprintf(stderr, "  ");
        if (inst->dst != IR_NONE) { print_vreg(inst->dst); fprintf(stderr, " = "); }
        fprintf(stderr, "%s", ir_op_name(inst->op));

        switch (inst->op) {
            case IR_IMM:
                fprintf(stderr, " %lld", inst->imm);
                break;
            case IR_STR:
                fprintf(stderr, " str_%lld", inst->imm);
                break;
            case IR_FUNC_ADDR:
            case IR_GLOBAL_ADDR:
                fprintf(stderr, " %s", inst->name ? inst->name : "?");
                break;
            case IR_LOCAL_ADDR:
                fprintf(stderr, " [rbp-%lld]", inst->imm);
                break;
            case IR_LOAD:
            case IR_STORE:
                fprintf(stderr, " ");
                print_vreg(inst->a);
                if (inst->op == IR_STORE) { fprintf(stderr, ", "); print_vreg(inst->b); }
                fprintf(stderr, " (%s%s, %db)", inst->is_signed ? "s" : "u",
                        datatype_name(inst->type), inst->size);
                break;
            case IR_COPY:
                fprintf(stderr, " "); print_vreg(inst->a);
                fprintf(stderr, ", "); print_vreg(inst->b);
                fprintf(stderr, ", %lld", inst->imm);
                break;
            case IR_OFFSET:
                fprintf(stderr, " "); print_vreg(inst->a);
                fprintf(stderr, " + %lld", inst->imm);
                break;
            case IR_INDEX:
                fprintf(stderr, " "); print_vreg(inst->a);
                fprintf(stderr, " + "); print_vreg(inst->b);
                fprintf(stderr, " * %lld", inst->imm);
                break;
            case IR_CMP:
            case IR_FCMP:
            case IR_I128_CMP:
                fprintf(stderr, ".%s ", cmp_name(inst->cmp));
                print_vreg(inst->a); fprintf(stderr, ", "); print_vreg(inst->b);
                break;
            case IR_BR:
                fprintf(stderr, " "); print_vreg(inst->a);
                fprintf(stderr, ", .L%d, .L%d", inst->label, inst->label2);
                break;
            case IR_JMP:
                fprintf(stderr, " .L%d", inst->label);
                break;
            case IR_RET:
                if (inst->a != IR_NONE) { fprintf(stderr, " "); print_vreg(inst->a); }
                break;
            case IR_CALL:
            case IR_DYN_CALL:
                fprintf(stderr, " %s(", inst->name ? inst->name : "?");
                for (int j = 0; j < inst->nargs; j++) {
                    if (j) fprintf(stderr, ", ");
                    print_vreg(f->args[inst->arg_start + j].vreg);
                }
                fprintf(stderr, ")");
                break;
            case IR_ICALL:
                fprintf(stderr, " "); print_vreg(inst->a); fprintf(stderr, "(");
                for (int j = 0; j < inst->nargs; j++) {
                    if (j) fprintf(stderr, ", ");
                    print_vreg(f->args[inst->arg_start + j].vreg);
                }
                fprintf(stderr, ")");
                break;
            default:
                if (inst->a != IR_NONE) { fprintf(stderr, " "); print_vreg(inst->a); }
                if (inst->b != IR_NONE) { fprintf(stderr, ", "); print_vreg(inst->b); }
                if (inst->imm && inst->op != IR_NOP) fprintf(stderr, ", %lld", inst->imm);
                break;
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "---\n");
}
