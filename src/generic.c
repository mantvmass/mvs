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
#include "diag.h"

/* Inferred concrete type (base + pointer depth + struct name + signature if it is a function pointer).
 * arr > 0 marks a whole [T; N] array value; base/ptr/sname then describe the element */
typedef struct { DataType base; int ptr; char *sname; Node *sig; int arr; } CType;

/* Mapping entry, name -> type (used as the per-function var-type map and the generic-param map).
 * is_const marks let/const so the type checker can reject writes to constants;
 * decl/used back the unused-variable warning (decl = the ND_VAR_DECL/ND_PARAM node) */
typedef struct { char *name; CType t; int is_const; Node *decl; int used; } Bind;

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

/* Check a '+'-separated bound list ("A+B") against a struct; returns 1 when every part
 * is implemented, else 0 with the first missing trait copied into missing (cap bytes) */
static int type_impls_all(Node *prog, const char *sname, const char *bounds, char *missing, size_t cap) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", bounds ? bounds : "");
    for (char *tok = strtok(tmp, "+"); tok; tok = strtok(NULL, "+")) {
        if (!type_impls_trait(prog, sname, tok)) {
            snprintf(missing, cap, "%s", tok);
            return 0;
        }
    }
    return 1;
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
            diag_print(d->file, d->line, d->col, "error", "unknown trait '%s' in impl for '%s'", d->name, d->type_name);
            mono_err++; continue;
        }
        for (int j = 0; j < tr->nitems; j++) {
            Node *sig = tr->items[j];
            if (sig->kind != ND_FUNC) continue;
            if (!type_has_method(prog, d->type_name, sig->name)) {
                diag_print(d->file, d->line, d->col, "error",
                           "type '%s' is missing method '%s' required by trait '%s'",
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
                    out->base = f->type; out->ptr = f->ptr; out->sname = f->type_name; out->sig = f->sig;
                    out->arr = f->arr;
                    return 1;
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
    CType r = { TYPE_I64, 0, NULL, NULL, 0 };
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
            if (bt.arr > 0 && n->name && strcmp(n->name, "len") == 0) { r.base = TYPE_USIZE; break; }
            CType ft; if (struct_field_type(prog, bt.sname, n->name, &ft)) return ft;
            break;
        }
        case ND_INDEX: {
            CType bt = infer(prog, n->lhs, map, nmap);
            if (bt.arr > 0)      { r = bt; r.arr = 0; }   /* array element */
            else if (bt.ptr > 0) { r = bt; r.ptr--; }     /* pointer indexing */
            break;
        }
        case ND_ARRAY_LIT:
            if (n->nitems > 0) { r = infer(prog, n->items[0], map, nmap); r.arr = n->nitems; }
            break;
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
                    CType ct = { cc.sig->type, cc.sig->ptr, cc.sig->type_name, NULL, 0 };
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
                if (bt.ptr == 0 && bt.base == TYPE_DYN && bt.sname) {
                    /* dynamic dispatch: the result type comes from the trait's signature */
                    Node *tr = find_trait(prog, bt.sname);
                    if (tr) for (int i = 0; i < tr->nitems; i++)
                        if (tr->items[i]->kind == ND_FUNC && callee->name &&
                            strcmp(tr->items[i]->name, callee->name) == 0) {
                            CType ct = { tr->items[i]->type, tr->items[i]->ptr, tr->items[i]->type_name, NULL, 0 };
                            return ct;
                        }
                }
                /* primitive receiver (impl Display for i64): the namespace is the type's name.
                 * A bare unresolved identifier stays a MODULE namespace (io.out), so the
                 * primitive path only applies when the base is a real variable/expression. */
                int prim_recv = 0;
                if (bt.ptr == 0 && bt.base != TYPE_STRUCT && bt.base != TYPE_DYN &&
                    bt.base != TYPE_FUNC && bt.base != TYPE_VOID && bt.base != TYPE_UNKNOWN) {
                    if (callee->operand->kind != ND_IDENT) prim_recv = 1;
                    else for (int i = nmap - 1; i >= 0; i--)
                        if (strcmp(map[i].name, callee->operand->name) == 0) { prim_recv = 1; break; }
                }
                if (prim_recv) scope = datatype_name(bt.base);
                else if (bt.base == TYPE_STRUCT && bt.sname) scope = bt.sname;
                else if (callee->operand->kind == ND_IDENT) scope = callee->operand->name;
                Node *best = NULL;
                for (int i = 0; i < prog->nitems; i++) {
                    Node *f = prog->items[i];
                    if (f->kind != ND_FUNC || strcmp(f->name, callee->name) != 0) continue;
                    if (scope && f->ns && strcmp(f->ns, scope) == 0) { best = f; break; }
                    if (!best) best = f;
                }
                if (best) { CType ct = { best->type, best->ptr, best->type_name, NULL, 0 }; return ct; }
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
    map[*nmap].t.arr = d->arr;
    map[*nmap].is_const = (d->kind == ND_VAR_DECL) ? d->is_const : 0;
    map[*nmap].decl = d;
    map[*nmap].used = 0;
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
                CType concrete = { TYPE_I64, 0, NULL, NULL, 0 };
                /* Find the first parameter whose type refers to this generic param */
                for (int pi = 0; pi < tmpl->nitems && pi < n->nitems; pi++) {
                    Node *pp = tmpl->items[pi];
                    if (pp->type == TYPE_STRUCT && pp->type_name &&
                        strcmp(pp->type_name, tmpl->gen[gi]) == 0) {
                        CType at = infer(prog, n->items[pi], map, *nmap);
                        if (at.arr > 0) { at.arr = 0; at.ptr++; }   /* [T; N] decays to *T for inference */
                        concrete.base = at.base; concrete.sname = at.sname; concrete.sig = at.sig;
                        concrete.ptr = at.ptr - pp->ptr; if (concrete.ptr < 0) concrete.ptr = 0;
                        break;
                    }
                }
                gmap[gi].name = tmpl->gen[gi]; gmap[gi].t = concrete;
            }
            /* Check trait bounds: with <T: A + B> the concrete type must impl every listed trait */
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                if (!tmpl->gen_bound[gi]) continue;
                CType ct = gmap[gi].t;
                char missing[128];
                snprintf(missing, sizeof(missing), "%s", tmpl->gen_bound[gi]);
                /* the bound holder may be a struct OR a primitive (impl Display for i64) */
                const char *bn = (ct.base == TYPE_STRUCT) ? ct.sname
                               : (ct.ptr == 0 && ct.base != TYPE_DYN && ct.base != TYPE_FUNC &&
                                  ct.base != TYPE_VOID && ct.base != TYPE_UNKNOWN)
                                   ? datatype_name(ct.base) : NULL;
                int ok = bn && type_impls_all(prog, bn, tmpl->gen_bound[gi], missing, sizeof(missing));
                if (!ok) {
                    const char *tn = (ct.base == TYPE_STRUCT && ct.sname) ? ct.sname : datatype_name(ct.base);
                    diag_print(n->file, n->line, n->col, "error",
                               "type '%s' does not implement trait '%s' (required by %s<%s: %s>)",
                               tn, missing, tmpl->name, tmpl->gen[gi], tmpl->gen_bound[gi]);
                    if (ct.base == TYPE_STRUCT && ct.sname)
                        diag_help("add 'impl %s for %s { ... }' to satisfy the bound", missing, tn);
                    else
                        diag_help("traits can only be implemented for structs; '%s' cannot satisfy the bound", tn);
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
        CType t = { fn->items[i]->type, fn->items[i]->ptr, fn->items[i]->type_name, NULL, 0 };
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
        if (t.arr > 0) { t.arr = 0; t.ptr++; }   /* [T; N] argument decays to *T for overload matching */
        char c[128]; if (exact) width_code(t, c); else cat_code(t, c);
        sig_append(buf, &len, i == 0, c);
    }
}

