/*
 * AFL++ harness for ch_http_read_next / array parsing in parser.c.
 *
 * Build modes (selected by compiler and Makefile defines):
 *   AFL compiler     - persistent mode, longjmp-based elog recovery
 *   regular compiler - single-run, elog prints and aborts
 *   ASSERT_VERBOSE   - print failing assertion before abort (pg_stub.h)
 */
#include "pg_stub.h"
#include "http.h"

jmp_buf		fuzz_jmpbuf;
int			fuzz_jmpbuf_set = 0;

#define MAX_INPUT (64 * 1024)
static char	input_buf[MAX_INPUT + 1];

static void
fuzz_one(char *data, int len)
{
	ch_http_read_state state = {0};

	ch_http_read_state_init(&state, data, len);

#ifdef __AFL_HAVE_MANUAL_CONTROL
	fuzz_jmpbuf_set = 1;
	if (setjmp(fuzz_jmpbuf) == 0)
	{
#endif
		int			status;

		do
		{
			status = ch_http_read_next(&state);
		} while (status != CH_EOF);
#ifdef __AFL_HAVE_MANUAL_CONTROL
	}
	fuzz_jmpbuf_set = 0;
#endif

	if (state.val.data)
		free(state.val.data);
}

int
main(int argc, char **argv)
{
#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();

	while (__AFL_LOOP(10000))
	{
#endif
		ssize_t		n = read(STDIN_FILENO, input_buf, MAX_INPUT);

		if (n > 0)
		{
			input_buf[n] = '\0';
			fuzz_one(input_buf, n);
		}
#ifdef __AFL_HAVE_MANUAL_CONTROL
	}
#endif

	return 0;
}
