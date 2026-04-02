# Fuzzing pg_clickhouse with AFL++

Fuzz the HTTP response parser (`src/parser.c`) which converts ClickHouse
TSV-formatted responses (including array/string literals) into PostgreSQL
values. PostgreSQL dependencies are stubbed so the harness runs standalone.

## Build

```sh
make -C fuzz
```

Requires `afl-clang-fast` (AFL++ with LLVM mode). Override `AFL_CC` to use a
different compiler, e.g. `make -C fuzz AFL_CC=afl-gcc`.

## Run

```sh
make -C fuzz fuzz
```

Or manually:

```sh
afl-fuzz -i fuzz/corpus -o fuzz/findings -- fuzz/fuzz_parser
```

## Corpus

`fuzz/corpus/` contains seed inputs representing ClickHouse TSV responses.
Add more seeds as you discover interesting edge cases.

## Tuning

Many ClickHouse responses will trigger `elog(ERROR, ...)` which the stub
converts to `longjmp`. The harness catches these via `setjmp` so AFL treats
them as expected failures rather than crashes. Adjust `pg_stub.h` if you
need different behavior (e.g. treating certain errors as bugs).
