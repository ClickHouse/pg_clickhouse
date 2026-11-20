/*-------------------------------------------------------------------------
 *
 * option.c
 *		  FDW option handling for pg_clickhouse
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/option.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fdw.h"
#include "kv_list.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/varlena.h"

static char* DEFAULT_DBNAME = "default";

#if PG_VERSION_NUM < 160000
extern PGDLLEXPORT void
_PG_init(void);
#endif

/*
 * Describes the valid options for objects that this wrapper uses.
 */
typedef struct ChFdwOption {
    const char* keyword;
    Oid optcontext; /* OID of catalog in which option may appear */
} ChFdwOption;

/*
 * Valid options for clickhouse_fdw.
 */
static const ChFdwOption clickhouse_fdw_options[] = {
    /* connection options */
    { "host",                    ForeignServerRelationId },
    { "secure",                  ForeignServerRelationId },
    { "min_tls_version",         ForeignServerRelationId },
    { "port",                    ForeignServerRelationId },
    { "dbname",                  ForeignServerRelationId },
    { "user",                    UserMappingRelationId   },
    { "password",                UserMappingRelationId   },
    { "compression",             ForeignServerRelationId },
    /* FDW-specific options */
    { "database",                ForeignTableRelationId  },
    { "table_name",              ForeignTableRelationId  },
    { "engine",                  ForeignTableRelationId  },
    { "driver",                  ForeignServerRelationId },
    { "encoding_check",          ForeignServerRelationId },
    { "settings",                ForeignServerRelationId },
    { "settings",                ForeignTableRelationId  },
    { "aggregatefunction",       AttributeRelationId     },
    { "simpleaggregatefunction", AttributeRelationId     },
    { "column_name",             AttributeRelationId     },
    { NULL,                      InvalidOid              }
};

/*
 * GUC parameters
 */
static char* ch_session_settings         = NULL;
static kv_list* ch_session_settings_list = NULL;
static bool ch_pushdown_regex            = true;
static bool ch_vector_planner_warnings   = true;

/*
 * Helper functions
 */
static bool
is_valid_option(const char* keyword, Oid context);
static bool
parse_min_tls_version(const char* val, tls_version* out);
static ch_driver
parse_driver(const char* val);
static tls_mode
parse_secure(const char* val);
static int
parse_port(const char* val);
static chc_compression
parse_compression(const char* val);

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses clickhouse_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
PG_FUNCTION_INFO_V1(clickhouse_fdw_validator);

Datum
clickhouse_fdw_validator(PG_FUNCTION_ARGS) {
    List* options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid catalog        = PG_GETARG_OID(1);
    ListCell* cell;

    /*
     * Check that only options supported by clickhouse_fdw, and allowed for
     * the current object type, are given.
     */
    foreach (cell, options_list) {
        DefElem* def = (DefElem*)lfirst(cell);

        if (!is_valid_option(def->defname, catalog)) {
            /*
             * Unknown option specified, complain about it. Provide a hint
             * with list of valid options for the object.
             */
            const ChFdwOption* opt;
            StringInfoData buf;

            initStringInfo(&buf);
            for (opt = clickhouse_fdw_options; opt->keyword; opt++) {
                if (catalog == opt->optcontext) {
                    appendStringInfo(
                        &buf, "%s%s", (buf.len > 0) ? ", " : "", opt->keyword
                    );
                }
            }

            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                errmsg("invalid option \"%s\"", def->defname),
                errhint("Valid options in this context are: %s", buf.data)
            );
        }

        if (strcmp(def->defname, "secure") == 0) {
            (void)parse_secure(defGetString(def));
        }

        if (strcmp(def->defname, "min_tls_version") == 0) {
            const char* val = defGetString(def);
            tls_version v;

            if (!parse_min_tls_version(val, &v)) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
                    errmsg("invalid value for option \"min_tls_version\": \"%s\"", val),
                    errhint("Valid values are: TLSv1, TLSv1.1, TLSv1.2, TLSv1.3.")
                );
            }
        }

        if (strcmp(def->defname, "port") == 0) {
            (void)parse_port(defGetString(def));
        }

        if (strcmp(def->defname, "driver") == 0) {
            (void)parse_driver(defGetString(def));
        }

        if (strcmp(def->defname, "compression") == 0) {
            (void)parse_compression(defGetString(def));
        }

        /* Check the syntax; ClickHouse checks the values */
        if (strcmp(def->defname, "settings") == 0) {
            chfdw_parse_options(defGetString(def));
        }

        if (strcmp(def->defname, "encoding_check") == 0) {
            const char* val = defGetString(def);
            pgch_encoding_check v;

            if (!pgch_parse_encoding_check(val, &v)) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
                    errmsg("invalid value for option \"encoding_check\": \"%s\"", val),
                    errhint("Valid values are: fail, truncate, remove, replace")
                );
            }
        }
    }

    PG_RETURN_VOID();
}

