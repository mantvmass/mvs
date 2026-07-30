/* Differential test reference for structs.mvs */
#include <stdio.h>

typedef struct { double x, y, z; } Vec3;
typedef struct { long long key, weight; } Item;
typedef struct { Item head; Vec3 tail; long long count; } Nested;

static double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static Vec3 scale(Vec3 v, double k) {
    Vec3 r; r.x = v.x * k; r.y = v.y * k; r.z = v.z * k; return r;
}

static long long fib(long long n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static long long ack(long long m, long long n) {
    if (m == 0) return n + 1;
    if (n == 0) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

static long long add(long long a, long long b) { return a + b; }
static long long mul(long long a, long long b) { return a * b; }

static long long apply(long long (*f)(long long, long long), long long a, long long b) {
    return f(a, b);
}

int main(void) {
    Vec3 a = { 1.5, 2.5, 3.5 };
    Vec3 b = { 0.5, 4.0, 2.0 };
    printf("dot %f\n", dot(a, b));
    Vec3 s = scale(a, 2.0);
    printf("scale %f %f %f\n", s.x, s.y, s.z);

    Nested n;
    n.head.key = 7; n.head.weight = 3; n.tail = b; n.count = 2;
    printf("nested %lld %lld %f %lld\n", n.head.key, n.head.weight, n.tail.y, n.count);

    long long arr[10] = { 42, 7, 19, 3, 88, 1, 55, 23, 9, 70 };
    for (long long i = 1; i < 10; i++) {
        long long key = arr[i];
        long long j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j = j - 1;
        }
        arr[j + 1] = key;
    }
    printf("sorted [%lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld]\n",
           arr[0], arr[1], arr[2], arr[3], arr[4], arr[5], arr[6], arr[7], arr[8], arr[9]);

    long long targets[4] = { 19, 88, 4, 1 };
    for (int t = 0; t < 4; t++) {
        long long lo = 0, hi = 9, found = -1;
        while (lo <= hi) {
            long long mid = (lo + hi) / 2;
            if (arr[mid] == targets[t]) { found = mid; lo = hi + 1; }
            else if (arr[mid] < targets[t]) { lo = mid + 1; }
            else { hi = mid - 1; }
        }
        printf("search %lld -> %lld\n", targets[t], found);
    }

    printf("fib %lld %lld %lld\n", fib(10), fib(20), fib(25));
    printf("ack %lld %lld %lld\n", ack(1, 3), ack(2, 3), ack(3, 3));

    long long (*fp)(long long, long long) = add;
    printf("fp %lld %lld\n", apply(fp, 3, 4), apply(mul, 3, 4));

    printf("[%8.3f] [%08lld] [%6s] [%04llx]\n", 2.718281828, 1234LL, "hi", 255ULL);
    return 0;
}
