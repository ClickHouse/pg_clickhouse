/*
 * binary.cpp
 *
 * The only C++ TU in pg_clickhouse. Wraps clickhouse-cpp and exposes a pure-C
 * ABI to the rest of the extension. Contains no PostgreSQL headers, no
 * Datum/Oid types, no palloc and no elog. PG-bound logic for the binary
 * driver lives in binary_decode.c (SELECT) and binary_encode.c (INSERT).
 */
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "clickhouse/columns/array.h"
#include "clickhouse/columns/date.h"
#include "clickhouse/columns/decimal.h"
#include "clickhouse/columns/enum.h"
#include "clickhouse/columns/factory.h"
#include "clickhouse/columns/ip4.h"
#include "clickhouse/columns/ip6.h"
#include "clickhouse/columns/lowcardinality.h"
#include "clickhouse/columns/nullable.h"
#include "clickhouse/columns/numeric.h"
#include "clickhouse/columns/string.h"
#include "clickhouse/columns/tuple.h"
#include "clickhouse/columns/uuid.h"
#include <clickhouse/client.h>
#include <clickhouse/query.h>
#include <clickhouse/types/types.h>

extern "C"
{
#include "binary.hh"
#include "ch_block.h"
#include "internal.h"
#include "kv_list.h"
}

using namespace clickhouse;

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#include <machine/endian.h>
#define HOST_TO_BIG_ENDIAN_64(x) OSSwapHostToBigInt64(x)
#define BIG_ENDIAN_64_TO_HOST(x) OSSwapBigToHostInt64(x)
#else
#include <endian.h>
#define HOST_TO_BIG_ENDIAN_64(x) htobe64(x)
#define BIG_ENDIAN_64_TO_HOST(x) be64toh(x)
#endif

#define CLICKHOUSE_SECURE_PORT 9440

#define UNEXPECTED_COLUMN(exp_type, col)                                                                               \
    std::runtime_error("unexpected column type for " + std::string(exp_type) + ": " + (col)->Type()->GetName())

/*
 * Caller-supplied error buffer. Never allocates — PG runs with overcommit
 * disabled, so a strdup(...) returning NULL silently dropped the message.
 */
static void
set_errbuf(char *errbuf, size_t errbuf_size, const char *msg)
{
    if (!errbuf || errbuf_size == 0)
        return;
    std::snprintf(errbuf, errbuf_size, "%s", msg ? msg : "");
}

/* ---- Arena ---------------------------------------------------------- */

/*
 * Owns every malloc'd buffer, ch_type tree node and ch_block_column array
 * referenced by materialised ch_block instances produced from a response.
 * Stored as vectors of unique_ptr so growing the vector never invalidates
 * pointers into already-owned objects.
 */
struct ChArena
{
    std::vector<std::unique_ptr<std::vector<uint8_t>>> bufs;
    std::vector<std::unique_ptr<std::vector<uint64_t>>> u64s;
    std::vector<std::unique_ptr<std::vector<ch_block_column>>> cols;
    std::vector<std::unique_ptr<ch_type>> types;
    std::vector<std::unique_ptr<std::string>> strings;
    std::vector<std::unique_ptr<std::vector<ch_type *>>> child_arrs;
    std::vector<std::unique_ptr<std::vector<char *>>> child_name_arrs;
};

static uint8_t *
arena_alloc_bytes(ChArena &a, size_t n)
{
    auto p = std::make_unique<std::vector<uint8_t>>(n);
    uint8_t *raw = p->data();
    a.bufs.push_back(std::move(p));
    return raw;
}

static uint64_t *
arena_alloc_u64(ChArena &a, size_t n)
{
    auto p = std::make_unique<std::vector<uint64_t>>(n);
    uint64_t *raw = p->data();
    a.u64s.push_back(std::move(p));
    return raw;
}

static ch_block_column *
arena_alloc_cols(ChArena &a, size_t n)
{
    auto p = std::make_unique<std::vector<ch_block_column>>(n);
    std::memset(p->data(), 0, n * sizeof(ch_block_column));
    ch_block_column *raw = p->data();
    a.cols.push_back(std::move(p));
    return raw;
}

static ch_type *
arena_alloc_type(ChArena &a)
{
    auto p = std::make_unique<ch_type>();
    std::memset(p.get(), 0, sizeof(ch_type));
    ch_type *raw = p.get();
    a.types.push_back(std::move(p));
    return raw;
}

static char *
arena_alloc_string(ChArena &a, const std::string &s)
{
    auto p = std::make_unique<std::string>(s);
    char *raw = p->data();
    a.strings.push_back(std::move(p));
    return raw;
}

/* ---- Type-tree construction from clickhouse-cpp TypeRef ------------- */

static ch_type *build_ch_type(ChArena &arena, const TypeRef &t);

