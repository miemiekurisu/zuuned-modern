# Zuuned Modern - the repository's own privacy gate.
#
#   pwsh -NoProfile -File tests/privacy/run.ps1
#
# packaging/windows/deploy.ps1 -Release audits the folder that leaves a build
# machine.  This audits what leaves the repository, because a public repository
# keeps every version of every mistake and a published release cannot be unsent.
# It takes the file list from git and the bytes from the working tree and asks
# four questions.  Is any tracked file somebody's music, photograph, video,
# playlist, database, log or scratch file rather than source?  Does any tracked
# path or text byte spell the account, the host, the home folder or the commit
# identity of whoever made this checkout?  How many tracked lines quote a
# machine-local absolute path, which is how build and device evidence is written
# down in this project and therefore has to be counted out loud rather than
# silently allowed?  And which tracked pictures cannot be read as text at all, so
# that each one sits in tests/privacy/reviewed-media.txt with a reason beside it
# instead of having slipped in?
#
# No device, no build, no network, and nothing here writes to the tree.
param()
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$reviewedPath = Join-Path $PSScriptRoot 'reviewed-media.txt'
$checks = 0
$failures = 0
function Step { param([string]$m) Write-Host ('== ' + $m) }
function Pass { param([string]$m) $script:checks++; Write-Host ('  ok    ' + $m) }
function Fail { param([string]$m) $script:failures++; Write-Host ('  FAIL  ' + $m) }
function Note { param([string]$m) Write-Host ('  note  ' + $m) }

