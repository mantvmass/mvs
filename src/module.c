/*
 * module.c - module loading and merging (resolves imports across files)
 *
 * Idea: the original front end parses one file at a time. The module system:
 *   1. Reads the entry file and parses it
 *   2. On ND_IMPORT, loads the referenced file/package (a loaded set prevents reloading)
 *   3. Merges functions/variables/externs from every module into a single ND_PROGRAM,
 *      tagging a namespace on modules imported as a package (e.g. io.out)
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
    int         nloaded;
    char       *ns_name[MAX_LOADED]; /* bound namespaces (io, net, alias); guards rebinding to another module */
    char       *ns_canon[MAX_LOADED];
    int         nns;
    char       *loading[MAX_LOADED]; /* modules currently loading in this call chain; detects circular import */
    int         nloading;
    const char *stddir;              /* standard library folder ("std" package) */
    const char *coredir;             /* core library folder ("core" package: pure MVS, --nostd safe) */
    int         nostd;               /* 1 = freestanding mode ("std" imports forbidden; "core" allowed) */
    const char *target_os;           /* current target OS, matched against @compile(target_os = ...) */
    const char *target_arch;         /* current target arch, matched against @compile(target_arch = ...) */
    int         had_error;
} Loader;

/* Does this item survive @compile filtering for the current target?
 * No attribute = always kept; both keys set = both must match (AND semantics). */
