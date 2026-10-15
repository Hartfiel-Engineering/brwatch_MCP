#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <time.h>
#include "main.h"
#include "cli.h"
#include "report.h"
#include "pvi_interface.h"
#include "cpusearch.h"
#include "settings.h"
#include "logger_db.h"
#include "sqlite/sqlite3.h"
#include "pvicom.h"

#define MAX_ARGV     256
#define ARGV_BUFSIZE 16384

typedef struct {
	const char *name;
	int (*func)(int argc, char **argv);
	const char *desc;
} Subcommand;

static HWND  s_hidden_wnd = NULL;
static volatile BOOL s_abort = FALSE;
static volatile BOOL s_pvi_objects_valid = FALSE;

/* ---- forward declarations ---- */
static int CmdHelp( int argc, char **argv );
static int CmdScan( int argc, char **argv );
static int CmdListPvs( int argc, char **argv );
static int CmdStartLogging( int argc, char **argv );
static int CmdStopLogging( int argc, char **argv );
static int CmdQuery( int argc, char **argv );
static int CmdDbInfo( int argc, char **argv );

static Subcommand subcommands[] = {
	{ "scan",          CmdScan,         "Discover B&R CPUs on the network" },
	{ "list-pvs",      CmdListPvs,      "List PVs loaded from a watch file (--watch FILE)" },
	{ "start-logging", CmdStartLogging, "Blocking logger - samples PVs to SQLite (--watch FILE, --pv cpu->var, --pv-file FILE)" },
	{ "stop-logging",  CmdStopLogging,  "Signal a running start-logging to exit cleanly" },
	{ "query",         CmdQuery,        "Query samples for a PV from the DB (--since/--until)" },
	{ "db-info",       CmdDbInfo,       "Show runs and PV counts in the DB" },
	{ "--help",        CmdHelp,         "Show this help" },
	{ "-h",            CmdHelp,         "Show this help" },
	{ "help",          CmdHelp,         "Show this help" },
	{ NULL, NULL, NULL }
};

/* ---- default path resolution ----
   Convention: a .brwatch/ folder in CWD holds brwatch.db, watch.ini,
   and the .stop sentinel. CLI commands default their path args here.
   Auto-created by start-logging if missing. */

static const char *DEFAULT_BRWATCH_DIR = ".brwatch";
static const char *DEFAULT_DB_NAME     = "brwatch.db";
static const char *DEFAULT_WATCH_NAME  = "watch.ini";
static const char *STOP_SENTINEL_NAME  = ".stop";

static void EnsureBrwatchDir( void ) {
	CreateDirectory( DEFAULT_BRWATCH_DIR, NULL );
}

static void BuildPathUnder( const char *dir, const char *name, char *out, int outlen ) {
	_snprintf( out, outlen, "%s\\%s", dir, name );
	out[outlen-1] = 0;
}

/* Parses --since/--until. Accepts:
     - "nowZ"  / "now"      -> current UTC epoch ms
     - "now-NN{s|m|h}"      -> N seconds/minutes/hours ago
     - decimal digits       -> treated as Unix epoch seconds (if <= 1e12) or ms (if >)
     - ISO-8601 "YYYY-MM-DDThh:mm:ss" assumed UTC
   Returns 0 on success and fills *out_ms. Returns -1 on parse failure. */
static int ParseTimeArg( const char *s, long long *out_ms ) {
	if( !s || !*s ) return -1;

	if( strncmp(s, "now", 3) == 0 ) {
		FILETIME ft; long long t;
		GetSystemTimeAsFileTime( &ft );
		t = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
		long long now_ms = (t - 116444736000000000LL) / 10000LL;

		if( s[3] == 0 ) { *out_ms = now_ms; return 0; }
		if( s[3] == '-' ) {
			long long n = atoll( s+4 );
			const char *end = s+4;
			while( *end && (*end == '-' || (*end >= '0' && *end <= '9')) ) ++end;
			char unit = *end ? *end : 's';
			long long mult_ms = 1000;
			switch( unit ) {
				case 's': mult_ms = 1000; break;
				case 'm': mult_ms = 60 * 1000; break;
				case 'h': mult_ms = 60 * 60 * 1000; break;
				case 'd': mult_ms = 24LL * 60 * 60 * 1000; break;
				default:  return -1;
			}
			*out_ms = now_ms - (n * mult_ms);
			return 0;
		}
		return -1;
	}

	/* All-digits: epoch seconds or ms */
	{
		int all_digits = 1;
		for( const char *p = s; *p; ++p ) {
			if( *p < '0' || *p > '9' ) { all_digits = 0; break; }
		}
		if( all_digits ) {
			long long v = atoll( s );
			*out_ms = (v > 100000000000LL) ? v : v * 1000LL;  /* heuristic */
			return 0;
		}
	}

	/* ISO-8601 YYYY-MM-DDThh:mm:ss assumed UTC */
	{
		struct tm t;
		int yr, mo, da, hr, mi, se;
		memset( &t, 0, sizeof(t) );
		if( sscanf(s, "%d-%d-%dT%d:%d:%d", &yr, &mo, &da, &hr, &mi, &se) >= 3 ) {
			t.tm_year = yr - 1900;
			t.tm_mon  = mo - 1;
			t.tm_mday = da;
			t.tm_hour = hr;
			t.tm_min  = mi;
			t.tm_sec  = se;
			time_t secs = _mkgmtime( &t );
			if( secs < 0 ) return -1;
			*out_ms = (long long)secs * 1000LL;
			return 0;
		}
	}

	return -1;
}

