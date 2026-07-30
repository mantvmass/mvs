/* Differential test reference for strings.mvs */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned long long fnv1a(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned long long h = 0xCBF29CE484222325ull;
    size_t i = 0;
    while (p[i] != 0) {
        h = (h ^ p[i]) * 0x100000001B3ull;
        i++;
    }
    return h;
}

static void reverse_into(char *dst, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) dst[i] = s[n - 1 - i];
    dst[n] = 0;
}

static long long cmp3(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != 0 && a[i] == b[i]) i++;
    if ((unsigned char)a[i] < (unsigned char)b[i]) return -1;
    if ((unsigned char)a[i] > (unsigned char)b[i]) return 1;
    return 0;
}

static long long findc(const char *s, char c) {
    for (long long i = 0; s[i] != 0; i++) if (s[i] == c) return i;
    return -1;
}

static int starts_with(const char *s, const char *p) {
    for (size_t i = 0; p[i] != 0; i++) if (s[i] != p[i]) return 0;
    return 1;
}

static long long popcount64(unsigned long long x) {
    long long n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}
static long long clz64(unsigned long long x) {
    if (x == 0) return 64;
    long long n = 0;
    while ((x & 0x8000000000000000ull) == 0) { x <<= 1; n++; }
    return n;
}
static long long ctz64(unsigned long long x) {
    if (x == 0) return 64;
    long long n = 0;
    while ((x & 1) == 0) { x >>= 1; n++; }
    return n;
}
static unsigned long long bswap64(unsigned long long x) {
    unsigned long long r = 0;
    for (int i = 0; i < 8; i++) { r = (r << 8) | (x & 0xFF); x >>= 8; }
    return r;
}
static unsigned long long rotl64(unsigned long long x, unsigned long long n) {
    n &= 63;
    return n == 0 ? x : (x << n) | (x >> (64 - n));
}
static unsigned long long rotr64(unsigned long long x, unsigned long long n) {
    n &= 63;
    return n == 0 ? x : (x >> n) | (x << (64 - n));
}

int main(void) {
    const char *words[5] = { "compiler", "mvs", "", "a", "differential" };
    for (int i = 0; i < 5; i++) {
        printf("%s len=%llu hash=%llu\n", words[i],
               (unsigned long long)strlen(words[i]), fnv1a(words[i]));
    }

    printf("cmp %lld %lld %lld\n", cmp3("abc", "abd"), cmp3("b", "a"), cmp3("x", "x"));
    printf("find %lld %lld %lld\n", findc("kernel", 'e'), findc("kernel", 'l'), findc("kernel", 'z'));
    printf("prefix %lld %lld\n", (long long)starts_with("multiboot", "multi"),
           (long long)starts_with("multi", "multiboot"));

    char *buf = malloc(64);
    reverse_into(buf, "differential");
    printf("reversed %s\n", buf);
    char *copy = malloc(64);
    memmove(copy, buf, strlen(buf) + 1);
    printf("copy eq %lld\n", (long long)(memcmp(buf, copy, 13) == 0));
    memset(copy, 65, 4);
    printf("after set %s eq %lld\n", copy, (long long)(memcmp(buf, copy, 13) == 0));

    unsigned long long h = fnv1a("mvs");
    printf("bits %lld %lld %lld %llu\n", popcount64(h), clz64(h), ctz64(h), bswap64(h));
    printf("rot %llu %llu\n", rotl64(h, 13), rotr64(h, 7));
    return 0;
}
