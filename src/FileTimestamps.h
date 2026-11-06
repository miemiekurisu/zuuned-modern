#pragma once

// One spelling for the sub-second timestamps struct stat carries.
//
// Linux names them st_mtim and st_ctim.  macOS names the same two values
// st_mtimespec and st_ctimespec and has no st_mtim at all, so a shared source
// file that writes st_mtim does not compile here — it is not a wrong value, it
// is a missing member (measured: "no member named 'st_mtim' in 'struct
// stat'").  Windows already carries st_mtim and st_ctim because
// compat/win/posix_shim.h defines that struct itself, so only this platform
// needs the second spelling.
//
// The names are not the only difference worth knowing about.  APFS on Apple
// silicon volumes is case-insensitive by default and its st_ctime is an inode
// change time like Linux's, so ctimeNs keeps the same meaning it has on Linux:
// the file's metadata changed, not its contents were created.
//
// Header-only and inline for the reason src/ProfilePaths.h gives: some of these
// translation units are also compiled stand-alone by gate scripts, and a second
// object file for three field reads would buy nothing.

#include <QtGlobal>

#include <sys/stat.h>

namespace Zuuned {
namespace FileTimestamps {

inline ::timespec mtime(const struct stat &info) {
#ifdef __APPLE__
    return info.st_mtimespec;
#else
    return info.st_mtim;
#endif
}

inline ::timespec ctime(const struct stat &info) {
#ifdef __APPLE__
    return info.st_ctimespec;
#else
    return info.st_ctim;
#endif
}

inline ::time_t mtimeSec(const struct stat &info) { return mtime(info).tv_sec; }

// Seconds and nanoseconds folded into one integer, which is how the library
// stores a fingerprint and how the artwork cache stamps a file.  A nanosecond
// field is below 1e9 by definition, so two files whose mtimeNs and ctimeNs both
// match had both fields match; comparing the pair of composites is the same
// test as comparing four fields, just shorter.
inline qint64 mtimeNs(const struct stat &info) {
    const ::timespec value = mtime(info);
    return qint64(value.tv_sec) * 1000000000LL + value.tv_nsec;
}

inline qint64 ctimeNs(const struct stat &info) {
    const ::timespec value = ctime(info);
    return qint64(value.tv_sec) * 1000000000LL + value.tv_nsec;
}

}  // namespace FileTimestamps
}  // namespace Zuuned
