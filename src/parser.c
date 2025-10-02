#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>

#include <http.h>
#include <internal.h>

void ch_http_read_state_init(ch_http_read_state *state, char *data, size_t datalen)
{
	state->data = datalen > 0 ? data : NULL;
	state->maxpos = datalen - 1;
	state->buflen = 1024;
	state->val = malloc(state->buflen);
	state->done = false;
}

void ch_http_read_state_free(ch_http_read_state *state)
{
	free(state->val);
}

int ch_http_read_next(ch_http_read_state *state)
{
	size_t pos = state->curpos,
		   len = 0;
	char  *data = state->data;

	state->val[0] = '\0';
	if (state->done)
		return CH_EOF;

	while (pos < state->maxpos && data[pos] != '\t' && data[pos] != '\n')
	{
		if (data[pos] == '\\')
		{
			// unescape some sequences
			switch (data[pos+1]) {
				case '\\': state->val[len] = '\\'; break;
				case '\'': state->val[len] = '\''; break;
				case 'n': state->val[len] = '\n'; break;
				case 't': state->val[len] = '\t'; break;
				case '0': state->val[len] = '\0'; break;
				case 'r': state->val[len] = '\r'; break;
				case 'b': state->val[len] = '\b'; break;
				case 'f': state->val[len] = '\f'; break;
				default:
					goto copy;
			}
			len++;
			pos += 2;
		}
		else
copy:
			state->val[len++] = data[pos++];

		/* extend the value size if needed */
		if (len == state->buflen)
		{
			state->buflen *= 2;
			state->val = realloc(state->val, state->buflen);
		}
	}

	state->val[len] = '\0';
	state->curpos = pos + 1;

	if (data[pos] == '\t')
		return CH_CONT;

	assert(data[pos] == '\n');
	int res = pos < state->maxpos ? CH_EOL : CH_EOF;
	state->done = (res == CH_EOF);

	return res;
}
