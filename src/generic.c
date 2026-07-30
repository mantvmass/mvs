/*
 * generic.c - monomorphization of generic functions (see generic.h for the overview)
 *
 * Algorithm:
 *   1. Collect the generic templates (ngen > 0) from the program
 *   2. Scan every concrete function for calls to generics
 *   3. Infer the concrete types from the arguments -> create an instance (clone + substitute types) if missing
 *   4. Rename the call site to point at that instance
 *   5. New instances get scanned too (supports generics calling generics) until nothing new appears
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generic.h"

/* Inferred concrete type (base + pointer depth + struct name + signature if it is a function pointer) */
typedef struct { DataType base; int ptr; char *sname; Node *sig; } CType;

/* Mapping entry, name -> type (used as the per-function var-type map and the generic-param map) */
typedef struct { char *name; CType t; } Bind;

/* ---------- program lookup helpers ---------- */

/* Check whether a type (struct name) impls the given trait (an ND_TRAIT_IMPL marker exists) */
static int type_impls_trait(Node *prog, const char *sname, const char *trait) {
    if (!sname || !trait) return 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_TRAIT_IMPL && d->name && d->type_name &&
            strcmp(d->name, trait) == 0 && strcmp(d->type_name, sname) == 0) return 1;
    }
    return 0;
}

static int mono_err; /* number of trait bound violation errors (reset in monomorphize) */

/* Find a trait declaration by name */
static Node *find_trait(Node *prog, const char *name) {
    for (int i = 0; i < prog->nitems; i++)
        if (prog->items[i]->kind == ND_TRAIT && strcmp(prog->items[i]->name, name) == 0) return prog->items[i];
    return NULL;
}

/* Does a function (method/associated) named mname exist in namespace = type */
static int type_has_method(Node *prog, const char *type, const char *mname) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->ns && strcmp(f->ns, type) == 0 && strcmp(f->name, mname) == 0) return 1;
    }
    return 0;
}

/* Check every `impl Trait for Type`: the trait must exist and Type must have every trait method */
static void check_trait_impls(Node *prog) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind != ND_TRAIT_IMPL) continue;
        Node *tr = find_trait(prog, d->name);
        if (!tr) {
            fprintf(stderr, "type error: unknown trait '%s' in impl for '%s'\n", d->name, d->type_name);
            mono_err++; continue;
        }
        for (int j = 0; j < tr->nitems; j++) {
            Node *sig = tr->items[j];
            if (sig->kind != ND_FUNC) continue;
            if (!type_has_method(prog, d->type_name, sig->name)) {
                fprintf(stderr, "type error: type '%s' is missing method '%s' required by trait '%s'\n",
                        d->type_name, sig->name, d->name);
                mono_err++;
            }
        }
    }
}

/* Find a generic template by name (ngen > 0) */
static Node *find_template(Node *prog, const char *name) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_FUNC && d->ngen > 0 && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* Find an ND_FUNC by name (generic or not) and return its return type */
static int func_ret_type(Node *prog, const char *name, CType *out) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_FUNC && strcmp(d->name, name) == 0) {
            out->base = d->type; out->ptr = d->ptr; out->sname = d->type_name; out->sig = NULL; return 1;
        }
    }
    return 0;
}

/* Find the type of a struct field (used to infer a.b) */
static int struct_field_type(Node *prog, const char *sname, const char *field, CType *out) {
    if (!sname) return 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_STRUCT_DECL && strcmp(d->name, sname) == 0) {
            for (int j = 0; j < d->nitems; j++) {
                Node *f = d->items[j];
                if (strcmp(f->name, field) == 0) {
                    out->base = f->type; out->ptr = f->ptr; out->sname = f->type_name; out->sig = f->sig; return 1;
                }
            }
        }
    }
    return 0;
}

/* Width rank of an integer type (bytes); used to widen arithmetic result types order-independently */
static int int_rank(DataType t) {
    switch (t) {
        case TYPE_I8: case TYPE_U8: case TYPE_BOOL: case TYPE_CHAR: return 1;
        case TYPE_I16: case TYPE_U16: return 2;
        case TYPE_I32: case TYPE_U32: return 4;
        case TYPE_I64: case TYPE_U64: case TYPE_ISIZE: case TYPE_USIZE: return 8;
        case TYPE_I128: case TYPE_U128: return 16;
        default: return 8;
    }
}

/* ---------- expression type inference (uses the var-type map built from declared types) ---------- */