/* Compare namespaces (NULL counts as "") */
static int ns_eq(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return strcmp(a, b) == 0;
}

/* Group of functions sharing a namespace + name (overload set): sig = exact, catsig = category (fallback) */
typedef struct { char *ns; char *orig; Node *defs[16]; char sig[16][SIGCAP]; char catsig[16][SIGCAP]; int n; } OvSet;

/* Is this function eligible for overloading (a plain function, possibly module-namespaced) */
static int ov_eligible(Node *f) {
    return f->kind == ND_FUNC && f->body && f->ngen == 0 && !f->variadic &&
           !f->is_export && !f->is_method && !f->is_extern &&
           strcmp(f->name, "main") != 0;
}

/* Find the overload set for (ns, name); -1 if none */
static int ov_find(OvSet *sets, int nsets, const char *ns, const char *name) {
    for (int s = 0; s < nsets; s++)
        if (ns_eq(sets[s].ns, ns) && strcmp(sets[s].orig, name) == 0) return s;
    return -1;
}

/* Module namespace of the function whose body is being scanned (for unqualified calls:
 * inside std/math, abs(x) must see math's overload set before the global one) */
static const char *ov_cur_mod = "";

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

    if (n->kind == ND_CALL && n->operand) {
        Node *callee = n->operand;
        int s = -1;
        if (callee->kind == ND_IDENT) {
            /* unqualified call: the enclosing module's overload set wins, then the global one */
            s = ov_find(sets, nsets, ov_cur_mod, callee->name);
            if (s < 0) s = ov_find(sets, nsets, "", callee->name);
        } else if (callee->kind == ND_MEMBER && callee->operand &&
                   callee->operand->kind == ND_IDENT && callee->name) {
            /* ns.f(...): the base must be a module namespace, not a variable (obj.method) */
            int bound = 0;
            for (int i = *nmap - 1; i >= 0; i--)
                if (strcmp(map[i].name, callee->operand->name) == 0) { bound = 1; break; }
            if (!bound) s = ov_find(sets, nsets, callee->operand->name, callee->name);
        }
        if (s >= 0 && sets[s].n >= 2) {
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
                    diag_print(n->file, n->line, n->col, "error",
                               "ambiguous call to '%s' with argument types (%s): multiple width overloads match",
                               sets[s].orig, wx);
                    diag_help("disambiguate with an explicit cast, e.g. f(x as i32)");
                    return;
                }
            }
            if (found >= 0) { free(callee->name); callee->name = strdup(sets[s].defs[found]->name); }
            else {
                diag_print(n->file, n->line, n->col, "error",
                           "no overload of '%s' matches argument types (%s)", sets[s].orig, wx);
            }
        }
    }
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
                diag_print(a->file, a->line, a->col, "error", "duplicate struct '%s'", a->name); errc++;
            } else if (a->kind == ND_TRAIT) {
                diag_print(a->file, a->line, a->col, "error", "duplicate trait '%s'", a->name); errc++;
            } else { /* ND_FUNC */
                if (a->is_extern || b->is_extern || a->ngen > 0 || b->ngen > 0) continue;
                if (!ns_eq(a->ns, b->ns)) continue;
                char sa[SIGCAP], sb[SIGCAP]; sig_func(a, sa, 1); sig_func(b, sb, 1);
                if (strcmp(sa, sb) != 0) continue; /* different signature = overload, not a duplicate */
                if (a->ns && a->ns[0])
                    diag_print(a->file, a->line, a->col, "error",
                               "duplicate function '%s.%s' with the same parameter types (%s)", a->ns, a->name, sa);
                else
                    diag_print(a->file, a->line, a->col, "error",
                               "duplicate function '%s' with the same parameter types (%s)", a->name, sa);
                diag_help("overloads must differ in parameter types; rename one of the definitions otherwise");
                errc++;
            }
        }
    }
    return errc;
}

void resolve_overloads(Node *prog) {
    static OvSet sets[256]; int nsets = 0;
    /* 1) group functions by namespace + original name (each module has its own sets) */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (!ov_eligible(f)) continue;
        int si = ov_find(sets, nsets, f->ns, f->name);
        if (si < 0) {
            if (nsets >= 256) { fprintf(stderr, "codegen error: too many distinct function names for overloading\n"); break; }
            si = nsets++; sets[si].ns = strdup(f->ns ? f->ns : ""); sets[si].orig = strdup(f->name); sets[si].n = 0;
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
            ov_cur_mod = f->mod ? f->mod : "";   /* unqualified calls inside a module see its sets first */
            scan_ov(prog, f->body, map, &nmap, sets, nsets);
            ov_cur_mod = "";
        } else if (f->kind == ND_VAR_DECL && f->operand) {
            Bind map[512]; int nmap = 0;   /* use a real map (avoids NULL deref if the init has a block/decl) */
            seed_globals(prog, map, &nmap);
            scan_ov(prog, f->operand, map, &nmap, sets, nsets);
        }
    }
}

