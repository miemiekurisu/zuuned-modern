/* Zuuned Windows (MSVC) POSIX compatibility entry point.
 *
 * Force-included (/FIposix_shim.h) into the shared C targets on Windows and
 * pulled in transitively by compat/win/unistd.h and friends for the C++ app.
 * Linux and macOS builds never compile this directory, so the portable sources
 * stay byte-identical between platforms.
 *
 * Style rules for this header, learned the hard way:
 *  - Declare, do not macro.  Every name the UCRT does not ship (stat, opendir,
 *    poll, mmap, flock, openat, pipe, usleep, ...) is a plain declaration that
 *    wincompat.c defines.  Object-like macros would rewrite unrelated members
 *    in Qt and Windows headers.
 *  - Macro only where the UCRT already ships a narrower name (off_t, ino_t,
 *    lseek, strdup), and then only where no CRT declaration can collide.
 *  - Never fake a guarantee.  Where Windows cannot honour a POSIX contract the
 *    declaration says so and the caller must branch explicitly.
 */
#ifndef ZUUNED_WIN_POSIX_SHIM_H
#define ZUUNED_WIN_POSIX_SHIM_H

#if !defined(_WIN32)
#error "compat/win/posix_shim.h is Windows-only"
#endif

/* UCRT hides the non-standard POSIX names (struct stat, access, close, read,
 * write, unlink, O_*, S_IF*, off_t, ino_t, ...) behind __STDC__, which MSVC
 * defines for C and for every C++ translation unit.  Ask for them explicitly
 * before any system header is parsed, and drop the deprecation noise that
 * comes with them. */
#define _CRT_DECLARE_NONSTDC_NAMES 1
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <share.h>
#include <sys/types.h>

/* Take <sys/stat.h> with _CRT_NO_TIME_T so the CRT does not emit its own
 * "struct stat" nor the stat()/fstat() inline wrappers: their st_size is a
 * 32-bit long, which silently truncates large media files.  The include guard
 * then keeps those definitions out of every later include, while the S_IF*
 * flag macros stay available.  time.h was parsed above, so time_t and struct
 * timespec are already in place. */
#define _CRT_NO_TIME_T
#include <sys/stat.h>
#undef _CRT_NO_TIME_T