/* UTF-8-ish ms-since-epoch formatter: ISO-8601 UTC. */
static void FormatIsoUtc( long long ms, char *buf, int buflen ) {
	time_t secs = (time_t)(ms / 1000);
	int frac_ms = (int)(ms % 1000);
	struct tm *g = gmtime( &secs );
	if( !g ) { _snprintf(buf, buflen, "%lld", ms); return; }
	_snprintf( buf, buflen, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
	          g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
	          g->tm_hour, g->tm_min, g->tm_sec, frac_ms );
	buf[buflen-1] = 0;
}

static long long NowUtcMs( void ) {
	FILETIME ft;
	long long t;
	GetSystemTimeAsFileTime( &ft );
	t = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return (t - 116444736000000000LL) / 10000LL;
}

/* ---- argv parsing ----
   Splits lpCmdLine into argv. Honors double-quoted substrings so paths with
   spaces survive. argv[0] is the first real token (NOT the exe name, since
   WinMain's lpCmdLine already excludes it).
   *truncated is set if tokens were dropped (MAX_ARGV or buflen exceeded) so
   the caller can fail loudly instead of running with a partial argv. */
static int ParseArgv( LPSTR cmdline, char *argv[MAX_ARGV], char *buf, int buflen, BOOL *truncated ) {
	int argc = 0;
	char *src = cmdline;
	char *dst = buf;
	char *end = buf + buflen - 1;

	*truncated = FALSE;
	for( ;; ) {
		while( *src == ' ' || *src == '\t' ) ++src;
		if( !*src ) break;
		if( argc >= MAX_ARGV - 1 ) { *truncated = TRUE; break; }

		argv[argc++] = dst;

		BOOL in_quote = FALSE;
		while( *src && dst < end ) {
			if( *src == '"' ) { in_quote = !in_quote; ++src; continue; }
			if( !in_quote && (*src == ' ' || *src == '\t') ) break;
			*dst++ = *src++;
		}
		if( dst < end ) *dst++ = 0;
		else { *dst = 0; *truncated = TRUE; break; }
	}
	argv[argc] = NULL;
	return argc;
}

/* Bind a C stdio FILE* to whatever Windows handle is in the given std slot.
   For a GUI-subsystem exe the C runtime initializes stdout/stderr/stdin to
   invalid, so printf is a no-op by default. Connecting them explicitly is
   what makes CLI output actually appear - and importantly, respects any
   redirection the parent set up (pipe, file) via _open_osfhandle. */
static void BindStdHandle( DWORD which, FILE *fp, const char *mode ) {
	HANDLE h = GetStdHandle( which );
	DWORD t;
	int fd;
	FILE *f;
	int flags;

	if( h == NULL || h == INVALID_HANDLE_VALUE ) return;
	t = GetFileType( h );
	if( t == FILE_TYPE_UNKNOWN ) return;

	flags = (mode[0] == 'r') ? _O_RDONLY : _O_WRONLY;
	fd = _open_osfhandle( (intptr_t)h, flags | _O_BINARY );
	if( fd == -1 ) return;
	f = _fdopen( fd, mode );
	if( !f ) return;
	*fp = *f;
	setvbuf( fp, NULL, _IONBF, 0 );
}

/* ---- console setup ----
   AttachConsole grabs the parent cmd/powershell console when there is one.
   If no console and no stdio redirect, allocate a new console (double-click
   case). Then wire stdout/stderr/stdin to whatever Windows handed us -
   respecting pipe redirects the parent (e.g. Python MCP server) set up. */
static void SetupConsole( void ) {
	if( !AttachConsole(ATTACH_PARENT_PROCESS) ) {
		HANDLE hOut = GetStdHandle( STD_OUTPUT_HANDLE );
		DWORD t = (hOut && hOut != INVALID_HANDLE_VALUE) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
		if( t == FILE_TYPE_UNKNOWN ) {
			AllocConsole();
		}
	}
	BindStdHandle( STD_OUTPUT_HANDLE, stdout, "w" );
	BindStdHandle( STD_ERROR_HANDLE,  stderr, "w" );
	BindStdHandle( STD_INPUT_HANDLE,  stdin,  "r" );
}

/* ---- hidden message-only window ----
   PVI's SetTimer+callback machinery needs a message pump. A HWND_MESSAGE
   window costs nothing, never becomes visible, and never goes "Not Responding"
   because Windows applies that logic only to top-level visible windows.
   We also point g_hwndMainWindow at it so existing code paths that pass
   MainWindowGetHandle() to Win32 APIs keep working. */
static void SetupMessageWindow( HINSTANCE hInstance ) {
	s_hidden_wnd = CreateWindowEx( 0, "STATIC", NULL, 0, 0, 0, 0, 0,
	                              HWND_MESSAGE, NULL, hInstance, NULL );
	g_hwndMainWindow = s_hidden_wnd;
}

static BOOL WINAPI CtrlHandler( DWORD type ) {
	switch( type ) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
			s_abort = TRUE;
			PostQuitMessage(0);
			return TRUE;
	}
	return FALSE;
}