static CType infer(Node *prog, Node *n, Bind *map, int nmap) {
    CType r = { TYPE_I64, 0, NULL, NULL };
    if (!n) return r;
    switch (n->kind) {
        case ND_INT: r.base = TYPE_I64; break;
        case ND_FLOAT: r.base = TYPE_F64; break;
        case ND_CHAR: r.base = TYPE_CHAR; break;
        case ND_BOOL: r.base = TYPE_BOOL; break;
        case ND_STR: r.base = TYPE_STR; break;
        case ND_STRUCT_LIT: r.base = TYPE_STRUCT; r.sname = n->name; break;
        case ND_IDENT:
            for (int i = nmap - 1; i >= 0; i--)
                if (strcmp(map[i].name, n->name) == 0) return map[i].t;
            /* not a variable: may be a "function name used as a value" -> function pointer (sig = func node) */
            for (int i = 0; i < prog->nitems; i++) {
                Node *d = prog->items[i];
                if (d->kind == ND_FUNC && !d->is_method && d->ngen == 0 && d->name && strcmp(d->name, n->name) == 0) {
                    r.base = TYPE_FUNC; r.sig = d; return r;
                }
            }
            break;
        case ND_MEMBER: {
            CType bt = infer(prog, n->operand, map, nmap);
            CType ft; if (struct_field_type(prog, bt.sname, n->name, &ft)) return ft;
            break;
        }
        case ND_UNARY:
            if (n->op == TK_AMP)       { r = infer(prog, n->operand, map, nmap); r.ptr++; }
            else if (n->op == TK_STAR) { r = infer(prog, n->operand, map, nmap); if (r.ptr > 0) r.ptr--; }
            else                       { r = infer(prog, n->operand, map, nmap); }
            break;
        case ND_BINARY:
            if (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||n->op==TK_LE||n->op==TK_GE||
                n->op==TK_AND||n->op==TK_OR) { r.base = TYPE_BOOL; break; }
            else {
                CType lt = infer(prog, n->lhs, map, nmap), rt = infer(prog, n->rhs, map, nmap);
                if (n->op==TK_PLUS || n->op==TK_MINUS) {
                    if (lt.ptr>0 && rt.ptr>0) { r.base=TYPE_ISIZE; break; }  /* ptr-ptr -> count */
                    if (lt.ptr>0) { r=lt; break; }
                    if (rt.ptr>0) { r=rt; break; }                          /* int+ptr -> ptr */
                }
                int lf = (lt.base==TYPE_F32||lt.base==TYPE_F64)&&lt.ptr==0;
                int rf = (rt.base==TYPE_F32||rt.base==TYPE_F64)&&rt.ptr==0;
                if (lf || rf) { r.base = (lt.base==TYPE_F64||rt.base==TYPE_F64)?TYPE_F64:TYPE_F32; break; }
                /* int-int: widen to the wider type (order-independent) so overload choice matches either order */
                r = (int_rank(rt.base) > int_rank(lt.base)) ? rt : lt;
            }
            break;
        case ND_ASSIGN: r = infer(prog, n->lhs, map, nmap); break;
        case ND_CAST: r.base = n->type; r.ptr = n->ptr; r.sname = n->type_name; break;
        case ND_CALL: {
            Node *callee = n->operand;
            if (!callee) break;
            /* Call through a function pointer (indirect): the callee is a TYPE_FUNC value
             * (a variable/parameter or a struct field) -> result type = the signature's return type */
            {
                CType cc = infer(prog, callee, map, nmap);
                if (cc.base == TYPE_FUNC && cc.sig) {
                    CType ct = { cc.sig->type, cc.sig->ptr, cc.sig->type_name, NULL };
                    return ct;
                }
            }
            if (callee->kind == ND_IDENT) {              /* direct call f(...) */
                CType ct; if (func_ret_type(prog, callee->name, &ct)) return ct;
            } else if (callee->kind == ND_MEMBER) {      /* ns.f(...) or obj.method(...) */
                /* Find the "scope": if the base is a struct, use the struct name (method);
                 * if the base is a bare name, use it as a namespace (e.g. net) so functions with a
                 * matching ns win first (avoids clashes with extern C, e.g. method accept vs Winsock accept) */
                const char *scope = NULL;
                CType bt = infer(prog, callee->operand, map, nmap);
                if (bt.base == TYPE_STRUCT && bt.sname) scope = bt.sname;
                else if (callee->operand->kind == ND_IDENT) scope = callee->operand->name;
                Node *best = NULL;
                for (int i = 0; i < prog->nitems; i++) {
                    Node *f = prog->items[i];
                    if (f->kind != ND_FUNC || strcmp(f->name, callee->name) != 0) continue;
                    if (scope && f->ns && strcmp(f->ns, scope) == 0) { best = f; break; }
                    if (!best) best = f;
                }
                if (best) { CType ct = { best->type, best->ptr, best->type_name, NULL }; return ct; }
            }
            break;
        }
        default: break;
    }
    return r;
}

/* Add a variable/parameter to the var-type map (scope stack); visible after its declaration point.
 * Shared by monomorphize/overload/typecheck so type inference stays correct under shadowing */
static void add_bind(Bind *map, int *nmap, Node *d) {
    if (*nmap >= 512 || !d->name) return;
    map[*nmap].name = d->name;
    map[*nmap].t.base = d->type; map[*nmap].t.ptr = d->ptr; map[*nmap].t.sname = d->type_name; map[*nmap].t.sig = d->sig;
    (*nmap)++;
}

/* Seed global variables (top-level let/const) into the base scope so global types infer correctly.
 * (Must be called before adding params; params/locals then shadow them correctly) */
static void seed_globals(Node *prog, Bind *map, int *nmap) {
    for (int i = 0; i < prog->nitems; i++)
        if (prog->items[i]->kind == ND_VAR_DECL) add_bind(map, nmap, prog->items[i]);
}

/* ---------- type substitution and instance naming ---------- */

/* Write the type code used for instance names (e.g. i32, pi32, Foo) */
static void type_code(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = 'p';
    const char *base = t.base == TYPE_STRUCT && t.sname ? t.sname : datatype_name(t.base);
    strcpy(p, base);
}

/* Substitute the generic type parameters with concrete types in every node of the instance */
static void substitute(Node *n, Bind *gmap, int ngmap) {
    if (!n) return;
    if (n->type == TYPE_STRUCT && n->type_name) {
        for (int i = 0; i < ngmap; i++) {
            if (strcmp(n->type_name, gmap[i].name) == 0) {
                n->type = gmap[i].t.base;
                n->ptr += gmap[i].t.ptr;                 /* *T + (T=*i32) -> **i32 */
                free(n->type_name);
                n->type_name = gmap[i].t.sname ? strdup(gmap[i].t.sname) : NULL;
                /* If T is bound to a "function value" (TYPE_FUNC) the signature must come along,
                 * or calling f(...) fails. Clone to avoid mutating shared structure
                 * (a real function's signature has no generic params anyway) */
                if (n->type == TYPE_FUNC) n->sig = gmap[i].t.sig ? node_clone(gmap[i].t.sig) : NULL;
                break;
            }
        }
    }
    substitute(n->lhs, gmap, ngmap);   substitute(n->rhs, gmap, ngmap);
    substitute(n->operand, gmap, ngmap); substitute(n->cond, gmap, ngmap);
    substitute(n->then_branch, gmap, ngmap); substitute(n->else_branch, gmap, ngmap);
    substitute(n->init, gmap, ngmap);  substitute(n->step, gmap, ngmap);
    substitute(n->body, gmap, ngmap);
    substitute(n->sig, gmap, ngmap);   /* generic params inside func-ptr signatures, e.g. f: func(T) -> T */
    for (int i = 0; i < n->nitems; i++) substitute(n->items[i], gmap, ngmap);
}

