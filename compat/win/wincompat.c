/* Windows implementation of the POSIX surface declared in posix_shim.h.
 *
 * Two rules govern this file.  A function either honours the POSIX contract it
 * names or it fails with a real errno - it never reports success for something
 * Windows did not do.  And every path crosses into Win32 through
 * zuuned_full_path(), so a narrow path is interpreted the same way everywhere.
 *
 * Narrow-path decoding is strict UTF-8 first, then the ANSI code page.  The
 * shared sources hand us QByteArray data from QFile::encodeName()/toUtf8() and
 * from wire-derived names, all UTF-8; the ANSI fallback keeps paths that come
 * from legacy C runtime callers working.  A byte sequence that is valid UTF-8
 * but meant as ANSI resolves to a path that does not exist, so the worst case
 * is a truthful ENOENT rather than an unrelated file.
 */
#include "wincompat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <winbase.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>
#include <share.h>
#include <errno.h>
#include <stdlib.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

#define ZUUNED_FILETIME_EPOCH_DIFF 116444736000000000ULL

static int errno_from_last_error(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_NET_NAME:
    case ERROR_NO_MORE_FILES:          return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_CURRENT_DIRECTORY:      return EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:         return EAGAIN;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:         return EEXIST;
    case ERROR_NOT_SAME_DEVICE:        return EXDEV;
    case ERROR_DIR_NOT_EMPTY:          return ENOTEMPTY;
    case ERROR_DIRECTORY:
    case ERROR_BAD_PATHNAME:             return ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_NOT_ENOUGH_MEMORY:      return ENOSPC;
    case ERROR_INVALID_HANDLE:
    case ERROR_INVALID_PARAMETER:      return EINVAL;
    case ERROR_HANDLE_EOF:             return 0;
    case ERROR_NOT_SUPPORTED:          return EOPNOTSUPP;
    case ERROR_BROKEN_PIPE:
    case ERROR_PIPE_NOT_CONNECTED:     return EPIPE;
    default:                           return EIO;
    }
}

static void set_last_error(void)
{
    const int code = errno_from_last_error();
    errno = code ? code : EIO;
}

static char *wide_to_narrow(const wchar_t *wide)
{
    const int needed = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) { errno = EINVAL; return NULL; }
    char *out = (char *)malloc((size_t)needed);
    if (!out) { errno = ENOMEM; return NULL; }
    if (WideCharToMultiByte(CP_ACP, 0, wide, -1, out, needed, NULL, NULL) <= 0) {
        free(out);
        errno = EINVAL;
        return NULL;
    }
    return out;
}

static wchar_t *decode_path(const char *path)
{
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (needed > 0) {
        wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
        if (!wide) { errno = ENOMEM; return NULL; }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, needed) > 0)
            return wide;
        free(wide);
    }
    needed = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (needed <= 0) { errno = EINVAL; return NULL; }
    wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (!wide) { errno = ENOMEM; return NULL; }
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, needed) <= 0) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }
    return wide;
}

static int is_rooted(const wchar_t *path)
{
    return path[0] && path[1] == L':' || path[0] == L'\\' && path[1] == L'\\';
}

/* Absolute, backslash-normalised, dot-free, no trailing separator - and with
 * the extended-length prefix once the plain form cannot carry it.  Backslashes
 * are stripped because "\\?\" disables normalisation, and CreateFileW then
 * treats a trailing separator as part of the name. */
static wchar_t *zuuned_full_path(const char *path)
{
    if (!path || !*path) { errno = EINVAL; return NULL; }
    wchar_t *raw = decode_path(path);
    if (!raw) return NULL;

    wchar_t *full = NULL;
    for (int pass = 0; pass < 2; ++pass) {
        const DWORD need = GetFullPathNameW(raw, 0, NULL, NULL);
        if (need == 0) { free(raw); set_last_error(); return NULL; }
        free(full);
        full = (wchar_t *)malloc(((size_t)need + 8) * sizeof(wchar_t));
        if (!full) { free(raw); errno = ENOMEM; return NULL; }
        if (GetFullPathNameW(raw, need, full, NULL) != 0) break;
        free(full);
        full = NULL;
        set_last_error();
    }
    free(raw);
    if (!full) return NULL;

    size_t length = wcslen(full);
    while (length > 3 && full[length - 1] == L'\\') full[--length] = L'\0';

    if (length < 248 || !is_rooted(full)) return full;

    const wchar_t *unc = wcsncmp(full, L"\\\\", 2) == 0 ? full + 2 : NULL;
    const size_t body = wcslen(unc ? unc : full);
    const wchar_t *prefix = unc ? L"\\\\?\\" : L"\\\\?\\";
    const size_t prefix_length = 4;
    const size_t extra = unc ? 4 : 0;                    /* "UNC\" minus "\\" */
    wchar_t *wide = (wchar_t *)malloc((prefix_length + extra + body + 2) * sizeof(wchar_t));
    if (!wide) { free(full); errno = ENOMEM; return NULL; }
    memcpy(wide, prefix, prefix_length * sizeof(wchar_t));
    if (unc) {
        memcpy(wide + prefix_length, L"UNC", 3 * sizeof(wchar_t));
        wide[prefix_length + 3] = L'\\';
    }
    memcpy(wide + prefix_length + extra, unc ? unc : full, (body + 1) * sizeof(wchar_t));
    free(full);
    return wide;
}

static void filetime_to_parts(const FILETIME *time, long long *seconds, long *nanoseconds)
{
    const unsigned long long raw = ((unsigned long long)time->dwHighDateTime << 32)
                                 | time->dwLowDateTime;
    if (raw < ZUUNED_FILETIME_EPOCH_DIFF) {
        *seconds = 0;
        *nanoseconds = 0;
        return;
    }
    const unsigned long long since = raw - ZUUNED_FILETIME_EPOCH_DIFF;
    *seconds = (long long)(since / 10000000ULL);
    *nanoseconds = (long)((since % 10000000ULL) * 100ULL);
}

/* ---------------------------------------------------------------------------
 * Chunk B: file status, descriptor-backed open, directory streams, at() family.
 * ------------------------------------------------------------------------- */
#include <wchar.h>

/* Lossless UTF-8 rendering of a wide path.  The at() family has to join a
 * directory resolved from a descriptor with a narrow relative path, and the
 * only way back into zuuned_full_path() without losing characters is to
 * re-encode as UTF-8, which decode_path() accepts first and verbatim. */
static char *wide_to_utf8(const wchar_t *wide)
{
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) { errno = EINVAL; return NULL; }
    char *out = (char *)malloc((size_t)needed);
    if (!out) { errno = ENOMEM; return NULL; }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, needed, NULL, NULL) <= 0) {
        free(out);
        errno = EINVAL;
        return NULL;
    }
    return out;
}

/* Resolves a descriptor to the handle behind it.
 *
 * Every descriptor this repository passes to fstat(), flock() or fcntl() is a CRT
 * descriptor.  The ones this file hands out come from _open_osfhandle(), _pipe()
 * and _dup(); the ones it does not hand out come from the CRT too, and that group
 * is what used to break.  QFile::handle() is documented as a native handle and
 * this file believed the documentation, but Qt opens a QFile on Windows through
 * _wopen(), so the number it returns is a CRT descriptor - measured on Qt 6.11,
 * where the first file a process opens reports 3.  The watermark that used to
 * stand in front of the descriptor table only ever counted descriptors this file
 * had handed out, so QFile's number sat above it, was read as a handle,
 * GetFileType refused it, and every audiobook import died on Windows at
 * "Audio source is not a regular file".
 *
 * The table is therefore consulted first, with no plausibility test, and the
 * number is read as a raw NT handle only when the CRT says it is not a descriptor
 * at all.  Asking is safe because fd_handle() puts the invalid-parameter guard
 * defined at the end of this file in place before it asks: the descriptor table
 * reports an out-of-range number through that machinery, and returning from it
 * yields the -1 this branch wants.  What remains ambiguous is a raw handle whose
 * value also happens to be a live descriptor; nothing in this tree creates one,
 * and the fallback still resolves the handles that are not. */
static void ensure_invalid_parameter_guard(void);

static HANDLE fd_handle(int fd)
{
    if (fd < 0) return INVALID_HANDLE_VALUE;
    ensure_invalid_parameter_guard();
    const intptr_t from_table = _get_osfhandle(fd);
    if (from_table != -1) return (HANDLE)from_table;
    if (fd == 0) return INVALID_HANDLE_VALUE;
    return (HANDLE)(uintptr_t)(unsigned int)fd;
}

