/*
 * opt.c - IR optimization passes
 *
 * Passes run after lowering, before backend emission. They reduce
 * the instruction count and eliminate redundant work. All transforms
 * are safe (only remove provably dead or duplicate work) and cheap
 * (linear or near-linear in instruction count).
 *
 * Pass order: const_fold -> strength_reduce -> cse_addr ->
 *             copy_prop -> dce
 */

#include "opt.h"
#include <stdlib.h>
#include <string.h>

/* 1 if the instruction has observable side effects (never remove) */
static int has_side_effect(IROp op) {
    switch (op) {
    case IR_STORE: case IR_COPY:
    case IR_CALL: case IR_ICALL: case IR_IOOUT:
    case IR_RET: case IR_BR: case IR_JMP:
    case IR_LABEL: case IR_LINE:
    case IR_I128_STORE: case IR_DYN_STORE:
    case IR_NOP:
        return 1;
    default:
        return 0;
    }
}

/* ---- constant folding ---- */

void ir_opt_const_fold(IRFunc *f) {
    int nv = f->next_vreg;
    if (nv <= 0) return;

    int *known = calloc(nv, sizeof(int));
    long long *val = calloc(nv, sizeof(long long));

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];

        if (inst->op == IR_IMM && inst->dst >= 0 && inst->dst < nv) {
            known[inst->dst] = 1;
            val[inst->dst] = inst->imm;
            continue;
        }

        int ak = (inst->a >= 0 && inst->a < nv) ? known[inst->a] : 0;
        int bk = (inst->b >= 0 && inst->b < nv) ? known[inst->b] : 0;

        /* binary constant folding */
        if (ak && bk && inst->dst != IR_NONE) {
            long long a = val[inst->a], b = val[inst->b];
            long long r; int ok = 1;
            switch (inst->op) {
            case IR_ADD:  r = a + b; break;
            case IR_SUB:  r = a - b; break;
            case IR_MUL:  r = a * b; break;
            case IR_SDIV: if (!b) { ok=0; break; } r = a / b; break;
            case IR_UDIV: if (!b) { ok=0; break; }
                r = (long long)((unsigned long long)a / (unsigned long long)b); break;
            case IR_SMOD: if (!b) { ok=0; break; } r = a % b; break;
            case IR_UMOD: if (!b) { ok=0; break; }
                r = (long long)((unsigned long long)a % (unsigned long long)b); break;
            case IR_SHL:  r = (long long)((unsigned long long)a << (b & 63)); break;
            case IR_SAR:  r = a >> (b & 63); break;
            case IR_SHR:  r = (long long)((unsigned long long)a >> (b & 63)); break;
            case IR_BAND: r = a & b; break;
            case IR_BOR:  r = a | b; break;
            case IR_BXOR: r = a ^ b; break;
            case IR_CMP:
                switch (inst->cmp) {
                case CMP_EQ:  r = (a == b); break;
                case CMP_NE:  r = (a != b); break;
                case CMP_SLT: r = (a < b); break;
                case CMP_SGT: r = (a > b); break;
                case CMP_SLE: r = (a <= b); break;
                case CMP_SGE: r = (a >= b); break;
                case CMP_ULT: r = ((unsigned long long)a < (unsigned long long)b); break;
                case CMP_UGT: r = ((unsigned long long)a > (unsigned long long)b); break;
                case CMP_ULE: r = ((unsigned long long)a <= (unsigned long long)b); break;
                case CMP_UGE: r = ((unsigned long long)a >= (unsigned long long)b); break;
                default: ok = 0; break;
                }
                break;
            default: ok = 0; break;
            }
            if (ok) {
                inst->op = IR_IMM; inst->a = IR_NONE; inst->b = IR_NONE;
                inst->imm = r;
                known[inst->dst] = 1; val[inst->dst] = r;
                continue;
            }
        }

        /* unary constant folding */
        if (ak && inst->b == IR_NONE && inst->dst != IR_NONE) {
            long long a = val[inst->a];
            long long r; int ok = 1;
            switch (inst->op) {
            case IR_NEG:  r = -a; break;
            case IR_BNOT: r = ~a; break;
            case IR_BOOL: r = (a != 0) ? 1 : 0; break;
            case IR_SEXT:
                switch (inst->size) {
                case 1: r = (long long)(signed char)a; break;
                case 2: r = (long long)(short)a; break;
                case 4: r = (long long)(int)a; break;
                default: r = a; break;
                }
                break;
            case IR_ZEXT:
                switch (inst->size) {
                case 1: r = a & 0xff; break;
                case 2: r = a & 0xffff; break;
                case 4: r = a & 0xffffffffLL; break;
                default: r = a; break;
                }
                break;
            case IR_TRUNC: r = a; break;
            default: ok = 0; break;
            }
            if (ok) {
                inst->op = IR_IMM; inst->a = IR_NONE; inst->imm = r;
                known[inst->dst] = 1; val[inst->dst] = r;
                continue;
            }
        }

        /* branch folding: constant condition -> unconditional jump */
        if (inst->op == IR_BR && ak) {
            if (val[inst->a] != 0)
                inst->op = IR_JMP;
            else {
                inst->op = IR_JMP;
                inst->label = inst->label2;
            }
            inst->a = IR_NONE;
            continue;
        }

        if (inst->dst >= 0 && inst->dst < nv)
            known[inst->dst] = 0;
    }

    free(known);
    free(val);
}

