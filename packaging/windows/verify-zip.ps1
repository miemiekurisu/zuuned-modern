# Zuuned Modern - audit a finished .zip: the bytes that actually leave the machine.
#
#   pwsh -NoProfile -File packaging/windows/verify-zip.ps1 E:\path\Zuuned-0.2.0-win64.zip `
#       -ExpectBytes 166220647 -ExpectSha256 ACC557...
#
# packaging/windows/deploy.ps1 -Release audits the folder it staged and then writes this
# archive, so a run of that script has only ever judged the folder on the way in.  This one
# opens the archive afterwards and reads it member by member, without unpacking it, because
# the thing a tester downloads is the archive.  An entry that survived a rewrite of its name,
# a stray entry added by whatever else was in the parent folder, or bytes that only read as
# clean when they are read off a disk, is a defect this script can see and that one cannot.
#
# Two probe lists, because the two halves of a package are judged differently: the
# what-counts-as-private step gives the reason an ancestor folder is asked of a
# third-party binary and never of a document this project wrote.
#
# Only the archive name is positional.  A hand invocation that left out one parameter name
# once pushed a fourth path into -ExpectBytes, where the binder tried to read it as a number
# and stopped.  Every other parameter has to be named, so a typo now fails at the command line
# instead of quietly changing what a check compares.
#
# It proves structural and privacy facts about one file.  It launches nothing, opens no
# device, and says nothing about whether the application works: docs/WINDOWS_PORT.md and
# README-Windows.txt are where capability is stated, and no packaging run of either script
# has ever added a device claim.
[CmdletBinding(PositionalBinding=$false)]
param(
    [Parameter(Mandatory,Position=0)][string]$Zip,
    [long]$ExpectBytes = 0,
    [string]$ExpectSha256 = '',
    [string[]]$BuildRoot = @(),
    [string[]]$AncestorRoot = @(),
    [string[]]$ExtraNeedle = @(),
    [string]$RootName = ''
)
$ErrorActionPreference = 'Stop'
$checks = 0
$failures = 0
$notes = New-Object System.Collections.Generic.List[string]
function Step { param([string]$m) Write-Host ('== ' + $m) }
function Pass { param([string]$m) $script:checks++; Write-Host ('  ok    ' + $m) }
function Fail { param([string]$m) $script:failures++; Write-Host ('  FAIL  ' + $m) }
function Note { param([string]$m) $script:notes.Add($m); Write-Host ('  note  ' + $m) }

. (Join-Path $PSScriptRoot 'PrivacyProbe.ps1')

# --------------------------------------------------------------------------- the file
Step 'the archive itself'
if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) {
    Fail ('no such file: ' + $Zip)
    Write-Host ''
    Write-Host '  RESULT: nothing was verified - the archive is not there'
    exit 1
}
$info = Get-Item -LiteralPath $Zip
Pass ($info.Length.ToString() + ' bytes at ' + $Zip)
if ($ExpectBytes -gt 0) {
    if ($info.Length -ne $ExpectBytes) {
        Fail ('size is ' + $info.Length.ToString() + ', expected ' + $ExpectBytes.ToString())
    } else { Pass 'the size matches the one recorded when it was written' }
}
$sha = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash
Pass ('sha256 ' + $sha)
if ($ExpectSha256) {
    if ($sha -ne $ExpectSha256.ToUpperInvariant()) {
        Fail ('digest does not match ' + $ExpectSha256.ToUpperInvariant())
    } else { Pass 'the digest matches the one recorded when it was written' }
}

