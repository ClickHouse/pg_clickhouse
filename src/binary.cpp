#include <iostream>
#include <cassert>
#include <stdexcept>

#include "clickhouse/columns/date.h"
#include "clickhouse/columns/ip4.h"
#include "clickhouse/columns/lowcardinality.h"
#include "clickhouse/columns/nullable.h"
#include "clickhouse/columns/factory.h"
#include <clickhouse/client.h>
#include <clickhouse/query.h>
#include <clickhouse/types/types.h>

#if __cplusplus > 199711L
#define register // Deprecated in C++11.
#endif // #if __cplusplus > 199711L

extern "C" {

#include "postgres.h"
#include "pgtime.h"
#include "funcapi.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memdebug.h"
#include "utils/palloc.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "binary.hh"
#include "internal.h"

using namespace clickhouse;

#if defined(__APPLE__) // Byte ordering on macOS
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define HOST_TO_BIG_ENDIAN_64(x) OSSwapHostToBigInt64(x)
#else
#include <endian.h>
#define HOST_TO_BIG_ENDIAN_64(x) htobe64(x)
#endif

/* palloc which will throw exceptions */
static void * exc_palloc(Size size)
{
	/* duplicates MemoryContextAlloc to avoid increased overhead */
	void * ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));

	if (!AllocSizeIsValid(size))
		throw std::bad_alloc();

	context->isReset = false;

#if PG_VERSION_NUM >= 170000
	ret = context->methods->alloc(context, size, 0);
#else
	ret = context->methods->alloc(context, size);
#endif
	if (unlikely(ret == NULL))
		throw std::bad_alloc();

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	return ret;
}

void * exc_palloc0(Size size)
{
	/* duplicates MemoryContextAllocZero to avoid increased overhead */
	void * ret;
	MemoryContext context = CurrentMemoryContext;

	Assert(MemoryContextIsValid(context));

	if (!AllocSizeIsValid(size))
		throw std::bad_alloc();

	context->isReset = false;

#if PG_VERSION_NUM >= 170000
	ret = context->methods->alloc(context, size, 0);
#else
	ret = context->methods->alloc(context, size);
#endif
	if (unlikely(ret == NULL))
		throw std::bad_alloc();

	VALGRIND_MEMPOOL_ALLOC(context, ret, size);

	MemSetAligned(ret, 0, size);

	return ret;
}

#define CLICKHOUSE_SECURE_PORT 9440

ch_binary_connection_t * ch_binary_connect(
	char * host, int port, char * database, char * user, char * password, char ** error)
{
	ClientOptions * options = NULL;
	ch_binary_connection_t * conn = NULL;

	try
	{
		options = new ClientOptions();
		options->SetPingBeforeQuery(true);

		if (host) {
			options->SetHost(std::string(host));
			if (!port && ch_is_cloud_host(host))
				options->SetPort(CLICKHOUSE_SECURE_PORT);
		}
		if (port)
			options->SetPort(port);
		if (database)
			options->SetDefaultDatabase(std::string(database));
		if (user)
			options->SetUser(std::string(user));
		if (password)
			options->SetPassword(std::string(password));
		if (options->port == CLICKHOUSE_SECURE_PORT)
			options->SetSSLOptions(ClientOptions::SSLOptions());

		//options->SetRethrowException(false);
		conn = new ch_binary_connection_t();

		Client * client = new Client(*options);
		conn->client = client;
		conn->options = options;
	}
	catch (const std::exception & e)
	{
		if (error)
			*error = strdup(e.what());

		if (conn != NULL)
			delete conn;

		if (options != NULL)
			delete options;

		conn = NULL;
	}
	return conn;
}

static void set_resp_error(ch_binary_response_t * resp, const char * str)
{
	if (resp->error)
		return;

	resp->error = (char *)malloc(strlen(str) + 1);
	strcpy(resp->error, str);
}

static void set_state_error(ch_binary_read_state_t * state, const char * str)
{
	assert(state->error == NULL);
	state->error = (char *)malloc(strlen(str) + 1);
	strcpy(state->error, str);
}

