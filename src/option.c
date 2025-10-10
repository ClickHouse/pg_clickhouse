/*-------------------------------------------------------------------------
 *
 * option.c
 *		  FDW option handling for clickhouse_fdw
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/clickhouse_fdw/src/option.c
*
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "utils/builtins.h"
#include "utils/varlena.h"

static char *DEFAULT_DBNAME = "default";

/*
 * Describes the valid options for objects that this wrapper uses.
 */
typedef struct ChFdwOption
{
	const char *keyword;
	Oid			optcontext;		/* OID of catalog in which option may appear */
	bool		is_ch_opt;	  /* true if it's used in clickhouseclient */
	char    dispchar[10];
} ChFdwOption;

/*
 * Valid options for clickhouse_fdw.
 * Allocated and filled in InitChFdwOptions.
 */
static ChFdwOption *clickhouse_fdw_options;

/*
 * Valid options for clickhouse client.
 * Allocated and filled in InitChFdwOptions.
 */
static const ChFdwOption ch_options[] =
{
	{"host", 0, false},
	{"port", 0, false},
	{"dbname", 0, false},
	{"user", 0, false},
	{"password", 0, false},
	{NULL}
};

/*
 * Helper functions
 */
static void InitChFdwOptions(void);
static bool is_valid_option(const char *keyword, Oid context);
static bool is_ch_option(const char *keyword);

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses clickhouse_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
PG_FUNCTION_INFO_V1(clickhouse_fdw_validator);

Datum
clickhouse_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *cell;

	/* Build our options lists if we didn't yet. */
	InitChFdwOptions();

	/*
	 * Check that only options supported by clickhouse_fdw, and allowed for the
	 * current object type, are given.
	 */
	foreach (cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			ChFdwOption *opt;
			StringInfoData buf;

			initStringInfo(&buf);
			for (opt = clickhouse_fdw_options; opt->keyword; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->keyword);
			}

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s",
							 buf.data)));
		}
	}

	PG_RETURN_VOID();
}

/*
 * Initialize option lists.
 */
static void
InitChFdwOptions(void)
{
	int			num_ch_opts;
	const ChFdwOption *lopt;
	ChFdwOption *popt;

	/* non-clickhouseclient FDW-specific FDW options */
	static const ChFdwOption non_ch_options[] =
	{
		{"database", ForeignTableRelationId, false},
		{"table_name", ForeignTableRelationId, false},
		{"engine", ForeignTableRelationId, false},
		{"driver", ForeignServerRelationId, false},
		{"aggregatefunction", AttributeRelationId, false},
		{"simpleaggregatefunction", AttributeRelationId, false},
		{"column_name", AttributeRelationId, false},
		{"arrays", AttributeRelationId, false},
		{NULL, InvalidOid, false}
	};

	/* Prevent redundant initialization. */
	if (clickhouse_fdw_options)
	{
		return;
	}


	/* Count how many clickhouseclient options are available. */
	num_ch_opts = 0;
	for (lopt = ch_options; lopt->keyword; lopt++)
	{
		num_ch_opts++;
	}

	/*
	 * We use plain malloc here to allocate clickhouse_fdw_options because it
	 * lives as long as the backend process does.  Besides, keeping
	 * ch_options in memory allows us to avoid copying every keyword
	 * string.
	 */
	clickhouse_fdw_options = (ChFdwOption *) malloc(sizeof(
								   ChFdwOption) * num_ch_opts + sizeof(non_ch_options));
	if (clickhouse_fdw_options == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	popt = clickhouse_fdw_options;
	for (lopt = ch_options; lopt->keyword; lopt++)
	{
		/* We don't have to copy keyword string, as described above. */
		popt->keyword = lopt->keyword;

		/*
		 * "user" and any secret options are allowed only on user mappings.
		 * Everything else is a server option.
		 */
		if (strcmp(lopt->keyword, "user") == 0 ||
				strcmp(lopt->keyword, "password") == 0 ||
				strchr(lopt->dispchar, '*'))
		{
			popt->optcontext = UserMappingRelationId;
		}
		else
		{
			popt->optcontext = ForeignServerRelationId;
		}
		popt->is_ch_opt = true;

		popt++;
	}

	/* Append FDW-specific options and dummy terminator. */
	memcpy(popt, non_ch_options, sizeof(non_ch_options));
	popt->is_ch_opt = true;
	popt++;
}

/*
 * Check whether the given option is one of the valid clickhouse_fdw options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char *keyword, Oid context)
{
	ChFdwOption *opt;

	Assert(clickhouse_fdw_options);	/* must be initialized already */

	for (opt = clickhouse_fdw_options; opt->keyword; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->keyword, keyword) == 0)
		{
			return true;
		}
	}

	return false;
}

/*
 * Check whether the given option is one of the valid clickhouseclient options.
 */
static bool
is_ch_option(const char *keyword)
{
	ChFdwOption *opt;

	Assert(clickhouse_fdw_options);	/* must be initialized already */

	for (opt = clickhouse_fdw_options; opt->keyword; opt++)
	{
		if (opt->is_ch_opt && strcmp(opt->keyword, keyword) == 0)
		{
			return true;
		}
	}

	return false;
}

/*
 * Generate key-value arrays which include only clickhouseclient options from the
 * given list (which can contain any kind of options).  Caller must have
 * allocated large-enough arrays.  Returns number of options found.
 */
void
chfdw_extract_options(List *defelems, char **driver,  char **host, int *port,
                         char **dbname, char **username, char **password)
{
	ListCell   *lc;

	/* Build our options lists if we didn't yet. */
	InitChFdwOptions();

	foreach (lc, defelems)
	{
		DefElem *def = (DefElem *) lfirst(lc);

		if (driver && strcmp(def->defname, "driver") == 0)
			*driver = defGetString(def);

		if (is_ch_option(def->defname))
		{
			if (host && strcmp(def->defname, "host") == 0)
				*host = defGetString(def);
			else if (port && strcmp(def->defname, "port") == 0)
				*port = atoi(defGetString(def));
			else if (username && strcmp(def->defname, "user") == 0)
				*username = defGetString(def);
			else if (password && strcmp(def->defname, "password") == 0)
				*password = defGetString(def);
			else if (dbname && strcmp(def->defname, "dbname") == 0)
            {
				*dbname = defGetString(def);
                if (*dbname[0] == '\0')
                    *dbname = DEFAULT_DBNAME;
            }
		}
	}
}
