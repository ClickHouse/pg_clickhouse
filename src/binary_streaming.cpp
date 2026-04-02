#include <memory>
#include <new>
#include <optional>
#include <string>

#include <clickhouse/client.h>
#include <clickhouse/query.h>

extern "C"
{

#include "postgres.h"
#include "internal.h"
#include "engine.h"

}

#include "binary_internal.hh"

using namespace clickhouse;

static const char kBinaryStreamingCanceled[] = "query was canceled";
static const char kBinaryStreamingOom[] = "out of memory";

struct ch_binary_streaming_state
{
	/* Current block returned by clickhouse-cpp. */
	std::optional<Block> current_block;
	size_t		current_row = 0;
	bool		have_block = false;
	bool		done = false;

	std::unique_ptr<Oid[]> coltypes;
	std::unique_ptr<Datum[]> values;
	std::unique_ptr<bool[]> nulls;
	size_t		columns_count = 0;

	std::optional<std::string> error;
	bool		(*check_cancel) (void) = nullptr;

	Client	   *client = nullptr;
	std::string	sql;
	QuerySettings settings;
	QueryParams	params;

	void
	SetError(const char *message)
	{
		if (error)
			return;
		error.emplace(message ? message : kBinaryStreamingOom);
	}

	const char *
	GetError() const
	{
		return error ? error->c_str() : nullptr;
	}
};

static bool
binary_streaming_fill_block(ch_binary_streaming_state * st)
{
	std::optional<Block> block;

	for (;;)
	{
		try
		{
			if (st->check_cancel && st->check_cancel())
			{
				st->SetError(kBinaryStreamingCanceled);
				st->done = true;
				return false;
			}

			block = st->client->ReceiveSelectBlock();
		}
		catch (const std::exception & e)
		{
			st->SetError(e.what());
			st->done = true;
			return false;
		}

		if (!block)
		{
			try
			{
				st->client->EndSelect();
			}
			catch (const std::exception & e)
			{
				st->SetError(e.what());
			}
			st->current_block.reset();
			st->have_block = false;
			st->done = true;
			return false;
		}

		/* Match the old callback path, which ignored zero-column blocks. */
		if (block->GetColumnCount() == 0)
			continue;

		if (st->columns_count != 0 &&
			block->GetColumnCount() != st->columns_count)
		{
			st->SetError("columns mismatch in blocks");
			st->done = true;
			return false;
		}

		st->current_block = std::move(block);
		st->columns_count = st->current_block->GetColumnCount();
		st->have_block = true;
		st->current_row = 0;
		return true;
	}
}

extern "C"
{

	ch_binary_streaming_state *
	ch_binary_begin_streaming(ch_binary_connection_t * conn,
							  const ch_query * query,
							  bool (*check_cancel) (void))
	{
		ch_binary_streaming_state *st;

		st = new (std::nothrow) ch_binary_streaming_state();
		if (!st)
			return NULL;

		st->check_cancel = check_cancel;
		st->client = (Client *) conn->client;
		st->sql = query->sql;
		st->settings = ch_binary_settings(query);
		st->params = ch_binary_params(query);

		try
		{
			st->client->BeginSelect(clickhouse::Query(st->sql)
									.SetQuerySettings(st->settings)
									.SetParams(st->params));
		}
		catch (const std::exception & e)
		{
			st->SetError(e.what());
			st->done = true;
			return st;
		}

		(void) binary_streaming_fill_block(st);

		return st;
	}

	bool
	ch_binary_fetch_block(ch_binary_streaming_state * st)
	{
		if (!st || st->done)
			return false;
		if (st->have_block)
			return true;

		return binary_streaming_fill_block(st);
	}

	bool
	ch_binary_streaming_read_row(ch_binary_streaming_state * st)
	{
		Block	   *block;
		size_t		row_count;

		if (!st || !st->have_block || !st->current_block)
			return false;

		block = &*st->current_block;
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
				st->SetError(kBinaryStreamingOom);
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
			st->SetError(e.what());
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

	const char *
	ch_binary_streaming_error(ch_binary_streaming_state * st)
	{
		return st ? st->GetError() : NULL;
	}

	void
	ch_binary_end_streaming(ch_binary_streaming_state * st)
	{
		if (!st)
			return;

		try
		{
			if (st->client)
				st->client->EndSelect();
		}
		catch (const std::exception &)
		{
			try
			{
				if (st->client)
					st->client->ResetConnection();
			}
			catch (const std::exception &)
			{
			}
		}

		delete st;
	}

}
