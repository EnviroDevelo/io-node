/* test.h - minimal host test harness for the node firmware. */
#ifndef TEST_H
#define TEST_H
#include <stdio.h>

extern int t_checks, t_fails;
#define CHECK(c) do { t_checks++; if (!(c)) { t_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); t_checks++; \
    if (_a != _b) { t_fails++; printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

void test_node(void);
void test_diff(void);
void test_drv(void);
void test_net(void);

#endif
