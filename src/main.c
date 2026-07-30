/*
 * main.c - MVS compiler entry point and driver of the whole pipeline
 *
 * Steps:
 *   1. Read the .mvs source file
 *   2. Parse into an AST                        (lexer + parser)
 *   3. Generate the .asm assembly file          (codegen)
 *   4. Assemble into a .obj object file         (nasm -f win64)
 *   5. Link into a .exe executable              (clang)
 *
 * Usage:
 *   mvs <input.mvs> [-o output.exe] [-S] [--keep] [--emit-ast]
 *     -S          emit only the .asm file, then stop (no nasm/clang)
 *     --keep      keep intermediate files (.asm, .obj) instead of deleting them
 *     -o <file>   set the output file name
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "generic.h"
#include "codegen.h"
#include "arch/common.h"   /* mvs_bounds_checks (the --no-check switch) */
#include "diag.h"

#ifdef _WIN32
#include <io.h>        /* _findfirst/_findnext for the built-in test runner */
#else
#include <dirent.h>
#include <sys/stat.h>  /* stat() for directory detection in the test runner */
#endif

#define MVS_VERSION "0.2.0"
#define PATHBUF 1024

/* popen is spelled _popen on Windows; everything else is portable */
#ifdef _WIN32
#define POPEN  _popen
#define PCLOSE _pclose
#else
#define POPEN  popen
#define PCLOSE pclose
#endif

/* Run "<tool> --version", read the first line into ver; returns 1 if the tool exists (and works).
 * Used to check that the user has nasm/clang installed and to report the version in use */
static int tool_version(const char *name, char *ver, size_t vn) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s --version 2>&1", name);
    FILE *f = POPEN(cmd, "r");
    if (!f) return 0;
    ver[0] = '\0';
    if (fgets(ver, (int)vn, f)) { char *nl = strchr(ver, '\n'); if (nl) *nl = '\0'; }
    int rc = PCLOSE(f);
    return (rc == 0 && ver[0] != '\0');
}

/* Copy the base name (with the .mvs extension stripped) into dst[PATHBUF].
 * Only strip a dot after the last path separator, so folders with dots (e.g. my.proj) are untouched */
static int base_name(const char *path, char *dst) {
    if (strlen(path) >= PATHBUF) { fprintf(stderr, "error: input path too long\n"); return 0; }
    snprintf(dst, PATHBUF, "%s", path);
    char *slash = strrchr(dst, '/');
    char *bslash = strrchr(dst, '\\');
    char *sep = slash > bslash ? slash : bslash;
    char *dot = strrchr(dst, '.');
    if (dot && (!sep || dot > sep)) *dot = '\0';
    return 1;
}

/* Copy the directory part of path into dst[PATHBUF] (handles both / and \); "." if none */
static void dir_name(const char *path, char *dst) {
    snprintf(dst, PATHBUF, "%s", path);
    char *slash = strrchr(dst, '/');
    char *bslash = strrchr(dst, '\\');
    char *cut = slash > bslash ? slash : bslash;
    if (cut) *cut = '\0';
    else strcpy(dst, ".");
}

/* ---------- -O: peephole cleanup of the NASM output (x86 targets) ----------
 *
 * A tiny text pass over the generated .asm. Patterns are exact-line matches and
 * conservative by construction (a label or any unexpected line breaks the window):
 *   P1: mov REG, rax  directly followed by  mov rax, REG   -> drop the second (no-op)
 *   P2: mov rax, IMM ; mov REG, rax ; <line redefining rax> -> mov REG, IMM
 * Program OUTPUT is unchanged; only redundant instructions disappear. */

/* the "REG" of "    mov REG, rax": returns the register name or NULL */
static const char *peep_mov_from_rax(const char *line, char *reg, size_t rn) {
    if (strncmp(line, "    mov ", 8) != 0) return NULL;
    const char *comma = strstr(line + 8, ", rax");
    if (!comma || comma[5] != '\n' || comma[6] != '\0') return NULL;
    size_t len = (size_t)(comma - (line + 8));
    if (len == 0 || len + 1 > rn) return NULL;
    static const char *regs[] = { "rcx", "rdx", "r8", "r9", "r10", "r11", "rdi", "rsi" };
    memcpy(reg, line + 8, len); reg[len] = '\0';
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        if (strcmp(reg, regs[i]) == 0) return reg;
    return NULL;
}

/* does this line load an integer immediate into rax? ("    mov rax, 42") */
static const char *peep_rax_imm(const char *line) {
    if (strncmp(line, "    mov rax, ", 13) != 0) return NULL;
    const char *v = line + 13;
    if (*v == '-') v++;
    if (*v < '0' || *v > '9') return NULL;
    while (*v >= '0' && *v <= '9') v++;
    return (*v == '\n' && v[1] == '\0') ? line + 13 : NULL;
}

