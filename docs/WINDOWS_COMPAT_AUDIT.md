# Windows path and encoding compatibility audit

Dated 2026-10-21.  A whole-tree sweep for the Windows-only ways this application
can mis-spell a path or a byte string, done after three symptoms appeared on a
real Zune HD that Linux had never shown: an upload answered `Fail to save`, the
music root displayed as `/E:/Temp/testmusic`, and Settings reported
`watch folder unreachable` for a folder the user was looking at in Explorer.

All three were one defect.  None of them was a protocol defect: libzune was
handed a path that does not exist on Windows and reported that faithfully.  The
protocol, the driver and the device were right the whole time, which is worth
saying because every one of those symptoms reads like a device bug.

## The one defect: a path is not a URL

Qt's folder and file pickers hand QML a `QUrl`.  The UI stripped the scheme with
string arithmetic - `url.toString().replace("file://", "")` and friends - and
that is correct by luck on Linux and wrong on Windows:

| spelling | on Linux | on Windows |
| --- | --- | --- |
| `file:///home/user/Music` -> drop `file://` | `/home/user/Music`, absolute | - |
| `file:///E:/Temp/testmusic` -> drop `file://` | - | `/E:/Temp/testmusic` |

`/E:/Temp/testmusic` is not absolute.  It is a path relative to the current
directory of drive E:, so `QFileInfo::exists()` is false, `QDir::entryList()` is
empty, and every consequence follows mechanically: the scan indexes nothing, the
health check calls the folder unreachable on every pass, the row shows up with
Unix-looking slashes in the UI, and a transfer that resolves its destination
under that root cannot create the file - `Fail to save`.

`QUrl::toLocalFile()` is the decoder that gets both platforms right: it
percent-decodes and drops the leading slash exactly when a drive letter follows
it.  Hand-stripping does neither.

## The rule now: one spelling, three forms

`src/library/LibraryPaths.h` owns the conversion.  Header-only and QtCore-only,
so `LibraryDb` and the command-line `librarytool` use it without pulling QtQml
into a tool.

- **canonical** - `LibraryPaths::canonicalRoot(raw)`: storage and comparison
  form.  Decodes a `file:` URL, converts backslashes with
  `QDir::fromNativeSeparators()`, repairs an existing `/E:/` scar, and folds the
  drive letter to upper case.  This is what goes into `watch_folders.path`.
- **url** - `LibraryPaths::fileUrl(path)`: what `Image.source` and friends need.
  Wraps `QUrl::fromLocalFile()`, which emits `file:///E:/...` and escapes spaces
  and non-ASCII.  Never stored.
- **display** - `PathText::display(path)`: what a human reads.  Windows shows
  `E:\Temp\testmusic`; nothing is stored from it.

`src/PathText.h` is the QML-facing side: `canonical()` and `url()` delegate to
`LibraryPaths`, `display()` stays presentation-only.  Both
`canonicalRoot()` steps are the identity on Linux, so an existing Linux database
- and a backslash inside a Linux filename, where it is an ordinary character -
is byte-for-byte unchanged.

Entry points that now go through the rule, every one of them previously doing its
own string arithmetic:

- `qml/SettingsPage.qml:1616`, `qml/SettingsPage.qml:1675`,
  `qml/OnboardingView.qml:1854`, `qml/OnboardingView.qml:1867` - folder and file
  pickers storing a watch folder or a wallpaper.
- `qml/LibraryPhotosView.qml:234` - a device photo saved into the library.
- `qml/EnvironmentBackground.qml:51`, `qml/OnboardingView.qml:700`,
  `qml/SettingsPage.qml:455`, `qml/LibraryVideosView.qml:198`,
  `qml/QueueEntryView.qml:147` and `qml/QueueEntryView.qml:148` - the five places
  that built a URL by concatenating `"file://"` in front of a stored path.
- `src/LibraryService.cpp:772` and `src/LibraryService.cpp:785` - the C++ add and
  reload boundary, so a path arriving from anywhere is canonical before it is
  compared with a stored one.
- `src/LibraryService.cpp:2268` and `src/LibraryService.cpp:1090` - the health-check
  detail line and the `displayPath` each Settings row reads, so a folder appears the
  way the user names it and the stored form never leaks into a label.
- `qml/SettingsPage.qml:761`, `qml/SettingsPage.qml:600`,
  `qml/OnboardingView.qml:1822`, `qml/SpokenFolderImport.qml:61` and
  `qml/SpokenFolderImport.qml:78` - every label that prints a stored path, so a
  Windows user reads backslashes and a Linux user reads what was always there.
- `qml/OnboardingView.qml:1511` and `qml/SettingsPage.qml:852` - the typed-path
  placeholders, which suggested a Linux mount path to someone on Windows.

## Repair, because the scar is already in real databases

`LibraryDb::open()` calls `repairWatchFolderRoots()`
(`src/library/LibraryDb.cpp:410`) on every launch.  For each `watch_folders` row
whose canonical spelling differs from the stored one:

- if another row already holds the repaired spelling, this row is deleted - the
  `UNIQUE(path)` constraint would refuse the update, and the duplicate is exactly
  what the user just recreated by re-picking the folder;
- otherwise the path is updated in place.

Repairing the string is the whole job because a mangled root can never have
produced media - the scanner asked the OS whether it existed and the answer was
no - so there is nothing underneath it whose prefix has to follow.  Live
evidence, first launch after the fix:

```
[librarydb] repaired watch folder /E:/Temp/testmusic to E:/Temp/testmusic
```

## Why the drive letter is folded to upper case

Windows paths are case-insensitive; `watch_folders` dedupes with a byte
`UNIQUE(path)`.  `e:/Music` and `E:/Music` would therefore be two identities for
one folder: a track scanned under one never matches a file resolved under the
other, each row keeps its own unreachable state, and no health pass or watcher
can retire either.  The drive letter is the one component where folding is free.

This is deliberately narrow.  Full case-insensitive dedup of the rest of the path
is **not** done: a SQLite collation change would also make the `substr` prefix
matching the purge and stale-sweep paths case-insensitive on Linux, where case is
meaningful.  Two folders differing only in letter case are a user choice, and
both remain legal rows.

## Second sweep: a name the drive can hold, and a shell that is not sh

With the library root repaired the app scanned, browsed and synced, which moved the
symptoms somewhere else.  A second whole-tree pass looked at the other direction -
not a path the app *reads*, but a name the app *writes* and a command the app *runs*.
Three defects came out of it.  All three are found by reading; none of them was
reproduced on the Zune HD at hand, so each one below says what it would cost rather
than claiming a fixed failure.

**A device tag is not necessarily a legal Windows filename.**  An artist, album or
photo title is arbitrary text that came off another device: `P:44 / Vol. 2`,
`what?` or `a<b>` is an ordinary album name and an impossible Windows path
component.  Every save-to-library site sanitised device text for `/` alone, so
`QDir::mkpath()` returned false and the transfer answered `Fail to save` for a track
with nothing wrong with it - the same message the root-path defect produced, from an
unrelated cause, which is worth remembering the next time that string appears.

`LibraryPaths::safeComponent(name, fallback)` (`src/library/LibraryPaths.h:148`) is
now the one answer, applied at `src/DeviceService.cpp:797`,
`src/DeviceService.cpp:829`, `src/DeviceService.cpp:835`, `src/DeviceService.cpp:849`
and `src/DeviceService.cpp:854` - video, photo (subfolder *and* raw filename) and
track (artist/album *and* filename), which is every place device text becomes a file
name:

- Windows: the shell's illegal set `<>:"/\|?*` and the control characters
  `\x00-\x1f` become `_`; trailing dots and spaces are stripped because the shell
  hides them; a component spelling a reserved stem (`CON`, `PRN`, `AUX`, `CLOCK$`,
  `NUL`, `COM0-9`, `LPT0-9`) takes a `_` prefix, because those names belong to the
  namespace and a file called `NUL.mp3` cannot be reopened by path; a component that
  comes out empty, `.` or `..` takes the caller's fallback instead.
- Linux: `/` becomes `_` and nothing else changes, which is what the app did before,
  so an existing Linux library keeps the folder names it already has.

`LibraryPaths::caseKey()` (`src/library/LibraryPaths.h:96`) ships with it - fold
case on Windows, identity on POSIX - for anywhere a stored `filepath` meets a path
that arrived from outside.

**`system()` on Windows is `cmd.exe`, and `cmd.exe` does not speak sh.**
`zune_grab_photo_thumb()` (`libzune/src/thumbnail.c`) has an ffmpeg fallback that
builds a command line.  It quoted both paths with single quotes and redirected with
`2>/dev/null`.  `cmd.exe` does not treat `'` as a quote character and takes
`2>/dev/null` as "redirect stderr to a file named dev/null", so on Windows that
branch failed on every single call whatever it was pointed at, and did so silently -
the return code was checked, the reason was not.  It was given a `_WIN32` branch:
double quotes with each inner quote doubled by a helper that refused to emit anything
rather than emit a truncated command line, and `>nul 2>&1`.  The Linux and macOS
command strings stayed byte-for-byte what they were.  **Superseded on 2026-11-17, and
read it as what was found then:** doubling quotes cannot protect a command line that a
shell expands `%VAR%` inside the quotes of, `shell_quote_path()` is deleted, and that
branch starts ffmpeg with argv and no shell at all - see the 2026-11-17 section at the
end of this file, which is also the first time this branch is known to have ever worked
on Windows.

**An m3u line is a claim about a file, not a string equal to a `filepath`.**
`LibraryService::importPlaylistM3U()` (`src/LibraryService.cpp:2220`) compared each
entry verbatim against the database.  That holds only for a playlist this app itself
wrote, on Linux, with absolute paths.  Windows Media Player writes
`E:\Music\A.mp3`, iTunes writes `file://` URLs, and a hand-made or foreign-exported
playlist lists files relative to itself; compared as text, all three come back
"missing" and the user is handed an empty playlist out of a library that contains
every track on it.  Relative entries are wrong on Linux too, so the resolution is
unconditional rather than Windows-branched: decode a URL, take
`QDir::fromNativeSeparators()` (a no-op on Linux, where a backslash is an ordinary
character), resolve a relative entry against the playlist's own folder, then
`QDir::cleanPath()` - the form `LibraryScanner` stores - and compare through
`caseKey()` on both sides.

## Third sweep: what a playlist is written in, and a name nobody can reopen

The second sweep left two holes of its own making and one claim it had overstated.
All three came from re-reading that diff a day later rather than from a new symptom,
which is the check worth running on your own fix the day after it ships.

**Deciding what an m3u line *means* was half the job; deciding what its bytes *are* was
the other half.**  The fix above still read every line with `QString::fromUtf8()`.  m3u
declares no encoding and carries no magic.  This app writes UTF-8 without a BOM, so its
own exports were always fine; a playlist the user points at usually came from somebody
else, and on Windows somebody else defaults to the ANSI code page - Notepad's "ANSI"
save, Windows Media Player exports, most shareware converters - which is CP1252 in the
west and CP936 here.  Run through `fromUtf8()` those bytes arrive as U+FFFD runs, no
entry matches a row, and the import answers "0 added, 41 missing" for a playlist naming
forty-one songs the library plainly holds.  That is the same symptom as the resolution
defect, from an unrelated cause, one function deep.  A BOM is the milder instance of the
same carelessness: Notepad, PowerShell and Visual Studio all add one to a UTF-8 file, and
it corrupts exactly one entry - the first - which reads like a bug in the matching code
and not in the decoding.

`PlaylistText::decode()` (`src/library/PlaylistText.h:39`) owns that decision now: honour
a BOM, take UTF-8 when the bytes really are UTF-8, otherwise fall back to the locale's own
code page, which is precisely what "no BOM and not valid UTF-8" means on the machine that
wrote the file.  The UTF-8 test is `fromUtf8()` followed by the round trip: Qt replaces an
invalid sequence with U+FFFD, which re-encodes to `EF BF BD` and so never reproduces the
input, making the comparison evidence rather than a tautology.  The resolution moved out
of the lambda into `LibraryPaths::resolveEntry()` (`src/library/LibraryPaths.h:179`),
beside the helpers it shares with the scanner, so a command line can reach both; the import
is now decode, split, resolve, `caseKey()` (`src/LibraryService.cpp:2264`,
`src/LibraryService.cpp:2270`).

**`safeFilePart()` did not in fact "already apply this shape".**  The comment in
`src/library/LibraryPaths.h` said so of the reserved-stem rule, and it was inexact:
`VideoIdentity::safeFilePart` applied the illegal set and the trailing dot, and not the
device namespace.  So a Zune holding an object named `NUL.wmv` still imported to a file
that extraction reports as saved and nothing can afterwards open - the write succeeds, the
read fails, and the file sits there in Explorer.  The rule is now one function,
`LibraryPaths::isReservedWindowsName()` (`src/library/LibraryPaths.h:115`), called from
`safeComponent()` (`src/library/LibraryPaths.h:148`) and from `safeFilePart()`
(`src/library/VideoIdentity.h:50`).  It answers false on POSIX, where `NUL.wmv` is an
ordinary filename and the import does exactly what it did before.

**None of the second sweep's helpers had a test, and neither gate could have given them
one.**  `tests/win-compat` is deliberately Qt-free - it compiles the shim with the MSVC C
compiler and nothing else - so it cannot reach a C++ header, and the bash suites in
`ci/check.sh` do not run here.  The helpers are header-only, run on every save, and fail by
printing "Fail to save", the least diagnosable message the app has.  `librarytool paths`
(`src/library/librarytool.cpp:319`) is their home now, in the shape the file already used
for `names` and `customize`: 28 checks, with the expectation branched `#ifdef Q_OS_WIN`
wherever the right answer genuinely differs, so one source gates both platforms and a
Linux run is told in as many words that `NUL` stays `NUL`, that a backslash stays an
ordinary filename character, and that `caseKey()` folds nothing there.  Both gates still
reach the headers only through the compiled tool, so `paths` now runs from the usability
gate's tools phase beside `names` and `customize` rather than being a command that has to
be remembered.

## Console output

The console renders the active ANSI code page, so a gate that prints a Hebrew or
emoji path reads its own output as `?`.  The application already fixes this through
`zuuned_win_runtime_init()` (`src/main.cpp:50`).  `librarytool` did not, and on a
CP936 host its scan output was unreadable for precisely the folders worth
testing; it now calls `zuuned_win_console_utf8()` directly
(`src/library/librarytool.cpp:311`).

