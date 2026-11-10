/* Runtime gate for the Windows POSIX shim (compat/win/wincompat.c).
 *
 * Every check asserts a contract a portable source in this tree depends on, so
 * a failure here predicts a real bug in the app rather than a cosmetic one:
 *  - st_size survives files past 2 GiB (the CRT struct stat would truncate it),
 *  - paths past 260 characters and UTF-8 names open, stat and read back,
 *  - a directory descriptor is a real descriptor: fstat says S_ISDIR, flock
 *    guards it, and the at() family resolves relative names against it,
 *  - fstat() also resolves a descriptor opened outside the shim, because that
 *    is what QFile::handle() hands AudiobookProbe on Windows,
 *  - pipe readiness answers EOF instead of blocking (SessionLog drains pipes),
 *  - mmap/pread read a staged file, and zuuned_link publishes it atomically,
 *  - fopen/remove/unlink/rename go through the wide API, so a UTF-8 media name
 *    opens under that exact name and a path past 260 characters still works,
 *  - a path handed to system() is re-spelled in the bytes cmd.exe decodes, and a
 *    name the active code page cannot spell is refused instead of guessed.
 *
 * Build and run with tests/win-compat/run.ps1.  Scaffolding (creating the
 * fixture tree) uses the Win32 API directly on purpose: the shim must be the
 * only thing under test.
 */
#include "wincompat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

/* winioctl.h only carries this behind an NTDDI level the shim does not set, */
/* so the control code is spelled out here. */
#ifndef FSCTL_SET_SPARSE
#define FSCTL_SET_SPARSE 0x0009002CU
#endif

static int checks;
static int failures;
static int skips;
static char root[1024];
static wchar_t rootw[1280];
static wchar_t root_ext[1300];

#define CHECK(cond, ...)                                                       \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define SKIP(why)                                                              \
    do {                                                                      \
        ++skips;                                                              \
        printf("SKIP %s\n", (why));                                           \
    } while (0)

static void to_wide(const char *narrow, wchar_t *wide, int cap)
{
    if (MultiByteToWideChar(CP_UTF8, 0, narrow, -1, wide, cap) <= 0) wide[0] = 0;
}

static void path_in(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", root, name);
}

/* Delete everything the run created; the sweep runs through the extended-path
 * prefix so the long-path fixture is reachable here too. */
static void sweep(const wchar_t *dir)
{
    wchar_t pattern[2600];
    WIN32_FIND_DATAW find;
    _snwprintf(pattern, 2600, L"%ls\\*", dir);
    const HANDLE handle = FindFirstFileW(pattern, &find);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(find.cFileName, L".") || !wcscmp(find.cFileName, L"..")) continue;
        wchar_t child[2600];
        _snwprintf(child, 2600, L"%ls\\%ls", dir, find.cFileName);
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sweep(child);
        if (!DeleteFileW(child)) RemoveDirectoryW(child);
    } while (FindNextFileW(handle, &find));
    FindClose(handle);
    RemoveDirectoryW(dir);
}

static int write_narrow(const char *path, const void *bytes, size_t length)
{
    const int fd = openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd == -1) return -1;
    const int want = (int)length;
    const int done = write(fd, bytes, (unsigned)length);
    close(fd);
    return done == want ? 0 : -1;
}

static int read_narrow(const char *path, char *buffer, size_t cap)
{
    const int fd = openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return -1;
    size_t filled = 0;
    for (;;) {
        const int got = read(fd, buffer + filled, (unsigned)(cap - filled));
        if (got < 0) { close(fd); return -1; }
        if (got == 0) break;
        filled += (size_t)got;
        if (filled == cap) break;
    }
    close(fd);
    return (int)filled;
}

/* Same conversion as to_wide() plus the separators the wide API expects, so a
 * fixture can be verified with GetFileAttributesW() without the shim involved. */
static void wide_path(const char *narrow, wchar_t *wide, int cap)
{
    to_wide(narrow, wide, cap);
    for (wchar_t *p = wide; *p; ++p) if (*p == L'/') *p = L'\\';
}

/* ---- 1. the temporary directory is usable by both sides of the shim ---- */
static void test_tmpdir(void)
{
    char probe[1200];
    snprintf(probe, sizeof(probe), "%szuuned-tmp-probe.txt", ZUUNED_TMPDIR);
    CHECK(write_narrow(probe, "x", 1) == 0, "cannot write into ZUUNED_TMPDIR: %s", probe);
    remove(probe);
    struct stat st;
    CHECK(stat(probe, &st) == -1 && errno == ENOENT, "removed tmp probe still stats");
}

/* ---- 2. mkstemps honours a suffix, which _mktemp_s cannot ---- */
static void test_mkstemps(void)
{
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%szuuned-mkst-XXXXXX.pcm", ZUUNED_TMPDIR);
    const int fd = mkstemps(pattern, 4);
    CHECK(fd != -1, "mkstemps failed: errno %d", errno);
    if (fd == -1) return;
    CHECK(strstr(pattern, "XXXXXX") == NULL, "template not filled: %s", pattern);
    CHECK(strcmp(pattern + strlen(pattern) - 4, ".pcm") == 0, "suffix moved: %s", pattern);
    CHECK(write(fd, "y", 1) == 1, "cannot write to mkstemps descriptor");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 1, "mkstemps size wrong");
    close(fd);
    remove(pattern);
    const char *leaf = strrchr(pattern, '/');
    CHECK(leaf && strlen(leaf) < 260, "mkstemps leaf name too long");
}