static ch_type *
build_ch_type(ChArena &arena, const TypeRef &t)
{
    ch_type *out = arena_alloc_type(arena);

    switch (t->GetCode())
    {
        case Type::Void:    out->kind = CH_T_VOID; break;
        case Type::Int8:    out->kind = CH_T_INT8; break;
        case Type::Int16:   out->kind = CH_T_INT16; break;
        case Type::Int32:   out->kind = CH_T_INT32; break;
        case Type::Int64:   out->kind = CH_T_INT64; break;
        case Type::Int128:  out->kind = CH_T_INT128; break;
        case Type::UInt8:   out->kind = CH_T_UINT8; break;
        case Type::UInt16:  out->kind = CH_T_UINT16; break;
        case Type::UInt32:  out->kind = CH_T_UINT32; break;
        case Type::UInt64:  out->kind = CH_T_UINT64; break;
        case Type::UInt128: out->kind = CH_T_UINT128; break;
        case Type::Float32: out->kind = CH_T_FLOAT32; break;
        case Type::Float64: out->kind = CH_T_FLOAT64; break;
        case Type::String:  out->kind = CH_T_STRING; break;
        case Type::FixedString:
            out->kind = CH_T_FIXEDSTRING;
            out->fixed_size = (uint32_t) t->As<FixedStringType>()->GetSize();
            break;
        case Type::Date:    out->kind = CH_T_DATE; break;
        case Type::Date32:  out->kind = CH_T_DATE32; break;
        case Type::DateTime:
            out->kind = CH_T_DATETIME;
            out->tz = arena_alloc_string(arena, t->As<DateTimeType>()->Timezone());
            break;
        case Type::DateTime64:
        {
            auto dt = t->As<DateTime64Type>();
            out->kind = CH_T_DATETIME64;
            out->scale = (uint32_t) dt->GetPrecision();
            out->tz = arena_alloc_string(arena, dt->Timezone());
            break;
        }
        case Type::UUID:    out->kind = CH_T_UUID; break;
        case Type::IPv4:    out->kind = CH_T_IPV4; break;
        case Type::IPv6:    out->kind = CH_T_IPV6; break;
        case Type::Enum8:   out->kind = CH_T_ENUM8; break;
        case Type::Enum16:  out->kind = CH_T_ENUM16; break;
        case Type::Decimal:
        case Type::Decimal32:
        case Type::Decimal64:
        case Type::Decimal128:
        {
            auto d = t->As<DecimalType>();
            switch (t->GetCode())
            {
                case Type::Decimal32:  out->kind = CH_T_DECIMAL32; break;
                case Type::Decimal64:  out->kind = CH_T_DECIMAL64; break;
                case Type::Decimal128: out->kind = CH_T_DECIMAL128; break;
                default:               out->kind = CH_T_DECIMAL128; break;
            }
            out->decimal_precision = (uint32_t) d->GetPrecision();
            out->decimal_scale = (uint32_t) d->GetScale();
            break;
        }
        case Type::Nullable:
            /*
             * Nullability is tracked at the column level (is_nullable +
             * nulls bitmap) by strip_nullable in materialize_column, so the
             * type tree itself never carries CH_T_NULLABLE — return the
             * inner type directly.
             */
            return build_ch_type(arena, t->As<NullableType>()->GetNestedType());
        case Type::Array:
        {
            out->kind = CH_T_ARRAY;
            auto child = t->As<clickhouse::ArrayType>()->GetItemType();
            auto kids = std::make_unique<std::vector<ch_type *>>();
            kids->push_back(build_ch_type(arena, child));
            out->child = kids->data();
            out->n_child = 1;
            arena.child_arrs.push_back(std::move(kids));
            break;
        }
        case Type::Tuple:
        {
            out->kind = CH_T_TUPLE;
            auto items = t->As<TupleType>()->GetTupleType();
            auto kids = std::make_unique<std::vector<ch_type *>>();
            for (auto &c : items)
                kids->push_back(build_ch_type(arena, c));
            out->child = kids->data();
            out->n_child = kids->size();
            arena.child_arrs.push_back(std::move(kids));
            break;
        }
        case Type::LowCardinality:
            /* Surface as the underlying type for the PG side (preserves the
             * legacy treatment of LowCardinality(String) as TEXT). */
            return build_ch_type(arena, t->As<LowCardinalityType>()->GetNestedType());
        default:
            throw std::runtime_error("unsupported type " + t->GetName());
    }
    return out;
}

/* ---- Column materialisation: ColumnRef → ch_block_column ------------ */

static void materialize_column(ChArena &arena, const std::string &name,
                               ColumnRef col, ch_block_column *out);

/* Strip Nullable wrapper; populate is_nullable + nulls in `out`. */
static ColumnRef
strip_nullable(ChArena &arena, ColumnRef col, ch_block_column *out, size_t rows)
{
    if (col->Type()->GetCode() != Type::Nullable)
        return col;

    auto n = col->AsStrict<ColumnNullable>();
    uint8_t *mask = arena_alloc_bytes(arena, rows);
    for (size_t i = 0; i < rows; i++)
        mask[i] = n->IsNull(i) ? 1 : 0;
    out->is_nullable = true;
    out->nulls = mask;
    return n->Nested();
}

static void
materialize_primitive(ChArena &arena, ColumnRef col, ch_block_column *out,
                      size_t elem_size)
{
    size_t rows = col->Size();
    uint8_t *buf = arena_alloc_bytes(arena, rows * elem_size);

    /* All ColumnVector<T>::At returns a const T&; copy by element. */
    switch (col->Type()->GetCode())
    {
        case Type::Int8:
            for (size_t i = 0; i < rows; i++)
                ((int8_t *) buf)[i] = col->AsStrict<ColumnInt8>()->At(i);
            break;
        case Type::Int16:
            for (size_t i = 0; i < rows; i++)
                ((int16_t *) buf)[i] = col->AsStrict<ColumnInt16>()->At(i);
            break;
        case Type::Int32:
            for (size_t i = 0; i < rows; i++)
                ((int32_t *) buf)[i] = col->AsStrict<ColumnInt32>()->At(i);
            break;
        case Type::Int64:
            for (size_t i = 0; i < rows; i++)
                ((int64_t *) buf)[i] = col->AsStrict<ColumnInt64>()->At(i);
            break;
        case Type::UInt8:
            for (size_t i = 0; i < rows; i++)
                buf[i] = col->AsStrict<ColumnUInt8>()->At(i);
            break;
        case Type::UInt16:
            for (size_t i = 0; i < rows; i++)
                ((uint16_t *) buf)[i] = col->AsStrict<ColumnUInt16>()->At(i);
            break;
        case Type::UInt32:
            for (size_t i = 0; i < rows; i++)
                ((uint32_t *) buf)[i] = col->AsStrict<ColumnUInt32>()->At(i);
            break;
        case Type::UInt64:
            for (size_t i = 0; i < rows; i++)
                ((uint64_t *) buf)[i] = col->AsStrict<ColumnUInt64>()->At(i);
            break;
        case Type::Float32:
            for (size_t i = 0; i < rows; i++)
                ((float *) buf)[i] = col->AsStrict<ColumnFloat32>()->At(i);
            break;
        case Type::Float64:
            for (size_t i = 0; i < rows; i++)
                ((double *) buf)[i] = col->AsStrict<ColumnFloat64>()->At(i);
            break;
        default:
            throw UNEXPECTED_COLUMN("primitive", col);
    }
    out->d.raw = buf;
    out->num_rows = rows;
}