/*
 * Map a driver option value to ch_driver. Raises an ERROR if the value is
 * unrecognized.
 */
static ch_driver
parse_driver(const char* val) {
    if (strcmp(val, "binary") == 0) {
        return CH_DRIVER_BINARY;
    }
    if (strcmp(val, "http") == 0) {
        return CH_DRIVER_HTTP;
    }

    ereport(
        ERROR,
        errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
        errmsg("invalid value for option \"driver\": \"%s\"", val),
        errhint("Valid values are: binary, http.")
    );
}

/*
 * Map a secure option value to tls_mode. Raises an ERROR if the value is
 * unrecognized.
 */
static tls_mode
parse_secure(const char* val) {
    if (strcmp(val, "on") == 0 || strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 ||
        strcmp(val, "1") == 0) {
        return CH_TLS_ON;
    }
    if (strcmp(val, "off") == 0 || strcmp(val, "false") == 0 ||
        strcmp(val, "no") == 0 || strcmp(val, "0") == 0) {
        return CH_TLS_OFF;
    }
    if (strcmp(val, "auto") == 0) {
        return CH_TLS_AUTO;
    }

    ereport(
        ERROR,
        errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
        errmsg("invalid value for option \"secure\": \"%s\"", val),
        errhint("Valid values are: on, off, auto.")
    );
}

/*
 * Parse a port option value. Raises an ERROR unless the value is an integer
 * between 1 and 65535.
 */
static int
parse_port(const char* val) {
    char* end;
    long port = strtol(val, &end, 10);

    /* Decimal only: an out-of-range value saturates and fails the bounds. */
    if (end == val || *end != '\0' || port < 1 || port > 65535) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
            errmsg("invalid value for option \"port\": \"%s\"", val),
            errhint("Valid values are integers between 1 and 65535.")
        );
    }
    return port;
}

/*
 * Map a compression option value to chc_compression, case-insensitively. An
 * empty value means LZ4, ClickHouse's native network default. Raises an ERROR
 * if the value is unrecognized.
 */
static chc_compression
parse_compression(const char* val) {
    if (val[0] == '\0' || pg_strcasecmp(val, "lz4") == 0) {
        return CHC_COMP_LZ4;
    }
    if (pg_strcasecmp(val, "none") == 0) {
        return CHC_COMP_NONE;
    }
    if (pg_strcasecmp(val, "zstd") == 0) {
        return CHC_COMP_ZSTD;
    }

    ereport(
        ERROR,
        errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
        errmsg("invalid value for option \"compression\": \"%s\"", val),
        errhint("Valid values are: none, lz4, zstd.")
    );
}

/*
 * Map a min_tls_version option value to tls_version. Accepts the same spellings
 * as Postgres's ssl_min_protocol_version GUC, case-insensitively. Returns false
 * if the value is unrecognized.
 */
static bool
parse_min_tls_version(const char* val, tls_version* out) {
    if (pg_strcasecmp(val, "TLSv1") == 0) {
        *out = CH_TLS_V1_0;
    } else if (pg_strcasecmp(val, "TLSv1.1") == 0) {
        *out = CH_TLS_V1_1;
    } else if (pg_strcasecmp(val, "TLSv1.2") == 0) {
        *out = CH_TLS_V1_2;
    } else if (pg_strcasecmp(val, "TLSv1.3") == 0) {
        *out = CH_TLS_V1_3;
    } else {
        return false;
    }
    return true;
}

/*
 * Check whether the given option is one of the valid clickhouse_fdw options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char* keyword, Oid context) {
    const ChFdwOption* opt;

    for (opt = clickhouse_fdw_options; opt->keyword; opt++) {
        if (context == opt->optcontext && strcmp(opt->keyword, keyword) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * Extract the connection options from the given list (which can contain any
 * kind of options) into the non-NULL output parameters. Leaves an output
 * untouched when the list does not contain its option. Raises an ERROR for an
 * invalid driver, port, compression, or secure value.
 */