/* ---------- main driver ---------- */

/* Scan nodes in a concrete function for generic calls, then instantiate + rename the call sites.
 * Scope-aware (map/nmap grows with declarations) so argument types infer correctly under shadowing.
 * Returns the number of new instances created (used for the fixpoint check) */
static int scan_calls(Node *prog, Node *n, Bind *map, int *nmap, int *made) {
    if (!n) return *made;
    switch (n->kind) {                          /* handle scopes first */
        case ND_BLOCK: { int s = *nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        case ND_VAR_DECL: { scan_calls(prog,n->operand,map,nmap,made); add_bind(map,nmap,n); return *made; }
        case ND_FOR: { int s=*nmap; scan_calls(prog,n->init,map,nmap,made); scan_calls(prog,n->cond,map,nmap,made); scan_calls(prog,n->step,map,nmap,made); scan_calls(prog,n->body,map,nmap,made); *nmap=s; return *made; }
        case ND_IF: { scan_calls(prog,n->cond,map,nmap,made); int s1=*nmap; scan_calls(prog,n->then_branch,map,nmap,made); *nmap=s1; int s2=*nmap; scan_calls(prog,n->else_branch,map,nmap,made); *nmap=s2; return *made; }
        case ND_WHILE: case ND_DOWHILE: { scan_calls(prog,n->cond,map,nmap,made); int s=*nmap; scan_calls(prog,n->body,map,nmap,made); *nmap=s; return *made; }
        case ND_SWITCH: { scan_calls(prog,n->cond,map,nmap,made); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        case ND_CASE: { scan_calls(prog,n->operand,map,nmap,made); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        default: break;
    }
    /* Walk children first; matters for nested generics such as f(g(x)): the inner call must be
     * instantiated+renamed to a concrete instance before the outer call infers its argument type */
    scan_calls(prog, n->lhs, map, nmap, made);   scan_calls(prog, n->rhs, map, nmap, made);
    scan_calls(prog, n->operand, map, nmap, made); scan_calls(prog, n->cond, map, nmap, made);
    scan_calls(prog, n->then_branch, map, nmap, made); scan_calls(prog, n->else_branch, map, nmap, made);
    scan_calls(prog, n->init, map, nmap, made);  scan_calls(prog, n->step, map, nmap, made);
    scan_calls(prog, n->body, map, nmap, made);
    for (int i = 0; i < n->nitems; i++) scan_calls(prog, n->items[i], map, nmap, made);

    /* Generic calls: both f(...) (ND_IDENT) and ns.f(...) (ND_MEMBER whose base is a namespace, not a struct)
     * - lets generic helpers in std be called as module.func (checking the base is not a struct avoids methods) */
    int is_gcall = 0;
    if (n->kind == ND_CALL && n->operand) {
        if (n->operand->kind == ND_IDENT) is_gcall = 1;
        else if (n->operand->kind == ND_MEMBER) {
            CType bt = infer(prog, n->operand->operand, map, *nmap);
            if (bt.base != TYPE_STRUCT) is_gcall = 1; /* ns.f(...), not obj.method() */
        }
    }
    if (is_gcall) {
        Node *tmpl = find_template(prog, n->operand->name);
        if (tmpl) {
            /* Infer each generic parameter's type from the arguments */
            Bind gmap[4];
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                CType concrete = { TYPE_I64, 0, NULL, NULL };
                /* Find the first parameter whose type refers to this generic param */
                for (int pi = 0; pi < tmpl->nitems && pi < n->nitems; pi++) {
                    Node *pp = tmpl->items[pi];
                    if (pp->type == TYPE_STRUCT && pp->type_name &&
                        strcmp(pp->type_name, tmpl->gen[gi]) == 0) {
                        CType at = infer(prog, n->items[pi], map, *nmap);
                        concrete.base = at.base; concrete.sname = at.sname; concrete.sig = at.sig;
                        concrete.ptr = at.ptr - pp->ptr; if (concrete.ptr < 0) concrete.ptr = 0;
                        break;
                    }
                }
                gmap[gi].name = tmpl->gen[gi]; gmap[gi].t = concrete;
            }
            /* Check trait bounds: with <T: Trait> the concrete type must impl that trait (a marked struct) */
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                if (!tmpl->gen_bound[gi]) continue;
                CType ct = gmap[gi].t;
                if (!(ct.base == TYPE_STRUCT && ct.sname && type_impls_trait(prog, ct.sname, tmpl->gen_bound[gi]))) {
                    const char *tn = (ct.base == TYPE_STRUCT && ct.sname) ? ct.sname : datatype_name(ct.base);
                    fprintf(stderr, "type error: type '%s' does not implement trait '%s' (required by %s<%s: %s>)\n",
                            tn, tmpl->gen_bound[gi], tmpl->name, tmpl->gen[gi], tmpl->gen_bound[gi]);
                    mono_err++;
                }
            }
            /* Build the instance name from the type codes */
            char mangled[600]; size_t ml = 0;   /* instance name (template name + type codes), bounded */
            ml = snprintf(mangled, sizeof(mangled), "%s", tmpl->name);
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                char code[128]; type_code(gmap[gi].t, code);
                ml += snprintf(mangled + ml, sizeof(mangled) - ml, "__%s", code);
                if (ml >= sizeof(mangled)) { ml = sizeof(mangled) - 1; break; }
            }
            /* Create the instance if missing (dedup by searching the program for the mangled name) */
            {
                int exists = 0;
                for (int i = 0; i < prog->nitems; i++)
                    if (prog->items[i]->kind == ND_FUNC && strcmp(prog->items[i]->name, mangled) == 0) { exists = 1; break; }
                if (!exists) {
                    Node *inst = node_clone(tmpl);
                    free(inst->name); inst->name = strdup(mangled);
                    inst->ngen = 0;                 /* now concrete */
                    substitute(inst, gmap, tmpl->ngen);
                    node_add_item(prog, inst);
                    (*made)++;
                }
            }
            /* Rewrite the call site to point at the instance */
            free(n->operand->name); n->operand->name = strdup(mangled);
        }
    }
    return *made;
}

