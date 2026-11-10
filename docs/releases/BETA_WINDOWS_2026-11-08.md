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
| checks | 30, 0 failures | 43, 0 failures |

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

`tests/privacy/run.ps1` is the counterpart for what gets committed: nineteen checks over
`git ls-files` and the bytes of what it lists. No log, database, settings file,
archive, binary or Finder/Explorer metadata may be tracked; no tracked path or text
byte may carry the account, host, profile folder or commit identity of the person who
made the checkout, except the name the repository is published under, which is on its
own clone URL; every tracked picture must sit under a prefix named in
`tests/privacy/reviewed-media.txt` with a reason beside it; and the number of tracked
lines quoting a drive-letter path is printed rather than hidden, because in this
repository those lines are recorded build and device evidence and a new one should
change a visible number. Green on 2026-11-08, after the commit above: `817 tracked
files, 19 checks, 0 failures`, 3 identity strings, 90 reviewed pictures, 164 lines in
31 files quoting a drive letter. Every tracked file is now opened and searched to its end, in
ASCII and in UTF-16LE - the 96 pictures, fonts and other files that an earlier version
of this gate answered `skipped as binary` are judged rather than assumed, which is the
difference between a clean tree and a clean-looking one, and 721 of the 817 are
additionally read as text for the line counter. The gate was proved by staging a file naming a real account
and watching it exit 1; the drive-letter counter needed a fix before it meant anything,
because its pattern asked for a path at the end of a line and this markdown ends its lines
with a carriage return, so the honest number is four times the first one. The newest check
is not a string at all. A device serial is the one identifier nobody would ever put in a
needle list, so its rule is a pattern shared with the archive audit below: the tail of a
USB device instance path, everything after `VID_0000&PID_0000\`.
`packaging/windows/DRIVER_INSTALL.md` transcribed one, because that is what a real devnode
answer looks like, and it prints `<serial>` now.

It found one real tracked offender immediately: `libzune/.DS_Store`, Finder metadata
that records which files a folder held. It is untracked now and `.gitignore` refuses
it.

**Screenshots: read, not assumed.** `docs/readme-screenshots/` and
`docs/readme-screenshots/zh/` hold 32 screenshots taken from the real library on this
machine, and the pixels show real music-folder paths and the actual collection. That is
what the owner of this repository chose to publish. The half that was asked for is the
other one: no device identifier. Pixels cannot be scanned by a text gate, so each picture
was looked at, and the question was aimed first at the two places in the interface that
can render a serial at all - the `Serial` row under `This Zune`
(`qml/SettingsPage.qml:1669`) and the remembered-device list's `serial %1`
(`qml/SettingsPage.qml:1755`) - both of which sit below the visible fold in
`settings-device.png` in each language, where the page is scrolled to its top and the
`This Zune` heading is the last thing on screen. Everywhere else the player appears as
its friendly name `Kurisus ZuneHD` beside `Zune HD · 16GB`, `97%` and
`4.7 GB used · 10.8 GB free`: a label the owner typed into the device and the same
identity the repository is published under, not a serial number, and no
8-4-4-4-12 hex GUID appears in any shot. `deviceService.deviceSerial` is bound on the
spoken-media device pages (`qml/DeviceSpokenPage.qml:15`) but only as a key that refuses
a stale request, never as text. `settings-library.png` has its watch-folder home path
masked in both languages. What remains outside proof is stated rather than glossed: this
is a person reading 32 images, and any later screenshot has to be read the same way
before it is committed.

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

## The published archive

The run that produced the folder now shipped staged a tree whose build record reads
`f4430dc`, with `applicationDirty: false`, and said out loud that the record and the
tree name the same revision:

```text
revision   f4430dc79b6700b987bd26e5c65c74b31c6683ba, clean tree
staged     1,539 files, 418.1 MB
archive    158.5 MB, 1,539 entries, every one relative and under Zuuned-0.2.0-win64/
sha256     ACC557F7F03B247D11EEC853DE3FBEC25F0610CA7EC02651023E199737AB1CB9
checks     43, failures 0
```

Three checks longer than the table above first recorded, and all three are things this
repository used to assert in prose instead of measuring: that the build record inside
the folder agrees with the revision the run was told it was packaging, and that the
smoke log and smoke profile really are gone after the run rather than merely written
somewhere else.

## The archive was audited after it was written

A folder that passes and an archive written from it are two files. The entries of that
archive were therefore read one by one, without unpacking anything, by
`packaging/windows/verify-zip.ps1`:

```text
members    1,539 files and 72 directories, every one relative and under the package folder
bytes      418.1 MB decompressed, all of it read to the end and searched in ASCII and UTF-16LE
identity   no account, host, home folder, test library or scratch profile name in any byte
build tree no executable this project linked names the folder that built it
checks     16, failures 0        RESULT: archive clean - fit to publish
```

Nothing is skipped by extension. The first version of the reader opened only the kinds
everybody thinks to name - `.exe`, `.dll`, `.qm`, a list of text suffixes - which quietly
meant that everything else got to be unjudged; a photo, a playlist or a settings file
would have had to arrive under an extension nobody had written down, and that is exactly
the hole this step exists to close.

Three defects were found in the byte reader itself while writing it, and each is the kind
that produces a clean verdict about bytes that were never read:

- A zip member is a `DeflateStream`, which is not seekable: `Position` answers nothing, a
  read returns fewer bytes than asked for, and sometimes it returns none between two
  reads. The loop drove off `Position`, so it declared `avcodec-63.dll` finished at
  2.4 MB of 115 MB and called the rest of the largest file in the package clean. Progress
  is now counted in bytes that arrived, a silent read is retried, and a stream that stops
  short of its declared size is reported as unreadable rather than as clean.
- Binding `$null` to a `[ref]` parameter throws. The `catch` wrapped that call and folded
  the throw into an empty hit list, so every caller that did not pass `-ReadError` - most
  of them, in both this gate and the one that reads the repository - was told the bytes
  were clean whether they were or not, including the UTF-16LE half of the search.
- Matching was case-sensitive, so a needle spelled one way walked past a byte string
  spelled another, and an object file keeps whatever case the shell happened to hold while
  it was built. A build folder hid in plain sight by being typed differently than the list
  said it was. The search is ordinal and case-insensitive in both halves now, and the
  eighth reader case names a path and an account each in the other's case.

That reader is now one file, `packaging/windows/PrivacyProbe.ps1`, shared by the
repository gate, the staged-folder pass in `deploy.ps1` and this archive audit, and
`tests/privacy/run.ps1` tests it before it is trusted: eight cases, one per way a scan
has been watched answering `clean` about bytes that were not clean. `deploy.ps1` calls
this audit itself as of the commit that ships with it, so no later release folder can be
declared clean on the strength of the tree it was packed from.

Wiring it in changed nothing about what the artifact is: 1,539 files, 418.1 MB, the same
`sha256` above, and **no packaging or audit run of either script has ever opened a
device handle or added a device capability.**

What that run measured, beyond the counts. It dropped `avdevice-63.dll` (4.7 MB) and
`avfilter-12.dll` (35.8 MB) because nothing else imports them, refused `icuuc.dll`
because a Windows copy is byte-identical, and kept the QML debugger out. It read back
`qt.conf`, the manifest's `asInvoker` and `longPathAware` entries, and the catalogue. It
inventoried 107 binaries and resolved every import inside the package, in Windows, or
an api set, and then launched the staged folder with its own PATH: `devicetool`
answered `agree`, the app painted a page from an isolated empty profile
(`onboarding=true tracks=0 wfRows=0`), closed itself with exit code 0, and across 195
loaded modules nothing came from a development tree - 57 modules from inside the
package, 137 from Windows, 1 from neither. Its privacy pass is the one described above:
1,539 staged paths name no account, host, build tree or test library; the 668 scannable
text files are byte-clean in ASCII and UTF-16 (that pass has since been widened to every
file the folder holds, by the same shared reader - see the archive section below); no `.mtpz-data` is anywhere in it; and
the five third-party DLLs whose own build paths they carry are the stated limit, not a
finding.

That launch happened with a Zune HD still bound on this machine, and the application browsed
it read-only exactly as every gate launch here does. Nothing in this sitting wrote to,
deleted from or read back from a player, and no capability on `docs/WINDOWS_PORT.md` moved.

One consequence of the agreement check is worth writing down for whoever ships the next
folder. The revision inside this archive stays `f4430dc` even though the commit that
records its digest is later - that is what a digest means. Re-running `-Release` from
the newer checkout would now *fail*, correctly, because nothing was recompiled; to ship
a later revision, rebuild with `--clean-first` first and let the run name the new
sha256.

## The archive was audited again, with the folders it was built in

The audit above ran with the two build roots that were obvious to type. The same file
audited a second time with the whole probe list `deploy.ps1` hands its folder pass - the
dependency root and the staging parent included, and the folders above them asked of the
binaries this project does not compile - answered differently:

```text
FAIL  driver/DRIVER_INSTALL.md names the serial tail of a USB device instance path
FAIL  driver/DRIVER_INSTALL.md names D:\dev, a folder on the machine that built it
note  5 build-folder hit(s) inside third-party binaries: gcrypt.dll, gpg-error.dll,
      libmp3lame.DLL, libusb-1.0.dll and one more the line truncates; none names a person