/*
 * Layout strings/decimals/inet/enum names as (offsets, data). offsets[i] is
 * the cumulative end byte offset for row i.
 */
struct StrBuilder
{
    std::vector<uint64_t> offs;
    std::vector<uint8_t> data;
};

static void
sb_push(StrBuilder &sb, const char *p, size_t n)
{
    sb.data.insert(sb.data.end(), (const uint8_t *) p, (const uint8_t *) p + n);
    sb.offs.push_back(sb.data.size());
}

static void
finish_str_builder(ChArena &arena, StrBuilder &sb, ch_block_column *out)
{
    uint64_t rows = sb.offs.size();
    uint64_t *offs = arena_alloc_u64(arena, rows);
    uint8_t *data = arena_alloc_bytes(arena, sb.data.size());
    if (rows)
        std::memcpy(offs, sb.offs.data(), rows * sizeof(uint64_t));
    if (!sb.data.empty())
        std::memcpy(data, sb.data.data(), sb.data.size());
    out->d.str.offsets = offs;
    out->d.str.data = data;
    out->num_rows = rows;
}

static void
materialize_string(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto s = col->AsStrict<ColumnString>();
    StrBuilder sb;
    sb.offs.reserve(s->Size());
    for (size_t i = 0; i < s->Size(); i++)
    {
        auto v = s->At(i);
        sb_push(sb, v.data(), v.size());
    }
    finish_str_builder(arena, sb, out);
}

static void
materialize_fixedstring(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto s = col->AsStrict<ColumnFixedString>();
    size_t width = col->Type()->As<FixedStringType>()->GetSize();
    size_t rows = s->Size();
    uint8_t *buf = arena_alloc_bytes(arena, rows * width);
    for (size_t i = 0; i < rows; i++)
    {
        auto v = s->At(i);
        std::memcpy(buf + i * width, v.data(), v.size());
        if (v.size() < width)
            std::memset(buf + i * width + v.size(), 0, width - v.size());
    }
    out->d.raw = buf;
    out->num_rows = rows;
}

static void
materialize_uuid(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto u = col->AsStrict<ColumnUUID>();
    size_t rows = u->Size();
    uint8_t *buf = arena_alloc_bytes(arena, rows * 16);
    for (size_t i = 0; i < rows; i++)
    {
        auto v = u->At(i);
        uint64_t a = HOST_TO_BIG_ENDIAN_64(v.first);
        uint64_t b = HOST_TO_BIG_ENDIAN_64(v.second);
        std::memcpy(buf + i * 16, &a, 8);
        std::memcpy(buf + i * 16 + 8, &b, 8);
    }
    out->d.raw = buf;
    out->num_rows = rows;
}

/* Format Int128 with sign and decimal point honoring scale. */
static std::string
format_decimal(const Int128 &val, size_t scale)
{
    std::stringstream ss;
    ss << val;
    std::string str = ss.str();
    std::stringstream res;

    if (val < 0)
    {
        res << '-';
        str.erase(0, 1);
    }
    if (scale == 0)
    {
        res << str;
    }
    else if (str.length() <= scale)
    {
        res << "0." << std::string(scale - str.length(), '0') << str;
    }
    else
    {
        auto decAt = str.length() - scale;
        res << str.substr(0, decAt);
        if (decAt < str.length())
            res << '.' << str.substr(decAt);
    }
    return res.str();
}

static void
materialize_decimal(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto d = col->AsStrict<ColumnDecimal>();
    size_t scale = (size_t) col->Type()->As<DecimalType>()->GetScale();
    StrBuilder sb;
    sb.offs.reserve(d->Size());
    for (size_t i = 0; i < d->Size(); i++)
    {
        auto s = format_decimal(d->At(i), scale);
        sb_push(sb, s.data(), s.size());
    }
    finish_str_builder(arena, sb, out);
}

static void
materialize_enum(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    StrBuilder sb;
    /* NameAt() throws on values not present in the dict; nullable rows can
     * hold arbitrary underlying ints, so emit empty for null rows. */
    auto is_null = [out](size_t i) {
        return out->is_nullable && out->nulls && out->nulls[i];
    };
    if (col->Type()->GetCode() == Type::Enum8)
    {
        auto e = col->AsStrict<ColumnEnum8>();
        sb.offs.reserve(e->Size());
        for (size_t i = 0; i < e->Size(); i++)
        {
            if (is_null(i))
            {
                sb_push(sb, "", 0);
                continue;
            }
            auto v = e->NameAt(i);
            sb_push(sb, v.data(), v.size());
        }
    }
    else
    {
        auto e = col->AsStrict<ColumnEnum16>();
        sb.offs.reserve(e->Size());
        for (size_t i = 0; i < e->Size(); i++)
        {
            if (is_null(i))
            {
                sb_push(sb, "", 0);
                continue;
            }
            auto v = e->NameAt(i);
            sb_push(sb, v.data(), v.size());
        }
    }
    finish_str_builder(arena, sb, out);
}

