/*
 * local.c
 *
 * Driver running each query in a new clickhouse local process, so foreign
 * tables can read data lake catalogs and object stores without a ClickHouse
 * server. Nothing persists between processes: each query creates its catalog
 * database again.
 *
 * Process channels:
 *   stdin   SQL script, closed once written
 *   stdout  Native blocks
 *   stderr  error text
 *   exit    0 on success, else ClickHouse error code modulo 256
 *
 * Process lifecycle follows chdb_helper from pg_chdb:
 * https://github.com/ClickHouse/pg_chdb/blob/main/src/helper.c
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace_d.h"
#include "commands/defrem.h"
#include "common/file_utils.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "cursor.h"
#include "fdw.h"

extern char** environ;

/* Native bytes read in one chunk */
#define LOCAL_CHUNK_BYTES (256 * 1024)

/* Milliseconds between wakeups while a channel is idle, so stderr drains */
#define LOCAL_POLL_MS 1000

typedef struct {
    MemoryContext cxt;
    const char* gate; /* setting enabling catalog engine, or NULL */
    char* bootstrap;  /* CREATE DATABASE statement, holds catalog secrets */
    ch_server_version version;
} ch_local_connection;

typedef struct {
    MemoryContext cxt;
    MemoryContextCallback cleanup;
    const char* sql; /* query to report, never the script holding secrets */
    pid_t pid;
    int in; /* our pipe ends, -1 once closed */
    int out;
    int err;
    int in_peer; /* process ends, held only until fork */
    int out_peer;
    int err_peer;
    char* tmpdir; /* TMPDIR given to process, removed after it exits */
    bool reaped;
    int status; /* wait status, negative when waitpid gave none */
    size_t err_len;
    char err_buf[CH_ERROR_MSG_LEN];
    char chunk[LOCAL_CHUNK_BYTES];
} ch_local_process;

/* Settings enabling DataLakeCatalog per catalog_type, named as of 25.3 */
static const struct {
    const char* catalog_type;
    const char* setting;
} catalog_gates[] = {
    { "rest",          "allow_experimental_database_iceberg"             },
    { "onelake",       "allow_experimental_database_iceberg"             },
    { "biglake",       "allow_experimental_database_iceberg"             },
    { "horizon",       "allow_experimental_database_iceberg"             },
    { "s3tables",      "allow_experimental_database_iceberg"             },
    { "delta_sharing", "allow_experimental_database_iceberg"             },
    { "glue",          "allow_experimental_database_glue_catalog"        },
    { "unity",         "allow_experimental_database_unity_catalog"       },
    { "hive",          "allow_experimental_database_hms_catalog"         },
    { "paimon_rest",   "allow_experimental_database_paimon_rest_catalog" },
};

static void
close_fd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* Sleep until fd is ready, letting cancel or shutdown through */
static void
wait_fd(int fd, uint32 event) {
    WaitLatchOrSocket(
        MyLatch,
        event | WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
        fd,
        LOCAL_POLL_MS,
        PG_WAIT_EXTENSION
    );
    ResetLatch(MyLatch);
}

/* Keep head of stderr and discard rest, since a full pipe stalls process */
static void
drain_err(ch_local_process* p) {
    char sink[1024];

    while (p->err >= 0) {
        size_t room = sizeof(p->err_buf) - 1 - p->err_len;
        char* into  = room ? p->err_buf + p->err_len : sink;
        ssize_t got = read(p->err, into, room ? room : sizeof(sink));

        if (got > 0) {
            p->err_len += into == sink ? 0 : (size_t)got;
        } else if (got == 0) {
            close_fd(&p->err);
        } else if (errno != EINTR) {
            return;
        }
    }
}

static int
reap(ch_local_process* p) {
    if (p->reaped) {
        return p->status;
    }
    while (waitpid(p->pid, &p->status, 0) < 0) {
        if (errno != EINTR) {
            p->status = -1;
            break;
        }
    }
    p->reaped = true;
    return p->status;
}

static int
drain_and_reap(ch_local_process* p) {
    while (p->err >= 0) {
        CHECK_FOR_INTERRUPTS();
        drain_err(p);
        if (p->err >= 0) {
            wait_fd(p->err, WL_SOCKET_READABLE);
        }
    }
    return reap(p);
}

