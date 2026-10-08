#ifndef ISOMATA_UNITY_H
#define ISOMATA_UNITY_H

/*
 * Minimal, API-compatible Unity subset (ThrowTheSwitch Unity, MIT).
 *
 * Chosen over vendoring the full framework deliberately: it keeps the CTOL
 * stack dependency-free (TESTING.md), compiles warning-clean under the
 * project's warning_level=2 + werror C11 build, and covers the API later
 * plain Unity-style test files (test_events.c, test_scene.c, ...) plug into.
 * Swap-in of full Unity later only means replacing these two files — every
 * consumer uses only the macros declared below.
 *
 * The complete macro/function surface (nothing outside this list exists):
 *   setUp / tearDown            per-test fixtures (weak no-op defaults)
 *   UNITY_BEGIN() / UNITY_END()
 *   RUN_TEST(fn)
 *   TEST_FAIL() / TEST_FAIL_MESSAGE(msg)
 *   TEST_ASSERT_TRUE / _FALSE   (+ _MESSAGE)
 *   TEST_ASSERT_NULL / _NOT_NULL (+ _MESSAGE)
 *   TEST_ASSERT_EQUAL_INT / _INT8 / _INT16 / _INT32 / _INT64 (+ _MESSAGE)
 *   TEST_ASSERT_EQUAL_UINT / _UINT8 / _UINT16 / _UINT32 / _UINT64 (+ _MESSAGE)
 *   TEST_ASSERT_EQUAL_PTR       (+ _MESSAGE)
 *   TEST_ASSERT_EQUAL_STRING / _STRING_LEN (+ _MESSAGE)
 *   TEST_ASSERT_EQUAL_MEMORY    (+ _MESSAGE)
 *   TEST_ASSERT_FLOAT_WITHIN / _DOUBLE_WITHIN (+ _MESSAGE)
 *
 * Divergences from full Unity, on purpose:
 *   - Tests are `static void test_<name>(void)`; a failed assertion aborts the
 *     test via longjmp (one FAIL record per test, like full Unity).
 *   - Output is immediate (unbuffered format flush is left to exit), as
 *     <file>:<line>:<test>:FAIL.
 *   - setUp/tearDown default to weak empty functions; define your own in the
 *     suite to override.
 *   - TEST_IGNORE and the pointer/support/protection helpers are not
 *     implemented.
 */

#include <stddef.h>
#include <string.h>

/* Runner heartbeat (unity.c). Assertion macros call these on failure. */
int UnityBegin(void);
int UnityEnd(void);
void UnityDefaultTestRun(void (*test)(void), const char *name, long line);
void UnityFail(const char *file, long line, const char *fmt, ...);
void UnityFailMsg(const char *file, long line, const char *msg, const char *fmt, ...);

/* Per-test fixtures. Override in the suite's test files; the defaults in
 * unity.c are weak no-ops. */
void setUp(void);
void tearDown(void);

#define UNITY_BEGIN() UnityBegin()
#define UNITY_END() UnityEnd()

#define RUN_TEST(test) UnityDefaultTestRun((test), #test, (long)__LINE__)

/* Failure entry points. */
#define TEST_FAIL_MESSAGE(msg) \
	UnityFailMsg(__FILE__, __LINE__, (msg), NULL)
#define TEST_FAIL() TEST_FAIL_MESSAGE("Failed")

