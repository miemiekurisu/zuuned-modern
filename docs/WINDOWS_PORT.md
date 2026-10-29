# Windows port status

Dated 2026-10-20, updated 2026-10-29.  This is where the native Windows build of Zuuned actually
stands: what compiles, what has been proven on a Windows machine, and what has
not.  It exists so that nobody reads "the Windows gates pass" as "the Windows
build works with a Zune".  Build steps and dependency locations live in
`BUILDING.md`; the two gates are documented in `tests/win-compat/README.md` and
`tests/win-usability/README.md`.

  Documentation note, 2026-10-25: ``README.md`` and the new ``README.zh-CN.md`` are the
same page in two languages, and every picture in either one is a window of a ``zuuned.exe``
built from this tree, captured on Windows with a Zune HD attached; the two upstream Linux
screenshots that used to sit in ``docs/readme-screenshots/`` are gone.  That says nothing new
about hardware.  This page still owns every device fact, and the READMEs still point here for
it.

## The first Windows device contact

**A Zune HD has now been connected to a Windows build, opened, and read.**  What
follows was measured on the development machine on 2026-10-20 and it is the whole
of the Windows device evidence as of this checkpoint.  The rule this page exists
for survives it: **a hardware fact proven on Linux still does not carry across
platforms, and reading a library is not syncing one.**

The driver first.  `packaging/windows/DRIVER_INSTALL.md` closes as written - a
libwdi-generated WinUSB package is the devnode's function driver, the problem code
is `CM_PROB_NONE`, the service is `WinUSB`, and the devnode registers the device
interface GUID libusb 1.0.29 insists on.  `check-driver.ps1` reads all of that
back and `build\devicetool.exe` answers `VERDICT=agree`.  That run did surface one
documentation and tooling bug, now fixed in both: the GUID is readable only from
the devnode's `Device Parameters` registry key, while
`DEVPKEY_Device_DeviceInterfaceGuids` is empty on a player that opens perfectly
well, so a script that trusts the DEVPKEY alone reports an openable device as
unopenable.

Then the application, which is where the first Windows device traffic is:

```
[libusb] opened: bulk_in=0x81(512) bulk_out=0x01(512) int=0x82
[ptp] detected split header/data mode (Zune/WMP)
[libzune] GetDeviceInfo: model='Zune' serial=[redacted]
[libzune] device family: Pavo (Zune HD) (0xD21A = 0x06040001)
[libzune] connected: [device name] (Zune), battery: 100%, storage: 10491396096/15543730176
[libzune-zmdb] totalSize=226688, payloadSize=226676, first chunk=12 bytes
[libzune-zmdb] received 226676/226676 bytes
[zunedb] scanned: 183 tracks, 0 videos, 0 photos, 9 albums, 10 artists, 0 playlists, 0 photo albums
[device] storage: 5.1/15.5 GB (used/total), attributed: music 4.88 + video 0.00 + photo 0.00 GB
```

So on Windows a Zune HD is opened over WinUSB, answers `GetDeviceInfo`, is
identified as Pavo by the same property read Linux uses, hands over all 226,676
bytes of ZMDB, and the application turns that into 183 tracks in 9 albums by 10
artists with per-category storage accounting and album thumbnails fetched off the
player - and the device page is operable, which is what the crash below was
blocking.  Unplugging and replugging produced a second identical read, so this is
the hotplug path working rather than one lucky connection.

Three things it is not, each a gate with its own name.

- **Not MTPZ authentication.**  The same log says `credentials unavailable;
  provide ~/.mtpz-data to authenticate a Zune` and `MTPZ authentication will
  fail`: this development checkout has no MTPZ credentials, so the library read
  happened unauthenticated, which the ZMDB path permits.  MTPZ authentication has
  therefore still never run on Windows, and no write path should be attempted
  until it has - see `docs/BUILD_WITH_MTPZ.md`.  That was a missing credential
  file rather than a platform defect, and it is resolved below.
- **Not a write.**  Nothing was sent, deleted, finalised or read back byte for
  byte.  Saving two device tracks into the library - still a read, but a
  different operation - answered `get_object 0x01000045: failed (response 0x200F)`
  and the app reported `AccessDenied`.  Observed, not retried, not diagnosed here,
  which was the right call: the answer was authentication, and the same save
  completed once MTPZ credentials were on the machine.
- **Not clean.**  `get_object_info 0x07000002` returns `0x2002` three times per
  scan against the very atom the scanner reports as a playlist record, and
  `get_representative_sample` reports `bad AUINT8 count 0 in 4-byte payload` for
  objects whose thumbnails then fail every method.  Recorded rather than asserted;
  `libzune/docs/` is where a diagnosis belongs, and neither code has been chased
  on Linux either.

The GUI crash that stood in front of all of this turned out to be a build
artefact rather than a Windows fault in the application.  The account and the
command that clears it are in `BUILDING.md` under *Incremental builds and the
compiler's include note*.

So a Windows build may now be described as building, launching, painting,
scanning, encoding, persisting, opening a Zune HD, reading its music library,
authenticating over MTPZ, and moving audio in both directions by hand.
It may not be described as syncing a queue unattended, deleting, or playing
anything back from a device.

## MTPZ, and the first writes (2026-10-21)

**MTPZ authenticated on Windows, and the first Windows device write completed.**

The `credentials unavailable` line had the same cause on both platforms and only
one platform showed it: the public source has no `libzune/src/mtpz_keys.h`, so
`ZUNE_HAS_EMBEDDED_MTPZ` is undefined and a build from it has no compiled keys at
all.  The development Linux checkout carries that header privately; the Windows
checkout does not.  Putting the five-line file at `%USERPROFILE%\.mtpz-data`, which
the loader already looks at because Windows never sets `HOME`, changed the log to

```
[mtpz] loaded keys from ~/.mtpz-data
[mtpz] handshake complete
[libzune] MTPZ authentication successful
```

With authentication in place the operator hand-tested both directions against the
same Zune HD: a track from `E:\Temp\testmusic` sent to the player, and a device
track saved back into the library.  Both completed and the operator confirmed both.
That is the first Windows write and the first Windows read-back of bytes this app did
not write on Linux.  Nobody has certified what the player does with either file:
device-side playback stays its own gate below and is not claimed here.

Three defects stood in front of it, all Windows-only, all root-caused, all now
gated.  They were never protocol bugs - libzune was asked for a path that did not
exist and answered truthfully:

- **The `/E:/` watch-folder scar.**  Folder pickers hand QML a `file:` URL and the
  UI stripped the scheme with string arithmetic, which on Linux happens to produce
  the correct `/home/user/Music` and on Windows produces `/E:/Music` - not an
  absolute path there, but a path relative to the current directory of drive E:.
  The folder therefore scanned nothing, reported itself unreachable on every
  health check, and any save built underneath it failed.  `src/library/LibraryPaths.h`
  is now the single spelling rule and `LibraryDb` repairs rows an older build
  already stored; `docs/WINDOWS_COMPAT_AUDIT.md` is the whole account.
- **file:// built by concatenation.**  Prefixing a stored path with the file://
  scheme by hand spells a Windows path as file://E:/Music, which parses as host
  `E:` and loads nothing, so album art and wallpapers were blank for reasons that
  had nothing to do with the image.  `PathText.url()` wraps
  `QUrl::fromLocalFile()` instead.
- **`librarytool` printing paths as `?`.**  The console renders the ANSI code page,
  so a gate that scans a Hebrew album folder could not read its own output.

Still not proven on Windows, each its own gate: a Classic, Draco or 80/120 (every
Windows device fact here is one Pavo Zune HD); deletion or purge; podcast and
audiobook transfer; photo and video transfer; device-side playback.  At that
point the finalise, re-index and byte readback evidence was still Linux
evidence; the next section is what closed part of it the same day.

## The round trip, logged (2026-10-21)

Those two directions were confirmed by an operator watching a window.  Both have
since been run again so that the log says it, and the bytes that came back were
compared with the bytes that went out.

`qml/Main.qml` accepts `--qa-device "<file>|<artist>|<album>|<title>"`: it queues
that one track through the ordinary `SyncEngine` admission, starts a sync, and
reports every decision as a `[qa-device]` line.  With the file field left empty
the same flag instead looks `<title>` up in the device inventory and runs the
ordinary Save-to-Library download for it.  Without the flag the timer never
starts and neither `Connections` block enables, so an ordinary launch behaves
identically on both platforms.

The upload used a folder and a file name in Chinese under `E:/Temp/testmusic`:

```
[qa-device] add added=1 duplicates=0 onDevice=0 rejected=0 ambiguous=0
[sync] forge done: 1 ok, 0 failed
[qa-device] sync-summary elapsedMs=1590 tracks=1 videos=0 photos=0 skipped=0 failed=0
[qa-device] upload-settled deviceTracks=0 failed=0 tracks=1
```

The player went from 186 tracks to 187.  The next launch found 187, and the pull
pass saved that title back into the library, where the MP3 audio stream is
byte-for-byte the 401,239 bytes that went in.  The difference between the two
files is metadata only: the source carries a 45-byte ID3v2.4 tag, the copy
carries the 1,096-byte ID3v2.3 tag this app writes before a send plus the
128-byte ID3v1 it appends.

Two launches are what a finished sync forces rather than a shortcut taken: it
finalizes with CleanDataStore and releases the device, and `awaitingDisconnect`
refuses to breach back in.  The second run is therefore also the evidence that
the upload survived the device's own re-index and was not merely accepted.

Getting a legible log out of this was itself two Windows-only fixes.
`qPrintable()` is `toLocal8Bit()`, so on this CP936 machine every `[sync]` line
naming that Chinese path arrived as U+FFFD while the C layer printed the same
path correctly - `src/` now uses `qUtf8Printable()` throughout.  And the harness
first tested its own verdict behind a device-presence guard that the completed
sync had already cleared, so a clean upload printed no verdict of its own; it
now latches on the sync summary.  Both are accounted for in
`docs/WINDOWS_COMPAT_AUDIT.md`.

What this does not say: nobody has listened to either file.  Device-side
playback stays its own gate, below.

## The ordinary controls, both directions, in one sitting (2026-10-21)

The `--qa-device` harness above puts one track on the wire because that is what
a harness is for.  What a person does with the application is the rest of it,
and the rest of it was then done by hand through the ordinary controls on the
same machine and the same Pavo, against the build that carries MTPZ credentials.

Two files went onto the queue from the Music page with `+ all music` - one from
`E:/Temp/testmusic/QA Windows`, one from the folder named in Chinese - and
`sync now` was the only command given:

```
[sync] retagged to ID3v2.3: E:/Temp/testmusic/QA Windows/Sine QA 440.mp3
[sync] retagged to ID3v2.3: E:/Temp/testmusic/测试音乐/正弦 660.mp3
[libzune] created album "QA Windows" by "Unknown Artist" (1 tracks, id=100663563)
[libzune] created album "测试音乐" by "Unknown Artist" (1 tracks, id=100663564)
[sync] forge done: 2 ok, 0 failed
[libzune-finalize] 0x9108 result: 0 (0=ok)
[libusb] closed
```

Two albums, the second named in Chinese, `2 ok, 0 failed`, and the device browser
header counted from 187 tracks to 189 while the window stayed open - that number
is the player's listing, not this app's idea of it.  The queue tab reads
`last sync: 2 tracks` over `no items queued`.  Vendor finalise answered `0x9201`
and `0x9202` with `0x2005` exactly as it does on Linux, CleanDataStore answered 0.

The other direction was a right-click on a row of the device browser:
`Save to Library` wrote 20,992,730 bytes to

```
C:\Users\Kurisu Chan\Music\Zuuned Imports\J. S. Bach\Messe in h-Moll, BWV 232\ Gloria- Cum Sancto Spiritu.mp3
```

and the library re-counted itself to `8 source rows, 6 displayed songs` and pulled
the embedded artwork out of the file.  Three facts from that one file are worth
keeping because none of them was expected:

- the first four bytes on disk are `30 26 b2 75`, the ASF GUID, while the object
  name the device gave ends `.mp3` - the WMA-inside-an-mp3 case the sync layer
  already sniffs for.  A pull keeps the device's own name and the scanner judged
  the payload, so the track indexed and gained art.  Anything that trusted the
  extension would have called that file broken.
- the album name the device reported ends in a space, visible in the app's own
  log line as `.../Messe in h-Moll, BWV 232/ Gloria-...`.  A Windows directory
  cannot keep a trailing space and the one on disk reads
  `Messe in h-Moll, BWV 232`, so the name landed in the only spelling this
  platform accepts.  Nothing was written back to the device to agree with it.
- `~/Music/Zuuned Imports/...` in that same log line is
  `src/diagnostics/Redactor.cpp` masking the home directory, not a mangled path:
  the file sits where the row says it does.  The same false lead is recorded in
  `docs/WINDOWS_COMPAT_AUDIT.md`.

Settings said the rest without being asked: the Library tab lists both roots the
way Windows spells them (`C:\Users\Kurisu Chan\Music\Zuuned Imports` and
`E:\Temp\testmusic`), and the Health tab reads `Watch folders reachable` `2 of 2`
and `Zune connected` `ready`.  That display has assertions behind it now rather
than a screenshot - see the path-spelling bullet under what the software gates
prove.

What a finished sync used to cost here is closed.  The panel no longer sits on
`Disconnected - the Zune is re-indexing` once the last write is finalised - the first
rung replaces it with `reconnecting in 6s` - and
`libusb_hotplug` still reports no capability on this machine, so
`src/UsbWatcher.cpp` still polls the device list every 500 ms for a change that a
player re-indexing without leaving the bus never produces.  What changed is that
the application no longer waits for an arrival that cannot happen on this
platform: the finalize chain arms its own reconnect.
`DeviceService::scheduleReprobeAfterReindex()` runs a bounded ladder of 6, 12,
20, 30, 45 and 60 seconds, and each attempt IS a real `zune_breach()`, because an
OpenSession is the only honest readiness probe and a player mid-re-index answers
late rather than never.  The gaps grow because a breach already auto-resets a
wedged session once, so hammering the same player every second would trade one
wedge for another.  A real arrival or an unplug owns the reconnect and cancels the
ladder outright; a manual `connect` that fails stays failed rather than turning
into a retry loop on a device the user can see is not answering; sync, purge,
pull and eject reset or drop the budget when they take the device.  When the
ladder runs out it logs `re-index reconnect ladder exhausted - connect manually`
and leaves the panel on its last real error instead of inventing a state.  Seen on
the Pavo the same day with nobody touching `connect`:

```
[sync] forge done: 1 ok, 0 failed
[libzune-finalize] 0x9108 result: 0 (0=ok)
[libusb] closed
[device] re-index reconnect: attempt 1 of 6 in 6000ms
[mtpz] handshake complete
[libzune] MTPZ authentication successful
[zunedb] scanned: 190 tracks, 0 videos, 0 photos, 16 albums, 12 artists, 0 playlists, 0 photo albums
[device] storage: 5.1/15.5 GB (used/total), attributed: music 4.88 + video 0.00 + photo 0.00 GB
```

That run went through `--qa-device`, which drives the ordinary
`SyncEngine.addTracks` / `startSync` admission and the ordinary
`DeviceService.saveTrackToLibrary`, so the same sitting carried both directions
again on this build: one 193,833-byte mp3 sent with `forge done: 1 ok, 0 failed`
and the device's own listing up from 189 tracks to 190, then that object read
back as `SineQA550.mp3` at 195,015 bytes with `readbackDurationMs=8000` - the
duration this app declared on the way out - into `Music\Zuuned Imports`, where
the library counted itself from 6 tracks to 7.  `awaitingDisconnect` still
refuses to breach back in *while* the player re-indexes, on both platforms; the
ladder is what runs after it.

Neither QA run cleaned up after itself, and this sitting does not claim it could:
device-side deletion is an open gate on Windows, so `ZuunedQA / SineQA550` is still on
the Pavo and the copy pulled back from it is still in `Music\Zuuned Imports`.  A
future run inherits both; removing them is part of closing the deletion gate, not
housekeeping to do by hand in the meantime.

## Toolchain that works here

- Qt 6.11.0 `msvc2022_64`.  `CMakeLists.txt` requires Qt 6.8, so this kit is
  comfortably new enough; it has to be the MSVC 64-bit kit because the app is
  built with MSVC, and the other kits in the same Qt installation do not apply.
- Visual Studio 2022 (MSVC x64) with the Windows SDK, plus the CMake and Ninja
  that ship in `Qt\Tools`.
- vcpkg `x64-windows` for libusb, sqlite3, libmp3lame and libgcrypt; PCRE2
  headers back libzune's pattern helpers and the shim gate.
- A shared FFmpeg build for the transcode layer and libmpv for playback.  libmpv
  ships a MinGW import library MSVC cannot read, so an MSVC import library is
  generated once from the DLL export table.
- Native throughout, by decision: no WSL, no MinGW, no Cygwin.  The consequence
  is stated plainly below under what is not proven - the Linux bash gates are
  not run from this checkout.
- Nothing on this machine rebuilds safely by accident.  `BUILDING.md` explains
  why an edit to a header can relink the object that was already there, and the
  first Windows device session died of exactly that.  When a change appears to do
  nothing, `cmake --build build --clean-first` before believing any diagnosis.

## What the port consists of

- `CMakeLists.txt`: `if(WIN32)` guards for dependency discovery in place of
  pkg-config, the `compat/win` POSIX surface force-included for the C layer,
  MPRIS and `Qt6::DBus` kept Linux-only, and the POSIX install rules skipped -
  shipping on Windows is a self-contained directory assembled with `windeployqt`
  beside the exe, and `cmake --install` deliberately does nothing.
- `src/DevicePresence.h`, `src/DevicePresence.cpp` and the two backends: Windows
  asks libusb first and then the PnP manager through `cfgmgr32`;
  `src/DevicePresenceUnix.cpp` answers from libusb alone with its existing
  wording byte-identical, so the line that has hardware evidence behind it is
  untouched.  `src/UsbWatcher.cpp` polls that presence every two seconds and
  emits `zuneBlocked`/`zuneUnblocked`.
- `src/ProfilePaths.h` plus `ZUUNED_PROFILE_DIR` and `ZUUNED_LOCK_SUFFIX`: one
  folder owns the settings ini, both databases, every cache, the session logs
  and the single-instance lock.  That is the Windows equivalent of relocating
  `$HOME`, and it is what makes an instrumented launch honest instead of
  destructive to the real profile.