/* Memory context callback, kills process and removes its temporary files */
static void
stop_process(void* arg) {
    ch_local_process* p = arg;

    close_fd(&p->in);
    close_fd(&p->out);
    close_fd(&p->err);
    close_fd(&p->in_peer);
    close_fd(&p->out_peer);
    close_fd(&p->err_peer);
    if (p->pid > 0 && !p->reaped) {
        kill(p->pid, SIGKILL);
        reap(p);
    }
    if (p->tmpdir) {
        rmtree(p->tmpdir, true);
    }
}

/* Raise with stderr text, else with how process ended */
static void
report_process(ch_local_process* p) {
    drain_and_reap(p);

    while (p->err_len &&
           (p->err_buf[p->err_len - 1] == '\n' || p->err_buf[p->err_len - 1] == '\r')) {
        p->err_len--;
    }
    /* Clip short of a split multibyte character; exceptions are UTF-8 */
    p->err_len =
        (size_t)pg_encoding_mbcliplen(PG_UTF8, p->err_buf, p->err_len, p->err_len);
    p->err_buf[p->err_len] = '\0';

    /* Prefer consistent interrupt error message when query interrupted */
    CHECK_FOR_INTERRUPTS();
    if (p->err_len) {
        ereport(
            ERROR,
            errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
            errmsg("pg_clickhouse: %s", p->err_buf),
            errdetail_internal("Remote Query: %.64000s", p->sql)
        );
    }
    if (p->status >= 0 && WIFSIGNALED(p->status)) {
        ereport(
            ERROR,
            errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
            errmsg(
                "pg_clickhouse: clickhouse local terminated by signal %d",
                WTERMSIG(p->status)
            ),
            errdetail_internal("Remote Query: %.64000s", p->sql)
        );
    }
    ereport(
        ERROR,
        errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
        errmsg(
            "pg_clickhouse: clickhouse local exited with status %d",
            p->status >= 0 && WIFEXITED(p->status) ? WEXITSTATUS(p->status) : -1
        ),
        errdetail_internal("Remote Query: %.64000s", p->sql)
    );
}

static void
set_flag(int fd, int get, int set, int flag) {
    int flags = fcntl(fd, get);

    if (flags < 0 || fcntl(fd, set, flags | flag) < 0) {
        ereport(
            ERROR,
            errcode_for_socket_access(),
            errmsg("pg_clickhouse: could not configure pipe: %m")
        );
    }
}

/* Store both ends before raising, so cleanup owns them */
static void
open_pipe(int* read_end, int* write_end) {
    int fd[2];

    if (pipe(fd) < 0) {
        ereport(
            ERROR,
            errcode_for_socket_access(),
            errmsg("pg_clickhouse: could not create pipe: %m")
        );
    }
    *read_end  = fd[0];
    *write_end = fd[1];
}

/*
 * Postmaster removes pgsql_tmp entries left by a crash, which SIGKILL on
 * cancel would otherwise leak from system temporary directory
 */
static char*
make_tmpdir(void) {
    char dir[MAXPGPATH];
    char* path;

    TempTablespacePath(dir, DEFAULTTABLESPACE_OID);
    if (MakePGDirectory(dir) < 0 && errno != EEXIST) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("pg_clickhouse: could not create directory \"%s\": %m", dir)
        );
    }
    path = psprintf(
        "%s/%s/%s%d.clickhouse.XXXXXX", DataDir, dir, PG_TEMP_FILE_PREFIX, MyProcPid
    );
    if (mkdtemp(path) == NULL) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("pg_clickhouse: could not create directory \"%s\": %m", path)
        );
    }
    return path;
}

/* Copy environment with TMPDIR replaced */
static char**
make_env(const char* tmpdir) {
    int n = 0;
    char** env;
    int i = 0;

    while (environ[n]) {
        n++;
    }
    env = palloc((n + 2) * sizeof(char*));
    for (char** e = environ; *e; e++) {
        if (strncmp(*e, "TMPDIR=", 7) != 0) {
            env[i++] = *e;
        }
    }
    env[i++] = psprintf("TMPDIR=%s", tmpdir);
    env[i]   = NULL;
    return env;
}