/* ---------- overload resolution (same name, different parameter types) ---------- */

/* "Category" code of a type (int/float/str/char/bool/pointer/struct), used as a fallback when matching.
 * Pointers get a 'p' prefix per depth, then the pointee's category (keeps *i32 and *Point apart) */
static void cat_code(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = 'p';
    switch (t.base) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
            strcpy(p, "i"); break;
        case TYPE_F32: case TYPE_F64: strcpy(p, "f"); break;
        case TYPE_STR:  strcpy(p, "s"); break;
        case TYPE_CHAR: strcpy(p, "c"); break;
        case TYPE_BOOL: strcpy(p, "b"); break;
        case TYPE_STRUCT: strcpy(p, t.sname ? t.sname : "S"); break;
        default: strcpy(p, "x");
    }
}

/* "Exact" type code: distinguishes int/float widths and "depth + pointee type"; used for exact matching.
 * e.g. *i32 -> "pi32", *u8 -> "pu8", **i32 -> "ppi32" (avoids clashes/false duplicates of pointer overloads) */
static void width_code(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = 'p';
    switch (t.base) {
        case TYPE_STR:    strcpy(p, "s"); break;
        case TYPE_CHAR:   strcpy(p, "c"); break;
        case TYPE_BOOL:   strcpy(p, "b"); break;
        case TYPE_STRUCT: strcpy(p, t.sname ? t.sname : "S"); break;
        default:          strcpy(p, datatype_name(t.base)); /* i8..i64, u8..u64, isize/usize, f32, f64 */
    }
}

#define SIGCAP 256  /* signature buffer size (guards against overflow with many params/struct names) */

/* Append a type code to buf with bounds checking (cap=SIGCAP); updates the length in place */
static void sig_append(char *buf, size_t *len, int first, const char *code) {
    size_t add = (first ? 0 : 1) + strlen(code);
    if (*len + add + 1 >= SIGCAP) return;   /* full: stop (no overflow) */
    if (!first) buf[(*len)++] = '_';
    strcpy(buf + *len, code); *len += strlen(code);
}

/* Build a function's signature from its parameter types (exact=1 -> width_code, 0 -> cat_code) */
static void sig_func(Node *fn, char *buf, int exact) {
    buf[0] = '\0';
    if (fn->nitems == 0) { strcpy(buf, "void"); return; }
    size_t len = 0;
    for (int i = 0; i < fn->nitems; i++) {
        CType t = { fn->items[i]->type, fn->items[i]->ptr, fn->items[i]->type_name, NULL };
        char c[128]; if (exact) width_code(t, c); else cat_code(t, c);
        sig_append(buf, &len, i == 0, c);
    }
}

/* Build a call site's signature from the argument types (inferred via the var-type map) */
static void sig_call(Node *prog, Node *call, Bind *map, int nmap, char *buf, int exact) {
    buf[0] = '\0';
    if (call->nitems == 0) { strcpy(buf, "void"); return; }
    size_t len = 0;
    for (int i = 0; i < call->nitems; i++) {
        CType t = infer(prog, call->items[i], map, nmap);
        char c[128]; if (exact) width_code(t, c); else cat_code(t, c);
        sig_append(buf, &len, i == 0, c);
    }
}

/* Group of functions sharing a name (overload set): sig = exact, catsig = category (fallback) */
typedef struct { char *orig; Node *defs[16]; char sig[16][SIGCAP]; char catsig[16][SIGCAP]; int n; } OvSet;

/* Is this function eligible for overloading (a plain global function) */
static int ov_eligible(Node *f) {
    return f->kind == ND_FUNC && f->body && f->ngen == 0 &&
           !f->is_export && !f->is_method && !f->is_extern &&
           (f->ns == NULL || f->ns[0] == 0) && strcmp(f->name, "main") != 0;
}

