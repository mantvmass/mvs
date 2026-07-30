/* mathops.c: C functions the MVS side will call (see use_c.mvs)
 * compiled in at link time: clang use_c.obj mathops.c -o use_c.exe ... */
#include <math.h>

int    c_add(int a, int b)         { return a + b; }
int    c_gcd(int a, int b)         { while (b) { int t = a % b; a = b; b = t; } return a; }
double c_hypot(double x, double y)  { return sqrt(x * x + y * y); }