checks 12, failures 2       RESULT: archive not clean
```

Both failures sit in a document this project writes, so both have a fix and neither has an
excuse. The driver guide transcribes a real `Get-PnpDevice` answer, and the tail of a USB
device instance path is the serial number of the one physical player this repository has
ever driven. It reads `USB\VID_045E&PID_063E\<serial>` now, the generated package name is
`<generated-guid>` and the registry line sets `<your-guid>`, and `git grep` for what used
to stand in those slots answers zero hits in a tracked file. The `D:\dev` quotation went
the same way.

Two changes keep that from happening quietly again. `deploy.ps1` now hands its own archive
audit the probes it hands the folder audit: the automated call named two of them, so the
run wired into a release had passed the very archive a stricter invocation of the same
script failed. And the folders *above* those roots go in as ancestors, which are asked of a
third-party binary and never of a document - the staging parent sits under the same drive
as `E:\Temp\testmusic`, which this documentation names on purpose because that is where the
hand tests ran. A needle list cannot hold a serial number, because nobody would type one,
so the rule is a pattern: `Get-PrivacySerialRule` in `packaging/windows/PrivacyProbe.ps1`,
anchored on the `VID_xxxx&PID_xxxx\` prefix so that album GUID monikers, MusicBrainz ids
and the `11111111-...` fixtures stay out of its way. It runs over every tracked file in
`tests/privacy/run.ps1` and over the members this project authored in an archive.

So the archive recorded above is withdrawn.

## The archive that takes its place

The rebuild went against a relinked tree, because a build-record check is worth nothing
over a stale record: `cmake --build build --clean-first` answered 341 steps, last progress line
`[340/341]` and `EXITCODE=0`, and `zuuned-build.json` then named
`21d93c89dbaadca6e9b9f3f29d6bce6580261519` with `applicationDirty: false` - the same 40
characters as `git rev-parse HEAD`, on a tree `git status --porcelain` called clean.

```text
revision       21d93c89dbaadca6e9b9f3f29d6bce6580261519, clean tree
staged         1,539 files, 418.1 MB
archive        158.5 MB = 166,220,702 bytes, 1,539 file entries and 72 directory entries,
               every one relative and under Zuuned-0.2.0-win64/
