#ifndef __REPORT_H
#define __REPORT_H

#include <windows.h>

/*
 * Unified error/info reporting.
 *
 * In GUI mode (g_cli_mode == FALSE) each call pops a MessageBox parented to
 * MainWindowGetHandle(). In CLI mode (g_cli_mode == TRUE) the message is
 * written to stderr and the formatted text is stored in g_last_error so the
 * CLI dispatcher can set a non-zero exit code.
 *
 * Phase 1 only wires the GUI path; g_cli_mode stays FALSE until Phase 3.
 */

typedef enum ReportSeverity {
	REPORT_ERROR = 0,  /* MB_ICONERROR */
	REPORT_WARN,       /* MB_ICONWARNING */
	REPORT_INFO        /* MB_ICONINFORMATION */
} ReportSeverity;

extern BOOL g_cli_mode;
extern char g_last_error[256];

void ReportError( ReportSeverity sev, const char *title, const char *fmt, ... );
void ReportInfo( const char *title, const char *msg );

#endif
