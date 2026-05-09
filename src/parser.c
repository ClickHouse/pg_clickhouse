#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>

#include <http.h>
#include <internal.h>

static void ch_http_read_array_string_literal(ch_http_read_state * state);
static void ch_http_read_array(ch_http_read_state * state);
static int	ch_http_read_eof(ch_http_read_state * state);
static void ch_http_parse_error(const char *msg);

/*
 * The streaming path can pass the parser a bounded slice rather than a
 * NUL-terminated buffer, so reaching datalen is the equivalent of the old
 * '\0' checks.
 */
inline static int
ch_http_read_eof(ch_http_read_state * state)
{
	state->done = true;
	return CH_EOF;
}

inline static void
ch_http_parse_error(const char *msg)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("pg_clickhouse: %s", msg)));
}

void
ch_http_read_state_init(ch_http_read_state * state, char *data, size_t datalen)
{
	state->data = datalen > 0 ? data : NULL;
	state->datalen = datalen;
	state->curpos = 0;
	state->done = false;
	state->is_null = false;
	if (state->val.data == NULL)
		initStringInfo(&state->val);
	else
		resetStringInfo(&state->val);
}

/*
 * Parse the next tab-separated value from state->data. Parses tab-delimited
 * ClickHouse literals including unquoted strings and arrays. Returns CH_CONT
 * if there are moe fields on the line to read, CH_EOL if it has reached the
 * end of the line, and CH_EOF if it has reached the end of the file.
 *
 * `is_array` tells the parser whether the destination Postgres column is an
 * array. TabSeparated is ambiguous: a `String` value beginning with `[` is not
 * escaped on the wire and is indistinguishable byte-for-byte from an array
 * literal until you know the column type. The caller knows the type, so it
 * makes the call.
 */
int
ch_http_read_next(ch_http_read_state * state, bool is_array)
{
	char	   *data = state->data;

	if (state->done)
		return CH_EOF;
	if (data == NULL)
		return ch_http_read_eof(state);

	resetStringInfo(&state->val);
	state->is_null = false;
	if (state->curpos >= state->datalen)
		return ch_http_read_eof(state);

	/*
	 * Detect the wire NULL marker. ClickHouse's TabSeparated format encodes
	 * NULL as the bare 2-byte sequence `\N`. A literal backslash in data is
	 * sent as `\\`, so legitimate non-null output never contains `\N` at the
	 * start of a field followed by a delimiter. Detect it here, before
	 * unescaping, so a non-null String value whose unescaped content happens
	 * to be `\N` is not collapsed with the NULL marker.
	 */
	if (state->curpos + 1 < state->datalen
		&& data[state->curpos] == '\\'
		&& data[state->curpos + 1] == 'N'
		&& (state->curpos + 2 == state->datalen
			|| data[state->curpos + 2] == '\t'
			|| data[state->curpos + 2] == '\n'))
	{
		state->is_null = true;
		state->curpos += 2;
	}
	else if (is_array && data[state->curpos] == '[')
		/* Parse array literal. */
		ch_http_read_array(state);
	else
	{
		while (state->curpos < state->datalen &&
			   data[state->curpos] != '\t' &&
			   data[state->curpos] != '\n')
		{
			if (data[state->curpos] == '\\')
			{
				state->curpos++;
				if (state->curpos >= state->datalen)
					return ch_http_read_eof(state);
				/* unescape some sequences */
				switch (data[state->curpos])
				{
					case 'n':
						appendStringInfoChar(&state->val, '\n');
						break;
					case 't':
						appendStringInfoChar(&state->val, '\t');
						break;
					case '0':
						appendStringInfoChar(&state->val, '\0');
						break;
					case 'r':
						appendStringInfoChar(&state->val, '\r');
						break;
					case 'b':
						appendStringInfoChar(&state->val, '\b');
						break;
					case 'f':
						appendStringInfoChar(&state->val, '\f');
						break;
					case 'N':
						/* NULL (format_tsv_null_representation) */
						appendStringInfoString(&state->val, "\\N");
						break;
					default:
						appendStringInfoChar(&state->val, data[state->curpos]);
				}
				state->curpos++;
			}
			else
				appendStringInfoChar(&state->val, data[state->curpos++]);
		}
	}

	if (state->curpos >= state->datalen)
		return ch_http_read_eof(state);

	if (data[state->curpos] == '\t')
	{
		/* There are more fields. */
		state->curpos++;
		return CH_CONT;
	}

	/* Should be at the end of the line or the file. */
	if (data[state->curpos] != '\n')
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("unexpected byte (%d) after array", data[state->curpos])));
	}

	state->curpos++;
	if (state->curpos >= state->datalen)
		return ch_http_read_eof(state);

	return CH_EOL;
}