static void
materialize_ip(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    StrBuilder sb;
    if (col->Type()->GetCode() == Type::IPv4)
    {
        auto ip = col->AsStrict<ColumnIPv4>();
        sb.offs.reserve(ip->Size());
        for (size_t i = 0; i < ip->Size(); i++)
        {
            auto s = ip->AsString(i);
            sb_push(sb, s.data(), s.size());
        }
    }
    else
    {
        auto ip = col->AsStrict<ColumnIPv6>();
        sb.offs.reserve(ip->Size());
        for (size_t i = 0; i < ip->Size(); i++)
        {
            auto s = ip->AsString(i);
            sb_push(sb, s.data(), s.size());
        }
    }
    finish_str_builder(arena, sb, out);
}

static void
materialize_lowcardinality(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto lc = col->AsStrict<ColumnLowCardinality>();
    StrBuilder sb;
    sb.offs.reserve(lc->Size());
    for (size_t i = 0; i < lc->Size(); i++)
    {
        auto item = lc->GetItem(i);
        auto bin = item.AsBinaryData();
        sb_push(sb, bin.data(), bin.size());
    }
    finish_str_builder(arena, sb, out);
}

static void
materialize_date_seconds(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    size_t rows = col->Size();
    uint8_t *buf = arena_alloc_bytes(arena, rows * sizeof(int64_t));
    int64_t *typed = (int64_t *) buf;

    switch (col->Type()->GetCode())
    {
        case Type::Date:
            for (size_t i = 0; i < rows; i++)
                typed[i] = (int64_t) col->AsStrict<ColumnDate>()->At(i);
            break;
        case Type::Date32:
            for (size_t i = 0; i < rows; i++)
                typed[i] = (int64_t) col->AsStrict<ColumnDate32>()->At(i);
            break;
        case Type::DateTime:
            for (size_t i = 0; i < rows; i++)
                typed[i] = (int64_t) col->AsStrict<ColumnDateTime>()->At(i);
            break;
        case Type::DateTime64:
            for (size_t i = 0; i < rows; i++)
                typed[i] = (int64_t) col->AsStrict<ColumnDateTime64>()->At(i);
            break;
        default:
            throw UNEXPECTED_COLUMN("date/time", col);
    }
    out->d.raw = buf;
    out->num_rows = rows;
}

static void
materialize_array(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto arr = col->AsStrict<ColumnArray>();
    size_t rows = arr->Size();
    uint64_t *offs = arena_alloc_u64(arena, rows);

    /* Build a flat inner column by appending each row's slice. */
    auto inner_type = arr->GetType().As<clickhouse::ArrayType>()->GetItemType();
    ColumnRef inner = CreateColumnByType(inner_type->GetName());
    uint64_t cum = 0;
    for (size_t i = 0; i < rows; i++)
    {
        ColumnRef slice = arr->GetAsColumn(i);
        inner->Append(slice);
        cum += slice->Size();
        offs[i] = cum;
    }

    ch_block_column *inner_col = arena_alloc_cols(arena, 1);
    materialize_column(arena, std::string(), inner, inner_col);

    out->d.arr.offsets = offs;
    out->d.arr.inner = inner_col;
    out->num_rows = rows;
}

static void
materialize_tuple(ChArena &arena, ColumnRef col, ch_block_column *out)
{
    auto t = col->AsStrict<ColumnTuple>();
    size_t n = t->TupleSize();
    ch_block_column *fields = arena_alloc_cols(arena, n);
    for (size_t i = 0; i < n; i++)
        materialize_column(arena, std::string(), (*t)[i], &fields[i]);
    out->d.tup.fields = fields;
    out->num_rows = col->Size();
}

static void
materialize_column(ChArena &arena, const std::string &name,
                   ColumnRef col, ch_block_column *out)
{
    if (!name.empty())
        out->name = arena_alloc_string(arena, name);

    size_t rows = col->Size();
    out->num_rows = rows;

    ColumnRef inner = strip_nullable(arena, col, out, rows);

    /* Underlying type after stripping Nullable. */
    out->type = build_ch_type(arena, inner->Type());

    switch (inner->Type()->GetCode())
    {
        case Type::Void:
            /* ColumnNothing has no data; out->num_rows already set. */
            break;
        case Type::Int8:
        case Type::UInt8:
            materialize_primitive(arena, inner, out, 1);
            break;
        case Type::Int16:
        case Type::UInt16:
            materialize_primitive(arena, inner, out, 2);
            break;
        case Type::Int32:
        case Type::UInt32:
        case Type::Float32:
            materialize_primitive(arena, inner, out, 4);
            break;
        case Type::Int64:
        case Type::UInt64:
        case Type::Float64:
            materialize_primitive(arena, inner, out, 8);
            break;
        case Type::String:
            materialize_string(arena, inner, out);
            break;
        case Type::FixedString:
            materialize_fixedstring(arena, inner, out);
            break;
        case Type::UUID:
            materialize_uuid(arena, inner, out);
            break;
        case Type::Decimal:
        case Type::Decimal32:
        case Type::Decimal64:
        case Type::Decimal128:
            materialize_decimal(arena, inner, out);
            break;
        case Type::Enum8:
        case Type::Enum16:
            materialize_enum(arena, inner, out);
            break;
        case Type::IPv4:
        case Type::IPv6:
            materialize_ip(arena, inner, out);
            break;
        case Type::LowCardinality:
            materialize_lowcardinality(arena, inner, out);
            break;
        case Type::Date:
        case Type::Date32:
        case Type::DateTime:
        case Type::DateTime64:
            materialize_date_seconds(arena, inner, out);
            break;
        case Type::Array:
            materialize_array(arena, inner, out);
            break;
        case Type::Tuple:
            materialize_tuple(arena, inner, out);
            break;
        default:
            throw UNEXPECTED_COLUMN("column", inner);
    }
}

