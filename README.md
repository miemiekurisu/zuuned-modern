> **Public source snapshot:** read [PUBLIC_SOURCE.md](PUBLIC_SOURCE.md) for credential requirements and differences from private builds.

**English** · [简体中文](README.zh-CN.md)

# Zuuned Modern

**Bring your Zune back into the rebellion - on Windows and on Apple silicon.**

Zuuned Modern is a **Windows client port** of [Zuuned Linux](https://github.com/badgeromen/zuuned-linux),
the community app for managing Microsoft Zune players. It is the same Qt 6 / QML and
C++ application and the same [libzune](https://github.com/badgeromen/libzune) PTP/MTP/MTPZ
protocol stack, built and run natively on Windows with MSVC - no WSL, no MinGW, no
Cygwin, no virtual machine.

One source tree serves all three platforms. The Windows work is additive: a
`compat/win/` layer that keeps the POSIX surface libzune and the transcode C layer are
written against, a Windows device-presence path, and native path and encoding handling
throughout. The Linux line is upstream's, and this repository exists to carry the port
until upstream wants it.

The same tree also builds and runs natively on Apple silicon: Qt 6 and Homebrew
dependencies, the userspace libusb path Linux uses rather than an Apple driver, and no
kext or DriverKit extension - a DEXT cannot load unless Apple notarises it and this
project has no Apple developer licence. On 2026-11-06 that build opened, authenticated,
read, wrote, read back and deleted a track on a real Zune HD.

Every picture in this file and in [README.zh-CN.md](README.zh-CN.md) was captured from
`zuuned.exe` built on this repository, on Windows, with a Zune HD plugged in.

![The local album grid with artwork, and a connected Zune HD](docs/readme-screenshots/music-library.png)

*The music page on the album pivot: cover art read out of the local files, song rows
carrying the same art, and a Pavo-generation Zune HD connected over WinUSB in the panel
at the right.*

[Status of the Windows port](docs/WINDOWS_PORT.md) and
[status of the macOS port](docs/MACOS_PORT.md) - **read those pages before trusting any
hardware claim in this repository.** They record, per capability, what has been done on a
Windows or Apple-silicon machine against a real player and what has only ever been done
on Linux.

## What you can do with Zuuned

Browse a Zune's library and transfer media to it over USB; enjoy a local music library
with artists, albums, playlists and playback; organize videos, podcasts and audiobooks;
edit metadata and artwork; build photo albums. Local playback and organization work with
no player connected, and each kind of media has exactly one default destination folder -
that folder shows the fact as a status on its row, the others offer it as a button. Sync
converts audio on the way to the player to whichever profile you pick under **Settings ->
Device -> Audio conversion**, WMA Lossless included; that target is written by the encoder
the operating system already ships rather than by a bundled codec, and it refuses rather
than quietly downgrading to lossy. The Windows port carries music in both directions
against a player and takes music back off it, one track at a time, with the player
agreeing in a later session; photos and video have been sent from Windows without a byte
of either coming back - see the status page.

## The pages

| | |
| --- | --- |
| ![A Zune's music library read over USB](docs/readme-screenshots/device-albums.png) | **The device page.** What is actually on the player - artists, albums, songs and genres browsed off the Zune, with album pictures fetched over USB rather than looked up anywhere. |
| ![The song list](docs/readme-screenshots/music-songs.png) | **The song list.** One row per track with its own thumbnail, sortable, searchable, and the same rows the queue is built from. |
| ![Now playing](docs/readme-screenshots/now-playing.png) | **Now playing.** The vinyl island over the library. The playing row here comes from the app's own QA hook, so it is labelled `QA tone` rather than by its tags. |
| ![Videos](docs/readme-screenshots/video-library.png) | **Videos**, with the profile each movie will be converted to before it can go onto a player. |
| ![Photos](docs/readme-screenshots/photo-library.png) | **Photos**, and the photo albums you build out of them for the player. |
| ![Settings, Library](docs/readme-screenshots/settings-library.png) | **Settings - Library.** Watch folders spelled the way Windows spells them, one default destination per media kind, podcast and audiobook import. |

The rest of the captures, if you want to see them at full size:
[welcome](docs/readme-screenshots/welcome.png),
[artists](docs/readme-screenshots/music-artists.png),
[the device artist grid](docs/readme-screenshots/device-music.png),
[podcasts](docs/readme-screenshots/podcasts.png),
[audiobooks](docs/readme-screenshots/audiobooks.png),
[Settings - Appearance](docs/readme-screenshots/settings-appearance.png),
[Settings - Device](docs/readme-screenshots/settings-device.png),
[Settings - Mixtapes](docs/readme-screenshots/settings-mixtapes.png),
[Settings - Health](docs/readme-screenshots/settings-health.png).
Chinese copies of all of them live in
[docs/readme-screenshots/zh/](docs/readme-screenshots/zh).

## Interface language

**Settings -> Appearance -> Language** writes the interface in English or
简体中文, or follows the Windows display language. It is read once, at launch, and the
card says so and tells you which one is on screen right now.

The wording is a file, not a code change: `i18n/zh_CN.tsv` holds every message and its
Chinese, `i18n/zuuned_zh_CN.ts` is the Qt translation source built from it, and
`python i18n/audit.py` fails the tree when a piece of interface text is typed straight
into QML or C++ where no translation could ever reach it. So rewording the interface is
an edit and a restart, and a third language is a new file rather than a refactor. See
[docs/I18N.md](docs/I18N.md).

Names, tags, file paths and what the Zune itself displays are never translated - only
what zuuned says.

## What has actually been done on Windows

Hand-tested against one Pavo-generation Zune HD on the development machine:

- **Driver binding** - the player bound to WinUSB, read back as `CM_PROB_NONE` with an
  interface GUID ([`packaging/windows/DRIVER_INSTALL.md`](packaging/windows/DRIVER_INSTALL.md)).
- **Enumeration and hotplug** - unplug and replug produced the same read twice.
- **Reading the device** - `GetDeviceInfo`, the ZMDB scan, the device page browsed with
  album thumbnails fetched off the player.
- **Authenticated transfers, both ways, through the ordinary controls** - two tracks sent
  to the player, one of them on an album named in Chinese, and a track pulled back into the
  local library with byte readback. A pull lands in the folder the library declares for
  music, not in a directory of the app's own choosing.
- **Local library on a Windows disk** - watch folders, native `E:\...` path display,
  non-ASCII file names, album artwork.
- **WMA Lossless end to end** - a FLAC master encoded by the encoder Windows already ships, sent
  to the player, taken back with PCM that still hashes to the master's, and played on the HD. The
  container that comes back is not the one that went out: a Zune re-wraps an ASF on ingest and
  rewrites the Author inside the file from the name it holds device-side, so the invariant for this
  format is the codec tag the player serves plus the decoded samples, never the bytes.
- **Deletion** - one music track purged through the right-click control, and the next
  launch read 168 tracks in 10 albums by 10 artists back off the player with its own free
  space 18.5 MB larger. The agreement is the device's, not a row dropped from a list.

**Not yet proven on Windows**, and not inherited from Linux either: any deletion beyond
that one music track, the podcast, audiobook, photo and video transfer categories, every Zune
model but that one Pavo, audible PC-side playback (the automated gate opens WASAPI muted, so
no automated run has heard anything), and the packaged folder with a player attached or on a machine
that did not build it - there is a preview artifact now, but still no installer and no code signing.
**WMA Lossless** is encoded on Windows, by the encoder the operating system already ships rather than
a bundled codec, and a track written that way has now
been heard on the HD; the format is documented for the Zune HD alone, so no other model has been
offered one and the setting says out loud that the likely result elsewhere is a silent track.

## Getting a Zune seen by Windows

libusb cannot see a Zune that has no driver bound, and Windows ships none for one. An
unbound player sits at PnP *problem 28* and appears nowhere. Binding is a one-time manual
step per machine and **nothing in Zuuned installs a driver**:

1. Plug the player in and run [`packaging/windows/check-driver.ps1`](packaging/windows/check-driver.ps1)
   to see what Windows thinks it has.
2. If it is unbound, follow
   [`packaging/windows/DRIVER_INSTALL.md`](packaging/windows/DRIVER_INSTALL.md) - the short
   version is *use Zadig and pick the device id that script prints*. Do not hand-write an
   INF, and do not install the Zune software.
3. Replug. The app then says plainly whether it can use what is plugged in.

## Build on Windows

The full, current recipe is
[the Windows section of BUILDING.md](BUILDING.md#build-on-windows). Short version:

- Visual Studio 2022 (`msvc2022_64`), CMake, Ninja
- Qt 6.11 for MSVC 2022 64-bit
- a vcpkg tree, or a `ZUUNED_WIN_DEPS` tree, for libusb, libgcrypt, SQLite, mpv, FFmpeg
  and LAME
- load the MSVC environment (`vcvars64.bat`) before configuring - loading none, or the
  one belonging to a different Visual Studio, fails as a flood of bogus `std::thread`
  diagnostics or an `STL1001` static assert rather than as a linker error
- MTPZ authentication data, unless you want the read-only case. This source ships no
  credentials, so a build without them browses a player and answers `AccessDenied` or a
  raw `0x200F` the moment it writes to one: the symptom reads like a platform fault and is
  not one. Put the five-line file at `%USERPROFILE%\.mtpz-data`, which needs no rebuild, or
  create `libzune\src\mtpz_keys.h` - [docs/BUILD_WITH_MTPZ.md](docs/BUILD_WITH_MTPZ.md)

```powershell
git clone https://github.com/miemiekurisu/zuuned-modern.git
cd zuuned-modern
cmake -B build -G Ninja
cmake --build build --clean-first
build\zuuned.exe
```

`--clean-first` is not a stylistic preference on this platform: on a localized Windows
install Ninja records zero header dependencies, so an incremental build after a header
edit quietly relinks stale objects. `BUILDING.md` explains, with the upstream issue
numbers.

Two software gates ship with the tree and neither one touches a player:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\win-compat\run.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\win-usability\run.ps1
```

They cover the POSIX-compatibility layer and launch every page, every settings tab, the
filename and library probes, plus the command-line self-tests that ride inside it -
`librarytool names`, `paths`, `customize`, `folders`, `vanish`, `videofloor`, `census`,
`cover` and `net`, `transcodetool retagcheck` and `copycheck`, and `mtpframingtool` - all
run against isolated profiles. A green run on 2026-10-29 reported
`201 checks, 0 failures, 0 skips` and
`launches=18 checks=134 failures=0 skips=1 killed=0`, the single skip being the two checks
that name a specific device with no Zune attached to compare. Those counters move with the
tree, so read them as a floor rather than an expectation. Neither gate says anything at
all about a Zune - that is deliberate, and is why the hardware evidence lives on
[docs/WINDOWS_PORT.md](docs/WINDOWS_PORT.md) instead.

### A folder that runs without this toolchain

A build tree runs where the toolchain lives. [`packaging/windows/deploy.ps1`](packaging/windows/deploy.ps1)
turns it into a folder that runs without it, and proves the claim the only way that settles it - by
launching the staged `zuuned.exe` with a `PATH` naming nothing but the folder itself, then reading
which DLLs the loader actually mapped:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File packaging\windows\deploy.ps1 -Out E:\Temp\zuuned-package
```

It stages the Qt runtime and its QML modules through `windeployqt`, the FFmpeg tools, `libmpv`,
SQLite, libusb, LAME and the VC++ runtime, the Chinese catalogue, the driver document and a
`README-Windows.txt` that says out loud what has and has not been tested; it reads the manifest back
out of the compiled executable, inventories the import table of every binary in the folder, checks
that each QML module the sources import arrived, and prints a SHA-256 for the `.zip`. A green run on
2026-11-08 reported `31 checks, 0 failures` over 1,556 files and a 176.8 MB archive, with 58 modules
mapped from inside the package, 135 from Windows and none from a development tree. It compiles
nothing, installs no driver, opens no device handle, signs nothing and builds no installer: the
artifact is a preview, and it is the build tree's own device evidence, not a new one.

## What has actually been done on macOS (Apple Silicon)

Hand-tested on one Apple-silicon Mac against the same Pavo-generation Zune HD, in three
sessions with the player unplugged between them:

- **No driver, and none needed** - macOS binds nothing to the player, so libusb sees
  `045e:063e` with nothing installed and claims interface 0 on the first attempt.
  [`packaging/macos/claimprobe.c`](packaging/macos/claimprobe.c) prints that sighting,
  endpoints included. Nothing is installed, bound or signed for the kernel, and there is
  no Zadig step: a DriverKit extension was ruled out on purpose because one cannot load
  unless Apple's own notarisation signed it, which this project cannot obtain.
- **Reading the device** - `GetDeviceInfo`, the Pavo property read, the whole 224,928-byte
  ZMDB payload, and 168 tracks in 10 albums by 10 artists browsed on the device page.
- **MTPZ authentication from the embedded keys** - measured with `~/.mtpz-data` deleted on
  purpose, which is what made every write path below reachable.
- **One music send, judged by what came back** - an untagged MP3 went in through the
  ordinary controls; the player's own free space fell by 196,608 bytes for a 193,097-byte
  file, album and track counts each grew by one, and the bounded re-index ladder read 169
  tracks back with nobody clicking. Taken off again in a second session it landed in the
  folder the library declares for music, 1,186 bytes the bigger for the tags the device
  wrote, with audio frames and decoded samples that both hash to the sent file's.
- **Deletion** - purged through the right-click control, and a third session read 168
  tracks back off the player with free space 194,283 bytes larger - exactly the size of
  the file that had just been pulled off it.
- **Arrival hotplug** - three sessions, three `Zune arrived` lines from the libusb
  callback rather than the poll fallback.
- **Packaging** - [`packaging/macos/deploy.sh`](packaging/macos/deploy.sh) publishes a
  self-contained bundle, a `.dmg` and a zip, and both archives were opened the way a
  tester opens them. Windows still has no installer.

**Not yet proven on macOS**, and not inherited from Windows or Linux either: any deletion
beyond that one music track, the podcast, audiobook, photo and video transfer categories,
every Zune model but that one Pavo, audible Mac-side playback (CoreAudio is measured to
bind and the playhead to move; nobody has listened), the *removal* half of hotplug -
`Zune left` has never once appeared in a macOS log here - and the packaged bundle with a
player attached, because the evidence above came from the build-tree bundle. The response
codes this player still answers, the thumbnail failures, and the reason no DEXT is in the
build are all on [docs/MACOS_PORT.md](docs/MACOS_PORT.md).

## Build on macOS (Apple Silicon)

The full, current recipe is
[the macOS section of BUILDING.md](BUILDING.md#build-on-macosapple-silicon). Short
version:

- Xcode command line tools, CMake, Ninja
- Homebrew for Qt, libusb, libgcrypt, SQLite, mpv, FFmpeg and LAME
- MTPZ authentication data, unless you want the read-only case - the same five-line file
  as every other platform, at `~/.mtpz-data`, or `libzune/src/mtpz_keys.h`
  ([docs/BUILD_WITH_MTPZ.md](docs/BUILD_WITH_MTPZ.md))

```bash
git clone https://github.com/miemiekurisu/zuuned-modern.git
cd zuuned-modern
cmake -B build-macos-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos-release --parallel 8
./build-macos-release/zuuned.app/Contents/MacOS/zuuned
```

`./packaging/macos/deploy.sh` turns that tree into the self-contained bundle and both
archives. There is no Apple developer licence behind this project, so nothing is
notarised: a first launch wants right-click -> Open or
`xattr -dr com.apple.quarantine`, and a bundle built under `~/Documents` also wants the
Documents-folder consent, because that is where macOS keeps the Music folder.

Run anything that touches a player, a video decoder or an audio device **outside** a
sandbox. Inside one libusb enumerates zero devices and VideoToolbox refuses its session,
and both look exactly like a broken build rather than like a denied permission.

## Building on Linux

The upstream instructions still apply and are unchanged by the port: see
[BUILDING.md](BUILDING.md), [docs/BUILD_WITH_MTPZ.md](docs/BUILD_WITH_MTPZ.md) for Zune
authentication data, and the upstream
[badgeromen/zuuned-linux](https://github.com/badgeromen/zuuned-linux) repository for
released Linux packages, the AppImage workflow and the udev rule. This repository builds
Linux too, but it publishes no Linux binaries. The Linux software gates ship here as well:
`ci/check.sh` drives the other directories under `tests/` - music import and sync,
artwork, spoken media, playlists, diagnostics and the rest - on a Linux machine, which is
the only place that suite runs.

## Known issues

**WMA Lossless does not exist outside Windows, on macOS or anywhere else, and closing
this is deferred.** The encoder this product writes the format with is the one already
inside Windows - Windows Media Audio 9.2 Lossless, reached through the Windows Media
Format runtime - so `CMakeLists.txt` adds `transcode/wma_lossless_enc.cpp` under
`if(WIN32)` and every other platform links `transcode/wma_lossless_stub.c`, whose
`zuuned_wma_lossless_available()` answers an honest 0. Nothing is hidden by that:
**Settings -> Device -> Audio conversion** builds its format list from that one answer,
so on a Mac the WMA Lossless row is disabled and the card's status line reads
`not on this platform` rather than offering a format this build cannot make, and no
other destination goes through that code path. FFmpeg is not a way round it, and that
was checked rather than assumed - the Homebrew FFmpeg 9.0.2 behind the Mac build lists
`wmav1` and `wmav2` as its only WMA encoders and carries `wmalossless`, `wmapro` and
`wmavoice` as decoders only, so asking for it answers `Unknown encoder 'wmalossless'`.
What encoders do exist in the community drive the same Windows codec library through
`wmvcore`, so there is no open-source encoder here to vendor, only a Windows one to
call. What that costs a Mac or Linux user is the lossless destination: a FLAC, WAV or
APE has somewhere lossless to go on Windows and gets converted down here. Deferred, not
forgotten - and a local library, a local file and every other sync format are
unaffected by it.

## Reporting a problem

Open **Settings -> Health -> export debug report**, read the saved text, and attach it to
an issue here. Say which platform and build, the Zune model, and the steps. Logs are under
`%LOCALAPPDATA%\Zuuned\Zuuned\State\logs\` on Windows and
`~/.local/state/Zuuned/Zuuned/logs/` on Linux; settings are an ini file under
`%APPDATA%\Zuuned` on Windows. Reports never upload themselves. See
[docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

## Layout

```
src/            C++; DeviceService (libzune bridge), app entry
qml/            UI; Theme.qml design tokens
compat/win/     the Windows POSIX surface, built once as zuuned_wincompat
libzune/        included source; shared C protocol library
libzune/tools/  zunetool and mtpframingtool, the protocol-side probes
transcode/      FFmpeg and LAME transcoding, plus the in-box WMA Lossless encoder
i18n/           the interface wording; zh_CN.tsv is the editable source
packaging/      Linux USB rule, PKGBUILD, AppImage builder and macOS bundle packager
packaging/windows/  WinUSB driver guide, check-driver.ps1 and deploy.ps1, the folder packager
packaging/macos/    macOS bundle packager and claimprobe.c, the USB claim probe
tests/win-compat/    Windows compatibility gate (no device)
tests/win-usability/ Windows launch and UI gate (no device)
tests/            the Linux gate suite, driven by ci/check.sh (no device)
docs/readme-screenshots/  these captures, and the zh/ set
docs/WINDOWS_PORT.md what is and is not proven on Windows
docs/MACOS_PORT.md   what is and is not proven on Apple silicon
docs/WINDOWS_COMPAT_AUDIT.md the whole-tree Windows compatibility sweeps
docs/SETTINGS_INVENTORY.md every switch, and the feature behind each one
docs/FEATURE_COMPLETION.md the implementation backlog
docs/ZUNE_CLIENT_FEATURES.md the original Zune software, feature by feature
```

## License

Original Zuuned application code is licensed under **GPL-3.0-or-later**. See
[LICENSE](LICENSE) and [licensing scope](COPYING.md). Third-party components retain their
own licenses and attribution. libzune has its own repository.

Zuuned is an independent community project, not affiliated with or endorsed by Microsoft.
Zune is a trademark of Microsoft Corporation.
