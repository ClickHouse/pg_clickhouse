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

# Collect all the C++ and C files to compile into MODULE_big.
OBJS = $(subst .cpp,.o, $(wildcard src/*.cpp src/*/*.cpp)) \
       $(subst .c,.o, $(wildcard src/*.c src/*/*.c))

# Build static on Darwin by default.
ifndef CH_BUILD
# ifeq ($(OS),darwin)
	CH_BUILD = static
# else
# 	CH_BUILD = dynamic
# endif
endif

# clickhouse-cpp source and build directories.
CH_CPP_DIR = vendor/clickhouse-cpp
CH_CPP_BUILD_DIR = vendor/_build/$(OS)-$(ARCH)-$(CH_BUILD)-$(shell git submodule status $(CH_CPP_DIR) | awk '{print substr($$1, 0, 7)}')

# List the clickhouse-cpp libraries we require.
CH_CPP_LIB = $(CH_CPP_BUILD_DIR)/clickhouse/libclickhouse-cpp-lib$(DLSUFFIX)
CH_CPP_FLAGS = -D CMAKE_BUILD_TYPE=Release -D WITH_OPENSSL=ON

# Are we statically compiling clickhouse-cpp into the extension or no?
ifeq ($(CH_BUILD), static)
# We'll need all the clickhouse-cpp static libraries.
	CH_CPP_LIB = $(CH_CPP_BUILD_DIR)/clickhouse/libclickhouse-cpp-lib.a
	SHLIB_LINK = $(CH_CPP_LIB) \
	  $(CH_CPP_BUILD_DIR)/contrib/cityhash/cityhash/libcityhash.a \
	  $(CH_CPP_BUILD_DIR)/contrib/absl/absl/libabsl_int128.a \
	  $(CH_CPP_BUILD_DIR)/contrib/lz4/lz4/liblz4.a \
	  $(CH_CPP_BUILD_DIR)/contrib/zstd/zstd/libzstdstatic.a
else
#   Build and install the shared library.
	SHLIB_LINK = -L$(CH_CPP_BUILD_DIR)/clickhouse -lclickhouse-cpp-lib
	CH_CPP_FLAGS += -D BUILD_SHARED_LIBS=ON
endif

# Add include directories.
PG_CPPFLAGS = -I./src/include -I$(CH_CPP_DIR) -I$(CH_CPP_DIR)/contrib/absl

# Include other libraries compiled into clickhouse-cpp.
PG_LDFLAGS = -lstdc++ -lssl -lcrypto $(shell $(CURL_CONFIG) --libs)

# clickhouse-cpp requires C++ v17.
PG_CXXFLAGS = -std=c++17

# Suppress annoying pre-c99 warning and include curl flags.
PG_CFLAGS = -Wno-declaration-after-statement -Werror=type-limits $(shell $(CURL_CONFIG) --cflags)

# We'll need libuuid except on darwin, where it's included in the OS.
ifneq ($(OS),darwin)
	PG_LDFLAGS += -luuid
endif

# Clean up the clickhouse-cpp build directory and generated files.
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql src/include/version.h compile_commands.json test/schedule $(EXTENSION)-$(DISTVERSION).zip
ifndef NO_VENDOR_CLEAN
	EXTRA_CLEAN += $(CH_CPP_BUILD_DIR)
endif

# Import PGXS.
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# We'll need the clickhouse-cpp library and rpath so it can be found.
SHLIB_LINK += -Wl,-rpath,$(pkglibdir)/

# PostgreSQL 15 and earlier violate a C++ v17 storage specifier error.
ifeq ($(shell test $(MAJORVERSION) -lt 16; echo $$?),0)
	PG_CXXFLAGS += -Wno-register
endif

# Add the flags to the bitcode compiler variables.
COMPILE.cc.bc += $(PG_CPPFLAGS)
COMPILE.cxx.bc += $(PG_CXXFLAGS)

# shlib is the final output product: clickhouse-cpp and all .o dependencies.
$(shlib): $(CH_CPP_LIB) $(OBJS)

# Clone clickhouse-cpp submodule.
$(CH_CPP_DIR)/CMakeLists.txt:
	git submodule update --init

# Require the vendored clickhouse-cpp and the version header.
$(OBJS): $(CH_CPP_DIR)/CMakeLists.txt src/include/version.h

# Build clickhouse-cpp.
$(CH_CPP_LIB): export CXXFLAGS=-fPIC
$(CH_CPP_LIB): export CFLAGS=-fPIC
$(CH_CPP_LIB): $(CH_CPP_DIR)/CMakeLists.txt # Sync with "Reset Vendor Timestamp" steps in workflows.
	cmake -B $(CH_CPP_BUILD_DIR) -S $(CH_CPP_DIR) $(CH_CPP_FLAGS) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build $(CH_CPP_BUILD_DIR) --parallel $$(nproc) --target all

# Require the versioned C source and SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql

# Versioned SQL script.
sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

# Versioned source file.
src/include/version.h: src/include/version.h.in
	sed -e 's,__VERSION__,$(DISTVERSION),g' $< > $@

# Configure install/uninstall of the clickhouse-cpp library.
ifneq ($(CH_BUILD), static)
# Copy all dynamic files; use -a to preserve symlinks.
install-ch-cpp: $(CH_CPP_LIB) $(shlib)
	cp -a $(CH_CPP_BUILD_DIR)/clickhouse/libclickhouse-cpp-lib*$(DLSUFFIX)* $(DESTDIR)$(pkglibdir)/
uninstall-ch-cpp:
	rm -f $(DESTDIR)$(pkglibdir)/libclickhouse-cpp-lib*$(DLSUFFIX)*
install: install-ch-cpp
uninstall: uninstall-ch-cpp
endif

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

# Format the .c, .h, and .hh files according to the PostgreSQL indentation
# standard. Requires `pg_bsd_indent` to be in the path.
indent: dev/indent.sh
	@$<
	clang-format --style=file:.clang-format -i src/binary.cpp

# Linting.
.PHONY: lint # Lint the project
lint: .pre-commit-config.yaml
	@pre-commit run --show-diff-on-failure --color=always --all-files

.PHONY: clang-tidy # Run clang-tidy static analysis (requires compile_commands.json)
clang-tidy: compile_commands.json
	clang-tidy -p . $(wildcard src/*.c src/*.cpp)

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
	$(MAKE) clean -j $$(nproc) NO_VENDOR_CLEAN=$(NO_VENDOR_CLEAN)
	bear -- $(MAKE) all -j $$(nproc) NO_VENDOR_CLEAN=$(NO_VENDOR_CLEAN)

# ClickHouse Docker Containers
start-containers: dev/Makefile dev/docker-compose.yml
	@$(MAKE) -C dev start

stop-containers: dev/Makefile dev/docker-compose.yml
	@$(MAKE) -C dev stop
