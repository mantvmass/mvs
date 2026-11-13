/*
 * module.c - module loading and merging (resolves use/mod across files)
 *
 * The module system:
 *   1. Reads the entry file and parses it
 *   2. On use/mod, loads the referenced module (a loaded set prevents reloading)
 *   3. Merges all definitions into a single ND_PROGRAM,
 *      tagging a namespace on modules used as a package (e.g. io.out)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "parser.h"
#include "diag.h"

#define MAX_LOADED 256

/* Loading state: the accumulated result plus the list of already loaded files */
typedef struct {
    Node       *result;              /* ND_PROGRAM accumulating the result */
    char       *loaded[MAX_LOADED];  /* canonical paths of loaded modules (prevents reload/loops) */
    char       *defs[MAX_LOADED];    /* top-level symbol list per module, form "|name|name|" (import checks) */
    char       *loaded_ns[MAX_LOADED];/* namespace a module was tagged with on first load (guards ns clash) */
    int         item_start[MAX_LOADED];/* range of result items this module contributed, so a later */
    int         item_end[MAX_LOADED];  /* import under the other form can alias its free functions */
    int         nloaded;
    char       *ns_name[MAX_LOADED]; /* bound namespaces (io, net, alias); guards rebinding to another module */
    char       *ns_canon[MAX_LOADED];
    int         nns;
    char       *loading[MAX_LOADED]; /* modules currently loading in this call chain; detects circular import */
    int         nloading;
    const char *stddir;              /* standard library folder ("std" package) */
    const char *coredir;             /* core library folder ("core" package: pure MVS, --nostd safe) */
    int         nostd;               /* 1 = freestanding mode ("use std::*" forbidden; "core" allowed) */
    const char *target_os;           /* current target OS, matched against @compile(target_os = ...) */
    const char *target_arch;         /* current target arch, matched against @compile(target_arch = ...) */
    int         had_error;
} Loader;

/* Does this item survive @compile filtering for the current target?
 * No attribute = always kept; both keys set = both must match (AND semantics).
 * cfg_not = 1 inverts the result (not(...) combinator). */
static int cfg_matches(Loader *L, Node *it) {
    if (!it->cfg_os && !it->cfg_arch) return 1;
    int m = 1;
    if (it->cfg_os && strcmp(it->cfg_os, L->target_os) != 0) m = 0;
    if (it->cfg_arch && strcmp(it->cfg_arch, L->target_arch) != 0) m = 0;
    return it->cfg_not ? !m : m;
}

/* Strip ND_CFG_BLOCK nodes from a block's items: matching blocks are inlined
 * (their statements replace the node), non-matching blocks are removed.
 * Loops until no ND_CFG_BLOCK remains at this level (handles nested cfg blocks
 * that appear after inlining an outer one). Then recurses into sub-blocks. */
static void cfg_strip_body(Loader *L, Node *block) {
    if (!block || !block->items) return;
    int found;
    do {
        found = 0;
        Node **out = NULL;
        int nout = 0, cap = 0;
        for (int i = 0; i < block->nitems; i++) {
            Node *s = block->items[i];
            if (s->kind == ND_CFG_BLOCK) {
                found = 1;
                if (cfg_matches(L, s)) {
                    for (int j = 0; j < s->nitems; j++) {
                        if (nout >= cap) { cap = cap ? cap * 2 : 16; out = realloc(out, cap * sizeof(Node*)); }
                        out[nout++] = s->items[j];
                    }
                }
            } else {
                if (nout >= cap) { cap = cap ? cap * 2 : 16; out = realloc(out, cap * sizeof(Node*)); }
                out[nout++] = s;
            }
        }
        free(block->items);
        block->items = out;
        block->nitems = nout;
    } while (found);
    for (int i = 0; i < block->nitems; i++) {
        Node *s = block->items[i];
        if (s->body) cfg_strip_body(L, s->body);
        if (s->then_branch) cfg_strip_body(L, s->then_branch);
        /* elseif chains: else_branch is ND_IF -> ND_IF -> ... -> ND_BLOCK;
         * walk the entire chain so deeply nested branches get stripped too */
        {
            Node *eb = s->else_branch;
            while (eb) {
                if (eb->kind == ND_BLOCK) { cfg_strip_body(L, eb); break; }
                if (eb->kind != ND_IF) break;
                if (eb->then_branch) cfg_strip_body(L, eb->then_branch);
                eb = eb->else_branch;
            }
        }
        if (s->kind == ND_BLOCK) cfg_strip_body(L, s);
        if (s->kind == ND_SWITCH) {
            for (int c = 0; c < s->nitems; c++)
                if (s->items[c]->kind == ND_CASE) cfg_strip_body(L, s->items[c]);
        }
        if (s->kind == ND_MATCH) {
            for (int a = 0; a < s->nitems; a++)
                if (s->items[a]->body) cfg_strip_body(L, s->items[a]->body);
        }
    }
}

