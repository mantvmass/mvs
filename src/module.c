/*
 * module.c - การโหลดและรวมโมดูล (resolve import ข้ามไฟล์)
 *
 * แนวคิด: front-end เดิม parse ได้ทีละไฟล์ ระบบโมดูลทำหน้าที่
 *   1. อ่านไฟล์ entry แล้ว parse
 *   2. พบ ND_IMPORT ก็ไปโหลดไฟล์/แพ็กเกจที่อ้างถึง (กันโหลดซ้ำด้วยชุดที่โหลดแล้ว)
 *   3. นำฟังก์ชัน/ตัวแปร/extern จากทุกโมดูลมารวมไว้ใน ND_PROGRAM เดียว
 *      โดยติดป้าย namespace ให้โมดูลที่นำเข้าแบบ package (เช่น io.out)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "parser.h"

#define MAX_LOADED 256

/* สถานะระหว่างการโหลด รวมผลลัพธ์และรายการไฟล์ที่โหลดแล้ว */
typedef struct {
    Node       *result;              /* ND_PROGRAM ที่กำลังสะสมผลลัพธ์ */
    char       *loaded[MAX_LOADED];  /* path (canonical) ของโมดูลที่โหลดแล้ว (กันโหลดซ้ำ/วนลูป) */
    char       *defs[MAX_LOADED];    /* รายชื่อสัญลักษณ์ระดับบนของแต่ละโมดูล รูป "|name|name|" (ใช้ตรวจ import) */
    char       *loaded_ns[MAX_LOADED];/* namespace ที่โมดูลถูก tag ตอนโหลดครั้งแรก (กัน import คนละ ns) */
    int         nloaded;
    char       *ns_name[MAX_LOADED]; /* namespace ที่ผูกแล้ว (io, net, alias) — กันผูกชื่อซ้ำคนละโมดูล */
    char       *ns_canon[MAX_LOADED];
    int         nns;
    char       *loading[MAX_LOADED]; /* โมดูลที่กำลังโหลดอยู่ในสายเรียกปัจจุบัน — ใช้จับ circular import */
    int         nloading;
    const char *stddir;              /* โฟลเดอร์ standard library */
    int         nostd;               /* 1 = โหมด freestanding (ห้าม import package เช่น std) */
    int         had_error;
} Loader;

/* อ่านไฟล์ทั้งไฟล์เข้าหน่วยความจำ คืนสตริง (ผู้เรียก free เอง) หรือ NULL */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }     /* ไฟล์อ่านขนาดไม่ได้ */
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* มีไฟล์นี้ให้เปิดอ่านได้หรือไม่ */
static int read_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* คัดลอกส่วนโฟลเดอร์ของ path ลง out (รองรับทั้ง / และ \) ถ้าไม่มีคืน "." */
static void path_dirname(const char *path, char *out) {
    strcpy(out, path);
    char *slash = strrchr(out, '/');
    char *bslash = strrchr(out, '\\');
    char *cut = slash > bslash ? slash : bslash;
    if (cut) *cut = '\0';
    else strcpy(out, ".");
}

/* ตรวจว่าเป็น path แบบ "package เปล่า" (ชื่อเดียว ไม่มี / \ หรือ .) เช่น "std"
 * รูปนี้ = namespace import (ชื่อใน {} คือ submodule ที่จะโหลดเป็น namespace) */
static int is_package_path(const char *p) {
    return strchr(p, '/') == NULL && strchr(p, '\\') == NULL && strchr(p, '.') == NULL;
}

/* ตรวจว่าเป็น path สัมบูรณ์ (ขึ้นต้นด้วย / \ หรือ "X:") */
static int is_absolute(const char *p) {
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (p[0] && p[1] == ':') return 1; /* รูปแบบ Windows เช่น C:\ */
    return 0;
}

/* ลงท้ายด้วย ".mvs" หรือไม่ (path ไฟล์ relative ต้องระบุนามสกุล) */
static int ends_with_mvs(const char *p) {
    size_t n = strlen(p);
    return n >= 4 && strcmp(p + n - 4, ".mvs") == 0;
}

/* ทำให้เป็น path สัมบูรณ์ (canonical) เพื่อเทียบความเป็นไฟล์เดียวกัน */
static void canonicalize(const char *path, char *out, size_t n) {
    if (!_fullpath(out, path, (int)n)) snprintf(out, n, "%s", path);
}

/* คืนดัชนีของโมดูลที่โหลดแล้ว (เทียบ canonical path) หรือ -1 */
static int find_loaded(Loader *L, const char *canon) {
    for (int i = 0; i < L->nloaded; i++)
        if (strcmp(L->loaded[i], canon) == 0) return i;
    return -1;
}