/* Pumps the message queue for approximately ms milliseconds, returning early
   if a WM_QUIT or Ctrl+C was received. PVI's cyclic timer fires every 500ms. */
static void PumpMessages( int ms ) {
	DWORD start = GetTickCount();
	MSG msg;
	while( !s_abort && (GetTickCount() - start) < (DWORD)ms ) {
		while( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
			if( msg.message == WM_QUIT ) { s_abort = TRUE; return; }
			TranslateMessage( &msg );
			DispatchMessage( &msg );
		}
		Sleep( 10 );
	}
}

static void CliObjectsValid( void ) {
	s_pvi_objects_valid = TRUE;
}

/* Pump messages until PVI reports objects valid (POBJ_EVENT_PVI_ARRANGE,
   where the line/device/CPU objects are created). A cold start that has to
   launch PVI Manager can take several seconds; without this event nothing
   exists to attach PVs to, so later steps would fail with a misleading
   "CPU not found". Returns FALSE (error printed) on timeout or abort. */
#define PVI_READY_TIMEOUT_MS 15000
static BOOL WaitForPviObjects( const char *cmd ) {
	DWORD start = GetTickCount();
	while( !s_pvi_objects_valid && !s_abort && (GetTickCount() - start) < PVI_READY_TIMEOUT_MS ) {
		PumpMessages( 100 );
	}
	if( s_pvi_objects_valid ) return TRUE;
	if( !s_abort ) {
		fprintf( stderr, "[ERROR] %s: PVI did not become ready within %d s "
		                 "(is PVI Manager running / installed?)\n", cmd, PVI_READY_TIMEOUT_MS / 1000 );
	}
	return FALSE;
}

/* ---- subcommand handlers ---- */

static int CmdHelp( int argc, char **argv ) {
	(void)argc; (void)argv;
	fprintf( stdout, "brwatch - B&R PLC diagnostic tool\n\n" );
	fprintf( stdout, "Usage: brwatch.exe <subcommand> [args]\n" );
	fprintf( stdout, "       brwatch.exe                   (no args: launch GUI)\n\n" );
	fprintf( stdout, "Subcommands:\n" );
	for( Subcommand *s = subcommands; s->name; ++s ) {
		fprintf( stdout, "  %-14s  %s\n", s->name, s->desc );
	}
	fprintf( stdout, "\nFlags common to data-producing subcommands:\n" );
	fprintf( stdout, "  --json          Output as JSON instead of TSV\n" );
	fprintf( stdout, "\nSee doc/cli.md for details.\n" );
	return 0;
}

static int CmdScan( int argc, char **argv ) {
	struct stEthernetCpuInfo *cpus;
	int n, i;
	BOOL as_json = FALSE;
	int result;
	char remote[80] = "COMT=10 AS=1 PT=20";

	for( int a = 1; a < argc; ++a ) {
		if( strcmp(argv[a], "--json") == 0 ) as_json = TRUE;
	}

	cpus = calloc( 256, sizeof(struct stEthernetCpuInfo) );
	if( !cpus ) {
		fprintf( stderr, "[ERROR] scan: out of memory\n" );
		return 2;
	}

	GetPrivateProfileString( "REMOTE", "parameter", "COMT=10 AS=1 PT=20",
	                        remote, sizeof(remote), SettingsGetFileName() );
	result = PviInitialize( 0, 0, remote, NULL );
	if( result ) {
		fprintf( stderr, "[ERROR] PviInitialize failed (rc=%d)\n", result );
		free( cpus );
		return 2;
	}

	n = SearchEthernetCpus( cpus, 256 );

	if( as_json ) {
		fprintf( stdout, "[\n" );
		for( i = 0; i < n; ++i ) {
			fprintf( stdout,
				"  {\"mac\":\"%s\",\"ip\":\"%s\",\"subnet\":\"%s\",\"gateway\":\"%s\","
				"\"target\":\"%s\",\"ar_version\":\"%s\",\"ar_state\":\"%s\","
				"\"snmp_mode\":%d,\"ina\":%d}%s\n",
				cpus[i].macAddress, cpus[i].ipAddress, cpus[i].subnetMask, cpus[i].gateway,
				cpus[i].targetTypeDescription, cpus[i].arVersion, cpus[i].arState,
				cpus[i].SNMP_mode, cpus[i].INA_activated,
				(i == n-1) ? "" : "," );
		}
		fprintf( stdout, "]\n" );
	} else {
		fprintf( stdout, "mac\tip\tsubnet\ttarget\tar_version\tar_state\tsnmp_mode\n" );
		for( i = 0; i < n; ++i ) {
			fprintf( stdout, "%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
				cpus[i].macAddress, cpus[i].ipAddress, cpus[i].subnetMask,
				cpus[i].targetTypeDescription, cpus[i].arVersion, cpus[i].arState,
				cpus[i].SNMP_mode );
		}
	}

	PviDeinitialize();
	free( cpus );
	return 0;
}

