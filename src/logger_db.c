#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sqlite/sqlite3.h"
#include "main.h"
#include "pvi_interface.h"
#include "logger_db.h"
#include "report.h"

static sqlite3     *g_db = NULL;
static long long    g_run_id = 0;

/* Prepared statements held across the lifetime of a logging run. */
static sqlite3_stmt *g_stmt_upsert_pv  = NULL;  /* INSERT OR IGNORE INTO pvs ... */
static sqlite3_stmt *g_stmt_lookup_pv  = NULL;  /* SELECT pv_id FROM pvs WHERE full_path=? */
static sqlite3_stmt *g_stmt_insert_sample = NULL;  /* INSERT INTO samples ... */

static const char *SCHEMA_SQL =
	"CREATE TABLE IF NOT EXISTS runs ("
	"    run_id       INTEGER PRIMARY KEY AUTOINCREMENT,"
	"    started_utc  INTEGER NOT NULL,"
	"    stopped_utc  INTEGER,"
	"    cycle_ms     INTEGER NOT NULL,"
	"    watch_file   TEXT,"
	"    pvi_version  TEXT,"
	"    hostname     TEXT,"
	"    notes        TEXT"
	");"
	"CREATE TABLE IF NOT EXISTS pvs ("
	"    pv_id        INTEGER PRIMARY KEY AUTOINCREMENT,"
	"    full_path    TEXT NOT NULL UNIQUE,"
	"    cpu_name     TEXT NOT NULL,"
	"    cpu_type     TEXT,"
	"    ar_version   TEXT,"
	"    task_name    TEXT,"
	"    var_name     TEXT NOT NULL,"
	"    br_type      INTEGER NOT NULL,"
	"    br_type_name TEXT,"
	"    scope        TEXT"
	");"
	"CREATE TABLE IF NOT EXISTS samples ("
	"    run_id       INTEGER NOT NULL REFERENCES runs(run_id),"
	"    pv_id        INTEGER NOT NULL REFERENCES pvs(pv_id),"
	"    ts_utc_ms    INTEGER NOT NULL,"
	"    v_int        INTEGER,"
	"    v_real       REAL,"
	"    v_text       TEXT"
	");"
	"CREATE INDEX IF NOT EXISTS idx_samples_pv_ts  ON samples(pv_id, ts_utc_ms);"
	"CREATE INDEX IF NOT EXISTS idx_samples_run_ts ON samples(run_id, ts_utc_ms);"
;

static const char *BrTypeName( PVIPVARTYPE t ) {
	switch( t ) {
		case BR_BOOL:    return "BR_BOOL";
		case BR_USINT:   return "BR_USINT";
		case BR_SINT:    return "BR_SINT";
		case BR_UINT:    return "BR_UINT";
		case BR_INT:     return "BR_INT";
		case BR_UDINT:   return "BR_UDINT";
		case BR_DINT:    return "BR_DINT";
		case BR_REAL:    return "BR_REAL";
		case BR_LREAL:   return "BR_LREAL";
		case BR_STRING:  return "BR_STRING";
		case BR_WSTRING: return "BR_WSTRING";
		case BR_STRUCT:  return "BR_STRUCT";
		case BR_TIME:    return "BR_TIME";
		case BR_DATI:    return "BR_DATI";
		case BR_TOD:     return "BR_TOD";
		case BR_DATE:    return "BR_DATE";
		default:         return "UNKNOWN";
	}
}

static void DbError( const char *where, int rc ) {
	ReportError( REPORT_ERROR, "SQLite", "%s failed (rc=%d): %s", where, rc,
	             g_db ? sqlite3_errmsg(g_db) : "no db" );
}

