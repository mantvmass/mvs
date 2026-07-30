/*
 * common.c - backend core (architecture independent)
 * See the overview comment in common.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

/* ---------- general helper functions ---------- */

/* Issue a new unique label number */
int new_label(Gen *g) { return g->label_id++; }

/* Write a function's label name into the buffer according to these rules:
 *   - extern (foreign): raw name, e.g. printf (to match the symbol in the C runtime)
 *   - main (empty namespace): keep the name main so the C runtime can call it
 *   - has a namespace: mvs_<ns>_<name>   e.g. io.out -> mvs_io_out
 *   - no namespace: mvs_<name>           e.g. factorial -> mvs_factorial */
void func_label_of(Node *fn, char *buf) {
    /* extern and export use the raw symbol name so C can match/call it (buf must be LABEL_MAX bytes) */
    if (fn->is_extern || fn->is_export) { snprintf(buf, LABEL_MAX, "%s", fn->name); return; }
    const char *ns = fn->ns ? fn->ns : "";
    if (ns[0] == 0 && strcmp(fn->name, "main") == 0) { snprintf(buf, LABEL_MAX, "main"); return; }
    if (ns[0]) snprintf(buf, LABEL_MAX, "mvs_%s_%s", ns, fn->name);   /* methods use ns = struct name */
    else       snprintf(buf, LABEL_MAX, "mvs_%s", fn->name);
}

/* Look up a function by (namespace, name); an empty namespace means global level */
Node *find_func(Gen *g, const char *ns, const char *name) {
    for (int i = 0; i < g->nfuncs; i++) {
        Node *f = g->funcs[i];
        const char *fns = f->ns ? f->ns : "";
        if (strcmp(f->name, name) == 0 && strcmp(fns, ns) == 0) return f;
    }
    return NULL;
}

/* Write a global variable's label name (prefix mvs_gv_ to avoid clashing with C runtime symbols
 * and with function/method labels of the form mvs_<ns>_<name>, e.g. a struct named g) */
void global_label(const char *name, char *buf) {
    snprintf(buf, LABEL_MAX, "mvs_gv_%s", name);
}

/* Look up a variable symbol: search visible scopes (inner to outer, for shadowing), then globals */
Sym *find_var(Gen *g, const char *name) {
    for (int i = g->nvisible - 1; i >= 0; i--) {
        Sym *s = &g->locals[g->visible[i]];
        if (strcmp(s->name, name) == 0) return s;
    }
    for (int i = 0; i < g->nglobals; i++)
        if (strcmp(g->globals[i].name, name) == 0) return &g->globals[i];
    return NULL;
}

/* Register a string in the pool; returns its index (used as the label name str_<idx>) */
int intern_string(Gen *g, const char *data, int len) {
    if (g->nstrs >= MAX_STR) { fprintf(stderr, "codegen error: too many string literals (max %d)\n", MAX_STR); g->had_error = 1; return 0; }
    int idx = g->nstrs++;
    g->strs[idx].data = (unsigned char *)malloc(len + 1);
    memcpy(g->strs[idx].data, data, len);
    g->strs[idx].data[len] = '\0';
    g->strs[idx].len = len;
    return idx;
}

/* ---------- type system: sizes, struct layout, type inference ---------- */

/* Find a struct's layout by name */
StructInfo *find_struct(Gen *g, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g->nstructs; i++)
        if (strcmp(g->structs[i].name, name) == 0) return &g->structs[i];
    return NULL;
}

/* Size in bytes of a type: pointer/str = 8, struct = size of its layout */
int type_size(Gen *g, DataType base, int ptr, const char *sname) {
    if (ptr > 0) return 8;
    switch (base) {
        case TYPE_I8: case TYPE_U8: case TYPE_BOOL: case TYPE_CHAR: return 1;
        case TYPE_I16: case TYPE_U16: return 2;
        /* f32 is stored as a real 4 bytes (single precision); math and call passing use double, converted on load/store */
        case TYPE_I32: case TYPE_U32: case TYPE_F32: return 4;
        case TYPE_I64: case TYPE_U64: case TYPE_ISIZE: case TYPE_USIZE: case TYPE_F64: return 8;
        case TYPE_I128: case TYPE_U128: return 16;
        case TYPE_STR: return 8;
        case TYPE_FUNC: return 8;   /* function pointer = an 8-byte address */
        case TYPE_STRUCT: { StructInfo *s = find_struct(g, sname); return s ? s->size : 0; }
        default: return 8;
    }
}

