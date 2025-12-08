EXTENSION    = $(shell grep -m 1 '"name":' META.json | \
               sed -e 's/[[:space:]]*"name":[[:space:]]*"\([^"]*\)",/\1/')
EXTVERSION   = $(shell grep -m 1 'default_version' pg_clickhouse.control | \
               sed -e "s/[[:space:]]*default_version[[:space:]]*=[[:space:]]*'\([^']*\)',\{0,1\}/\1/")
DISTVERSION  = $(shell grep -m 1 '[[:space:]]\{3\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

DATA         = $(wildcard sql/$(EXTENSION)--*.sql)
DOCS         = $(wildcard doc/*.md)
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION)
PG_CONFIG   ?= pg_config
MODULE_big   = $(EXTENSION)
CURL_CONFIG ?= curl-config
OS 	        ?= $(shell uname -s)

# Collect all the C++ and C files to compile into MODULE_big.
OBJS = $(sort \
    $(subst .cpp,.o, $(wildcard src/*.cpp src/*/*.cpp)) \
    $(subst .c.in,.o, $(wildcard src/*.c.in src/*/*.c)) \
    $(subst .c,.o, $(wildcard src/*.c src/*/*.c)) \
)

# clickhouse-cpp source and build directories.
CH_CPP_DIR = vendor/clickhouse-cpp
MACHINE = $(shell echo "$$(uname -s)-$$(uname -m)" | tr '[:upper:]' '[:lower:]')
CH_CPP_BUILD_DIR = vendor/_build/$(MACHINE)

# List the clickhouse-cpp libraries we require.
CH_CPP_LIB = $(CH_CPP_BUILD_DIR)/clickhouse/libclickhouse-cpp-lib$(DLSUFFIX)
CH_CPP_FLAGS = -D CMAKE_BUILD_TYPE=Release -D WITH_OPENSSL=ON

# Build static on Darwin by default.
ifndef ($(CH_BUILD))
ifeq ($(OS),Darwin)
	CH_BUILD = static
endif
endif

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
PG_CFLAGS = -Wno-declaration-after-statement $(shell $(CURL_CONFIG) --cflags)

# We'll need libuuid except on darwin, where it's included in the OS.
ifneq ($(OS),Darwin)
	PG_LDFLAGS += -luuid
endif

# Clean up the clickhouse-cpp build directory and generated files.
EXTRA_CLEAN = $(CH_CPP_BUILD_DIR) sql/$(EXTENSION)--$(EXTVERSION).sql src/fdw.c compile_commands.json

# Import PGXS.
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# We'll need the clickhouse-cpp library and rpath so it can be found.
SHLIB_LINK += -Wl,-rpath,$(pkglibdir)/

# PostgresSQL 15 and earlier violate a C++ v17 storage specifier error.
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

# Require the vendored clickhouse-cpp.
$(OBJS): $(CH_CPP_DIR)/CMakeLists.txt

# Build clickhouse-cpp.
$(CH_CPP_LIB): export CXXFLAGS=-fPIC
$(CH_CPP_LIB): export CFLAGS=-fPIC
$(CH_CPP_LIB): $(CH_CPP_DIR)/CMakeLists.txt
	cmake -B $(CH_CPP_BUILD_DIR) -S $(CH_CPP_DIR) $(CH_CPP_FLAGS) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build $(CH_CPP_BUILD_DIR) --parallel $(nproc) --target all

# Require the versioned C source and SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql src/fdw.c

# Versioned SQL script.
sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

# Versioned source file.
src/fdw.c: src/fdw.c.in
	sed -e 's,__VERSION__,$(DISTVERSION),g' $< > $@

# Configure the installation of the clickhouse-cpp library.
ifeq ($(CH_BUILD), static)
install-ch-cpp:
else
# Copy all dynamic files; use -a to preserve symlinks.
install-ch-cpp: $(CH_CPP_LIB) $(shlib)
	cp -a $(CH_CPP_BUILD_DIR)/clickhouse/libclickhouse-cpp-lib*$(DLSUFFIX)* $(DESTDIR)$(pkglibdir)/
endif

install: install-ch-cpp

# Build a PGXN distribution bundle.
dist: $(EXTENSION)-$(DISTVERSION).zip

$(EXTENSION)-$(DISTVERSION).zip:
	git archive-all -v --prefix "$(EXTENSION)-$(DISTVERSION)/" --force-submodules $(EXTENSION)-$(DISTVERSION).zip

# Test the PGXN distribution.
dist-test: $(EXTENSION)-$(DISTVERSION).zip
	unzip $(EXTENSION)-$(DISTVERSION).zip
	cd $(EXTENSION)-$(DISTVERSION)
	make && make install && make installcheck

# Generate a list of the changes just for the current version.
latest-changes.md: Changes
	perl -e 'while (<>) {last if /^(v?\Q${DISTVERSION}\E)/; } print "Changes for v${DISTVERSION}:\n"; while (<>) { last if /^\s*$$/; s/^\s+//; print }' Changes > $@

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

# Format the .c and .h files according to the PostgreSQL indentation standard.
# Requires `pg_bsd_indent` to be in the path.
indent:
	@for fn in $(wildcard src/*.c.in src/*.c src/*/*.hh src/*/*.cpp); do printf "%s\n" "$$fn"; pg_bsd_indent -bad -bap -bbb -bc -bl -cli1 -cp33 -cdb -nce -d0 -di12 -nfc1 -i4 -l79 -lp -lpl -nip -npro -sac -tpg -ts4 "$$fn"; done
	@rm *.BAK

.PHONY: lsp # Generate compile_commands.json for IDE/clangd support.
lsp: compile_commands.json

# Requires https://github.com/rizsotto/Bear.
compile_commands.json:
	$(MAKE) clean
	bear -- $(MAKE) all