int LoggerDbFormatPvFullPath( PVIOBJECT *obj, char *buf, int buflen ) {
	if( !obj || obj->type != POBJ_PVAR || !buf || buflen <= 0 ) {
		if( buf && buflen > 0 ) buf[0] = 0;
		return 0;
	}

	PVIOBJECT *task = obj->ex.pv.task;
	PVIOBJECT *cpu  = obj->ex.pv.cpu;
	const char *cputype  = (cpu && cpu->ex.cpu.cputype[0])   ? cpu->ex.cpu.cputype   : "";
	const char *arver    = (cpu && cpu->ex.cpu.arversion[0]) ? cpu->ex.cpu.arversion : "";
	const char *cpudesc  = (cpu)  ? cpu->descriptor  : "";
	const char *taskdesc = (task) ? task->descriptor : "";
	const char *vardesc  = obj->descriptor;
	int n;

	if( obj->ex.pv.scope[0] == 'l' || obj->ex.pv.scope[0] == 'd' ) {
		n = _snprintf( buf, buflen, "%s[%s]%s->%s:%s", cputype, arver, cpudesc, taskdesc, vardesc );
	} else {
		n = _snprintf( buf, buflen, "%s[%s]%s->%s", cputype, arver, cpudesc, vardesc );
	}
	if( n < 0 || n >= buflen ) {
		buf[buflen-1] = 0;
		return buflen - 1;
	}
	return n;
}

BOOL LoggerDbIsOpen( void ) {
	return g_db != NULL;
}

static int PrepareStatements( void ) {
	int rc;

	rc = sqlite3_prepare_v2( g_db,
		"INSERT OR IGNORE INTO pvs (full_path, cpu_name, cpu_type, ar_version, task_name, var_name, br_type, br_type_name, scope) "
		"VALUES (?,?,?,?,?,?,?,?,?)",
		-1, &g_stmt_upsert_pv, NULL );
	if( rc != SQLITE_OK ) { DbError("prepare upsert_pv", rc); return -1; }

	rc = sqlite3_prepare_v2( g_db,
		"SELECT pv_id FROM pvs WHERE full_path=?",
		-1, &g_stmt_lookup_pv, NULL );
	if( rc != SQLITE_OK ) { DbError("prepare lookup_pv", rc); return -1; }

	rc = sqlite3_prepare_v2( g_db,
		"INSERT INTO samples (run_id, pv_id, ts_utc_ms, v_int, v_real, v_text) VALUES (?,?,?,?,?,?)",
		-1, &g_stmt_insert_sample, NULL );
	if( rc != SQLITE_OK ) { DbError("prepare insert_sample", rc); return -1; }

	return 0;
}

static void FinalizeStatements( void ) {
	if( g_stmt_upsert_pv )     { sqlite3_finalize(g_stmt_upsert_pv);     g_stmt_upsert_pv = NULL; }
	if( g_stmt_lookup_pv )     { sqlite3_finalize(g_stmt_lookup_pv);     g_stmt_lookup_pv = NULL; }
	if( g_stmt_insert_sample ) { sqlite3_finalize(g_stmt_insert_sample); g_stmt_insert_sample = NULL; }
}

int LoggerDbOpen( const char *path ) {
	int rc;
	char *errmsg = NULL;

	if( g_db != NULL ) {
		return 0;  /* already open */
	}
	if( !path || !*path ) {
		ReportError( REPORT_ERROR, "SQLite", "No SQLite file path configured" );
		return -1;
	}

	rc = sqlite3_open( path, &g_db );
	if( rc != SQLITE_OK ) {
		DbError( "sqlite3_open", rc );
		if( g_db ) { sqlite3_close(g_db); g_db = NULL; }
		return -1;
	}

	/* WAL mode so a reader (MCP server) can query while we write. */
	rc = sqlite3_exec( g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &errmsg );
	if( rc != SQLITE_OK ) {
		DbError( "pragma wal", rc );
		if( errmsg ) sqlite3_free(errmsg);
		sqlite3_close(g_db);
		g_db = NULL;
		return -1;
	}

	/* NORMAL sync is safe under WAL and much faster than FULL. */
	sqlite3_exec( g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL );

	rc = sqlite3_exec( g_db, SCHEMA_SQL, NULL, NULL, &errmsg );
	if( rc != SQLITE_OK ) {
		ReportError( REPORT_ERROR, "SQLite", "Schema init failed: %s", errmsg ? errmsg : "unknown" );
		if( errmsg ) sqlite3_free(errmsg);
		sqlite3_close(g_db);
		g_db = NULL;
		return -1;
	}

	if( PrepareStatements() != 0 ) {
		FinalizeStatements();
		sqlite3_close(g_db);
		g_db = NULL;
		return -1;
	}

	return 0;
}

