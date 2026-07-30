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

/* forward declarations for the generic-name utilities defined further down */
static int subst_in_cname(const char *src, Bind *gmap, int ngmap, char *out, size_t on);
static int gs_split(const char *cname, char *base, size_t bn, char *args[4]);

/* Specialize a TEMPLATE method's declared type name for a concrete receiver:
 * decl_name "Option<V>" + method of HashMap<K,V> + receiver "HashMap<i64,str>"
 * yields "Option<str>" (and a bare "V" yields "str"). Returns a fresh string,
 * or NULL when nothing changes. */
static char *spec_type_name(const char *decl_name, Node *tmpl_method, const char *recv_canon) {
    char base[128];
    char *args[4];
    int nargs = gs_split(recv_canon, base, sizeof(base), args);
    if (nargs <= 0) return NULL;
    char *out = NULL;
    for (int i = 0; i < tmpl_method->ngen && i < nargs; i++)
        if (tmpl_method->gen[i] && strcmp(decl_name, tmpl_method->gen[i]) == 0) {
            out = strdup(args[i]);
            break;
        }
    if (!out && strchr(decl_name, '<')) {
        Bind bm[4];
        CType ts[4];
        int nb = 0;
        for (int i = 0; i < tmpl_method->ngen && i < nargs; i++) {
            DataType p = datatype_from_name(args[i]);
            if (p != TYPE_UNKNOWN) { ts[nb].base = p; ts[nb].sname = NULL; }
            else { ts[nb].base = TYPE_STRUCT; ts[nb].sname = args[i]; }
            ts[nb].ptr = 0; ts[nb].sig = NULL; ts[nb].arr = 0;
            bm[nb].name = tmpl_method->gen[i];
            bm[nb].t = ts[nb];
            bm[nb].is_const = 0; bm[nb].decl = NULL; bm[nb].used = 0;
            nb++;
        }
        char buf[512];
        if (subst_in_cname(decl_name, bm, nb, buf, sizeof(buf))) out = strdup(buf);
    }
    for (int i = 0; i < nargs; i++) free(args[i]);
    return out;
}

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

/* Module namespace of the function whose body the current pass is walking ("" = entry file).
 * Unqualified name lookups must mirror codegen's resolution order: functions of the
 * enclosing module win, then global (ns "") ones. A function that belongs to ANOTHER
 * module's namespace is not reachable without ns.func(...), so name-only matching
 * would bind to the wrong definition (e.g. a user 'read' vs fs.read). */
static const char *g_cur_mod = "";

/* Find a generic template by name (ngen > 0); prefers the enclosing module's template,
 * then a global one, then any (ns.f generic calls are rewritten to the instance name,
 * which keeps the template's namespace, so codegen still resolves them correctly) */
static Node *find_template(Node *prog, const char *name) {
    Node *global = NULL, *any = NULL;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind != ND_FUNC || d->ngen == 0 || strcmp(d->name, name) != 0) continue;
        if (d->is_method) continue;   /* generic-struct methods instantiate with their struct */
        const char *ns = d->ns ? d->ns : "";
        if (g_cur_mod[0] && strcmp(ns, g_cur_mod) == 0) return d;
        if (!ns[0] && !global) global = d;
        if (!any) any = d;
    }
    return global ? global : any;
}

/* Find an ND_FUNC reachable by unqualified name (generic or not) and return its return
 * type: the enclosing module's function wins, then a global (ns "") one. Functions in a
 * foreign namespace are skipped; they are only callable as ns.func(...). */
static int func_ret_type(Node *prog, const char *name, CType *out) {
    /* the inline-assembly intrinsic: its value is whatever the instructions left
     * in the result register, so it types as a plain 64-bit integer */
    if (strcmp(name, "asm") == 0) {
        out->base = TYPE_I64; out->ptr = 0; out->sname = NULL; out->sig = NULL; out->arr = 0;
        return 1;
    }
    Node *best = NULL;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind != ND_FUNC || strcmp(d->name, name) != 0) continue;
        const char *ns = d->ns ? d->ns : "";
        if (g_cur_mod[0] && strcmp(ns, g_cur_mod) == 0) { best = d; break; }
        if (!ns[0] && !best) best = d;
    }
    if (!best) return 0;
    out->base = best->type; out->ptr = best->ptr; out->sname = best->type_name; out->sig = NULL;
    return 1;
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
                /* a canonical generic receiver ("HashMap<i64,str>") holds its methods
                 * under the TEMPLATE name until instantiation; remember the arguments
                 * so the declared return type can be specialized below */
                char gbase[128];
                const char *recv_canon = NULL;
                if (prim_recv) scope = datatype_name(bt.base);
                else if (bt.base == TYPE_STRUCT && bt.sname) {
                    scope = bt.sname;
                    const char *lt = strchr(bt.sname, '<');
                    if (lt) {
                        size_t bl = (size_t)(lt - bt.sname);
                        if (bl >= sizeof(gbase)) bl = sizeof(gbase) - 1;
                        memcpy(gbase, bt.sname, bl); gbase[bl] = '\0';
                        scope = gbase;
                        recv_canon = bt.sname;
                    }
                } else if (callee->operand->kind == ND_IDENT) scope = callee->operand->name;
                Node *best = NULL;
                for (int i = 0; i < prog->nitems; i++) {
                    Node *f = prog->items[i];
                    if (f->kind != ND_FUNC || strcmp(f->name, callee->name) != 0) continue;
                    if (scope && f->ns && strcmp(f->ns, scope) == 0) { best = f; break; }
                    if (!best) best = f;
                }
                if (best) {
                    CType ct = { best->type, best->ptr, best->type_name, NULL, 0 };
                    /* specialize a template method's return type for this instance:
                     * HashMap<i64,str>::get declares Option<V> -> Option<str> */
                    if (recv_canon && best->ngen > 0 && ct.base == TYPE_STRUCT && ct.sname) {
                        char *spec = spec_type_name(ct.sname, best, recv_canon);
                        if (spec) {
                            DataType prim = datatype_from_name(spec);
                            if (prim != TYPE_UNKNOWN) { ct.base = prim; ct.sname = NULL; }
                            else ct.sname = spec;
                        }
                    }
                    return ct;
                }
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

/* The canonical text of a bound type for use INSIDE a composite generic name,
 * e.g. T=i64 -> "i64", T=String -> "String". Pointer/func bindings cannot appear
 * inside generic argument lists (the parser rejects them), so those return NULL. */
static const char *bind_canon(CType t) {
    if (t.ptr > 0 || t.base == TYPE_FUNC || t.base == TYPE_DYN) return NULL;
    if (t.base == TYPE_STRUCT) return t.sname;
    return datatype_name(t.base);
}

/* Textually substitute generic parameter names inside a composite type name:
 * "Vec<T>" with T=i64 becomes "Vec<i64>". Whole identifiers only. Returns 1 if changed */
static int subst_in_cname(const char *src, Bind *gmap, int ngmap, char *out, size_t on) {
    size_t o = 0;
    int changed = 0;
    for (const char *s = src; *s && o + 2 < on; ) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_') {
            const char *e = s;
            while ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') ||
                   (*e >= '0' && *e <= '9') || *e == '_') e++;
            size_t idl = (size_t)(e - s);
            const char *rep = NULL;
            for (int i = 0; i < ngmap; i++)
                if (strlen(gmap[i].name) == idl && strncmp(gmap[i].name, s, idl) == 0) {
                    rep = bind_canon(gmap[i].t);
                    break;
                }
            if (rep) {
                if (o + strlen(rep) + 1 >= on) return 0;
                strcpy(out + o, rep); o += strlen(rep);
                changed = 1;
            } else {
                if (o + idl + 1 >= on) return 0;
                memcpy(out + o, s, idl); o += idl;
            }
            s = e;
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = '\0';
    return changed;
}