sha256         8AA1F6C8D98260F60534F1F8715E4632F2E2A249AEDE3F8493B9CABE6D2D0FA4
deploy checks  45, failures 0         RESULT: beta Windows artifact produced
archive audit  16 checks, failures 0  RESULT: archive clean - fit to publish
```

Two checks joined the `deploy.ps1` run since the 43 above, and both are the widening the
two sections before this one describe. The staged-folder privacy pass is two checks rather
than one - no identity string in any of the 1,539 staged files, and every staged file
opened and read to its end instead of 668 read and the rest trusted - and one more records
that the finished `.zip` passed the same byte audit member by member. That last check is no
longer only what a person remembers to type: the run that staged this folder handed
`verify-zip.ps1` the same eight build-root probes and eleven ancestor probes it hands its
own folder pass, where the first wiring named two and had passed an archive that a stricter
invocation of the same script failed.

The same file audited a third time from where it now sits on disk - moved, not rewritten -
answered `16 checks, failures 0` against the digest above, which is the evidence that
shipping it did not change it.

What the staged folder proved about itself is the same list as the run above, and one of
its numbers belongs to the machine rather than the artifact: 196 loaded modules resolved as
57 from inside the package, 138 from Windows and 1 from neither, that one again this
desktop's shell utility injected into every process. The two gates run against that same
relinked build report `201 checks, 0 failures, 0 skips` from `tests\win-compat` and
`launches=18 checks=135 failures=0 skips=0 killed=0` from `tests\win-usability`. The repository
gate, re-run once the two privacy scripts named above were tracked and this record had been
written, reads `819 tracked files, 19 checks, 0 failures`, 90 reviewed pictures and 171
drive-letter lines in 33 files - 7 lines and 2 files more than the counter near the top of
this page, which is what printing the number is for.

`gh release list` and `git tag -l` answered empty up to the moment this section was
written, and nothing has been published since: the withdrawn file was never published, so
there is no download of it to un-publish. One path detail for whoever runs this next -
`-Out` names the parent, so `-Out E:\Temp\zuuned-beta\windows` stages into
`E:\Temp\zuuned-beta\windows\windows\Zuuned-0.2.0-win64`.

**Neither script opens a device handle; the application they launch does.** A Zune HD was
bound on this machine for all of it, and every launch in this sitting - the smoke launch
and the eighteen gate launches - auto-connected and browsed it read-only, which is what
every gate launch on this page has always done. Nothing here wrote to, deleted from or read
back from a player, and no capability on `docs/WINDOWS_PORT.md` moved.

## Published, 2026-11-09

The sentence two sections above that reads "nothing has been published" is superseded
here: `git tag -l` and `gh release list` are no longer empty. An annotated tag
`v0.2.0-beta1` points at `68ce562`, the commit that records the digest rather than the
build it describes, and the prerelease
<https://github.com/miemiekurisu/zuuned-modern/releases/tag/v0.2.0-beta1> carries two
assets - `Zuuned-0.2.0-win64.zip` (166,220,702 bytes) and `SHA256SUMS.txt` (90 bytes).

The upload is verified by something other than this repository's own arithmetic: GitHub
recomputed the archive on arrival and reports
`sha256:8aa1f6c8d98260f60534f1f8715e4632f2e2a249aede3f8493b9cabe6d2d0fa4`, the same 64
hex characters the section above measured locally, so what a tester downloads is the
archive the privacy pass audited, byte for byte. `SHA256SUMS.txt` beside it hashes to
`37cf661ed5e15337a0cb45ceb0ed2933081972e5f255760b107fbd5c57c84cff`, which is also the
digest GitHub reports for it.

The release body is bilingual, names Zadig as the first step a tester takes, names what
the folder deliberately does not carry (`ffmpeg.exe` and `ffprobe.exe`, `icuuc.dll`, the
QML debugger, MTPZ credentials), and repeats the closed and open device lists from this
page rather than a shorter claim. Publishing moved no capability: the artifact is still
the folder built from `21d93c8` on a clean tree, its `zuuned.exe --version` still answers
`app 21d93c89dbaa`, and the open list - every model but this Pavo, podcasts, audiobooks,
photos and video on a player, audible PC playback, and this folder on a machine that did
not build it - is what the release itself tells a tester to report against. The two
WITHDRAWN artifacts on the staging drive were never uploaded, so there is still no
download of either to un-publish.

## The published executable carries no icon, 2026-11-10

The owner ran the downloaded beta and came back with one defect.  It is real and it is
measurable: the main application icon is not on the executable.  The tree at `21d93c8` held
no `.rc` and no `.ico` - `git ls-files` names `packaging/windows/zuuned.manifest` as the
only Windows resource file in the repository - so the shipped image carries resource type
24 alone, RT_MANIFEST, with no `RT_GROUP_ICON` for the shell to read.  Explorer, a desktop
shortcut, a Jump List and every thumbnail painted before a window exists therefore give it
the generic application icon.  `setWindowIcon` has been called since before this fork and
is a different fact: a `QIcon` answers what a live window and its taskbar button look like,
and the shell never asks Qt about a file on disk.

Nothing in this document noticed.  45 deploy checks, a 16-check archive audit and a
19-check privacy pass all called that folder healthy, and none of them asked the file the
question a user asks before running anything.

The fix is on `main`, after the tag.  `packaging/windows/make_icon.py` builds
`packaging/windows/zuuned.ico` from the hicolor PNGs this repository already shipped - 16
through 256, with the 512 left out because an ICO size byte caps at 256 and spells that as
0 - and reproduces the same bytes on a second run; `packaging/windows/zuuned.rc` names it;
and twelve lines of `CMakeLists.txt` give it to the app target alone, the CLI probes staying
iconless by design, which doubles as the negative control: `devicetool.exe` still answers
`types=24 groups=0`.  The rebuilt image reports types 3, 14 and 24, one icon group, seven
RT_ICON blobs and declared sizes 16, 24, 32, 48, 64, 128 and 256 at 32 bpp, manifest
untouched, 32,812,032 bytes - and the 32x32 that
`System.Drawing.Icon.ExtractAssociatedIcon` pulls back out of it is byte-identical to
`packaging/icons/hicolor/32x32/apps/zuuned.png`, mean absolute difference 0.0.  A preview
`deploy.ps1` run answers `42 checks, 0 failures` with the three new icon checks green, and
`tests/privacy/run.ps1` on the tree that carries the fix answers 822 tracked files,
19 checks, 0 failures and 91 reviewed pictures: `.ico` counts as media to that gate, so the
icon is registered by exact file name in `tests/privacy/reviewed-media.txt` rather than by
its directory.

**The published tag and its two assets stay exactly where they are.**  A re-packaged folder
is a different archive with a different digest, so it needs its own record here and, if it
is published at all, its own tag - a decision for the owner rather than for a documentation
edit.  The icon changes what the executable looks like and nothing else: no capability on
`docs/WINDOWS_PORT.md` moved, and no player was written to, deleted from or read back from
in this sitting.


## The icon is in the bytes that ship, 2026-11-10

A fix that lives only in a source tree is not an artifact, so the packaging run got to be
the judge, and it justified itself on the first try.  The first `-Release` run after
`d39833a` refused to publish what it had just staged:

```
  FAIL  the build record says be8494ad37da1d30fad47f6a3cc14f2cc64f1279 and the tree is
        d39833a: rebuild from that commit before publishing
  checks      47, failures 1
  RESULT: not a deliverable - 1 failed check(s)