/* Scan for calls to overloaded functions and rename them to match the argument signature (scope-aware) */
static void scan_ov(Node *prog, Node *n, Bind *map, int *nmap, OvSet *sets, int nsets) {
    if (!n) return;
    switch (n->kind) {                          /* handle scopes first (supports shadowing) */
        case ND_BLOCK: { int s=*nmap; for (int i=0;i<n->nitems;i++) scan_ov(prog,n->items[i],map,nmap,sets,nsets); *nmap=s; return; }
        case ND_VAR_DECL: { scan_ov(prog,n->operand,map,nmap,sets,nsets); add_bind(map,nmap,n); return; }
        case ND_FOR: { int s=*nmap; scan_ov(prog,n->init,map,nmap,sets,nsets); scan_ov(prog,n->cond,map,nmap,sets,nsets); scan_ov(prog,n->step,map,nmap,sets,nsets); scan_ov(prog,n->body,map,nmap,sets,nsets); *nmap=s; return; }
        case ND_IF: { scan_ov(prog,n->cond,map,nmap,sets,nsets); int s1=*nmap; scan_ov(prog,n->then_branch,map,nmap,sets,nsets); *nmap=s1; int s2=*nmap; scan_ov(prog,n->else_branch,map,nmap,sets,nsets); *nmap=s2; return; }
        case ND_WHILE: case ND_DOWHILE: { scan_ov(prog,n->cond,map,nmap,sets,nsets); int s=*nmap; scan_ov(prog,n->body,map,nmap,sets,nsets); *nmap=s; return; }
        case ND_SWITCH: { scan_ov(prog,n->cond,map,nmap,sets,nsets); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_ov(prog,n->items[i],map,nmap,sets,nsets); *nmap=s; return; }
        case ND_CASE: { scan_ov(prog,n->operand,map,nmap,sets,nsets); int s=*nmap; for (int i=0;i<n->nitems;i++) scan_ov(prog,n->items[i],map,nmap,sets,nsets); *nmap=s; return; }
        default: break;
    }
    /* Resolve children first (nested calls in arguments) so argument types that are themselves
     * overloaded calls infer correctly, e.g. outer(inner(x)) must resolve inner first */
    scan_ov(prog, n->lhs, map, nmap, sets, nsets);   scan_ov(prog, n->rhs, map, nmap, sets, nsets);
    scan_ov(prog, n->operand, map, nmap, sets, nsets); scan_ov(prog, n->cond, map, nmap, sets, nsets);
    scan_ov(prog, n->then_branch, map, nmap, sets, nsets); scan_ov(prog, n->else_branch, map, nmap, sets, nsets);
    scan_ov(prog, n->init, map, nmap, sets, nsets);  scan_ov(prog, n->step, map, nmap, sets, nsets);
    scan_ov(prog, n->body, map, nmap, sets, nsets);
    for (int i = 0; i < n->nitems; i++) scan_ov(prog, n->items[i], map, nmap, sets, nsets);

    if (n->kind == ND_CALL && n->operand && n->operand->kind == ND_IDENT) {
        for (int s = 0; s < nsets; s++) {
            if (sets[s].n < 2 || strcmp(sets[s].orig, n->operand->name) != 0) continue;
            char wx[SIGCAP], wc[SIGCAP];
            sig_call(prog, n, map, *nmap, wx, 1);   /* exact signature */
            sig_call(prog, n, map, *nmap, wc, 0);   /* category signature (fallback) */
            int found = -1;
            /* 1) exact width match first (e.g. i32 matches i32) */
            for (int d = 0; d < sets[s].n; d++)
                if (strcmp(sets[s].sig[d], wx) == 0) { found = d; break; }
            /* 2) if not found, use the category; must match exactly one (e.g. literal i64 -> the sole show(i32)) */
            if (found < 0) {
                int cnt = 0, cd = -1;
                for (int d = 0; d < sets[s].n; d++)
                    if (strcmp(sets[s].catsig[d], wc) == 0) { cnt++; cd = d; }
                if (cnt == 1) found = cd;
                else if (cnt > 1) {
                    fprintf(stderr, "codegen error: ambiguous call to '%s' with argument types (%s): multiple width overloads match; use 'as' to disambiguate\n", sets[s].orig, wx);
                    break;
                }
            }
            if (found >= 0) { free(n->operand->name); n->operand->name = strdup(sets[s].defs[found]->name); }
            else fprintf(stderr, "codegen error: no overload of '%s' matches argument types (%s)\n", sets[s].orig, wx);
            break;
        }
    }
}

/* Compare namespaces (NULL counts as "") */
static int ns_eq(const char *a, const char *b) {
    if (!a) a = ""; if (!b) b = "";
    return strcmp(a, b) == 0;
}

/* Check top-level duplicates: repeated struct/trait names, and functions with the same ns+name+exact
 * parameter types. Overloads (same name, different types) are not duplicates; externs may repeat
 * (they are declarations, deduped later). Called before monomorphize to catch errors early. Returns the count */
int check_duplicates(Node *prog) {
    int errc = 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *a = prog->items[i];
        if (a->kind != ND_STRUCT_DECL && a->kind != ND_TRAIT && a->kind != ND_FUNC) continue;
        for (int j = 0; j < i; j++) {
            Node *b = prog->items[j];
            if (a->kind != b->kind || !a->name || !b->name || strcmp(a->name, b->name) != 0) continue;
            if (a->kind == ND_STRUCT_DECL) {
                fprintf(stderr, "error: duplicate struct '%s'\n", a->name); errc++;
            } else if (a->kind == ND_TRAIT) {
                fprintf(stderr, "error: duplicate trait '%s'\n", a->name); errc++;
            } else { /* ND_FUNC */
                if (a->is_extern || b->is_extern || a->ngen > 0 || b->ngen > 0) continue;
                if (!ns_eq(a->ns, b->ns)) continue;
                char sa[SIGCAP], sb[SIGCAP]; sig_func(a, sa, 1); sig_func(b, sb, 1);
                if (strcmp(sa, sb) != 0) continue; /* different signature = overload, not a duplicate */
                if (a->ns && a->ns[0])
                    fprintf(stderr, "error: duplicate function '%s.%s' with the same parameter types (%s)\n", a->ns, a->name, sa);
                else
                    fprintf(stderr, "error: duplicate function '%s' with the same parameter types (%s)\n", a->name, sa);
                errc++;
            }
        }
    }
    return errc;
}

