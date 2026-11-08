# Building Zuuned Modern

This guide covers the supported native source build and documents the separate
maintainer AppImage workflow with its current public-layout limitations. Run
commands from the repository root unless a step says otherwise.

## Get the source

This public repository includes `libzune/` as ordinary source files. No submodule
initialization is required. Use the bundled revision for this application; do not
replace it with a newer standalone library without testing the integration.

```bash
git clone https://github.com/miemiekurisu/zuuned-modern.git
cd zuuned-modern
```

For Zune authentication, follow [BUILD_WITH_MTPZ.md](docs/BUILD_WITH_MTPZ.md).
You may build without embedded data and use the external file at runtime.
Local library and playback do not require authentication data.

## Install native build dependencies

Qt 6.8 or newer is required. The application also links libusb, libgcrypt,
SQLite, mpv, FFmpeg and LAME. The Arch and Debian package lists belong to the
upstream Linux project and are kept in its
[README](https://github.com/badgeromen/zuuned-linux); the Windows tree is a
different list again and is under [Toolchain and dependencies](#toolchain-and-dependencies)
below.

## Build the native application

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/zuuned
```

CMake builds the included `libzune.a` automatically. If the linker reports an
unrecognized file format after moving the checkout between macOS and Linux,
remove the stale library objects and rebuild:

```bash
make -C libzune clean
cmake --build build --parallel 4
```

The native outputs are:

- `build/zuuned` — the Qt application
- `build/librarytool` — local-library/scanner diagnostics
- `build/playertool` — playback diagnostics
- `build/transcodetool` — media conversion diagnostics

This build uses the current machine's libraries. It is suitable for development
and hardware checks on that machine, but it is not the distributable AppImage.

## Build on Windows

The Windows port compiles and runs natively with MSVC. Its two software gates
pass and it has been driven against a real Zune HD, but which specific device
capabilities have Windows evidence is recorded capability by capability in
[the port status page](docs/WINDOWS_PORT.md) - read that before quoting any
hardware claim, because the list of what is still Linux-only is long. Outputs are
development builds: there is no Windows installer.

Qt 6.8 or newer is required. The reference machine uses the Qt 6.11.0
`msvc2022_64` kit together with the CMake and Ninja that ship in `Qt\Tools`.

### Toolchain and dependencies

- Visual Studio 2022 with the "Desktop development with C++" workload plus a
  Windows SDK.
- Qt including Qt Quick Controls, Qt Quick Dialogs and Qt 5Compat
  GraphicalEffects.
- vcpkg, for the four ports the build consumes:

```powershell
vcpkg install libusb sqlite3 libmp3lame libgcrypt:x64-windows
```

- One extra dependency root for what vcpkg does not carry. `ZUUNED_WIN_DEPS`
  points at it and this layout is expected:

```
zuuned-deps/
      ffmpeg-master-latest-win64-gpl-shared/   win64 gpl *shared* FFmpeg build
      mpv-dev/include/mpv/client.h             mpv development headers
      pcre2/include, pcre2/lib                 PCRE2, static, with POSIX wrapper
      probe/libmpv.lib                         MSVC import library for libmpv
```

libmpv ships as a DLL plus a MinGW import library that MSVC cannot read, so the
MSVC import library is generated once from the DLL export table
(`dumpbin /exports libmpv-2.dll`, rewritten as a `LIBRARY`/`EXPORTS` definition
file) and then:

```powershell
lib /def:libmpv.def /out:libmpv.lib /machine:x64
```

### Configure and build

Load the MSVC x64 environment into the shell first (call `vcvars64.bat` from
`VC\Auxiliary\Build`, or dot-source a helper that imports the variables it
sets), then:

`vcvars64.bat` has to come from the same Visual Studio the build directory was
configured with. CMake pins the compiler's absolute path in the cache, so
`build\CMakeFiles\<version>\CMakeCXXCompiler.cmake` is the authority (here
`Microsoft Visual Studio\2022\Community\...\14.44.35207`, `cl` 19.44); calling a
newer `vcvars64.bat` instead leaves `cl` where it was and moves only `INCLUDE`,
and the newer standard library headers then refuse the older compiler as
`error C2338 ... STL1001: expected MSVC 19.50 or newer` - a wall of
standard-header noise that reads like a source bug and is really the wrong shell.

```powershell
cmake -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_TOOLCHAIN_FILE=D:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=D:/dev/Qt/6.11.0/msvc2022_64 `
  -DZUUNED_VCPKG_PREFIX=D:/dev/vcpkg/installed/x64-windows `
  -DZUUNED_WIN_DEPS=D:/dev/zuuned-deps
cmake --build build
build\zuuned.exe
```

`CMAKE_TOOLCHAIN_FILE` also wires up vcpkg's `applocal.ps1`, which copies the
vcpkg DLLs next to every executable. Nothing else needs copying to run from the
build tree: `CMAKE_PREFIX_PATH` answers Qt, and the FFmpeg, mpv and LAME DLLs
already sit beside the executable. For a folder that runs on a machine with none
of that installed, `windeployqt` by hand is the wrong instrument - see
"Package it for another Windows machine" below.

Every Windows executable built here embeds `packaging/windows/zuuned.manifest`, through
`zuuned_long_paths()` (`CMakeLists.txt:166`). That file is what opts the process out of
`MAX_PATH`: `longPathAware` lifts the 260-character ceiling for the image that declares it
and for no other, and the listener that matters is the C runtime - the route FFmpeg, LAME
and SQLite take to a file, and therefore the route a transcode temporary takes during a
sync.  CMake supplies a manifest of its own for every Windows target and two cannot share
resource ID 1, so the target first passes `/MANIFEST:NO` to stop CMake generating one,
which means this file has to be complete - the `asInvoker` `trustInfo` included - and not a
fragment. Nothing else in the build describes that resource afterwards, so a link is
confirmed by reading it back:

```powershell
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe' `
  '-inputresource:build\zuuned.exe;#1' '-out:manifest.xml'
```

Expect exactly one manifest, `level="asInvoker"`, `longPathAware` true. Two of those checks
in `librarytool paths` (`deep crt:`) fail when the declaration is missing, so the pair is the
gate that says the manifest is still doing something.

Two build-hygiene facts cost time on 2026-10-21 and are worth having written down.
Building without the MSVC environment loaded in the *same* shell does not fail the way
you expect: instead of `LNK1104: ws2_32.lib` it emits around a hundred impossible
`std::thread`/`std::mutex` diagnostics pointing into
`...\Windows Kits\10\Include\...\ScopeCppSDK\vc15\VC\include`, because that SDK folder
shadows the real STL headers. The cure is the environment, not the code.

Second, and the one that can make a passing gate mean nothing: on a localized Visual
Studio an incremental build does not know that a header changed. The Ninja rules carry
`deps = msvc`, so Ninja learns what a `.cpp` includes by reading the `/showIncludes`
echo from `cl`, and it looks for the literal English prefix `Note: including file:`.
A Chinese MSVC writes `注意: 包含文件:` instead, nothing matches, and the dependency
log comes back empty:

```
build> ninja -t deps CMakeFiles\zuuned.dir\src\LibraryService.cpp.obj
CMakeFiles/zuuned.dir/src/LibraryService.cpp.obj: #deps 0, deps mtime ... (VALID)
```

`#deps 0` is the whole story: no translation unit on this machine knows about any
header. Edit `src/library/LibraryPaths.h` - header-only, compiled into the app and into
`librarytool` - and `cmake --build build` recompiles nothing, relinks anyway, and prints
`[5/7] Building CXX object ...mocs_compilation.cpp.obj` because AUTOMOC touched its own
generated file, which makes the run look busy and reassuring. A gate executed on the
result can pass against an executable that predates the change under test, which is the
failure this page has had to record more than once. Nor is there a depfile to fall back
on: the eleven `.d` files under `build/CMakeFiles/d/` belong to CMake's own configure
steps, not to the C++ translation units.

So before writing down any gate counter on Windows, `cmake --build build --clean-first`
- a full rebuild costs a few minutes, and every recorded number in these documents came
from one. When you only need one target, touch each `.cpp` that includes the changed
header instead: source-file dependencies are tracked correctly. Fixing the underlying
problem needs an English `cl` or a Ninja that parses localized notes; `VSLANG=1033` was
tried on the 2026-10-21 machine and did not change the output language.

`CMakeLists.txt` keeps one source tree for both platforms: the D-Bus/Mpris
service and the POSIX-only install rules are skipped on Windows, and
`compat/win/` is built once as `zuuned_wincompat` and force-included into every
shared translation unit through `posix_shim.h`, so libzune and the transcode
layer keep the POSIX surface they expect without `#ifdef` sprawl.

### Package it for another Windows machine

The build tree runs where the toolchain lives: it contains no Qt DLL at all,
because `CMAKE_PREFIX_PATH` leaves Qt's `bin` on `PATH` for a developer shell.
`packaging/windows/deploy.ps1` is the counterpart of
`packaging/macos/deploy.sh`, and like that one it is a script rather than a CMake
install rule, because `cmake --install` deliberately does nothing here.

```powershell
cmake --build build --clean-first   # the script packages this tree
pwsh -NoProfile -ExecutionPolicy Bypass -File packaging\windows\deploy.ps1 -Out E:\Temp\zuuned-package
```

`-Out` defaults to `_tmp`, gitignored; give it a drive with 2 GB free, because a
staged folder and its zip exist at the same time. It stages `zuuned.exe`,
`devicetool.exe` and the DLLs beside them, runs `windeployqt`, makes the VC++
runtime app-local, and adds the FFmpeg command-line tools, the Chinese catalogue,
`DRIVER_INSTALL.md` and `check-driver.ps1`, the licence and a generated
`README-Windows.txt` that states what has and has not been tested. Then it checks
instead of assuming: `mt.exe` reads the manifest back out of the compiled
executable, `dumpbin /imports` runs over every binary in the folder, each QML
module the sources `import` has to exist inside it, and the last word is a launch
of the staged `zuuned.exe` with a `PATH` naming nothing but the package, which is
where a DLL that only resolved because `D:\dev\Qt` is on this machine's `PATH` gets
caught. A green run on 2026-11-08 reported `31 checks, 0 failures` over 1,556 files
with `58` modules mapped from inside the package and `135` from Windows, and wrote
`Zuuned-0.2.0-win64.zip` with its SHA-256.

Two findings changed the script. `windeployqt --dir` lays the plugin categories at
the package root - `platforms\qwindows.dll` beside `zuuned.exe` and no `plugins`
folder anywhere - so `qt.conf` has to say `Plugins = .`; the file is written from
the layout the staging actually produced rather than from a recipe, because a
`qt.conf` naming a folder that does not exist still launches, Qt also searches the
application directory, and a package that works for that reason alone is a trap for
the next reader. And the module list of a live process includes DLLs this machine
injects into every process - a portable shell utility's hook read as a
development-tree leak - so a module from somewhere else is a failure only when
something in the package imports it by name; otherwise it is recorded as an
injection, which is a fact about this desktop and not about a tester's.

The script compiles nothing, installs no driver, opens no device handle, signs
nothing and builds no installer. `docs/WINDOWS_PORT.md` stays the device status
page: a package proves where bytes came from on one machine, never what a Zune
will do with them.

### Windows gate

```powershell
pwsh -NoProfile -File tests/win-compat/run.ps1
```

It rebuilds the compat layer and runs the software probes: temporary
directories, `mkstemps`, UTF-8 and long paths, large files, directory
descriptors, pipes, `mmap`-backed publishing and wide `stdio`. It also pins how
a descriptor is resolved: `fstat()` works on a handle the shim never created and
on a descriptor the shim never created - the second is what `QFile::handle()`
hands the audiobook probe on Windows - and the CRT invalid-parameter guard is in
force without any `main()` asking for it. It also pins how a path is spelled for the
shell `system()` runs, because `cmd.exe` decodes its command line with the ANSI code
page while the app hands UTF-8. Expected result:
`201 checks, 0 failures, 0 skips`. Since 2026-10-21 it also pins `stat()`
identity: `st_dev`/`st_ino` are the volume serial and file index, distinct files
never share one, and it survives a rename - the artwork cache's
"this image is its own source" check is built on that.

The bash suites in `ci/check.sh` need pkg-config and a POSIX shell, so on
Windows the library core is covered by the native tools instead:
`build\librarytool.exe names`, `build\librarytool.exe customize` (the latter
rebuilds a pre-Customize database and runs the real v0/v2 to v9 migration chain,
including the pre-migration backup) and `build\librarytool.exe paths` - 73 checks
over the filename, path-comparison, playlist-resolution and playlist-decoding rules
the Windows port depends on plus the once-only rule for a watch folder that contains
a junction, the reserved-stem and trailing-dot rule a playlist export needs, m3u written
in UTF-16 with a BOM, and one path past 260 characters written twice - once through Qt
and once through `_wfopen()`, which is the pair that pins the embedded manifest,
with the expectations branched per platform so the same file gates a
Linux run without changing a Linux answer. `build\librarytool.exe cover` adds 33 checks that
pin the album-art resolution order and the music-store request pacing without opening a
socket, and `net` the transport identity. All five probes pass; none touches a
device, and all five run inside `tests\win-usability\run.ps1`'s tools phase.

Scanning is covered the same way, with a fixture built from the cases Windows
historically gets wrong: a plain MP3, a WMA file named `.mp3`, a folder named in
Chinese characters, and a track nested deep enough that its absolute path is 277
characters.

```powershell
python tests\win-usability\make_scan_fixture.py build\wtest\fx\music `
  --ffmpeg D:\dev\zuuned-deps\ffmpeg-master-latest-win64-gpl-shared\bin\ffmpeg.exe
build\librarytool.exe scan build\wtest\fx\music build\wtest\fx-library.db
python tests\win-usability\inspect_scan_db.py build\wtest\fx-library.db `
  --expect-tracks 4 --expect-codepoints 0x6F22,0x8A5E --min-longest-path 200
```

Expected: the fixture prints `created=4` with a longest path of about 277
characters, the scan exits 0 reporting `4 tracks added`, and the inspector reports
`fingerprints=4 probe_failures=0 codepoint_path_rows=1 OVERALL=ok`. The stored path
is a little shorter than the one on disk because the folder was given relative.
Each of those numbers has been wrong at some point during the port.

### Windows usability gate

Whether the built app is actually usable, as opposed to merely compiling: one
instrumented launch per page (plus the device page with no device attached), the five
settings tabs, both pinned chrome states, the compact Now Playing island reached by
resizing a live window, the four command-line probes, the transcode layer making real
ASF, MP4 and MP3 files instead of only probing them, the same clip decoded once forced
through software and once through the accelerator Settings asks for with the decoder's first
pixel format read back off it, two launches into a profile folder
named in Chinese, a read-only account of what the machine can see about a Zune, a seek
on the null audio driver, a muted audible run that has to bind WASAPI,
single-instance refusal, and before-and-after proof that the
real profile stayed untouched.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\win-usability\run.ps1
```

Expected: `launches=18 checks=123 failures=0 skips=0 killed=0` and `OVERALL=ok` -
the check count is whatever the gate pins today and moves when a pin is added, so the
number to hold it to is `failures=0`. It
needs the same MSVC environment as the build and compiles its own capture tool into
`build\win-usability\`. `-Only fixture,pages` and similar run a subset - the phase
list is comma-separated, and because the scratch tree is rebuilt on every run a page
phase has to be paired with `fixture`. Each launch gets its own
`ZUUNED_PROFILE_DIR`, its own `ZUUNED_LOCK_SUFFIX` and `ZUUNED_MPV_AO=null`, and every
one ends with a capture of the live window plus the app's own `--qa-probe` account of
what it decided to show, because a Qt Quick window can come up with a clean stderr
and paint nothing.

The gate opens no USB device handle and calls no libzune function, so it certifies
nothing about the wire. What a Windows build does on the wire is the hand test in
`docs/WINDOWS_PORT.md`: one Pavo Zune HD bound to WinUSB, opened, MTPZ-authenticated,
read, and sent audio that came back. The `what is plugged in` phase reads presence
only - what libusb enumerated, what the PnP manager reports, and whether the two
agree. `tests/win-usability/README.md` lists the phases and what they do not cover.

### Path encodings on a non-English Windows

`compat/win/` decodes the narrow path it is handed as **UTF-8 first**, and as the
**system ANSI code page** only when the bytes are not valid UTF-8. A caller that
passes UTF-8 - which is what a POSIX filename already is, on every platform - is
therefore never second-guessed, and that is the whole reason the rule is
UTF-8-first rather than ACP-only. A library with its own encoding policy still
has to be fed explicitly rather than inherit anything: FFmpeg documents UTF-8
file names on Windows, which is why
`src/library/AudioCopyFingerprint.h` passes `path.toUtf8()` and not
`QFile::encodeName(path)`. On a GBK machine the difference is visible - a CJK song
name becomes bytes FFmpeg cannot resolve, and the track is then indexed with no
copy fingerprint and no error to show for it.

That same ACP convention is why no shared source may hand a user path to CRT
`access()`: it is ANSI-only, so a folder with a non-ASCII name reads as
unreadable, and mode bit 1 is not even a valid argument - `_access()` runs the
invalid parameter handler, which `__fastfail()`s the process rather than setting
`errno`. `X_OK` is therefore `0` in `compat/win/posix_shim.h`, and
`LibraryScanner::accessibleDirectory()` lets the shim's UTF-16 `opendir()` prove
read access. The shim is not where a name gets lost: handed UTF-8 it decodes
strictly and keeps every character, so the loss belongs to a caller that
encoded with the local 8-bit codec before the call.

Anything with its own encoding policy has to be named explicitly rather than inherit
the ACP convention, and SQLite is the second one found after FFmpeg. Its Windows VFS
decodes a path name with `CP_UTF8` and never consults the ANSI code page, so handing it
`QFile::encodeName(path)` asked for a different file than the folder Qt had just made:
a two-byte GBK sequence for a common Han character is very often valid UTF-8 as well
(`U+4E00` is `D2 BB`, which reads back as `U+04BB`). Under a profile folder named in
Chinese - a Chinese Windows user name is enough - `spoken/` appeared, `spoken.db` did
not, and every podcast and audiobook operation answered "Cannot open spoken-media
database", while `library.db` in the same folder opened fine because `LibraryDb` passes
`toUtf8()`. `src/spoken/SpokenStore.cpp` passes `toUtf8()` now, which is the same
bytes on Linux, and the `cjkprofile` phase of the usability gate launches twice into a
Han-named profile folder and reads the store's schema back with
`tests/win-usability/inspect_spoken_db.py`.

The section above used to close by claiming that local 8-bit was the correct
thing to hand the shim. It is not, and that claim cost a scan that reported a
healthy empty library. Every `QFile::encodeName()`
call in the tree has now been read rather than assumed, and four of them were
bugs: `LibraryScanner`, `ArtworkPipeline`, `ArtistImageService` and
`OnlineAlbumArtService` were handing `stat()`, `opendir()` and `link()` the local
8-bit form. Measured on this CP936 machine against five scan roots - ASCII,
Chinese, Hebrew, Hangul, emoji - the scanner found its one track under the ASCII
and Chinese roots and nothing under the other three: `stat()` failed, the copy
fingerprint returned false, and the scan exited 0 looking complete. CP936 covers
Han, so a Chinese host cannot see this defect at all; an English host meets it on
its first Hebrew album. All four now pass `toUtf8()`, which is the identical byte
string on Linux. The only remaining caller sits in the non-Windows branch of
`#if defined(Q_OS_WIN)` in `src/diagnostics/SupportReport.cpp`, where local 8-bit
is right because it reads ASCII sysfs paths on Linux.

The `fixture` phase is what makes that measurable: `make_scan_fixture.py
--extra-roots` lays out one scan root per script, and the gate scans each into its
own database and asserts one track each, so a root that works cannot mask a root
that does not - which is exactly how the Chinese root used to hide the other three.

The one known loss is still inside Qt itself: with a profile folder the ANSI code
page cannot spell, Qt's own shader/pipeline cache cannot open its file even though
the directory exists, so such a profile pays for a cold shader cache on every
start. It is recorded by the gate, not asserted, and no data is involved.

### Incremental builds and the compiler's include note

The note is the line Ninja cannot read, and on 2026-10-20 it cost a long chase
after a crash that did not exist. What is actually on this machine, measured:

- `cl.exe` writes its localised "including file" note in whatever code page the
  console it is attached to uses. Under `chcp 936` the note begins `d7 a2 d2 e2
  3a 20`; under `chcp 65001` the same note begins `e6 b3 a8 e6 84 8f 3a 20`.
  `VSLANG=1033` changes neither, byte for byte, which is why that variable appears
  in no recipe that works.
- CMake captures the prefix once, at configure time, by running `cl` and decoding
  the note as UTF-8 with the ANSI code page as a fallback. On this machine it was
  handed GBK bytes, decoded them as UTF-8, and stored the result - so
  `build\CMakeFiles\rules.ninja` says `msvc_deps_prefix = ` followed by
  `e5 a8 89 e3 84 a6 e5 89 b0 3a 20`, mojibake of mojibake. It matches neither
  form `cl` can emit, so `deps = msvc` never fires.
- The result is silent, not noisy. `ninja -t deps` answers zero dependencies for a
  translation unit that demonstrably includes headers, an edit to that header
  rebuilds nothing, and the link reuses the object already on disk. Deleting
  `build\CMakeFiles\<version>` and configuring again stores the same bytes: this
  is a property of the machine, not a cache to clean.

That is what killed the first Windows device session, and it is worth spelling out
because it looked exactly like a device bug. A member had been added to
`DeviceService` in the middle of its data section, so every member declared after
it moved. The translation unit that owns `DeviceService` rebuilt; another, which
includes its header and constructs a lambda over the same object, kept the object
file it already had. The link joined both layouts into one `zuuned.exe` without a
word, and at the moment a device arrived code compiled against the old layout
incremented the reference count of the `QString` one member away from the one it
meant: `0xc0000005` inside `Qt6Core.dll`, at `lock inc dword ptr [r8]` with
`r8 = 0xf`. Both objects were correct for the header each saw. Nothing in the
application was wrong, and reading the application could not have shown otherwise -
`cmake --build build --clean-first` fixed it, 428 files removed, 329 rebuilt.

Upstream has had this shape for years: Ninja issues 2205 and 1766, CMake issue
24068, whose merge request is the `cmcldeps` work that removes the whole class of
problem. Until that lands here, or the machine enables "Use Unicode UTF-8 for
worldwide language support", or English compiler resources are installed, force a
full rebuild after a header-only change:

```powershell
cmake --build build --clean-first
```

A stale object looks exactly like a fix that did not work. When a change appears
to have no effect, prove the object is new before believing the diagnosis.

### Settings and profile location

A Windows Zuuned profile lives in files, not the registry: settings are the ini
at `%APPDATA%\Zuuned\Zuuned.ini`, while the library, cache, logs and the
single-instance lock stay under `%LOCALAPPDATA%\Zuuned\Zuuned`. On first start the
app copies any values an earlier build left in `HKCU\Software\Zuuned\Zuuned` into
the ini and logs `[settings] imported N value(s)`.

Set `ZUUNED_PROFILE_DIR` to an absolute directory to keep the whole profile in one
folder instead - a portable folder you carry with you, or a throwaway one for a
test run. That is the Windows equivalent of relocating `$HOME` on Linux, and it is
needed because `QStandardPaths` here asks the shell known folders and ignores
`LOCALAPPDATA`/`APPDATA` overrides. The settings ini moves with it, to
`<profile>\Zuuned\Zuuned.ini`, and the app logs
`[profile] every profile file goes to "..."` so the effective location is never a
guess. Your music, video, picture and document folders are deliberately left where
the platform puts them. Unset, every path is as described above, on either
platform.

### Device access

The Linux udev rules do nothing here, and a Zune plugged into a clean Windows
machine has no driver at all: Windows enumerates it, marks it
`CM_PROB_FAILED_INSTALL` (problem 28) and leaves it out of
`libusb_get_device_list()` entirely, so libusb reports nothing rather than
something useful.

zuuned therefore asks the PnP manager as well as libusb (`src/DevicePresenceWin.cpp`,
through `cfgmgr32`). A player that is on the bus but unopenable is named as such —
status "Zune plugged in, but no driver zuuned can use" instead of the silent
"waiting for a zune…" — and the device id to select in Zadig is printed where the
Linux udev command would have been. Nothing is installed for you: bind WinUSB with
the Zadig utility and replug. That binding step and everything behind it remain
untested on Windows.

`build\devicetool.exe` asks the same question in one command, with no app, no UI and
no device handle:

    build\devicetool.exe

It prints both accounts and a verdict. On a machine holding a driver-less player the
expected answer is `blocked`:

    libusb_enumerated=1 libusb_devices=7 libusb_hotplug=0 libusb_sighted=0
    os_supported=1 os_present=1 os_usable=0 os_problem=28
    VERDICT=blocked

## Build on macOS (Apple Silicon)

There is no macOS branch and no macOS source tree. One tree builds Linux, Windows
and macOS, and the platform choice is made where it has to be: `if(WIN32)` /
`if(APPLE)` in `CMakeLists.txt`, `Q_OS_MACOS` in the C++, `Qt.platform.os` in the
QML. Nothing is duplicated and nothing is merged back, because there is only one
line of development.

### Toolchain and dependencies

The Apple toolchain is clang from the Command Line Tools (`xcode-select
--install`). The SDK already supplies `regex.h`, `sqlite3` and `m`, so unlike the
Windows tree this one needs no POSIX compatibility layer and no PCRE2, and unlike
Linux it needs no udev.

```bash
xcode-select --install
brew install cmake ninja pkg-config libusb libgcrypt lame mpv ffmpeg \
  qtbase qtdeclarative qt5compat qtsvg qtshadertools qttools
```

Those six formulae are the whole Qt surface this tree uses, and Homebrew now ships
Qt as exactly that: separate formulae, no `qt` umbrella keg. `qtbase` carries Core,
Gui, Network, Concurrent and OpenGL; `qtdeclarative` carries Quick, QML, the Quick
Controls and the Quick Dialogs; `qt5compat` is the `Core5Compat` component the
language file loader uses and the `Qt5Compat.GraphicalEffects` import almost every
`qml/` card makes; `qtshadertools` builds the `FastBlur` behind the settings
backdrop; `qttools` is `lupdate`/`lrelease` for `i18n/`; and `qtsvg` is the icon
engine the theme loads at run time rather than link time, so it is missing from
`find_package` and still required. `brew install qt` remains a metapackage that
pulls all of this plus `qtwebengine`, which `zuuned` does not link — `rg WebEngine
CMakeLists.txt` answers nothing — so a `qtwebengine` left at an older version than
its upgraded dependencies is untidy and irrelevant, and a Mac with no `qt` umbrella
keg at all builds normally.

One Homebrew upgrade hazard is worth its own paragraph because it silently invalidates
evidence. A Qt upgrade replaces `/opt/homebrew/Cellar/qtdeclarative/<version>/...`,
which is what the recorded `deps` of every QML-cache and moc object name, and Ninja
does not treat a vanished dependency as dirty: it reports `no work to do`, the old
executable stays in place, and it then loads the *new* dylibs through
`/opt/homebrew/opt/qtbase` and `/opt/homebrew/opt/qtdeclarative`. `otool -L
build-macos-release/zuuned.app/Contents/MacOS/zuuned` is how to tell — the `current
version 6.12.0` it prints belongs to the link, not to the headers it was compiled
against. After any Qt upgrade, `cmake --build build-macos-release --clean-first`, and
prefer a cold `-B <new-dir>` configure when the point being proven is that a fresh
checkout builds. Both were done here after the 2026-11-06 upgrade that moved
`qtdeclarative`, `qtsvg`, `qtwebchannel`, `qtserialport`, `qtpositioning` and
`qttools` onto 6.12.0 beside `qtbase` 6.12.0_2: the cold tree configured and built 311
targets with `build_exit=0`, and the rebuilt in-tree Release passed the twelve-launch
page sweep again with the same figures and the same `pos=1906 → 3343 → 4807` against
`dur=6000` from a Launch Services launch.

Homebrew installs under `/opt/homebrew` on Apple silicon and `/usr/local` on
Intel, and neither is on the Apple linker's or the preprocessor's default search
path. So the bare `-lgcrypt` / `-lmp3lame` / `-lavformat` spellings that resolve
against a Linux distribution's system tree resolve to nothing here, and the Apple
linker exits on an unresolved symbol rather than warning. `CMakeLists.txt` handles
this once, in the `APPLE` branches: `find_library`/`find_path` (which do search the
Homebrew prefix) hand back the resolved file for gcrypt, lame, libusb, every
FFmpeg component and libmpv, and the include directories come from `find_path`
too. Linux keeps its bare names.

One link difference has no dependency reason behind it. `transcodetool` reaches
`zune_probe()` through a weak reference so the gate still links without libzune:
GNU ld binds an undefined weak symbol to zero and lets the executable stand, the
macOS linker refuses it outright (measured here — the plain weak reference and
`weak_import` fail the same way). So `transcodetool` gets
`-Wl,-U,_zune_probe`, naming the one expected absence instead of loosening every
undefined symbol with `-undefined dynamic_lookup`, which would also hide a real
missing library. `zuuned_probe_metadata()`'s NULL check still answers `-1`, which
is what this standalone gate does on Linux.

### Configure and build

```bash
cmake -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos --parallel 8
open build-macos/zuuned.app
```

`libzune/Makefile` has always had a Darwin branch and builds `libzune.a` against
the Homebrew libusb and libgcrypt through `pkg-config`; the abandoned
`driverkit_usb.c` backend is deliberately not compiled (see [Device
access](#device-access) below). CMake drives that `make` and links the archive, as
it does on Linux.

The application is a bundle, and on this platform that is not packaging polish
but the only way the link works: `qt_add_qml_module(zuuned URI Zuuned)` writes the
compiled QML module into a `Zuuned/` directory beside the executable, APFS is
case-insensitive by default, and so a flat `-o zuuned` asks the linker to replace
that directory with an ordinary file — `ld: open() failed, errno=21 (Is a
directory) for 'zuuned'` after every object has already compiled. A bundle puts
the binary at `build-macos/zuuned.app/Contents/MacOS/zuuned`, which cannot
collide, and carries the `Info.plist`. The CLI probes stay flat on purpose: no QML
module, nothing to collide with.

```
build-macos/zuuned.app         the Qt application
build-macos/librarytool        local-library / scanner diagnostics
build-macos/playertool         playback diagnostics
build-macos/transcodetool      media conversion diagnostics
build-macos/devicetool         one-command "what is plugged in"
```

The bundle is unsigned and ad-hoc at best — this project has no Apple Developer
licence, and a USB DriverKit extension would need one. `codesign -s -
build-macos/zuuned.app` is an ad-hoc signature: it satisfies nothing Gatekeeper
would otherwise demand and buys nothing across a download. Running the executable
from a terminal is the supported local launch and is what every measurement here
used. A Launch Services launch (Finder, or `open`) works on this machine too, which
is newer than that sentence and is what the next section is about.

### Package it for another Mac

The build-tree bundle runs on the machine that built it and nowhere else, because its
binary still resolves QtCore, libusb, libmpv and every FFmpeg component through
`/opt/homebrew`. `packaging/macos/deploy.sh` is the counterpart of the Linux AppImage
builder, and it is a script rather than a CMake install rule because this platform has
no install rules to extend -- a `.desktop` entry, an hicolor tree and a udev rule all
have no counterpart here.

```bash
cmake --build build-macos-release --parallel 8   # the script packages this tree
./packaging/macos/deploy.sh                      # --no-smoke skips the launch
```

It stages a copy, deletes the build-tree `qt.conf` (this application's QML is compiled
into the executable by `qt_add_qml_module`, and a bundle with the file removed paints
every page), runs `macdeployqt -qmldir=qml -no-strip -no-codesign`, drops the plugins
whose Qt framework does not exist here -- Homebrew Qt ships no QtPdf, so
`libpdfquickplugin.dylib` and `libqpdf.dylib` go, and the symlink that pointed at the
first of them goes with them because a dangling link stops `xattr -c` outright -- gives
every Mach-O the name it now answers to (`install_names_rewritten=42`: `macdeployqt`
rewrites every *reference* and leaves the 42 frameworks it copied out of
`qtdeclarative` still naming themselves `/opt/homebrew/...`), deletes the
`/opt/homebrew/lib` rpath CMake left on the executable, and then refuses the bundle
unless all 1936 load commands land on a file that is inside it. It builds the bundle
icon from the hicolor PNGs the Linux package already installs -- the deepest committed
tile is 512, so that one tile is enlarged for the 1024 slot -- signs every Mach-O
ad-hoc and inside-out, `signed_macho=173`, then the 52 frameworks and the bundle, and
launches what it published before it writes an archive.

**The bundle is assembled and signed in `$TMPDIR`, not in `_tmp`, and copied back when
it is finished.** That is measured necessity. This repository sits under
`~/Documents`, whose Desktop-and-Documents sync is owned by iCloud's file provider, and
that provider stamps `com.apple.FinderInfo` onto every *package* directory -- `.app`,
`.framework`, `.bundle` -- within about four seconds of one appearing, and writes the
attribute back within two seconds of deleting it. `codesign` refuses to sign a bundle
wearing it, so signing in place races a background daemon. Three runs of this script lost
that race: the first died at the bundle root, which `codesign --verbose=4` named as
`Disallowed xattr com.apple.FinderInfo found on .../zuuned.app` although it had been
asked about a file, and the next two only after every Mach-O inside had signed, at a
`QtQuickControls2.framework` directory. A `.framework`, a `.bundle` and a plain directory
created here next to three identical ones under `$TMPDIR` acquired the attribute in the
first place and never acquired it in the second. `_tmp` keeps the logs, the finished
bundle and the archives, and the `rsync -a` that carries the bundle back copies no
extended attributes, so what lands there is the signed bytes and nothing else -- for a
few seconds. The same provider watches `_tmp`, and once it has stamped the bundle the
signature that passed in the scratch tree is refused in place: `codesign --verify --deep
--strict` names all 52 frameworks, each one `Disallowed xattr com.apple.FinderInfo`, and
without `--verbose` the same question answers `resource fork, Finder information, or
similar detritus not allowed`. That is an inspection refusal, not a broken bundle -- the
stamped copy launches and paints `[qa] tick1 page=music` with no mapped image from
`/opt/homebrew` -- but it decides where a signature gets checked: verify an archive, or
a copy taken outside the synced folders, and judge the bundle in `_tmp` by running it.
The archives are written from the scratch tree for the same reason: `hdiutil` and
`ditto` copy attributes the way `rsync` does not, and the metadata fork
`ditto` writes by default stored every one of them as an in-place AppleDouble file --
the first zip this script built carried 2,526 `._*` entries, all of them
`com.apple.provenance`, this machine's own bookkeeping. Hence `--norsrc`, and hence the
`zip_appledouble_entries=0` the script reads back out of the archive it just wrote.

The launch that finishes the script is the structural proof, and it reads the copy in
`_tmp` because that is the copy a tester runs: an isolated `ZUUNED_PROFILE_DIR` seeded
with `onboardingComplete=true`, `DYLD_PRINT_LIBRARIES=1`, and the same `--qa-probe`
answer the usability gates ask for -- `[qa] tick1 page=music ...`, with 1182 images
mapped and not one of them from `/opt/homebrew` or `/usr/local`. The count of mapped
images is asserted before the absence is, because an ignored `DYLD_PRINT_LIBRARIES`
would otherwise make "nothing outside the bundle" true of a run that mapped nothing.
Two archives land in `_tmp`, gitignored: `Zuuned-0.2.0-arm64.dmg` and the same bundle as
a zip, 91 MB and 80 MB, because a disk image and a browser download are different
Gatekeeper experiences and a zip survives the transport tools that flatten a dmg.
`spctl` answers `rejected`, which is what an ad-hoc unnotarised bundle means, and is
recorded rather than treated as a failure.

What a tester does about that, and what was measured about the archives themselves, is
worth knowing before anyone is handed a file. `xattr -dr com.apple.quarantine` on the
bundle, or right-click → Open, is the path; there is no Apple Developer licence behind
this project, so there is nothing to notarise and no promise a fresh macOS will
volunteer to like what it is. Both archives were opened the way a tester opens them: the
dmg mounted read-only, and its bundle answered zero AppleDouble entries, zero
`com.apple.FinderInfo`, `codesign --verify --deep --strict` valid and satisfying its
Designated Requirement, and a launch from inside the read-only volume rendering
`[qa] tick1 page=photos` with 99 of 1208 mapped images coming from `/Volumes` and none
from Homebrew; the zip unpacked by `unzip` left no `._` file on disk and gave the same
verdict. Packaging is about structure and nothing else: the bundle carries its own
libraries, which is a statement about this script and not about a device -- the bundle
that produced every device measurement is the build-tree one, and the device facts
themselves are [MACOS_PORT.md](docs/MACOS_PORT.md) -- and the Documents-folder consent
in the next section is still a consent a tester has to answer.

### First launch, and the privacy consent a Finder launch needs

A `.app` that lives under `~/Documents` is behind macOS privacy protection for the
Documents folder, and the two ways of starting it meet that gate differently.
Starting `build-macos/zuuned.app/Contents/MacOS/zuuned` from a terminal inherits
whatever the terminal already has, which is why every measurement here could run
before any consent existed. Starting the same bundle through Launch Services —
Finder double-click, or `open build-macos/zuuned.app` — makes the app its own TCC
subject, and until the answer exists the `open()` that resolves a path under
`~/Documents` does not return. Measured on this machine: the process starts, writes
five session-log lines, then the main thread waits in `QWaitCondition::wait` for
`QQmlThread`, which is parked in `__open_nocancel` inside `populateFileSet` →
`QDirListing` while Qt locates the `Zuuned` module's `qmldir`. No window ever
appears, nothing crashes, and `sample` names no path. `sandboxd` is the only thing
that says why:
`tcc_server_message_request_authorization(kTCCServiceSystemPolicyDocumentsFolder)
failed: Error Domain=kTCCErrorDomain Code=5 "server error: Failed to create
Attribution Chain from message."` — an ad-hoc-signed bundle with no developer team
cannot build the attribution chain a prompt needs, so no dialog is ever offered and
the syscall simply never comes back.

Two facts were measured to rule out the wrong explanations. Copying the bundle to
`/tmp` and launching it there hangs identically, so this is the Launch Services path,
not one path string. And a `QML_IMPORT_TRACE=1` run stops after
`importExtension: loaded ":/qt/qml/Zuuned/qmldir"`, which is the same instruction the
stack is sitting on — the app is not slow, it is blocked.

What to do about it, in the order a tester should try:

1. Answer the consent when it is offered. On a machine where the responsible process
   can be resolved, System Settings → Privacy & Security → Files and Folders lists the
   Documents-folder ask; granting it is what turned the hang into a rendered window on
   this machine, and the same launch then reported `[qa] splash page=music` with
   mpv and the USB watcher both up. Re-run after the grant, through `open` alone, both
   answers came back: a seeded profile reported `page=music onboarding=false tracks=2
   wfRows=3 errs=0` and its screenshot is a native window, and a brand new empty
   profile reported `onboarding=true tracks=0 wfRows=0` and painted the first-run page
   — Chinese UI, and the step that reads `running on macOS` rather than a Linux or
   Windows sentence.
2. Or keep the build outside every protected folder — a tree under `/Users/<name>/dev`
   or `/tmp` is asked about nothing, and the terminal launch needs no consent either way.

A real user's first run will be asked for more than the Documents folder once watch
folders point at `~/Music`, `~/Pictures` and `~/Movies`; expect those prompts too, and
treat a windowless zuuned as an unanswered prompt rather than a hung player. A stray
`AddressBook` authorisation request was logged beside the Documents one and comes from
a system framework, not from this application, which opens no contacts API.

### Interface language

`packaging/macos/Info.plist.in` declares `CFBundleLocalizations` as `en` and
`zh-Hans`. macOS matches that list against the user's preferred languages and
hands the result to `CFPreferences`, which is where `QLocale::system()` reads — a
bundle that declares only a development region is told it is an English app even
on a Chinese macOS, and Settings → Language → "system" silently resolves to
English there while it follows the desktop on every other platform.

### Device access

The Mac build runs `libzune/src/usb_libusb.c` over Homebrew libusb, the same
userspace path Linux uses: no kernel extension, no DriverKit extension, no udev
rule, nothing installed, nothing to sign. `libzune/driver/` and its
`driverkit_usb.c` backend stay out of the build. `driverkit_usb.c` was the
abandoned original approach — an out-of-process DEXT is the officially supported
way to own a USB interface on a modern macOS, and it cannot be loaded unless
Apple's own notarisation signed it, which this project cannot obtain. The source
stays in the tree at `libzune/src/driverkit_usb.c`, unread by either build.

`src/DevicePresence.cpp` therefore answers from libusb alone on this platform, the
way Linux does; the `cfgmgr32` presence pass is Windows-only, and so the "plugged
in but no driver" card cannot fire here yet. Settings → Device says macOS needs no
driver rather than naming the Windows-only Zadig utility.

Two tools answer the interface question before the application is involved. The
three-platform one is `devicetool`, which with a player attached answers
`libusb_enumerated=1 libusb_hotplug=1 libusb_sighted=1` and `VERDICT=agree`; the
macOS-specific one is `packaging/macos/claimprobe.c`, which opens the player, asks
who holds interface 0, prints the endpoints and claims the interface -- the Mac
counterpart of `packaging/windows/check-driver.ps1`:

```bash
cc -Wall -Wextra -o /tmp/claimprobe packaging/macos/claimprobe.c \
    $(pkg-config --cflags --libs libusb-1.0)
/tmp/claimprobe                # VERDICT=claim_ok, measured 2026-11-06
```

Run either tool outside a sandbox. A seatbelt shell denies USB, libusb then
enumerates **zero** devices, and `VERDICT=absent` from inside one is the sandbox
answering, not the bus.

What a Mac has actually done to a Zune -- the claim, the ZMDB read, MTPZ
authentication, one music send judged byte for byte, a deletion -- is
[MACOS_PORT.md](docs/MACOS_PORT.md). Nothing in this section is device evidence, and no
device result measured on one platform carries over to another.

### What the Mac build is measured to do

Software only, measured on Apple silicon (macOS 26, Qt 6.12, FFmpeg 9, libusb
1.0.30): the application starts, the Chinese catalogue loads, every page and every
Settings tab paints, one file plays through CoreAudio, the CLI gates that need no
device pass, and the transcoder decodes on the GPU through VideoToolbox and falls
back to the CPU when told to or when the system refuses a session.

That sentence is two sweeps, not one. `build-macos/` (Debug) and
`build-macos-release/` (Release, `cmake -B build-macos-release -G Ninja
-DCMAKE_BUILD_TYPE=Release`) each launched once per page and once per Settings tab —
twelve launches apiece — in an isolated `ZUUNED_PROFILE_DIR` seeded with two tone
files, one video and one photo, and all twenty-four reported `state=alive errs=0`
with the `[qa] splash` page agreeing with the last tick and the same figures:
`tracks=2 photos=1 albums=1 wfRows=3 wfDefaults=3`. `librarytool names`, `customize`,
`paths` and `videofloor` answer `ALL PASS (0 failures)` from the Release tree too, and
Release `devicetool` answers `VERDICT=absent` — the honest answer for this machine,
which has no player attached. The Release binary is `Mach-O 64-bit executable arm64`
carrying `flags=0x20002(adhoc,linker-signed)` and no TeamIdentifier: it is arm64-only,
so nothing here has run under Rosetta, and it runs because it is a local build, not
because anything signed it.

Sound and discipline were measured in the same two configurations. With
`ZUUNED_MPV_AO=coreaudio` and `--qa-play`, `playing=true` carried
`pos=1906 → 3292 → 5041` against `dur=6000` from the Release tree, and
`pos=1905 → 3345 → 4807` from the Launch Services launch of that same bundle — so
CoreAudio is reached both from a terminal and from a Finder-shaped launch, and nobody
has listened to either. A second instance pointed at the same profile prints
`another instance is already running — this one is bowing out` and leaves the first one
alone, which is the profile lock living where `ZUUNED_PROFILE_DIR` put it. A podcasts
launch leaves a readable `spoken/spoken.db` in that profile — `items` plus
`spoken_parent`, `integrity_check ok` — so the spoken store opens on this platform
rather than beside the profile. Sample the ticks early: a six-second tone is over
before the third tick, and an ended track reports `playing=false pos=0 dur=0`, which is
the end of the file rather than a silent player.

Two things a first Mac build gets wrong if you believe it blindly, both recorded
because they were found by running rather than reading:

- A refused VideoToolbox *encoder* session used to fail the whole transcode.
  `transcode/libav_transcode.c` now retries the open on `libx264` on this platform
  only, so the encoder choice degrades the way the decoder choice already did
  instead of ending the job.
- `librarytool paths` had never been run on POSIX. Two of its 67 checks carried a
  Windows expectation while one of them was titled "…on Linux", and the other
  disagreed with what the code has always produced; both branches are pinned now.
  The Linux line would have failed the same two, and `ci/check.sh` has never
  called `paths`, so nothing was covering it.

Run these gates outside any sandbox. A seatbelt/sandboxed shell denies
VideoToolbox and the audio devices, which looks exactly like a broken encoder and
a broken player and is neither.

Every figure above was taken with no player attached, and none of it says anything
about a Zune. The device measurements for this platform -- the interface claim, the
ZMDB read, MTPZ authentication, one music send judged byte for byte, a deletion --
are [MACOS_PORT.md](docs/MACOS_PORT.md), which is also where a macOS hardware claim
has to be quoted from rather than inferred from this page.

## Build the tester AppImage

The exporter supports both the private submodule checkout and the public
repository's included libzune source. Public release commits contain
`PUBLIC_LIBZUNE.json`, recording the exact library commit and Git tree. The
exporter requires a clean public checkout and verifies its included library
against that tree before archiving the captured application commit. If you alter
libzune, commit it in your library repository and update the manifest to match.

Do not invent a library revision or mark modified input clean to bypass that
check. Official downloads also require the private authentication input, as
described in [the release architecture](docs/RELEASE_ARCHITECTURE.md).

The release workflow builds inside pinned Debian 13 images. This gives the
package a known Qt 6.8.2 and glibc baseline while keeping build dependencies off
the development desktop. Docker is used only to compile and inspect the bundle:
the containers receive no USB device, personal library, audio device or desktop
session. Zune hardware testing still runs on a physical host.

Install Docker first. If the current account can access Docker directly, omit
`sudo` from the following commands. The checked-in images use tester UID/GID
`1000:1000`; if the checkout owner differs, follow the UID instructions in the
detailed container README rather than changing only `--user` below.

Build the two images whenever either Dockerfile or the runtime smoke script
changes:

```bash
sudo docker build --platform linux/amd64 \
  -f packaging/containers/Dockerfile.debian13 \
  -t zuuned-tester-debian13 packaging/containers

sudo docker build --platform linux/amd64 \
  -f packaging/containers/Dockerfile.runtime-debian13 \
  -t zuuned-runtime-debian13 packaging/containers
```

The package must come from committed, clean application and library states.
This prevents an AppImage whose filename identifies one revision while its
contents came from another.

```bash
test -z "$(git status --porcelain --untracked-files=normal --ignore-submodules=none)"
test -z "$(git -C libzune status --porcelain --untracked-files=normal)"

repo=$(pwd)
release_root=$(mktemp -d /tmp/zuuned-release-XXXXXXXX)
snapshot="$release_root/source"

node packaging/containers/snapshot.mjs "$snapshot"
grep -qx 'export ZUUNED_SOURCE_DIRTY=false' "$snapshot/snapshot.env"
grep -qx 'export ZUUNED_LIBZUNE_DIRTY=false' "$snapshot/snapshot.env"

mkdir -p packaging/appimage-tools
tools=$(realpath packaging/appimage-tools)

sudo docker run --rm --platform linux/amd64 --user 1000:1000 \
  --env LANG=C.UTF-8 --env ZUUNED_APPIMAGE_TOOLS=/tools \
  --mount "type=bind,src=$snapshot,dst=/work" \
  --mount "type=bind,src=$tools,dst=/tools" \
  --workdir /work zuuned-tester-debian13 \
  bash -lc 'source /work/snapshot.env; bash packaging/build-appimage.sh'

mkdir -p "$repo/build-appimage/dist"
cp -a "$snapshot/build-appimage/dist/." "$repo/build-appimage/dist/"
find "$repo/build-appimage/dist" -maxdepth 1 -type f -printf '%f\n' | sort
```

The first package run may download the AppImage deployment tools. Their hashes
are pinned in `packaging/appimage-tools.lock`; the builder verifies cached and
downloaded copies before executing them.

The output set under `build-appimage/dist/` contains:

- `Zuuned-<version>-g<revision>-<UTC>-x86_64.AppImage`
- the matching `.AppImage.sha256`
- `.AppImage.build.json` with the exact app and libzune revisions
- `.AppImage.environment.txt` with the build environment and glibc requirements
- `.AppImage.tools.lock` with the deployment-tool hashes

Verify the exact candidate from inside its output directory, because the
checksum file contains the AppImage's basename:

```bash
cd build-appimage/dist
candidate=$(find . -maxdepth 1 -type f -name 'Zuuned-*.AppImage' \
  -printf '%T@ %f\n' | sort -nr | head -1 | cut -d' ' -f2-)
sha256sum --check "$candidate.sha256"
chmod +x "$candidate"
"./$candidate" --version
cd ../..
```

## Run the isolated package gate

This launches the actual AppImage in Xvfb with software OpenGL, disposable XDG
profiles, no network and an empty USB view. It checks packaged libraries,
onboarding, local music import, artwork rendering, desktop file pickers,
playlist import, diagnostics export and normal shutdown. It cannot certify real
GPU/audio/USB behavior.

```bash
candidate=$(realpath "build-appimage/dist/$candidate")
runtime_evidence="$release_root/runtime"
mkdir -p "$runtime_evidence"

sudo docker run --rm --platform linux/amd64 --user 1000:1000 \
  --network none --shm-size 512m \
  --mount type=tmpfs,dst=/sys/bus/usb/devices \
  --mount "type=bind,src=$candidate,dst=/candidate.AppImage,readonly" \
  --mount "type=bind,src=$runtime_evidence,dst=/evidence" \
  zuuned-runtime-debian13 /candidate.AppImage /evidence

cat "$runtime_evidence/latest-runtime.txt"
```

The detailed evidence contract and failure interpretation live in
[`packaging/containers/README.md`](packaging/containers/README.md). A passing
container gate does not replace playback, transfer and USB recovery tests on a
physical machine.

## Run the AppImage

```bash
chmod +x Zuuned-*.AppImage
./Zuuned-*.AppImage
```

If FUSE is unavailable:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./Zuuned-*.AppImage
```

If a file manager says “archive not supported,” it has handed the executable to
an archive application instead of running it. Use the commands above or enable
the file's executable permission in its Properties dialog. Code inside an
AppImage cannot change a desktop's file association before the package starts.

Install the two USB rules only on the physical test system; the application can
also install these through its USB permission card:

```bash
sudo install -m 644 packaging/68-zuuned.rules /etc/udev/rules.d/68-zuuned.rules
sudo install -m 644 packaging/68-zuuned.rules /etc/udev/rules.d/72-zuuned.rules
sudo udevadm control --reload-rules
```

Replug the Zune after installing the rules. See
[`packaging/TESTER_INSTALL.md`](packaging/TESTER_INSTALL.md) for tester-facing
launch, diagnostics and reporting instructions.