`devicetool`, `spokenmediatool` and `playertool` now put their console in UTF-8
as well, each calling `zuuned_win_console_utf8()` at the top of its own `main()`.
None of the three uses the POSIX surface, so each declares the helper itself
rather than forcing `compat/win/wincompat.h` through the Qt and vendor headers,
and `CMakeLists.txt` links those three targets against the compat objects.

## Diagnostics print UTF-8, not the ANSI code page

`qPrintable()` is `toLocal8Bit()`.  On a CP936 host every `[sync]`,
`[sync-canon]` and `[worker]` line naming a Chinese path therefore arrived in the
log as U+FFFD while the C layer beside it, which is handed `toUtf8()`, printed the
same path correctly.  The mojibake is the smaller half of the problem: the channel
an operator reads to find out why a save failed was naming a path that does not
exist on disk, for exactly the roots worth testing.  Everything under `src/` now
uses `qUtf8Printable()`, which is the same bytes on Linux.  The one call among
them that is not logging is mpv's `alang` option (`src/player/MpvController.cpp:53`),
where UTF-8 is what mpv expects anyway.  The harnesses under `tests/` and `tools/`
keep `qPrintable()` - their lines are assertion text, not user paths.

## Checked and cleared - do not re-litigate these

Every narrow-path and hand-rolled-URL site in the tree was read, not grepped for a
pattern.  These look like defects and are not:

- `src/diagnostics/SupportReport.cpp:113` - `QFile::encodeName()` plus CRT
  `access()`, in the `#else` of `#if defined(Q_OS_WIN)`.  Local 8-bit is right
  there: that branch is the Linux one, and the paths are ASCII sysfs entries.
- `src/DeviceService.cpp:895` `udevRulesApply()` - returns false on Windows and
  the access card then names WinUSB and Zadig instead of a rule file.  The udev
  chain is Linux-only by design and never runs here.  `src/DeviceWorker.cpp:203`
  was the one leftover that still told a Windows user to check a udev rule when no
  device answered; it now names the WinUSB binding.
- `LibraryService::ensureGvfsMounts()` - its regex anchors on `/run/user/<uid>/gvfs/`,
  which no Windows path can match, so it is inert rather than wrong.
- Picker-to-C++ URLs (`qml/CustomizeSheet.qml:1273`, `qml/SpokenFolderImport.qml:108`,
  `qml/SettingsPage.qml:1686`, `qml/SpokenCoverSheet.qml:88`) - the C++ side calls
  `QUrl::toLocalFile()`.  Correct all along; only the QML-side hand-stripping was
  not.
- `compat/win/wincompat.c` - decodes UTF-8 with `MB_ERR_INVALID_CHARS` first and
  the ANSI code page second, so both byte conventions arrive intact.  Its
  invalid-parameter handler installs lazily, so tools need no runtime init for it.
- `src/artwork/ArtworkPipeline.cpp:56` `stamp()` compares `st_dev` and `st_ino` to
  refuse a cache image that is its own source.  That is sound here, not a POSIX
  leftover: `fill_stat_from_handle()` (`compat/win/wincompat.c:261`) fills the pair
  from the volume serial and the NTFS file index, and the `file_identity` phase of
  `tests/win-compat` pins it - two files never share an identity, neither identity
  is zero, and it survives the rename `QSaveFile` publishes with.
- The remaining `file://` mentions under `src/` and `qml/` all go through
  `QUrl::toLocalFile()` (`src/LibraryService.cpp:1633`, `src/LibraryService.cpp:1942`,
  `src/spoken/SpokenMediaService.cpp:33` and the rest of them), which is the decoder
  that gets a drive letter right.  Only the hand-stripping was ever wrong.
- `QFile::rename()` at `src/library/LibraryDb.cpp:362` has no POSIX replace semantics
  on Windows, and does not need them: it is guarded by `!QFile::exists(backup)` in
  the same statement.
- `QStandardPaths` Music/Pictures/Movies and the `QLockFile` in RuntimeLocation
  resolve to real Windows locations; nothing in the tree hardcodes a `$HOME` subpath
  for the app's own data.
- `QT_QPA_PLATFORMTHEME=xdgdesktopportal` is inside `#if !defined(Q_OS_WIN)`
  (`src/main.cpp:89`), so a Windows run never asks for a portal that is not there.
- `ci/check.sh` and everything under `packaging/` is Linux-only by construction, and
  there is no CI workflow to run it - `.github/` holds issue templates only.

## Encoding: `QFile::encodeName()` is not UTF-8

Four call sites handed the POSIX surface Qt's local 8-bit codec, which on Windows
is the ANSI code page.  Measured on this CP936 machine across five scan roots -
ASCII, Chinese, Hebrew, Hangul, emoji - the scanner found its track under the
ASCII and Chinese roots and nothing under the other three: `stat()` failed, the
copy fingerprint returned false, and the scan exited 0 looking complete.  CP936
covers Han, so a Chinese host cannot see this defect; an English host meets it on
its first Hebrew album.  `LibraryScanner`, `ArtworkPipeline`, `ArtistImageService`
and `OnlineAlbumArtService` now pass `toUtf8()` - the same bytes on Linux.  Full
account in `BUILDING.md` under *Path encodings on a non-English Windows*.

## Fourth sweep: a watch folder that contains itself

The path work closed the `E:/Temp/testmusic` report and the report came back: the folder
scanned, and the library filled with copies of itself.  The cause was not spelling.  A
Windows junction — `mklink /J`, the link type that needs neither elevation nor developer
mode — is an ordinary directory to `QFileInfo`: `isSymLink()` is false and the only
attribute it carries is `FILE_ATTRIBUTE_DIRECTORY`.  The scanner's entire cycle guard was
that one call, so a music folder holding a link back into one of its own ancestors looked
like music, and the recursive `QDirIterator` walked into it.

Measured against `music/loop -> music` on Qt 6.11: 160 entries, 96 folders, not one of
them a link as far as Qt was concerned, 41 levels deep at the point the walk gave up
because the path had reached 260 characters.  Nothing hung and nothing failed.  The scan
returned success, reported a complete walk, and had handed in the same handful of songs
under 41 different paths, which on a real library is one duplicate album per level written
into SQLite.  A naive `GetFileSystemEntries` recursion over the fixture the gate now uses
counts 192 entries; three songs is what the walk has to produce.

The two answers that look obvious are not answers:

- `QDirIterator::FollowSymlinks` does detect loops, and Qt documents that it does, but
  that detection lives in the mode that follows links.  The default mode never descends a
  POSIX symlink so it never needed one, and a junction is not a link as far as Qt is
  concerned, so the flag changes nothing about the case that broke.
- A depth or entry cap turns a wrong answer into a truncated one, and a watch folder that
  stops early still certifies every folder it never read — which is precisely the signal
  the music phase uses to decide whether a stored probe failure has genuinely gone away.

So every folder in the walk got an identity.  `LibraryScan::walkFiles()` is a depth-first
walk with an explicit stack, keyed on the `(st_dev, st_ino)` pair that
`accessibleDirectory()` gets from `compat/win/wincompat.c:261`: a volume serial number and
the 64-bit NTFS file index, the same pair the artwork cache identity check already leans
on.  Opening a junction resolves it, so a link to an ancestor answers with the ancestor's
identity and cannot pass for somewhere new.  A filesystem that reports zero in both fields
is walked anyway, without the once-only guarantee, because it could not offer that
guarantee before this change either.  Music, video and photos all go through the one walk,
so a future media type cannot skip the guard by forgetting to ask for it.

Three consequences are worth pinning down because none of them is visible in the diff:

- A file reached through a link reports the link's path, not the target's.  A junction
  pointing out of the music folder at songs that are not inside it contributes those songs
  once, as `music/<link>/Song.mp3`.  That was already true of junctions before this
  change; it is now asserted in both directions.
- A folder that cannot be opened makes `complete` false and the music phase stops
  certifying that root, so a hole in a walk cannot be read back as "this probe failure is
  gone".
- `MAX_PATH` still bounds the walk, because `zuuned.exe` ships no `longPathAware` entry
  (see the manifest entry below).  Depth 41 is where the loop used to stop, and it is also
  where a genuinely 41-level album tree still stops.  Same ceiling, different fix.

## Fifth sweep: the manifest a process opts out of `MAX_PATH` with, and two names

Nothing new broke. This sweep closed three findings the earlier sweeps wrote down and left
open on purpose, on the theory that a recorded gap is only honest until somebody has the
afternoon.

**`MAX_PATH` is opted out of in the executable's own manifest, and nowhere else.**
`FileSystem\LongPathsEnabled` is necessary and not sufficient: the file APIs lift the
260-character ceiling for an image that declares `longPathAware`, and for no other.  Nothing
built here declared it, so a machine whose owner had already enabled long paths - and expects
that to mean something - still held songs in Explorer that the library never saw.
`packaging/windows/zuuned.manifest` declares it, and `zuuned_long_paths()`
(`CMakeLists.txt:166`) gives it to all six executables rather than to the app alone: the
library CLI, the transcode CLI and the app reach the same folders through the same C runtime,
so one of them declaring it is not the same as all of them doing so.

Two facts about that link, each bought with a build that did not happen.

- `/MANIFEST:EMBED /MANIFESTINPUT:` on its own does not work with CMake, and the failure
  does not look related to the cause.  CMake writes `CMakeFiles\<tgt>.dir\embed.manifest`,
  `cvtres` then meets two manifests on resource ID 1, and the link dies with `CVT1100
  duplicate resource MANIFEST` followed by `LNK1123`.  Dropping a foreign `*.manifest` in the
  intermediate directory does nothing: CMake's `vs_link_exe` mt step does not see it, so
  there is no manifest to append to.  `/MANIFEST:NO` is the switch CMake reads *before* it
  generates one, which is why the file in `packaging/windows/` is a whole manifest - the
  `asInvoker` `trustInfo` and `uiAccess="false"` included - and not a fragment.  It follows
  that nothing else in the build describes the manifest any more, so all six images are read
  back after the link with `mt.exe -inputresource:"build\zuuned.exe;#1" -out:manifest.xml`:
  one manifest each, `asInvoker`, `longPathAware=true`.
- Whether the declaration is load-bearing was measured, and the obvious experiment answers
  the wrong question.  The `deep:` checks in `librarytool paths` build a fixture past 260
  characters; on this machine `QFile`, `QDir` and `LibraryScan::walkFiles()` each reach it
  **with the declaration absent**.  What fails without it is `deep crt:`, which writes that
  same file with `_wfopen()` and hands kernel32 the path unchanged.  The C runtime is where
  FFmpeg, LAME and SQLite live, and a transcode temporary is exactly the deep file a sync
  writes mid-transfer.  A gate written only against Qt would have concluded that the manifest
  buys nothing.
- `deep crt:` runs *inside* the declared binary, so it is the regression guard and not the
  proof.  The proof is a pair outside the tree: two copies of a 36-line C probe, one linked
  with this manifest and one with the default, run against the same 356-character file that
  the declared one had just created, on a machine reporting `LongPathsEnabled` = 1.  The
  undeclared binary answers `_wfopen()` with `ERROR_PATH_NOT_FOUND` (3) and `errno 2`; the
  declared one writes it and reads `z` back.  Same path, same file, same registry - the only
  difference is the resource the linker embedded.

**A playlist called `NUL` exported to a file nobody can reopen, and `Mixtape.` trampled
`Mixtape `.**  `LibraryService::exportPlaylistsM3U()` carried a shorter rule than the rest of
the app: it replaced the eight characters it knew about and let a trailing dot or space
stand, which Explorer drops, so two playlists wrote onto one `.m3u8` and one of them quietly
won.  `LibraryPaths::playlistFileName()` (`src/library/LibraryPaths.h:182`) is that rule with
both holes closed, and it takes `safeComponent()` on Windows only.  POSIX keeps the old
inline set spelled out in the same function, because a colon is a character in a Linux
filename and renaming a year of somebody's existing exports to fix a bug that platform cannot
have is not a fix; the `#else` branch of the gate says exactly that.

**UTF-16 m3u is decoded.**  `FF FE` and `FE FF` are honoured in `PlaylistText::decode()`
(`src/library/PlaylistText.h:72`), ahead of the UTF-8 validity test - the only order that
works, since `FE FF` inside a longer file is nobody's signal and `fromLocal8Bit()` turns
either BOM into two letters of the active code page and calls it a filename.  A big-endian
body is swapped byte by byte rather than entrusted to `QString::fromUtf16()`, whose reading
depends on the order the build happens to use.  Still unread, deliberately: UTF-16 with no
BOM, which cannot be told apart from a byte pair that happens to look like text, and UTF-32,
which no playlist writer in the wild produces.

`librarytool paths` is now 52 checks, up from 39: three `playlistFileName:`, three UTF-16
`decode:`, five `deep:` and the two `deep crt:` that settle the manifest question.

## Shutdown: a `QThread` destroyed while its thread was still running

The fifth pass wrote `tests/win-usability` down as green, and a re-run of it minutes later on
the same binaries answered `failures=14`.  That was not the gate being flaky.  It was a crash
on close that had been in the application longer than this port has existed, and the Zune HD
attached to this machine made it reproducible.

- The exit code is the lead, so it was searched rather than argued with: `-1073740791` is
  `0xC0000409`, `STATUS_STACK_BUFFER_OVERRUN`, which is also how `abort()` leaves a Windows
  process under `__fastfail`.  The Application event log named `Qt6Core.dll 6.11.0.0`, which
  points at `qFatal` rather than at a memory bug, and the last line of every affected session
  log read `[qt.fatal] QThread: Destroyed while thread '' is still running`.  Qt makes
  destroying a running `QThread` fatal deliberately, and it names the thread by object name -
  which no worker in this app carries - so the message alone does not say which thread; the
  repro does.
- The site was `DeviceService::~DeviceService()`: `m_workerThread.quit()`, `wait(5000)`,
  then the member destructor whether or not the wait answered yes.  `quit()` cannot recall a
  thread that is already inside a call, and the call here is libzune.  The session logs say
  why five seconds were not enough: `[libusb] bulk_write failed: ep=0x01 err=Operation timed
  out`, then `open_session: failed to send command`, then `OpenSession dead - resetting
  device and retrying`, the documented wedge on this hardware, and that retry outlasts the
  bet.  Fifteen of the seventeen launches in the failed run were inside it when the window
  was closed; the two that exited cleanly had already finished theirs.