ch_binary_response_t * ch_binary_simple_query(
	ch_binary_connection_t * conn, const char * query, bool (*check_cancel)(void))
{
	Client * client = (Client *)conn->client;
	ch_binary_response_t * resp;
	std::vector<std::vector<clickhouse::ColumnRef>> * values;

	try
	{
		resp = new ch_binary_response_t();
		values = new std::vector<std::vector<clickhouse::ColumnRef>>();
		client->Select(
			clickhouse::Query(query).SetQuerySettings(QuerySettings{
				/* Enable SQL compatibility. */
				{"join_use_nulls", QuerySettingsField{ "1", 1 }},
			}).OnDataCancelable([&resp, &values, &check_cancel](const Block & block) {
				if (check_cancel && check_cancel())
				{
					set_resp_error(resp, "query was canceled");
					return false;
				}

				/* some empty block */
				if (block.GetColumnCount() == 0)
					return true;

				auto vec = std::vector<clickhouse::ColumnRef>();

				if (resp->columns_count && block.GetColumnCount() != resp->columns_count)
				{
					set_resp_error(resp, "columns mismatch in blocks");
					return false;
				}

				resp->columns_count = block.GetColumnCount();
				resp->blocks_count++;

				for (size_t i = 0; i < resp->columns_count; ++i)
					vec.push_back(block[i]);

				values->push_back(std::move(vec));
				return true;
			})
		);

		resp->values = (void *)values;
	}
	catch (const std::exception & e)
	{
        client->ResetConnection();

		values->clear();
		set_resp_error(resp, e.what());
		delete values;
		values = NULL;
	}

	resp->success = (resp->error == NULL);
	return resp;
}

static Oid get_corr_postgres_type(const TypeRef & type)
{
	switch (type->GetCode())
	{
		case Type::Code::Int8:
		case Type::Code::Int16:
		case Type::Code::UInt8:
			return INT2OID;
		case Type::Code::Int32:
		case Type::Code::UInt16:
			return INT4OID;
		case Type::Code::Int64:
		case Type::Code::UInt64:
		case Type::Code::UInt32:
			return INT8OID;
		case Type::Code::Float32:
			return FLOAT4OID;
		case Type::Code::Float64:
			return FLOAT8OID;
		case Type::Code::FixedString:
		case Type::Code::Enum8:
		case Type::Code::Enum16:
		case Type::Code::String:
			return TEXTOID;
		case Type::Code::LowCardinality:
			return get_corr_postgres_type(type->As<LowCardinalityType>()->GetNestedType());
		case Type::Code::Date:
			return DATEOID;
		case Type::Code::DateTime:
			return TIMESTAMPOID;
		case Type::Code::DateTime64:
			return TIMESTAMPOID;
		case Type::Code::UUID:
			return UUIDOID;
		case Type::Code::Array: {
			Oid array_type = get_array_type(
				get_corr_postgres_type(type->As<clickhouse::ArrayType>()->GetItemType()));
			if (array_type == InvalidOid)
				throw std::runtime_error(
					"clickhouse_fdw: could not find array "
					" type for column type "
					+ type->GetName());

			return array_type;
		}
		case Type::Code::Tuple:
			return RECORDOID;
		case Type::Code::Nullable:
			return get_corr_postgres_type(type->As<NullableType>()->GetNestedType());
		default:
			throw std::runtime_error("clickhouse_fdw: unsupported column type " + type->GetName());
	}
}

void ch_binary_insert_state_free(void * c)
{
	auto * state = (ch_binary_insert_state *)c;
	if (state->columns)
	{
		/* try to send empty block that sets proper ClickHouse state */
		if (!state->success)
		{
			try
			{
				Client * client = (Client *)state->conn->client;
				client->Insert(state->table_name, Block());
			}
			catch (const std::exception & e)
			{
				// just ignore, next query will fail
				elog(NOTICE, "clickhouse_fdw: could not send empty packet");
			}
		}

		delete (std::vector<clickhouse::ColumnRef> *)state->columns;
	}
}

