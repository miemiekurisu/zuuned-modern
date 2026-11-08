# Windows beta artifact, 2026-11-08

Two questions had to be answered before a folder could be called a beta. Whether it
has any right to carry the codecs it carries, and whether it carries anything about
the person who built it. Both are now checks a script runs rather than promises in a
README, and this page records what those checks measured.

## The licence question, and why FFmpeg is half in

Nothing here forks or rebuilds FFmpeg. The folder ships the shared FFmpeg build that
`BUILDING.md` says to download, unmodified, and the application resolves it at run
time. Three routes were considered for the two command-line programs `ffmpeg` and
`ffprobe`:

1. **Ship an LGPL-only FFmpeg.** Not free. `transcode/libav_transcode.c` uses
   `libx264` for the H.264 profile, and libzune probes media through libav
   (`libzune/src/util.c`), so the whole transcode layer and the device scanner would
   have to be re-verified against a different FFmpeg configuration - for a codec set
   a Zune cannot play. This is a lightweight client; a codec pipeline rewrite is not
   its job.
2. **Move the CLI use behind a plugin or a delay-load boundary** so the library set
   could shrink. The ZMDB scan probes each object's payload through libav inside the
   process, so the library is not optional and splitting the CLI out saves nothing
   that the app does not already need.
3. **Stay with the GPL build, name it, and stop shipping what nothing runs.** Chosen.
   Zuuned Modern is GPL-3.0-or-later, so the GPL runtime combines on the same terms
   and the folder stays under the GPL; `THIRD-PARTY-NOTICES.txt` names every
   component, says which are shared libraries and therefore relinkable, names the
   static exception (PCRE2, BSD-3), states that this FFmpeg is configured
   `--enable-gpl` and so contains `libx264`, and says where the corresponding source
   is for the application and for the bundled FFmpeg.

So `-Release` leaves `ffmpeg.exe` and `ffprobe.exe` out, and leaves out the two DLLs
that only those two programs import. Nothing else about FFmpeg changes, because the
application really does load it: conversion, the WMA Lossless and WMV profiles, and
every metadata probe run through the library inside the folder. Where a feature
genuinely wants the command line - a video thumbnail, or a file the built-in library
cannot open - the app already says so and names the download page
(`Settings -> Device`, `src/diagnostics/DiagnosticsService.cpp`, and the fix-it card
that `src/LibraryService.cpp` raises), so the instruction the user asked for is in the
product, not in a README.

## What `-Release` changes, measured

Both runs below staged the same build tree on the same machine. The preview numbers
come from a `-NoSmoke` run, which is why its check count is lower.

| | preview | `-Release` |
| --- | --- | --- |
| files staged | 1,557 | 1,539 |
| staged size | 460.4 MB | 418.1 MB |
| archive | 176.8 MB | 158.5 MB |
| checks | 30, 0 failures | 40, 0 failures |

The 18 files and 42 MB that a release refuses to carry:

- `tools/ffmpeg.exe` and `tools/ffprobe.exe`, 0.7 MB together;
- `avdevice-63.dll` (4.7 MB) and `avfilter-12.dll` (35.8 MB), which `dumpbin` shows
  nothing but those two programs imports;
- `icuuc.dll`, byte-identical to the copy in `C:\Windows\System32` - a release does
  not re-host an operating-system file;
- the 13 files of `qmltooling/`, the QML debugger.

A release also stops treating the notices as optional: `LICENSE`, `COPYING.md` and
`THIRD-PARTY-NOTICES.txt` are staged, and a missing one is a failure. And it requires
a clean tree, because a beta must not be made from work nobody has committed:
`git status --porcelain` non-empty fails the run rather than producing a beta wearing
a preview's hat. It then compares that clean revision against the one stamped inside
`zuuned-build.json`. That second check exists because the first is not enough: the stamp
is written when the build compiles its version object, and the first release run here
passed a clean tree over a build record naming an older revision and marking itself
dirty.

What the first release run found is in the script now. It died in its own release trim:
`Remove-Item` on `avfilter-12.dll`, a file the run had copied seconds earlier, answered
"being used by another process". A minifilter holding a handle to a fresh DLL is
ordinary, and the script already knew a staged tree needs care when deleting
(`Remove-Tree`) while the trim used a bare `Remove-Item`. `Remove-StagedFile` retries
for five seconds, keeps the long-path prefix, and answers instead of throwing, so the
caller reports a Pass or a Fail rather than dying between the two. It is also why this
folder comes from `cmake --build build --clean-first` run from the commit it names,
not from whichever build was already on disk.

