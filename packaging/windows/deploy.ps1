# Zuuned Modern - turn an existing Windows build tree into a folder that runs on its own.
#
#   pwsh -NoProfile -ExecutionPolicy Bypass -File packaging\windows\deploy.ps1
#
# What it does, in order: it copies the built executables and the DLLs that sit beside
# them in the build tree, runs windeployqt over the result so Qt's plugins, QML modules
# and translations come along, pins the Qt lookup at the package with a qt.conf, puts
# the VC++ runtime inside the folder so the app does not need the redistributable,
# stages the FFmpeg command-line tools, the Chinese catalogue, the driver document and
# the licence, reads the executable manifest back out of the file it is compiled into,
# inventories the import table of every binary in the package, checks that each QML
# module the sources import arrived, then launches the staged zuuned.exe once with a
# PATH that names nothing but the package, reads which DLLs the loader actually mapped,
# and closes it.  Then it writes a .zip and a SHA-256 and prints what was and was not
# proved.
#
# What it does not do: it compiles nothing; it installs no driver and opens no device
# handle; it signs nothing and builds no installer.  Artifacts land under -Out, which
# defaults to _tmp (gitignored), so nothing here can be committed by accident.
#
# The one question this exists to answer is "does the folder carry its own dependencies",
# and the only honest answer to that comes from a running process, so the script treats
# the launch as the check rather than as a flourish at the end: the import table says
# what a binary needs, PATH says where a loader is told to look, and only the module
# list of a live process says where the bytes actually came from.
#
# Artifacts are a preview and print themselves as one.  docs\WINDOWS_PORT.md is the
# device status page and packaging proves none of it: what is proved here is where bytes
# come from on one machine, never what a Zune will do with them.
#
# Two PowerShell rules this file obeys on purpose, because breaking them is invisible on
# a console and fatal to a parser: never start a continuation line with an operator (the
# parser ends the statement at the newline and then reports a missing parenthesis), and
# never let a here-string interpolate a path a tester is meant to type.
[CmdletBinding()]
param(
    [string]$Build = '',
    [string]$Out = '',
    [string]$QtDir = 'D:\dev\Qt\6.11.0\msvc2022_64',
    [string]$DepsDir = 'D:\dev\zuuned-deps',
    [string]$FfmpegDir = '',
    [string]$VcpkgBin = 'D:\dev\vcpkg\installed\x64-windows\bin',
    [int]$SmokeDeadline = 240,
    [switch]$NoSmoke
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Build) { $Build = Join-Path $root 'build' }
if (-not $Out) { $Out = Join-Path $root '_tmp' }
if (-not $FfmpegDir) {
    $FfmpegDir = Join-Path $DepsDir 'ffmpeg-master-latest-win64-gpl-shared\bin'
}

$script:checks = 0
$script:failures = 0
$script:notes = New-Object System.Collections.Generic.List[string]

function Step { param([string]$m) Write-Host ('== ' + $m) }
function Pass { param([string]$m) $script:checks++; Write-Host ('  ok    ' + $m) }
function Fail { param([string]$m) $script:failures++; Write-Host ('  FAIL  ' + $m) }
function Note { param([string]$m) $script:notes.Add($m); Write-Host ('  note  ' + $m) }
function Find-Latest {
    # Redist folders, toolset folders and SDK bins accumulate one directory per installed
    # version and none of them says which belongs to this build.  Newest by name is the
    # same answer the toolset already pinned in the CMake cache gives, and reading the
    # version back out of the path is what lets the summary name the runtime that was
    # actually shipped instead of claiming "the VC++ one".
    param([string]$Pattern, [string]$Tail = '')
    foreach ($d in @(Get-ChildItem -Path $Pattern -Directory -ErrorAction SilentlyContinue |
                     Sort-Object Name -Descending)) {
        $candidate = if ($Tail) { Join-Path $d.FullName $Tail } else { $d.FullName }
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return ''
}

function Read-SharedText {
    # The application holds its own session log open through a descriptor, so ask for the
    # loosest share that can still read it, and answer '' when even that is refused: a
    # reader that throws turns a healthy launch into a hang inside the harness.
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $fs = $null
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                              [IO.FileShare]::ReadWrite)
        $sr = New-Object IO.StreamReader($fs, [Text.UTF8Encoding]::new($false))
        try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
    } catch {
        return ''
    } finally {
        if ($fs) { $fs.Dispose() }
    }
}