/* Substitute the generic type parameters with concrete types in every node of the instance */
static void substitute(Node *n, Bind *gmap, int ngmap) {
    if (!n) return;
    if (n->type == TYPE_STRUCT && n->type_name) {
        int exact = 0;
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
                exact = 1;
                break;
            }
        }
        if (!exact && strchr(n->type_name, '<')) {
            /* composite name using the parameters, e.g. a local `let v: Vec<T>` */
            char buf[512];
            if (subst_in_cname(n->type_name, gmap, ngmap, buf, sizeof(buf))) {
                free(n->type_name);
                n->type_name = strdup(buf);
            }
        }
    }
    /* generic struct literals inside a template body: Option<T> { ... };
     * likewise Vec<T>::new()-style associated calls (an ident named "Vec<T>") */
    if ((n->kind == ND_STRUCT_LIT || n->kind == ND_IDENT) && n->name && strchr(n->name, '<')) {
        char buf[512];
        if (subst_in_cname(n->name, gmap, ngmap, buf, sizeof(buf))) {
            free(n->name);
            n->name = strdup(buf);
        }
    }
    /* explicit generic call arguments: none<T>() cloned into an instance becomes none<i64>() */
    for (int i = 0; i < n->ngen && n->kind == ND_IDENT; i++) {
        if (!n->gen[i]) continue;
        for (int g = 0; g < ngmap; g++)
            if (strcmp(n->gen[i], gmap[g].name) == 0) {
                const char *rep = bind_canon(gmap[g].t);
                if (rep) { free(n->gen[i]); n->gen[i] = strdup(rep); }
                break;
            }
        if (n->gen[i] && strchr(n->gen[i], '<')) {
            char buf[512];
            if (subst_in_cname(n->gen[i], gmap, ngmap, buf, sizeof(buf))) {
                free(n->gen[i]);
                n->gen[i] = strdup(buf);
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

/* ---------- enums + match (Rust-style, desugared before every other pass) ----------
 *
 * enum Shape { Circle(f64), ... }            -> tagged struct + constructors
 * enum Option<T> { None, Some(T) }           -> struct TEMPLATE + template constructors
 * match (e) { Some(v) => ..., None => ... }  -> if/else chain on __tag with typed bindings
 *
 * The pass is SCOPE-AWARE: bare patterns (Some(v) without the enum prefix) and
 * generic enums need the scrutinee's TYPE, inferred with the same machinery as
 * every other front-end pass. A match whose arms are expressions is a VALUE;
 * it may appear as an initializer, an assignment source, or a return value and
 * is distributed into the arms (target = arm-expr / return arm-expr).
 * Exhaustiveness is checked here (without '_', every variant must appear), and
 * the final arm of an exhaustive match becomes the else branch so the
 * missing-return analysis accepts all-return matches. */

static int gs_split(const char *cname, char *base, size_t bn, char *args[4]);   /* defined below */

#define MAX_ENUMS 64
typedef struct {
    char *name;
    Node *decl;               /* the ND_ENUM_DECL (variants = decl->items) */
} EnumInfo;
static EnumInfo g_enum_tab[MAX_ENUMS];
static int g_num_enums = 0;
static int de_err = 0;
static int de_tmp = 0;        /* unique scrutinee temp counter */

static EnumInfo *de_find(const char *name) {
    for (int i = 0; i < g_num_enums; i++)
        if (strcmp(g_enum_tab[i].name, name) == 0) return &g_enum_tab[i];
    return NULL;
}

/* registry lookup tolerating canonical instance names: "Option<i64>" finds the
 * Option template and (optionally) hands back the argument texts */
static EnumInfo *de_find_base(const char *name, char *args_out[4], int *nargs_out) {
    if (nargs_out) *nargs_out = 0;
    if (!strchr(name, '<')) return de_find(name);
    char base[128];
    char *args[4];
    int na = gs_split(name, base, sizeof(base), args);
    if (na < 0) return NULL;
    EnumInfo *ei = de_find(base);
    if (ei && args_out && nargs_out) {
        for (int i = 0; i < na; i++) args_out[i] = args[i];
        *nargs_out = na;
    } else {
        for (int i = 0; i < na; i++) free(args[i]);
    }
    return ei;
}

/* nodes built here must carry the SOURCE location they desugar; node_new's
 * file default is whatever file parsed last, which is wrong for diagnostics */
static Node *de_node(NodeKind kind, const Node *src) {
    Node *n = node_new(kind, src ? src->line : 0);
    if (src) { n->file = src->file; n->col = src->col; }
    return n;
}

static Node *de_ident(const char *name, const Node *src) {
    Node *n = de_node(ND_IDENT, src);
    n->name = strdup(name);
    n->type = TYPE_UNKNOWN;
    return n;
}

static Node *de_member(const char *base, const char *field, const Node *src) {
    Node *m = de_node(ND_MEMBER, src);
    m->operand = de_ident(base, src);
    m->name = strdup(field);
    return m;
}

/* substitute the enum's generic parameter names in a payload type text:
 * exact "T" -> the argument text, composite "Vec<T>" -> textual replacement */
static char *de_subst_ptype(const char *src, Node *edecl, char *args[4], int nargs) {
    for (int i = 0; i < edecl->ngen && i < nargs; i++)
        if (strcmp(src, edecl->gen[i]) == 0) return strdup(args[i]);
    if (strchr(src, '<')) {
        Bind bm[4];
        CType ts[4];
        int nb = 0;
        for (int i = 0; i < edecl->ngen && i < nargs; i++) {
            DataType p = datatype_from_name(args[i]);
            if (p != TYPE_UNKNOWN) { ts[nb].base = p; ts[nb].sname = NULL; }
            else { ts[nb].base = TYPE_STRUCT; ts[nb].sname = args[i]; }
            ts[nb].ptr = 0; ts[nb].sig = NULL; ts[nb].arr = 0;
            bm[nb].name = edecl->gen[i];
            bm[nb].t = ts[nb];
            nb++;
        }
        char buf[512];
        if (subst_in_cname(src, bm, nb, buf, sizeof(buf))) return strdup(buf);
    }
    return strdup(src);
}

/* fill a binding declaration's type from a payload type node, substituting the
 * enum's generic parameters with the scrutinee's argument texts */
static void de_bind_type(Node *bd, Node *pt, Node *edecl, char *args[4], int nargs) {
    bd->ptr = pt->ptr;
    bd->sig = pt->sig ? node_clone(pt->sig) : NULL;
    if (pt->type == TYPE_STRUCT && pt->type_name && edecl->ngen > 0) {
        char *txt = de_subst_ptype(pt->type_name, edecl, args, nargs);
        DataType p = datatype_from_name(txt);
        if (p != TYPE_UNKNOWN && bd->ptr == 0) {
            bd->type = p;
            bd->type_name = NULL;
            free(txt);
        } else if (p != TYPE_UNKNOWN) {          /* pointer to a primitive: *T with T=i64 */
            bd->type = p;
            bd->type_name = NULL;
            free(txt);
        } else {
            bd->type = TYPE_STRUCT;
            bd->type_name = txt;
        }
        return;
    }
    bd->type = pt->type;
    bd->type_name = pt->type_name ? strdup(pt->type_name) : NULL;
}

static Node *de_walk(Node *prog, Node *n, Bind *map, int *nmap);

/* insert child into parent->items at index idx (shifting the rest right) */
static void de_block_insert(Node *parent, int idx, Node *child) {
    node_add_item(parent, child);                /* grow by one */
    for (int i = parent->nitems - 1; i > idx; i--)
        parent->items[i] = parent->items[i - 1];
    parent->items[idx] = child;
}

/* Desugar one ND_MATCH. Children are walked HERE (bindings must be in scope
 * for the arm bodies). Modes:
 *   assign_target != NULL -> value match; each arm assigns its expression
 *   ret_mode != 0         -> value match; each arm returns its expression
 *   neither               -> statement match; arms must be blocks */
static Node *de_match(Node *prog, Node *n, Bind *map, int *nmap, Node *assign_target, int ret_mode) {
    int value_mode = (assign_target != NULL) || ret_mode;
    n->cond = de_walk(prog, n->cond, map, nmap);

    /* resolve the enum: a qualified arm names it; otherwise the scrutinee's type */
    EnumInfo *ei = NULL;
    char *gargs[4];
    int ngargs = 0;
    CType ct = infer(prog, n->cond, map, *nmap);
    EnumInfo *cei = NULL;
    if (ct.ptr == 0 && ct.base == TYPE_STRUCT && ct.sname)
        cei = de_find_base(ct.sname, gargs, &ngargs);
    for (int i = 0; i < n->nitems && !ei; i++)
        if (n->items[i]->type_name) {
            ei = de_find(n->items[i]->type_name);
            if (!ei) {
                diag_print(n->items[i]->file, n->items[i]->line, n->items[i]->col, "error",
                           "unknown enum '%s' in match pattern", n->items[i]->type_name);
                de_err++;
                return n;
            }
        }
    if (!ei) ei = cei;
    if (!ei) {
        diag_print(n->file, n->line, n->col, "error",
                   "cannot infer the enum type of this match; qualify a pattern as Enum::Variant");
        de_err++;
        return n;
    }
    if (cei && ei != cei) {
        diag_print(n->file, n->line, n->col, "error",
                   "the match patterns name enum '%s' but the value is of enum '%s'",
                   ei->name, cei->name);
        de_err++;
        return n;
    }
    if (ei->decl->ngen > 0 && ngargs != ei->decl->ngen) {
        diag_print(n->file, n->line, n->col, "error",
                   "cannot infer the generic arguments of enum '%s' for this match", ei->name);
        diag_help("give the scrutinee an annotated type, e.g. let r: %s<...> = ...", ei->name);
        de_err++;
        return n;
    }

    /* validate the arms */
    Node *def_arm = NULL;
    int seen[64] = {0};
    for (int i = 0; i < n->nitems; i++) {
        Node *arm = n->items[i];
        if (value_mode && arm->body && !arm->operand) {
            diag_print(arm->file, arm->line, arm->col, "error",
                       "a match used as a value needs `pattern => expression` arms");
            de_err++;
        }
        if (!value_mode && arm->operand) {
            diag_print(arm->file, arm->line, arm->col, "error",
                       "a match statement needs `pattern => { ... }` block arms (or use its value)");
            de_err++;
        }
        if (!arm->name) {
            if (def_arm) {
                diag_print(arm->file, arm->line, arm->col, "error", "duplicate '_' arm in match");
                de_err++;
            }
            def_arm = arm;
            continue;
        }
        if (arm->type_name && strcmp(arm->type_name, ei->name) != 0) {
            EnumInfo *other = de_find(arm->type_name);
            diag_print(arm->file, arm->line, arm->col, "error",
                       other ? "match arms mix enums '%s' and '%s'" : "unknown enum '%s' in match pattern (mixed with '%s')",
                       other ? ei->name : arm->type_name, other ? other->name : ei->name);
            de_err++;
            continue;
        }
        int vi = -1;
        for (int v = 0; v < ei->decl->nitems; v++)
            if (strcmp(ei->decl->items[v]->name, arm->name) == 0) { vi = v; break; }
        if (vi < 0) {
            diag_print(arm->file, arm->line, arm->col, "error",
                       "enum '%s' has no variant '%s'", ei->name, arm->name);
            de_err++;
            continue;
        }
        if (seen[vi]) {
            diag_print(arm->file, arm->line, arm->col, "error",
                       "duplicate arm for variant '%s'", arm->name);
            de_err++;
        }
        seen[vi] = 1;
        int arity = ei->decl->items[vi]->nitems;
        if (arm->nitems != arity) {
            diag_print(arm->file, arm->line, arm->col, "error",
                       "variant '%s' carries %d value(s) but the pattern binds %d",
                       arm->name, arity, arm->nitems);
            de_err++;
        }
    }
    if (!def_arm) {                              /* Rust-style exhaustiveness */
        for (int v = 0; v < ei->decl->nitems; v++) {
            if (!seen[v]) {
                diag_print(n->file, n->line, n->col, "error",
                           "match on enum '%s' is not exhaustive: variant '%s' is not covered",
                           ei->name, ei->decl->items[v]->name);
                diag_help("add an arm for it, or a final catch-all `_ => ...`");
                de_err++;
            }
        }
    }

    /* the scrutinee temp keeps the CANONICAL type for generic enums */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "__match%d", de_tmp++);
    Node *blk = de_node(ND_BLOCK, n);
    Node *decl = de_node(ND_VAR_DECL, n);
    decl->name = strdup(tmp);
    decl->type = TYPE_STRUCT;
    decl->type_name = strdup(ei->decl->ngen > 0 ? ct.sname : ei->name);
    decl->operand = n->cond;                     /* evaluated exactly once */
    node_add_item(blk, decl);
    int scope_base = *nmap;
    add_bind(map, nmap, decl);                   /* visible to nothing outside, but keeps counts honest */

    /* turn one arm into its then-block: bindings (in scope) + the body/value */
    int last_variant = -1;
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i]->name) last_variant = i;

    Node *first_if = NULL, *cur = NULL;
    for (int i = 0; i < n->nitems; i++) {
        Node *arm = n->items[i];
        if (!arm->name) continue;
        int vi = -1;
        for (int v = 0; v < ei->decl->nitems; v++)
            if (strcmp(ei->decl->items[v]->name, arm->name) == 0) { vi = v; break; }
        if (vi < 0) continue;                    /* already reported */
        Node *tb = de_node(ND_BLOCK, arm);
        Node *variant = ei->decl->items[vi];
        int arm_scope = *nmap;
        for (int j = 0; j < arm->nitems && j < variant->nitems; j++) {
            Node *pt = variant->items[j];
            Node *bd = de_node(ND_VAR_DECL, arm);
            bd->name = strdup(arm->items[j]->name);
            de_bind_type(bd, pt, ei->decl, gargs, ngargs);
            char f[32];
            snprintf(f, sizeof(f), "v%d_%d", vi, j);
            bd->operand = de_member(tmp, f, arm);
            node_add_item(tb, bd);
            add_bind(map, nmap, bd);             /* the arm body sees its bindings */
        }
        if (value_mode) {
            arm->operand = de_walk(prog, arm->operand, map, nmap);
            Node *st;
            if (ret_mode) {
                st = de_node(ND_RETURN, arm);
                st->operand = arm->operand;
            } else {
                Node *as = de_node(ND_ASSIGN, arm);
                as->op = TK_ASSIGN;
                as->lhs = node_clone(assign_target);
                as->rhs = arm->operand;
                st = de_node(ND_EXPR_STMT, arm);
                st->operand = as;
            }
            node_add_item(tb, st);
        } else {
            arm->body = de_walk(prog, arm->body, map, nmap);
            node_add_item(tb, arm->body);
        }
        *nmap = arm_scope;                       /* bindings drop out of scope */
        if (!def_arm && i == last_variant) {     /* exhaustive: final arm needs no tag test */
            if (cur) cur->else_branch = tb;
            else if (!first_if) first_if = tb;   /* a single-arm match is just its block */
            break;
        }
        Node *iff = de_node(ND_IF, arm);
        Node *cmp = de_node(ND_BINARY, arm);
        cmp->op = TK_EQ;
        cmp->lhs = de_member(tmp, "__tag", arm);
        cmp->rhs = de_node(ND_INT, arm);
        cmp->rhs->int_val = vi;
        cmp->rhs->type = TYPE_I64;
        iff->cond = cmp;
        iff->then_branch = tb;
        if (!first_if) first_if = iff;
        else cur->else_branch = iff;
        cur = iff;
    }
    if (def_arm) {
        Node *db;
        if (value_mode) {
            def_arm->operand = de_walk(prog, def_arm->operand, map, nmap);
            db = de_node(ND_BLOCK, def_arm);
            Node *st;
            if (ret_mode) {
                st = de_node(ND_RETURN, def_arm);
                st->operand = def_arm->operand;
            } else {
                Node *as = de_node(ND_ASSIGN, def_arm);
                as->op = TK_ASSIGN;
                as->lhs = node_clone(assign_target);
                as->rhs = def_arm->operand;
                st = de_node(ND_EXPR_STMT, def_arm);
                st->operand = as;
            }
            node_add_item(db, st);
        } else {
            def_arm->body = de_walk(prog, def_arm->body, map, nmap);
            db = def_arm->body;
        }
        if (cur) cur->else_branch = db;
        else if (!first_if) first_if = db;       /* '_'-only would have errored; keep safe anyway */
    }
    if (first_if) node_add_item(blk, first_if);
    *nmap = scope_base;
    for (int i = 0; i < ngargs; i++) free(gargs[i]);
    return blk;
}