void
chfdw_extract_options(
    List* defelems,
    ch_driver* driver,
    char** host,
    int* port,
    char** dbname,
    char** username,
    char** password,
    chc_compression* compression,
    tls_mode* tls,
    tls_version* min_tls_version,
    pgch_encoding_check* encoding_check
) {
    ListCell* lc;

    foreach (lc, defelems) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (driver && strcmp(def->defname, "driver") == 0) {
            /* The validator rejects invalid values; older servers may not. */
            *driver = parse_driver(defGetString(def));
        } else if (encoding_check && strcmp(def->defname, "encoding_check") == 0) {
            /* invalid values rejected by the validator; ignore here */
            pgch_parse_encoding_check(defGetString(def), encoding_check);
        } else if (host && strcmp(def->defname, "host") == 0) {
            *host = defGetString(def);
        } else if (port && strcmp(def->defname, "port") == 0) {
            /* The validator rejects invalid values; older servers may not. */
            *port = parse_port(defGetString(def));
        } else if (username && strcmp(def->defname, "user") == 0) {
            *username = defGetString(def);
        } else if (password && strcmp(def->defname, "password") == 0) {
            *password = defGetString(def);
        } else if (compression && strcmp(def->defname, "compression") == 0) {
            /* The validator rejects invalid values; older servers may not. */
            *compression = parse_compression(defGetString(def));
        } else if (dbname && strcmp(def->defname, "dbname") == 0) {
            *dbname = defGetString(def);
            if (*dbname[0] == '\0') {
                *dbname = DEFAULT_DBNAME;
            }
        } else if (tls && strcmp(def->defname, "secure") == 0) {
            *tls = parse_secure(defGetString(def));
        } else if (min_tls_version && strcmp(def->defname, "min_tls_version") == 0) {
            /* invalid values rejected by the validator; ignore here */
            parse_min_tls_version(defGetString(def), min_tls_version);
        }
    }
}

/*
 * Parse ClickHouse settings as key/value pairs. Based on the Postgres
 * conninfo_parse() function. The format:
 *
 *     key value, key 'value',...
 *
 * Parameter names may not contain spaces, and support no escapes.
 *
 * Values may contain backslash-escaped spaces, backslashes, and commas. Use
 * SQL single-quoted literals to remove the need to escape commas and spaces.
 *
 * Returns a PostgreSQL List containing DefElem cells.
 */
List*
chfdw_parse_options(const char* options_string) {
    char* pname;
    char* pval;
    char* buf;
    char* cp;
    char* cp2;
    List* options = NIL;

    /* Need a modifiable copy of the input string */
    buf = pstrdup(options_string);
    cp  = buf;

    while (*cp) {
        /* Skip blanks before the parameter name */
        if (isspace((unsigned char)*cp)) {
            cp++;
            continue;
        }

        /* Get the parameter name */
        pname = cp;
        while (*cp) {
            if (isspace((unsigned char)*cp)) {
                *cp++ = '\0';
                while (*cp) {
                    if (!isspace((unsigned char)*cp)) {
                        break;
                    }
                    cp++;
                }
                break;
            }
            cp++;
        }

        /* Get the parameter value */
        pval = cp;

        if (*cp != '\'') {
            cp2 = pval;
            while (*cp) {
                if (isspace((unsigned char)*cp)) {
                    while (isspace((unsigned char)*cp)) {
                        cp++;
                    }

                    if (*cp != ',' && *cp != '\0') {
                        ereport(
                            ERROR,
                            errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg(
                                "pg_clickhouse: missing comma after \"%s\" value "
                                "in options string",
                                pname
                            )
                        );
                    }
                    break;
                }
                if (*cp == ',') {
                    *cp++ = '\0';
                    break;
                }
                if (*cp == '\\') {
                    cp++;
                    if (*cp != '\0') {
                        *cp2++ = *cp++;
                    }
                } else {
                    *cp2++ = *cp++;
                }
            }
            *cp2 = '\0';
            if (cp2 == pval) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg(
                        "pg_clickhouse: missing value for parameter \"%s\" in options "
                        "string",
                        pname
                    )
                );
            }
        } else {
            cp2 = pval;
            cp++;
            for (;;) {
                if (*cp == '\0') {
                    ereport(
                        ERROR,
                        errcode(ERRCODE_SYNTAX_ERROR),
                        errmsg(
                            "pg_clickhouse: unterminated quoted string in options "
                            "string"
                        )
                    );
                }
                if (*cp == '\\') {
                    cp++;
                    if (*cp != '\0') {
                        *cp2++ = *cp++;
                    }
                    continue;
                }
                if (*cp == '\'') {
                    *cp2 = '\0';
                    cp++;
                    break;
                }
                *cp2++ = *cp++;
            }

            /* Make sure there's a trailing comma or end of the input. */
            while (isspace((unsigned char)*cp)) {
                cp++;
            }
            if (*cp == ',') {
                cp++;
            } else if (*cp != '\0') {
                ereport(
                    ERROR,
                    errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg(
                        "pg_clickhouse: missing comma after \"%s\" value in "
                        "options string",
                        pname
                    )
                );
            }
        }

        /*
         * Now that we have the name and the value, store the record.
         */
        options = lappend(
            options, makeDefElem(pstrdup(pname), (Node*)makeString(pstrdup(pval)), -1)
        );
    }

    return options;
}