static int cfg_matches(Loader *L, Node *it) {
    if (it->cfg_os && strcmp(it->cfg_os, L->target_os) != 0) return 0;
    if (it->cfg_arch && strcmp(it->cfg_arch, L->target_arch) != 0) return 0;
    return 1;
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

/* Check for a "bare package" path (single name, no / \ or .) such as "std".
 * This form = namespace import (names in {} are submodules loaded as namespaces) */
static int is_package_path(const char *p) {
    return strchr(p, '/') == NULL && strchr(p, '\\') == NULL && strchr(p, '.') == NULL;
}

/* Check for an absolute path (starts with / \ or "X:") */
static int is_absolute(const char *p) {
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (p[0] && p[1] == ':') return 1; /* Windows form such as C:\ */
    return 0;
}

/* Ends with ".mvs"? (relative file paths must include the extension) */
static int ends_with_mvs(const char *p) {
    size_t n = strlen(p);
    return n >= 4 && strcmp(p + n - 4, ".mvs") == 0;
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

/* Collect a module's top-level symbol names (struct/trait/non-method functions) as "|a|b|c|" */
static char *collect_defs(Node *prog) {
    size_t cap = 256, len = 1;
    char *s = (char *)malloc(cap);
    s[0] = '|'; s[1] = '\0';
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        const char *nm = NULL;
        if ((d->kind == ND_FUNC && !d->is_method) || d->kind == ND_STRUCT_DECL ||
            d->kind == ND_TRAIT || d->kind == ND_ENUM_DECL)
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

/* Which package does a path belong to? "core"/"core/x" -> coredir, else stddir */
static const char *package_dir(Loader *L, const char *path) {
    if (strcmp(path, "core") == 0 || strncmp(path, "core/", 5) == 0) return L->coredir;
    return L->stddir;
}

/* Resolve a "module path" (not a bare package) to a real file; returns 1 on success
 *   absolute            -> use as is
 *   ends in .mvs        -> relative file against base_dir (e.g. ./lib.mvs)
 *   otherwise (has '/') -> package submodule, e.g. "std/string" -> <stddir>/string.mvs,
 *                          "core/mem" -> <coredir>/mem.mvs (allowed under --nostd) */
static int resolve_module_file(Loader *L, const char *path, const char *base_dir, char *out, size_t n) {
    if (is_absolute(path)) { snprintf(out, n, "%s", path); return 1; }
    if (ends_with_mvs(path)) { snprintf(out, n, "%s/%s", base_dir, path); return 1; }
    const char *pdir = package_dir(L, path);
    if (L->nostd && pdir != L->coredir) {
        fprintf(stderr, "error: cannot import package module '%s' in --nostd (freestanding) mode\n", path);
        fprintf(stderr, "help: the \"core\" package is pure MVS and stays available, e.g. import { copy } from \"core/mem\"\n");
        L->had_error = 1; return 0;
    }
    const char *slash = strchr(path, '/');
    const char *sub = slash ? slash + 1 : path;       /* strip "std/" or "core/" */
    snprintf(out, n, "%s/%s.mvs", pdir, sub);
    return 1;
}

/* Handle one import statement; base_dir is the folder of the file containing the import.
 * Supports 3 forms (see GUIDE):
 *   A) import { io, net } from "std"            submodule namespaces (io.out, net.X)
 *   B) import { String } from "std/string"      pull symbols directly + verify they exist
 *      import { factorial } from "./lib.mvs"
 *   C) import lib from "./lib.mvs"               whole module as namespace lib (lib.factorial)
 *      import str from "std/string" */
static void handle_import(Loader *L, Node *imp, const char *base_dir) {
    const char *path = imp->str_val;
    if (!path) return;

    /* Form C: alias import (no {}); the whole module becomes namespace = imp->name */
    if (imp->name) {
        if (is_package_path(path)) {
            fprintf(stderr, "error: cannot alias-import a whole package '%s'; use a module path like \"%s/<module>\"\n", path, path);
            L->had_error = 1; return;
        }
        char file[1024];
        if (!resolve_module_file(L, path, base_dir, file, sizeof(file))) return;
        if (!read_file_exists(file)) {
            fprintf(stderr, "error: cannot open module '%s' (looked for %s)\n", path, file);
            L->had_error = 1; return;
        }
        char canon[1024]; canonicalize(file, canon, sizeof(canon));
        register_ns(L, imp->name, canon);     /* guard: same alias must not point to another module */
        load_module(L, file, imp->name);
        return;
    }

    if (is_package_path(path)) {
        /* Form A: namespace import of submodules */
        const char *pdir = package_dir(L, path);
        if (L->nostd && pdir != L->coredir) {
            fprintf(stderr, "error: cannot import package '%s' in --nostd (freestanding) mode\n", path);
            fprintf(stderr, "help: the \"core\" package is pure MVS and stays available, e.g. import { mem } from \"core\"\n");
            L->had_error = 1;
            return;
        }
        for (int i = 0; i < imp->nitems; i++) {
            const char *name = imp->items[i]->name;
            char file[1024];
            snprintf(file, sizeof(file), "%s/%s.mvs", pdir, name);
            if (!read_file_exists(file)) {
                fprintf(stderr, "error: package '%s' has no module '%s' (looked for %s)\n", path, name, file);
                L->had_error = 1; continue;
            }
            char canon[1024]; canonicalize(file, canon, sizeof(canon));
            register_ns(L, name, canon);      /* io/net are namespaces; guard against binding another module */
            load_module(L, file, name);
        }
    } else {
        /* Form B: symbol import; pull symbols directly (namespace = "") + verify they exist */
        char file[1024];
        if (!resolve_module_file(L, path, base_dir, file, sizeof(file))) return;
        load_module(L, file, "");

        char canon[1024];
        canonicalize(file, canon, sizeof(canon));
        int idx = find_loaded(L, canon);
        if (idx >= 0 && L->defs[idx]) {
            for (int i = 0; i < imp->nitems; i++) {
                const char *nm = imp->items[i]->name;
                if (!defs_has(L->defs[idx], nm)) {
                    fprintf(stderr, "error: module '%s' has no exported symbol '%s'\n", path, nm);
                    L->had_error = 1;
                }
            }
        }
    }
}

/* Load one module from path and append all its definitions to the result (tag namespace if any) */
static void load_module(Loader *L, const char *path, const char *ns) {
    /* Canonicalize to an absolute path first so "./lib.mvs" and "lib.mvs" count as
     * the same file; prevents reloads and loops caused by different path spellings */
    char canon[1024];
    canonicalize(path, canon, sizeof(canon));

    int prev = find_loaded(L, canon);
    if (prev >= 0) {                            /* already loaded; skip reload (supports diamond imports) */
        /* If bound under two different namespaces (alias clash), symbols would live under
         * different names, so report a clear error. (An empty ns = symbol/direct import does not
         * clash because struct/trait symbols are global anyway.) */
        const char *prev_ns = L->loaded_ns[prev] ? L->loaded_ns[prev] : "";
        const char *now_ns = ns ? ns : "";
        if (strcmp(prev_ns, now_ns) != 0) {
            /* Mixing import forms for one module used to dedup silently; the second import's
             * free functions then live under the first form's namespace and are unreachable
             * under the requested one. Structs/traits/methods are global either way, so this
             * is a warning, not an error; two DIFFERENT namespaces stay a hard error. */
            if (prev_ns[0] && now_ns[0]) {
                fprintf(stderr, "error: module '%s' is already imported as namespace '%s'; cannot also import it as '%s'\n",
                        path, prev_ns, now_ns);
                L->had_error = 1;
            } else if (prev_ns[0]) {
                fprintf(stderr, "warning: module '%s' was already imported as namespace '%s'; free functions from this "
                        "symbol import remain reachable only as '%s.<name>' (structs/traits work either way)\n",
                        path, prev_ns, prev_ns);
            } else {
                fprintf(stderr, "warning: module '%s' was already symbol-imported; its free functions are NOT reachable "
                        "as '%s.<name>' (structs/traits work either way)\n", path, now_ns);
            }
        }
        return;
    }
    /* Circular import check: this module is already being loaded in the current call chain (A -> B -> A) */
    for (int i = 0; i < L->nloading; i++)
        if (strcmp(L->loading[i], canon) == 0) {
            fprintf(stderr, "error: circular import detected involving module '%s'\n", path);
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

    for (int i = 0; i < prog->nitems; i++) {
        Node *it = prog->items[i];
        if (!cfg_matches(L, it)) continue;    /* @compile says this item is not for this target */
        if (it->kind == ND_IMPORT) {
            handle_import(L, it, dir);            /* nested import: keep loading recursively */
        } else {
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

    *had_error = L.had_error;
    return L.result;
}
