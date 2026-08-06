# Changelog

All notable changes to this project will be documented in this file. It uses the
[Keep a Changelog] format, and this project adheres to [Semantic Versioning].

  [Keep a Changelog]: https://keepachangelog.com/en/1.1.0/
  [Semantic Versioning]: https://semver.org/spec/v2.0.0.html
    "Semantic Versioning 2.0.0"

## [0.4.0] — Unreleased

### ⚡ Improvements

*   Added the `clickhouse_server_version(server)` function, which reports the
    ClickHouse server version ("major.minor.patch") for a foreign server
    ([#293]).
*   Added pushdown for subqueries the planner cannot flatten into joins
    (SubPlans): correlated and uncorrelated scalar aggregate subqueries,
    `EXISTS`, and single-column equality `IN`/`NOT IN`, in `WHERE` and `HAVING`.
    Covers TPC-H Q2, Q11, Q15, Q17, Q20, and Q22 shapes. `NOT IN` preserves
    PostgreSQL's NULL semantics under ClickHouse's two-valued `IN`, deparsing
    with compensating guards when the columns involved are nullable ([#315]).
    Requires ClickHouse 25.8 or later; older servers keep evaluating the
    subquery locally ([#289]).
*   Added `transform_null_in 0` to the default value of the
    `pg_clickhouse.session_settings` parameter, so that a ClickHouse server
    profile cannot silently change the `IN` semantics the pushdown rules rely
    on ([#315]).
*   Added or corrected pushdown for `IN`-family operations on arrays. Previous
    behavior was missing or incorrect, either structurally or due to behavioral
    differences in the ClickHouse implementation of the operation. Corrected
    the structurally incorrect pushdown ([#315]) and handled ClickHouse
    behavioral changes using special case statements where needed ([#317]).
*   Added pushdown for more aggregate functions ([#290]):
    *   [corr]
    *   [covarpop]
    *   [covarsamp]
    *   [stddev_pop]
    *   [stddev_samp/stddev]
    *   [var_op]
    *   [var_samp/variance]
    *   [any_value]
*   Mapped the following PostgreSQL ordered set aggregate functions to push
    down to corresponding ClickHouse parametric aggregate functions ([#291]):
    *   `percentile_cont(double[])` → `quantiles()`
    *   `percentile_disc(double)` → `quantileExactLow()`
    *   `percentile_disc(double[])` → `quantilesExactLow()`
*   Added pushdown for `encode(bytea, fmt)` with a constant format:
    *   `encode(bytea, 'hex')` → `lower(hex())`
    *   `encode(bytea, 'base64')` → `base64Encode()` wrapped to reproduce
        PostgreSQL's MIME line break every 76 characters.
    *   `encode(bytea, 'base64url')` (PostgreSQL 19+) → `base64URLEncode()`.
*   Extended interval arithmetic pushdown to `date` and `timestamp` operands
    and to interval subtraction, beyond the previous `timestamptz + interval`.
    Deparses to ClickHouse `INTERVAL <n> <unit>` terms, one per nonzero month,
    day, and second component, replacing nested
    `addMonths`/`addDays`/`addSeconds` calls that mishandled day arithmetic on
    dates and drifted across DST boundaries ([#301]).
*   Binary driver now flushes insert block once it buffers 64MiB,
    bounding memory for large `COPY FROM` and `INSERT SELECT` ([#303]).
*   Binary driver now supports inserting into `Array(Nullable(T))` columns ([#316]).
*   Added pushdown for the three-argument forms of `ltrim`, `rtrim`, and
    `btrim` ([#307]).
*   Added `clickhouse_query(server, sql)`, a set-returning function that runs a
    query against a configured foreign server and returns its rows typed by the
    caller's column definition list ([#309]).
*   Added `clickhouse_perform(server, sql)`, a procedure that runs a statement
    against a configured foreign server and discards any result, for statements
    such as DDL returning no results ([#329]).
*   Deprecated `clickhouse_raw_query()`, which will be removed in next release.
    Use `clickhouse_query(server, sql)` or `CALL clickhouse_perform(server, sql)`,
    which use a foreign server rather than a connection string ([#329]).
*   Any ClickHouse column now reads into `text` or another string type, rendered
    by the output function of the PostgreSQL type it maps to, rather than
    failing with `could not cast value` ([#329]).
*   Added pushdown support for partial aggregates under partitionwise
    aggregation, so a query over a partitioned table mixing local and foreign
    partitions computes the foreign partition's aggregate on ClickHouse
    instead of fetching its rows. Covers decomposable aggregates (`count`,
    `sum`, `min`, `max`, `bool_and`/`bool_or`, `bit_and`/`bit_or`), plus `avg`
    over integers and `avg`/`var_pop`/`var_samp`/`stddev_pop`/`stddev_samp`
    over floats. Aggregates with an internal transition state (anything over
    `numeric`, `avg(bigint)`, `avg(interval)`) still fall back to local
    aggregation. Requires `enable_partitionwise_aggregate` ([#298]).
*   Disabled remote MIN/MAX optimization by raising `MinMaxAggPath` cost
    rather than understating foreign scan cost ([#310]).
*   Added mapping to push down the re2 v0.4 `@~` operator to ClickHouse as
    the `match()` function ([#318]).
*   Binary driver errors now name the column and type involved, for example
    `cannot encode integer into ClickHouse Array(Array(Int32)) (column "c2")`
    in place of `unexpected PG/CH type pair for column 0` ([#326]).
*   Coerce array elements in binary driver. `Array(Int32)` to `bigint[]`, or
    `quantilesExactLow()` results into `double precision[]`, no longer fails
    with `could not cast value from integer[] to bigint[]` ([#326]).
*   Added support for the third argument to `array_position()` when it's a
    positive constant value, pushing it down to a call to `arraySlice()` to
    start the search from the specified index. Thanks to Minh Vu for the PR
    ([#334])!
*   The HTTP driver now uses ClickHouse's Native format, sharing encoding and
    decoding with binary driver. Only `clickhouse_raw_query()` retains
    `TabSeparated` behavior; it will likely be removed in favor of
    `clickhouse_query() in a future release. ([#328])
*   Deprecated the `fetch_size` setting, now ignored option. With the switch
    go binary encoding, the HTTP driver always streams results the same as the
    binary driver. Setting `fetch_size` triggers a warning and will be removed
    in a future release. ([#328])

### 🐞 Bug Fixes

*   Fixed crashes when a single query ran more than one foreign scan on the
    binary (native-protocol) driver at once, such as a correlated subquery or a
    nested-loop join over foreign tables, colliding in the single connection.
    Each concurrently active scan now gets its own connection
    ([#296]).
*   Fixed a use-after-free of a foreign scan's batch memory context on rescan
    that could corrupt memory and hang ([#296]).
*   Fixed subsecond precision lost inserting timestamps over HTTP ([#300]).
*   Fixed binary driver to use `pg_clickhouse.session_settings` with inserts.
*   Fixed `date_part('dow', ...)` and `EXTRACT(DOW FROM ...)` pushdown to use
    PostgreSQL's Sunday (`0`) through Saturday (`6`) numbering, rather than
    ClickHouse's default Monday through Sunday scheme. Thanks to Minh Vu for
    the PR ([#331])!
*   Fixed undefined behavior: out-of-bounds reads in key/value iteration, and
    binary driver's simple query path; `CollapsingMergeTree` validation; missing
    rejection of non-`Const`/null units in `date_trunc`/`date_part` pushdown;
    NULL handling in record conversion; unchecked `curl_easy_escape` failures;
    and unbounded `DateTime64` scale lookups ([#313]).
*   Fixed wrong results from pushed-down `= ANY`/`<> ALL` expressions and `IN`
    expressions over constant lists and when a `NULL` could reach the
    comparison, because ClickHouse evaluates `IN` under two-valued logic while
    PostgreSQL evaluates three-value `NULL` logic ([#315], [#317]).
*   Fixed `<> ANY(array)` deparsing to ClickHouse SQL that computes `<> ALL`,
    which is wrong even with no `NULL`s involved: In Postgres,
    `1 <> ANY('{1,5}')` is `TRUE`. For now, do not push down ([#315]).
*   Fixed the pushdown of a `CASE arg WHEN ..` expression without checking its
    branches when the tested expression was itself unshippable ([#315]).
*   Fixed an error when incorrectly selecting an invalid relation OID when
    selecting the user to execute the remote query. Thanks to Kostia R for the
    PR ([#319])!
*   Fixed omission of typmod behavior in binary driver causing incorrect values.
    For example, padding will be preserved in char(x) columns now ([#330]).
*   Encode `bytea` array elements with the ClickHouse binary-literal
    serializer previously for scalar values (now simplified), preventing
    PostgreSQL `IN` lists from producing invalid or mismatched `FixedString`
    comparisons. Thanks to @jxom for the PR (#333)!
*   Fixed `array_position()` pushdown to return `NULL`, rather than zero, when
    ClickHouse does not find an element. Thanks to Minh Vu for the PR ([#334])!
*   Fixed `array_length()` pushdown to preserve empty-array and requested
    dimension semantics. Thanks to Minh Vu for the PR ([#336])!
*   Taught `json_extract_path*()` and `jsonb_extract_path*()` not to push down
    when the path array contains any `NULL` elements. Thanks to Minh Vu for
    the PR ([#335])!

### 📚 Documentation

*   Separated the list of custom ordered set aggregate functions provided by
    pg_clickhouse (currently `quantile()` and `quantileExact()`) from the
    Postgres ordered set aggregate functions, putting them under their own
    header, "Custom Ordered Set Aggregates".
*   Fixed the links to the ClickHouse docs for the functions to which `ltrim`
    and `rtrim` push down.

  [v0.4.0]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.3.2...v0.4.0
  [#290]: https://github.com/ClickHouse/pg_clickhouse/pull/290
    "ClickHouse/pg_clickhouse#290 Add pushdown for statistical aggregate functions"
  [corr]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/corr
  [covarpop]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/covarpop
  [covarsamp]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/covarsamp
  [stddev_pop]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/stddevpop
  [stddev_samp/stddev]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/stddevsamp
  [var_op]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/varPop
  [var_samp/variance]: https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/varSamp
  [#291]: https://github.com/ClickHouse/pg_clickhouse/pull/291
    "ClickHouse/pg_clickhouse#291 Push down ordered set aggregate functions"
  [#293]: https://github.com/ClickHouse/pg_clickhouse/pull/293
    "ClickHouse/pg_clickhouse#293 Add ClickHouse server-version detection plumbing"
  [#296]: https://github.com/ClickHouse/pg_clickhouse/pull/296
    "ClickHouse/pg_clickhouse#296 Fix benchmark queries that crash/hang with binary driver"
  [#298]: https://github.com/ClickHouse/pg_clickhouse/pull/298
    "ClickHouse/pg_clickhouse#296 Simple partitioned aggregation"
  [#300]: https://github.com/ClickHouse/pg_clickhouse/pull/300
    "ClickHouse/pg_clickhouse#300 fix(http): handle subsecond precision"
  [#301]: https://github.com/ClickHouse/pg_clickhouse/pull/301
    "ClickHouse/pg_clickhouse#301 improve interval support"
  [#303]: https://github.com/ClickHouse/pg_clickhouse/pull/303
    "ClickHouse/pg_clickhouse#303 Flush buffered data during binary insert"
  [#307]: https://github.com/ClickHouse/pg_clickhouse/pull/307
    "ClickHouse/pg_clickhouse#307 Push down three-argument trim functions"
  [#309]: https://github.com/ClickHouse/pg_clickhouse/pull/309
    "ClickHouse/pg_clickhouse#309 add binary support to clickhouse_raw_query, add clickhouse_query"
  [#310]: https://github.com/ClickHouse/pg_clickhouse/pull/310
    "ClickHouse/pg_clickhouse#310 Disable MinMaxAgg without impacting overall query planner costs"
  [#289]: https://github.com/ClickHouse/pg_clickhouse/pull/289
    "ClickHouse/pg_clickhouse#289 Push down subquery (SubPlan) residue to ClickHouse"
  [#313]: https://github.com/ClickHouse/pg_clickhouse/pull/313
    "ClickHouse/pg_clickhouse#313 Fix misc undefined behavior"
  [#315]: https://github.com/ClickHouse/pg_clickhouse/pull/315
    "ClickHouse/pg_clickhouse#315 Preserve NULL semantics in IN-family pushdown"
  [#316]: https://github.com/ClickHouse/pg_clickhouse/pull/316
    "ClickHouse/pg_clickhouse#316 Support inserting Array(Nullable(T)) via binary driver"
  [#318]: https://github.com/ClickHouse/pg_clickhouse/pull/318
    "ClickHouse/pg_clickhouse#318 Push down the re2 v0.4 `@~` operator"
  [#317]: https://github.com/ClickHouse/pg_clickhouse/pull/317
    "ClickHouse/pg_clickhouse#317 Push down the array IN family unconditionally"
  [#319]: https://github.com/ClickHouse/pg_clickhouse/pull/319
    "ClickHouse/pg_clickhouse#319 Fix foreign scan RTE selection"
  [#326]: https://github.com/ClickHouse/pg_clickhouse/pull/326
    "ClickHouse/pg_clickhouse#326 pg-clickhouse-c"
  [#328]: https://github.com/ClickHouse/pg_clickhouse/pull/328
    "ClickHouse/pg_clickhouse#328use native format for http"
  [#329]: https://github.com/ClickHouse/pg_clickhouse/pull/329
    "ClickHouse/pg_clickhouse#329 Deprecate clickhouse_raw_query, add clickhouse_perform"
  [#330]: https://github.com/ClickHouse/pg_clickhouse/pull/330
    "ClickHouse/pg_clickhouse#330 preserve typmod in binary driver"
  [#331]: https://github.com/ClickHouse/pg_clickhouse/pull/331
    "ClickHouse/pg_clickhouse#331 Preserve PostgreSQL DOW semantics in pushdown"
  [#333]: https://github.com/ClickHouse/pg_clickhouse/pull/333
    "ClickHouse/pg_clickhouse#333 fix: encode bytea array literals"
  [#334]: https://github.com/ClickHouse/pg_clickhouse/pull/334
    "ClickHouse/pg_clickhouse#334 Preserve array_position not-found semantics"
  [#335]: https://github.com/ClickHouse/pg_clickhouse/pull/335
    "ClickHouse/pg_clickhouse#335 Reject JSON paths containing NULL elements"
  [#336]: https://github.com/ClickHouse/pg_clickhouse/pull/336
    "ClickHouse/pg_clickhouse#336 Preserve array_length semantics"

## [v0.3.2] — 2026-06-16

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.3 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added a `compression` server option for the binary driver to enable
    ClickHouse native protocol compression of query results and `INSERT` data.
    Accepts `none`, `lz4`, or `zstd`, and defaults to `lz4` ([#268]).
*   Added a `secure` server option giving explicit control over TLS for
    both the binary and HTTP drivers, rather than inferring it from the host
    name and port. Accepts `on` (force TLS), `off` (force plaintext), or `auto`
    (the previous cloud-host/port heuristic, still the default). Thanks to
    Andrey Borodin for the PR ([#272]).
*   Added a `min_tls_version` server option to set the minimum TLS protocol
    version negotiated by both drivers. Accepts `TLSv1`, `TLSv1.1`, `TLSv1.2`,
    or `TLSv1.3`, and defaults to the TLS library's own minimum ([#272]).
*   Added mapping for `regexp_match()` to pushdown to `extractGroups()`, or
    `arraySlice(extractAll(text, pattern), 1, 1)` if the regex contains no
    capturing groups ([#270]).

### 🚀 Distribution

*   Added support for PostgreSQL 19beta1 ([#269]).

### 🐞 Bug Fixes

*   Fixed incorrect translation of regular expression flags introduced in
    [v0.2.0](#v020--2026-04-13) to more accurately match the Postgres behavior
    when executing in ClickHouse. We no longer automatically push down `-s`,
    because it is enabled by default in both Postgres and ClickHouse. But the
    Postgres flags `s` and `m` cancel each other out, so we only set `s` if
    there is no `m` and if there is an `m` we set `(?m-s)`.
*   Changed the pushdown of the `p` regular expression fag as `-s` instead of
    `s` to more accurately match the Postgres behavior ([#271]).
*   Eliminated push down of regular expression functions when the regular
    expression argument is not a constant ([#270]).
*   Fixed a memory leak in the http driver when not using streaming ([#281]).
*   Fixed a memory leak when a foreign scan repeatedly re-scans, typically a
    nested-loop join with a parameterized inner foreign scan ([#282]).
*   Changed the deparsing of `ANY()` with an empty array (`WHERE x = ANY('{}')`)
    to a `has()` function rather than an `IN()` expression. This fixes errors
    on versions prior to ClickHouse 25, where `IN()` with no list returns an
    error ([#285])

### 🏗️ Build Setup

*   Replaced code formatting, previously performed by `pg_bsd_indent`, with
    [clang-format] using a modified "Mozilla style" with an emphasis on
    legibility and safety ([#283]).

  [v0.3.2]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.3.1...v0.3.2
  [#227]: https://github.com/ClickHouse/pg_clickhouse/pull/227
    "ClickHouse/pg_clickhouse#227 add three-state secure option for TLS control"
  [#268]: https://github.com/ClickHouse/pg_clickhouse/pull/268
    "ClickHouse/pg_clickhouse#268 add compression option for binary protocol"
  [#269]: https://github.com/ClickHouse/pg_clickhouse/pull/269
    "ClickHouse/pg_clickhouse#269 Add support for PostgreSQL 19beta1"
  [#270]: https://github.com/ClickHouse/pg_clickhouse/pull/270
    "ClickHouse/pg_clickhouse#270 Improve Regex flag mapping & support regexp_match"
  [#271]: https://github.com/ClickHouse/pg_clickhouse/pull/271
    "ClickHouse/pg_clickhouse#271 Parse regex flags separate from emitting them"
  [#272]: https://github.com/ClickHouse/pg_clickhouse/pull/272
    "ClickHouse/pg_clickhouse#272 feat: add three-state secure option (on/off/auto) for TLS control"
  [#281]: https://github.com/ClickHouse/pg_clickhouse/pull/281
    "ClickHouse/pg_clickhouse#281 Properly free Curl memory on palloc error"
  [#282]: https://github.com/ClickHouse/pg_clickhouse/pull/282
    "ClickHouse/pg_clickhouse#282 Free batch context on re-scan"
  [clang-format]: https://clang.llvm.org/docs/ClangFormat.html
  [#283]: https://github.com/ClickHouse/pg_clickhouse/pull/283
    "ClickHouse/pg_clickhouse#283 clang-format"
  [#285]: https://github.com/ClickHouse/pg_clickhouse/pull/285
    "ClickHouse/pg_clickhouse#285 Don't use `IN` for empty array (`ANY('{}')`)"

## [v0.3.1] — 2026-05-02

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.3 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Replaced the `clickhouse-cpp` binary client with [ClickHouse/clickhouse-c],
    pulled in as a git submodule and included in the release package under
    `vendor/clickhouse-c`. This change eliminates conflicts between the C++
    and Postgres memory & exception handling and streams query results by the
    ClickHouse block for reduced memory consumption. It also greatly reduces
    build time and the size of the library by over 75%. Thanks to serprex for
    the the new library and the PR ([#254]).
*   Added multidimensional array support across `SELECT` and `INSERT` to both
    the binary and http drivers. Rectangular ClickHouse `Array(Array(...))`
    values now map to PostgreSQL multidimensional arrays (jagged arrays not
    supported) and PostgreSQL multidimensional arrays inserted into ClickHouse
    `Array(Array(...))` columns preserve their nesting. Thanks to serprex for
    the PR ([#233]).
*   Added pushdown for [re2 extension] functions introduced in pg_re2 v0.3.0:
    `re2extractallgroupshorizontal`, `re2extractallgroupsvertical`,
    `re2regexpquotemeta`, and `re2splitbyregexp`. Thanks to serprex for the PR
    ([#232]).
*   Add pushdown for the Postgres regular expression flag `w` as `m` in
    ClickHouse.

### 🐞 Bug Fixes

*   Fixed incorrect casting of ClickHouse `UInt16` values to `int16` in the
    Binary driver. They now correctly convert to `int32` (Postgres `INT4`).
    Part of the omnibus binary c driver conversion contributed by serprex
    ([#233]).

  [v0.3.1]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.3.0...v0.3.1
  [#232]: https://github.com/ClickHouse/pg_clickhouse/pull/232
    "ClickHouse/pg_clickhouse#232 pushdown for new functions in pg_re2 0.3"
  [#233]: https://github.com/ClickHouse/pg_clickhouse/pull/233
    "ClickHouse/pg_clickhouse#233 Support multidimensional arrays"
  [ClickHouse/clickhouse-c]: https://github.com/ClickHouse/clickhouse-c
  [#254]: https://github.com/ClickHouse/pg_clickhouse/pull/254
    "ClickHouse/pg_clickhouse#254 Replace clickhouse-cpp with clickhouse-c"

## [v0.3.0] — 2026-05-11

This release makes binary-compatible changes to the v0.2 releases. Once
installed, any existing use of pg_clickhouse v0.2 will benefit from its
improvements on reload. The only change that requires an upgrade revokes
`EXECUTE` from `clickhouse_raw_query()`. We recommend running this command
make this security-sensitive change:

```sql
ALTER EXTENSION pg_clickhouse UPDATE TO '0.3';
```

### ⚡ Improvements

*   Added pushdown for [re2 extension] functions, if available, to their
    ClickHouse equivalents (e.g., `re2match` → `match`, `re2extractall` →
    `extractAll`). Thanks to serprex for the PR ([#204]).
*   Added pushdown for [fuzzystrmatch] functions `soundex()` and
    `levenshtein()` (2-arg, mapped to `editDistanceUTF8`). Thanks to serprex
    for the PR ([#210]).
*   Added mapping for `JSON` => `jsonb` to the binary driver (requires
    ClickHouse 24.10 or later).
*   Added support for ClickHouse `JSON` mapped to Postgres `json`, supporting
    all the same operators and functions as the `jsonb` mapping.
*   Added pushdown for `to_char(timestamp[tz], fmt)` to ClickHouse
    `formatDateTime()`, with strict format-string validation.  Only pushes
    down when the format is a constant whose every keyword has an identical CH
    equivalent (`YYYY`, `MM`, `DD`, `DDD`, `HH24`, `HH12`, `HH`, `MI`, `SS`,
    `Q`, `Mon`, `Dy`, `AM`/`PM`, plus lowercase variants). Thanks to serprex
    for the PR ([#244]).
*   Made builtin function pushdown opt-in: Postgres builtins now ship to
    ClickHouse only when explicitly mapped, so name or signature differences
    cannot silently alter results. Thanks to serprex for the PR ([#245]).
*   Added explicit mappings for `mod`, `pow`/`power`, `bit_count(bytea)`, and
    `reverse(text)` (→ `reverseUTF8`) to retain previously working pushdowns.
    Thanks to serprex for the PR ([#245]).

### 🐞 Bug Fixes

*   Fixed `EXPLAIN (VERBOSE)` failing with "could not find window clause for
    winref N" when window functions push down to ClickHouse. Thanks to serprex
    for the PR ([#223]).
*   Fixed the parsing of strings that start with `[` in the http driver so
    that it no longer assumes it's the start of an array. Thanks to Kaushik
    Iska for the PR ([#234]).
*   Fixed the parsing of strings in the http driver to distinguish a true
    `NULL` value from a string containing `\N`. Thanks to Kaushik
    Iska for the PR ([#235]).
*   Fixed the `column_name` foreign-table column option being ignored by
    `INSERT`, which caused the binary engine to fail to match ClickHouse block
    columns and the HTTP engine to deparse PostgreSQL attribute names. Thanks
    to serprex for the PR ([#231]).
*   Fixed `length(text)` and `strpos(text, text)` pushdown to map to
    `lengthUTF8` and `positionUTF8` rather than ClickHouse's byte-counting
    `length` and `position`, matching Postgres character semantics. Thanks to
    serprex for the PR ([#245]).
*   Stopped pushing down `asin`, `acos`, `atanh`, and `acosh`: Postgres raises
    an error on out-of-range input where ClickHouse returns `NaN`. Evaluating
    locally preserves Postgres semantics. Thanks to serprex for the PR
    ([#245]).

### 📚 Documentation

*   Added "Extension Pushdown" section to the [reference
    docs](doc/pg_clickhouse.md), covering re2, intarray, and fuzzystrmatch
    support.
*   Added recommendation to the [reference docs](doc/pg_clickhouse.md) to
    consider using the [re2 extension] and disabling Postgres regular
    expression pushdown.
*   Documented the `column_name` foreign table column option in the [reference
    docs](doc/pg_clickhouse.md).
*   Added `jsonb_extract_path_text()` and `jsonb_extract_path()` to the list
    of push down functions in the [reference docs](doc/pg_clickhouse.md),
    along with `json_extract_path_text()` and `json_extract_path()`, which are
    new in this release.
*   Fixed the reversed descriptions of `->>` and `->` in the list of pushed
    down operators in the [reference docs](doc/pg_clickhouse.md).

### 🚀 Distribution

*   Added the [ca-certificates package] and the [re2 extension] to the OCI
    (née Docker) images.

### 🚨 Security Fixes

*   Added SQL to revoke `EXECUTE` permission on `clickhouse_raw_query()` from
    `PUBLIC`. Leaving it executable by `PUBLIC` would allow any database user
    to reach internal services (metadata endpoints, private APIs, etc.) from
    the PostgreSQL server — a classic [SSRF] vector. This ensures that admins
    can limit access only to those who legitimately need to execute ad-hoc
    ClickHouse queries (e.g., a dedicated ClickHouse admin role). Thanks to
    Andrey Borodin for the PR ([#228]).

  [v0.3.0]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.2.0...v0.3.0
  [ca-certificates package]: https://packages.debian.org/source/trixie/ca-certificates
    "Debian Packages: Common CA certificates"
  [re2 extension]: https://github.com/ClickHouse/pg_re2
    "ClickHouse/pg_re2: ClickHouse-compatible regex functions using RE2"
  [#204]: https://github.com/ClickHouse/pg_clickhouse/pull/204
    "ClickHouse/pg_clickhouse#204 Support pushdown for re2 extension"
  [fuzzystrmatch]: https://www.postgresql.org/docs/current/fuzzystrmatch.html
    "PostgreSQL Docs: fuzzystrmatch"
  [#210]: https://github.com/ClickHouse/pg_clickhouse/pull/210
    "ClickHouse/pg_clickhouse#210 Support pushing down soundex & levenshtein from fuzzystrmatch"
  [#223]: https://github.com/ClickHouse/pg_clickhouse/pull/223
    "ClickHouse/pg_clickhouse#223 Fix EXPLAIN (VERBOSE) for window functions"
  [#234]: https://github.com/ClickHouse/pg_clickhouse/pull/234
    "ClickHouse/pg_clickhouse#234 Make TSV parser column-type-aware for bracket-leading strings"
  [#235]: https://github.com/ClickHouse/pg_clickhouse/pull/235
    "ClickHouse/pg_clickhouse#235 Detect TSV NULL marker before unescaping"
  [#231]: https://github.com/ClickHouse/pg_clickhouse/pull/231
    "ClickHouse/pg_clickhouse#231 Fix column_name option not being respected by inserts"
  [#244]: https://github.com/ClickHouse/pg_clickhouse/pull/244
    "ClickHouse/pg_clickhouse#244 Push down compatible to_char"
  [#223]: https://github.com/ClickHouse/pg_clickhouse/pull/223
    "pg_clickhouse#223 Fix EXPLAIN (VERBOSE) for window functions"
  [SSRF]: https://en.wikipedia.org/wiki/Server-side_request_forgery
    "Wikipedia: Server-side request forgery"
  [#228]: https://github.com/ClickHouse/pg_clickhouse/pull/228
    "pg_clickhouse#228 Security: revoke PUBLIC execute on clickhouse_raw_query"
  [#245]: https://github.com/ClickHouse/pg_clickhouse/pull/245
    "ClickHouse/pg_clickhouse#245 Don't push down functions by default"

## [v0.2.0] — 2026-04-13

This release makes binary-compatible changes to the v0.1 releases. Once
installed, any existing use of pg_clickhouse v0.1 will benefit from its
improvements on reload. The only new feature that requires an upgrade is the
`pgch_version()` function. Run this command to add it to the extension:

```sql
ALTER EXTENSION pg_clickhouse UPDATE TO '0.2';
```

### ⚡ Improvements

*   Changed the pushdown mappings for the current date and timestamp functions
    to account for the session time zone and Postgres-standard millisecond
    precision, as follows:
    *   `CURRENT_DATE` -> `toDate(now(TZ))`
    *   `CURRENT_TIMESTAMP` and `LOCALTIMESTAMP` => `now64(9, TZ)`
    *   `CURRENT_TIMESTAMP(n)` and `LOCALTIMESTAMP(n)` => `now64(n, TZ)`
    *   `clock_timestamp()`, `statement_timestamp()`, &
        `transaction_timestamp()` => `nowInBlock64(n, TZ)`
*   Added pushdown for the `CURRENT_TIME` and `LOCALTIME` SQL Value Functions
    to `toTime64(now64(6, TZ), 6)`, supported by ClickHouse 25.8+.
*   Added `pgch_version()`, which returns the full semantic version.
    This is the same value visible in `pg_get_loaded_modules()`, but available
    in Postgres versions prior to 18, and without having to load
    pg_clickhouse in advance.
*   Added support for pushing down the flags passed to `regexp_like()` by
    prepending them to the regular expression (e.g., `(?i)foo`). If any of the
    flags cannot be pushed down, the regular expression function will not be
    pushed own.
*   Added pushdown for `regexp_split_to_array()` to `splitByRegexp()`,
    including pushdown of applicable flags.
*   Added pushdown mappings for array functions: `array_cat`, `array_append`,
    `array_remove`, `array_to_string`, `cardinality`, `array_length`,
    `array_prepend`, `string_to_array`, `trim_array`, `array_fill`,
    `array_reverse`, `array_shuffle`, `array_sample`, `array_sort`.
*   Added mapping for `split_part()` to pushdown `splitByString()[n]`.
*   Added pushdown for array operators: `@>` (`hasAll`), `<@` (`hasAll`),
    `&&` (`hasAny`).
*   Array slice syntax (`arr[L:U]`, `arr[:U]`, `arr[L:]`) now pushes down
    as `arraySlice()`.
*   Added mapping for `regexp_replace(4-arg)` to pushdown to
    `replaceRegexpAll()` when the `g` flag is set, and to prepend compatible
    flags to the pushed down expression, or not to push down if any are not
    compatible.
*   All regular expression functions with compatible flags and all regular
    expression operators now push down prepended with `(?-s)` unless the `s`
    flag is set, so that the behavior more closely approximates that of
    Postgres.
*   Added the `pg_clickhouse.pushdown_regex` setting to prevent regular
    expressions from being pushed down.

### ⬆️ Dependency Updates

*   Updated vendored clickhouse-cpp library to v2.6.1.

### 🐞 Bug Fixes

*   Fixed a malformed type name in the error message when the http driver is
    unable to map a ClickHouse type to a Postgres type.
*   Fixed reversal of the arguments passed to the ClickHouse `match()`
    function by the mapping from `regexp_like()`.
*   `array_dims`, `array_ndims`, `array_lower`, `array_upper`, `array_replace`,
    `array_positions`, `array_fill (3-arg)`, `array_sort (3-arg)`, and
    `string_to_array(3-arg)` now evaluate locally instead of being pushed to
    ClickHouse where they would fail.
*   Changed pushdown for `regexp_replace(3-arg)` from `replaceRegexpAll()` to
    `replaceRegexpOne()`.

### 📔 Notes

*   Added tests to ensure that `concat_ws()` successfully pushes down to the
    compatible function of the same name (an alias for [concatWithSeparator]).

  [v0.2.0]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.10...v0.2.0
  [concatWithSeparator]: https://clickhouse.com/docs/sql-reference/functions/string-functions#concatWithSeparator


## [v0.1.10] — 2026-04-06

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added mapping for the `JSON` and `JSONB` `-> TEXT` and `->> TEXT`
    operators to be passed down to ClickHouse using its [sub-column syntax].
    Thanks Kaushik Iska for the PR ([#169]).
*   Added pushdown support for `jsonb_extract_path_text()` and
    `jsonb_extract_path()` to ClickHouse [sub-column syntax]. Thanks Kaushik
    Iska for the PR ([#176]).
*   Added mapping to push down `now()` to [now64] rather than [now], as
    previously, because PostgreSQL's `now()` produces sub-second precision, so
    should its clickHouse equivalent.
*   Added mappings to push down the Postgres `statement_timestamp()`,
    `transaction_timestamp()`, and `clock_timestamp()` functions to to
    [nowInBlock64][] (requires ClickHouse 25.8 or higher).
*   Pushdown window functions (`ROW_NUMBER`, `RANK`, `DENSE_RANK`, `LEAD`,
    `LAG`, `FIRST_VALUE`, `LAST_VALUE`, `NTH_VALUE`, `NTILE`, `CUME_DIST`,
    `PERCENT_RANK`, `MIN`/`MAX OVER`) to ClickHouse instead of computing
    them locally. Thanks Kaushik Iska for the PR ([#175]).
*   Pushdown `bool_and`/`every` as `groupBitAnd`, `bool_or` as `groupBitOr`,
    and `string_agg` as `groupConcat` to ClickHouse. Thanks serprex for the PR
    ([#184]).
*   Added mapping to push down the Postgres "SQL Value Functions", including
    `CURRENT_TIMESTAMP`, `CURRENT_USER`, and `CURRENT_DATABASE`.
*   Changed the behavior of `CURRENT_DATABASE()` to push down the name of the
    current Postgres database rather than to the ClickHouse
    `current_database()` function.
*   Added result set streaming to the HTTP driver. The new `fetch_size` server
    and table option specifies the size of each batch to stream and defaults
    to `50000000`, about 50MB. Set it to `0` to disable streaming altogether.
    A testing loading a 1GB table reduced memory consumption from over 1GB to
    73MB peak. Thanks Kaushik Iska for the testing and PR ([#181]).

### 🐛 Bug Fixes

*   Improved memory management, fixing potential crashes in out of memory
    situations. Thanks to serprex for the PRs ([#173], [#173]).
*   Fixed issue where the `-Merge` suffix was not consistently appended to
    aggregates on `AggregateFunction` columns. Thanks to serprex for the PR
    ([#179]).
*   Fixed `NTILE`, `CUME_DIST`, and `PERCENT_RANK` pushdown failing because
    the FDW emitted a `ROWS UNBOUNDED PRECEDING` frame clause that ClickHouse
    rejects for ranking functions. Thanks serprex for the PR ([#184]).
*   `regr_avgx`, `regr_avgy`, `regr_count`, `regr_intercept`, `regr_r2`,
    `regr_slope`, `regr_sxx`, `regr_sxy`, `regr_syy`, `json_agg_strict`, and
    `jsonb_agg_strict` now evaluate locally instead of being pushed to
    ClickHouse where they would fail. Thanks serprex for the PR ([#184]).

### 📔 Notes

*   Eliminated use of a constant that required libcurl 7.87.0, restoring
    support for earlier versions.
*   Introduced clang-tidy and integrated it into `make lint` and for use in
    CI. Thanks to serprex for the PR ([#177]).

  [v0.1.10]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.6...v0.1.10
  [sub-column syntax]: https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns
    "ClickHouse Docs: Reading JSON paths as sub-columns"
  [#169]: https://github.com/ClickHouse/pg_clickhouse/pull/169
    "pg_clickhouse#169 Push down jsonb -> and ->> operators as ClickHouse dot notation"
  [#176]: https://github.com/ClickHouse/pg_clickhouse/pull/176
    "pg_clickhouse#176 Pushdown jsonb_extract_path_text and jsonb_extract_path"
  [#173]: https://github.com/ClickHouse/pg_clickhouse/pull/173
    "pg_clickhouse#173 ch_http_simple_query: return NULL on OOM"
  [#178]: https://github.com/ClickHouse/pg_clickhouse/pull/178
    "pg_clickhouse#178 pglink.c: handle OOM preventing error being set"
  [#175]: https://github.com/ClickHouse/pg_clickhouse/pull/175
    "pg_clickhouse#175 Pushdown window functions"
  [#179]: https://github.com/ClickHouse/pg_clickhouse/pull/179
    "pg_clickhouse#179 Fix appending Merge suffix incorrectly"
  [now64]: https://clickhouse.com/docs/sql-reference/functions/date-time-functions#now64
  [now]: https://clickhouse.com/docs/sql-reference/functions/date-time-functions#now
  [nowInBlock64]: https://clickhouse.com/docs/sql-reference/functions/date-time-functions#nowInBlock64
  [#177]: https://github.com/ClickHouse/pg_clickhouse/pull/177
    "pg_clickhouse#177 clang-tidy static analysis"
  [#181]: https://github.com/ClickHouse/pg_clickhouse/pull/181
    "pg_clickhouse#181 HTTP streaming"
  [#184]: https://github.com/ClickHouse/pg_clickhouse/pull/184
    "pg_clickhouse#184 More function support fixes"

## [v0.1.6] — 2026-04-02

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added support for the PostgreSQL `to_timestamp(float8)` function, mapped to
    `fromUnixTimestamp(toInt64())` in ClickHouse.

### 🪲 Bug Fixes

*   Disabled query pushdown for JSONB subscript syntax (e.g.,
    `col_name['field']`) for now. Thanks Kaushik Iska for the PR (#161).
*   Added query cancellation via Ctrl-C and `statement_timeout` to the binary
    driver. Thanks Kaushik Iska for the PR (#162) that fixed this issue (#41).
*   Fixed `LIKE`, `ILIKE`, and regex operator pushdown, including `~~*`,
    `!~~*`, `~`, `!~`, `~*`, `!~*`. Thanks Kaushik Iska for the PR (#164).

  [v0.1.6]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.5...v0.1.6

## [v0.1.5] — 2026-03-20

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### 🚨 Security Fixes

*   Changed the http driver connection function to raise an error if the
    database name contains line ending characters to prevent HTTP header
    injection.
*   Fixed an SQL injection vulnerability in parsing of parameters to the
    `engine 'CollapsingMergeTree($sign)` option to `CREATE FOREIGN TABLE`.

### 🪲 Bug Fixes

*   Fixed a crash due to an unexpected EOF while the http driver parses a
    response. Thanks to @serprex for the fix (#153)
*   Fixed a crash due to an unchecked memory allocation while the http driver
    reads a response. Thanks to @serprex for the fix (#154).

  [v0.1.5]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.4...v0.1.5

## [v0.1.4] — 2026-02-17

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added support for the PostgreSQL `md5()` function, mapped to
    `lower(hex(MD5()))` in ClickHouse.
*   Added support for mapping PostgreSQL BYTEA columns to ClickHouse String
    columns.
*   Added explicit setting of `format_tsv_null_representation` and
    `output_format_tsv_crlf_end_of_line` to all http requests, as unexpected
    values will interfere with its operation.
*   Improved the error message from the binary driver when attempting to
    insert a `NULL` into a column that is not `Nullable(T)`.

### 🪲 Bug Fixes

*   Fixed binary driver errors when attempting to insert a `NULL` value into
    `Nullable` Numeric, Text, `Enum`, `UUID`, and `INET` columns. Thanks to
    Rahul Mehta for the report (#140).
*   Fixed http driver array parsing, which previously did not properly parse
    string values and would raise an error on values containing brackets
    (`[]`). Thanks to serprex for the spot (#142).
*   Fixed a bug where the binary driver would raise an error on an empty
    array.

### 📔 Notes

*   Refactored and improved the http engine's result processing, bringing it
    into closer alignment with the binary engine and removing double
    processing of row values.
*   The http driver now ignores the following session settings from the
    `pg_clickhouse.session_settings` to prevent them from interfering with its
    operation: `date_time_output_format`, `format_tsv_null_representation`,
    and `output_format_tsv_crlf_end_of_line`.

  [v0.1.4]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.3...v0.1.4

## [v0.1.3] — 2026-01-23

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Changed the default mapping for `DateTime` and `DateTime64` values from
    `TIMESTAMP` to `TIMESTAMPTZ`, because ClickHouse stores `DateTime`s as a
    Unix timestamp, always normalized to UTC, even if it displays as a
    different time zone. As of the first bug fix listed below, pg_clickhouse
    (almost) always fetches these values in UTC, so can store them as
    `TIMESTAMPTZ` values.
*   Implemented `INSERT` support for the UUID and INET (IPv4 and IPv6) types.
    Thanks to Rahul Mehta for the report ([#127])!

### 🪲 Bug Fixes

*   Fixed time zone conversion in the http engine. Does not work with
    parameterized execution on ClickHouse versions prior to 25.8 due to
    [ClickHouse Issue 88088]; recommend using the binary engine for tables
    with timestamp values on earlier ClickHouse versions to avoid the issue.
*   Fixed a server crash when attempting to insert types not yet supported by
    the binary engine.

### 🚀 Distribution

*   Added the [security policy](SECURITY.md).

### 📔 Notes

*   [Scripted] the generation of the TPC-H results table and updated it in the
    [README](README.md).
*   Cleaned up some comments and old references to postgres_fdw left from the
    original fork in 2019.
*   Added tests demonstrating subqueries that pg_clickhouse does not yet push
    down, to be improved in future releases.

### 🏗️ Build Setup

*   Added pre-commit hooks to lint the code, including for indentation
    enforced by `pg_bsd_indent`. A new workflow ensures consistency for these
    quality checks. Relatedly, a number of issues found by the linters have
    been corrected.
*   Configured `make installcheck` to run the tests in parallel, resulting in
    far faster test execution on multi-core systems. Adjusted the schemas in
    which some of the tests work to ensure they don't stomp on each other.

  [v0.1.3]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.2...v0.1.3
  [ClickHouse Issue 88088]: https://github.com/ClickHouse/ClickHouse/pull/88088
  [#127]: https://github.com/ClickHouse/pg_clickhouse/issues/127
    "ClickHouse/pg_clickhouse#127 INSERT into ClickHouse table with UUID column fails: unexpected column type for 2950: UUID"
  [Scripted]: https://github.com/ClickHouse/pg_clickhouse/tree/main/dev/tpch
    "pg_clickhouse: TPC-H Benchmark"

## [v0.1.2] — 2026-01-07

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added support for parameterized execution, including `PREPARE` and
    `EXECUTE`, by converting PostgreSQL `$1`-style parameters to ClickHouse
    `{param:type}`-style parameters.
*   Added support for inserting arrays to the http engine.

### 🪲 Bug Fixes

*   Fixed the http engine's parsing of UUID arrays selected from ClickHouse.
*   Fixed the binary engine's conversion of Date values, which in arrays ended
    up too large by several orders of magnitude (e.g., `2025-12-05` would be
    converted to `10529827-09-17` 😱). Thanks to Tom Lane for the pointer to
    the proper function to easily convert epoch seconds to a date.
*   Fixed a binary engine bug where dates and timestamps for epoch 0
    (`1970-01-01 00:00:00`) rendered as `NULL`.
*   Added support for the `Date32` ClickHouse type.
*   Fixed conversion of `array_agg()` to support ClickHouse versions prior to
    23.8.
*   Fixed the precision of fractional seconds in the binary engine's
    conversion of ClickHouse `DateTime64` values to Postgres `TIMESTAMP`
    (#114).

### 📔 Notes

*   Removed unused code designed to support custom PostgreSQL extensions:
    [ajbool], ajtime, [country], and [istore].
*   Tweaked cost estimation to encourage pushdown of `min()` and `max()`.

### 📚 Documentation

*   Documented `IMPORT FOREIGN SCHEMA` identifier case preservation behavior.
*   Fixed the Postgres Docker start and connect info in the
    [tutorial](doc/tutorial.md).
*   Fixed the commands to start and connect to the pg_clickhouse Docker image
    in the [tutorial].
*   Added complete DML documentation to the [reference
    docs](doc/pg_clickhouse.md), including the new `PREPARE`/`EXECUTE` support
    and `INSERT`, `SET`, `COPY`, as well as shared library preloading.
*   Documented the Postgres aggregate functions known (via new tests) to push
    down to ClickHouse.

  [v0.1.2]: https://github.com/clickhouse/pg_clickhouse/compare/v0.1.1...v0.1.2
  [ajbool]: https://pgxn.org/dist/ajbool/ "ajbool on PGXN"
  [country]: https://pgxn.org/dist/country/ "country on PGXN"
  [istore]: https://pgxn.org/dist/istore/ "istore on PGXN"
  [tutorial]: ./doc/tutorial.md

## [v0.1.1] — 2025-12-17

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Refactored the internal handling of the `pg_clickhouse.session_settings`
    GUC to parse the settings only once rather than for every query sent to
    ClickHouse

### 🚨 Security Fixes

*   Updated the SQL query rewriting to properly quote strings and identifiers
    in SQL queries sent to ClickHouse, fixing potential SQL injection
    vulnerabilities, notably via `IMPORT SCHEMA`. This has the effect of
    preserving mixed-case and uppercase identifiers.

### 🪲 Bug Fixes

*   Fixed a crash when sending an empty `COPY FROM` via the binary driver

### ⬆️ Dependency Updates

*   Updated vendored clickhouse-cpp library

### 🏗️ Build Setup

*   Fixed the `Makefile` targets so that calling `make install` without first
    calling `make` will properly create the versioned SQL file.

### 📚 Documentation

*   Added a [versioning policy] the documentation
*   Fixed the badges and broken TPC-H links in [README.md]
*   Added PGXN installation instructions to [README.md]

  [v0.1.1]: https://github.com/clickhouse/pg_clickhouse/compare/v0.1.0...v0.1.1
  [versioning policy]: ./doc/pg_clickhouse.md#versioning-policy
  [README.md]: README.md

## [v0.1.0] — 2025-12-09

### ⚡ Improvements

*   Improved function and aggregate pushdown
*   Added TLS support to both the http and binary engines
*   Added pushdown aggregate functions:
    *   `uniq()`
    *   `uniqExact()`
    *   `uniqCombined()`
    *   `uniqCombined64()`
    *   `uniqExact()`
    *   `uniqHLL12()`
    *   `uniqTheta()`
*   Added pushdown functions:
    *   `toUInt8()`
    *   `toUInt16()`
    *   `toUInt32()`
    *   `toUInt64()`
    *   `toUInt128()`
    *   `quantile()`
    *   `quantileExact()`
*   Mapped PostgreSQL `regexp_like()` to push down to ClickHouse `match()`
    function
*   Mapped PostgreSQL `extract()` to push down to equivalent ClickHouse
    DateTime extraction functions (already mapped to `date_part()`)
*   Mapped PostgreSQL `percentile_cont()` ordered set aggregate function to
    push down to the ClickHouse `quantile()` parametric aggregate function
*   Mapped the `COUNT()` return value to `bigint`
*   Added the query text and, for the http engine, the status code to error
    messages
*   Added `pg_clickhouse.session_settings` GUC, defaulting to
    `join_use_nulls 1, group_by_use_nulls 1, final 1`
*   Added mappings and support for additional data types:
    *   `Bool` => `boolean`
    *   `Decimal` => `numeric`
    *   `JSON` => `jsonb` (http engine only)
*   Added the `dbname` option to http engine connections, including
    `clickhouse_raw_query()`
*   Added LEFT SEMI JOIN pushdown for `EXISTS()` subqueries

### 🏗️ Build Setup

*   Ported from clickhouse_fdw
*   Made lots of general code cleanup
*   Added PGXS build pipeline
*   Added PGXN and GitHub release workflows
*   Added `pg_clickhouse` OCI image workflow that publishes to
    ghcr.io/clickhouse/pg_clickhouse, with tags for PostgreSQL versions 13-18

### 📚 Documentation

*   Added tutorial in [doc/tutorial.md](doc/tutorial.md)
*   Added reference documentation in [doc/pg_clickhouse.md](doc/pg_clickhouse.md)

### 🪲 Bug Fixes

*   Fixed `dictGet()`, `argMin()`, and `argMax()`
*   Fixed bug in filtered `COUNT()`
*   Fixed `AggregateFunction` option to propagate to a nested aggregate
    function call
*   Improved unsigned integer support

### ⬆️ Dependency Updates

*   Updated vendored clickhouse-cpp library
*   Reimplemented binary engine inserts with clickhouse-cpp improvements
*   Support and tested against PostgreSQL 13-18
*   Support and tested against ClickHouse 23-25

### 📔 Notes

*   Set full version in `PG_MODULE_MAGIC_EXT`
*   Set to default ports to TLS for ClickHouse Cloud host names

  [v0.1.0]: https://github.com/clickhouse/pg_clickhouse/compare/a1487bd...v0.1.0