/* ---------- default argument filling ----------
 *
 * A parameter may declare a default value: func f(a: i32, b: i32 = 5). A call that
 * omits trailing arguments gets clones of those default expressions appended here,
 * before monomorphize/overload/typecheck run, so every later pass sees complete calls.
 * Filling only happens when the callee resolves to exactly one candidate:
 *   - plain call f(...): exactly one non-extern, non-method function with that name
 *   - method call obj.m(...): the method found by (struct name, m)
 * Overloaded names are left alone (mixing defaults into overload resolution would be ambiguous). */

/* The single non-extern function with this name, or NULL if none/overloaded */
static Node *df_find_unique(Node *prog, const char *name) {
    Node *found = NULL;
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind != ND_FUNC || f->is_extern || f->is_method || !f->name) continue;
        if (strcmp(f->name, name) != 0) continue;
        if (found) return NULL;                      /* overloaded: leave the call alone */
        found = f;
    }
    return found;
}

/* Count of leading parameters without a default (the minimum a caller must pass) */
static int df_min_args(Node *fn, int argoff) {
    for (int i = argoff; i < fn->nitems; i++)
        if (fn->items[i]->operand) return i - argoff;
    return fn->nitems - argoff;
}

/* Append default-value clones to a call that omits trailing arguments */
static void df_fill_call(Node *prog, Node *call, Bind *map, int nmap) {
    Node *callee = call->operand, *fn = NULL;
    int argoff = 0;
    if (!callee) return;
    if (callee->kind == ND_IDENT) {
        fn = df_find_unique(prog, callee->name);
    } else if (callee->kind == ND_MEMBER) {
        CType bt = infer(prog, callee->operand, map, nmap);
        if (bt.base == TYPE_STRUCT && bt.sname) {    /* obj.method(...) */
            for (int i = 0; i < prog->nitems; i++) {
                Node *d = prog->items[i];
                if (d->kind == ND_FUNC && d->is_method && d->ns && callee->name &&
                    strcmp(d->ns, bt.sname) == 0 && strcmp(d->name, callee->name) == 0) { fn = d; argoff = 1; break; }
            }
        }
    }
    if (!fn || fn->ngen > 0 || fn->variadic) return; /* generic/variadic: not default-fillable */
    int total = fn->nitems - argoff;
    int min = df_min_args(fn, argoff);
    if (call->nitems < min || call->nitems >= total) return;  /* wrong count: typecheck reports it */
    for (int i = call->nitems + argoff; i < fn->nitems; i++)
        node_add_item(call, node_clone(fn->items[i]->operand));
}

/* Scope-aware walk over a function body (same shape as scan_calls/scan_ov) */
static void df_walk(Node *prog, Node *n, Bind *map, int *nmap) {
    if (!n) return;
    switch (n->kind) {
        case ND_BLOCK: { int s=*nmap; for (int i=0;i<n->nitems;i++) df_walk(prog,n->items[i],map,nmap); *nmap=s; return; }
        case ND_VAR_DECL: { df_walk(prog,n->operand,map,nmap); add_bind(map,nmap,n); return; }
        case ND_FOR: { int s=*nmap; df_walk(prog,n->init,map,nmap); df_walk(prog,n->cond,map,nmap); df_walk(prog,n->step,map,nmap); df_walk(prog,n->body,map,nmap); *nmap=s; return; }
        case ND_IF: { df_walk(prog,n->cond,map,nmap); int s1=*nmap; df_walk(prog,n->then_branch,map,nmap); *nmap=s1; int s2=*nmap; df_walk(prog,n->else_branch,map,nmap); *nmap=s2; return; }
        case ND_WHILE: case ND_DOWHILE: { df_walk(prog,n->cond,map,nmap); int s=*nmap; df_walk(prog,n->body,map,nmap); *nmap=s; return; }
        case ND_SWITCH: { df_walk(prog,n->cond,map,nmap); int s=*nmap; for (int i=0;i<n->nitems;i++) df_walk(prog,n->items[i],map,nmap); *nmap=s; return; }
        case ND_CASE: { df_walk(prog,n->operand,map,nmap); int s=*nmap; for (int i=0;i<n->nitems;i++) df_walk(prog,n->items[i],map,nmap); *nmap=s; return; }
        default: break;
    }
    df_walk(prog, n->lhs, map, nmap);   df_walk(prog, n->rhs, map, nmap);
    df_walk(prog, n->operand, map, nmap); df_walk(prog, n->cond, map, nmap);
    df_walk(prog, n->then_branch, map, nmap); df_walk(prog, n->else_branch, map, nmap);
    df_walk(prog, n->init, map, nmap);  df_walk(prog, n->step, map, nmap);
    df_walk(prog, n->body, map, nmap);
    for (int i = 0; i < n->nitems; i++) df_walk(prog, n->items[i], map, nmap);
    if (n->kind == ND_CALL) df_fill_call(prog, n, map, *nmap);
}