function Remove-Tree {
    # A staged Qt qml tree is deep enough that a second run of a packager can lose a
    # Remove-Item to it, which then reads as "cannot write the output folder".  Python's
    # rmtree takes the long path and the Windows usability gate already depends on it.
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if (Get-Command python -ErrorAction SilentlyContinue) {
        python -c 'import shutil,sys;shutil.rmtree(sys.argv[1])' $Path
    } else {
        Remove-Item -LiteralPath ('\\?\' + $Path) -Recurse -Force
    }
}

function Copy-Into {
    # Every copy goes through here so a file that did not arrive is a failure at the
    # moment it was meant to land, and not a missing-DLL message on a tester's machine
    # three months later.
    param([string]$From, [string]$To)
    if (-not (Test-Path -LiteralPath $From)) { throw ('missing source: ' + $From) }
    $dir = Split-Path -Parent $To
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Copy-Item -LiteralPath $From -Destination $To -Force
    if (-not (Test-Path -LiteralPath $To)) { throw ('did not arrive: ' + $To) }
}
# ------------------------------------------------------------------------ what and where
Step 'prerequisites'
foreach ($need in @((Join-Path $Build 'zuuned.exe'), (Join-Path $root 'CMakeLists.txt'),
                    (Join-Path $root 'qml'))) {
    if (-not (Test-Path -LiteralPath $need)) {
        Write-Host ('no ' + $need + ' - build first (BUILDING.md), or pass -Build')
        exit 1
    }
}
$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $windeployqt)) {
    Write-Host ('no windeployqt under ' + $QtDir + '; pass -QtDir')
    exit 1
}
$deviceTool = Join-Path $Build 'devicetool.exe'
$qmlDir = Join-Path $root 'qml'
$version = ''
foreach ($line in [IO.File]::ReadAllLines((Join-Path $root 'CMakeLists.txt'))) {
    if ($line -match '^project\(zuuned VERSION ([0-9.]+)') { $version = $Matches[1]; break }
}
if (-not $version) { $version = '0.0.0'; Note 'CMakeLists.txt named no version' }
$name = 'Zuuned-' + $version + '-win64'
$stageParent = Join-Path $Out 'windows'
$stage = Join-Path $stageParent $name
$profile = Join-Path $stageParent 'smoke-profile'
$smokeLog = Join-Path $stageParent 'smoke.log'
Pass ('version ' + $version + '; stage ' + $stage)

# A launch while another Zuuned is running is the one way this script could damage
# something.  The single-instance lock lives inside a profile, so an isolated smoke
# profile is deliberately not refused, and two processes that can both see the player can
# both want it.  A gate may make that judgement about its own device; a packager has no
# business making it about somebody else's sync.
$busy = @(Get-Process -Name zuuned -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited })
if ($busy.Count -and -not $NoSmoke) {
    $pids = ($busy | ForEach-Object { [string]$_.Id }) -join ','
    Write-Host ('a Zuuned instance is already running (pid ' + $pids + ').')
    Write-Host 'Refusing to start a second one: the instance lock is per-profile, so an'
    Write-Host 'isolated profile would not be refused, and the running one may be mid sync.'
    Write-Host 'Close it, or run with -NoSmoke to stage and inventory only.'
    exit 1
}