static int handle_is_directory(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) return 0;
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK    0xA000000CEL
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003EL
#endif

static int handle_is_symlink(HANDLE handle)
{
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
        return 0;
    return tag.ReparseTag == IO_REPARSE_TAG_SYMLINK
        || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
}

/* Windows carries no uid/gid and no permission bits beyond the read-only
 * attribute, so identity is always root and the mode is derived from the two
 * facts the file system really knows: is it a directory, and is it read-only. */
static int fill_stat_from_handle(HANDLE handle, struct stat *out)
{
    BY_HANDLE_FILE_INFORMATION info;
    memset(out, 0, sizeof(*out));
    if (!GetFileInformationByHandle(handle, &info)) { set_last_error(); return -1; }

    out->st_dev = (dev_t)info.dwVolumeSerialNumber;
    out->st_ino = ((zuuned_ino_t)info.nFileIndexHigh << 32) | (zuuned_ino_t)info.nFileIndexLow;
    out->st_nlink = (unsigned short)(info.nNumberOfLinks ? info.nNumberOfLinks : 1);
    out->st_blksize = 65536;
    out->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 ? (mode_t)(S_IFDIR | 0755) : (mode_t)(S_IFREG | 0644);
    if (handle_is_symlink(handle)) out->st_mode = (mode_t)(S_IFLNK | 0777);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        out->st_mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);

    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        out->st_size = ((long long)info.nFileSizeHigh << 32) | (long long)info.nFileSizeLow;
        out->st_blocks = (out->st_size + 511) / 512;
    }

    long long seconds = 0;
    long nanoseconds = 0;
    filetime_to_parts(&info.ftLastAccessTime, &seconds, &nanoseconds);
    out->st_atime = (time_t)seconds;
    out->st_atim.tv_sec = (time_t)seconds;
    out->st_atim.tv_nsec = nanoseconds;
    filetime_to_parts(&info.ftLastWriteTime, &seconds, &nanoseconds);
    out->st_mtime = (time_t)seconds;
    out->st_mtim.tv_sec = (time_t)seconds;
    out->st_mtim.tv_nsec = nanoseconds;
    filetime_to_parts(&info.ftCreationTime, &seconds, &nanoseconds);
    out->st_ctime = (time_t)seconds;
    out->st_ctim.tv_sec = (time_t)seconds;
    out->st_ctim.tv_nsec = nanoseconds;
    return 0;
}

/* BACKUP_SEMANTICS is what makes a directory handle legal; without it
 * CreateFileW refuses directories outright. */
static HANDLE open_information_handle(const wchar_t *wide, int follow)
{
    return CreateFileW(wide, FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                       NULL);
}

static int stat_wide(const wchar_t *wide, struct stat *out, int follow)
{
    const HANDLE handle = open_information_handle(wide, follow);
    if (handle == INVALID_HANDLE_VALUE) { set_last_error(); return -1; }
    const int result = fill_stat_from_handle(handle, out);
    CloseHandle(handle);
    return result;
}

int stat(const char *path, struct stat *out)
{
    if (!out) { errno = EINVAL; return -1; }
    wchar_t *wide = zuuned_full_path(path);
    if (!wide) return -1;
    const int result = stat_wide(wide, out, 1);
    free(wide);
    return result;
}

int lstat(const char *path, struct stat *out)
{
    if (!out) { errno = EINVAL; return -1; }
    wchar_t *wide = zuuned_full_path(path);
    if (!wide) return -1;
    const int result = stat_wide(wide, out, 0);
    free(wide);
    return result;
}

int fstat(int fd, struct stat *out)
{
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    switch (GetFileType(handle)) {
    case FILE_TYPE_DISK:
        return fill_stat_from_handle(handle, out);
    case FILE_TYPE_PIPE:
        out->st_mode = (mode_t)(S_IFIFO | 0600);
        out->st_blksize = 65536;
        return 0;
    case FILE_TYPE_CHAR:
        out->st_mode = (mode_t)(S_IFCHR | 0600);
        return 0;
    default:
        errno = EBADF;
        return -1;
    }
}

/* Opens a directory for traversal only.  The descriptor is what dirfd(),
 * flock() and the at() family resolve against, so it has to outlive the call
 * and be a real CRT descriptor - hence _open_osfhandle rather than a handle
 * kept in a private table. */
static int open_directory_wide(const wchar_t *wide)
{
    const HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      NULL, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                                      NULL);
    if (handle == INVALID_HANDLE_VALUE) { set_last_error(); return -1; }
    if (!handle_is_directory(handle)) { CloseHandle(handle); errno = ENOTDIR; return -1; }
    const int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd == -1) { CloseHandle(handle); set_last_error(); return -1; }
    return fd;
}

int zuuned_open_directory(const char *path)
{
    wchar_t *wide = zuuned_full_path(path);
    if (!wide) return -1;
    const int fd = open_directory_wide(wide);
    free(wide);
    return fd;
}

/* The CRT open() cannot express O_DIRECTORY or O_NOFOLLOW, so the at() family
 * carries its own opener.  O_NONBLOCK is deliberately absent: Windows has no
 * non-blocking regular-file read (see zuuned_fd_readable). */