$encWas = [Console]::OutputEncoding
Push-Location $root
try {
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

    # ---------------------------------------------------------------- the needles
    Step 'who is asking'
    $remote = ''
    try { $remote = [string](& git remote get-url origin 2>$null) } catch { }
    $needles = New-Object System.Collections.Generic.List[string]
    $skipped = New-Object System.Collections.Generic.List[string]
    function Add-Needle {
        param([string]$label, [string]$value)
        $t = ('' + $value).Trim()
        if ($t.Length -lt 4) { return }
        # The identity the repository is published under is on its own clone URL,
        # so spelling it in a README is not a leak.  Anything else that identifies
        # a person, a box or a home folder is.
        if ($remote -and $remote.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            $script:skipped.Add($label + ' (the name this repository is published under)')
            return
        }
        foreach ($e in $script:needles) {
            if ($e.Equals($t, [StringComparison]::OrdinalIgnoreCase)) { return }
        }
        $script:needles.Add($t)
    }
    Add-Needle 'USERNAME' $env:USERNAME
    Add-Needle 'COMPUTERNAME' $env:COMPUTERNAME
    Add-Needle 'USERPROFILE' $env:USERPROFILE
    if ($env:USERPROFILE) {
    Add-Needle 'USERPROFILE leaf' $([IO.Path]::GetFileName($env:USERPROFILE.TrimEnd('\', '/')))
    }
    Add-Needle 'git user.name' ([string](& git config user.name 2>$null))
    $mail = [string](& git config user.email 2>$null)
    if ($mail -match '@') { Add-Needle 'git user.email' ($mail -split '@')[0] }
    if ($needles.Count -eq 0) {
        Fail 'no identity to look for, so this gate proved nothing'
    } else {
        Pass ($needles.Count.ToString() + ' identity strings this checkout must not put in the tree')
    }
    foreach ($s in $skipped) { Note ('not treated as private: ' + $s) }

    # --------------------------------------------------------------- the tracked set
    Step 'tracked set'
    $files = @(& git -c core.quotepath=off ls-files)
    if (-not $files -or $LASTEXITCODE -ne 0) { Fail 'git ls-files answered nothing' }
    $absent = @($files | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($absent.Count) {
        Fail ($absent.Count.ToString() + ' tracked files are not in the working tree: ' +
            (($absent | Select-Object -First 4) -join '; '))
    }
    Pass ($files.Count.ToString() + ' tracked files read from the index')

    $junkNames = @('.DS_Store', 'Thumbs.db', 'ehthumbs.db', 'desktop.ini', '.mtpz-data', 'nohup.out')
    $byName = @($files | Where-Object { $junkNames -contains [IO.Path]::GetFileName($_) })
    if ($byName.Count) {
        Fail ($byName.Count.ToString() + ' tracked file(s) are machine droppings or credentials: ' +
            (($byName | Select-Object -First 4) -join '; '))
    } else {
        Pass 'no .DS_Store, Thumbs.db, desktop.ini or credential file is tracked'
    }

    # Anything that records what a program did or a user has, rather than what a
    # program is.  A log, a database, a settings file, a scratch file, a container,
    # or a compiled artefact, has no business in a public source repository.
    $junkExt = '\.(log|etl|trc|trace|db|db-wal|db-shm|sqlite|sqlite3|ini|env|pdb|ilk|exp|bak|orig|tmp|temp|part|swp|zip|7z|gz|bz2|xz|tar|rar|exe|dll|lib|obj|o|a|dylib|so|dmg|app|appimage|AppImage|msi|aab)$'
    $junk = @($files | Where-Object { $_ -match $junkExt })
    if ($junk.Count) {
        Fail ($junk.Count.ToString() + ' tracked file(s) are logs, databases, settings, archives or' +
            ' binaries rather than source: ' + (($junk | Select-Object -First 6) -join '; '))
    } else {
        Pass 'nothing tracked is a log, database, settings file, archive or binary'
    }

    # ------------------------------------------------------------------- the media
    Step 'media'
    $mediaExt = '\.(png|jpg|jpeg|webp|gif|bmp|tif|tiff|heic|icns|ico|mp3|m4a|m4b|aac|flac|wav|wma|ogg|opus|mp4|m4v|mov|wmv|avi|mkv)$'
    $media = @($files | Where-Object { $_ -match $mediaExt })
    $prefixes = New-Object System.Collections.Generic.List[string]
    $why = @{ }
    if (-not (Test-Path -LiteralPath $reviewedPath)) {
        Fail 'tests/privacy/reviewed-media.txt is missing'
    } else {
        foreach ($l in ([IO.File]::ReadAllText($reviewedPath) -split "`n")) {
            $t = $l.Trim()
            if (-not $t -or $t.StartsWith('#')) { continue }
            $p = $t
            $r = ''
            if ($t.Contains('|')) {
                $p = $t.Substring(0, $t.IndexOf('|')).Trim()
                $r = $t.Substring($t.IndexOf('|') + 1).Trim()
            }
            $prefixes.Add($p.ToLowerInvariant())
            $why[$p.ToLowerInvariant()] = $r
        }
    }
    $stray = New-Object System.Collections.Generic.List[string]
    foreach ($m in $media) {
        $n = $m.Replace('\', '/').ToLowerInvariant()
        $seen = $false
        foreach ($p in $prefixes) { if ($n.StartsWith($p)) { $seen = $true; break } }
        if (-not $seen) { $stray.Add($m) }
    }
    if ($stray.Count) {
        Fail ($stray.Count.ToString() + ' tracked picture(s) or recording(s) sit under no reviewed' +
            ' prefix: ' + (($stray | Select-Object -First 6) -join '; '))
    } else {
        Pass ($media.Count.ToString() + ' tracked pictures each sit under a prefix named in' +
            ' reviewed-media.txt')
    }
    foreach ($p in ($prefixes | Sort-Object)) {
        $c = @($media | Where-Object { $_.Replace('\', '/').ToLowerInvariant().StartsWith($p) }).Count
        if ($c) { Note ($c.ToString() + ' under ' + $p + ' - ' + $why[$p]) }
    }

    # ---------------------------------------------------- names, then bytes, then count
    Step 'identity in names and bytes'
    $named = New-Object System.Collections.Generic.List[string]
    foreach ($f in $files) {
        foreach ($t in $needles) {
            if ($f.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $named.Add($f + ' names ' + $t)
                break
            }
        }
    }
    if ($named.Count) {
        Fail ($named.Count.ToString() + ' tracked path(s) name this account, host or home folder:' +
            ' ' + (($named | Select-Object -First 4) -join '; '))
    } else {
        Pass ($files.Count.ToString() + ' tracked paths name nobody and nowhere')
    }

    $latin = [Text.Encoding]::GetEncoding(28591)
    $hits = New-Object System.Collections.Generic.List[string]
    $pathHits = 0
    $pathFiles = 0
    $roots = @{ }
    $scanned = 0
    $notScanned = 0
    foreach ($f in $files) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $item = Get-Item -LiteralPath $f -Force
        if ($item.Length -gt 4MB) { $notScanned++; continue }
        $bytes = [IO.File]::ReadAllBytes($f)
        $head = [Math]::Min(8191, $bytes.Length - 1)
        $binary = $false
        for ($i = 0; $i -le $head; $i++) { if ($bytes[$i] -eq 0) { $binary = $true; break } }
        if ($binary) { $notScanned++; continue }
        $scanned++
        $text = $latin.GetString($bytes)
        foreach ($t in $needles) {
            if ($text.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $hits.Add($f + ' carries ' + $t)
                break
            }
        }
        $n = 0
        # A Windows checkout has CRLF everywhere, and a line anchored on $ matches nothing while the
        # carriage return is still in the way, so this counter would read 0 over every document in
        # the tree. Normalise the line endings before asking the question of them.
        $flat = $text.Replace("`r`n", "`n")
        foreach ($m in [regex]::Matches($flat, '(?im)^[^\r\n]*\b[a-z]:[\\/][^\r\n]*$')) { $n++ }
        if ($n) {
            $pathHits += $n
            $pathFiles++
            foreach ($m in [regex]::Matches($flat, '(?i)\b([a-z]:)[\\/]+([A-Za-z0-9 ._-]+)')) {
                $k = ($m.Groups[1].Value + '\' + $m.Groups[2].Value).ToLowerInvariant()
                if ($roots.ContainsKey($k)) { $roots[$k] += 1 } else { $roots[$k] = 1 }
            }
        }
    }
    if ($hits.Count) {
        Fail ($hits.Count.ToString() + ' tracked text file(s) carry this account, host or home' +
            ' folder: ' + (($hits | Select-Object -First 5) -join '; '))
    } else {
        Pass ('no identity string in any byte of the ' + $scanned.ToString() + ' text files')
    }
    Pass ($notScanned.ToString() + ' files skipped as binary or oversized: their bytes are judged by' +
        ' the media rule above, not here')

    Step 'machine-local paths'
    Pass ($pathHits.ToString() + ' line(s) in ' + $pathFiles.ToString() + ' tracked file(s) quote an' +
        ' absolute path with a drive letter')
    foreach ($k in ($roots.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 6)) {
        Note ('quoted ' + $k.Value.ToString() + 'x: ' + $k.Key)
    }
    Note 'a drive-letter path in a document here is recorded build or device evidence, not an'
    Note 'accident; a NEW one changes the count above, which is the point of printing it'

    Step 'summary'
    Write-Host ''
    Write-Host ('  tracked     ' + $files.Count.ToString() + ' files')
    Write-Host ('  identity    ' + $needles.Count.ToString() + ' string(s) searched in names and bytes')
    Write-Host ('  paths       ' + $pathHits.ToString() + ' line(s) quote a drive letter')
    Write-Host ('  checks      ' + $checks + ', failures ' + $failures)
    Write-Host ''
    if ($failures -gt 0) {
        Write-Host ('  RESULT: not clean - ' + $failures + ' failed check(s)')
        exit 1
    }
    Write-Host '  RESULT: tracked tree carries no personal data this gate can read'
    exit 0
} finally {
    [Console]::OutputEncoding = $encWas
    Pop-Location
}