static void cfg_strip_program(Loader *L, Node *program) {
    for (int i = 0; i < program->nitems; i++) {
        Node *it = program->items[i];
        if (it->kind == ND_FUNC && it->body)
            cfg_strip_body(L, it->body);
    }
}

/* Read a whole file into memory; returns a string (caller frees) or NULL */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }     /* could not determine file size */
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Does this file exist and open for reading? */
static int read_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Copy the directory part of path into out (handles both / and \); "." if none */
static void path_dirname(const char *path, char *out) {
    strcpy(out, path);
    char *slash = strrchr(out, '/');
    char *bslash = strrchr(out, '\\');
    char *cut = slash > bslash ? slash : bslash;
    if (cut) *cut = '\0';
    else strcpy(out, ".");
}


/* Make an absolute (canonical) path so identical files compare equal */
static void canonicalize(const char *path, char *out, size_t n) {
#ifdef _WIN32
    if (!_fullpath(out, path, (int)n)) snprintf(out, n, "%s", path);
#else
    char *r = realpath(path, NULL);          /* POSIX: malloc'd resolved path */
    if (r) { snprintf(out, n, "%s", r); free(r); }
    else snprintf(out, n, "%s", path);
#endif
}

/* Return the index of an already loaded module (canonical path compare) or -1 */
static int find_loaded(Loader *L, const char *canon) {
    for (int i = 0; i < L->nloaded; i++)
        if (strcmp(L->loaded[i], canon) == 0) return i;
    return -1;
}

/* Collect a module's top-level symbol names (struct/trait/enum/global/non-method
 * function) as "|a|b|c|". A global is a symbol like any other: importing one by
 * name used to be rejected while the module's functions imported fine. */
static char *collect_defs(Node *prog) {
    size_t cap = 256, len = 1;
    char *s = (char *)malloc(cap);
    s[0] = '|'; s[1] = '\0';
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        const char *nm = NULL;
        if ((d->kind == ND_FUNC && !d->is_method) || d->kind == ND_STRUCT_DECL ||
            d->kind == ND_TRAIT || d->kind == ND_ENUM_DECL || d->kind == ND_VAR_DECL)
            nm = d->name;
        if (!nm) continue;
        size_t need = len + strlen(nm) + 2;
        while (need + 1 > cap) { cap *= 2; s = (char *)realloc(s, cap); }
        strcpy(s + len, nm); len += strlen(nm);
        s[len++] = '|'; s[len] = '\0';
    }
    return s;
}

/* Does the module define this symbol? (matches "|name|" within the defs string) */
static int defs_has(const char *defs, const char *name) {
    if (!defs) return 0;
    char pat[300];
    snprintf(pat, sizeof(pat), "|%s|", name);
    return strstr(defs, pat) != NULL;
}

static void load_module(Loader *L, const char *path, const char *ns);
static void handle_use(Loader *L, Node *imp, const char *base_dir, const char *parent_ns);
static void handle_mod_decl(Loader *L, Node *mod_node, const char *base_dir, const char *parent_ns);

static void append_def(char **defs, const char *sym) {
    if (defs_has(*defs, sym)) return;
    size_t cur = strlen(*defs);
    size_t need = cur + strlen(sym) + 2;
    *defs = (char *)realloc(*defs, need);
    strcat(*defs, sym);
    strcat(*defs, "|");
}