/* walk a subtree with real scoping, desugaring matches and bare unit variants */
static Node *de_walk(Node *prog, Node *n, Bind *map, int *nmap) {
    if (!n) return NULL;
    switch (n->kind) {
        case ND_BLOCK: {
            int s = *nmap;
            for (int i = 0; i < n->nitems; i++) {
                Node *it = n->items[i];
                if (it->kind == ND_MATCH) {      /* a plain match statement */
                    n->items[i] = de_match(prog, it, map, nmap, NULL, 0);
                    continue;
                }
                if (it->kind == ND_VAR_DECL && it->operand && it->operand->kind == ND_MATCH) {
                    /* let x: T = match (...) { ... } -> declare, then assign per arm */
                    Node *m = it->operand;
                    it->operand = NULL;
                    add_bind(map, nmap, it);
                    Node *target = de_ident(it->name, it);
                    Node *rep = de_match(prog, m, map, nmap, target, 0);
                    de_block_insert(n, i + 1, rep);
                    i++;
                    continue;
                }
                if (it->kind == ND_RETURN && it->operand && it->operand->kind == ND_MATCH) {
                    n->items[i] = de_match(prog, it->operand, map, nmap, NULL, 1);
                    continue;
                }
                if (it->kind == ND_EXPR_STMT && it->operand && it->operand->kind == ND_ASSIGN &&
                    it->operand->op == TK_ASSIGN && it->operand->rhs &&
                    it->operand->rhs->kind == ND_MATCH) {
                    Node *as = it->operand;
                    as->lhs = de_walk(prog, as->lhs, map, nmap);
                    n->items[i] = de_match(prog, as->rhs, map, nmap, as->lhs, 0);
                    continue;
                }
                n->items[i] = de_walk(prog, it, map, nmap);
            }
            *nmap = s;
            return n;
        }
        case ND_VAR_DECL:
            n->operand = de_walk(prog, n->operand, map, nmap);
            add_bind(map, nmap, n);
            return n;
        case ND_FOR: {
            int s = *nmap;
            n->init = de_walk(prog, n->init, map, nmap);
            n->cond = de_walk(prog, n->cond, map, nmap);
            n->step = de_walk(prog, n->step, map, nmap);
            n->body = de_walk(prog, n->body, map, nmap);
            *nmap = s;
            return n;
        }
        case ND_IF: {
            n->cond = de_walk(prog, n->cond, map, nmap);
            int s1 = *nmap; n->then_branch = de_walk(prog, n->then_branch, map, nmap); *nmap = s1;
            int s2 = *nmap; n->else_branch = de_walk(prog, n->else_branch, map, nmap); *nmap = s2;
            return n;
        }
        case ND_WHILE: case ND_DOWHILE: {
            n->cond = de_walk(prog, n->cond, map, nmap);
            int s = *nmap; n->body = de_walk(prog, n->body, map, nmap); *nmap = s;
            return n;
        }
        case ND_SWITCH: case ND_CASE: {
            n->cond = de_walk(prog, n->cond, map, nmap);
            n->operand = de_walk(prog, n->operand, map, nmap);
            int s = *nmap;
            for (int i = 0; i < n->nitems; i++) n->items[i] = de_walk(prog, n->items[i], map, nmap);
            *nmap = s;
            return n;
        }
        case ND_MATCH:
            /* reached through an expression slot: not a supported position */
            diag_print(n->file, n->line, n->col, "error",
                       "a match value may only initialize a variable, be assigned, or be returned");
            de_err++;
            return n;
        default: break;
    }
    /* the callee of a call must not be auto-wrapped (Cmd::Go(30) is a constructor call) */
    if (n->kind == ND_CALL && n->operand && n->operand->kind == ND_MEMBER) {
        n->operand->operand = de_walk(prog, n->operand->operand, map, nmap);
    } else {
        n->operand = de_walk(prog, n->operand, map, nmap);
    }
    n->lhs = de_walk(prog, n->lhs, map, nmap);
    n->rhs = de_walk(prog, n->rhs, map, nmap);
    n->cond = de_walk(prog, n->cond, map, nmap);
    n->then_branch = de_walk(prog, n->then_branch, map, nmap);
    n->else_branch = de_walk(prog, n->else_branch, map, nmap);
    n->init = de_walk(prog, n->init, map, nmap);
    n->step = de_walk(prog, n->step, map, nmap);
    n->body = de_walk(prog, n->body, map, nmap);
    for (int i = 0; i < n->nitems; i++) n->items[i] = de_walk(prog, n->items[i], map, nmap);
    if (n->kind == ND_MEMBER && n->operand && n->operand->kind == ND_IDENT && n->name) {
        /* bare unit variant (Signal::Red, Option<T>::None): wrap into its constructor call */
        EnumInfo *ei = de_find_base(n->operand->name, NULL, NULL);
        if (ei) {
            for (int v = 0; v < ei->decl->nitems; v++) {
                if (strcmp(ei->decl->items[v]->name, n->name) != 0) continue;
                if (ei->decl->items[v]->nitems == 0) {
                    Node *call = de_node(ND_CALL, n);
                    call->operand = n;
                    return call;
                }
                diag_print(n->file, n->line, n->col, "error",
                           "variant '%s' carries %d value(s); construct it as %s::%s(...)",
                           n->name, ei->decl->items[v]->nitems, ei->name, n->name);
                de_err++;
                return n;
            }
        }
    }
    return n;
}