/* Is it a floating-point type */
int is_float_type(DataType t) { return t == TYPE_F32 || t == TYPE_F64; }

/* Is it a signed integer type (used to pick movsx/movzx when loading) */
int is_signed_type(DataType t) {
    switch (t) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64: case TYPE_I128: case TYPE_ISIZE: return 1;
        default: return 0;
    }
}

/* Register a struct's name + field info (offset/size not computed yet; done in layout_structs) */
void register_struct(Gen *g, Node *decl) {
    if (g->nstructs >= MAX_FUNC) { fprintf(stderr, "codegen error: too many structs\n"); g->had_error = 1; return; }
    StructInfo *s = &g->structs[g->nstructs++];
    s->name = decl->name;
    s->nfields = 0;
    s->size = 0;
    for (int i = 0; i < decl->nitems; i++) {
        if (s->nfields >= 64) { fprintf(stderr, "codegen error: struct '%s' has too many fields (max 64)\n", decl->name); g->had_error = 1; break; }
        Node *f = decl->items[i];
        Field *fd = &s->fields[s->nfields++];
        fd->name = f->name; fd->type = f->type; fd->ptr = f->ptr; fd->sname = f->type_name;
        fd->sig = f->sig;   /* signature (only for function-pointer fields) */
        fd->offset = 0; fd->size = 0;
    }
}

/* DFS to detect structs that contain themselves "by value" (ptr==0), which would have infinite size */
static int struct_has_cycle(Gen *g, int si, int *onpath) {
    if (onpath[si]) return 1;
    onpath[si] = 1;
    StructInfo *s = &g->structs[si];
    for (int i = 0; i < s->nfields; i++) {
        if (s->fields[i].type == TYPE_STRUCT && s->fields[i].ptr == 0) {
            StructInfo *fs = find_struct(g, s->fields[i].sname);
            if (fs && struct_has_cycle(g, (int)(fs - g->structs), onpath)) { onpath[si] = 0; return 1; }
        }
    }
    onpath[si] = 0;
    return 0;
}

/* Compute offset/size of every struct as a fixpoint (supports structs referencing structs declared later) */
void layout_structs(Gen *g) {
    /* Detect structs containing themselves by value first (infinite size, needs a pointer; avoids layout/print loops) */
    int onpath[MAX_FUNC];
    for (int si = 0; si < g->nstructs; si++) {
        memset(onpath, 0, sizeof(onpath));
        if (struct_has_cycle(g, si, onpath)) {
            fprintf(stderr, "codegen error: struct '%s' contains itself by value (infinite size); use a pointer field (*%s)\n",
                    g->structs[si].name, g->structs[si].name);
            g->had_error = 1;
            return;   /* stop before layout to avoid endless recursion */
        }
    }
    for (int round = 0; round <= g->nstructs; round++) {
        int changed = 0;
        for (int si = 0; si < g->nstructs; si++) {
            StructInfo *s = &g->structs[si];
            int off = 0;
            for (int i = 0; i < s->nfields; i++) {
                int fsize = type_size(g, s->fields[i].type, s->fields[i].ptr, s->fields[i].sname);
                s->fields[i].offset = off;
                s->fields[i].size = fsize;
                off += fsize; /* packed layout (no padding between fields) */
            }
            int newsize = (off + 7) / 8 * 8; /* round up to a multiple of 8 */
            if (newsize != s->size) { s->size = newsize; changed = 1; }
        }
        if (!changed) break;
    }
}

/* Find a field within a struct by name */
Field *find_field(StructInfo *s, const char *name) {
    for (int i = 0; i < s->nfields; i++)
        if (strcmp(s->fields[i].name, name) == 0) return &s->fields[i];
    return NULL;
}

/* If the callee expression is a "function pointer value" (variable/field of type TYPE_FUNC), return its signature.
 * If it is a direct function/method call (a function name, not a func-ptr value), return NULL.
 * Used to tell "indirect calls through a function pointer" apart from direct calls in both type_of and gen_call */
Node *expr_func_sig(Gen *g, Node *callee) {
    if (!callee) return NULL;
    if (callee->kind == ND_IDENT) {
        Sym *s = find_var(g, callee->name);
        if (s && s->type == TYPE_FUNC && s->ptr == 0) return s->sig;  /* variable/parameter that is a func-ptr */
        return NULL;                                                  /* not a variable -> function name, direct call */
    }
    if (callee->kind == ND_MEMBER) {
        ExprType bt = type_of(g, callee->operand);
        StructInfo *s = find_struct(g, bt.sname);
        if (s) { Field *f = find_field(s, callee->name); if (f && f->type == TYPE_FUNC && f->ptr == 0) return f->sig; }
    }
    return NULL;
}