static int open_normalized_wide(const wchar_t *wide, int flags)
{
    if (flags & ZUUNED_O_DIRECTORY) return open_directory_wide(wide);

    DWORD access = 0;
    if (flags & O_APPEND) access |= FILE_APPEND_DATA;
    else if (flags & (O_WRONLY | O_RDWR)) access |= GENERIC_WRITE;
    if ((flags & O_ACCMODE) != O_WRONLY) access |= GENERIC_READ;

    DWORD creation;
    if (flags & O_CREAT)
        creation = (flags & O_EXCL) ? CREATE_NEW : ((flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS);
    else
        creation = OPEN_EXISTING;

    const HANDLE handle = CreateFileW(wide, access,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      NULL, creation,
                                      FILE_ATTRIBUTE_NORMAL
                                          | ((flags & ZUUNED_O_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
                                      NULL);
    if (handle == INVALID_HANDLE_VALUE) { set_last_error(); return -1; }

    if ((flags & O_TRUNC) && GetFileType(handle) == FILE_TYPE_DISK) {
        const LARGE_INTEGER zero = { { 0, 0 } };
        if (!SetFilePointerEx(handle, zero, NULL, FILE_BEGIN) || !SetEndOfFile(handle)) {
            const DWORD last = GetLastError();
            CloseHandle(handle);
            SetLastError(last);
            set_last_error();
            return -1;
        }
    }
    if (flags & O_CLOEXEC) SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

    const int fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_NOINHERIT);
    if (fd == -1) { CloseHandle(handle); set_last_error(); return -1; }
    return fd;
}

/* Hard link, used to publish a staged file atomically.  Windows refuses across
 * volumes, which surfaces as EXDEV exactly as on POSIX, so the callers fall
 * back to copy-and-rename on their own. */
int zuuned_link(const char *existing_path, const char *new_path)
{
    wchar_t *existing = zuuned_full_path(existing_path);
    if (!existing) return -1;
    wchar_t *fresh = zuuned_full_path(new_path);
    if (!fresh) { free(existing); return -1; }
    const BOOL ok = CreateHardLinkW(fresh, existing, NULL);
    free(existing);
    free(fresh);
    if (!ok) { set_last_error(); return -1; }
    return 0;
}

/* ---- at() family ---------------------------------------------------------
 * No descriptor-to-path registry: the directory handle answers the question
 * itself through GetFinalPathNameByHandleW, so a descriptor opened by anyone
 * (including _open_osfhandle of a handle we never saw) works. */
static wchar_t *fd_to_dir_path(int dir_fd)
{
    const HANDLE handle = fd_handle(dir_fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return NULL; }
    if (GetFileType(handle) != FILE_TYPE_DISK || !handle_is_directory(handle)) {
        errno = ENOTDIR;
        return NULL;
    }

    DWORD capacity = 512;
    wchar_t *buffer = NULL;
    for (int pass = 0; pass < 8; ++pass) {
        wchar_t *grown = (wchar_t *)realloc(buffer, (size_t)capacity * sizeof(wchar_t));
        if (!grown) { free(buffer); errno = ENOMEM; return NULL; }
        buffer = grown;
        const DWORD written = GetFinalPathNameByHandleW(handle, buffer, capacity, FILE_NAME_OPENED);
        if (written > 0 && written < capacity) break;
        if (written == 0) { free(buffer); set_last_error(); return NULL; }
        if (pass == 7) { free(buffer); errno = ENAMETOOLONG; return NULL; }
        capacity = written + 16;
    }

    /* Drop the extended-length prefix so the result normalises like any other
     * path; UNC shares get their leading backslash pair back. */
    wchar_t *body = buffer;
    if (wcsncmp(buffer, L"\\\\?\\", 4) == 0) {
        if (wcsncmp(buffer + 4, L"UNC\\", 4) == 0) {
            memmove(buffer + 2, buffer + 6, (wcslen(buffer + 6) + 1) * sizeof(wchar_t));
        } else {
            body = buffer + 4;
        }
    }

    char *narrow = wide_to_utf8(body);
    free(buffer);
    if (!narrow) return NULL;
    wchar_t *full = zuuned_full_path(narrow);
    free(narrow);
    return full;
}

static int path_is_absolute(const char *path)
{
    const char first = path[0];
    const char second = path[0] ? path[1] : '\0';
    return first == '/' || first == '\\'
        || (second == ':' && ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')));
}

static wchar_t *resolve_at_path(int dir_fd, const char *path)
{
    if (!path) { errno = ENOENT; return NULL; }
    if (!*path) { errno = ENOENT; return NULL; }
    if (path_is_absolute(path) || dir_fd == AT_FDCWD) return zuuned_full_path(path);

    wchar_t *base = fd_to_dir_path(dir_fd);
    if (!base) return NULL;
    char *base_narrow = wide_to_utf8(base);
    free(base);
    if (!base_narrow) return NULL;

    const size_t base_length = strlen(base_narrow);
    const size_t tail_length = strlen(path);
    char *joined = (char *)malloc(base_length + tail_length + 2);
    if (!joined) { free(base_narrow); errno = ENOMEM; return NULL; }
    memcpy(joined, base_narrow, base_length);
    joined[base_length] = '/';
    memcpy(joined + base_length + 1, path, tail_length + 1);
    free(base_narrow);

    wchar_t *full = zuuned_full_path(joined);
    free(joined);
    return full;
}

int openat(int dir_fd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, unsigned int);
        va_end(arguments);
    }
    (void)mode;   /* Windows derives permissions from the parent directory ACL */
    wchar_t *wide = resolve_at_path(dir_fd, path);
    if (!wide) return -1;
    const int fd = open_normalized_wide(wide, flags);
    free(wide);
    return fd;
}

int fstatat(int dir_fd, const char *path, struct stat *out, int flags)
{
    if (!out) { errno = EINVAL; return -1; }
    wchar_t *wide = resolve_at_path(dir_fd, path);
    if (!wide) return -1;
    const int result = stat_wide(wide, out, (flags & AT_SYMLINK_NOFOLLOW) ? 0 : 1);
    free(wide);
    return result;
}

/* POSIX unlink does not care about a read-only file as long as the directory
 * is writable, and that is exactly what a temporary-file cleanup hits after a
 * crashed run left a read-only staging file behind, so the read-only attribute
 * is cleared once before giving up. */
static int delete_wide(const wchar_t *wide, int remove_dir)
{
    if (remove_dir) {
        if (RemoveDirectoryW(wide)) return 0;
        set_last_error();
        return -1;
    }
    if (DeleteFileW(wide)) return 0;
    const DWORD first = GetLastError();
    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        errno = EISDIR;
        return -1;
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        if (SetFileAttributesW(wide, attributes & ~FILE_ATTRIBUTE_READONLY) && DeleteFileW(wide))
            return 0;
    }
    SetLastError(first);
    set_last_error();
    return -1;
}

int unlinkat(int dir_fd, const char *path, int flags)
{
    wchar_t *wide = resolve_at_path(dir_fd, path);
    if (!wide) return -1;
    const int result = delete_wide(wide, (flags & AT_REMOVEDIR) != 0);
    free(wide);
    return result;
}

/* ---- directory streams ---------------------------------------------------
 * FindFirstFileW already yields "." and "..", which POSIX readdir() yields too,
 * so the stream stays truthful by passing them through. */
struct zuuned_DIR {
    WIN32_FIND_DATAW find;
    HANDLE search;
    int pending;
    int finished;
    int fd;
    wchar_t *path;
    struct dirent entry;
};


/* Case-insensitive FNV-1a over a wide name; see entry_from_find(). */
static zuuned_ino_t name_hash(const wchar_t *name)
{
    zuuned_ino_t hash = 1469598103934665603ULL;
    for (const wchar_t *p = name; *p; ++p) {
        wchar_t c = *p;
        if (c >= L'A' && c <= L'Z') c += (wchar_t)(L'a' - L'A');
        hash = (hash ^ (zuuned_ino_t)c) * 1099511628211ULL;
    }
    return hash;
}
static void entry_from_find(struct zuuned_DIR *dir)
{
    memset(&dir->entry, 0, sizeof(dir->entry));
    /* WIN32_FIND_DATA carries no file index, and no caller in this tree reads
     * d_ino, so it is a documented synthetic: a case-insensitive hash of the
     * entry name, stable for the life of the scan. */
    dir->entry.d_ino = name_hash(dir->find.cFileName);
    if (dir->find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        dir->entry.d_type = DT_LNK;
    else if (dir->find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        dir->entry.d_type = DT_DIR;
    else
        dir->entry.d_type = DT_REG;

    /* Names leave as UTF-8, the byte string the rest of this tree uses; the ANSI
     * code page would mangle non-ASCII media names and cannot round-trip. */
    char *narrow = wide_to_utf8(dir->find.cFileName);
    if (narrow) {
        const size_t length = strlen(narrow);
        const size_t room = sizeof(dir->entry.d_name) - 1;
        memcpy(dir->entry.d_name, narrow, length < room ? length : room);
        dir->entry.d_name[length < room ? length : room] = '\0';
        dir->entry.d_reclen = (unsigned short)(sizeof(struct dirent)
            - sizeof(dir->entry.d_name) + strlen(dir->entry.d_name) + 1);
        free(narrow);
    }
}

DIR *opendir(const char *path)
{
    wchar_t *wide = zuuned_full_path(path);
    if (!wide) return NULL;

    const size_t length = wcslen(wide);
    wchar_t *pattern = (wchar_t *)malloc((length + 3) * sizeof(wchar_t));
    if (!pattern) { free(wide); errno = ENOMEM; return NULL; }
    memcpy(pattern, wide, length * sizeof(wchar_t));
    pattern[length] = L'\\';
    pattern[length + 1] = L'*';
    pattern[length + 2] = L'\0';

    WIN32_FIND_DATAW find;
    const HANDLE search = FindFirstFileW(pattern, &find);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) { set_last_error(); free(wide); return NULL; }

    struct zuuned_DIR *dir = (struct zuuned_DIR *)calloc(1, sizeof(*dir));
    if (!dir) { FindClose(search); free(wide); errno = ENOMEM; return NULL; }
    dir->search = search;
    dir->find = find;
    dir->pending = 1;
    dir->finished = 0;
    dir->fd = -1;
    dir->path = wide;
    return dir;
}

struct dirent *readdir(DIR *dir)
{
    if (!dir) { errno = EBADF; return NULL; }
    if (dir->pending) {
        dir->pending = 0;
        entry_from_find(dir);
        return &dir->entry;
    }
    if (dir->finished) return NULL;
    if (!FindNextFileW(dir->search, &dir->find)) {
        dir->finished = 1;
        return NULL;                        /* end of stream, not an error */
    }
    entry_from_find(dir);
    return &dir->entry;
}

void rewinddir(DIR *dir)
{
    if (!dir) { errno = EBADF; return; }
    FindClose(dir->search);
    dir->search = INVALID_HANDLE_VALUE;
    dir->pending = 0;
    dir->finished = 1;

    const size_t length = wcslen(dir->path);
    wchar_t *pattern = (wchar_t *)malloc((length + 3) * sizeof(wchar_t));
    if (!pattern) { errno = ENOMEM; return; }
    memcpy(pattern, dir->path, length * sizeof(wchar_t));
    pattern[length] = L'\\';
    pattern[length + 1] = L'*';
    pattern[length + 2] = L'\0';
    const HANDLE search = FindFirstFileW(pattern, &dir->find);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) { set_last_error(); return; }
    dir->search = search;
    dir->pending = 1;
    dir->finished = 0;
}