static void append_all_defs(char **mod_defs, const char *src_defs) {
    if (!src_defs || strlen(src_defs) <= 1) return;
    const char *p = src_defs + 1;
    while (*p) {
        const char *end = strchr(p, '|');
        if (!end) break;
        size_t len = (size_t)(end - p);
        if (len > 0 && len < 256) {
            char sym[256];
            memcpy(sym, p, len);
            sym[len] = '\0';
            append_def(mod_defs, sym);
        }
        p = end + 1;
    }
}

/* A module imported under one form and then the other: clone its free functions
 * under the second spelling so both resolve. `from_ns` is the namespace the
 * module was first loaded with (empty for a symbol import), `to_ns` the one the
 * second import asked for. Methods, externs and exports keep their own naming.
 * The clones are ordinary functions, so tree-shaking drops the unused copies. */
static void alias_module_functions(Loader *L, int mod_index, const char *from_ns, const char *to_ns) {
    int start = L->item_start[mod_index], end = L->item_end[mod_index];
    if (end > L->result->nitems) end = L->result->nitems;
    int added = 0;
    for (int i = start; i < end; i++) {
        Node *it = L->result->items[i];
        if (it->kind != ND_FUNC || it->is_method || it->is_extern || it->is_export) continue;
        /* generic templates are found by name across the whole program, so a
         * second copy would give monomorphization two templates to instantiate */
        if (it->ngen > 0) continue;
        const char *cur = it->ns ? it->ns : "";
        if (strcmp(cur, from_ns) != 0) continue;
        Node *cl = node_clone(it);
        free(cl->ns);  cl->ns  = to_ns[0] ? strdup(to_ns) : NULL;
        free(cl->mod); cl->mod = to_ns[0] ? strdup(to_ns) : (from_ns[0] ? strdup(from_ns) : NULL);
        node_add_item(L->result, cl);
        added++;
    }
    (void)added;
}

/* Bind namespace name to module canon; if already bound to another module, error */
static void register_ns(Loader *L, const char *name, const char *canon) {
    for (int i = 0; i < L->nns; i++) {
        if (strcmp(L->ns_name[i], name) != 0) continue;
        if (strcmp(L->ns_canon[i], canon) != 0) {
            fprintf(stderr, "error: namespace '%s' is already bound to a different module\n", name);
            L->had_error = 1;
        }
        return; /* rebinding to the same module is fine */
    }
    if (L->nns < MAX_LOADED) {
        L->ns_name[L->nns] = strdup(name);
        L->ns_canon[L->nns] = strdup(canon);
        L->nns++;
    }
}


/* Handle a use statement (Rust-style :: path).
 * str_val holds the :: separated path, items hold braced symbols,
 * name="*" for glob, is_export for pub use. */
