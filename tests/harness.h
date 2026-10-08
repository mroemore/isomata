#ifndef ISOMATA_HARNESS_H
#define ISOMATA_HARNESS_H

/*
 * CTOL micro-harness (option A): dependency-free assertions for C tests.
 *
 * Shape, and why it is this shape:
 *   - A failed assertion RETURNS from the current test function with non-zero;
 *     it never aborts the process. That is what makes the fault-injection
 *     "loop-until-clean" pattern possible: a test can assert, fail, unwind to
 *     its caller, and let the very next iteration run. An aborting/longjmp
 *     harness cannot do that.
 *   - Every test is `static int test_<name>(void)` returning 0 on success.
 *     `main` runs them through RUN() and accumulates failures.
 *   - Diagnostics go to stderr as `FAIL file:line: <expr>`.
 *
 * Add new ASSERT_* helpers here, once, for the whole suite. Do not hand-roll
 * assertion macros in individual test files.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#define ISOMATA_FAIL(...) do { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return 1; \
    } while (0)

#define ASSERT_TRUE(cond) do { \
        if (!(cond)) { \
            ISOMATA_FAIL("%s", #cond); \
        } \
    } while (0)

#define ASSERT_FALSE(cond) do { \
        if ((cond)) { \
            ISOMATA_FAIL("!(%s)", #cond); \
        } \
    } while (0)

#define ASSERT_NULL(p)     ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define ASSERT_EQ_INT(a, b) do { \
        long long _a = (long long)(a); \
        long long _b = (long long)(b); \
        if (_a != _b) { \
            ISOMATA_FAIL("%s == %s (%lld != %lld)", #a, #b, _a, _b); \
        } \
    } while (0)

#define ASSERT_EQ_UINT(a, b) do { \
        unsigned long long _a = (unsigned long long)(a); \
        unsigned long long _b = (unsigned long long)(b); \
        if (_a != _b) { \
            ISOMATA_FAIL("%s == %s (%llu != %llu)", #a, #b, _a, _b); \
        } \
    } while (0)

#define ASSERT_STR_EQ(a, b) do { \
        const char *_a = (a); \
        const char *_b = (b); \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
            ISOMATA_FAIL("%s == %s (\"%s\" != \"%s\")", #a, #b, \
                          _a ? _a : "(null)", _b ? _b : "(null)"); \
        } \
    } while (0)

#define ASSERT_MEM_EQ(a, b, n) do { \
        if (memcmp((a), (b), (size_t)(n)) != 0) { \
            ISOMATA_FAIL("%s == %s for %zu bytes", #a, #b, (size_t)(n)); \
        } \
    } while (0)

#define ASSERT_NEAR(a, b, eps) do { \
        double _d = (double)(a) - (double)(b); \
        double _e = (double)(eps); \
        if (_d < 0) { \
            _d = -_d; \
        } \
        if (!(_d <= _e)) { \
            ISOMATA_FAIL("|%s - %s| <= %s (%g > %g)", #a, #b, #eps, _d, _e); \
        } \
    } while (0)

/* Run one test function and accumulate its result. Prints PASS on success. */
#define RUN(fn) do { \
        if ((fn)() != 0) { \
            failed++; \
        } else { \
            printf("PASS %s\n", #fn); \
        } \
    } while (0)

#define HARNESS_SUMMARY(label) do { \
        printf("\n%s: %s (%d test%s failed)\n", \
               (failed) ? "FAILED" : "PASSED", (label), (failed), \
               (failed) == 1 ? "" : "s"); \
        return (failed) ? 1 : 0; \
    } while (0)

/* Opt-in leak assertion: compile the test with -DISOMATA_HARNESS_FAULTINJECT
 * and link tests/support/faultinject.c, then ASSERT_NO_LEAKS() checks that
 * every allocation the code under test made was also freed. */
#if defined(ISOMATA_HARNESS_FAULTINJECT)
#include "faultinject.h"
#define ASSERT_NO_LEAKS() ASSERT_EQ_INT(fi_live(), 0)
#define ASSERT_ALLOC_FAILED(n) do { \
        fi_reset(); \
        fi_fail_after(n); \
    } while (0)
#endif

#endif /* ISOMATA_HARNESS_H */