/*
 * Convert a single-quoted string from a ClickHouse array to a double-quoted
 * PostgreSQL array string. Based on `readQuotedStringFieldInto()` from
 * `src/IO/ReadHelpers.cpp` in the ClickHouse source. The basic conversion is:
 *
 * - " => \"
 * - \\ => \\
 * - \ followed by b, f, r, n, t, or 0 => literal char
 * - \ followed by any other character (including ') => append that character
 * - Append any other character
 *
 * https://clickhouse.com/docs/interfaces/formats/TabSeparated
 * https://www.postgresql.org/docs/current/arrays.html#ARRAYS-IO
 */
void
ch_http_read_array_string_literal(ch_http_read_state * state)
{
	/* Postgres array string is double-quoted. */
	appendStringInfoChar(&state->val, '"');
	state->curpos++;

	while (state->curpos < state->datalen &&
		   state->data[state->curpos] != '\'')
	{
		char		ch = state->data[state->curpos];

		if (ch == '"')
		{
			/* Escape double quotation mark. */
			appendStringInfoChar(&state->val, '\\');
			appendStringInfoChar(&state->val, ch);
			state->curpos++;
		}
		else if (ch == '\\')
		{
			/* Emit the escaped character. */
			state->curpos++;
			if (state->curpos >= state->datalen)
				ch_http_parse_error("invalid array string");

			switch (state->data[state->curpos])
			{
				case '\\':
					appendStringInfoChar(&state->val,
										 state->data[state->curpos]);
					appendStringInfoChar(&state->val,
										 state->data[state->curpos]);
					break;
				case 'b':
					appendStringInfoChar(&state->val, '\b');
					break;
				case 'f':
					appendStringInfoChar(&state->val, '\f');
					break;
				case 'r':
					appendStringInfoChar(&state->val, '\r');
					break;
				case 'n':
					appendStringInfoChar(&state->val, '\n');
					break;
				case 't':
					appendStringInfoChar(&state->val, '\t');
					break;
				case '0':
					appendStringInfoChar(&state->val, '\0');
					break;
				default:
					/* Includes ' and probably no other character. */
					appendStringInfoChar(&state->val,
										 state->data[state->curpos]);
			}
			state->curpos++;
		}
		else
		{
			/* Append any other character. */
			appendStringInfoChar(&state->val, ch);
			state->curpos++;
		}
	}

	if (state->curpos >= state->datalen ||
		state->data[state->curpos] != '\'')
		ch_http_parse_error("invalid array string");

	appendStringInfoChar(&state->val, '"');
	state->curpos++;
}

/*
 * Convert a ClickHouse array literal to a Postgres array literal. Supports
 * nested array. The conversions are:
 *
 * - [ => {
 * - ] => }
 * - ' => Start of string, convert to double-quoted string
 * - Append any other character
 *
 * Based on `readQuotedFieldInBracketsInto()` from `src/IO/ReadHelpers.cpp` in
 * the ClickHouse source. Additional References:
 *
 * https://clickhouse.com/docs/interfaces/formats/TabSeparated
 * https://www.postgresql.org/docs/current/arrays.html#ARRAYS-IO
 */
void
ch_http_read_array(ch_http_read_state * state)
{
	size_t		balance = 1;

	/* Postgres arrays are wrapped in { and }. */
	appendStringInfoChar(&state->val, '{');
	state->curpos++;

	while (state->curpos < state->datalen && balance)
	{
		switch (state->data[state->curpos])
		{
			case '\'':
				ch_http_read_array_string_literal(state);
				break;
			case '[':
				++balance;
				appendStringInfoChar(&state->val, '{');
				state->curpos++;
				break;
			case ']':
				--balance;
				appendStringInfoChar(&state->val, '}');
				state->curpos++;
				break;
			default:
				appendStringInfoChar(&state->val, state->data[state->curpos]);
				state->curpos++;
		}
	}

	if (balance != 0)
		ch_http_parse_error("malformed array literal");
}