void fill_default_args(Node *prog) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->body) {         /* includes generic templates: instances clone the filled calls */
            Bind map[512]; int nmap = 0;
            seed_globals(prog, map, &nmap);
            for (int j = 0; j < f->nitems; j++) if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
            df_walk(prog, f->body, map, &nmap);
        } else if (f->kind == ND_VAR_DECL && f->operand) {
            Bind map[512]; int nmap = 0;
            seed_globals(prog, map, &nmap);
            df_walk(prog, f->operand, map, &nmap);
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

/* Write a readable type name into buf (e.g. "u8", "*i32", "str", "Point", "dyn Area") */
static void tc_name(CType t, char *buf) {
    char *p = buf;
    for (int i = 0; i < t.ptr; i++) *p++ = '*';
    if (t.base == TYPE_DYN && t.sname) { sprintf(p, "dyn %s", t.sname); return; }
    const char *b = (t.base == TYPE_STRUCT && t.sname) ? t.sname : datatype_name(t.base);
    strcpy(p, b);
}

static int tc_is128(CType t) { return t.ptr == 0 && (t.base == TYPE_I128 || t.base == TYPE_U128); }

/* Check whether a value (s) can be assigned to a target (d) */
static int tc_assignable(CType d, CType s) {
    /* 128-bit rules: narrowing out of i128 must be explicit ('as'), and floats never
     * convert to/from i128 implicitly (no 128-bit float path in codegen) */
    if (tc_is128(s) && !tc_is128(d)) return 0;
    if (tc_is128(d) && s.ptr == 0 && (s.base == TYPE_F32 || s.base == TYPE_F64)) return 0;
    /* trait objects: dyn Trait accepts another dyn of the SAME trait or a pointer to a
     * struct (the impl is verified at the assignment site); dyn converts to nothing else */
    if (d.ptr == 0 && d.base == TYPE_DYN) {
        if (s.ptr == 0 && s.base == TYPE_DYN) return d.sname && s.sname && strcmp(d.sname, s.sname) == 0;
        return s.ptr == 1;   /* pointer to struct OR primitive; the impl is verified separately */
    }
    if (s.ptr == 0 && s.base == TYPE_DYN) return 0;
    if (d.arr > 0 || s.arr > 0) {
        /* whole arrays never assign; a [T; N] value decays to a pointer when the target is one */
        if (d.arr > 0) return 0;                        /* array targets are handled at the declaration */
        return tc_isptr(d) || tc_isstr(d);
    }
    if (tc_isstruct(d)) return tc_isstruct(s) && d.sname && s.sname && strcmp(d.sname, s.sname) == 0;
    if (tc_isstruct(s)) return 0;                       /* a struct value only fits the same struct type */
    if (tc_isptr(d) || tc_isstr(d))                     /* pointer/str target: accepts pointer/str/integer (address/null) */
        return tc_isptr(s) || tc_isstr(s) || tc_integer(s);
    if (tc_numeric(d)) return tc_numeric(s);            /* numeric target: no str/pointer/struct */
    return 1;                                           /* void etc.: not strict */
}

/* ---------- control-flow helpers (missing-return and unreachable-code checks) ---------- */

/* Does this subtree contain a `break` that binds to the ENCLOSING loop?
 * Nested loops and switches keep their breaks to themselves, so recursion stops there. */
static int has_break(Node *n) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_BREAK: return 1;
        case ND_WHILE: case ND_DOWHILE: case ND_FOR: case ND_SWITCH: return 0;
        default: break;
    }
    if (has_break(n->lhs) || has_break(n->rhs) || has_break(n->operand) || has_break(n->cond) ||
        has_break(n->then_branch) || has_break(n->else_branch) || has_break(n->init) ||
        has_break(n->step) || has_break(n->body)) return 1;
    for (int i = 0; i < n->nitems; i++) if (has_break(n->items[i])) return 1;
    return 0;
}

/* Conservative "this statement guarantees a return" analysis. When unsure it answers no,
 * which surfaces as a missing-return error the programmer can silence with an explicit
 * return; it never silently accepts a function that could fall off the end. */
static int always_returns(Node *n) {
    if (!n) return 0;
    switch (n->kind) {
        case ND_RETURN: return 1;
        case ND_BLOCK:
            /* sequential: if some statement guarantees a return, the block does */
            for (int i = 0; i < n->nitems; i++) if (always_returns(n->items[i])) return 1;
            return 0;
        case ND_IF:
            return n->else_branch != NULL && always_returns(n->then_branch) && always_returns(n->else_branch);
        case ND_SWITCH: {
            int has_default = 0;
            for (int i = 0; i < n->nitems; i++) {
                Node *cs = n->items[i];
                if (!cs->operand) has_default = 1;
                int r = 0;
                for (int j = 0; j < cs->nitems; j++) if (always_returns(cs->items[j])) { r = 1; break; }
                if (!r) return 0;   /* a case may fall through and then out of the switch */
            }
            return has_default;
        }
        case ND_DOWHILE:
            return always_returns(n->body);   /* the body runs at least once */
        case ND_WHILE:
            /* while (true) without a break can only leave through a return */
            return n->cond && n->cond->kind == ND_BOOL && n->cond->int_val == 1 && !has_break(n->body);
        case ND_FOR:
            return n->cond == NULL && !has_break(n->body);   /* for (;;) likewise */
        default: return 0;
    }
}

/* A statement after which the rest of the block cannot run */
static int stmt_terminates(Node *n) {
    return n && (n->kind == ND_BREAK || n->kind == ND_CONTINUE || always_returns(n));
}

/* Context of the function being checked. map/nmap is a scope stack that grows with declarations
 * (push/pop per block) so variable types infer correctly under shadowing (nearest in-scope declaration wins) */
typedef struct { Node *prog; Bind *map; int *nmap; CType ret; int *errc; } TcCtx;

static void tc_err(TcCtx *c, Node *n, const char *msg, CType a, CType b) {
    char an[128], bn[128]; tc_name(a, an); tc_name(b, bn);
    diag_print(n->file, n->line, n->col, "error", "%s '%s' and '%s'", msg, an, bn);
    /* the most common confusion: strings never convert to numbers implicitly */
    if ((tc_isstr(a) && tc_numeric(b)) || (tc_numeric(a) && tc_isstr(b)))
        diag_help("MVS never converts between strings and numbers implicitly; use extern atoi/strtod");
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
    c->map[*c->nmap].t.arr = d->arr;
    c->map[*c->nmap].is_const = (d->kind == ND_VAR_DECL) ? d->is_const : 0;
    c->map[*c->nmap].decl = d;
    c->map[*c->nmap].used = 0;
    (*c->nmap)++;
}

/* Close a scope: warn about local variables that were never mentioned, then pop.
 * Warnings fire only for the entry file (imported modules stay quiet) and skip
 * names starting with '_' (the conventional way to keep something intentionally) */