#ifdef __cplusplus
extern "C" {
#endif

/* ---- scalar types the UCRT does not ship at all ---- */
#ifndef _SSIZE_T_DEFINED
typedef long long ssize_t;
#define _SSIZE_T_DEFINED
#endif
#ifndef _ZUUNED_MODE_T_DEFINED
typedef unsigned int mode_t;
#define _ZUUNED_MODE_T_DEFINED
#endif
#ifndef _ZUUNED_UID_T_DEFINED
typedef unsigned int uid_t;
#define _ZUUNED_UID_T_DEFINED
#endif
#ifndef _ZUUNED_GID_T_DEFINED
typedef unsigned int gid_t;
#define _ZUUNED_GID_T_DEFINED
#endif
#ifndef _ZUUNED_PID_T_DEFINED
typedef int pid_t;
#define _ZUUNED_PID_T_DEFINED
#endif

/* sys/types.h already typedef'd off_t (long) and ino_t (unsigned short), so a
 * second, wider typedef would be a hard error.  Redirect the spellings
 * instead: our struct stat needs a 64-bit st_size, and no later CRT or Qt
 * header mentions either name. */
typedef long long zuuned_off_t;
#ifndef off_t
#define off_t zuuned_off_t
#endif
typedef unsigned long long zuuned_ino_t;
#ifndef ino_t
#define ino_t zuuned_ino_t
#endif
/* ---- file status: 64-bit size plus the timespec fields the sources use ----
 * stat()/lstat()/fstat() are defined by wincompat.c on top of the Win32 file
 * information APIs; the CRT variants stay suppressed (see above). */
struct stat {
    dev_t           st_dev;
    ino_t           st_ino;
    mode_t          st_mode;
    unsigned short  st_nlink;
    uid_t           st_uid;
    gid_t           st_gid;
    dev_t           st_rdev;
    long long       st_size;
    time_t          st_atime;
    struct timespec st_atim;
    time_t          st_mtime;
    struct timespec st_mtim;
    time_t          st_ctime;
    struct timespec st_ctim;
    unsigned long   st_blksize;
    long long       st_blocks;
};

int stat(const char *path, struct stat *out);
int lstat(const char *path, struct stat *out);
int fstat(int fd, struct stat *out);

/* ---- mode bits ----
 * With non-standard names the CRT ships S_IFMT, S_IFDIR, S_IFCHR and S_IFREG
 * (numerically POSIX) plus S_IREAD/S_IWRITE/S_IEXEC, so only the types it
 * never had need a definition of their own. */
#ifndef S_IFMT
#define S_IFMT   0170000
#endif
#ifndef S_IFREG
#define S_IFREG  0100000
#endif
#ifndef S_IFDIR
#define S_IFDIR  0040000
#endif
#ifndef S_IFCHR
#define S_IFCHR  0020000
#endif
#ifndef S_IFIFO
#define S_IFIFO  0010000
#endif
#ifndef S_IFBLK
#define S_IFBLK  0060000
#endif
#ifndef S_IFLNK
#define S_IFLNK  0120000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0140000
#endif
#ifndef S_IRUSR
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXG (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#define S_IRWXO (S_IROTH | S_IWOTH | S_IXOTH)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#endif

/* ---- access() modes and standard descriptors ----
 * X_OK is zero here on purpose.  UCRT _access() accepts only mode 0, 2, 4 or 6
 * and reports bit 1 as an invalid parameter, which runs the invalid parameter
 * handler and __fastfail()s the process rather than setting errno.  Windows has
 * no execute bit that access() can consult anyway, and read access to a
 * directory already implies traversing it, so a POSIX "read or execute" query
 * means "read" here.  Do not OR X_OK into an access() mode on Windows. */
#ifndef F_OK
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 0
#endif
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

/* ---- open() flags the CRT lacks ----
 * O_CLOEXEC maps onto _O_NOINHERIT.  O_DIRECTORY and O_NOFOLLOW get private
 * bits that wincompat.c honours; O_NONBLOCK is honest: Windows has no
 * non-blocking regular-file reads, so it is zero and code that needs a
 * non-blocking read must use zuuned_fd_readable() (see wincompat.h). */
#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#define ZUUNED_O_DIRECTORY 0x01000000
#define ZUUNED_O_NOFOLLOW  0x02000000
#ifndef O_DIRECTORY
#define O_DIRECTORY ZUUNED_O_DIRECTORY
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW ZUUNED_O_NOFOLLOW
#endif

/* ---- how shared sources open a file on Windows ----
 * The UCRT declares open() (and access()) as _ACRTIMP, so this shim cannot
 * replace them: callers keep binding to __imp_open, which is ANSI-only and
 * stops at the legacy 260-character limit.  Sources that pass a user-supplied
 * media path therefore call openat(AT_FDCWD, path, flags) instead, which is
 * portable, reaches open_normalized_wide() here, and is exactly what glibc
 * open() itself does on Linux. */

/* lseek must follow the wide offset.  Restricted to C: the CRT declares a
 * 32-bit lseek() above, and a C++ macro could shadow unrelated members. */
#ifndef __cplusplus
#define lseek _lseeki64
#endif

/* Thread-local storage.  libzune keeps the last error message in one
 * thread-local buffer; MSVC has no C11 "__thread" keyword but does accept
 * __declspec(thread), so this redirects a keyword spelling, same as lseek
 * above.  Legal here because every consumer links these sources statically:
 * __declspec(thread) is not usable from a separately loaded DLL. */
#ifndef __thread
#define __thread __declspec(thread)
#endif
/* ---- descriptor flags ----
 * Only the flags the sources actually use are honoured; a lock request fails
 * with EOPNOTSUPP rather than silently succeeding (see flock() below for the
 * Windows story). */
#define ZUUNED_F_DUPFD          0
#define ZUUNED_F_GETFD          1
#define ZUUNED_F_SETFD          2
#define ZUUNED_F_GETFL          3
#define ZUUNED_F_SETFL          4
#define ZUUNED_F_GETLK          5
#define ZUUNED_F_SETLK          6
#define ZUUNED_F_SETLKW         7
#define ZUUNED_F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
#ifndef F_DUPFD
#define F_DUPFD ZUUNED_F_DUPFD
#endif
#ifndef F_GETFD
#define F_GETFD ZUUNED_F_GETFD
#endif
#ifndef F_SETFD
#define F_SETFD ZUUNED_F_SETFD
#endif
#ifndef F_GETFL
#define F_GETFL ZUUNED_F_GETFL
#endif
#ifndef F_SETFL
#define F_SETFL ZUUNED_F_SETFL
#endif
#ifndef F_GETLK
#define F_GETLK ZUUNED_F_GETLK
#endif
#ifndef F_SETLK
#define F_SETLK ZUUNED_F_SETLK
#endif
#ifndef F_SETLKW
#define F_SETLKW ZUUNED_F_SETLKW
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC ZUUNED_F_DUPFD_CLOEXEC
#endif
int fcntl(int fd, int request, ...);

/* ---- string helpers ---- */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcasestr(const char *haystack, const char *needle);
char *strndup(const char *s, size_t n);
#ifndef strdup
#define strdup _strdup
#endif

/* ---- time ---- */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME             0
#define CLOCK_MONOTONIC            1
#define CLOCK_PROCESS_CPUTIME_ID   2
#define CLOCK_THREAD_CPUTIME_ID    3
#endif
int clock_gettime(int clock_id, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
int usleep(unsigned int useconds);

/* ---- stdio and descriptors ---- */
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
int fchmod(int fd, mode_t mode);   /* maps onto the read-only attribute only */
int fsync(int fd);
int ftruncate(int fd, off_t length);

/* ---- pipes ----
 * pipe()/_pipe() are byte pipes with no O_NONBLOCK.  Reads block, so a caller
 * that needs a non-blocking read must consult zuuned_fd_readable(). */
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);

/* ---- directories ----
 * opendir() hands back a handle plus a directory descriptor from dirfd(), so
 * fstat(dirfd(d)) reports the directory itself, as LibraryScanner relies on. */
#ifndef _ZUUNED_DIRENT_DEFINED
#define _ZUUNED_DIRENT_DEFINED
#define ZUUNED_NAME_MAX 1024
struct dirent {
    ino_t           d_ino;
    unsigned short  d_reclen;
    unsigned char   d_type;
    char            d_name[ZUUNED_NAME_MAX];
};
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK    10
typedef struct zuuned_DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
void rewinddir(DIR *dir);
int closedir(DIR *dir);
int dirfd(DIR *dir);
#endif

/* ---- poll() ----
 * Emulated: pipes use PeekNamedPipe, everything else falls back to a short
 * sleep loop.  Enough for the capture/control pipes in SessionLog. */
#ifndef _ZUUNED_POLLFD_DEFINED
#define _ZUUNED_POLLFD_DEFINED
struct pollfd {
    int   fd;
    short events;
    short revents;
};
typedef unsigned int nfds_t;
#endif
#ifndef POLLIN
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#endif
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* ---- file mapping ---- */
#ifndef PROT_NONE
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#ifndef MS_ASYNC
#define MS_ASYNC 0x1
#define MS_SYNC  0x4
#endif
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int msync(void *addr, size_t length, int flags);

/* ---- advisory locks ----
 * Windows has no lock on a file handle obtained without one, so flock() on a
 * directory descriptor locks a companion "zuuned.lock" file in that directory;
 * the lock is released when the descriptor or the process goes away. */
#ifndef LOCK_SH
#define LOCK_SH 0x01
#define LOCK_EX 0x02
#define LOCK_NB 0x04
#define LOCK_UN 0x08
#endif
int flock(int fd, int operation);

/* ---- *at() family ----
 * No descriptor-to-path registry is kept.  A dir_fd is resolved live with
 * GetFinalPathNameByHandleW(), so the answer always matches what the handle
 * really points at; a descriptor that is not a directory fails rather than
 * guessing. */
#ifndef AT_FDCWD
#define AT_FDCWD              (-100)
#define AT_SYMLINK_NOFOLLOW   0x0100
#define AT_REMOVEDIR          0x0200
#define AT_EMPTY_PATH         0x1000
#endif
int openat(int dir_fd, const char *path, int flags, ...);
int fstatat(int dir_fd, const char *path, struct stat *out, int flags);
int unlinkat(int dir_fd, const char *path, int flags);

/* ---- positioned I/O and identity ----
 * geteuid()/getuid() report 0 because every Windows file here is already
 * protected by the per-user LOCALAPPDATA ACL; nothing may use them as a
 * security decision. */
ssize_t pread(int fd, void *buffer, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t count, off_t offset);
uid_t geteuid(void);
uid_t getuid(void);

/* ---- hard links ----
 * Used for atomic publish: write a staged file, then link it into place. */
int zuuned_link(const char *existing_path, const char *new_path);
/* Only spell the POSIX name as a macro in C.  In C++ QFile declares a static
 * QFile::link(), and the UCRT may already map "link" onto "_link".
 * #undef plus an overload keeps both spellings usable. */
#undef link
#if defined(__cplusplus)
inline int link(const char *existing_path, const char *new_path)
{
    return zuuned_link(existing_path, new_path);
}
#else
#define link(existing_path, new_path) zuuned_link(existing_path, new_path)
#endif

/* ---- stdio and file replacement over UTF-8 paths ----
 * The CRT ships fopen(), remove(), unlink() and rename() as _ACRTIMP
 * functions, so unlike stat() or openat() they cannot be replaced by a
 * declaration: a caller keeps binding to __imp_fopen, which decodes the narrow
 * path with the ANSI code page.  A function-like macro does reach them, and it
 * is safe for the same reason _CRT_NO_TIME_T is - <stdio.h> and <io.h> were
 * parsed at the top of this header, so the CRT declarations are already in
 * scope before the macro exists and only genuine call sites expand. */
FILE *zuuned_fopen(const char *path, const char *mode);
int zuuned_remove(const char *path);
int zuuned_rename(const char *from_path, const char *to_path);
/* Only the C spellings get shadowed.  In C++ remove() and rename() are also
 * members of QString, QByteArray and std, and a function-like macro expands
 * through obj.rename(a, b) too - it would rewrite the standard library and
 * the Qt headers themselves.  Portable C++ call sites use the zuuned_ names. */
#if !defined(__cplusplus)
#define fopen(path, mode) zuuned_fopen((path), (mode))
#define remove(path) zuuned_remove((path))
#define unlink(path) zuuned_remove((path))
#define rename(from, to) zuuned_rename((from), (to))
#endif

/* ---- temporary files ---- */
char *zuuned_win_tmpdir(void);
#ifndef ZUUNED_TMPDIR
#define ZUUNED_TMPDIR zuuned_win_tmpdir()
#endif
int zuuned_mkstemps(char *templat, int suffix_length);
#define mkstemps(templat, suffix_length) zuuned_mkstemps(templat, suffix_length)
int zuuned_mkstemp(char *templat);
#define mkstemp(templat) zuuned_mkstemp(templat)

/* ---- one path, spelled for the shell system() runs ----
 * system() runs cmd.exe here, and cmd.exe reads its command line with the
 * system ANSI code page, while the portable sources hand UTF-8 - the shape a
 * POSIX filename already has.  This returns the same path in the bytes that
 * shell decodes back into the same file.  Bytes that are not valid UTF-8 are
 * taken for code-page bytes already, the same UTF-8-first rule decode_path()
 * applies when opening a file, so the two never disagree about which file a
 * name means.  Returns a malloc'd string the caller frees, or NULL when the
 * name has no spelling in the active code page at all. */
char *zuuned_narrow_path_for_shell(const char *path);

/* ---- run a program without a shell ----
 * system() on Windows means cmd.exe, and cmd.exe expands %VAR% inside double
 * quotes exactly as it does outside them.  Doubling a double quote is the
 * correct escape for the argument parser of the program being run and no
 * escape at all for the shell that reads the line first, and percent has no
 * cmd.exe escape in the first place.  A file or album name that spells
 * %PATH% therefore reached the encoder as the environment value in place of
 * the name the caller chose.  No quoting scheme fixes that, so do not run the
 * shell.
 *
 * argv is NULL-terminated and argv[0] is the program name, resolved by
 * CreateProcessW search (PATH included, which is how cmd.exe found it too).
 * Each element arrives as the bytes that program own argument parser should
 * read: for a path that is the output of zuuned_narrow_path_for_shell(),
 * because those are the bytes cmd.exe used to decode back into the file.
 * Standard input, output and error are wired to NUL, which is where the
 * "2>/dev/null" and ">nul 2>&1" this replaces put the encoder chatter.
 * Returns the child exit code, 0 for success as system() reported it, or -1
 * when the program could not be started.  Blocks until it exits, as system()
 * did, and shows no console window.
 */
int zuuned_run_no_shell(const char *const argv[]);

#ifdef __cplusplus
}
#endif

#endif /* ZUUNED_WIN_POSIX_SHIM_H */