/* ---- response ------------------------------------------------------- */

struct ch_binary_response_t
{
    std::vector<std::vector<ColumnRef>> blocks;
    std::vector<std::string> column_names; /* names from block 0 */
    size_t columns_count = 0;
    /* Empty string => no error. std::string (not a fixed buffer) because
     * ClickHouse errors can embed the full query and easily exceed CH_ERR_LEN. */
    std::string error;
    bool success = false;

    /* materialised state */
    std::vector<bool> mat_valid;
    std::vector<ch_block> mat;
    std::vector<std::unique_ptr<ChArena>> mat_arenas;
};

static void
set_resp_error(ch_binary_response_t *resp, const char *str)
{
    if (!resp->error.empty())
        return;
    try
    {
        resp->error = (str && *str) ? str : "?";
    }
    catch (...)
    {
        /* bad_alloc fallback — must not leave error empty when one occurred. */
        resp->error = "?";
    }
}

ch_binary_connection_t *
ch_binary_connect(ch_connection_details *details, char *errbuf, size_t errbuf_size)
{
    ClientOptions *options = nullptr;
    ch_binary_connection_t *conn = nullptr;

    try
    {
        options = new ClientOptions();
        options->SetPingBeforeQuery(true);

        if (details->host)
        {
            options->SetHost(std::string(details->host));
            if (!details->port && ch_is_cloud_host(details->host))
                options->SetPort(CLICKHOUSE_SECURE_PORT);
        }
        if (details->port)
            options->SetPort(details->port);
        if (details->dbname)
            options->SetDefaultDatabase(std::string(details->dbname));
        if (details->username)
            options->SetUser(std::string(details->username));
        if (details->password)
            options->SetPassword(std::string(details->password));
        if (options->port == CLICKHOUSE_SECURE_PORT)
            options->SetSSLOptions(ClientOptions::SSLOptions());

        conn = new ch_binary_connection_t();
        Client *client = new Client(*options);
        conn->client = client;
        conn->options = options;
    }
    catch (const std::exception &e)
    {
        set_errbuf(errbuf, errbuf_size, e.what());
        if (conn)
            delete conn;
        if (options)
            delete options;
        conn = nullptr;
    }
    return conn;
}

void
ch_binary_close(ch_binary_connection_t *conn)
{
    delete (Client *) conn->client;
    delete (ClientOptions *) conn->options;
}

static QuerySettings
build_settings(const ch_query *query)
{
    kv_iter iter;
    QuerySettings res;
    for (iter = new_kv_iter(query->settings); !kv_iter_done(&iter); kv_iter_next(&iter))
        res.insert_or_assign(iter.name, QuerySettingsField{iter.value, 1});
    return res;
}

static QueryParams
build_params(const ch_query *query)
{
    QueryParams res;
    for (int i = 0; i < query->num_params; i++)
    {
        char key[32];
        std::snprintf(key, sizeof(key), "p%d", i + 1);
        res.insert_or_assign(key, QueryParamValue(query->param_values[i]));
    }
    return res;
}

ch_binary_response_t *
ch_binary_simple_query(ch_binary_connection_t *conn, const ch_query *query,
                       bool (*check_cancel)(void))
{
    Client *client = (Client *) conn->client;
    auto *resp = new ch_binary_response_t();

    try
    {
        client->Select(clickhouse::Query(query->sql)
                           .SetQuerySettings(build_settings(query))
                           .SetParams(build_params(query))
                           .OnProgress([&check_cancel](const Progress &) {
                               if (check_cancel && check_cancel())
                                   throw std::runtime_error("query was canceled");
                           })
                           .OnDataCancelable([resp, &check_cancel](const Block &block) {
                               if (check_cancel && check_cancel())
                               {
                                   set_resp_error(resp, "query was canceled");
                                   return false;
                               }
                               if (block.GetColumnCount() == 0)
                                   return true;
                               if (resp->columns_count && block.GetColumnCount() != resp->columns_count)
                               {
                                   set_resp_error(resp, "columns mismatch in blocks");
                                   return false;
                               }
                               if (resp->columns_count == 0)
                               {
                                   resp->columns_count = block.GetColumnCount();
                                   resp->column_names.reserve(resp->columns_count);
                                   for (Block::Iterator bi(block); bi.IsValid(); bi.Next())
                                       resp->column_names.emplace_back(bi.Name());
                               }
                               std::vector<ColumnRef> vec;
                               vec.reserve(resp->columns_count);
                               for (size_t i = 0; i < resp->columns_count; ++i)
                                   vec.push_back(block[i]);
                               resp->blocks.push_back(std::move(vec));
                               return true;
                           }));
    }
    catch (const std::exception &e)
    {
        client->ResetConnection();
        resp->blocks.clear();
        set_resp_error(resp, e.what());
    }

    resp->success = resp->error.empty();
    resp->mat.resize(resp->blocks.size());
    resp->mat_valid.assign(resp->blocks.size(), false);
    resp->mat_arenas.resize(resp->blocks.size());
    return resp;
}

void
ch_binary_response_free(ch_binary_response_t *resp)
{
    delete resp;
}

const char *
ch_binary_response_error(const ch_binary_response_t *resp)
{
    return (resp && !resp->error.empty()) ? resp->error.c_str() : nullptr;
}