/* รวบรวมชื่อสัญลักษณ์ระดับบนของโมดูล (struct/trait/ฟังก์ชันที่ไม่ใช่ method) เป็น "|a|b|c|" */
static char *collect_defs(Node *prog) {
    size_t cap = 256, len = 1;
    char *s = (char *)malloc(cap);
    s[0] = '|'; s[1] = '\0';
    for (int i = 0; i < prog->nitems; i++) {
        Node *d = prog->items[i];
        const char *nm = NULL;
        if ((d->kind == ND_FUNC && !d->is_method) || d->kind == ND_STRUCT_DECL || d->kind == ND_TRAIT)
            nm = d->name;
        if (!nm) continue;
        size_t need = len + strlen(nm) + 2;
        while (need + 1 > cap) { cap *= 2; s = (char *)realloc(s, cap); }
        strcpy(s + len, nm); len += strlen(nm);
        s[len++] = '|'; s[len] = '\0';
    }
    return s;
}

/* โมดูลมีสัญลักษณ์ชื่อนี้หรือไม่ (เทียบกับ "|name|" ในสตริง defs) */
static int defs_has(const char *defs, const char *name) {
    if (!defs) return 0;
    char pat[300];
    snprintf(pat, sizeof(pat), "|%s|", name);
    return strstr(defs, pat) != NULL;
}

static void load_module(Loader *L, const char *path, const char *ns);

/* ผูก namespace ชื่อ name เข้ากับโมดูล canon — ถ้าชื่อนี้ถูกผูกกับโมดูลอื่นแล้ว = error */
static void register_ns(Loader *L, const char *name, const char *canon) {
    for (int i = 0; i < L->nns; i++) {
        if (strcmp(L->ns_name[i], name) != 0) continue;
        if (strcmp(L->ns_canon[i], canon) != 0) {
            fprintf(stderr, "error: namespace '%s' is already bound to a different module\n", name);
            L->had_error = 1;
        }
        return; /* ผูกซ้ำกับโมดูลเดิม = ไม่เป็นไร */
    }
    if (L->nns < MAX_LOADED) {
        L->ns_name[L->nns] = strdup(name);
        L->ns_canon[L->nns] = strdup(canon);
        L->nns++;
    }
}

/* resolve "module path" (ไม่ใช่ bare package) ให้เป็นไฟล์จริง คืน 1 ถ้าสำเร็จ
 *   absolute            -> ตามเดิม
 *   ลงท้าย .mvs         -> ไฟล์ relative เทียบ base_dir (เช่น ./lib.mvs)
 *   อื่น ๆ (มี '/')      -> package submodule เช่น "std/string" -> <stddir>/string.mvs */
static int resolve_module_file(Loader *L, const char *path, const char *base_dir, char *out, size_t n) {
    if (is_absolute(path)) { snprintf(out, n, "%s", path); return 1; }
    if (ends_with_mvs(path)) { snprintf(out, n, "%s/%s", base_dir, path); return 1; }
    if (L->nostd) {
        fprintf(stderr, "error: cannot import package module '%s' in --nostd (freestanding) mode\n", path);
        L->had_error = 1; return 0;
    }
    const char *slash = strchr(path, '/');
    const char *sub = slash ? slash + 1 : path;       /* ตัด "std/" ออก เหลือ "string" */
    snprintf(out, n, "%s/%s.mvs", L->stddir, sub);
    return 1;
}

/* จัดการคำสั่ง import หนึ่งคำสั่ง โดย base_dir คือโฟลเดอร์ของไฟล์ที่มี import นี้
 * รองรับ 3 รูปแบบ (ดู GUIDE):
 *   A) import { io, net } from "std"            namespace ของ submodule (io.out, net.X)
 *   B) import { String } from "std/string"      ดึงสัญลักษณ์ตรง ๆ + ตรวจว่ามีจริง
 *      import { factorial } from "./lib.mvs"
 *   C) import lib from "./lib.mvs"               ทั้งโมดูลเป็น namespace lib (lib.factorial)
 *      import str from "std/string" */