int desugar_enums(Node *prog) {
    de_err = 0;
    g_num_enums = 0;
    /* 1) register every enum declaration */
    for (int i = 0; i < prog->nitems; i++) {
        Node *e = prog->items[i];
        if (e->kind != ND_ENUM_DECL || !e->name) continue;
        if (de_find(e->name)) {
            diag_print(e->file, e->line, e->col, "error", "duplicate enum '%s'", e->name);
            de_err++;
            continue;
        }
        if (g_num_enums >= MAX_ENUMS) {
            diag_print(e->file, e->line, e->col, "error", "too many enums (max %d)", MAX_ENUMS);
            de_err++;
            break;
        }
        if (e->nitems > 64) {
            /* do NOT register: later stages index per-variant arrays by 64 */
            diag_print(e->file, e->line, e->col, "error", "enum '%s' has too many variants (max 64)", e->name);
            de_err++;
            continue;
        }
        g_enum_tab[g_num_enums].name = e->name;
        g_enum_tab[g_num_enums].decl = e;
        g_num_enums++;
    }
    /* 2) desugar matches everywhere: function bodies (scope-aware, with globals
     * and parameters bound), global initializers, and trait default methods */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind == ND_FUNC && f->body) {
            Bind map[512]; int nmap = 0;
            seed_globals(prog, map, &nmap);
            for (int j = 0; j < f->nitems; j++)
                if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
            f->body = de_walk(prog, f->body, map, &nmap);
        } else if (f->kind == ND_VAR_DECL && f->operand) {
            if (f->operand->kind == ND_MATCH) {
                diag_print(f->file, f->line, f->col, "error",
                           "a match value cannot initialize a global variable");
                de_err++;
                continue;
            }
            Bind map[512]; int nmap = 0;
            seed_globals(prog, map, &nmap);
            f->operand = de_walk(prog, f->operand, map, &nmap);
        } else if (f->kind == ND_TRAIT) {
            for (int j = 0; j < f->nitems; j++) {
                Node *m = f->items[j];
                if (m->kind != ND_FUNC || !m->body) continue;
                Bind map[512]; int nmap = 0;
                seed_globals(prog, map, &nmap);
                for (int k = 0; k < m->nitems; k++)
                    if (m->items[k]->kind == ND_PARAM) add_bind(map, &nmap, m->items[k]);
                m->body = de_walk(prog, m->body, map, &nmap);
            }
        }
    }
    /* 3) turn each enum declaration into a (possibly generic) tagged struct
     * plus one associated constructor per variant */
    for (int i = 0; i < prog->nitems; i++) {
        Node *e = prog->items[i];
        if (e->kind != ND_ENUM_DECL || !e->name) continue;
        /* the canonical self-type: "Shape" or "Option<T>" for templates */
        char canon[256];
        if (e->ngen > 0) {
            size_t cl = (size_t)snprintf(canon, sizeof(canon), "%s<", e->name);
            for (int gi = 0; gi < e->ngen; gi++)
                cl += (size_t)snprintf(canon + cl, sizeof(canon) - cl, "%s%s", gi ? "," : "", e->gen[gi]);
            snprintf(canon + cl, sizeof(canon) - cl, ">");
        } else {
            snprintf(canon, sizeof(canon), "%s", e->name);
        }
        Node *st = de_node(ND_STRUCT_DECL, e);
        st->name = strdup(e->name);
        st->ngen = e->ngen;
        for (int gi = 0; gi < e->ngen; gi++) st->gen[gi] = strdup(e->gen[gi]);
        Node *tagf = de_node(ND_PARAM, e);
        tagf->name = strdup("__tag");     /* double underscore: not meant to be touched */
        tagf->type = TYPE_I64;
        node_add_item(st, tagf);
        for (int v = 0; v < e->nitems; v++) {
            Node *variant = e->items[v];
            for (int j = 0; j < variant->nitems; j++) {
                Node *pt = variant->items[j];
                Node *fld = de_node(ND_PARAM, variant);
                char f[32];
                snprintf(f, sizeof(f), "v%d_%d", v, j);
                fld->name = strdup(f);
                fld->type = pt->type;
                fld->ptr = pt->ptr;
                fld->type_name = pt->type_name ? strdup(pt->type_name) : NULL;
                fld->sig = pt->sig ? node_clone(pt->sig) : NULL;
                node_add_item(st, fld);
            }
        }
        prog->items[i] = st;                     /* the struct replaces the enum in place */
        /* one associated constructor per variant: Shape::Circle(p0) -> Shape;
         * for a generic enum these are struct-TEMPLATE methods, instantiated
         * together with each Option<i64>-style instance */
        for (int v = 0; v < e->nitems; v++) {
            Node *variant = e->items[v];
            Node *fn = de_node(ND_FUNC, variant);
            fn->name = strdup(variant->name);
            fn->is_method = 1;
            fn->ns = strdup(e->name);
            fn->ngen = e->ngen;
            for (int gi = 0; gi < e->ngen; gi++) fn->gen[gi] = strdup(e->gen[gi]);
            fn->type = TYPE_STRUCT;
            fn->type_name = strdup(canon);
            Node *lit = de_node(ND_STRUCT_LIT, variant);
            lit->name = strdup(canon);
            Node *tset = de_node(ND_ASSIGN, variant);
            tset->lhs = de_ident("__tag", variant);
            tset->rhs = de_node(ND_INT, variant);
            tset->rhs->int_val = v;
            tset->rhs->type = TYPE_I64;
            node_add_item(lit, tset);
            for (int j = 0; j < variant->nitems; j++) {
                Node *pt = variant->items[j];
                Node *par = de_node(ND_PARAM, variant);
                char pn[16];
                snprintf(pn, sizeof(pn), "p%d", j);
                par->name = strdup(pn);
                par->type = pt->type;
                par->ptr = pt->ptr;
                par->type_name = pt->type_name ? strdup(pt->type_name) : NULL;
                par->sig = pt->sig ? node_clone(pt->sig) : NULL;
                node_add_item(fn, par);
                Node *fset = de_node(ND_ASSIGN, variant);
                char f[32];
                snprintf(f, sizeof(f), "v%d_%d", v, j);
                fset->lhs = de_ident(f, variant);
                fset->rhs = de_ident(pn, variant);
                node_add_item(lit, fset);
            }
            Node *ret = de_node(ND_RETURN, variant);
            ret->operand = lit;
            Node *body = de_node(ND_BLOCK, variant);
            node_add_item(body, ret);
            fn->body = body;
            node_add_item(prog, fn);
        }
    }
    return de_err;
}