/* does this line overwrite rax without reading it? (mov/lea rax, ...) */
static int peep_redefines_rax(const char *line) {
    return (strncmp(line, "    mov rax, ", 13) == 0 && !strstr(line, "rax]")) ||
           strncmp(line, "    lea rax, ", 13) == 0;
}

static void peephole_x86(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t cap = 1024, n = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        size_t bl = strlen(buf);      /* normalize CRLF -> LF so patterns match on Windows too */
        if (bl >= 2 && buf[bl - 2] == '\r' && buf[bl - 1] == '\n') { buf[bl - 2] = '\n'; buf[bl - 1] = '\0'; }
        if (n + 1 >= cap) { cap *= 2; lines = (char **)realloc(lines, cap * sizeof(char *)); }
        lines[n++] = strdup(buf);
    }
    fclose(f);

    int removed = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        char reg[8];
        /* P2: forward an immediate when rax is immediately redefined afterwards */
        const char *imm = peep_rax_imm(lines[i]);
        if (imm && i + 2 < n && peep_mov_from_rax(lines[i + 1], reg, sizeof(reg)) &&
            peep_redefines_rax(lines[i + 2])) {
            char merged[64];
            snprintf(merged, sizeof(merged), "    mov %s, %s", reg, imm); /* imm keeps its \n */
            free(lines[i]); lines[i] = strdup(merged);
            free(lines[i + 1]);
            memmove(&lines[i + 1], &lines[i + 2], (n - i - 2) * sizeof(char *));
            n--; removed++;
            continue;
        }
        /* P1: mov REG, rax ; mov rax, REG -> the second is a no-op */
        if (peep_mov_from_rax(lines[i], reg, sizeof(reg))) {
            char back[64];
            snprintf(back, sizeof(back), "    mov rax, %s\n", reg);
            if (strcmp(lines[i + 1], back) == 0) {
                free(lines[i + 1]);
                memmove(&lines[i + 1], &lines[i + 2], (n - i - 2) * sizeof(char *));
                n--; removed++;
            }
        }
    }

    if (removed) {
        f = fopen(path, "wb");
        if (f) {
            for (size_t i = 0; i < n; i++) fputs(lines[i], f);
            fclose(f);
        }
        printf("[mvs] -O removed %d redundant instruction(s)\n", removed);
    }
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* The same pass for the AArch64 backend (GNU as syntax, x-registers):
 *   P1: mov xR, x0 ; mov x0, xR              -> drop the second (no-op)
 *   P2: mov x0, #N ; mov xR, x0 ; <redef x0> -> mov xR, #N */
static const char *peep_a64_mov_from_x0(const char *line, char *reg, size_t rn) {
    if (strncmp(line, "    mov ", 8) != 0) return NULL;
    const char *comma = strstr(line + 8, ", x0");
    if (!comma || comma[4] != '\n' || comma[5] != '\0') return NULL;
    size_t len = (size_t)(comma - (line + 8));
    if (len < 2 || len + 1 > rn || line[8] != 'x') return NULL;
    memcpy(reg, line + 8, len); reg[len] = '\0';
    for (size_t i = 1; i < len; i++)
        if (reg[i] < '0' || reg[i] > '9') return NULL;   /* only x1..x30 style names */
    return strcmp(reg, "x0") == 0 ? NULL : reg;
}

static const char *peep_a64_x0_imm(const char *line) {
    /* the backend loads integer constants as "ldr x0, =N" (literal pool) */
    if (strncmp(line, "    ldr x0, =", 13) != 0) return NULL;
    const char *v = line + 13;
    if (*v == '-') v++;
    if (*v < '0' || *v > '9') return NULL;
    while (*v >= '0' && *v <= '9') v++;
    return (*v == '\n' && v[1] == '\0') ? line + 13 : NULL;
}

static int peep_a64_redefines_x0(const char *line) {
    return strncmp(line, "    mov x0, ", 12) == 0 ||
           (strncmp(line, "    ldr x0, ", 12) == 0 && !strstr(line, "x0]")) ||
           strncmp(line, "    adrp x0, ", 13) == 0 ||
           strncmp(line, "    sub x0, x29, ", 17) == 0;   /* addr_local: reads only the frame pointer */
}