int LoggerDbClose( void ) {
	if( g_db == NULL ) return 0;
	FinalizeStatements();
	sqlite3_close( g_db );
	g_db = NULL;
	g_run_id = 0;
	return 0;
}

int LoggerDbBeginRun( int cycle_ms, const char *watch_file, const char *notes ) {
	sqlite3_stmt *stmt = NULL;
	int rc;
	char hostname[128] = {0};
	DWORD hostlen = sizeof(hostname);

	if( !g_db ) {
		ReportError( REPORT_ERROR, "SQLite", "LoggerDbBeginRun: db not open" );
		return -1;
	}

	GetComputerName( hostname, &hostlen );

	rc = sqlite3_prepare_v2( g_db,
		"INSERT INTO runs (started_utc, cycle_ms, watch_file, pvi_version, hostname, notes) "
		"VALUES (?,?,?,?,?,?)",
		-1, &stmt, NULL );
	if( rc != SQLITE_OK ) { DbError("prepare begin_run", rc); return -1; }

	sqlite3_bind_int64( stmt, 1, (long long)time(NULL) );
	sqlite3_bind_int(   stmt, 2, cycle_ms );
	if( watch_file && *watch_file ) sqlite3_bind_text( stmt, 3, watch_file, -1, SQLITE_TRANSIENT );
	else                            sqlite3_bind_null( stmt, 3 );
	if( g_PVIVersionString[0] )     sqlite3_bind_text( stmt, 4, g_PVIVersionString, -1, SQLITE_TRANSIENT );
	else                            sqlite3_bind_null( stmt, 4 );
	sqlite3_bind_text( stmt, 5, hostname, -1, SQLITE_TRANSIENT );
	if( notes && *notes )           sqlite3_bind_text( stmt, 6, notes, -1, SQLITE_TRANSIENT );
	else                            sqlite3_bind_null( stmt, 6 );

	rc = sqlite3_step( stmt );
	sqlite3_finalize( stmt );
	if( rc != SQLITE_DONE ) { DbError("step begin_run", rc); return -1; }

	g_run_id = sqlite3_last_insert_rowid( g_db );
	return 0;
}

int LoggerDbEndRun( void ) {
	sqlite3_stmt *stmt = NULL;
	int rc;

	if( !g_db || g_run_id == 0 ) return 0;

	rc = sqlite3_prepare_v2( g_db,
		"UPDATE runs SET stopped_utc=? WHERE run_id=?",
		-1, &stmt, NULL );
	if( rc != SQLITE_OK ) { DbError("prepare end_run", rc); return -1; }

	sqlite3_bind_int64( stmt, 1, (long long)time(NULL) );
	sqlite3_bind_int64( stmt, 2, g_run_id );
	rc = sqlite3_step( stmt );
	sqlite3_finalize( stmt );
	if( rc != SQLITE_DONE ) { DbError("step end_run", rc); return -1; }

	g_run_id = 0;
	return 0;
}

