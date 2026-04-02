/*
 * Minimal PostgreSQL and libcurl stubs for fuzzing parser.c standalone.
 * Replaces postgres.h, lib/stringinfo.h, and elog/Assert.
 */
#ifndef PG_STUB_H
#define PG_STUB_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

/* PostgreSQL List stub */
typedef struct List List;

/* libcurl stub */
typedef void CURL;

/* StringInfoData: simplified version of lib/stringinfo.h */
typedef struct StringInfoData
{
	char	   *data;
	int			len;
	int			maxlen;
	int			cursor;
}			StringInfoData;

typedef StringInfoData *StringInfo;

static inline void
initStringInfo(StringInfo str)
{
	str->maxlen = 1024;
	str->data = (char *) malloc(str->maxlen);
	str->data[0] = '\0';
	str->len = 0;
	str->cursor = 0;
}

static inline void
resetStringInfo(StringInfo str)
{
	str->data[0] = '\0';
	str->len = 0;
	str->cursor = 0;
}

static inline void
enlargeStringInfo(StringInfo str, int needed)
{
	if (str->len + needed >= str->maxlen)
	{
		str->maxlen = (str->len + needed) * 2;
		str->data = (char *) realloc(str->data, str->maxlen);
	}
}

static inline void
appendStringInfoChar(StringInfo str, char ch)
{
	enlargeStringInfo(str, 2);
	str->data[str->len] = ch;
	str->len++;
	str->data[str->len] = '\0';
}

static inline void
appendStringInfoString(StringInfo str, const char *s)
{
	int			slen = strlen(s);

	enlargeStringInfo(str, slen + 1);
	memcpy(str->data + str->len, s, slen + 1);
	str->len += slen;
}

static inline void
pfree(void *ptr)
{
	free(ptr);
}

/* Error handling */
extern jmp_buf fuzz_jmpbuf;
extern int	fuzz_jmpbuf_set;

#define ERROR 21

#ifdef ELOG_ABORT
/* Debug/triage: print and abort so sanitizers report freely */
#define elog(level, ...) do { if ((level) >= ERROR) { \
	fprintf(stderr, "elog ERROR: " __VA_ARGS__); \
	fprintf(stderr, "\n"); abort(); } } while(0)
#else
/* AFL: longjmp back to harness so expected parse failures continue */
#define elog(level, ...) \
	do { \
		if ((level) >= ERROR && fuzz_jmpbuf_set) \
			longjmp(fuzz_jmpbuf, 1); \
	} while (0)
#endif

#ifdef ASSERT_VERBOSE
#define Assert(x) do { if (!(x)) { fprintf(stderr, "ASSERT FAIL: %s at %s:%d\n", #x, __FILE__, __LINE__); abort(); } } while(0)
#else
#define Assert(x) do { if (!(x)) abort(); } while (0)
#endif

/* Suppress postgres.h / nodes/pg_list.h / lib/stringinfo.h includes */
#define POSTGRES_H
#define STRINGINFO_H

#endif							/* PG_STUB_H */