static void peephole_a64(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t cap = 1024, n = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        size_t bl = strlen(buf);
        if (bl >= 2 && buf[bl - 2] == '\r' && buf[bl - 1] == '\n') { buf[bl - 2] = '\n'; buf[bl - 1] = '\0'; }
        if (n + 1 >= cap) { cap *= 2; lines = (char **)realloc(lines, cap * sizeof(char *)); }
        lines[n++] = strdup(buf);
    }
    fclose(f);

    int removed = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        char reg[8];
        const char *imm = peep_a64_x0_imm(lines[i]);
        if (imm && i + 2 < n && peep_a64_mov_from_x0(lines[i + 1], reg, sizeof(reg)) &&
            peep_a64_redefines_x0(lines[i + 2])) {
            char merged[64];
            snprintf(merged, sizeof(merged), "    ldr %s, =%s", reg, imm); /* imm keeps its \n */
            free(lines[i]); lines[i] = strdup(merged);
            free(lines[i + 1]);
            memmove(&lines[i + 1], &lines[i + 2], (n - i - 2) * sizeof(char *));
            n--; removed++;
            continue;
        }
        if (peep_a64_mov_from_x0(lines[i], reg, sizeof(reg))) {
            char back[64];
            snprintf(back, sizeof(back), "    mov x0, %s\n", reg);
            if (strcmp(lines[i + 1], back) == 0) {
                free(lines[i + 1]);
                memmove(&lines[i + 1], &lines[i + 2], (n - i - 2) * sizeof(char *));
                n--; removed++;
            }
        }
    }

    if (removed) {
        f = fopen(path, "wb");
        if (f) {
            for (size_t i = 0; i < n; i++) fputs(lines[i], f);
            fclose(f);
        }
        printf("[mvs] -O removed %d redundant instruction(s)\n", removed);
    }
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* ---------- --test-main: synthesize main() for a test file ----------
 *
 * A test file (somefile.test.mvs) defines functions named test_* and no main.
 * This pass appends a generated main() that calls every test_* from the ENTRY
 * file in order and prints "  ok    <name>" after each returns (an assertion
 * failure inside std/test prints FAIL and exits first). A file that already
 * has a main() is left untouched. Returns the number of tests, -1 if none. */
static int synthesize_test_main(Node *program) {
    for (int i = 0; i < program->nitems; i++) {
        Node *f = program->items[i];
        if (f->kind == ND_FUNC && f->body && f->name && strcmp(f->name, "main") == 0)
            return 0;                          /* the file brings its own harness */
    }
    /* is std/test imported? then test.begin(name) exists and failures can be
     * attributed to the test that raised them */
    int have_begin = 0;
    for (int i = 0; i < program->nitems; i++) {
        Node *f = program->items[i];
        if (f->kind == ND_FUNC && f->name && f->ns && strcmp(f->ns, "test") == 0 &&
            strcmp(f->name, "begin") == 0) { have_begin = 1; break; }
    }
    Node *body = node_new(ND_BLOCK, 0);
    int ntests = 0;
    for (int i = 0; i < program->nitems; i++) {
        Node *f = program->items[i];
        if (f->kind != ND_FUNC || !f->body || f->is_method || f->ngen > 0 || !f->name) continue;
        if (!f->is_test && strncmp(f->name, "test_", 5) != 0) continue; /* @test or test_* naming */
        if (!diag_is_primary(f->file)) continue;   /* only the entry file's own tests */
        if (have_begin) {
            /* test.begin("<name>") so a FAIL line names the test it came from */
            Node *bn = node_new(ND_STR, 0);
            bn->str_val = strdup(f->name); bn->str_len = (int)strlen(f->name); bn->type = TYPE_STR;
            Node *bc = node_new(ND_CALL, 0);
            bc->operand = node_new(ND_MEMBER, 0);
            bc->operand->operand = node_new(ND_IDENT, 0);
            bc->operand->operand->name = strdup("test");
            bc->operand->name = strdup("begin");
            node_add_item(bc, bn);
            Node *bs = node_new(ND_EXPR_STMT, 0);
            bs->operand = bc;
            node_add_item(body, bs);
        }
        Node *call = node_new(ND_CALL, 0);
        call->operand = node_new(ND_IDENT, 0);
        call->operand->name = strdup(f->name);
        Node *st = node_new(ND_EXPR_STMT, 0);
        st->operand = call;
        node_add_item(body, st);
        char buf[300];
        snprintf(buf, sizeof(buf), "  ok    %s\n", f->name);
        Node *msg = node_new(ND_STR, 0);
        msg->str_val = strdup(buf); msg->str_len = (int)strlen(buf); msg->type = TYPE_STR;
        Node *pc = node_new(ND_CALL, 0);
        pc->operand = node_new(ND_IDENT, 0);
        pc->operand->name = strdup("printf");
        node_add_item(pc, msg);
        Node *ps = node_new(ND_EXPR_STMT, 0);
        ps->operand = pc;
        node_add_item(body, ps);
        ntests++;
    }
    if (ntests == 0) return -1;
    Node *ret = node_new(ND_RETURN, 0);
    ret->operand = node_new(ND_INT, 0);
    ret->operand->int_val = 0; ret->operand->type = TYPE_I64;
    node_add_item(body, ret);
    /* declare extern printf for the ok lines (repeated extern declarations are fine) */
    Node *pf = node_new(ND_FUNC, 0);
    pf->name = strdup("printf"); pf->is_extern = 1; pf->type = TYPE_I32;
    Node *p0 = node_new(ND_PARAM, 0);
    p0->name = strdup("fmt"); p0->type = TYPE_STR;
    node_add_item(pf, p0);
    node_add_item(program, pf);
    Node *mn = node_new(ND_FUNC, 0);
    mn->name = strdup("main"); mn->type = TYPE_I8; mn->body = body;
    node_add_item(program, mn);
    return ntests;
}

