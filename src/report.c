#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "main.h"
#include "report.h"

BOOL g_cli_mode = FALSE;
char g_last_error[256] = {0};

static UINT SeverityToIcon( ReportSeverity sev ) {
	switch( sev ) {
		case REPORT_WARN: return MB_ICONWARNING;
		case REPORT_INFO: return MB_ICONINFORMATION;
		case REPORT_ERROR:
		default:          return MB_ICONERROR;
	}
}

static const char *SeverityToTag( ReportSeverity sev ) {
	switch( sev ) {
		case REPORT_WARN: return "WARN";
		case REPORT_INFO: return "INFO";
		case REPORT_ERROR:
		default:          return "ERROR";
	}
}

void ReportError( ReportSeverity sev, const char *title, const char *fmt, ... ) {
	char buf[512];
	va_list ap;

	va_start( ap, fmt );
	_vsnprintf( buf, sizeof(buf), fmt, ap );
	va_end( ap );
	buf[sizeof(buf)-1] = 0;

	if( sev == REPORT_ERROR ) {
		strncpy( g_last_error, buf, sizeof(g_last_error)-1 );
		g_last_error[sizeof(g_last_error)-1] = 0;
	}

	if( g_cli_mode ) {
		fprintf( stderr, "[%s] %s: %s\n", SeverityToTag(sev), title ? title : "", buf );
		fflush( stderr );
	} else {
		MessageBox( MainWindowGetHandle(), buf, title ? title : "", MB_OK | SeverityToIcon(sev) );
	}
}

void ReportInfo( const char *title, const char *msg ) {
	ReportError( REPORT_INFO, title, "%s", msg ? msg : "" );
}