The largest things left in the folder are `libmpv-2.dll` (115.5 MB),
`avcodec-63.dll` (114.3 MB), `zuuned.exe` (31.2 MB), `avformat-63.dll` (24.8 MB),
`opengl32sw.dll` (19.7 MB) and `dxcompiler.dll` (13.7 MB). An archive this size is a
GitHub Release asset, never a commit: nothing under `-Out` is tracked, and `-Out`
defaults to gitignored `_tmp`.

## The privacy pass

`packaging/windows/deploy.ps1 -Release` reads every staged relative path and every
byte of every staged text file, executable and `.qm` catalogue - once as Latin-1 and
once looking for UTF-16 code points - against the account name, the host name, the
profile folder, the build tree and the test-library folder of whoever ran it. It also
checks the entries of the `.zip` after writing it, not just the folder they came from.

What it found, on the first run against a folder that had been shipped as a preview:

- `icuuc.dll`, a byte-for-byte copy of a Windows system file;
- `D:\workspace\github\zuuned-moden` inside `zuuned.exe` and `devicetool.exe`, written
  there by the linker as the recorded path of the program database. Fixed at the root:
  `/PDBALTPATH:%_OBJNAME%` in `CMakeLists.txt` under `MSVC`, so no shipped image
  carries the folder that built it. The three test strings in
  `src/library/librarytool.cpp` that used a real account name as an artist name became
  `Test Artist`, and three quotations of it in `docs/WINDOWS_PORT.md` are redacted.
- five third-party DLLs (`gcrypt`, `gpg-error`, `libmp3lame`, `libusb-1.0`, `sqlite3`)
  that record where *their* build was made, in their own debug-path and `assert`
  strings. This project does not compile them and cannot rewrite them, and none of the
  strings names a person, so a release pass records them and says so. That is the
  stated limit of the pass, not an oversight.

A release run then deletes its own smoke profile and smoke log - both are written
outside the stage, and the check says so rather than assuming - and the summary line
says which kind of artifact was produced and which rules were run.

## The repository's side of the same rule

`tests/privacy/run.ps1` is the counterpart for what gets committed: nine checks over
`git ls-files` and the bytes of what it lists. No log, database, settings file,
archive, binary or Finder/Explorer metadata may be tracked; no tracked path or text
byte may carry the account, host, profile folder or commit identity of the person who
made the checkout, except the name the repository is published under, which is on its
own clone URL; every tracked picture must sit under a prefix named in
`tests/privacy/reviewed-media.txt` with a reason beside it; and the number of tracked
lines quoting a drive-letter path is printed rather than hidden, because in this
repository those lines are recorded build and device evidence and a new one should
change a visible number. Green on 2026-11-08: `813 tracked files, 9 checks, 0
failures`, 3 identity strings, 90 reviewed pictures, 96 files skipped as binary, 155
lines quoting a drive letter. The gate was proved by staging a file naming a real account
and watching it exit 1; the drive-letter counter needed a fix before it meant anything,
because its pattern asked for a path at the end of a line and this markdown ends its lines
with a carriage return, so the honest number is four times the first one.

It found one real tracked offender immediately: `libzune/.DS_Store`, Finder metadata
that records which files a folder held. It is untracked now and `.gitignore` refuses
it.

**Known open.** `docs/readme-screenshots/` and `docs/readme-screenshots/zh/` hold 32
screenshots taken from the real library on this machine, and the pixels show a device
name, real music-folder paths and the actual collection. Pixels cannot be scanned by
this or any text gate, which is why the prefix is listed as `KNOWN OPEN` rather than
silently allowed. Replacing them needs a capture from an isolated profile pointed at a
generated fixture, and the device-page ones additionally need the player unplugged;
the git history keeps the originals either way, and rewriting it is a decision that
belongs to the owner of the repository, not to a script.

## What this artifact does not prove

The packaging pass is about the folder and nothing else.

- Device evidence is unchanged and stays one Pavo (Zune HD), by hand, on one machine:
  WinUSB binding, hotplug, the music library read, MTPZ authentication, one send with
  byte readback, one download, one deletion, one WMA Lossless round trip that played
  on the player.
- Nobody has heard audio out of this application on a PC; every automated run asks for
  a null audio output.
- Podcasts, audiobooks, photos and video on a player, every model but that one Zune
  HD, and this folder on a machine that did not build it, are unproven on Windows.
- No installer, no code signing, no SmartScreen history, and no MTPZ credentials
  inside it, so a tester who wants to write to a player supplies their own.
- Nothing was compiled by the packaging run: the folder is a copy of a build tree, and
  `zuuned.exe --version` names the revision that tree came from.

The digest of the published archive is added by the commit that follows the run that
staged it, because the run refuses a dirty tree and a document cannot contain the
digest of an archive that does not exist yet.