/* ---- 3. a UTF-8 media name round-trips ---- */
static void test_utf8_name(void)
{
    /* "temp-zuuned-<Chinese name>.mp3" written as explicit UTF-8 bytes so this
     * source stays ASCII regardless of the code page it is compiled under. */
    char file[1200];
    snprintf(file, sizeof(file), "%s/\xc3\xa9\xc3\xa9t\xc3\xa9-\xe6\xb5\x8b\xe8\xaf\x95\xe6\x96\x87\xe4\xbb\xb6.mp3",
             root);
    const char body[] = "ID3-zuuned";
    CHECK(write_narrow(file, body, sizeof(body) - 1) == 0, "UTF-8 open+write failed: errno %d", errno);
    struct stat st;
    CHECK(stat(file, &st) == 0, "UTF-8 stat failed: errno %d", errno);
    CHECK(st.st_size == (long long)sizeof(body) - 1, "UTF-8 size %lld", st.st_size);
    CHECK(S_ISREG(st.st_mode), "UTF-8 name is not a regular file");
    char back[32] = { 0 };
    CHECK(read_narrow(file, back, sizeof(back)) == (int)sizeof(body) - 1, "UTF-8 read-back short");
    CHECK(memcmp(back, body, sizeof(body) - 1) == 0, "UTF-8 contents differ");
    /* The CRT, which cannot see the name, must fail - proving we are not
     * accidentally reading a transliterated cousin. */
    struct __stat64 probe;
    wchar_t wide[1200];
    to_wide(file, wide, 1200);
    CHECK(wide[0] != 0 && wcsstr(wide, L"\x6d4b\x8bd5") != NULL, "UTF-8 decode lost the name");
    CHECK(_wstat64(wide, &probe) == 0, "wide stat of the same name failed");
    CHECK((long long)probe.st_size == (long long)sizeof(body) - 1, "wide size differs");
    remove(file);
}

/* ---- 4. paths past the legacy 260-character limit ---- */
static void test_long_path(void)
{
    wchar_t dir[2600];
    char narrow[1200];
    _snwprintf(dir, 2600, L"%ls\\long", root_ext);
    /* The parent has to exist before CreateDirectoryW can build inside it. */
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        CHECK(0, "cannot create the long-path parent: %d", (int)GetLastError());
    char segment[48];
    memset(segment, 'd', sizeof(segment) - 1);
    segment[sizeof(segment) - 1] = 0;
    int deep = 0;
    for (int level = 0; level < 8; ++level) {
        wchar_t child[2600];
        _snwprintf(child, 2600, L"%ls\\%hs", dir, segment);
        if (!CreateDirectoryW(child, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) break;
        wcscpy(dir, child);
        ++deep;
    }
    if (deep < 8) { SKIP("could not build the deep fixture"); return; }

    char leaf[64];
    memset(leaf, 'f', sizeof(leaf) - 1);
    leaf[sizeof(leaf) - 1] = 0;
    snprintf(narrow, sizeof(narrow), "%s/long", root);
    for (int level = 0; level < deep; ++level) snprintf(narrow + strlen(narrow), sizeof(narrow) - strlen(narrow), "/%s", segment);
    char file[2600];
    snprintf(file, sizeof(file), "%s/%s", narrow, leaf);
    CHECK(strlen(file) > 260, "fixture is not long enough: %u", (unsigned)strlen(file));

    CHECK(write_narrow(file, "deep", 4) == 0, "long-path write failed: errno %d", errno);
    struct stat st;
    CHECK(stat(file, &st) == 0, "long-path stat failed: errno %d", errno);
    CHECK(st.st_size == 4, "long-path size %lld", st.st_size);
    char back[8] = { 0 };
    CHECK(read_narrow(file, back, sizeof(back)) == 4, "long-path read failed");
    CHECK(memcmp(back, "deep", 4) == 0, "long-path contents differ");
    const int fd = openat(AT_FDCWD, file, O_RDONLY | O_NOFOLLOW);
    CHECK(fd != -1, "long-path openat failed: errno %d", errno);
    if (fd != -1) close(fd);
    CHECK(unlinkat(AT_FDCWD, file, 0) == 0, "long-path unlinkat failed: errno %d", errno);
    CHECK(stat(file, &st) == -1, "long-path unlink did not remove the file");
}
/* ---- 5. st_size past 2 GiB (the CRT struct stat truncates here) ---- */
static void test_large_file(void)
{
    const long long want = 3LL * 1024 * 1024 * 1024;
    wchar_t file[2600];
    _snwprintf(file, 2600, L"%ls\\big.bin", root_ext);
    DeleteFileW(file);
    const HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) { SKIP("cannot create the large fixture"); return; }
    DWORD returned = 0;
    DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
    LARGE_INTEGER end;
    end.QuadPart = want;
    const BOOL moved = SetFilePointerEx(handle, end, NULL, FILE_BEGIN);
    const BOOL sized = moved && SetEndOfFile(handle);
    CloseHandle(handle);
    if (!sized) { SKIP("volume refused the large fixture"); DeleteFileW(file); return; }

    char narrow[1200];
    path_in(narrow, sizeof(narrow), "big.bin");
    struct stat st;
    CHECK(stat(narrow, &st) == 0, "large stat failed: errno %d", errno);
    CHECK(st.st_size == want, "large st_size %lld, expected %lld", st.st_size, want);
    const int fd = openat(AT_FDCWD, narrow, O_RDONLY);
    CHECK(fd != -1, "large openat failed: errno %d", errno);
    if (fd != -1) {
        struct stat via_fd;
        CHECK(fstat(fd, &via_fd) == 0 && via_fd.st_size == want, "large fstat size wrong");
        char byte = 'x';
        CHECK(pread(fd, &byte, 1, want - 1) == 1 && byte == 0, "pread at the end of a sparse tail failed");
        close(fd);
    }
    DeleteFileW(file);
}