void resolve_overloads(Node *prog) {
    static OvSet sets[128]; int nsets = 0;
    /* 1) group functions by their original name */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (!ov_eligible(f)) continue;
        int si = -1;
        for (int s = 0; s < nsets; s++) if (strcmp(sets[s].orig, f->name) == 0) { si = s; break; }
        if (si < 0) {
            if (nsets >= 128) { fprintf(stderr, "codegen error: too many distinct function names for overloading\n"); break; }
            si = nsets++; sets[si].orig = strdup(f->name); sets[si].n = 0;
        }
        if (sets[si].n < 16) {
            sets[si].defs[sets[si].n] = f;
            sig_func(f, sets[si].sig[sets[si].n], 1);     /* exact */
            sig_func(f, sets[si].catsig[sets[si].n], 0);  /* category */
            sets[si].n++;
        }
        else fprintf(stderr, "codegen error: too many overloads of '%s' (max 16)\n", f->name);
    }
    /* 2) rename members of every group with more than 1 entry (overloaded) using the exact signature
     *    (identical-signature redefinitions were already caught by check_duplicates before monomorphize) */
    for (int s = 0; s < nsets; s++) {
        if (sets[s].n < 2) continue;
        for (int d = 0; d < sets[s].n; d++) {
            char mangled[600]; snprintf(mangled, sizeof(mangled), "%s__%s", sets[s].orig, sets[s].sig[d]);
            free(sets[s].defs[d]->name); sets[s].defs[d]->name = strdup(mangled);
        }
    }
    /* 3) resolve call sites everywhere (every concrete function + global initializers)
     *    skip generic templates (ngen>0): their types are unresolved; their instances are scanned separately */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->body && f->ngen == 0) {
            Bind map[512]; int nmap = 0;
            seed_globals(prog, map, &nmap);
            for (int j = 0; j < f->nitems; j++) if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
            scan_ov(prog, f->body, map, &nmap, sets, nsets);
        } else if (f->kind == ND_VAR_DECL && f->operand) {
            Bind map[512]; int nmap = 0;   /* use a real map (avoids NULL deref if the init has a block/decl) */
            seed_globals(prog, map, &nmap);
            scan_ov(prog, f->operand, map, &nmap, sets, nsets);
        }
    }
}

/* ---------- compile-time type checker ----------
 *
 * Purpose: catch type nonsense at compile time (instead of letting it break at runtime), e.g.
 *   50 + "50"           -> error (cannot add a number and a string)
 *   let x: u8 = "hi"    -> error (cannot assign a string to a numeric variable)
 * Designed to be "strict only where clearly wrong" to avoid false positives on valid low-level code
 * (e.g. pointer +/- int, comparing a pointer with 0 for a null check, mixing int widths) */

static int tc_numeric(CType t) {            /* numeric (int/float/char/bool), not a pointer */
    if (t.ptr) return 0;
    switch (t.base) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
        case TYPE_F32: case TYPE_F64: case TYPE_CHAR: case TYPE_BOOL: return 1;
        default: return 0;
    }
}
static int tc_integer(CType t) {            /* integer/char/bool (used for bitwise/shift) */
    if (t.ptr) return 0;
    switch (t.base) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_U128: case TYPE_USIZE:
        case TYPE_CHAR: case TYPE_BOOL: return 1;
        default: return 0;
    }
}
static int tc_isptr(CType t)    { return t.ptr > 0; }
static int tc_isstr(CType t)    { return t.ptr == 0 && t.base == TYPE_STR; }
static int tc_isstruct(CType t) { return t.ptr == 0 && t.base == TYPE_STRUCT; }
static int tc_isvoid(CType t)   { return t.ptr == 0 && t.base == TYPE_VOID; }

/* Write a readable type name into buf (e.g. "u8", "*i32", "str", "Point") */
static void tc_name(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = '*';
    const char *b = (t.base == TYPE_STRUCT && t.sname) ? t.sname : datatype_name(t.base);
    strcpy(p, b);
}

/* Check whether a value (s) can be assigned to a target (d) */
static int tc_assignable(CType d, CType s) {
    if (tc_isstruct(d)) return tc_isstruct(s) && d.sname && s.sname && strcmp(d.sname, s.sname) == 0;
    if (tc_isstruct(s)) return 0;                       /* a struct value only fits the same struct type */
    if (tc_isptr(d) || tc_isstr(d))                     /* pointer/str target: accepts pointer/str/integer (address/null) */
        return tc_isptr(s) || tc_isstr(s) || tc_integer(s);
    if (tc_numeric(d)) return tc_numeric(s);            /* numeric target: no str/pointer/struct */
    return 1;                                           /* void etc.: not strict */
}

/* Context of the function being checked. map/nmap is a scope stack that grows with declarations
 * (push/pop per block) so variable types infer correctly under shadowing (nearest in-scope declaration wins) */
typedef struct { Node *prog; Bind *map; int *nmap; CType ret; int *errc; } TcCtx;

static void tc_err(TcCtx *c, Node *n, const char *msg, CType a, CType b) {
    char an[128], bn[128]; tc_name(a, an); tc_name(b, bn);
    fprintf(stderr, "type error (line %d): %s '%s' and '%s'\n", n->line, msg, an, bn);
    (*c->errc)++;
}

/* Add a variable to the current scope (visible after its declaration point) */
static void tc_add(TcCtx *c, Node *d) {
    if (*c->nmap >= 512 || !d->name) return;
    c->map[*c->nmap].name = d->name;
    c->map[*c->nmap].t.base = d->type;
    c->map[*c->nmap].t.ptr = d->ptr;
    c->map[*c->nmap].t.sname = d->type_name;
    c->map[*c->nmap].t.sig = d->sig;
    (*c->nmap)++;
}