- Nothing to do with the manifest, playlist naming or UTF-16.  The same destructor runs
  unchanged on Linux, where a responsive device usually wins those five seconds: the port
  inherited the race rather than creating it, and what caught it is the gate that closes
  windows the way a user does.

`src/ThreadDrain.h` is the repair, and it is one function.  `Zuuned::drainThread()` asks the
loop to quit, waits in five-second slices, prints which worker it is still waiting for, and
after twenty-five seconds stops the thread itself - the same last resort `UsbWatcher::stop()`
already uses - because a stopped thread may be destroyed and a running one costs the process.
`~DeviceService` uses it for the device worker, and `~LibraryService` for the scanner and
matcher threads, which held the same five-second bet.

Measured here with the Zune HD attached: four launches into a seeded profile, closed eleven
seconds in, gave four exit codes of `0` and a log reading `[device] worker still busy at
shutdown, waited 5s` then `waited 10s` before `[libusb] closed`.  In the re-run of the
whole gate, every launch log but one carried the three lines, running to `waited 15s`, and
none of the 17 launches produced a fatal; the run immediately before that one logged 15 of
17, because how many launches are still inside a device operation when their window closes
is what the device happened to be doing.  The cost is stated in the open rather than
hidden: a polite close now takes up to twenty seconds while a device operation finishes,
and a worker still running after twenty-five is terminated instead of taking the app down.
Neither is what anybody wants to wait for; both are better than a crash dialog from an app
that already looked closed.

The same shape one thread over is the spoken worker, and it is closed too.
`SpokenMediaService::shutdown()` used to hand the last resume-point write to its own
thread over `Qt::BlockingQueuedConnection`, and `~SpokenMediaService` used to follow it
with a `QThread::wait()` carrying no timeout; the write is a queued post with a
two-second deadline now, the destructor calls `drainThread(m_thread, "spoken", 2)` - the
`2` counts five-second slices, so ten seconds after the write's two - and the miss case
prints `[spoken] last position not saved after 2000ms, closing anyway`.  Why the last
resort is tolerable here, after this file argued against it once, is scope rather than
nerve: `src/spoken/` holds no libzune call and no USB handle, the encode a spoken transfer
needs belongs to the sync worker, and podcast and audiobook transfers are prepared there
and sent by `SyncEngine`/`DeviceWorker`, so `terminate()` could cut short a fetch, a probe,
a cover write or a download - the last of which lands through `QSaveFile`, so an abandoned
one never appears at the episode path and costs at most a scratch file beside it.  It
has not fired: the `spokenclose` phase of `tests\win-usability` closes a spoken page on
every run and no launch has logged `[spoken] worker would not stop`.  Recorded rather than
asserted, because the phase does not reach it: nothing in the suite plays a spoken item on
Windows, so `m_positionId` is empty when those closes happen and the deadline protects a
branch the harness never arrives at.

## Sixth sweep: the same questions, asked again on one binary (2026-10-21)

The tree went back through the reader with three narrow questions: what walks the
filesystem, what launches a program, and what reads the environment.  The answers are a
list now rather than a suspicion, and the third question is what surfaced the two
asymmetric diagnostics recorded under the tenth pass of `docs/WINDOWS_PORT.md`.

- **Two recursive walks, and no third.**  `LibraryScan::walkFiles()`
  (`src/library/LibraryScanner.cpp:271`) is the bounded one and the audiobook import at
  `src/spoken/SpokenMediaService.cpp:1145` is the exception recorded under Known gaps.
  Everything else that lists a directory in `src` is one level deep: the two
  `entryList()` loops at `src/diagnostics/SupportReport.cpp:57` and `:99`.
- **Three process spawns, two of them unreachable here.**  The udev helper at
  `src/DeviceService.cpp:1031` sits behind `udevRulesApply()`, which is false on
  Windows, and answers with the driver instruction instead of a child process;
  `ensureGvfsMounts()` (`src/LibraryService.cpp:752`) only fires for a path matching
  `/run/user/N/gvfs/`, which no Windows path does.  The third is Windows-only by
  construction: `src/library/librarytool.cpp:673` runs `cmd.exe /c mklink /J` so the
  compat gate can build the junction that reproduces a self-containing watch folder.
  Nothing else in the application hands a string to a shell, so there is no quoting
  surface left to port.
- **No POSIX-only device paths.**  No literal `/dev/null` and no `/tmp/` in any source
  file; the single hit is prose in `tools/artwork-preview/README.md`.
- **The shim cannot travel.**  It reaches a build only as a force-include through
  `ZUUNED_SHIM_FLAGS` (`CMakeLists.txt:112`), and `compat/win/posix_shim.h:21` refuses
  to compile on anything that is not Windows, so a Linux build cannot inherit it by
  accident.
- **Eight environment reads, all accounted for.**  Each has a named home:
  `QT_QPA_PLATFORMTHEME` (`src/main.cpp:88`), `ZUUNED_LOCK_SUFFIX`
  (`src/main.cpp:172`), `ZUUNED_RESCAN_ON_LAUNCH` (`src/main.cpp:362`),
  `ZUUNED_PROFILE_DIR` (`src/ProfilePaths.h:41`) and `ZUUNED_MPV_AO`
  (`src/player/MpvController.cpp:57`, plus the same read in
  `src/player/playertool.cpp:52`), the two `XDG_` names at
  `src/diagnostics/SupportReport.cpp:50`, and `USER`
  (`src/diagnostics/DiagnosticsService.cpp:51`).  There is no `HOME`, no `TMPDIR` and
  no `PATH` read in `src`.  `USER` is the tenth pass defect: Windows defines
  `USERNAME`, so until that pass the redactor silently lost its name rule here.
- **The question that paid for itself.**  For every `#if defined(Q_OS_WIN)`: does the
  `#else` branch do more than the Windows half?  All 55 platform guards in 21 files
  under `src`, `transcode` and `compat` were read in place.  Exactly one was
  asymmetric, and it cost a report field rather than a compile -
  `src/diagnostics/SupportReport.cpp` filled `zune_usb_devices` only on Linux.  The
  other thing worth asking again came back clean: all six executables call
  `zuuned_long_paths()` (`CMakeLists.txt` 528, 566, 579, 607, 638, 657).
- **Where symmetry is not the same as equal detail.**  The `graphics_devices` block at
  `src/diagnostics/SupportReport.cpp:55` runs on both platforms and simply finds no
  `/sys/class/drm` here, and `user_can_read_write` is emitted only from the Linux
  branch at `:112`.  A Windows report therefore says less, by omission, and the Known
  gaps entry says as much.

## A Zune whose album is named "{09316835-...}": the GUID moniker (2026-10-22)

Reported from the Windows device page: the ARIA album's line carried a GUID where a
name belongs, and the albums around it did not - "so some places still have
problems".  It is not the album.  The pull log for the same album writes
`~/Music/Zuuned Imports/{09316835-3188-481c-a359-0f8d2a937ac8}/ARIA The NATURAL
VOCAL SONG COLLECTION/...`, and `saveTrackToLibrary()` builds
`<imports>/<artist>/<album>/<file>`, so the moniker is the **artist** and the album
string is intact; the album row's subtitle is an artist, and that is the string on
screen.

`ffprobe` on the files pulled off the player says why one album and not the others.
All three are ASF/WMA under an `.mp3` name the device supplied; the ARIA file
carries `album`, `title`, `genre`, `disc` and `WM/Year` and **no Author attribute at
all**, the EGOIST file carries only `AlbumArtist`, and the Sakai Noriko file carries
a real author.  Where the source file had no artist the Zune software stored a GUID
moniker in the ZMDB artist record instead of leaving it empty, and the player
displays that.  Three independent cross-checks: `libzune/src/album.c:113` already
documents the sibling case, an orphaned nameless album object that "displays as its
GUID moniker"; the reference ZMDB parser this repository cites renders the artist
record's varint field `0x14` 16-byte GUID in braces, in exactly this shape; and the
literal string names artist pages on Last.fm across unrelated albums, so it is a
placeholder other people's libraries carry too, not an artefact of this one.  `rg`
for GUID-formatting code in `src` and `include` finds nothing unrelated to this
question: nothing here makes the string.

A scar on the disk says the value arrives padded as well as nameless.  `Music\Zuuned
Imports` still holds *two* folders for this artist:
`                {09316835-3188-481c-a359-0f8d2a937ac8}` - sixteen leading spaces, and
empty - created 2026-10-21 00:08, and the clean `{09316835-…}`, created 21:38 the same
day, holding both ARIA files.  The first predates `8564616` at 07:30, the commit that
made `saveTrackToLibrary()` name its folders through `LibraryPaths::safeComponent()`,
which trims; the second is what that trim produces.  `isGuidMoniker()` therefore trims
before matching because the device pads, not for tidiness, and the padded form is one
of the gated cases.
Read as a name, that string goes further than the one row it was seen on.  It is a
filesystem component a pull creates and therefore the name of a folder the user has
to explain; it is the albumartist hint `m_trackImportBatch` hands the import; it is
the artist the completed batch primes an online artist-image lookup with; and, the
quiet one, it is the **artwork cache key** `importDeviceArt()` stores the cover
fetched off the device under - while the album indexed from those same files
locally has no artist tag, so it asks for its art under `Unknown Artist`.  Two keys
for one album, which is how a device cover that was downloaded perfectly ends up as
no thumbnail at all in the local library.  It is also what a later sync writes back
to the next player.

- `MusicIdentity::isGuidMoniker()` is the one predicate: 8-4-4-4-12 hex, braces
  optional because the value arrives both ways, anchored so text that merely
  contains one stays text.
- `MusicIdentity::deviceArtist()` / `deviceAlbum()` answer the app's own
  `Unknown Artist` / `Unknown Album`, the words LibraryScanner, AlbumArtService and
  both pivots already agree on, so no downstream code learns a new vocabulary and no
  QML binding changes.
- Two places apply it: the ZMDB scan rows in `src/DeviceWorker.cpp:248` (which feed
  the track rows, the artists/albums pivots, the sort, the artwork identity and the
  arguments a pull passes back) and the album object artist at
  `src/DeviceWorker.cpp:492`, which is the albumartist hint.  `saveTrackToLibrary()`
  resolves its two arguments too, so a row that reaches it from anywhere else cannot
  name a folder after a moniker.
- Deliberately **not** touched: the identity comparison in
  `doPurgeInterruptedObject()` (`src/DeviceWorker.cpp:576`), where the raw device
  value is what a persisted recovery record has to match, so sanitising one side
  would break a deletion guard; the albumartist canonicalisation in
  `src/sync/SyncEngine.cpp:126`, which votes among *local* metadata and would be a
  behaviour change to a path the user is actively testing; spoken, video and
  playlist names, where no moniker has been observed; and the `0x44`
  `.art`/`.alb` name fallback and leading-artifact stripping the reference parser
  performs, which stay unimplemented until a device shows a need for them.
- Nothing repairs artwork already cached under a moniker key.  It is an orphaned
  cache file, not a wrong answer, and the next fetch writes the right key.

Gated by ten cases in `librarytool paths` - the observed value, the unbraced form,
case and surrounding space, a real artist, an album title, the empty string, text
that only contains one, a GUID one digit short, and the two name substitutions.  The
last case is the point of the change rather than a restatement of it: a moniker
artist and a tagless local file must answer the same artist, because that is the
difference between a device cover landing where the local album can find it and
landing where nothing ever will.  The rule is about the player, not the platform, so
the cases are not branched and Linux gates them identically.  On the `--clean-first`
rebuild that carries this change `librarytool paths` answers `ALL PASS (0 failures)` at 62
checks, `tests\win-compat` answers `201 checks, 0 failures, 0 skips`, and
`tests\win-usability` answers `launches=18 checks=99 failures=6` - six failures that are the
two desktop-dependent cases of the next section, four ribbon checks asking a 150 per cent
screen for a window it cannot host and two profile checks accusing the run of the new
`_qt_QGfxShaderBuilder` file, none of them this change.

**Re-run on the device afterwards, and it changed the answer twice.**  The previous
draft of this section stopped there and called the display fix compile-and-gate
evidence.  The next sitting opened the player again, counted what was actually on it,
and found both a confirmation and a refutation - see the seventh sweep below.

## Seventh sweep: the moniker came back in through the front door (2026-10-22)

One question, asked from the same device page: why does the ARIA album carry a GUID
where the albums beside it carry a name, if the moniker is the artist?  The honest
answer was that nobody had counted the device.

**Counted now, on the player.**  The ZMDB scan loop in `src/DeviceWorker.cpp` tallies
the raw fields before it maps them and prints one line:

    [zmdb] GUID moniker fields on 189 track rows: artist 27 ({09316835-3188-481c-a359-0f8d2a937ac8}), album 0 (-); blank-padded artist 27, album 0

Twenty-seven of 189 rows name the **artist** by moniker; **not one row names an album
by one**.  "An album called `{09316835-...}`" has therefore never been true on this
player - the text under an album row is its subtitle, which is an artist, exactly as
the section above said.  And the 27 moniker rows are precisely the 27 blank-padded
rows: where the source file had no author, the Zune software stored a GUID and padded
it with spaces, so padding and placeholder are one phenomenon on the artist field.
That is why trimming now lives in `deviceArtist()`/`deviceAlbum()` rather than in
whatever caller remembers it - a name that differs from itself by blanks is a
different artwork cache key, a different comparison against the locally indexed album
of the same name, and a different folder for a pull to land in.

**The local library was poisoned, by this application.**  `library.db` carried
`{09316835-...}` as `tracks.artist` on two rows, which refutes the claim above that
`library.db` was never poisoned.  What happened is worth naming precisely because it
is a hole in the rule and not in a tag: the pull wrote
`Music\Zuuned Imports\{09316835-...}\<album>\<file>`, the watch folder then scanned
what the pull had just written, and `extractFolderMetadata()` implements
`Root/Artist/Album/file` - so a file with no Author attribute had its grandparent
folder promoted to `artist` and `albumartist`.  Sanitising only the device-facing side
left the disk-facing side free to reintroduce the same string as a name.