# bsdtar and dumpbin both take plain paths, and a space in one makes every column of a
# later verdict wrong, so refuse it here rather than ship a miscounted archive.
if ($stage -match '\s') {
    Write-Host ('refusing to package into a path containing spaces: ' + $stage)
    exit 1
}
$drive = (Split-Path -Qualifier $Out).TrimEnd(':')
$free = (Get-PSDrive $drive).Free
if ($free -lt 2GB) {
    Write-Host (('only {0:N1} GB free on {1}: a staged tree and its zip together need 2 GB') -f `
        $free/1GB, (Split-Path -Qualifier $Out))
    exit 1
}
$crtDir = Find-Latest -Pattern 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT'
$dumpbin = Find-Latest -Pattern 'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64' -Tail 'dumpbin.exe'
$mt = Find-Latest -Pattern 'C:\Program Files (x86)\Windows Kits\10\bin\*' -Tail 'x64\mt.exe'
if (-not $dumpbin) {
    Write-Host 'dumpbin.exe is needed to inventory the imports of a staged tree.'
    Write-Host 'Install the MSVC C++ tools; BUILDING.md loads their environment.'
    exit 1
}
Pass ('dumpbin ' + (Split-Path -Parent $dumpbin))
if ($crtDir) { Pass ('app-local CRT from ' + (Split-Path -Parent $crtDir)) } else { Note 'no VC143 CRT redist found' }
if ($mt) { Pass 'mt.exe for the manifest read-back' } else { Note 'no mt.exe, so the manifest check degrades' }
# ----------------------------------------------------------------------------- staging
Step 'stage'
Remove-Tree $stage
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Into (Join-Path $Build 'zuuned.exe') (Join-Path $stage 'zuuned.exe')
Pass 'zuuned.exe'
if (Test-Path -LiteralPath $deviceTool) {
    Copy-Into $deviceTool (Join-Path $stage 'devicetool.exe')
    Pass 'devicetool.exe, so a tester can answer "is the driver bound" without a terminal setup'
} else {
    Note 'no devicetool.exe in the build tree, so the package cannot answer the driver question itself'
}
$buildDlls = @(Get-ChildItem -LiteralPath $Build -Filter '*.dll' -File)
foreach ($d in $buildDlls) { Copy-Into $d.FullName (Join-Path $stage $d.Name) }
Pass ($buildDlls.Count.ToString() + ' runtime DLLs from the build tree: ' +
    (($buildDlls | ForEach-Object { ($_.BaseName -replace '-\d+$','') }) -join ' '))

# windeployqt is a Qt tool and needs Qt's own bin on PATH.  Restore afterwards: the proof
# at the end of this script runs the staged exe with a PATH naming nothing but the staged
# directory, and a Qt bin left in front of it would answer that question with D:\dev\Qt
# before the question had been asked.
Step 'windeployqt'
$pathWas = $env:PATH
$env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
$eapWas = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $wdqOut = & $windeployqt --release --force --qmldir $qmlDir --dir $stage `
        (Join-Path $stage 'zuuned.exe') 2>&1
    $wdqExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $eapWas
    $env:PATH = $pathWas
}
if ($wdqExit -ne 0) {
    Fail ('windeployqt exit ' + $wdqExit)
    foreach ($l in @($wdqOut | Select-Object -Last 25)) { Write-Host ('    ' + $l) }
} else {
    # The exit code of a copy tool is not evidence that the thing needed to open a window
    # is where the loader will look for it.  And where that is depends on how windeployqt
    # was asked: with --dir it lays the plugin categories out at the package root, so the
    # first run of this script produced platforms\qwindows.dll beside zuuned.exe and no
    # plugins folder at all.  The layout is therefore discovered from the staged tree and
    # the same answer writes qt.conf below, because a qt.conf naming a folder the staging
    # never made is a package that works only because Qt also searches the application
    # directory, which is not a fact to ship a stranger's machine on.
    $stageTrim = $stage.TrimEnd('\')
    $qwd = @(Get-ChildItem -LiteralPath $stage -Recurse -Depth 3 -Filter 'qwindows.dll' -File `
             -ErrorAction SilentlyContinue | Select-Object -First 1)
    if (-not $qwd) {
        Fail 'windeployqt exited 0 but no platforms\qwindows.dll landed anywhere in the package'
        $plugRel = ''
        $plugRootFull = $stageTrim
    } else {
        $plugRootFull = Split-Path -Parent $qwd[0].Directory.FullName
        if ($plugRootFull.TrimEnd('\') -ieq $stageTrim) {
            $plugRel = '.'
        } else {
            $plugRel = ($plugRootFull.Substring($stageTrim.Length).Trim('\')) -replace '\\', '/'
        }
        $qtCount = @(Get-ChildItem -LiteralPath $stage -Filter 'Qt*.dll' -File).Count
        $plugCount = @(Get-ChildItem -LiteralPath $plugRootFull -Recurse -Filter '*.dll' -File `
                       -ErrorAction SilentlyContinue |
                       Where-Object { $_.FullName -notmatch '\\qml\\' -and $_.FullName -notmatch '\\tools\\' }).Count
        $qmlCount = @(Get-ChildItem -LiteralPath (Join-Path $stage 'qml') -Directory `
                      -ErrorAction SilentlyContinue).Count
        Pass ('windeployqt staged ' + $qtCount + ' Qt DLLs, ' + $plugCount +
            ' plugin DLLs, ' + $qmlCount + ' QML module directories, plugin root "' +
            $plugRel + '"')
    }
}

# QLibraryInfo was compiled with the prefix this Qt was configured with, so without this
# file the package looks for its plugins and QML modules in D:\dev\Qt on a machine that
# has no D:\dev\Qt, and the failure it produces is a window that never appears rather
# than a message about a path.
$qtConf = New-Object System.Collections.Generic.List[string]
$qtConf.Add('[Paths]')
$qtConf.Add('Prefix = .')
$qtLookup = New-Object System.Collections.Generic.List[object]
$qtLookup.Add([pscustomobject]@{ Key = 'Plugins'; Rel = $plugRel })
$qtLookup.Add([pscustomobject]@{ Key = 'Qml2Imports'; Rel = 'qml' })
$qtLookup.Add([pscustomobject]@{ Key = 'Imports'; Rel = 'imports' })
$qtLookup.Add([pscustomobject]@{ Key = 'Translations'; Rel = 'translations' })
foreach ($entry in $qtLookup) {
    $rel = [string]$entry.Rel
    if (-not $rel) { continue }
    # Only a directory that is really in the package earns a line.  A missing entry on the
    # QML import path costs nothing, and a wrong one invites the next reader to trust a
    # layout the staging did not produce.
    $probe = if ($rel -eq '.') { $stageTrim } else { Join-Path $stage ($rel -replace '/', '\') }
    if (-not (Test-Path -LiteralPath $probe)) { continue }
    $qtConf.Add($entry.Key + ' = ' + $rel)
}
$qtConf.Add('')
$qtConfPath = Join-Path $stage 'qt.conf'
[IO.File]::WriteAllText($qtConfPath, ($qtConf -join "`r`n"), [Text.UTF8Encoding]::new($true))
$qtConfText = Read-SharedText $qtConfPath
if (-not $plugRel) {
    Fail 'qt.conf could not be written because no plugin root was found'
} elseif ($qtConfText -notmatch ('(?m)^Plugins\s*=\s*' + [regex]::Escape($plugRel) + '\s*$')) {
    Fail ('qt.conf says something other than Plugins = ' + $plugRel + ': ' + ($qtConfText -join ' | '))
} elseif ($qtConfText -match '(?m)^Prefix\s*=\s*\.') {
    Pass ('qt.conf pins the Qt lookup at the package: ' + (($qtConf | Where-Object { $_ -match '=' }) -join '; '))
} else {
    Fail 'qt.conf did not land'
}

# The redistributable is a machine-wide install, and "install the VC++ redist first" is a
# support ticket waiting to be filed.  Inside the folder it is one fewer thing to ask.
if ($crtDir) {
    $crtDlls = @(Get-ChildItem -LiteralPath $crtDir -Filter '*.dll' -File)
    foreach ($d in $crtDlls) { Copy-Into $d.FullName (Join-Path $stage $d.Name) }
    Pass ($crtDlls.Count.ToString() + ' MSVC runtime DLLs made app-local')
}
# ----------------------------------------------------------------- the things that are not Qt
Step 'tools, i18n, driver, licence'
foreach ($tool in @('ffmpeg.exe', 'ffprobe.exe')) {
    $src = Join-Path $FfmpegDir $tool
    if (Test-Path -LiteralPath $src) {
        Copy-Into $src (Join-Path $stage ('tools\' + $tool))
    } else {
        Note ('no ' + $tool + ' under ' + $FfmpegDir)
    }
}
if (Test-Path -LiteralPath (Join-Path $stage 'tools\ffmpeg.exe')) {
    Pass 'ffmpeg and ffprobe staged, so Settings, Device, FFmpeg command-line tools has an answer on a fresh machine'
}
# src/main.cpp looks for catalogues in applicationDirPath()/i18n before the resource, so
# this folder is the one a Chinese tester's package needs without a settings change.
$qms = @(Get-ChildItem -LiteralPath $Build -Filter 'zuuned_*.qm' -File -Recurse -ErrorAction SilentlyContinue)
foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $root 'i18n') -Filter 'zuuned_*.qm' -File `
                 -Recurse -ErrorAction SilentlyContinue)) { $qms += $f }
$freshest = @{}
foreach ($q in $qms) {
    $key = $q.Name
    if (-not $freshest.ContainsKey($key) -or $q.LastWriteTime -gt $freshest[$key].LastWriteTime) {
        $freshest[$key] = $q
    }
}
foreach ($key in @($freshest.Keys | Sort-Object)) {
    $q = $freshest[$key]
    Copy-Into $q.FullName (Join-Path $stage ('i18n\' + $key))
    Pass ('catalogue ' + $key + ' from ' + $q.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
}
if (-not $freshest.Count) { Fail 'no compiled catalogue anywhere, so the package cannot answer in Chinese' }
foreach ($doc in @('DRIVER_INSTALL.md', 'check-driver.ps1')) {
    $src = Join-Path $PSScriptRoot $doc
    if (Test-Path -LiteralPath $src) {
        Copy-Into $src (Join-Path $stage ('driver\' + $doc))
    } else {
        Note ('no ' + $doc + ' to stage')
    }
}
if (Test-Path -LiteralPath (Join-Path $stage 'driver\DRIVER_INSTALL.md')) {
    Pass 'driver\ carries the WinUSB binding document and the check that reads it back'
}
foreach ($legal in @('LICENSE', 'COPYING.md')) {
    $src = Join-Path $root $legal
    if (Test-Path -LiteralPath $src) { Copy-Into $src (Join-Path $stage $legal) }
}
$buildJson = Join-Path $Build 'generated\zuuned-build.json'
if (Test-Path -LiteralPath $buildJson) {
    Copy-Into $buildJson (Join-Path $stage 'zuuned-build.json')
    $bj = (Read-SharedText (Join-Path $stage 'zuuned-build.json')) -replace '\s+', ' '
    Pass ('zuuned-build.json names the revision this came from: ' + $bj.Trim())
} else {
    Note 'no zuuned-build.json, so the package cannot say which revision it was built from'
}
# ----------------------------------------------------------------------- the words to read
Step 'README-Windows.txt'
$readme = @(
    'Zuuned Modern VERSION - Windows preview build',
    '=============================================',
    '',
    'What this is',
    '   A portable folder for a music library and a Microsoft Zune.  Nothing installs,',
    '   nothing is registered, no service is left behind: delete the folder and it is gone.',
    '   It is a preview and not a release - there is no installer and no code signing, so',
    '   Windows will warn before it runs, and Settings, General, Open config folder is the',
    '   only place it keeps anything you would want to keep.',
    '',
    '1. Bind the player to WinUSB, once per player',
    '   Windows cannot see a Zune at all until something has bound a driver to it, so the',
    '   player will not appear in this app, or in libusb, until that is done.  Read',
    '     driver\DRIVER_INSTALL.md',
    '   and check what Windows answers with',
    '     driver\check-driver.ps1        or       devicetool.exe',
    '   devicetool prints agree, blocked, absent or contradictory.  blocked means the',
    '   player is in the machine with no usable driver and names the device id to pick.',
    '   This package does not install a driver and never tries to.',
    '',
    '2. Run it',
    '   Double-click zuuned.exe, or start it from a terminal to keep its output:',
    '     zuuned.exe',
    '   zuuned.exe --version prints the build, including the source revision it came from.',
    '   The interface starts in English; Settings, General, Language switches to Chinese.',
    '   Add your music folders when it asks, or from Settings, Music, Watch folders.',
    '',
    '3. Optional: the FFmpeg command line',
    '   Conversion, WMA Lossless and every probe use the FFmpeg library inside this',
    '   package and need nothing extra.  Two things ask for the ffmpeg and ffprobe',
    '   programs instead: a video thumbnail, and a file the built-in library cannot open.',
    '   The tools folder here holds both from the same build as the DLLs, so Settings,',
    '   Device, FFmpeg command-line tools can point at it and nothing has to be downloaded.',
    '   Leaving it alone is fine: the app then says which one is missing rather than',
    '   failing quietly.',
    '',
    '4. If you want to write to a player: authentication',
    '   Reading a player needs no extra file.  Writing to one needs MTPZ credentials, and',
    '   this package deliberately does not carry them - they belong to the user and are',
    '   not part of the build.  Without them zuuned connects, browses and reads the',
    '   library, and reports that MTPZ authentication failed before any transfer starts.',
    '   If you hold credentials you may use, put a file named .mtpz-data in your user',
    '   folder, the same file the Linux client reads from your home directory, and restart.',
    '',
    '5. What has actually been tested on Windows',
    '   By hand, against one Pavo (Zune HD): the WinUSB binding, unplugging and replugging,',
    '   reading the music library, authenticating, sending music and reading a track back',
    '   byte for byte, downloading a track from the player, deleting a track, and one WMA',
    '   Lossless encode that played on the player and came back with the same samples.',
    '   Not tested on Windows: podcasts and audiobooks, sending or reading photos and',
    '   video, deleting photos, video or playlists, every Zune model except that one Zune',
    '   HD, hearing playback through this app on a PC - it has only ever been run muted',
    '   inside a test harness - and this package on a machine other than the one that',
    '   built it.  If you meet one of those, say so: docs\WINDOWS_PORT.md is the list.',
    '')
[IO.File]::WriteAllText((Join-Path $stage 'README-Windows.txt'),
    (($readme -join "`r`n").Replace('VERSION', $version)), [Text.UTF8Encoding]::new($true))
if ((Read-SharedText (Join-Path $stage 'README-Windows.txt')) -match 'WinUSB') {
    Pass 'README-Windows.txt states what has and has not been tested'
} else {
    Fail 'README-Windows.txt did not land'
}
# ------------------------------------------------------------- the manifest, read back
Step 'manifest'
$manifestXml = Join-Path $stageParent 'manifest.xml'
$manifestText = ''
Remove-Item -LiteralPath $manifestXml -Force -ErrorAction SilentlyContinue
if ($mt) {
    $resArg = '-inputresource:' + (Join-Path $stage 'zuuned.exe') + ';#1'
    $outArg = '-out:' + $manifestXml
    $eapWas = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $mt $resArg $outArg 2>&1 | Out-Null
    $ErrorActionPreference = $eapWas
    $manifestText = Read-SharedText $manifestXml
}
if (-not $manifestText) {
    # A resource is stored as XML, so the words are in the image whether or not a tool to
    # lift it out is installed here.
    $manifestText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $stage 'zuuned.exe')))
    Note 'the manifest was read out of the image rather than lifted by mt.exe'
}
if ($manifestText -match 'level=[''"]asInvoker[''"]') {
    Pass 'the manifest asks to run as the invoker, not elevated'
} else {
    Fail 'no asInvoker in the manifest, so a tester may be asked to run as administrator'
}
if ($manifestText -match 'longPathAware[^<]*>\s*true') {
    Pass 'the manifest declares longPathAware, so a deep album tree is not capped at 260 characters'
} else {
    Fail 'no longPathAware in the manifest'
}

# ----------------------------------------------------------------- what each binary needs
Step 'import inventory'
$binaries = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -eq '.exe' -or $_.Extension -eq '.dll' })
$stagedNames = @{}
foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File)) {
    $stagedNames[$f.Name.ToLowerInvariant()] = $true
}
$winDir = (Join-Path $env:SystemRoot 'System32').ToLowerInvariant()
# These belong to the package even on a machine that already has them, because the copy in
# System32 belongs to somebody else's installer: it may be older, it may be missing, and
# on the machine that matters it is a different build of FFmpeg than this one was linked
# against.
$mustShip = '^(qt6|avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale|libmpv|' +
    'sqlite3|gcrypt|gpg-error|libusb|libmp3lame|vcruntime140|msvcp140|vccorlib140|concrt140)'
