/* caller.c: a C program calling functions exported from MVS (see export_lib.mvs)
 * build:  mvs.exe examples/07_c_interop/export_lib.mvs -c
 *         clang caller.c export_lib.obj -o app.exe -llegacy_stdio_definitions
 *         app.exe */
#include <stdio.h>

/* prototypes matching the names/types exported from MVS */
int       mvs_square(int x);
long long mvs_sum_to(long long n);

int main(void) {
    printf("mvs_square(6)  = %d\n", mvs_square(6));        /* 36 */
    printf("mvs_sum_to(100) = %lld\n", mvs_sum_to(100));   /* 5050 */
    return 0;
}