int LoggerDbUpsertPv( PVIOBJECT *obj, long long *out_pv_id ) {
	int rc;
	char full_path[768];
	PVIOBJECT *task;
	PVIOBJECT *cpu;
	char scope_buf[2];

	if( out_pv_id ) *out_pv_id = 0;
	if( !g_db || !obj || obj->type != POBJ_PVAR ) return -1;

	LoggerDbFormatPvFullPath( obj, full_path, sizeof(full_path) );
	task = obj->ex.pv.task;
	cpu  = obj->ex.pv.cpu;
	scope_buf[0] = obj->ex.pv.scope[0] ? obj->ex.pv.scope[0] : 'g';
	scope_buf[1] = 0;

	sqlite3_reset( g_stmt_upsert_pv );
	sqlite3_clear_bindings( g_stmt_upsert_pv );
	sqlite3_bind_text( g_stmt_upsert_pv, 1, full_path, -1, SQLITE_TRANSIENT );
	sqlite3_bind_text( g_stmt_upsert_pv, 2, cpu ? cpu->descriptor : "", -1, SQLITE_TRANSIENT );
	sqlite3_bind_text( g_stmt_upsert_pv, 3, cpu ? cpu->ex.cpu.cputype : "", -1, SQLITE_TRANSIENT );
	sqlite3_bind_text( g_stmt_upsert_pv, 4, cpu ? cpu->ex.cpu.arversion : "", -1, SQLITE_TRANSIENT );
	if( task && (scope_buf[0] == 'l' || scope_buf[0] == 'd') ) {
		sqlite3_bind_text( g_stmt_upsert_pv, 5, task->descriptor, -1, SQLITE_TRANSIENT );
	} else {
		sqlite3_bind_null( g_stmt_upsert_pv, 5 );
	}
	sqlite3_bind_text( g_stmt_upsert_pv, 6, obj->descriptor, -1, SQLITE_TRANSIENT );
	sqlite3_bind_int(  g_stmt_upsert_pv, 7, (int)obj->ex.pv.type );
	sqlite3_bind_text( g_stmt_upsert_pv, 8, BrTypeName(obj->ex.pv.type), -1, SQLITE_STATIC );
	sqlite3_bind_text( g_stmt_upsert_pv, 9, scope_buf, -1, SQLITE_TRANSIENT );

	rc = sqlite3_step( g_stmt_upsert_pv );
	if( rc != SQLITE_DONE ) { DbError("step upsert_pv", rc); return -1; }

	/* INSERT OR IGNORE may be a no-op; always SELECT the id. */
	sqlite3_reset( g_stmt_lookup_pv );
	sqlite3_clear_bindings( g_stmt_lookup_pv );
	sqlite3_bind_text( g_stmt_lookup_pv, 1, full_path, -1, SQLITE_TRANSIENT );
	rc = sqlite3_step( g_stmt_lookup_pv );
	if( rc != SQLITE_ROW ) { DbError("step lookup_pv", rc); return -1; }
	if( out_pv_id ) *out_pv_id = sqlite3_column_int64( g_stmt_lookup_pv, 0 );
	sqlite3_reset( g_stmt_lookup_pv );

	return 0;
}

/* Binds the PV's current value to columns 4/5/6 (v_int / v_real / v_text) of
   g_stmt_insert_sample. Columns 1/2/3 (run_id, pv_id, ts_utc_ms) are the
   caller's responsibility. */