/* ---- 6. a directory descriptor behaves like one ---- */
static void test_directory_descriptor(void)
{
    char dir[1200];
    snprintf(dir, sizeof(dir), "%s/tree", root);
    wchar_t ext[2600];
    _snwprintf(ext, 2600, L"%ls\\tree", root_ext);
    if (!CreateDirectoryW(ext, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        CHECK(0, "cannot create the tree directory");

    for (int index = 0; index < 3; ++index) {
        char leaf[1200];
        snprintf(leaf, sizeof(leaf), "tree/child-%d.txt", index);
        char full[1200];
        path_in(full, sizeof(full), leaf);
        CHECK(write_narrow(full, "abc", 3) == 0, "tree child %d not written", index);
    }
    wchar_t subext[2600];
    _snwprintf(subext, 2600, L"%ls\\tree\\nested", root_ext);
    if (!CreateDirectoryW(subext, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        CHECK(0, "cannot create the nested directory");

    const int fd = zuuned_open_directory(dir);
    CHECK(fd != -1, "zuuned_open_directory failed: errno %d", errno);
    if (fd == -1) return;
    struct stat st;
    CHECK(fstat(fd, &st) == 0, "fstat on a directory descriptor failed: errno %d", errno);
    CHECK(S_ISDIR(st.st_mode), "directory descriptor is not S_ISDIR");

    /* O_DIRECTORY through openat must land in the same place. */
    const int via_flags = openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
    CHECK(via_flags != -1, "openat with O_DIRECTORY failed: errno %d", errno);
    if (via_flags != -1) {
        struct stat flag_stat;
        CHECK(fstat(via_flags, &flag_stat) == 0 && S_ISDIR(flag_stat.st_mode), "O_DIRECTORY fstat wrong");
        close(via_flags);
    }
    /* O_DIRECTORY on a regular file must fail, not quietly succeed. */
    char file[1200];
    path_in(file, sizeof(file), "tree/child-0.txt");
    CHECK(openat(AT_FDCWD, file, O_RDONLY | O_DIRECTORY) == -1, "O_DIRECTORY accepted a regular file");

    /* The at() family resolves a relative name against that descriptor. */
    struct stat at_stat;
    CHECK(fstatat(fd, "child-1.txt", &at_stat, 0) == 0, "fstatat failed: errno %d", errno);
    CHECK(at_stat.st_size == 3, "fstatat size %lld", at_stat.st_size);
    const int child = openat(fd, "child-2.txt", O_RDONLY);
    CHECK(child != -1, "relative openat failed: errno %d", errno);
    if (child != -1) close(child);
    CHECK(fstatat(fd, "does-not-exist", &at_stat, 0) == -1, "fstatat invented a file");

    /* flock over a directory descriptor is the single-instance guard. */
    const int second = zuuned_open_directory(dir);
    CHECK(second != -1, "second open of the directory failed");
    if (second != -1) {
        CHECK(flock(fd, LOCK_EX) == 0, "first LOCK_EX failed: errno %d", errno);
        errno = 0;
        CHECK(flock(second, LOCK_EX | LOCK_NB) == -1, "second LOCK_EX was allowed");
        CHECK(errno == EWOULDBLOCK || errno == EAGAIN, "busy lock errno %d", errno);
        CHECK(flock(fd, LOCK_UN) == 0, "unlock failed: errno %d", errno);
        CHECK(flock(second, LOCK_EX | LOCK_NB) == 0, "lock not released after unlock");
        flock(second, LOCK_UN);
        close(second);
    }
    CHECK(unlinkat(fd, "child-1.txt", 0) == 0, "unlinkat failed: errno %d", errno);
    CHECK(unlinkat(fd, "nested", AT_REMOVEDIR) == 0, "unlinkat(AT_REMOVEDIR) failed: errno %d", errno);
    CHECK(unlinkat(fd, "nested", 0) == -1, "unlinkat removed a directory without the flag");
    close(fd);
}
/* ---- 7. opendir/readdir/dirfd, which LibraryScanner walks ---- */
static void test_opendir(void)
{
    char dir[1200];
    snprintf(dir, sizeof(dir), "%s/scan", root);
    wchar_t ext[2600];
    _snwprintf(ext, 2600, L"%ls\\scan", root_ext);
    CreateDirectoryW(ext, NULL);
    for (int index = 0; index < 4; ++index) {
        char full[1200];
        snprintf(full, sizeof(full), "%s/scan/leaf-%d.mp3", root, index);
        CHECK(write_narrow(full, "z", 1) == 0, "scan leaf %d not written", index);
    }
    char deep[1200];
    snprintf(deep, sizeof(deep), "%s/scan/subdir", root);
    wchar_t deep_ext[2600];
    _snwprintf(deep_ext, 2600, L"%ls\\scan\\subdir", root_ext);
    CreateDirectoryW(deep_ext, NULL);

    DIR *stream = opendir(dir);
    CHECK(stream != NULL, "opendir failed: errno %d", errno);
    if (!stream) return;
    int files = 0;
    int directories = 0;
    int total = 0;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        ++total;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (entry->d_type == DT_DIR) ++directories;
        else if (entry->d_type == DT_REG) ++files;
        CHECK(strchr(entry->d_name, '/') == NULL, "readdir returned a path, not a name");
    }
    CHECK(files == 4, "readdir found %d regular files, expected 4", files);
    CHECK(directories == 1, "readdir found %d directories, expected 1", directories);
    rewinddir(stream);
    CHECK(readdir(stream) != NULL, "rewinddir did not restart the scan");
    const int fd = dirfd(stream);
    CHECK(fd != -1, "dirfd failed");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode), "fstat(dirfd()) is not a directory");
    CHECK(closedir(stream) == 0, "closedir failed");

    /* fstat() through a raw NT handle.  This is the fallback branch of
     * fd_handle(), kept for callers that do pass a handle.  It is NOT what Qt
     * does: Qt opens a QFile on Windows with _wopen(), so QFile::handle() is a
     * CRT descriptor - the case the block below covers. */
    wchar_t file_ext[2600];
    _snwprintf(file_ext, 2600, L"%ls\\scan\\leaf-0.mp3", root_ext);
    const HANDLE raw = CreateFileW(file_ext, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(raw != INVALID_HANDLE_VALUE, "raw fixture open failed");
    if (raw != INVALID_HANDLE_VALUE) {
        struct stat via_raw;
        const int as_int = (int)(unsigned int)(uintptr_t)raw;
        CHECK(fstat(as_int, &via_raw) == 0, "fstat on a raw NT handle failed: errno %d", errno);
        CHECK(via_raw.st_size == 1, "raw handle size %lld", via_raw.st_size);
        CHECK(S_ISREG(via_raw.st_mode), "raw handle is not a regular file");
        CloseHandle(raw);
    }

    /* The descriptor Qt actually reports.  Nothing in this repository handed out
     * this number, which is precisely the condition that used to break: the shim
     * read it as a raw handle, GetFileType refused it, and every Windows
     * audiobook import died on "Audio source is not a regular file". */
    const int foreign = _wopen(file_ext, _O_RDONLY | _O_BINARY);
    CHECK(foreign != -1, "opening the fixture outside the shim failed: errno %d", errno);
    if (foreign != -1) {
        struct stat via_descriptor;
        CHECK(fstat(foreign, &via_descriptor) == 0,
              "fstat on a foreign CRT descriptor failed: errno %d", errno);
        CHECK(via_descriptor.st_size == 1, "foreign descriptor size %lld", via_descriptor.st_size);
        CHECK(S_ISREG(via_descriptor.st_mode), "foreign descriptor is not a regular file");
        _close(foreign);
    }
    CHECK(fstat(-1, &st) == -1 && errno == EBADF, "fstat(-1) is not EBADF");

    /* The guard that makes the two questions above safe to ask belongs to the
     * shim, not to whoever wrote main(): a tool never calls anything first.  It is
     * cleared before this function runs, so if it is standing here the shim put it
     * back itself.  Without it, asking the descriptor table about a raw handle
     * raises a CRT dialog on the calling thread, which in a headless run is a
     * hang, and the two checks above would never have printed anything. */
    CHECK(_get_invalid_parameter_handler() != NULL,
          "fd_handle() did not install the invalid-parameter guard itself");
}

/* ---- 8. pipes and readiness: SessionLog drains both ends ---- */
static void test_pipes(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe failed: errno %d", errno);
    if (fds[0] == -1) return;

    struct pollfd out[1];
    out[0].fd = fds[1];
    out[0].events = POLLOUT;
    out[0].revents = 0;
    CHECK(poll(out, 1, 0) == 1 && (out[0].revents & POLLOUT), "write end is not ready");

    errno = 0;
    CHECK(zuuned_fd_readable(fds[0]) == 0, "empty read end reported readable");
    struct pollfd in[1];
    in[0].fd = fds[0];
    in[0].events = POLLIN;
    in[0].revents = 0;
    CHECK(poll(in, 1, 0) == 0, "poll waited on an empty pipe");

    CHECK(write(fds[1], "ping", 4) == 4, "pipe write failed");
    CHECK(zuuned_fd_readable(fds[0]) == 1, "data not reported readable");
    in[0].revents = 0;
    CHECK(poll(in, 1, 100) == 1 && (in[0].revents & POLLIN), "poll missed pipe data");
    char buffer[8] = { 0 };
    CHECK(read(fds[0], buffer, 4) == 4 && memcmp(buffer, "ping", 4) == 0, "pipe read-back differs");

    /* EOF must wake a reader: a blocking read here deadlocks the log drain. */
    close(fds[1]);
    in[0].revents = 0;
    const int woke = poll(in, 1, 2000);
    CHECK(woke == 1, "poll did not report a closed pipe (%d, revents %d)", woke, in[0].revents);
    CHECK((in[0].revents & POLLIN) && (in[0].revents & POLLHUP),
          "closed pipe revents %d", in[0].revents);
    CHECK(zuuned_fd_readable(fds[0]) == 1, "EOF not reported readable");
    CHECK(read(fds[0], buffer, sizeof(buffer)) == 0, "read after EOF is not end of file");
    close(fds[0]);

    struct pollfd bad[1];
    bad[0].fd = 12345;
    bad[0].events = POLLIN;
    bad[0].revents = 0;
    poll(bad, 1, 0);
    CHECK(bad[0].revents & POLLNVAL, "a bogus descriptor is not POLLNVAL");
}

/* ---- 9. mmap, pread and atomic publish through a hard link ---- */
static void test_mapping_and_publish(void)
{
    char staged[1200];
    snprintf(staged, sizeof(staged), "%s/staged.bin", root);
    char body[4096];
    for (int index = 0; index < (int)sizeof(body); ++index) body[index] = (char)(index * 7 + 3);
    CHECK(write_narrow(staged, body, sizeof(body)) == 0, "staged file not written");

    const int fd = openat(AT_FDCWD, staged, O_RDONLY);
    CHECK(fd != -1, "staged open failed: errno %d", errno);
    if (fd != -1) {
        void *mapped = mmap(NULL, sizeof(body), PROT_READ, MAP_PRIVATE, fd, 0);
        CHECK(mapped != MAP_FAILED, "mmap failed: errno %d", errno);
        if (mapped != MAP_FAILED) {
            CHECK(memcmp(mapped, body, sizeof(body)) == 0, "mmap contents differ");
            CHECK(munmap(mapped, sizeof(body)) == 0, "munmap failed");
        }
        char tail[4] = { 0 };
        const off_t where = (off_t)sizeof(body) - 4;
        CHECK(pread(fd, tail, 4, where) == 4, "pread failed: errno %d", errno);
        CHECK(memcmp(tail, body + sizeof(body) - 4, 4) == 0, "pread read the wrong offset");
        struct stat after;
        CHECK(fstat(fd, &after) == 0 && after.st_size == (long long)sizeof(body),
              "pread moved the file position");
        close(fd);
    }

    char published[1200];
    snprintf(published, sizeof(published), "%s/published.bin", root);
    struct stat scratch;
    if (stat(published, &scratch) == 0) unlinkat(AT_FDCWD, published, 0);
    CHECK(link(staged, published) == 0, "zuuned_link failed: errno %d", errno);
    struct stat staged_stat;
    struct stat published_stat;
    CHECK(stat(staged, &staged_stat) == 0 && stat(published, &published_stat) == 0,
          "publish did not leave both names");
    CHECK(published_stat.st_size == (long long)sizeof(body), "published size wrong");
    CHECK(staged_stat.st_nlink == 2 || published_stat.st_nlink == 2, "link is not a hard link");
    CHECK(link(staged, published) == -1, "link overwrote an existing name");
    CHECK(unlinkat(AT_FDCWD, staged, 0) == 0, "unlink staged failed");
    char back[4096];
    CHECK(read_narrow(published, back, sizeof(back)) == (int)sizeof(back), "published read failed");
    CHECK(memcmp(back, body, sizeof(body)) == 0, "published contents differ");
    unlinkat(AT_FDCWD, published, 0);
}
/* ---- 10. strings, line reading, clocks, and the errno contract ---- */
static void test_miscellany(void)
{
    CHECK(strcasecmp("Album.FLAC", "album.flac") == 0, "strcasecmp differs");
    CHECK(strncasecmp("ZuneHD", "zune", 4) == 0, "strncasecmp differs");
    CHECK(strcasestr("The Moonbound Trailer", "MOON") != NULL, "strcasestr missed");
    CHECK(strcasestr("Plain", "moon") == NULL, "strcasestr invented a match");
    char *cut = strndup("podcast-episode.mp3", 8);
    CHECK(cut != NULL && strcmp(cut, "podcast-") == 0, "strndup cut wrong");
    free(cut);

    char lines_path[1200];
    snprintf(lines_path, sizeof(lines_path), "%s/lines.txt", root);
    CHECK(write_narrow(lines_path, "first\nsecond line\nthird", 23) == 0, "lines file not written");
    FILE *stream = fopen(lines_path, "rb");
    if (!stream) { SKIP("fopen cannot see the probe root (non-ASCII temp path)"); }
    else {
        char *line = NULL;
        size_t capacity = 0;
        const ssize_t first = getline(&line, &capacity, stream);
        CHECK(first == 6 && line && memcmp(line, "first", 5) == 0, "getline first line %d", (int)first);
        const ssize_t second = getline(&line, &capacity, stream);
        CHECK(second == 12 && line && memcmp(line, "second line", 11) == 0, "getline second line %d", (int)second);
        const ssize_t third = getline(&line, &capacity, stream);
        CHECK(third == 5 && line && memcmp(line, "third", 5) == 0, "getline tail %d", (int)third);
        const ssize_t fourth = getline(&line, &capacity, stream);
        CHECK(fourth == -1, "getline did not report end of file");
        free(line);
        fclose(stream);
    }
    remove(lines_path);

    struct timespec before;
    struct timespec after;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0, "monotonic clock failed");
    usleep(20000u);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0, "monotonic clock failed twice");
    const long long elapsed = (after.tv_sec - before.tv_sec) * 1000000000LL
                            + (after.tv_nsec - before.tv_nsec);
    CHECK(elapsed >= 15000000LL, "usleep returned early: %lld ns", elapsed);
    CHECK(clock_gettime(CLOCK_REALTIME, &after) == 0, "realtime clock failed");
    CHECK(after.tv_sec > 1700000000L, "realtime clock is not wall time: %ld", (long)after.tv_sec);

    /* Self descriptor duplication stands in for /proc/self/fd/2. */
    const int duplicated = zuuned_open_self_fd(2);
    CHECK(duplicated != -1, "zuuned_open_self_fd(2) failed: errno %d", errno);
    if (duplicated != -1) {
        struct stat self_stat;
        CHECK(fstat(duplicated, &self_stat) == 0, "the duplicated descriptor is not usable");
        close(duplicated);
    }

    /* Real errno, not a bare -1. */
    char missing[1200];
    path_in(missing, sizeof(missing), "no-such-file-anywhere");
    errno = 0;
    CHECK(openat(AT_FDCWD, missing, O_RDONLY) == -1 && errno == ENOENT, "missing file errno %d", errno);
    errno = 0;
    struct stat missing_stat;
    CHECK(lstat(missing, &missing_stat) == -1 && errno == ENOENT, "missing lstat errno %d", errno);
    char here[1200];
    path_in(here, sizeof(here), "exists.bin");
    CHECK(write_narrow(here, "q", 1) == 0, "exists.bin not written");
    errno = 0;
    CHECK(openat(AT_FDCWD, here, O_WRONLY | O_CREAT | O_EXCL) == -1 && errno == EEXIST,
          "O_EXCL errno %d", errno);
    struct stat touched;
    CHECK(stat(here, &touched) == 0, "stat of an existing file failed");
    CHECK(touched.st_mtime > 1700000000L, "st_mtime is not wall time: %ld", (long)touched.st_mtime);
    CHECK(touched.st_ctime > 1700000000L, "st_ctime is not wall time");
    CHECK(touched.st_ino != 0, "st_ino was never filled in");
    CHECK(unlink(here) == 0 || unlinkat(AT_FDCWD, here, 0) == 0, "could not remove exists.bin");
}

