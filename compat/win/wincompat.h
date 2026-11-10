/* Windows-only helpers that the portable sources may call inside a
 * Q_OS_WIN / _WIN32 branch.  These deliberately have no POSIX twin: each one
 * covers a contract Windows cannot honour through the plain POSIX spelling.
 *
 *   zuuned_open_directory()  - open() with O_DIRECTORY is not expressible on
 *                              the CRT, and the descriptor it returns is what
 *                              the at() family and flock() resolve against.
 *   zuuned_open_self_fd()    - there is no /proc/self/fd/N.  Handing an
 *                              inherited descriptor (stderr) to a second
 *                              writer needs an explicit duplicate.
 *   zuuned_fd_readable()     - there is no non-blocking read.  Callers that
 *                              must not block on an empty pipe consult this
 *                              before reading.
 *   zuuned_win_runtime_init()- binary stdio mode and unbuffered stderr, set
 *                              before the first byte is touched.
 *   zuuned_win_console_utf8()- the console renders the active ANSI code page,
 *                              not UTF-8, so every path and title we print
 *                              turns into mojibake.  Any binary that prints
 *                              user-readable text calls this first; it asks
 *                              the console for UTF-8 and restores what it
 *                              found on exit.
 *   zuuned_win_output_available()- a GUI-subsystem image has no console, so
 *                              GetStdHandle() answers with nothing and every
 *                              diagnostic line is dropped.  This says whether
 *                              a caller still has somewhere to write, which is
 *                              how main() decides between stderr and a dialog.
 *   zuuned_win_notice()        - a GUI-subsystem process has no console, so a
 *                              decision not to start has to be said some other
 *                              way.  Modal, UTF-8 in, and the last channel left
 *                              to a double-clicked instance.
 *   zuuned_win_debug_console()- the other half: give the process a console on
 *                              request (--debug).  It never replaces a stream
 *                              that already works, so a harness or shell that
 *                              redirected our output keeps reading the pipe it
 *                              made, and it attaches to the parent terminal
 *                              before it allocates a window of its own.
 */
#ifndef ZUUNED_WIN_WINCOMPAT_H
#define ZUUNED_WIN_WINCOMPAT_H
#include "posix_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

int zuuned_open_directory(const char *path);
int zuuned_open_self_fd(int fd);
int zuuned_fd_readable(int fd);
void zuuned_win_runtime_init(void);
void zuuned_win_console_utf8(void);
int zuuned_win_output_available(void);
int zuuned_win_debug_console(void);
void zuuned_win_notice(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* ZUUNED_WIN_WINCOMPAT_H */