/* ---- strength reduction ---- */

void ir_opt_strength_reduce(IRFunc *f) {
    int nv = f->next_vreg;
    if (nv <= 0) return;

    int *known = calloc(nv, sizeof(int));
    long long *val = calloc(nv, sizeof(long long));

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];
        if (inst->op == IR_IMM && inst->dst >= 0 && inst->dst < nv) {
            known[inst->dst] = 1; val[inst->dst] = inst->imm;
        }
    }

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];
        int ak = (inst->a >= 0 && inst->a < nv) ? known[inst->a] : 0;
        int bk = (inst->b >= 0 && inst->b < nv) ? known[inst->b] : 0;
        long long av = ak ? val[inst->a] : 0;
        long long bv = bk ? val[inst->b] : 0;

        switch (inst->op) {
        case IR_MUL:
            if      (bk && bv == 0) { inst->op = IR_IMM; inst->a = inst->b = IR_NONE; inst->imm = 0; }
            else if (bk && bv == 1) { inst->op = IR_MOV; inst->b = IR_NONE; }
            else if (ak && av == 0) { inst->op = IR_IMM; inst->a = inst->b = IR_NONE; inst->imm = 0; }
            else if (ak && av == 1) { inst->op = IR_MOV; inst->a = inst->b; inst->b = IR_NONE; }
            break;
        case IR_ADD:
            if      (bk && bv == 0) { inst->op = IR_MOV; inst->b = IR_NONE; }
            else if (ak && av == 0) { inst->op = IR_MOV; inst->a = inst->b; inst->b = IR_NONE; }
            break;
        case IR_SUB:
            if (bk && bv == 0) { inst->op = IR_MOV; inst->b = IR_NONE; }
            break;
        case IR_BAND:
            if      (bk && bv == 0) { inst->op = IR_IMM; inst->a = inst->b = IR_NONE; inst->imm = 0; }
            else if (bk && bv == -1LL) { inst->op = IR_MOV; inst->b = IR_NONE; }
            break;
        case IR_BOR:
            if      (bk && bv == 0)    { inst->op = IR_MOV; inst->b = IR_NONE; }
            else if (bk && bv == -1LL) { inst->op = IR_IMM; inst->a = inst->b = IR_NONE; inst->imm = -1LL; }
            break;
        case IR_BXOR:
            if (bk && bv == 0) { inst->op = IR_MOV; inst->b = IR_NONE; }
            break;
        case IR_SHL: case IR_SAR: case IR_SHR:
            if (bk && bv == 0) { inst->op = IR_MOV; inst->b = IR_NONE; }
            break;
        default: break;
        }
    }

    free(known);
    free(val);
}

/* ---- basic-block CSE for address computations ---- */

typedef struct { IROp op; long long imm; const char *name; int base; int vreg; } AddrSlot;
#define CSE_CAP 256