static void tc_pop(TcCtx *c, int save) {
    for (int i = save; i < *c->nmap; i++) {
        Bind *b = &c->map[i];
        if (b->used || !b->decl || b->decl->kind != ND_VAR_DECL) continue;
        if (!b->name || b->name[0] == '_') continue;
        if (!diag_is_primary(b->decl->file)) continue;
        diag_print(b->decl->file, b->decl->line, b->decl->col, "warning", "unused variable '%s'", b->name);
        diag_help("prefix it with an underscore ('_%s') to silence this warning", b->name);
    }
    *c->nmap = save;
}

/* Find the nearest in-scope binding for a name (shadowing: most recent wins), NULL if not found */
static Bind *tc_find(TcCtx *c, const char *name) {
    if (!name) return NULL;
    for (int i = *c->nmap - 1; i >= 0; i--)
        if (strcmp(c->map[i].name, name) == 0) return &c->map[i];
    return NULL;
}

/* A bare i128 or dyn value in a boolean context would test its ADDRESS (always true) */
static void tc_check_cond128(TcCtx *c, Node *cond) {
    if (!cond) return;
    CType t = infer(c->prog, cond, c->map, *c->nmap);
    if (t.ptr == 0 && (t.base == TYPE_I128 || t.base == TYPE_U128)) {
        diag_print(cond->file, cond->line, cond->col, "error",
                   "a 128-bit integer cannot be used directly as a condition");
        diag_help("compare it explicitly: write 'x != 0'");
        (*c->errc)++;
    } else if (t.ptr == 0 && t.base == TYPE_DYN) {
        diag_print(cond->file, cond->line, cond->col, "error",
                   "a trait object cannot be used as a condition");
        (*c->errc)++;
    }
}

/* When a dyn Trait target receives a pointer, the pointee type (struct OR primitive)
 * must impl the trait */
static void tc_dyn_impl_check(TcCtx *c, Node *site, CType d, CType s) {
    if (!(d.ptr == 0 && d.base == TYPE_DYN) || !d.sname) return;
    if (s.ptr != 1) return;
    const char *tn = s.base == TYPE_STRUCT ? s.sname : datatype_name(s.base);
    if (tn && !type_impls_trait(c->prog, tn, d.sname)) {
        diag_print(site->file, site->line, site->col, "error",
                   "type '%s' does not implement trait '%s'", tn, d.sname);
        diag_help("add 'impl %s for %s { ... }' before storing it in a 'dyn %s'", d.sname, tn, d.sname);
        (*c->errc)++;
    }
}

/* If an assignment/increment target is (a field of) a const variable, report it.
 * Walks member chains (p.x.y) down to the base identifier; writes through a
 * dereferenced pointer are allowed (the pointer may point at mutable memory) */
static void tc_check_const_target(TcCtx *c, Node *target, Node *site) {
    Node *base = target;
    while (base && base->kind == ND_MEMBER) base = base->operand;
    if (!base || base->kind != ND_IDENT) return;
    Bind *b = tc_find(c, base->name);
    if (b && b->is_const) {
        diag_print(site->file, site->line, site->col, "error",
                   "cannot assign to constant '%s' (declared with 'const')", base->name);
        diag_help("declare '%s' with 'let' to make it mutable", base->name);
        (*c->errc)++;
    }
}

