# AGENTS.md — AI Assistant Guide for Zuuned Modern

**Local build handoff (user decision, 2026-10-13):** after a completed
user-facing local build, restart only Zuuned with the fresh executable, preserving
the current profile and workspace. Do not interrupt an active device sync; defer
the restart until it finishes. Never close the terminal, Codex or desktop session.

**Windows port, 2026-10-20:** one source tree now also builds and runs natively on
Windows with MSVC (Qt 6.11 `msvc2022_64`, vcpkg plus a `ZUUNED_WIN_DEPS` tree; see
the Windows section of `BUILDING.md`). `compat/win/` builds once as
`zuuned_wincompat` and is force-included through `posix_shim.h` so libzune and the
transcode C layer keep their POSIX surface without per-file `#ifdef`s; D-Bus/Mpris
and the POSIX-only install rules are skipped on Windows; `regex.h` maps onto PCRE2.
Gate: `tests/win-compat/run.ps1`, 171 software checks.  Superseded the same day by
the Windows hardware checkpoint below: WinUSB binding, enumeration, hotplug and the
music library read have since been done on Windows against a Zune HD.  What this
sentence forbids still stands - transfers, readback and every write are Linux
evidence only, so no hardware claim may be carried across platforms.
Runtime fixes proven on Windows: `LibraryDb` stages its pre-migration snapshot
through SQLite's own backup API and publishes it with `QFile::rename`, because a
live `QTemporaryFile` handle made that rename fail with a sharing violation and
blocked any library that still needed migrations; the `librarytool customize`
legacy fixture now rewinds the v6 probe columns, v7 photo-album tables, the v9
fingerprint and the v10 import default, so the whole v0/v2 to v10 migration chain is exercised there
(`ci/check.sh` itself stays Linux-only). `fd_handle()` consults the CRT
descriptor table before reading a number as an NT handle: Qt opens a `QFile`
through `_wopen()`, so `QFile::handle()` is a descriptor even though its
documentation says native, and the watermark that used to stand in front of the
table only counted descriptors `compat/win` handed out - every Windows
audiobook import died at `Audio source is not a regular file` because of it.
The CRT invalid-parameter guard that makes asking the table safe now goes in
lazily from `fd_handle()`, because a `.CRT$XIB` initialiser is folded into
`.rdata` by the linker (LNK4254) and never runs. Settings are an ini in
`%APPDATA%\Zuuned`, not the registry, with a one-time import of any older
`HKCU\Software\Zuuned\Zuuned` values, so a Windows profile is as inspectable as
`$HOME` on Linux. For isolation `ZUUNED_PROFILE_DIR` names one folder that owns
every profile file - library and spoken databases, artwork and thumbnail caches,
session logs, the single-instance lock and the settings ini - because
`QStandardPaths` asks the shell known folders and ignores
`LOCALAPPDATA`/`APPDATA` overrides, so an instrumented Windows launch would
otherwise read and write the real profile and lose the log-capture lock to the
instance already running. Only locations a profile owns are redirected: the
user's music, video, picture and document folders plus the temp dir and home pass
through to Qt, and with the variable unset every call is exactly what Qt returns,
so the Linux line and its gates keep their behaviour. The implementation is
header-only `inline` on purpose because `src/library/LibraryDb.cpp` is also
compiled stand-alone by Linux gate scripts, which a separate translation unit
would break.
Three more Windows faults are found and fixed. CRT `_access()` accepts only mode
0, 2, 4 or 6 and reports bit 1 as an invalid parameter, which runs the invalid
parameter handler and `__fastfail()`s the process instead of setting `errno`, so
`X_OK` is 0 in `compat/win/posix_shim.h` and
`LibraryScanner::accessibleDirectory()` lets the shim's UTF-16 `opendir()` prove
read-and-traverse access rather than asking `access()` about a user folder; as it
was, adding any music folder on Windows killed the app on the spot.
`src/library/AudioCopyFingerprint.h` gives FFmpeg `path.toUtf8()` instead of
`QFile::encodeName(path)`, because FFmpeg names are UTF-8 on Windows (as in
libzune's in-process probe) while the local 8-bit codec here is GBK, which is the
difference between a song with a Chinese name having copy evidence or not. And on
this locale-936 machine `cl.exe /showIncludes` emits its note in UTF-8 while
Ninja matches only the English text, so `ninja -t deps` reports `#deps 0` and a
header-only edit silently links a stale object: force a full rebuild, with
`--clean-first`, after any header-only change until that changes.
The Windows usability gate is finished and passing:
`pwsh -NoProfile -ExecutionPolicy Bypass -File tests\win-usability\run.ps1` reports
`launches=18 checks=99 failures=0 skips=0 killed=0` and `OVERALL=ok`. It launches the
built app once per page (music, videos, photos, podcasts, audiobooks, and the device
page with no device attached), the five settings tabs, the header pinned at 0 and at
1, a queue played through the compact island after a real `winshot --move` resize, the
four command-line probes, a silent `playertool` seek, a decode pair that has to name the
pixel format the decoder really returned, a muted
audible `playertool` run that has to bind WASAPI, single-instance refusal, and a
before-and-after comparison of the real profile, plus a podcasts page closed on a seeded
profile so the spoken worker exit is measured, not assumed. `-Only` takes a comma list
and pairs a phase with `fixture` because the scratch tree is rebuilt each run. It is
software only: it opens no USB device handle and calls no libzune function, so no run
of this gate has ever touched a Zune: **the Windows wire evidence is the hand round
trip recorded below, not this harness**. The device page is certified only in its
healthy no-device state, and `what is plugged in` reads presence without touching the
device. `winshot`
captures what a live window actually paints, `--qa-probe` makes `qml/Main.qml`
report the page, onboarding and island state it actually chose, and
`inspect_scan_db.py` reports track, fingerprint, probe-failure and code-point
counts from a scan database.
Two harness faults found there are worth remembering because both blame the
app for a bug in the test: PowerShell 7 caches
`Process.MainWindowHandle` after the first zero read, so an instrumented launch
whose property was polled during the splash looks like a windowless hang (30 of 30
healthy launches looked that way), and `Process.Kill()` leaves the caller reading an
exit code the application never chose, so a destroyed app looks like it exited
cleanly.  The window is now resolved by process id through `winshot --pid`, which
route answered is recorded, and a forced kill is reported as `124`.
Four more findings from finishing it. Qt's automatic shader/pipeline cache asks
`QStandardPaths::CacheLocation` from inside Qt6Quick, so it ignored
`ZUUNED_PROFILE_DIR` and wrote into the real profile; `src/main.cpp` now sets
`QSG_RHI_PIPELINE_CACHE_LOAD`/`_SAVE` inside the profile whenever that variable is
set, and a brand new profile logs one expected
`Could not open pipeline cache source file` warning before Qt has written the file.
An `HWND` above 0x7FFFFFFF arrives negative from `Process.MainWindowHandle` and
positive from `winshot --pid`, so widening it must mask rather than cast through
`[int]`, and only a run that needed the pid scan could ever show that. The
single-instance lock lives *inside* the profile, so a refusal test that hands the
rival a second profile proves nothing but the test; the rival has to name the same
profile and the same suffix. And the gate now carries a `trap` that closes every
instance it started, because a harness that dies mid-case otherwise leaves a live
Zuuned holding a profile folder the next run intends to delete.
Unrelated open item: the EnvironmentBackground spray halo shows a white-ring
artifact under the Windows blur path; that file is still identical to HEAD.

**2026-10-20 Windows device presence:** libusb on Windows cannot see a Zune that has
no driver bound — a driver-less player is enumerated by the PnP manager with
`CM_PROB_FAILED_INSTALL` (problem 28) and appears nowhere in
`libusb_get_device_list()`, so "is the device there?" had no honest answer on this
platform. `src/DevicePresence.{h,cpp}` asks both sources: libusb first, then, on
Windows only, `src/DevicePresenceWin.cpp` through `cfgmgr32` when libusb is blind
(`src/DevicePresenceUnix.cpp` keeps Linux answering from libusb alone, with the
existing wording byte-identical). `UsbWatcher` gained a Windows-only `pollPresence`
pass emitting `zuneBlocked(detail)`/`zuneUnblocked()`; a blocked sighting sets
`DeviceService.devicePresent` and the status "Zune plugged in, but no driver zuuned
can use" and deliberately schedules no breach. The fix-it card and Settings → Device
now read `DeviceService.usbAccessTitle/Detail/Command/Fixable` instead of the
udev-specific properties, so Windows names WinUSB and prints the exact device id to
pick in Zadig where the Linux one-click command used to be; `usbAccessFixable` is
`udevRulesApply`, and nothing here pretends to install a driver. `devicetool.exe`
(`src/devicetool.cpp`) is that same question as one command, answering
`agree`/`blocked`/`absent`/`contradictory`/`unknown`, and the gate phase
`what is plugged in` (`-Only device`) holds it against `Win32_PnPEntity` for instance
id and problem code without ever asserting that a Zune *is* attached — on this
machine the honest verdict is `blocked`, at problem 28, with both APIs naming the
same instance. Seeing is not driving: WinUSB binding through Zadig has still never
been done here, and **no Zune has ever been opened, transferred to or read back from
on Windows.**  Superseded later the same day by the hardware checkpoint below.  The
blocked wording stays right for every Windows machine where nobody has installed a
driver yet, which is what a tester normally meets, so it stays.

**2026-10-20 Windows path encoding checkpoint:** `src/spoken/SpokenStore.cpp` handed
`sqlite3_open_v2` the `QFile::encodeName()` of its database and nothing else in the app
does that: SQLite's Windows VFS decodes path bytes as UTF-8 and never consults the ANSI
code page, so on this GBK machine a profile folder with a Han character in it (a Chinese
Windows user name is enough) made the store open a *different* file than the folder Qt
had created. Measured before the fix: `spoken/` existed, `spoken.db` did not, and every
podcast and audiobook operation failed with "Cannot open spoken-media database", while
`library.db` beside it opened fine because `LibraryDb` passes `toUtf8()`. It passes
`toUtf8()` now - the same bytes on Linux, where paths already are UTF-8. Two gate phases
cover it and the encoder from here on: `cjkprofile` launches twice into one profile
folder named from code points and reads the spoken store's schema back through
`tests/win-usability/inspect_spoken_db.py`, and `encode` makes the transcode layer
produce rather than probe - FFmpeg-built H.264 source through profile 0 (ASF magic
`30 26 b2 75`, source size, duration intact) and profile 2 (`ftyp`, 480x272), plus the
LAME leg judged by probing the mp3 for the length of the tone. `decode` then decodes a clip twice and reads the pixel format off
the decoder rather than off its device context: forced software answers one decoder, no
accelerator and `yuv420p`, and the default answered `cuda` on this machine, so NVDEC does
engage on Windows. Full gate now
`launches=17 checks=94 failures=0 skips=0 killed=0`. Two things deliberately left
alone after being checked, so do not "fix" them again: `compat/win/` already decodes a
narrow path as UTF-8 then ANSI, and CRT `access()` is ANSI-only, which is what the
`QFile::encodeName()` callers in `LibraryScanner`, `ArtworkPipeline`, the artwork
services and `SupportReport` correctly hand it. And Qt's own shader/pipeline cache
cannot open its file when the profile folder is outside the local code page (all
sixteen ASCII profiles of a run write `qqpc_opengl`, neither Han-named one does); that
byte-path open is inside Qt, the gate records it, and it costs a warm cache, not data.
**Still no Zune hardware of any kind on Windows**: the attached Zune HD sits at PnP
problem 28 with no driver bound, `devicetool` answers `blocked`, and nothing here has
opened a device handle.  Superseded the same day - see the hardware checkpoint
below.

**2026-10-20 Windows port checkpoint:** `docs/WINDOWS_PORT.md` is now the single
status page for the port - toolchain, what the two software gates prove and what
they explicitly do not. Keep it current whenever a Windows gate changes, and never
state a Windows hardware capability the page has no capture of: since the hardware
checkpoint the closed gates are driver binding, enumeration, hotplug, the music
library read, MTPZ authentication, one music send with byte readback, the same two
directions through the ordinary controls, and native path display; the open ones
are deletion, the podcast, audiobook, photo and video categories, every model but
this Pavo, audible audio, a `spokenmediatool` gate and packaging. The only Windows audio fact this checkpoint states is that a muted
audible `playertool` run binds WASAPI, as mpv reports through its
`current-ao` property; nothing has been listened to. The decode fact is narrower and
measured: forcing software mode opens one decoder with no accelerator and a first frame
in `yuv420p`, while the default Settings leaves on reported a first frame in `cuda` here
and muxed a valid file through the hardware frame download path. Speed against software
and every machine without an NVIDIA decoder stay unmeasured, so neither is ever claimed.
The port stays native MSVC -
no WSL, MinGW or Cygwin - which is why the Linux line is unchanged by construction
at this checkpoint rather than re-verified.

**2026-10-20 Windows hardware checkpoint - the first Zune on Windows:** the first
hardware gate closed.  A Zune HD is bound to WinUSB on the development machine,
`packaging/windows/check-driver.ps1` reads it back as `CM_PROB_NONE` with
`service : WinUSB` and a real interface GUID, `devicetool` answers `VERDICT=agree`,
and `build\zuuned.exe` opened the player over WinUSB, read `GetDeviceInfo`,
identified Pavo (Zune HD) from `0xD21A`, took the whole 226,676-byte ZMDB payload
and listed **183 tracks in 9 albums by 10 artists**, `music 4.88 GB` attributed,
album thumbnails fetched off the device - the device page browses, which is what the
user confirmed on screen.  Unplug and replug produced a second identical read, so
Windows hotplug works rather than having got lucky once.  **That is a read, not an
authentication and not a sync.**  The same log states `MTPZ authentication will
fail`, because a development checkout carries no credentials and the ZMDB path reads
anyway: Windows MTPZ has still never run, and nothing has been written, deleted or
read back byte for byte.  Three responses went unanswered and are recorded, not
diagnosed - `0x2002` on `get_object_info(0x07000002)`, the atom the scanner itself
calls a playlist record, three times per scan; `0x200F` on `get_object` for two
device tracks the UI reported as `AccessDenied`; and
`get_representative_sample ... bad AUINT8 count 0 in 4-byte payload` for objects
whose thumbnails then fail every method.  No Linux hardware evidence carried into
any of it and none was used.

**The crash in front of that was a build artefact, not the application.**  Windows
`cl.exe` writes its `/showIncludes` note in the console code page, CMake stored the
prefix as double mojibake in `build\CMakeFiles\rules.ninja`, Ninja's `deps = msvc`
therefore never matched, and header dependencies were silently zero.  A member
inserted into `DeviceService`'s data section rebuilt its own translation unit and
left another one's object file as it was; the link joined both layouts of the class
into one exe; and on device arrival a `QString` reference count got incremented one
member away from the one the code meant - `0xc0000005` inside `Qt6Core.dll` at
`lock inc dword ptr [r8]` with `r8 = 0xf`.  `VSLANG=1033` does not change one byte
of the note, and deleting `build\CMakeFiles\<version>` and configuring again stores
the same prefix, so there is nothing to repair in place: `cmake --build build
--clean-first` is the rule (428 files removed, 329 rebuilt, and the app then
started).  `BUILDING.md` has the byte-level account; Ninja issues 2205 and 1766 and
CMake issue 24068 track it upstream.  Two habits fall out.  When a Windows change
appears to do nothing, prove the object is new before believing the diagnosis.  And
read a device interface GUID from the devnode's `Device Parameters` key, not from
`DEVPKEY_Device_DeviceInterfaceGuids`, which is empty on a player that opens
perfectly well - `check-driver.ps1` now falls back and prints which source answered.
Both gates that paragraph ended on closed the same day: the development build now
carries MTPZ credentials and every write path became reachable, and the path labels
go through `PathText::display()`, so a folder added as `E:\Temp\testmusic` reads
back with backslashes while SQLite keeps the one forward-slash identity that
`qml/OnboardingView.qml` compares.

**2026-10-21 Windows GUI checkpoint - both directions through the ordinary controls:**
on the build that carries MTPZ credentials, `+ all music` then `sync now` from the Music
page put two tracks on a Zune HD - `forge done: 2 ok, 0 failed`, one of the two albums
named in Chinese, and the device browser header going from 187 tracks to 189 - and a
right-click `Save to Library` on a device row wrote 20,992,730 bytes into
`Music\Zuuned Imports`, where the file indexed and gave up its embedded art. Two facts
from that one download are worth carrying: its first four bytes are the ASF GUID behind
an `.mp3` name the device itself supplied, and its album directory lost a trailing space
that Windows will not store. A pull keeps the device name and judges the payload. Health
reads `Watch folders reachable 2 of 2` and both roots display natively. **Nobody has
listened to anything**: device-side playback, deletion, the spoken and photo/video
categories and every model but this Pavo stay open, and none of it is inherited from
Linux. Two things closed after that sitting. A finished sync no longer waits on a hand
click: WinUSB keeps the interface for a whole re-index, `libusb_hotplug` is not even
capable here, and the 500 ms device-list poll in `src/UsbWatcher.cpp` gets no arrival,
so the finalize chain now arms its own bounded reconnect ladder of 6/12/20/30/45/60 s
whose attempts are each a real `zune_breach()` because an OpenSession is the only
honest readiness probe. A real arrival or unplug cancels it, a failed manual connect is
not retried, sync/purge/pull/eject reset or drop the budget, and an exhausted ladder
logs `connect manually` rather than inventing a state; observed live, attempt 1 landed
the device back at 190 tracks with nobody touching `connect`. Native path display is
also gated now rather than eyeballed: `--qa-probe` reports
`wfRows/wfNative/wfRaw/wfStored/wfLabel` and the usability music case pins that every
watch-folder label is spelled natively, that storage keeps one canonical forward-slash
identity, and that the rendered row agrees with `PathText::display()`. Gates at this
checkpoint: `171 checks, 0 failures, 0 skips` from `tests/win-compat` and
`launches=17 checks=94 failures=0 skips=0 killed=0` from `tests/win-usability`.

**2026-10-21 Windows wedge measurement - a reset that reported success:** the Zune HD
came back present, driver-clean (`CM_PROB_NONE`, `devicetool` `VERDICT=agree`) and deaf:
enumeration and control transfers answered, every bulk OUT timed out, and no software
call brought it back. Measured returns are in `libzune/docs/WINUSB_QUIRKS.md` -
`clear_halt()` on both endpoints returns success, `set_configuration(1)` returns success
six times, and `libusb_reset_device()` returns success with the player still on the same
bus address. That falsifies what this repository carried in
`packaging/windows/DRIVER_INSTALL.md`, namely that `libusb_reset_device()` always fails
on Windows; the truth is worse, because a success code from a recovery that did nothing
points at libzune when the fault is physical. So `zune_breach()` now logs the reset's
return value and its failure text names a power cycle. No retry rung was added - three
rounds of clear-halt plus reset were measured, every bulk attempt still `-7` - and the
device round trip was not re-run in this sitting: the player needs a power cycle first.

**Windows compatibility sweep, 2026-10-21:** a second whole-tree pass over the names
the app writes and the commands it runs, after the library-root defect was closed.
`LibraryPaths::safeComponent()` is now the only place device text becomes a file or
folder name - a Zune album named `P:44 / Vol. 2` made `mkpath()` fail and read back as
`Fail to save`, the same string the root defect produced -
`LibraryPaths::caseKey()` compares a stored `filepath` against a path that arrived
from outside, `zune_grab_photo_thumb()` builds a `cmd.exe` command line instead of
single quotes and `2>/dev/null`, and `importPlaylistM3U()` resolves an entry as a file
instead of comparing it as text. `tests/win-compat` is 184 checks and gains a
`file_identity` group pinning `st_dev`/`st_ino`, which is what the artwork cache's
cache-is-its-own-source refusal is built on. The three fixes are compile-verified
only: no hand test yet, and `zuuned.exe` has not been relinked since.
`docs/WINDOWS_COMPAT_AUDIT.md` records each one and what the sweep deliberately left
alone - four uncalled libzune shell sites, and no `<longPathAware>` in the executable
manifest, which keeps the Qt-side paths under 260 characters. The last clause is this
sweep's last word rather than the tree's: later the same day every executable gained the
entry (`packaging/windows/zuuned.manifest`, applied to six targets by `zuuned_long_paths()`
in `CMakeLists.txt`), so read it as what was found then and not as what the tree holds.
Device evidence is unchanged and still one hand-tested Pavo Zune HD.

**Windows third sweep, and everything re-run on the relinked build, 2026-10-21:** the
sweep above was re-read rather than re-run, which found three defects inside it. An m3u
line was resolved correctly and still decoded line-by-line with `fromUtf8()`, so a
Notepad "ANSI"/CP936 playlist matched no row at all and a UTF-8 BOM corrupted exactly the
first entry; `PlaylistText::decode()` owns the bytes now and the resolution moved into
`LibraryPaths::resolveEntry()`. `VideoIdentity::safeFilePart()` did not in fact carry the
reserved-stem rule the comment in `LibraryPaths.h` claimed it did, so a device object
named `NUL.wmv` still imported to a file that reports success and cannot be reopened;
`LibraryPaths::isReservedWindowsName()` is now the single rule both call. And none of it
had a test - neither Windows gate can reach a C++ header - so `librarytool paths`
(28 checks, expectations branched `#ifdef Q_OS_WIN` so one source gates both platforms)
 is where the filename, comparison, resolution and decoding rules are pinned, and the
 usability gate's tools phase now runs it beside `names` and `customize` - which is why
 that gate counts one check more than the checkpoint above. Both executables were rebuilt
 with `cmake --build build --clean-first` and the gates pass against them: 184 compat
 checks, `launches=17 checks=95 failures=0`, and
 `names`/`customize`/`paths`. Three corrections worth keeping. The usability gate's
`SCOPE: no device handle` line describes the harness, not the application - with a Zune
attached its own hotplug auto-connect browses it inside every isolated profile. A build
with no MSVC environment loaded fails as ~100 bogus `std::thread` diagnostics from the
Windows SDK's `ScopeCppSDK` include, not as a linker error. And the one that invalidates
evidence rather than only costing time: Ninja's `deps = msvc` never matches this machine's
localized `/showIncludes` output, so every object records `#deps 0` and an incremental
build silently recompiles nothing after a header edit while still relinking and looking
busy. Check with `ninja -t deps <obj>`; rebuild `--clean-first` before quoting any Windows
gate counter. Details in `BUILDING.md`. Deliberate gaps, all in
`docs/WINDOWS_COMPAT_AUDIT.md`:
playlist *export* names keep their own narrower rule because sharing `safeComponent()`
would rename existing Linux exports, and `safeComponent()` bounds spelling but not length.
Two items that were on that list closed in the same day's paragraph below -
`PlaylistText::decode()` honours a UTF-16 BOM in either byte order, and every executable
now declares `longPathAware` - so quote that paragraph and not this one.  POSIX behaviour
is unchanged by construction and still unverified on this machine. Device evidence is unchanged: one hand-tested Pavo
Zune HD, and no device claim was added.

**2026-10-22 GUID moniker, and the counters that depend on the desktop:** an album on the
Windows device page read `{09316835-3188-481c-a359-0f8d2a937ac8}` where a name belongs while
the albums beside it read normally. It is the *artist*: `saveTrackToLibrary()` writes
`<imports>/<artist>/<album>/<file>` and the pull log for that album puts the GUID in the
artist slot, and an album row's subtitle is an artist. `ffprobe` on the pulled files says
why one album and not the others - the ARIA file carries album/title/genre/disc/year and no
Author attribute at all, the EGOIST file only AlbumArtist, the third a real author - so the
Zune software stored a GUID moniker in the ZMDB artist record wherever the source file had
no artist, exactly as `libzune/src/album.c` already documents for an orphaned album object
that "displays as its GUID moniker". `library.db` was reported clean on the theory that pulls reach the
library through the watch-folder scan so the local artist came from the (empty) tag,
and that was falsified the same day: the pull itself wrote
`<imports>/<GUID>/<album>/<file>`, and the scan's folder inference promoted that folder
back into the artist - the paragraph below records it.
`MusicIdentity::isGuidMoniker()` is now the one predicate - 8-4-4-4-12 hex, braces optional,
anchored - and `deviceArtist()`/`deviceAlbum()` answer the `Unknown Artist`/`Unknown Album`
the rest of the app already uses, applied to the ZMDB scan rows and the album-object artist
in `src/DeviceWorker.cpp` and to both arguments of `saveTrackToLibrary()`. The quiet damage
was the artwork cache key: a cover fetched off the device was stored under the moniker while
the album indexed from those same files asks under `Unknown Artist`, which is how a
downloaded cover becomes no picture locally. `Music\Zuuned Imports` still holds a
16-leading-space folder for the same artist next to the trimmed one, dated before `8564616`
routed import folders through `LibraryPaths::safeComponent()`, so the device pads as well as
leaving the field nameless and the predicate trims because of that measurement. Untouched on
purpose: the raw device strings in `doPurgeInterruptedObject()`, which a persisted recovery
record has to match, and the albumartist vote in `src/sync/SyncEngine.cpp`. Ten new
unbranched cases in `librarytool paths` (52 to 62) because the placeholder belongs to the
player, not to a platform. Two counters in this sitting depend on the desktop and not on the
code, and both are now written down instead of rediscovered: `tests/win-usability` answers
`launches=18 checks=99 failures=6`, four of them ribbon checks asking a 150 per cent screen
(960 logical pixels tall) for the 1100 logical pixels a roomy window needs, and two of them
one new `cache\_qt_QGfxShaderBuilder_6.11.0\*.qsb` in the real profile - a third Qt-internal
cache, owned by the Qt5Compat `QGfxShaderBuilder` whose `FastBlur`
`qml/EnvironmentBackground.qml` uses, that asks `QStandardPaths::CacheLocation` for itself
and offers only `QT_GFXSHADERBUILDER_REFRESH_CACHE`, a rebuild switch rather than a redirect,
so `ZUUNED_PROFILE_DIR` cannot own it. The sibling check that passes matches a process id in
a file *name* and therefore cannot clear a launch on a hash-named file. On this machine a
`vcvars64.bat` from the wrong Visual Studio moves only `INCLUDE` - CMake pins the compiler in
the cache - and the newer STL headers answer `error C2338 ... STL1001: expected MSVC 19.50 or
newer`; read the toolset out of `CMakeCXXCompiler.cmake`. On one `--clean-first` rebuild
(329/329) carrying the change: `paths` 62 `ALL PASS`, `tests/win-compat` 201 checks / 0
failures / 0 skips. **No Zune was opened after the change**, so the display fix is
compile-and-gate evidence - a statement the paragraph below supersedes by
opening the player twice.

**2026-10-22 the moniker came back in through the disk, and a device count settled the
album question:** the user asked why the ARIA album on the Windows device page shows a
GUID where the albums beside it show names, and the answer came from counting instead of
reasoning - the ZMDB scan in `src/DeviceWorker.cpp` now tallies the raw fields before it
maps them and logs `GUID moniker fields on 189 track rows: artist 27
({09316835-...}), album 0 (-); blank-padded artist 27, album 0`. On this Pavo a moniker
is an **artist-field** phenomenon and no album has ever been named by one, so the string
on screen is an album row's subtitle, which is an artist. The same count refuted the
paragraph above: `library.db` did hold the moniker as `tracks.artist` on two rows,
because a pre-sanitisation pull wrote `<imports>/<GUID>/<album>/<file>`, the watch folder
scanned what the pull had just written, and `LibraryScanner`'s `Root/Artist/Album/file`
inference promoted the GUID folder to `artist` and `albumartist` for a file with no
Author tag - a string the device boundary refused re-entered as a name through the disk
boundary. A row whose stored probe version already matches is never read again and
`musicParserVersion` is pinned at 1 by `tests/music-import/backend.cpp`, so no rescan can
undo it: `MusicIdentity::usableName()` is the same predicate read from the other end -
"may this text be a name at all", not "what should the UI show" - and now guards every
candidate `resolveTrack()` takes (the three tags, both folder inferences, the
`Artist - Title` filename fallback), while `LibraryDb::repairDeviceMonikers()` runs from
`open()` beside `repairWatchFolderRoots()` for the same reason that neighbour exists: the
rows a healthy scan will never revisit are exactly the ones that need help. It rewrites
only a field that really held a moniker, mirrors the scanner's own fallbacks
(`Unknown Artist`, album artist filled from it, `Unknown Album`), skips any `user_edited`
row, was measured fixing tracks 13 and 14 on the real profile while preserving the row
whose album artist was really `EGOIST`, and prints nothing on a second launch.
`deviceArtist()`/`deviceAlbum()` trim first now, because the 27 moniker rows are exactly
the 27 blank-padded rows and an untrimmed name is two artwork cache keys, two album
comparisons and two import folders. One trap for whoever writes the next diagnostic: the
session-log redactor lists `device name` and `friendly name` as credential keys
(`src/diagnostics/Redactor.cpp`), so a line worded `device name <value>` folds its own
evidence into `[redacted]` - the tally line is `GUID moniker fields` now.
`librarytool paths` is 67 checks, fifteen of them this rule and none branched
`Q_OS_WIN`, and `tests\win-compat` is 201 checks / 0 failures / 0 skips against the
relinked build; `tests\win-usability` was deliberately not re-run because the app was in
the user's hands on the real profile. **No new Windows capability is claimed:** the
device work is a re-read inside the already-closed music-library-read gate, and deletion,
the podcast, audiobook, photo and video categories, every model but this Pavo and
anything anyone has listened to all stay open. The GUID-named import folders the old pull
left on disk are the user's files and stay there, now read as Unknown Artist.

**2026-10-22 the uuid was a genre, and 130 tracks of them:** the same question came back
from the device page - the ARIA album still shows a GUID where other albums show a name -
and this time every name field was counted before anything was touched. The album premise
is still false (0 moniker albums in the 189 track rows, 0 among the 15 album objects, 15
real album strings) and the screen was still right: `genre` was the one device name field
this app copied over raw, it holds the same `{09316835-...}` on **130 of 189** rows, and the
Genres pivot titles a tile with that string and puts up to four of the genre's albums in a
2x2 mosaic under it (`qml/MusicPage.qml:737`, `genreAlbumRefs()`), which is exactly what
makes a genre read as an album and why only some albums looked affected.
`MusicIdentity::deviceGenre()` yields an empty `QString`, not an "Unknown Genre" word,
because both pivots skip an empty genre and that is precisely what an untagged file answers
- `deviceArtist`/`deviceAlbum` yield words only because those fields have a word to yield -
while the device's own literal `Unknown Genre` is its content and stays untouched.
`LibraryScanner` asks `usableName()` of the genre tag too and
`repairDeviceMonikers()` carries the column, though the real `library.db` (15 tracks: `""`,
`JPop`, `アニメ`) needed no repair, so that half is the door and not the mop. Side effect
worth remembering: `DeviceService::pickMoodColor()` takes the highest-count genre, so on this
player every device session had been keying the accent colour off a GUID and falling through
to a random palette entry. The scan now also prints the distinct artist/album/genre pools
and each album object's name, bounded, because "did the known placeholder shape appear"
cannot answer a question about what is on screen; `librarytool paths` is 74 checks (six this
rule), `tests\win-compat` 201/0/0, and `tests\win-usability` was again not re-run - the app
was in the user's hands on the real profile and its two profile-isolation checks fail by
design while that is true. **Still no new Windows capability and no new device claim:** one
hand-tested Pavo Zune HD, read-only this time. Left open with its evidence:
`[customize-http] HTTP 0, Qt error 4 (https://api.deezer.com/artist/...)` alongside
`QIODevice::read (QNetworkReplyHttpImpl): device not open` in the live log is the lead for
the unresolved artwork complaint - a request that never started, not a provider with no art.

**2026-10-22 Windows fourth moniker pass - the last mile spoke a second language:** the
uuid-under-ARIA report came back a third time, and this time the fault was in the
application though not on the device. A read-only re-sighting now prints the labels the
screen draws, not just the columns it was handed (`[zmdb] artist tiles ...`,
`[zmdb] album tiles ...`, and a `title` counter added to the field census: artist 27, album
0, genre 130, title 0 of 189 rows), and all 15 album tiles on this player carry their real
names - so `qml/MusicIdentity.js`, which is not a binding but the same rule re-written in
JavaScript and the last mile for MusicPage, ArtistDetail and GenreDetail on both sources, was
grouping with no moniker rule at all; `MusicPage`'s `computeArtists()` never asked the
identity module either, `AlbumDetail`'s headline and byline fell back to a raw row field that
is also the name of the artist page the byline opens, and the customize re-key compared a
drill-down key built from a raw artist against one the mirror had already ruled. SyncEngine
payloads stay raw on purpose: an identity a send writes is not a label. Neither gate reaches a
`.js`, so the module's answers were checked by evaluating it and its QML callers by `qmllint`,
which does resolve the relative import and does name a missing export - not a tree-wide gate,
because 3246 of its `missing-property` warnings name `Theme`. The compact usability phase then failed
four checks against an unchanged app because the harness was scale-blind - `Main.qml`'s
breakpoints are logical pixels, `winshot`'s `MoveWindow` takes physical ones, and
`GetDpiForWindow` answers 96 to a DPI-unaware PowerShell caller, so the first repair read a
150% desktop as 100% and blamed the application for a window it could not physically have
made. Scale now comes from `winshot --dpi` and the minimum-size check compares logical pixels,
which also made one thing visible: at 150% a 2560x1440 desktop is 960 logical pixels tall,
below `headroomCompactBelow` (1100), so the top ribbon is unreachable in any window that fits
that screen - raised as a design question, deliberately not tuned inside a sweep. And a
retraction: `Qt error 4` is `QNetworkReply::TimeoutError`, not `HostNotFoundError`; measured
curls reach GitHub in 0.39 s and time out on Deezer and Google, both of which answer in under
1 s through `192.168.2.100:8117`, but `ProxyEnable` is 0 and a fresh Qt 6.11 process reports
`usesSystemConfiguration=1`, so Qt already follows the Windows shell and going direct is
correct - this machine has its proxy switched off, not a broken client. What was missing was
the route in the log: `src/library/NetworkAccess.h` prints `[network] ... HTTPS goes direct`
once at startup and `via <route>` on every failed `[customize-http]` line, with a
`ZUUNED_PROXY` override that installs nothing when unset, pinned by 27 `librarytool net`
checks. Gates on the relinked binary: `paths` 73 (the eighth sweep's "74" was a miscount),
`net` 27, `tests\win-compat` 201 checks, `tests\win-usability`
`launches=18 checks=102 failures=0 skips=0 killed=0`, re-run at the same counters on the relink
that carries the fourth door - its `pid-scan-fallbacks` read 1 once and 0 once, a tally. No capability added and no device claim
changed - one hand-tested Pavo Zune HD, read-only here, nothing listened to.

**2026-10-22 Windows spoken walk, and the fork's own name:** a third full-tree Windows
compatibility read found one defect, and it was the one the last three passes kept listing
without picking up - `SpokenMediaService::previewBookFolder()` still walked a user-picked
audiobook folder with its own recursive `QDirIterator` and `QDir::NoSymLinks`, which is the
filter that reads like a guard and does nothing about a junction because `QFileInfo` does not
call one a link, so a folder linking back into an ancestor was an ordinary directory and one
book came out filed under every phantom path the loop invented. It calls
`LibraryScan::walkFiles()` now - the watch-folder walk, with a real per-folder identity and
the `mklink /J` fixture in `librarytool walk` behind it - and four behaviours were compared
rather than assumed: the file set (`walkFiles()` lists with `filters | Dirs | NoDotAndDotDot`
and visits only non-directories, so a folder named `track.mp3` never becomes a chapter, and
the visitor keeps the `isSymLink()` test that was the one thing `NoSymLinks` achieved), the
5,000-file ceiling word for word, the 100,000 ceiling now counting visited entries, and the
unreadable-folder refusal split into a root sentence and a subfolder sentence read off
`complete`/`directories` instead of `QDir`'s metadata. Cancellation is the lesson worth
keeping: `walkFiles()` stops on the one flag it is handed and this call hands it the ceilings'
atomic, so the visitor has to read the caller's flag itself - the first cut checked it after
the walk, which would have made a Cancel press wait out a six-figure folder while the bounded
spoken-worker close at shutdown waits on that same loop. No spoken phase is gated on Windows,
so the usability `tools` phase pins structure instead of pretending to have walked a tree: no
`.cpp` or `.h` under `src/` may contain `QDirIterator::Subdirectories`, and
`src/spoken/SpokenMediaService.cpp` must call `LibraryScan::walkFiles(`. What the same read
cleared, so it is not read a fourth time: no second recursive walk anywhere; three `QProcess`
sites, none reachable here except as a fixture (`gio` behind a `/run/user/<uid>/gvfs` regex,
the udev one-click behind `udevRulesApply()`, `cmd.exe /c mklink /J` inside `librarytool
walk`); artwork and thumbnail cache keys are digests; every atomic write is a `QSaveFile`;
`ZUUNED_TMPDIR` maps to `zuuned_win_tmpdir()`, so the transcode layer's `/tmp/` is spelled
correctly. And the fork's own documents were still talking about somebody else: this file and
`docs/PORT_PLAN.md` called the project ZuunedLinux and `packaging/TESTER_README.md` pointed at
upstream's Linux downloads, so those three plus `docs/ANNOUNCEMENT_VIDEO.md` now say whether
they describe this fork (`miemiekurisu/zuuned-modern`, the Windows client port) or the upstream
a feature came from - while runtime identity stays `Zuuned`, `zuuned.exe`, `%APPDATA%\Zuuned`
and `ZUUNED_PROFILE_DIR`, because renaming those orphans real profiles for a label. Two
statements this file had already outgrown (no `<longPathAware>` entry, UTF-16 m3u undecoded)
are qualified to point at the pass that closed them instead of being copied forward. Gates on
a `cmake --build build --clean-first` relink: `tests\win-compat` 201 checks, 0 failures, 0
skips; `tests\win-usability` `launches=18 checks=104 failures=0 skips=0 killed=0`,
`OVERALL=ok`, both new pins passing, `pid-scan-fallbacks=2` a tally. No Windows capability is
added and no device claim changes - one hand-tested Pavo Zune HD, read-only, nothing listened
to.

**2026-10-22 Windows eleventh sweep - a second executable, and a GUID that was never an
album name:** `tools/artwork-preview` is the only process in the tree that has to name a
Zuuned profile from the outside, and it did so with Qt's generic writable location plus a
literal `/Zuuned/Zuuned` - correct on Linux by XDG luck, wrong on Windows in two directions
at once, because `GenericDataLocation` is `C:\ProgramData`, where Zuuned has never written
a file (`Test-Path C:\ProgramData\Zuuned` is `False`), and `GenericCacheLocation` is the
shell's local app-data folder, one level *above* the `<LocalAppData>\Zuuned\Zuuned\cache`
that holds `art`, `artistart`, `stills` and `vidthumbs`. It could not have used
`QStandardPaths::AppDataLocation` either: that answers for the calling process, and the lab
runs as `ZuunedPreview` / "Artwork Print Lab" so that its settings and pipeline cache stay
out of the application's. `Zuuned::ProfilePaths::applicationDataLocation()` and
`applicationCacheLocation()` now name both folders, `ZUUNED_PROFILE_DIR` winning on either
platform and neither of them calling `mkpath()`, because a tool looking for someone else's
library has no business creating an empty one. Measured against one build of the tool with
no explicit roots: the old defaults exit `1` with `No cached artwork found`, the new ones
exit `0` and export four sheets, one named `ARIA The NATURAL VOCAL SONG COLLECTION /
Unknown Artist`. The usability gate's `tools` phase pins the rule rather than the artwork,
including a sweep-by-construction check that no source file under `src/` or `tools/` spells
a profile folder out of a string; its three new pins are why it now counts 107. Asked the
same day why the ARIA album on the player carries a GUID where a name belongs while other
albums do not, the scan answers without anybody clicking into a tile: the moniker is on 27
of 189 artist fields and 130 of 189 genre fields and on **0 album and 0 title fields**, the
album pivot prints 15 of 15 real names with ARIA's by-line `Unknown Artist`, the player's
own album object carries the real name as text, and the print lab keys the same album out of
the cache as `... / Unknown Artist`. The GUID is the player's placeholder for an artist or
genre it was never given, so an album whose rip carried an `AlbumArtist` reads normally and
one whose rows did not does not - `Extra terrestrial Biological Entities` is the pair that
shows both, object `EGOIST` against rows holding the moniker - and
`MusicIdentity::isGuidMoniker`/`deviceArtist`/`deviceAlbum`/`deviceGenre` with their
`qml/MusicIdentity.js` mirror is what keeps it out of every name the application renders,
caches, writes to a folder or would sync onward. No album name in this tree's data has ever
held a GUID; if one appears in an album tile on a build carrying the rule, that is a new
defect and needs the string that was seen. Everything was re-run on the relinked build
(`cmake --build build --clean-first`): `tests\win-compat` reports 201 checks, 0 failures
and 0 skips, and `tests\win-usability` reports launches=18 checks=107 failures=0 skips=0
killed=0 with `OVERALL=ok`, and the preview lab compiles and links on Windows as its own CMake project.
Naming is aligned with the fork: `docs/README.md` carries the Zuuned Modern banner and points
at `docs/WINDOWS_PORT.md`, `docs/releases/README.md` and `packaging/TESTER_INSTALL.md` say
they are upstream's Linux notes kept as shipped, `ci/README.md` says its procedure is
upstream's and `ci/check.sh` stays Linux-only, and `CLAUDE.md` - 818 lines of the upstream
Linux guide frozen at an earlier revision, whose only three unique lines were its old
`ZuunedLinux` title and a superseded `usbreset 045e:0710`-only recovery - is a 58-line
pointer to `AGENTS.md` and `docs/WINDOWS_PORT.md`, because a duplicated guide is how a
superseded instruction survives a fork. No Windows capability is added and no device claim
changes - still one hand-tested Pavo Zune HD, nothing listened to. (Its `read-only` was
false as written: the 2026-10-21 GUI checkpoint above had already sent and saved.)

**2026-10-23 Windows QML name collision, two more categories on the wire, and a lookup
that waits to be asked:** three things, one of them silent and user-visible.
`src/PathText.h` registered its path singleton with `QML_ELEMENT`, and `Qt6Quick.dll`
already exports a `PathText` shapes type beside `PathSvg`. Every QML file here imports
`QtQuick`, so in a file that does not also import `Zuuned` the name resolves to Qt's and
every binding on it throws: `QueueEntryView.qml:147` logged
`Property 'url' of object QtQuick/PathText is not a function` six times in one
launch, which is queue-row artwork failing per row while both gates passed. The singleton
is `QML_NAMED_ELEMENT(ZPathText)` now, and a scan of every `Qt6*.dll` in the Qt bin answers
that it was the only name this tree exports that Qt also owns; the usability gate pins the
rule off the source - no bare `PathText.` anywhere in `qml/`, plus the registered name -
because no phase of it loads a QML file that forgets the import.
`transcode/transcodetool.c` rebuilds `argv` from `GetCommandLineW()` as UTF-8: the CRT
hands `main()` the ANSI code page, so on a GBK machine the encoder phases were probing
question marks where the application passes `toUtf8()` to the same C functions. That is
what lets them gate a Chinese path instead of the ASCII one next to it.
`librarytool photoscan`, `--qa-photo` and `--qa-video` make a photo album and a movie
answer in log lines through the same typed admission the ordinary controls use, and they
were used on hardware: five pictures prepared at 480px and sent into one album,
`photos=5 failed=0`; one movie transcoded to container `0xB982` at 20,575,375 bytes and
sent, `videos=1 failed=0`; the player re-listed both. A send the firmware counted is not a
read-back, so the sentence in `tests/win-usability/run.ps1` that the hand test had "sent
music, photos and video, and read back from" now says the truth - only music has come back
byte for byte. Lastly, `AppSettings.autoMetadataLookup` (an ini key, default **false**)
makes every provider lookup opt-in, because `discoverMissingArtwork()` had been asking
MusicBrainz and Deezer for a portrait of *every* artist on every library load - an artist
image has no local source, so unlike album art it never had a local-first hop to fail -
and the shape asked for was one item looked up by right-click, not a library searched
unconditionally. `retryArtwork` and the Settings button force their key and still reach the
network whatever the switch says. On the profile that complained, six provider request
lines, one of them `HTTP 503` from musicbrainz.org, became zero beside one
`[artwork] online lookup off: 13 albums and 7 artists read locally, none queried`; the
music-page case asserts both halves, so offline-by-default is protected rather than
inherited. Gates on the relinked tree: 201 compat checks, and
`launches=18 checks=116 failures=0 skips=0 killed=0` with `OVERALL=ok`. No device claim
changes: still one Pavo Zune HD, and nothing on it has been heard.

**2026-10-24 Windows album-art pass: three ways to lose a cover, and a store that will not be
rushed:** "the PC albums have no covers" was four faults, and none of them is in the matching.
`storeToCache()` staged a picture into a live `QTemporaryFile` and then did `QFile::remove(dst)`
followed by `QFile::copy()` onto it, and Windows will not delete a name its owning object still
holds - not after `close()` either - so the verbatim-JPEG branch returned false: every picture
that was already a JPEG of 400px or less, which is what a `folder.jpg` and a device thumbnail
usually are, was thrown away on this platform alone. `copyBytes()` opens the destination
`WriteOnly | Truncate` and streams, which the holder allows because it withholds only deletion.
The local resolution order is now a known name (`cover`, `folder`, `artwork`, `album`, `front`,
case-insensitive, jpg/jpeg/png) in the track's own folder, that same name one level up when the
folder is only a disc - `^(?:cd|disc|disk|vinyl|lp|pt|part)[\s._-]*[0-9]+$`, so `CD1` inherits the
album's picture and an album actually called `Vol 2` does not - then the picture embedded in the
file, and last of all an image that goes by no recognised name; embedded art used to sit *behind*
"any image in the folder", so a scanned lyric sheet dropped in with the lossless files beat the
real cover, and `tests/device-music-import/album-art-precedence.cpp` now pins the order. Artist
tiles and the player keyed art by the *track* artist, which stops being the album owner at the
first compilation or featured artist; `localArtOwners()` in `qml/MusicIdentity.js` derives the
owner per album and `ArtistCard`, `ArtistDetail`, `MusicPage` and `Main` ask for that key with the
album's own cover as fallback. Album-cover lookup (`albumCoverLookup`, default **on** until the
2026-10-24 import-default pass below ships it **off** alongside the switch beside it) and
MusicBrainz identity lookup (`autoMetadataLookup`, default **off**, portraits included) are
separate switches now, so a file that carries its own art still causes no request while an album
with no local art does get a real cover - and that last clause is what exposed the fourth fault.
`ArtworkHttp` paced MusicBrainz and nothing else, and `resolveStoreAlbumArtwork()` walked straight
through a catalogue that had *errored*, so a 13-album library put roughly 2.2 requests a second on
`itunes.apple.com/search`, was answered HTTP 429, burned all six rungs per album, and filed every
one of them as "no music store carries this album". One store search is now in flight
process-wide at least 2 s apart; `artworkStoreSearch()` pushes the next slot out by `Retry-After`
or four times the interval on 429/503; a wait over 30 s is answered locally with a synthesized 503
so a rate limit is never recorded as an absent cover; and the first catalogue that errors ends the
ladder, with `OnlineAlbumArtService` parking the album for five minutes. Measured on a
`VACUUM INTO` copy of the real library: 42 store searches in 87.6 s, minimum gap 1.7 s, no 429 and
no 503 at all, and the probe reports `albums=13 artWith=4 artAgree=13 artOwnerWanted=4
artOwnerFound=4`; the nine albums still blank are the sine-wave scan fixtures, which have no
picture in a file, a folder or a store. Two harness facts. PowerShell 5.1's `-Include` is
relative to the *parent* of a `-Recurse` path, so `cache\art\*` with `-Include *.jpg` matched
nothing and the gate had been certifying an empty directory for a run. And
`tests/device-music-import` isolates its cache with `qputenv("XDG_CACHE_HOME")`, which Windows
ignores, so five `device-album-art` checks fail here for that reason alone - reproduced against a
scratch build of HEAD's own `AlbumArtService.cpp`, which fails the identical five, so it is a
Linux-only gate and not a regression. Gates on the relinked build: `tests\win-compat` 201 checks
0 failures, `tests\win-usability` `launches=18 checks=123 failures=0 skips=0 killed=0`
`OVERALL=ok`, `librarytool cover` 33 checks including the pacing arithmetic and "one unanswered
catalogue is enough to learn it". Header-only edits still need `--clean-first` because Ninja's
`deps = msvc` never matches this machine's localized `/showIncludes`. No device claim changes.

**2026-10-24 Windows import-default pass:** a pull off a Zune no longer lands beside the library.
Three `static` `DeviceService::*ImportsDir()` helpers returned a `QStandardPaths` location plus
`Zuuned Imports` whatever the library held, and the device pages finished a save by registering that
path as a watch folder - so a download could never reach a folder the user added, and the moment one
could, that same line would silently retype their declaration. `watch_folders.import_default`
arrives at schema v10; `LibraryDb::importRoot(kind)` answers "the marked one, else the earliest
added" in one `ORDER BY import_default DESC, id ASC LIMIT 1` read, and the kind-to-type sets restate
`LibraryScanner`'s root rule on purpose, because a file written into a folder no walk visits is one
the library owns, occupies disk and can never browse. `setImportDefault(id)` unmarks only the rows
that compete for the same kind (same type, plus any `all`), so a photo default and a music default
coexist and choosing one never moves the other; `reloadFolders()` derives a row's `importDefault`
from what `importRoot()` returns instead of echoing the column, so the label stays honest once the
marked folder is gone; `ensureImportRoot()` mkpaths and registers `~/Music|Pictures|Videos/Zuuned
Imports` *before* the first byte when the library holds nothing of that kind, so even the fallback is
indexed rather than merely written. Measured on the player: destination `E:/Temp/testmusic/测试音乐`,
the first music folder added and therefore the default with nobody clicking, `[worker] extracting
track 16777453` to `download-saved ok=true` in 2.71 s for 34,379,623 bytes, `verified music copies`
25 to 26 source rows while displayed songs stayed at 15 because the pull is a second copy of a song
the library already showed. `readbackDurationMs` in that same probe line is the track's audio
duration - 272,093 ms, 4 min 32 s - not a transfer time: `qml/Main.qml` prints `row.durationMs`, and
the 8000 it carried for the eight-second sine fixture is what proves it. Two faults found on the way
are not Windows faults at all: `purgeFolder()` and `countsUnder()` bounded `substr()` with
`QByteArray::size()` while SQLite indexes TEXT by character, so a Chinese-named folder counted 0 of
everything and releasing it logged `purged 0 items` while keeping every row it owned;
`QString::size()` at both sites, pinned by the new `librarytool folders` (20 checks, `tools` phase).
`isSkippedVideoFile()`'s 50 MB floor is `movies`-only now rather than "anything that is not
tv/anime", so a folder typed `all` keeps a 20 MB clip. `LibraryScanner` also censuses, per root, the
files its own declared type will not index and reports them as Settings -> Device warning rows plus
one `[scan] ... will not index` log line - coded and compiled, and still with no live capture on this
machine (every root here is typed to match its contents), though the signal carrying it has one since
the pass below. The
usability gate no longer seeds a settings ini at all, so it measures the shipping defaults instead of
a value it wrote for itself. Gates: `tests\win-compat` 201 checks 0 failures, `tests\win-usability`
`launches=18 checks=129 failures=0 skips=0 killed=0` `OVERALL=ok` - six above the nineteenth pass,
from `vanish`, `folders`, the `wfDefaults` assertion (the seed library holds two unmarked music roots,
so exactly one row may read `default`), two ships-off source invariants and the `clearSearch()`
invariant. Harness fact worth keeping: the first run was `launches=3 checks=91 failures=25`, every
failure `0xC0000135`, because Qt's bin was not on `PATH` and the gate's own `librarytool`,
`devicetool` and `playertool` never started - a gate that cannot launch its own tools is not an
application verdict. No device claim changes.

**2026-10-24 Windows video floor pass:** the pass above narrowed `isSkippedVideoFile()`'s 50 MB floor to
`movies`-only and that was still wrong where it mattered: the user's video folder *is* typed `movies`, so
a 20,575,375-byte `Save to Library` wrote itself to `E:/Temp/testmusic/测试视频/测试视频.mp4`, the watcher
fired, and the Videos page never showed it, with no log line anywhere. `VideoNaming::videoSkipReason()`
answers `none`/`bonusName`/`sizeFloor` instead of a bool - a scan cannot report what it did not
distinguish - and the floor no longer applies beneath `LibraryDb::importRoot("video")`, the folder
downloads land in, because Zune video is small by nature and a pull the user asked for is not trailer
debris; `trailer.mp4` is still refused everywhere and every other `movies` root keeps the 50 MB screen.
Refusals are counted per root and reported as `[scan] ... holds N video file(s) under the size floor`, and
as `sizeSkipped` on the `mediaTypeNotes` row Settings -> Device renders, because `videoMinSizeMB` is an
ini key with no control. A refusal is counted after the unchanged-mtime fast-path on purpose: counted
first, a folder whose video is on screen starts complaining the moment the default moves elsewhere.
`librarytool videofloor` (19 checks, in the usability `tools` phase) plants two 2 KB clips in two movie
folders and proves the marked one indexes, the other is refused and named once, and moving the mark
indexes the file and retires the complaint rather than stacking one. Live: on the relaunched build the
launch sweep indexed the 20 MB file that had been invisible since 02:43:14 - two video rows, a probed
60,325 ms duration, no size-floor line for its folder. Nobody has watched it. Gates: `tests\win-compat`
201 checks 0 failures, `tests\win-usability` `launches=18 checks=130 failures=0 skips=0 killed=0`
`OVERALL=ok`, on a `--clean-first` relink after the header edit. No device claim changes.

**2026-10-24 per-kind import defaults:** a library is one place, so music has to import into the
music folder, and among several folders of one kind exactly one receives - a status on that row, a
`default` button beside `type`/`remove` on the others, earliest-added holding the title until somebody
clicks. `watch_folders.import_default` could not express that: one bit per row, and a folder typed
`all` qualifies for every kind, so marking a picture folder cleared the mark that had been carrying
music, and `ORDER BY import_default DESC, id ASC` let a root that merely promised everything win a kind
another folder was declared for. Schema v11 keys the mark by kind in `import_defaults (kind PRIMARY
KEY, folder_id)`; the migration seeds only real v10 marks (`WHERE import_default=1`) so a fallback is
never frozen into a choice nobody made, and the v10 column stays, unread, because dropping a column is
the one migration a downgrade cannot forgive. `LibraryDb::importWinnerId()` is the whole rule - the
mark for that kind while its type still serves the kind, else the first folder added that serves it,
`ORDER BY CASE WHEN type='all' THEN 1 ELSE 0 END, id ASC` - `setImportDefault()` upserts one row per
kind the chosen type serves, and `importKindsForType()`/`watchTypesForKind()` in
`src/library/LibraryTypes.h` are the kind-to-type rule stated once beside the third copy of it in
`LibraryScanner::scan()`'s root sets. Status is derived from a row *id* per kind, never from a stored
path (two rows spell one folder differently) and never from a column: `default`, `default - video`, or
an empty string, which matters because the label used to emit a truthy `"default . "` for a row that
receives nothing, so "no status" and "blank status" were one thing to the QML fallback and to a gate
counting rows. Same pass: the music walk was the one walk missing the `rootType != "all"` guard, so it
censused an `all` root for videos and pictures it had already indexed and Settings offered to set that
folder's type to the type it already had. Gates: `librarytool folders` 20 checks to 35, now rewinding
a real v10 database (marked column, no `import_defaults`, `user_version` 10) and reading the upgrade -
the rewind drops `import_defaults` too, because a fixture that answers the v11 probe is not a v2
fixture; new `librarytool census`, 10 checks, one real `LibraryScanner` pass over a tree holding all
three kinds; `wfDefaultLabels`/`wfDefaultAgree` in `--qa-probe` with a `pages` check that only a
default row carries a default string. `tests\win-compat` 201 checks 0 failures,
`tests\win-usability` `launches=18 checks=132 failures=0 skips=0 killed=0` `OVERALL=ok`, on the build
the running app is. Deliberate: a mark survives its folder being retyped, it just stops qualifying, so
an accidental retype does not cost the user their choice. No device claim changes and one is narrower:
the music destination on this machine moved from `Zuuned Imports` to the declared music folder, and no
player has written into it.

**2026-10-13 compact playback UI:** windows below the existing 1100-logical-pixel
headroom breakpoint use Now Playing instead of the top ribbon, as do narrow
windows. Resizing an existing queue opens the island unless a builder is active;
closing it persists through track advances. The island has a fixed top-right X
that turns red on hover/press/focus, with no background, and focus-scoped Escape dismissal. Verification and desktop
limits are tracked in `docs/PLAYER_LAYOUT_AUDIT_2026-10-12.md` and
`tests/player-layout/README.md`.

**Scope update:** podcasts and audiobooks are active; defer HD artist backgrounds
until both work. Use the dedicated `feature/podcasts-audiobooks` branches.

**Active private workstream, 2026-10-08:** read
`docs/SPOKEN_MEDIA_AND_DEVICE_ART.md` before podcast, audiobook or Zune HD artist
background changes. Both repositories use `feature/podcasts-audiobooks`,
based on the private visualizer checkpoint. Device protocols belong in libzune;
feeds, downloads, local persistence and provider art selection belong in native
app services. Preserve serialized device ownership. Advertised descriptors are
not hardware write validation; keep category, resume and artwork gates separate.
The initial `zunetool media-capabilities` probe is read-only after connection.
Experimental podcast C APIs now have 50 software checks. After a physical reconnect,
the Keel gate passes isolated series/MP3 creation, repeat reuse, byte-for-byte
extraction and cleanup with the original wildcard inventory restored. This does
not prove playback, resume or HD support. The subsequent real Moonbound trailer
transfer passed repeat reuse and byte readback; the user confirmed podcast
category presentation on the Classic. That initial trailer had no artwork. The
October 9 app transfer sends show artwork. The user subsequently confirmed
podcast audio playback and images work on the Classic. Firmware resume and
other models remain separate gates.
Counted UTF-16 text requires a terminal NUL; preserve the documented Keel
filename/schema/timestamp readback semantics and successful-auth write guard.
`src/spoken/` now includes the native RSS parser, local audiobook probe,
serial service and separate `spoken/spoken.db` schema 1. The first native pages
support RSS preview/subscription/downloads, ordered local book imports, chapters,
custom covers and local playback/resume/bookmarks. Customize is right-click only;
podcast covers reuse split + / arrow; books use one whole-book +. Podcast transfer and drags now use
typed SyncEngine/DeviceWorker admission
and recovery. Audiobooks now have dedicated typed admission and sends, book/part
drags and explicit selection. Small multipart MP3 protocol gates pass on Classic
and HD. Chaptered sources now prepare ordered device parts through native PCM
range encoding; the complete source timeline is preserved. Local image drops work offline. Do not use music identities for spoken media.
Gates: `tests/spoken-media/run.sh` and `tests/spoken-media/native/run.sh`; the
latter uses real native services/QML/mpv, isolated profiles, loopback HTTP and no
USB. See the dated audiobook hardware evidence and remaining resume/chapter
gates in the spoken-media plan.
This work stays on private Gitea until approved for wider distribution. Existing
per-push approval and visualizer/public-export restrictions remain in force.

**2026-10-09 spoken UI correction:** playback belongs in the existing top ribbon
and now-playing island, never a separate bottom strip. Both use spoken cover IDs,
mic/book icons and skips/speed/bookmarks/sleep/chapter controls. Music modes stay
preserved but inactive for spoken queues; music speed resets to 1×. Settings →
Library offers reviewed native audiobook folder import (one book or one book per
immediate subfolder), with nested discs, natural order, atomic batch saves,
repeat reuse and overlap refusal. It is a manual importer, not a watch folder.
See the private plan and `tests/spoken-media/native/run.sh` for exact contracts.

**2026-10-09 podcast readback correction:** Pavo schema-0x10 podcast objects
normalize redundant SeriesName to empty and MetaGenre to zero, as Keel does.
libzune preserves exact series/source/title/size checks. Explicit
`ZUNE_PODCAST_FAILED_CLEANED` proves new-object deletion; the app retires recovery
only for that correlated outcome, never merely ID zero. Unknown writes retain
review. See the private spoken plan; HD playback and Classic retest remain gates.

**2026-10-09 device spoken browser:** the Zune island includes Podcasts and
Audiobooks with lazy serialized native inventory, series/folder-ID grouping,
covers, selection and confirmed removal. Keep request/serial/session guards,
live full-selection validation before deletion and successful-ID-only pruning.
Tests: `tests/device-spoken/` and libzune `tests/run-spoken-inventory.sh`.
No browsing action authorizes deletion; live deletion remains user-confirmed.

**2026-10-09 spoken organization:** local podcasts use Shows, Starred shows and
Starred episodes; audiobooks use Books, Libraries and Starred. Show/episode/book
favorites are independent and persist through refresh/import. Sublibraries are
ordered many-to-many virtual collections, never folders or transfer identities.
Use native revision-checked atomic saves/deletes; deleting a collection preserves
all books/files. Preserve shared GalleryView/header/breadcrumb styling, offline
organization, right-click Customize and whole-book queue actions. Gates:
`tests/spoken-organization/run-qml.sh` and `tests/spoken-media/native/run.sh`.

**2026-10-09 podcast retention:** optional `podcastAutoCleanup` defaults off.
Sync may run with an empty queue to clean firmware-marked-played podcasts.
Persist exact device-scoped listening history before deletion; then revalidate the
complete plan on the worker. Unknown state never authorizes deletion. Preserve
Keep on Zune independently of favorites, local files and other media. Remembered
consumed history suppresses re-addition only for the same device while enabled.
Podcast cover-arrow/show-drag selection offers newest/oldest count and manual
selection, with optional unplayed filter and per-show preferences. No background
queueing on feed refresh. See the spoken plan for tests and hardware limits.

**2026-10-10 podcast visibility:** local episodes expose played/played-on-Zune
and the existing PNG device badge from exact feed/source inventory. Cache status
per session, defer reads during device work, treat unavailable state as unknown,
and prune only confirmed deletion IDs. The show picker defaults to newest 10
unless a saved preference exists, with oldest/count/manual options preserved.
Cleanup actions appear as transient `podcast-removal` queue rows with truthful
progress. Never persist/replay these actions or count them as upload capacity.
See the private spoken plan for current contracts and verification.

**2026-10-10 cleanup preview correction:** removal actions must appear before
Sync, from correlated device source/played cache plus local retention choices.
Sync still revalidates the complete plan before deletion. Removing a preview
row/group or clearing the queue excludes those episodes for the current
connection; Keep on Zune is persistent protection. Previewing never writes
listening history or sends a delete. See the spoken plan for live Classic
preview evidence and the remaining actual-deletion gate.

**2026-10-10 starred podcast queue:** Settings → Library offers opt-in queueing
of newest unplayed episodes from starred shows on connection, with 5/10/custom
count and saved per-show count overrides. Native `PodcastQueuePlanner` owns RSS
refresh/selection; + honors saved selection and accepts feed-only entries.
Existing device episodes count toward N before upload filtering. Preserve exact
identity, unknown-inventory deferral, connection generation guards, pending-edit
ordering and one-pass behavior. Clear queue cancels in-flight additions; transfers
and played cleanup still require explicit Sync. See the spoken plan and native
planner gates; this does not authorize device deletion or public distribution.

**2026-10-13 Draco podcasts:** Zune 80/120 podcast readback now recognizes the
observed native `0x10` title-derived filename, empty redundant SeriesName and
zero MetaGenre. Podcast inventory/source maps accept Draco with strict validation;
audiobook inventory/lookup/send now also accept Draco after the isolated two-part
audio/art roundtrip. Podcast state now admits Draco Classic v2 after a 15-record
read-only identity-matched snapshot; Keel v2/Pavo v5 regression captures pass.
Cleanup still requires complete live firmware state and exact identities.
The Draco completed-playback/deletion test remains separate from this baseline. See libzune
`docs/SPOKEN_MEDIA_RESEARCH.md` for the failure evidence and retest boundaries.

## Project Overview

ZuunedLinux is a native Qt 6 / QML application for managing Microsoft Zune
media players on Linux. Sister project of ZuunedMac — both apps share
libzune, the C library carrying the reverse-engineered PTP/MTP/MTPZ stack.

No kernel driver. No webview, no Electron, no Python. libusb IS the driver
on Linux — one udev rule and the hardware is ours. Compiled C++ linking
libzune.a directly (zero FFI: it's C, we `#include "zune.h"`).

**Hardware-validated 2026-08-27:** full MTPZ auth, library read, track
send, and purge all proven against a real Zune 30 over libusb on this
stack.

## Build & Run

```bash
cmake -B build -G Ninja     # configures + wires libzune's Makefile
cmake --build build         # builds libzune.a, then the app
./build/zuuned
```

Qt 6.8 or newer is required, including Qt Quick Controls, Dialogs and
Qt5Compat GraphicalEffects. See `README.md` for the complete Arch and
Debian build dependencies; the app also links mpv, FFmpeg, LAME and SQLite.

CMake drives `make -C libzune` automatically — after C changes just
rebuild; no manual copy step (unlike ZuunedMac's `cp libzune.a ../lib/`).

## Submodule Discipline — READ THIS BEFORE TOUCHING libzune

These rules exist because each was violated once and each violation cost
real debugging time (2026-08-27):

1. **libzune is pinned to `master`** — the wire-protocol line merged
   there 2026-09-02 (ZLP framing, autopsy codes, retry triage,
   mid-transfer abort, OpenSession auto-reset, playlist forge). The
   branch is recorded in `.gitmodules`; update with
   `git submodule update --remote libzune`, then commit the new pin.
2. **Never commit build artifacts** (`obj/`, `*.a`, `*.so`) — in libzune
   or here. Committed macOS `.o` files broke Linux three ways: `ar`
   choked on the Mach-O archive, the linker rejected stale objects, and
   16 "modified" files blocked branch switches. `.gitignore` now covers
   them; keep it that way.
3. **Linker says "file format not recognized"?** Stale Mach-O objects
   from a mac checkout: `make -C libzune clean`, rebuild.
4. **Pushing libzune changes:** commit on `master` (or a new branch +
   PR), push, then bump the pin here. Both apps consume libzune —
   protocol fixes belong in the submodule, never patched app-side.

## Key Architecture

```
┌──────────────────────────────────────┐
│ QML UI (qml/)                        │
│ Main.qml · Theme.qml design tokens   │
└──────────┬───────────────────────────┘
           │ Qt properties / signals
┌──────────┴───────────────────────────┐
│ DeviceService (C++ QML singleton)    │
│ = MTPService.swift of this app       │
│ libzune calls off-thread             │
└──────────┬───────────────────────────┘
           │ #include "zune.h" (plain C)
┌──────────┴───────────────────────────┐
│ libzune (static .a, submodule)       │
│ Zune Rebellion API                   │
│ breach, smuggle, forge, purge, etc.  │
├──────────────────────────────────────┤
│ Native PTP/MTP/MTPZ Stack            │
│ Split header/data · ZLP rule         │
│ SendObjectPropList (0x9808)          │
└──────────┬───────────────────────────┘
           │ usb_libusb.c backend
┌──────────┴───────────────────────────┐
│ libusb-1.0 (userspace — no driver)   │
│ detach kernel driver, claim iface 0  │
└──────────┬───────────────────────────┘
           │ USB bulk pipes
┌──────────┴───────────────────────────┐
│ Zune Hardware                        │
│ VID=0x045E PID=0x0710/063E           │
└──────────────────────────────────────┘
```

## Directory Structure

```
zuunedlinux/
├── CMakeLists.txt          Build config (drives libzune's Makefile too)
├── src/
│   ├── main.cpp            Entry point
│   └── DeviceService.*     libzune bridge — ALL device calls go here
├── qml/
│   ├── Main.qml            App shell
│   ├── Theme.qml           Design tokens (ported from ZuunedMac Theme/)
│   └── fonts/              Permanent Marker, Specimen Alien (bundled)
├── packaging/
│   └── 68-zuuned.rules     installed as both 68- and 72- rules
└── libzune/                submodule — branch master
```

## Code Conventions

### C++
- C++20, Qt idioms. `QML_ELEMENT`/`QML_SINGLETON` registration, not
  manual `qmlRegisterType`.
- **All libzune calls go through DeviceService**, and always off the UI
  thread (QtConcurrent + QFutureWatcher). libzune is synchronous C — one
  device operation at a time, same discipline the `actor` gave ZuunedMac.
- Results land on the UI thread via signals; QML reads reactive
  properties only.

### QML
- **Every color, spacing, radius, and duration references `Theme.qml`.**
  Never raw values in views — same token names as the Swift source
  (`ZuneColors.swift`/`ZuneStyles.swift`). When the mac theme changes,
  port the token, not the usage sites.
- QML is the native scene graph, not a webview. No JS libraries, no
  network in the UI layer.

### C (libzune)
- C99, Zune Rebellion naming (breach/smuggle/forge/purge…).
- See `libzune/AGENTS.md` for full conventions and `libzune/docs/TOC.md`
  before searching the filesystem.

## Device Testing

### zunetool — test protocol changes WITHOUT the app

```bash
cd libzune && make zunetool
./zunetool info              # breach + MTPZ auth + device info
./zunetool list              # library listing
./zunetool send <mp3> [title] [artist] [album] [genre]
./zunetool purge <item_id>
./zunetool torture <file>    # 20× send — flushes ZLP edge cases
```

Every failure prints `zune_autopsy()` — the device's real PTP response
code. `0x2005` = refused on purpose (don't retry), `0x200C` = store
full, `0x02FF` = transport wounded (reset/unjam territory).

### USB access
- `packaging/68-zuuned.rules` is installed as both `68-zuuned.rules` and
  `72-zuuned.rules`: suppress libmtp probing before 69, reapply exclusions
  before seat-late ACLs at 73. The app embeds the same canonical bytes.
  See `packaging/TESTER_INSTALL.md`; never restore a lone 45/99 rule.
- Dev box note: a homegrown `zune_usb` DKMS module (author BadgerOmen)
  may already claim the device and provide open node perms. libusb
  detaches it automatically on open; it does not conflict.

### Hardware quirks (learned on a real Zune 30, Keel family)
- **OpenSession wedge:** after ~2 breach/sever cycles the device may
  stop answering OpenSession (bulk IN timeout, autopsy 0x02FF).
  `zune_breach()` auto-resets and retries once and now prints what the reset
  returned. On Linux the fallback is `usbreset 045e:0710`; on Windows
  `libusb_reset_device()` reports success without reviving the player, so the
  fallback there is a power cycle - `libzune/docs/WINUSB_QUIRKS.md`.
- **ZLP rule:** in split header/data mode the zero-length packet decision
  uses PAYLOAD length, not container length (`ptp.c`). Wire-verified
  against the Windows 8 client. Don't "simplify" it back.
- **Finalize on Keel:** vendor ops 0x9201/0x9202 return 0x2005
  OperationNotSupported — expected, not a bug. CleanDataStore (0x9108)
  is what triggers re-index.
- **4 GiB cap:** MTP ObjectCompressedSize is 32-bit; libzune refuses
  larger files rather than silently truncating.
- New content may not appear in `list` until the device re-indexes.

## Customize sheet

**Connection rule (user decisions, 2026-09-06 and 2026-09-27):** every addition
to the Zune transfer queue requires a connected Zune. Media drags require a
connection except local photos dragged into the photo-album builder: that
organization workflow is explicitly offline. Local photo drags expose a
separate target key; device drops recheck connection. Opening the music
playlist builder is not an exception. Playback queues, metadata/artwork edits,
and playlist creation/membership/order edits work without a device. Recheck
connection when an action/drop/picker completes; SyncEngine enforces the same
rule at its add boundary. Existing saved transfer queues survive disconnects.
The action audit and native verification gates are documented in
`docs/ACTION_AUDIT_FIXES.md`.
Queue remove targets, the custom ZUUNED badge, device-row library membership,
and the bottom notification strip are covered by `docs/QUEUE_AND_FEEDBACK.md`.
Video transfer titles, filename/readback separation, existing-device title
repair, and silent gallery scans are covered by `docs/VIDEO_TITLES.md`. Never
use an episode title alone for device dedup or interrupted-send recovery.

**Customize / Sleeve (2026-09-06):** the user-approved native redesign is on
`ux/6-customize-design`. Read `docs/SLEEVE_CUSTOMIZE.md` before changing it.
Artwork and identity are one staged draft; browsing must never commit. Apply
prepares resources off-thread and reports actual save completion. Video schema
v3 separates `custom_poster` from manual identity so automatic matching can
preserve art without freezing unmatched items. Keep request correlation and
the automatic-matcher write guards intact. Music identity is manual; music
reset releases artwork only. Future mockups default to wireframes unless the
user asks for a richer study.

## Known Issues

The active safety/identity cleanup and its verification gates are tracked in
`docs/CODEBASE_CLEANUP.md`. Device disc/track availability varies; software
matching tests do not certify physical readback. Hardware photo transfer and
host audio/GPU/USB release gates remain separate. Classic per-media byte
discovery is deferred, not an active release blocker.

Historical rendering issues were resolved in August: stale OpacityMask content,
per-pixel QML mask generation, and uncapped mask bite depth. Preserve the native
GrungeMaskProvider and the rim cap.

## Debugging

- App/libzune logs: stderr with `[prefix]` tags (`[ptp]`, `[mtpz]`,
  `[libusb]`, `[libzune]`)
- USB enumeration: `lsusb -d 045e:` · `journalctl -k | grep -i usb`
- Wedged device: `usbreset 045e:0710`
- Desktop interference: gvfs-mtp may probe the device — the udev rule's
  `MTP_NO_PROBE` handles it; check `pgrep -a gvfs` if something steals it.

## Roadmap

**2026-09-27 feature scope:** read `docs/FEATURE_COMPLETION.md` for the active
phased backlog: custom/nested photo albums first, then disc-aware device dedup,
on-device metadata/artwork editing and compatible hardware video encoding.
Device-browser video playback is explicitly out of scope (save-to-library
remains). Classic per-media storage byte discovery is deferred, potentially
requiring reverse engineering; do not treat it as an active release blocker.

**Custom photos, 2026-09-27:** schema v7 stores virtual photo albums, nested
parents and ordered many-to-many membership. `docs/PHOTO_ALBUMS.md` documents
the semantics. Organization is offline; album deletion preserves photo files
and library rows. Transfers select direct members only, never silently flatten
subalbums. Keep the backed-up migration, non-reused photo/album IDs, cycle/depth
validation, atomic create-with-members and stale-reorder guards. Software gates
live in `tests/photo-albums/` and `tests/actions/photo-run.sh`; packaged/device
photo presentation remains a separate release gate.

**Photo builder, 2026-09-27:** Photos New/Edit opens a floating island beside
the sidebar, matching mixtape composition. `PhotoTrayState` is an independent
staged draft; bulk selection survives folder navigation and local photo drags
work offline. While composing, photo plus buttons feed the draft. Save compares
the original album snapshot and atomically commits name/parent/member order;
stale saves keep the draft and fail truthfully. Dirty replacement/cancel requires
discard confirmation. Keep selection order, bounded GridView/ListView delegates,
picker-generation guards and device-drop connection checks. Gates: 73 native
service, 22 photo action and 10 builder checks; photo/builder suites also pass
X11/OpenGL, including a narrow 5,000-photo draft. Existing playlist actions pass.

**The authoritative plan is `docs/PORT_PLAN.md`** — a 10-phase sequence
covering the full mac-app port (shell/feel → transcode C → local
library + onboarding → music player → sync engine → video section →
video player → photos → playlists → packaging), each phase with a
hardware/verification gate. Read it before starting any feature work;
update its phase status and this file when a gate passes.

Done so far: protocol stack (hardware-proven), app chrome + grunge/spray
visual identity, device browsing with album art, hotplug auto-connect,
DeviceWorker serialization. **Phase 1 (Shell & Feel) complete**: splash,
device-presence bloom + genre mood color, toast host, panel slide motion,
settings shell with live Appearance tab + wallpaper layer, AppSettings
singleton (all mac keys), connection staging, device photo thumbnails.
**Phases 3-5 complete**: local library (SQLite/scanner/onboarding/
kubeplex CIFS mount), music player (mpv core, vinyl, ribbon, mini
player), sync engine — HARDWARE-GATED 2026-08-28: album synced from
FLAC with correct album/artist objects on the Zune 30. Critical libzune
finds: zero-length ZLP guard AND zero-byte abstract-object commit (the
1-byte hack contradicted the declared size → 0x2002 on every album
forge; BACKPORT TO MAC). Device deletion, on-device badges live.
**Phase 2 (transcode C layer) complete**: transcode/ builds vs system
FFmpeg 9 + LAME (`zuuned_transcode` + `transcodetool`); profile 0 =
wmv2+wmav2 ASF verified (Zune 30 video, no forked FFmpeg); libzune
probes in-process (USE_LIBAV) on Linux. LAME superframe chunking bug
fixed here — backport to the mac. Sync must sniff magic bytes, not
extensions (WMA-as-.mp3 files exist in real libraries).
**Phase 6 (video section) software-complete 2026-08-28**: video scan
(folder-truth series, skip rules, deferred probing), VideoNaming
(17-case self-test: `librarytool names`), TMDB/Fanart clients +
VideoMatcher on its own thread (series memo, absolute-anime mapping,
60s→24h backoff), VideoBrowserModel + LibraryVideosView (Movies/TV/
Anime/Needs-Match, poster cards, detail pages, bulk fix-match sheet),
FrameThumbnailService, video sync phase (transcode w/ retry → FAT32
wire names → smuggle movie/episode/clip/other w/ poster + description),
Settings Sync tab live (auto profile default: Zune 30 → WMV).
`librarytool videoscan <folder> <tmpdb> [movies|tv|anime]` is the
headless gate — PASSED vs live TMDB. Hardware gate PASSED 2026-08-28:
48-episode season → Zune 30 as profile-0 WMV, 0 failures. Video
PLAYBACK is Phase 7 — play buttons toast until then.

**Phase 7 (video player) — DONE 2026-08-29, eyeball-gated live.**
MpvVideoItem (QQuickFramebufferObject + mpv_render_context, OpenGL RHI
pinned in main; FLIP_Y=0 and NO setMirrorVertically — that double-
flips), VideoPlayerService (binge queue mirrored into mpv's playlist,
EOF auto-advance syncs index only, 90%-watched via saveVideoPosition,
5s position saves, music ducks on video start), VideoPlayerView
overlay (vinyl OR single-ring "disc" style — Settings → Appearance;
episode ribbon top-center on binge queues, jumpTo on card click; track
pickers anchored above their chips). Scrub bracket: beginScrub pauses,
150ms keyframe scrubs, endScrubMs exact-seek + resume — seeking while
PLAYING audibly loops the same keyframe. Disc style is a faithful port
of zuuned-web's Tg component (extracted from the minified bundle on
zw.badgeromen.xyz; geometry scales by size/200): band centered in the
poster→rim annulus, smooth 200-segment opaque gradient (translucent
segments seam at joints), rounded start cap, pink head just proud of
the band. Volume arc/mute layout is the web original — three attempts
to "improve" the mute chip all read worse; leave it.
DeviceVideosPage playback is explicitly out of scope (2026-09-27); device
videos can be saved to the library. Full-orbital vinyl uses real mpv chapters.

**Sync performance (2026-08-28):** three-layer speedup, all
hardware-gated. (1) NVDEC (CUDA) decode in the transcode layer —
22min 1080p x265 → WMV in ~38s vs ~2.5-3min SW (~4x); automatic SW
fallback; Settings toggle 'hwDecode'. (2) Pipelined video sync: ≤2
transcodes run ahead of the STRICTLY-SERIAL one-at-a-time USB sends,
≤3 buffered temps; failed markers keep the in-order cursor moving.
(3) libzune 1MB bulk chunks (was 256KB): 15+ MB/s sustained vs ~7
(zunetool torture 20/20 on Keel); '[ptp] sent N MB …(MB/s)' telemetry
on payloads >8MB. Net: a 12-episode season ~35min → ~8-10min.

## Development Methodology

### Core Principles
1. **Full-scale solutions, not shortcuts** — root cause, not symptom.
2. **Think before coding** — read the docs and captures first; the
   protocol answers live in `libzune/docs/` (WIRE_CAPTURE_FINDINGS.md is
   ground truth from the Windows client).
3. **Document everything** — hardware findings go in libzune docs;
   app decisions go here.
4. **No external runtime dependencies** — system libs only, bundled at
   packaging time. Nothing downloaded at runtime.
5. **Theme parity is a feature** — this app should be unmistakably
   Zuuned next to the mac app. Tokens stay in sync.

### Agent Workflow (for non-trivial tasks)
1. **Architect (Claude + User)** — define problem, design solution, set goals
2. **Developer Agents** — full context, no TODOs or placeholders
3. **QA Agents** — validate changes; hardware claims require a real
   device test via zunetool, not "should work"

**Application license (user decision, 2026-10-01):** original Zuuned app code
is GPL-3.0-or-later; see `LICENSE` and `COPYING.md`. Preserve separate third-party
licenses. Source will be available elsewhere; keep release source links accurate.

**GitHub distribution (user decision, 2026-10-04):** `badgeromen/zuuned-linux`
hosts Linux application source AND releases. `badgeromen/libzune` hosts the shared
C library. This supersedes the earlier downloads-only repository decision for
`badger-orson/zuuned`. Move public-facing links to the new account; do not alter
historical release evidence. Keep populated MTPZ data and private history out of
public source. Prepare clean histories and review author identities before push.

**MTPZ release behavior (user decision, 2026-10-03):** official downloads retain
embedded MTPZ authentication. Keep the populated header in private Gitea and
supply it to trusted release builds; omit its values from public source. Preserve
the external-file override. Public source preparation must not make release users
supply a separate authentication file. See `docs/RELEASE_ARCHITECTURE.md`.

**Git push (user decision, 2026-09-08):** always ask the user before every
push and wait for explicit approval. Earlier push approvals do not authorize
later pushes. Prepare and verify the local change before asking.

### Documentation
- **2026-10-03 public source preparation:** `tools/export-public-source.mjs`
  creates a fresh source snapshot without private history, the embedded MTPZ
  header or build artifacts. Approved provider API defaults remain. Private files stay
  in place. Read `docs/PUBLIC_SOURCE_EXPORT.md` before publication. A fresh push
  approval remains required; never mirror private Git history.
- **2026-10-02 local music copies:** schema v9 stores verified audio payload
  fingerprints for every indexed song, regardless of metadata completeness.
  Group by verified audio, preserving known full album and disc/track conflicts;
  incomplete metadata must never bridge ambiguous editions. Preserve raw source rows and
  playlist IDs; only the browser/model display list and counts are grouped.
  Existing playlist repetitions survive; new adds recognize equivalent source IDs.
  Hash work stays on the scanner worker, with stat guards and cancellation;
  never replace it with title-only or whole-file-size dedup. See
  `docs/LOCAL_MUSIC_COPIES.md` and `tests/local-music-copies/`.
- **2026-10-02 native music matching candidate:** read `docs/MUSIC_MATCHING_PLAN.md`
  before implementing the artist/album follow-up. Automatic discovery must not
  change tags, grouping or device dedup. Keep exact-release versus general-cover
  scope explicit, manual match persistence and local/custom art protection.
  October 2 correction: apply confidently matched general album covers
  automatically; only uncertain artist/album identity requires a match.
  Software gates pass (98 matching, 20 discovery, 7 persistence, 63 QML,
  actual Apply and regression suite). User acceptance/new packaging remain.
  Auxiliary `_artworkMatch`, `_artworkDiscovery`, `_artworkChoiceRevision` JSON
  fields are not metadata pins; preserve manual-save revision race protection.
- **2026-10-02 device artist reuse:** local libzune branch
  `fix/reuse-device-artists` makes forge_artist reuse a live Name match before
  creation. Failed inventory reads never justify creation. Existing duplicates
  remain; repeat-sync software gate is `libzune/tests/run-artist-reuse.sh`.
  Live Zune HD logs confirm reuse for Selena Gomez, Zweihänder and Post Malone
  (2026-10-02); existing-duplicate cleanup remains separate. See libzune/docs/
  ARTIST_REUSE.md for the Jelly Roll deletion/reindex caveat. Push libzune
  before staging a new pin.
- **2026-10-01 artist album evidence:** ambiguous/incomplete name searches can
  use up to three local album titles to corroborate exactly one candidate MBID.
  Reject conflicting credits and incomplete album responses; never replace a
  saved identity or fall back to an unrelated same-name artist. Context is
  ephemeral, not written into media tags. Provider fixture: 76 checks passed.
- **2026-10-01 automatic album artwork candidate:** `OnlineAlbumArtService`
  serializes missing-cover lookup after `AlbumArtService::requestSources`
  exhausts the complete local batch. Preserve exact artist/full-title or pinned
  provider matching, generation invalidation, Customize pause and atomic
  no-overwrite publication. Setup/imports share background discovery; Settings
  has Find missing artwork. Details/gates: `docs/MUSIC_ARTWORK_DISCOVERY.md`.
- **2026-10-01 multi-folder music artwork:** on-demand album covers try all
  known track paths, with per-source misses and serialized per-album fallback.
  Artist lookup failures get one delayed retry with identity/customization
  guards preserved. See `docs/MUSIC_ARTWORK_DISCOVERY.md` for tests and limits.
- **2026-10-01 display font:** Specimen Alien replaces Misdemeanor in app,
  previews and test resources. Legacy `headerFont=misdemeanor` resolves to
  `specimenAlien`; Permanent Marker remains the default. Source/coverage live
  in `tools/font-studio/`; preserve the separate original Specimen study.
  Font integration gates: settings persistence, QML bundled-family loading,
  and the Alien validator's declared 368-character Latin coverage.
- **2026-09-28 identity/recovery cleanup:** `MusicIdentity` is the shared policy
  for music dedup, badges, playlist linking and reverse imports. Ambiguous rows
  fail explicitly; missing disc/track metadata stays unknown. Schema v8 preserves
  pending playlist disc/track values with a backed-up migration. Prepared audio
  preserves source disc tags or an explicit positive override. Music/photo
  interrupted recovery requires an exact object ID, device serial, compatible
  live readback and a correlated completion; legacy/ID-less markers require
  manual review. Software gates and hardware limits: `docs/CODEBASE_CLEANUP.md`.
  Run `bash ci/check.sh` for the repeatable native regression suite.
- **2026-09-28 photo testing candidate:** the Debian AppImage now passes actual
  photo-folder import, staged parent/nested-child creation and restart
  persistence, native portal folder/open/save, diagnostics, and copied v5→v7
  profile migration with normal shutdown. Exact checksum and isolated evidence:
  `docs/releases/TESTER_PHOTOS_2026-09-28.md`. This is a labeled dirty development
  artifact; physical photo-transfer and host audio/GPU/USB gates remain separate.
- **2026-09-28 provider shutdown:** cancel `ArtworkHttp` on `aboutToQuit` and
  drain the global worker pool before destroying QML services or QApplication.
  Destroying a watcher does not cancel its QtConcurrent callable. Active replies
  must abort on their owning thread; MusicBrainz mutex/pacing/retry waits must
  observe shutdown too. The isolated stalled-request gate is
  `tests/provider-shutdown/run-backend.mjs` (6 checks; existing provider/cache
  suites also pass). This does not add cancellation to TMDB/scanning/transcode.
- **2026-09-09 device album-art import:** successful Zune music pulls preserve
  the separate representative-sample JPEG carried by the abstract album
  object. The image is normalized into the local album cache after extraction
  and never replaces local/custom artwork. `ZuneDBArtist` exposes only ID and
  name, so artist portraits remain online/custom with the flat fallback. The
  focused gate is `tests/device-music-import/run.sh`.
- **2026-09-09 desktop file pickers:** the user requires the desktop's native
  chooser and its own theme for every open/save/folder action. Keep the standard
  QtQuick.Dialogs API, bundle `platformthemes/libqxdgdesktopportal.so` built for
  the AppImage's Qt, and select it in AppRun. Native installs default to the
  portal when no platform theme is configured. Do not restore the `GTK_THEME`
  force or ship a replacement file manager. Packaging must verify the plugin;
  a runtime gate must prove real portal folder/open/save behavior, not merely
  click Qt's fallback dialog. The desktop supplies its FileChooser backend.
  The `87b2ecd` AppImage passes actual GTK portal folder/open/save selections,
  correlated by app/backend PIDs and returned URIs; diagnostics and copied v5
  upgrade also pass. See `docs/releases/TESTER_R2_DESKTOP_DIALOGS.md` for the
  exact artifact/harness and host-desktop limits.
- **2026-09-09 tester diagnostics:** automatic native stderr/Qt session logs
  start before GUI initialization and stop after app/worker teardown. Keep
  `--version` side-effect-free. Logs retain at most eight 2 MiB files, redact
  credentials/authentication dumps before persistence, and stay owner-only.
  Health exports a local, reviewable `.txt` report with allowlisted system/app
  details and bounded recent logs, off-thread with truthful completion. Never
  include a full environment, settings, database or automatic upload. Preserve
  snapshot reads outside the capture mutex and the process ownership lock.
  Native gate: 72 core checks (also ASan/UBSan), 34 report/QML checks. Read
  `docs/DIAGNOSTICS.md`. The clean `9a6eeb6` AppImage also passes actual Health
  export, private session-log lifecycle and fresh/copied-profile runtime gates;
  exact evidence is in `docs/releases/TESTER_R2_DIAGNOSTICS.md`. Physical device
  playback and the remaining release checks are still separate.
- **2026-09-08 tester preparation:** schema v6 stores disc numbers and
  successful probe fingerprints. Native libzune metadata merges container and
  selected-stream tags (Ogg/Opus included), with additive disc/year fields;
  consumers must rebuild. Failed probes preserve good rows and retry later;
  automatic repair preserves manual edits and collection art/identity pins.
  Album playback and transfer queues retain disc/track order. Migration backup
  uses SQLite's backup API so committed WAL pages survive. Native import checks
  (33), native/fallback probe checks (64 each, sanitizers), migration checks (19)
  and QML music checks (23) pass. See `tests/music-import/README.md`.
  Packaging now checks its Qt/QML dependencies, bundles FFmpeg executables,
  identifies app/libzune builds, and verifies canonical USB installation.
  `docs/TESTER_RELEASE.md` tracks candidate, clean-system and hardware gates;
  these are separate from a successful developer build.
  **Packaging compatibility:** QML resources are aliased into the `Zuuned`
  module root with `NO_GENERATE_EXTRA_QMLDIRS`; preserve relative asset paths.
  The former subdirectory redirect produced an ambiguous self-import on Debian
  Qt 6.8.2. Both 6.8.2 and the native 6.11.2 build now pass. AppImage inventory
  checks every bundled ELF/plugin dependency against a narrow host allowlist;
  linuxdeploy's broad blacklist alone is insufficient. Tools and the AppImage
  launcher runtime are checksum-pinned. The container rehearsal uses no USB or
  network, disposable XDG profiles and an optional read-only copied v5 profile;
  this never substitutes for host hardware/audio/GPU checks.
- **2026-09-08 Artwork print Appearance integration:** the main app now has
  Original / Clean ink / Halftone / Worn print choices, a live local sample,
  collapsed fine-tuning and per-look persistent reset. Original is the default;
  accepted print presets are Detail 100, Halftone strength 14 / Dot size 17 /
  color, Worn wear 14. `ArtworkPipeline` + `ArtworkRequest` share native local
  processing and cache work; `ArtworkImage.qml` routes selected local artwork
  on album/artist/poster/player/queue/genre/mixtape/Customize surfaces. Keep its
  public source URL original: metadata, artwork choices and device transfers
  never use derived print bytes. Photos, wallpapers, app marks and remote
  provider candidates retain original rendering. No Zune is required.
  Gate passed: native build, 58 pipeline checks, 12 OpenGL display passes,
  settings persistence/interactions and 117 existing surface/action checks.
  The app opens on Settings and the actual Appearance capture is warning-free.
  Keep image opacity switching for hidden mask sources and the displayed(url)
  acknowledgment that protects both current and loading PNGs from eviction.
  Read `docs/ARTWORK_STYLE_STUDY.md` for the approved defaults and gate.
- **2026-09-07 local print proof:** `src/artwork/PrintRenderer` now provides
  deterministic CleanInk/Halftone/WornPrint processing. The separate native tool
  in `tools/artwork-preview/` reads cached artwork and opens live comparisons;
  it remains a developer proof alongside the main-app integration above.
  Original image bytes, alpha, stale-worker publication and cache pruning have isolated tests under
  `tests/artwork-print/`. Read the tool README before expanding this pipeline.
  **2026-09-08 wear reference:** the user's worn VHS carton calls for chipped
  edges plus rubbed coating and sparse surface scratches/creases. Wear is seeded
  by image content: different artwork gets different damage, stable across
  controls/reopening. A border alone or dense noise across faces misses the brief.
  Surface scuffs use warped gradient noise and directional rubbing; do not
  restore the square-cell thresholding that made damage look like QR patterns.
  **Comic Halftone:** the preview has a Halftone-only Dot size slider and
  color/black-and-white choices. These controls must not alter or invalidate
  Original/Clean/Worn variants; keep their worker correlation and cache tests.
- **2026-09-07 artwork study and parsing audit:** optional print treatments are
  explored in `docs/ARTWORK_STYLE_STUDY.md` and `mockups/artwork/`; these are
  generated concepts whose provenance remains separate from the implemented
  native filters and Appearance integration. The music/video reports under
  `docs/audits/` distinguish native reproductions from code-path findings and
  preserve diagnostics. Consult them before scanner/matcher fixes or a rewrite.
- **2026-09-07 provider correction:** Fanart v3's HTTP200 `{}` means no artwork.
  Same-name artist browsing preserves successful portraits despite absent/failed
  siblings. MusicBrainz identity and album-art searches share cached requests,
  temporary-failure cooldowns and bounded stale positive fallback. See the
  provider outage gates in `tests/music-identity/README.md`.
- **2026-09-07 library completion:** Sleeve Customize now matches album/artist
  identities online and stores genre/mixtape artwork. MusicBrainz/Fanart/Deezer
  remain native, cached worker services; artist choices use stable provider IDs.
  Playlist imports reserve ordered nullable slots (schema v4); collection
  overrides use schema v5. See `docs/SLEEVE_CUSTOMIZE.md`,
  `tests/playlist-order/README.md`, and `tests/music-identity/README.md`.
  Virtual/nested photo albums were implemented September 27; expanded device
  editing remains upcoming. Local collection editing never requires a Zune.
- `libzune/docs/TOC.md` — exhaustive function index
- `libzune/docs/LINUX_TESTING.md` — the Linux rig this app grew from
- `libzune/docs/WIRE_CAPTURE_FINDINGS.md` — wire-protocol ground truth
- `README.md` — user-facing build + udev setup

### Restarting the app (HARD RULE)
On this Omarchy development desktop, read and use the Omarchy skill for window,
launch and capture operations (user preference, 2026-09-08). Prefer its command
helpers; do not guess older Hyprland dispatcher syntax.

NEVER kill/restart the running app without first checking the current
app log for an in-flight sync (look for a send in progress with no
matching done line). Killing the process mid-`SendObject` aborts the
USB session with the object's DB row already created on the device —
the Zune is left with a truncated media entry that CRASHES THE
FIRMWARE on playback (device restarts when playing). Happened
2026-08-29 with a 2.65GB movie mid-wire. Recovery: delete the partial
object off the device, then clean Eject to trigger re-index.

## 2026-10-05: Experimental music visualizer

Read `docs/MUSIC_VISUALIZER.md` before extending `VisualizerService`.
`ZUUNED_VISUALIZER` defaults OFF; current development builds opt in with projectM
4.1.7 and libpulse. Preserve process-specific mpv stream identity and exact
per-stream monitoring; never capture desktop audio or microphones as fallback.
OpenGL creation must wait for a usable drawable, and default draw/read buffers
must be restored explicitly. Escape preserves music and the library window.
The isolated native gate is `tests/visualizer`; existing playback regression is
`node tests/customize/run-backend.mjs --player`. Preset/product polish and
AppImage packaging remain follow-up gates, not completed release claims.

**Live mouth correction, 2026-10-05:** Zuuned Viz now uses `PortalState.h` and
`PortalRenderer.h`, independently of the unchanged Winamp/projectM preset.
The mouth is the current PCM-derived contour. Store immutable previous contours
for trails, expand/fade them, and mask them against the live mouth before drawing
its edge. Never let retained pixels determine the opening. The later approved
base-shape scheduler is described below; palette changes stay audio-driven.
Colors follow a slow separate envelope.
The response and real-GL mouth tests in `tests/visualizer` verify the actual
contour and clear opening; generic pixel movement is insufficient. User visual
acceptance and packaging remain separate. See `docs/MUSIC_VISUALIZER.md`.

**Bass/color/smoke, 2026-10-06:** Zuuned Viz uses continuous low-pass bass RMS
and a separate transient envelope so normalization cannot flatten low-end
response. Keep the slow, rate-limited audio-steered palette. Smoke surrounds
saved contours outside the live-mouth mask; it never reshapes the mouth.
Gates: 13 state, 8 GL, 14 host checks; visual acceptance remains separate.

**Sustained bands/random shapes, 2026-10-06:** the user now explicitly wants
smoothly varying base shapes as well as the live audio deformation. Preserve
`PortalShapeMorph`'s minimum hold, 3.2-second eased transitions, no immediate
family repeats and silence/pause freeze. Spectral sustain determines per-point
line width; saved contours retain their widths. Space broad bands apart and
keep the opening masked even under the thick live rim. Note duration remains
an approximation from mixed audio. Gates: 21 state, 10 GL, 14 host checks.

**Tiny mouth/travel, 2026-10-06:** keep audio-space radii separate from the 0.1
opening projection in `PortalRenderer`, and apply that projection to the mask
as well as the ribbons. Centers are viewport fractions; never amplify them by
depth scale. Historical centers ease into the path with age while contour
snapshots remain immutable. Pause freezes travel. Preserve visible foreground
walls despite the tiny opening. Gates: 25 state, 30 GL, 14 host checks; square,
ultrawide and portrait readbacks assert a nonempty clear mouth. User visual
acceptance and release packaging remain separate.

**Private checkpoint, 2026-10-08 (user decision):** the current application
snapshot, including the experimental visualizer, is for private Gitea only.
Do not publish this checkpoint or its visualizer to GitHub, export it as public
source, or include it in public releases without a new user decision. Preserve
the corresponding libzune checkpoint when restoring the application. A local
checkpoint is not a release gate or permission to push; the existing per-push
approval rule still applies.


**2026-10-09 podcast app transfer:** typed podcast queue and worker routing now
exist. Podcast show covers use DragGhost to open episode selection on device
drop; episode selections drag into the existing device queue. Feed-only selection
originally downloaded before live admission; the quiet queue correction below
moves that work into sync. Persist `deviceSourceUrl` across GUID-preserving
feed refreshes, and never use music canonicalization/identities for podcasts.
Read-only exact feed/source lookup excludes already-present episodes before
queue insertion and repeats before send. Keep the unknown-write recovery marker;
podcast auto-purge is not supported. Spoken worker jobs dispatch one at a time
because the HTTP nested event loop otherwise reenters subsequent database jobs.
Keel app-object hardware gate uploaded Moonbound episode 13, accepted show art,
finalized, and excluded the same episode on a later admission. The user confirmed
podcast playback and visible artwork on Classic; firmware resume and HD podcast
acceptance remain pending. Audiobook integration and limits are recorded below.


**2026-10-09 audiobook GUI candidate:** libzune exposes experimental
`zune_find_audiobook_part` / `zune_smuggle_audiobook_part` for Keel/Pavo.
Small two-part MP3 gates proved schema 0x12, ordered metadata and exact audio/JPEG
readback on both; the user accepted the Classic test. The HD omits native book
parts from MP3-filtered inventories, so lookup uses wildcard inventory then
schema filtering. Never infer absence from the MP3 listing. The app hashes the
reviewed whole-book source set, keeps book/part identities separate from music,
rechecks device identity before sending, copies and verifies immutable source input before audio
preparation, and reports books separately. Books use one + or cover/row drag for the whole book, with no part picker.
Chaptered M4B sources now map to ordered named device parts; intros/gaps/outros
are included. Source copies are verified once per source and reused across ranges;
large source containers are supported, prepared parts remain capped at 512 MiB.
Whole-book capacity admission is atomic. Local chapter playback remains.
Unknown writes keep manual-review recovery, never speculative purge/retry.
Firmware resume and HD visual/playback acceptance remain separate gates.


**2026-10-09 whole-book correction:** the user rejected partial book selection
and requires chaptered M4B. `queueAudiobooks` accepts book or part IDs but always
expands whole parent books. Podcast selection stays unchanged. Native range
encoding uses local-only FFmpeg decoding, PCM sample trimming, LAME delay/padding
metadata and cancellation cleanup. Never send an entire chaptered source once
per chapter or infer duplicate identity from its shared filepath. Range fields
persist in queue/recovery. Source immutability covers the entire sync using a
shared temporary copy; release it after its final range. See the latest section
of the spoken plan and `tests/audio-ranges/` for verification and hardware limits.


**2026-10-09 quiet spoken queue correction:** adding either spoken type must use
cached library metadata synchronously, through the existing queue. No file stat,
probe, full-file hash, download or live device query belongs on the add path.
No success/preparation toast or library-page busy banner for normal admission.
Connection/capacity and queued-identity checks still apply. Books remain whole-book
operations; podcast episode selection remains explicit. Drafts persist with
`needsPreparation`, including feed-only podcasts without a local path. Sync alone
requests token-correlated quiet preparation from SpokenMediaService, resolves
book hashes/episode downloads and verifies the queued manifest, then encodes and
sends with live device dedup. Cancellation/disconnect must drain the correlated
preparation before another run. Never send a provisional book key to libzune.
Use `foregroundBusy` for library task feedback; sync work belongs in the existing
queue/device progress. Keep stable bookId grouping across content-key resolution.

**2026-10-10 spoken onboarding:** optional Podcasts and Audiobooks steps follow
Photos. `PodcastPreferences` is shared with Settings → Library; keep setup edits
staged until completion and commit a focused custom-count field before saving.
Automation/cleanup remain opt-in. Audiobook import reuses the reviewed manual
folder importer; only successful import changes Skip to Next. No watch-folder
promise or implicit book sync. See the spoken plan and organization QML gates.

**2026-10-11 audiobook Libraries builder:** `BookTrayState` and `BookLibraryTray`
replace the modal spoken collection editor. The user-facing label is Libraries;
existing native collection IDs/schema remain unchanged. Local book drags into
this organizer are explicitly offline using `zuuned-local-books`; device keys
remain connection-gated. Cover/detail + feeds the draft while building. Preserve
whole-book identity, ordered deduplication, revision-checked saves, pending-write
locks and dirty replacement/cancel confirmation. Playlist/photo/book drafts stay
independent in the shared sidebar location. `RoundedArtwork` clips rectangular
spoken covers using Theme.radiusMd; fitted portraits retain their full image.
See the spoken plan and `tests/spoken-organization/` for gesture/regression gates.

**2026-10-21 Windows path compatibility:** MTPZ authentication and both audio
directions now work against a real Zune HD on Windows by hand test. The three
symptoms that blocked it were path spelling, not protocol: an upload answered
`Fail to save`, the music root displayed as `/E:/Temp/testmusic`, and Settings
called a visible folder unreachable. Folder pickers hand QML a `QUrl`; stripping
the scheme by hand is correct by luck on Linux and produces a drive-relative
`/E:/Music` on Windows. `src/library/LibraryPaths.h` now owns the one canonical
storage form - decode with `QUrl::toLocalFile()`, forward slashes, repair a stored
`/E:/` scar, fold the drive letter to upper case because `UNIQUE(path)` is a byte
comparison - and `ZPathText.canonical/url/display` are the QML-facing forms, named
`ZPathText` because `Qt6Quick.dll` exports a `PathText` shapes type of its own. No QML
site may strip a scheme or concatenate `file://` again: that form spells
`file://E:/Music`, parses host `E:` and loads no image. `LibraryDb::open()` repairs
`watch_folders` rows an older build wrote, dropping a row whose repaired spelling is
already registered. `QFile::encodeName()` is the ANSI code page, not UTF-8:
`LibraryScanner`, `ArtworkPipeline`, `ArtistImageService` and
`OnlineAlbumArtService` pass `toUtf8()`, because a scan root the local code page
cannot spell otherwise fails `stat()` and the scan still exits 0 looking complete
- invisible on a Chinese host, met immediately on an English one. `librarytool` puts
the console in UTF-8. Gates: `tests/win-usability/watch_folder_repair.py` and one
scan root per script in the `fixture` phase. Read
`docs/WINDOWS_COMPAT_AUDIT.md` before touching path handling, and note its
deliberately-cleared list. Windows device scope remains exactly one Pavo Zune HD:
read, authenticate, send audio, save audio back. No deletion, no podcast,
audiobook, photo or video transfer, and no other model has been plugged in.

**2026-10-21 Windows round trip, logged end to end:** the two audio directions are
now proven from a log rather than from an operator's word. `--qa-device` in
`qml/Main.qml` drives one real upload through the ordinary `SyncEngine` admission,
or - with an empty file field - one Save-to-Library download of that title, and
reports `[qa-device]` lines; without the flag it is inert on every platform. An
upload out of a Chinese music root ended `forge done: 1 ok, 0 failed` and
`sync-summary tracks=1 failed=0`, and the next session found the track again
(187 tracks) and saved it back, where the MP3 audio is byte-for-byte the 401,239
source bytes - the only difference is the ID3v2.3 retag this app writes before a
send plus the ID3v1 it appends. A finished sync finalizes with CleanDataStore and
releases the player, so upload and pull are deliberately separate launches and the
second one also proves the upload survived the device's own re-index. Two
Windows-only reporting faults found on the way: `qPrintable()` is `toLocal8Bit()`,
so on a CP936 host every `[sync]` and `[sync-canon]` line naming a CJK path
arrived as U+FFFD while the C layer was correct - `src/` now uses
`qUtf8Printable()` throughout, the same bytes on Linux, and mpv's `alang` wants
UTF-8 anyway; and the harness tested its own verdict behind a device-presence
guard that a completed sync had already cleared, so it printed no verdict of its
own - it now latches on `onSyncSummary`. `devicetool`, `playertool` and
`spokenmediatool` put the console in UTF-8 too. Gates unchanged: `tests/win-compat`
171 checks, `tests/win-usability` launches=17 checks=94 failures=0 skips=0
killed=0. Still not claimed: anything listened to on the player, deletion,
podcast, audiobook, photo or video transfer, and any model but this Pavo Zune HD.

**2026-10-21 Windows full re-check on one binary:** the Windows surface was walked
again end to end against a single `--clean-first` build rather than inherited from
earlier checkpoints. `tests/win-compat` 184 checks / 0 failures / 0 skips,
`librarytool paths` 28 checks, `names` and `customize` all pass, and
`tests/win-usability` launches=17 checks=95 failures=0 skips=0 killed=0
`OVERALL=ok`. The `--qa-device` round trip ran again in both directions from that
same binary: out with `add added=1` and `sync-summary tracks=1 failed=0`, then the
next session found the track at `readbackDurationMs=8000` and answered
`download-saved name='SineQA1021.mp3' ok=true`. A decoder from outside this
repository confirmed the pulled file is MP3, mono, 44.1 kHz, 8.0457 s, and that
all 193,723 audio bytes match the source; the 1,177-byte difference is exactly the
rewritten ID3v2.3 plus the appended ID3v1. A tagless source therefore comes back
carrying only the injected album artist in its file tags, because the identity the
Zune shows lives in the MTP object, which came back correct. One GUI-gate launch
failed once on this binary and passed a clean re-run: `06-page-deviceMusic` exited
124 on a white frame with no `[qa] splash report`, and its own log held 712
clipboard complaints - 474 `Retrying to obtain clipboard.` and 237
`Unable to obtain clipboard.`, seven a second for 36 s. No source file in this
repository mentions a clipboard; Qt's own Windows clipboard backend opens it and
retries with a sleep on the GUI thread, and while a foreign process held it
`SplashView` never reached `onFinished`, so the tick timer the QA handshake
reports through never started. A clean launch, a process holding the clipboard
open, and a process writing it every 120 ms all reproduced nothing. Recorded as an
environment fault, not a regression. Scope claims are otherwise unchanged: still
one Pavo Zune HD, still no deletion, no podcast, audiobook, photo or video
transfer, and nothing listened to on the player.

**2026-10-21 Windows scan fix:** a watch folder can contain itself on Windows and
`QFileInfo::isSymLink()` will not say so — a `mklink /J` junction is a plain
`FILE_ATTRIBUTE_DIRECTORY` to Qt, and making one needs neither elevation nor developer
mode — so every recursive `QDirIterator` scan walked `music/loop -> music` 41 levels deep
to the `MAX_PATH` ceiling and reported a clean, complete scan on the way, filing the same
songs at 41 paths. `LibraryScan::walkFiles()` is now the only library walk, for music,
video and photos: explicit-stack depth-first, each folder keyed on the `(st_dev, st_ino)`
pair the shim takes from the volume serial and the NTFS file index, links listed but never
descended, and `complete` false whenever a folder could not be read, so a partial walk can
neither certify a watch root nor clear a stored probe failure. `librarytool paths` is now
39 checks, all passing (the 11 new ones build their own `mklink /J` fixture in a
`QTemporaryDir`: each song exactly once, the loop counted and never entered, a link out
followed once under the link's own path, POSIX symlinks still not entered), `names` and
`customize` pass, and on disk a three-song folder with `loop -> music` scans to
`3 tracks added` where a naive recursion counts 192 entries. Gates on one relinked binary:
`tests/win-compat` 184 checks / 0 failures / 0 skips, `tests/win-usability`
launches=17 checks=95 failures=0 skips=0 killed=0 `OVERALL=ok`, and the `--qa-device` round
trip in both directions — `add added=1`, `forge done: 1 ok, 0 failed`, `sync-summary
tracks=1 failed=0`, next session 191 -> 192 device tracks, `download-saved ... ok=true`
with 193,723 audio bytes byte-identical to the source. Harness trap worth keeping: the spec
contains spaces, so it must arrive as one quoted argv element — passed unquoted,
`qaDeviceSpec` returns null, nothing runs, and the launch looks ordinary. Not fixed, and
recorded as a gap: the audiobook import walk at `src/spoken/SpokenMediaService.cpp:1123`,
and the `MAX_PATH` ceiling that now also bounds folder depth (no `longPathAware` manifest
entry). The manifest half of that sentence is closed by the entry below; the audiobook walk
is still open. Scope claims are otherwise unchanged: one hand-tested Pavo Zune HD, still no
deletion, and nothing listened to on the player.

**2026-10-21 Windows long-path manifest, playlist naming, UTF-16 m3u:** three gaps the
entry above recorded as gaps are closed, and one of them was load-bearing in a place nobody
was looking. `longPathAware` is opted out of per executable and nowhere else -
`LongPathsEnabled` alone is necessary and not sufficient - so
`packaging/windows/zuuned.manifest` now goes to all six Windows executables through
`zuuned_long_paths()` (`CMakeLists.txt:166`). CMake has to be told `/MANIFEST:NO` before it
will accept one: CMake writes its own `CMakeFiles\<tgt>.dir\embed.manifest`, two manifests
collide on resource ID 1, and the link dies in `cvtres` with `CVT1100` then `LNK1123`,
which is why that file is a whole manifest with `asInvoker` in it rather than a fragment
and why every link is confirmed by reading resource 1 back with `mt.exe`. Measured rather
than assumed, because the obvious test answers wrong: `QFile`, `QDir` and
`LibraryScan::walkFiles()` reach a path past 260 characters with the declaration absent,
`_wfopen()` reaches it only with it - and FFmpeg, LAME and SQLite all arrive at a file
through the C runtime, so a gate written only against Qt would have concluded the
declaration buys nothing. `librarytool paths` is 52 checks, all passing (the thirteen new
ones: three `playlistFileName:`, three UTF-16 `decode:`, five `deep:`, two `deep crt:`),
`names` 17 and `customize` 21 pass, `tests/win-compat` is 184 checks / 0 failures / 0
skips. `tests/win-usability` had to be repaired first: a re-run of it caught the app
aborting on its way out (`0xC0000409`, last log line `[qt.fatal] QThread: Destroyed while
thread '' is still running`) because `~DeviceService` bet five seconds on the device worker
and then destroyed a running `QThread` anyway, which Qt makes fatal;
`Zuuned::drainThread()` (`src/ThreadDrain.h`, used by `~LibraryService` too) waits a worker
out and says how long it is waiting, and the gate then answers launches=17 checks=95
failures=0 skips=0 killed=0 `OVERALL=ok` on the rebuilt binary. Shutdown rule for any new
worker thread: never destruct a running `QThread` - drain it. The audio round trip repeated
on the same build (`SineQA1021C`: `add added=1`, `forge done: 1 ok, 0 failed`, `sync-
summary tracks=1 failed=0`, device 192 -> 193, `download-saved ... ok=true`, payload
193,723 bytes with no differing byte, the firmware appending its own 128-byte ID3v1).
`LibraryPaths::playlistFileName()` replaces the export's hand-rolled rule on Windows only -
a playlist called `NUL`, and the trailing dot or space that folded two playlists onto one
`.m3u8` - and spells the old rule out on POSIX so every existing Linux export keeps its
filename byte for byte; `PlaylistText::decode()` honours a UTF-16 BOM in either byte order,
ahead of the UTF-8 test. Harness discipline, learned the confusing way: the profile-
isolation pair in `tests/win-usability` compares the *real* profile before and after, so a
`zuuned.exe` left running from an earlier check makes it fail truthfully (`Qt pipeline
cache stayed out of the real profile`) while the sibling check for the gate's own processes
passes. Close the app before that gate and restart it after, per the handoff rule at the
top of this file. Still one hand-tested Pavo Zune HD, still no deletion, nothing listened
to on the player, and the audiobook import walk at `src/spoken/SpokenMediaService.cpp:1123`
plus the 255-character component limit in `safeComponent()` stay open gaps, and
`~SpokenMediaService` waits its worker out with no bound at all - it cannot abort, but it
can hold a closed window open. Closing the window with a device operation in flight now
waits up to twenty seconds for it and exits 0, where it used to abort; a worker still
running after twenty-five seconds is terminated, which is a bound this repository chose
over a crash dialog and has not hardware-tested mid-transfer.

**2026-10-21 Windows spoken close:** the entry above records `~SpokenMediaService`
waiting its worker out with no bound at all, and that is closed here - the sentence
should be read as superseded. `SpokenMediaService::shutdown()` used to hand the last
resume-point write to its own thread over `Qt::BlockingQueuedConnection`, which has no
deadline, and the destructor followed it with a `QThread::wait()` carrying none either,
so a close landing during an RSS fetch, an episode download or an audiobook probe waited
for that job as long as it liked. It is the pass above's `qt.fatal` seen from the other
side: that one died on `QThread: Destroyed while thread`, this one never reached the
destructor, and what a user got was a window that had closed with the process still in
Task Manager. The write is a queued post with a two-second deadline now, printing
`[spoken] last position not saved after 2000ms, closing anyway` when it does not land,
and the destructor calls `Zuuned::drainThread(m_thread, "spoken", 2)`, where the `2` counts
two five-second slices rather than two seconds. Why a `terminate()` last resort reads
tolerable here after this file argued against it once is scope rather than nerve: nothing
under `src/spoken/` calls libzune or holds a USB handle, the encode a spoken transfer needs
belongs to the sync worker, and podcast and audiobook transfers are prepared there and sent
by `SyncEngine`/`DeviceWorker` - so the worst the cut can do is abandon a feed fetch, a
probe, a cover write or a download, and a download lands through `QSaveFile`, so an
abandoned one never appears at the episode path and costs at most a scratch file beside it.
`tests/win-usability` gained a `spokenclose` phase, the first thing in this repository to
watch a spoken close rather than reason about one, and one clean rebuild answers
`launches=18 checks=99 failures=0 skips=0 killed=0` with `OVERALL=ok`, alongside
`librarytool paths` 52, `names` 17, `customize` 21 and `tests/win-compat` 184 checks / 0
failures / 0 skips. What it does not prove is written down rather than inferred: the last
resort has not fired, and no Windows gate plays a podcast or an audiobook, so the store
that close leaves behind answers `rows=0` and the deadline bounds a branch the harness
never arrives at. Details in `docs/WINDOWS_PORT.md` and `docs/WINDOWS_COMPAT_AUDIT.md`.

**2026-10-21 Windows photo shell-out, and the code page behind it:** the sweep entry above
described `zune_arm_photo()` as one of the shell-outs nothing calls. It is called, and from
the live photo transfer - `src/DeviceWorker.cpp:736` -> `zune_smuggle_photo()` ->
`zune_arm_photo()` - and its NULL return tells that caller there is nothing to preprocess,
so Windows failed every prepare and still reported a successful transfer. The old command
line was reproduced through `cmd.exe` against a fixture folder with a space in its name:
`ERRORLEVEL=-22`, and no `ffmpeg` output at all, because a single quote is not quoting
there, `2>/dev/null` is a folder nobody has, and the output path was a hardcoded `/tmp/`
that `access()` then tested back. `libzune/src/photo.c` quotes the way `thumbnail.c` Method
2 already did (`shell_quote_dup()`, `ZUUNED_TMPDIR`, `>nul 2>&1`) and refuses rather than
launching half an argument list. Quoting was not enough: `system()` runs `cmd.exe`, which
decodes its command line with the ANSI code page, while `src/DeviceWorker.cpp` hands UTF-8.
Measured on this machine's cp936 - same picture, three spellings - an ASCII path prepared,
the Han folder in code-page bytes prepared, and the Han folder as UTF-8 answered
`ARM=failed`. `zuuned_narrow_path_for_shell()` (`compat/win/wincompat.c:1580`) re-spells a
path on the UTF-8-first rule `decode_path()` already uses when it opens one and returns NULL
where the code page has no spelling, because the `?` bytes `WideCharToMultiByte` would
substitute name a different file and read back as a missing picture; `zune_arm_photo()` uses
it for its input and `zune_grab_photo_thumb()` for the cache path it writes
(`libzune/src/thumbnail.c:155`), and neither touches a `ZUUNED_TMPDIR` path the shim already
wrote in the code page. `ffprobe` on the artifact after the fix says `mjpeg 480x315` from a
640x420 source, and 8.3 short names were ruled out by measurement - `dir /x` hands out none
for a directory the code page cannot spell. The gate is a new `shell_bytes` group, 17 checks
that ask Windows itself for the expected bytes so the group holds on any code page; the
assertion it replaced, that U+E000 is unspellable, is cp1252 folklore - GBK has a spelling
for the private use area - and that false assumption was the gate's only real failure. One
clean rebuild, `tests/win-compat` 201 checks / 0 failures / 0 skips, `librarytool paths` 52,
`names` 17, `customize` 21, and `tests/win-usability`
`launches=18 checks=99 failures=0 skips=0 killed=0` with `OVERALL=ok`; `BUILDING.md` and the
audit carry 201, while the dated 184-checkpoint lines stay as the evidence they are. That
earlier entry's "compile-verified only ... `zuuned.exe` has not been relinked since" is
superseded: this tree has been rebuilt clean since and the gates run against that binary.
What is still not proven, said plainly: no USB handle was opened anywhere in this pass.
`zune_arm_photo()` has never met a player, photo transfer to a device is still unproven on
Windows, and the audio round trip was not re-run because the Zune HD is still in the wedge
recorded above. `ffmpeg` is still not bundled, so Method 2 needs it on `PATH`.
**2026-10-25 Chinese build, and why a working card looked empty:** the interface is
translated from a file and nowhere else - `i18n/zh_CN.tsv` is the only place a
sentence the user reads is written, `python i18n/audit.py` fails the tree when it
finds one typed into a property or a C++ literal, and the catalogue now stands at
1493 translatable sources, 1493 with Chinese, `lrelease` 0 unfinished. Wording is
not the only thing in the file, so a language line is either
`source ~=> Chinese` or `Context!source ~=> Chinese`; the prefix counts only when it
names a `<context>` that exists, and beats a bare line for that context. All 29
scoped lines are pivot bars, because `Albums` is 专辑 beside Artists and 相册 beside
photo Folders - and `Albums` deliberately has three scoped lines and no bare one, so
a fourth `Albums` in a new file reports `MISSING: NewFile!Albums` instead of
quietly inheriting the wrong word. What never enters the catalogue is an identity
token: `pivots.tabs`, the `--settings-tab Device` argument,
`picker.mode = current.toLowerCase()` and `page.filter === "all"` compare the same
bytes they always did, and only the painted label is Chinese.

`Settings -> Device` printed the six labels of the 这台 Zune card and none of the
six values, which reads exactly like an unwired feature; `DeviceService` was
answering. The tab was **2263 logical pixels wide**. `StackLayout` sizes every child
to the implicit width of the widest child and lets it overflow its own bounds, so the
widest row in any one of the five tabs widened all five, and a right-aligned value
was being painted at `mapx ~ 2200`, off the window, which no screenshot of a window
that tall can show. Measured rather than reasoned about, with a headless `qml.exe`
repro (a 300 px stack, one child at 4800): a Layout child cannot opt out -
`width: stack.width` is clobbered, `implicitWidth: 300` is ignored, a filler
`Item { Layout.preferredWidth: stack.width }` cannot cap a `max()` - while
`anchors.left/right/top` inside a plain `Item` take the Item's width and are immune
to content. `qml/SettingsPage.qml` therefore holds its five tab bodies in a plain
`Item` selected by `visible`, each anchored, with `implicitHeight` the max of the
five; `PhaseNote` wraps and fills instead of asking for its own width, which is what
let a single long note widen a tab at all. Verified on a window that fits the screen
(`MoveWindow 300,0,1300,1720` yields the honest `1416x1147`; a negative Y produces an
off-screen black band and is not evidence), where 名称, 型号, 序列号, 存储,
音乐/视频/照片 and 电池 all render right-aligned inside the frame. Two habits
carry out of this: a `--qa-probe` line printed from inside the app beats a tall
screenshot, and `QT_FORCE_STDERR_LOGGING=1` is not optional when driving `qml.exe`
or the output is simply lost.

Nothing here touched a player: the Zune HD was browsed read-only, and no sync, pull,
deletion, eject or audio playback was triggered in this pass, so no device claim
changes. One wording defect is recorded and left alone - the 转码加速 note still says
NVDEC, then VAAPI, then software, and VAAPI has no meaning on Windows.
The gate that certifies the settings page was itself broken, and it failed for a
reason that had nothing to do with the application: `tests/win-usability/run.ps1`
put Qt and ffmpeg on `PATH` only inside `Start-Zuuned` and the single-instance
rival, so every tool check - `Invoke-Proc -File $libraryTool`, `transcodetool`,
`devicetool`, `playertool` - inherited whatever shell happened to call the gate.
From a baseline shell that names none of them each child died at `0xC0000135`
(`DLL_NOT_FOUND`) before printing a line, 27 checks failed, the seed library was
never built, and the page, five-settings-tab, compact and spoken cases skipped
themselves on "no seed library" against an app that was fine. One line, next to the
`msvc-env.ps1` load, prepends `$QtDir\bin` and the ffmpeg directory once, because a
gate must not pass or fail on its caller's shell; `tests/win-compat` loads no Qt and
needed no change. Same tree before and after: `checks=93 failures=27 skips=6`, then
`launches=18 checks=132 failures=0 skips=0 killed=0` - 39 checks higher because six
cases that had been skipping now actually run - with `tests/win-compat` at
`201 checks, 0 failures, 0 skips` and `python i18n/audit.py` at `AUDIT=ok`. The run
happened with the Zune HD attached, so the app's own hotplug browsed it inside every
isolated profile; the diagnostics that recorded are all reads (`get_object_info`,
`get_object_prop_value`, thumbnails), and no send, deletion or eject was triggered.

**2026-10-25 Device-tab knobs and a self-test:** the user asked twice about
`Settings -> Device` - 「这台Zune没有信息」 and 「音频转换真的可用吗？你既没有lame设置，也没ffmpeg设置，实用不便」 -
and both were the same fault: a card that reports a fact nobody can check and offers no
knob. The This Zune card was gated on `DeviceService.connected`, so it was absent rather
than empty on every screen Settings is actually opened from; `DeviceHistory::latest()`
(`src/DeviceHistory.cpp:139`) now feeds it the last session's identity in the same map shape
the history list uses, live values win, and only Contents and Battery stay
connected-only - a remembered row is labelled a memory with its date, never a present
reading. `mp3Quality` (Detail 0/2/4/6, hidden for WMA because `wmav2` takes a bit rate and
nothing else) is read in `readAudioSyncPolicy()` beside the format and bit rate so a queue
cannot change effort mid-album, clamped again by `zuuned_mp3_set_quality()`
(`transcode/libav_transcode.c:81`), and applied at both `lame_set_quality()` sites
(`:255`, and the `:773` chapter-range path audiobook parts use); default 2 is what the code
always passed. `ffmpegDir` is a folder, not two pickers, because a Windows FFmpeg download is
one `bin` and `zune_grab_photo_thumb()` asks the shell for a bare `ffmpeg`: `src/main.cpp:254`
prepends it to `PATH` at launch and `conversionTools()` reports `folder`/`PATH`/`none` per
program. Measured on isolated profiles with no FFmpeg on `PATH`: both the doubled-backslash and
the forward-slash spelling log `put in front of PATH` and resolve `ffmpeg.exe`; a missing folder
logs the fallback. That probe also proved **`QSettings`' INI reader unescapes backslashes** -
`D:\dev\x` typed by hand arrives as `D:evx` - which Qt writes correctly itself, so only a
hand-editor meets it. `DiagnosticsService::runAudioSelfTest()` answers "does conversion
actually work" by converting: a 2 s 440 Hz tone through the same `zuuned_transcode_audio_ex()` a
sync uses at the stored bit rate and detail, first-four-bytes container check (ID3 / MPEG sync /
ASF GUID), probed length inside 85-115 %, size above half the nominal CBR payload, outputs
deleted; it answers in codes and QML owns the wording (34 scoped lines, catalogue 1524/1524,
`lrelease` 0 unfinished), and it refuses while `transfer_running` or `zune_busy` because one
transcode at a time is a property of the encoder layer - the abort flag is read, cleared and put
back so a user cancel is never eaten by a test. The AMD pill is `disabled` on Windows and the
转码加速 note is platform-branched, closing the VAAPI-on-Windows wording the previous pass
recorded and left. Gates unchanged because nothing was added to what they assert:
`tests\win-compat` 201 checks 0 failures, `tests\win-usability`
`launches=18 checks=132 failures=0 skips=0 killed=0` `OVERALL=ok`, `i18n\audit.py` `AUDIT=ok`.
Relinking taught one habit: `--clean-first` printed `FAILED: clean` because the app held
`build\zuuned.exe`, continued, rebuilt all 336 targets and exited 0 - prove the executable's
timestamp, not the exit code. No device claim changes: no send, deletion, eject or playback was
triggered, the self-test opens no device handle, and the self-test button has no gate case of its
own, so its evidence is the `transcodetool` encode legs plus a hand press.

**README bilingual, and every capture is a Windows window, 2026-10-25:** `README.md` and the
new `README.zh-CN.md` are one document in two languages, section for section, with a language
nav line above the title in both. Every image in either file now comes from `zuuned.exe` built
from this tree on Windows with a Zune HD attached, which retires the two upstream Linux
captures `getting-started.png` and `customize.png` - removed rather than re-shot, because
Customize is right-click-only and no command-line hook reaches it. The set is 16 named captures
under `docs/readme-screenshots/` plus the same 16 Chinese renders under
`docs/readme-screenshots/zh/`: welcome, the three music pivots, videos, photos, podcasts,
audiobooks, the two device grids, the now-playing island and the five settings tabs, at
1728x1080. Capturing them is a driver outside the tree (`build\shots\shoot.ps1`): one launch
per case into an isolated `ZUUNED_PROFILE_DIR`, a per-language `ZUUNED_LOCK_SUFFIX`,
`ZUUNED_MPV_AO=null` so nothing is audible, `--page`/`--music-pivot`/`--settings-tab` to
navigate, and `--qa-probe` read from the log to prove which page the app itself chose before
`winshot` is allowed to write a pixel - the now-playing shot uses `--qa-play`, which is why that
capture is labelled `QA tone` in both languages instead of naming a file. `--qa-device`,
`--qa-photo` and `--qa-video` were deliberately never run: they write to the player. Because a
shot needs a library with artwork in it, the fixtures were copied from the development profile,
so the pictures carry that machine's own data, including the player name and one
`%USERPROFILE%` watch-folder path. No gate assertion changed and no claim changed: nothing in
this sitting sent, deleted, ejected or played anything, the device is in these pictures only
because the app's own hotplug browsed it, and every not-proven-on-Windows list in both READMEs
stays as long as it was.

**2026-10-26 WMA Lossless, and the encoder that was already in Windows:** the standing answer
to "can this encode WMA Lossless" was a true refusal - the linked FFmpeg carries `wmav1` and
`wmav2` and nothing else, and System32 has no WMA Lossless ACM codec - and the wrong answer
was to vendor or fork a codec, because this is a lightweight client and the user said so. The
third door is `transcode/wma_lossless_enc.cpp` (Windows-only; `transcode/wma_lossless_stub.c`
answers "not available" everywhere else) driving the Windows Media Audio 9.2 Lossless codec
that ships inside `wmvcore.dll`, in process, through `IWMCodecInfo3` -> `IWMStreamConfig` ->
`IWMProfile` -> `IWMWriter`, behind the entry point a sync already uses: a
`ZUUNED_AUDIO_WMA_LOSSLESS` target on `zuuned_transcode_audio_ex()`, reported by
`zuuned_audio_encoder_available()`, linked only against SDK import libraries
(`wmvcore wmcodecdspuuid ole32`). Four traps, each one a build: the eight lossless formats do
not exist for enumeration until `SetCodecEnumerationSetting()` has been given
`g_wszVBREnabled` TRUE and `g_wszNumPasses` 1 per codec, and they must be re-applied before
the final `GetCodecFormat()`; `GetMediaType()` returns one buffer with `pbFormat` pointing
inside it, so freeing that pointer is `0xc0000409` and not a leak; `IWMWriter::GetInputFormat()`
takes the input number and the index, and `GetCodecInfoCount()` takes its GUID by value; and
`ksmedia.h` collides with `wmcodecdsp.h` so it stays out. The file is STL-, exception- and
constant-init-only because the C `transcodetool` links it, and FFmpeg's headers carry no
`extern "C"` of their own, so without an explicit block the link dies on every `av_*`. The
codec's own table is the specification - 2ch/44100/16, 2ch/44100/24, 2ch/48000/24,
6ch/48000/24, 2ch/88200/24, 6ch/88200/24, 2ch/96000/24, 6ch/96000/24 - so the policy is:
never resample, never change the channel count, widen 16-bit to 24-bit at the same rate and
channel count when the table has no other entry (a left shift moves no sample value), and
refuse everything else with its own queue message instead of delivering a file that quietly
stopped being lossless. Measured on the relinked binary, all software: `transcodetool
encoders` answers `wma-lossless=1`, and 44.1 kHz/16-bit stereo, 48 kHz/16-bit stereo (widened
to 24-bit), 44.1 kHz/24-bit stereo and the first under a Chinese file name each wrote an ASF
that probes as `wmalossless` and decoded back to bit-identical PCM, equal sample counts, zero
delay. A real master cost 71.2 % of its PCM (1,006 kbps); a sine costs about 795 kbps, so a
tone is the wrong fixture to size a lossless queue with, and `estimateTrackBytes()` reserves
1,280 kbps for a lossless leg on the recording. `readAudioSyncPolicy()` maps `wma-lossless`
and falls back to MP3 whenever the running build has no encoder for the target a profile asks
for - which is also what happens to a profile that saved lossless on Windows and then runs on
Linux - Settings gained a third option that is disabled unless
`conversionTools().wmaLosslessEncoder` and hides the bit-rate pill, the self-test runs a third
leg judged on container and length only, and the Chinese went into `i18n/zh_CN.tsv` like every
other string (1531 translated, 0 missing, `AUDIT=ok`). Gates re-run against
the relinked binary: `tests/win-compat` 201 checks, 0 failures, 0 skips;
`tests/win-usability` `launches=18 checks=131 failures=0 skips=1 killed=0`,
the one skip being the check that names a specific device with no player
attached to compare. **Nothing has been heard.** Only the Zune HD is
documented to play WMA Lossless, the models this repository has actually
held are documented neither way, the card says so in as many words, and
hearing one is an open gate on this page's list.
**2026-10-29 audit closure, and a gate for every finding:** the review of the
audio line that came in as `Zuuned_Modern_代码审计及_WMA_Lossless_编码同步实施方案.md` -
ten findings, F-01 to F-10, each read out of the source rather than guessed -
is closed in code, and the one that mattered most was also the quietest. F-01:
`transcode/libav_transcode.c` called `avcodec_send_packet()` and on any
negative return unref'd the packet and `continue`d, but `AVERROR(EAGAIN)`
means the decoder is full, not the packet is bad, so the correct answer is
drain-and-retry-the-same-packet; as it was the packet vanished, nothing was
logged, and the output was a few hundred milliseconds shorter than the source.
Keep the shape, not just the fix: a loop that reads backpressure as rejection
loses audio silently, and a slightly short file looks like nothing. F-03:
`mtp_send_object()` took a buffer and `libzune/src/track.c` slurped the whole
track before a byte went on the wire, which is fine for a 5 MB MP3 and not
fine for a lossless album side, so `ptp_transaction_send_file()` (`src/ptp.c`)
now drives the same transaction from an open `FILE *` with a 1 MB
`ptp_data_source` scratch - same transaction id, command phase and long commit
timeout - behind `mtp_send_object_from_file_progress()`, with the buffered
path left alone, and `libzune/tools/mtpframingtool.c` runs both over one fake
transport and compares the bytes (`framing 1245 checks 0 failures`):
byte-identical framing is the whole claim. It refuses a 0-byte object (a
12-byte container leaves the device waiting for a data phase that never
speaks) and refuses a source that ends before the length `SendObjectInfo`
promised rather than padding it. F-06: the transcode scratch name was
source-plus-PID, unique per process and not per job, and three sites take
`mkstemps()` (`O_EXCL`) now. F-07: `transcode/id3_rewrite.c` no longer reads a
whole MP3 to rewrite four frames and no longer writes UTF-8 bytes under an
ISO-8859-1 flag - `normalize_text_frame_for_v23()` picks Latin-1 when the text
fits and UTF-16-with-BOM when it does not, bounded by `TEXT_CODE_POINTS_MAX` -
and `transcodetool retagcheck` judges 27 things over a fixture whose frames
declare three encodings, including that the audio after the tag returns
byte-exact; the lesson worth keeping is that the audio offset a rewriter wants
is the *end of the tag*, because a UTF-16 BOM is `FF FE` and `FF FE` is a
legal MPEG sync word, so scanning forward for a sync word stopped inside the
tag. F-08: the abort and hw-decode flags cross a thread boundary and
`volatile` orders nothing, so every access goes through ``zuuned_flag_load()`
and `zuuned_flag_store()`` - `_Interlocked*` on MSVC, `__atomic_*` at
`__ATOMIC_SEQ_CST` elsewhere - with the declarations still `volatile int` on
both platforms. F-04: a lossless profile on a build with no encoder used to
fall back to MP3 without saying so, which is the same crime as F-01, so
`readAudioSyncPolicy()` refuses and the queue card names the missing encoder
and states that the file was not converted instead. F-05: `transcodetool
container` reports the ASF codec GUID, the `WAVEFORMATEX` tag, losslessness
and the copy-policy verdict, so a native copy is decided off the codec and not
off a fourcc that looked carryable. F-09: `zune_verify()` is still the
after-the-fact warning pass it always was, and the length is now enforced on
the way out instead, where being wrong costs a wedge rather than a log line.
F-10: `estimateTrackBytes()` takes the bitrate and a lossless flag, reserves
1,280 kbps for a lossless leg, and an encoded file over its reservation is not
sent - the queue says how far over and how much room the Zune has. Two bugs
belonging to this sitting were found in this sitting's own code:
`transcodetool container` with no argument dereferenced a null path and died
`0xc0000005` (every command but the three that ask about the build now returns
a usage error for a missing file, because a gate that crashes on a typo
reports nothing), and `i18n/audit.py` flagged 34 non-findings because it
matched `UiText::tr(` on one line only, so an adjacent-literal string had its
continuation lines read as untranslated CJK - it tracks a continuation now,
and the loosening was regression-tested against a bare `QStringLiteral` with
Chinese in it and a continuation whose head is not a `tr()`, both still
reported. Gates on the relinked binaries: `tests\win-compat` 201 checks / 0
failures / 0 skips; `tests\win-usability` `launches=18 checks=134 failures=0
skips=1 killed=0`, the tools phase re-run alone on the last binary `checks=47
failures=0 skips=0`; `librarytool paths` 67 and the other nine subcommands
`ALL PASS (0 failures)`; `retagcheck` 27/0, `copycheck` 14/0, `mtpframingtool`
1245/0; i18n 1535 translated / 0 missing, 1535 finished / 0 unfinished,
`AUDIT=ok`. **No Zune was opened for any of it.** The streaming sender is
proven against the buffered one on a fake transport, never against a player,
nothing this tree wrote has been heard, and deletion, the spoken and
photo/video categories, every model but this Pavo Zune HD and anything audible
all stay open.
