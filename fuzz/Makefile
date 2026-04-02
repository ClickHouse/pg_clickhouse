AFL_CC    ?= afl-clang-fast
CC        ?= clang
CFLAGS    ?= -g -O2 -fsanitize=address,undefined -fno-omit-frame-pointer
SRC_DIR    = ../src
INCLUDE    = -I. -I$(SRC_DIR)/include

# Stub out curl/curl.h
CFLAGS    += -include pg_stub.h -Dcurl_off_t=long $(INCLUDE)

# Override postgres.h via pg_stub.h
# parser.c includes http.h -> postgres.h, nodes/pg_list.h, lib/stringinfo.h, engine.h

SRCS       = fuzz_parser.c $(SRC_DIR)/parser.c

.PHONY: all clean fuzz triage

all: fuzz_parser

STUB_HEADERS = stubs/postgres.h stubs/nodes/pg_list.h stubs/nodes/pathnodes.h \
               stubs/lib/stringinfo.h stubs/engine.h stubs/curl/curl.h

fuzz_parser: $(SRCS) $(STUB_HEADERS)
	$(AFL_CC) $(CFLAGS) -I stubs -o $@ $(SRCS)

fuzz_parser_debug: $(SRCS) $(STUB_HEADERS)
	$(CC) $(CFLAGS) -O0 -DELOG_ABORT -I stubs -o $@ $(SRCS)

fuzz_parser_triage: $(SRCS) $(STUB_HEADERS)
	$(CC) $(CFLAGS) -O0 -DELOG_ABORT -DASSERT_VERBOSE -I stubs -o $@ $(SRCS)

# Stub headers — empty files satisfying #include directives
$(STUB_HEADERS):
	@mkdir -p $(dir $@)
	@touch $@

clean:
	rm -rf fuzz_parser fuzz_parser_debug fuzz_parser_triage stubs findings

fuzz: fuzz_parser corpus
	AFL_SKIP_CPUFREQ=1 afl-fuzz -i corpus -o findings -- ./fuzz_parser

# Reproduce each crash with verbose Assert/elog diagnostics
triage: fuzz_parser_triage
	@for f in findings/default/crashes/id:*; do \
		printf '=== %s ===\n' "$$(basename "$$f")"; \
		./fuzz_parser_triage < "$$f" 2>&1 || true; \
		echo; \
	done