/* Infer the type/size/pointer depth of an expression (deep enough for struct/pointer) */
ExprType type_of(Gen *g, Node *n) {
    ExprType r = { TYPE_I64, 0, NULL, NULL };
    if (!n) return r;
    switch (n->kind) {
        case ND_INT: r.base = TYPE_I64; break;
        case ND_FLOAT: r.base = TYPE_F64; break;
        case ND_CHAR: r.base = TYPE_CHAR; break;
        case ND_BOOL: r.base = TYPE_BOOL; break;
        case ND_STR: r.base = TYPE_STR; break;
        case ND_STRUCT_LIT: r.base = TYPE_STRUCT; r.sname = n->name; break;
        case ND_IDENT: {
            Sym *s = find_var(g, n->name);
            if (s) { r.base = s->type; r.ptr = s->ptr; r.sname = s->sname; r.sig = s->sig; }
            else {  /* not a variable: may be a "function name used as a value" -> function pointer */
                Node *f = find_func(g, g->cur_ns ? g->cur_ns : "", n->name);
                if (!f) f = find_func(g, "", n->name);
                if (f) { r.base = TYPE_FUNC; r.ptr = 0; r.sig = f; }
            }
            break;
        }
        case ND_MEMBER: {
            ExprType bt = type_of(g, n->operand);
            StructInfo *s = find_struct(g, bt.sname);
            if (s) { Field *f = find_field(s, n->name); if (f) { r.base = f->type; r.ptr = f->ptr; r.sname = f->sname; r.sig = f->sig; } }
            break;
        }
        case ND_UNARY:
            if (n->op == TK_AMP)       { r = type_of(g, n->operand); r.ptr++; }
            else if (n->op == TK_STAR) { r = type_of(g, n->operand); if (r.ptr > 0) r.ptr--; }
            else                       { r = type_of(g, n->operand); }
            break;
        case ND_BINARY:
            if (n->op==TK_EQ||n->op==TK_NEQ||n->op==TK_LT||n->op==TK_GT||
                n->op==TK_LE||n->op==TK_GE||n->op==TK_AND||n->op==TK_OR) { r.base = TYPE_BOOL; break; }
            else {
                ExprType lt = type_of(g, n->lhs), rt = type_of(g, n->rhs);
                /* pointer arithmetic: ptr +/- int -> pointer, ptr - ptr -> a count (isize) */
                if (n->op==TK_PLUS || n->op==TK_MINUS) {
                    if (lt.ptr>0 && rt.ptr>0) { r.base=TYPE_ISIZE; break; }
                    if (lt.ptr>0) { r=lt; break; }
                    if (rt.ptr>0) { r=rt; break; }
                }
                /* float mixed in: result is float (the widest); matters for format selection/type conversion */
                int lf = is_float_type(lt.base)&&lt.ptr==0, rf = is_float_type(rt.base)&&rt.ptr==0;
                if (lf || rf) { r.base = (lt.base==TYPE_F64||rt.base==TYPE_F64)?TYPE_F64:TYPE_F32; break; }
                /* int-int: result type = the wider one (order independent) */
                r = (type_size(g, rt.base, 0, NULL) > type_size(g, lt.base, 0, NULL)) ? rt : lt;
            }
            break;
        case ND_ASSIGN: r = type_of(g, n->lhs); break;
        case ND_CAST: r.base = n->type; r.ptr = n->ptr; r.sname = n->type_name; break;
        case ND_FRAMEREF: r.base = n->type; r.ptr = n->ptr; r.sname = n->type_name; break;
        case ND_CALL: {
            Node *callee = n->operand;
            /* indirect call through a function pointer: result type = the signature's return type */
            Node *fsig = expr_func_sig(g, callee);
            if (fsig) { r.base = fsig->type; r.ptr = fsig->ptr; r.sname = fsig->type_name; break; }
            Node *f = NULL;
            if (callee->kind == ND_IDENT) {
                f = find_func(g, g->cur_ns ? g->cur_ns : "", callee->name);
                if (!f) f = find_func(g, "", callee->name);
            } else if (callee->kind == ND_MEMBER) {
                ExprType bt = type_of(g, callee->operand);
                if (bt.base == TYPE_STRUCT && bt.sname) f = find_func(g, bt.sname, callee->name); /* method */
                if (!f && callee->operand->kind == ND_IDENT) f = find_func(g, callee->operand->name, callee->name);
            }
            if (f) { r.base = f->type; r.ptr = f->ptr; r.sname = f->type_name; }
            break;
        }
        default: break;
    }
    return r;
}