static void handle_import(Loader *L, Node *imp, const char *base_dir) {
    const char *path = imp->str_val;
    if (!path) return;

    /* ── รูปแบบ C: alias import (ไม่มี {}) ── ทั้งโมดูลกลายเป็น namespace = imp->name */
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
        register_ns(L, imp->name, canon);     /* กัน alias ซ้ำชี้คนละโมดูล */
        load_module(L, file, imp->name);
        return;
    }

    if (is_package_path(path)) {
        /* ── รูปแบบ A: namespace import ของ submodule ── */
        if (L->nostd) {
            fprintf(stderr, "error: cannot import package '%s' in --nostd (freestanding) mode\n", path);
            L->had_error = 1;
            return;
        }
        for (int i = 0; i < imp->nitems; i++) {
            const char *name = imp->items[i]->name;
            char file[1024];
            snprintf(file, sizeof(file), "%s/%s.mvs", L->stddir, name);
            if (!read_file_exists(file)) {
                fprintf(stderr, "error: package '%s' has no module '%s' (looked for %s)\n", path, name, file);
                L->had_error = 1; continue;
            }
            char canon[1024]; canonicalize(file, canon, sizeof(canon));
            register_ns(L, name, canon);      /* io/net เป็น namespace — กันผูกซ้ำคนละโมดูล */
            load_module(L, file, name);
        }
    } else {
        /* ── รูปแบบ B: symbol import ── ดึงสัญลักษณ์ตรง ๆ (namespace = "") + ตรวจว่ามีจริง */
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

/* โหลดหนึ่งโมดูลจาก path แล้วผนวกนิยามทั้งหมดเข้าผลลัพธ์ (ติดป้าย namespace ถ้ามี) */
static void load_module(Loader *L, const char *path, const char *ns) {
    /* ทำให้เป็น path สัมบูรณ์ (canonical) ก่อน เพื่อให้ "./lib.mvs" กับ "lib.mvs"
     * ถือเป็นไฟล์เดียวกัน — กันโหลดซ้ำและกันวนลูปจากการสะกด path ต่างกัน */
    char canon[1024];
    canonicalize(path, canon, sizeof(canon));

    int prev = find_loaded(L, canon);
    if (prev >= 0) {                            /* โหลดเสร็จแล้ว — กันโหลดซ้ำ (รองรับ diamond import) */
        /* ถ้าถูกผูกเป็น "namespace สองชื่อต่างกัน" (alias ชนกัน) สัญลักษณ์จะอยู่คนละชื่อ → error ชัดเจน
         * (กรณี ns ว่าง = symbol/direct import ไม่ถือว่าชน เพราะ struct/trait เป็น global อยู่แล้ว) */
        const char *prev_ns = L->loaded_ns[prev] ? L->loaded_ns[prev] : "";
        const char *now_ns = ns ? ns : "";
        if (prev_ns[0] && now_ns[0] && strcmp(prev_ns, now_ns) != 0) {
            fprintf(stderr, "error: module '%s' is already imported as namespace '%s' — cannot also import it as '%s'\n",
                    path, prev_ns, now_ns);
            L->had_error = 1;
        }
        return;
    }
    /* ตรวจ circular import: โมดูลนี้กำลังถูกโหลดอยู่แล้วในสายเรียกปัจจุบัน (A -> B -> A) */
    for (int i = 0; i < L->nloading; i++)
        if (strcmp(L->loading[i], canon) == 0) {
            fprintf(stderr, "error: circular import detected involving module '%s'\n", path);
            L->had_error = 1;
            return;
        }
    if (L->nloading >= MAX_LOADED) { fprintf(stderr, "error: import nesting too deep\n"); L->had_error = 1; return; }
    L->loading[L->nloading++] = strdup(canon);   /* mark: กำลังโหลด */

    char *src = read_file(path);
    if (!src) {
        fprintf(stderr, "error: cannot open module '%s'\n", path);
        L->had_error = 1;
        free(L->loading[--L->nloading]);
        return;
    }

    int err = 0;
    Node *prog = parse_program(src, path, &err);
    free(src);
    if (err) { L->had_error = 1; free(L->loading[--L->nloading]); return; }

    char *mod_defs = collect_defs(prog); /* จดสัญลักษณ์ระดับบนของโมดูลนี้ ไว้ตรวจ import */

    /* โฟลเดอร์ของโมดูลนี้ ใช้ resolve import ภายในโมดูลต่อ */
    char dir[1024];
    path_dirname(path, dir);

    for (int i = 0; i < prog->nitems; i++) {
        Node *it = prog->items[i];
        if (it->kind == ND_IMPORT) {
            handle_import(L, it, dir);            /* import ซ้อน: โหลดต่อแบบ recursive */
        } else {
            if (ns && ns[0] && it->kind == ND_FUNC && !it->is_extern) {
                /* ทุกฟังก์ชันที่มี body ในโมดูลนี้สังกัดโมดูล ns (ใช้ resolve การเรียกภายใน) */
                it->mod = strdup(ns);
                /* ป้ายชื่อสัญลักษณ์ (ns) ติดเฉพาะฟังก์ชันธรรมดา (เรียกแบบ ns.func)
                 * — method ใช้ชื่อ struct เป็น ns อยู่แล้ว, export ใช้ชื่อดิบให้ C */
                if (!it->is_method && !it->is_export) it->ns = strdup(ns);
            }
            node_add_item(L->result, it);
        }
    }

    /* เสร็จสิ้น: pop ออกจากสายโหลด แล้วลงทะเบียนเป็น "โหลดแล้ว" พร้อมรายชื่อสัญลักษณ์ */
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

Node *module_load(const char *entry_path, const char *stddir, int nostd, int *had_error) {
    Loader L;
    memset(&L, 0, sizeof(L));
    L.result = node_new(ND_PROGRAM, 1);
    L.stddir = stddir;
    L.nostd = nostd;

    /* ไฟล์ entry โหลดด้วย namespace ว่าง (ใช้ชื่อฟังก์ชันตรง ๆ และมี main) */
    load_module(&L, entry_path, "");

    *had_error = L.had_error;
    return L.result;
}