/* Runs between fork and exec with backend state, so may only _exit */
static void
exec_process(const char* program, char** env, int in, int out, int err) {
    /* argv[0] selects local mode of clickhouse multi-call binary */
    char* const argv[] = { "clickhouse-local", "--output-format", "Native", NULL };

    if (dup2(in, STDIN_FILENO) < 0 || dup2(out, STDOUT_FILENO) < 0 ||
        dup2(err, STDERR_FILENO) < 0) {
        _exit(126);
    }
    /* Postgres ignores SIGPIPE; ClickHouse wants default disposition */
    signal(SIGPIPE, SIG_DFL);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    execve(program, argv, env);
    _exit(127);
}

static void
write_script(ch_local_process* p, const char* script, size_t len) {
    while (len) {
        ssize_t put;

        CHECK_FOR_INTERRUPTS();
        put = write(p->in, script, len);
        if (put > 0) {
            script += put;
            len -= put;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            drain_err(p);
            wait_fd(p->in, WL_SOCKET_WRITEABLE);
        } else if (errno == EPIPE) {
            /* Process exited before reading whole script */
            report_process(p);
        } else if (errno != EINTR) {
            ereport(
                ERROR,
                errcode_for_socket_access(),
                errmsg("pg_clickhouse: could not send query to clickhouse local: %m")
            );
        }
    }
    close_fd(&p->in);
}

/*
 * Local ClickHouse reads host files and networks as Postgres OS user, so treat
 * it like COPY PROGRAM
 */
static void
check_local_privilege(void) {
    if (!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM)) {
        ereport(
            ERROR,
            errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("pg_clickhouse: permission denied to use driver \"local\""),
            errdetail(
                "Only roles with privileges of the \"%s\" role may use driver "
                "\"local\".",
                "pg_execute_server_program"
            )
        );
    }
}

static const char*
local_program(void) {
    const char* program = chfdw_clickhouse_path();

    if (program == NULL || *program == '\0') {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: pg_clickhouse.clickhouse_path is not set"),
            errhint(
                "Set pg_clickhouse.clickhouse_path to absolute path of clickhouse "
                "executable."
            )
        );
    }
    if (access(program, X_OK) != 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("pg_clickhouse: could not execute \"%s\": %m", program)
        );
    }
    return program;
}

/* Start process on script; cleanup registered on new child of current context */
static ch_local_process*
spawn(const char* script, const char* sql) {
    const char* program = local_program();
    MemoryContext cxt   = AllocSetContextCreate(
        CurrentMemoryContext, "pg_clickhouse local process", ALLOCSET_SMALL_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ch_local_process* p;
    char** env;

    p           = palloc0(sizeof(*p));
    p->cxt      = cxt;
    p->sql      = pstrdup(sql);
    p->pid      = -1;
    p->in       = -1;
    p->out      = -1;
    p->err      = -1;
    p->in_peer  = -1;
    p->out_peer = -1;
    p->err_peer = -1;

    /* Registered first, so every resource below has an owner already */
    p->cleanup.func = stop_process;
    p->cleanup.arg  = p;
    MemoryContextRegisterResetCallback(cxt, &p->cleanup);

    p->tmpdir = make_tmpdir();
    env       = make_env(p->tmpdir);
    open_pipe(&p->in_peer, &p->in);
    open_pipe(&p->out, &p->out_peer);
    open_pipe(&p->err, &p->err_peer);

    /* Only peer ends survive exec, as dup2 clears close-on-exec */
    set_flag(p->in, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p->out, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p->err, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p->in_peer, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p->out_peer, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p->err_peer, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p->in, F_GETFL, F_SETFL, O_NONBLOCK);
    set_flag(p->out, F_GETFL, F_SETFL, O_NONBLOCK);
    set_flag(p->err, F_GETFL, F_SETFL, O_NONBLOCK);

    /* Postgres buffers would otherwise be flushed by both processes */
    fflush(NULL);
    p->pid = fork();
    if (p->pid < 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("pg_clickhouse: could not fork: %m")
        );
    }
    if (p->pid == 0) {
        exec_process(program, env, p->in_peer, p->out_peer, p->err_peer);
    }
    close_fd(&p->in_peer);
    close_fd(&p->out_peer);
    close_fd(&p->err_peer);

    elog(DEBUG1, "pg_clickhouse: clickhouse local pid %d", (int)p->pid);
    write_script(p, script, strlen(script));
    MemoryContextSwitchTo(old);
    return p;
}