/* ---------- `mvs test`: the built-in test runner ----------
 *
 * Runs the portable core of the suite from the repo root without PowerShell:
 *   - run-pass: every golden in tests/expected/ (compile + run + diff stdout)
 *   - compile-fail: every .mvs file in tests/compile_fail must fail with its //~ ERROR text
 * On Windows it uses the native win64 pipeline; on Linux it compiles with
 * --target elf64 and links with gcc. C-interop and cross-target runs stay in
 * scripts/ + CI (they need extra toolchains). */

#define TR_MAX_FILES 512
#define TR_NAME_LEN  256

/* list the files in dir with the given extension (extension stripped), sorted */
static int tr_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int tr_list_dir(const char *dir, const char *ext, char names[][TR_NAME_LEN], int max) {
    int n = 0;
    size_t el = strlen(ext);
#ifdef _WIN32
    char pat[PATHBUF];
    snprintf(pat, sizeof(pat), "%s/*%s", dir, ext);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 0;
    do {
        size_t l = strlen(fd.name);
        if (l > el && n < max) {
            snprintf(names[n], TR_NAME_LEN, "%.*s", (int)(l - el), fd.name);
            n++;
        }
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > el && strcmp(e->d_name + l - el, ext) == 0 && n < max) {
            snprintf(names[n], TR_NAME_LEN, "%.*s", (int)(l - el), e->d_name);
            n++;
        }
    }
    closedir(d);
#endif
    qsort(names, (size_t)n, TR_NAME_LEN, tr_cmp);
    return n;
}

/* --- discovery of user test files (somefile.test.mvs) --- */

#define TR_MAX_TF 256
static char g_tf[TR_MAX_TF][PATHBUF + 320];   /* discovered .test.mvs paths */
static int  g_ntf = 0;

static int tr_is_test_file(const char *name) {
    size_t l = strlen(name);
    return l > 9 && strcmp(name + l - 9, ".test.mvs") == 0;
}

/* recursively collect *.test.mvs under dir (skips .git) */
static void tr_find_tests(const char *dir) {
    char full[PATHBUF + 300];
#ifdef _WIN32
    char pat[PATHBUF + 310];
    snprintf(pat, sizeof(pat), "%s/*", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0 || strcmp(fd.name, ".git") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", dir, fd.name);
        if (fd.attrib & _A_SUBDIR) tr_find_tests(full);
        else if (tr_is_test_file(fd.name) && g_ntf < TR_MAX_TF)
            snprintf(g_tf[g_ntf++], sizeof(g_tf[0]), "%s", full);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 || strcmp(e->d_name, ".git") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) tr_find_tests(full);
        else if (tr_is_test_file(e->d_name) && g_ntf < TR_MAX_TF)
            snprintf(g_tf[g_ntf++], sizeof(g_tf[0]), "%s", full);
    }
    closedir(d);
#endif
}

static int tr_tf_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* golden name -> example path: "01_language_arrays" -> "examples/01_language/arrays"
 * (groups are NN_word, so the SECOND underscore is the separator; no underscore = top level) */
static void tr_name_to_path(const char *name, char *out, size_t on) {
    const char *u1 = strchr(name, '_');
    const char *u2 = u1 ? strchr(u1 + 1, '_') : NULL;
    if (!u2) { snprintf(out, on, "examples/%s", name); return; }
    snprintf(out, on, "examples/%.*s/%s", (int)(u2 - name), name, u2 + 1);
}

/* read a whole file/stream into a malloc'd string */
static char *tr_read_all(FILE *f) {
    size_t cap = 4096, len = 0;
    char *s = (char *)malloc(cap);
    size_t got;
    while ((got = fread(s + len, 1, cap - len - 1, f)) > 0) {
        len += got;
        if (cap - len < 1024) { cap *= 2; s = (char *)realloc(s, cap); }
    }
    s[len] = '\0';
    return s;
}

/* normalize in place: drop \r, trim trailing whitespace/newlines */
static void tr_normalize(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) if (*r != '\r') *w++ = *r;
    *w = '\0';
    while (w > s && (w[-1] == '\n' || w[-1] == ' ' || w[-1] == '\t')) *--w = '\0';
}

/* run a command line, capture stdout+stderr, return the exit status */
static int tr_capture(const char *cmdline, char **out) {
    char cmd[PATHBUF * 2];
    snprintf(cmd, sizeof(cmd), "%s 2>&1", cmdline);
    FILE *p = POPEN(cmd, "r");
    if (!p) { *out = strdup(""); return -1; }
    *out = tr_read_all(p);
    return PCLOSE(p);
}

