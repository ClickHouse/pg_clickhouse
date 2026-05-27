EXTENSION    = $(shell grep -m 1 '"name":' META.json | \
               sed -e 's/[[:space:]]*"name":[[:space:]]*"\([^"]*\)",/\1/')
EXTVERSION   = $(shell grep -m 1 'default_version' pg_clickhouse.control | \
               sed -e "s/[[:space:]]*default_version[[:space:]]*=[[:space:]]*'\([^']*\)',\{0,1\}/\1/")
DISTVERSION  = $(shell grep -m 1 '^[[:space:]]\{2\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

DATA         = $(sort $(wildcard sql/$(EXTENSION)--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql)
DOCS         = $(wildcard doc/*.md)
TESTS        ?= $(wildcard test/sql/*.sql)
REGRESS      = --schedule test/schedule
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION)
PG_CONFIG   ?= pg_config
MODULE_big   = $(EXTENSION)
CURL_CONFIG ?= curl-config
OS 	        ?= $(shell uname -s | tr A-Z a-z)
ARCH         = $(shell uname -m)

# Collect all the C files to compile into MODULE_big.
OBJS = $(subst .c,.o, $(wildcard src/*.c src/*/*.c))

# clickhouse-c is a header-only single-header library. Override
# CH_C_DIR to point elsewhere when developing against a local checkout.
CH_C_DIR ?= vendor/clickhouse-c

# Add include directories.
PG_CPPFLAGS = -I./src/include -I$(CH_C_DIR)

# Link OpenSSL (for TLS in the binary driver), curl (for the HTTP driver),
# libuuid (for http_streaming.c's query-id generator), and lz4 / zstd
# (for the binary driver's compressed-frame codecs).
PG_LDFLAGS = -lssl -lcrypto -llz4 -lzstd $(shell $(CURL_CONFIG) --libs)

# libuuid is provided by the OS on darwin; explicit link elsewhere.
ifneq ($(OS),darwin)
	PG_LDFLAGS += -luuid
endif

# Suppress annoying pre-c99 warning and include curl flags.
PG_CFLAGS = -Wno-declaration-after-statement -Werror=type-limits $(shell $(CURL_CONFIG) --cflags)

# Clean up generated files.
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql src/include/version.h compile_commands.json test/schedule $(EXTENSION)-$(DISTVERSION).zip

# Import PGXS.
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Clone clickhouse-c submodule.
$(CH_C_DIR)/clickhouse.h: .gitmodules
	git submodule update --init

# Require clickhouse-c and the version header.
$(OBJS): $(CH_C_DIR)/clickhouse.h src/include/version.h

# Require the versioned C source and SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql

# Versioned SQL script.
sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

# Versioned source file.
src/include/version.h: src/include/version.h.in
	sed -e 's,__VERSION__,$(DISTVERSION),g' $< > $@

# Build a PGXN distribution bundle.
dist: $(EXTENSION)-$(DISTVERSION).zip

$(EXTENSION)-$(DISTVERSION).zip:
	git archive-all -v --prefix "$(EXTENSION)-$(DISTVERSION)/" --force-submodules $(EXTENSION)-$(DISTVERSION).zip

.PHONY: test/schedule # Depends on $(TESTS), so always rebuild.
test/schedule:
	@echo "test: $(patsubst test/sql/%.sql,%,$(TESTS))" > $@

installcheck: test/schedule

# Test the PGXN distribution.
dist-test: $(EXTENSION)-$(DISTVERSION).zip
	unzip $(EXTENSION)-$(DISTVERSION).zip
	cd $(EXTENSION)-$(DISTVERSION)
	make && make install && make installcheck

.PHONY: release-notes # Show release notes for current version (must have `mknotes` in PATH).
release-notes: CHANGELOG.md
	mknotes -v v$(DISTVERSION) -f $< -r https://github.com/$(or $(GITHUB_REPOSITORY),ClickHouse/pg_clickhouse)

.PHONY: tempcheck # Run tests with a temporary PostgreSQL instance
tempcheck: install
	$(pg_regress_installcheck) --temp-instance=/tmp/pg_clickhouse_test $(REGRESS_OPTS) $(REGRESS)

# Run `make installcheck` and copy all result files to test/expected/. Use for
# basic test changes with the latest version of Postgres, but be aware that
# alternate `_n.out` files will not be updated.
#
# DO NOT RUN UNLESS YOU'RE CERTAIN ALL YOUR TESTS ARE PASSING!
.PHONY: results
results:
	$(MAKE) installcheck || true
	rsync -rlpgovP results/ test/expected

# Run make print-VARIABLE_NAME to print VARIABLE_NAME's flavor and value.
print-%	: ; $(info $* is $(flavor $*) variable set to "$($*)") @true

# OCI images.
REGISTRY ?= localhost:5001
REVISION := $(shell git rev-parse --short HEAD)
PLATFORMS ?= linux/amd64,linux/arm64
PG_VERSIONS ?= 18,17,16,15,14,13
.PHONY: image # Build the linux/amd64 OCI image.
image:
	registry=$(REGISTRY) version=$(DISTVERSION) revision=$(REVISION) pg_versions=$(PG_VERSIONS) \
	docker buildx bake --set "*.platform=$(PLATFORMS)" \
	$(if $(filter true,$(PUSH)),--push,) \
	$(if $(filter true,$(LOAD)),--load,) \

bake-vars:
	@echo "registry=$(REGISTRY)"
	@echo "version=$(DISTVERSION)"
	@echo "revision=$(REVISION)"
	@echo "pg_versions=$(PG_VERSIONS)"

# Format the .c and .h files according to the PostgreSQL indentation
# standard. Requires `pg_bsd_indent` to be in the path.
indent: dev/indent.sh
	@$<

# Linting.
.PHONY: lint # Lint the project
lint: .pre-commit-config.yaml
	@pre-commit run --show-diff-on-failure --color=always --all-files

.PHONY: clang-tidy # Run clang-tidy static analysis (requires compile_commands.json)
clang-tidy: compile_commands.json
	clang-tidy -p . $(wildcard src/*.c src/*/*.c)

## .git/hooks/pre-commit: Install the pre-commit hook
.git/hooks/pre-commit:
	@printf "#!/bin/sh\nmake lint\n" > $@
	@chmod +x $@

debian-install-lint:
	@curl -SsLo /tmp/pre-commit.pyz https://github.com/pre-commit/pre-commit/releases/download/v4.5.1/pre-commit-4.5.1.pyz
	@printf "#!/bin/sh\npython3 /tmp/pre-commit.pyz \"\$$@\"\n" > /usr/local/bin/pre-commit
	@chmod +x /usr/local/bin/pre-commit

.PHONY: lsp # Generate compile_commands.json for IDE/clangd support.
lsp: compile_commands.json

# Requires https://github.com/rizsotto/Bear.
compile_commands.json:
	$(MAKE) clean -j $$(nproc)
	bear -- $(MAKE) all -j $$(nproc)

# ClickHouse Docker Containers
start-containers: dev/Makefile dev/docker-compose.yml
	@$(MAKE) -C dev start

stop-containers: dev/Makefile dev/docker-compose.yml
	@$(MAKE) -C dev stop
