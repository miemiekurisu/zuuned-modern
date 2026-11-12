# Windows capture tool

`winshot.cpp` is the eye of the Windows usability gate.  It captures what a
window of a running `zuuned.exe` actually paints, opening no device handle of its
own and playing no user audio
and without disturbing the desktop session, because a Qt Quick window can start
with a perfectly clean stderr and still render nothing.  Every launch case in
the gate is meant to end with a capture and a blankness check.

Build it from an MSVC x64 developer prompt at the checkout root:

    cl /nologo /W3 /Od /Zi /MD /EHsc /std:c++17 /DUNICODE /D_UNICODE ^
       tests\win-usability\winshot.cpp /Fe:build\win-usability\winshot.exe ^
       /Fo:build\win-usability\ /Fd:build\win-usability\winshot.pdb ^
       /link user32.lib gdi32.lib gdiplus.lib

`/Fd` is not decoration: without it the compiler drops an incremental
`vc140.pdb` into the checkout.  `<objidl.h>` has to precede `<gdiplus.h>`
because this file defines `WIN32_LEAN_AND_MEAN`, and `GetImageEncoders` takes
its `size` argument by value.

Usage:

- `winshot <hwndDecimal> <out.png>` - capture, write a PNG, and print
  `ok src=... rect=... distinct=... dominant=... mean=... black=... white=...`.
- `winshot --move <hwndDecimal> <w> <h>` - resize a window, so the compact
  Now-Playing layout can be reached without asking the user to drag anything.
- `winshot --foreground <hwndDecimal>` - raise a window before a capture that
  needs the screen fallback.
- `winshot --pid <processId>` - print `hwnd=<dec> pid=<dec> titled=<0|1>` for that
  process's visible window and capture nothing, for a harness that has a pid but
  cannot trust `Process.MainWindowHandle`.  A visible non-tool window with a title
  wins; an untitled visible window is still reported rather than nothing.

Qt Quick on Windows presents through the window's own swapchain, so `PrintWindow`
with `PW_RENDERFULLCONTENT` is the primary path and a screen `BitBlt` of the
window rect is the fallback; the process is per-monitor-v2 DPI aware so the
rect and the pixels agree.  Exit codes: `0` captured, `1` API failure, `2`
usage, `3` captured but blank - fewer than four distinct colours, or one colour
over 99.5% of the frame.  A healthy music page measures about 9,500 distinct
colours with the single most common colour near 61% of the frame.

## Companions

`inspect_scan_db.py` reads a `librarytool scan` database and prints ASCII
`key=value` lines - `tracks`, `fingerprints`, `probe_failures`, `watch_folders`,
`min_duration`, escaped `paths`/`titles`, then `OVERALL=ok|fail`. Expectations go
in as code points (`--expect-codepoints 0x6F22,0x8A5E`) and as path-length
expectations (`--min-longest-path 200`, or `--expect-long-segments` when the
fixture uses a repeated segment marker), so neither the gate's arguments nor its
console output has to survive a non-ASCII round trip. It is what proves a scan
stored a Chinese folder name and hashed the audio behind it, rather than merely
counting rows.

`inspect_spoken_db.py` does the same job for the spoken-media store: it opens
`<profile>/spoken/spoken.db` read-write (a launch the gate stopped can leave an
un-checkpointed `-wal` that a read-only connection cannot recover), prints
`user_version`, `tables`, `indexes`, `columns`, `rows`, `integrity` and
`OVERALL=ok|fail` as ASCII, and fails unless the file really is the schema 1 `items`
store the application means. It exists because the bug it gates leaves a folder behind:
a file listing can prove something was written there, only the schema proves it is the
database the next launch will find.

`make_scan_fixture.py` builds the library fixture those checks turn on: a plain
tone, a WMA stream inside a file named `.mp3`, an album folder named in Chinese
characters, and twelve nested folders that push the leaf path to 277 characters. It
writes the CJK name from code points so this file stays ASCII too, and it needs the
FFmpeg from `ZUUNED_WIN_DEPS` to make the audio. `BUILDING.md` has the three-command
scan gate built from it, currently `created=4` then `4 tracks added` then
`OVERALL=ok`.

The same script answers for the one path a scan root takes that a folder name does
not: argv.  `--extra-roots <dir>` builds three one-track trees whose own root
folder name is the non-ASCII part - Hebrew, Hangul, and an emoji off the BMP,
because a surrogate pair is precisely what a code page conversion mangles.  That
road used to fail quietly: the scan found nothing and still exited 0.  The gate
scans each root on its own so a root that works cannot mask one that does not.