static int run_test_suite(const char *argv0, const char *path_arg) {
    static char names[TR_MAX_FILES][TR_NAME_LEN];
    char cmd[PATHBUF * 2], path[PATHBUF], gold_path[PATHBUF];
    int pass = 0, fail = 0;

    /* discover user test files first: an explicit file, a directory, or the
     * current tree (recursively); the repo golden suite joins in when
     * tests/expected/ exists next to us */
    g_ntf = 0;
    if (path_arg && tr_is_test_file(path_arg)) {
        snprintf(g_tf[g_ntf++], sizeof(g_tf[0]), "%s", path_arg);
    } else {
        tr_find_tests(path_arg ? path_arg : ".");
        qsort(g_tf, (size_t)g_ntf, sizeof(g_tf[0]), tr_tf_cmp);
    }
    int ngold = path_arg ? 0 : tr_list_dir("tests/expected", ".txt", names, TR_MAX_FILES);
    if (ngold == 0 && g_ntf == 0) {
        fprintf(stderr, "error: no tests found: no *.test.mvs file%s%s\n",
                path_arg ? " under " : " here (and no tests/expected/ repo suite)",
                path_arg ? path_arg : "");
        return 1;
    }

    /* --- user test files (somefile.test.mvs) --- */
    /* g_tf rows/names rows are formatted with an explicit "%.*s" precision: gcc's
     * -Wformat-truncation at -O2 otherwise assumes a row could span the whole 2D
     * array and flags every snprintf as possibly truncating */
    int tfw = (int)sizeof(g_tf[0]) - 1;
    if (g_ntf > 0) printf("=== test files (*.test.mvs) ===\n");
    for (int i = 0; i < g_ntf; i++) {
        printf("--- %s\n", g_tf[i]);
        char *out = NULL;
        char tbase[PATHBUF + 320];
        snprintf(tbase, sizeof(tbase), "%.*s", tfw, g_tf[i]);
        tbase[strlen(tbase) - 4] = '\0';           /* strip ".mvs" -> base ends in ".test" */
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "\"%s\" %.*s --test-main", argv0, tfw, g_tf[i]);
        int crc = tr_capture(cmd, &out);
        if (crc != 0) { printf("  FAIL  (compile)\n%s", out); free(out); fail++; continue; }
        free(out);
        char texe[PATHBUF + 340];
        snprintf(texe, sizeof(texe), "%.*s.exe", tfw, tbase);
        for (char *c = texe; *c; c++) if (*c == '/') *c = '\\';
        int rrc = tr_capture(texe, &out);
        printf("%s", out);
        snprintf(texe, sizeof(texe), "%.*s.exe", tfw, tbase); remove(texe);
        snprintf(texe, sizeof(texe), "%.*s.obj", tfw, tbase); remove(texe);
#else
        snprintf(cmd, sizeof(cmd), "\"%s\" %.*s --test-main --target elf64", argv0, tfw, g_tf[i]);
        int crc = tr_capture(cmd, &out);
        if (crc != 0) { printf("  FAIL  (compile)\n%s", out); free(out); fail++; continue; }
        free(out);
        snprintf(cmd, sizeof(cmd), "gcc \"%.*s.o\" -o /tmp/mvs_selftest -no-pie -lm -lpthread", tfw, tbase);
        int lrc = tr_capture(cmd, &out);
        if (lrc != 0) { printf("  FAIL  (link)\n%s", out); free(out); fail++; continue; }
        free(out);
        int rrc = tr_capture("/tmp/mvs_selftest", &out);
        printf("%s", out);
        snprintf(cmd, sizeof(cmd), "%.*s.o", tfw, tbase); remove(cmd);
#endif
        if (rrc != 0) { printf("  FAILED (exit %d)\n", rrc); fail++; }
        else pass++;
        free(out);
    }
    if (ngold == 0) {
        printf("\n");
        if (fail > 0) { printf("FAILED: %d failure(s), %d passed\n", fail, pass); return 1; }
        printf("ALL PASS: %d test file(s)\n", pass);
        return 0;
    }

    printf("=== run-pass (golden output, %s) ===\n",
#ifdef _WIN32
           "win64"
#else
           "elf64"
#endif
    );
    for (int i = 0; i < ngold; i++) {
        if (strncmp(names[i], "interop_", 8) == 0) continue;   /* needs a C toolchain; covered by scripts/CI */
#ifndef _WIN32
        /* its golden embeds the Windows argv[0] path; the Linux CI scripts skip it too */
        if (strcmp(names[i], "01_language_args") == 0) continue;
#endif
        tr_name_to_path(names[i], path, sizeof(path));
        const char *runargs = strcmp(names[i], "01_language_args") == 0 ? " 10 32" : "";

        char *out = NULL;
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "\"%s\" %s.mvs", argv0, path);
        int crc = tr_capture(cmd, &out);
        if (crc != 0) { printf("  FAIL  %s (compile)\n%s", names[i], out); free(out); fail++; continue; }
        free(out);
        char exe[PATHBUF + 32];
        snprintf(exe, sizeof(exe), "%s.exe%s", path, runargs);
        for (char *c = exe; *c; c++) if (*c == '/') *c = '\\';   /* cmd.exe wants backslashes */
        int rrc = tr_capture(exe, &out);