```

That check exists to stop a stale build being published and it fired on the machine that
built the tree, which is the only honest way to learn it works.  The repair is a rebuild:
`cmake --build build --clean-first`, 341 steps, last progress line
`[341/342] Linking CXX executable zuuned.exe`, `EXITCODE=0`, after which
`build/generated/zuuned-build.json` answers `d39833a5e6be4e4f0583c7e40862620081faca32`
with `applicationDirty: false` and `git rev-parse HEAD` returns the same 40 characters.
The archive that rejected run had already written - 166,256,567 bytes at
`70E1F3D889BA54839A1A2462E3C574A70EEEAD25FBF4F210F8C68653A07F4E48` - describes a build
that did not match its own tree, is not a deliverable, and no copy of it left this
machine.  One asymmetry worth recording while it is fresh: `verify-zip.ps1` called that
archive `fit to publish`, because the byte audit asks about privacy and structure and
nothing about provenance, and the verdict that stopped it came from the deploy summary.

The second run passed.

```
  command     packaging\windows\deploy.ps1 -Release -Out E:\Temp\zuuned-beta2
  package     E:\Temp\zuuned-beta2\windows\Zuuned-0.2.0-win64
  staged      1,539 files, 418.2 MB
  archive     E:\Temp\zuuned-beta2\windows\Zuuned-0.2.0-win64.zip
  size        166,256,019 bytes = 158.6 MB
  sha256      B22E3B3A74F0B1C0CA1214FD1E1E4281DBAE57A8BE554F9980BEE1181BE9FAE5
  checks      48, failures 0        RESULT: beta Windows artifact produced
  archive     16 checks, failures 0  RESULT: archive clean - fit to publish