/* Infer the base type (used to pick the io.out format specifier) */
DataType infer_type(Gen *g, Node *n) {
    return type_of(g, n).base;
}

/* Append string s to a growable buffer (updates the values pointed to by out, len, cap) */
static void buf_append(char **out, size_t *len, size_t *cap, const char *s) {
    size_t sl = strlen(s);
    while (*len + sl + 2 > *cap) { *cap *= 2; *out = (char *)realloc(*out, *cap); }
    memcpy(*out + *len, s, sl);
    *len += sl;
}

/* Pick a printf specifier by type (pointer=address, str=%s, char=%c, float=%f, unsigned=%llu, others=%lld) */
static const char *spec_for(DataType base, int ptr, const char *user) {
    if (user && (strcmp(user, "x") == 0 || strcmp(user, ":x") == 0)) return "%llx";
    if (ptr > 0) return "%llu";                 /* pointer = address */
    if (base == TYPE_STR) return "%s";
    if (base == TYPE_CHAR) return "%c";
    if (is_float_type(base)) return "%f";
    if (base==TYPE_U8||base==TYPE_U16||base==TYPE_U32||base==TYPE_U64||base==TYPE_U128||base==TYPE_USIZE) return "%llu";
    return "%lld";
}

/* Expand a struct into the form "Name { f1: <spec>, f2: <spec> }" (like Rust's {:?})
 * and store the member-access nodes (base.field) into vals to pass to printf */
static void expand_struct(Gen *g, Node *base, const char *sname,
                          char **out, size_t *len, size_t *cap,
                          Node **vals, int *nv, int vals_cap, int depth) {
    StructInfo *s = find_struct(g, sname);
    if (!s) { buf_append(out, len, cap, "?"); return; }
    if (depth > 32) { buf_append(out, len, cap, "..."); return; }  /* guard against abnormally deep recursion */
    /* If base is a struct result from a function (ND_CALL), point at the reserved "temp slot" instead
     * (members read from the same temp); otherwise each field would gen_call again -> repeated calls/wrong values */
    if (base->kind == ND_CALL && base->int_val) {
        Node *fr = node_new(ND_FRAMEREF, base->line);
        fr->int_val = base->int_val;
        fr->type = TYPE_STRUCT; fr->type_name = strdup(sname);
        base = fr;
    }
    buf_append(out, len, cap, sname);
    buf_append(out, len, cap, " { ");
    for (int i = 0; i < s->nfields; i++) {
        if (i) buf_append(out, len, cap, ", ");
        buf_append(out, len, cap, s->fields[i].name);
        buf_append(out, len, cap, ": ");
        Node *m = node_new(ND_MEMBER, base->line);  /* base.field node */
        m->operand = base; m->name = strdup(s->fields[i].name);
        if (s->fields[i].type == TYPE_STRUCT && s->fields[i].ptr == 0) {
            expand_struct(g, m, s->fields[i].sname, out, len, cap, vals, nv, vals_cap, depth + 1); /* nested */
        } else if (*nv < vals_cap) {
            buf_append(out, len, cap, spec_for(s->fields[i].type, s->fields[i].ptr, NULL));
            vals[(*nv)++] = m;
        } else {
            buf_append(out, len, cap, "?"); /* over the vals limit; emit a literal so specifiers match the arg count */
        }
    }
    buf_append(out, len, cap, " }");
}

/* Convert an MVS format string (Rust-style {}) into a C printf format
 *   {} / {:?}  -> specifier chosen by the argument's type; structs are expanded into "Name { ... }"
 *   {:x}       -> hexadecimal ; {{ }} -> { } ; % -> %% ; a \n is appended automatically
 * Returns a new (malloc) buffer; sets *out_len, *out_nph (number of MVS-level placeholders),
 * and fills vals[] (values actually passed to printf; structs expanded into multiple fields) plus *out_nv */