void ir_opt_cse_addr(IRFunc *f) {
    AddrSlot slots[CSE_CAP];
    int ns = 0;

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];

        if (inst->op == IR_LABEL || inst->op == IR_BR ||
            inst->op == IR_JMP   || inst->op == IR_RET) {
            ns = 0;
            continue;
        }

        if (inst->op == IR_LOCAL_ADDR || inst->op == IR_STR) {
            for (int j = 0; j < ns; j++) {
                if (slots[j].op == inst->op && slots[j].imm == inst->imm) {
                    inst->op = IR_MOV;
                    inst->a = slots[j].vreg;
                    inst->imm = 0;
                    goto cse_next;
                }
            }
            if (ns < CSE_CAP) {
                slots[ns].op = inst->op;
                slots[ns].imm = inst->imm;
                slots[ns].name = NULL;
                slots[ns].base = IR_NONE;
                slots[ns].vreg = inst->dst;
                ns++;
            }
        } else if (inst->op == IR_GLOBAL_ADDR || inst->op == IR_FUNC_ADDR) {
            for (int j = 0; j < ns; j++) {
                if (slots[j].op == inst->op && slots[j].name && inst->name &&
                    strcmp(slots[j].name, inst->name) == 0) {
                    inst->op = IR_MOV;
                    inst->a = slots[j].vreg;
                    inst->name = NULL;
                    goto cse_next;
                }
            }
            if (ns < CSE_CAP) {
                slots[ns].op = inst->op;
                slots[ns].imm = inst->imm;
                slots[ns].name = inst->name;
                slots[ns].base = IR_NONE;
                slots[ns].vreg = inst->dst;
                ns++;
            }
        } else if (inst->op == IR_OFFSET) {
            for (int j = 0; j < ns; j++) {
                if (slots[j].op == IR_OFFSET &&
                    slots[j].base == inst->a && slots[j].imm == inst->imm) {
                    inst->op = IR_MOV;
                    inst->a = slots[j].vreg;
                    inst->imm = 0;
                    goto cse_next;
                }
            }
            if (ns < CSE_CAP) {
                slots[ns].op = IR_OFFSET;
                slots[ns].imm = inst->imm;
                slots[ns].name = NULL;
                slots[ns].base = inst->a;
                slots[ns].vreg = inst->dst;
                ns++;
            }
        }
        cse_next:;
    }
}

/* ---- copy/move propagation (local, per basic block) ---- */

void ir_opt_copy_prop(IRFunc *f) {
    int nv = f->next_vreg;
    if (nv <= 0) return;

    int *alias = malloc(nv * sizeof(int));
    for (int j = 0; j < nv; j++) alias[j] = j;

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];

        if (inst->op == IR_LABEL) {
            for (int j = 0; j < nv; j++) alias[j] = j;
            continue;
        }

        /* substitute operands a, b */
        if (inst->a >= 0 && inst->a < nv) inst->a = alias[inst->a];
        if (inst->b >= 0 && inst->b < nv) inst->b = alias[inst->b];

        /* substitute call arguments */
        for (int k = 0; k < inst->nargs; k++) {
            int idx = inst->arg_start + k;
            int v = f->args[idx].vreg;
            if (v >= 0 && v < nv) f->args[idx].vreg = alias[v];
        }

        /* record or kill alias for the destination */
        if (inst->dst >= 0 && inst->dst < nv) {
            if (inst->op == IR_MOV && inst->a != IR_NONE) {
                alias[inst->dst] = inst->a;
            } else {
                int killed = inst->dst;
                alias[killed] = killed;
                for (int j = 0; j < nv; j++)
                    if (alias[j] == killed && j != killed)
                        alias[j] = j;
            }
        }

        if (inst->op == IR_BR || inst->op == IR_JMP || inst->op == IR_RET) {
            for (int j = 0; j < nv; j++) alias[j] = j;
        }
    }

    free(alias);
}

/* ---- dead code elimination ---- */

void ir_opt_dce(IRFunc *f) {
    int nv = f->next_vreg;
    if (nv <= 0) return;

    int *uses = calloc(nv, sizeof(int));

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];
        if (inst->a >= 0 && inst->a < nv) uses[inst->a]++;
        if (inst->b >= 0 && inst->b < nv) uses[inst->b]++;
    }
    for (int i = 0; i < f->nargs; i++)
        if (f->args[i].vreg >= 0 && f->args[i].vreg < nv)
            uses[f->args[i].vreg]++;

    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < f->ninsts; i++) {
            IRInst *inst = &f->insts[i];
            if (inst->op == IR_NOP) continue;
            if (has_side_effect(inst->op)) continue;
            if (inst->dst == IR_NONE) continue;
            if (inst->dst < 0 || inst->dst >= nv) continue;
            if (uses[inst->dst] > 0) continue;

            if (inst->a >= 0 && inst->a < nv) uses[inst->a]--;
            if (inst->b >= 0 && inst->b < nv) uses[inst->b]--;
            inst->op = IR_NOP;
            inst->imm = 0;
            changed = 1;
        }
    }

    free(uses);
}

/* ---- store forwarding (local variables) ---- */

#define SF_MAX 256