bool
ch_binary_response_success(const ch_binary_response_t *resp)
{
    return resp && resp->success;
}

size_t
ch_binary_response_block_count(const ch_binary_response_t *resp)
{
    return resp ? resp->blocks.size() : 0;
}

size_t
ch_binary_response_columns(const ch_binary_response_t *resp)
{
    return resp ? resp->columns_count : 0;
}

int
ch_binary_response_block_at(ch_binary_response_t *resp, size_t idx,
                            ch_block *out, char *errbuf, size_t errbuf_size)
{
    if (idx >= resp->blocks.size())
    {
        set_errbuf(errbuf, errbuf_size, "pg_clickhouse: block index out of range");
        return -1;
    }
    if (resp->mat_valid[idx])
    {
        *out = resp->mat[idx];
        return 0;
    }

    try
    {
        auto arena = std::make_unique<ChArena>();
        auto &block_cols = resp->blocks[idx];
        size_t ncols = resp->columns_count;
        ch_block_column *out_cols = arena_alloc_cols(*arena, ncols);
        uint64_t nrows = ncols ? block_cols[0]->Size() : 0;

        for (size_t i = 0; i < ncols; i++)
            materialize_column(*arena, resp->column_names[i], block_cols[i], &out_cols[i]);

        ch_block blk{};
        blk.num_columns = ncols;
        blk.num_rows = nrows;
        blk.columns = out_cols;
        blk.arena = arena.get();
        resp->mat[idx] = blk;
        resp->mat_valid[idx] = true;
        resp->mat_arenas[idx] = std::move(arena);
        *out = blk;
        return 0;
    }
    catch (const std::exception &e)
    {
        set_errbuf(errbuf, errbuf_size, e.what());
        return -1;
    }
}

void
ch_block_free(ch_block * /*blk*/)
{
    /* No-op: response_free owns the arena. */
}

/* ---- INSERT --------------------------------------------------------- */

struct ch_binary_insert_handle
{
    Client *client = nullptr;     /* borrowed; owned by connection */
    Block *block = nullptr;       /* owned here */
    ChArena type_arena;           /* owns ch_type trees */
    std::vector<ch_binary_column_info> infos;
    std::vector<std::string> name_storage;

    /*
     * Active array context. For nested Array(Array(...)) writes each
     * ch_binary_array_begin pushes a new items column; stack.back() is
     * the current append target. array_col_idx names the outermost
     * column when stack is non-empty.
     */
    std::vector<ColumnRef> array_stack;
    size_t array_col_idx = 0;

    ~ch_binary_insert_handle()
    {
        delete block;
    }
};

ch_binary_insert_handle *
ch_binary_begin_insert(ch_binary_connection_t *conn, const ch_query *query,
                       ch_binary_column_info **out_cols, size_t *out_n,
                       char *errbuf, size_t errbuf_size)
{
    auto h = std::unique_ptr<ch_binary_insert_handle>(new ch_binary_insert_handle());
    h->client = (Client *) conn->client;

    try
    {
        h->block = new Block(h->client->BeginInsert(std::string(query->sql) + " VALUES"));
    }
    catch (const std::exception &e)
    {
        h->client->ResetConnection();
        set_errbuf(errbuf, errbuf_size, e.what());
        return nullptr;
    }

    size_t n = h->block->GetColumnCount();
    h->infos.resize(n);
    h->name_storage.reserve(n);
    size_t i = 0;
    for (Block::Iterator bi(*h->block); bi.IsValid(); bi.Next())
    {
        h->name_storage.emplace_back(bi.Name());
        h->infos[i].name = h->name_storage[i].c_str();
        TypeRef t = bi.Type();
        bool is_nullable = (t->GetCode() == Type::Nullable);
        if (is_nullable)
            t = t->As<NullableType>()->GetNestedType();
        try
        {
            h->infos[i].type = build_ch_type(h->type_arena, t);
        }
        catch (const std::exception &e)
        {
            set_errbuf(errbuf, errbuf_size, e.what());
            return nullptr;
        }
        h->infos[i].is_nullable = is_nullable;
        i++;
    }
    *out_cols = h->infos.data();
    *out_n = n;
    return h.release();
}

/*
 * Resolve the column to append to (top-level or active array's inner)
 * AND strip a Nullable wrapper, appending the null bit if needed.
 *
 * Throws on NULL-into-non-Nullable.
 */
static ColumnRef
resolve_append_col(ch_binary_insert_handle *h, size_t col_idx, bool isnull)
{
    ColumnRef col;
    if (!h->array_stack.empty())
        col = h->array_stack.back();
    else
        col = (*h->block)[col_idx];

    bool is_nullable = (col->Type()->GetCode() == Type::Nullable);
    if (isnull && !is_nullable)
        throw std::runtime_error("cannot append NULL to NOT NULL " + col->Type()->GetName() + " column");
    if (is_nullable)
    {
        auto n = col->AsStrict<ColumnNullable>();
        n->Append(isnull);
        col = n->Nested();
    }
    return col;
}

template <class F>
static int
guarded(char *errbuf, size_t errbuf_size, F &&f)
{
    try
    {
        f();
        return 0;
    }
    catch (const std::exception &e)
    {
        set_errbuf(errbuf, errbuf_size, e.what());
        return -1;
    }
}

int
ch_binary_append_int(ch_binary_insert_handle *h, size_t col, int64_t val,
                     bool isnull, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        switch (c->Type()->GetCode())
        {
            case Type::Int8:  c->AsStrict<ColumnInt8>()->Append((int8_t) val); break;
            case Type::Int16: c->AsStrict<ColumnInt16>()->Append((int16_t) val); break;
            case Type::Int32: c->AsStrict<ColumnInt32>()->Append((int32_t) val); break;
            case Type::Int64: c->AsStrict<ColumnInt64>()->Append((int64_t) val); break;
            default:
                throw UNEXPECTED_COLUMN("int", c);
        }
    });
}