char *build_c_format(Gen *g, Node *call, const char *fmt, int *out_len, int *out_nph,
                     Node **vals, int *out_nv, int vals_cap) {
    size_t cap = strlen(fmt) * 2 + 32, len = 0;
    char *out = (char *)malloc(cap);
    int argi = 1, ph = 0, nv = 0;
    for (const char *p = fmt; *p; p++) {
        if (len + 8 > cap) { cap *= 2; out = (char *)realloc(out, cap); }
        if (p[0] == '{' && p[1] == '{') { out[len++] = '{'; p++; continue; }
        if (p[0] == '}' && p[1] == '}') { out[len++] = '}'; p++; continue; }
        if (p[0] == '{') {
            char spec[16]; int si = 0;
            p++;
            while (*p && *p != '}') { if (si < 15) spec[si++] = *p; p++; }
            spec[si] = '\0';
            if (*p == '\0') break;   /* unclosed '{': avoid reading past the buffer */
            Node *arg = (argi < call->nitems) ? call->items[argi] : NULL;
            ExprType at = { TYPE_I64, 0, NULL, NULL };
            if (arg) at = type_of(g, arg);
            int is_hex = (strcmp(spec, "x") == 0 || strcmp(spec, ":x") == 0);
            if (arg && at.base == TYPE_STRUCT && at.ptr == 0 && !is_hex) {
                expand_struct(g, arg, at.sname, &out, &len, &cap, vals, &nv, vals_cap, 0);
            } else {
                buf_append(&out, &len, &cap, spec_for(at.base, at.ptr, spec));
                if (arg && nv < vals_cap) vals[nv++] = arg;
            }
            argi++; ph++;
            continue;
        }
        if (p[0] == '%') { out[len++] = '%'; out[len++] = '%'; continue; }
        out[len++] = p[0];
    }
    out[len++] = '\n'; /* automatic trailing newline */
    *out_len = (int)len;
    *out_nph = ph;
    *out_nv = nv;
    return out;
}

/* ---------- reserving variable space on the stack ---------- */

/* Add a local variable and assign its offset (slot rounded up to a multiple of 8 bytes); returns the offset */
int add_local(Gen *g, const char *name, DataType type, int ptr, char *sname, Node *sig, int *frame) {
    if (g->nlocals >= MAX_SYM) { fprintf(stderr, "codegen error: too many local variables (max %d)\n", MAX_SYM); g->had_error = 1; return 0; }
    int size = type_size(g, type, ptr, sname);
    int slot = (size + 7) / 8 * 8;       /* reserve at least 8 bytes, in multiples of 8 */
    *frame += slot;
    Sym *s = &g->locals[g->nlocals++];
    s->name = (char *)name;
    s->type = type;
    s->ptr = ptr;
    s->sname = sname;
    s->size = size;
    s->is_global = 0;
    s->sig = sig;                        /* signature (function pointers only) */
    s->offset = *frame;                  /* the variable lives at [rbp - offset .. rbp - offset + size) */
    return s->offset;
}

/* Walk the function body to reserve space for every variable declaration (pre-pass),
 * so the whole frame size is known before real code generation starts */
void collect_locals(Gen *g, Node *n, int *frame) {
    if (!n) return;
    switch (n->kind) {
        case ND_VAR_DECL:
            n->int_val = g->nlocals;   /* record this variable's slot index, used during gen + scoping */
            add_local(g, n->name, n->type, n->ptr, n->type_name, n->sig, frame);
            break;
        case ND_BLOCK:
            for (int i = 0; i < n->nitems; i++) collect_locals(g, n->items[i], frame);
            break;
        case ND_IF:
            collect_locals(g, n->then_branch, frame);
            collect_locals(g, n->else_branch, frame);
            break;
        case ND_WHILE:
            collect_locals(g, n->body, frame);
            break;
        case ND_FOR:
            collect_locals(g, n->init, frame); /* variables in the for-init */
            collect_locals(g, n->body, frame);
            break;
        case ND_DOWHILE:
            collect_locals(g, n->body, frame);
            break;
        case ND_SWITCH:
            /* Reserve a frame slot for the switch comparison value (offset stored in int_val)
             * so it need not stay on the temp stack; leaving the switch any way (break/continue) is safe */
            n->int_val = add_local(g, "$switch", TYPE_I64, 0, NULL, NULL, frame);
            for (int i = 0; i < n->nitems; i++) collect_locals(g, n->items[i], frame);
            break;
        case ND_CASE:
            for (int i = 0; i < n->nitems; i++) collect_locals(g, n->items[i], frame);
            break;
        default: break;
    }
}