- Path encodings, the subtlest part of the port: SQLite wants UTF-8, FFmpeg is
  documented UTF-8 on Windows, the CRT is ANSI-only, and `compat/win` decodes a
  narrow path as UTF-8 with an ANSI fallback.  `src/spoken/SpokenStore.cpp`
  handed SQLite the local code page until 2026-10-20, which silently lost the
  whole spoken library under a Chinese profile folder.  `BUILDING.md` has the
  full account.
- Twelve translation units carry platform guards.  The rest of the application
  is the same shared code as on Linux.

## What is proven on a Windows machine, by software alone

Numbers below come from the development machine at this checkpoint.  Re-run
`tests\win-usability\run.ps1` and `tests\win-compat\run.ps1` from an MSVC
environment to reproduce them.

- The shim gate reports `184 checks, 0 failures, 0 skips`: temporary names,
  long paths past 260 characters, 64-bit sizes, directory descriptors,
  `opendir` semantics and UTF-8 narrow names under the same force-include the
  application builds with. Since 2026-10-20 it also pins how `fstat()` reads a
  number: a raw NT handle *and* a descriptor the shim never created both resolve.
  That closed a Windows-only bug which killed every audiobook import - Qt opens a
  `QFile` through `_wopen()`, so `QFile::handle()` is a CRT descriptor rather than
  the native handle its documentation promises, and the watermark in front of the
  descriptor table only counted descriptors `compat/win` handed out. The
  invalid-parameter guard that makes asking the table safe goes in lazily from
  `fd_handle()`: a `.CRT$XIB` initialiser was tried first, the linker folded this
  object's `.CRT` subsection into `.rdata` (LNK4254) and start-up never ran it,
  which the gate caught by finding the handler still null.
  Since 2026-10-21 it carries a `file_identity` group, and that one is load-bearing
  in a way the others are not: `st_dev`/`st_ino` come back as the volume serial and
  the NTFS file index, two files never share one, and it survives the rename
  `QSaveFile` publishes with - which is what the artwork cache's "this image is its
  own source" refusal is built on.  Left at zero by a naive shim it would have
  rejected every cached artwork render on Windows, with a message about the cache.
- The usability gate makes seventeen instrumented launches plus the tool,
  encoder and profile probes and ends `launches=17 checks=94 failures=0
  skips=0 killed=0` and `OVERALL=ok`.
  Re-running it here on 2026-10-21 aborted at the chrome collapse pin instead.
  Eight of the seventeen launches answer `Process.MainWindowHandle` with nothing
  and fall back to the `winshot --pid` scan, and that scan parsed the handle it
  got back with `[long]::Parse`.  `winshot` prints `%llu` of `uintptr_t`, and a
  real 64-bit handle such as `0xFFFFFFFF80010E3A` reaches the harness as
  18446744071562137146, which does not fit a signed 64-bit integer, so the whole
  run died on "Value was either too large or too small for an Int64" having
  proven nothing about the build.  The 32-bit mask on the way out was the mirror
  of the same mistake: on a 64-bit Windows a handle is a whole pointer, so
  masking renames the window and `GetWindowRect` fails on one that exists.  Both
  directions now reinterpret the same eight bytes in the unsigned decimal
  `winshot` uses, and the run is back to the counters above with
  `pid-scan-fallbacks=8`.  A harness that cannot find its own window is not a
  property of the application, so this is a checkpoint note rather than a
  Windows defect.
- What those launches actually establish: every page paints real content rather
  than an empty window (`winshot` captures the live swapchain and the app
  reports its own state through `--qa-probe`); the five settings tabs, the
  pinned-header states and the compact Now-Playing island behave as designed; a
  second instance on the same profile refuses in its own words; a library scan
  stores a Chinese album folder name and a path of 277 characters and hashes the
  audio behind both; the transcode layer *produces* - profile 0 returns ASF at
  the source frame size, profile 2 returns `ftyp` at 480x272, and the LAME leg
  returns an mp3 as long as the tone it came from; and a profile folder named in
  Chinese gains both databases with the spoken store's own schema readable back
  out of it.
- Native path spelling, asserted by the running application rather than read off a
  screenshot.  The `music` page case reads fields the app reports about itself
  through `--qa-probe` and pins five of them: `wfRows >= 1` that the seeded library
  reached it as a watch folder, `wfRaw = 0` and `wfNative = wfRows` that every
  watch-folder label is spelled the way this platform spells it and that no forward
  slash survived into one, `wfStored = wfRows` that storage still holds one
  canonical forward-slash identity per folder, and `wfLabel = 1`, which asks
  `PathText::display()` (`src/PathText.h:49`) what it would make of the stored path
  and refuses unless the row is showing exactly that string.  The live profile
  answers `wfRows=2 wfNative=2 wfRaw=0 wfStored=2 wfLabel=1`.  What this retires is
  the weakest claim this document ever carried: `/E:/Music` reached a release
  because a person read the label instead of a gate reading it.
- Real audio output, silenced.  `playertool play <tone> 3 --audible --mute` with
  `ZUUNED_MPV_AO=wasapi` ends `audio_out=wasapi` with the position advancing, and the
  same run with the variable empty ends `audio_out=null`, so the Windows audio backend
  is proven to open and to clock playback.  Nothing was listened to.
- Which decoder path actually ran.  `transcodetool video` now prints the pixel format of
  the first frame the decoder returned and the gate reads it back: `ZUUNED_HW_DECODE=0`
  opens one decoder, asks for no accelerator and decodes in `yuv420p`, while the default
  Settings leaves on reported a first frame in `cuda` on this machine and muxed a valid
  480x272 file through the hardware frame download path.  What this corrects is worth
  keeping: the line the layer used to print inferred a hardware decode from an attached
  device context, which stays attached even when `get_format` answers with a software
  format, so the earlier note here that Windows had only ever decoded in software was
  read off a label that could not tell the two apart either way.

## Re-checked against a clean rebuild (2026-10-21, later the same day)

The filename and playlist fixes committed earlier that morning were merged while
`build\zuuned.exe` still predated them, so "the gates pass" was a statement about the
source and not about the binary.  Everything was rebuilt and run again.

- `cmake --build build --clean-first`: 329 steps, no errors, `zuuned.exe` and
  `librarytool.exe` both produced from the sources in the tree.
- **A relink is not evidence on this machine, which is why the line above is a clean
  rebuild.**  The Ninja rules use `deps = msvc`: Ninja learns what a `.cpp` includes by
  parsing `cl`'s `/showIncludes` echo for the literal English prefix, and this Visual
  Studio answers `注意: 包含文件:`, so nothing matches and
  `ninja -t deps CMakeFiles\zuuned.dir\src\LibraryService.cpp.obj` reports `#deps 0`.
  Edit one of the header-only helpers - the filename, playlist and video-name rules all
  live in headers - rebuild, and nothing recompiles while the AUTOMOC steps and the link
  make the run look productive.  A Windows gate counter is worth exactly what the last
  clean rebuild cost; `BUILDING.md` carries the diagnosis and the workaround.
- `tests\win-compat\run.ps1`: `184 checks, 0 failures, 0 skips`.
- `librarytool names`, `customize` and the new `paths` group: all pass, `paths` at
  28 checks.  `paths` is where the filename, comparison, resolution and decoding rules
  are pinned (`src/library/librarytool.cpp:319`); it exists because neither existing
  gate could reach a C++ header, and it now runs inside the usability gate's tools phase
  so that it cannot be the one check nobody remembered to run.
- `tests\win-usability\run.ps1`: `launches=17 checks=95 failures=0 skips=0
  killed=0 pid-scan-fallbacks=6`, `OVERALL=ok` - the same seventeen launches as the
  checkpoint above and one check more, the extra one being `paths`.  Against a binary
  that provably contains the fixes, this time.  The Han profile-folder,
  profile-isolation and single-instance cases are all in that count.
  `pid-scan-fallbacks` is a harness-side timing artifact rather than an application
  result: it counts the launches where the harness had to enumerate the process's windows
  because `MainWindowHandle` had not filled in on the first poll, and it read 7, then 3,
  then 6 across three runs of this gate on 2026-10-21.
- One qualification on the usability numbers, because the gate's `SCOPE` line is
  narrower than it reads: it means the harness opened no device handle.  A Zune HD was
  attached while the run happened, so the application's own hotplug auto-connect opened
  and browsed the player inside each isolated profile - visible in the recorded
  diagnostics as `[mtp] get_object_info ... response 0x2002` and the thumbnail
  fallbacks.  Isolated profiles hold no library and no transfer queue, so nothing was
  written to the device, but a future run should either unplug it or expect the device
  in the log.

Re-reading that commit also turned up three defects in it, documented in
`docs/WINDOWS_COMPAT_AUDIT.md` under the third sweep: the m3u fix decoded nothing about
encoding, `safeFilePart` did not carry the reserved-stem rule the comment claimed it
did, and none of it had a gate.  All three are fixed here.  Still not hand-tested: an
album with a colon in its name saved to the library, a foreign-authored playlist
imported, a device photo thumbnail through the `cmd.exe` branch.

## Re-checked in one sitting, both directions, on one binary (2026-10-21, again)

Nothing in the tree changed between this pass and the one above, and that is the
point of it: every number below was produced on this machine, in order, from the
same `build\zuuned.exe`, rather than carried forward from a checkpoint that
predates the rebuild it claims to prove.

- `tests\win-compat\run.ps1`: `184 checks, 0 failures, 0 skips`.
- `build\librarytool.exe paths` at 28 checks, plus `names` and `customize`: all
  pass.
- `tests\win-usability\run.ps1`: `launches=17 checks=95 failures=0 skips=0
  killed=0 pid-scan-fallbacks=7`, `OVERALL=ok`.