void ch_binary_prepare_insert(void * conn, char * query, ch_binary_insert_state * state)
{
	throw std::runtime_error("clickhouse_fdw: XXX ch_binary_prepare_insert not implemented");

// 	std::vector<clickhouse::ColumnRef> * vec = nullptr;
// 	Client * client = (Client *)((ch_binary_connection_t *)conn)->client;

// 	try
// 	{
// 		client->PrepareInsert(
// 			std::string(query) + " VALUES", [&state, &vec](const Block & sample_block) {
// 				if (sample_block.GetColumnCount() == 0)
// 					return true;

// 				vec = new std::vector<clickhouse::ColumnRef>();

// 				state->len = sample_block.GetColumnCount();

// #if PG_VERSION_NUM < 120000
// 				state->outdesc = CreateTemplateTupleDesc(state->len, false);
// #else
// 			    state->outdesc = CreateTemplateTupleDesc(state->len);
// #endif

// 				for (size_t i = 0; i < state->len; i++)
// 				{
// 					bool error = false;
// 					clickhouse::ColumnRef col = sample_block[i];

// 					auto chtype = col->Type();
// 					if (chtype->GetCode() == Type::LowCardinality)
// 					{
// 						chtype = col->As<ColumnLowCardinality>()->GetNestedType();
// 					}

// 					Oid pg_type = get_corr_postgres_type(chtype);

// 					vec->push_back(clickhouse::CreateColumnByType(col->Type()->GetName()));
// 					const char * colname = sample_block.GetColumnName(i).c_str();

// 					/* we can't afford long jumps outside of this function */
// 					PG_TRY();
// 					{
// 						TupleDescInitEntry(
// 							state->outdesc, (AttrNumber)i + 1, colname, pg_type, -1, 0);
// 					}
// 					PG_CATCH();
// 					{
// 						error = true;
// 					}
// 					PG_END_TRY();

// 					if (error)
// 						throw std::runtime_error("could not init tuple descriptor");
// 				}

// 				return true;
// 			});
// 	}
// 	catch (const std::exception & e)
// 	{
//         client->ResetConnection();

// 		if (vec != nullptr)
// 			delete vec;

// 		elog(ERROR, "clickhouse_fdw: error while insert preparation - %s", e.what());
// 	}

// 	if (vec != nullptr)
// 		state->columns = (void *)vec;
}