/* pgch_chunk_source callback; raises process errors with stderr text */
static bool
local_next_chunk(void* ud, const void** chunk, size_t* n, char** error) {
    ch_local_process* p = ud;

    for (;;) {
        ssize_t got;

        CHECK_FOR_INTERRUPTS();
        got = read(p->out, p->chunk, sizeof(p->chunk));
        if (got > 0) {
            *chunk = p->chunk;
            *n     = (size_t)got;
            return true;
        }
        if (got == 0) {
            close_fd(&p->out);
            if (drain_and_reap(p) != 0) {
                report_process(p);
            }
            *n = 0;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            drain_err(p);
            wait_fd(p->out, WL_SOCKET_READABLE);
        } else if (errno != EINTR) {
            *error = psprintf("could not read from clickhouse local: %m");
            return false;
        }
    }
}

static void
local_reader_init(pgch_reader* reader, void* response) {
    pgch_chunk_source src = {
        .ud         = response,
        .next_chunk = local_next_chunk,
    };

    pgch_reader_init_chunks(reader, &src, NULL);
}

static void
local_response_free(void* response) {
    if (response) {
        MemoryContextDelete(((ch_local_process*)response)->cxt);
    }
}

static void
check_setting_name(const char* name) {
    if (!chfdw_is_setting_name(name)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
            errmsg("pg_clickhouse: invalid ClickHouse setting name \"%s\"", name)
        );
    }
}

/* Append to SET statement, starting it when empty */
static void
append_setting(StringInfo buf, const char* name, const char* value) {
    appendStringInfo(buf, "%s%s = %s", buf->len ? ", " : "SET ", name, value);
}

static ch_server_version
local_server_version(void* c);

/* SET statement, catalog bootstrap, then query, with only query output */
static char*
build_script(ch_local_connection* conn, const ch_query* query) {
    ch_server_version version = local_server_version(conn);
    kv_iter it                = new_kv_iter(query->settings);
    StringInfoData buf;

    initStringInfo(&buf);
    while (kv_iter_next(&it)) {
        check_setting_name(it.name);
        append_setting(&buf, it.name, ch_quote_literal(it.value));
    }
    for (int i = 0; i < query->num_params; i++) {
        const char* value = query->param_values[i];

        append_setting(
            &buf,
            psprintf("param_p%d", i + 1),
            value ? ch_quote_literal(value) : "'\\\\N'"
        );
    }
    if (conn->gate) {
        append_setting(&buf, conn->gate, "1");
    }
    /* Decoder needs these; last value wins over session settings */
    if (chfdw_version_ge(version, 24, 7)) {
        append_setting(&buf, "output_format_native_encode_types_in_binary_format", "0");
    }
    if (chfdw_version_ge(version, 24, 10)) {
        append_setting(&buf, "output_format_native_write_json_as_string", "1");
    }
    if (buf.len) {
        appendStringInfoString(&buf, ";\n");
    }
    if (conn->bootstrap) {
        appendStringInfoString(&buf, conn->bootstrap);
    }
    appendStringInfoString(&buf, query->sql);
    return buf.data;
}

static ch_cursor*
open_cursor(const char* script, const ch_query* query) {
    ch_cursor_source src = {
        .init_reader   = local_reader_init,
        .free_response = local_response_free,
    };

    check_local_privilege();
    src.response = spawn(script, query->sql);
    return chfdw_cursor_open(NULL, query, &src);
}

static ch_cursor*
local_query(void* c, const ch_query* query) {
    return open_cursor(build_script(c, query), query);
}

/* Bypass build_script, which needs version to choose settings */
static ch_server_version
local_server_version(void* c) {
    ch_local_connection* conn = c;

    if (conn->version.major == 0) {
        ch_query query =
            new_query("SELECT version()", 0, NULL, NULL, NULL, CHC_ENC_FAIL);
        ChFdwScanRowContext ctx = { .retrieved_attrs = list_make1_int(1) };
        Datum* row;

        ctx.cursor = open_cursor(query.sql, &query);
        row        = chfdw_cursor_fetch_row(&ctx);
        if (row != NULL) {
            sscanf(
                TextDatumGetCString(row[0]),
                "%d.%d.%d",
                &conn->version.major,
                &conn->version.minor,
                &conn->version.patch
            );
        }
        MemoryContextDelete(ctx.cursor->memcxt);
    }
    return conn->version;
}