static void BindPvValue( PVIOBJECT *obj ) {
	void *pvalue = obj->ex.pv.pvalue;

	sqlite3_bind_null( g_stmt_insert_sample, 4 );
	sqlite3_bind_null( g_stmt_insert_sample, 5 );
	sqlite3_bind_null( g_stmt_insert_sample, 6 );

	if( pvalue == NULL ) return;

	switch( obj->ex.pv.type ) {
		case BR_BOOL:
		case BR_USINT:
			sqlite3_bind_int( g_stmt_insert_sample, 4, *(unsigned char*)pvalue );
			break;
		case BR_SINT:
			sqlite3_bind_int( g_stmt_insert_sample, 4, *(signed char*)pvalue );
			break;
		case BR_UINT:
			sqlite3_bind_int( g_stmt_insert_sample, 4, *(unsigned short*)pvalue );
			break;
		case BR_INT:
			sqlite3_bind_int( g_stmt_insert_sample, 4, *(signed short*)pvalue );
			break;
		case BR_UDINT:
		case BR_DATI:
		case BR_TIME:
		case BR_TOD:
		case BR_DATE:
			sqlite3_bind_int64( g_stmt_insert_sample, 4, *(unsigned int*)pvalue );
			break;
		case BR_DINT:
			sqlite3_bind_int( g_stmt_insert_sample, 4, *(signed int*)pvalue );
			break;
		case BR_REAL:
			sqlite3_bind_double( g_stmt_insert_sample, 5, (double)(*(float*)pvalue) );
			break;
		case BR_LREAL:
			sqlite3_bind_double( g_stmt_insert_sample, 5, *(double*)pvalue );
			break;
		case BR_STRING: {
			/* Mirror logger.c's escape of non-printables: 1 byte -> \xNNN (5 bytes).
			   Clamp at 120 chars like CSV does to keep row width bounded. */
			char buf[256];
			char *s = (char*)pvalue;
			char *d = buf;
			int count = 0;
			while( *s && count < (int)(sizeof(buf) - 6) ) {
				unsigned ch = (unsigned)(*s & 0xff);
				if( ch < ' ' ) {
					sprintf( d, "\\x%3.3x", ch );
					d += 5; ++s; count += 5;
				} else {
					if( obj->gui_info.interpret_as_oem ) OemToCharBuff( s++, d++, 1 );
					else                                 *d++ = *s++;
					++count;
				}
			}
			*d = 0;
			sqlite3_bind_text( g_stmt_insert_sample, 6, buf, -1, SQLITE_TRANSIENT );
			break;
		}
		case BR_WSTRING: {
			/* Convert wide to UTF-8 for text storage. */
			char u8[512];
			int n = WideCharToMultiByte( CP_UTF8, 0, (LPCWSTR)pvalue, -1, u8, sizeof(u8), NULL, NULL );
			if( n > 0 ) sqlite3_bind_text( g_stmt_insert_sample, 6, u8, -1, SQLITE_TRANSIENT );
			break;
		}
		default:
			break;
	}
}

int LoggerDbWriteSample( long long pv_id, long long ts_utc_ms, PVIOBJECT *obj ) {
	int rc;

	if( !g_db || g_run_id == 0 || !obj || obj->type != POBJ_PVAR ) return -1;

	sqlite3_reset( g_stmt_insert_sample );
	sqlite3_clear_bindings( g_stmt_insert_sample );
	sqlite3_bind_int64( g_stmt_insert_sample, 1, g_run_id );
	sqlite3_bind_int64( g_stmt_insert_sample, 2, pv_id );
	sqlite3_bind_int64( g_stmt_insert_sample, 3, ts_utc_ms );
	BindPvValue( obj );

	rc = sqlite3_step( g_stmt_insert_sample );
	if( rc != SQLITE_DONE ) { DbError("step insert_sample", rc); return -1; }
	return 0;
}

int LoggerDbBeginBatch( void ) {
	int rc;
	if( !g_db ) return -1;
	rc = sqlite3_exec( g_db, "BEGIN;", NULL, NULL, NULL );
	if( rc != SQLITE_OK ) { DbError("BEGIN", rc); return -1; }
	return 0;
}

int LoggerDbCommitBatch( void ) {
	int rc;
	if( !g_db ) return -1;
	rc = sqlite3_exec( g_db, "COMMIT;", NULL, NULL, NULL );
	if( rc != SQLITE_OK ) {
		DbError("COMMIT", rc);
		sqlite3_exec( g_db, "ROLLBACK;", NULL, NULL, NULL );
		return -1;
	}
	return 0;
}