int
ch_binary_append_uint(ch_binary_insert_handle *h, size_t col, uint64_t val,
                      bool isnull, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        switch (c->Type()->GetCode())
        {
            case Type::UInt8:  c->AsStrict<ColumnUInt8>()->Append((uint8_t) val); break;
            case Type::UInt16: c->AsStrict<ColumnUInt16>()->Append((uint16_t) val); break;
            case Type::UInt32: c->AsStrict<ColumnUInt32>()->Append((uint32_t) val); break;
            case Type::UInt64: c->AsStrict<ColumnUInt64>()->Append((uint64_t) val); break;
            default:
                throw UNEXPECTED_COLUMN("uint", c);
        }
    });
}

int
ch_binary_append_double(ch_binary_insert_handle *h, size_t col, double val,
                        bool isnull, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        if (c->Type()->GetCode() != Type::Float64)
            throw UNEXPECTED_COLUMN("Float64", c);
        c->AsStrict<ColumnFloat64>()->Append(val);
    });
}

int
ch_binary_append_float(ch_binary_insert_handle *h, size_t col, float val,
                       bool isnull, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        if (c->Type()->GetCode() != Type::Float32)
            throw UNEXPECTED_COLUMN("Float32", c);
        c->AsStrict<ColumnFloat32>()->Append(val);
    });
}

int
ch_binary_append_bytes(ch_binary_insert_handle *h, size_t col, const void *p,
                       size_t n, bool isnull, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        std::string s;
        if (!isnull && p && n > 0)
            s.assign((const char *) p, n);
        switch (c->Type()->GetCode())
        {
            case Type::FixedString:
                c->AsStrict<ColumnFixedString>()->Append(s);
                break;
            case Type::String:
                c->AsStrict<ColumnString>()->Append(s);
                break;
            case Type::Enum8:
                if (isnull)
                    c->AsStrict<ColumnEnum8>()->Append(0, false);
                else
                    c->AsStrict<ColumnEnum8>()->Append(s);
                break;
            case Type::Enum16:
                if (isnull)
                    c->AsStrict<ColumnEnum16>()->Append(0, false);
                else
                    c->AsStrict<ColumnEnum16>()->Append(s);
                break;
            case Type::LowCardinality:
                if (c->AsStrict<ColumnLowCardinality>()->GetNestedType()->GetCode() == Type::Nullable)
                    throw std::runtime_error("nested Nullable is not supported");
                c->AsStrict<ColumnLowCardinalityT<ColumnString>>()->Append(s);
                break;
            default:
                throw UNEXPECTED_COLUMN("text", c);
        }
    });
}

int
ch_binary_append_decimal(ch_binary_insert_handle *h, size_t col,
                         const char *digits, bool isnull,
                         char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        switch (c->Type()->GetCode())
        {
            case Type::Decimal:
            case Type::Decimal32:
            case Type::Decimal64:
            case Type::Decimal128:
                if (isnull || !digits)
                    c->AsStrict<ColumnDecimal>()->Append(Int128{});
                else
                    c->AsStrict<ColumnDecimal>()->Append(std::string(digits));
                break;
            default:
                throw UNEXPECTED_COLUMN("decimal", c);
        }
    });
}

int
ch_binary_append_uuid(ch_binary_insert_handle *h, size_t col,
                      const uint8_t bytes[16], bool isnull,
                      char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        if (c->Type()->GetCode() != Type::UUID)
            throw UNEXPECTED_COLUMN("UUID", c);
        UUID u{};
        if (!isnull)
        {
            uint64_t a, b;
            std::memcpy(&a, bytes, 8);
            std::memcpy(&b, bytes + 8, 8);
            u.first = BIG_ENDIAN_64_TO_HOST(a);
            u.second = BIG_ENDIAN_64_TO_HOST(b);
        }
        c->AsStrict<ColumnUUID>()->Append(u);
    });
}

int
ch_binary_append_inet(ch_binary_insert_handle *h, size_t col,
                      const char *ip_text, bool isnull,
                      char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        if (isnull || !ip_text)
        {
            switch (c->Type()->GetCode())
            {
                case Type::IPv4: c->AsStrict<ColumnIPv4>()->Append(0); break;
                case Type::IPv6: c->AsStrict<ColumnIPv6>()->Append("::"); break;
                default: throw UNEXPECTED_COLUMN("INET", c);
            }
        }
        else
        {
            switch (c->Type()->GetCode())
            {
                case Type::IPv4: c->AsStrict<ColumnIPv4>()->Append(ip_text); break;
                case Type::IPv6: c->AsStrict<ColumnIPv6>()->Append(ip_text); break;
                default: throw UNEXPECTED_COLUMN("INET", c);
            }
        }
    });
}

int
ch_binary_append_date_seconds(ch_binary_insert_handle *h, size_t col,
                              int64_t seconds, bool isnull,
                              char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        switch (c->Type()->GetCode())
        {
            case Type::Date:   c->AsStrict<ColumnDate>()->Append((std::time_t) seconds); break;
            case Type::Date32: c->AsStrict<ColumnDate32>()->Append((std::time_t) seconds); break;
            default: throw UNEXPECTED_COLUMN("DATE", c);
        }
    });
}

int
ch_binary_append_datetime_seconds(ch_binary_insert_handle *h, size_t col,
                                  int64_t seconds, bool isnull,
                                  char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        if (c->Type()->GetCode() != Type::DateTime)
            throw UNEXPECTED_COLUMN("DateTime", c);
        c->AsStrict<ColumnDateTime>()->Append((std::time_t) seconds);
    });
}

