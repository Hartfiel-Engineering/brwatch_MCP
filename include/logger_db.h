#ifndef __LOGGER_DB_H
#define __LOGGER_DB_H

#include <windows.h>
#include "pvi_interface.h"

/*
 * SQLite logging backend.
 *
 * Writes samples to a WAL-mode SQLite file, so a separate reader (the MCP
 * server) can query while a logging run is active. Called from logger.c's
 * tick path when a SQLite file is configured; also used by the CLI.
 *
 * Lifecycle:
 *   LoggerDbOpen(path)          -- open file, create schema if missing
 *   LoggerDbBeginRun(...)       -- INSERT into runs, remembers run_id
 *   repeated per tick:
 *     LoggerDbBeginBatch()      -- BEGIN
 *     LoggerDbUpsertPv(obj)     -- lazy insert into pvs; cached by linkid
 *     LoggerDbWriteSample(...)  -- INSERT INTO samples
 *     LoggerDbCommitBatch()     -- COMMIT
 *   LoggerDbEndRun()            -- set runs.stopped_utc
 *   LoggerDbClose()             -- finalize statements, close db
 *
 * Error reporting goes through ReportError, so GUI pops MessageBox and
 * CLI prints to stderr.
 */

int  LoggerDbOpen( const char *path );
int  LoggerDbClose( void );
BOOL LoggerDbIsOpen( void );

/* Returns 0 on success. Stores the new run_id for subsequent sample inserts. */
int  LoggerDbBeginRun( int cycle_ms, const char *watch_file, const char *notes );
int  LoggerDbEndRun( void );

/* Upserts the PV into the pvs table and returns its pv_id in out_pv_id.
   Reuses the full-path string that logger.c already builds (cputype[ar]cpu->[task:]var). */
int  LoggerDbUpsertPv( PVIOBJECT *obj, long long *out_pv_id );

/* Writes one sample for the given PV. ts_utc_ms is UTC milliseconds since epoch. */
int  LoggerDbWriteSample( long long pv_id, long long ts_utc_ms, PVIOBJECT *obj );

/* Batch wrapper for per-tick atomicity. */
int  LoggerDbBeginBatch( void );
int  LoggerDbCommitBatch( void );

/* Utility: build the canonical "cputype[arversion]cpu_desc->[task_desc:]var_desc"
   string that the CSV logger uses, so DB and CSV reference the same PV name.
   Writes up to buflen-1 chars + NUL. Returns written length (not counting NUL). */
int  LoggerDbFormatPvFullPath( PVIOBJECT *obj, char *buf, int buflen );

#endif