static int CmdListPvs( int argc, char **argv ) {
	BOOL as_json = FALSE;
	const char *watch_file = NULL;
	int result;
	PVIOBJECT *obj;
	int printed = 0;

	for( int a = 1; a < argc; ++a ) {
		if( strcmp(argv[a], "--json") == 0 ) as_json = TRUE;
		else if( strcmp(argv[a], "--watch") == 0 && a+1 < argc ) watch_file = argv[++a];
	}

	if( !watch_file ) {
		fprintf( stderr, "[ERROR] list-pvs: --watch FILE is required\n" );
		return 1;
	}

	/* Register the objects-valid callback before StartPvi so we can tell
	   when PVI has finished initial setup. */
	pvi_interface_notify.cbobjects_valid = CliObjectsValid;

	result = StartPvi();
	if( result ) {
		fprintf( stderr, "[ERROR] StartPvi failed (rc=%d)\n", result );
		return 2;
	}

	if( !WaitForPviObjects( "list-pvs" ) ) {
		StopPvi();
		return 2;
	}

	LoadPviObjectsFromWatchFile( POBJ_CPU,  (char*)watch_file );
	LoadPviObjectsFromWatchFile( POBJ_TASK, (char*)watch_file );
	LoadPviObjectsFromWatchFile( POBJ_PVAR, (char*)watch_file );

	/* Give PVI a short window to resolve link IDs for the loaded objects. */
	PumpMessages( 500 );

	if( as_json ) fprintf( stdout, "[\n" );
	else          fprintf( stdout, "full_path\tbr_type\tscope\n" );

	obj = GetNextPviObject( TRUE );
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type != POBJ_PVAR ) continue;

		PVIOBJECT *task = obj->ex.pv.task;
		PVIOBJECT *cpu  = obj->ex.pv.cpu;
		char full_path[768];
		const char *cputype = (cpu && cpu->ex.cpu.cputype[0])   ? cpu->ex.cpu.cputype   : "";
		const char *arver   = (cpu && cpu->ex.cpu.arversion[0]) ? cpu->ex.cpu.arversion : "";
		const char *cpudesc = (cpu)  ? cpu->descriptor  : "";
		const char *tdesc   = (task) ? task->descriptor : "";

		if( obj->ex.pv.scope[0] == 'l' || obj->ex.pv.scope[0] == 'd' ) {
			_snprintf( full_path, sizeof(full_path), "%s[%s]%s->%s:%s",
			          cputype, arver, cpudesc, tdesc, obj->descriptor );
		} else {
			_snprintf( full_path, sizeof(full_path), "%s[%s]%s->%s",
			          cputype, arver, cpudesc, obj->descriptor );
		}
		full_path[sizeof(full_path)-1] = 0;

		if( as_json ) {
			fprintf( stdout, "%s  {\"full_path\":\"%s\",\"br_type\":%d,\"scope\":\"%c\"}",
			         printed ? ",\n" : "",
			         full_path,
			         (int)obj->ex.pv.type,
			         obj->ex.pv.scope[0] ? obj->ex.pv.scope[0] : 'g' );
		} else {
			fprintf( stdout, "%s\t%d\t%c\n",
			         full_path,
			         (int)obj->ex.pv.type,
			         obj->ex.pv.scope[0] ? obj->ex.pv.scope[0] : 'g' );
		}
		++printed;
	}
	if( as_json ) fprintf( stdout, "\n]\n" );

	StopPvi();
	return 0;
}

/* ---- Phase 4: start-logging ----
   Blocking subcommand. Starts PVI, loads the watch file, opens the SQLite DB,
   begins a run, enters a message loop with a WM_TIMER tick that writes samples
   each cycle. Stops on Ctrl+C or when the stop sentinel appears. */

static int CountWatchlistPvs( void ) {
	PVIOBJECT *obj;
	int n = 0;
	obj = GetNextPviObject( TRUE );
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_PVAR && obj->watchsort >= 0 ) ++n;
	}
	return n;
}

static void WriteSampleTick( long long ts_ms ) {
	PVIOBJECT *obj;
	LoggerDbBeginBatch();
	obj = GetNextPviObject( TRUE );
	while( (obj = GetNextPviObject(FALSE)) != NULL ) {
		if( obj->type == POBJ_PVAR && obj->watchsort >= 0 ) {
			long long pv_id = 0;
			if( LoggerDbUpsertPv( obj, &pv_id ) == 0 && pv_id > 0 ) {
				LoggerDbWriteSample( pv_id, ts_ms, obj );
			}
		}
	}
	LoggerDbCommitBatch();
}

/* Add one PV spec to the watch list. Format:
   - "cpu_desc->var_name"           -> global PVAR
   - "cpu_desc->task_desc:var_name" -> local PVAR
   Returns 0 on success, 1 on a malformed spec, 2 if PVI rejected it. */
static int AddPvSpec( const char *spec ) {
	const char *arrow = strstr( spec, "->" );
	char cpu_desc[128];
	size_t cpu_len;
	const char *after_arrow;
	const char *colon;

	if( !arrow ) {
		fprintf( stderr, "[ERROR] start-logging: PV spec '%s' missing '->' (expected cpu_desc->[task:]var_name)\n", spec );
		return 1;
	}
	cpu_len = (size_t)(arrow - spec);
	if( cpu_len >= sizeof(cpu_desc) ) cpu_len = sizeof(cpu_desc) - 1;
	memcpy( cpu_desc, spec, cpu_len );
	cpu_desc[cpu_len] = 0;

	after_arrow = arrow + 2;
	colon = strchr( after_arrow, ':' );
	if( colon ) {
		/* local: cpu_desc->task_desc:var_name */
		char task_desc[128], var_name[256];
		size_t task_len = (size_t)(colon - after_arrow);
		if( task_len >= sizeof(task_desc) ) task_len = sizeof(task_desc) - 1;
		memcpy( task_desc, after_arrow, task_len );
		task_desc[task_len] = 0;
		strncpy( var_name, colon + 1, sizeof(var_name) - 1 );
		var_name[sizeof(var_name) - 1] = 0;
		return AddLocalPvarByCpuTaskDescriptor( cpu_desc, task_desc, var_name ) ? 2 : 0;
	}
	/* global: cpu_desc->var_name */
	return AddGlobalPvarByCpuDescriptor( cpu_desc, after_arrow ) ? 2 : 0;
}