static void column_append(clickhouse::ColumnRef col, Datum val, Oid valtype, bool isnull)
{
	bool nullable = false;

	if (col->Type()->GetCode() == Type::Code::Nullable)
		nullable = true;

	if (isnull && !nullable)
		throw std::runtime_error(
			"unexpected column "
			"type for NULL: "
			+ col->Type()->GetName());

	if (nullable)
	{
		auto nullable = col->As<ColumnNullable>();
		nullable->Append(isnull);
		col = nullable->Nested();
	}

	switch (valtype)
	{
		case INT2OID: {
			switch (col->Type()->GetCode())
			{
				case Type::Code::UInt8:
					col->As<ColumnUInt8>()->Append((uint8_t)val);
					break;
				case Type::Code::Int8:
					col->As<ColumnInt8>()->Append((int8_t)val);
					break;
				case Type::Code::Int16:
					col->As<ColumnInt16>()->Append((int16_t)val);
					break;
				default:
					throw std::runtime_error(
						"clickhouse_fdw: unexpected column "
						"type for INT2: "
						+ col->Type()->GetName());
			}
			break;
		}
		case INT4OID: {
			switch (col->Type()->GetCode())
			{
				case Type::Code::Int32:
					col->As<ColumnInt32>()->Append((int32_t)val);
					break;
				case Type::Code::UInt16:
					col->As<ColumnUInt16>()->Append((uint16_t)val);
					break;
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for INT4: "
						+ col->Type()->GetName());
			}
			break;
		}
		case INT8OID: {
			switch (col->Type()->GetCode())
			{
				case Type::Code::Int64:
					col->As<ColumnInt64>()->Append((int64_t)val);
					break;
				case Type::Code::UInt32:
					col->As<ColumnUInt32>()->Append((uint32_t)val);
					break;
				case Type::Code::UInt64:
					col->As<ColumnUInt64>()->Append((uint64_t)val);
					break;
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for INT8: "
						+ col->Type()->GetName());
			}
			break;
		}
		case FLOAT4OID: {
			switch (col->Type()->GetCode())
			{
				case Type::Code::Float32:
					col->As<ColumnFloat32>()->Append(DatumGetFloat4(val));
					break;
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for FLOAT4: "
						+ col->Type()->GetName());
			}
			break;
		}
		case FLOAT8OID: {
			switch (col->Type()->GetCode())
			{
				case Type::Code::Float64:
					col->As<ColumnFloat64>()->Append(DatumGetFloat8(val));
					break;
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for FLOAT8: "
						+ col->Type()->GetName());
			}
			break;
		}
		case TEXTOID: {
			char * s = TextDatumGetCString(val);

			switch (col->Type()->GetCode())
			{
				case Type::Code::FixedString:
					col->As<ColumnFixedString>()->Append(s);
					break;
				case Type::Code::String:
					col->As<ColumnString>()->Append(s);
					break;
				case Type::Code::Enum8:
					col->As<ColumnEnum8>()->Append(s);
					break;
				case Type::Code::Enum16:
					col->As<ColumnEnum16>()->Append(s);
					break;
				case Type::Code::LowCardinality: {
					// XXX Figure out proper value to create and pass to
					// Append.
					throw std::runtime_error(
						"clickhouse_fdw: XXX unsupported column type "
						+ col->Type()->GetName()
					);
					// auto item = ItemView{Type::String, std::string_view(s)};
					// col->As<ColumnLowCardinality>()->Append(item);
					break;
				}
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for TEXT: "
						+ col->Type()->GetName());
			}

			break;
		}
		case DATEOID: {
			Timestamp t = date2timestamp_no_overflow(DatumGetDateADT(val));
			pg_time_t d = timestamptz_to_time_t(t);

			switch (col->Type()->GetCode())
			{
				case Type::Code::Date:
					col->As<ColumnDate>()->Append(d);
					break;
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for DATE: "
						+ col->Type()->GetName());
			}
			break;
		}
		case TIMESTAMPOID: {
			switch (col->Type()->GetCode())
			{
				case Type::Code::DateTime: {
					pg_time_t d = timestamptz_to_time_t(DatumGetTimestamp(val));
					col->As<ColumnDateTime>()->Append(d);
					break;
				}
				case Type::Code::DateTime64: {
					auto dt64_col = col->As<ColumnDateTime64>();
					Timestamp t = DatumGetTimestamp(val);
					Int64 dt64 = ((1.0 * t) / USECS_PER_SEC
								  + ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY))
						* pow(10.0, dt64_col->GetPrecision());

					dt64_col->Append(dt64);
					break;
				}
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for TIMESTAMPOID: "
						+ col->Type()->GetName());
			}
			break;
		}
		case ANYARRAYOID: {
			// auto arr = (ch_binary_array_t *)DatumGetPointer(val);

			switch (col->Type()->GetCode())
			{
				case Type::Array: {
					// XXX Figure out proper value to create and pass to
					// Append.
					throw std::runtime_error(
						"clickhouse_fdw: XXX unsupported column type "
						+ col->Type()->GetName()
					);
					// auto arrcol = col->As<ColumnArray>();

					// arrcol->OffsetsIncrease(arr->len);
					// for (size_t i = 0; i < arr->len; i++)
					// 	column_append(
					// 		arrcol->Nested(), arr->datums[i], arr->item_type, arr->nulls[i]);

					// break;
				}
				default:
					throw std::runtime_error(
						"unexpected column "
						"type for array: "
						+ col->Type()->GetName());
			}
			break;
		}
		default: {
			throw std::runtime_error(
				"unexpected type " + std::to_string(valtype)
				+ " type for : " + col->Type()->GetName());
		}
	}
}

void ch_binary_column_append_data(ch_binary_insert_state * state, size_t colidx)
{
	try
	{
		auto columns = *(std::vector<clickhouse::ColumnRef> *)state->columns;
		auto col = columns[colidx];

		Datum val = state->values[colidx];
		Oid valtype = TupleDescAttr(state->outdesc, colidx)->atttypid;
		bool isnull = state->nulls[colidx];

		column_append(col, val, valtype, isnull);
	}
	catch (const std::exception & e)
	{
		elog(ERROR, "clickhouse_fdw: could not append data to column - %s", e.what());
	}
}

