#include <memory>
#include <new>
#include <string>

#include <clickhouse/client.h>
#include <clickhouse/query.h>

#include "minicoro.h"

extern "C"
{

#include "postgres.h"
#include "internal.h"
#include "engine.h"

}

#include "binary_internal.hh"

using namespace clickhouse;

static constexpr size_t kBinaryStreamingStackSize = 1024 * 1024;
static const char kBinaryStreamingCanceled[] = "query was canceled";
static const char kBinaryStreamingOom[] = "out of memory";

struct ch_binary_streaming_state
{
	mco_coro   *co = nullptr;

	/* Current block yielded by the coroutine. Valid until the next resume. */
	Block	   *current_block = nullptr;
	size_t		current_row = 0;
	bool		have_block = false;
	bool		done = false;

	std::unique_ptr<Oid[]> coltypes;
	std::unique_ptr<Datum[]> values;
	std::unique_ptr<bool[]> nulls;
	size_t		columns_count = 0;

	char	   *error = nullptr;
	bool		error_owned = false;
	bool		(*check_cancel) (void) = nullptr;

	Client	   *client = nullptr;
	std::string	sql;
	QuerySettings settings;
	QueryParams	params;

	~ch_binary_streaming_state()
	{
		if (error && error_owned)
			free(error);
	}

	void
	SetStaticError(const char *message)
	{
		if (error)
			return;
		error = const_cast<char *>(message);
		error_owned = false;
	}

	void
	SetOwnedError(const char *message)
	{
		char	   *copy;

		if (error)
			return;
		if (!message)
		{
			SetStaticError(kBinaryStreamingOom);
			return;
		}

		copy = strdup(message);
		if (!copy)
		{
			SetStaticError(kBinaryStreamingOom);
			return;
		}

		error = copy;
		error_owned = true;
	}
};

static bool
binary_streaming_resume(ch_binary_streaming_state * st)
{
	mco_result	res;

	res = mco_resume(st->co);
	if (res == MCO_SUCCESS)
		return true;

	st->SetOwnedError(mco_result_description(res));
	st->done = true;
	return false;
}

/*
 * Coroutine function: runs Client::Select, yielding each block.
 */
static void
binary_streaming_coro(mco_coro * co)
{
	ch_binary_streaming_state *st =
		(ch_binary_streaming_state *) mco_get_user_data(co);

	try
	{
		st->client->Select(
			clickhouse::Query(st->sql)
			.SetQuerySettings(st->settings)
			.SetParams(st->params)
			.OnDataCancelable(
							  [st, co](const Block & block) -> bool
		{
			if (st->check_cancel && st->check_cancel())
			{
				st->SetStaticError(kBinaryStreamingCanceled);
				return false;
			}

			if (block.GetColumnCount() == 0)
				return true;

			st->current_block = const_cast<Block *>(&block);
			st->columns_count = block.GetColumnCount();
			st->have_block = true;
			st->current_row = 0;

			mco_yield(co);
			return true;
		}));
	}
	catch (const std::exception & e)
	{
		st->SetOwnedError(e.what());
	}

	st->have_block = false;
	st->done = true;
}

extern "C"
{

	ch_binary_streaming_state *
	ch_binary_begin_streaming(ch_binary_connection_t * conn,
							  const ch_query * query,
							  bool (*check_cancel) (void))
	{
		ch_binary_streaming_state *st;
		mco_desc	desc;
		mco_result	res;

		st = new (std::nothrow) ch_binary_streaming_state();
		if (!st)
			return NULL;

		st->check_cancel = check_cancel;
		st->client = (Client *) conn->client;
		st->sql = query->sql;
		st->settings = ch_binary_settings(query);
		st->params = ch_binary_params(query);

		desc = mco_desc_init(binary_streaming_coro, kBinaryStreamingStackSize);
		desc.user_data = st;
		res = mco_create(&st->co, &desc);
		if (res != MCO_SUCCESS)
		{
			st->SetOwnedError(mco_result_description(res));
			st->done = true;
			return st;
		}

		if (!binary_streaming_resume(st))
			return st;

		return st;
	}

	bool
	ch_binary_fetch_block(ch_binary_streaming_state * st)
	{
		if (!st || st->done)
			return false;
		if (st->have_block)
			return true;
		if (mco_status(st->co) != MCO_SUSPENDED)
			return false;

		if (!binary_streaming_resume(st))
			return false;

		return st->have_block;
	}

	bool
	ch_binary_streaming_read_row(ch_binary_streaming_state * st)
	{
		Block	   *block;
		size_t		row_count;

		if (!st || !st->have_block || !st->current_block)
			return false;

		block = st->current_block;
		row_count = block->GetRowCount();
		if (st->current_row >= row_count)
		{
			st->have_block = false;
			return false;
		}

		if (!st->coltypes && st->columns_count > 0)
		{
			st->coltypes.reset(new (std::nothrow) Oid[st->columns_count]);
			st->values.reset(new (std::nothrow) Datum[st->columns_count]);
			st->nulls.reset(new (std::nothrow) bool[st->columns_count]);
			if (!st->coltypes || !st->values || !st->nulls)
			{
				st->SetStaticError(kBinaryStreamingOom);
				return false;
			}
		}

		try
		{
			for (size_t i = 0; i < st->columns_count; i++)
			{
				st->values[i] = ch_binary_make_datum((*block)[i], st->current_row,
													 &st->coltypes[i], &st->nulls[i]);
			}
		}
		catch (const std::exception & e)
		{
			st->SetOwnedError(e.what());
			return false;
		}

		st->current_row++;
		return true;
	}

	size_t
	ch_binary_streaming_columns(ch_binary_streaming_state * st)
	{
		return st ? st->columns_count : 0;
	}

	Datum
	ch_binary_streaming_value(ch_binary_streaming_state * st, size_t col,
							  Oid * valtype, bool * is_null)
	{
		if (!st || col >= st->columns_count)
		{
			*is_null = true;
			*valtype = InvalidOid;
			return (Datum) 0;
		}

		*valtype = st->coltypes[col];
		*is_null = st->nulls[col];
		return st->values[col];
	}

	char *
	ch_binary_streaming_error(ch_binary_streaming_state * st)
	{
		return st ? st->error : NULL;
	}

	void
	ch_binary_end_streaming(ch_binary_streaming_state * st)
	{
		if (!st)
			return;

		if (st->co)
		{
			bool		was_suspended = mco_status(st->co) == MCO_SUSPENDED;

			mco_destroy(st->co);
			st->co = NULL;

			if (was_suspended)
				st->client->ResetConnection();
		}

		delete st;
	}

}