static void handle_use(Loader *L, Node *imp, const char *base_dir, const char *parent_ns) {
    const char *path = imp->str_val;
    if (!path) return;
    int is_glob = (imp->name && strcmp(imp->name, "*") == 0);

    /* String path form: use "../../path/to/module"::{items}; */
    if (imp->is_extern) {
        char file[1024];
        snprintf(file, sizeof(file), "%s/%s.mvs", base_dir, path);
        if (!read_file_exists(file)) {
            char dir_mod[1024];
            snprintf(dir_mod, sizeof(dir_mod), "%s/%s/mod.mvs", base_dir, path);
            if (read_file_exists(dir_mod))
                strncpy(file, dir_mod, sizeof(file));
        }
        if (!read_file_exists(file)) {
            fprintf(stderr, "error: cannot find module '%s' (from %s)\n", path, base_dir);
            L->had_error = 1; return;
        }
        const char *sub_ns = "";
        if (imp->is_export && parent_ns && parent_ns[0])
            sub_ns = parent_ns;
        if (imp->nitems > 0) {
            load_module(L, file, sub_ns);
            char canon[1024]; canonicalize(file, canon, sizeof(canon));
            int idx = find_loaded(L, canon);
            if (idx >= 0 && L->defs[idx]) {
                for (int i = 0; i < imp->nitems; i++) {
                    const char *nm = imp->items[i]->name;
                    if (!defs_has(L->defs[idx], nm)) {
                        fprintf(stderr, "error: module has no exported symbol '%s' (use \"%s\")\n", nm, path);
                        L->had_error = 1;
                    }
                }
            }
        } else if (is_glob) {
            load_module(L, file, sub_ns);
        } else {
            const char *last = strrchr(path, '/');
            const char *ns_name = last ? last + 1 : path;
            char ns_copy[256];
            strncpy(ns_copy, ns_name, sizeof(ns_copy) - 1); ns_copy[sizeof(ns_copy) - 1] = '\0';
            char canon[1024]; canonicalize(file, canon, sizeof(canon));
            register_ns(L, ns_copy, canon);
            load_module(L, file, ns_copy);
        }
        return;
    }

    char buf[1024];
    strncpy(buf, path, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    char *segs[16]; int nseg = 0;
    { char *p = buf;
      while (*p && nseg < 16) {
          segs[nseg++] = p;
          char *sep = strstr(p, "::");
          if (!sep) break;
          *sep = '\0';
          p = sep + 2;
      }
    }
    if (nseg == 0) return;

    const char *root_dir = NULL;
    int is_pkg = 0;
    if (strcmp(segs[0], "std") == 0) {
        if (L->nostd) {
            fprintf(stderr, "error: cannot use 'std' in --nostd (freestanding) mode\n");
            fprintf(stderr, "help: the 'core' package stays available, e.g. use core::mem\n");
            L->had_error = 1; return;
        }
        root_dir = L->stddir; is_pkg = 1;
    } else if (strcmp(segs[0], "core") == 0) {
        root_dir = L->coredir; is_pkg = 1;
    } else if (strcmp(segs[0], "self") == 0) {
        root_dir = base_dir;
    } else if (strcmp(segs[0], "super") == 0) {
        static char parent_buf[1024];
        path_dirname(base_dir, parent_buf);
        root_dir = parent_buf;
    } else {
        fprintf(stderr, "error: use path must start with std, core, self, or super (got '%s')\n", segs[0]);
        L->had_error = 1; return;
    }

    /* Braced items with package-only prefix: use std::{io, math} */
    if (imp->nitems > 0 && nseg == 1 && is_pkg) {
        for (int i = 0; i < imp->nitems; i++) {
            const char *name = imp->items[i]->name;
            char file[1024];
            snprintf(file, sizeof(file), "%s/%s.mvs", root_dir, name);
            if (!read_file_exists(file))
                snprintf(file, sizeof(file), "%s/%s/mod.mvs", root_dir, name);
            if (!read_file_exists(file)) {
                fprintf(stderr, "error: package has no module '%s'\n", name);
                L->had_error = 1; continue;
            }
            char canon[1024]; canonicalize(file, canon, sizeof(canon));
            register_ns(L, name, canon);
            load_module(L, file, name);
        }
        return;
    }

    /* Walk segments from 1..nseg to find the module file */
    char dir[1024];
    strncpy(dir, root_dir, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    int file_seg = -1;
    char file[1024]; file[0] = '\0';
    for (int i = 1; i < nseg; i++) {
        char try_f[1024], try_m[1024];
        snprintf(try_f, sizeof(try_f), "%s/%s.mvs", dir, segs[i]);
        snprintf(try_m, sizeof(try_m), "%s/%s/mod.mvs", dir, segs[i]);
        if (read_file_exists(try_f)) {
            strncpy(file, try_f, sizeof(file)); file_seg = i; break;
        }
        if (read_file_exists(try_m)) {
            strncpy(file, try_m, sizeof(file)); file_seg = i; break;
        }
        snprintf(try_f, sizeof(try_f), "%s/%s", dir, segs[i]);
        strncpy(dir, try_f, sizeof(dir));
    }
    if (file_seg < 0) {
        fprintf(stderr, "error: cannot resolve module path '%s'\n", path);
        L->had_error = 1; return;
    }

    int sym_start = file_seg + 1;
    const char *sub_ns = "";
    if (imp->is_export && parent_ns && parent_ns[0])
        sub_ns = parent_ns;

    if (imp->nitems > 0) {
        /* Braced symbol import: use std::vec::{Vec, VecIter} */
        load_module(L, file, sub_ns);
        char canon[1024]; canonicalize(file, canon, sizeof(canon));
        int idx = find_loaded(L, canon);
        if (idx >= 0 && L->defs[idx]) {
            for (int i = 0; i < imp->nitems; i++) {
                const char *nm = imp->items[i]->name;
                if (!defs_has(L->defs[idx], nm)) {
                    fprintf(stderr, "error: module has no exported symbol '%s' (use %s)\n", nm, path);
                    L->had_error = 1;
                }
            }
        }
    } else if (is_glob) {
        /* Glob import: use path::* */
        load_module(L, file, sub_ns);
    } else if (sym_start < nseg) {
        /* Bare use with trailing symbol: use std::vec::Vec */
        load_module(L, file, sub_ns);
        char canon[1024]; canonicalize(file, canon, sizeof(canon));
        int idx = find_loaded(L, canon);
        if (idx >= 0 && L->defs[idx]) {
            for (int i = sym_start; i < nseg; i++) {
                /* segs are in buf which was modified; rebuild from original path */
                char seg_buf[256];
                const char *orig = imp->str_val;
                int si = 0, cnt = 0;
                while (*orig) {
                    if (cnt == i) { int j = 0; while (*orig && !(orig[0] == ':' && orig[1] == ':')) seg_buf[j++] = *orig++; seg_buf[j] = '\0'; break; }
                    if (orig[0] == ':' && orig[1] == ':') { cnt++; orig += 2; } else orig++;
                }
                (void)si;
                if (!defs_has(L->defs[idx], seg_buf)) {
                    fprintf(stderr, "error: module has no exported symbol '%s' (use %s)\n", seg_buf, path);
                    L->had_error = 1;
                }
            }
        }
    } else {
        /* Namespace import: use std::io */
        const char *ns_name = NULL;
        { const char *orig = imp->str_val;
          const char *last = orig;
          while (*orig) {
              if (orig[0] == ':' && orig[1] == ':') { orig += 2; last = orig; }
              else orig++;
          }
          ns_name = last;
        }
        char ns_copy[256];
        strncpy(ns_copy, ns_name, sizeof(ns_copy) - 1); ns_copy[sizeof(ns_copy) - 1] = '\0';
        char canon[1024]; canonicalize(file, canon, sizeof(canon));
        register_ns(L, ns_copy, canon);
        load_module(L, file, ns_copy);
    }
}

/* Handle a mod declaration: load the submodule file */
static void handle_mod_decl(Loader *L, Node *mod_node, const char *base_dir, const char *parent_ns) {
    const char *name = mod_node->name;
    if (!name) return;
    char file[1024];
    snprintf(file, sizeof(file), "%s/%s.mvs", base_dir, name);
    if (!read_file_exists(file))
        snprintf(file, sizeof(file), "%s/%s/mod.mvs", base_dir, name);
    if (!read_file_exists(file)) {
        fprintf(stderr, "error: cannot find module '%s' (looked for %s/%s.mvs and %s/%s/mod.mvs)\n",
                name, base_dir, name, base_dir, name);
        L->had_error = 1; return;
    }
    if (mod_node->is_export) {
        const char *ns = (parent_ns && parent_ns[0]) ? parent_ns : "";
        load_module(L, file, ns);
    } else {
        char canon[1024]; canonicalize(file, canon, sizeof(canon));
        register_ns(L, name, canon);
        load_module(L, file, name);
    }
}

/* Load one module from path and append all its definitions to the result (tag namespace if any) */
static void load_module(Loader *L, const char *path, const char *ns) {
    /* Canonicalize to an absolute path first so "./lib.mvs" and "lib.mvs" count as
     * the same file; prevents reloads and loops caused by different path spellings */
    char canon[1024];
    canonicalize(path, canon, sizeof(canon));

    int prev = find_loaded(L, canon);
    if (prev >= 0) {                            /* already loaded; skip reload (supports diamond use) */
        /* If bound under two different namespaces (alias clash), symbols would live under
         * different names, so report a clear error. (An empty ns = symbol/direct use does not
         * clash because struct/trait symbols are global anyway.) */
        const char *prev_ns = L->loaded_ns[prev] ? L->loaded_ns[prev] : "";
        const char *now_ns = ns ? ns : "";
        if (strcmp(prev_ns, now_ns) != 0) {
            /* Mixing use forms for one module used to dedup silently; the second form's
             * free functions then live under the first form's namespace and are unreachable
             * under the requested one. Structs/traits/methods are global either way, so this
             * is a warning, not an error; two DIFFERENT namespaces stay a hard error. */
            if (prev_ns[0] && now_ns[0]) {
                fprintf(stderr, "error: module '%s' is already used as namespace '%s'; cannot also use it as '%s'\n",
                        path, prev_ns, now_ns);
                L->had_error = 1;
            } else {
                /* One namespace form and one symbol form of the SAME module. Both
                 * spellings are legitimate (use std::{io, string} next to
                 * use std::string::String is the normal way to write it), so the
                 * module's free functions are made reachable under both instead
                 * of one of them silently not working. */
                alias_module_functions(L, prev, prev_ns, now_ns);
            }
        }
        return;
    }
    /* Circular use check: this module is already being loaded in the current call chain (A -> B -> A) */
    for (int i = 0; i < L->nloading; i++)
        if (strcmp(L->loading[i], canon) == 0) {
            fprintf(stderr, "error: circular use detected involving module '%s'\n", path);
            L->had_error = 1;
            return;
        }
    if (L->nloading >= MAX_LOADED) { fprintf(stderr, "error: import nesting too deep\n"); L->had_error = 1; return; }
    L->loading[L->nloading++] = strdup(canon);   /* mark: currently loading */

    char *src = read_file(path);
    if (!src) {
        fprintf(stderr, "error: cannot open module '%s'\n", path);
        L->had_error = 1;
        free(L->loading[--L->nloading]);
        return;
    }

    /* diag keeps the source text for error excerpts; the returned name pointer is
     * stable, so AST nodes can carry it for later passes (typecheck etc.) */
    const char *fname = diag_register_source(path, src);
    int err = 0;
    Node *prog = parse_program(src, fname, &err);
    if (err) { L->had_error = 1; free(L->loading[--L->nloading]); return; }

    char *mod_defs = collect_defs(prog); /* record this module's top-level symbols for import checks */

    /* This module's folder, used to resolve its own nested imports */
    char dir[1024];
    path_dirname(path, dir);

    int item_start = L->result->nitems;   /* where this module's items begin */
    for (int i = 0; i < prog->nitems; i++) {
        Node *it = prog->items[i];
        if (!cfg_matches(L, it)) continue;    /* @compile says this item is not for this target */
        if (it->kind == ND_IMPORT) {
            handle_use(L, it, dir, ns);
            if (it->is_export) {
                if (it->nitems > 0) {
                    for (int j = 0; j < it->nitems; j++)
                        append_def(&mod_defs, it->items[j]->name);
                } else if (it->name && strcmp(it->name, "*") == 0) {
                    /* pub use path::* : re-export all symbols from the resolved module */
                    /* The module was just loaded by handle_use; find its defs */
                    /* Resolve file again to find the loaded module */
                    char rebuf[1024]; strncpy(rebuf, it->str_val, sizeof(rebuf) - 1); rebuf[sizeof(rebuf) - 1] = '\0';
                    char *rsegs[16]; int rnseg = 0;
                    { char *rp = rebuf; while (*rp && rnseg < 16) { rsegs[rnseg++] = rp; char *rs = strstr(rp, "::"); if (!rs) break; *rs = '\0'; rp = rs + 2; } }
                    const char *rroot = NULL;
                    if (rnseg > 0 && strcmp(rsegs[0], "std") == 0) rroot = L->stddir;
                    else if (rnseg > 0 && strcmp(rsegs[0], "core") == 0) rroot = L->coredir;
                    else rroot = dir;
                    char rdir2[1024]; strncpy(rdir2, rroot, sizeof(rdir2) - 1); rdir2[sizeof(rdir2) - 1] = '\0';
                    char rfile[1024]; rfile[0] = '\0';
                    for (int ri = 1; ri < rnseg; ri++) {
                        char rtf[1024]; snprintf(rtf, sizeof(rtf), "%s/%s.mvs", rdir2, rsegs[ri]);
                        if (read_file_exists(rtf)) { strncpy(rfile, rtf, sizeof(rfile)); break; }
                        snprintf(rtf, sizeof(rtf), "%s/%s/mod.mvs", rdir2, rsegs[ri]);
                        if (read_file_exists(rtf)) { strncpy(rfile, rtf, sizeof(rfile)); break; }
                        snprintf(rtf, sizeof(rtf), "%s/%s", rdir2, rsegs[ri]);
                        strncpy(rdir2, rtf, sizeof(rdir2));
                    }
                    if (rfile[0]) {
                        char rcanon[1024]; canonicalize(rfile, rcanon, sizeof(rcanon));
                        int ridx = find_loaded(L, rcanon);
                        if (ridx >= 0 && L->defs[ridx])
                            append_all_defs(&mod_defs, L->defs[ridx]);
                    }
                } else {
                    /* pub use std::vec::Vec (bare, last segment is the symbol) */
                    const char *orig = it->str_val;
                    const char *last = orig;
                    while (*orig) { if (orig[0] == ':' && orig[1] == ':') { orig += 2; last = orig; } else orig++; }
                    if (last[0]) append_def(&mod_defs, last);
                }
            }
        } else if (it->kind == ND_MOD) {
            handle_mod_decl(L, it, dir, ns);
            if (it->is_export) {
                char mfile[1024];
                snprintf(mfile, sizeof(mfile), "%s/%s.mvs", dir, it->name);
                if (!read_file_exists(mfile))
                    snprintf(mfile, sizeof(mfile), "%s/%s/mod.mvs", dir, it->name);
                if (read_file_exists(mfile)) {
                    char mcanon[1024]; canonicalize(mfile, mcanon, sizeof(mcanon));
                    int midx = find_loaded(L, mcanon);
                    if (midx >= 0 && L->defs[midx])
                        append_all_defs(&mod_defs, L->defs[midx]);
                }
            }
        } else {
            if (it->kind == ND_FUNC && !(ns && ns[0])) {
                /* A module imported for its symbols has no namespace, but it still
                 * needs an identity: without one, its own call to an extern it
                 * declared could resolve to a same-named function in a DIFFERENT
                 * module. The canonical path can never be typed as a namespace,
                 * so it identifies the module without becoming reachable as one. */
                it->mod = strdup(canon);
            }
            if (ns && ns[0] && it->kind == ND_FUNC) {
                /* Every function in this module belongs to module ns. For functions with a
                 * body this resolves internal calls; for externs it lets ns.func(...) reach
                 * foreign functions the module declares (e.g. math.fmod -> libm fmod). */
                it->mod = strdup(ns);
                /* The symbol label (ns) is applied only to plain functions (called as ns.func):
                 * - methods already use the struct name as ns; exports keep the raw name for C
                 * - externs keep the raw C symbol name */
                if (!it->is_extern && !it->is_method && !it->is_export) it->ns = strdup(ns);
            }
            node_add_item(L->result, it);
        }
    }

    /* Done: pop from the loading chain, then register as "loaded" with its symbol list */
    free(L->loading[--L->nloading]);
    if (L->nloaded < MAX_LOADED) {
        L->loaded[L->nloaded] = strdup(canon);
        L->defs[L->nloaded] = mod_defs;
        L->loaded_ns[L->nloaded] = strdup(ns ? ns : "");
        L->item_start[L->nloaded] = item_start;
        L->item_end[L->nloaded] = L->result->nitems;
        L->nloaded++;
    } else {
        free(mod_defs);
        fprintf(stderr, "error: too many imported modules (max %d)\n", MAX_LOADED);
        L->had_error = 1;
    }
}

Node *module_load(const char *entry_path, const char *stddir, const char *coredir, int nostd,
                  const char *target_os, const char *target_arch, int *had_error) {
    Loader L;
    memset(&L, 0, sizeof(L));
    L.result = node_new(ND_PROGRAM, 1);
    L.stddir = stddir;
    L.coredir = coredir;
    L.nostd = nostd;
    L.target_os = target_os;
    L.target_arch = target_arch;

    /* The entry file loads with an empty namespace (function names used directly, contains main) */
    load_module(&L, entry_path, "");

    cfg_strip_program(&L, L.result);

    *had_error = L.had_error;
    return L.result;
}