static void tc_check(TcCtx *c, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_BLOCK: {                       /* open a new scope: block variables vanish at its end */
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            *c->nmap = save;
            return;
        }
        case ND_VAR_DECL: {                    /* check the initializer in the current scope, then add the var */
            if (n->operand) {
                tc_check(c, n->operand);
                CType dt = { n->type, n->ptr, n->type_name, NULL };
                CType vt = infer(c->prog, n->operand, c->map, *c->nmap);
                if (!tc_assignable(dt, vt)) tc_err(c, n, "cannot initialize variable: type mismatch between", dt, vt);
            }
            tc_add(c, n);
            return;
        }
        case ND_FOR: {                         /* variables in for-init are scoped to the loop */
            int save = *c->nmap;
            tc_check(c, n->init); tc_check(c, n->cond); tc_check(c, n->step); tc_check(c, n->body);
            *c->nmap = save;
            return;
        }
        case ND_IF: {
            tc_check(c, n->cond);
            int s1 = *c->nmap; tc_check(c, n->then_branch); *c->nmap = s1;
            int s2 = *c->nmap; tc_check(c, n->else_branch); *c->nmap = s2;
            return;
        }
        case ND_WHILE: case ND_DOWHILE: {
            tc_check(c, n->cond);
            int save = *c->nmap; tc_check(c, n->body); *c->nmap = save;
            return;
        }
        case ND_SWITCH: {
            tc_check(c, n->cond);
            /* switch compares as integers (je): the value must be integer/char/bool or it silently misbehaves */
            CType ct = infer(c->prog, n->cond, c->map, *c->nmap);
            if (!tc_integer(ct)) {
                char tn[128]; tc_name(ct, tn);
                fprintf(stderr, "type error (line %d): switch requires an integer/char/bool value, got '%s'\n", n->line, tn);
                (*c->errc)++;
            }
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            *c->nmap = save;
            return;
        }
        case ND_CASE: {
            tc_check(c, n->operand);
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            *c->nmap = save;
            return;
        }
        case ND_STRUCT_LIT: {
            /* items are ND_ASSIGN (field = value); check each value against the declared field type */
            for (int i = 0; i < n->nitems; i++) {
                Node *fi = n->items[i];
                if (!fi || !fi->rhs) continue;
                tc_check(c, fi->rhs);
                CType ft;
                if (fi->lhs && fi->lhs->kind == ND_IDENT &&
                    struct_field_type(c->prog, n->name, fi->lhs->name, &ft)) {
                    CType vt = infer(c->prog, fi->rhs, c->map, *c->nmap);
                    if (!tc_assignable(ft, vt)) tc_err(c, fi, "struct field type mismatch between", ft, vt);
                }
            }
            return;
        }
        default: break;
    }

    /* general expressions: walk the children first, then check the operator */
    tc_check(c, n->lhs); tc_check(c, n->rhs); tc_check(c, n->operand);
    tc_check(c, n->cond); tc_check(c, n->then_branch); tc_check(c, n->else_branch);
    tc_check(c, n->init); tc_check(c, n->step); tc_check(c, n->body);
    for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);

    if (n->kind == ND_BINARY) {
        CType lt = infer(c->prog, n->lhs, c->map, *c->nmap);
        CType rt = infer(c->prog, n->rhs, c->map, *c->nmap);
        TokenType op = n->op;
        if (op == TK_PLUS || op == TK_MINUS) {
            int lp = tc_isptr(lt), rp = tc_isptr(rt);
            if (lp && rp) {                                  /* ptr - ptr is allowed, ptr + ptr is not */
                if (op == TK_PLUS) tc_err(c, n, "cannot add two pointers:", lt, rt);
            } else if (lp || rp) {                           /* only ptr +/- int */
                if (!((lp && tc_numeric(rt)) || (rp && tc_numeric(lt) && op == TK_PLUS)))
                    tc_err(c, n, "invalid pointer arithmetic between", lt, rt);
            } else if (!(tc_numeric(lt) && tc_numeric(rt))) {
                tc_err(c, n, "cannot apply '+'/'-' to", lt, rt);
            }
        } else if (op == TK_STAR || op == TK_SLASH || op == TK_STARSTAR) {
            if (!(tc_numeric(lt) && tc_numeric(rt))) tc_err(c, n, "cannot apply arithmetic to", lt, rt);
        } else if (op == TK_PERCENT) {
            /* modulo is integer-only (floats need fmod); avoids silently wrong results */
            if (!(tc_integer(lt) && tc_integer(rt))) tc_err(c, n, "modulo '%' requires integer operands, got", lt, rt);
        } else if (op == TK_AMP || op == TK_PIPE || op == TK_CARET || op == TK_SHL || op == TK_SHR) {
            if (!(tc_integer(lt) && tc_integer(rt))) tc_err(c, n, "bitwise/shift requires integer operands, got", lt, rt);
        } else if (op == TK_EQ || op == TK_NEQ || op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE) {
            if (tc_isstruct(lt) || tc_isstruct(rt) || tc_isvoid(lt) || tc_isvoid(rt))
                tc_err(c, n, "cannot compare", lt, rt);
        } else if (op == TK_AND || op == TK_OR) {
            if (tc_isstruct(lt) || tc_isstruct(rt) || tc_isvoid(lt) || tc_isvoid(rt))
                tc_err(c, n, "logical operator requires scalar operands, got", lt, rt);
        }
    } else if (n->kind == ND_ASSIGN) {
        CType lt = infer(c->prog, n->lhs, c->map, *c->nmap);
        CType rt = infer(c->prog, n->rhs, c->map, *c->nmap);
        if (!tc_assignable(lt, rt)) tc_err(c, n, "cannot assign value of type", rt, lt);   /* "... rt to lt" */
    } else if (n->kind == ND_RETURN && n->operand) {
        CType vt = infer(c->prog, n->operand, c->map, *c->nmap);
        if (!tc_assignable(c->ret, vt)) tc_err(c, n, "return type mismatch between", c->ret, vt);
    } else if (n->kind == ND_UNARY && n->op == TK_STAR) {
        /* dereference `*x` only works on a pointer (avoids silent segfaults) */
        CType ot = infer(c->prog, n->operand, c->map, *c->nmap);
        if (ot.ptr == 0) {
            char on[128]; tc_name(ot, on);
            fprintf(stderr, "type error (line %d): cannot dereference non-pointer value of type '%s'\n", n->line, on);
            (*c->errc)++;
        }
    } else if (n->kind == ND_CALL && n->operand) {
        /* Check call argument types: direct calls f(...) and user method calls obj.m(...).
         * (Skips extern/generic/namespaced: incomplete type info or variadic.) argoff=1 skips the method's self */
        Node *callee = n->operand, *fn = NULL;
        int argoff = 0;
        if (callee->kind == ND_IDENT) {
            for (int i = 0; i < c->prog->nitems; i++) {
                Node *d = c->prog->items[i];
                if (d->kind == ND_FUNC && !d->is_extern && d->ngen == 0 && !d->is_method &&
                    d->name && strcmp(d->name, callee->name) == 0) { fn = d; break; }
            }
        } else if (callee->kind == ND_MEMBER) {
            CType bt = infer(c->prog, callee->operand, c->map, *c->nmap);
            if (bt.base == TYPE_STRUCT && bt.sname) {       /* a method call obj.m(...) */
                for (int i = 0; i < c->prog->nitems; i++) {
                    Node *d = c->prog->items[i];
                    if (d->kind == ND_FUNC && d->is_method && d->ngen == 0 && d->ns && callee->name &&
                        strcmp(d->ns, bt.sname) == 0 && strcmp(d->name, callee->name) == 0) { fn = d; argoff = 1; break; }
                }
            }
        }
        if (fn) {
            if (n->nitems != fn->nitems - argoff) {
                fprintf(stderr, "type error (line %d): %s '%s' expects %d argument(s) but got %d\n",
                        n->line, argoff ? "method" : "function", fn->name, fn->nitems - argoff, n->nitems);
                (*c->errc)++;
            } else {
                for (int i = 0; i < n->nitems; i++) {
                    Node *pp = fn->items[i + argoff];
                    CType pt = { pp->type, pp->ptr, pp->type_name, pp->sig };
                    CType at = infer(c->prog, n->items[i], c->map, *c->nmap);
                    if (!tc_assignable(pt, at)) {
                        char an[128], pn[128]; tc_name(at, an); tc_name(pt, pn);
                        fprintf(stderr, "type error (line %d): argument %d to '%s': cannot pass '%s' where '%s' is expected\n",
                                n->line, i + 1, fn->name, an, pn);
                        (*c->errc)++;
                    }
                }
            }
        }
    }
    /* note: ND_VAR_DECL is handled in the switch above (checks the init + adds the variable to scope) */
}