void ir_opt_store_forward(IRFunc *f) {
    int nv = f->next_vreg;
    if (nv <= 0) return;

    int *is_la = calloc(nv, sizeof(int));
    long long *la_off = calloc(nv, sizeof(long long));

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];
        if (inst->op == IR_LOCAL_ADDR && inst->dst >= 0 && inst->dst < nv) {
            is_la[inst->dst] = 1;
            la_off[inst->dst] = inst->imm;
        }
    }

    /* store cache: local frame offset -> last stored value vreg */
    int sf_off[SF_MAX], sf_val[SF_MAX], sf_n = 0;
    /* load cache: address vreg -> loaded value vreg (any address, not just locals) */
    int lc_addr[SF_MAX], lc_val[SF_MAX], lc_n = 0;

    for (int i = 0; i < f->ninsts; i++) {
        IRInst *inst = &f->insts[i];

        if (inst->op == IR_LABEL || inst->op == IR_BR ||
            inst->op == IR_JMP   || inst->op == IR_RET ||
            inst->op == IR_CALL  || inst->op == IR_ICALL ||
            inst->op == IR_IOOUT) {
            sf_n = 0; lc_n = 0;
            continue;
        }

        if (inst->op == IR_STORE) {
            /* invalidate load cache for this address and any alias */
            lc_n = 0;
            /* f32 stores narrow f64->f32 in the backend, so the stored
             * value differs from the vreg value; skip caching them */
            if (inst->type == TYPE_F32 && inst->ptr == 0) {
                if (inst->a >= 0 && inst->a < nv && is_la[inst->a]) {
                    int off = (int)la_off[inst->a];
                    for (int j = 0; j < sf_n; j++)
                        if (sf_off[j] == off) {
                            sf_off[j] = sf_off[sf_n - 1];
                            sf_val[j] = sf_val[sf_n - 1];
                            sf_n--; break;
                        }
                } else {
                    sf_n = 0;
                }
                continue;
            }
            if (inst->a >= 0 && inst->a < nv && is_la[inst->a]) {
                int off = (int)la_off[inst->a];
                int found = -1;
                for (int j = 0; j < sf_n; j++)
                    if (sf_off[j] == off) { found = j; break; }
                if (found >= 0)
                    sf_val[found] = inst->b;
                else if (sf_n < SF_MAX) {
                    sf_off[sf_n] = off; sf_val[sf_n] = inst->b; sf_n++;
                }
            } else {
                sf_n = 0;
            }
            continue;
        }

        if (inst->op == IR_COPY) {
            lc_n = 0;
            if (inst->a >= 0 && inst->a < nv && is_la[inst->a]) {
                int off = (int)la_off[inst->a];
                for (int j = 0; j < sf_n; j++)
                    if (sf_off[j] == off) {
                        sf_off[j] = sf_off[sf_n - 1];
                        sf_val[j] = sf_val[sf_n - 1];
                        sf_n--; break;
                    }
            } else {
                sf_n = 0;
            }
            continue;
        }

        if (inst->op == IR_LOAD && inst->a >= 0 && inst->a < nv) {
            int forwarded = 0;
            /* try store forwarding for locals */
            if (is_la[inst->a]) {
                int off = (int)la_off[inst->a];
                for (int j = 0; j < sf_n; j++) {
                    if (sf_off[j] == off && sf_val[j] >= 0 && sf_val[j] < nv) {
                        inst->op = IR_MOV;
                        inst->a = sf_val[j];
                        inst->size = 0; inst->is_signed = 0;
                        forwarded = 1; break;
                    }
                }
            }
            /* try load-after-load forwarding for any address */
            if (!forwarded) {
                for (int j = 0; j < lc_n; j++) {
                    if (lc_addr[j] == inst->a && lc_val[j] >= 0 && lc_val[j] < nv) {
                        inst->op = IR_MOV;
                        inst->a = lc_val[j];
                        inst->size = 0; inst->is_signed = 0;
                        forwarded = 1; break;
                    }
                }
            }
            /* cache this load result */
            if (!forwarded && inst->dst >= 0 && inst->dst < nv) {
                /* also update store cache for locals */
                if (is_la[inst->a]) {
                    int off = (int)la_off[inst->a];
                    int found = -1;
                    for (int j = 0; j < sf_n; j++)
                        if (sf_off[j] == off) { found = j; break; }
                    if (found >= 0) sf_val[found] = inst->dst;
                    else if (sf_n < SF_MAX) {
                        sf_off[sf_n] = off; sf_val[sf_n] = inst->dst; sf_n++;
                    }
                }
                /* add to load cache */
                if (lc_n < SF_MAX) {
                    lc_addr[lc_n] = inst->a; lc_val[lc_n] = inst->dst; lc_n++;
                }
            }
        }
    }

    free(is_la);
    free(la_off);
}

/* ---- entry point ---- */

void ir_optimize(IRFunc *f) {
    if (!f || f->ninsts <= 0) return;
    for (int pass = 0; pass < 2; pass++) {
        ir_opt_const_fold(f);
        ir_opt_strength_reduce(f);
        ir_opt_cse_addr(f);
        ir_opt_copy_prop(f);
        ir_opt_store_forward(f);
        ir_opt_copy_prop(f);
        ir_opt_dce(f);
    }
}