- The `--qa-device` round trip, both directions, from a tagless 193,768-byte sine
  fixture that sits outside every watch folder.  Out: `add added=1 duplicates=0
  onDevice=0 rejected=0 ambiguous=0`, `sync-summary elapsedMs=1614 tracks=1
  failed=0`, `upload-settled failed=0 tracks=1`, with the device at 190 tracks
  before the sync and 191 in the next session.  Back: `download-from
  itemId=16777480 readbackDurationMs=8000` and `download-saved
  name='SineQA1021.mp3' ok=true`, landing in
  `Music\Zuuned Imports\ZuunedQA\Windows Roundtrip 1021\` as a native
  drive-letter path built from `QStandardPaths::MusicLocation` - the same call
  the Linux build answers with `$HOME/Music`.
- The landing file was then read by something that is not this repository's code:
  `ffprobe` reports MP3, mono, 44.1 kHz, 8.0457 s, the fixture's own length.
  Strip the tag from each side and 193,723 audio bytes remain, byte-for-byte
  equal, so the payload survived the send, the device's own re-index and the
  pull.  The 1,177-byte size difference is exactly the ID3v2.3 written ahead of a
  send (1,049) plus the ID3v1 tail (128).
- Recorded rather than fixed, because it is not a defect: `zuuned_retag_mp3()`
  carries over the frames the source already had and injects only the album
  artist, so a source with no tags comes back with an ID3v1 that holds nothing
  but the artist and an empty title.  The Zune still shows the right title, album
  and duration because those are MTP object properties, and those came back
  correct; but a tagless import does not name itself once it is on disk.
- One launch of the usability gate failed on this binary, once, and passed a clean
  re-run.  `06-page-deviceMusic` timed out at exit 124 on a near-blank frame with
  no `[qa] splash report`; its own session log held one application line,
  `[qa] startup page=deviceMusic panel=false`, and 712 clipboard complaints - 474
  `qt.qpa.mime: Retrying to obtain clipboard.` and 237 `Unable to obtain
  clipboard.` - seven a second for 36 s.  No source file in this repository
  mentions a clipboard, so those reads are Qt's own; its Windows clipboard
  backend opens the clipboard and retries with a sleep, on the GUI thread, which
  is enough to keep `SplashView` from reaching `onFinished` and therefore from
  starting the tick timer through which the harness reports its verdict.  Three
  controlled attempts reproduced nothing: a clean launch, a separate process
  holding the clipboard open for the whole window, and a process writing it every
  120 ms.  The machine still answers `GetClipboardOwner()` with 0 while
  `OpenClipboard()` keeps returning `ERROR_ACCESS_DENIED`, which is what a
  process that opened the clipboard and never closed it looks like.  That is the
  desktop and not the application; the practical rule is that one white-frame
  launch is worth a re-run before it is worth a bisect.

## The fourth pass: the scan, and a folder that contains itself (2026-10-21)

A junction is a directory to `QFileInfo` on Windows — `isSymLink()` false, one
attribute, `FILE_ATTRIBUTE_DIRECTORY` — so the library scanner's cycle guard did not
see it and the recursive `QDirIterator` walked down `music/loop -> music` 41 levels until
the path hit 260 characters.  It reported a clean, complete scan on the way, having filed
the same songs at 41 paths.  `LibraryScan::walkFiles()` replaces all three media walks
with one explicit-stack walk that keys each folder on the `(st_dev, st_ino)` pair the shim
takes from the volume serial and the NTFS file index, and a junction onto an ancestor
answers with the ancestor's identity.  `docs/WINDOWS_COMPAT_AUDIT.md` has the fourth sweep
in full, including why `FollowSymlinks` and why an entry cap were both rejected.

Everything below was produced afterwards, in order, on one relinked `build\zuuned.exe`,
with the device attached throughout.

- `tests\win-compat\run.ps1`: `184 checks, 0 failures, 0 skips`.
- `build\librarytool.exe paths`: 39 checks, `ALL PASS` — the eleven new ones build a
  `mklink /J` junction inside a `QTemporaryDir` and assert the walk visits each song once.
  `names` and `customize` still pass on the same build.
- Real junction, real scan: a music folder of three copies of one MP3 plus `loop ->
  music`, where `GetFileSystemEntries` recursion counts 192 entries, gave `scan done: 3
  tracks added, 0 updated, 0 photos` and one `[scan] ...: 1 folder(s) skipped as already
  walked` line on stderr.
- `tests\win-usability\run.ps1`: `launches=17 checks=95 failures=0 skips=0 killed=0
  pid-scan-fallbacks=4`, `OVERALL=ok`.  No launch failed this time, so the clipboard
  fault recorded above stays an environment story rather than a pattern.
- The `--qa-device` round trip again, both directions, on the same 193,768-byte sine
  fixture under a new title and album (`SineQA1021B` / `Windows Roundtrip 1021B`): `add
  added=1 duplicates=0 onDevice=0 rejected=0 ambiguous=0`, `forge done: 1 ok, 0 failed`,
  `sync-summary elapsedMs=1638 tracks=1 failed=0`, `upload-settled failed=0 tracks=1`,
  `[libzune-finalize] 0x9108 result: 0`, `[libusb] closed`.  The next session counted the
  device at 192 tracks where it had counted 191, then `download-from itemId=16777481
  readbackDurationMs=8000` and `download-saved name='SineQA1021B.mp3' ok=true`.  Read back
  with code from outside this repository, the 193,723 audio bytes are identical; the
  1,177-byte difference is the 1,094-byte ID3v2.3 this app writes ahead of a send plus the
  128-byte ID3v1 it appends.
- One harness trap worth writing down, because it makes a false pass: the spec contains
  spaces, so it has to reach the app as one quoted argv element.  Passed unquoted,
  `--qa-device` parses three fields instead of five, `qaDeviceSpec` returns null, the
  timer never starts — and the run looks like an ordinary launch.  That is exactly what
  the first attempt here did before the log was read.

Still nothing heard on the player, still one Pavo Zune HD, still no deletion.

## The fifth pass: a manifest, two playlist names, and the round trip again (2026-10-21)

Three things the fourth pass recorded as gaps are closed.  `docs/WINDOWS_COMPAT_AUDIT.md` has
the fifth sweep in full - why the manifest had to be a whole manifest rather than a fragment,
why CMake needs `/MANIFEST:NO` before it will accept one, and why a gate written only against
Qt would have concluded that the declaration buys nothing.

Every software gate below ran on one `cmake --build build --clean-first` rebuild - 329
steps, the habit `BUILDING.md` asks for before any number is written down - in order, on
this machine, with the Zune HD attached.  The `--qa-device` round trip ran on a binary
carrying the same code a few minutes earlier, and is marked as such below.

- All six executables carry exactly one embedded manifest, read back with `mt.exe`:
  `asInvoker`, and `longPathAware=true`.
- `build\librarytool.exe paths`: 52 checks, `ALL PASS` - thirteen more than the fourth pass,
  being three `playlistFileName:`, three UTF-16 `decode:`, five `deep:` and two `deep crt:`.
  Those last two are what justifies the manifest: `QFile`, `QDir` and the watch-folder walk
  reach the fixture past 260 characters with or without it, `_wfopen()` reaches it only with
  it, and a transcode temporary is written through the C runtime.  Reproduced outside the
  tree with two copies of the same 36-line C probe: on this machine, against a 356-character
  file, the undeclared binary gets `ERROR_PATH_NOT_FOUND` from `_wfopen()` and the declared
  one reads the byte back.
- `build\librarytool.exe names` 17 checks and `customize` 21 checks, both `ALL PASS` on the
  same build.
- `tests\win-compat\run.ps1`: `184 checks, 0 failures, 0 skips`.
- `tests\win-usability\run.ps1`, and all four runs of it in this pass belong to the record.
  The first answered `failures=2`, and the fault was the harness's precondition rather than
  the build: a `zuuned.exe` left open from a GUI check wrote its own `qqpc_opengl`
  pipeline-cache entry into the real profile that the isolation check compares, while the
  sibling check that looks only at processes the gate started passed.  The rule is that
  nothing else is signed in to the profile when that gate runs.  The second run was green.
  The third answered `launches=17 failures=14` on those same binaries, and that one *was* the
  application: every failing launch came down on `0xC0000409` with
  `[qt.fatal] QThread: Destroyed while thread '' is still running` as its last line, because
  closing the window destroyed the device worker's `QThread` while a device operation was
  still inside libzune - fatal by design in Qt, and taken apart in
  `docs/WINDOWS_COMPAT_AUDIT.md`.  `src/ThreadDrain.h` waits that worker out now, naming what
  it is waiting for, instead of betting five seconds on it.  A fourth run answered green on
  the repaired binary; the fifth, on the clean rebuild above, is the number this pass
  keeps: `launches=17 checks=95 failures=0 skips=0 killed=0 pid-scan-fallbacks=4`,
  `OVERALL=ok`, with every launch log but one carrying three `worker still busy at
  shutdown` lines - `waited 5s`, `waited 10s`, `waited 15s` - before closing at exit code
  0.  How many logs carry them is the device's choice rather than the application's: the
  run before this one logged 15 of 17.
- The manifest read-back and the library gates were repeated on that clean rebuild rather
  than quoted from the run before it: `mt.exe` still finds exactly one embedded manifest per
  image, `asInvoker` and `longPathAware=true` on all six; `librarytool` answers `paths` 52,
  `names` 17, `customize` 21 - `ALL PASS` each; and `tests\win-compat` answered
  `184 checks, 0 failures, 0 skips` there too.
- The `--qa-device` round trip again, both directions, on the binary a few minutes older
  than the clean rebuild, under a new title and album
  (`SineQA1021C` / `Windows Roundtrip 1021C`): `add added=1`, `forge done: 1 ok, 0 failed`,
  `sync-summary elapsedMs=1657 tracks=1 failed=0`; the device counted 193 tracks where the
  session before it counted 192; then `download-from itemId=16777482 readbackDurationMs=8000`
  and `download-saved name='SineQA1021C.mp3' ok=true`.
- The read-back was compared afterwards with a script outside this repository: the file
  the Zune HD returned is 194,945 bytes, the audio payload on either side is 193,723 bytes,
  and the first byte that differs inside that payload does not exist.  What surrounds the
  audio is the 1,094-byte ID3v2.3 this app puts ahead of a send and a 128-byte ID3v1 `TAG`
  the firmware appends, whose artist field reads `ZuunedQA` - the artist the send declared.
- The application by hand on its own profile, music library at `E:\Temp\testmusic`, device
  panel showing the Zune HD and its library: no `/E:` spelling in any label, no
  `watch folder unreachable` line.

Still nothing heard on the player, still one Pavo Zune HD, and still no deletion.  The player
now carries four QA albums from these round trips - `ZuunedQA`, and `Windows Roundtrip 1021`,
`1021B`, `1021C` - which this repository cannot take back off it, so the deletion gate starts
from a device with litter on it.

## The sixth pass: the close that outlived its window (2026-10-21)

The fifth pass repaired the device worker's exit and left a second member of the same
family recorded as a gap: the spoken worker was drained with no bound at all.
`SpokenMediaService::shutdown()` runs on `aboutToQuit` and passed the last resume-point
write to its own thread over a `Qt::BlockingQueuedConnection`, which has no deadline, and
`~SpokenMediaService` followed it with a `QThread::wait()` carrying none either - so a
close landing during an RSS fetch, an episode download or an audiobook probe waited for
that job for as long as the job liked.  It is the fifth pass's abort from the other side:
that one died on `QThread: Destroyed while thread`, this one never reached the
destructor, and what a user got was a window that had closed with the process still in
Task Manager.

`shutdown()` now posts that write and gives it two seconds to land, printing
`[spoken] last position not saved after 2000ms, closing anyway` when it does not, and the
destructor drains the thread through `Zuuned::drainThread()`, where the `2` counts two
five-second slices rather than two seconds.  An earlier sweep refused a bound here because
the helper ends in `terminate()`; what changed is scope rather than nerve, and the reason
is worth keeping.  Nothing under `src/spoken/` calls libzune or touches a USB handle - the
only matches for either in that directory are two comments saying so, and the encode a
spoken transfer needs belongs to the sync worker - so a podcast or audiobook transfer is
*prepared* on that thread and sent by `SyncEngine`/`DeviceWorker`.  The last resort can
therefore cut short a feed fetch, a probe, a cover write or a download, and a download
lands through `QSaveFile`, so an abandoned one never appears at the episode path: the cost
is a scratch file beside it, because `terminate()` skips `cancelWriting()`.  It has not
fired: no launch in this pass logged `[spoken] worker would not stop`, which leaves
`terminate()` in the same standing as the device worker's above it - reasoned about, never
exercised.

Every number below came off this machine, in order, on one
`cmake --build build --clean-first` rebuild.

- `build\librarytool.exe paths` 52 checks, `names` 17 and `customize` 21, each `ALL
  PASS`, and `tests\win-compat\run.ps1` answering `184 checks, 0 failures, 0 skips`.
- `tests\win-usability\run.ps1`: `launches=18 checks=99 failures=0 skips=0 killed=0`,
  `OVERALL=ok`, `pid-scan-fallbacks=2`.  The gate gained a phase, `spokenclose`, which is
  the first thing in this repository to watch a spoken close rather than reason about one:
  it opens the podcasts page on a seeded profile, closes the window, fails if that close
  logged `worker would not stop` or an abandoned resume point, and reads the spoken store
  the launch left behind back through `inspect_spoken_db.py`.
- What that phase prints instead of asserting is how long the close took - 17.2 s here -
  because that belongs to the machine, and this machine spent nearly all of it in the
  device worker: the same launch's log carries three `[device] worker still busy at
  shutdown` lines around `OpenSession dead — resetting device and retrying`, and no spoken
  line at all.  Spoken's share of that close is the two assertions above rather than the
  clock.

What this pass does not prove is narrower than the phase, and is written down rather than
inferred: nothing here plays a spoken item, the store that close left behind answers
`rows=0`, so in every launch above `m_positionId` was empty and `shutdown()` returned
before asking for the write.  The bound on the close is measured; the resume point inside
that bound is bounded by construction and is not gated, and the message that would show it
working has never come out of a test.

## The seventh pass: a reset that reported success (2026-10-21)

The Zune HD arrived in the wedge this port has never had to face on Windows: it
enumerated, `devicetool` read the devnode as `CM_PROB_NONE`, `VERDICT=agree`, and every
bulk transfer timed out, so no `OpenSession` and no device work at all.  The interesting
part is not the wedge.  It is that the documentation about the wedge was wrong.

`packaging/windows/DRIVER_INSTALL.md` said WinUSB cannot issue a reset and that
`libusb_reset_device()` therefore always fails.  Two probes against the live device say
otherwise, and the difference matters: `libusb_reset_device()` **returned 0**, the handle
reopened on the same bus address 2 / address 40, and the bulk OUT pipe went on timing out
exactly as before.  `libusb_clear_halt()` returned 0 on both endpoints, and
`libusb_set_configuration(1)` returned 0 six times, with the same result each time.  The
control plane answered a device-descriptor request the whole while, and
`libusb_get_kernel_driver_active()` returned `LIBUSB_ERROR_NOT_SUPPORTED` as it always
does here.  Three rounds of the whole ladder, then OpenSession: still `-7`.

A recovery path that reports success while doing nothing is the one result guaranteed to
send the next reader into the wrong layer, so `zune_breach()` is changed rather than
merely documented.  `libzune/src/device.c` keeps the ladder it has - reset, reopen, retry
once - because that retry is both the fix on Linux and the only honest liveness probe
available, and it adds the honesty this measurement asked for: the reset's return value
now goes to the log, and the error that follows a failed retry says the device still
answers enumeration but not the bulk pipes and that a power cycle is what remains.  No
`zune_unjam()` rung was added to the ladder.  It measured useless on this wedge, and a
rung here costs a second 10-second PTP command timeout on every connect that is going to
fail anyway, which is a price for nothing.  The returns, the probe shape and the
reasoning are in `libzune/docs/WINUSB_QUIRKS.md`; the driver guide's bullet is corrected
in the same commit as the code that changes its consequence.

What this pass does not do is the round trip.  Nothing was written to or read from the
player while it was in this state, the upload and download gates were not re-run, and the
measurement is one wedged Pavo on one machine rather than a claim about WinUSB resets in
general.  The player needs a power cycle before any device gate can be re-run from here.

## The eighth pass: the photo recipe that never ran (2026-10-21)

The compatibility sweep in `docs/WINDOWS_COMPAT_AUDIT.md` carried a bullet calling
`zune_arm_photo()` one of the libzune shell-outs that nothing calls.  It is called, and it
is on the live photo transfer: `src/DeviceWorker.cpp:736` -> `zune_smuggle_photo()` ->
`zune_arm_photo()`.  The contract on failure is what hid it - that caller takes a NULL
return to mean "no preprocessing needed" and sends the untouched original, so every
prepare on Windows failed quietly and the transfer still reported success.

The command the old code built was reproduced verbatim through `cmd.exe` against a fixture
folder with a space in its name (`build\scratch\picfix\My Pics\in.png`).  `ffmpeg` never
starts: no `ffmpeg` output at all, `ERRORLEVEL=-22`, twice.  `cmd.exe` reads a single quote
as an ordinary character, so `'C:\My Pics\in.png'` is several arguments, and its
`2>/dev/null` is a redirect into a `\dev\null` folder nobody has.  The line also wrote its
output under a hardcoded `/tmp/` and then tested that same path with `access()`, so even a
successful encode would have been reported as a failure.  `zune_arm_photo()` now quotes the
way `thumbnail.c` Method 2 already did - `shell_quote_dup()`, `ZUUNED_TMPDIR`,
`>nul 2>&1` - and refuses when the command line will not fit rather than launching half an
argument list.

Quoting is not the whole defect, and the second half was measured rather than reasoned.
This machine's ANSI code page is 936, and a harness that links the rebuilt `libzune` and
calls `zune_arm_photo()` directly shows three cases:

- an ASCII path with a space, handed over as `argv` bytes: `ARM=ok`.
- the same picture one folder deeper, spelled `照片`, also as `argv` bytes - the
  CRT gives narrow `argv` in the code page, so this is a path already in what `cmd.exe`
  reads: `ARM=ok`.
- that same folder as UTF-8 bytes, which is the convention every Qt caller uses
  (`filepath.toUtf8()`): **`ARM=failed bytes=80`** before, `ARM=ok` after, and `ffprobe`
  on the artifact answers `mjpeg 480x315` from a 640x420 source, so the resize really ran
  rather than a stale file being reused.

8.3 short names are no rescue: NTFS does not hand one out for a directory the code page
cannot spell, and `dir /x` shows none for `照片`.  So `compat/win/wincompat.c:1580`
adds `zuuned_narrow_path_for_shell()`, on the UTF-8-first rule `decode_path()` already
applies when it opens a file - valid UTF-8 is UTF-8, anything else is already code-page
bytes - and re-spells the path with `WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS)`.
The used-default flag is the point, not decoration: a name the active code page genuinely
cannot spell returns NULL instead of the `?` bytes that would name some other file and
read back as a missing picture.  `zune_arm_photo()` narrows its input
(`libzune/src/photo.c:361`); `zune_grab_photo_thumb()` narrows the cache path it writes
(`libzune/src/thumbnail.c:155`).  Neither narrows a `ZUUNED_TMPDIR` path, because
`build_win_tmpdir()` wrote that one in the code page already and a two-byte GBK sequence is
very often valid UTF-8, so re-spelling it would invent a different directory.

The gate for that rule is the `shell_bytes` group in `tests/win-compat`: 17 checks that use
Windows as the oracle, spelling every candidate through `WideCharToMultiByte` itself and
requiring NULL for whatever the code page refuses.  That replaced an assertion that U+E000
is unspellable, which holds on cp1252 and is **false on cp936** - GBK has a spelling for
the private use area - and it was the first time this gate ever failed here for a reason
inside the gate rather than inside the code.

All of it on one clean rebuild (`cmake --build build --clean-first`, 329 targets, no new
warnings from the touched C files): `tests\win-compat\run.ps1` answers
`201 checks, 0 failures, 0 skips`; `librarytool paths` 52, `names` 17 and `customize` 21
each answer `ALL PASS (0 failures)`; `tests\win-usability\run.ps1` answers
`launches=18 checks=99 failures=0 skips=0 killed=0` and `OVERALL=ok` - the number being a
clean solo re-run, because the first run lost two timed checks with the photo harness
running beside it (`docs/WINDOWS_COMPAT_AUDIT.md` carries why).  The three-case photo
harness is scratch code under `build\scratch\`, not a committed gate; the committed gate
is `shell_bytes`, and the photo path still needs a device.

What this pass did not do: nothing opened a USB handle.  `zune_arm_photo()` is proven in a
harness and never once against a player, so photo transfer to a device is still unproven on
Windows, and the upload and download gates were not re-run because the player is still in
the state the pass above describes.  `ffmpeg` is still not bundled, so on a machine without
it on `PATH` Method 2 fails exactly as it always did.

## The ninth pass: a send the firmware counted, and the wedge that followed (2026-10-21)

Nothing in `src/` moved for this pass.  It is a re-proof of the upload on one
`cmake --build build --clean-first` of `021d40c`, run serially, because the fifth pass
already recorded what happens when two gates share a sitting.  That binary:
`tests\win-compat` 201 checks / 0 failures / 0 skips, `librarytool paths`, `names` and
`customize` each `ALL PASS (0 failures)`, `tests\win-usability` `OVERALL=ok`.

The upload gate read `add added=1 duplicates=0 onDevice=0 rejected=0 ambiguous=0`, then
`[libzune] created album "Windows Roundtrip 1021F" by "ZuunedQA" (1 tracks,
id=100663574)`, then `[sync] forge done: 1 ok, 0 failed` and
`[qa-device] sync-summary elapsedMs=1665 tracks=1 videos=0 photos=0 skipped=0 failed=0`,
closed by `[libzune-finalize] trying CleanDataStore (0x9108)...`.  Every one of those is
the host talking about itself.  What is not came after the ladder reconnected: the same
session that had read the player's media database as `scanned: 193 tracks, 0 videos, 0
photos, 19 albums, 12 artists` read it again as `scanned: 194 tracks, ... 20 albums`.
That line is `libzune/src/zmdb.c` parsing the Zune's own database over vendor opcode
0x1792, so the count belongs to the firmware, after its own re-index, of an album this
repository put there.  It is the half of an upload that an ok can fake and a readback
cannot.

Eleven lines later the wedge came back on its own.  Last success is that device library
scan; the first failure is the video handle query inside the same reconnect.  The log
carries 20 `all thumbnail methods failed`, 20 `bulk_write failed: ep=0x01 err=Operation
timed out`, 16 `prop 0xDC86` misses and 5 `get_object_handles: failed`, and a second
process printed the seventh pass's ladder line for line, including
`device reset returned 0 (reported success)`.

With the application shut down a scratch libusb probe asked what the device was actually
doing, and none of this is committed code:

- `claim_interface` answered 0 and `get_configuration` answered 0 with `cfg=1`.  The
  interface is ours and the device is configured; nothing is contending for it.
- The interrupt pipe was holding one 24-byte container, `18 00 00 00 04 00 0c 40 00 00
  00 00 01 00 01 00 00 00 00 00 00 00 00 00`: a device event, type 0x0004, code 0x400C.
  libzune names no event codes anywhere in the tree, so this is recorded as bytes and
  given no name.
- The bulk IN pipe was holding 32 stale bytes: a `GetObjectInfo` (0x1007) data wrapper
  for transaction 0x0000000e, an 8-byte fragment of it, then the matching response 0x2001
  for that same transaction.  A command the application had timed out on had been
  answered afterwards, into a pipe nobody was reading.
- Bulk OUT still accepted a 16-byte OpenSession twice and a GetDeviceInfo once, and
  answered none of the three.  Every class control-IN timed out, and one vendor IN
  returned `-9 LIBUSB_ERROR_PIPE`.

A responder that is still working, then, and not a device that has gone away - the same
shape the seventh pass found.  Cause and symptom are still not separated: nothing here
tells the PTP responder on the player wedging apart from a WinUSB pipe holding bytes
nobody will drain, and only a power cycle has ever cleared it.  What is new is that one
end state has now arrived twice from opposite directions, once in the middle of a send and
once after a send that read itself back.

What this pass did not do: the download half was not re-run on this binary.  The pull
launch spent its whole patience on the OpenSession dance and never reached a device row,
so the both-directions proof this document leans on is still the fifth pass's
`SineQA1021C` pair.  `Windows Roundtrip 1021F` is on the player - the firmware counted it -
and has never been read back as bytes or heard, so the Zune HD is holding the fifth pass's
four QA albums plus one more, on hardware this repository cannot clean.

## The tenth pass: the tree re-read, and two diagnostics that only told the truth on Linux (2026-10-21)

A pass proves what it went looking for, so this one went looking at everything else.  The
path and encoding questions from the earlier sweeps were asked again - recursive walkers,
`QProcess`, environment reads, `/dev/null` and `/tmp` - and are answered by the sixth
sweep in `docs/WINDOWS_COMPAT_AUDIT.md`.  The new question was platform `#if` symmetry:
all 55 `Q_OS_WIN`, `Q_OS_LINUX`, `Q_OS_UNIX` and `_WIN32` hits in `src`, `transcode` and
`compat` were read in place, and every `if(WIN32)` in `CMakeLists.txt` with them.  Most
are a hint, a header, or a descriptor trick with the same shape on both sides.  Two were
not.

