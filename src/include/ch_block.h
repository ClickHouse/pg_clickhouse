/*
 * ch_block.h
 *
 * Pure C representation of a decoded ClickHouse block. Produced by the C++
 * wire layer in binary.cpp from a clickhouse-cpp Block, consumed by the
 * pure-C binary_decode.c which builds PG Datums without touching C++.
 *
 * Mirrors src/local/native_types.h + src/local/native.h on the `local`
 * branch so the two can converge once both land (see PLAN_localcpp.md).
 */

#ifndef PG_CLICKHOUSE_CH_BLOCK_H
#define PG_CLICKHOUSE_CH_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef enum ch_type_kind
	{
		CH_T_UNKNOWN = 0,

		/* Nothing/Void (ClickHouse type of bare NULL constants) */
		CH_T_VOID,

		/* fixed-width primitives */
		CH_T_INT8,
		CH_T_INT16,
		CH_T_INT32,
		CH_T_INT64,
		CH_T_INT128,
		CH_T_INT256,
		CH_T_UINT8,
		CH_T_UINT16,
		CH_T_UINT32,
		CH_T_UINT64,
		CH_T_UINT128,
		CH_T_UINT256,
		CH_T_FLOAT32,
		CH_T_FLOAT64,
		CH_T_BFLOAT16,
		CH_T_BOOL,

		/* date/time */
		CH_T_DATE,
		CH_T_DATE32,
		CH_T_DATETIME,
		CH_T_DATETIME64,

		/* network */
		CH_T_UUID,
		CH_T_IPV4,
		CH_T_IPV6,

		/* numeric / decimal */
		CH_T_DECIMAL32,
		CH_T_DECIMAL64,
		CH_T_DECIMAL128,
		CH_T_DECIMAL256,

		/* enums */
		CH_T_ENUM8,
		CH_T_ENUM16,

		/* strings */
		CH_T_STRING,
		CH_T_FIXEDSTRING,

		/* composites */
		CH_T_NULLABLE,
		CH_T_ARRAY,
		CH_T_TUPLE,
		CH_T_MAP,
		CH_T_LOWCARDINALITY
	}			ch_type_kind;

	typedef struct ch_enum_entry
	{
		int64_t		value;
		const char *name;
		size_t		name_len;
	}			ch_enum_entry;

	typedef struct ch_type
	{
		ch_type_kind kind;

		/*
		 * recursive children (Array(T), Tuple(...), Map(K,V), Nullable(T),
		 * ...)
		 */
		struct ch_type **child;
		size_t		n_child;

		/* optional per-child name */
		char	  **child_name;

		uint32_t	fixed_size; /* FixedString(N) */
		uint32_t	decimal_precision;
		uint32_t	decimal_scale;
		uint32_t	scale;		/* DateTime64 */
		char	   *tz;			/* DateTime[64] */

		ch_enum_entry *enum_dict;
		size_t		enum_dict_len;
	}			ch_type;

	typedef struct ch_block_column
	{
		char	   *name;
		ch_type    *type;		/* Nullable stripped, see is_nullable */
		bool		is_nullable;
		uint64_t	num_rows;
		const		uint8_t *nulls; /* num_rows bytes or NULL */

		union
		{
			const		uint8_t *raw;	/* fixed-width / FixedString */

			struct
			{
				const		uint64_t *offsets;	/* cumulative end offsets */
				const		uint8_t *data;
			}			str;

			struct
			{
				const		uint64_t *offsets;
				struct ch_block_column *inner;
			}			arr;

			struct
			{
				struct ch_block_column *fields;
			}			tup;
		}			d;
	}			ch_block_column;

	typedef struct ch_block
	{
		uint64_t	num_columns;
		uint64_t	num_rows;
		ch_block_column *columns;
		void	   *arena;		/* opaque; owned by producer (response) */
	}			ch_block;

	/*
	 * No-op for blocks materialized from a ch_binary_response (the response
	 * owns the storage). Kept as an explicit symbol so future producers can
	 * own the arena themselves.
	 */
	extern void ch_block_free(ch_block * blk);

#ifdef __cplusplus
}
#endif

#endif							/* PG_CLICKHOUSE_CH_BLOCK_H */
