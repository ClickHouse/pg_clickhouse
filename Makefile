EXTENSION    = $(shell grep -m 1 '"name":' META.json | \
               sed -e 's/[[:space:]]*"name":[[:space:]]*"\([^"]*\)",/\1/')
EXTVERSION   = $(shell grep -m 1 '[[:space:]]\{8\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')
DISTVERSION  = $(shell grep -m 1 '[[:space:]]\{3\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

DATA         = $(wildcard sql/*.sql)
DOCS         = $(wildcard doc/*.md)
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test
PG_CONFIG   ?= pg_config

MODULE_big = $(EXTENSION)

# Collect all the C++ and C files to compile into MODULE_big.
OBJS = $(subst .cpp,.o, $(wildcard src/*.cpp src/*/*.cpp))
OBJS += $(subst .c,.o, $(wildcard src/*.c src/*/*.c))

# Find all the clickhouse-cpp static libraries we need.
SHLIB_LINK = $(shell find build -name '*.a')

# Add include directories.
PG_CPPFLAGS = -I./src/include -I./clickhouse-cpp -I./clickhouse-cpp/contrib/absl

# Include other libraries compiled into clickhouse-cpp.
PG_LDFLAGS = -lstdc++ -lssl -lcrypto

# clickhouse-cpp requires C++ v17.
PG_CXXFLAGS = -std=c++17

# Suppress annoying pre-c99 warning.
PG_CFLAGS = -Wno-declaration-after-statement

EXTRA_CLEAN = build

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

all:

clickhouse: build/clickhouse/libclickhouse-cpp-lib.a

build/clickhouse/libclickhouse-cpp-lib.a:
	cmake -B build -S clickhouse-cpp -D CMAKE_BUILD_TYPE=Release -D WITH_OPENSSL=ON
	cmake --build build --target all

dist:
	git archive --format zip --prefix=$(EXTENSION)-$(DISTVERSION)/ -o $(EXTENSION)-$(DISTVERSION).zip HEAD

latest-changes.md: Changes
	perl -e 'while (<>) {last if /^(v?\Q${DISTVERSION}\E)/; } print "Changes for v${DISTVERSION}:\n"; while (<>) { last if /^\s*$$/; s/^\s+//; print }' Changes > $@
