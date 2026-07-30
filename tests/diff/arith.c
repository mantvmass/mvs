/* Differential test reference: the same algorithm as arith.mvs, in C.
 * Both are compiled and run; their stdout must match byte for byte. */
#include <stdio.h>
#include <stdint.h>

static long long collatz_len(long long start) {
    long long n = start, steps = 0;
    while (n != 1) {
        if (n % 2 == 0) n = n / 2;
        else n = 3 * n + 1;
        steps = steps + 1;
    }
    return steps;
}

static long long gcd(long long a, long long b) {
    long long x = a, y = b;
    while (y != 0) {
        long long t = y;
        y = x % y;
        x = t;
    }
    return x;
}

int main(void) {
    for (long long i = -20; i <= 20; i += 3) {
        if (i != 0) {
            printf("%lld %lld %lld\n", i, 7 / i, 7 % i);
            printf("%lld %lld %lld\n", i, i / 3, i % 3);
        }
    }

    unsigned char u8v = 200;
    u8v = (unsigned char)(u8v + 100);
    signed char i8v = 100;
    i8v = (signed char)(i8v + 100);
    unsigned short u16v = 65000;
    u16v = (unsigned short)(u16v + 1000);
    int i32v = 2147483000;
    i32v = (int)((unsigned)i32v + 1000u);
    printf("wrap %llu %lld %llu %lld\n", (unsigned long long)u8v, (long long)i8v,
           (unsigned long long)u16v, (long long)i32v);

    long long s = -1024;
    printf("shifts %lld %lld %llu\n", s >> 3, s << 2, ((unsigned long long)s) >> 3);

    unsigned long long m = 0xF0F0F0F0F0F0F0F0ull;
    printf("bits %llu %llu %llu %llu\n", m & 0xFF00FF00FF00FF00ull, m | 0x0F0Full,
           m ^ 0xFFFFull, ~m);

    double f = 3.0, g = 7.0;
    printf("float %f %f %f %f\n", f / g, g / f, f * g, g - f);
    printf("cast %lld %f %f\n", (long long)(g / f), (double)(-7) / 2.0, (double)42);

    __int128 big = ((__int128)1) << 100;
    /* print the three 128-bit values by hand: C has no %lld for __int128 */
    char b1[64], b2[64], b3[64];
    {
        __int128 vals[3];
        char *outs[3] = { b1, b2, b3 };
        vals[0] = big; vals[1] = big / 1000000; vals[2] = big % 1000000;
        for (int k = 0; k < 3; k++) {
            unsigned __int128 v = (unsigned __int128)vals[k];
            char tmp[64];
            int n = 0;
            if (v == 0) tmp[n++] = '0';
            while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
            int w = 0;
            while (n > 0) outs[k][w++] = tmp[--n];
            outs[k][w] = '\0';
        }
    }
    printf("i128 %s %s %s\n", b1, b2, b3);

    printf("collatz %lld %lld %lld\n", collatz_len(27), collatz_len(97), collatz_len(871));
    printf("gcd %lld %lld %lld\n", gcd(1071, 462), gcd(270, 192), gcd(17, 5));
    return 0;
}