/* Walk the whole tree (expressions included), reserving a temp slot for every call that "returns a struct".
 * The offset is stored in the ND_CALL node's int_val, used when the struct result is used as an rvalue
 * (e.g. as an argument, or as the base of .field) that needs a real address for the result */
void collect_struct_temps(Gen *g, Node *n, int *frame) {
    if (!n) return;
    if (n->kind == ND_CALL) {
        ExprType rt = type_of(g, n);
        if (rt.base == TYPE_STRUCT && rt.ptr == 0)
            n->int_val = add_local(g, "$tmp", TYPE_STRUCT, 0, rt.sname, NULL, frame);
    }
    collect_struct_temps(g, n->lhs, frame);   collect_struct_temps(g, n->rhs, frame);
    collect_struct_temps(g, n->operand, frame); collect_struct_temps(g, n->cond, frame);
    collect_struct_temps(g, n->then_branch, frame); collect_struct_temps(g, n->else_branch, frame);
    collect_struct_temps(g, n->init, frame);  collect_struct_temps(g, n->step, frame);
    collect_struct_temps(g, n->body, frame);
    for (int i = 0; i < n->nitems; i++) collect_struct_temps(g, n->items[i], frame);
}

/* ---------- reachability analysis for dropping unused functions ---------- */

/* Find a function's index in the funcs table (returns -1 if not found) */
int func_index(Gen *g, Node *f) {
    if (!f) return -1;
    for (int i = 0; i < g->nfuncs; i++) if (g->funcs[i] == f) return i;
    return -1;
}

void reach_node(Gen *g, Node *n, const char *ns, char *reached);

/* Mark a function reachable, then scan its body for further calls (DFS; cycles guarded by reached) */
void reach_func(Gen *g, int idx, char *reached) {
    if (idx < 0 || reached[idx]) return;
    reached[idx] = 1;
    Node *f = g->funcs[idx];
    reach_node(g, f->body, f->mod ? f->mod : "", reached);
}

/* Scan a node for function calls and mark the targets reachable (recursively visits every child) */
void reach_node(Gen *g, Node *n, const char *ns, char *reached) {
    if (!n) return;
    if (n->kind == ND_CALL) {
        Node *callee = n->operand;
        if (callee->kind == ND_MEMBER && callee->operand->kind == ND_IDENT &&
            strcmp(callee->operand->name, "io") == 0 && strcmp(callee->name, "out") == 0) {
            reach_func(g, func_index(g, find_func(g, "", "printf")), reached); /* io.out uses printf */
        } else if (callee->kind == ND_IDENT) {
            Node *t = find_func(g, ns, callee->name);
            if (!t) t = find_func(g, "", callee->name);
            reach_func(g, func_index(g, t), reached);
        } else if (callee->kind == ND_MEMBER) {
            /* A call through . may be a module namespace or a struct method.
             * (The reachability pass has no variable type info yet, so over-approximate:
             *  mark every method with a matching name, plus functions in the base's namespace) */
            if (callee->operand->kind == ND_IDENT)
                reach_func(g, func_index(g, find_func(g, callee->operand->name, callee->name)), reached);
            for (int k = 0; k < g->nfuncs; k++)
                if (g->funcs[k]->is_method && strcmp(g->funcs[k]->name, callee->name) == 0)
                    reach_func(g, k, reached);
        }
    }
    /* A function name used as a "value" (function pointer), e.g. `let h = handler;` or passed as an argument,
     * must also count as reachable, otherwise tree-shaking drops it and lea references a missing label */
    if (n->kind == ND_IDENT) {
        Node *t = find_func(g, ns, n->name);
        if (!t) t = find_func(g, "", n->name);
        if (t) reach_func(g, func_index(g, t), reached);
    }
    reach_node(g, n->lhs, ns, reached);
    reach_node(g, n->rhs, ns, reached);
    reach_node(g, n->operand, ns, reached);
    reach_node(g, n->cond, ns, reached);
    reach_node(g, n->then_branch, ns, reached);
    reach_node(g, n->else_branch, ns, reached);
    reach_node(g, n->init, ns, reached);
    reach_node(g, n->step, ns, reached);
    reach_node(g, n->body, ns, reached);
    for (int i = 0; i < n->nitems; i++) reach_node(g, n->items[i], ns, reached);
}