`watch_folder_repair.py` seeds and then audits the watch-folder rows an older
build mangled.  `--seed <db>` creates the table with four rows - a root stored as
`/E:/Scar Alone`, the same root again with a lowercase drive, that root spelled
correctly, and a clean path - so the leading slash that made a Windows root
relative, the drive letter case that turned one folder into two rows, and a row
that must survive untouched are all in play at once.  `--check <db> [--allow
<path>]` then asserts that opening the library repaired every one of them: no
leading slash, no lowercase drive letter, and exactly the expected set left alive,
where `--allow` names the root the scan itself legitimately added.  Run the same
script on POSIX and it fails if any of the four rows moved, which keeps the
repair provably Windows-only.

`qml/Main.qml` answers to `--qa-probe`: while the flag is present the window logs
one line per state change and then every 1.5 s, naming the page, the onboarding
sheet, the Now-Playing island, the collapse state, the device panel, the window
size, the track and photo counts, and the queue with its position. `--qa-play
<file>` puts one real file in the queue so a gate can reach the ribbon and, after
`winshot --move`, the compact island. A capture proves that a window drew
something; only these lines prove that `--page` took effect and that the frame
belongs to the page rather than to the splash or the onboarding sheet. Both stay
silent unless asked, on either platform.

## Status

`run.ps1` is that gate and it is complete.  An unfiltered run makes eighteen
instrumented launches plus the tool, encoder, decode and profile probes and ends
`launches=18 checks=116 failures=0 skips=0 killed=0` and `OVERALL=ok`:

    pwsh -NoProfile -ExecutionPolicy Bypass -File tests\win-usability\run.ps1

Phases run in this order.  `-Only` takes a comma list, and the scratch tree is
rebuilt at the start of every run, so pair a phase with `fixture` whenever it needs
the seeded library (`-Only fixture,pages`).

