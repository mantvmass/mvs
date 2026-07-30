/*
 * diag.c - Rust-style diagnostic output (see diag.h for the format)
 *
 * Sources are kept in memory for the whole compilation so that any pass can
 * quote the offending line. The table is small (one entry per imported file)
 * and freed by process exit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "diag.h"

#define MAX_SOURCES 256

typedef struct { char *name; char *src; } SrcEntry;
static SrcEntry sources[MAX_SOURCES];
static int nsources = 0;
static const char *primary = NULL;

const char *diag_register_source(const char *name, char *src) {
    for (int i = 0; i < nsources; i++)
        if (strcmp(sources[i].name, name) == 0) { free(src); return sources[i].name; }
    if (nsources >= MAX_SOURCES) { free(src); return strdup(name); }
    sources[nsources].name = strdup(name);
    sources[nsources].src = src;
    return sources[nsources++].name;
}

void diag_set_primary(const char *name) { primary = name; }

int diag_is_primary(const char *name) {
    return primary && name && strcmp(primary, name) == 0;
}

/* Find the start of the given 1-based line in a registered file, NULL if unknown */
static const char *find_line(const char *file, int line) {
    if (!file || line <= 0) return NULL;
    const char *src = NULL;
    for (int i = 0; i < nsources; i++)
        if (strcmp(sources[i].name, file) == 0) { src = sources[i].src; break; }
    if (!src) return NULL;
    const char *p = src;
    for (int l = 1; l < line; l++) {
        p = strchr(p, '\n');
        if (!p) return NULL;
        p++;
    }
    return p;
}

/* Quote the source line and draw a caret under col (0 = no caret) */
static void show_line(const char *file, int line, int col) {
    const char *p = find_line(file, line);
    if (!p) return;
    const char *end = strchr(p, '\n');
    int len = end ? (int)(end - p) : (int)strlen(p);
    while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    fprintf(stderr, "%5d | %.*s\n", line, len, p);
    if (col > 0 && col <= len + 1) {
        fprintf(stderr, "      | ");
        /* keep tabs aligned: copy the line's leading whitespace shape up to the caret */
        for (int i = 0; i < col - 1 && i < len; i++) fputc(p[i] == '\t' ? '\t' : ' ', stderr);
        fprintf(stderr, "^\n");
    }
}

void diag_print(const char *file, int line, int col, const char *kind, const char *fmt, ...) {
    va_list ap;
    if (file && line > 0) {
        if (col > 0) fprintf(stderr, "%s:%d:%d: %s: ", file, line, col, kind);
        else         fprintf(stderr, "%s:%d: %s: ", file, line, kind);
    } else {
        fprintf(stderr, "%s: ", kind);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    show_line(file, line, col);
}

void diag_help(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "help: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