int
ch_binary_append_datetime64_raw(ch_binary_insert_handle *h, size_t col,
                                int64_t raw, bool isnull,
                                char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef c = resolve_append_col(h, col, isnull);
        if (c->Type()->GetCode() != Type::DateTime64)
            throw UNEXPECTED_COLUMN("DateTime64", c);
        c->AsStrict<ColumnDateTime64>()->Append(raw);
    });
}

int
ch_binary_array_begin(ch_binary_insert_handle *h, size_t col_idx, size_t,
                      char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        ColumnRef parent;
        if (h->array_stack.empty())
        {
            parent = (*h->block)[col_idx];
            h->array_col_idx = col_idx;
        }
        else
            parent = h->array_stack.back();

        /* Top-level column may be Nullable(Array(...)); CH disallows
         * Nullable inside Array so only the outer level needs unwrapping. */
        if (parent->Type()->GetCode() == Type::Nullable)
            parent = parent->AsStrict<ColumnNullable>()->Nested();
        if (parent->Type()->GetCode() != Type::Array)
            throw UNEXPECTED_COLUMN("array", parent);

        auto arrcol = parent->AsStrict<ColumnArray>();
        auto items =
            CreateColumnByType(arrcol->GetType().As<clickhouse::ArrayType>()->GetItemType()->GetName());
        h->array_stack.push_back(items);
    });
}

int
ch_binary_array_end(ch_binary_insert_handle *h, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        if (h->array_stack.empty())
            throw std::runtime_error("ch_binary_array_end without matching begin");

        ColumnRef inner = h->array_stack.back();
        h->array_stack.pop_back();

        ColumnRef parent;
        if (h->array_stack.empty())
            parent = (*h->block)[h->array_col_idx];
        else
            parent = h->array_stack.back();
        if (parent->Type()->GetCode() == Type::Nullable)
            parent = parent->AsStrict<ColumnNullable>()->Nested();
        parent->AsStrict<ColumnArray>()->AppendAsColumn(inner);
    });
}

bool
ch_binary_array_active(const ch_binary_insert_handle *h)
{
    return h && !h->array_stack.empty();
}

ch_type_kind
ch_binary_column_kind(const ch_binary_insert_handle *h, size_t col)
{
    ColumnRef c;
    if (!h->array_stack.empty())
        c = h->array_stack.back();
    else
        c = (*h->block)[col];

    /* Strip Nullable. */
    if (c->Type()->GetCode() == Type::Nullable)
        c = c->AsStrict<ColumnNullable>()->Nested();

    switch (c->Type()->GetCode())
    {
        case Type::Int8:        return CH_T_INT8;
        case Type::Int16:       return CH_T_INT16;
        case Type::Int32:       return CH_T_INT32;
        case Type::Int64:       return CH_T_INT64;
        case Type::UInt8:       return CH_T_UINT8;
        case Type::UInt16:      return CH_T_UINT16;
        case Type::UInt32:      return CH_T_UINT32;
        case Type::UInt64:      return CH_T_UINT64;
        case Type::Float32:     return CH_T_FLOAT32;
        case Type::Float64:     return CH_T_FLOAT64;
        case Type::String:      return CH_T_STRING;
        case Type::FixedString: return CH_T_FIXEDSTRING;
        case Type::Date:        return CH_T_DATE;
        case Type::Date32:      return CH_T_DATE32;
        case Type::DateTime:    return CH_T_DATETIME;
        case Type::DateTime64:  return CH_T_DATETIME64;
        case Type::UUID:        return CH_T_UUID;
        case Type::IPv4:        return CH_T_IPV4;
        case Type::IPv6:        return CH_T_IPV6;
        case Type::Enum8:       return CH_T_ENUM8;
        case Type::Enum16:      return CH_T_ENUM16;
        case Type::Decimal:
        case Type::Decimal32:   return CH_T_DECIMAL32;
        case Type::Decimal64:   return CH_T_DECIMAL64;
        case Type::Decimal128:  return CH_T_DECIMAL128;
        case Type::Array:       return CH_T_ARRAY;
        case Type::Tuple:       return CH_T_TUPLE;
        case Type::LowCardinality: return CH_T_LOWCARDINALITY;
        default:                return CH_T_UNKNOWN;
    }
}

uint32_t
ch_binary_column_datetime64_precision(const ch_binary_insert_handle *h, size_t col)
{
    ColumnRef c;
    if (!h->array_stack.empty())
        c = h->array_stack.back();
    else
        c = (*h->block)[col];

    if (c->Type()->GetCode() == Type::Nullable)
        c = c->AsStrict<ColumnNullable>()->Nested();
    if (c->Type()->GetCode() != Type::DateTime64)
        return 0;
    return (uint32_t) c->Type()->As<DateTime64Type>()->GetPrecision();
}

int
ch_binary_flush_block(ch_binary_insert_handle *h, char *errbuf, size_t errbuf_size)
{
    return guarded(errbuf, errbuf_size, [&] {
        h->block->RefreshRowCount();
        try
        {
            h->client->SendInsertBlock(*h->block);
            h->block->Clear();
        }
        catch (...)
        {
            h->client->ResetConnection();
            throw;
        }
    });
}

void
ch_binary_end_insert(ch_binary_insert_handle *h, char *errbuf, size_t errbuf_size)
{
    if (!h)
        return;
    try
    {
        h->client->EndInsert();
    }
    catch (const std::exception &e)
    {
        h->client->ResetConnection();
        set_errbuf(errbuf, errbuf_size, e.what());
    }
    delete h;
}