```

**48 is the number to expect from here on.**  The published beta's run reported 45 green
and `Step 'icon'` adds three, so a `-Release` run answering anything else is a step that
did not answer rather than a package that changed.  What the folder proved about itself is
the same list as the run above plus one new question: `devicetool` answering `agree` from
inside it, one page painted from an isolated profile with an empty library
(`onboarding=true tracks=0 wfRows=0`), a self-close with exit 0, 107 binaries inventoried
with all 460 package imports resolved and 258 from Windows, avdevice and avfilter dropped
as command-line-only weight, `icuuc.dll` refused as a system duplicate, the QML debugger
out, no `ffmpeg.exe` and no `ffprobe.exe`, and `1539 staged files, none of them a log,
database, ini, pdb, playlist, audio, image or video file`.

**The icon was then read out of the archive rather than out of the folder**, because the
`.zip` is what a tester downloads.  Its `Zuuned-0.2.0-win64/zuuned.exe` member is
32,812,032 bytes and hashes to
`B3355DF31D0045B555EC5217BC70F1AB6AB2479A86372AFCA644441FDE2274AF`, byte-identical to
`build\zuuned.exe`, so the file the run audited is the file the container carries.  A
second reader, in Python rather than PowerShell, walks the same tree and answers resource
types 3, 14 and 24; one `RT_GROUP_ICON` declaring 16, 24, 32, 48, 64, 128 and 256 at
32 bpp; seven `RT_ICON` blobs whose sizes are exactly the seven the group declares; and,
as the negative control inside the shipped container, `devicetool.exe` at types `24` alone
with no group and no blob.

Two traps from writing that second reader, both mine and neither present in
`deploy.ps1`: `IMAGE_FILE_HEADER` contains a `TimeDateStamp`, so `SizeOfOptionalHeader` is
at `pe + 20` - reading it at `pe + 16` answers 0 and hands back a section table of
nonsense that still parses without complaining; and a resource `DATA` entry's own
structure sits at *resource directory start + offset*, with the RVA as the first word
inside it, so treating the `offset` itself as an RVA yields a small number that lives in no
section at all.  `Convert-PeRvaToOffset` already had both right, which is worth knowing
before assuming the script you inherited is the thing to debug.

Diffed against the published archive member by member, this is measurably the same
product: 1,539 entries in each, none added and none removed, and exactly three changed.
`zuuned.exe` 32,665,600 to 32,812,032 bytes, which is the `.res` payload and the whole of
the archive's 35,317-byte growth; `zuuned-build.json` the same 201 bytes naming the new
revision; and `devicetool.exe` the same 93,184 bytes with a different checksum, which is a
relink of unchanged source and not the icon - the negative control above is that file.

A `SHA256SUMS.txt` sidecar now sits beside it in the format `v0.2.0-beta1` shipped,
lowercase hex, two spaces, the file name, CRLF, 90 bytes, and that sidecar hashes to
`81a1c32eca76aaef84b408d47150b90b0028edcaa5c47d2d3c1faf180829362d`.  For whoever runs this
next: `-Out` names the parent, so the paths above carry a `windows` component of their
own, and the beta1 archive at `E:\Temp\zuuned-beta\windows` was not touched by any of it.
`tests/privacy/run.ps1` on the tree that carries this record answers `822 tracked files,
19 checks, 0 failures` with 91 reviewed pictures and 171 drive-letter lines in 33 files,
the same counters as the section before this one because a record written into an existing
file adds no tracked file.

**Publishing it is the owner's decision, and nothing in this sitting published anything.**
`v0.2.0-beta1` and its two assets stay exactly where they are; a re-packaged folder is a
different archive with a different digest and would need its own tag.  **And no capability
moved.**  The Pavo (Zune HD) bound on this machine throughout the sitting was browsed
read-only by the smoke launch, as every launch on this page has always been; nothing was
written to, deleted from or read back from a player, and the open list - every model but
that one, podcasts, audiobooks, photos and video on a player, audible PC playback, and
this folder opened on a machine that did not build it - is exactly as the section above
left it.