# ----------------------------------------------------------------------- the needles
# A person, in either encoding, is what may never appear.  A folder a build ran in is a fact
# about one machine: for an executable this project linked it is a defect a release must not
# ship, for a DLL somebody else linked it is baked into bytes this project cannot rewrite, so
# it is counted and said out loud.  Same split deploy.ps1 uses, for the same reason.
Step 'what counts as private'
$who = New-Object System.Collections.Generic.List[string]
foreach ($t in @($env:USERNAME, $env:COMPUTERNAME, (Split-Path -Leaf $env:USERPROFILE)) +
                @('testmusic', 'smoke-profile') + @($ExtraNeedle)) {
    $v = ('' + $t).Trim()
    if ($v.Length -ge 4 -and -not $who.Contains($v)) { $who.Add($v) }
}
if ($env:USERPROFILE) {
    $up = $env:USERPROFILE.TrimEnd('\')
    if ($up.Length -ge 4 -and -not $who.Contains($up)) { $who.Add($up) }
}
$roots = New-Object System.Collections.Generic.List[string]
foreach ($t in @((Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path, $env:ZUUNED_WIN_DEPS,
                $env:QTDIR, $env:VCPKG_ROOT, $env:FFMPEG_DIR) + @($BuildRoot)) {
    if (-not $t) { continue }
    $v = ('' + $t).TrimEnd('\')
    if ($v.Length -ge 4 -and -not $roots.Contains($v)) { $roots.Add($v) }
}
# Two probe lists, not one, because the two halves of a package are judged differently.  A
# path an own file could have learned - the repository, the build tree, the Qt and dependency
# roots it was linked against, the folder it was staged in - may not appear in a document or an
# executable written here, and one that does fails the archive.  The folder one level above
# those roots is a different case: it is all a third-party DLL remembers about where it was
# compiled, and it is also the folder a hand test wrote its evidence into, so it is asked of the
# binaries somebody else linked and counted there, never of the documents that name the same
# folder on purpose.  packaging/windows/deploy.ps1 splits the two the same way, and this script
# refuses to be the looser of the pair.
$foreignRoots = New-Object System.Collections.Generic.List[string]
foreach ($t in @($AncestorRoot)) {
    if (-not $t) { continue }
    $v = ('' + $t).TrimEnd('\')
    if ($v.Length -ge 4 -and -not $roots.Contains($v) -and -not $foreignRoots.Contains($v)) {
        $foreignRoots.Add($v)
    }
}
Pass ($who.Count.ToString() + ' person needle(s), ' + $roots.Count.ToString() +
      ' build-root needle(s) an own file may not name, ' +
      $foreignRoots.Count.ToString() + ' ancestor needle(s) judged only in foreign binaries')

# ------------------------------------------------------------------------ the members
Step 'entries'
try { Add-Type -AssemblyName System.IO.Compression.FileSystem } catch { }
$archive = [IO.Compression.ZipFile]::OpenRead($Zip)
$files = @()
$personHits = New-Object System.Collections.Generic.List[string]
$ownPathHits = New-Object System.Collections.Generic.List[string]
$foreignPathHits = New-Object System.Collections.Generic.List[string]
$unreadable = New-Object System.Collections.Generic.List[string]
$serialHits = New-Object System.Collections.Generic.List[string]
$scanned = 0
$bytesRead = [int64]0
$bigPng = New-Object System.Collections.Generic.List[string]
try {
    $entries = @($archive.Entries)
    $files = @($entries | Where-Object { $_.Name })
    $dirs = @($entries | Where-Object { -not $_.Name })
    Pass ($files.Count.ToString() + ' file entries, ' + $dirs.Count.ToString() + ' directory entries')

    $rooted = @($files | Where-Object {
            $_.FullName -match '^[A-Za-z]:' -or $_.FullName.StartsWith('/') -or
            $_.FullName.Contains('\') })
    $escapes = @($files | Where-Object { $_.FullName -match '(^|/)\.\.(/|$)' })
    $tops = @($files | ForEach-Object { ($_.FullName -split '/')[0] } | Sort-Object -Unique)
    if ($rooted.Count) {
        Fail ($rooted.Count.ToString() + ' entries carry an absolute or backslash path')
    } elseif ($escapes.Count) {
        Fail ($escapes.Count.ToString() + ' entries escape the folder they unpack into')
    } elseif ($tops.Count -ne 1) {
        Fail ('the archive does not unpack into one folder: ' + ($tops -join ' '))
    } else {
        Pass ('every entry is relative and under ' + $tops[0] + '/')
    }
    if ($RootName -and ($tops.Count -ne 1 -or $tops[0] -ne $RootName)) {
        Fail ('the root folder is ' + ($tops -join ' ') + ', expected ' + $RootName)
    }

    # A package built by this project has ASCII names through and through.  A name in another
    # script is somebody else's folder, and on this machine the folders used to test transfers
    # are written in Chinese, so an odd code point here is exactly the file that must not ship.
    $odd = @($files | Where-Object { $_.FullName -match '[^\x20-\x7E]' })
    if ($odd.Count) {
        Fail ($odd.Count.ToString() + ' entry name(s) are not plain ASCII: ' +
            ((($odd | Select-Object -First 4 | ForEach-Object { $_.FullName })) -join '; '))
    } else {
        Pass ('all ' + $files.Count.ToString() + ' entry names are plain ASCII')
    }

    $junkExts = @('.log', '.etl', '.trc', '.db', '.db-wal', '.db-shm', '.sqlite', '.sqlite3',
                  '.ini', '.env', '.pdb', '.bak', '.tmp', '.part', '.mtpz-data',
                  '.mp3', '.m4a', '.aac', '.wma', '.wav', '.flac', '.ogg', '.opus', '.mka',
                  '.mp4', '.m4v', '.mov', '.wmv', '.avi', '.mkv', '.asf', '.m3u', '.m3u8',
                  '.pls', '.jpg', '.jpeg', '.heic', '.gif', '.bmp', '.tif', '.tiff', '.webp')
    $junk = @($files | Where-Object {
            $junkExts -contains [IO.Path]::GetExtension($_.Name).ToLowerInvariant() })
    if ($junk.Count) {
        Fail ($junk.Count.ToString() + ' entries are a log, database, settings file, debug ' +
            'symbol or a piece of somebodies media: ' +
            ((($junk | Select-Object -First 6 | ForEach-Object { $_.FullName })) -join '; '))
    } else {
        Pass ('no log, database, ini, pdb, playlist, audio, photo or video entry among ' +
              $files.Count.ToString() + ' members')
    }
    $auth = @($files | Where-Object { $_.Name -eq '.mtpz-data' })
    if ($auth.Count) {
        Fail 'the archive carries MTPZ credentials, which are never part of a build'
    } else {
        Pass 'no MTPZ credentials in the archive'
    }

    # PNG is legal - Qt ships hundreds of its own icons - so the extension cannot answer
    # "is there a photograph in here".  A photograph is large and an icon is not, so the big
    # ones are named here for a human to look at instead of being waved through by a list.
    foreach ($e in $files) {
        if ([IO.Path]::GetExtension($e.Name).ToLowerInvariant() -eq '.png' -and $e.Length -gt 262144) {
            $bigPng.Add($e.FullName + ' ' + [math]::Round($e.Length / 1KB).ToString() + ' KB')
        }
    }
    if ($bigPng.Count) {
        Note ($bigPng.Count.ToString() + ' PNG entries over 256 KB, named for the record: ' +
            (($bigPng | Select-Object -First 4) -join '; '))
    } else {
        Pass 'every PNG in the archive is icon-sized, not photograph-sized'
    }

    $named = New-Object System.Collections.Generic.List[string]
    foreach ($e in $files) {
        foreach ($t in (@($who) + @($roots))) {
            if ($e.FullName.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $named.Add($t + ' in ' + $e.FullName)
            }
        }
    }
    if ($named.Count) {
        Fail ($named.Count.ToString() + ' entry name(s) name this account, host or build tree: ' +
            (($named | Select-Object -First 6) -join '; '))
    } else {
        Pass 'no entry name names this account, host or build tree'
    }

    # -------------------------------------------------------------------- the bytes
    Step 'bytes'
    # Every member, no whitelist.  A filter is a promise that whatever it skips is boring, and
    # the point of this gate is that nobody has to take that on trust: a photo, a playlist or a
    # settings file smuggled in under an extension nobody thought about is the defect this step
    # exists to catch, and the junk-extension list above only bans the ones somebody named.
    # Which members are this project's own work.  A build path inside an executable or a
    # text file this repository wrote is a defect with a known fix, so it fails the archive; the
    # same path inside a DLL somebody else linked is baked into bytes nobody here can rewrite,
    # so it is counted and said out loud instead.  The text kinds are the same list
    # packaging/windows/deploy.ps1 uses for the folder, so the two gates cannot disagree about
    # which half of the package a hit belongs to.
    $ownExt = @('.exe', '.txt', '.md', '.json', '.conf', '.xml', '.ps1', '.qml', '.ts',
                '.ini', '.csv', '.html', '.py', '.sh', '.c', '.h', '.qrc', '.pro')
    foreach ($e in $files) {
        $ext = [IO.Path]::GetExtension($e.Name).ToLowerInvariant()
        $scanned++
        $err = $null
        $s = $null
        try {
            $s = $e.Open()
            # A foreign member is asked about the folders above the build roots as well; an
            # own file is not, so that a document recording a hand-test library is not
            # failed for naming the drive that test ran on.
            $bytesOnly = @($roots)
            if (-not ($ownExt -contains $ext)) { $bytesOnly = @($roots) + @($foreignRoots) }
            $hits = @(Find-PrivacyNeedlesInStream -Stream $s -Needles @($who) `
                        -BytesOnlyNeedles $bytesOnly -Length $e.Length -ReadError ([ref]$err))
            if ($err) { $unreadable.Add($e.FullName + ': ' + $err) }
            $bytesRead += [int64]$e.Length
            foreach ($h in $hits) {
                if ($who.Contains($h)) {
                    if (-not $personHits.Contains($h)) { $personHits.Add($h + ' in ' + $e.FullName) }
                } elseif ($ownExt -contains $ext) {
                    $ownPathHits.Add($h + ' in ' + $e.FullName)
                } else {
                    $foreignPathHits.Add($h + ' in ' + $e.FullName)
                }
            }
        } catch {
            $unreadable.Add($e.FullName + ': ' + $_.Exception.Message)
        } finally {
            if ($s) { $s.Dispose() }
        }
    }
    # The needles above are strings.  This one is a pattern, because the half of a USB device
    # instance path that must not ship is a serial number, and nobody here would ever type a
    # string anybody could put in a list.  Only the members this project authored are judged;
    # a third-party binary carrying such a string is somebody else's build evidence, counted
    # and named by the pass above rather than excused here.  A zip member stream is read
    # forward once and cannot be rewound, so the text members are opened a second time for a
    # prefix read - they are documents and scripts, so the second pass costs nothing next to
    # the hundreds of megabytes already read above.  A member that fails here lands in the
    # same unreadable list with the name of this pass beside it, so a double count would be
    # visible rather than silent.
    $serialTexts = 0
    foreach ($e in $files) {
        if (-not ($ownExt -contains [IO.Path]::GetExtension($e.Name).ToLowerInvariant())) {
            continue
        }
        $terr = $null
        $txt = ''
        try {
            $ts = $null
            try {
                $ts = $e.Open()
                $txt = Read-PrivacyTextPrefix -Stream $ts -Length $e.Length -ReadError ([ref]$terr)
            } finally { if ($ts) { $ts.Dispose() } }
        } catch {
            $terr = $_.Exception.Message
        }
        if ($terr) {
            $unreadable.Add($e.FullName + ' (serial pass): ' + $terr)
            continue
        }
        $serialTexts++
        foreach ($m in [regex]::Matches($txt, (Get-PrivacySerialRule))) {
            if (-not $serialHits.Contains($e.FullName)) { $serialHits.Add($e.FullName) }
        }
    }
    if ($serialHits.Count) {
        Fail ($serialHits.Count.ToString() + ' archived file(s) name the serial tail of a USB' +
            ' device instance path, which identifies one physical player: ' +
            (($serialHits | Select-Object -First 4) -join '; '))
    } else {
        Pass ('no member this project authored names the serial number of a device; ' +
              $serialTexts.ToString() + ' text members were read a second time for it')
    }

    if ($personHits.Count) {
        Fail ('an archived file carries this account, host or home folder: ' +
            (($personHits | Select-Object -First 6) -join '; '))
    } else {
        Pass ('no account, host, home folder or test-library name in any of the ' +
              $scanned.ToString() + ' files that leave this machine (' +
              [math]::Round($bytesRead / 1MB, 1).ToString() + ' MB read and judged), ' +
              'in ASCII or UTF-16')
    }
    if ($ownPathHits.Count) {
        Fail ('a file this project authored names the folder that built it: ' +
            (($ownPathHits | Select-Object -First 4) -join '; '))
    } else {
        Pass 'no executable or text file this project authored names a folder on this machine'
    }
    if ($foreignPathHits.Count) {
        Note ($foreignPathHits.Count.ToString() + ' build-folder hit(s) inside third-party ' +
            'binaries this project does not compile and cannot rewrite; none names a person: ' +
            (($foreignPathHits | Select-Object -First 4) -join '; '))
    } else {
        Pass 'no third-party member names a folder on this machine either'
    }
    if ($unreadable.Count) {
        Fail ($unreadable.Count.ToString() + ' member(s) could not be read, so their bytes were ' +
            'not judged: ' + (($unreadable | Select-Object -First 4) -join '; '))
    } else {
        Pass ('all ' + $scanned.ToString() + ' members were read to their end, none was skipped')
    }
} finally { $archive.Dispose() }

# -------------------------------------------------------------------------- summary
Step 'summary'
Write-Host ''
Write-Host ('  archive     ' + $Zip)
Write-Host ('  size        ' + [math]::Round($info.Length / 1MB, 1) + ' MB')
Write-Host ('  sha256      ' + $sha)
Write-Host ('  members     ' + $files.Count.ToString() + ' files, all ' + $scanned.ToString() +
              ' read, ' + [math]::Round($bytesRead / 1MB, 1) + ' MB judged')
Write-Host ('  checks      ' + $checks + ', failures ' + $failures)
Write-Host ''
Write-Host '  | What this proves: the archive as it stands on disk unpacks into one folder,'
Write-Host '  | holds no log, database, settings file, debug symbol, playlist, photo, audio'
Write-Host '  | or video, carries no MTPZ credentials, and carries no account, host, home'
Write-Host '  | folder or test-library name in any byte of any member - every file in the'
Write-Host '  | archive was opened, read to its end and searched, none by extension whitelist.'
Write-Host '  | What it does not prove: that the application runs, or anything at all about a'
Write-Host '  | Zune - this opens no device handle and launches nothing.'
Write-Host ''
if ($failures -gt 0) {
    Write-Host ('  RESULT: archive not clean - ' + $failures + ' failed check(s)')
    exit 1
}
Write-Host '  RESULT: archive clean - fit to publish'
exit 0