/*
 * Return the current list of current session settings parsed from the
 * session_settings GUC.
 */
kv_list*
chfdw_get_session_settings(void) {
    return ch_session_settings_list;
}

/*
 * Return the current value of the `pushdown_regex` GUC.
 */
bool
chfdw_pushdown_regex_ok(void) {
    return ch_pushdown_regex;
}

/*
 * Return the current value of the `vector_planner_warnings` GUC.
 */
bool
chfdw_vector_planner_warnings(void) {
    return ch_vector_planner_warnings;
}

/*
 * Validates the provided settings key/value pairs.
 */
static bool
chfdw_check_settings_guc(char** newval, void** extra, GucSource source) {
    /*
     * The value may be an empty string, so we have to accept that value.
     * Leave extra unset; chfdw_settings_assign_hook() will assign NULL to
     * ch_session_settings_list.
     */
    if (*newval == NULL || *newval[0] == '\0') {
        return true;
    }

    /* Make sure we can parse the settings. */
    List* list = chfdw_parse_options(*newval);

    if (!list) {
        return false;
    }

    /* Convert them into a guc_malloc'd kv_list. */
    kv_list* settings = new_kv_list_from_pg_list(list, kv_pair_guc_malloc);

    list_free(list);

    if (!settings) {
        return false;
    }

    /* All good; stash for chfdw_settings_assign_hook and return true. */
    *extra = settings;
    return true;
}

/*
 * Assigns the kv_list stored in extra to the ch_session_settings_list global.
 */
static void
chfdw_settings_assign_hook(const char* newval, void* extra) {
    /*
     * From PostgreSQL's POV: (a) failure here is not acceptable, and (b) it
     * is not necessarily called inside a transaction, so e.g. catalog lookups
     * are not okay. IOW, keep it as simple as possible, and leave error
     * returning behavior to chfdw_check_settings_guc().
     */
    ch_session_settings_list = (kv_list*)extra;
}

/*
 * Module load callback
 */
void
_PG_init(void) {
    /*
     * Key/value pairs for ClickHouse session settings. The format
     *
     * key = 'val', key = 'val'
     *
     * Spaces are optional. Full list of options:
     * https://clickhouse.com/docs/operations/settings/settings
     *
     */
    DefineCustomStringVariable(
        "pg_clickhouse.session_settings",
        "Sets the default ClickHouse session settings.",
        NULL,
        &ch_session_settings,
        "",
        PGC_USERSET,
        0,
        chfdw_check_settings_guc,
        chfdw_settings_assign_hook,
        NULL
    );

    DefineCustomBoolVariable(
        "pg_clickhouse.pushdown_regex",
        "Whether to push down regular expression operations.",
        "By default, pg_clickhouse attempts to push down regular "
        "expression functions and operations, which is fine for "
        "simple expressions. However, ClickHouse and Postgres use "
        "fundamentally different regular expression engines, so it "
        "may not make sense to push them all down. Set this option "
        "to false to prevent the Postgres regular expression functions "
        "and operators from pushing down to ClickHouse.",
        &ch_pushdown_regex,
        true,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );

    DefineCustomBoolVariable(
        "pg_clickhouse.vector_planner_warnings",
        "Warn when a vector search cannot use a ClickHouse vector index.",
        "When on, pg_clickhouse will emit a warning when an ORDER BY "
        "... LIMIT over pgvector column on a pg_clickhouse table cannot use a "
        "vector similarity index.",
        &ch_vector_planner_warnings,
        true,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );

#if PG_VERSION_NUM >= 150000
    MarkGUCPrefixReserved("pg_clickhouse");
#endif
}