/* ---- 11. stdio and file replacement over UTF-8 paths ----
 * fopen(), remove(), unlink() and rename() are _ACRTIMP in the CRT, so the shim
 * claims them with function-like macros instead of a declaration.  Left on the
 * CRT original a UTF-8 path arriving from Qt would be decoded with the ANSI code
 * page and open some other file, which on a zh-CN machine is silent corruption.
 * The proofs therefore run through the wide API, and the rename() check pins the
 * POSIX replace semantics that transcode/id3_rewrite.c publishes with. */
static void test_wide_stdio(void)
{
    /* "zuuned-wide-<Chinese song>.mp3" as explicit UTF-8 bytes. */
    char song[1200];
    snprintf(song, sizeof(song), "%s/zuuned-wide-\xe4\xb8\xad\xe6\x96\x87\xe6\xad\x8c\xe6\x9b\xb2.mp3", root);
    FILE *stream = fopen(song, "wb");
    CHECK(stream != NULL, "fopen on a UTF-8 name failed: errno %d", errno);
    if (!stream) return;
    CHECK(fwrite("zuuned-body", 1, 11, stream) == 11, "fopen write came up short");
    fclose(stream);

    /* The name on disk is the one that was asked for, character for character,
     * and no transliterated cousin appeared next to it. */
    WIN32_FIND_DATAW find;
    wchar_t pattern[2600];
    _snwprintf(pattern, 2600, L"%ls\\zuuned-wide-*.mp3", root_ext);
    const HANDLE handle = FindFirstFileW(pattern, &find);
    CHECK(handle != INVALID_HANDLE_VALUE, "the UTF-8 name is not in the directory");
    int matches = 0;
    wchar_t leaf_seen[264] = { 0 };
    while (handle != INVALID_HANDLE_VALUE) {
        if (matches == 0) wcscpy(leaf_seen, find.cFileName);
        ++matches;
        CHECK(wcscmp(find.cFileName, L"zuuned-wide-\x4e2d\x6587\x6b4c\x66f2.mp3") == 0,
              "fopen wrote a different name: %ls", find.cFileName);
        if (!FindNextFileW(handle, &find)) break;
    }
    if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
    CHECK(matches == 1, "one fopen produced %d files", matches);

    /* Independent oracle: the name that reached the filesystem must be the exact
     * UTF-8 bytes that were asked for.  The expected bytes are assembled from
     * integer values here, so a compiler that folded the literals through the
     * ANSI code page could not certify itself. */
    static const int wanted[] = {
        'z', 'u', 'u', 'n', 'e', 'd', '-', 'w', 'i', 'd', 'e', '-',
        0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87, 0xe6, 0xad, 0x8c, 0xe6, 0x9b, 0xb2,
        '.', 'm', 'p', '3', -1
    };
    char wanted_utf8[64];
    int wanted_length = 0;
    while (wanted[wanted_length] != -1) wanted_utf8[wanted_length] = (char)wanted[wanted_length++];
    wanted_utf8[wanted_length] = 0;
    char leaf_utf8[160] = { 0 };
    const int leaf_bytes = WideCharToMultiByte(CP_UTF8, 0, leaf_seen, -1, leaf_utf8,
                                              (int)sizeof(leaf_utf8), NULL, NULL);
    CHECK(leaf_bytes == wanted_length + 1 && strcmp(leaf_utf8, wanted_utf8) == 0,
          "the file on disk is not the UTF-8 name that was requested");
    wchar_t songw[1200];
    wide_path(song, songw, 1200);
    CHECK(GetFileAttributesW(songw) != INVALID_FILE_ATTRIBUTES, "GetFileAttributesW cannot see the UTF-8 name");

    /* The handle is an ordinary CRT stream: seeking and reading behave. */
    stream = fopen(song, "rb");
    CHECK(stream != NULL, "reopening the UTF-8 name failed: errno %d", errno);
    char back[32] = { 0 };
    if (stream) {
        CHECK(fseek(stream, 0, SEEK_END) == 0, "fseek failed on a shim stream");
        CHECK(ftell(stream) == 11, "ftell after seeking to the end: %ld", ftell(stream));
        CHECK(fseek(stream, 0, SEEK_SET) == 0, "rewinding a shim stream failed");
        CHECK(fread(back, 1, sizeof(back), stream) == 11, "read-back came up short");
        fclose(stream);
    }
    CHECK(memcmp(back, "zuuned-body", 11) == 0, "UTF-8 contents differ");

    /* Append mode survives the mode-string conversion. */
    stream = fopen(song, "ab");
    CHECK(stream != NULL, "append fopen failed: errno %d", errno);
    if (stream) { CHECK(fwrite("xy", 1, 2, stream) == 2, "append write came up short"); fclose(stream); }
    struct stat st;
    CHECK(stat(song, &st) == 0 && st.st_size == 13, "append left size %lld", st.st_size);

    /* rename() replaces an existing destination, which CRT rename() refuses. */
    char from[1200];
    char to[1200];
    snprintf(from, sizeof(from), "%s/zuuned-wide-source.txt", root);
    snprintf(to, sizeof(to), "%s/zuuned-wide-\xe7\x9b\xae\xe6\xa0\x87.txt", root);
    CHECK(write_narrow(from, "moved", 5) == 0, "rename source could not be written");
    CHECK(write_narrow(to, "stale-destination", 17) == 0, "rename destination could not be written");
    errno = 0;
    CHECK(rename(from, to) == 0, "rename over an existing file failed: errno %d", errno);
    CHECK(stat(from, &st) == -1 && errno == ENOENT, "rename left the source behind");
    char moved[32] = { 0 };
    CHECK(read_narrow(to, moved, sizeof(moved)) == 5 && memcmp(moved, "moved", 5) == 0,
          "rename kept the old destination instead of replacing it");

    /* A directory is still not a replacement target, as POSIX requires. */
    wchar_t dirw[2600];
    _snwprintf(dirw, 2600, L"%ls\\zuuned-wide-dir", root_ext);
    if (CreateDirectoryW(dirw, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        char dirpath[1200];
        snprintf(dirpath, sizeof(dirpath), "%s/zuuned-wide-dir", root);
        CHECK(write_narrow(from, "again", 5) == 0, "rename source could not be rewritten");
        CHECK(rename(from, dirpath) == -1, "rename replaced a directory");
        remove(from);
    } else {
        SKIP("could not create the rename destination directory");
    }

    /* remove() and unlink() delete the UTF-8 name itself, with real errno. */
    errno = 0;
    CHECK(remove(to) == 0, "remove on a UTF-8 name failed: errno %d", errno);
    CHECK(stat(to, &st) == -1 && errno == ENOENT, "remove left the UTF-8 name behind");
    wchar_t tow[1200];
    wide_path(to, tow, 1200);
    CHECK(GetFileAttributesW(tow) == INVALID_FILE_ATTRIBUTES, "the wide name survived remove()");
    char gone[1200];
    path_in(gone, sizeof(gone), "zuuned-wide-unlink.txt");
    CHECK(write_narrow(gone, "z", 1) == 0, "unlink fixture could not be written");
    errno = 0;
    CHECK(unlink(gone) == 0, "unlink failed: errno %d", errno);
    CHECK(stat(gone, &st) == -1 && errno == ENOENT, "unlink left the file behind");

    /* A missing UTF-8 name reports ENOENT rather than a bare NULL. */
    char absent[1200];
    /* "missing" in Chinese, spelled with an even number of escape bytes: this
     * compiler reads consecutive \xHH values as double-byte pairs in a code page
     * like 936, so an odd count leaves a dangling lead byte (warning C4819). */
    snprintf(absent, sizeof(absent), "%s/zuuned-wide-\xe7\xbc\xba\xe5\xa4\xb1.mp3", root);
    errno = 0;
    CHECK(fopen(absent, "rb") == NULL && errno == ENOENT, "missing UTF-8 fopen errno %d", errno);

    /* And past the legacy limit the shim supplies the extended prefix itself. */
    wchar_t deep[2600];
    _snwprintf(deep, 2600, L"%ls\\zuuned-wide-deep", root_ext);
    if (!CreateDirectoryW(deep, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        SKIP("could not create the long stdio fixture");
        return;
    }
    char segment[132];
    memset(segment, 'u', sizeof(segment) - 1);
    segment[sizeof(segment) - 1] = 0;
    char deepn[1200];
    snprintf(deepn, sizeof(deepn), "%s/zuuned-wide-deep", root);
    for (int level = 0; level < 3; ++level) {
        wchar_t child[2600];
        _snwprintf(child, 2600, L"%ls\\%hs", deep, segment);
        if (!CreateDirectoryW(child, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) break;
        wcscpy(deep, child);
        snprintf(deepn + strlen(deepn), sizeof(deepn) - strlen(deepn), "/%s", segment);
    }
    char longfile[1400];
    snprintf(longfile, sizeof(longfile), "%s/\xe4\xb8\xad\xe6\x96\x87.mp3", deepn);
    CHECK(strlen(longfile) > 260, "the long stdio fixture is only %u characters", (unsigned)strlen(longfile));
    stream = fopen(longfile, "wb");
    CHECK(stream != NULL, "fopen past 260 characters failed: errno %d", errno);
    if (stream) { CHECK(fwrite("long", 1, 4, stream) == 4, "long write came up short"); fclose(stream); }
    char longback[8] = { 0 };
    stream = fopen(longfile, "rb");
    if (stream) { CHECK(fread(longback, 1, sizeof(longback), stream) == 4, "long read came up short"); fclose(stream); }
    else CHECK(0, "reopening past 260 characters failed: errno %d", errno);
    CHECK(memcmp(longback, "long", 4) == 0, "long-path contents differ");
    CHECK(remove(longfile) == 0, "remove past 260 characters failed: errno %d", errno);
    CHECK(stat(longfile, &st) == -1, "the long stdio file survived remove()");
}

/* ---- 12. st_dev/st_ino carry a real file identity ---- */
/* ArtworkPipeline::stamp() decides "this cache image is its own source" by
 * comparing st_dev and st_ino, and re-reads the source stamp before it commits
 * a render.  Both questions are only meaningful if the shim reports the volume
 * serial and the NTFS file index rather than leaving the pair at zero: with a
 * constant identity any two existing files would look like the same file and
 * every cached artwork render would be refused. */

static void test_file_identity(void)
{
    char first[1200], second[1200], moved[1200];
    path_in(first, sizeof(first), "identity-a.png");
    path_in(second, sizeof(second), "identity-b.png");
    path_in(moved, sizeof(moved), "identity-renamed.png");
    CHECK(write_narrow(first, "aaaa", 4) == 0, "identity fixture A not written");
    CHECK(write_narrow(second, "bb", 2) == 0, "identity fixture B not written");

    struct stat a, again, b;
    CHECK(stat(first, &a) == 0, "identity A did not stat: errno %d", errno);
    CHECK(stat(first, &again) == 0, "identity A did not stat a second time: errno %d", errno);
    CHECK(stat(second, &b) == 0, "identity B did not stat: errno %d", errno);
    CHECK(a.st_size == 4 && b.st_size == 2, "identity fixtures report %lld and %lld",
          a.st_size, b.st_size);
    CHECK(a.st_dev == again.st_dev && a.st_ino == again.st_ino,
          "one file reported two identities");
    CHECK(a.st_ino != 0 && b.st_ino != 0, "st_ino is a placeholder: %llu and %llu",
          (unsigned long long)a.st_ino, (unsigned long long)b.st_ino);
    CHECK(a.st_ino != b.st_ino || a.st_dev != b.st_dev, "two files share one identity");

    /* QSaveFile publishes a render by renaming a temporary over the target, and
     * the stamp comparison straddles that rename, so identity has to survive it. */
    CHECK(rename(first, moved) == 0, "identity rename failed: errno %d", errno);
    struct stat after;
    CHECK(stat(moved, &after) == 0, "renamed identity did not stat: errno %d", errno);
    CHECK(after.st_ino == a.st_ino && after.st_dev == a.st_dev,
          "identity changed across rename");
    CHECK(stat(first, &after) == -1 && errno == ENOENT, "the rename left the old name behind");
    unlink(second);
    unlink(moved);
}

/* ---- 13. a shell-out gets bytes the shell can read back ---- */
/* system() runs cmd.exe here, and cmd.exe decodes its command line with the
 * active ANSI code page while the portable sources hand UTF-8, so a name outside
 * ASCII reaches ffmpeg as mojibake and the caller falls back without noticing.
 * zuuned_narrow_path_for_shell() is the one place that re-spells it, and the
 * expectation below is asked of Windows rather than guessed, because a code page
 * that cannot spell a name has to produce NULL instead of a substitute byte. */
/* Encode a wide name the way a UTF-8 caller would hand it over. */
static void wide_to_utf8(const wchar_t *wide, char *out, int cap)
{
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, NULL, NULL) <= 0) out[0] = 0;
}

/* zuuned_narrow_path_for_shell(): the bytes cmd.exe reads back as this file. */
static void test_shell_bytes(void)
{
    char *plain = zuuned_narrow_path_for_shell("D:\\Music\\Song.mp3");
    CHECK(plain && strcmp(plain, "D:\\Music\\Song.mp3") == 0,
          "an ASCII path did not come back as it went in: %s", plain ? plain : "(null)");
    free(plain);

    /* Bytes that are not valid UTF-8 arrived in the code page already, and
     * reading them as UTF-8 would invent different characters.  The fixture is
     * two literals so \xff stays one byte: MSVC reads "\xffb" as one escape. */
    char *legacy_path = "D:\\Music\\a\xff" "b.mp3";
    char *legacy = zuuned_narrow_path_for_shell(legacy_path);
    CHECK(legacy && strcmp(legacy, legacy_path) == 0,
          "code-page bytes were not passed through: %s", legacy ? legacy : "(null)");
    free(legacy);

    /* The rest is agreement with Windows itself.  Whichever of these characters
     * this machine's code page can spell has to come back as the exact bytes it
     * spells it with, and whichever it cannot has to come back NULL rather than
     * the '?' substitute - a substituted byte names a different file, which
     * reads back as a missing picture.  Which candidate is which depends on the
     * active code page, and it is not the one guessed: GBK spells the private
     * use U+E000 that cp1252 cannot, so the expectation is asked of Windows. */
    static const wchar_t candidates[] = { 0x9753, 0x00E9, 0x0416, 0x3042,
                                          0xE000, 0x0700, 0x16A0, 0 };
    int spellable = 0, unspellable = 0;
    for (const wchar_t *cand = candidates; *cand; ++cand) {
        wchar_t wide[40];
        int n = 0;
        wide[n++] = L'D'; wide[n++] = L':'; wide[n++] = L'\\'; wide[n++] = L'a';
        wide[n++] = *cand; wide[n++] = L'.'; wide[n++] = L'j'; wide[n++] = L'p';
        wide[n++] = L'g'; wide[n] = 0;
        char expected[32];
        BOOL used_default = FALSE;
        const int needed = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1,
                                               expected, (int)sizeof(expected),
                                               NULL, &used_default);
        char utf8[64];
        wide_to_utf8(wide, utf8, (int)sizeof(utf8));
        CHECK(utf8[0] != 0, "the probe could not encode U+%04X as UTF-8", (unsigned)*cand);
        char *narrow = zuuned_narrow_path_for_shell(utf8);
        if (needed > 0 && !used_default) {
            spellable++;
            CHECK(narrow && strcmp(narrow, expected) == 0,
                  "U+%04X narrowed to \"%s\" rather than the code page's \"%s\"",
                  (unsigned)*cand, narrow ? narrow : "(null)", expected);
        } else {
            unspellable++;
            CHECK(narrow == NULL,
                  "U+%04X has no spelling in this code page yet came back \"%s\"",
                  (unsigned)*cand, narrow ? narrow : "(null)");
        }
        free(narrow);
    }
    CHECK(spellable > 0, "no candidate character is spellable in this code page");
    if (unspellable == 0)
        SKIP("every candidate character is spellable in this machine's ANSI code page");
}