int closedir(DIR *dir)
{
    if (!dir) { errno = EBADF; return -1; }
    if (dir->search != INVALID_HANDLE_VALUE) FindClose(dir->search);
    if (dir->fd != -1) _close(dir->fd);
    free(dir->path);
    free(dir);
    return 0;
}

int dirfd(DIR *dir)
{
    if (!dir) { errno = EBADF; return -1; }
    if (dir->fd == -1) dir->fd = open_directory_wide(dir->path);
    return dir->fd;
}
/* ---------------------------------------------------------------------------
 * Chunk C: descriptors, pipes, poll, locks, mappings, positioned I/O, strings.
 * ------------------------------------------------------------------------- */

/* Deliberately no open() and no access() here.  The UCRT declares both as
 * _ACRTIMP (dllimport), so a replacement definition is a C2491 and callers
 * would keep resolving to __imp_open anyway.  Shared sources that pass a user
 * media path open through openat(AT_FDCWD, ...) instead, which reaches
 * open_normalized_wide() on every platform.  The CRT access() that callers do
 * get is ANSI-only, so no shared source may hand it a user-supplied path:
 * LibraryScanner::accessibleDirectory() lets opendir() prove read access
 * instead, and SupportReport's readability probe is Linux-only code. */

/* ---- pipes and readiness -------------------------------------------------
 * Windows byte pipes have no non-blocking mode, so the shim documents reads as
 * blocking and callers that must not block consult zuuned_fd_readable(). */
int pipe2(int fds[2], int flags)
{
    if (!fds) { errno = EINVAL; return -1; }
    int descriptors[2] = { -1, -1 };
    if (_pipe(descriptors, 65536, _O_BINARY | _O_NOINHERIT) != 0) {
        if (errno == 0) errno = EMFILE;
        return -1;
    }
    for (int index = 0; index < 2; ++index) {
        const HANDLE handle = fd_handle(descriptors[index]);
        if (handle != INVALID_HANDLE_VALUE)
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
    (void)flags;   /* O_NONBLOCK cannot be honoured on a Windows byte pipe */
    fds[0] = descriptors[0];
    fds[1] = descriptors[1];
    return 0;
}

int pipe(int fds[2])
{
    return pipe2(fds, 0);
}

static int pipe_available(HANDLE handle, int *readable, int *closed)
{
    DWORD available = 0;
    DWORD queued = 0;
    *readable = 0;
    *closed = 0;
    if (PeekNamedPipe(handle, NULL, 0, NULL, &available, &queued)) {
        *readable = available > 0 ? 1 : 0;
        return 0;
    }
    if (GetLastError() == ERROR_BROKEN_PIPE) {
        *closed = 1;                         /* writer gone: end of file */
        *readable = 1;
        return 0;
    }
    set_last_error();
    return -1;
}

int zuuned_fd_readable(int fd)
{
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    switch (GetFileType(handle)) {
    case FILE_TYPE_DISK:
    case FILE_TYPE_CHAR:
        return 1;                            /* always "ready" in POSIX terms */
    case FILE_TYPE_PIPE: {
        int readable = 0;
        int closed = 0;
        if (pipe_available(handle, &readable, &closed) != 0) return 0;
        return readable;
    }
    default:
        return 0;
    }
}

/* poll() by polling.  A regular file is always ready, which is what POSIX
 * reports; a pipe asks PeekNamedPipe, but only when the caller wants POLLIN.
 * POLLOUT on a pipe reports ready: Windows exposes no write-queue space, the
 * control messages SessionLog writes are far below the 64K buffer, and a write
 * that genuinely cannot proceed fails with EPIPE anyway.  The deadline is a
 * coarse 5 ms step rather than a kernel wait. */
static int scan_poll_fds(struct pollfd *fds, nfds_t nfds)
{
    int ready = 0;
    for (nfds_t index = 0; index < nfds; ++index) {
        struct pollfd *slot = &fds[index];
        slot->revents = 0;
        const HANDLE handle = fd_handle(slot->fd);
        const DWORD type = handle == INVALID_HANDLE_VALUE ? FILE_TYPE_UNKNOWN : GetFileType(handle);
        if (handle == INVALID_HANDLE_VALUE || type == FILE_TYPE_UNKNOWN) {
            slot->revents = POLLNVAL;
            ++ready;
            continue;
        }
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_CHAR) {
            slot->revents = (short)(slot->events & (POLLIN | POLLOUT));
            if (slot->revents) ++ready;
            continue;
        }
        int readable = 0;
        int closed = 0;
        if (slot->events & POLLIN) {
            /* PeekNamedPipe answers on a read end only, so POLLOUT never queries:
             * a write end has no queue space to report, and a write that really
             * cannot proceed fails with EPIPE from write(). */
            if (pipe_available(handle, &readable, &closed) != 0) {
                slot->revents = POLLERR;
                ++ready;
                continue;
            }
        }
        if (readable) slot->revents |= (short)(slot->events & POLLIN);
        if (closed) slot->revents |= (short)(POLLHUP | (slot->events & POLLIN));
        if (slot->events & POLLOUT) slot->revents |= POLLOUT;
        if (slot->revents) ++ready;
    }
    return ready;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (!fds && nfds > 0) { errno = EFAULT; return -1; }
    const unsigned long long deadline =
        timeout < 0 ? 0ULL : GetTickCount64() + (unsigned long long)timeout;
    for (;;) {
        const int ready = scan_poll_fds(fds, nfds);
        if (ready > 0) return ready;
        if (timeout == 0) return 0;
        if (timeout > 0 && GetTickCount64() >= deadline) return 0;
        Sleep(5);
    }
}

/* ---- fcntl ----
 * Duplicated descriptors are real (a CRT dup), close-on-exec maps onto the
 * handle inherit flag, and O_NONBLOCK is the one flag with no Windows meaning
 * - so F_SETFL accepts it and nothing else.  Byte-range locks report
 * EOPNOTSUPP rather than pretending, because flock() is the contract the
 * sources actually rely on. */
int fcntl(int fd, int request, ...)
{
    long argument = 0;
    if (request == F_DUPFD || request == F_DUPFD_CLOEXEC
        || request == F_SETFD || request == F_SETFL) {
        va_list arguments;
        va_start(arguments, request);
        argument = (long)va_arg(arguments, int);
        va_end(arguments);
    }
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) == FILE_TYPE_UNKNOWN) {
        errno = EBADF;
        return -1;
    }

    switch (request) {
    case F_DUPFD: {
        const int copy = _dup(fd);
        if (copy == -1) { set_last_error(); return -1; }
        return copy;
    }
    case F_DUPFD_CLOEXEC: {
        const int copy = _dup(fd);
        if (copy == -1) { set_last_error(); return -1; }
        const HANDLE copy_handle = fd_handle(copy);
        if (copy_handle != INVALID_HANDLE_VALUE)
            SetHandleInformation(copy_handle, HANDLE_FLAG_INHERIT, 0);
        return copy;
    }
    case F_GETFD: {
        DWORD flags = 0;
        if (!GetHandleInformation(handle, &flags)) { set_last_error(); return -1; }
        return (flags & HANDLE_FLAG_INHERIT) ? 0 : FD_CLOEXEC;
    }
    case F_SETFD: {
        const DWORD flags = (argument & FD_CLOEXEC) ? 0 : HANDLE_FLAG_INHERIT;
        if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags)) {
            set_last_error();
            return -1;
        }
        return 0;
    }
    case F_GETFL:
        return O_RDONLY;   /* the handle access mask is not queryable portably */
    case F_SETFL:
        if ((int)argument != 0) { errno = EOPNOTSUPP; return -1; }
        return 0;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        errno = EOPNOTSUPP;
        return -1;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ---- advisory locks ----
 * A lock needs a handle opened for writing, which a traversal-only directory
 * descriptor is not, so for directories the lock lives in a companion file
 * inside the directory.  The companion stays open until the descriptor number
 * is recycled or the process exits; a lock held slightly longer than POSIX
 * would hold it is the conservative direction for a single-instance guard,
 * which is the only thing the sources lock. */
