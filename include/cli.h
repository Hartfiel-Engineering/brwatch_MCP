#ifndef __CLI_H
#define __CLI_H

#include <windows.h>

/*
 * Command-line entry point.
 *
 * Called from WinMain between SettingsInitialize() and InitCommonControls().
 * If lpCmdLine contains a subcommand, dispatches to it, runs its own message
 * loop as needed, and returns the exit code via *out_exit_code.
 *
 * Returns:
 *   TRUE  - CLI mode was entered. WinMain should return *out_exit_code
 *           without creating the GUI window.
 *   FALSE - No CLI args were present (empty command line, or just a path
 *           quoted by Explorer when launched by double-click). WinMain
 *           should proceed with GUI initialization.
 */

BOOL CliTryDispatch( LPSTR lpCmdLine, HINSTANCE hInstance, int *out_exit_code );

#endif