static void
local_disconnect(void* c) {
    MemoryContextDelete(((ch_local_connection*)c)->cxt);
}

static void*
local_prepare_insert(
    void* conn pg_attribute_unused(),
    ResultRelInfo* rri pg_attribute_unused(),
    List* attrs pg_attribute_unused(),
    const ch_query* query pg_attribute_unused(),
    char* table pg_attribute_unused()
) {
    ereport(
        ERROR,
        errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("pg_clickhouse: driver \"local\" does not support INSERT")
    );
}

static libclickhouse_methods local_methods = {
    .disconnect          = local_disconnect,
    .simple_query        = local_query,
    .fetch_row           = chfdw_cursor_fetch_row,
    .prepare_insert      = local_prepare_insert,
    .streaming_query     = local_query,
    .streaming_fetch_row = chfdw_cursor_fetch_row,
    .server_version      = local_server_version,
};

/* Merge catalog_settings, user mapping replacing same-named server settings */
static List*
merge_catalog_settings(List* settings, List* options) {
    ListCell* lc;

    foreach (lc, options) {
        DefElem* def = lfirst(lc);
        ListCell* sc;

        if (strcmp(def->defname, "catalog_settings") != 0) {
            continue;
        }
        foreach (sc, chfdw_parse_options(defGetString(def))) {
            DefElem* setting = lfirst(sc);
            ListCell* old;

            foreach (old, settings) {
                if (strcmp(((DefElem*)lfirst(old))->defname, setting->defname) == 0) {
                    settings = foreach_delete_current(settings, old);
                }
            }
            settings = lappend(settings, setting);
        }
    }
    return settings;
}

static const char*
find_option(List* options, const char* name) {
    ListCell* lc;

    foreach (lc, options) {
        DefElem* def = lfirst(lc);

        if (strcmp(def->defname, name) == 0) {
            return defGetString(def);
        }
    }
    return NULL;
}

ch_connection
chfdw_local_connect(
    ch_connection_details* details,
    ForeignServer* server,
    UserMapping* user
) {
    const char* url = find_option(server->options, "catalog_url");
    List* settings  = merge_catalog_settings(
        merge_catalog_settings(NIL, server->options), user->options
    );
    const char* gate = NULL;
    StringInfoData buf;
    ListCell* lc;
    MemoryContext cxt;
    ch_local_connection* conn;

    initStringInfo(&buf);
    if (url) {
        appendStringInfo(
            &buf,
            "CREATE DATABASE %s ENGINE = DataLakeCatalog(%s)",
            quote_identifier(details->dbname),
            ch_quote_literal(url)
        );
    }
    foreach (lc, settings) {
        DefElem* def      = lfirst(lc);
        const char* value = defGetString(def);

        check_setting_name(def->defname);
        appendStringInfo(
            &buf,
            "%s%s = %s",
            foreach_current_index(lc) ? ", " : " SETTINGS ",
            def->defname,
            ch_quote_literal(value)
        );
        if (strcmp(def->defname, "catalog_type") == 0) {
            for (size_t i = 0; i < lengthof(catalog_gates); i++) {
                if (strcmp(value, catalog_gates[i].catalog_type) == 0) {
                    gate = catalog_gates[i].setting;
                }
            }
        }
    }

    /* Allocate only once nothing else can raise */
    cxt = AllocSetContextCreate(
        CacheMemoryContext, "pg_clickhouse local connection", ALLOCSET_SMALL_SIZES
    );
    conn       = MemoryContextAllocZero(cxt, sizeof(*conn));
    conn->cxt  = cxt;
    conn->gate = gate;
    if (url) {
        appendStringInfoString(&buf, ";\n");
        conn->bootstrap = MemoryContextStrdup(cxt, buf.data);
    }
    return (ch_connection){
        .methods        = &local_methods,
        .encoding_check = details->encoding_check,
        .conn           = conn,
    };
}