/* Which channel the process really owns.  zuuned.exe is a GUI-subsystem image
 * now, so a double-clicked app has no stdio at all until --debug asks for it
 * back, and the one rule that makes that safe is that a stream somebody is
 * already reading never gets replaced underneath them.  This probe is always
 * launched with both streams redirected - the gate reads the lines below off a
 * pipe - so the honest answers here are 1 and then 0: the launcher owns the
 * output, and zuuned_win_debug_console() has to see that and decline.  What is
 * deliberately left unexercised is the other branch.  A process handed no
 * handles at all really does get a console window, and that is the app's own
 * double-click, measured by launching the built executable; opening one from in
 * here would only steal the harness that is reporting this probe. */
static void test_output_channel(void)
{
    CHECK(zuuned_win_output_available() == 1,
          "a probe with both streams redirected claims it has nowhere to print");
    CHECK(zuuned_win_debug_console() == 0,
          "zuuned_win_debug_console() took the console from a launcher that redirected it");
    CHECK(zuuned_win_output_available() == 1,
          "the stream this probe prints on changed underneath it");
}

int main(void)
{
    zuuned_win_runtime_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Undo what the runtime init above installs.  Everything from here on has to
     * reinstate the guard on its own for the descriptor checks to be survivable,
     * which is the contract the tools depend on. */
    _set_invalid_parameter_handler(NULL);

    const char *tmp = zuuned_win_tmpdir();
    if (!tmp || !*tmp) { printf("FAIL no temporary directory\n"); return 1; }
    char base[1000];
    snprintf(base, sizeof(base), "%s", tmp);
    size_t base_length = strlen(base);
    while (base_length && (base[base_length - 1] == '/' || base[base_length - 1] == '\\')) base[--base_length] = 0;
    snprintf(root, sizeof(root), "%s/zuuned-compat-probe-%lu", base, (unsigned long)GetCurrentProcessId());
    to_wide(root, rootw, 1280);
    for (wchar_t *slash = rootw; *slash; ++slash) if (*slash == L'/') *slash = L'\\';
    _snwprintf(root_ext, 1300, L"\\\\?\\%ls", rootw);
    if (root_ext[4] == 0 || !CreateDirectoryW(root_ext, NULL)) {
        printf("FAIL cannot create the probe root: %ls\n", root_ext);
        return 1;
    }
    printf("probe root: %s\n", root);

    printf("--- tmpdir\n");
    test_tmpdir();
    printf("--- mkstemps\n");
    test_mkstemps();
    printf("--- utf8_name\n");
    test_utf8_name();
    printf("--- long_path\n");
    test_long_path();
    printf("--- large_file\n");
    test_large_file();
    printf("--- directory_descriptor\n");
    test_directory_descriptor();
    printf("--- opendir\n");
    test_opendir();
    printf("--- pipes\n");
    test_pipes();
    printf("--- mapping_and_publish\n");
    test_mapping_and_publish();
    printf("--- miscellany\n");
    test_miscellany();
    printf("--- wide_stdio\n");
    test_wide_stdio();
    printf("--- file_identity\n");
    test_file_identity();
    printf("--- shell_bytes\n");
    test_shell_bytes();
    printf("--- output_channel\n");
    test_output_channel();

    sweep(root_ext);
    printf("%d checks, %d failures, %d skips\n", checks, failures, skips);
    return failures ? 1 : 0;
}
