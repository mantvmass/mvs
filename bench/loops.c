/* bench/loops.c - the C reference for bench/loops.mvs, same program */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int64_t sum_to(int64_t n) {
    int64_t acc = 0, i = 0;
    while (i < n) { acc = acc + i; i = i + 1; }
    return acc;
}

static int64_t triangle(int64_t n) {
    int64_t total = 0, i = 0;
    while (i < n) {
        int64_t j = 0;
        while (j < i) { total = total + (i * j) % 7; j = j + 1; }
        i = i + 1;
    }
    return total;
}

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static int64_t sieve(int64_t limit) {
    unsigned char *flags = malloc((size_t)limit + 1);
    int64_t i = 0;
    while (i <= limit) { flags[i] = 1; i = i + 1; }
    int64_t count = 0, p = 2;
    while (p <= limit) {
        if (flags[p] == 1) {
            count = count + 1;
            int64_t q = p * p;
            while (q <= limit) { flags[q] = 0; q = q + p; }
        }
        p = p + 1;
    }
    free(flags);
    return count;
}

int main(void) {
    printf("%lld\n", (long long)sum_to(200000000));
    printf("%lld\n", (long long)triangle(20000));
    printf("%lld\n", (long long)fib(32));
    printf("%lld\n", (long long)sieve(20000000));
    return 0;
}
