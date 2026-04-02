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

void
ch_http_read_state_init(ch_http_read_state * state, char *data, size_t datalen)
{
	state->data = datalen > 0 ? data : NULL;
	state->done = false;
	initStringInfo(&state->val);
}

/*
 * Parse the next tab-separated value from state->data. Parses tab-delimited
 * ClickHouse literals including unquoted strings and arrays. Returns CH_CONT
 * if there are moe fields on the line to read, CH_EOL if it has reached the
 * end of the line, and CH_EOF if it has reached the end of the file.
 */
int
ch_http_read_next(ch_http_read_state * state)
{
	char	   *data = state->data;

	resetStringInfo(&state->val);
	if (state->done)
		return CH_EOF;

	if (data[state->curpos] == '[')
		/* Parse array literal. */
		ch_http_read_array(state);
	else
	{
		while (data[state->curpos] != '\0' && data[state->curpos] != '\t' && data[state->curpos] != '\n')
		{
			if (data[state->curpos] == '\\')
			{
				state->curpos++;
				/* unescape some sequences */
				switch (data[state->curpos])
				{
					case '\0':
						/* unexpected end */
						state->done = true;
						state->curpos++;
						return CH_EOF;
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

	if (data[state->curpos] == '\t')
	{
		/* There are more fields. */
		state->curpos++;
		return CH_CONT;
	}

	/* Should be at the end of the line or the file. */
	if (data[state->curpos] != '\n' && data[state->curpos] != '\0')
	{
		elog(ERROR, "Unexpected character %c after array", data[state->curpos]);
	}
	int			res = data[state->curpos] == '\0' || data[state->curpos + 1] == '\0' ? CH_EOF : CH_EOL;

	state->done = (res == CH_EOF);
	state->curpos++;

	return res;
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
	char	   *src = state->data + state->curpos;

	while (*src != '\0' && *src != '\'')
	{
		if (*src == '"')
		{
			/* Escape double quotation mark. */
			appendStringInfoChar(&state->val, '\\');
			appendStringInfoChar(&state->val, *src);
			state->curpos++;
		}
		else if (*src == '\\')
		{
			/* Emit the escaped character. */
			++src;
			switch (*src)
			{
				case '\\':
					appendStringInfoChar(&state->val, *src);
					appendStringInfoChar(&state->val, *src);
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
					appendStringInfoChar(&state->val, *src);
			}
			state->curpos += 2;
		}
		else
		{
			/* Append any other character. */
			appendStringInfoChar(&state->val, *src);
			state->curpos++;
		}

		++src;
	}

	if (*src != '\'')
		elog(ERROR, "Invalid array string");

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

	while (state->data[state->curpos] != '\0' && balance)
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
		elog(ERROR, "malformed array literal");
}