#define ZUUNED_LOCK_NAME L"zuuned.lock"
#define ZUUNED_TRACKED_LOCKS 16

struct zuuned_tracked_lock {
    int fd;
    HANDLE directory;
    HANDLE companion;
};

static struct zuuned_tracked_lock tracked_locks[ZUUNED_TRACKED_LOCKS];
static RTL_SRWLOCK lock_table_guard = SRWLOCK_INIT;

static int lock_range(HANDLE handle, int operation)
{
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    if (operation & LOCK_UN) {
        UnlockFileEx(handle, 0, 0xFFFFFFFF, 0xFFFFFFFF, &overlapped);
        return 0;                            /* unlocking an unlocked file is 0 */
    }
    DWORD flags = 0;
    if (operation & LOCK_EX) flags |= LOCKFILE_EXCLUSIVE_LOCK;
    if (operation & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (LockFileEx(handle, flags, 0, 0xFFFFFFFF, 0xFFFFFFFF, &overlapped)) return 0;
    set_last_error();
    return -1;
}

/* Only the companion file is ours to close.  The directory handle belongs to
 * the descriptor that asked for the lock, and closing it here would pull the
 * rug out from under every later fstat() and at() call on that descriptor. */
static void drop_lock_entry(int index)
{
    if (tracked_locks[index].companion) CloseHandle(tracked_locks[index].companion);
    memset(&tracked_locks[index], 0, sizeof(tracked_locks[index]));
}

static int find_lock_entry(int fd, HANDLE directory)
{
    for (int index = 0; index < ZUUNED_TRACKED_LOCKS; ++index) {
        if (tracked_locks[index].fd == 0 && tracked_locks[index].companion == NULL) continue;
        if (tracked_locks[index].fd != fd) continue;
        if (tracked_locks[index].directory == directory) return index;
        /* The descriptor was closed and the number reused: whatever was locked
         * under the old descriptor is already gone with its handle. */
        drop_lock_entry(index);
        return -2;                           /* caller retries as a fresh entry */
    }
    return -1;
}

static int acquire_companion(int fd, HANDLE directory, HANDLE *companion)
{
    *companion = NULL;
    int index = find_lock_entry(fd, directory);
    if (index >= 0) { *companion = tracked_locks[index].companion; return 0; }

    wchar_t *raw = NULL;
    DWORD capacity = 512;
    for (int pass = 0; pass < 8 && !raw; ++pass) {
        wchar_t *buffer = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (!buffer) { errno = ENOMEM; return -1; }
        const DWORD written = GetFinalPathNameByHandleW(directory, buffer, capacity, FILE_NAME_OPENED);
        if (written > 0 && written < capacity) { raw = buffer; break; }
        free(buffer);
        if (written == 0) { set_last_error(); return -1; }
        capacity = written + 16;
    }
    if (!raw) { errno = ENAMETOOLONG; return -1; }

    wchar_t *body = raw;
    if (wcsncmp(raw, L"\\\\?\\", 4) == 0) {
        if (wcsncmp(raw + 4, L"UNC\\", 4) == 0)
            memmove(raw + 2, raw + 6, (wcslen(raw + 6) + 1) * sizeof(wchar_t));
        else
            body = raw + 4;
    }

    const size_t length = wcslen(body);
    const size_t name_length = wcslen(ZUUNED_LOCK_NAME);
    wchar_t *path = (wchar_t *)malloc((length + name_length + 2) * sizeof(wchar_t));
    if (!path) { free(raw); errno = ENOMEM; return -1; }
    memcpy(path, body, length * sizeof(wchar_t));
    path[length] = L'\\';
    memcpy(path + length + 1, ZUUNED_LOCK_NAME, (name_length + 1) * sizeof(wchar_t));
    free(raw);

    const HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    free(path);
    if (handle == INVALID_HANDLE_VALUE) { set_last_error(); return -1; }

    for (int slot = 0; slot < ZUUNED_TRACKED_LOCKS; ++slot) {
        if (tracked_locks[slot].companion) continue;
        tracked_locks[slot].fd = fd;
        tracked_locks[slot].directory = directory;
        tracked_locks[slot].companion = handle;
        *companion = handle;
        return 0;
    }
    CloseHandle(handle);
    errno = ENOSPC;
    return -1;
}

int flock(int fd, int operation)
{
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) == FILE_TYPE_UNKNOWN) {
        errno = EBADF;
        return -1;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) { errno = EBADF; return -1; }

    AcquireSRWLockExclusive(&lock_table_guard);
    int result;
    if (!(operation & LOCK_UN) || !handle_is_directory(handle)) {
        if (!handle_is_directory(handle)) {
            result = lock_range(handle, operation);
        } else {
            HANDLE companion = NULL;
            result = acquire_companion(fd, handle, &companion) == 0 && companion
                   ? lock_range(companion, operation) : -1;
        }
    } else {
        const int index = find_lock_entry(fd, handle);
        if (index >= 0) {
            lock_range(tracked_locks[index].companion, LOCK_UN);
            drop_lock_entry(index);
        }
        result = 0;
    }
    ReleaseSRWLockExclusive(&lock_table_guard);
    return result;
}

/* ---- file mappings -------------------------------------------------------
 * Read-only in this tree, and the mapping is created over the whole file so an
 * offset stays a plain view offset.  MAP_PRIVATE is a real copy-on-write view
 * (PAGE_WRITECOPY), never a silent shared one. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)addr;
    if (length == 0 || offset < 0) { errno = EINVAL; return MAP_FAILED; }
    if (offset % 65536 != 0) { errno = EINVAL; return MAP_FAILED; }

    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK) {
        errno = fd < 0 ? EBADF : ENODEV;
        return MAP_FAILED;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || offset >= size.QuadPart) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    const long long available = size.QuadPart - offset;
    if ((long long)length > available) { errno = EINVAL; return MAP_FAILED; }

    DWORD section_protect;
    DWORD view_access;
    if (prot & PROT_WRITE) {
        section_protect = (flags & MAP_SHARED) ? PAGE_READWRITE : PAGE_WRITECOPY;
        view_access = (flags & MAP_SHARED) ? FILE_MAP_WRITE : FILE_MAP_COPY;
    } else if (prot & PROT_EXEC) {
        section_protect = PAGE_EXECUTE_READ;
        view_access = FILE_MAP_EXECUTE | FILE_MAP_READ;
    } else if (prot & PROT_READ) {
        section_protect = (flags & MAP_SHARED) ? PAGE_READONLY : PAGE_WRITECOPY;
        view_access = (flags & MAP_SHARED) ? FILE_MAP_READ : FILE_MAP_COPY;
    } else {
        section_protect = PAGE_NOACCESS;
        view_access = 0;
    }

    const HANDLE mapping = CreateFileMappingW(handle, NULL, section_protect,
                                              (DWORD)((unsigned long long)size.QuadPart >> 32),
                                              (DWORD)((unsigned long long)size.QuadPart & 0xFFFFFFFFULL),
                                              NULL);
    if (!mapping) { set_last_error(); return MAP_FAILED; }
    void *view = MapViewOfFile(mapping, view_access,
                               (DWORD)((unsigned long long)offset >> 32),
                               (DWORD)((unsigned long long)offset & 0xFFFFFFFFULL),
                               length);
    CloseHandle(mapping);          /* the view keeps the section alive */
    if (!view) { set_last_error(); return MAP_FAILED; }
    return view;
}

int munmap(void *addr, size_t length)
{
    if (!addr || length == 0) { errno = EINVAL; return -1; }
    if (!UnmapViewOfFile(addr)) { set_last_error(); return -1; }
    return 0;
}

int msync(void *addr, size_t length, int flags)
{
    (void)addr;
    (void)length;
    (void)flags;
    errno = EOPNOTSUPP;            /* a copy view cannot be flushed in place */
    return -1;
}

/* ---- positioned I/O ----
 * An OVERLAPPED structure on a synchronous handle means "use this offset and
 * do not move the file pointer", which is exactly pread and pwrite. */