A rescan cannot undo it.  A file whose stored probe version already equals
`musicParserVersion` is never probed again, so those two rows are permanent, and
raising `musicParserVersion` is not available anyway - `tests/music-import/backend.cpp`
pins `probeVersion == 1` on the Linux line.  So the fix sits in two places.
`MusicIdentity::usableName()` is the same predicate read from the other end: not "what
should the UI show" but "may this text be a name at all", which is the question a scan
asking for a *candidate* has to ask.  It now guards every candidate in
`resolveTrack()` - the `artist`, `albumartist` and `album` tags, both folder
inferences, and the `Artist - Title` filename fallback - so a Zune-authored path
answers "no candidate" and the ordinary Unknown Artist / Unknown Album fallback
answers instead.  And `LibraryDb::repairDeviceMonikers()` runs from `open()` beside
`repairWatchFolderRoots()` for the very reason that neighbour exists: the rows a
healthy scan will never revisit are exactly the rows that need help.  It rewrites only
a field that really held a moniker, mirrors the scanner's own fallbacks rather than
inventing its own (`Unknown Artist`, album artist filled from it, `Unknown Album`), and
skips any row with `user_edited` set - there, the text is the user's, moniker included.

Measured on the real profile rather than argued: the first launch of the rebuilt app
printed `repaired device moniker on track 13: {09316835-...} / ARIA The NATURAL VOCAL
SONG COLLECTION is now Unknown Artist / ...` and the same for track 14, and every
launch since prints neither line, which is the idempotence claim.  Read back out of
`library.db`, row 13 is `Unknown Artist / Unknown Artist` and row 14 is
`Unknown Artist / EGOIST` - the album artist that was real kept its spelling, which is
the distance between a repair and a sledgehammer - and no `{` remains anywhere in
`tracks`.

**A logging trap that ate its own evidence.**  The first wording of the audit line was
`[zmdb] device name <value> rows: ...`, and what it printed was
`[zmdb] device name [redacted] rows: ...`, because `src/diagnostics/Redactor.cpp`
lists `(?:device[-_ ]?|friendly[-_ ]?)name` among its credential keys and folds
everything after such a word.  The line is `GUID moniker fields` now, with the reason
in a comment at the call: a diagnostic that redacts its own payload is worse than no
diagnostic, because it reads like proof.  The old line is still in the log directory,
which is the only reason the trap is reproducible.

Gates against the relinked build: `librarytool paths` `ALL PASS (0 failures)` at 67
checks - fifteen of them the moniker rule, none branched `Q_OS_WIN`, because the
placeholder belongs to the player and not to a platform - and `tests\win-compat`
`201 checks, 0 failures, 0 skips`.  `tests\win-usability` was deliberately not
re-run for this change: the app was in the user's hands on the real profile, and
neither gate that reaches this code is the GUI harness.  What this sweep adds to the
Windows record is a re-read of the device and a read of the library, both inside the
music-library-read gate that was already closed; **it adds no new capability.**
Deletion, the podcast, audiobook, photo and video categories, every model but this
Pavo, and anything anyone has listened to all stay open, and the GUID-named import
folders the old pull left on disk stay where they are - they are the user's files, and
they now read as Unknown Artist.

## Eighth sweep: the uuid was a genre, and there were 130 of them (2026-10-22)

Asked again from the device page, after two sweeps that both said the album string was fine:
the ARIA album on the player still shows a string of hex where a name belongs, and other
albums do not, "so some places still have problems".  The premise was tested before the code
was, and the premise is false - which is exactly why the two sweeps above did not find it.

**No album on this player is named by a moniker, and the fields had never been counted
separately.**  The tally in the ZMDB scan loop now reads all three name fields and prints the
whole pool rather than "did the one known placeholder shape appear":

    [zmdb] GUID moniker fields on 189 track rows: artist 27 ({09316835-3188-481c-a359-0f8d2a937ac8}), album 0 (-), genre 130 ({09316835-3188-481c-a359-0f8d2a937ac8}); blank-padded artist 27, album 0, genre 130

    [zmdb] distinct albums (15): "1995-2001 光华真纪录 - 单身日记", "ARIA The NATURAL VOCAL SONG COLLECTION", "Bi・Ki・Ni", "Extra terrestrial Biological Entities", "MENDELSSOHN: SONGS WITHOUT WORDS", "Messe in h-Moll, BWV 232", "QA Windows", "Watercolour", "Windows Round Trip", "Windows Round Trip 660", "Windows Round Trip 777", "Windows Round Trip 888", "【M3-41】虚構の楽園", "测试音乐", "真爱无敌精选 (2015)"
    [zmdb] distinct genres (6): "                {09316835-3188-481c-a359-0f8d2a937ac8}", "Classical", "JPop", "QA", "Unknown Genre", "アニメ"
    [zmdb] genre tiles after the moniker rule (5): Classical (17), JPop (13), QA (4), Unknown Genre (12), アニメ (13)
    [zmdb] device playlists (0):

plus one `album object <id> name="..." artist="..." tracks=N` line for each of the player's
15 album objects, every one of them a real name - `album object 100663309 name="ARIA The
NATURAL VOCAL SONG COLLECTION" artist="Unknown Artist" tracks=13`.  So: 0 moniker albums in
the ZMDB rows, 0 in the album objects, 0 in the playlists (there are none), and 15 real album
strings.  "An album named by a GUID" has never once been true on this device.

**What was on screen is a genre, and the Genres pivot is drawn like an album shelf.**
`genre` is the one name field this application has always copied over raw -
`row.genre = QString::fromUtf8(t.genre ? t.genre : "")` - and the display path is
`qml/MusicPage.qml:154 genreRows <- DeviceService.genresList <- src/DeviceWorker.cpp
genreCounts <- row.genre`.  That pivot is a tile gallery (`qml/MusicPage.qml:737`) whose title
is `modelData.name` and whose artwork is up to four of that genre's albums in a 2x2 mosaic
(`genreAlbumRefs()`, `qml/MusicPage.qml:280`), with `GenreDetail` under the tile listing
those albums by name.  A tile titled `{09316835-...}` carrying the ARIA covers therefore
reads, perfectly reasonably, as one album named by a uuid, while the albums pivot beside it -
whose artist got a rule two sweeps ago - read correctly.  That is the whole of "some albums
are like this and some are not": one GUID, one field, and the one pivot that had no rule.

**Genre is where a Zune parks an unlearned value far more often than artist - 130 rows of
189, not 27.**  Before this change the moniker was the largest genre tile on the device by
almost nine times, and `DeviceService::pickMoodColor()` (`src/DeviceService.cpp:400`) takes
the highest-count genre as its dominant string, so every device session keyed the device
page's accent colour off a GUID that matches none of the genre keywords and silently fell
through to a random palette entry.  That is fixed by the same line, not by a second one.

**What a genre yields, and why it is nothing.**  `MusicIdentity::deviceGenre()` trims and
turns a moniker into an empty `QString`.  It deliberately does not yield an
"Unknown Artist"-style word: both pivots skip an empty genre (`src/DeviceWorker.cpp:382`,
`LibraryService`), so empty is precisely what an untagged file already answers, and a device
row has to answer what the local scan of the same file answers.  `deviceArtist()` and
`deviceAlbum()` yield words only because those two fields have a word to yield.  The
device's own literal `Unknown Genre`, seen in that very pool beside the moniker, is the
player's content and stays exactly as it is - rewriting it would be editing the player.

**The local side got the same door, and needed no mop.**  `LibraryScanner.cpp` now asks
`usableName()` of the genre tag beside the artist/albumartist/album lines, so a pull written
before this change cannot teach a scan a GUID genre through the `Root/Artist/Album` folder
inference the seventh sweep documented; `LibraryDb::repairDeviceMonikers()` carries `genre`
beside artist/albumartist/album for the rows a probe version has already retired.  Read back
out of the real `library.db` (15 tracks) the genre column is `""`, `JPop` and `アニメ`, and
`tracks.artist` holds no `{`: this launch repaired nothing, because on this machine the
scanner change is the door and not the mop.  The repair's log line grew to name all three
fields - `%s / %s / %s is now %s / %s / %s` - so the next time this is asked the evidence
exists before the question.

Gates, on a `--clean-first` rebuild (`MusicIdentity.h` is header-only and Ninja records
`#deps 0` on this host): `build\librarytool.exe paths` answers `ALL PASS (0 failures)` at 73
checks (74 as first written here, re-counted against the current
binary), six of them the genre rule - the padded observed value, the unbraced form, `アニメ`
untouched, `"   JPop  "` trimmed, the device's own `Unknown Genre` kept, and moniker genre
reading the same off the player as off a tag.  `tests\win-compat` answers
`201 checks, 0 failures, 0 skips`.  `tests\win-usability` was again deliberately not re-run:
the application was in the user's hands on the real profile, and with that instance open its
two profile-isolation checks fail by design (see the bullet on them below), so a run now
would measure the harness.  What this adds to the Windows record is one more read of the same
player inside the music-library-read gate that was already closed; **it adds no new
capability.**  Device facts are unchanged - one hand-tested Pavo Zune HD - and deletion, the
spoken and photo/video categories, every model but this Pavo and anything anyone has listened
to all stay open.