/* ---------- generic structs (struct Vec<T>) ----------
 *
 * Struct templates (ND_STRUCT_DECL with ngen > 0) never reach codegen. Every
 * concrete use site carries a canonical name like "Vec<i64>" (built by the
 * parser or by substitute() inside function/method instances); the sweep below
 * finds those names, clones the template with the parameters substituted, adds
 * the instance under a mangled name ("Vec__i64"), instantiates the template's
 * methods for it, and rewrites the use site to the mangled name. Runs inside
 * the monomorphize fixpoint loop so functions and structs can instantiate
 * each other in any order. */

static void gs_resolve_node(Node *prog, Node *n, int *made);

/* "Vec<i64,str>" -> "Vec__i64_str" (angle brackets never reach assembly labels) */
static void gs_mangle(const char *cname, char *out, size_t on) {
    size_t o = 0;
    for (const char *s = cname; *s && o + 3 < on; s++) {
        if (*s == '<') { out[o++] = '_'; out[o++] = '_'; }
        else if (*s == ',') out[o++] = '_';
        else if (*s == '>') { /* dropped */ }
        else out[o++] = *s;
    }
    out[o] = '\0';
}

/* find a struct declaration by exact name (template or instance) */
static Node *gs_find_struct(Node *prog, const char *name) {
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_STRUCT_DECL && d->name && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

/* split "Vec<i64,Pair<u8,str>>" into base "Vec" and top-level args (strdup'd) */
static int gs_split(const char *cname, char *base, size_t bn, char *args[4]) {
    const char *lt = strchr(cname, '<');
    if (!lt) return -1;
    size_t bl = (size_t)(lt - cname);
    if (bl + 1 > bn) return -1;
    memcpy(base, cname, bl); base[bl] = '\0';
    int nargs = 0, depth = 0;
    const char *s = lt + 1, *start = lt + 1;
    for (; *s; s++) {
        if (*s == '<') depth++;
        else if (*s == '>' && depth > 0) depth--;
        else if ((*s == ',' && depth == 0) || (*s == '>' && depth == 0)) {
            if (nargs >= 4) return -1;
            size_t al = (size_t)(s - start);
            args[nargs] = (char *)malloc(al + 1);
            memcpy(args[nargs], start, al); args[nargs][al] = '\0';
            nargs++;
            start = s + 1;
            if (*s == '>') break;
        }
    }
    return nargs;
}

static void gs_instantiate(Node *prog, const char *cname, char *out_mangled, size_t on,
                           int *made, const char *file, int line, int col);

/* canonical argument text -> a concrete CType (instantiating nested generics) */
static CType gs_text_type(Node *prog, const char *text, int *made,
                          const char *file, int line, int col) {
    CType t = { TYPE_UNKNOWN, 0, NULL, NULL, 0 };
    DataType prim = datatype_from_name(text);
    if (prim != TYPE_UNKNOWN) { t.base = prim; return t; }
    t.base = TYPE_STRUCT;
    if (strchr(text, '<')) {
        char m[256];
        gs_instantiate(prog, text, m, sizeof(m), made, file, line, col);
        t.sname = strdup(m);
    } else {
        t.sname = strdup(text);
    }
    return t;
}

/* ensure an instance exists for canonical cname; writes the mangled name to out_mangled */
static void gs_instantiate(Node *prog, const char *cname, char *out_mangled, size_t on,
                           int *made, const char *file, int line, int col) {
    gs_mangle(cname, out_mangled, on);
    if (gs_find_struct(prog, out_mangled)) return;      /* already instantiated */

    char base[128]; char *args[4];
    int nargs = gs_split(cname, base, sizeof(base), args);
    if (nargs < 0) {
        diag_print(file, line, col, "error", "malformed generic type name '%s'", cname);
        mono_err++;
        return;
    }
    Node *tmpl = gs_find_struct(prog, base);
    if (!tmpl || tmpl->ngen == 0) {
        diag_print(file, line, col, "error", "unknown generic struct '%s'", base);
        if (tmpl) diag_help("'%s' is declared without generic parameters", base);
        mono_err++;
        for (int i = 0; i < nargs; i++) free(args[i]);
        return;
    }
    if (nargs != tmpl->ngen) {
        diag_print(file, line, col, "error",
                   "generic struct '%s' takes %d type argument(s) but got %d", base, tmpl->ngen, nargs);
        mono_err++;
        for (int i = 0; i < nargs; i++) free(args[i]);
        return;
    }

    /* register the instance BEFORE resolving its fields so self-referential
     * templates (struct Node<T> { next: *Node<T>; }) terminate */
    Node *inst = node_clone(tmpl);
    free(inst->name);
    inst->name = strdup(out_mangled);
    free(inst->type_name);
    inst->type_name = strdup(cname);   /* the pretty name, shown by io.out */
    inst->ngen = 0;
    node_add_item(prog, inst);
    (*made)++;

    Bind gmap[4];
    for (int i = 0; i < nargs; i++) {
        gmap[i].name = tmpl->gen[i];
        gmap[i].t = gs_text_type(prog, args[i], made, file, line, col);
        gmap[i].is_const = 0; gmap[i].decl = NULL; gmap[i].used = 0;
    }
    substitute(inst, gmap, nargs);            /* field types: T -> the argument */
    gs_resolve_node(prog, inst, made);        /* nested composite fields -> mangled names */

    /* instantiate the template's methods for this instance (impl Vec<T> { ... }) */
    int nprog = prog->nitems;
    for (int i = 0; i < nprog; i++) {
        Node *m = prog->items[i];
        if (m->kind != ND_FUNC || !m->is_method || m->ngen != tmpl->ngen || !m->ns) continue;
        if (strcmp(m->ns, base) != 0) continue;
        Node *mi = node_clone(m);
        mi->ngen = 0;
        free(mi->ns);
        mi->ns = strdup(out_mangled);
        Bind mm[4];
        for (int j = 0; j < m->ngen; j++) {   /* the method carries the SAME param names */
            mm[j].name = m->gen[j];
            mm[j].t = gmap[j].t;
            mm[j].is_const = 0; mm[j].decl = NULL; mm[j].used = 0;
        }
        substitute(mi, mm, m->ngen);
        gs_resolve_node(prog, mi, made);
        node_add_item(prog, mi);
        (*made)++;
    }
    for (int i = 0; i < nargs; i++) free(args[i]);
}

/* rewrite every concrete "Base<...>" type reference in a subtree to its mangled
 * instance name, instantiating on first sight */
static void gs_resolve_node(Node *prog, Node *n, int *made) {
    if (!n) return;
    if (n->type == TYPE_STRUCT && n->type_name && strchr(n->type_name, '<')) {
        char m[256];
        gs_instantiate(prog, n->type_name, m, sizeof(m), made, n->file, n->line, n->col);
        free(n->type_name);
        n->type_name = strdup(m);
    }
    if ((n->kind == ND_STRUCT_LIT || n->kind == ND_IDENT) && n->name && strchr(n->name, '<')) {
        /* struct literal Vec<i64>{...} or the base of Vec<i64>::new(...) */
        char m[256];
        gs_instantiate(prog, n->name, m, sizeof(m), made, n->file, n->line, n->col);
        free(n->name);
        n->name = strdup(m);
    }
    gs_resolve_node(prog, n->lhs, made);   gs_resolve_node(prog, n->rhs, made);
    gs_resolve_node(prog, n->operand, made); gs_resolve_node(prog, n->cond, made);
    gs_resolve_node(prog, n->then_branch, made); gs_resolve_node(prog, n->else_branch, made);
    gs_resolve_node(prog, n->init, made);  gs_resolve_node(prog, n->step, made);
    gs_resolve_node(prog, n->body, made);  gs_resolve_node(prog, n->sig, made);
    for (int i = 0; i < n->nitems; i++) gs_resolve_node(prog, n->items[i], made);
}

/* one sweep over every CONCRETE item (templates wait for their instances) */
static int gs_sweep(Node *prog) {
    int made = 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_FUNC && d->ngen == 0) gs_resolve_node(prog, d, &made);
        else if (d->kind == ND_VAR_DECL) gs_resolve_node(prog, d, &made);
        else if (d->kind == ND_STRUCT_DECL && d->ngen == 0) gs_resolve_node(prog, d, &made);
    }
    return made;
}