#define TEST_ASSERT_TRUE_MESSAGE(cond, msg) \
	do { \
		if (!(cond)) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected TRUE Was FALSE (%s)", #cond); \
		} \
	} while (0)

#define TEST_ASSERT_TRUE(cond) \
	TEST_ASSERT_TRUE_MESSAGE((cond), NULL)

#define TEST_ASSERT_FALSE_MESSAGE(cond, msg) \
	do { \
		if ((cond)) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected FALSE Was TRUE (%s)", #cond); \
		} \
	} while (0)

#define TEST_ASSERT_FALSE(cond) \
	TEST_ASSERT_FALSE_MESSAGE((cond), NULL)

#define TEST_ASSERT_NULL(p) \
	TEST_ASSERT_TRUE_MESSAGE((p) == NULL, #p " was not NULL")

#define TEST_ASSERT_NULL_MESSAGE(p, msg) \
	TEST_ASSERT_TRUE_MESSAGE((p) == NULL, (msg))

#define TEST_ASSERT_NOT_NULL(p) \
	TEST_ASSERT_TRUE_MESSAGE((p) != NULL, #p " was NULL")

#define TEST_ASSERT_NOT_NULL_MESSAGE(p, msg) \
	TEST_ASSERT_TRUE_MESSAGE((p) != NULL, (msg))

/* Signed integers: both sides widen to long long, report in that width. */
#define UNITY_ASSERT_EQUAL_INT(expected, actual, msg) \
	do { \
		long long _unityE = (long long)(expected); \
		long long _unityA = (long long)(actual); \
		if (_unityE != _unityA) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected %lld Was %lld", _unityE, _unityA); \
		} \
	} while (0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_INT_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, (msg))

#define TEST_ASSERT_EQUAL_INT8(expected, actual) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_INT16(expected, actual) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_INT32(expected, actual) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_INT64(expected, actual) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_INT8_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, (msg))
#define TEST_ASSERT_EQUAL_INT16_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, (msg))
#define TEST_ASSERT_EQUAL_INT32_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, (msg))
#define TEST_ASSERT_EQUAL_INT64_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_INT(expected, actual, (msg))

/* Unsigned integers: both sides widen to unsigned long long. */
#define UNITY_ASSERT_EQUAL_UINT(expected, actual, msg) \
	do { \
		unsigned long long _unityE = (unsigned long long)(expected); \
		unsigned long long _unityA = (unsigned long long)(actual); \
		if (_unityE != _unityA) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected %llu Was %llu", _unityE, _unityA); \
		} \
	} while (0)

#define TEST_ASSERT_EQUAL_UINT(expected, actual) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, (msg))

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_UINT16(expected, actual) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_UINT32(expected, actual) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_UINT64(expected, actual) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, (msg))
#define TEST_ASSERT_EQUAL_UINT16_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, (msg))
#define TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, (msg))
#define TEST_ASSERT_EQUAL_UINT64_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_UINT(expected, actual, (msg))

#define UNITY_ASSERT_EQUAL_PTR(expected, actual, msg) \
	do { \
		const void *_unityE = (const void *)(expected); \
		const void *_unityA = (const void *)(actual); \
		if (_unityE != _unityA) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected %p Was %p", _unityE, _unityA); \
		} \
	} while (0)

#define TEST_ASSERT_EQUAL_PTR(expected, actual) \
	UNITY_ASSERT_EQUAL_PTR(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_PTR_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_PTR(expected, actual, (msg))

/* Strings: either side may be NULL; NULL never equals a string. */
#define UNITY_ASSERT_EQUAL_STRING(expected, actual, msg) \
	do { \
		const char *_unityE = (const char *)(expected); \
		const char *_unityA = (const char *)(actual); \
		if (_unityE == NULL || _unityA == NULL \
			|| strcmp(_unityE, _unityA) != 0) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected '%s' Was '%s'", \
				_unityE ? _unityE : "(null)", \
				_unityA ? _unityA : "(null)"); \
		} \
	} while (0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
	UNITY_ASSERT_EQUAL_STRING(expected, actual, NULL)
#define TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, actual, msg) \
	UNITY_ASSERT_EQUAL_STRING(expected, actual, (msg))

#define UNITY_ASSERT_EQUAL_STRING_LEN(expected, actual, len, msg) \
	do { \
		const char *_unityE = (const char *)(expected); \
		const char *_unityA = (const char *)(actual); \
		int _unityN = (int)(len); \
		int _unityP = _unityN < 0 ? 0 : _unityN; \
		if (_unityE == NULL || _unityA == NULL || \
		    strncmp(_unityE, _unityA, (size_t)_unityP) != 0) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected '%.*s' Was '%.*s'", \
				_unityP, _unityE ? _unityE : "(null)", \
				_unityP, _unityA ? _unityA : "(null)"); \
		} \
	} while (0)

#define TEST_ASSERT_EQUAL_STRING_LEN(expected, actual, len) \
	UNITY_ASSERT_EQUAL_STRING_LEN(expected, actual, (len), NULL)
#define TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(expected, actual, len, msg) \
	UNITY_ASSERT_EQUAL_STRING_LEN(expected, actual, (len), (msg))

#define UNITY_ASSERT_EQUAL_MEMORY(expected, actual, len, msg) \
	do { \
		if (memcmp((expected), (actual), (size_t)(len)) != 0) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Memory differs over %zu bytes", (size_t)(len)); \
		} \
	} while (0)

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len) \
	UNITY_ASSERT_EQUAL_MEMORY(expected, actual, (len), NULL)
#define TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, actual, len, msg) \
	UNITY_ASSERT_EQUAL_MEMORY(expected, actual, (len), (msg))

/* Floating point: absolute-difference comparison in double precision. */
#define UNITY_ASSERT_WITHIN(delta, expected, actual, msg) \
	do { \
		double _unityD = (double)(delta); \
		double _unityE = (double)(expected); \
		double _unityA = (double)(actual); \
		double _unityDiff = _unityA - _unityE; \
		if (_unityDiff < 0.0) { \
			_unityDiff = -_unityDiff; \
		} \
		if (!(_unityDiff <= _unityD)) { \
			UnityFailMsg(__FILE__, __LINE__, (msg), \
				"Expected %g Was %g (|diff %g| > %g)", \
				_unityE, _unityA, _unityDiff, _unityD); \
		} \
	} while (0)

#define TEST_ASSERT_FLOAT_WITHIN(delta, expected, actual) \
	UNITY_ASSERT_WITHIN(delta, expected, actual, NULL)
#define TEST_ASSERT_FLOAT_WITHIN_MESSAGE(delta, expected, actual, msg) \
	UNITY_ASSERT_WITHIN(delta, expected, actual, (msg))
#define TEST_ASSERT_DOUBLE_WITHIN(delta, expected, actual) \
	UNITY_ASSERT_WITHIN(delta, expected, actual, NULL)
#define TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(delta, expected, actual, msg) \
	UNITY_ASSERT_WITHIN(delta, expected, actual, (msg))

#endif /* ISOMATA_UNITY_H */
