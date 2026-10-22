/* test_main.c - host test runner for the node firmware core. */
#include "test.h"

int t_checks, t_fails;

int main(void)
{
    struct { const char *name; void (*fn)(void); } t[] = { { "node", test_node }, { "diff", test_diff }, { "drv", test_drv } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int c0 = t_checks, f0 = t_fails;
        t[i].fn();
        printf("%-6s %6d checks, %d failed\n", t[i].name, t_checks - c0, t_fails - f0);
    }
    printf("%d checks passed, %d failed\n", t_checks - t_fails, t_fails);
    return t_fails != 0;
}