/* ---------- main driver ---------- */

/* The type the surrounding code EXPECTS from the expression being scanned:
 * the declared type of a variable being initialized, or the return type of the
 * function containing a return statement. A generic call uses it to bind type
 * parameters that appear in the template's return type, so
 *     let o: Option<u16> = Some(400);
 * infers T = u16 from the annotation instead of i64 from the literal. */
static const char *g_expect_sname = NULL;
static const char *g_cur_ret_sname = NULL;

/* The pretty canonical name of a struct instance ("Option__u16" -> "Option<u16>"),
 * which the instantiation stored in the declaration's type_name */
static const char *canon_of_instance(Node *prog, const char *mangled) {
    if (!mangled) return NULL;
    if (strchr(mangled, '<')) return mangled;          /* already canonical */
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        if (d->kind == ND_STRUCT_DECL && d->name && strcmp(d->name, mangled) == 0)
            return d->type_name;                        /* NULL for a plain struct */
    }
    return NULL;
}

/* Unify a template's declared return type with the expected type and fill the
 * matching generic parameters. Returns a bitmask of the parameters it bound. */
static int unify_ret_with_expected(Node *prog, Node *tmpl, const char *expect_sname, Bind *gmap) {
    if (!expect_sname || !tmpl->type_name || tmpl->type != TYPE_STRUCT || tmpl->ptr != 0) return 0;
    const char *want = canon_of_instance(prog, expect_sname);
    if (!want || !strchr(want, '<') || !strchr(tmpl->type_name, '<')) return 0;

    char wbase[128], tbase[128];
    char *wargs[4], *targs[4];
    int wn = gs_split(want, wbase, sizeof(wbase), wargs);
    int tn = gs_split(tmpl->type_name, tbase, sizeof(tbase), targs);
    int bound = 0;
    if (wn > 0 && wn == tn && strcmp(wbase, tbase) == 0) {
        for (int i = 0; i < tn; i++) {
            for (int gi = 0; gi < tmpl->ngen; gi++) {
                if (!tmpl->gen[gi] || strcmp(targs[i], tmpl->gen[gi]) != 0) continue;
                int made = 0;
                gmap[gi].name = tmpl->gen[gi];
                gmap[gi].t = gs_text_type(prog, wargs[i], &made, tmpl->file, tmpl->line, tmpl->col);
                gmap[gi].is_const = 0; gmap[gi].decl = NULL; gmap[gi].used = 0;
                bound |= 1 << gi;
                break;
            }
        }
    }
    for (int i = 0; i < wn; i++) free(wargs[i]);
    for (int i = 0; i < tn; i++) free(targs[i]);
    return bound;
}