$missing = New-Object System.Collections.Generic.List[string]
$notLocal = New-Object System.Collections.Generic.List[string]
# Every name any staged binary asks the loader for, kept so the smoke test can tell a
# dependency that resolved in the wrong place from a DLL a third-party shell extension,
# input method or hook injected into our process on this machine.  Only the first is a
# defect in this package, and only the first is a fact about another machine.
$importedNames = @{}
$inPackage = 0
$fromWindows = 0
$apiSet = 0
$eapWas = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
foreach ($img in $binaries) {
    $dump = & $dumpbin /nologo /imports $img.FullName 2>$null
    foreach ($line in @($dump)) {
        if ($line -notmatch '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') { continue }
        $dep = $Matches[1].ToLowerInvariant()
        $importedNames[$dep] = $true
        if ($stagedNames.ContainsKey($dep)) { $inPackage++; continue }
        if ($dep -match '^(api-ms-|ext-ms-)') { $apiSet++; continue }
        $onWindows = Test-Path -LiteralPath (Join-Path $winDir $dep)
        if ($dep -match $mustShip) { $notLocal.Add($img.Name + ' needs ' + $dep); continue }
        if ($onWindows) { $fromWindows++; continue }
        $missing.Add($img.Name + ' needs ' + $dep)
    }
}
$ErrorActionPreference = $eapWas
Pass ($binaries.Count.ToString() + ' binaries inventoried: ' + $inPackage +
    ' resolved inside the package, ' + $fromWindows + ' from Windows, ' + $apiSet + ' api sets')