- `winshot` - compiles this tool into `build\win-usability\` and proves the build
  left no incremental `vc140.pdb` in the checkout.  Built on every run, filtered or
  not, for the run deletes the scratch tree first.
- QML type names - outside every phase, so no `-Only` list can lose it.  `Qt6Quick.dll`
  exports a `PathText` shapes type, every QML file here imports `QtQuick`, and in a file
  that does not also import `Zuuned` the app's own singleton registered under that bare
  name was Qt's type, so every binding on it threw at run time and queue-row artwork was
  missing with both gates green.  It is now `ZPathText`; the phase fails if any
  `qml/*.qml` writes a bare `PathText.` and pins the registered name, because no phase
  launches a QML file that forgets the import, so only the source can be checked.
- `fixture` - the fixture, a real `librarytool scan` over it, and the inspector: four
  tracks, four fingerprints, zero probe failures, the Chinese album folder and a path
  past 260 characters present in the database.  Then the two roots bugs: one scan per
  non-ASCII root folder, each expected to count its one track, and a seeded library
  full of `//e:/`-style watch-folder roots that a real `librarytool` run has to heal
  on open.  Then `librarytool photoscan` over two pictures in a Chinese-named album
  folder, because a photo library is otherwise only readable by an eye.  Also seeds the
  library the page cases copy into their profiles.
- `pages` - one launch each for music, videos, photos, podcasts and audiobooks with
  the seeded library, plus the device page, which this harness attaches nothing to.
  Every one ends
  with a capture, a paint verdict and the app's own `[qa]` report.  The music case
  also pins the path spelling it reports: at least one watch folder reached the app,
  every label it shows is spelled natively with no forward slash left in it, every
  stored path keeps one canonical forward-slash identity, and the row agrees with
  `ZPathText.display()` for the path behind it.  It also pins the offline default: the
  launch has to report `online lookup off`, and no provider request line may appear at
  all, because online artwork and metadata lookup is opt-in now.
- `settings` - the five settings tabs, each proving it is on the settings page.
- `collapse` - the header pinned at 0 and at 1, with the value the window reports.
- `compact` - one real file in the queue, then `winshot --move` through tall, narrow
  and tall again, reading the app's own ribbon and island state at each stage: the
  ribbon is retired below the headroom breakpoint, a queue plays through the island,
  growing the window brings the ribbon back, and shrinking it retires the ribbon with
  the island already up.
- `tools` - `transcodetool probe` on a real MP3 and on a WMA stream named `.mp3`,
  `librarytool names`, `librarytool customize`, `librarytool paths` and
  `librarytool net`, plus `devicetool`, which answers what is plugged in without
  opening it.
- `encode` - the transcode layer *producing*, not only reading, because video sync and
  FLAC sync are nothing but production: FFmpeg makes a real three-second H.264 clip,
  profile 0 has to return ASF (magic `30 26 b2 75`) at the source frame size with the
  running time intact, profile 2 has to return `ftyp` at 480x272, and the LAME leg is
  judged by probing the mp3 for the length of the tone it came from.  Containers are
  read by magic number because a name proves nothing and this repository already knows
  real libraries hold WMA streams inside `.mp3` files.  The products land in `%TEMP%`,
  exactly where the application writes, and the phase deletes them again.
- `decode` - the same clip decoded twice, because Settings ships a decode accelerator
  that no other phase reaches.  `ZUUNED_HW_DECODE=0` has to open exactly one decoder,
  ask for no accelerator and hand back its first frame as `yuv420p`; the default run has
  to name the path it used exactly once, and when that path is hardware the downloaded
  frames have to survive the scaler and the muxer as a 480x272 file of the right length.
  Which path the default picks belongs to the machine - a box with no NVIDIA decoder is
  right to fall back - so the run prints it as a note and asserts nothing about it.
  Neither case says a word about speed: this phase certifies that the GPU path really
  produces frames and a whole file, never that it beats software.
- `seek` - `playertool seek` and `play`, on the null audio driver.
- `audio` - one audible `playertool play` with `ZUUNED_MPV_AO=wasapi` and `--mute`,
  then one run with the variable empty.  mpv names the output it really bound through
  `current-ao` - there is no `audio-out` property and reading it answers nothing at
  all - so the first run has to print `audio_out=wasapi` and the second
  `audio_out=null`.  That is what separates "mpv clocked a null driver" from "the
  Windows audio backend actually opened", and muting keeps the phase polite enough to
  run on the desktop of someone who is working.
- `single` - a second instance pointed at the same profile and the same lock suffix
  refuses in its own words and never builds a scene, while the holder keeps its window
  and still closes politely.
- `profile` - session log, settings ini, both databases, the instance lock and the Qt
  pipeline cache inside the profile, and the real profile unchanged before and after.
- `cjkprofile` - two launches into one profile folder whose name is Chinese, because a
  Chinese Windows user name puts exactly that in front of every path the application
  builds.  The folder has to gain both databases, the second launch has to re-open what
  the first left behind, and `inspect_spoken_db.py` has to read the spoken store's own
  schema back out of it.  This phase is the regression gate for `SpokenStore` handing
  SQLite the local ANSI code page instead of UTF-8, which on a GBK machine opens a
  different file and lost every podcast and audiobook silently.
- `spokenclose` - a podcasts page on its own seeded profile, closed through the window
  rather than killed, because the spoken worker used to be waited on with no deadline
  at all. The phase fails if that close logs `worker would not stop` or an abandoned
  resume point, reads the spoken store the launch left behind back through
  `inspect_spoken_db.py`, and prints rather than asserts how long the close took -
  17.2 s here, nearly all of it the device worker retrying a wedged `OpenSession`.

Six things it cannot certify, and does not claim to:

- **This harness drives no device.**  It opens no USB handle and no MTP session of its
  own, and the device-page case asserts only that the page opens and paints.  When a
  player happens to be on the cable the application's own auto-connect browses it
  inside every isolated profile, which is the app's behaviour observed rather than a
  claim this gate makes.  The Windows wire evidence stays the hand test in
  `docs/WINDOWS_PORT.md`: one Pavo Zune HD over WinUSB, and one Zune HD over the
  in-box Windows MTP driver.
- The WASAPI run is muted.  It proves the Windows audio output opens and clocks
  playback; nobody listened to it, so it certifies no waveform.
- Dismissing the Now Playing island by clicking its X needs a mouse.  The gate proves
  the island opens on resize and that the ribbon retires and returns with the window.
- Nothing needs the network, and by default nothing uses it: online artwork and
  metadata lookup is a Settings switch, so the music-page case asserts the app's
  own `online lookup off` report and that no provider request line appears at all.
  A user who turns the switch on does reach musicbrainz.org, as on Linux.
- On a 1080p-class logical desktop the default 1280x800 window is already below
  `Theme.headroomCompactBelow`, so the compact island is the ordinary Windows view and
  the top ribbon needs an enlarged window by design.
- Nothing here plays a spoken item, so the bounded spoken close is gated only in
  shape: `spokenclose` proves the worker stops and the store stays readable, while
  the resume point inside its two-second deadline has never been written by a run.

Profile isolation, the piece that made any of that impossible to run honestly, is
now in place.  Every profile-owned path in the application goes through
`src/ProfilePaths.h`, so `ZUUNED_PROFILE_DIR` keeps a launch's `library.db`,
`spoken/spoken.db`, `State/logs/*.log`, caches and `Zuuned/Zuuned.ini` inside one
folder; a probe run proved that while the real `%APPDATA%\Zuuned` stayed
untouched.  `ZUUNED_LOCK_SUFFIX` isolates the single-instance lock so an
instrumented instance can run beside the daily one, `ZUUNED_MPV_AO=null` keeps it
silent, and with the variables unset every path stays where Qt would put it.

Qt's automatic shader/pipeline cache is the one profile file this application does
not open itself: Qt6Quick asks `QStandardPaths::CacheLocation` directly, so it wrote
into the real `%LOCALAPPDATA%` while the rest of the profile was elsewhere.
`src/main.cpp` now names `QSG_RHI_PIPELINE_CACHE_LOAD` and `QSG_RHI_PIPELINE_CACHE_SAVE`
inside the profile whenever `ZUUNED_PROFILE_DIR` is set.  A brand new profile logs one
`Could not open pipeline cache source file` warning before Qt has written that file
once: expected, recorded, and the price of leaving the real cache alone.

Under a profile folder whose name the local ANSI code page cannot spell, Qt's *save*
fails as well - the `cache` folder is plainly there and Windows still answers that the
path cannot be found. All sixteen ASCII profiles of one full run wrote their own
`qqpc_opengl`; neither Han-named profile did. That byte-path open is inside Qt, so the
gate records the warnings instead of asserting on them: what a Chinese profile folder
costs is a cold shader cache on every start, never a byte of the library.

Three probe findings shape the gate.  A fresh profile shows the onboarding wizard
instead of the requested `--page`, so a page case has to dismiss onboarding first
or report the onboarding state rather than claim a page rendered.  And the splash
lasts about two seconds plus a fade, so a window captured 1.5 s after it appears
is the splash, not the page - settle for several seconds, or poll the capture
statistics until the frame stops being one flat colour.  And a handle that the
harness reports is not trustworthy by itself: PowerShell 7 reads
`Process.MainWindowHandle` once and keeps the answer, so a probe that looks at it
while the splash is still mapping keeps saying "no window" for an app the user can
see - one probe found no handle on 30 consecutive healthy launches, while
`winshot --pid 77180` returned the same handle that a fresh `Get-Process` reported.
`run.ps1` now refreshes the property, falls back to the pid scan, and records which
route answered.  For the same reason `Stop-Zuuned` reports `124` rather than
`ExitCode` after it had to destroy a process: a killed app must not be able to
report a clean shutdown.

A handle above 0x7FFFFFFF is the same trap somewhere else: it arrives negative from
`Process.MainWindowHandle` and positive from `winshot --pid`, and only a run that
needed the pid scan can ever find what is wrong with it.  Masking to 32 bits is not
the answer - on a 64-bit Windows a window handle is a whole pointer, so masking
renames the window and `GetWindowRect` fails on a handle that exists.  The rule is to
carry the same eight bytes in the form `winshot` itself uses, unsigned decimal of
`uintptr_t`, in both directions: `ConvertTo-HwndDecimal` reinterprets and
`ConvertFrom-HwndDecimal` reverses it.  The parse side used to run the number through
`[long]::Parse`, and a real handle like `0xFFFFFFFF80010E3A` reaches that as
18446744071562137146, which does not fit a signed 64-bit integer, so on 2026-10-21 the
harness aborted halfway through the collapse pin phase with "Value was either too
large or too small for an Int64".  Both helpers are pinned by hand against those
numbers.  And
because a case can die halfway, the script carries a trap that closes every instance
it started - an aborted run must not leave a live Zuuned holding a profile folder the
next run intends to delete.

## What is plugged in

`devicetool.cpp` answers one question with no app, no UI and no USB device handle:
can libusb reach a Zune, and can the operating system see one at all? On Windows
those are different facts, because a player with no driver bound is enumerated by the
PnP manager with `CM_PROB_FAILED_INSTALL` while `libusb_get_device_list()` does not
contain it. The tool prints both accounts and a verdict — `agree`, `blocked`,
`absent`, `contradictory` or `unknown` — and the `what is plugged in` phase
(`-Only device`) holds them against a third, unrelated API, `Win32_PnPEntity`,
including the exact instance id and problem code. Nothing asserts that a Zune *is*
attached, so the phase tells the same truth on a machine with nothing plugged in.

The honest verdict on the development machine today is `blocked`: a Zune HD is
attached that Windows reports at problem 28 and libusb cannot open, and the scan
names the same instance Windows names. A Zune 30 record from weeks ago is still in
the registry, which is why the phase reports devnode records and attached devices as
two numbers — `Win32_PnPEntity` hides that stale record and `Get-PnpDevice` does not.

Seeing a device is not the same as driving one. The phase opens no handle and calls no
libzune function, so the only Windows wire evidence stays the hand test recorded in
`docs/WINDOWS_PORT.md`.
