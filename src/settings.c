/*-------------------------------------------------------------------------
 *
 * settings.c
 *		  ClickHouse query settings from the "settings" FDW options and the
 *		  pg_clickhouse.session_settings GUC
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/settings.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fdw.h"
#include "kv_list.h"

#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"

/*
 * Settings that older ClickHouse versions reject as unknown, with the version
 * that added each, from system.settings_changes.
 */
static const struct {
    const char* name;
    int min_major;
    int min_minor;
} known_settings[] = {
    { "hnsw_candidate_list_size_for_search",        24, 10 },
    { "max_limit_for_vector_search_queries",        25, 5  },
    { "vector_search_filter_strategy",              25, 5  },
    { "vector_search_index_fetch_multiplier",       25, 8  },
    { "vector_search_postfilter_multiplier",        25, 5  },
    { "vector_search_with_rescoring",               25, 8  },
    { "vector_search_use_quantized_codes",          26, 7  },
    { "query_plan_try_use_vector_search",           25, 1  },
    { "enable_vector_similarity_index",             25, 7  },
    { "allow_experimental_vector_similarity_index", 24, 8  },
    { "materialize_skip_indexes_on_insert",         24, 6  },
    { "exclude_materialize_skip_indexes_on_insert", 25, 10 },
    { "use_skip_indexes",                           23, 3  },
    { "use_skip_indexes_if_final",                  23, 3  },
    { "use_skip_indexes_for_top_k",                 25, 12 },
    { "enable_qbit_type",                           26, 1  },
    { "allow_experimental_qbit_type",               25, 10 },
    { "optimize_qbit_distance_function_reads",      25, 10 },
    { "enable_quantized_codec",                     26, 8  },
};

/* Settings pg_clickhouse needs for PostgreSQL semantics */
static const struct {
    char* name;
    char* value;
} defaults[] = {
    { "join_use_nulls",     "1" },
    { "group_by_use_nulls", "1" },
    { "final",              "1" },
    { "transform_null_in",  "0" },
};

/* Return settings option value from options, or NULL if absent. */
const char*
chfdw_settings_option(const List* options) {
    ListCell* lc;

    foreach (lc, options) {
        DefElem* def = lfirst(lc);

        if (strcmp(def->defname, "settings") == 0) {
            return defGetString(def);
        }
    }
    return NULL;
}

/* Add settings to base, replacing those with the same names */
static List*
overlay(List* base, List* settings) {
    ListCell* lc;

    foreach (lc, settings) {
        DefElem* def = lfirst(lc);
        ListCell* bc;

        foreach (bc, base) {
            if (strcmp(((DefElem*)lfirst(bc))->defname, def->defname) == 0) {
                break;
            }
        }
        if (bc) {
            lfirst(bc) = def;
        } else {
            base = lappend(base, def);
        }
    }
    return base;
}

static List*
default_settings(void) {
    List* settings = NIL;

    for (size_t i = 0; i < lengthof(defaults); i++) {
        settings = lappend(
            settings,
            makeDefElem(defaults[i].name, (Node*)makeString(defaults[i].value), -1)
        );
    }
    return settings;
}

static List*
session_settings(void) {
    List* settings = NIL;
    kv_iter it     = new_kv_iter(chfdw_get_session_settings());

    while (kv_iter_next(&it)) {
        settings =
            lappend(settings, makeDefElem(it.name, (Node*)makeString(it.value), -1));
    }
    return settings;
}

/* Return built-in defaults overridden by pg_clickhouse.session_settings */
kv_list*
chfdw_session_settings(void) {
    return new_kv_list_from_pg_list(
        overlay(default_settings(), session_settings()), kv_pair_palloc
    );
}

/* Return table settings, keeping last value for each name */
List*
chfdw_table_settings(Oid relid) {
    const char* opt = chfdw_settings_option(GetForeignTable(relid)->options);

    return opt ? overlay(NIL, chfdw_parse_options(opt)) : NIL;
}