ssize_t pread(int fd, void *buffer, size_t count, off_t offset)
{
    if (offset < 0) { errno = EINVAL; return -1; }
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    if (GetFileType(handle) == FILE_TYPE_DISK) {
        OVERLAPPED overlapped;
        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.Offset = (DWORD)((unsigned long long)offset & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
        DWORD transferred = 0;
        const DWORD wanted = count > 0x7FFFF000u ? 0x7FFFF000u : (DWORD)count;
        if (ReadFile(handle, buffer, wanted, &transferred, &overlapped))
            return (ssize_t)transferred;
        const DWORD last = GetLastError();
        if (last == ERROR_HANDLE_EOF) return 0;
        SetLastError(last);
        set_last_error();
        return -1;
    }

    const long long saved = _lseeki64(fd, 0, SEEK_CUR);
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    const int got = _read(fd, buffer, count > 0x7FFFF000u ? 0x7FFFF000 : (int)count);
    if (saved >= 0) _lseeki64(fd, saved, SEEK_SET);
    return got < 0 ? -1 : (ssize_t)got;
}

ssize_t pwrite(int fd, const void *buffer, size_t count, off_t offset)
{
    if (offset < 0) { errno = EINVAL; return -1; }
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

    if (GetFileType(handle) == FILE_TYPE_DISK) {
        OVERLAPPED overlapped;
        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.Offset = (DWORD)((unsigned long long)offset & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
        DWORD transferred = 0;
        const DWORD wanted = count > 0x7FFFF000u ? 0x7FFFF000u : (DWORD)count;
        if (WriteFile(handle, buffer, wanted, &transferred, &overlapped))
            return (ssize_t)transferred;
        set_last_error();
        return -1;
    }

    const long long saved = _lseeki64(fd, 0, SEEK_CUR);
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    const int wrote = _write(fd, buffer, count > 0x7FFFF000u ? 0x7FFFF000 : (int)count);
    if (saved >= 0) _lseeki64(fd, saved, SEEK_SET);
    return wrote < 0 ? -1 : (ssize_t)wrote;
}

/* Every Windows file here is already behind the per-user profile ACL, and
 * nothing in this tree may read these as a security decision. */
uid_t geteuid(void) { return 0; }
uid_t getuid(void) { return 0; }

int fsync(int fd)
{
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    switch (GetFileType(handle)) {
    case FILE_TYPE_DISK:
        if (FlushFileBuffers(handle)) return 0;
        set_last_error();
        return -1;
    case FILE_TYPE_CHAR:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int ftruncate(int fd, off_t length)
{
    if (length < 0) { errno = EINVAL; return -1; }
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK) {
        errno = EINVAL;
        return -1;
    }
    LARGE_INTEGER position;
    position.QuadPart = length;
    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN) || !SetEndOfFile(handle)) {
        set_last_error();
        return -1;
    }
    return 0;
}

/* Windows has one permission bit: read-only.  S_IWUSR is numerically S_IWRITE,
 * so the mapping is exact in one direction and lossy in the other. */
int fchmod(int fd, mode_t mode)
{
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (GetFileType(handle) != FILE_TYPE_DISK) return 0;

    DWORD capacity = 512;
    wchar_t *path = NULL;
    for (int pass = 0; pass < 8; ++pass) {
        wchar_t *grown = (wchar_t *)realloc(path, (size_t)capacity * sizeof(wchar_t));
        if (!grown) { free(path); errno = ENOMEM; return -1; }
        path = grown;
        const DWORD written = GetFinalPathNameByHandleW(handle, path, capacity, FILE_NAME_OPENED);
        if (written > 0 && written < capacity) break;
        if (written == 0) { free(path); return 0; }   /* pipes and consoles have no mode */
        capacity = written + 16;
    }
    if (!path) return 0;

    const DWORD attributes = GetFileAttributesW(path);
    int result = 0;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        DWORD wanted = attributes;
        if (mode & S_IWUSR) wanted &= ~FILE_ATTRIBUTE_READONLY;
        else wanted |= FILE_ATTRIBUTE_READONLY;
        if (wanted != attributes && !SetFileAttributesW(path, wanted)) { set_last_error(); result = -1; }
    }
    free(path);
    return result;
}

/* ---- string helpers ---- */
int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
int strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, (int)n); }

char *strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    for (const char *walk = haystack; *walk; ++walk) {
        const char *left = walk;
        const char *right = needle;
        while (*right && *left && tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (!*right) return (char *)walk;
    }
    return NULL;
}

char *strndup(const char *s, size_t n)
{
    if (!s) { errno = EINVAL; return NULL; }
    size_t length = 0;
    while (length < n && s[length]) ++length;
    char *copy = (char *)malloc(length + 1);
    if (!copy) { errno = ENOMEM; return NULL; }
    memcpy(copy, s, length);
    copy[length] = '\0';
    return copy;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }
    size_t length = 0;
    for (;;) {
        const int character = fgetc(stream);
        if (character == EOF) {
            if (length == 0) return -1;
            break;
        }
        if (length + 2 > *n) {
            size_t grown = *n ? *n * 2 : 128;
            while (length + 2 > grown) grown *= 2;
            char *bigger = (char *)realloc(*lineptr, grown);
            if (!bigger) return -1;
            *lineptr = bigger;
            *n = grown;
        }
        (*lineptr)[length++] = (char)character;
        if (character == '\n') break;
    }
    (*lineptr)[length] = '\0';
    return (ssize_t)length;
}

/* ---- time ---- */
static long long performance_frequency(void)
{
    static long long frequency;
    if (frequency == 0) {
        LARGE_INTEGER counter_frequency;
        QueryPerformanceFrequency(&counter_frequency);
        frequency = counter_frequency.QuadPart ? counter_frequency.QuadPart : 1;
    }
    return frequency;
}

int clock_gettime(int clock_id, struct timespec *ts)
{
    if (!ts) { errno = EINVAL; return -1; }
    if (clock_id == CLOCK_MONOTONIC || clock_id == CLOCK_PROCESS_CPUTIME_ID
        || clock_id == CLOCK_THREAD_CPUTIME_ID) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        const long long frequency = performance_frequency();
        ts->tv_sec = (time_t)(counter.QuadPart / frequency);
        ts->tv_nsec = (long)((counter.QuadPart % frequency) * 1000000000LL / frequency);
        return 0;
    }
    if (clock_id != CLOCK_REALTIME) { errno = EINVAL; return -1; }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    long long seconds = 0;
    long nanoseconds = 0;
    filetime_to_parts(&now, &seconds, &nanoseconds);
    ts->tv_sec = (time_t)seconds;
    ts->tv_nsec = nanoseconds;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0) { errno = EINVAL; return -1; }
    unsigned long long milliseconds = (unsigned long long)req->tv_sec * 1000ULL
                                    + (unsigned long long)req->tv_nsec / 1000000ULL;
    if (milliseconds > 0 && milliseconds < 0xFFFFFFFFULL) Sleep((DWORD)milliseconds);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

int usleep(unsigned int useconds)
{
    const unsigned long long milliseconds = (useconds + 999u) / 1000u;
    if (milliseconds > 0) Sleep(milliseconds > 0xFFFFFFFFull ? 0xFFFFFFFF : (DWORD)milliseconds);
    return 0;
}
/* ---- descriptors without a /proc ----------------------------------------
 * There is no /proc/self/fd/2 on Windows, so duplicating an already-open
 * standard descriptor is the honest way to keep a second writer of it. */
int zuuned_open_self_fd(int fd)
{
    const HANDLE handle = fd_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    const int copy = _dup(fd);
    if (copy == -1) { set_last_error(); return -1; }
    return copy;
}

/* ---- temporary files ----
 * _mktemp_s is not usable here: it aborts the process when the X group is not
 * the last thing in the template, and every caller in this tree writes a suffix
 * after it.  So the X group is filled by hand and O_EXCL decides. */
static unsigned long long zuuned_random(void)
{
    static unsigned long long state;
    if (state == 0) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        state = (unsigned long long)counter.QuadPart
              ^ ((unsigned long long)GetCurrentProcessId() << 21)
              ^ (unsigned long long)(uintptr_t)&state;
        if (state == 0) state = 1;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

/* ---- stdio and file replacement over UTF-8 paths ----
 * fopen(), remove(), unlink() and rename() are _ACRTIMP bindings, so the shim
 * cannot override them with a declaration the way it does stat() or openat().
 * posix_shim.h takes them with function-like macros instead, and every call
 * lands here.  decode_path() gives UTF-8 priority exactly as the other wide
 * openers in this file do; left on the CRT original a name that came from Qt
 * would be read in the ANSI code page and open the wrong file. */
FILE *zuuned_fopen(const char *path, const char *mode)
{
    wchar_t *wide = zuuned_full_path(path);
    if (!wide) return NULL;
    wchar_t wide_mode[16];
    if (!mode || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                                     wide_mode, (int)ARRAYSIZE(wide_mode)) <= 0) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }
    FILE *stream = _wfopen(wide, wide_mode);
    free(wide);
    if (!stream) set_last_error();
    return stream;
}