**Left open, with the line that shows it - and retracted by the ninth sweep.**  The artwork
complaint is not this bug and reproduced in the current log every time a detail page was
opened:

    2026-10-22T02:40:27.899Z [qt.warning] QIODevice::read (QNetworkReplyHttpImpl): device not open
    2026-10-22T02:40:27.899Z [customize-http] HTTP 0, Qt error 4 (https://api.deezer.com/artist/325381)

What this paragraph went on to conclude - that error 4 is `HostNotFoundError` and the lead
is "a request that never started" - is wrong, and the ninth sweep corrects it with the
measurements: error 4 is `QNetworkReply::TimeoutError`, the request started, and this machine
has its proxy switched off.  Still open and still unexamined, because it is a different
question: a 0-byte `AppData\Local\Zuuned\Zuuned\State\library.db` beside the live profile's
real database, which is about which location was asked for, not about this fix.

## Ninth sweep: the last mile spoke a second language (2026-10-22)

**The question came back a third time, and this time the answer was in the app.**  On the
current build the device page shows no moniker anywhere - the capture below is the proof -
so the uuid seen under ARIA is a sighting of an older binary or of the local Music page.
The conclusion the user drew from it stands regardless: some place still had no rule.
Three places did, and none of them was in C++ - which is how a rule closed in the C++
device read earlier the same day could still leave a uuid on a screen after it.

**What the player stores, re-measured read-only before touching anything.**  Isolated
`ZUUNED_PROFILE_DIR`, `build\zuuned.exe --page deviceMusic --qa-probe`,
`ZUUNED_MPV_AO=null`, exit 0:

    [zunedb] scanned: 189 tracks, 0 videos, 0 photos, 15 albums, 11 artists, 0 playlists, 0 photo albums
    [zmdb] GUID moniker fields on 189 track rows: artist 27 ({09316835-3188-481c-a359-0f8d2a937ac8}), album 0 (-), genre 130 ({09316835-...}), title 0 (-); blank-padded artist 27, album 0, genre 130
    [zmdb] artist tiles after the moniker rule (8): Anri (10), J. S. Bach (27), QA Windows (4), Unknown Artist (27), Walter Gieseking (17), 葉月ゆら (9), 许茹芸, (82), 酒井法子 (13)
    [zmdb] album tiles after the moniker rule (15 of 15): "1995-2001 光华真纪录 - 单身日记" by "许茹芸," (15), "ARIA The NATURAL VOCAL SONG COLLECTION" by "Unknown Artist" (13), "Bi・Ki・Ni" by "Anri" (10), "Extra terrestrial Biological Entities" by "Unknown Artist" (12), "MENDELSSOHN: SONGS WITHOUT WORDS" by "Walter Gieseking" (17), "Messe in h-Moll, BWV 232" by "J. S. Bach" (27), "QA Windows" by "Unknown Artist" (1), "Watercolour" by "酒井法子" (13), "Windows Round Trip" by "QA Windows" (1), "Windows Round Trip 660" by "QA Windows" (1), "Windows Round Trip 777" by "QA Windows" (1), "Windows Round Trip 888" by "QA Windows" (1), "【M3-41】虚構の楽園" by "葉月ゆら" (9), "测试音乐" by "Unknown Artist" (1), "真爱无敌精选 (2015)" by "许茹芸," (67)

`title 0` is the counter this sweep added, because a report can be about a title as easily
as about an album and until now nobody could have answered that one either way.  The two
new tile lines exist for the same reason: "the album tile shows a uuid" is a statement
about a *label*, and the label is a grouped list built from several rows, so counting
columns could never settle it.  Every one of the 15 album tiles on this player carries its
real name, and ARIA's subtitle reads `Unknown Artist` - the rule firing where it should.
What this capture does is bound the sighting, not reproduce it; it is honest about being
unable to do the second.

**Door one: `qml/MusicIdentity.js` was a second implementation of the rule and had none of
it.**  It is not a binding, it is the same decision re-written in JavaScript, and it is the
last mile for `MusicPage`, `ArtistDetail` and `GenreDetail` on **both** sources - local
library rows and device rows all group through `fromTrack` / `fromAlbum` / `owner`.  The
C++ predicate written in `src/library/MusicIdentity.h` never reached it: `owner()` returned
`track.albumartist || track.artist` verbatim, so a moniker arriving through an import
folder a pull had been filed under, or through a local file a tagless rip left behind,
became a tile name - the complaint - while the C++ device read printed `Unknown Artist`
for the same row on the same screen.  It now carries `guidMoniker`, the same pattern character for character as the
concatenated C++ literal in `MusicIdentity::isGuidMoniker` (braces optional, surrounding
blanks not part of the answer), and one `known()` helper that yields the text or empty and lets the callers' own
`Unknown Artist` / `Unknown Album` do what they already did with empty.  Device rows cannot
carry a moniker into it - `DeviceWorker` already replaced them at the source - so this
door was open on the library side, which is precisely where a pull filed under the
folder name the seventh sweep documented lands.

**Door two: the Artists pivot never went through the identity module at all.**
`qml/MusicPage.qml` `computeArtists()` grouped off raw `t.artist`, so a correct mirror
would still have left an artist tile named with a GUID.  It calls `MusicIdentity.artistName()`
now.

**Door three: the album page painted its own headline from an unruled field.**  Grepping
the whole `qml/` tree for the other name fields found this one: `AlbumDetail`'s
`primaryArtist` / `primaryAlbumArtist` fall back to the first row's own fields, and that
value is both the page's headline text and the name of the artist page its byline opens -
so a moniker here is a tile name one level down.  Both go through `known()` now.  What was
deliberately left raw is the SyncEngine payload (`syncItemOf`, the playlist drag and add shapes): that object
is the identity a send writes, not a label, and the rules that govern it are the scanner
and `repairDeviceMonikers()`, not the painter.

**Door four, found by reading the callers of the mirror rather than the mirror.**  The
customize re-key in `qml/MusicPage.qml` compares a navigation-stack entry against a key it
builds itself, and it built that key from `before.artist || "Unknown Artist"` while the entry
it was comparing against had been built *through* the mirror.  Its one caller
(`customizeAlbum`) already hands it `MusicIdentity.owner(first)`, so no moniker reaches it
today - which is precisely the kind of door worth closing regardless, because a key
comparison that disagrees does not fail, it silently re-keys nothing.  It rules the value it
was given now instead of trusting who gave it.  Sweeping the tree for the rest found nothing
else standing: `key()`, `matches()` and `groupAlbums()` all build through `fromTrack`, and
the artwork lookups stay raw on purpose because `LibraryService.artKey` is keyed on what the
database stores, not on what the screen says.

**How a `.js` file got verified when neither Windows gate reaches one.**  `tests\win-compat`
compiles C++ and `tests\win-usability` drives a window, and the grouping the mirror owns
happens in neither's line of sight, so the mirror was checked by parsing and asking: strip
the `.pragma library` line, evaluate the module, and put the observed device values through
it.  Every answer it has to give reproduces - `{09316835-3188-481c-a359-0f8d2a937ac8}` and its
blank-padded and unbraced forms all yield empty, `  ARIA  ` yields `ARIA`, an undefined
field yields empty rather than throwing, and `fromAlbum({name: "ARIA", artArtist: "{09316835-..."})`
yields `{"name":"ARIA","artist":"Unknown Artist"}`, which is the line the device capture
printed.  That is a check performed once by hand and recorded here, not a gate: the
consequence of having two copies of one rule is that nothing fails when they drift, so
`MusicIdentity.h` now says so above the predicate both are meant to agree with.

**The other half of that verification, and the half that could be a gate.**  Parsing and
evaluating the module proves what it answers; it does not prove that a caller in QML can see
it.  `qmllint` from the same Qt 6.11 answers that, and it does resolve a relative
`import "MusicIdentity.js"`: given a call to an export that is not there it prints
`Member "noSuchFunction" not found on type "MusicIdentity" [missing-property]`, checked
against a planted fault so that the silence on the real files means something.  Across the 96
`.qml` files in `qml/` no `missing-property` warning names `MusicIdentity` and there is no
`Error:` line - the one line that matches that string is qmllint echoing a source line which
contains `connectionError:`.  It is not adopted as a tree-wide gate, because 3246
`missing-property` warnings name `Theme` (2897 of them) and other singletons whose compiled
QML module qmllint cannot see from a source directory: a gate on that category would fail on
files nobody touched.  The recipe until that changes is to lint what touches the mirror and
grep for the mirror's name.

**A harness fault, and the third of its kind in this file: the usability gate was
scale-blind.**  Re-running the compact phase for this fix failed four checks against an
application that had not been changed.  `Main.qml`'s breakpoints are logical pixels
(`ribbonRetired: width < 1280 || height < Theme.headroomCompactBelow`) and `winshot`'s
`MoveWindow` speaks physical ones, so the fixed 1400x1220 request the drive had always made
asks two different questions on two different desktops: at 100% it clears the ribbon, and at
150% it lands on 933x813 logical and can never reach it.  The first repair attempt made it
worse in an instructive way: a PowerShell `Add-Type` helper calling `GetDpiForWindow`
answers **96 to a DPI-unaware caller whatever the desktop is doing**, so it reported a 150%
screen as 100% and the drive went on blaming the app.  The scale now comes from `winshot`,
which declares itself per-monitor aware before it asks (`winshot --dpi <hwnd>`, resolved
through `GetProcAddress` because the function needs an SDK level this file does not
otherwise request), the tall stages are computed from that scale, and the minimum-size check
- which had been comparing a *physical* client rectangle against Main.qml's *logical*
1100x700 floor, and so passed vacuously at 150% - compares logical.  Third lesson of the
same shape as the cached `Process.MainWindowHandle` and the `Process.Kill()` exit code: the
harness measured its own assumption and reported it as the application's behaviour.

**Two more faults of the same shape, both from the throwaway drive written to re-check this
fix, and both blamed on the application.**  It waited on `zuuned --qa-exit-ms 9000`, a flag no
file in this repository mentions - the application ignores an unknown argument and carries on,
so a healthy app read as a 45-second hang and the drive killed it.  It then grepped for
`[qa-probe]`, which is the name of the flag, while the line the QML prints begins `[qa] `, and
concluded nothing had been reported.  And it launched with `Start-Process -WindowStyle Hidden`,
after which `winshot --pid` answers `no visible window for pid 65944`,
`Process.MainWindowHandle` is 0, and neither `CloseMainWindow()` nor `taskkill` without `/F`
has a window to ask - so that app genuinely would not come down politely, because there was
nothing there to be polite to.  The gate does none of these three things and answers the
question in one measured line: `this close took 0.5s from the window close to the exit code`.
A drive written from memory of a harness reproduces the harness's assumptions.

**One consequence of that measurement, recorded and not fixed.**  At 150% a 2560x1440
desktop is 1706x960 logical, and 960 is below `headroomCompactBelow` (1100), so
`spaceScarce` is true in *every* window that fits on the screen and the top ribbon cannot be
reached at all at that scale - the gate only passes its ribbon stages because `MoveWindow`
does not clamp a taller-than-monitor window that the user could never have made.  Whether a
chrome that needs 1100 logical pixels of height is right for a 150% 1440p laptop is a design
decision, not a defect, so it is raised here rather than tuned in a sweep.

**Left open, with the line that shows it - and a retraction.**  The artwork complaint is not
this bug, and the diagnosis this file carried for it was wrong in its one technical claim:

    2026-10-22T02:40:27.899Z [customize-http] HTTP 0, Qt error 4 (https://api.deezer.com/artist/325381)

**Qt error 4 is `QNetworkReply::TimeoutError`, not `HostNotFoundError`** (`QtNetwork/qnetwork
reply.h`); `HostNotFoundError` is 3.  A request that never resolved a name and a request
that got no answer are different faults and the log as written could not tell them apart,
which is why the guess went wrong.  Four curls on the same machine say what is actually
happening:

    curl https://api.deezer.com/artist/1006              connect fails, 21.07 s
    curl https://www.google.com/generate_204            connect fails, 21.06 s
    curl https://api.github.com                         200, 0.39 s
    curl -x http://192.168.2.100:8117 the two failures  200 and 204, under 1 s

The obvious reading - a Windows application that had never been told about the one proxy
this network has - was then measured rather than assumed, and died: `ProxyEnable` is 0
(`ProxyServer` still names `192.168.2.100:8117`), and a fresh Qt 6.11 process reports
`usesSystemConfiguration=1` and answers `NoProxy` from the platform default, so **Qt already
follows the Windows shell and going direct is correct**.  This is a machine with its proxy
switched off, not a broken client.  What was genuinely missing was the route in the log:
`HTTP 0, Qt error 4` reads identically direct or proxied, so `src/library/NetworkAccess.h`
now prints which route a request took - `[network] Windows proxy settings: HTTPS goes
direct` once at startup, and `via <route>` on every failed `[customize-http]` line - plus
`ZUUNED_PROXY=[scheme://][user[:pass]@]host:port` for a tester who wants provider art
without switching the whole desktop.  Unset, nothing is installed and Qt's answer stands on
both platforms.  Both halves of that plumbing are live in the run quoted below, and they
carry apart the two faults that used to read the same:

    [customize-http] HTTP 503, Qt error 403 via direct (https://musicbrainz.org/ws/2/release-group/)
    [customize-http] HTTP 0, Qt error 5 via direct (https://musicbrainz.org/ws/2/artist/)

One provider answered "unavailable" and one request this process gave up on, and each line
names the route that was asked to carry it.
A proxy-consulting factory was deliberately *not* written; the probe above is in the header
so nobody implements it again on the strength of a comment.

Gates, all re-measured on the relinked binary: `build\librarytool.exe paths` answers
`ALL PASS (0 failures)` at 73 checks, `build\librarytool.exe net` answers
`ALL PASS (0 failures)` at 27 checks, `tests\win-compat` answers `201 checks, 0 failures,
0 skips`, and `tests\win-usability` answers `launches=18 checks=102 failures=0 skips=0
killed=0 pid-scan-fallbacks=1` with `OVERALL=ok` - the fallback counted one of the eighteen
launches whose window had to be resolved through `winshot --pid` after the process property
came back empty - and the compact phase printing
`note window scale 1.5`, `note narrow asked 900x640 client 1650x1050 logical 1100x700` and
`note state sequence: compact-retired -> roomy-ribbon -> compact-retired -> roomy-ribbon`.
The QML is qrc-embedded, so the JS and QML fixes need a relink and not a recompile of
anything else.

**Door four landed after those numbers, so the half that can see it was measured again.**  A
QML change needs the relink and nothing else, and the relink is what `tests\win-usability`
was then run against: `launches=18 checks=102 failures=0 skips=0 killed=0
pid-scan-fallbacks=0`, `OVERALL=ok`, its tools phase still passing `names`, `customize`,
`paths` and `net`.  `paths` at 73 and `net` at 27 were asked again against the same binary and
both answer `ALL PASS (0 failures)`; `tests\win-compat`'s 201 checks stand from the build that
preceded it, because no C++ file has changed since and no C++ gate can reach the line that
did.  One of these numbers is a tally and not a constant: `pid-scan-fallbacks` read 1 on the
first run of the pair and 0 on the second, which is what an opportunistic fallback looks like
when nothing forces it - it is recorded so a later change cannot make the fallback the only
path that works, not so that any one run can promise a value.

**This pass adds no Windows capability and no device claim.**  The device evidence is one
read-only re-sighting of the same hand-tested Pavo Zune HD inside the music-library-read
gate that was already closed.  Deletion, the spoken and photo/video categories, every model
but this Pavo, audible playback and anything anyone has listened to all stay open.

## Tenth sweep: the last hand-rolled walk, and a re-read that found one (2026-10-22)

**One defect, and it is the one the last three sweeps pointed at without picking it up.**
`SpokenMediaService::previewBookFolder()` was the last recursive directory walk in the
source tree: a `QDirIterator` over `QDir::AllEntries | QDir::NoDotAndDotDot |
QDir::NoSymLinks` with `Subdirectories`.  `NoSymLinks` is the filter that looks like
protection and is not - `QFileInfo::isSymLink()` is false for a Windows junction, so a
folder linking back into one of its own ancestors is an ordinary directory, the walk
descends, and one book comes out filed under every phantom path the loop invented.  It
calls `LibraryScan::walkFiles()` (`src/library/LibraryScanner.h:84`) now, which is what a
watch folder uses: an explicit stack instead of recursion, an identity per folder (volume
serial plus file index, real ones through `compat/win`), and a refusal to enter a folder it
has already listed.  `librarytool walk` holds that against a real `mklink /J`
(`src/library/librarytool.cpp:778`), so the import is inside a gate it used to stand
outside, and the pattern below is the only place left in the tree that names it.

**Four behaviours were compared one at a time rather than assumed preserved.**

- The file set is the iterator's.  `walkFiles()` lists with `filters | QDir::Dirs |
  QDir::NoDotAndDotDot` (`src/library/LibraryScanner.cpp:308`) and calls the visitor only
  for entries that are not directories, so a folder called `track.mp3` stays a place to
  look and never becomes a chapter.  The visitor keeps its own `isSymLink()` test, which is
  the one thing `NoSymLinks` was actually achieving.
- The 5,000-audio-file refusal is unchanged word for word.  The 100,000 refusal now counts
  entries the visitor saw instead of entries of every kind, so a tree of nothing but
  folders is no longer capped - with a real identity per folder the depth is the
  filesystem's own, which is what makes that number a backstop here rather than the
  protection.
- The unreadable-folder refusal tells two cases apart instead of covering both with one
  sentence: an unreadable root says `Choose a readable local audiobook folder`, an
  unreadable subfolder says the old sentence.  It reads `complete` and `directories` off
  the walk rather than asking `QDir` whether a folder `isReadable()`, which answers from
  metadata, while the walk has already opened that folder to `fstat` it.  An empty folder
  still says `No supported audiobook files found in this folder`, and nothing outside this
  source file keys off either string - no test, no QML file.
- Cancellation keeps its per-entry granularity, and only because it was wired by hand.
  `walkFiles()` stops on the one flag it is handed, and that flag belongs to the two
  ceilings, so the visitor asks the caller's flag itself and stops the walk through the
  shared one.  The first cut read cancellation only after the walk, which imposed a wait the
  old iterator never did and which the bounded spoken-worker close at shutdown waits on.

**Neither Windows gate can drive that folder chooser, so the gate pins the structure instead
of pretending to have walked a tree.**  Two static checks joined the `tools` phase of
`tests/win-usability`: no `.cpp` or `.h` under `src/` may contain
`QDirIterator::Subdirectories`, and `src/spoken/SpokenMediaService.cpp` must contain the
`LibraryScan::walkFiles(` call.  They cost nothing, and each fails by naming the file.

**A second whole read of the tree, and the list of what it did not find.**  One change out
of a full re-read is a result, and it belongs here as plainly as the change does.

- No second recursive walk: outside the pin itself the pattern appears nowhere in the tree,
  and the other `LibraryScan::walkFiles()` callers are the music, video and photo scan
  phases that already went through it.
- Three `QProcess` sites under `src/`, none reachable on Windows except as a fixture.  `gio`
  (`src/LibraryService.cpp:752`) starts only for a watch folder matching
  `^/run/user/<uid>/gvfs/smb-share:...`, which no Windows path can match; the udev one-click
  returns at `src/DeviceService.cpp:1025` before it builds a process, because
  `udevRulesApply()` is false here; and `cmd.exe /c mklink /J`
  (`src/library/librarytool.cpp:787`) exists to make the junction this sweep leans on.
- Artwork and thumbnail cache keys are digests (`AlbumArtService::cacheKey`,
  `ArtistImageService::cacheKey`, the `MusicIdentityClient` key), so no device or tag text
  reaches a filename and no illegal character can.
- Every write that has to be atomic goes through `QSaveFile`, and the one bare
  `QFile::rename()` is guarded by an existence test in the same statement.
- No Linux-only environment read is left standing: the one that mattered (`USER`) went in
  the tenth pass, and `ZUUNED_TMPDIR` maps onto `zuuned_win_tmpdir()`
  (`compat/win/posix_shim.h:465`), so the transcode layer's `/tmp/` is spelled correctly
  rather than optimistically.

**What this fork calls itself, in its own documents.**  Not a path or an encoding, but a
correctness defect the same read turned up: `AGENTS.md` and `docs/PORT_PLAN.md` still named
the project `ZuunedLinux`, and `packaging/TESTER_README.md` sent a reader to upstream's
Linux downloads.  Those three and `docs/ANNOUNCEMENT_VIDEO.md` now say which product they
describe - this fork (`miemiekurisu/zuuned-modern`, the Windows client port) or the upstream
it inherited the feature from.  Runtime identity is untouched on purpose: `Zuuned`,
`zuuned.exe`, `%APPDATA%\Zuuned` and `ZUUNED_PROFILE_DIR` stay, because renaming them would
orphan every profile and library this fork already has on a real machine for the sake of a
label.  Two claims in `AGENTS.md` that the same day's fifth pass had already invalidated -
no `<longPathAware>` entry, UTF-16 m3u undecoded - are qualified to point at that pass
instead of being repeated as open.

Gates on the relinked binary (`cmake --build build --clean-first`): `tests\win-compat`
`201 checks, 0 failures, 0 skips` and `tests\win-usability`
`launches=18 checks=104 failures=0 skips=0 killed=0`, `OVERALL=ok`, with both new pins
passing; `pid-scan-fallbacks=2` is a tally, not a constant.  No Windows capability is added
and no device claim changes - still one hand-tested Pavo Zune HD, read-only, nothing
listened to.  The harness opened no device handle, and as on every run of it the instrumented
launches browsed the attached player read-only.

## Eleventh sweep: a second executable asking Qt where the application lives (2026-10-22)

Ten sweeps walked the names the application writes and the commands it runs. This one
walks a different question, which none of them could have reached: what happens when a
process that is *not* ``zuuned.exe`` asks where a Zuuned profile lives. There is exactly
one such place in the tree - ``tools/artwork-preview/main.cpp``, the artwork print lab,
which defaults its ``--cache-root`` and ``--library-db`` and otherwise expects to find
the application's own cached art and its library.

The defaults were Qt's *generic* writable location plus a literal ``"/Zuuned/Zuuned"``.
On Linux that expression is exactly the application's folder, by XDG luck: Qt puts
``AppDataLocation`` under ``XDG_DATA_HOME`` and ``CacheLocation`` under
``XDG_CACHE_HOME``, and both already end in ``Zuuned/Zuuned``. On Windows it is wrong
twice over, and wrong in two different directions.

- ``GenericDataLocation`` is ``C:\ProgramData``. Zuuned has never written a file there;
  ``Test-Path C:\ProgramData\Zuuned`` is ``False`` on the port's machine while the
  library sits at ``%APPDATA%\Zuuned\Zuuned\library.db``, measured at 176,128 bytes.
  ``--library-db`` pointed into a folder that does not exist.
- ``GenericCacheLocation`` is ``%LOCALAPPDATA%``, one level *above* the folder that holds
  ``art``, ``artistart``, ``stills`` and ``vidthumbs``. One level above is worse than a
  wrong drive, because it exists, so the tool reads a directory that looks like a cache
  root, finds no sample in it, and says so.

``QStandardPaths::AppDataLocation`` is not the fix either. Those two calls answer for
*this process*, and the preview lab sets ``organizationName("ZuunedPreview")`` and
``applicationName("Artwork Print Lab")`` on purpose - that identity is what keeps its
settings and its Qt pipeline cache out of the application's. Qt therefore names the
tool's own folder, which holds nothing. A tool that has to read somebody else's profile
cannot ask Qt about it; the rule has to be stated independently, and stating that rule
independently is already what ``Zuuned::ProfilePaths`` exists for.

``ProfilePaths::applicationDataLocation()`` and ``applicationCacheLocation()`` state it.
``ZUUNED_PROFILE_DIR`` wins on both platforms, so an instrumented tool reads the same
throwaway profile the application wrote, which is the whole point of the variable.
Unset, they name ``%APPDATA%\Zuuned\Zuuned`` and ``%LOCALAPPDATA%\Zuuned\Zuuned\cache``
on Windows and ``$XDG_DATA_HOME/Zuuned/Zuuned`` and ``$XDG_CACHE_HOME/Zuuned/Zuuned`` on
Linux - the same answers ``writableLocation()`` gives, minus its ``mkpath()``, because a
tool looking for someone else's library has no business creating an empty one.

Measured on Windows against one build of the tool, with no explicit roots given:

- the old defaults (``%LOCALAPPDATA%`` and ``C:\ProgramData\Zuuned\Zuuned\library.db``):
  exit ``1``, ``No cached artwork found. Supply --cache-root and --library-db.``
- the new defaults: exit ``0``, four ``READY`` comparison sheets, one of them naming
  itself ``ARIA The NATURAL VOCAL SONG COLLECTION / Unknown Artist`` - artwork out of the
  real cache, titles out of the real library.

Neither Windows gate builds this tool, so the ``tools`` phase of ``tests\win-usability``
pins the rule instead of pretending to have read an artwork cache: the two helpers exist,
the preview lab resolves through them, and no source file under ``src/`` or ``tools/``
spells a profile folder out of a string any more except ``ProfilePaths.h`` itself, where
the check lists its findings and listed none. The compile and the two runs above are separate evidence, done by
hand with ``cmake -S tools\artwork-preview -B build\scratch\preview``.

Left alone on purpose: the print lab's own render path, which is a Linux-line artefact and
carries no Windows claim, and the three uncalled ``popen()`` sites in
``libzune/src/transcode.c``, already recorded above under what this sweep does not re-check.

## Twelfth sweep: a name you may rewrite but not delete (2026-10-24)

The album-art path, asked as a compatibility question rather than a UI one, because that is what
turned out to be in it.  Four findings, and the first is the same class of defect that opened this
document: an assumption that is simply true on POSIX and has no Windows counterpart.

**A destination an in-process handle still owns cannot be deleted.**  ``AlbumArtService``'s
``storeToCache()`` staged a picture into a live ``QTemporaryFile`` and published it with
``QFile::remove(dst)`` plus ``QFile::copy(src, dst)``.  Windows refuses the removal of a name whose
owning object has not been destroyed - closing the file is not enough, the handle is held by the
object - so ``remove()`` returned false with a sharing violation, ``copy()`` then refused the
destination that was still sitting there, and the branch returned false.  POSIX happily unlinks an
open file, so these lines have never misbehaved anywhere else.  The branch is the verbatim-JPEG
fast path for a picture that is already a JPEG of 400 px or less, which is what a ``folder.jpg``
and a device thumbnail usually are, and a slow path that *decodes and re-saves* is unaffected:
this is why the symptom was "folder and device covers are missing" and not "artwork is broken".
``copyBytes()`` opens the destination ``WriteOnly | Truncate`` and streams; a holder that shares
reads and writes allows that, it withholds only deletion.

The rule is worth more than the fix, so the shape was hunted rather than assumed.  What the sweep
looked at, and cleared:

- the four ``QFile::remove(dest)`` sites in ``src/LibraryService.cpp`` remove a cache path they then
  write themselves with ``img.save()`` or an external frame grabber.  No live object owns those
  names, so there is no held handle to refuse the delete, and none of them copies onto a staged
  temporary name.
- the two publishers that *do* stage into a live ``QTemporaryFile`` - ``install()`` in
  ``src/library/OnlineAlbumArtService.cpp`` and ``installAutomaticPortrait()`` in
  ``src/library/ArtistImageService.cpp`` - publish with ``link()`` onto a destination they first
  prove absent.  ``link()`` fails with ``EEXIST`` instead of removing anything, so the sharing rule
  is never reached; they were not carrying the defect, which is also why store and portrait covers
  kept working while every folder cover disappeared.
- the ``QFile::remove()`` calls in ``src/sync/SyncEngine.cpp`` delete transcode output the engine
  owns outright.  The ``QTemporaryFile`` in that file is a *source* snapshot handed to the encoder,
  not a destination being published onto, so it is not this idiom.

One instance, one fix.  ``tests/win-compat``'s ``file_identity`` group already pins that ``st_dev``
and ``st_ino`` answer for the volume serial and file index, which is the surface the artwork
cache's "this image is its own source" refusal is built on; nothing there needed to change.

**A name rule that had to be written from nothing: the disc folder.**  Resolution now looks one
level up for the album's picture when the track's own folder is only a disc, and the rule for
*is this folder a disc* is ``^(?:cd|disc|disk|vinyl|lp|pt|part)[\s._-]*[0-9]+$``, case-insensitive.
It belongs in this audit because it is a naming rule that decides whether a folder's own name is
allowed to be an identity at all, and the exactness is the point: the prefix is required and the
whole name has to be accounted for, so an album actually named ``Vol 2``, ``LP`` plus a year, or
``Album 2024`` is not a disc folder and never inherits whatever its parent happens to contain.
Nothing is written under that rule - it only reads - so ``LibraryPaths::safeComponent()`` and the
export naming rule are untouched.

**A remote URL given the same discipline as a local path.**  The store rung now keeps only an
``https`` URL whose host is ``*.mzstatic.com`` before it will fetch anything, and the file that
result lands in is named from the album's own cache digest, never from anything in the response.
That is the founding line of this document read in the other direction: the first defect here was
treating a path as a URL, and the mirror mistake - letting a URL decide what to call a file - is
the one that turns a provider into a directory traversal.

**A rate limit that was filed as data.**  ``docs/WINDOWS_PORT.md`` carries the mechanics of the
music-store pacing in full, because it is transport, not naming.  What belongs here is the shape of
the mistake, which this document has seen several times already: the ladder could not tell "the
provider declined to answer" from "the provider has no such album", so a ``429`` became a permanent
"no music store carries this album" in the UI and a five-minute park became a missing cover.  A
refusal from the environment and a refusal from the truth must not share a representation, and when
they do, the bug outlives the outage.

Two harness findings, both of which made evidence that was not evidence.  PowerShell 5.1 resolves
``-Include`` against the *parent* of a ``-Recurse`` path, so ``<profile>\cache\art\*`` together with
``-Include *.jpg`` matches nothing at all: two art assertions in ``tests/win-usability`` had been
reading an empty list and passing.  And ``tests/device-music-import`` isolates its artwork cache
with ``qputenv("XDG_CACHE_HOME")``, which the Windows known folders do not consult, so its five
``device-album-art`` checks fail on this platform for that reason alone - reproduced against a
scratch build of the pre-fix ``AlbumArtService.cpp``, which failed the identical five.  Neither is
a Windows behaviour worth a shim; both are worth remembering the next time a gate reports a number.

**Not re-checked by this sweep**, unchanged from the eleventh: the four uncalled shell-invoking
sites in ``libzune``, the absence of a ``<longPathAware>`` manifest entry, UTF-16 without a
BOM in playlist files, and the narrower naming rule that playlist *exports* keep on purpose.
POSIX behaviour is unchanged by construction here and remains unverified on this machine.  Device
evidence is unchanged: one hand-tested Pavo Zune HD.

## Thirteenth sweep: a count of characters, not bytes (2026-10-24)

Called a sweep loosely.  This pass started from two reported symptoms - a Videos page with one clip
in a folder that demonstrably holds more, and a download that appeared not to have arrived - and
what it found was one rule about a database engine applied with the wrong unit in two adjacent
functions, plus a size floor filed under the wrong category.  It did not re-read the tree, so the
gaps the twelfth sweep left open stay open for the same reason.

**The rule.**  SQLite indexes TEXT by **character**, so the width bound into ``substr()`` over a TEXT
value is a character count.  ``LibraryDb::purgeFolder()`` and ``LibraryDb::countsUnder()`` both match
a watch folder and everything beneath it with ``path = ? OR substr(path, 1, ?) = ?`` - exact prefix
rather than ``LIKE`` on purpose, so an ``_`` or a ``[`` in a path cannot over-match - and both took
the width from ``QByteArray::size()`` after ``.toUtf8()``.  That is a byte count.  A Han character is
three UTF-8 bytes, so for a folder named in Chinese the window came out wider than the string it was
measuring, ``substr(filepath, 1, plen)`` returned the prefix *plus the next several characters*, and
an equality that can therefore never hold.  Only a row whose path was exactly the folder could match.

**What that looked like from the outside.**  Settings -> Library read ``0 photos`` beside a folder
holding five pictures, and releasing that watch folder logged ``purged 0 items`` while keeping all
five rows - a folder that would not leave the library, on a machine where the folder was open and
readable the whole time.  Nothing about the folder was wrong; the count asked about it was.  Both
sites take the width from ``QString::size()`` now, and bind the ``QString``, so the comparison is
character against character.

**Why it survived every gate, and why it is not a Windows bug.**  For an ASCII path the byte count
and the character count are equal, and every fixture
folder in every gate on either platform is ASCII.  The rule is SQLite's and applies identically on
Linux, where a Chinese folder name has always miscounted the same way.  It is recorded here because
it was *met* on Windows and the reflex is to file it under ``compat/win``; the fix belongs to the
database, and the only Windows-specific fact is that a Chinese folder name is ordinary on this
machine rather than a deliberate test.

**The gate.**  ``librarytool folders``, 20 checks, run in the ``tools`` phase of
``tests\win-usability``.  It pins both halves of the fault - a Chinese-named root counts the media it
holds, an ASCII root beside it counts its own and nothing else, releasing the Chinese-named root
takes exactly its own row - and the whole import-default rule that the same pass added, including
that a photo default and a music default coexist and that releasing a marked root falls back to the
earliest remaining folder of that kind.  Expectations branch on ``#ifdef Q_OS_WIN`` where a rule is
platform-specific, so one source gates both lines.  It exists as a tool phase because neither Windows
gate can execute a C++ rule directly, and these are ``LibraryDb`` rules.

**One size floor in the wrong category.**  ``VideoNaming::isSkippedVideoFile()`` applied the 50 MB
floor to every folder type that is not ``tv`` or ``anime``, which includes ``all`` - the type a user
gets when they point the app at one mixed folder - so such a root silently discarded every clip
under 50 MB while its own declaration promised to hold everything.  The floor is ``movies``-only now;
every other type, ``all`` included, keeps the 5 MB one.  Not a Windows difference either, and now
stated in the header comment rather than only in the ``.cpp``.

Device evidence is unchanged: one hand-tested Pavo Zune HD.

## Fourteenth sweep: three destinations, and a folder censused for what it had already indexed (2026-10-24)

Also not a re-read of the tree.  It started from a product complaint - a music download did not go
into the music folder, and a library is supposed to be one place - and it found one data-model
mistake, one derivation done in the wrong currency, one status string that could not be absent, and
one scan walk that had forgotten a guard its two sibling walks already carried.

**The rule.**  A library has *three* import destinations - one each for music, photos and video - not
one.  The twentieth sweep's ``watch_folders.import_default`` was one bit on one row, which cannot say
"this folder receives pictures and that one receives songs" the moment a folder typed ``all`` is in
the list: ``all`` qualifies for every kind, so it competes for all three at once, and marking a
picture folder cleared the bit on the ``all`` root that had been carrying music too.  Choosing where
songs go moved where pictures went.  Schema v11 keys the mark by kind - ``import_defaults (kind
PRIMARY KEY, folder_id)`` - and the migration copies *only* marks the user actually made
(``... WHERE import_default=1``), because seeding the remaining kinds would freeze today's fallback
into a choice nobody made.  The v10 column stays where it is, unread: dropping a column is the one
migration a downgraded app cannot forgive.

**The fallback had the wrong winner, in the direction the user cares about.**  ``ORDER BY
import_default DESC, id ASC`` let a folder that merely promised to hold everything win a kind for
which another folder had actually been declared, so a library with a real music folder still imported
songs somewhere else.  Declared beats generic now - ``ORDER BY CASE WHEN type='all' THEN 1 ELSE 0 END,
id ASC`` - which is the point of naming a folder for a kind.  The mark itself is honoured only while
the row's type still serves that kind: a mark cannot promise a kind no walk reads out of that folder,
and an import that lands in a folder no walk visits is a file the library owns, occupies disk and can
never browse.  That is also why ``watchTypesForKind()`` and ``importKindsForType()`` moved into
``src/library/LibraryTypes.h`` as one rule stated twice, next to the third copy of it in
``LibraryScanner::scan()``'s root sets.

**Status is derived by row id, not by path.**  Whether a row shows a status or a button was computed
by comparing a stored path against ``importRoot()``'s answer, so two rows naming one folder at
different spellings both claimed it, and it could not express the case the per-kind table makes
possible: a folder typed ``all`` that receives video but not songs.  ``LibraryService::reloadFolders()``
now asks for a row *id* per kind and spells the status from the kinds that row actually receives -
``default``, ``default · video``, or nothing at all.

**And that "nothing at all" was the last defect.**  The label for a partial default was built as
``"default · " + kinds.join("+")``, so a row receiving *nothing* got ``"default · "`` - a truthy string
- which made "no status" indistinguishable from "blank status" to the QML fallback and to any gate
counting the rows.  A row that receives nothing returns an empty string now, and the usability gate
counts the absence: ``only a default row carries a default status string`` compares
``wfDefaultLabels`` with ``wfDefaults`` and ``wfDefaultAgree`` with the row count, off counters added
to the ``--qa-probe`` report.

**The scan walk that complained about its own root.**  Every walk hands its visitor every file under
its root, discards the kinds the root was not typed for, and reports that count, because a folder that
shows no videos is otherwise indistinguishable from an empty one.  The video and photo walks check
``rootType.value(root) != "all"`` first - a root in all three root sets excludes nothing, so counting
it is a lie.  The music walk did not check, so it filed the videos and pictures inside an ``all`` root
as hidden, and Settings offered to set that folder's type to the type it already had.  Guarded, and
pinned by a tool because no screenshot of a one-folder library can show three kinds in one tree.

**The gates.**  ``librarytool folders`` grew from 20 checks to 35 and now rewinds a database to the
exact v10 shape - marked column present, ``import_defaults`` absent, ``user_version`` 10 - and reads
the upgrade back: the marked folder keeps the kind it had, unmarked kinds keep falling back, nothing
is invented, and the library is v11 again.  The rewind drops ``import_defaults`` with the v6 columns
and the v9 fingerprint, since a fixture that still answers the v11 probe is not a v2 fixture.
``librarytool census`` is new, 10 checks, one real ``LibraryScanner`` pass over a music, a photos, a
movies and an ``all`` folder: exactly one folder complains about the clip, the ``all`` root says
nothing, and everything inside the ``all`` root is indexed rather than counted.  Both run in the
``tools`` phase of ``tests\win-usability``; the counters for the pass are ``201 checks, 0 failures,
0 skips`` from ``tests\win-compat`` and ``launches=18 checks=132 failures=0 skips=0 killed=0`` from
the usability gate.

**Deliberate, so it does not get "fixed" twice.**  The v10 ``import_default`` column is left in place
and unread.  A mark is *not* deleted when a folder is retyped into a kind it no longer serves - it
stops qualifying, and if the type comes back the user's choice comes back with it.  Device evidence is
unchanged and it matters here more than usual: the destination for ``music`` on this machine moved
from ``Music\Zuuned Imports`` to the declared music folder, and no player has written into the new one.

Device evidence is unchanged: one hand-tested Pavo Zune HD.

## What is proven, and by which gate

- `tests/win-usability/watch_folder_repair.py` seeds four roots - a `/E:/` scar, a
  `/e:/` scar whose repaired spelling collides with a healthy row, that healthy
  row, and a clean path - then opens the database with a real `librarytool scan`
  and asserts the survivors.  The collision row is the interesting one: without
  the drive-case fold it repairs to `e:/Scar Duplicate`, a *different* UNIQUE
  value, and two identities for one folder survive.  On POSIX the same four
  strings are ordinary absolute paths and the gate asserts they are untouched.
- The `fixture` phase of `tests/win-usability` scans one root per script into its
  own database, so a root that works cannot mask one that does not - the exact
  way the Chinese root used to hide the other three.
- The `file_identity` phase of `tests/win-compat` is the contract the artwork cache
  identity check depends on, asserted against the shim itself rather than assumed
  from the Win32 documentation.
- `tests/win-compat` and `tests/win-usability` are source and runtime gates;
  neither opens a USB handle.  The compat gate is currently 201 checks.
- `librarytool folders` is 35 checks and the only gate that reaches the watch-folder rules in
  `LibraryDb` directly: the character-width prefix counts above, the release of a Chinese-named
  folder, which folder a download of each kind is written into, that two marks cannot disturb each
  other, and that a library written against the old one-flag schema keeps the destination its user
  chose through the v10 to v11 upgrade.  It runs in the `tools` phase of
  `tests\win-usability` beside `names`, `paths`, `vanish`, `customize` and `cover`, on the same
  reasoning as `librarytool paths` - a header-only or database-level rule is unreachable from a
  QML launch, so the tool is where it gets pinned.
- `librarytool census` is 10 checks and the only capture the scan census has: which folder may
  report the files of a kind it does not index, and the one type that may report nothing because it
  indexes everything it holds.  It runs one real `LibraryScanner` pass over a real tree holding all
  three kinds, which is more than any page screenshot with a single music folder can show.
- `librarytool videofloor` is 19 checks and the only capture the video size floor has: which folder may
  refuse a small video, the one folder that may not (the download destination, so a pull the user asked
  for cannot be discarded as debris), and the note the scan owes for every file it did refuse.  Nothing
  about the rule is Windows-specific, so it is pinned once, on both platforms, in the tool rather than in
  a QML launch.
- The profile-isolation pair in `tests\win-usability` reads the *real* profile before and
  after the run, so it gates the gate: it is only valid while nothing else is using that
  profile.  One run taken with a `build\zuuned.exe` left open from an earlier check failed on
  `the real profile gained no file while the gate ran` and `Qt pipeline cache stayed out of
  the real profile`, while its own sibling check `no process this gate started wrote into the
  real profile` passed - the running application's own `qqpc_opengl` cache entry, 4,120
  bytes, and a true observation about the wrong subject.  With the application closed the
  same gate returned `launches=17 checks=95 failures=0 skips=0`, `OVERALL=ok`.  Read that
  failure as designed rather than as flaky: it is the check in the suite that would notice a
  launch escaping its profile, and it noticed.
  The pair failed again on 2026-10-22 for a cause that is real but is not this
  application's: one new file,
  `AppData\Local\Zuuned\Zuuned\cache\_qt_QGfxShaderBuilder_6.11.0\1088d1721e08fe3ecb3414ce01b8226540ad6d3e.qsb`.
  It is not the pipeline cache `src/main.cpp` redirects into the profile.  It belongs to
  the `QGfxShaderBuilder` singleton inside `qtgraphicaleffectsprivateplugin` - Qt5Compat
  GraphicalEffects, whose `FastBlur` `qml/EnvironmentBackground.qml:60` uses as a layer
  effect - and that singleton asks `QStandardPaths::CacheLocation` for itself.  Its one
  environment variable, read out of the plugin's strings, is
  `QT_GFXSHADERBUILDER_REFRESH_CACHE`, which forces a rebuild rather than a redirect, so
  there is nothing for `src/main.cpp` to set the way it sets the two
  `QSG_RHI_PIPELINE_CACHE_*` paths.  A third Qt-internal cache therefore sits outside
  `ZUUNED_PROFILE_DIR`, and it stays invisible until some variant has to be compiled for
  the first time: the folder holds 29 files today, 28 of them created on 2026-10-21 and
  one at 08:36:47 on 2026-10-22, and a file that already exists is never rewritten - which
  is why every earlier run of this gate saw nothing.  Read the passing sibling
  check honestly too - `no process this gate started wrote into the real profile` matches
  the process id inside a file *name*, so on a content-hash-named cache file it cannot
  clear the gate's own launches.  Recorded and left alone: keeping it in the profile costs
  either the GraphicalEffects import or the profile indirection, and what it costs a user
  is a warm shader cache, never data.
- The `compact` phase sizes the window through `winshot --move`, which speaks device
  pixels, while the ribbon breakpoints are logical pixels.  On a desktop where the
  window happens to open on a 150 per cent display the same request lands below the
  breakpoints, and four ribbon checks fail while the application does exactly what it
  was told.  Measured twice on 2026-10-21, eleven minutes apart, same executable:
  `client 1384x1181` with the sequence
  `compact-retired -> roomy-ribbon -> compact-retired -> roomy-ribbon` on the 100 per
  cent screen, and `client 1650x1164` for a request of `1400x1220` - which is
  1100x776 in the application's own units - when the window opened on the scaled one.
  Nothing in the application is wrong in the second run; the harness asked for a size
  it did not get.  Multiply the request by `GetDpiForWindow()`, or keep the
  instrumented window on a 100 per cent display, and the phase stops depending on
  which screen Qt chose.  Until then a compact-phase failure is a question about the
  desktop, not about the ribbon.
  Measured again on 2026-10-22 with the arithmetic written out instead of inferred: this
  desktop answers `\\.\DISPLAY6` primary at logical 2560x1440 - 100 per cent, where a
  roomy window fits - and `\\.\DISPLAY1` at logical 1707x960, which is a 2560x1440 panel
  at 150 per cent.  A roomy window needs 1100 logical pixels of height, so on the scaled
  screen the size that phase asks for cannot exist at any request: the app clamps to its
  own 1100x776 logical minimum, which is 1650x1164 in the device pixels `winshot --move`
  speaks.  Four ribbon checks fail on such a desktop and nothing in the ribbon is wrong.
- `build\librarytool.exe paths` is 74 checks over the helpers above (67 at the
   seventh sweep, 74 after the genre rule), and
  `names`/`customize` still pass on the same build.  All three ran here, against the
  executables produced by `cmake --build build --clean-first`.  Eleven of those 67
  checks are the fourth sweep's walk above, and they build their own junction rather
  than assume one: `mklink /J` inside a `QTemporaryDir`, which is the only reason the
  case is testable from an unprivileged account at all.  They assert that the three
  songs really inside the root arrive exactly once, that the link back into the root is
  counted and never descended, that a link *out* is followed once and under the link's
  own path, that the root still certifies complete with a cycle inside it, and — on
  POSIX — that a symlink to a directory is still not entered, so the guard cannot
  quietly become "follow everything".  End to end, on disk, the same shape: three
  copies of one MP3 plus `loop -> music` scanned to `3 tracks added` and one
  `[scan] ...: 1 folder(s) skipped as already walked` line, where a naive recursion
  over that tree counts 192 entries.
- **"The executable was relinked" is not evidence, and that is a finding of this
  sweep.**  The Ninja rules carry `deps = msvc`: Ninja learns a translation unit's
  includes by parsing the `/showIncludes` echo from `cl`, against the literal English
  prefix.  On this localized host the notes arrive as `注意: 包含文件:`, nothing matches,
  and `ninja -t deps CMakeFiles\zuuned.dir\src\LibraryService.cpp.obj` answers `#deps 0` -
  not one header dependency recorded for any object in the build.  Editing a header-only
  helper above and rebuilding therefore recompiles nothing, relinks anyway, and prints
  enough AUTOMOC steps to look entirely honest, which means a gate can pass against a
  binary that predates the very fix it is supposed to prove.  Every counter in this file
  comes from a `--clean-first` rebuild instead; `BUILDING.md` has the diagnosis and the
  habit.
- `tests\win-usability` ran against that `build\zuuned.exe`: 17 launches,
  94 checks, 0 failures, `OVERALL=ok`, including the Han-spelled profile folder and
  the two profile-isolation checks that compare the real profile byte-for-byte
  before and after.  One qualification, because the gate's own `SCOPE` line only
  speaks for the harness: a Zune HD was plugged in while it ran, so the application's
  own hotplug auto-connect opened and read the device inside every isolated profile.
  Those profiles hold no library and no transfer queue, so nothing was written to the
  player, but "no device handle" is true of the harness and not of the app.
- A later confirmation pass on the same executable reproduced all of it -
  `184 checks, 0 failures, 0 skips`, `launches=17 checks=95 failures=0 skips=0`,
  `paths` still at 28 - and is written up next to the device round trip in
  `docs/WINDOWS_PORT.md`.  One launch of that gate failed there and passed a clean
  re-run because a process outside this repository was holding the Windows
  clipboard open.  A GUI gate runs on somebody's desktop, so a single white-frame
  launch is worth a re-run before it is worth a bisect.  The eighth pass
  (`docs/WINDOWS_PORT.md`) reproduced the same class of flake with the cause inside
  this repository: running the photo harness at the same time as the gate cost two
  timed checks - `the QA queue reached the player and the tone played` saw 13 `[qa]`
  reports and none of them said `playing=true`, and one launch printed no splash
  report at all - while the same executable answered
  `launches=18 checks=99 failures=0 skips=0` and `OVERALL=ok` the moment nothing else
  was running.  Run two gates one after the other, not at once.
- Device facts are in `docs/WINDOWS_PORT.md` and are hand-tested, not gated.

## Not re-checked by this sweep, and what it would take

The first two bullets below were open findings when this page was written and are closed
now; `docs/WINDOWS_PORT.md` carries the before/after measurement for both.  The rest are
left as findings on purpose, because the honest fix is bigger than the defect and
pretending otherwise would put an unverified claim in the tree.

- **A fourth entry point sat in that bullet, described as uncalled. It is called,
  and it was broken.**  `zune_arm_photo()` shelled out the same POSIX way the
  photo-thumbnail branch just lost by, and it sits on the live photo transfer:
  `src/DeviceWorker.cpp:736` -> `zune_smuggle_photo()` -> `zune_arm_photo()`
  (`libzune/src/photo.c:409`), whose NULL makes that caller send the untouched
  original.  `cmd.exe` reads `'C:\My Pics\a.jpg'` as two arguments and
  `2>/dev/null` as a redirect into a folder nobody has, so on Windows every
  prepare failed and the transfer still reported success.  It now quotes the way
  `thumbnail.c` does - `shell_quote_dup()`, `ZUUNED_TMPDIR`, `>nul 2>&1` - and
  refuses rather than truncating a command line it cannot fit; the before/after
  measurement is in `docs/WINDOWS_PORT.md`.
- **A shell-out speaks the ANSI code page, and the app speaks UTF-8.**  Quoting
  alone did not make either photo shell-out work: `system()` runs `cmd.exe`, which
  decodes its command line with the active ANSI code page, while `src/DeviceWorker.cpp`
  hands `filepath.toUtf8()`.  A name outside ASCII therefore reached `ffmpeg` as
  mojibake - measured, not inferred, on this machine's cp936: the same picture in a
  Han-spelled folder prepared fine as UTF-8-invisible ASCII and failed the moment
  the app's own byte convention was used.  8.3 short names are no help, because
  NTFS does not hand one out for a directory the code page cannot spell (`dir /x`
  shows none for `照片`).  `zuuned_narrow_path_for_shell()`
  (`compat/win/wincompat.c:1580`) now re-spells the path into the bytes that
  decode back to the same file, on the same UTF-8-first rule `decode_path()` uses
  when opening it, and refuses rather than substituting `?` when the code page
  genuinely has no spelling for the name - a substitute names a different file,
  which reads back as a missing picture.  `zune_arm_photo()` narrows its input
  (`libzune/src/photo.c:361`); `zune_grab_photo_thumb()` narrows the cache path it
  writes (`libzune/src/thumbnail.c:155`).  Neither narrows a `ZUUNED_TMPDIR` path,
  because `wide_to_narrow()` already wrote that one in the code page and a two-byte
  code-page sequence is very often valid UTF-8, so re-spelling it would invent a
  different directory name.
- **Three libzune entry points still build POSIX command lines, and none of the
  three is called.**  `zune_snap_thumb()` (`libzune/src/util.c:379`) shells out to
  `ffmpeg` with a `/tmp` path and `2>/dev/null`; the `popen()` sites in
  `libzune/src/transcode.c` carry a hardcoded `/opt/homebrew/bin/ffmpeg`.  Both
  repeat the single-quote and `/dev/null` assumptions the photo-thumbnail branch
  above just lost, and `transcode.c` adds an absolute macOS path no Windows `PATH`
  satisfies - `/FIposix_shim.h` does make its
  `popen` resolve to `_popen`, which is exactly how it would fail: the shell starts,
  the program is not there.  They are public API with **no callers** in `src/`,
  `tools/` or `tests/`.  The app owns its `transcode/` layer and probes media
  in-process instead, including the one probe that is live: `zune_probe()`, called
  from `src/library/LibraryScanner.cpp:185`, compiles here as the `USE_LIBAV`
  in-process libavformat variant (`libzune/src/util.c:623`, with the define at
  `CMakeLists.txt:181`) - the `fork()`/`execl()` variant beside it
  (`libzune/src/util.c:712`) is not compiled on Windows at all.  Patching the shell
  sites would mean patching code nobody has run here; whoever wires one up meets
  this first.
- The asymmetry the fourth sweep started on is still true and is now worked around
  rather than depended on: `QFileInfo::isSymLink()` does not report a Windows junction,
  although the shim's own `lstat()` classifies `IO_REPARSE_TAG_MOUNT_POINT` as
  `S_IFLNK` (`compat/win/wincompat.c:249`).  The library walk no longer asks that
  question at all — it asks who this folder is — and the tenth sweep took the same question out of the last
  hand-rolled walk in `src/`, so nothing under `src/` asks it any more.  Making a junction needs neither
  privileges nor developer mode, so "nothing real contains one" was never the
  assumption it looked like.
- **`safeComponent()` bounds nothing but spelling.**  A component longer than 255
  characters - legal on a Zune, since MTP has no such limit - still fails `mkpath()`
  on Windows and the save still reports a plain failure.  Truncating a name is a
  visible change to a user's folder layout and needs a collision rule with it, which
  is a bigger decision than this sweep.

## Known gaps, on purpose

- The audiobook folder import no longer owns a walk: `previewBookFolder()` goes through
  `LibraryScan::walkFiles()` the way a watch folder does, so a junction there ends in a
  folder the walk refuses to enter rather than a phantom path.  What is left of this item
  is a gap in evidence and not a hole in the rule: no Windows phase imports an audiobook
  folder, so the routing stands on two static checks and the `mklink /J` fixture in
  `librarytool walk`, not on an import that ran.  See the tenth sweep.
- The spoken worker's close is bounded, and what is left of it is on purpose: the last
  resort in `Zuuned::drainThread()` has never fired here, and no Windows gate plays a
  podcast or an audiobook, so neither `[spoken] last position not saved` nor the resume
  point it protects has ever been produced by a test - the store the `spokenclose` phase
  closes over answers `rows=0`, which is precisely why that deadline is unexercised.
  Proving it needs a phase that plays a spoken item, which `docs/WINDOWS_PORT.md` carries
  on its list; the shape of the close is gated, the write inside it is not.
- Case-insensitive dedup below the drive letter: see the reasoning above.
- UNC roots are produced for `file://host/share` on Windows and are untested
  against a real share; the code path is unit-probed, not device-probed.
- `~/Music` in a log is the redactor, not a mangled path: see
  `src/diagnostics/Redactor.cpp`. It was the most convincing false lead in this
  investigation.
- `zune_usb_devices` and the user-name redaction were both Linux-only facts until the
  tenth pass.  `src/diagnostics/SupportReport.cpp` built the USB array on Windows and
  inserted it only in the Linux branch, and the redaction context read `USER` at
  `src/diagnostics/DiagnosticsService.cpp:51`, a variable Windows does not define, so
  `src/diagnostics/Redactor.cpp` had no name to fold and exported it verbatim.  Both
  are fixed and each was measured by hand against the real class: a report that now
  names the attached Zune, and one line that redacts to `[user]` where it used to print
  the account name.  Neither is pinned by a gate.  The two diagnostics suites are bash,
  and neither file links into a CLI tool - they reach a build only through the
  `zuuned` target.
- Two Linux-only facts a Windows support report cannot state.  `graphics_devices`
  is read from sysfs and is empty here - nothing enumerates WMI or DXGI in its
  place - and the `st_uid == geteuid()` owner checks in
  `src/diagnostics/SessionLog.cpp` are vacuous, because the shim reports uid 0
  for every file and `geteuid()` answers 0 (`compat/win/wincompat.c` says why).
  Neither loses or corrupts data; both simply report less than Linux does.
- One Windows device, one family.  Every device statement here is a Pavo Zune HD
  statement.

## Audit round of 2026-11-16, closed 2026-11-17: no shell in front of a name you did not choose

`docs/audits/CODE_AUDIT_2026-11-16.md` holds the findings as they were written and
`docs/audits/CODE_AUDIT_RESPONSE_2026-11-16.md` answers them one at a time, including
the finding that turned out not to be true and the one that was deliberately declined.
Only the parts of that round that are statements about this platform belong here, and
they belong here because one of them closed a gate that had been open since the first
Windows thumbnail.

**Percent is not quotable, so quoting was never the fix.**
A-09 was written as "`%VAR%` expands inside the quotes, and the quoting helper only
handles `\"`". Reading it against `cmd.exe` semantics says more: `cmd.exe` expands an
environment variable inside double quotes exactly as it does outside them, doubling a
`"` protects the argument from the program being run and not from the shell that reads
the line first, and there is no escape for percent at all - `%%` is a batch-file
artifact, not an escape on a command line. A Zune photo named `%PATH%.jpg`, or a video
named `%TEMP%.wmv`, therefore reached the encoder under a name nobody chose. Nothing a
quoting function emits makes a device-supplied file name inert, because the expansion
happens in a parser this code does not control. So the fix is subtraction: on Windows
there is no shell in front of ffmpeg any more.

**`zuuned_run_no_shell()` is one `CreateProcessW` with an argv it builds itself.**
Declared `compat/win/posix_shim.h:505`, implemented `compat/win/wincompat.c:1680`. It
widens each argument with `MultiByteToWideChar(CP_ACP, ...)` and writes the command
line with the quoting rules `CommandLineToArgvW` reverses - wrap in `"`, double an
inner `"`, and double a run of backslashes only where it abuts a quote - then starts
the program directly. Three details are load-bearing rather than incidental:

- The bytes are code-page bytes, not UTF-8, because that is what `cmd.exe` decoded
  before. A UTF-8 argv here would quietly change which file a name means, in the middle
  of a fix whose entire claim is that nothing else moved.
- One inheritable `NUL` goes behind all three child standard handles, which is where
  the old `>nul 2>&1` put the encoder output and what makes a child that reads standard
  input see end of file instead of a console it has no business touching. If even `NUL`
  cannot be opened the run still happens with inherited handles - the old behaviour -
  rather than refusing a picture over a handle.
- `CREATE_NO_WINDOW`. A GUI process has no console, so `system()` allocated a fresh one
  for `cmd.exe` and the user watched a window flash for every photo transferred. The
  wait stays unbounded because `system()` was unbounded; these calls only happen on a
  worker, and inventing a timeout would be a second behaviour change in one fix.

It returns the child exit code, so callers judge exactly what they judged before, or -1
when the program could not be started.

**Three call sites, one helper deleted, and one set of quotes that stays.**
`libzune/src/photo.c:432`, `libzune/src/thumbnail.c:136` and `libzune/src/util.c:391`
now build an argv array and call it. `shell_quote_path()` - the Windows half of the
2026-10-21 "double the quotes" fix, which this round found to be unfixable as stated -
is deleted; `shell_quote_dup()` is `#ifndef _WIN32` and says in its comment that
Windows having no version of it is the finding, not an omission; `SHELL_QUIET` is back
to being the POSIX `2>/dev/null` alone. The `scale='min(480,iw)':'min(480,ih)'` filter
keeps its single quotes on purpose: ffmpeg hands that string to its own filter-graph
parser, which is what the quotes protect the commas inside `min()` from, and `cmd.exe`
never treated a single quote as quoting either, so the encoder already received them
this way. The POSIX and macOS command strings are byte-for-byte what they were.

**Measured on a player, and it is the first Windows evidence that branch has.**
The 2026-11-17 `tests\win-usability` run - 19 instrumented launches, each in its own
profile, each auto-connecting to a Zune HD that holds photo objects - logged
`[libzune] resized full file for thumbnail` 51 times, from 17 of those launches, and
`all photo thumbnail methods failed` zero times. That line is emitted only at
`libzune/src/thumbnail.c:159`, which `_WIN32` reaches only when
`zuuned_run_no_shell()` returned 0. The single-quoted command line that this file
already recorded as failing on `cmd.exe` on every call therefore worked 51 times on
real device photos - the ffmpeg resize branch is now
proven on Windows, where before this sitting its Windows behaviour was only ever
compiled. ffmpeg itself is not bundled and stays a download (see `BUILDING.md`); its
absence is what makes these methods fail as a group, which is the pre-existing
behaviour, not something this change introduced. The separate
`all thumbnail methods failed` lines that do still appear come from
`libzune/src/thumbnail.c:69`, which never shells out at all; they are the known
`get_representative_sample` defect and are untouched.

**Not done on purpose, so nobody mistakes them for oversights.**

- `libzune/src/transcode.c:99`, `:161` and `:286` still call `popen()`. Nothing in the
  tree calls those functions - `transcode/libav_transcode.c` replaced the ffmpeg shell
  outs years ago - and they are the entire source of the three C4047 warnings that show
  up on a clean build. Deleting dead code is a separate change from closing an audit.
- Child output goes to `NUL`, so a failing encoder is silent. That mirrors `>nul
  2>&1`, which was equally silent, and it is the first thing to lift when a transfer
  needs debugging rather than a defect to fix now.
- Percent still goes out unescaped in the one place a shell is still involved: nowhere.
  The live `system()` calls in the tree are now only inside `#else` branches -
  `libzune/src/photo.c:472`, `libzune/src/thumbnail.c:154`, `libzune/src/util.c:399` -
  plus the commented-out block in `libzune/src/device.c` and the dead `popen` sites.
  Verified by reading to the end of each conditional rather than by grepping a line.

**Counters for the relinked build carrying all of this** (`cmake --build build
--clean-first`, 352/352): `mtpframingtool` 1285 checks 0 failures (was 1245 - the new
bounds have cases), `librarytool paths` ALL PASS 73, `tests\win-compat` 204 checks 0
failures 0 skips, `tests\win-usability`
`launches=19 checks=157 failures=0 skips=0 killed=0` `OVERALL=ok`, `tests\privacy`
19 checks 0 failures, i18n audit `AUDIT=ok`, and `devicetool` `VERDICT=agree` with the
devnode at `os_problem=0`. Configure prints `Zuuned hardening: warning level default,
warnings as errors OFF, control guard OFF`, so the command line a released artifact is
built with appends nothing.