#else
        snprintf(cmd, sizeof(cmd), "\"%s\" %s.mvs --target elf64", argv0, path);
        int crc = tr_capture(cmd, &out);
        if (crc != 0) { printf("  FAIL  %s (compile)\n%s", names[i], out); free(out); fail++; continue; }
        free(out);
        snprintf(cmd, sizeof(cmd), "gcc %s.o -o /tmp/mvs_selftest -no-pie -lm -lpthread", path);
        int lrc = tr_capture(cmd, &out);
        if (lrc != 0) { printf("  FAIL  %s (link)\n%s", names[i], out); free(out); fail++; continue; }
        free(out);
        snprintf(cmd, sizeof(cmd), "/tmp/mvs_selftest%s", runargs);
        int rrc = tr_capture(cmd, &out);
        snprintf(cmd, sizeof(cmd), "%s.o", path); remove(cmd);
#endif
        /* diff against the golden */
        snprintf(gold_path, sizeof(gold_path), "tests/expected/%.*s.txt", TR_NAME_LEN - 1, names[i]);
        FILE *gf = fopen(gold_path, "rb");
        char *want = gf ? tr_read_all(gf) : strdup("");
        if (gf) fclose(gf);
        tr_normalize(out); tr_normalize(want);
        if (rrc != 0) {
            printf("  FAIL  %s (exit %d)\n%s\n", names[i], rrc, out); fail++;
        } else if (strcmp(out, want) != 0) {
            printf("  FAIL  %s (output mismatch)\n--- expected ---\n%s\n--- got ---\n%s\n", names[i], want, out);
            fail++;
        } else {
            printf("  ok    %s\n", names[i]); pass++;
        }
        free(out); free(want);
    }

    printf("=== compile-fail (expected errors) ===\n");
    int nfails = tr_list_dir("tests/compile_fail", ".mvs", names, TR_MAX_FILES);
    for (int i = 0; i < nfails; i++) {
        snprintf(path, sizeof(path), "tests/compile_fail/%.*s.mvs", TR_NAME_LEN - 1, names[i]);
        FILE *sf = fopen(path, "rb");
        if (!sf) { printf("  FAIL  %s (unreadable)\n", names[i]); fail++; continue; }
        char first[512] = "";
        if (!fgets(first, sizeof(first), sf)) first[0] = '\0';
        fclose(sf);
        char *want = strstr(first, "//~ ERROR:");
        if (!want) { printf("  FAIL  %s (no //~ ERROR header)\n", names[i]); fail++; continue; }
        want += 10;
        while (*want == ' ') want++;
        char *end = want + strlen(want);
        while (end > want && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';

        char *out = NULL;
        snprintf(cmd, sizeof(cmd), "\"%s\" %s", argv0, path);
        int crc = tr_capture(cmd, &out);
        if (crc == 0) {
            printf("  FAIL  %s (compiled, expected an error)\n", names[i]); fail++;
        } else if (!strstr(out, want)) {
            printf("  FAIL  %s (missing '%s')\n%s\n", names[i], want, out); fail++;
        } else {
            printf("  ok    %s\n", names[i]); pass++;
        }
        free(out);
        /* remove anything a partially-successful compile left behind */
        char base[PATHBUF];
        snprintf(base, sizeof(base), "tests/compile_fail/%.*s", TR_NAME_LEN - 1, names[i]);
        const char *exts[] = { ".asm", ".obj", ".exe", ".o", ".s" };
        for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
            char junk[PATHBUF + 8];
            snprintf(junk, sizeof(junk), "%s%s", base, exts[e]);
            remove(junk);
        }
    }

    printf("\n");
    if (fail > 0) { printf("FAILED: %d failure(s), %d passed\n", fail, pass); return 1; }
    printf("ALL PASS: %d test(s)\n", pass);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("mvs %s\n", MVS_VERSION);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "test") == 0)
        return run_test_suite(argv[0], argc >= 3 ? argv[2] : NULL);
    if (argc < 2) {
        fprintf(stderr,
            "MVS compiler %s\n"
            "usage: %s <input.mvs> [options]\n"
            "       %s test [path]     run tests: every *.test.mvs under path (default .),\n"
            "                          plus the repo golden suite when tests/expected/ exists\n"
            "  -o <file>     set the output file name\n"
            "  -S            emit assembly (.asm) only, then stop\n"
            "  -c            emit an object file (.obj) only (for linking with C)\n"
            "  -O            peephole-optimize the generated assembly\n"
            "  --no-check    drop the runtime bounds checks on [T; N] indexing\n"
            "  -g            emit debug line info (step through .mvs in gdb/lldb; keeps the .asm)\n"
            "  --nostd       freestanding mode: no std/C runtime/OS (emits .obj) - for OS dev\n"
            "  --target <t>  target: win64 (default), elf64 (x86-64 Linux), arm64 (AArch64 Linux)\n"
            "  --test-main   generate main() from the file's test_* functions (used by `mvs test`)\n"
            "  --keep        keep intermediate files (.asm, .obj)\n"
            "  --version     print the compiler version\n", MVS_VERSION, argv[0], argv[0]);
        return 1;
    }

    /* parse command line arguments */
    const char *input = NULL;
    const char *output = NULL;
    int only_asm = 0;  /* -S */
    int emit_obj = 0;  /* -c / --emit-obj : stop at the .obj file */
    int nostd = 0;     /* --nostd : freestanding (no std/CRT dependency) */
    int keep = 0;      /* --keep */
    int optimize = 0;  /* -O : peephole pass over the generated assembly */
    int test_main = 0; /* --test-main : synthesize main() from test_* functions */
    TargetArch arch = ARCH_X86_64_WIN;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) only_asm = 1;
        else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--emit-obj") == 0) emit_obj = 1;
        else if (strcmp(argv[i], "-O") == 0) optimize = 1;
        else if (strcmp(argv[i], "--no-check") == 0) mvs_bounds_checks = 0;
        else if (strcmp(argv[i], "-g") == 0) { mvs_debug_lines = 1; keep = 1; }
        else if (strcmp(argv[i], "--test-main") == 0) test_main = 1;
        else if (strcmp(argv[i], "--nostd") == 0) { nostd = 1; emit_obj = 1; } /* freestanding -> produce .obj */
        else if (strcmp(argv[i], "--keep") == 0) keep = 1;
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcmp(t, "win64") == 0) arch = ARCH_X86_64_WIN;
            else if (strcmp(t, "elf64") == 0) arch = ARCH_X86_64_SYSV;
            else if (strcmp(t, "arm64") == 0) arch = ARCH_ARM64_LINUX;
            else { fprintf(stderr, "error: unknown target '%s' (supported: win64, elf64, arm64)\n", t); return 1; }
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output = argv[++i];
        else if (argv[i][0] != '-') input = argv[i];
    }
    if (!input) { fprintf(stderr, "error: no input file\n"); return 1; }
    /* elf64/arm64 objects cannot be linked into a Windows exe here: stop at the .o
     * (link on a Linux system, e.g. `gcc file.o -o file`) */
    if (arch == ARCH_X86_64_SYSV || arch == ARCH_ARM64_LINUX) emit_obj = 1;

    /* derive the various output file names from the base name */
    char base[PATHBUF];
    if (!base_name(input, base)) return 1;
    char asm_path[PATHBUF + 8], obj_path[PATHBUF + 8], exe_path[PATHBUF + 8];
    snprintf(asm_path, sizeof(asm_path), arch == ARCH_ARM64_LINUX ? "%s.s" : "%s.asm", base);
    snprintf(obj_path, sizeof(obj_path), arch == ARCH_X86_64_WIN ? "%s.obj" : "%s.o", base);
    if (output) snprintf(exe_path, sizeof(exe_path), "%s", output);
    else        snprintf(exe_path, sizeof(exe_path), "%s.exe", base);

    /* locate the library folders: MVS_STD / MVS_CORE env vars win, else next to mvs.exe */
    char stddir[PATHBUF + 8], coredir[PATHBUF + 8];
    const char *env_std = getenv("MVS_STD");
    const char *env_core = getenv("MVS_CORE");
    char exedir[1024];
    dir_name(argv[0], exedir);
    if (env_std) snprintf(stddir, sizeof(stddir), "%s", env_std);
    else         snprintf(stddir, sizeof(stddir), "%s/std", exedir);
    if (env_core) snprintf(coredir, sizeof(coredir), "%s", env_core);
    else          snprintf(coredir, sizeof(coredir), "%s/core", exedir);

    /* Target identity for @compile(target_os/target_arch) filtering in the loader */
    const char *target_os = (arch == ARCH_X86_64_WIN) ? "windows" : "linux";
    const char *target_arch = (arch == ARCH_ARM64_LINUX) ? "aarch64" : "x86_64";

    /* 1+2. load the entry file + resolve all imports, then parse into one merged AST */
    diag_set_primary(input);   /* warnings are only emitted for the entry file's own code */
    int had_error = 0;
    Node *program = module_load(input, stddir, coredir, nostd, target_os, target_arch, &had_error);
    if (had_error) { fprintf(stderr, "compilation failed (parse/import errors)\n"); return 1; }

    /* enums + match desugar into structs and ifs before anything else runs */
    if (desugar_enums(program) > 0) {
        fprintf(stderr, "compilation failed (enum/match errors)\n");
        return 1;
    }

    /* --test-main: a test file has test_* functions instead of a main; generate one */
    if (test_main && synthesize_test_main(program) < 0) {
        fprintf(stderr, "error: no test functions found (define `func test_xxx() -> void`, or write a main)\n");
        return 1;
    }

    /* monomorphization: turn generic functions into instances for the types actually called,
     * followed by overload resolution (including generic instances that may call overloads) */
    if (check_duplicates(program) > 0) {   /* duplicate names (struct/trait/func) -> error right away */
        fprintf(stderr, "compilation failed (duplicate definitions)\n");
        return 1;
    }
    fill_default_args(program);        /* append declared default values to calls that omit them */
    if (monomorphize(program) > 0) {   /* checks trait bounds during instantiation */
        fprintf(stderr, "compilation failed (trait bound errors)\n");
        return 1;
    }
    resolve_overloads(program);

    /* compile-time type check (catches type mismatches such as 50 + "50") before codegen */
    if (typecheck(program) > 0) {
        fprintf(stderr, "compilation failed (type errors)\n");
        return 1;
    }

    /* 3. codegen -> .asm */
    if (codegen_generate(program, asm_path, arch) != 0) {
        fprintf(stderr, "compilation failed (codegen errors)\n");
        return 1;
    }
    printf("[mvs] generated %s\n", asm_path);
    if (optimize) {
        if (arch == ARCH_ARM64_LINUX) peephole_a64(asm_path);
        else                          peephole_x86(asm_path);
    }
    if (only_asm) return 0; /* -S: stop at the assembly file */

    /* 4. assemble into an object file (nasm for x86-64; a GNU-as-compatible driver for arm64) */
    char cmd[PATHBUF * 3], ver[256];
    if (arch == ARCH_ARM64_LINUX) {
        const char *as_tool = NULL;
        if (tool_version("aarch64-linux-gnu-gcc", ver, sizeof(ver))) as_tool = "aarch64-linux-gnu-gcc -c";
        else if (tool_version("clang", ver, sizeof(ver))) as_tool = "clang --target=aarch64-linux-gnu -c";
        else {
            fprintf(stderr, "error: no AArch64 assembler found.\n"
                            "       Install aarch64-linux-gnu-gcc (Linux) or clang (any platform).\n");
            return 1;
        }
        printf("[mvs] using %s\n", ver);
        snprintf(cmd, sizeof(cmd), "%s%s \"%s\" -o \"%s\"",
                 as_tool, mvs_debug_lines ? " -g" : "", asm_path, obj_path);
        printf("[mvs] %s\n", cmd);
        if (system(cmd) != 0) { fprintf(stderr, "error: assembler failed\n"); return 1; }
    } else {
        if (!tool_version("nasm", ver, sizeof(ver))) {
            fprintf(stderr, "error: 'nasm' not found. MVS needs the NASM assembler.\n"
                            "       Install it from https://www.nasm.us and make sure it is on your PATH.\n");
            return 1;
        }
        printf("[mvs] using %s\n", ver);
        /* -g -F dwarf: nasm builds a DWARF line table from the %line directives,
         * so a debugger shows .mvs source instead of the generated assembly */
        snprintf(cmd, sizeof(cmd), "nasm -f %s%s \"%s\" -o \"%s\"",
                 arch == ARCH_X86_64_SYSV ? "elf64" : "win64",
                 mvs_debug_lines ? (arch == ARCH_X86_64_SYSV ? " -g -F dwarf" : " -g -F cv8") : "",
                 asm_path, obj_path);
        printf("[mvs] %s\n", cmd);
        if (system(cmd) != 0) { fprintf(stderr, "error: nasm failed\n"); return 1; }
    }

    /* -c / --nostd / elf64 mode: stop at the object file */
    if (emit_obj || nostd) {
        printf("[mvs] produced object file %s%s\n", obj_path,
               nostd ? " (freestanding, no std/CRT)" :
               arch == ARCH_X86_64_SYSV ? " (ELF64; link on Linux, e.g. gcc file.o -o file)" :
               arch == ARCH_ARM64_LINUX ? " (AArch64; link with aarch64-linux-gnu-gcc, run with qemu-aarch64)" : "");
        if (!keep) remove(asm_path);
        return 0;
    }

    /* 5. link with clang (acting as the linker driver, bringing in the C runtime)
     *    - legacy_stdio_definitions: provides real printf/scanf/... symbols (UCRT inlines them)
     *    - ws2_32: Winsock for the net module (always linked; harmless for programs not using it) */
    if (!tool_version("clang", ver, sizeof(ver))) {
        fprintf(stderr, "error: 'clang' not found. MVS uses clang as the linker.\n"
                        "       Install LLVM/clang (https://llvm.org) and make sure it is on your PATH.\n");
        return 1;
    }
    printf("[mvs] using %s\n", ver);
    snprintf(cmd, sizeof(cmd), "clang \"%s\" -o \"%s\" -llegacy_stdio_definitions -lws2_32", obj_path, exe_path);
    printf("[mvs] %s\n", cmd);
    if (system(cmd) != 0) { fprintf(stderr, "error: link failed\n"); return 1; }

    printf("[mvs] built %s\n", exe_path);

    /* delete intermediate files unless asked to keep them */
    if (!keep) { remove(asm_path); remove(obj_path); }
    return 0;
}
