/*
 * Minimal Unity subset implementation (see unity.h for the API contract and
 * the reasoning behind not vendoring the full framework).
 *
 * One FAIL record per test: a failed assertion aborts the running test via
 * longjmp into the frame set here, so setUp/tearDown state stays usable for
 * the next test and fault-injection loops can keep running afterwards.
 */

#include "unity.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* PascalCase type, g_ global per repo conventions. */
typedef struct UnityState {
	const char *currentTest;
	unsigned testsRun;
	unsigned testFailures;
	int frameLive;
	jmp_buf abortFrame;
} UnityState;

static UnityState g_Unity;

/* Weak no-op fixture defaults; a suite overrides them by defining its own
 * strong setUp()/tearDown(). */
#ifdef __GNUC__
__attribute__((weak)) void setUp(void)
{
}

__attribute__((weak)) void tearDown(void)
{
}
#endif

static void UnityVFail(const char *file, long line, const char *msg,
	const char *fmt, va_list args)
{
	printf("%s:%ld:%s:FAIL", file ? file : "?", line,
		g_Unity.currentTest ? g_Unity.currentTest : "?");
	if (fmt != NULL) {
		fputs(": ", stdout);
		vprintf(fmt, args);
	}
	if (msg != NULL && msg[0] != '\0') {
		printf(": %s", msg);
	}
	putchar('\n');
	g_Unity.testFailures++;
	if (g_Unity.frameLive != 0) {
		longjmp(g_Unity.abortFrame, 1);
	}
}

void UnityFail(const char *file, long line, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	UnityVFail(file, line, NULL, fmt, args);
	va_end(args);
}

void UnityFailMsg(const char *file, long line, const char *msg,
	const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	UnityVFail(file, line, msg, fmt, args);
	va_end(args);
}

int UnityBegin(void)
{
	memset(&g_Unity, 0, sizeof(g_Unity));
	return 0;
}

void UnityDefaultTestRun(void (*test)(void), const char *name, long line)
{
	(void)line;
	if (setjmp(g_Unity.abortFrame) == 0) {
		g_Unity.currentTest = name;
		g_Unity.frameLive = 1;
		setUp();
		test();
	}
	/* On abort (failed assertion) setUp's test body is cut short here;
	 * tearDown still runs so per-test cleanup never leaks across runs. */
	g_Unity.frameLive = 0;
	tearDown();
	g_Unity.testsRun++;
}

int UnityEnd(void)
{
	printf("\n%u Tests %u Failures 0 Ignored\n",
		g_Unity.testsRun, g_Unity.testFailures);
	if (g_Unity.testFailures == 0) {
		printf("OK\n");
	} else {
		printf("FAIL\n");
	}
	/* meson's exitcode protocol reads the low 8 bits; a raw failure count
	 * >= 256 would wrap to 0 and pass. Report presence, not magnitude. */
	return (g_Unity.testFailures == 0) ? 0 : 1;
}