/* Scan nodes in a concrete function for generic calls, then instantiate + rename the call sites.
 * Scope-aware (map/nmap grows with declarations) so argument types infer correctly under shadowing.
 * Returns the number of new instances created (used for the fixpoint check) */
static int scan_calls(Node *prog, Node *n, Bind *map, int *nmap, int *made) {
    if (!n) return *made;
    switch (n->kind) {                          /* handle scopes first */
        case ND_BLOCK: { int s = *nmap; for (int i=0;i<n->nitems;i++) scan_calls(prog,n->items[i],map,nmap,made); *nmap=s; return *made; }
        case ND_VAR_DECL: {
            /* the declared type is what the initializer is expected to produce */
            const char *save = g_expect_sname;
            g_expect_sname = (n->type == TYPE_STRUCT && n->ptr == 0) ? n->type_name : NULL;
            scan_calls(prog,n->operand,map,nmap,made);
            g_expect_sname = save;
            add_bind(map,nmap,n);
            return *made;
        }
        case ND_RETURN: {
            /* a returned expression is expected to have the function's return type */
            const char *save = g_expect_sname;
            g_expect_sname = g_cur_ret_sname;
            scan_calls(prog,n->operand,map,nmap,made);
            g_expect_sname = save;
            return *made;
        }
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
            /* Type::assoc(...) is NOT a generic free call even when a same-named
             * generic function exists (Option__i64::Some vs the helper Some<T>);
             * only a real namespace base (module) may be one */
            int type_base = 0;
            if (n->operand->operand->kind == ND_IDENT && n->operand->operand->name)
                type_base = gs_find_struct(prog, n->operand->operand->name) != NULL;
            if (bt.base != TYPE_STRUCT && !type_base) is_gcall = 1; /* ns.f(...), not obj.method() */
        }
    }
    if (is_gcall) {
        Node *tmpl = find_template(prog, n->operand->name);
        if (!tmpl && n->operand->ngen > 0) {
            /* explicit type arguments on a call that resolves to no generic function:
             * silently dropping them would change the call's meaning behind the
             * programmer's back, so it is an error when the name exists at all
             * (a fully unknown name gets the normal undefined-function error later) */
            for (int i = 0; i < prog->nitems; i++) {
                Node *d = prog->items[i];
                if (d->kind == ND_FUNC && d->name && strcmp(d->name, n->operand->name) == 0) {
                    diag_print(n->file, n->line, n->col, "error",
                               "'%s' takes no generic type arguments (it is not a generic function)",
                               n->operand->name);
                    mono_err++;
                    break;
                }
            }
        }
        if (tmpl) {
            Bind gmap[4];
            if (n->operand->ngen > 0) {
                /* explicit type arguments: none<i64>() overrides inference entirely */
                if (n->operand->ngen != tmpl->ngen) {
                    diag_print(n->file, n->line, n->col, "error",
                               "'%s' takes %d type argument(s) but got %d",
                               tmpl->name, tmpl->ngen, n->operand->ngen);
                    mono_err++;
                    return *made;
                }
                int gmade = 0;
                for (int gi = 0; gi < tmpl->ngen; gi++) {
                    gmap[gi].name = tmpl->gen[gi];
                    gmap[gi].t = gs_text_type(prog, n->operand->gen[gi], &gmade,
                                              n->file, n->line, n->col);
                }
                *made += gmade;
            } else {
            /* Infer each generic parameter's type from the arguments */
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
            /* the expected type wins over argument inference for the parameters
             * it determines: `let o: Option<u16> = Some(400)` must bind T = u16,
             * not i64 from the literal */
            unify_ret_with_expected(prog, tmpl, g_expect_sname, gmap);
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
            /* Rewrite the call site to point at the instance; the explicit type
             * arguments are consumed here (a later fixpoint round must see a
             * plain concrete call, not "instance name + leftover type args") */
            free(n->operand->name); n->operand->name = strdup(mangled);
            for (int gi = 0; gi < n->operand->ngen; gi++) { free(n->operand->gen[gi]); n->operand->gen[gi] = NULL; }
            n->operand->ngen = 0;
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

/* (unqualified overload calls use g_cur_mod: inside std/math, abs(x) must see
 * math's overload set before the global one) */

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
            s = ov_find(sets, nsets, g_cur_mod, callee->name);
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
        /* 'asm' is the inline-assembly intrinsic: a user function by that name
         * would be silently shadowed by the emitter, so reject it outright */
        if (a->kind == ND_FUNC && a->name && !a->is_method && strcmp(a->name, "asm") == 0) {
            diag_print(a->file, a->line, a->col, "error",
                       "'asm' is a reserved intrinsic (inline assembly) and cannot be declared");
            errc++;
        }
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
    /* The overload table is sized to THIS program: one slot per eligible
     * function is always enough (a fixed ceiling here used to reject large
     * programs with a message about overloading, which was doubly wrong: they
     * had no overloads, and the limit was not about overloads at all). */
    int cap = 0;
    for (int i = 0; i < prog->nitems; i++)
        if (ov_eligible(prog->items[i])) cap++;
    if (cap == 0) return;
    OvSet *sets = (OvSet *)calloc((size_t)cap, sizeof(OvSet));
    if (!sets) {
        fprintf(stderr, "error: out of memory building the overload table (%d function names)\n", cap);
        return;
    }
    int nsets = 0;
    /* 1) group functions by namespace + original name (each module has its own sets) */
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (!ov_eligible(f)) continue;
        int si = ov_find(sets, nsets, f->ns, f->name);
        if (si < 0) {
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
            g_cur_mod = f->mod ? f->mod : "";   /* unqualified calls inside a module see its sets first */
            scan_ov(prog, f->body, map, &nmap, sets, nsets);
            g_cur_mod = "";
        } else if (f->kind == ND_VAR_DECL && f->operand) {
            Bind map[512]; int nmap = 0;   /* use a real map (avoids NULL deref if the init has a block/decl) */
            seed_globals(prog, map, &nmap);
            scan_ov(prog, f->operand, map, &nmap, sets, nsets);
        }
    }
    free(sets);   /* the renames are already in the AST; the table itself is done */
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

/* The single non-extern function reachable by this unqualified name, or NULL if
 * none/overloaded. Mirrors codegen resolution: the enclosing module's functions
 * win over global ones; foreign-namespace functions are not candidates at all. */
static Node *df_find_unique(Node *prog, const char *name) {
    Node *modf = NULL, *globf = NULL;
    int nmod = 0, nglob = 0;
    for (int i = 0; i < prog->nitems; i++) {
        Node *f = prog->items[i];
        if (f->kind != ND_FUNC || f->is_extern || f->is_method || !f->name) continue;
        if (strcmp(f->name, name) != 0) continue;
        const char *ns = f->ns ? f->ns : "";
        if (g_cur_mod[0] && strcmp(ns, g_cur_mod) == 0) { modf = f; nmod++; }
        else if (!ns[0]) { globf = f; nglob++; }
    }
    if (nmod == 1) return modf;                      /* module-local match wins */
    if (nmod == 0 && nglob == 1) return globf;       /* else the sole global one */
    return NULL;                                     /* none or overloaded: leave the call alone */
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
            /* this pass runs BEFORE monomorphization, so a generic receiver still
             * carries its canonical name ("Counter<i64>"); the methods live under
             * the TEMPLATE name ("Counter") until instantiation */
            char mbase[128];
            const char *mns = bt.sname;
            const char *lt = strchr(bt.sname, '<');
            if (lt) {
                size_t bl = (size_t)(lt - bt.sname);
                if (bl >= sizeof(mbase)) bl = sizeof(mbase) - 1;
                memcpy(mbase, bt.sname, bl); mbase[bl] = '\0';
                mns = mbase;
            }
            for (int i = 0; i < prog->nitems; i++) {
                Node *d = prog->items[i];
                if (d->kind == ND_FUNC && d->is_method && d->ns && callee->name &&
                    strcmp(d->ns, mns) == 0 && strcmp(d->name, callee->name) == 0) { fn = d; argoff = 1; break; }
            }
        } else if (callee->operand->kind == ND_IDENT && callee->name) {
            /* ns.f(...): fill defaults for a module function too, but only when the base
             * is really a namespace (not a variable) and exactly one candidate exists */
            int bound = 0;
            for (int i = nmap - 1; i >= 0; i--)
                if (strcmp(map[i].name, callee->operand->name) == 0) { bound = 1; break; }
            if (!bound) {
                int cnt = 0;
                for (int i = 0; i < prog->nitems; i++) {
                    Node *d = prog->items[i];
                    if (d->kind == ND_FUNC && !d->is_extern && !d->is_method &&
                        d->ns && strcmp(d->ns, callee->operand->name) == 0 &&
                        d->name && strcmp(d->name, callee->name) == 0) { fn = d; cnt++; }
                }
                if (cnt != 1) fn = NULL;             /* overloaded: leave the call alone */
            }
        }
    }
    /* generic FUNCTIONS are not default-fillable (their defaults would bypass
     * inference), but generic-STRUCT methods are: the default expression is a
     * plain value cloned into the caller, and the instance keeps the same list */
    if (!fn || (fn->ngen > 0 && !fn->is_method) || fn->variadic) return;
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
            g_cur_mod = f->mod ? f->mod : "";
            df_walk(prog, f->body, map, &nmap);
            g_cur_mod = "";
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
        /* `a == b` on two str values compares ADDRESSES (a str is a pointer),
         * which is almost never what the programmer means. Comparing against a
         * literal 0 stays silent: that is the idiomatic null check. */
        if ((op == TK_EQ || op == TK_NEQ) &&
            lt.ptr == 0 && rt.ptr == 0 && lt.base == TYPE_STR && rt.base == TYPE_STR &&
            diag_is_primary(n->file)) {
            diag_print(n->file, n->line, n->col, "warning",
                       "comparing two 'str' values compares their addresses, not their text");
            diag_help("compare the contents instead: cstr.eq(a, b) from \"core\", or strcmp(a, b) == 0");
        }
        /* a CONSTANT division or modulo by zero is a program bug the compiler
         * can see: reject it here rather than let the hardware trap at run time
         * (a constant array index out of range is already rejected the same way) */
        if ((op == TK_SLASH || op == TK_PERCENT) && n->rhs->kind == ND_INT && n->rhs->int_val == 0) {
            diag_print(n->file, n->line, n->col, "error",
                       op == TK_SLASH ? "division by zero" : "remainder by zero");
            diag_help("the divisor is the literal 0; integer division by zero traps at run time");
            (*c->errc)++;
        }
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
            /* unqualified call: enclosing module's function wins, then a global one
             * (a foreign module's ns function is NOT what this call site will run) */
            for (int i = 0; i < c->prog->nitems; i++) {
                Node *d = c->prog->items[i];
                if (d->kind != ND_FUNC || d->is_extern || d->ngen != 0 || d->is_method ||
                    !d->name || strcmp(d->name, callee->name) != 0) continue;
                const char *ns = d->ns ? d->ns : "";
                if (g_cur_mod[0] && strcmp(ns, g_cur_mod) == 0) { fn = d; break; }
                if (!ns[0] && !fn) fn = d;
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
            } else if (callee->operand->kind == ND_IDENT && callee->name &&
                       !tc_find(c, callee->operand->name)) {
                /* ns.f(...): a module function called through its namespace. Externs are
                 * skipped on purpose (their C signatures may be variadic or shortened,
                 * e.g. printf declared with one parameter) and intrinsics like io.out
                 * simply have no ND_FUNC to match. */
                for (int i = 0; i < c->prog->nitems; i++) {
                    Node *d = c->prog->items[i];
                    if (d->kind == ND_FUNC && !d->is_extern && !d->is_method && d->ngen == 0 &&
                        d->ns && strcmp(d->ns, callee->operand->name) == 0 &&
                        d->name && strcmp(d->name, callee->name) == 0) { fn = d; break; }
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
            g_cur_mod = f->mod ? f->mod : "";          /* unqualified lookups resolve module-first */
            tc_check(&c, f->body);
            g_cur_mod = "";
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
    /* loop until no new instances appear (supports generics calling generics,
     * and generic functions/structs instantiating each other in either order) */
    for (int round = 0; round < 64; round++) {
        int made = 0;
        made += gs_sweep(prog);    /* struct templates: resolve Vec<i64>-style names first */
        int nfuncs = prog->nitems; /* scan only what existed at round start (new instances wait a round) */
        for (int i = 0; i < nfuncs; i++) {
            Node *f = prog->items[i];
            if (f->kind == ND_FUNC && f->ngen == 0 && f->body) {
                Bind map[512]; int nmap = 0;
                seed_globals(prog, map, &nmap);
                for (int j = 0; j < f->nitems; j++) if (f->items[j]->kind == ND_PARAM) add_bind(map, &nmap, f->items[j]);
                g_cur_mod = f->mod ? f->mod : "";
                /* return statements inside expect the function's return type */
                g_cur_ret_sname = (f->type == TYPE_STRUCT && f->ptr == 0) ? f->type_name : NULL;
                scan_calls(prog, f->body, map, &nmap, &made);
                g_cur_ret_sname = NULL;
                g_cur_mod = "";
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
