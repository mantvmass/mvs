/* caller.c: a C program calling functions exported from MVS (see export_lib.mvs)
 * build:  mvs.exe examples/07_c_interop/export_lib.mvs -c
 *         clang caller.c export_lib.obj -o app.exe -llegacy_stdio_definitions
 *         app.exe */
#include <stdio.h>

/* prototypes matching the names/types exported from MVS */
int       mvs_square(int x);
long long mvs_sum_to(long long n);
float     mvs_scale(float x, float k);
float     mvs_sumf(float a, float b, float c, float d, float e, float f);
float     mvs_scale_twice(float x);

int main(void) {
    printf("mvs_square(6)  = %d\n", mvs_square(6));        /* 36 */
    printf("mvs_sum_to(100) = %lld\n", mvs_sum_to(100));   /* 5050 */
    printf("mvs_scale(1.5, 4) = %.2f\n", mvs_scale(1.5f, 4.0f));                       /* 6.00 */
    printf("mvs_sumf = %.2f\n", mvs_sumf(1.5f, 2.25f, 3.0f, 4.5f, 5.25f, 6.5f));      /* 23.00 */
    printf("mvs_scale_twice(1.25) = %.2f\n", mvs_scale_twice(1.25f));                  /* 5.00 */
    return 0;
}