void ch_binary_insert_columns(ch_binary_insert_state * state)
{
	try
	{
		Block block;
		auto columns = *(std::vector<clickhouse::ColumnRef> *)state->columns;
		for (int i = 0; i < state->outdesc->natts; ++i)
		{
			Form_pg_attribute att = TupleDescAttr(state->outdesc, i);
			block.AppendColumn(NameStr(att->attname), columns[i]);
		}

		Client * client = (Client *)state->conn->client;
		client->Insert(state->table_name, block);
	}
	catch (const std::exception & e)
	{
		elog(ERROR, "clickhouse_fdw: could not insert columns - %s", e.what());
	}
}

void ch_binary_close(ch_binary_connection_t * conn)
{
	delete (Client *)conn->client;
	delete (ClientOptions *)conn->options;
}

void ch_binary_response_free(ch_binary_response_t * resp)
{
	if (resp->values)
	{
		auto values = (std::vector<std::vector<clickhouse::ColumnRef>> *)resp->values;
		values->clear();
		delete values;
	}

	if (resp->error)
		free(resp->error);

	delete resp;
}

void ch_binary_read_state_init(ch_binary_read_state_t * state, ch_binary_response_t * resp)
{
	state->resp = resp;
	state->block = 0;
	state->row = 0;
	state->done = false;
	state->error = NULL;
	state->coltypes = NULL;
	state->values = NULL;
	state->nulls = NULL;

	/* it response was errored just set error in state too */
	if (resp->error)
	{
		state->done = true;
		set_state_error(state, resp->error);
		return;
	}

	try
	{
		assert(resp->values);
		auto & values = *((std::vector<std::vector<clickhouse::ColumnRef>> *)resp->values);

		if (resp->columns_count && values.size() > 0)
		{
			state->coltypes = new Oid[resp->columns_count];
			state->values = new Datum[resp->columns_count];
			state->nulls = new bool[resp->columns_count];
		}
	}
	catch (const std::exception & e)
	{
		set_state_error(state, e.what());
	}
}

/*
 * This function is preparing values for `convert_datum` which is called in upper
 * code.
 *
 * This function calls postgres functions, which can call `palloc` so we can end up
 * with elog(ERROR) and longjmp to upper postgres code with leaking c++ memory.
 *
 * There is no an adequate (without huge overheads) solution, we just consider
 * this state unfixable.
 */