if ($missing.Count) {
    Fail ($missing.Count.ToString() + ' imports resolve to nothing on this machine: ' +
        (($missing | Select-Object -First 6) -join '; '))
} else {
    Pass 'every import is in the package, in Windows, or an api set'
}
if ($notLocal.Count) {
    Fail ($notLocal.Count.ToString() + ' dependencies are app-local material living elsewhere: ' +
        (($notLocal | Select-Object -First 6) -join '; '))
} else {
    Pass 'nothing FFmpeg, Qt, mpv, SQLite, libusb or MSVC runtime related is left to the machine'
}
# ------------------------------------------------------------------------ the QML side
Step 'qml modules'
# An import table says nothing about a QML module: Qt opens those from directories it
# computes for itself, and a module windeployqt missed paints nothing until the one
# tester who opens that screen meets a blank rectangle.  So the imports the sources make
# are the list, and each one has to exist inside the package.  Zuuned is the exception
# because qt_add_qml_module() compiles this application's own QML into the executable,
# which is also why the build tree needs no qml folder.
$mods = New-Object System.Collections.Generic.List[string]
foreach ($f in @(Get-ChildItem -LiteralPath $qmlDir -Recurse -Filter '*.qml' -File)) {
    foreach ($line in @(Get-Content -LiteralPath $f.FullName)) {
        if ($line -match '^\s*import\s+([A-Za-z][A-Za-z0-9_.]*)') { $mods.Add($Matches[1]) }
    }
}
$absent = New-Object System.Collections.Generic.List[string]
$found = 0
foreach ($m in @($mods | Sort-Object -Unique)) {
    if ($m -eq 'Zuuned') { continue }
    $dir = Join-Path $stage ('qml\' + ($m -replace '\.', '\'))
    if (Test-Path -LiteralPath (Join-Path $dir 'qmldir')) { $found++; continue }
    if (Test-Path -LiteralPath $dir) { $found++; continue }
    $absent.Add($m)
}
if ($absent.Count) {
    Fail ('QML modules the sources import that are not in the package: ' +
        (($absent | Select-Object -First 8) -join ' '))
} else {
    Pass ($found.ToString() + ' imported QML modules staged; Zuuned itself is compiled in')
}
# ------------------------------------------------------------------------ the proof
Step 'smoke'
if ($NoSmoke) {
    Note '-NoSmoke: nothing was launched, so nothing here proves where a DLL resolves'
} else {
    # Presence first and on purpose.  With a bound player attached a launch of the real
    # application auto-connects and browses it, exactly as every usability run has, so
    # this says which kind of launch it is instead of leaving a tester to wonder whether
    # a packager wrote to their player.
    $verdict = 'not run'
    $eapWas = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $dv = & (Join-Path $stage 'devicetool.exe') 2>&1
    $ErrorActionPreference = $eapWas
    $vline = @($dv | Where-Object { [string]$_ -match 'VERDICT=' }) | Select-Object -First 1
    if ($vline) { $verdict = ([string]$vline -replace '.*VERDICT=([a-z]+).*', '$1') }
    Pass ('devicetool run out of the package: ' + $verdict)
    if ($verdict -eq 'agree') {
        Note 'a usable Zune is attached: this launch browses it read-only, as the gates do'
    }

    # The package runs with a PATH that names itself and Windows and nothing else, so a
    # DLL that only resolves because D:\dev\Qt happens to be on this machine is caught
    # here rather than on a tester's machine.  Everything else is the launch the
    # usability gate already makes: its own profile, a null audio output, the QA probe.
    Remove-Tree $profile
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = Join-Path $stage 'zuuned.exe'
    $psi.Arguments = '--page "music" --qa-probe'
    $psi.WorkingDirectory = $stage
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $psi.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $psi.EnvironmentVariables['PATH'] = $stage + ';' + (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
    $psi.EnvironmentVariables['ZUUNED_PROFILE_DIR'] = $profile
    $psi.EnvironmentVariables['ZUUNED_MPV_AO'] = 'null'
    $psi.EnvironmentVariables['QT_LOGGING_TO_CONSOLE'] = '1'
    $psi.EnvironmentVariables['QML_IMPORT_TRACE'] = '1'
    $p = $null
    $outTask = $null
    $errTask = $null
    $seen = ''
    $tick = ''
    $mapped = @()
    $killed = $false
    $exitCode = $null
    try {
        $p = [System.Diagnostics.Process]::Start($psi)
        $outTask = $p.StandardOutput.ReadToEndAsync()
        $errTask = $p.StandardError.ReadToEndAsync()
        # The application writes its own session log, named with the process id, and
        # console.log lands in it, which is the channel the usability gate already trusts.
        # Waiting on the same line here means one mechanism, not two that can disagree
        # about whether a frame was ever painted.
        $logDir = Join-Path $profile 'State\logs'
        $needle = '-' + [string]$p.Id + '-'
        $deadline = (Get-Date).AddSeconds($SmokeDeadline)
        while ((Get-Date) -lt $deadline) {
            if ($p.HasExited) { $seen = 'exited before painting'; break }
            $own = @(Get-ChildItem -LiteralPath $logDir -Filter 'zuuned-*.log' -File -Force `
                     -ErrorAction SilentlyContinue | Where-Object { $_.Name.Contains($needle) })
            foreach ($f in $own) {
                $text = Read-SharedText $f.FullName
                $hit = @(($text -split "`n") | Where-Object { $_ -match '\[qa\] tick' })
                if ($hit.Count) { $seen = 'rendered'; $tick = [string]$hit[0]; break }
            }
            if ($seen -eq 'rendered') { break }
            Start-Sleep -Milliseconds 300
        }
        if ($seen -eq '') { $seen = 'no rendered frame in ' + $SmokeDeadline + 's' }
        # The whole argument of this section.  A module list comes out of the loader of a
        # live process, so it says where the DLLs really are, which is neither where a
        # manifest says they should be nor where PATH claims they will be found.
        if ($seen -eq 'rendered') {
            $p.Refresh()
            try {
                $mapped = @($p.Modules | ForEach-Object { [string]$_.FileName })
            } catch {
                $mapped = @()
                Note ('the module list could not be read: ' + $_.Exception.Message)
            }
        }
        # Closing the window is the shutdown a user takes.  A forced kill is recorded as
        # such because a destroyed process reports an exit code it never chose.
        if (-not $p.HasExited) {
            [void]$p.CloseMainWindow()
            if (-not $p.WaitForExit(30000)) {
                $killed = $true
                try { $p.Kill($true) } catch { $p.Kill() }
                [void]$p.WaitForExit(5000)
            }
        }
        if (-not $killed) { $exitCode = $p.ExitCode }
    } catch {
        $seen = 'launch failed: ' + $_.Exception.Message
    }
    $outText = ''
    $errText = ''
    if ($outTask) { try { $outText = [string]$outTask.Result } catch {} }
    if ($errTask) { try { $errText = [string]$errTask.Result } catch {} }
    [IO.File]::WriteAllText($smokeLog, $outText + "`n" + $errText, (New-Object Text.UTF8Encoding($false)))
    if ($seen -ne 'rendered') {
        Fail ($seen + '; console output in ' + $smokeLog)
        foreach ($l in @((($errText + "`n" + $outText) -split "`r?`n") | Where-Object { $_ } |
                          Select-Object -Last 15)) { Write-Host ('    ' + $l) }
    } else {
        Pass ('the staged package painted a page: ' + $tick.Trim())
        if ($killed) {
            Fail 'the staged package would not close politely and had to be killed'
        } elseif ($exitCode -ne 0) {
            Fail ('the staged package exited ' + $exitCode)
        } else {
            Pass 'the staged package closed on its own with exit code 0'
        }
    }
    $logLeak = New-Object System.Collections.Generic.List[string]
    foreach ($d in @($QtDir, $DepsDir, $VcpkgBin)) {
        if ($d -and (($outText + "`n" + $errText) -match [regex]::Escape($d))) { $logLeak.Add($d) }
    }
    if ($logLeak.Count) {
        Fail ('the launch named a development tree on its output: ' + ($logLeak -join ' '))
    } else {
        Pass 'nothing on the launch output named a development tree'
    }
    if ($mapped.Count -eq 0) {
        Note 'no module list was read, so where each DLL resolved is unproven'
    } else {
        $stageLower = $stage.ToLowerInvariant() + '\'
        $winLower = $env:SystemRoot.ToLowerInvariant() + '\'
        # Only the three trees this package could plausibly have been built against count as
        # a development leak.  The first run of this script failed on a Listary hook DLL
        # from a portable-utility folder: that is a shell extension the owner of this
        # machine injects into every process on it, not something this package imports, and
        # it says nothing a tester with a clean machine would ever learn.  The name test
        # below is the one that can be right.
        $devDirs = @($QtDir, $DepsDir, $VcpkgBin) | Where-Object { $_ } |
            ForEach-Object { $_.ToLowerInvariant().TrimEnd('\') + '\' } | Select-Object -Unique
        $inPackageCount = 0
        $fromWindowsCount = 0
        $foreign = @()
        $fromDev = @()
        foreach ($m in $mapped) {
            $lower = $m.ToLowerInvariant()
            $devHit = $false
            foreach ($d in $devDirs) { if ($lower.StartsWith($d)) { $devHit = $true; break } }
            if ($devHit) { $fromDev += $m; continue }
            if ($lower.StartsWith($stageLower)) { $inPackageCount++; continue }
            if ($lower.StartsWith($winLower)) { $fromWindowsCount++; continue }
            $foreign += $m
        }
        if ($fromDev.Count) {
            Fail ('loaded out of a development tree: ' + (($fromDev | Select-Object -First 4) -join ' '))
        } else {
            Pass ('nothing loaded out of a development tree, across ' + $mapped.Count + ' modules')
        }
        $mustShip = '^(qt6|avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale|libmpv|' +
            'sqlite3|gcrypt|gpg-error|libusb|libmp3lame|vcruntime140|msvcp140|vccorlib140|concrt140)'
        # A module from nowhere that no staged binary asks for is somebody else's injection.
        # One that something in the package does ask for by name - or that is app-local
        # material by name, whether or not the import table survives a case difference -
        # means the loader answered out of a folder this package is quietly depending on.
        $byName = New-Object System.Collections.Generic.List[string]
        $injected = New-Object System.Collections.Generic.List[string]
        foreach ($m in $foreign) {
            $bn = [IO.Path]::GetFileName($m).ToLowerInvariant()
            if ($importedNames.ContainsKey($bn) -or $bn -match $mustShip) { $byName.Add($m) }
            else { $injected.Add($m) }
        }
        if ($byName.Count) {
            Fail ($byName.Count.ToString() + ' modules a staged binary imports by name resolved ' +
                'outside the package: ' +
                ((($byName | ForEach-Object { [IO.Path]::GetFileName($_) }) |
                  Select-Object -First 4) -join ' '))
        }
        $core = @($mapped | Where-Object { [IO.Path]::GetFileName($_) -ieq 'Qt6Core.dll' })
        if ($core.Count -and $core[0].ToLowerInvariant().StartsWith($stageLower)) {
            Pass 'Qt6Core.dll resolved from inside the package'
        } else {
            Fail ('Qt6Core.dll did not resolve from inside the package: ' + ($core -join ' '))
        }
        if ($inPackageCount -lt 10) {
            Fail ('only ' + $inPackageCount + ' modules came out of the package')
        } else {
            Pass ('modules loaded from the package ' + $inPackageCount +
                ', from Windows ' + $fromWindowsCount + ', from neither ' + $foreign.Count)
        }
        if ($injected.Count) {
            Note ($injected.Count.ToString() + ' modules came from neither the package nor ' +
                'Windows while nothing in the package imports them by name, so this machine ' +
                'injected them: ' +
                ((($injected | ForEach-Object { [IO.Path]::GetFileName($_) }) |
                  Select-Object -First 6) -join ' '))
        } elseif (-not $byName.Count) {
            Pass 'every loaded module came out of the package or out of Windows'
        }
    }
}
# ----------------------------------------------------------------------- the archive
Step 'archive'
$stagedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force)
$fileCount = $stagedFiles.Count
# Measure-Object with -InputObject handed an array returns one object whose Length is
# null, so the first run of this script reported "0 files, 0 MB staged" over a 177 MB
# folder.  A plain sum over the files is the honest count.
$sumBytes = [int64]0
foreach ($sf in $stagedFiles) { $sumBytes += [int64]$sf.Length }
$stageBytes = [math]::Round(($sumBytes / 1MB), 1)
Pass ($fileCount.ToString() + ' files, ' + $stageBytes + ' MB staged')
$zip = Join-Path $stageParent ($name + '.zip')
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar)) {
    $cmd = Get-Command tar.exe -ErrorAction SilentlyContinue
    $tar = if ($cmd) { $cmd.Source } else { '' }
}
if (-not $tar) {
    Fail 'no tar.exe, so the staged folder could not be archived'
} else {
    $eapWas = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $tarOut = & $tar -a -cf $zip -C $stageParent $name 2>&1
    $tarCode = $LASTEXITCODE
    $ErrorActionPreference = $eapWas
    foreach ($l in @($tarOut | Select-Object -First 5)) { Write-Host ('    ' + $l) }
    if ($tarCode -ne 0 -or -not (Test-Path -LiteralPath $zip)) {
        Fail ('tar.exe exited ' + $tarCode)
    } else {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $zipped = @($archive.Entries | Where-Object { $_.Name })
            $rooted = @($zipped | Where-Object {
                    $_.FullName -match '^[A-Za-z]:' -or $_.FullName.StartsWith('/') -or
                    $_.FullName.Contains('\') })
            $escapes = @($zipped | Where-Object { $_.FullName -match '(^|/)\.\.(/|$)' })
            $tops = @($zipped | ForEach-Object { ($_.FullName -split '/')[0] } | Sort-Object -Unique)
            if ($rooted.Count) {
                Fail ($rooted.Count.ToString() + ' entries carry an absolute or backslash path')
            } elseif ($escapes.Count) {
                Fail ($escapes.Count.ToString() + ' entries escape the folder they unpack into')
            } elseif ($tops.Count -ne 1 -or $tops[0] -ne $name) {
                Fail ('the archive does not unpack into one folder named ' + $name + ': ' + ($tops -join ' '))
            } elseif ($zipped.Count -ne $fileCount) {
                Fail ('the archive holds ' + $zipped.Count.ToString() + ' files, the stage holds ' +
                    $fileCount.ToString())
            } else {
                Pass ($zipped.Count.ToString() + ' entries, every one relative and under ' + $name + '/')
            }
        } finally { $archive.Dispose() }
    }
}
$zipInfo = $null
$sha256 = ''
if (Test-Path -LiteralPath $zip) {
    $zipInfo = Get-Item -LiteralPath $zip
    $sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    Pass ('archive written: ' + [math]::Round($zipInfo.Length / 1MB, 1) + ' MB')
}
# ---------------------------------------------------------------------------- summary
Step 'summary'
$scope = @(
    'What this run does prove: the staged folder launches on its own with a PATH that',
    'names nothing but itself and Windows, paints a page, loads its modules from inside',
    'the package, and closes politely.',
    'What it does not prove, in short - README-Windows.txt and docs\WINDOWS_PORT.md',
    'carry the long form:',
    '  - device evidence is one Pavo (Zune HD), by hand, on one machine: the WinUSB',
    '    binding, hotplug, the music library read, authentication, one send with byte',
    '    readback, one download, one deletion, one WMA Lossless round trip that played',
    '    on the player;',
    '  - nobody has heard audio out of this application on a PC; every automated run',
    '    asks for a null audio output;',
    '  - podcasts, audiobooks, photos and video on a player, every model but that one',
    '    Zune HD, and this package on a machine that did not build it, are all unproven',
    '    on Windows;',
    '  - no installer, no code signing, no SmartScreen history, and no MTPZ credentials',
    '    inside it.  This is a preview build, not a signed release.'
)
Write-Host ''
Write-Host ('  package     ' + $stage)
Write-Host ('  staged      ' + $fileCount.ToString() + ' files, ' + $stageBytes + ' MB')
if ($zipInfo) {
    Write-Host ('  archive     ' + $zip)
    Write-Host ('  size        ' + [math]::Round($zipInfo.Length / 1MB, 1) + ' MB')
    Write-Host ('  sha256      ' + $sha256)
}
Write-Host ('  from        ' + $Build + ' (nothing was compiled here)')
Write-Host ('  checks      ' + $checks + ', failures ' + $failures)
Write-Host ''
foreach ($l in $scope) { Write-Host ('  | ' + $l) }
Write-Host ''
if ($failures -gt 0) {
    Write-Host ('  RESULT: not a deliverable - ' + $failures + ' failed check(s)')
    exit 1
}
Write-Host '  RESULT: preview Windows artifact produced'
exit 0