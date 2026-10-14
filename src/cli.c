#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "main.h"
#include "cli.h"
#include "report.h"
#include "pvi_interface.h"
#include "cpusearch.h"
#include "settings.h"
#include "pvicom.h"

#define MAX_ARGV 32

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

static Subcommand subcommands[] = {
	{ "scan",     CmdScan,    "Discover B&R CPUs on the network" },
	{ "list-pvs", CmdListPvs, "List PVs loaded from a watch file (--watch FILE)" },
	{ "--help",   CmdHelp,    "Show this help" },
	{ "-h",       CmdHelp,    "Show this help" },
	{ "help",     CmdHelp,    "Show this help" },
	{ NULL, NULL, NULL }
};

/* ---- argv parsing ----
   Splits lpCmdLine into argv. Honors double-quoted substrings so paths with
   spaces survive. argv[0] is the first real token (NOT the exe name, since
   WinMain's lpCmdLine already excludes it). */
static int ParseArgv( LPSTR cmdline, char *argv[MAX_ARGV], char *buf, int buflen ) {
	int argc = 0;
	char *src = cmdline;
	char *dst = buf;
	char *end = buf + buflen - 1;

	while( *src && argc < MAX_ARGV - 1 ) {
		while( *src == ' ' || *src == '\t' ) ++src;
		if( !*src ) break;

		argv[argc++] = dst;

		BOOL in_quote = FALSE;
		while( *src && dst < end ) {
			if( *src == '"' ) { in_quote = !in_quote; ++src; continue; }
			if( !in_quote && (*src == ' ' || *src == '\t') ) break;
			*dst++ = *src++;
		}
		if( dst < end ) *dst++ = 0;
		else break;
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

	/* Pump up to 3 s waiting for cbobjects_valid; PviCyclicRequests fires
	   every 500ms and processes the response queue. */
	{
		DWORD start = GetTickCount();
		while( !s_pvi_objects_valid && !s_abort && (GetTickCount() - start) < 3000 ) {
			PumpMessages( 100 );
		}
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

/* ---- dispatcher ---- */

static Subcommand *FindSubcommand( const char *name ) {
	for( Subcommand *s = subcommands; s->name; ++s ) {
		if( strcmp(name, s->name) == 0 ) return s;
	}
	return NULL;
}

BOOL CliTryDispatch( LPSTR lpCmdLine, HINSTANCE hInstance, int *out_exit_code ) {
	char buf[2048];
	char *argv[MAX_ARGV];
	int argc;
	Subcommand *sub;

	if( !lpCmdLine || !*lpCmdLine ) return FALSE;

	argc = ParseArgv( lpCmdLine, argv, buf, sizeof(buf) );
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

	SettingsInitialize();

	fflush( stdout );
	*out_exit_code = sub->func( argc, argv );
	fflush( stdout );
	fflush( stderr );
	return TRUE;
}