int zuuned_remove(const char *path)
{
    wchar_t *wide = zuuned_full_path(path);
    if (!wide) return -1;
    const int result = _wremove(wide);
    free(wide);
    if (result != 0) set_last_error();
    return result;
}

/* POSIX rename() replaces an existing destination and the shared sources rely
 * on it - id3_rewrite publishes a retagged file straight over the original -
 * while the CRT rename() refuses an existing target with EACCES.  MoveFileExW
 * with MOVEFILE_REPLACE_EXISTING is the contract those callers were written
 * against, so this is the portable spelling rather than a Windows quirk.
 * A directory destination still fails, as POSIX requires. */
int zuuned_rename(const char *from_path, const char *to_path)
{
    wchar_t *from = zuuned_full_path(from_path);
    if (!from) return -1;
    wchar_t *to = zuuned_full_path(to_path);
    if (!to) { free(from); return -1; }
    const BOOL replaced = MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    free(from);
    free(to);
    if (!replaced) { set_last_error(); return -1; }
    return 0;
}

/* ---- unique temporary files ----
 * mkstemps() is not in the CRT; the template is decoded the same way as every
 * other path here, so a caller can build one out of ZUUNED_TMPDIR freely.
 */

int zuuned_mkstemps(char *templat, int suffix_length)
{
    if (!templat || suffix_length < 0) { errno = EINVAL; return -1; }
    const size_t length = strlen(templat);
    if (length < 6 + (size_t)suffix_length) { errno = EINVAL; return -1; }
    const size_t start = length - 6 - (size_t)suffix_length;
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    for (int attempt = 0; attempt < 256; ++attempt) {
        for (int index = 0; index < 6; ++index)
            templat[start + (size_t)index] = alphabet[zuuned_random() % 62];
        wchar_t *wide = zuuned_full_path(templat);
        if (!wide) return -1;
        const int fd = open_normalized_wide(wide,
                                      O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC);
        free(wide);
        if (fd != -1) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int zuuned_mkstemp(char *templat)
{
    return zuuned_mkstemps(templat, 0);
}

/* The temporary directory has to be usable by both sides of this shim: our own
 * open(), which decodes UTF-8 first, and the CRT unlink()/rename() the transcode
 * layer still calls directly, which are ANSI-only.  An ANSI code page rendering
 * satisfies both, and when the profile path cannot be written in the
 * active code page the 8.3 form is used, because that is pure ASCII and both
 * sides agree on it. */
#include <ctype.h>

static char win_tmpdir_buffer[4096];

static int narrow_matches_wide(const wchar_t *wide, const char *narrow)
{
    const int needed = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow, -1, NULL, 0);
    if (needed <= 0) return 0;
    wchar_t *again = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (!again) return 0;
    const int ok = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow, -1, again, needed) > 0
                && wcscmp(again, wide) == 0;
    free(again);
    return ok;
}

static void build_win_tmpdir(void)
{
    if (win_tmpdir_buffer[0]) return;

    wchar_t wide[2 * MAX_PATH];
    const DWORD length = GetTempPathW(ARRAYSIZE(wide) - 4, wide);
    if (length > 0 && length < ARRAYSIZE(wide) - 4) {
        size_t end = length;
        while (end > 0 && (wide[end - 1] == L'\\' || wide[end - 1] == L'/')) wide[--end] = L'\0';

        char *narrow = wide_to_narrow(wide);
        if (narrow && !narrow_matches_wide(wide, narrow)) {
            free(narrow);
            narrow = NULL;
            wchar_t short_path[2 * MAX_PATH];
            if (GetShortPathNameW(wide, short_path, ARRAYSIZE(short_path)) > 0)
                narrow = wide_to_narrow(short_path);
        }
        if (narrow) {
            const size_t size = strlen(narrow);
            if (size && size + 2 < sizeof(win_tmpdir_buffer)) {
                memcpy(win_tmpdir_buffer, narrow, size);
                win_tmpdir_buffer[size] = '/';
                win_tmpdir_buffer[size + 1] = '\0';
            }
            free(narrow);
        }
    }
    if (!win_tmpdir_buffer[0]) memcpy(win_tmpdir_buffer, "./", 3);
}

char *zuuned_win_tmpdir(void)
{
    build_win_tmpdir();
    return win_tmpdir_buffer;
}

/* Spell one path in the bytes the shell behind system() decodes.
 *
 * cmd.exe reads its command line with the active ANSI code page.  The portable
 * sources hand UTF-8, which is the shape a POSIX filename already has, so a
 * name outside ASCII reaches ffmpeg as mojibake: the encoder cannot open the
 * file, the shell-out reports a plain failure, and zune_smuggle_photo() then
 * sends the untouched original.  Re-encode into what that shell reads back as
 * the same file.
 *
 * The UTF-8-first test is the one decode_path() already makes, so a name means
 * the same file here as it does wherever the shim opens it.  That shared rule is
 * also why a caller may only hand this UTF-8: a two-byte GBK sequence is very
 * often valid UTF-8 as well, so bytes that arrived in the code page would be
 * read as some other characters entirely.  ZUUNED_TMPDIR is such a path and is
 * never passed here - build_win_tmpdir() already wrote it in the code page, or
 * in the 8.3 name when the code page could not carry it.
 *
 * WC_NO_BEST_FIT_CHARS and the used-default flag are the point rather than
 * decoration: a name with no spelling in the active code page has to come back
 * NULL, because the '?' bytes WideCharToMultiByte substitutes otherwise name a
 * different file and the failure looks like a missing picture. */
char *zuuned_narrow_path_for_shell(const char *path)
{
    if (!path) return NULL;

    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_len <= 0) return _strdup(path);   /* not UTF-8: code-page bytes already */

    wchar_t *wide = (wchar_t *)malloc((size_t)wide_len * sizeof(wchar_t));
    if (!wide) { errno = ENOMEM; return NULL; }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, wide_len) <= 0) {
        free(wide);
        return _strdup(path);
    }

    int plain_ascii = 1;
    for (const wchar_t *c = wide; *c; ++c)
        if (*c > 0x7F) { plain_ascii = 0; break; }
    if (plain_ascii) { free(wide); return _strdup(path); }

    char *narrow = NULL;
    BOOL used_default = FALSE;
    const int narrow_len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1,
                                               NULL, 0, NULL, NULL);
    if (narrow_len > 0) {
        narrow = (char *)malloc((size_t)narrow_len);
        if (narrow
            && WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, narrow,
                                   narrow_len, NULL, &used_default) > 0
            && !used_default) {
            free(wide);
            return narrow;
        }
    }
    free(narrow);
    free(wide);
    errno = EILSEQ;
    return NULL;
}

/* ---- process start ----
 * Binary stdio is not cosmetic: the default text mode translates CRLF and stops
 * at 0x1A, which would corrupt every media file read through the CRT and garble
 * the log stream.  Unbuffered stderr keeps a crash log in step with the crash.
 *
 * The invalid-parameter handler is load-bearing, not hygiene.  fd_handle() has
 * to answer for descriptors it never created - QFile::handle() hands out a raw
 * NT handle and the shared sources pass it straight to fstat() - and the only
 * way to tell a descriptor from a handle is to ask the CRT, which reports an
 * out-of-range descriptor through the invalid-parameter machinery.  Left at its
 * default that raises a message box, and under a debug CRT a Watson dialog, on
 * whatever thread made the call; in a headless run it reads as a hang, which is
 * exactly what fstat() on a raw handle did in the first run of the gate.
 * Returning from the handler is the documented way back: the CRT call yields
 * its failure value, so fd_handle() falls through to the handle reading that a
 * raw QFile::handle() actually needs.  It must therefore do nothing at all. */