static Datum make_datum(clickhouse::ColumnRef col, size_t row, Oid * valtype, bool * is_null)
{
	Datum ret = (Datum)0;

nested_col:
	auto type_code = col->Type()->GetCode();

	*valtype = InvalidOid;
	*is_null = false;

	switch (type_code)
	{
		case Type::Code::UInt8: {
			int16 val = col->As<ColumnUInt8>()->At(row);
			ret = (Datum)val;
			*valtype = INT2OID;
		}
		break;
		case Type::Code::UInt16: {
			int16 val = col->As<ColumnUInt16>()->At(row);
			ret = (Datum)val;
			*valtype = INT4OID;
		}
		break;
		case Type::Code::UInt32: {
			int64 val = col->As<ColumnUInt32>()->At(row);
			ret = Int64GetDatum(val);
			*valtype = INT8OID;
		}
		break;
		case Type::Code::UInt64: {
			uint64 val = col->As<ColumnUInt64>()->At(row);
			if (val > LONG_MAX)
				throw std::overflow_error("clickhouse_fdw: int64 overflow");

			ret = Int64GetDatum((int64)val);
			*valtype = INT8OID;
		}
		break;
		case Type::Code::Int8: {
			int16 val = col->As<ColumnInt8>()->At(row);
			ret = (Datum)val;
			*valtype = INT2OID;
		}
		break;
		case Type::Code::Int16: {
			int16 val = col->As<ColumnInt16>()->At(row);
			ret = (Datum)val;
			*valtype = INT2OID;
		}
		break;
		case Type::Code::Int32: {
			int val = col->As<ColumnInt32>()->At(row);
			ret = (Datum)val;
			*valtype = INT4OID;
		}
		break;
		case Type::Code::Int64: {
			int64 val = col->As<ColumnInt64>()->At(row);
			ret = Int64GetDatum(val);
			*valtype = INT8OID;
		}
		break;
		case Type::Code::Float32: {
			float val = col->As<ColumnFloat32>()->At(row);
			ret = Float4GetDatum(val);
			*valtype = FLOAT4OID;
		}
		break;
		case Type::Code::Float64: {
			double val = col->As<ColumnFloat64>()->At(row);
			ret = Float8GetDatum(val);
			*valtype = FLOAT8OID;
		}
		break;
		case Type::Code::FixedString: {
			auto s = std::string(col->As<ColumnFixedString>()->At(row));
			ret = CStringGetTextDatum(s.c_str());
			*valtype = TEXTOID;
		}
		break;
		case Type::Code::String: {
			auto s = std::string(col->As<ColumnString>()->At(row));
			ret = CStringGetTextDatum(s.c_str());
			*valtype = TEXTOID;
		}
		break;
		case Type::Code::Enum8: {
			auto s = std::string(col->As<ColumnEnum8>()->NameAt(row));
			ret = CStringGetTextDatum(s.c_str());
			*valtype = TEXTOID;
		}
		break;
		case Type::Code::Enum16: {
			auto s = std::string(col->As<ColumnEnum16>()->NameAt(row));
			ret = CStringGetTextDatum(s.c_str());
			*valtype = TEXTOID;
		}
		break;
		case Type::Code::Date: {
			auto val = static_cast<pg_time_t>(col->As<ColumnDate>()->At(row));
			*valtype = DATEOID;

			if (val == 0)
				/* clickhouse special case */
				*is_null = true;
			else
			{
				Timestamp t = (Timestamp)time_t_to_timestamptz(val);
				ret = TimestampGetDatum(t);
			}
		}
		break;
		case Type::Code::DateTime: {
			auto val = static_cast<pg_time_t>(col->As<ColumnDateTime>()->At(row));
			*valtype = TIMESTAMPOID;

			if (val == 0)
				*is_null = true;
			else
			{
				Timestamp t = (Timestamp)time_t_to_timestamptz(val);
				ret = TimestampGetDatum(t);
			}
		}
		break;
		case Type::Code::DateTime64: {
			auto dt_col = col->As<ColumnDateTime64>();
			auto val = dt_col->At(row);

			*valtype = TIMESTAMPOID;

			if (val == 0)
				*is_null = true;
			else
			{
				ret = ((1.0 * val) / pow(10, dt_col->GetPrecision())
					   - (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)
					* USECS_PER_SEC;
			}
		}
		break;
		case Type::Code::UUID: {
			/* we form char[16] from two uint64 numbers, and they should
			 * be big endian */
			auto val = col->As<ColumnUUID>()->At(row);
			pg_uuid_t * uuid_val = (pg_uuid_t *)exc_palloc(sizeof(pg_uuid_t));

			val.first = HOST_TO_BIG_ENDIAN_64(val.first);
			val.second = HOST_TO_BIG_ENDIAN_64(val.second);
			memcpy(uuid_val->data, &val.first, 8);
			memcpy(uuid_val->data + 8, &val.second, 8);

			ret = UUIDPGetDatum(uuid_val);
			*valtype = UUIDOID;
		}
		break;
		case Type::Code::Nullable: {
			auto nullable = col->As<ColumnNullable>();
			if (nullable->IsNull(row))
			{
				*is_null = true;
			}
			else
			{
				col = nullable->Nested();
				goto nested_col;
			}
		}
		break;
		case Type::Code::Array: {
			auto arr = col->As<ColumnArray>()->GetAsColumn(row);
			size_t len = arr->Size();
			auto slot = (ch_binary_array_t *)exc_palloc(sizeof(ch_binary_array_t));

			Oid item_type = get_corr_postgres_type(arr->Type());
			Oid array_type = get_array_type(item_type);

			if (array_type == InvalidOid)
				throw std::runtime_error(
					std::string("clickhouse_fdw: could not") + " find array type for "
					+ std::to_string(item_type));

			slot->len = len;
			slot->array_type = array_type;
			slot->item_type = item_type;

			if (len > 0)
			{
				slot->datums = (Datum *)exc_palloc0(sizeof(Datum) * len);
				slot->nulls = (bool *)exc_palloc0(sizeof(bool) * len);

				for (size_t i = 0; i < len; ++i)
					slot->datums[i] = make_datum(arr, i, &slot->item_type, &slot->nulls[i]);
			}

			/* this one will need additional work, since we just return raw slot */
			ret = PointerGetDatum(slot);
			*valtype = ANYARRAYOID;
		}
		break;
		case Type::Code::Tuple: {
			auto tuple = col->As<ColumnTuple>();
			auto len = tuple->TupleSize();

			if (len == 0)
				throw std::runtime_error("clickhouse_fdw: returned tuple is empty");

			auto slot = (ch_binary_tuple_t *)exc_palloc(sizeof(ch_binary_tuple_t));

			slot->datums = (Datum *)exc_palloc(sizeof(Datum) * len);
			slot->nulls = (bool *)exc_palloc0(sizeof(bool) * len);
			slot->types = (Oid *)exc_palloc0(sizeof(Oid) * len);
			slot->len = len;

			for (size_t i = 0; i < len; ++i)
			{
				auto tuple_col = (*tuple)[i];

				slot->datums[i] = make_datum(tuple_col, row, &slot->types[i], &slot->nulls[i]);
			}

			/* this one will need additional work, since we just return raw slot */
			ret = PointerGetDatum(slot);
			*valtype = RECORDOID;
		}
		break;
		case Type::Code::LowCardinality: {
			auto item = col->As<ColumnLowCardinality>()->GetItem(row);
			auto data = item.AsBinaryData();
			ret = PointerGetDatum(cstring_to_text_with_len(data.data(), data.size()));
			*valtype = TEXTOID;
		}
		break;
        case Type::Code::IPv4: {
			auto item = col->As<ColumnIPv4>()->AsString(row);
            ret = DirectFunctionCall1(inet_in, CStringGetDatum(item.c_str()));
            *valtype = INETOID;
        }
        break;
        case Type::Code::IPv6: {
			auto item = col->As<ColumnIPv6>()->AsString(row);
            ret = DirectFunctionCall1(inet_in, CStringGetDatum(item.c_str()));
            *valtype = INETOID;
        }
        break;
		default:
			throw std::runtime_error("unsupported type in binary protocol");
	}

	return ret;
}

bool ch_binary_read_row(ch_binary_read_state_t * state)
{
	/* coltypes is NULL means there are no blocks */
	bool res = false;

	if (state->done || state->coltypes == NULL || state->error)
		return false;

	assert(state->resp->values);
	auto & values = *((std::vector<std::vector<clickhouse::ColumnRef>> *)state->resp->values);
	try
	{
	again:
		assert(state->block < state->resp->blocks_count);
		auto & block = values[state->block];
		size_t row_count = block[0]->Size();

		if (row_count == 0)
			goto next_row;

		for (size_t i = 0; i < state->resp->columns_count; i++)
		{
			/* fill value and null arrays */
			state->values[i]
				= make_datum(block[i], state->row, &state->coltypes[i], &state->nulls[i]);
		}
		res = true;

	next_row:
		state->row++;
		if (state->row >= row_count)
		{
			state->row = 0;
			state->block++;
			if (state->block >= state->resp->blocks_count)
				state->done = true;
			else if (row_count == 0)
				goto again;
		}
	}
	catch (const std::exception & e)
	{
		set_state_error(state, e.what());
	}

	return res;
}

void ch_binary_read_state_free(ch_binary_read_state_t * state)
{
	if (state->coltypes)
	{
		delete[] state->coltypes;
		delete[] state->values;
		delete[] state->nulls;
	}

	if (state->error)
		free(state->error);
}
}