int typecheck(Node *prog) {
    int errc = 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->body && f->ngen == 0) {
            Bind map[512]; int nmap = 0;
            CType ret = { f->type, f->ptr, f->type_name, NULL };
            TcCtx c = { prog, map, &nmap, ret, &errc };
            seed_globals(prog, map, &nmap);            /* globals live in the base scope */
            for (int j = 0; j < f->nitems; j++)        /* parameters may shadow globals */
                if (f->items[j]->kind == ND_PARAM) tc_add(&c, f->items[j]);
            tc_check(&c, f->body);
        } else if (f->kind == ND_VAR_DECL && f->operand) {  /* also check global initializers */
            Bind map[512]; int nmap = 0;
            CType ret = { TYPE_VOID, 0, NULL, NULL };
            TcCtx c = { prog, map, &nmap, ret, &errc };
            seed_globals(prog, map, &nmap);
            tc_check(&c, f->operand);
            CType dt = { f->type, f->ptr, f->type_name, NULL };
            CType vt = infer(prog, f->operand, map, nmap);
            if (!tc_assignable(dt, vt)) tc_err(&c, f, "cannot initialize global: type mismatch between", dt, vt);
        }
    }
    return errc;
}

/* Fill in trait default methods for types that impl the trait but did not write the method themselves
 * (clone the body from the trait, then substitute Self -> the type name). Runs before the completeness check */
static void apply_trait_defaults(Node *prog) {
    int n = prog->nitems; /* only markers present at the start (newly appended methods need no re-scan) */
    for (int i = 0; i < n; i++) {
        Node *d = prog->items[i];
        if (d->kind != ND_TRAIT_IMPL) continue;
        Node *tr = find_trait(prog, d->name);
        if (!tr) continue;
        for (int j = 0; j < tr->nitems; j++) {
            Node *sig = tr->items[j];
            if (sig->kind != ND_FUNC || !sig->body) continue;           /* no default */
            if (type_has_method(prog, d->type_name, sig->name)) continue;/* the type wrote its own */
            Node *m = node_clone(sig);
            m->is_method = 1;
            free(m->ns); m->ns = strdup(d->type_name);
            Bind self_map[1];
            self_map[0].name = "Self";
            self_map[0].t.base = TYPE_STRUCT; self_map[0].t.ptr = 0; self_map[0].t.sname = d->type_name;
            substitute(m, self_map, 1);                                  /* Self -> the concrete type */
            node_add_item(prog, m);
        }
    }
}

int monomorphize(Node *prog) {
    mono_err = 0;
    apply_trait_defaults(prog); /* fill in default methods first, then check completeness */
    check_trait_impls(prog);   /* impl Trait for Type must be complete and the trait must exist */
    /* loop until no new instances appear (supports generics calling generics) */
    for (int round = 0; round < 64; round++) {
        int made = 0;
        int nfuncs = prog->nitems; /* scan only what existed at round start (new instances wait a round) */
        for (int i = 0; i < nfuncs; i++) {
            Node *f = prog->items[i];
            if (f->kind == ND_FUNC && f->ngen == 0 && f->body) {
                Bind map[512]; int nmap = 0;
                seed_globals(prog, map, &nmap);
                for (int j = 0; j < f->nitems; j++) if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
                scan_calls(prog, f->body, map, &nmap, &made);
            } else if (f->kind == ND_VAR_DECL && f->operand) {   /* generic calls in global initializers */
                Bind map[512]; int nmap = 0;
                seed_globals(prog, map, &nmap);
                scan_calls(prog, f->operand, map, &nmap, &made);
            }
        }
        if (made == 0) break;
    }
    return mono_err;
}
