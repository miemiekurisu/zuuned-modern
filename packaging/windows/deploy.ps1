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
# -Release changes the deliverable, not the checks.  The two FFmpeg command-line
# programs and the two DLLs only they need stay out of it, because those programs are a
# separate download and the app already points at that download from Settings, Device
# when a feature wants one; nothing byte-identical to something in C:\Windows\System32
# is carried; the QML debugger stays out; the licence notices stop being optional; and a
# last pass reads every staged path and every staged byte for the account name, the
# machine name and the folders this build was made in, and fails the run on a hit.
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
    [switch]$NoSmoke,
    [switch]$Release
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

function Remove-StagedFile {
    # A file copied a second ago can still be held open by the minifilter scanning it, and a
    # delete that fails on that handle says nothing about the file.  Retry for a few
    # seconds, keep the long-path prefix Remove-Tree needs, and answer instead of throwing,
    # so the caller still reports a Pass or a Fail a reader can trust.
    param([string]$Path)
    for ($try = 0; $try -lt 20; $try++) {
        if (-not (Test-Path -LiteralPath $Path)) { return $true }
        try { Remove-Item -LiteralPath ('\\?\' + $Path) -Force -ErrorAction Stop } catch { }
        if (-not (Test-Path -LiteralPath $Path)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return (-not (Test-Path -LiteralPath $Path))
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

# A beta artifact has to name the revision it came from, and a tree with uncommitted
# work in it names a revision that is not the bytes inside the folder.  The build
# records the same answer in build.json; saying it here, before anything is copied,
# is what makes "commit first" a check rather than a habit, because dirty is a fact
# a tester can act on and a preview cannot hide it the way a beta cannot.
if ($Release) {
    $revision = ''
    $porcelain = @()
    try {
        $revision = [string](& git -C $root rev-parse --short HEAD 2>$null)
        $porcelain = @(& git -C $root status --porcelain 2>$null)
    } catch { }
    if ($LASTEXITCODE -ne 0) { $revision = '' }
    if ($revision) { Pass ('beta of revision ' + $revision) } else { Note 'no git revision' }
    if ($porcelain.Count) {
        Fail ('the tree this artifact is built from carries ' + $porcelain.Count.ToString() +
            ' uncommitted change(s): ' + (($porcelain | Select-Object -First 4) -join '; '))
    } else {
        Pass 'clean tree, so the revision inside the package is the source of these bytes'
    }
}
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
if ($Release) {
    # They are a separate program with their own download, and the application already
    # answers "which one is missing" with a link to it, so a release folder that shipped
    # them would be bundling somebody else's product rather than depending on it.
    Note 'release: ffmpeg.exe and ffprobe.exe are not staged. Settings, Device, FFmpeg command-line tools names the download instead.'
} else {
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
foreach ($legal in @('LICENSE', 'COPYING.md', 'packaging\windows\THIRD-PARTY-NOTICES.txt')) {
    $src = Join-Path $root $legal
    $leaf = Split-Path -Leaf $legal
    if (Test-Path -LiteralPath $src) {
        Copy-Into $src (Join-Path $stage $leaf)
    } elseif ($Release) {
        Fail ('no ' + $leaf + ' to stage: a release folder may not ship without its licence notices')
    } else {
        Note ('no ' + $leaf + ' to stage')
    }
}
if (Test-Path -LiteralPath (Join-Path $stage 'tools\ffmpeg.exe')) {
    # The notices file speaks for a release folder.  A preview folder that does carry the
    # command-line pair says so in its own copy rather than shipping a page that describes
    # a different folder than the one on the tester's disk.
    $notices = Join-Path $stage 'THIRD-PARTY-NOTICES.txt'
    if (Test-Path -LiteralPath $notices) {
        $add = "`r`nNote on this particular folder`r`n------------------------------`r`n" +
            'This one is a preview folder, so it does additionally carry' + "`r`n" +
            '  tools\ffmpeg.exe   tools\ffprobe.exe' + "`r`n" +
            'from the same FFmpeg build as the libraries above and under the same' + "`r`n" +
            'GPL-3.0-or-later terms.  A release folder does not carry them.' + "`r`n"
        [IO.File]::AppendAllText($notices, $add, [Text.Encoding]::ASCII)
        Pass 'the preview notices say they carry the command-line pair a release folder omits'
    }
}
$buildJson = Join-Path $Build 'generated\zuuned-build.json'
if (Test-Path -LiteralPath $buildJson) {
    Copy-Into $buildJson (Join-Path $stage 'zuuned-build.json')
    $bj = (Read-SharedText (Join-Path $stage 'zuuned-build.json')) -replace '\s+', ' '
    Pass ('zuuned-build.json names the revision this came from: ' + $bj.Trim())
    # The stamp is written when the build compiles its version object, so a clean tree at
    # packaging time does not prove the binary agrees with it.  The first release run here
    # passed the clean-tree check on one revision over a build record naming an older one
    # and marking itself dirty.  Read both back and compare them: a tester quotes the stamp
    # in a crash report and never sees this console.
    $stampRev = ''
    $stampDirty = 'unknown'
    try {
        $rec = $bj | ConvertFrom-Json
        $stampRev = [string]$rec.application
        if ($null -eq $rec.applicationDirty) { $stampDirty = 'unknown' }
        elseif ($rec.applicationDirty) { $stampDirty = 'true' }
        else { $stampDirty = 'false' }
    } catch { }
    $headRev = ''
    try { $headRev = [string](& git -C $root rev-parse HEAD 2>$null) } catch { }
    if ($LASTEXITCODE -ne 0) { $headRev = '' }
    $truth = ''
    if (-not $stampRev) {
        $truth = 'the build record names no application revision, so nothing compares it to the tree'
    } elseif (-not $headRev) {
        $truth = 'no git revision to compare the build record against'
    } elseif (-not $headRev.StartsWith($stampRev, [StringComparison]::OrdinalIgnoreCase)) {
        $truth = ('the build record says ' + $stampRev + ' and the tree is ' +
            $headRev.Substring(0, 7) + ': rebuild from that commit before publishing')
    } elseif ($stampDirty -eq 'true') {
        $truth = ('the build record marks ' + $stampRev + ' as built from a dirty tree')
    }
    if (-not $truth) {
        Pass ('the build record and the tree name the same revision ' + $stampRev)
    } elseif ($Release) {
        Fail $truth
    } else {
        Note $truth
    }
} else {
    Note 'no zuuned-build.json, so the package cannot say which revision it was built from'
}
# ------------------------------------------------------------- what a release folder drops
Step 'release trim'
if (-not $Release) {
    Note 'preview: the command-line pair and the two DLLs only it needs stay staged'
} else {
    # Measured rather than assumed.  dumpbin over every binary in the last package named
    # avdevice-63.dll and avfilter-12.dll from ffmpeg.exe and ffprobe.exe and from nothing
    # else in the folder, so once the programs are gone the libraries have no user left.
    $drop = @(Get-ChildItem -LiteralPath $stage -Filter '*.dll' -File |
              Where-Object { $_.Name -match '^(avdevice|avfilter)-[0-9]+\.dll$' })
    foreach ($d in $drop) {
        $size = [math]::Round($d.Length / 1MB, 1)
        if (-not (Remove-StagedFile $d.FullName)) {
            Fail ('could not remove ' + $d.Name)
        } else {
            Pass ('dropped ' + $d.Name + ' (' + $size + ' MB): only the command-line programs named it')
        }
    }
    # windeployqt answers an import table by copying whatever it can find, and Qt6Core
    # names icuuc.dll, which since Windows 10 version 1903 is an operating-system file that
    # windeployqt happily takes out of System32.  A Windows system binary is not this
    # project to redistribute, and the machine that receives the folder has its own.
    $sysDir = Join-Path $env:SystemRoot 'System32'
    $osCopies = New-Object System.Collections.Generic.List[string]
    foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File |
                     Where-Object { $_.Extension -eq '.dll' -or $_.Extension -eq '.exe' })) {
        $twin = Join-Path $sysDir $f.Name
        if (-not (Test-Path -LiteralPath $twin)) { continue }
        if ((Get-Item -LiteralPath $twin).Length -ne $f.Length) { continue }
        $h1 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
        $h2 = (Get-FileHash -LiteralPath $twin -Algorithm SHA256).Hash
        if ($h1 -ne $h2) { continue }
        if (-not (Remove-StagedFile $f.FullName)) {
            Fail ('could not remove the operating-system copy of ' + $f.Name)
        } else {
            $osCopies.Add($f.Name)
        }
    }
    if ($osCopies.Count) {
        Pass ('refused to carry ' + $osCopies.Count + ' file(s) identical to a Windows system file: ' +
            ($osCopies -join ' '))
    } else {
        Pass 'nothing in the package is a copy of a Windows system binary'
    }
    # The QML debugger listens for a client.  Nothing a stranger downloads needs it and its
    # presence is a capability rather than a feature, so it stays out of a release.
    $dbg = Join-Path $stage 'qmltooling'
    if (Test-Path -LiteralPath $dbg) {
        Remove-Tree $dbg
        if (Test-Path -LiteralPath $dbg) { Fail 'the QML debugger folder is still in the package' }
        else { Pass 'the QML debugger is out of the release folder' }
    } else {
        Pass 'no QML debugger folder had been staged'
    }
}
# ----------------------------------------------------------------------- the words to read
Step 'README-Windows.txt'
# One text for both kinds of folder, because the two differ in three sentences and nobody
# reading a README knows which flag made theirs.  A release says plainly what it left out
# and why; a preview says where the extra tools sit.  Nothing here is a placeholder the
# writer has to remember to substitute: the version is in the string already.
$flavour = 'preview'
if ($Release) { $flavour = 'beta' }
$readmeTitle = 'Zuuned Modern ' + $version + ' - Windows ' + $flavour + ' build'
$readme = New-Object System.Collections.Generic.List[string]
$readme.Add($readmeTitle)
$readme.Add([string]::new('=', $readmeTitle.Length))
$readme.Add('')
$readme.Add('What this is')
$readme.Add('   A portable folder for a music library and a Microsoft Zune.  Nothing installs,')
$readme.Add('   nothing is registered, no service is left behind: delete the folder and it is')
$readme.Add('   gone.  It is a ' + $flavour + ' build and not a signed product - there is no')
$readme.Add('   installer and no code signing, so Windows will warn before it runs, and')
$readme.Add('   Settings, General, Open config folder is the only place it keeps anything of')
$readme.Add('   yours that you would want to keep.')
if ($Release) {
    $readme.Add('   This folder carries the application and the libraries it needs in order to')
    $readme.Add('   run, and deliberately nothing else: no FFmpeg command-line programs, no')
    $readme.Add('   copy of a Windows system file, and none of the music, photographs, video,')
    $readme.Add('   logs, databases or scratch files belonging to whoever built it.')
    $readme.Add('   THIRD-PARTY-NOTICES.txt names every part of it and the licence it arrived')
    $readme.Add('   under, including where to read the source for the parts the GPL requires.')
}
$readme.Add('')
$readme.Add('1. Bind the player to WinUSB, once per player')
$readme.Add('   Windows cannot see a Zune at all until something has bound a driver to it, so')
$readme.Add('   the player will not appear in this app, or in libusb, until that is done.  Read')
$readme.Add('     driver\DRIVER_INSTALL.md')
$readme.Add('   and check what Windows answers with')
$readme.Add('     driver\check-driver.ps1        or       devicetool.exe')
$readme.Add('   devicetool prints agree, blocked, absent or contradictory.  blocked means the')
$readme.Add('   player is in the machine with no usable driver and names the device id to pick.')
$readme.Add('   This package does not install a driver and never tries to.')
$readme.Add('')
$readme.Add('2. Run it')
$readme.Add('   Double-click zuuned.exe, or start it from a terminal to keep its output:')
$readme.Add('     zuuned.exe')
$readme.Add('   zuuned.exe --version prints the build, including the source revision it came')
$readme.Add('   from.  The interface starts in English; Settings, General, Language switches')
$readme.Add('   to Chinese.  Add your music folders when it asks, or from Settings, Music,')
$readme.Add('   Watch folders.')
$readme.Add('')
$readme.Add('3. Optional: the FFmpeg command line')
$readme.Add('   Conversion, WMA Lossless and every probe use the FFmpeg library inside this')
$readme.Add('   package and need nothing extra.  Two things ask for the ffmpeg and ffprobe')
$readme.Add('   programs instead: a video thumbnail, and a file the built-in library cannot')
$readme.Add('   open.')
if ($Release) {
    $readme.Add('   This folder deliberately does not carry those two programs.  They belong to')
    $readme.Add('   the FFmpeg project and are distributed by its own builds under the GNU GPL,')
    $readme.Add('   and nothing inside this folder has to run either of them.  Settings, Device,')
    $readme.Add('   FFmpeg command-line tools names the page to download them from and says which')
    $readme.Add('   one is wanted when a feature asks for one.  Leaving it alone is fine:')
    $readme.Add('   conversion and every probe keep working, a video thumbnail simply stays')
    $readme.Add('   blank and the app says why rather than failing quietly.')
} else {
    $readme.Add('   The tools folder here holds both from the same build as the DLLs, so')
    $readme.Add('   Settings, Device, FFmpeg command-line tools can point at it and nothing has')
    $readme.Add('   to be downloaded.  Leaving it alone is fine: the app then says which one is')
    $readme.Add('   missing rather than failing quietly.')
}
$readme.Add('')
$readme.Add('4. If you want to write to a player: authentication')
$readme.Add('   Reading a player needs no extra file.  Writing to one needs MTPZ credentials,')
$readme.Add('   and this package deliberately does not carry them - they belong to the user and')
$readme.Add('   are not part of the build.  Without them zuuned connects, browses and reads the')
$readme.Add('   library, and reports that MTPZ authentication failed before any transfer')
$readme.Add('   starts.  If you hold credentials you may use, put a file named .mtpz-data in')
$readme.Add('   your user folder, the same file the Linux client reads from your home')
$readme.Add('   directory, and restart.')
$readme.Add('')
$readme.Add('5. What has actually been tested on Windows')
$readme.Add('   By hand, against one Pavo (Zune HD): the WinUSB binding, unplugging and')
$readme.Add('   replugging, reading the music library, authenticating, sending music and')
$readme.Add('   reading a track back byte for byte, downloading a track from the player,')
$readme.Add('   deleting a track, and one WMA Lossless encode that played on the player and')
$readme.Add('   came back with the same samples.')
$readme.Add('   Not tested on Windows: podcasts and audiobooks, sending or reading photos and')
$readme.Add('   video, deleting photos, video or playlists, every Zune model except that one')
$readme.Add('   Zune HD, hearing playback through this app on a PC - it has only ever been run')
$readme.Add('   muted inside a test harness - and this package on a machine other than the one')
$readme.Add('   that built it.  If you meet one of those, say so: docs\WINDOWS_PORT.md is the')
$readme.Add('   list.')
if ($Release) {
    $readme.Add('')
    $readme.Add('6. Privacy')
    $readme.Add('   Nothing in this folder is somebody else''s library, camera roll, playlist or')
    $readme.Add('   log file, and no file in it carries the name of the account or the machine')
    $readme.Add('   that produced it.  That is a check the packager runs, not a promise: run')
    $readme.Add('   packaging\windows\deploy.ps1 -Release yourself from a build tree to make one.')
}
[IO.File]::WriteAllText((Join-Path $stage 'README-Windows.txt'),
    ($readme -join [Environment]::NewLine), [Text.UTF8Encoding]::new($true))
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

# --------------------------------------------------------------------- the privacy pass
Step 'privacy'
# What sits in -Out was built on a working machine, and a working machine has an account
# name, a host name, a folder layout, somebody's music, somebody's photographs and a log
# file.  A release folder goes to strangers, so it is read the way a stranger would read
# it - every staged path, every staged byte, both encodings a name can arrive in - rather
# than trusted because the copy list looked short.
#
# Two kinds of token, deliberately treated differently.  An account or host name is a
# person: no file in a package may carry one, in either mode.  A folder this build
# happened to be made in is a fact about one machine.  For an image this project linked
# that is a defect with a known fix (/PDBALTPATH: in CMakeLists.txt is that fix), so it
# fails a release, and for a DLL somebody else built it is baked into bytes this project
# cannot rewrite and names nobody, so it is counted and said out loud instead of failed on.

# The byte reader lives in packaging/windows/PrivacyProbe.ps1, shared with the audit of the
# finished .zip in packaging/windows/verify-zip.ps1 and with tests/privacy/run.ps1.  It used to
# be a private Find-Needles in this file, and that copy had two ways to call a file clean that
# it had not finished reading: it measured progress by stream Position, and it folded every
# read error into an empty hit list.  The three gates now push the same bytes through the same
# decoder, which is the only reason their three verdicts mean the same thing.
. (Join-Path $PSScriptRoot 'PrivacyProbe.ps1')
# Shorter than four characters and the token stops being a name: a three-letter account
# would be found in half of Qt.  The path test below uses every token regardless of
# length, because a path in a file name is never an accident.
$who = New-Object System.Collections.Generic.List[string]
foreach ($t in @($env:USERNAME, $env:COMPUTERNAME, (Split-Path -Leaf $env:USERPROFILE),
                 'testmusic', 'smoke-profile')) {
    if ($t -and $t.Length -ge 4 -and -not $who.Contains($t)) { $who.Add($t) }
}
$here = New-Object System.Collections.Generic.List[string]
foreach ($t in @($root, $Build, $QtDir, $DepsDir, $VcpkgBin, $FfmpegDir, $Out, $smokeLog)) {
    if (-not $t) { continue }
    $v = $t.TrimEnd('\')
    if ($v.Length -ge 4 -and -not $here.Contains($v)) { $here.Add($v) }
}
$ownNames = New-Object System.Collections.Generic.List[string]
foreach ($e in @(Get-ChildItem -LiteralPath $Build -Filter '*.exe' -File)) {
    $ownNames.Add($e.Name.ToLowerInvariant())
}
# A third-party image does not name the folder this script was handed; it names wherever
# its own build ran, several levels above the file that ended up here - vcpkg's sqlite
# records D:\dev\vcpkg\buildtrees\sqlite3\x64-windows-rel\sqlite3.pdb, not the bin folder
# it was copied out of.  So the ancestors of every dev folder are the probes that actually
# answer, and they are reported apart for the reason two paragraphs above gives.
$ancestors = New-Object System.Collections.Generic.List[string]
foreach ($t in @($here)) {
    $p = $t
    for ($up = 0; $up -lt 2; $up++) {
        $p = Split-Path -Parent $p
        if (-not $p -or $p.Length -lt 5) { break }
        if (-not $ancestors.Contains($p)) { $ancestors.Add($p) }
    }
}
$stagedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force)
$textExts = @('.txt', '.md', '.json', '.conf', '.xml', '.ps1', '.qml', '.ts', '.ini', '.csv',
              '.html', '.py', '.sh', '.c', '.h', '.qrc', '.pro')
# .exe, .dll and .qm used to be the only binary kinds this pass opened, alongside a list of
# text kinds.  The list stays for one purpose only - to tell a file this project authored from
# one it merely shipped - and not to decide what gets read.
# No log, no database, no settings file, no debug symbols and above all no media: the
# point of this list is that a tester's download cannot contain another person's library,
# camera roll, playlist or session.  PNG stays out of it - Qt's own icons and QML imports
# are PNG, eight hundred of them in a healthy package, and this gate bans kinds, not
# extensions it has not counted.
$junkExts = @('.log', '.etl', '.trc', '.db', '.db-wal', '.db-shm', '.sqlite', '.sqlite3',
              '.ini', '.env', '.pdb', '.bak', '.tmp', '.part', '.mtpz-data',
              '.mp3', '.m4a', '.aac', '.wma', '.wav', '.flac', '.ogg', '.opus', '.mka',
              '.mp4', '.m4v', '.mov', '.wmv', '.avi', '.mkv', '.asf', '.m3u', '.m3u8',
              '.pls', '.jpg', '.jpeg', '.heic', '.gif', '.bmp', '.tif', '.tiff', '.webp')

$junk = New-Object System.Collections.Generic.List[string]
foreach ($f in $stagedFiles) {
    if ($junkExts -contains $f.Extension.ToLowerInvariant()) {
        $junk.Add($f.FullName.Substring($stage.Length + 1))
    }
}
if ($junk.Count) {
    Fail ($junk.Count.ToString() + ' staged file(s) are a log, database, settings file, ' +
        'debug symbol or a piece of somebody''s media: ' + (($junk | Select-Object -First 8) -join ' '))
} else {
    Pass ($stagedFiles.Count.ToString() + ' staged files, none of them a log, database, ' +
        'ini, pdb, playlist, audio, image or video file')
}
$auth = @(Get-ChildItem -LiteralPath $stage -Recurse -Force -File |
          Where-Object { $_.Name -eq '.mtpz-data' })
if ($auth.Count) {
    Fail 'the package carries MTPZ credentials, which are never part of a build'
} else {
    Pass 'no MTPZ credentials in the package'
}

$nameLeaks = New-Object System.Collections.Generic.List[string]
foreach ($f in $stagedFiles) {
    $rel = $f.FullName.Substring($stage.Length + 1)
    foreach ($t in (@($who) + @($here))) {
        if ($rel.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $nameLeaks.Add($t + ' in ' + $rel)
        }
    }
}
if ($nameLeaks.Count) {
    Fail (($nameLeaks.Count.ToString()) + ' staged file name(s) name this machine: ' +
        (($nameLeaks | Select-Object -First 6) -join '; '))
} else {
    Pass ('no staged path names this account, host, build tree or test folder, across ' +
        $stagedFiles.Count.ToString() + ' files')
}

$personHits = New-Object System.Collections.Generic.List[string]
$ownPathHits = New-Object System.Collections.Generic.List[string]
$foreignPathHits = New-Object System.Collections.Generic.List[string]
$unreadable = New-Object System.Collections.Generic.List[string]
$scanned = 0
foreach ($f in $stagedFiles) {
    $ext = $f.Extension.ToLowerInvariant()
    $isText = ($textExts -contains $ext)
    $own = $ownNames.Contains($f.Name.ToLowerInvariant())
    $scanned++
    $rel = $f.FullName.Substring($stage.Length + 1)
    # A person's name is looked for in both encodings everywhere, because it is a wide string
    # in the .rdata of a DLL and a plain one in a text file.  A build folder is matched as
    # bytes only, which is what keeps the split below honest: for an image this project linked
    # that path is a defect with a known fix, for anything else it is baked into bytes somebody
    # else wrote and names nobody.  No file is skipped by extension - the whitelist used to
    # read .exe/.dll/.qm plus the text kinds, which quietly meant that everything else got to
    # be unjudged - and a file that cannot be read is a failure, not a pass.
    $err = $null
    if ($isText -or $own) {
        $hits = @(Find-PrivacyNeedles -Path $f.FullName -Needles @($who) -BytesOnlyNeedles @($here) `
                    -ReadError ([ref]$err))
    } else {
        $hits = @(Find-PrivacyNeedles -Path $f.FullName -Needles @($who) `
                    -BytesOnlyNeedles @($ancestors) -ReadError ([ref]$err))
    }
    if ($err) { $unreadable.Add($rel + ': ' + $err); continue }
    foreach ($h in $hits) {
        if ($who.Contains($h)) {
            if (-not $personHits.Contains($h)) { $personHits.Add($h) }
        } elseif ($isText -or $own) {
            $ownPathHits.Add($h + ' in ' + $rel)
        } else {
            $foreignPathHits.Add($h + ' in ' + $rel)
        }
    }
}
if ($personHits.Count) {
    Fail ('a staged file carries this account or machine: ' + ($personHits -join ' '))
} else {
    Pass ('no account name, host name or test-library name in any of the ' +
        $scanned.ToString() + ' staged files, in ASCII or UTF-16')
}
if ($ownPathHits.Count) {
    if ($Release) {
        Fail ('a linked executable names the folder that built it: ' +
            (($ownPathHits | Select-Object -First 4) -join '; '))
    } else {
        Note ('a linked executable names the folder that built it, allowed in a preview: ' +
            (($ownPathHits | Select-Object -First 4) -join '; '))
    }
} else {
    Pass 'nothing this project linked names the folder that built it'
}
if ($foreignPathHits.Count) {
    Note ($foreignPathHits.Count.ToString() + ' hit(s) for a build folder inside third-party ' +
        'binaries this project does not compile and cannot rewrite (a vcpkg or FFmpeg build ' +
        'records where it was made); none of them names a person: ' +
        (($foreignPathHits | Select-Object -First 4) -join '; '))
} else {
    Pass 'no third-party binary in the package names a folder on this machine either'
}
if ($unreadable.Count) {
    # Same rule as the archive gate, and for the same reason: a privacy pass that shrugs at a
    # file it could not open has measured nothing about that file, and a preview that ships
    # unjudged bytes is not a preview anyone can trust the rest of.
    Fail ($unreadable.Count.ToString() + ' staged file(s) could not be read to their end, so ' +
        'their bytes were never judged: ' + (($unreadable | Select-Object -First 4) -join '; '))
} else {
    Pass ('every staged file was opened and read to its end; none was skipped by extension')
}

if ($Release) {
    # A release leaves nothing of itself behind on the machine that made it either.  The
    # smoke profile is a real SQLite library database plus a session log and the smoke log
    # is console output; all three sit outside the staged folder so none of them can reach
    # the archive, and all three are exactly the kind of file this pass exists to keep off
    # a disk that other people see.
    $left = 0
    if (Test-Path -LiteralPath $profile) { $left++ }
    if (Test-Path -LiteralPath $smokeLog) { $left++ }
    Remove-Tree $profile
    Remove-Item -LiteralPath $smokeLog -Force -ErrorAction SilentlyContinue
    if ($left -gt 0) {
        Pass ('the smoke profile and smoke log were written outside the package and are deleted')
    } else {
        Pass 'nothing was left behind by a launch'
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
            # The archive is what leaves this machine, so the name test runs against its
            # entries as well, not only against the folder they were packed from: a path
            # that survives a tar -C and a slash conversion is still a path.
            $named = @($zipped | Where-Object {
                    $hit = $false
                    foreach ($t in (@($who) + @($here))) {
                        if ($_.FullName.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                            $hit = $true
                        }
                    }
                    $hit
                })
            if ($named.Count) {
                Fail ($named.Count.ToString() + ' entries in the archive name this account, host or ' +
                    'build tree: ' + ((($named | ForEach-Object { $_.FullName }) |
                      Select-Object -First 4) -join '; '))
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
if ($zipInfo) {
    # The archive is the deliverable and the folder is not, so the archive gets the audit too -
    # every member opened, read to its end and searched in both encodings, by
    # packaging/windows/verify-zip.ps1.  A folder that passes and an archive written from it
    # are two files: entries can gain a prefix, lose a byte to a rewrite, or arrive from
    # wherever else was in the parent folder, and the thing a tester downloads is the second.
    Step 'archive audited'
    $verifier = Join-Path $PSScriptRoot 'verify-zip.ps1'
    if (-not (Test-Path -LiteralPath $verifier)) {
        Fail 'packaging/windows/verify-zip.ps1 is missing, so the archive was never read'
    } else {
        $eapWas = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        # Every probe the folder audit was handed, not the two obvious ones.  The leak this
        # line is written after was found by a hand run of verify-zip.ps1 that named the
        # dependency root in -BuildRoot, while this call named only the repository and the
        # build tree - so the automated run had passed the very archive a stricter invocation
        # of the same script failed.  The archive is the deliverable, so it gets the whole
        # list, and the ancestors go in as the ancestors: asked of foreign binaries only.
        $vOut = & $verifier $zip -ExpectBytes $zipInfo.Length -ExpectSha256 $sha256 `
                -RootName $name -BuildRoot (@($root, $Build) + @($here) | Select-Object -Unique) `
                -AncestorRoot @($ancestors) 2>&1
        $vCode = $LASTEXITCODE
        $ErrorActionPreference = $eapWas
        foreach ($l in @($vOut | Select-Object -Last 4)) { Write-Host ('    ' + $l) }
        if ($vCode -ne 0) {
            Fail ('the .zip did not pass the audit of its own bytes, exit code ' + $vCode.ToString())
        } else {
            Pass 'the finished .zip passed the same byte audit, member by member'
        }
    }
}
# ---------------------------------------------------------------------------- summary
Step 'summary'
$scope = New-Object System.Collections.Generic.List[string]
if ($NoSmoke) {
    $scope.Add('What this run does prove: the folder was assembled and audited from the')
    $scope.Add('build tree, and nothing was launched, so no run of this shape proves where')
    $scope.Add('a DLL resolves on a machine that did not build it.')
} else {
    $scope.Add('What this run does prove: the staged folder launches on its own with a PATH')
    $scope.Add('that names nothing but itself and Windows, paints a page, loads its modules')
    $scope.Add('from inside the package, and closes politely.')
}
$scope.Add('What it does not prove, in short - README-Windows.txt and docs\WINDOWS_PORT.md')
$scope.Add('carry the long form:')
$scope.Add('  - device evidence is one Pavo (Zune HD), by hand, on one machine: the WinUSB')
$scope.Add('    binding, hotplug, the music library read, authentication, one send with byte')
$scope.Add('    readback, one download, one deletion, one WMA Lossless round trip that played')
$scope.Add('    on the player;')
$scope.Add('  - nobody has heard audio out of this application on a PC; every automated run')
$scope.Add('    asks for a null audio output;')
$scope.Add('  - podcasts, audiobooks, photos and video on a player, every model but that one')
$scope.Add('    Zune HD, and this package on a machine that did not build it, are all')
$scope.Add('    unproven on Windows;')
$scope.Add('  - no installer, no code signing, no SmartScreen history, and no MTPZ')
$scope.Add('    credentials inside it.')
if ($Release) {
    $scope.Add('  - what the release pass adds is about the folder and nothing else: no FFmpeg')
    $scope.Add('    command-line program, no copy of a Windows system binary, no QML debugger,')
    $scope.Add('    and no account name, host name, build path, log, database, music, photo or')
    $scope.Add('    video of whoever built it is inside it - a rule the finished .zip is audited')
    $scope.Add('    against member by member, not only the folder it was packed from.  It says')
    $scope.Add('    nothing new about a Zune.')
    $scope.Add('  - this is a beta build of a GNU GPL program that is not signed, not')
    $scope.Add('    installed, and not a finished product.')
} else {
    $scope.Add('  - this is a preview build, not a signed release, and unlike -Release it does')
    $scope.Add('    carry the FFmpeg command-line programs and the two DLLs only they need.')
}
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
if ($Release) {
    Write-Host '  RESULT: beta Windows artifact produced'
} else {
    Write-Host '  RESULT: preview Windows artifact produced'
}
exit 0