static void __cdecl zuuned_invalid_parameter(const wchar_t *expression,
                                             const wchar_t *function,
                                             const wchar_t *file,
                                             unsigned int line,
                                             uintptr_t reserved)
{
    (void)expression;
    (void)function;
    (void)file;
    (void)line;
    (void)reserved;
    /* Deliberately empty.  The handler runs while the CRT already holds the
     * per-thread-data lock, so even touching errno here deadlocks against the
     * call that raised it - observed as fstat() on a raw handle wedging the
     * process before this returned.  Callers in this file set their own errno. */
}

/* The guard cannot wait for a main() that never comes.  The tools in this tree
 * reach fstat() through the shared sources without initialising anything - only
 * src/main.cpp calls zuuned_win_runtime_init() - so an out-of-range descriptor in
 * spokenmediatool or librarytool would meet the CRT's own handler, which answers
 * with a dialog on the calling thread under a debug CRT and terminates the
 * process under a release one.
 *
 * A .CRT$XIB initialiser was tried first, and the linker said why it does not
 * work: this object's .CRT subsection carries different section characteristics
 * from the CRT's own, LNK4254 folded it into .rdata, and start-up never ran it.
 * The win-compat gate proved that by finding the handler still null in main().
 *
 * So it goes in lazily, from the one function that ever asks the descriptor table
 * about a number it did not hand out, which exists in exactly the binaries that
 * can be affected and runs after the CRT is certainly up.  Nothing else is
 * installed here: the stdio mode below still wants an explicit init call. */
/* Re-asserted rather than installed once, which is a change of design found by
 * the win-compat gate.  This was an InitOnceExecuteOnce, and a spent one-shot
 * keeps answering "installed" to a process whose handler has since been
 * replaced - the probe clears it on purpose precisely to prove fd_handle() puts
 * it back, and with the flag consumed by zuuned_win_runtime_init() the answer
 * came back as __fastfail 0xC0000409 inside test_opendir() instead of the -1
 * that branch is built to receive.  Reading the current handler costs one UCRT
 * lookup per fd_handle(), and the handler is process-wide here - measured, a
 * thread created before the set sees it afterwards - so nobody needs a private
 * copy and whoever loads this shim still gets a table that is safe to ask. */
static void ensure_invalid_parameter_guard(void)
{
    if (_get_invalid_parameter_handler() == zuuned_invalid_parameter) return;
    _set_invalid_parameter_handler(zuuned_invalid_parameter);
}

/* The console owns its own code page, and it is not UTF-8 by default, so the
 * UTF-8 text this tree writes arrives as mojibake - on a CP936 host every line
 * naming a CJK track read as garbage, which turns the main Windows diagnostic
 * channel into a false report.  Because the setting belongs to the shared
 * console rather than to this process, it goes back on the way out. */
static UINT zuuned_console_output_cp;

static void zuuned_restore_console_output_cp(void)
{
    if (!zuuned_console_output_cp) return;
    const UINT previous = zuuned_console_output_cp;
    zuuned_console_output_cp = 0;
    SetConsoleOutputCP(previous);
}

void zuuned_win_console_utf8(void)
{
    if (zuuned_console_output_cp) return;
    const UINT previous = GetConsoleOutputCP();
    if (previous == 0 || previous == CP_UTF8) return;
    if (!SetConsoleOutputCP(CP_UTF8)) { set_last_error(); return; }
    zuuned_console_output_cp = previous;
    /* If the handler cannot be queued, leave the console alone rather than
     * restore a code page nobody remembers. */
    if (atexit(zuuned_restore_console_output_cp) != 0) zuuned_console_output_cp = 0;
}

void zuuned_win_runtime_init(void)
{
    /* The guards come first because the stdio calls below are precisely what
     * they answer.  A GUI-subsystem process started by double-click has no
     * console, so the CRT gets nothing from GetStdHandle() and _fileno() of a
     * stream that never received an OS handle is out of range for _setmode(),
     * which reports that through the invalid-parameter machinery: a modal box,
     * and under a release CRT a __fastfail() before any handler could exist.
     * SetErrorMode() belongs here for the same reason - it is the other thing
     * that has to be true before anything is allowed to fail, including a
     * missing DLL or a failed drive, which the app reports by itself. */
    ensure_invalid_parameter_guard();
    SetErrorMode(SEM_FAILCRITICALERRORS);

    _fmode = _O_BINARY;
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    setvbuf(stderr, NULL, _IONBF, 0);
    zuuned_win_console_utf8();
#ifdef _DEBUG
    /* Debug-CRT reports default to a modal window as well as the debugger, and
     * a box that nobody can dismiss is indistinguishable from a hang.  Keep them
     * debugger-only; the application has its own diagnostics on stderr.  The
     * types are enumerated values, so they are set one at a time, not OR-ed. */
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
    build_win_tmpdir();
}

/* ---- debug console -------------------------------------------------------
 *
 * zuuned.exe is a GUI-subsystem image so that opening it shows a window and
 * nothing else; a command window is a build artefact as far as a user is
 * concerned.  That leaves stderr, which this tree treats as the primary
 * diagnostic channel, with no reader at all, so --debug asks for one back.
 *
 * Two rules keep that honest.  It never takes a stream that already works: a
 * launcher that handed us a pipe - both Windows gates, and any Start-Process
 * with redirection - keeps that pipe, because a console that steals stdout
 * silently blinds every harness that reads it.  And it attaches before it
 * allocates: when Zuuned came out of a terminal that simply did not wire the
 * handles, the lines belong in that terminal, not in a second window.
 */
int zuuned_win_output_available(void)
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return 1;
    if (err && err != INVALID_HANDLE_VALUE) return 1;
    return 0;
}

int zuuned_win_debug_console(void)
{
    if (zuuned_win_output_available()) return 0;

    int allocated = 0;
    if (GetConsoleWindow() == NULL && !AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) { set_last_error(); return 0; }
        allocated = 1;
    }

    /* A console wants CRLF, so these three opt out of the binary mode the
     * runtime init chose for files; the mode is spelled on the open as well
     * because _fmode is _O_BINARY by now, and CONIN$/CONOUT$ are devices rather
     * than paths.  freopen_s is used instead of the plain POSIX name because it
     * answers with a status, so a stream that would not open is counted rather
     * than assumed, and because the deprecated-alias spelling it is tempting to
     * reach for - _freopen - is one the UCRT does not even export: the first
     * build of this function linked as far as an unresolved _freopen and said
     * so, with three implicit-declaration warnings beside it. */
    int opened = 0;
    FILE *stream = NULL;
    if (freopen_s(&stream, "CONIN$", "rt", stdin) == 0 && stream) {
        _setmode(_fileno(stdin), _O_TEXT);
        ++opened;
    }
    stream = NULL;
    if (freopen_s(&stream, "CONOUT$", "wt", stdout) == 0 && stream) {
        _setmode(_fileno(stdout), _O_TEXT);
        setvbuf(stdout, NULL, _IOLBF, 0);
        ++opened;
    }
    stream = NULL;
    if (freopen_s(&stream, "CONOUT$", "wt", stderr) == 0 && stream) {
        _setmode(_fileno(stderr), _O_TEXT);
        setvbuf(stderr, NULL, _IONBF, 0);
        ++opened;
    }
    if (!opened) {
        set_last_error();
        /* Deliberately no FreeConsole(): the console this process allocated is
         * the one holding the log the user asked for, and it goes with the
         * process rather than being closed under them on the way out. */
        return 0;
    }

    /* The call in runtime init ran while there was no console to ask. */
    zuuned_win_console_utf8();
    if (allocated) SetConsoleTitleW(L"Zuuned - debug console");
    return 1;
}

/* A dialog is the last channel a GUI-subsystem process has.  A double-clicked
 * instance that decides not to start - a second copy bowing out in front of the
 * first - otherwise leaves the user with no window and no message, which reads
 * as a crash that produced nothing.  The strings arrive UTF-8 because that is
 * what the portable sources hold, and decode_path() is already this file's
 * strict-UTF-8-then-ANSI decoder; text that will not decode still has to produce
 * a dialog, hence the fixed fallbacks. */
void zuuned_win_notice(const char *title, const char *text)
{
    wchar_t *wide_text = text ? decode_path(text) : NULL;
    wchar_t *wide_title = title ? decode_path(title) : NULL;
    MessageBoxW(NULL, wide_text ? wide_text : L"Zuuned",
                wide_title ? wide_title : L"Zuuned", MB_OK | MB_ICONINFORMATION);
    free(wide_text);
    free(wide_title);
}