/*
 * Combine table settings for ClickHouse's single settings list per query.
 * Mark conflicting values with NULL so server or session settings apply.
 */
List*
chfdw_merge_settings(const List* a, const List* b) {
    List* merged = list_copy(a);
    ListCell* lc;

    foreach (lc, b) {
        DefElem* def = lfirst(lc);
        ListCell* mc;

        foreach (mc, merged) {
            if (strcmp(((DefElem*)lfirst(mc))->defname, def->defname) == 0) {
                break;
            }
        }
        if (!mc) {
            merged = lappend(merged, def);
        } else if (!equal(((DefElem*)lfirst(mc))->arg, def->arg)) {
            lfirst(mc) = makeDefElem(def->defname, NULL, -1);
        }
    }
    return merged;
}

/*
 * Skip settings that conn's ClickHouse version does not know, since drivers
 * mark settings important, causing ClickHouse to reject unknown names.
 */
static List*
drop_unsupported(List* settings, const ch_connection* conn) {
    List* kept                = NIL;
    bool have_version         = false;
    ch_server_version version = { 0 };
    ListCell* lc;

    foreach (lc, settings) {
        DefElem* def = lfirst(lc);
        size_t i     = 0;

        while (i < lengthof(known_settings) &&
               strcmp(known_settings[i].name, def->defname) != 0) {
            i++;
        }
        if (i == lengthof(known_settings)) {
            kept = lappend(kept, def);
            continue;
        }
        if (!have_version) {
            have_version = true;
            version      = conn->methods->server_version
                               ? conn->methods->server_version(conn->conn)
                               : (ch_server_version){ 0 };
        }
        if (version.major == 0 ||
            chfdw_version_ge(
                version, known_settings[i].min_major, known_settings[i].min_minor
            )) {
            kept = lappend(kept, def);
            continue;
        }
        ereport(
            WARNING,
            errmsg(
                "pg_clickhouse: ClickHouse %d.%d.%d does not support setting \"%s\"; "
                "not "
                "sent",
                version.major,
                version.minor,
                version.patch,
                def->defname
            ),
            errdetail(
                "Setting \"%s\" requires ClickHouse %d.%d or later.",
                def->defname,
                known_settings[i].min_major,
                known_settings[i].min_minor
            )
        );
    }
    return kept;
}

/*
 * Build query settings from built-in defaults, server settings, table settings,
 * then session settings. Let later values replace earlier ones for each name.
 * Skip settings unsupported by conn's ClickHouse version.
 */
kv_list*
chfdw_query_settings(Oid serverid, const List* tables, const ch_connection* conn) {
    const char* server = chfdw_settings_option(GetForeignServer(serverid)->options);
    List* settings     = default_settings();

    if (server) {
        settings = overlay(settings, chfdw_parse_options(server));
    }
    ListCell* lc;

    foreach (lc, tables) {
        DefElem* def = lfirst(lc);

        if (def->arg) {
            settings = overlay(settings, list_make1(def));
        }
    }
    settings = overlay(settings, session_settings());
    settings = drop_unsupported(settings, conn);
    foreach (lc, tables) {
        DefElem* def = lfirst(lc);

        if (!def->arg) {
            const char* value = NULL;
            ListCell* sc;

            foreach (sc, settings) {
                DefElem* setting = lfirst(sc);

                if (strcmp(setting->defname, def->defname) == 0) {
                    value = defGetString(setting);
                    break;
                }
            }
            ereport(
                NOTICE,
                errmsg(
                    "pg_clickhouse: foreign tables in query set \"%s\" differently; "
                    "ignoring table values",
                    def->defname
                ),
                value ? errdetail("Using query value: %s.", value)
                      : errdetail("Using ClickHouse default.")
            );
        }
    }
    return new_kv_list_from_pg_list(settings, kv_pair_palloc);
}