/* Add every PV spec in a --pv-file: one spec per line, blank lines and
   lines starting with '#' ignored. Returns 0 on success or AddPvSpec's
   error code; *added receives the number of specs added. */
static int AddPvSpecsFromFile( const char *path, int *added ) {
	char line[1024];
	FILE *f = fopen( path, "r" );
	int first = 1;

	*added = 0;
	if( !f ) {
		fprintf( stderr, "[ERROR] start-logging: cannot open --pv-file %s\n", path );
		return 2;
	}
	while( fgets( line, sizeof(line), f ) ) {
		char *s = line;
		char *e;
		int rc;

		/* Skip a UTF-8 BOM on the first line. */
		if( first && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF ) s += 3;
		first = 0;

		while( *s == ' ' || *s == '\t' ) ++s;
		e = s + strlen( s );
		while( e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t') ) --e;
		*e = 0;
		if( !*s || *s == '#' ) continue;

		rc = AddPvSpec( s );
		if( rc ) { fclose( f ); return rc; }
		++*added;
	}
	fclose( f );
	return 0;
}

static int CmdStartLogging( int argc, char **argv ) {
	int cycle_ms = 500;
	const char *db_path = NULL;
	const char *watch_file = NULL;
	const char *notes = NULL;
	/* --pv entries: inline PV specs of the form "cpu_desc->[task:]var_name".
	   argc < MAX_ARGV, so the arrays can't overflow. */
	const char *inline_pvs[MAX_ARGV];
	int inline_pv_count = 0;
	/* --pv-file entries: files of PV specs, one per line */
	const char *pv_files[MAX_ARGV];
	int pv_file_count = 0;
	int file_pv_count = 0;
	char db_default[MAX_PATH];
	char watch_default[MAX_PATH];
	char sentinel_path[MAX_PATH];
	int result;
	int watchlist_size;

	for( int a = 1; a < argc; ++a ) {
		if( strcmp(argv[a], "--cycle") == 0 && a+1 < argc )       cycle_ms   = atoi(argv[++a]);
		else if( strcmp(argv[a], "--db") == 0 && a+1 < argc )     db_path    = argv[++a];
		else if( strcmp(argv[a], "--watch") == 0 && a+1 < argc )  watch_file = argv[++a];
		else if( strcmp(argv[a], "--notes") == 0 && a+1 < argc )  notes      = argv[++a];
		else if( strcmp(argv[a], "--pv") == 0 && a+1 < argc )      inline_pvs[inline_pv_count++] = argv[++a];
		else if( strcmp(argv[a], "--pv-file") == 0 && a+1 < argc ) pv_files[pv_file_count++]     = argv[++a];
	}

	if( cycle_ms < 50 ) {
		fprintf( stderr, "[ERROR] start-logging: --cycle must be at least 50 ms\n" );
		return 1;
	}

	EnsureBrwatchDir();
	if( !db_path ) {
		BuildPathUnder( DEFAULT_BRWATCH_DIR, DEFAULT_DB_NAME, db_default, sizeof(db_default) );
		db_path = db_default;
	}
	BuildPathUnder( DEFAULT_BRWATCH_DIR, STOP_SENTINEL_NAME, sentinel_path, sizeof(sentinel_path) );

	/* Clear any stale sentinel from a previous run. */
	DeleteFile( sentinel_path );

	pvi_interface_notify.cbobjects_valid = CliObjectsValid;

	result = StartPvi();
	if( result ) {
		fprintf( stderr, "[ERROR] start-logging: StartPvi failed (rc=%d)\n", result );
		return 2;
	}

	if( !WaitForPviObjects( "start-logging" ) ) {
		StopPvi();
		return 2;
	}

	/* Load the watch file if requested OR fall back to default. If --pv or
	   --pv-file args are present and no explicit --watch, skip the default-file load. */
	if( watch_file || (inline_pv_count == 0 && pv_file_count == 0) ) {
		if( !watch_file ) {
			BuildPathUnder( DEFAULT_BRWATCH_DIR, DEFAULT_WATCH_NAME, watch_default, sizeof(watch_default) );
			watch_file = watch_default;
		}
		if( GetFileAttributes(watch_file) != INVALID_FILE_ATTRIBUTES ) {
			if( LoadPviObjectsFromWatchFile( POBJ_CPU,  (char*)watch_file ) < 0 ||
			    LoadPviObjectsFromWatchFile( POBJ_TASK, (char*)watch_file ) < 0 ||
			    LoadPviObjectsFromWatchFile( POBJ_PVAR, (char*)watch_file ) < 0 ) {
				fprintf( stderr, "[ERROR] start-logging: could not load watch file %s\n", watch_file );
				StopPvi();
				return 2;
			}
		} else if( inline_pv_count == 0 && pv_file_count == 0 ) {
			fprintf( stderr, "[ERROR] start-logging: no watch file at %s and no --pv/--pv-file args\n", watch_file );
			StopPvi();
			return 2;
		}
	}

	/* Add any --pv inline specs, then any --pv-file specs. */
	for( int i = 0; i < inline_pv_count; ++i ) {
		int rc = AddPvSpec( inline_pvs[i] );
		if( rc ) { StopPvi(); return rc; }  /* error already printed */
	}
	for( int i = 0; i < pv_file_count; ++i ) {
		int added;
		int rc = AddPvSpecsFromFile( pv_files[i], &added );
		if( rc ) { StopPvi(); return rc; }
		file_pv_count += added;
	}

	/* Let PVI resolve link IDs for the loaded objects. */
	PumpMessages( 1000 );

	watchlist_size = CountWatchlistPvs();
	if( watchlist_size == 0 ) {
		fprintf( stderr, "[ERROR] start-logging: no PVARs in watch list (watch_file=%s, --pv count=%d, --pv-file count=%d)\n",
		         watch_file ? watch_file : "(none)", inline_pv_count, file_pv_count );
		StopPvi();
		return 2;
	}

	if( LoggerDbOpen( db_path ) != 0 ) {
		StopPvi();
		return 2;
	}
	if( LoggerDbBeginRun( cycle_ms, watch_file, notes ) != 0 ) {
		LoggerDbClose();
		StopPvi();
		return 2;
	}

	fprintf( stderr, "[INFO] start-logging: run started, %d PVs, %d ms cycle, db=%s\n",
	         watchlist_size, cycle_ms, db_path );
	fflush( stderr );

	/* Main loop: tick every cycle_ms, check sentinel each tick. */
	{
		DWORD next_tick = GetTickCount();
		while( !s_abort ) {
			DWORD now = GetTickCount();
			if( (int)(now - next_tick) >= 0 ) {
				WriteSampleTick( NowUtcMs() );
				next_tick += cycle_ms;
				/* If we've drifted far behind (sleep, debugger break), resync. */
				if( (int)(GetTickCount() - next_tick) > cycle_ms * 4 ) {
					next_tick = GetTickCount() + cycle_ms;
				}
			}

			if( GetFileAttributes( sentinel_path ) != INVALID_FILE_ATTRIBUTES ) {
				fprintf( stderr, "[INFO] start-logging: stop sentinel detected\n" );
				DeleteFile( sentinel_path );
				break;
			}

			PumpMessages( 50 );
		}
	}

	LoggerDbEndRun();
	LoggerDbClose();
	StopPvi();
	fprintf( stderr, "[INFO] start-logging: clean exit\n" );
	return 0;
}

/* ---- Phase 4: stop-logging ----
   Writes an empty sentinel file that a running start-logging process polls
   for. Simple, no IPC, cross-process safe. */

static int CmdStopLogging( int argc, char **argv ) {
	const char *dir = DEFAULT_BRWATCH_DIR;
	char sentinel_path[MAX_PATH];
	HANDLE h;

	for( int a = 1; a < argc; ++a ) {
		if( strcmp(argv[a], "--dir") == 0 && a+1 < argc ) dir = argv[++a];
	}

	BuildPathUnder( dir, STOP_SENTINEL_NAME, sentinel_path, sizeof(sentinel_path) );
	h = CreateFile( sentinel_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	               FILE_ATTRIBUTE_NORMAL, NULL );
	if( h == INVALID_HANDLE_VALUE ) {
		fprintf( stderr, "[ERROR] stop-logging: could not create sentinel %s (rc=%lu)\n",
		         sentinel_path, GetLastError() );
		return 2;
	}
	CloseHandle( h );
	fprintf( stderr, "[INFO] stop-logging: sentinel written to %s\n", sentinel_path );
	return 0;
}

/* ---- Phase 4: query ----
   Reads samples for a PV from the DB (opens read-only; WAL mode lets this
   coexist with a running start-logging). Outputs TSV by default, JSON with
   --json. The <pv> is matched against pvs.full_path. */

static int CmdQuery( int argc, char **argv ) {
	const char *db_path = NULL;
	const char *pv_name = NULL;
	long long t_since_ms = -1, t_until_ms = -1;
	long long run_id = -1;
	BOOL as_json = FALSE;
	char db_default[MAX_PATH];
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;
	int printed = 0;

	for( int a = 1; a < argc; ++a ) {
		if( argv[a][0] != '-' && !pv_name )                             pv_name    = argv[a];
		else if( strcmp(argv[a], "--db") == 0 && a+1 < argc )           db_path    = argv[++a];
		else if( strcmp(argv[a], "--since") == 0 && a+1 < argc ) {
			if( ParseTimeArg(argv[++a], &t_since_ms) != 0 ) {
				fprintf( stderr, "[ERROR] query: bad --since value: %s\n", argv[a] );
				return 1;
			}
		}
		else if( strcmp(argv[a], "--until") == 0 && a+1 < argc ) {
			if( ParseTimeArg(argv[++a], &t_until_ms) != 0 ) {
				fprintf( stderr, "[ERROR] query: bad --until value: %s\n", argv[a] );
				return 1;
			}
		}
		else if( strcmp(argv[a], "--run") == 0 && a+1 < argc )          run_id = atoll(argv[++a]);
		else if( strcmp(argv[a], "--json") == 0 )                       as_json = TRUE;
	}

	if( !pv_name ) {
		fprintf( stderr, "[ERROR] query: PV name required\n" );
		return 1;
	}
	if( !db_path ) {
		BuildPathUnder( DEFAULT_BRWATCH_DIR, DEFAULT_DB_NAME, db_default, sizeof(db_default) );
		db_path = db_default;
	}

	rc = sqlite3_open_v2( db_path, &db, SQLITE_OPEN_READONLY, NULL );
	if( rc != SQLITE_OK ) {
		fprintf( stderr, "[ERROR] query: open %s failed: %s\n", db_path,
		         db ? sqlite3_errmsg(db) : "unknown" );
		if( db ) sqlite3_close(db);
		return 2;
	}

	/* Build dynamic WHERE based on which filters were supplied. */
	{
		char sql[512];
		strcpy( sql,
			"SELECT s.ts_utc_ms, s.v_int, s.v_real, s.v_text, s.run_id "
			"FROM samples s JOIN pvs p ON p.pv_id = s.pv_id "
			"WHERE p.full_path = ?" );
		if( t_since_ms >= 0 ) strcat( sql, " AND s.ts_utc_ms >= ?" );
		if( t_until_ms >= 0 ) strcat( sql, " AND s.ts_utc_ms <= ?" );
		if( run_id >= 0 )     strcat( sql, " AND s.run_id = ?" );
		strcat( sql, " ORDER BY s.ts_utc_ms" );

		rc = sqlite3_prepare_v2( db, sql, -1, &stmt, NULL );
		if( rc != SQLITE_OK ) {
			fprintf( stderr, "[ERROR] query: prepare failed: %s\n", sqlite3_errmsg(db) );
			sqlite3_close( db );
			return 2;
		}
	}

	{
		int bind_idx = 1;
		sqlite3_bind_text( stmt, bind_idx++, pv_name, -1, SQLITE_TRANSIENT );
		if( t_since_ms >= 0 ) sqlite3_bind_int64( stmt, bind_idx++, t_since_ms );
		if( t_until_ms >= 0 ) sqlite3_bind_int64( stmt, bind_idx++, t_until_ms );
		if( run_id     >= 0 ) sqlite3_bind_int64( stmt, bind_idx++, run_id );
	}

	if( as_json ) fprintf( stdout, "[\n" );
	else          fprintf( stdout, "ts_iso\tts_utc_ms\tvalue\trun_id\n" );

	while( (rc = sqlite3_step(stmt)) == SQLITE_ROW ) {
		long long ts = sqlite3_column_int64( stmt, 0 );
		int   has_int = sqlite3_column_type(stmt, 1) != SQLITE_NULL;
		int   has_real = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
		int   has_text = sqlite3_column_type(stmt, 3) != SQLITE_NULL;
		long long rid = sqlite3_column_int64( stmt, 4 );
		char ts_iso[40];
		FormatIsoUtc( ts, ts_iso, sizeof(ts_iso) );

		if( as_json ) {
			fprintf( stdout, "%s  {\"ts_iso\":\"%s\",\"ts_utc_ms\":%lld,\"value\":",
			         printed ? ",\n" : "", ts_iso, ts );
			if( has_int )       fprintf( stdout, "%lld", sqlite3_column_int64(stmt, 1) );
			else if( has_real ) fprintf( stdout, "%g",   sqlite3_column_double(stmt, 2) );
			else if( has_text ) fprintf( stdout, "\"%s\"", (const char*)sqlite3_column_text(stmt, 3) );
			else                fprintf( stdout, "null" );
			fprintf( stdout, ",\"run_id\":%lld}", rid );
		} else {
			fprintf( stdout, "%s\t%lld\t", ts_iso, ts );
			if( has_int )       fprintf( stdout, "%lld", sqlite3_column_int64(stmt, 1) );
			else if( has_real ) fprintf( stdout, "%g",   sqlite3_column_double(stmt, 2) );
			else if( has_text ) fprintf( stdout, "%s",   (const char*)sqlite3_column_text(stmt, 3) );
			fprintf( stdout, "\t%lld\n", rid );
		}
		++printed;
	}
	if( as_json ) fprintf( stdout, "\n]\n" );

	sqlite3_finalize( stmt );
	sqlite3_close( db );

	if( printed == 0 ) {
		fprintf( stderr, "[INFO] query: no rows matched (PV=\"%s\")\n", pv_name );
	}
	return 0;
}

/* ---- Phase 4: db-info ----
   Dumps runs table + PV counts for the DB. */

static int CmdDbInfo( int argc, char **argv ) {
	const char *db_path = NULL;
	BOOL as_json = FALSE;
	char db_default[MAX_PATH];
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	int rc;
	int printed = 0;

	for( int a = 1; a < argc; ++a ) {
		if( strcmp(argv[a], "--db") == 0 && a+1 < argc ) db_path = argv[++a];
		else if( strcmp(argv[a], "--json") == 0 )        as_json = TRUE;
	}

	if( !db_path ) {
		BuildPathUnder( DEFAULT_BRWATCH_DIR, DEFAULT_DB_NAME, db_default, sizeof(db_default) );
		db_path = db_default;
	}

	rc = sqlite3_open_v2( db_path, &db, SQLITE_OPEN_READONLY, NULL );
	if( rc != SQLITE_OK ) {
		fprintf( stderr, "[ERROR] db-info: open %s failed: %s\n", db_path,
		         db ? sqlite3_errmsg(db) : "unknown" );
		if( db ) sqlite3_close(db);
		return 2;
	}

	rc = sqlite3_prepare_v2( db,
		"SELECT r.run_id, r.started_utc, r.stopped_utc, r.cycle_ms, "
		"       COALESCE(r.watch_file,''), COALESCE(r.notes,''), "
		"       (SELECT COUNT(DISTINCT pv_id) FROM samples WHERE run_id = r.run_id), "
		"       (SELECT COUNT(*) FROM samples WHERE run_id = r.run_id) "
		"FROM runs r ORDER BY r.started_utc DESC",
		-1, &stmt, NULL );
	if( rc != SQLITE_OK ) {
		fprintf( stderr, "[ERROR] db-info: prepare failed: %s\n", sqlite3_errmsg(db) );
		sqlite3_close( db );
		return 2;
	}

	if( as_json ) fprintf( stdout, "[\n" );
	else          fprintf( stdout, "run_id\tstarted_iso\tstopped_iso\tcycle_ms\tpv_count\tsample_count\tnotes\twatch_file\n" );

	while( (rc = sqlite3_step(stmt)) == SQLITE_ROW ) {
		long long run_id      = sqlite3_column_int64( stmt, 0 );
		long long started_sec = sqlite3_column_int64( stmt, 1 );
		int       stopped_is_null = (sqlite3_column_type(stmt, 2) == SQLITE_NULL);
		long long stopped_sec = sqlite3_column_int64( stmt, 2 );
		int cycle_ms   = sqlite3_column_int( stmt, 3 );
		const char *watch = (const char*)sqlite3_column_text( stmt, 4 );
		const char *notes = (const char*)sqlite3_column_text( stmt, 5 );
		long long pv_count     = sqlite3_column_int64( stmt, 6 );
		long long sample_count = sqlite3_column_int64( stmt, 7 );
		char started_iso[40], stopped_iso[40];

		FormatIsoUtc( started_sec * 1000, started_iso, sizeof(started_iso) );
		if( stopped_is_null ) strcpy( stopped_iso, "(running)" );
		else                  FormatIsoUtc( stopped_sec * 1000, stopped_iso, sizeof(stopped_iso) );

		if( as_json ) {
			fprintf( stdout, "%s  {\"run_id\":%lld,\"started_iso\":\"%s\","
			                 "\"stopped_iso\":%s%s%s,\"cycle_ms\":%d,"
			                 "\"pv_count\":%lld,\"sample_count\":%lld,"
			                 "\"notes\":\"%s\",\"watch_file\":\"%s\"}",
			         printed ? ",\n" : "", run_id, started_iso,
			         stopped_is_null ? "null" : "\"",
			         stopped_is_null ? ""     : stopped_iso,
			         stopped_is_null ? ""     : "\"",
			         cycle_ms, pv_count, sample_count,
			         notes ? notes : "", watch ? watch : "" );
		} else {
			fprintf( stdout, "%lld\t%s\t%s\t%d\t%lld\t%lld\t%s\t%s\n",
			         run_id, started_iso, stopped_iso, cycle_ms,
			         pv_count, sample_count,
			         notes ? notes : "", watch ? watch : "" );
		}
		++printed;
	}
	if( as_json ) fprintf( stdout, "\n]\n" );

	sqlite3_finalize( stmt );
	sqlite3_close( db );
	return 0;
}

/* ---- dispatcher ---- */

static Subcommand *FindSubcommand( const char *name ) {
	for( Subcommand *s = subcommands; s->name; ++s ) {
		if( strcmp(name, s->name) == 0 ) return s;
	}
	return NULL;
}

BOOL CliTryDispatch( LPSTR lpCmdLine, HINSTANCE hInstance, int *out_exit_code ) {
	static char buf[ARGV_BUFSIZE];
	char *argv[MAX_ARGV];
	int argc;
	BOOL truncated;
	Subcommand *sub;

	if( !lpCmdLine || !*lpCmdLine ) return FALSE;

	argc = ParseArgv( lpCmdLine, argv, buf, sizeof(buf), &truncated );
	if( argc == 0 ) return FALSE;

	sub = FindSubcommand( argv[0] );
	if( !sub ) {
		/* Not a known subcommand - treat as a GUI launch (preserves the
		   case where Explorer passes a file path when double-clicking
		   a .wtc file associated with brwatch). */
		return FALSE;
	}

	/* Enter CLI mode. g_cli_mode flips ReportError to stderr. */
	g_cli_mode = TRUE;
	SetupConsole();
	SetupMessageWindow( hInstance );
	SetConsoleCtrlHandler( CtrlHandler, TRUE );

	if( truncated ) {
		fprintf( stderr, "[ERROR] %s: command line too long (max %d args / %d bytes); "
		                 "use --pv-file for large PV lists\n", argv[0], MAX_ARGV - 1, ARGV_BUFSIZE );
		fflush( stderr );
		*out_exit_code = 1;
		return TRUE;
	}

	SettingsInitialize();

	fflush( stdout );
	*out_exit_code = sub->func( argc, argv );
	fflush( stdout );
	fflush( stderr );
	return TRUE;
}
