EXTENSION    = $(shell grep -m 1 '"name":' META.json | \
               sed -e 's/[[:space:]]*"name":[[:space:]]*"\([^"]*\)",/\1/')
EXTVERSION   = $(shell grep -m 1 'default_version' clickhouse_fdw.control | \
               sed -e "s/[[:space:]]*default_version[[:space:]]*=[[:space:]]*'\([^']*\)',\{0,1\}/\1/")
DISTVERSION  = $(shell grep -m 1 '[[:space:]]\{3\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

DATA         = $(wildcard sql/$(EXTENSION)--*.sql)
# DOCS         = $(wildcard doc/*.md)
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test
PG_CONFIG   ?= pg_config
MODULE_big   = $(EXTENSION)

# Collect all the C++ and C files to compile into MODULE_big.
OBJS = $(subst .cpp,.o, $(wildcard src/*.cpp src/*/*.cpp))
OBJS += $(subst .c,.o, $(wildcard src/*.c src/*/*.c))

# clickhouse-cpp source and build directories.
CH_CPP_DIR = vendor/clickhouse-cpp
CH_CPP_BUILD_DIR = $(CH_CPP_DIR)/build

# List the clickhouse-cpp libraries we require.
CH_CPP_LIBS = $(CH_CPP_BUILD_DIR)/clickhouse/libclickhouse-cpp-lib.a \
  $(CH_CPP_BUILD_DIR)/contrib/cityhash/cityhash/libcityhash.a \
  $(CH_CPP_BUILD_DIR)/contrib/absl/absl/libabsl_int128.a \
  $(CH_CPP_BUILD_DIR)/contrib/lz4/lz4/liblz4.a \
  $(CH_CPP_BUILD_DIR)/contrib/zstd/zstd/libzstdstatic.a

# We'll need the clickhouse-cpp libraries.
SHLIB_LINK = $(CH_CPP_LIBS)

# Add include directories.
PG_CPPFLAGS = -I./src/include -I$(CH_CPP_DIR) -I$(CH_CPP_DIR)/contrib/absl

# Include other libraries compiled into clickhouse-cpp.
PG_LDFLAGS = -lstdc++ -lssl -lcrypto

# clickhouse-cpp requires C++ v17.
PG_CXXFLAGS = -std=c++17

# Suppress annoying pre-c99 warning.
PG_CFLAGS = -Wno-declaration-after-statement

# Clean up the clickhouse-cpp build directory.
EXTRA_CLEAN = $(CH_CPP_BUILD_DIR)

# Import PGXS.
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PostgresSQL 15 and earlier violate a C++ v17 storage specifier error.
ifeq ($(shell test $(MAJORVERSION) -lt 16; echo $$?),0)
	PG_CXXFLAGS += -Wno-register
endif

# Add the flags to the bitcode compiler variables.
COMPILE.cc.bc += $(PG_CPPFLAGS)
COMPILE.cxx.bc += $(PG_CXXFLAGS)

# shlib is the final output product: clickhouse-cpp and all .o dependencies.
$(shlib): $(CH_CPP_LIBS) $(OBJS)

# Clone clickhouse-cpp submodule.
$(CH_CPP_DIR)/CMakeLists.txt:
	git submodule update --init

# Build the clickhouse-cpp libraries.
$(CH_CPP_LIBS): export CXXFLAGS=-fPIC
$(CH_CPP_LIBS): export CFLAGS=-fPIC
$(CH_CPP_LIBS): $(CH_CPP_DIR)/CMakeLists.txt
	cmake -B $(CH_CPP_BUILD_DIR) -S $(CH_CPP_DIR) -D CMAKE_BUILD_TYPE=Release -D WITH_OPENSSL=ON
	cmake --build $(CH_CPP_BUILD_DIR) --parallel $(nproc) --target all

# Require the versioned SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

# Build a PGXN distribution bundle.
dist:
	git archive --format zip --prefix=$(EXTENSION)-$(DISTVERSION)/ -o $(EXTENSION)-$(DISTVERSION).zip HEAD

# Generate a list of the changes just for the current version.
latest-changes.md: Changes
	perl -e 'while (<>) {last if /^(v?\Q${DISTVERSION}\E)/; } print "Changes for v${DISTVERSION}:\n"; while (<>) { last if /^\s*$$/; s/^\s+//; print }' Changes > $@