static void tc_check(TcCtx *c, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case ND_IDENT: {                       /* mark the nearest binding as used (unused-var warning) */
            Bind *b = tc_find(c, n->name);
            if (b) b->used = 1;
            return;
        }
        case ND_BLOCK: {                       /* open a new scope: block variables vanish at its end */
            int save = *c->nmap;
            int dead_reported = 0;
            for (int i = 0; i < n->nitems; i++) {
                tc_check(c, n->items[i]);
                /* anything after a guaranteed return/break/continue can never run */
                if (!dead_reported && i + 1 < n->nitems && stmt_terminates(n->items[i])) {
                    Node *next = n->items[i + 1];
                    if (diag_is_primary(next->file)) {
                        diag_print(next->file, next->line, next->col, "warning", "unreachable code");
                        diag_help("the statement above always returns or jumps away");
                    }
                    dead_reported = 1;
                }
            }
            tc_pop(c, save);
            return;
        }
        case ND_VAR_DECL: {                    /* check the initializer in the current scope, then add the var */
            if (n->operand) {
                tc_check(c, n->operand);
                if (n->arr > 0) {
                    /* [T; N]: only an array literal with exactly N assignable elements */
                    if (n->operand->kind != ND_ARRAY_LIT) {
                        diag_print(n->file, n->line, n->col, "error",
                                   "an array variable can only be initialized with an array literal [e1, e2, ...]");
                        (*c->errc)++;
                    } else {
                        if (n->operand->nitems != n->arr) {
                            diag_print(n->operand->file, n->operand->line, n->operand->col, "error",
                                       "array literal has %d element(s) but the type needs exactly %d",
                                       n->operand->nitems, n->arr);
                            diag_help("[T; %d] requires exactly %d elements in the literal", n->arr, n->arr);
                            (*c->errc)++;
                        }
                        CType et = { n->type, n->ptr, n->type_name, n->sig, 0 };
                        for (int i = 0; i < n->operand->nitems; i++) {
                            CType vt2 = infer(c->prog, n->operand->items[i], c->map, *c->nmap);
                            if (!tc_assignable(et, vt2))
                                tc_err(c, n->operand->items[i], "array element type mismatch between", et, vt2);
                        }
                    }
                } else if (n->operand->kind == ND_ARRAY_LIT) {
                    diag_print(n->file, n->line, n->col, "error",
                               "an array literal can only initialize a [T; N] variable");
                    diag_help("declare the variable as e.g. 'let x: [i32; %d] = ...'", n->operand->nitems);
                    (*c->errc)++;
                } else {
                    CType dt = { n->type, n->ptr, n->type_name, NULL, 0 };
                    CType vt = infer(c->prog, n->operand, c->map, *c->nmap);
                    if (!tc_assignable(dt, vt)) tc_err(c, n, "cannot initialize variable: type mismatch between", dt, vt);
                    else tc_dyn_impl_check(c, n, dt, vt);
                }
            }
            tc_add(c, n);
            return;
        }
        case ND_FOR: {                         /* variables in for-init are scoped to the loop */
            int save = *c->nmap;
            tc_check(c, n->init); tc_check(c, n->cond); tc_check(c, n->step); tc_check(c, n->body);
            tc_check_cond128(c, n->cond);
            tc_pop(c, save);
            return;
        }
        case ND_IF: {
            tc_check(c, n->cond);
            tc_check_cond128(c, n->cond);
            int s1 = *c->nmap; tc_check(c, n->then_branch); tc_pop(c, s1);
            int s2 = *c->nmap; tc_check(c, n->else_branch); tc_pop(c, s2);
            return;
        }
        case ND_WHILE: case ND_DOWHILE: {
            tc_check(c, n->cond);
            tc_check_cond128(c, n->cond);
            int save = *c->nmap; tc_check(c, n->body); tc_pop(c, save);
            return;
        }
        case ND_SWITCH: {
            tc_check(c, n->cond);
            /* switch compares as integers (je): the value must be integer/char/bool or it silently misbehaves */
            CType ct = infer(c->prog, n->cond, c->map, *c->nmap);
            if (!tc_integer(ct) || tc_is128(ct)) {
                char tn[128]; tc_name(ct, tn);
                diag_print(n->file, n->line, n->col, "error",
                           "switch requires an integer/char/bool value (64-bit or less), got '%s'", tn);
                diag_help("switch compares with integer equality; use if/elseif chains for '%s'", tn);
                (*c->errc)++;
            }
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            tc_pop(c, save);
            return;
        }
        case ND_CASE: {
            tc_check(c, n->operand);
            int save = *c->nmap;
            for (int i = 0; i < n->nitems; i++) tc_check(c, n->items[i]);
            tc_pop(c, save);
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
                    if (ft.arr > 0) {
                        /* [T; N] field: needs an array literal with exactly N assignable elements */
                        if (fi->rhs->kind != ND_ARRAY_LIT) {
                            diag_print(fi->file, fi->line, fi->col, "error",
                                       "array field '%s' can only be initialized with an array literal", fi->lhs->name);
                            (*c->errc)++;
                        } else if (fi->rhs->nitems != ft.arr) {
                            diag_print(fi->rhs->file, fi->rhs->line, fi->rhs->col, "error",
                                       "array literal has %d element(s) but field '%s' needs exactly %d",
                                       fi->rhs->nitems, fi->lhs->name, ft.arr);
                            (*c->errc)++;
                        } else {
                            CType et = ft; et.arr = 0;
                            for (int j = 0; j < fi->rhs->nitems; j++) {
                                CType vt2 = infer(c->prog, fi->rhs->items[j], c->map, *c->nmap);
                                if (!tc_assignable(et, vt2))
                                    tc_err(c, fi->rhs->items[j], "array element type mismatch between", et, vt2);
                            }
                        }
                        continue;
                    }
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
            if (tc_isstruct(lt) || tc_isstruct(rt) || tc_isvoid(lt) || tc_isvoid(rt) ||
                (lt.ptr == 0 && lt.base == TYPE_DYN) || (rt.ptr == 0 && rt.base == TYPE_DYN))
                tc_err(c, n, "cannot compare", lt, rt);
        } else if (op == TK_AND || op == TK_OR) {
            if (tc_isstruct(lt) || tc_isstruct(rt) || tc_isvoid(lt) || tc_isvoid(rt) ||
                (lt.ptr == 0 && lt.base == TYPE_DYN) || (rt.ptr == 0 && rt.base == TYPE_DYN))
                tc_err(c, n, "logical operator requires scalar operands, got", lt, rt);
        }
    } else if (n->kind == ND_ASSIGN) {
        CType lt = infer(c->prog, n->lhs, c->map, *c->nmap);
        CType rt = infer(c->prog, n->rhs, c->map, *c->nmap);
        if (lt.arr > 0) {
            diag_print(n->file, n->line, n->col, "error", "whole-array assignment is not supported");
            diag_help("copy element by element instead");
            (*c->errc)++;
        } else if (tc_is128(lt) && n->op != TK_ASSIGN) {
            diag_print(n->file, n->line, n->col, "error",
                       "compound assignment is not supported on 128-bit integers");
            diag_help("write it out: x = x + y");
            (*c->errc)++;
        } else if (!tc_assignable(lt, rt)) tc_err(c, n, "cannot assign value of type", rt, lt); /* "... rt to lt" */
        else tc_dyn_impl_check(c, n, lt, rt);
        tc_check_const_target(c, n->lhs, n);                       /* const is read-only */
    } else if (n->kind == ND_UNARY && (n->op == TK_PLUSPLUS || n->op == TK_MINUSMINUS)) {
        tc_check_const_target(c, n->operand, n);                   /* x++ / x-- also writes x */
        {
            CType ot = infer(c->prog, n->operand, c->map, *c->nmap);
            if (tc_is128(ot)) {
                diag_print(n->file, n->line, n->col, "error",
                           "'++'/'--' is not supported on 128-bit integers");
                diag_help("write it out: x = x + 1");
                (*c->errc)++;
            }
        }
    } else if (n->kind == ND_RETURN && n->operand) {
        CType vt = infer(c->prog, n->operand, c->map, *c->nmap);
        if (!tc_assignable(c->ret, vt)) tc_err(c, n, "return type mismatch between", c->ret, vt);
        else tc_dyn_impl_check(c, n, c->ret, vt);
    } else if (n->kind == ND_CAST) {
        /* floats never convert to/from 128-bit integers (no such codegen path) */
        CType st = infer(c->prog, n->operand, c->map, *c->nmap);
        CType dt = { n->type, n->ptr, n->type_name, NULL, 0 };
        int sf = st.ptr == 0 && (st.base == TYPE_F32 || st.base == TYPE_F64);
        int df = dt.ptr == 0 && (dt.base == TYPE_F32 || dt.base == TYPE_F64);
        if ((tc_is128(st) && df) || (sf && tc_is128(dt))) {
            diag_print(n->file, n->line, n->col, "error",
                       "cannot cast between floating point and 128-bit integers directly");
            diag_help("go through a 64-bit integer: (x as i64) as f64, or (x as i64) as i128");
            (*c->errc)++;
        }
    } else if (n->kind == ND_INDEX) {
        /* a[i]: the base must be indexable, the index an integer, and a constant
         * index into a [T; N] must be inside the bounds (checked at compile time) */
        CType bt = infer(c->prog, n->lhs, c->map, *c->nmap);
        CType it = infer(c->prog, n->rhs, c->map, *c->nmap);
        if (bt.arr == 0 && bt.ptr == 0) {
            char bn[128]; tc_name(bt, bn);
            diag_print(n->file, n->line, n->col, "error", "cannot index a value of type '%s'", bn);
            diag_help("indexing works on arrays ([T; N]) and pointers (*T)");
            (*c->errc)++;
        }
        if (!tc_integer(it)) {
            char in2[128]; tc_name(it, in2);
            diag_print(n->rhs->file, n->rhs->line, n->rhs->col, "error",
                       "array index must be an integer, got '%s'", in2);
            (*c->errc)++;
        }
        if (bt.arr > 0 && n->rhs->kind == ND_INT &&
            (n->rhs->int_val < 0 || n->rhs->int_val >= bt.arr)) {
            diag_print(n->rhs->file, n->rhs->line, n->rhs->col, "error",
                       "index out of bounds: the length is %d but the index is %lld", bt.arr, n->rhs->int_val);
            (*c->errc)++;
        }
    } else if (n->kind == ND_UNARY && n->op == TK_STAR) {
        /* dereference `*x` only works on a pointer (avoids silent segfaults) */
        CType ot = infer(c->prog, n->operand, c->map, *c->nmap);
        if (ot.ptr == 0) {
            char on[128]; tc_name(ot, on);
            diag_print(n->file, n->line, n->col, "error",
                       "cannot dereference non-pointer value of type '%s'", on);
            diag_help("only pointer values (*T) can be dereferenced");
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
            /* primitive receiver (impl Display for i64): resolve by the type-name namespace,
             * but only when the base is a real variable/expression (not a module name) */
            const char *mns = NULL;
            if (bt.base == TYPE_STRUCT && bt.sname) mns = bt.sname;
            else if (bt.ptr == 0 && bt.base != TYPE_DYN && bt.base != TYPE_FUNC &&
                     bt.base != TYPE_VOID && bt.base != TYPE_UNKNOWN &&
                     (callee->operand->kind != ND_IDENT || tc_find(c, callee->operand->name)))
                mns = datatype_name(bt.base);
            if (mns) {                                      /* a method call obj.m(...) */
                for (int i = 0; i < c->prog->nitems; i++) {
                    Node *d = c->prog->items[i];
                    if (d->kind == ND_FUNC && d->is_method && d->ngen == 0 && d->ns && callee->name &&
                        strcmp(d->ns, mns) == 0 && strcmp(d->name, callee->name) == 0) { fn = d; argoff = 1; break; }
                }
            } else if (bt.ptr == 0 && bt.base == TYPE_DYN && bt.sname && callee->name) {
                /* dynamic dispatch d.m(...): the method must exist in the trait; check
                 * arguments against the trait's signature (items[0] = self) */
                Node *tr = find_trait(c->prog, bt.sname);
                if (tr) {
                    for (int i = 0; i < tr->nitems; i++)
                        if (tr->items[i]->kind == ND_FUNC && strcmp(tr->items[i]->name, callee->name) == 0) {
                            fn = tr->items[i]; argoff = 1; break;
                        }
                    if (!fn) {
                        diag_print(n->file, n->line, n->col, "error",
                                   "trait '%s' has no method '%s'", bt.sname, callee->name);
                        (*c->errc)++;
                    }
                }
            }
        }
        if (fn && fn->variadic && argoff == 0) {
            /* variadic function: fixed params, then any number of extras that must implement
             * the variadic parameter's trait (they are wrapped into dyn blobs at the call site) */
            int fixed = fn->nitems - 2;                    /* minus the slice param + hidden len */
            const char *trait = fixed >= 0 ? fn->items[fixed]->type_name : NULL;
            if (n->nitems < fixed) {
                diag_print(n->file, n->line, n->col, "error",
                           "function '%s' expects at least %d argument(s) but got %d",
                           fn->name, fixed, n->nitems);
                (*c->errc)++;
            } else {
                for (int i = 0; i < fixed; i++) {          /* fixed arguments: normal type check */
                    Node *pp = fn->items[i];
                    CType pt = { pp->type, pp->ptr, pp->type_name, pp->sig, 0 };
                    CType at = infer(c->prog, n->items[i], c->map, *c->nmap);
                    if (!tc_assignable(pt, at)) {
                        char an[128], pn[128]; tc_name(at, an); tc_name(pt, pn);
                        diag_print(n->items[i]->file, n->items[i]->line, n->items[i]->col, "error",
                                   "argument %d to '%s': cannot pass '%s' where '%s' is expected",
                                   i + 1, fn->name, an, pn);
                        (*c->errc)++;
                    }
                }
                for (int i = fixed; i < n->nitems; i++) {  /* extras: must implement the trait */
                    CType at = infer(c->prog, n->items[i], c->map, *c->nmap);
                    const char *tn = NULL;
                    if (at.ptr == 0 && at.arr == 0) {
                        if (at.base == TYPE_DYN) tn = (at.sname && trait && strcmp(at.sname, trait) == 0) ? at.sname : NULL;
                        else if (at.base == TYPE_STRUCT) tn = at.sname;
                        else if (at.base != TYPE_FUNC && at.base != TYPE_VOID && at.base != TYPE_UNKNOWN)
                            tn = datatype_name(at.base);
                    }
                    int ok = tn && (at.base == TYPE_DYN || type_impls_trait(c->prog, tn, trait));
                    if (!ok) {
                        char an[128]; tc_name(at, an);
                        diag_print(n->items[i]->file, n->items[i]->line, n->items[i]->col, "error",
                                   "argument %d to '%s': type '%s' does not implement trait '%s'",
                                   i + 1, fn->name, an, trait ? trait : "?");
                        diag_help("add 'impl %s for %s { ... }' (values are passed by reference into the variadic slice)",
                                  trait ? trait : "?", an);
                        (*c->errc)++;
                    }
                }
            }
        } else if (fn) {
            int total = fn->nitems - argoff;
            int min = df_min_args(fn, argoff);
            if (n->nitems != total) {
                /* defaults are filled before this pass; a count mismatch here is a real error */
                if (min != total)
                    diag_print(n->file, n->line, n->col, "error", "%s '%s' expects between %d and %d arguments but got %d",
                               argoff ? "method" : "function", fn->name, min, total, n->nitems);
                else
                    diag_print(n->file, n->line, n->col, "error", "%s '%s' expects %d argument(s) but got %d",
                               argoff ? "method" : "function", fn->name, total, n->nitems);
                (*c->errc)++;
            } else {
                for (int i = 0; i < n->nitems; i++) {
                    Node *pp = fn->items[i + argoff];
                    CType pt = { pp->type, pp->ptr, pp->type_name, pp->sig, 0 };
                    CType at = infer(c->prog, n->items[i], c->map, *c->nmap);
                    if (tc_is128(pt) && !tc_is128(at)) {
                        /* the callee copies 16 bytes from an address; a 64-bit value has none */
                        Node *arg = n->items[i];
                        diag_print(arg->file, arg->line, arg->col, "error",
                                   "argument %d to '%s' must be a 128-bit value", i + 1, fn->name);
                        diag_help("cast it explicitly: pass 'x as %s'", datatype_name(pp->type));
                        (*c->errc)++;
                        continue;
                    }
                    if (!tc_assignable(pt, at)) {
                        char an[128], pn[128]; tc_name(at, an); tc_name(pt, pn);
                        Node *arg = n->items[i];
                        diag_print(arg->file, arg->line, arg->col, "error",
                                   "argument %d to '%s': cannot pass '%s' where '%s' is expected",
                                   i + 1, fn->name, an, pn);
                        if ((tc_isstr(at) && tc_numeric(pt)) || (tc_numeric(at) && tc_isstr(pt)))
                            diag_help("MVS never converts between strings and numbers implicitly; use extern atoi/strtod");
                        (*c->errc)++;
                    } else {
                        tc_dyn_impl_check(c, n->items[i], pt, at);
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
            CType ret = { f->type, f->ptr, f->type_name, NULL, 0 };
            TcCtx c = { prog, map, &nmap, ret, &errc };
            seed_globals(prog, map, &nmap);            /* globals live in the base scope */
            int param_base = nmap;
            for (int j = 0; j < f->nitems; j++)        /* parameters may shadow globals */
                if (f->items[j]->kind == ND_PARAM) tc_add(&c, f->items[j]);
            tc_check(&c, f->body);
            /* unused parameters (entry file only; self and '_'-prefixed names are exempt) */
            for (int j = param_base; j < nmap; j++) {
                Bind *b = &map[j];
                if (b->used || !b->decl || b->decl->kind != ND_PARAM) continue;
                if (!b->name || b->name[0] == '_' || strcmp(b->name, "self") == 0) continue;
                if (!diag_is_primary(b->decl->file)) continue;
                diag_print(b->decl->file, b->decl->line, b->decl->col, "warning",
                           "unused parameter '%s' in '%s'", b->name, f->name);
                diag_help("prefix it with an underscore ('_%s') to silence this warning", b->name);
            }
            /* a non-void function must return a value on every path; falling off the end
             * would silently return whatever is in rax */
            if (!(f->type == TYPE_VOID && f->ptr == 0) && !always_returns(f->body)) {
                diag_print(f->file, f->line, f->col, "error",
                           "function '%s' can reach the end of its body without returning a value", f->name);
                diag_help("add a 'return' on every path, or change the return type to 'void'");
                errc++;
            }
        } else if (f->kind == ND_VAR_DECL && f->operand) {  /* also check global initializers */
            Bind map[512]; int nmap = 0;
            CType ret = { TYPE_VOID, 0, NULL, NULL, 0 };
            TcCtx c = { prog, map, &nmap, ret, &errc };
            seed_globals(prog, map, &nmap);
            tc_check(&c, f->operand);
            if (f->arr > 0) {
                /* [T; N] global: same rules as a local array declaration */
                if (f->operand->kind != ND_ARRAY_LIT) {
                    diag_print(f->file, f->line, f->col, "error",
                               "an array variable can only be initialized with an array literal [e1, e2, ...]");
                    errc++;
                } else if (f->operand->nitems != f->arr) {
                    diag_print(f->operand->file, f->operand->line, f->operand->col, "error",
                               "array literal has %d element(s) but the type needs exactly %d",
                               f->operand->nitems, f->arr);
                    errc++;
                } else {
                    CType et = { f->type, f->ptr, f->type_name, f->sig, 0 };
                    for (int j = 0; j < f->operand->nitems; j++) {
                        CType vt2 = infer(prog, f->operand->items[j], map, nmap);
                        if (!tc_assignable(et, vt2))
                            tc_err(&c, f->operand->items[j], "array element type mismatch between", et, vt2);
                    }
                }
            } else if (f->operand->kind == ND_ARRAY_LIT) {
                diag_print(f->file, f->line, f->col, "error",
                           "an array literal can only initialize a [T; N] variable");
                errc++;
            } else {
                CType dt = { f->type, f->ptr, f->type_name, NULL, 0 };
                CType vt = infer(prog, f->operand, map, nmap);
                if (!tc_assignable(dt, vt)) tc_err(&c, f, "cannot initialize global: type mismatch between", dt, vt);
            }
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
            DataType pb = datatype_from_name(d->type_name);
            if (pb != TYPE_UNKNOWN) {   /* impl on a primitive: Self = i64/f64/str/... */
                self_map[0].t.base = pb; self_map[0].t.ptr = 0; self_map[0].t.sname = NULL;
            } else {
                self_map[0].t.base = TYPE_STRUCT; self_map[0].t.ptr = 0; self_map[0].t.sname = d->type_name;
            }
            self_map[0].t.sig = NULL; self_map[0].t.arr = 0;
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