`src/diagnostics/SupportReport.cpp` enumerated the Zune on Windows and then threw the
answer away.  The Windows branch built a `QJsonArray usb` from libusb descriptors and the
Linux branch built the same array from sysfs, but the `info.insert` for
`zune_usb_devices` sat at the bottom of the *Linux* branch, so a support report exported on
Windows named no Zune at all - on the platform where the driver is the question, the
report stayed silent about the one field that settles it.  The array is declared before the
split and inserted once after it now, and a scratch probe over that translation unit
prints what this machine's report really carries: `zune_usb_devices` with `vendor_id
045e`, `product_id 063e`, bus 2, address 40.  Before the change the key did not exist.

`src/diagnostics/DiagnosticsService.cpp` asked the environment for `USER` to get the name
it redacts out of reports and session logs.  Windows spells that variable `USERNAME`, so
here the name arrived empty and `src/diagnostics/Redactor.cpp:125` skipped its rule:
every support report exported from this port kept the real user name in plain text while
the home path next to it was folded to `~`.  It falls back to `USERNAME` now, and both
halves of that are measured rather than argued - one sample line, the real `Redactor`, a
context built the old way and a context built the new way, which come out
`for user Kurisu Chan root ~` and `for user [user] root ~`.

Neither fix gained a gate.  The redactor's rules belong to `tests/diagnostics` and
`tests/diagnostics-report`, both bash, and the `USER`-or-`USERNAME` line lives in a file no
CLI tool links, so the two probes above are scratch code under `build\scratch\`.  Two
Windows-only defects, both in exactly the text a user would attach to a bug report, both
fixed and measured on the machine that had them, neither pinned by a test.

The gates were re-run on one clean rebuild carrying these two changes: `tests\win-compat`
201 checks / 0 failures / 0 skips; `librarytool paths`, `names` and `customize` each
`ALL PASS (0 failures)`; `tests\win-usability` 18 launches, `OVERALL=ok`, exit 0.  None of
them exercises either fix.

## The eleventh pass: an album that answers with a GUID (2026-10-22)

Reported from the device page while transfers were being re-tested by hand: the ARIA
album's line reads `{09316835-3188-481c-a359-0f8d2a937ac8}` where a name belongs, and the
albums beside it read `EGOIST` and a person's name - "so some places still have
problems".  The moniker is not the album.  `saveTrackToLibrary()` writes
`<imports>/<artist>/<album>/<file>` and the pull log for that album says
`Music\Zuuned Imports\{09316835-3188-481c-a359-0f8d2a937ac8}\ARIA The NATURAL VOCAL SONG
COLLECTION\...`, so the GUID is the artist and the album string was always intact; the
subtitle on an album row *is* an artist.

`ffprobe` on the files pulled off the player says why that album and not the others, and
`docs/WINDOWS_COMPAT_AUDIT.md` carries the full account: all three are ASF/WMA under an
`.mp3` name the device supplied, the ARIA file carries album/title/genre/disc/year and no
Author attribute at all, the EGOIST file carries only AlbumArtist, the third carries a
real author.  Where the source file had no artist the Zune software stored a GUID moniker
in the ZMDB artist record rather than leaving it empty, and the player displays what it
has.  `librarytool paths` gained ten cases for that, unbranched because the placeholder
belongs to the player and not to a platform, and the rule now lives in
`MusicIdentity::isGuidMoniker()` with `deviceArtist()`/`deviceAlbum()` answering the
app's own `Unknown Artist`/`Unknown Album` at the ZMDB scan rows and the album-object
artist in `src/DeviceWorker.cpp` plus both arguments of `saveTrackToLibrary()`.  The
consequence that mattered is the artwork cache key: a cover fetched off the device was
stored under the moniker while the album indexed from those same files asks for its art
under `Unknown Artist`, which is one way a downloaded cover ends up as no picture at all.
Nothing here repairs artwork already cached under the old key, and nothing touched
`doPurgeInterruptedObject()`, whose raw device strings are what a persisted recovery
record has to match.

Two environment findings came out of the same sitting, neither of them the application's
fault and both now recorded rather than rediscovered.  A `vcvars64.bat` from the other
Visual Studio on this machine - it has 18 and 2022 side by side - cannot break the build
in the way its name suggests: CMake pins the compiler path in the cache, so only `INCLUDE`
moves, and the newer STL headers answer `error C2338 ... STL1001: expected MSVC 19.50 or
newer` at the configured `cl` 19.44.  `BUILDING.md` says to read the toolset out of
`CMakeCXXCompiler.cmake` before believing that.  And one file appeared in the real profile
during the usability run,
`cache\_qt_QGfxShaderBuilder_6.11.0\1088d1721e08fe3ecb3414ce01b8226540ad6d3e.qsb`: a third
Qt-internal cache, owned by the `QGfxShaderBuilder` singleton inside Qt5Compat
GraphicalEffects whose `FastBlur` `qml/EnvironmentBackground.qml` uses, which asks
`QStandardPaths::CacheLocation` for itself and offers only
`QT_GFXSHADERBUILDER_REFRESH_CACHE` - a rebuild switch, not a redirect - so it escapes
`ZUUNED_PROFILE_DIR` and only shows when some variant has to be compiled for the first
time.  Left alone, with the reason in `docs/WINDOWS_COMPAT_AUDIT.md`.

Gates on one `cmake --build build --clean-first` (329/329) carrying the change:
`librarytool paths` `ALL PASS (0 failures)` at 62 checks, `names` and `customize` each
`ALL PASS (0 failures)`, `tests\win-compat` `201 checks, 0 failures, 0 skips`, and
`tests\win-usability` `launches=18 checks=99 failures=6 skips=0 killed=0`.  Those six are
the two desktop-dependent checks, not six defects: the display this run landed on is
150 per cent and 960 logical pixels tall, and a roomy window needs 1100, so the four
ribbon checks asked for a size that screen cannot show; the other two are the new `.qsb`
above.  A re-run of `-Only compact,fixture` reproduced four of them exactly, which is what
makes them desktop rather than flake.

**No Zune was opened after this change.**  The evidence for the moniker is the screenshot
description the user gave, the two pulled files, and the log line; the fix for it is
compile-and-gate evidence only, and no Windows device claim is added by this pass.
Superseded the same day by the twelfth pass, which opened the player again.

## The twelfth pass: the moniker came back in through the front door (2026-10-22)

**The eleventh pass is superseded at its last line.**  That pass ended without opening
the player.  It has been opened since, twice, on the build carrying that change, and the
browse still answers `189 tracks, 15 albums, 11 artists`.  What follows lives inside
the music-library-read gate that closed at the hardware checkpoint; this page gains no
new Windows capability here, and nothing is carried over from Linux.

Counting beat reasoning.  The ZMDB scan now tallies the raw device fields before it
maps them and prints the tally, which answers the question the user asked off the
device page - why the ARIA album shows a GUID where the albums beside it show names:

    [zmdb] GUID moniker fields on 189 track rows: artist 27 ({09316835-3188-481c-a359-0f8d2a937ac8}), album 0 (-); blank-padded artist 27, album 0

Twenty-seven of the player's 189 track rows name their artist by GUID moniker;
not one row names an album by one.  So
the text the user read as an album name is an album row's subtitle, which is an artist,
and the eleventh pass's diagnosis is confirmed rather than merely plausible.  The count
settles a second thing: those 27 rows are exactly the 27 rows whose artist arrives
blank-padded, so placeholder and padding are one phenomenon, and trimming a device name
belongs in `deviceArtist()`/`deviceAlbum()` rather than in whichever caller remembers
it.  Untrimmed, one artist is two artwork cache keys, two comparisons against the
locally indexed album of the same name, and two import folders.

What the count turned up is that the **local library was poisoned**, by this
application, which is what the eleventh pass said could not happen.  The mechanism is
the port's own and it is worth stating as a rule rather than an anecdote: a pull written
before the device fields were sanitised put the files at
`Music\Zuuned Imports\{09316835-...}\<album>\<file>`; the watch folder then scanned
what the pull had just written; and `LibraryScanner`'s `Root/Artist/Album/file`
inference promoted that GUID folder to `artist` and `albumartist` for a file with no
Author tag.  A string the device boundary refused came back as a name through the disk
boundary.  Because a row whose stored probe version already matches is never read again,
no rescan reaches it and `musicParserVersion` cannot move - `tests/music-import`
pins it at 1 on the Linux line.  `MusicIdentity::usableName()` therefore refuses a
moniker as a name for every candidate a scan takes - the three tags, both folder
inferences, the `Artist - Title` filename fallback - and
`LibraryDb::repairDeviceMonikers()` repairs what a scan cannot reach, from `open()`,
mirroring the scanner's own Unknown Artist / Unknown Album fallbacks and leaving any
`user_edited` row alone.  Measured on the real profile: two rows rewritten on the first
launch, `EGOIST` preserved on the row whose album artist was real, no `{` left in
`tracks`, and no repair line on any launch after that.

Gates on the relinked build: `librarytool paths` `ALL PASS (0 failures)` at 67 checks,
`tests\win-compat` `201 checks, 0 failures, 0 skips`.  `tests\win-usability` was not
re-run for this change - the app was in the user's hands on the real profile, and the
GUI harness is not what reaches this code - so its last recorded counter still belongs
to the eleventh pass.  `docs/WINDOWS_COMPAT_AUDIT.md` carries the same sitting as its
seventh sweep, including the session-log redactor that folded the first version of that
audit line into `[redacted]`.  The list below is unchanged by this pass: deletion, the
podcast, audiobook, photo and video categories, every model but this Pavo, and anything
anyone has listened to are still open.

## The thirteenth pass: the uuid was a genre (2026-10-22)

Same question, asked again from the device page: on the player, the ARIA album's name is
still a GUID and the albums around it are not.  The twelfth pass answered "that is the
artist subtitle, not the album name", and was right about the album and wrong about the
screen.

**The premise was measured this time, per field, before anything was changed.**  The scan
now prints the whole name pool it holds, not a count of the one placeholder shape:

    [zmdb] GUID moniker fields on 189 track rows: artist 27 ({09316835-3188-481c-a359-0f8d2a937ac8}), album 0 (-), genre 130 ({09316835-3188-481c-a359-0f8d2a937ac8}); blank-padded artist 27, album 0, genre 130
    [zmdb] distinct genres (6): "                {09316835-3188-481c-a359-0f8d2a937ac8}", "Classical", "JPop", "QA", "Unknown Genre", "アニメ"
    [zmdb] genre tiles after the moniker rule (5): Classical (17), JPop (13), QA (4), Unknown Genre (12), アニメ (13)

Zero moniker albums in the track rows, zero among the 15 album objects, and 15 real album
strings; 130 of 189 rows carry the moniker in **genre**, the one name field this app copied
over raw.  So the GUID on screen is a genre tile: the Genres pivot titles a tile with
`modelData.name` and puts up to four of that genre's albums in a 2x2 mosaic under it, with
the albums listed below - which is what makes a genre look like an album, and why only
"some" albums appeared to be affected.  `MusicIdentity::deviceGenre()` yields empty for it,
because both pivots skip an empty genre and that is what an untagged file already answers;
the device's own literal `Unknown Genre` is content and stays.

Reads only, on the same player, inside the music-library-read gate that closed at the
hardware checkpoint.  The local `library.db` (15 tracks) turned out to hold no moniker
genre, so nothing needed repairing this time; `LibraryScanner` asks `usableName()` of the
genre tag now and `repairDeviceMonikers()` carries the column, so the door is shut for the
next pull.  A side effect worth naming: `DeviceService::pickMoodColor()` keys the device
page's accent off the highest-count genre, so on this player it had been keying off that
GUID and falling through to a random colour on every single connection.

Gates on the `--clean-first` rebuild: `librarytool paths` `ALL PASS (0 failures)` at 73
checks (74 as first written, re-counted; six of them the genre rule), `tests\win-compat` `201 checks, 0 failures, 0 skips`.
`tests\win-usability` again not re-run: as in the twelfth pass the app was in the
user's hands on the real profile, and while such an instance is open the gate's two
profile-isolation checks fail by design (`docs/WINDOWS_COMPAT_AUDIT.md`).  `docs/WINDOWS_COMPAT_AUDIT.md` carries this as its
eighth sweep.  No Windows capability is added and no device claim is changed - still one
hand-tested Pavo Zune HD, still nothing listened to, and deletion, the spoken and
photo/video categories and every other model stay open.  One lead left on purpose for the
open artwork complaint: `[customize-http] HTTP 0, Qt error 4 (https://api.deezer.com/...)`
with `QIODevice::read (QNetworkReplyHttpImpl): device not open`, in the current log - the
fourteenth pass picks this up and corrects it: the request did start, and error 4 is a
timeout on a machine whose proxy is switched off.

## The fourteenth pass: the last mile spoke a second language (2026-10-22)

**The same question, third time, and this time the answer was in the application.**  A
read-only re-sighting of the Zune HD now prints the labels the screen draws and not just the
columns it was handed - `[zmdb] artist tiles ...`, `[zmdb] album tiles ...`, and a `title`
counter added to the field census, which reads artist 27, album 0, genre 130, title 0 of 189
rows - and all 15 album tiles on this player carry their real names, ARIA's included, with
its subtitle answered as `Unknown Artist`.  No field on this device is a GUID except the two
that are already ruled, so the place a moniker still reached a screen was in QML.

**`qml/MusicIdentity.js` is a second implementation of the rule and had none of it.**  It is
not a binding; it is the same decision re-written in JavaScript, and it is the last mile for
`MusicPage`, `ArtistDetail` and `GenreDetail` on local rows and device rows alike.  It
carries the C++ predicate now - braces optional, surrounding blanks not part of the answer -
so a moniker cannot become a tile name.  Three more raw doors went with it: `MusicPage`'s
`computeArtists()` grouped off `t.artist` without ever asking the identity module,
`AlbumDetail`'s headline and byline fell back to the first row's own fields unruled - a value
that is also the name of the artist page its byline opens - and the customize re-key compared
a drill-down key it built from a raw `before.artist` against an entry the mirror had already
ruled, which fails by re-keying nothing and saying nothing.  The SyncEngine payload stays raw
on purpose: the identity a send writes is not a label, and the rules that govern it are the
scanner and `repairDeviceMonikers()`.

**Neither Windows gate reaches a `.js` file, so the mirror was checked twice over by hand.**
Evaluating the module says what it decides; `qmllint` says whether QML can see it, and it does
resolve the relative `import "MusicIdentity.js"` - it names
`Member "noSuchFunction" not found on type "MusicIdentity" [missing-property]` for an export
that is not there, measured against a planted fault.  Across the 96 `.qml` files in `qml/` no
`missing-property` warning names `MusicIdentity` and there is no `Error:` line, but it is not
a gate: 3246 of those warnings name `Theme` and other singletons a source-directory lint
cannot resolve.

**The gate that failed here was the harness, third time in that shape.**  `Main.qml`'s
breakpoints are logical pixels and `winshot`'s `MoveWindow` takes physical ones, so the
compact phase's fixed 1400x1220 request cannot reach the ribbon at 150% scaling; and asking
`GetDpiForWindow` from PowerShell answers 96 to a DPI-unaware caller, so the first repair
reported a 150% desktop as 100% and went on blaming the application.  The scale now comes
from `winshot --dpi`, which declares itself per-monitor aware before it asks, and the
minimum-size check compares logical pixels against `Main.qml`'s 1100x700 floor instead of
comparing physical ones and passing vacuously.  One consequence recorded and deliberately
not tuned: at 150% a 2560x1440 desktop is 960 logical pixels tall, below
`headroomCompactBelow` (1100), so the top ribbon is unreachable in any window that fits that
screen.  That is a design question, not a Windows defect.

**The artwork lead is corrected, and the fix this time is a log line.**  `Qt error 4` is
`QNetworkReply::TimeoutError`, not `HostNotFoundError`.  Four curls on the same machine
reach `api.github.com` in 0.39 s and fail to connect to Deezer and Google in ~21 s, both of
which answer in under 1 s through `http://192.168.2.100:8117` - but `ProxyEnable` is 0 and a
fresh Qt 6.11 process reports `usesSystemConfiguration=1`, so **Qt already follows the
Windows shell and going direct is correct**; this machine simply has its proxy switched off.
What was missing was the route in the log, so `src/library/NetworkAccess.h` prints
`[network] ... HTTPS goes direct` once at startup and `via <route>` on every failed
`[customize-http]` line, plus a `ZUUNED_PROXY` override for a tester who wants provider art
without switching the whole desktop.  Unset, nothing is installed and Qt's answer stands on
both platforms.

Gates on the relinked binary: `librarytool paths` `ALL PASS (0 failures)` at 73 checks,
`librarytool net` `ALL PASS (0 failures)` at 27, `tests\win-compat` `201 checks, 0 failures,
0 skips`, `tests\win-usability` `launches=18 checks=102 failures=0 skips=0 killed=0
pid-scan-fallbacks=1` with `OVERALL=ok`, and run again on the relink that carries the fourth
door for `launches=18 checks=102 failures=0 skips=0 killed=0 pid-scan-fallbacks=0` and
`OVERALL=ok` - that fallback counter is a tally, not a constant.  No Windows capability is added and no device claim
changes - still one hand-tested Pavo Zune HD, read-only in this pass, nothing listened to.
`docs/WINDOWS_COMPAT_AUDIT.md` carries the same sitting as its ninth sweep.

## The fifteenth pass: the last hand-rolled walk, and what this fork calls itself (2026-10-22)

**The item the closing-order list has carried since the fourth pass is closed.**
`SpokenMediaService::previewBookFolder()` walked a user-picked audiobook folder with its own
recursive `QDirIterator`, and `QDir::NoSymLinks` - the filter that reads like a guard - does
not exclude a Windows junction, because `QFileInfo::isSymLink()` is false for one.  A folder
linking back into one of its own ancestors stayed an ordinary directory to it, and one book
came out filed under every phantom path the loop invented.  It calls `LibraryScan::walkFiles()`
now, the walk a watch folder uses, whose per-folder identity (volume serial plus file index)
refuses a folder it has already listed and which `librarytool walk` holds against a real
`mklink /J`.  Four behaviours were compared against the iterator instead of assumed
unchanged: the file set, the two ceilings, the unreadable-folder refusal (now two sentences
rather than one, and an empty folder still reads as empty) and cancellation.
`docs/WINDOWS_COMPAT_AUDIT.md` carries the sitting as its tenth sweep, with the list of what a
second whole-tree read did not find.

**Cancellation had to be wired by hand, and the first cut lost it.**  `walkFiles()` stops on
the one flag it is handed and the flag this call hands it belongs to the two ceilings, so a
cancelled reading of the caller's flag had to move into the visitor and stop the walk through
the shared atomic.  Read only after the walk, as the first cut read it, a Cancel press would
have waited out a folder of six figures - and the bounded spoken-worker close at shutdown
waits on that very loop, which is the one place a slow cancel is not merely annoying.

**This fork now says its own name in its own documents.**  `AGENTS.md` and `docs/PORT_PLAN.md`
called the project `ZuunedLinux`, and `packaging/TESTER_README.md` sent a reader to upstream's
Linux release downloads - accurate about upstream, wrong about the checkout they were sitting
in, because this fork publishes no Linux binaries.  Those three and `docs/ANNOUNCEMENT_VIDEO.md`
now name which product each one describes: this fork (`miemiekurisu/zuuned-modern`, a Windows
client port of [badgeromen/zuuned-linux](https://github.com/badgeromen/zuuned-linux)) or the
upstream a feature came from.  `README.md` already said Zuuned Modern.  Runtime identity is
left alone on purpose - `Zuuned`, `zuuned.exe`, `%APPDATA%\Zuuned`, `ZUUNED_PROFILE_DIR` -
because a rename orphans every profile and library this fork already has on a real machine,
for the sake of a label.  Two statements in `AGENTS.md` the fifth pass had already invalidated
(no `<longPathAware>` entry; UTF-16 m3u undecoded) are qualified to point at that pass instead
of being copied forward as open.

Gates on a `cmake --build build --clean-first` relink: `tests\win-compat`
`201 checks, 0 failures, 0 skips` and `tests\win-usability`
`launches=18 checks=104 failures=0 skips=0 killed=0`, `OVERALL=ok`, including two new `tools`
pins that keep a second recursive walk out of `src/`.  No Windows capability is added and no
device claim changes - still one hand-tested Pavo Zune HD, read-only, nothing listened to.

## The sixteenth pass: a second executable, a name that is not a name, and what this fork is called (2026-10-22)

Everything was re-run on a relinked binary: ``cmake --build build --clean-first`` over the
whole tree, then the two gates. ``tests\win-compat`` reports 201 checks, 0 failures and 0
skips; ``tests\win-usability`` reports launches=18 checks=107 failures=0 skips=0
killed=0 pid-scan-fallbacks=0, with ``OVERALL=ok``. The three checks the usability gate gained since
the fifteenth pass are the profile-path pins described in
``docs/WINDOWS_COMPAT_AUDIT.md``; they are source pins, so they gate a rule the harness
cannot otherwise reach, and the tool they describe was built and run by hand.

A sighting was then answered from the read rather than from a guess. Asked why the ARIA
album on the attached player carries a GUID where a name belongs while other albums do
not, a scan of that player answers it in four lines, from an instrumented launch into an
isolated profile over the same 189 tracks:

- the GUID moniker sits on **27 of 189 artist fields and 130 of 189 genre fields, and on
  0 album fields and 0 title fields**. Blank padding arrives with it: the same 27 and 130.
- the album pivot the screen draws prints **15 of 15 real names**, and ARIA's tile reads
  ``"ARIA The NATURAL VOCAL SONG COLLECTION" by "Unknown Artist" (13)``.
- the player's own album object ``100663309`` carries its real name as plain text,
  ``name="ARIA The NATURAL VOCAL SONG COLLECTION"``.
- the artwork print lab, which keys off the cache and the local database rather than off
  ZMDB, exports a sheet named ``ARIA The NATURAL VOCAL SONG COLLECTION / Unknown Artist``.

So the GUID is the player's, not this application's: a Zune HD that never learned an
artist or a genre stores a GUID moniker in that field and displays it, and the albums that
read normally are the rips whose tags carried one. ``Extra terrestrial Biological
Entities`` is the case that shows both halves of one album at once - its album object says
``EGOIST`` while its track rows carry the moniker - which is exactly why a GUID can be on
screen in one view of an album and a name in the next. ``MusicIdentity::isGuidMoniker`` and
``deviceArtist``/``deviceAlbum``/``deviceGenre``, with their mirror in
``qml/MusicIdentity.js``, turn every one of those into ``Unknown Artist``, ``Unknown Album``
and ``Unknown Genre`` wherever the application renders a name, keys an artwork cache, names
a pull folder or would write an artist back to the next player. Nothing in this tree's data
has ever held a GUID in an album name.

The limit on that paragraph is worth keeping: it is a read of one player, and a GUID
appearing in an album tile of *this* application on a build that carries the rule would be
a different defect, one to be chased with the string that was actually seen.

What this fork is called is now said consistently. ``README.md``, ``BUILDING.md``,
``AGENTS.md`` and ``docs/PORT_PLAN.md`` already read Zuuned Modern; four documents that
still read as upstream's now say which product they describe. ``docs/README.md`` opens with
the fork banner and points at ``docs/WINDOWS_PORT.md`` for current Windows status;
``docs/releases/README.md`` and ``packaging/TESTER_INSTALL.md`` are labelled as upstream's
Linux notes and AppImage instructions, kept as shipped; ``ci/README.md`` says its release
procedure is upstream's, that the fork publishes no binaries, that its gate is the two
Windows scripts, and that ``ci/check.sh`` itself stays Linux-only.

``CLAUDE.md`` was the worst of the lot and is 58 lines now. It held 818 lines of the
upstream Linux guide frozen at an earlier revision, and diffed against ``AGENTS.md`` it had
exactly three lines that were not already there: its old ``ZuunedLinux`` title, and a
recovery note claiming ``usbreset 045e:0710`` was the way back from a wedged OpenSession -
Linux-only, and superseded by the measured Windows account of a reset that reports success
and does nothing. A duplicated guide is how a superseded instruction survives a fork, so
the file is a pointer now: ``AGENTS.md`` is the log, ``docs/WINDOWS_PORT.md`` is the status,
and the history it carried is in ``git log -- CLAUDE.md``.

No Windows capability is added and no device claim changes - still one hand-tested Pavo
Zune HD, read-only, nothing listened to. Deletion, the podcast, audiobook, photo and video
categories, every model but this Pavo, audible audio and packaging stay open.

## The seventeenth pass: a QML name that belonged to Qt, and the two categories that reached the player (2026-10-23)

Everything here was re-run on a relinked tree - ``cmake --build build --clean-first``, then both
gates.  ``tests\win-compat`` reports 201 checks, 0 failures and 0 skips; ``tests\win-usability``
reports launches=18 checks=116 failures=0 skips=0 killed=0 pid-scan-fallbacks=2 and ``OVERALL=ok``.
Two of the eighteen launches needed the ``winshot --pid`` scan instead of
``Process.MainWindowHandle``, which is the cached-zero behaviour the harness was built for and is
counted rather than smoothed away.

**A singleton whose name was already taken.**  ``src/PathText.h`` registered the path singleton
with ``QML_ELEMENT``, which offers QML the bare name ``PathText`` - and ``Qt6Quick.dll`` exports a
``PathText`` of its own, a shapes type, beside ``PathSvg``.  Every QML file in this application
imports ``QtQuick``, so in a file that does not also import ``Zuuned`` the name resolves to Qt's
type and every binding on it throws at run time.  It did, and the log said so while the gate
passed:

    [qml-warn] qrc:/qt/qml/Zuuned/QueueEntryView.qml:147: TypeError:
    Property 'url' of object QtQuick/PathText is not a function

six sightings in one instrumented launch, one site, which is queue-row artwork failing once per
row and again per refresh.  ``qml/QueueEntryView.qml`` and ``qml/OnboardingView.qml`` import
``QtQuick`` without ``Zuuned``; ``qml/LibraryPhotosView.qml`` and ``qml/LibraryVideosView.qml``
import both, which is why the same call worked on one page and not the next.  The singleton is
``QML_NAMED_ELEMENT(ZPathText)`` now, and a scan of every ``Qt6*.dll`` in the Qt bin answers that
``PathText`` was the only name this tree exports that Qt also owns.  The usability gate reads the
rule off the source outside every phase - it fails if any ``qml/*.qml`` writes a bare
``PathText.``, and it pins the registered name - because no phase of it loads a QML file that
forgets the import, so nothing but the source can gate it.  The launches since the rename log
neither line of that TypeError, in the harness profiles or in the profile the user actually uses.

**A tool that could not read its own arguments.**  ``transcode/transcodetool.c`` rebuilds
``argv`` from ``GetCommandLineW()`` as UTF-8 on Windows.  The CRT hands ``main()`` its arguments
in the ANSI code page, so on this locale-936 machine a Chinese path arrived as question marks and
probe, art and frame all failed on a file the application opens happily.  That mattered because
the tool is the gate: ``FrameThumbnailService``, ``LibraryService`` and ``AudioCopyFingerprint``
all hand the same C functions ``toUtf8()``, so an ANSI ``argv`` made the encoder phases test a
different byte string from the one the application uses.  Two ``encode`` cases and one ``decode``
case now run over a Chinese album folder and over a root the ANSI code page cannot spell at all.

**One command per category.**  ``librarytool photoscan <folder> <tmpdb>`` prints the photo rows a
scan produced, so a photo library becomes something a gate counts instead of something an eye
checks; the fixture gained two photos in a Chinese-named album folder and the gate insists both
are in the database with the folder name intact.  ``--qa-photo "<file>[;<file>...]|<album>"`` and
``--qa-video "<file>|<title>"`` drive a photo album and a movie through ``SyncEngine.addPhotos``
and ``addVideos``, the same typed admission the ordinary controls use, and the semicolons keep a
whole album inside one device session the way the album action does.

**Then the two categories crossed the wire.**  Both are recorded from a launch into an isolated
profile against the attached Pavo Zune HD, and both answer in the application's own lines:

- five photos into an album named in Chinese: ``photo prepared (480px JPEG): DSC00891.JPG`` then
  ``photo sent: DSC00891.JPG -> folder 83886360``, four more the same way, closing
  ``sync-summary elapsedMs=5288 tracks=0 videos=0 photos=5 skipped=0 failed=0`` over a device
  re-read that counted ``devicePhotos=5``.
- one movie, titled in Chinese: the transcoder ran on the real clip
  (``video: loop done - 1795 video frames, 2660352 audio samples``) and the send answers
  ``video sent, item_id=33554718`` at 20,575,375 bytes in container ``0xB982``, closing
  ``sync-summary elapsedMs=14089 tracks=0 videos=1 photos=0 skipped=0 failed=0`` over a device
  re-scan that answered ``[videos] 1 device keys, e.g.: ....``

That is a send the firmware accepted and re-listed, and it is not a read-back.  No photo byte and
no video byte came off the player to this machine, nothing on the player was played or heard, and
the movie this application made answers every thumbnail method with
``get_representative_sample 0x0200011E: bad AUINT8 count 0 in 4-byte payload`` and
``all thumbnail methods failed for 33554718`` - one of the response classes the first Windows read
left unanswered, now attached to an object this port wrote rather than one it inherited.

**Four sentences in this repository were wrong, and three of them undersold the evidence.**
"no picture has been prepared and smuggled to a device from Windows" (item 10 below) is closed by
the five lines above; "photo and video transfer" as a single open item is now a send that is done
and a read that is not, and the open list says it in those words; the sixteenth pass closed itself
with "still one hand-tested Pavo Zune HD, read-only" when that document had already recorded a
write and a byte read-back two passes earlier.  The fourth overreached the other way:
``tests\win-usability\run.ps1`` described the hand test as having "sent music, photos and video,
and read back from", and only the music has ever been read back byte for byte.  All four now say
what was measured.

## The eighteenth pass: a lookup that waits until it is asked (2026-10-23)

Asked why the application was calling ``https://musicbrainz.org/ws/2/artist/`` at all when the
files on disk already carry their covers, the read answers it.  ``src/LibraryService.cpp`` armed
one 750 ms timer per library load into ``discoverMissingArtwork()``, which asked for artwork for
*every* album and a portrait for *every* artist.  Album art is local-first - the provider is
reached only at the ``artReady(ok=false)`` hop, after the embedded picture and the folder image
have both failed - but an artist portrait has no local source at all, so every artist in a
library became a MusicBrainz lookup and a Deezer image fetch, and
``qml/MusicPage.qml`` and ``qml/ArtistDetail.qml`` asked again on every visibility change.  A
profile of fourteen songs was enough to make six provider requests and take a
``HTTP 503 ... (https://musicbrainz.org/ws/2/artist/)`` for it.

The answer that was asked for is not "search less", it is "ask per item", and that is what shipped.

- ``AppSettings.autoMetadataLookup`` is a real setting, an ini key, and it defaults to **false**.
- Both automatic hops check it before they reach a provider.  A forced key
  (``m_forceArtworkRetry``, the set the Customize sheet retry and the Settings button already
  used) still reaches the network whatever the switch says, so no explicit request lost a path.
- Right-click asks for exactly one thing: ``albumLookupAction`` on an album,
  ``artistLookupAction`` on an artist, and a "Search Cover Online" row on a track row, each of
  which calls ``LibraryService.retryArtwork`` for that one artist/album and nothing else.
- A library read with the switch off says what it did instead of being silent about it:
  ``[artwork] online lookup off: 13 albums and 7 artists read locally, none queried;
  right-click an album, artist or track to look one up``.

Measured, on the profile that raised the complaint rather than on a fixture: the launch before the
change logged six ``customize-http`` lines including that 503; the launch of this build logs the
line above and **zero** provider lines.  In the gate the music-page case asserts both halves - the
report line appears, and no provider request line appears at all - so the offline default is a
fact the harness protects instead of a coincidence nobody noticed, and the gate header no longer
has to disclaim a network call it could not see coming.

What it costs is worth stating instead of discovering.  On a library whose files carry no embedded
picture and no folder image, album tiles and artist circles now stay empty until someone asks, and
the artist grid says ``Needs a match`` under every circle.  That is the requested behaviour: the
network is a thing a user does to one item, not a thing that happens to a library.

No device claim changes with either pass: still the one hand-tested Pavo Zune HD, and nothing on
it has been heard.

## The nineteenth pass: three ways to lose a cover, and a store that will not be rushed (2026-10-24)

Asked, again, to fix the album covers on the PC side.  Nothing about album grouping, identity or
cache keys was wrong: every album on this machine that *owns* a picture owns it correctly, and the
application was losing or overriding that picture between the file and the screen.  Four faults,
and the fourth was caused by fixing the first three.

**A destination Windows refuses to delete.**  ``storeToCache()`` staged every picture into a live
``QTemporaryFile`` and then published it with ``QFile::remove(dst)`` followed by ``QFile::copy(src,
dst)``.  On Windows a name whose owning ``QTemporaryFile`` object is still alive cannot be
removed - not after ``close()``, because the holder keeps the handle open - so ``remove()`` failed
with a sharing violation and ``copy()`` then refused the file that was still there, and the branch
returned false.  POSIX unlinks an open file without complaint, which is why the same nine lines
have always worked on the Linux line.  The branch is the *verbatim JPEG* one, taken for any picture
that is already a JPEG of 400 px or less, which is exactly what a ``folder.jpg`` and a
device-supplied thumbnail normally are: album art whose source was a folder image or the device
never reached the cache, on this platform alone.  ``copyBytes()`` opens the destination
``WriteOnly | Truncate`` and streams into it, which the holder permits because it shares reads and
writes and withholds only deletion.

**An order that rewarded the wrong file.**  The local chain was known-names -> *any* image in the
folder -> embedded picture, so a scanned lyric sheet or a band photo dropped in beside the lossless
files beat the picture inside those files.  The order is now a known name (``cover``, ``folder``,
``artwork``, ``album``, ``front``, case-insensitive, ``.jpg``/``.jpeg``/``.png``) in the track's own
folder; that same known name one level up when the folder is only a disc; the embedded picture;
and last of all an image that goes by no recognised name.  The disc rule is
``^(?:cd|disc|disk|vinyl|lp|pt|part)[\s._-]*[0-9]+$`` - prefix required, whole name accounted for -
so ``CD1`` and ``Disc 2`` inherit the album's picture while an album genuinely called ``Vol 2`` or
``Album 2024`` is not a disc folder and never inherits whatever its own parent happens to hold.
``tests/device-music-import/album-art-precedence.cpp`` pins all four rungs.

**A key that belonged to the wrong entity.**  Artist tiles and the player asked for art keyed by
*the track artist*.  For a local picture that key is also a path the artist row does not have, and
it stops being the album owner at the first compilation or featured artist, so an album with a real
cover painted nothing on the artist page that owns it.  ``localArtOwners()`` in
``qml/MusicIdentity.js`` derives, per album, whose art that album owns, and ``qml/ArtistCard.qml``,
``qml/ArtistDetail.qml``, ``qml/MusicPage.qml`` and ``qml/Main.qml`` ask for the owner's key and
fall back to that album's own cover.  The usability gate asserts both halves now - every album
resolves the same picture in the grid and in the player, and every album carrying a cover hands one
to its artist tile.

**What the fix then exposed.**  Nine of this machine's thirteen albums have no picture in a file,
in a folder, or in a store: they are the sine-wave fixtures the scan gates generate, and they stay
blank because blank is the truth.  For an album a store *does* carry, ``albumCoverLookup`` - a
switch of its own, and since October 24, 2026 it ships **off**, exactly like ``autoMetadataLookup`` - walks the
``us, jp, tw, hk, gb, de`` catalogues, accepts only an exact normalised artist+album match
(earliest release date, then smallest id), and keeps only an ``https`` URL on ``*.mzstatic.com``,
rewriting the ``/NNNxNNNbb.jpg`` size token to ``600x600``.  MusicBrainz stays behind
``autoMetadataLookup``, off, exactly as the eighteenth pass left it: a file that carries its own
art causes no request, and right-click is still how you ask about one thing.

**And then the store said no.**  An album with no local art is up to six catalogue requests, and a
library issues them for every such album at once.  ``ArtworkHttp`` paced MusicBrainz and nothing
else, so the library sized that burst measured about 2.2 requests a second on Apple's search
endpoint; and ``resolveStoreAlbumArtwork()`` treated a catalogue that had *errored* the same as one
that had answered nothing and kept descending the ladder, so a rate-limited album spent all six
rungs.  ``HTTP 429`` arrived, every rung errored, each error was filed as "no music store carries
this album", and the outcome was a library in which every store lookup had been tried and none had
been believed.  Three changes, all in transport and ladder rather than in matching:

- one store search in flight process-wide, at least 2 s apart, sleeping in 25 ms slices so
  ``beginShutdown()`` still stops promptly;
- on 429 or 503 the next slot moves out by ``Retry-After`` when the response carries one and by
  four times the interval otherwise, and a request that would have to wait more than 30 s is
  answered locally with a synthesized 503 - so a rate limit is *never* recorded as an album with no
  cover, which is the difference between a retry later and a permanently wrong answer;
- the first catalogue that errors ends the ladder, and ``OnlineAlbumArtService`` parks that album
  for five minutes instead of spending five more requests on an outage it has already seen.

``isStoreSearch()`` is public so ``librarytool cover`` can pin the rule without opening a socket;
that self-test is 33 checks, and among them the pacing arithmetic and "one unanswered catalogue is
enough to learn it", which asserts ``asked == 1``.

Measured rather than reasoned.  A ``VACUUM INTO`` copy of the real library database with its
customizations cleared, launched as its own profile on the built app and left alone:
**42 store searches in 87.6 s, about 29 a minute, minimum gap 1.7 s, no 429 and no 503 at all**.
Every album reached a terminal honest verdict - seven ``noArtwork ... (No music store carries this
album.)`` and two ``needsMatch`` for the Unknown Artist, which was never sent anywhere - and the
probe reported ``albums=13 artWith=4 artAgree=13 artOwnerWanted=4 artOwnerFound=4 wfNative=3
wfRaw=0``.  The four albums that own art paint, on Albums and on Artists.  A store-only album was
proven separately and end to end: 33,762 bytes of cover for one Anri album fetched into an isolated
profile's cache and published through the same ``link()`` publisher the artist portraits use.  On
the relinked build ``tests\win-compat`` answers ``201 checks, 0 failures, 0 skips`` and
``tests\win-usability`` ``launches=18 checks=123 failures=0 skips=0 killed=0``, ``OVERALL=ok``.

Two harness facts worth carrying.  PowerShell 5.1 resolves ``-Include`` against the *parent* of a
``-Recurse`` path, so ``...\cache\art\*`` combined with ``-Include *.jpg`` matched nothing: the
gate had been certifying an empty directory, in two places, and would have passed no matter what
the application cached.  And ``tests\device-music-import`` cannot run on this platform at all - it
isolates its artwork cache with ``qputenv("XDG_CACHE_HOME")``, which the Windows shell folders
ignore - so five of its ``device-album-art`` checks fail here.  Those five were reproduced against
a scratch build of HEAD's own ``AlbumArtService.cpp`` and came back identical, so they are a
Linux-only gate with no Windows verdict in them, not something this pass broke.

## The twentieth pass: where a download is allowed to land, and a folder that could not
count itself (2026-10-24)

The complaint was about the library rather than the player: a music library is one thing, and a song
pulled off a Zune was not appearing in it.  It was appearing *beside* it.  Three ``static`` helpers
in ``DeviceService`` returned a ``QStandardPaths`` location plus ``Zuuned Imports`` - behaviour
inherited from the Linux line and never questioned there either - so a pull landed in a folder the
application had invented and the user had never added, next to the folders they had.  What was asked
for came with the rule the app had never held: when several folders hold the same kind of media, one
of them is the default; a folder that is not the default gets a ``default`` button on the same line
as ``type`` and ``remove``; the folder that *is* the default says so as a status rather than as a
button; and until anybody clicks, the folder added first is the default.  That last clause is why
the feature needs no setup.  The first music folder added on this machine is
``E:\Temp\testmusic\测试音乐``, and a download went there with nobody choosing anything.

**Three constants became one question.**  ``musicImportsDir()``, ``photoImportsDir()`` and
``importsDir()`` are members now and answer ``LibraryService::ensureImportRoot(kind)`` for
``music`` / ``photos`` / ``video``, wired by ``device->setLibraryService(library)`` in
``src/main.cpp``.  The fallback keeps its old spelling for a library that holds no folder of that
kind, and registers it as a watch folder *before* the first byte is written, so the one case where
the app still invents a folder invents a scanned one - ``musicImportsReady`` calls
``ensureImportRoot("music")`` for the same reason.  What had to go was the other half of the old
design: the videos page ended a save with ``addWatchFolder(importsDir(), "all")`` and the photos
page with ``"photos"``, which existed only because the alternative was a file nothing indexed.  Once
a download may land in a folder the user added, that line would silently retype their declaration,
so all three call sites are gone and the pages say why in a comment.

**Schema v10, and one query that answers both halves.**  ``watch_folders.import_default`` arrives at
``kSchemaVersion`` 10 with a v9 ``PRAGMA table_info`` probe for a database that already carries it.
``LibraryDb::importRoot(kind)`` is a single statement - ``SELECT path FROM watch_folders WHERE type
IN (<qualifying types>) ORDER BY import_default DESC, id ASC LIMIT 1`` - so "the marked folder wins,
else the earliest added" is one read rather than two rules that can disagree.  The kind-to-type sets
(``music`` -> ``music, all``; ``photos`` -> ``photos, all``; ``video`` -> ``movies, tv, anime, all``)
restate ``LibraryScanner``'s root rule deliberately, and they have to agree: a file written into a
folder no walk visits is one the library owns, that occupies disk and that can never be browsed.
``setImportDefault(id)`` clears only the rows that compete for *that* kind - same type, plus any
folder typed ``all``, which competes for everything - so a photo default and a music default
coexist and choosing one never moves the other.  ``reloadFolders()`` then reports ``importDefault``
as "this row is what ``importRoot()`` returns for a kind my type serves" instead of echoing the
stored column, which is what keeps the status honest after the marked folder is removed and the rule
falls back on its own.

**One pull, measured, into the folder that is the default because it was added first.**  Session
``zuuned-20261024T021257990Z-109328-d7b12c2a-0000.log``: the player answered 168 tracks and 5
photos, the probe printed ``download-into E:/Temp/testmusic/测试音乐``, ``[worker] extracting track
16777453`` named ``.../Anri/Bi・Ki・Ni/Good Bye Boogie Dance.mp3`` at 02:13:10.749 and
``download-saved name='Good Bye Boogie Dance.mp3' ok=true`` at 02:13:13.459 - **2.71 s for the
34,379,623 bytes** that are now on disk there, roughly 12 MB/s.  Five seconds later
``folder change detected - incremental rescan`` ran and ``verified music copies`` went from
``25 source rows`` to ``26``; the wall stayed at 15 displayed songs because the pull is a second
copy of a song the library already displayed, which is the identity dedup answering and not a row
lost.  That the destination is a Chinese path is a fact about this library and no longer a hazard -
see the two faults below.  **One number in that log line does not mean what it looks like**:
``readbackDurationMs=272093`` is the *track's audio duration*, 4 min 32 s, printed from
``row.durationMs`` in ``qml/Main.qml``; the same field carried ``8000`` for the eight-second sine
fixture, which is what proves it.  Quote durations as durations.

**A folder that could not count itself, or let go.**  ``LibraryDb::purgeFolder()`` and
``countsUnder()`` bound a path prefix inside ``substr()`` with a width taken from
``QByteArray::size()``, and SQLite indexes TEXT by **character**.  A Han character is three UTF-8
bytes, so for a Chinese folder the window came out wider than the string, ``substr(filepath,1,plen)``
returned the prefix plus six more characters, could never equal it, and every "beneath this folder"
test failed.  Settings read ``0 photos`` beside a folder holding five, and releasing that folder
logged ``purged 0 items`` while keeping every row it owned.  Both now take the width from
``QString::size()``.  An ASCII path has the same count either way, which is how this survived every
gate ever written for it; and the rule is SQLite's, not Windows', so the whole account is in
``WINDOWS_COMPAT_AUDIT.md``, thirteenth sweep.  ``librarytool folders`` is 20 checks and is the
first place either half was ever pinned.

**Two ways a folder's declared type used to be silently wrong.**  ``isSkippedVideoFile()`` applied
its 50 MB floor to every type that is not ``tv`` or ``anime``, so a folder typed ``all`` - the
answer when one mixed folder is pointed at the app - discarded every clip under 50 MB; the floor is
``movies``-only now and every other type keeps the 5 MB one.  And the walk hands every file under a
root to the visitor and then discards the kinds that root was not typed for, which is why a mixed
folder typed ``music`` produced an empty Videos page, an empty Photos page, and no word about
either.  ``LibraryScanner`` now keeps one counter per excluded kind inside the walk that is already
running and reports once per root - ``mediaTypeNotes`` to Settings -> Device as warning rows naming
the setting to change, and one log line reading ``[scan] <root> holds N video file(s) that the type
"x" will not index - Settings > Library > treat as "all"``.  Coded, compiled, and **not sighted**:
the last twelve session logs on this machine contain no such line, because every root here is typed
to match what is in it.  It had no capture when this pass closed, and the capture it has now comes from the
next pass rather than from a mistyped folder - because the floor sitting beside this
census was still eating a real download, quietly.

**Two fixture faults found by writing the gate, not by running the app.**  The import-root checks
first shared the database the counting checks had already filled, where the Chinese-named folder was
the earliest music root and so answered every ordering question the import root was being tested on;
it gets its own ``roots.db`` now.  And rewinding the legacy ``customize`` fixture for v10 the obvious
way - one more ``DROP COLUMN`` in the list that rewinds v6 - fails as ``duplicate column name:
discnumber``, because that column is in the ``CREATE TABLE`` the fixture writes *and* the subject of
migration 6, so an era must be dropped in full.  Both read as ``LibraryDb`` being broken and neither
was.

**Gates, and a harness trap worth a paragraph.**  On the relinked build ``tests\win-compat`` answers
``201 checks, 0 failures, 0 skips`` and ``tests\win-usability``
``launches=18 checks=129 failures=0 skips=0 killed=0``, ``OVERALL=ok``; ``librarytool folders``
answers 20 inside the ``tools`` phase beside ``names``, ``paths``, ``vanish``, ``cover`` and
``customize``, whose label now reads "v0/v2 to v10 migration chain".  The six checks above the
nineteenth pass's 123 are ``vanish``, ``folders``, the ``wfDefaults`` assertion, two "ships off"
source invariants and the ``clearSearch()`` invariant.  ``wfDefaults`` matters enough to assert
through a live launch because the flag is derived per kind in C++ and turned into either a status or
a button in QML: the seed library holds two music roots and nobody marked either, so exactly one row
may claim to be the default.  The first run of this pass reported ``launches=3 checks=91
failures=25 OVERALL=fail`` with every failure at ``-1073741515`` (``0xC0000135``, DLL not found) -
``zuuned.exe`` starts from the Qt copy beside it, ``librarytool.exe``, ``devicetool.exe`` and
``playertool.exe`` need ``D:\dev\Qt\6.11.0\msvc2022_64\bin`` on ``PATH``, and it was not.  A gate
that cannot start its own tools is not an application verdict.

No device claim changes with this pass: still the one hand-tested Pavo Zune HD, still nothing on it
heard, and the artwork it does supply to the application is a read.

## The twenty-first pass: a download that arrived and stayed invisible (2026-10-24)

**The twentieth pass shipped a fix that still ate the user's file.**  At 02:43:13 a right-click
``Save to Library`` on a device video logged ``[worker] extracting video 33554718 ->
E:/Temp/testmusic/测试视频/测试视频.mp4`` and put 20,575,375 bytes on disk; the folder watcher fired
six seconds later; and the Videos page went on showing the single 76 MB clip it had shown before, with
nothing in any log explaining where 20 MB had gone.  The explanation was this pass's own rule:
``isSkippedVideoFile()``'s 50 MB floor had just been narrowed to ``movies``-only, and this folder *is*
typed ``movies``, which is the honest thing to call a folder of video.  Zune video is small by nature -
the pull was 20.6 MB and a device-sized transcode is smaller still - so a floor written to drop trailer
debris in a movie collection was refusing the application's own purpose.  A file the user asked for by
right-clicking is not a guess about what a folder might hold.

**A refusal answers with a reason now, and says itself out loud.**  ``VideoNaming::videoSkipReason()``
returns ``none`` / ``bonusName`` / ``sizeFloor`` where the old function returned a bool, because a scan
cannot report what it never distinguished, and ``isSkippedVideoFile()`` stays as the yes/no form of it.
The floor does not apply beneath ``LibraryDb::importRoot("video")`` - the very folder downloads land in -
while ``trailer.mp4`` is refused wherever it sits and every other ``movies`` folder keeps its 50 MB
screen for the debris the floor was written against.  What the floor does refuse is counted per root and
named: ``[scan] <root> holds N video file(s) under the size floor - Settings > Device > set as the
download folder, or lower videoMinSizeMB``, with the same count on the ``mediaTypeNotes`` row Settings ->
Device already renders.  ``videoMinSizeMB`` remains an ini key with no control anywhere, which is exactly
why a refusal may no longer be swallowed.

**The census forced one ordering fact.**  A refusal is counted *after* the unchanged-mtime fast-path.
Counted before it, a folder whose video is plainly on the Videos page starts complaining the moment the
download mark moves elsewhere - a warning that tells the user to repair something already working, which
is worse than the silence it replaced.

**The diagnostic has a capture, and it is a produced one.**  ``librarytool videofloor`` is 19 checks:
seven on the rule itself (a 20 MB file refused in ``movies`` and kept as a download and kept in a folder
typed ``all``; the 5 MB ``tv`` floor; ``videoMinSizeMB`` overriding the type; ``trailer.mp4`` at 900 MB
still refused even as a download; a size nobody read being no reason to refuse) and twelve through one
real ``LibraryScanner`` pass over two real folders holding two real 2 KB mp4s.  The marked folder indexes
its clip, the unmarked folder does not and is named exactly once with a size complaint rather than a
wrong-type one, and moving the mark indexes the other file *and* retires the first complaint instead of
stacking a second.  The wrong-kind census of the twentieth pass still has no live capture on this
machine - every root here is typed to match what it holds - but the signal carrying it does now.

**And the file came back.**  On the relaunched build the launch sweep of 03:04:35.562 took in the file
that had been sitting unseen since 02:43:14: ``videos`` went from one row to two, ``测试视频.mp4``
carries a probed duration of 60,325 ms, and its folder logs no size-floor line because the floor is no
longer allowed to answer for it.  Nobody has watched it.  A row and a duration are a read.

**Gates.**  ``tests\win-compat`` answers ``201 checks, 0 failures, 0 skips`` and ``tests\win-usability``
``launches=18 checks=130 failures=0 skips=0 killed=0``, ``OVERALL=ok``, against a build relinked with
``--clean-first`` after the ``VideoNaming.h`` edit; the single check above the twentieth pass's 129 is
``videofloor``, wired into the ``tools`` phase beside ``folders``.  No device claim changes with this
pass: still the one hand-tested Pavo Zune HD, still nothing heard or seen on its screen.

## The twenty-second pass: three destinations, and a library that is one place (2026-10-24)

**The complaint was about the library, not about the folder list.**  "The music library has to be
consistent": a song that arrives - imported, or pulled off a player - belongs in the library the user
browses, which means music goes into the music folder; and when several folders hold the same kind,
exactly one of them has to be the one that receives, shown as a status on the row that is and as a
button beside *type* and *remove* on the rows that are not, with the earliest added folder holding the
title until somebody clicks.  The twentieth pass shipped a mark for that.  The mark was wrong in a way
a single folder can never show.

**One flag per row cannot name three destinations.**  ``watch_folders.import_default`` was one bit on
one row, and a row typed ``all`` qualifies for music, photos *and* video, so marking a picture folder
cleared the mark on the folder typed ``all`` - and with it the music destination that folder had been
carrying.  Choosing where songs go moved where pictures go.  The fallback made it worse in the other
direction: ``ORDER BY import_default DESC, id ASC`` let a root that merely promised to hold everything
win a kind for which a folder had actually been declared, so a library with a music folder could still
import songs somewhere else.  And the status was derived by comparing *paths*, so two rows spelling one
folder at different lengths both lit up.

**The mark became a table keyed by kind.**  Schema v11 adds ``import_defaults (kind PRIMARY KEY,
folder_id)`` - one row per destination, which is how many destinations a library has - and leaves
``watch_folders.import_default`` alone as the v10 column it is, now unread.  The migration copies
*only* the marks a user really made, one row per kind from ``... WHERE import_default=1`` in id order:
seeding the other kinds would freeze today's fallback into a choice nobody made, and the row would then
wear a default the user never picked.  The legacy probe answers 10 when the old column exists and 11
when the new table does, so a library that has been upgraded is never migrated a second time.

**Two rules, in the order the user gave them.**  ``LibraryDb::importWinnerId()`` names the row marked
for *that* kind - unless its type has since stopped serving the kind, because a mark cannot promise a
kind no walk would read out of that folder - and otherwise the first folder added that serves it, under
``ORDER BY CASE WHEN type='all' THEN 1 ELSE 0 END, id ASC``: a folder declared for a kind beats a
generic root added years before it.  ``setImportDefault()`` upserts one row for every kind the chosen
type serves and for no others, so a music folder takes music alone while a folder typed ``all`` takes
all three at once - which is what clicking *default* on a folder that promised to hold everything says,
out loud, at the moment it is clicked.  ``removeWatchFolder()`` prunes the marks of a folder that is
gone, because a rule whose dead rows are invisible is a rule nobody can read.

**The kind-to-type rule is written once.**  ``importKindsForType()`` and ``watchTypesForKind()`` in
``src/library/LibraryTypes.h`` are that rule read forwards and backwards; ``LibraryScanner::scan()``'s
three root sets are the third copy, and they have to agree, because a file written into a folder no
walk visits is one the library owns, occupies disk and can never browse.  The two SQL sites that had
spelled the sets out by hand now ask the header.

**Status is derived, never read off a column.**  ``LibraryService::reloadFolders()`` asks
``importDefaults()`` for a row *id* per kind rather than matching paths - two rows can name one folder,
and a folder typed ``all`` may receive some kinds and not others - and paints ``default`` when a row
receives every kind its type serves, ``default · video`` when it receives some, and nothing at all when
it receives none, which is what turns the button on.  The live profile reads ``user_version=11`` with
``import_defaults`` empty: four folders, nobody has clicked, and each row shows the button while the
fallback answers `E:/Temp/testmusic/测试音乐` for songs, `测试图片` for pictures and `测试视频`
for video.

**The label lied about a row that receives nothing.**  It emitted ``"default · "`` - a truthy string
with an empty tail - so ``defaultLabel || "default"`` could not tell *no status* from *blank status*, and
a gate could not count the absence of one.  A row that receives nothing now returns an empty string.

**A folder typed "all" was censused for the files it had already indexed.**  Every walk counts the files
of a kind its root excludes and says the count out loud, because a folder that shows no videos is
otherwise indistinguishable from an empty one.  The video and photo walks ask first whether the root is
typed ``all``, which sits in all three root sets and therefore excludes nothing; the music walk did not
ask, so it counted the videos and pictures inside an ``all`` root as skipped and Settings offered to set
that folder's type to the type it already had.  The music walk carries the same guard now.

**Both halves have a capture, and the upgrade has a rewind.**  ``librarytool census`` is 10 checks over
one real ``LibraryScanner`` pass on a real tree holding a music, a photos, a movies and an ``all``
folder: the clip is named once, under the folder that really does exclude it; the ``all`` root says
nothing; and the clip and the picture inside it are indexed, not merely counted.  ``librarytool
folders`` grew from 20 checks to 35 and now rewinds a database to the exact shape the previous build
wrote - marked column present, ``import_defaults`` absent, ``user_version`` 10 - and reads the upgrade:
the folder a v10 library marked keeps the kind it had been given, the kinds v10 never marked keep
falling back, and the result is v11 again with no mark invented.  The rewind drops ``import_defaults``
alongside the v6 columns and the v9 fingerprint, because leaving it behind would let a fixture labelled
"v2" answer the v11 probe - and a fixture's whole claim is that it holds nothing newer than its label.

**Gates.**  ``tests\win-compat`` answers ``201 checks, 0 failures, 0 skips`` and ``tests\win-usability``
``launches=18 checks=132 failures=0 skips=0 killed=0``, ``OVERALL=ok``, against a build relinked after a
full rebuild following the ``LibraryTypes.h`` edit; the two checks above the twenty-first pass's 130 are
``census`` in the ``tools`` phase and *only a default row carries a default status string* in the
``pages`` phase, driven by the ``wfDefaultLabels``/``wfDefaultAgree`` counters added to the
``--qa-probe`` report.  The app runs on that executable.  No transfer was attempted for this pass and
no device was asked anything: device evidence is unchanged at the one hand-tested Pavo Zune HD, and
nothing has been heard or seen on its screen.

## The twenty-third pass: the wording leaves the source, and a tab too wide to show what it knew (2026-10-25)

**No Chinese is typed into the interface.**  ``i18n/zh_CN.tsv`` is the only place a sentence the
user reads is written, ``src/UiText.h`` and a ``python i18n/audit.py`` gate keep it that way, and
the catalogue stands at 1493 translatable sources, 1493 of them answered, ``lrelease`` reporting
0 unfinished.  A line is either ``source ~=> Chinese`` or ``Context!source ~=> Chinese``; the
prefix counts only when it names a ``<context>`` that exists, and beats a bare line inside that
context.  All 29 scoped lines are pivot bars, because ``Albums`` is 专辑 next to Artists and
相册 next to photo Folders, and that one source deliberately carries three scoped lines and no
bare line, so a fourth appearance in a new file reports ``MISSING: NewFile!Albums`` instead of
quietly borrowing the wrong word.  Identity bytes stay untranslated: ``pivots.tabs``, the
``--settings-tab Device`` argument, ``picker.mode = current.toLowerCase()`` and
``page.filter === "all"`` compare the same bytes they always did and only the painted label
changes.

**One long sentence widened five tabs and hid a card that was working.**  ``Settings -> Device``
showed the six labels of the 这台 Zune card and none of the six values, which reads exactly like
a feature nobody wired; ``DeviceService`` had been answering the whole time.  The tab was
**2263 logical pixels wide**: ``StackLayout`` sizes every child to the widest child's
``implicitWidth`` and lets it overflow its own bounds, so the longest row in any one of the five
tabs widened all five and a right-aligned value was painted at ``mapx ~ 2200``, outside a window
no screenshot of which can be that tall.  A headless ``qml.exe`` repro settled it - a Layout child
cannot opt out, ``width: stack.width`` is clobbered and ``implicitWidth: 300`` is ignored, while
``anchors.left/right/top`` inside a plain ``Item`` take that Item's width and are immune to
content.  ``qml/SettingsPage.qml`` now holds the five tab bodies in a plain ``Item`` selected by
``visible``, and ``PhaseNote`` wraps and fills instead of asking for its own width, which is what
let a single note widen a tab at all.

**The gate that certifies it inherited its environment from whoever ran it.**  ``run.ps1`` named
Qt and ffmpeg on ``PATH`` only for ``Start-Zuuned`` and the single-instance rival, so the tool
checks - ``librarytool``, ``transcodetool``, ``devicetool``, ``playertool`` - inherited the
caller's shell.  From a baseline shell that names neither, every one of those children died at
``0xC0000135`` (``DLL_NOT_FOUND``) before printing a line: 27 failures, no seed library, and the
page, five-settings-tab, compact and spoken cases reporting ``SKIP: no seed library`` against an
application that was fine.  One line now prepends both directories for the whole run, because a
gate must not pass or fail on its caller's shell; ``tests\win-compat`` loads no Qt and needed no
change.

**Gates.**  The same tree before and after that line: ``launches=3 checks=93 failures=27
skips=6``, then ``launches=18 checks=132 failures=0 skips=0 killed=0`` and ``OVERALL=ok`` - the
39-check difference is six cases that had been skipping and now run - with ``tests\win-compat``
at ``201 checks, 0 failures, 0 skips`` and ``python i18n\audit.py`` at ``AUDIT=ok``.  The
counters end where the twenty-second pass left them, because nothing new was added to what the
harness asserts.

**What this pass did not do.**  The Zune HD stayed attached and the app\'s own hotplug browsed it
inside every isolated profile; each recorded diagnostic is a read.  No send, deletion, eject or
audio playback was triggered, so device evidence is unchanged at the one hand-tested Pavo Zune
HD.  Two defects are recorded and left alone: the 转码加速 note still offers NVDEC, then VAAPI,
then software, and VAAPI means nothing on Windows; and no capture shows a context menu in
Chinese, because a Qt menu is its own top-level window and this app\'s DWM blur paints white-ring
artifacts under ``CopyFromScreen``, which would make such a screenshot inconclusive rather than
evidence.

## The twenty-fourth pass: two cards that have to answer for themselves (2026-10-25)

Two complaints about `Settings -> Device`, both honest, and both the same weakness: a card that
reports something the user cannot check and offers no knob.  "这台Zune没有信息" -
the This Zune card was gated on `DeviceService.connected`, so on every screen the user actually
opened Settings from it was not blank, it was absent, and absent is exactly what an unwired
feature looks like.  "音频转换真的可用吗？你既没有lame设置，也没ffmpeg设置，实用不便"
- the Audio conversion card named the linked encoders and then asked to be believed.

**The identity card outlives the cable.**  `DeviceHistory::latest()` (`src/DeviceHistory.cpp:139`)
hands back the newest row of the history in the same map shape the history list uses, so the card
and the list under it speak one vocabulary.  A live player still wins, and the two rows only a
live read can answer - Contents (`%1 tracks · %2 albums · %3 artists`) and Battery -
exist only while connected.  With the cable out the identity rows fall back to what the last
session read and the subtitle says so, with the date, instead of showing a number that is a
minute into a lie: storage becomes `%1 GB total, read on the last session`, and per-category
bytes become "read while connected" rather than an old reading.  The `PhaseNote` stays for the
case it was written for - nothing connected *and* nothing ever remembered.  Still no firmware
row, because `libzune/include/zune.h` offers `zune_get_serial` and no firmware getter.

**The knob LAME has and this app never passed.**  `mp3Quality` is one ini key behind a Detail
0/2/4/6 pill that hides itself when the target is WMA, because `wmav2` takes a bit rate and
nothing else.  It is read with the format and the bit rate, once per run, in
`readAudioSyncPolicy()` (`src/sync/SyncEngine.cpp:292`) so a queue cannot change the effort
halfway through an album, and the layer that owns the encoder clamps again in
`zuuned_mp3_set_quality()` (`transcode/libav_transcode.c:81`) because both ends are reachable on
their own.  Both `lame_set_quality()` sites honour it - the album encode at `:255` and the
chapter-range encode at `:773`, which is the path an audiobook part takes.  Default 2 is what the
code always passed, so the control changes nothing until somebody moves it.  The card also names
the build it is talking about now: `mp3EncoderVersion` is `get_lame_version()` read out of the
linked library rather than a string this repository believes.

**A folder instead of a lecture about PATH.**  `ffmpegDir` names a directory and `src/main.cpp:254`
puts it in front of `PATH` for the process, once, before any worker starts.  A folder rather than
two file pickers because a Windows FFmpeg download *is* one `bin` directory, and because the one
shell-out this application cannot hand a path to - `zune_grab_photo_thumb()`, through libzune,
running a bare `ffmpeg` through `cmd.exe` - can only be answered through `PATH`.
`conversionTools()` reports where each program was found (`folder` / `PATH` / `none`) and whether
the named folder is a folder at all, so the card says "found on folder" instead of the
unactionable "found on PATH", beside `choose folder`, `check again`, `clear` and a download link.
Measured on an isolated profile with a `PATH` carrying no FFmpeg at all: with
`ffmpegDir=D:\\dev\\zuuned-deps\\...\\bin` the launch logs
`[ffmpeg] Settings folder "..." put in front of PATH; ffmpeg resolves to
"D:/dev/.../bin/ffmpeg.exe"`, and the forward-slash spelling answers the same; a folder that does
not exist logs `is not a folder; falling back to PATH` and changes nothing.  That probe also
turned up a fact about the file itself: **`QSettings`' INI reader unescapes backslashes**, so a
path typed into `Zuuned.ini` by hand as `D:\dev\x` reaches the program as `D:evx` and is
reported as not a folder.  Qt writes the doubled form itself, and the picker writes forward
slashes, so only somebody editing the ini by hand meets it - which is worth knowing the next time
a settings value "does not stick".

**"Is it really usable" is answered by doing it.**  `DiagnosticsService::runAudioSelfTest()`
(`src/diagnostics/DiagnosticsService.cpp:271`) writes a two-second 440 Hz tone, sends it through
`zuuned_transcode_audio_ex()` at the stored bit rate and detail - the same entry point a sync uses -
reads the first four bytes back to check the container they claim (an ID3 tag or a bare MPEG sync
for MP3, the ASF GUID for WMA), probes the result for a length inside 85-115 % of the tone and a
size above half the nominal constant-bit-rate payload, and deletes everything it made.  The answer
is codes (`no-encoder`, `refused`, `cancelled`, `container`, `unprobeable`, `duration`, `size`,
`ok`) and the sentence belongs to QML, so a self-test reads in the language the user is reading.
It refuses while a transfer or a device job is running, because one transcode at a time is a
property of the encoder layer rather than a preference, and the abort flag is read, cleared for
the measurement and put back so a cancellation the user asked for is never eaten by a test.  The
legs it runs are the legs `transcodetool` already gates, and that phase passed on this build in
the run below (`the audio encoder produced an mp3 and named it`, `wrote an ID3 tag or a frame
sync header`, `the encoded audio is as long as the tone it came from`, `profile 0 wrote an ASF
container`); what is new is that the running application can be asked the question without a
harness.  A Windows build asks it three times now, because this build reaches a third encoder -
see the lossless section at the end of this page - and the lossless leg is judged on container
and length alone, because a lossless file has no bit rate to be measured against and a tone is
worth very little to one.

**One defect the twenty-third pass recorded and deliberately left is closed.**  The AMD pill is
`disabled: Qt.platform.os === "windows"` and the 转码加速 note is branched by platform, so the
page no longer offers VAAPI to somebody who cannot use it.  Software and NVDEC stay selectable
because NVDEC was measured engaging here.

**Gates.**  `tests\win-compat` `201 checks, 0 failures, 0 skips`; `tests\win-usability`
`launches=18 checks=132 failures=0 skips=0 killed=0` and `OVERALL=ok`; `python i18n\audit.py`
`AUDIT=ok` with the catalogue at 1524 sources, 1524 with Chinese, `lrelease` 0 unfinished.  Both
counters end exactly where the twenty-third pass left them because nothing was added to what
either harness asserts, and both ran against the relinked tree.  One harness lesson carried out of
the relink: `cmake --build build --clean-first` printed `FAILED: clean` at the head of the log -
the running application held `build\zuuned.exe` - continued anyway, rebuilt all 336 targets and
exited `0`.  What proved the artefact was new was the executable's timestamp, not the exit code.

**What this pass did not do.**  No send, deletion, eject or audio playback was triggered; the
self-test touches libav and the temp directory and opens no device handle; the attached Zune HD
was browsed by the application's own hotplug and every recorded diagnostic is a read, so device
evidence is unchanged at the one hand-tested Pavo Zune HD.  Not proven by anything here: that what
the self-test accepts is a file a player will play - it judges container magic, length and size,
not sound, and nobody has listened; the new rows in Chinese, for which there is no capture yet;
and there is no gate case for the self-test button itself, so today the only record of it is the
`transcodetool` legs above plus a hand press.

## What is not proven here

1. Everything past audio.  Deletion and purge, the firmware categories and
   firmware resume, podcast and audiobook transfer, and every model but this Pavo
   Zune HD - each a real gate rather than a ported assumption.  Photos and video
   are half a gate now, and only half: the seventeenth pass sent five of one and
   one of the other and the player re-listed both, so what is missing is the read -
   no photo byte, no video byte, nothing played.  Of
   the two responses the first reads left open, `0x200F` on `GetObject` is
   explained: the same save that added MTPZ credentials made that write succeed,
   so it was an unauthenticated save and not a refusal.  One is still unanswered,
   `0x2002` on `GetObjectInfo(0x07000002)`, three times per scan.  The reset claim that
   sat behind the wedge is corrected in the seventh pass and measured in
   `libzune/docs/WINUSB_QUIRKS.md`: a reset on WinUSB reports success without reviving
   anything, which is a different failure than the one this document used to describe.
2. Audio anyone can hear.  The gate opens WASAPI for real and asserts mpv reports it
   through `current-ao`, but always muted, so samples never reach the speaker.
   Audible playback, exclusive mode, a hot-plugged or Bluetooth endpoint, and anything
   a human listened to remain unverified.
3. What hardware decode is worth.  The gate proves NVDEC engages on this machine and
   that the downloaded frames survive the scaler and the muxer, never that decoding on
   the GPU is faster than the software path here - the ~4x figure in the roadmap was
   measured on Linux.  A machine without an NVIDIA decoder legitimately takes the
   software path, and the gate asserts the software run only when it forces one.
4. `spokenmediatool` builds and both legs were driven by hand on 2026-10-20 - an
   RSS feed with a Chinese item title, and the audiobook probe the descriptor fix
   above unblocked - but the tool still has no phase in the usability gate.  Nor does
   any phase play a podcast or an audiobook, so the bounded spoken close is gated in
   shape only: the resume point inside its deadline has never been written by a run.
5. Packaging.  No installer, no `cmake --install` layout, no verified
   `windeployqt` directory and no release artifact.
6. The Linux line is unchanged by construction rather than re-verified: this
   machine is the Windows checkout, Windows changes are platform-neutral or
   inside `Q_OS_WIN`, and the bash gates were not re-run from here.
7. Cosmetic and recorded: the white ring in the `qml/EnvironmentBackground.qml`
   spray halo under the Windows blur path.
8. Inside Qt and recorded rather than asserted: with a profile folder whose name
   the local ANSI code page cannot spell, Qt's own shader/pipeline cache cannot
   open its file even though the directory exists, so such a user pays a cold
   shader cache on every start and loses no data.
9. Recorded, not asserted: a brand new profile legitimately shows onboarding
   rather than the requested page.  The other half of this entry is gone - a GUI
   launch reaching musicbrainz.org for artwork was the last unasserted network
   claim, and online lookup became opt-in with two assertions in the `pages`
   phase instead - the eighteenth pass has the before-and-after log lines.
   Native path display, which was the last entry in this list, is gated now - see
   the `wf*` bullet above.  Where it reaches without a gate is narrower than it
   was: the labels at `qml/OnboardingView.qml:1822`,
   `qml/SpokenFolderImport.qml:61` and `src/LibraryService.cpp:2268` share
   `PathText::display()` with the Settings row the gate does pin, and the repair
   recorded in `docs/WINDOWS_COMPAT_AUDIT.md` is a stored-path concern that the
   `watch_folder_repair` phase already covers.
10. The second compatibility sweep in `docs/WINDOWS_COMPAT_AUDIT.md` - device titles
    that cannot be a Windows filename, the `cmd.exe` photo-thumbnail command line,
    m3u entries written in another dialect.  All three compile clean under MSVC and
    none of them has a gate or a hand test: no album with a colon in its name has
    been saved to the library, no device photo thumbnail has been produced through
    the fallback, and no foreign-authored playlist has been imported.  The library
    naming path in particular is a *change* to what a save-to-library file is called
    on Linux not at all and on Windows only for names that could not be created
    before, so it is safe in the ordinary case and unverified in the one case it
    exists for.  A clean rebuild has since happened and the app has been restarted
    from it, so those rules are in the running binary; what they still do not have
    is the hand test.  The fifth pass put software gates behind two of the three -
    `safeFilePart:` and the `decode:` checks, the latter extended to UTF-16 with a BOM - and
    the photo-thumbnail fallback still has neither gate nor hand test.  The eighth pass put
    both photo shell-outs right and gave the code-page rule a gate of its own, and neither
    one has met a player.  The seventeenth pass closed the first half of that on real
    hardware - five pictures prepared at 480px on this machine and sent, one album, no
    failures - and left the second half exactly as it was: no device thumbnail has ever
    come back through the `zune_grab_photo_thumb()`
11. The download half on a binary that carries the photo, shell and diagnostics
    fixes.  The ninth pass re-proved the send and had the firmware count it, then
    wedged before the pull could run, so the newest QA track is proven only as a
    device-side library count: never read back as bytes, never compared against the
    fixture, never heard.
12. Recorded, not asserted: Qt5Compat's `QGfxShaderBuilder` cache is a third Qt-internal
    location that `ZUUNED_PROFILE_DIR` does not own, so an instrumented launch that has to
    compile a new `FastBlur` variant writes it into the real profile.  It costs a warm
    shader cache and no data, and the profile pair-check in the usability gate is the
    thing that sees it - the eleventh pass has the file name and the measurement.
13. Where a pull lands moved the same day it was last proven by hand.  The twentieth pass put device
    tracks into `Music\Zuuned Imports`; the twenty-second pass made a folder *declared* for a kind
    outrank a generic root, so `music` now answers the declared music folder and `Zuuned Imports` is
    an ordinary row wearing a button.  The rule is pinned in software and its label is pinned on
    screen, but no player has written into the new destination: not one byte has been pulled since it
    changed, so "downloads land in the right folder" is a database fact and not a device fact.

## Order to close it in

1. Play back on the player what the round trip above put on it.  Both files are
   there, in both directions, and nobody has heard either, so "it transfers"
   still does not mean "it plays".
2. Repeat the remaining Linux hardware gates in the order Linux used, from here,
   each with its own evidence and none inherited: library read (done 2026-10-20),
   one track send with byte readback (done 2026-10-21), then one deletion, then
   the podcast and audiobook categories and firmware resume, then the read half of
   photos and video (both sent and re-listed 2026-10-23, neither read back), then a
   Classic, a Draco and an 80/120.
3. Answer `0x2002` on `GetObjectInfo(0x07000002)` in `libzune/docs/` before it is
   assumed benign or changed.  `0x200F` needs nothing further - see above.
4. Add the remaining software gates: audible (unmuted) playback, a
   `spokenmediatool` phase, and a launch that plays a spoken item so the resume write
   behind the bounded spoken close is exercised.  Measure NVDEC against software on a
   real 1080p item before quoting a speed-up for Windows.
5. One picture back and one spoken item across the wire.  The two photo shell-outs the
   eighth pass fixed are no longer software-only claims - the seventeenth pass sent five
   pictures through them - but the `zune_grab_photo_thumb()` fallback still has never
   returned a thumbnail from a player, which is the half of it a read would close.  The
   spoken import walk this item carried is routed through `LibraryScan::walkFiles()` as
   of the fifteenth pass, so what is left there is the wire, and the import itself still
   has no phase, gated or hand.  And hear one WMA Lossless track on the HD: one has been put
   on the HD and taken off again bit-identical (2026-10-29, below), so the encode and the wire
   are both done and only the listening is left, on a format documented for that model alone.
6. Only then talk about Windows packaging or a Windows release channel.

## 2026-10-26 - WMA Lossless, from the encoder that is already in Windows

The Audio conversion card had been answering "can it do lossless?" with a refusal, and the
refusal was true of the two doors it had looked at: the linked FFmpeg carries `wmav1`/`wmav2`
and no `wmalossless`, and System32 has no WMA Lossless ACM codec.  The third door is not a fork.
`wmvcore.dll`, which is on every Windows install, carries **Windows Media Audio 9.2 Lossless** as
an ordinary in-process codec, and `transcode/wma_lossless_enc.cpp` drives it through
`IWMCodecInfo3` -> `IWMStreamConfig` -> `IWMProfile` -> `IWMWriter` behind the same
`zuuned_transcode_audio_ex()` a sync already uses.  One translation unit, Windows-only
(`transcode/wma_lossless_stub.c` answers "not available" everywhere else), linked against import
libraries from the Windows SDK (`wmvcore`, `wmcodecdspuuid`, `ole32`).  No codec is bundled,
vendored or forked, which is the shape this client is supposed to keep.

Four traps measured while finding the sequence, all recorded in the file header because each one
cost a build:

- The lossless formats are **invisible until asked for**.  `GetCodecFormatCount()` answers 0 for
  the lossless codec until `SetCodecEnumerationSetting()` has received `g_wszVBREnabled` TRUE and
  `g_wszNumPasses` 1, per codec - and they have to be re-applied before the final
  `GetCodecFormat()` or the format that comes back is not the format that was found.
- `GetMediaType()` returns one buffer and `pbFormat` points *inside* it; `free(pbFormat)` is
  heap corruption (`0xc0000409`), not a leak.
- `IWMWriter::GetInputFormat()` takes the input number *and* the index, and
  `GetCodecInfoCount()` takes its subtype GUID by value.
- `ksmedia.h` stays out: it collides with `wmcodecdsp.h`.

The codec's own table (WMFSDK 12.0.19041 here, four audio codecs in total - WMA Voice 9 refuses
the enumeration settings with `0xC00D0BCD`) is the whole specification of the feature:

    2ch/44100/16   2ch/44100/24   2ch/48000/24   2ch/88200/24   2ch/96000/24
    6ch/48000/24   6ch/88200/24   6ch/96000/24

No mono, nothing above 96 kHz or 24-bit, and no 16-bit format above 44.1 kHz.  The policy that
follows is deliberately unhelpful: **the encoder never changes a sample rate, a channel count or
a sample value.**  A 48 kHz/16-bit stereo file is handed over in 24-bit containers because the
table has no 16-bit entry that high and widening is a left shift; a mono file, a 5.1 file at
44.1 kHz, or anything at 192 kHz is refused with a sentence of its own in the queue rather than
delivered quietly altered.  Lossless that silently stopped being lossless would be worse than no
lossless.

Measured on this machine against the relinked binary, all of it software and none of it a player:
`transcodetool encoders` answers `wma-lossless=1`; a 30 s slice of a real master at 44.1/16/
stereo, a 48 kHz/16-bit stereo tone, a 44.1 kHz/24-bit stereo tone, and the first of those under
a Chinese file name each encoded, each probed as `wmalossless`, and each decoded back to
**bit-identical PCM** with equal sample counts and zero delay.  The real master cost 71.2 % of its
PCM, 1,006 kbps.  A sine costs about 795 kbps, so a tone is the wrong fixture to size a lossless
queue with, and `estimateTrackBytes()` reserves 1,280 kbps for a lossless leg on the real
recording instead.  Settings' self-test runs the leg too, judged on container and length.

What is *not* claimed: only the Zune HD is documented to play WMA Lossless, and the models this
repository has actually held are documented neither way, so the card says plainly that the likely
failure is a track that stays silent rather than one that sounds worse.  The option is a third
choice on the Audio conversion card, `disabled` when `conversionTools().wmaLosslessEncoder` is
false, and it hides the bit-rate pill because the format has none.  **Nothing this build has
written has been heard on any Zune**, and that stays the sentence to quote until somebody plays
one.
## 2026-10-29 - The twenty-fifth pass: an outside audit of the audio line, and the gates that answer it

An audit document came into the repo root -
`Zuuned_Modern_代码审计及_WMA_Lossless_编码同步实施方案.md` - with ten findings against the
audio main line, F-01 to F-10, filed by severity and each one read out of the
source rather than guessed at. This pass takes them in order and records what
each became. No Zune was attached for any of it, so every number below is
software and none of it is wire evidence.

**F-01, the one that lost audio in silence.** `transcode/libav_transcode.c` fed
the decoder through `avcodec_send_packet()` and, on any negative return,
unref'd the packet and `continue`d. `AVERROR(EAGAIN)` does not mean "this
packet is unusable", it means "the decoder is full": the answer is to drain
frames and retry the same packet. As it was the packet went away and the output
kept going without it - no error, no log line, a file a few hundred
milliseconds shorter than its source. The branch drains and retries now, and
only a second real failure drops the packet. The finding worth keeping is the
shape: a decode loop that reads backpressure as rejection is silent, and a
slightly short file does not look like a bug to anybody.

**F-03, sending a file without owning it.** `mtp_send_object()` took a
`uint8_t *`, and its caller in `libzune/src/track.c` read the whole track into
RAM before a byte reached the wire - harmless for a 5 MB MP3, and a lossless
album side is not a 5 MB file. `ptp_transaction_send_file()` drives the same
transaction off an open `FILE *`: same transaction-id handling, same command
phase, same long response timeout a device needs to commit a large payload, and
a 1 MB `ptp_data_source` scratch instead of the whole payload.
`mtp_send_object_from_file_progress()` wraps it, and the buffered
`mtp_send_object()` is untouched so nothing that already worked changed shape.
Two refusals came with it: no path, and a zero-length object, because a 12-byte
container would leave the player waiting for a data phase that never says
anything - a size of 0 means the file vanished or was never what
`SendObjectInfo` promised. `libzune/tools/mtpframingtool.c` is the gate: both
paths run through the same fake transport and the bytes they put on the wire
are compared, which answers `framing 1245 checks 0 failures`. Byte-identical
framing is the entire claim - streaming that moved one container byte would be
a regression dressed as an optimisation. A file that ends before the promised
length is refused rather than padded, because the header already told the
device a number.

**F-06, scratch files that could collide.** The transcode scratch name was
source-name plus PID, which is unique per process and not per concurrent job
inside one process. Three sites take their uniqueness from `mkstemps()` now,
which creates with `O_EXCL` and therefore cannot be talked into overwriting
something it did not make.

**F-07, the tag rewriter.** It read the whole MP3 into memory and wrote UTF-8
text into ID3v2 frames flagged as ISO-8859-1, which is a file that lies about
its own encoding. `transcode/id3_rewrite.c` streams - a 256 KiB copy chunk over
`read_at()`/`write_all()`, no whole-file buffer - and
`normalize_text_frame_for_v23()` owns the codec question: text representable in
Latin-1 goes out as `[00]`, anything else takes UTF-16 with its BOM, and the
code-point walk is capped by `TEXT_CODE_POINTS_MAX` because a corrupt frame
header must not turn a rewrite into an allocation. `transcodetool retagcheck`
builds a fixture whose four frames declare three different encodings - a
Chinese album name under a UTF-8 flag, a Latin-1 title, a UTF-8 artist that
only claims ISO-8859-1, a UTF-16 genre - and judges 27 things, among them that
the 600 KB of fake audio after the tag comes back byte-exact. One lesson
deserves its own sentence: the audio-start offset a rewriter needs is the **end
of the tag**, not the first MPEG sync word it can find, because a UTF-16
byte-order mark is `FF FE` and `FF FE` is a legal MPEG sync word - a forward
scan for the start of audio stopped inside the tag instead.

**F-08, `volatile` is not a synchronisation primitive.** The abort and
hardware-decode flags cross a thread boundary: the UI sets them, the transcode
loop samples them. The declarations stay `volatile int` so the type reads the
same on both platforms, and every access goes through
`zuuned_flag_load()`/`zuuned_flag_store()` -
`_InterlockedCompareExchange`/`_InterlockedExchange` on MSVC,
`__atomic_load_n`/`__atomic_store_n` at `__ATOMIC_SEQ_CST` elsewhere. The
keyword keeps the compiler from parking a copy in a register; what it never did
was order anything, and now the ordering lives in one pair of functions instead
of nowhere.

**F-04 and F-05, the two findings about lying about a format.** A profile that
asked for lossless and received MP3 because this build has no encoder is F-04,
and it is the same class of defect as F-01: the thing the user asked for did
not happen and nothing said so. `readAudioSyncPolicy()` no longer substitutes
in silence; the queue gets a card that names the missing encoder and says, in
as many words, that nothing was queued and that the file was not turned into
MP3 instead. F-05 is its mirror image, and `transcodetool container` is the
answer: it reports the ASF codec GUID, the `WAVEFORMATEX` tag, whether the
stream is lossless, and what the native-copy policy decided. On this machine a
lossless file the user owns answers
`name=wmalossless tag=0x0163 lossless=1 copy_wma=0 copy_wma_lossless=0` and an
mp3 answers `copy_wma=2 copy_wma_lossless=2` - the "not even an ASF" arm of the
policy - so the copy decision is made off the codec rather than off a fourcc
that looked carryable.

**F-09 and F-10, sizing.** `zune_verify()` is what it always was, a warning
pass after the fact, and this pass does not pretend otherwise; what changed is
that the length is now enforced on the way out, where a wrong number costs a
wedge rather than a log line. `estimateTrackBytes()` takes the target bitrate
and whether the leg is lossless, and reserves 1,280 kbps for a lossless leg -
measured against a real master, see the 2026-10-26 section. When an encoder
still produces more than the reservation the track is not sent: the queue says
how many MB over it is and how many MB the Zune has left.

Two defects belonging to this pass are recorded because both were found in code
this pass had just written. `transcodetool container` with no argument
dereferenced a null path and died with `0xc0000005`; a gate binary that crashes
on a typo reports nothing about the thing it was asked to check, so every
command but the three that ask about the build now answers a missing file
argument with a usage error. And `i18n/audit.py` reported 34 findings that were
not findings: it matched `UiText::tr(` on one line only, so a string written as
adjacent literals across three lines had its two continuation lines flagged as
untranslated CJK. It tracks a continuation now and `cxx` is back to 0 findings
and `AUDIT=ok`. The loosening was regression-tested by handing the auditor a
bare `QStringLiteral` holding Chinese and a continuation whose head is not a
`tr()`; it still reports both.

**Gates.** Against the freshly relinked binaries: `tests\win-compat`
`201 checks, 0 failures, 0 skips`; `tests\win-usability`
`launches=18 checks=134 failures=0 skips=1 killed=0`, the one skip being the
two checks that name a specific device with no player attached to compare; the
tools phase alone, re-run on the last binary, `checks=47 failures=0 skips=0`;
`librarytool` `paths` 67 and `names`, `customize`, `vanish`, `folders`,
`videofloor`, `census`, `cover`, `net` each `ALL PASS (0 failures)`;
`transcodetool retagcheck` 27/0, `copycheck` 14/0, `mtpframingtool` 1245/0;
`python i18n\fill.py` 1535 translated and 0 missing, `i18n\build_qm.ps1` 1535
finished and 0 unfinished.

**Not claimed.** No Zune was opened during this pass. The streaming sender has
been compared against the buffered one on a fake transport and never against a
player; no WMA Lossless file this tree wrote has been played; deletion, the
podcast, audiobook, photo and video categories, every model but this Pavo Zune
HD, and anything anyone has heard all stay exactly where the 2026-10-26 section
left them.

## 2026-10-29 - WMA Lossless across the wire: one real recording in and back out bit-identical

The 2026-10-26 section ended on the encode and nothing else - "nothing this build has written
has been heard on any Zune". That encode has now been put on a player and taken off again.
Nobody has heard it, and that stays the sentence to quote.

Fixture, supplied for exactly this: the 3-CD FLAC box
`E:\Temp\testmusic\测试音乐\Wakin Chau (周華健) - 周而復始 1987-2002 (2003) {RD1668-4} [FLAC]`,
40 FLACs over `Disc 1/2/3`, each disc with its own `cover.jpg`. The leg ran on
`Disc 1\04 - 擺渡人的歌.flac`, 18,039,399 bytes, which `ffprobe` names as 16-bit stereo at
44.1 kHz - inside the eight formats the encoder in Windows publishes, so this is not the refusal
path.

Software first, before anything reached the player. `transcodetool audio-wma-lossless` took the
CJK path in 2 s and reported `wrote 7158312 samples as 2 ch / 44100 Hz / 16-bit lossless`, and
7,158,312 is 162.320 x 44100 to the sample. `ffprobe` calls the output `codec_name=wmalossless`,
`format_name=asf`, 18,452,689 bytes. Then the answer that matters: the source FLAC and the
produced WMA each decoded to `s16le` at **28,633,248 bytes** - equal sample counts, no priming
or padding difference at all - and both hashed
`E5C6FA3C1EF4A1D6B586E9573FB7B976231448BB72425AD23F54A8C4E86BC3D0`. This recording cost 909 kbps
against 1,411 kbps of PCM, so the 1,280 kbps reservation in `estimateTrackBytes()` still fits.

On the player, through the ordinary admission and no path a user cannot reach: an isolated
profile whose only setting is `syncAudioFormat=wma-lossless`, launched
`--page device --qa-probe --qa-device "<flac>|Wakin Chau|WL QA 2026-10-29|Lossless Roundtrip
Test|162320"`. The log is the evidence, in order.

- `[sync] native copy refused verdict=2 tag=0x0010 codec=flac asf=0 lossless=0 -- encoding
  instead`: F-05 refusing to mistake a carryable-looking container for a carryable codec.
- `[transcode/wma-lossless] target 2 ch / 44100 Hz / 16-bit for a 2 ch / 44100 Hz / 16-bit
  source`, then `wrote 7158312 samples ... lossless`.
- `[sync] audio target=wma-lossless produced=wmalossless tag=0x0163`: the byte check that sits
  between "the encoder ran" and "this file may leave", naming what it found. `0x0163` is
  WMA_LOSSLESS; had it answered anything else the file would have been deleted and nothing sent.
- MTPZ `handshake complete`, `forge done: 1 ok, 0 failed`, device rows 168 -> 169,
  `sync-summary elapsedMs=8822 tracks=1 failed=0`, attributed music 4.51 -> 4.53 GB.

The pull is a separate run, so it also proves the upload survived the device's own re-index:
same binary, `--qa-device "|Wakin Chau|WL QA 2026-10-29|Lossless Roundtrip Test"`, answering
`download-from itemId=16777486 readbackDurationMs=162320` - the duration the device stored,
equal to the source's 162.320 s - and `download-saved name='Lossless Roundtrip Test.mp3'
ok=true`. What came back is 18,452,703 bytes, 14 more than the bytes that went out, behind the
`.mp3` name the device supplies, and `ffprobe` on it says `codec_name=wmalossless` / `asf` /
162.319 s. Decoded to `s16le` it is 28,633,248 bytes and hashes to
`E5C6FA3C1EF4A1D6B586E9573FB7B976231448BB72425AD23F54A8C4E86BC3D0`, the same hash as the source
FLAC decoded. FLAC -> the encoder in Windows -> this Pavo Zune HD -> back over USB -> PCM is
bit-exact, which is the strongest thing this repository has ever been able to say about a write.

Two lessons about the harness rather than the application. `Start-Process -ArgumentList @(...)`
does not quote an element containing spaces, so the first attempt reached
`Qt.application.arguments` as five separate words, `qaDeviceSpec` came out null, and the run sat
through 80 ticks reporting `[qa]` states without ever asking the device for anything - this flag
has to be handed over as one pre-quoted command line. And a launch made with `-WindowStyle Hidden`
has no window for `winshot --pid` or `Process.CloseMainWindow()` to find, so the only way out was
a forced kill; instrumented runs want `-WindowStyle Minimized`. That kill followed a read-only
session, and `devicetool` answered `VERDICT=agree` at problem 0 afterwards, with no write in
flight and nothing needing a power cycle.

What this does not change, stated so it is not rediscovered: **nobody has listened**, so the HD
accepting the bytes is not the HD playing them, and open item 5 above stays open for that reason
alone. Deletion is still untested, so the test track is still on the player. WMA Lossless stays
documented for the HD alone, and no other model has been offered one. The podcast, audiobook and
photo/video read halves and packaging are untouched by any of it.
