# Windows usability gate for Zuuned.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\win-usability\run.ps1
#
# Answers one question with evidence: is this Windows build actually usable?  It
# is the GUI equivalent of tests/win-compat: no device, no sound (ZUUNED_MPV_AO is
# the null driver) and no contact with the real profile, because every launch gets
# its own ZUUNED_PROFILE_DIR plus its own ZUUNED_LOCK_SUFFIX and the run compares
# the real profile before and after.  It needs no network to pass, and it does not
# get there by hoping the defaults are quiet: both online questions - album cover
# lookup and artist portrait lookup - ship off, so a seeded case writes no ini at
# all and the music page case asserts the app's own "online lookup off" line plus
# that no provider request line appears.  That assertion is therefore about the
# shipping defaults and not about a value the harness typed in for itself, which
# is the whole point of it: a gate that silences the app and then measures the
# silence proves nothing but the gate.  (With Settings -> Library switched on a
# launch whose seeded library lacks covers does reach a provider; that is the
# user's choice, not the gate's.)
#
# A started process is not proof of anything on Windows: a Qt Quick window can
# come up with a clean stderr and paint nothing.  So each case ends with a
# winshot capture of the live window, and the app itself reports what it decided
# to show through `--qa-probe`, which is what lets the gate say that --page took
# effect instead of inferring it from a screenshot.
#
# Needs a built build\zuuned.exe (and librarytool, transcodetool, playertool) and
# the MSVC x64 environment for winshot, loaded from msvc-env.ps1 when cl is not
# already on PATH.  Nothing in this gate opens a USB device handle, so it certifies
# the UI, the tools and the profile - never the wire.  The Windows wire evidence is
# the hand test recorded in docs\WINDOWS_PORT.md: one Pavo Zune HD, authenticated,
# browsed, sent music, photos and video, and read the music back byte for byte -
# a photo and a video have been re-listed on the player, never read off it.
# Nothing here adds to that, and no run of this gate has ever opened a device.
param(
    [string]$Deps = 'D:\dev\zuuned-deps',
    [string]$QtDir = 'D:\dev\Qt\6.11.0\msvc2022_64',
    [string]$FfmpegDir = 'D:\dev\zuuned-deps\ffmpeg-master-latest-win64-gpl-shared\bin',
    [int]$Settle = 6,
    [int]$CaseTimeout = 180,
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'build'
$out = Join-Path $build 'win-usability'
$profiles = Join-Path $out 'profiles'
$shots = Join-Path $out 'shots'
$fixture = Join-Path $out 'fixture'
$exe = Join-Path $build 'zuuned.exe'
$libraryTool = Join-Path $build 'librarytool.exe'
$transcodeTool = Join-Path $build 'transcodetool.exe'
$mtpFraming = Join-Path $build 'mtpframingtool.exe'
$playerTool = Join-Path $build 'playertool.exe'
$deviceTool = Join-Path $build 'devicetool.exe'
$winshot = Join-Path $out 'winshot.exe'
$python = (Get-Command python -ErrorAction SilentlyContinue)

foreach ($d in @($out, $profiles, $shots, $fixture)) {
    if (Test-Path $d) { python -c "import shutil,sys;shutil.rmtree(sys.argv[1])" $d }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    . (Join-Path $Deps 'msvc-env.ps1')
}

# The Qt and ffmpeg directories are part of the environment every child needs, not
# just the instrumented launches.  Start-Zuuned and the single-instance rival name
# PATH themselves; `Invoke-Proc -File $libraryTool -Arguments @('names')` and the
# other tool checks do not, so they inherited it from whoever ran the gate.  From a
# shell without Qt on PATH every one of those tools died at 0xC0000135
# (DLL_NOT_FOUND) before printing a line, 27 checks failed, the seed library never
# got built, and the page, settings-tab and compact cases SKIPPED themselves while
# the app itself was fine.  A gate must not pass or fail on its caller's shell.
$env:PATH = (Join-Path $QtDir 'bin') + ';' + $FfmpegDir + ';' + $env:PATH

$script:checks = 0
$script:failures = 0
$script:skips = 0
$script:failed = New-Object System.Collections.Generic.List[string]
$script:noise = New-Object System.Collections.Generic.List[string]
$script:caseSeq = 0
$script:pidScans = 0
$script:killed = 0
$script:pids = New-Object System.Collections.Generic.List[int]

# A harness that dies halfway must not leave an instrumented Zuuned holding a
# profile folder and an instance lock that the next run intends to delete: one
# broken helper once aborted this script mid-case and left a launched app behind.
trap {
    Write-Host ('harness stopped: ' + $_.Exception.Message)
    foreach ($orphan in @($script:pids)) {
        $proc = Get-Process -Id $orphan -ErrorAction SilentlyContinue
        if (-not $proc -or $proc.HasExited) { continue }
        [void]$proc.CloseMainWindow()
        if (-not $proc.WaitForExit(10000)) {
            try { $proc.Kill($true) } catch { $proc.Kill() }
        }
    }
    Write-Host 'OVERALL=fail'
    exit 1
}

function Check {
    param([string]$Name, [bool]$Condition, [string]$Detail = '')
    $script:checks++
    if ($Condition) {
        Write-Host ('  PASS ' + $Name)
    }
    else {
        $script:failures++
        $script:failed.Add($Name)
        Write-Host ('  FAIL ' + $Name + $(if ($Detail) { '  :: ' + $Detail } else { '' }))
    }
}

function Skip {
    param([string]$Name, [string]$Why)
    $script:checks++
    $script:skips++
    Write-Host ('  SKIP ' + $Name + '  ::  ' + $Why)
}

function Select-Phase {
    # -Only takes a list so a filtered run can pair a phase with the fixture it
    # needs: the scratch directory is wiped per invocation, so nothing carries over.
    param([string]$Name)
    if (-not $Only) { return $true }
    foreach ($pattern in ($Only -join ',' -split ',')) {
        $pattern = $pattern.Trim()
        if (-not $pattern) { continue }
        if ($Name -like ('*' + $pattern + '*')) { return $true }
    }
    return $false
}

# Quote every argument so a path with a space or a bracket survives the MSVC CRT
# argument parser untouched.  Trailing backslashes are dropped because inside
# quotes they would escape the closing quote.
function Format-Args {
    param([string[]]$Arguments)
    return ($Arguments | ForEach-Object {
        $a = $_
        while ($a.EndsWith('\')) { $a = $a.Substring(0, $a.Length - 1) }
        '"' + $a + '"'
    }) -join ' '
}

function Invoke-Proc {
    param(
        [string]$File,
        [string[]]$Arguments = @(),
        [string]$WorkDir = $build,
        [hashtable]$Env = @{},
        [int]$Timeout = 120
    )
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $File
    $psi.Arguments = Format-Args -Arguments $Arguments
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $psi.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($k in $Env.Keys) { $psi.EnvironmentVariables[$k] = [string]$Env[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($Timeout * 1000)) {
        try { $p.Kill($true) } catch { $p.Kill() }
        return [pscustomobject]@{ Code = 124; Out = ''; Err = 'timed out after ' + $Timeout + 's'; TimedOut = $true }
    }
    $p.WaitForExit()
    return [pscustomobject]@{
        Code = $p.ExitCode
        Out = $outTask.Result
        Err = $errTask.Result
        TimedOut = $false
    }
}

# winshot prints and parses a handle as `%llu` of uintptr_t, in decimal, so the
# wire form of a handle is the UNSIGNED decimal of the whole pointer.  A 64-bit
# Windows handle is routinely above Int64.MaxValue - 0xFFFFFFFF80010E3A is one -
# and the .NET handle property hands that back sign-extended and negative, so
# both directions have to reinterpret the same bytes instead of shrinking or
# signing them.  Masking to 32 bits, which these helpers used to do, quietly
# renames the window: GetWindowRect then fails on a handle that exists.  Parsing
# with [long]::Parse, which the pid scan did, throws "Value was either too large
# or too small for an Int64" and took the whole harness down on a run that
# needed the scan - chrome collapse pin, 2026-10-21.
function ConvertTo-HwndDecimal {
    param([IntPtr]$Handle)
    $bytes = [BitConverter]::GetBytes([int64]$Handle.ToInt64())
    if ([IntPtr]::Size -eq 8) { return ([BitConverter]::ToUInt64($bytes, 0)).ToString() }
    return ([BitConverter]::ToUInt32($bytes, 0)).ToString()
}

function ConvertFrom-HwndDecimal {
    # The exact inverse of the above; keep the two in step.
    param([string]$Text)
    $bytes = [BitConverter]::GetBytes([uint64]::Parse($Text))
    if ([IntPtr]::Size -eq 8) { return [IntPtr][BitConverter]::ToInt64($bytes, 0) }
    return [IntPtr][BitConverter]::ToInt32($bytes, 0)
}

function Get-WindowScale {
    # The scale the window is really painting at.  The breakpoints in Main.qml are
    # logical pixels and winshot's MoveWindow takes physical ones, so a drive that
    # asks for a fixed 1400x1220 is asking two different questions on two different
    # desktops: at 100% that clears the ribbon's 1280x1100 requirement, and at 150%
    # the same request lands on 933x773 logical and can never reach it - measured on
    # 2026-10-22, when the compact playback phase failed four checks against an
    # application it had not changed, because the desktop was at 150%.  The answer
    # comes from winshot, which declares itself per-monitor DPI aware before it asks.
    # Asking from here does not work: GetDpiForWindow answers 96 to a DPI-unaware
    # process whatever the desktop is doing, so the first version of this helper
    # reported the 150% screen as 100%, asked for a window that could not fit the
    # ribbon, and then blamed the application for four checks it could not physically
    # have passed.  Where nothing answers, 1 is what the phase assumed all along.
    param([string]$Hwnd)
    $r = Invoke-Proc -File $winshot -Arguments @('--dpi', $Hwnd) -WorkDir $out -Timeout 20
    $dpi = 0
    if ([int]::TryParse([regex]::Match($r.Out, 'dpi=(\d+)').Groups[1].Value, [ref]$dpi)) {
        if ($dpi -ge 96) { return [math]::Round($dpi / 96, 4) }
    }
    return 1
}

function ConvertTo-LogicalSize {
    # What the application itself sees for a physical client rectangle: QML
    # measures the content area in logical pixels, so this is the only way to
    # check a resize drive against a breakpoint written in Main.qml.
    param([string]$Client, [double]$Scale)
    $parts = $Client -split 'x'
    if ($parts.Count -ne 2) { return '0x0' }
    $w = [math]::Round([int]$parts[0] / $Scale)
    $h = [math]::Round([int]$parts[1] / $Scale)
    return ([string][int]$w + 'x' + [string][int]$h)
}

# ---------------------------------------------------------------------------
# launch plumbing
# ---------------------------------------------------------------------------

function Start-Zuuned {
    param([string]$Profile, [string]$Suffix, [string[]]$Arguments)
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    $psi.Arguments = Format-Args -Arguments $Arguments
    $psi.WorkingDirectory = $build
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $psi.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $psi.EnvironmentVariables['ZUUNED_PROFILE_DIR'] = $Profile
    $psi.EnvironmentVariables['ZUUNED_LOCK_SUFFIX'] = $Suffix
    $psi.EnvironmentVariables['ZUUNED_MPV_AO'] = 'null'
    $psi.EnvironmentVariables['PATH'] = (Join-Path $QtDir 'bin') + ';' + $FfmpegDir + ';' + $env:PATH
    $p = [System.Diagnostics.Process]::Start($psi)
    return [pscustomobject]@{
        Proc = $p
        OutTask = $p.StandardOutput.ReadToEndAsync()
        ErrTask = $p.StandardError.ReadToEndAsync()
        Profile = $Profile
        Suffix = $Suffix
        Killed = $false
    }
}

function Resolve-ZuunedWindowByPid {
    # Win32 caches nothing, so this is the ground truth for "which visible window
    # belongs to this process id".
    param([int]$ProcessId)
    $r = Invoke-Proc -File $winshot -Arguments @('--pid', [string]$ProcessId) -WorkDir $out -Timeout 20
    if ($r.Code -ne 0) { return [IntPtr]::Zero }
    $m = [regex]::Match($r.Out, 'hwnd=(\d+)')
    if (-not $m.Success) { return [IntPtr]::Zero }
    return ConvertFrom-HwndDecimal -Text $m.Groups[1].Value
}

function Wait-ZuunedWindow {
    # PowerShell 7 reads Process.MainWindowHandle once and then keeps the answer, so
    # a harness that looks at it while the splash is still mapping reports an empty
    # handle for a window the user can plainly see: one probe found no handle on 30
    # consecutive healthy launches.  Refresh() drops that cache and the pid scan is
    # the check that cannot lie, and the case records which one answered because a
    # screenshot of a handle nobody can explain is not evidence.
    param($Handle, [int]$Seconds = 30)
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ($true) {
        if ($Handle.Proc.HasExited) {
            return [pscustomobject]@{ Handle = [IntPtr]::Zero; Source = 'exited' }
        }
        $Handle.Proc.Refresh()
        $h = $Handle.Proc.MainWindowHandle
        if ($h -ne [IntPtr]::Zero) {
            return [pscustomobject]@{ Handle = $h; Source = 'property' }
        }
        $h = Resolve-ZuunedWindowByPid -ProcessId $Handle.Proc.Id
        if ($h -ne [IntPtr]::Zero) {
            $script:pidScans++
            return [pscustomobject]@{ Handle = $h; Source = 'pid-scan' }
        }
        if ((Get-Date) -ge $deadline) {
            return [pscustomobject]@{ Handle = [IntPtr]::Zero; Source = 'none' }
        }
        Start-Sleep -Milliseconds 250
    }
}

function Stop-Zuuned {
    # Closing the window is the shutdown path a user takes; Kill is recorded as a
    # defect because it means the app would not come down politely.
    param($Handle, [int]$Seconds = 30)
    $p = $Handle.Proc
    $closeSeconds = 0.0
    if (-not $p.HasExited) {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        [void]$p.CloseMainWindow()
        if (-not $p.WaitForExit($Seconds * 1000)) {
            $Handle.Killed = $true
            try { $p.Kill($true) } catch { $p.Kill() }
            [void]$p.WaitForExit(5000)
        }
        $clock.Stop()
        $closeSeconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
    }
    $text = ($Handle.ErrTask.Result -split "`r?`n") + ($Handle.OutTask.Result -split "`r?`n")
    $code = $p.ExitCode
    if ($Handle.Killed) {
        # A destroyed process reports an exit code it never chose, so 124 is the
        # gate's own: a forced kill can never be read as a clean shutdown.
        $code = 124
    }
    return [pscustomobject]@{
        Code = $code
        Killed = [bool]$Handle.Killed
        CloseSeconds = $closeSeconds
        Lines = @($text | Where-Object { $_ -ne '' })
    }
}

# Only the harness's own instrumentation is forgiven, and it is forgiven by
# name.  A blanket 'qml:' allowance used to sit in this pattern, and that is
# what hid a real defect for a whole checkpoint: a QML binding that threw on
# every transfer-queue row printed
#   [qml-warn] qrc:/qt/qml/Zuuned/QueueEntryView.qml:147: TypeError: ...
# which matched, so the gate stayed green while the app was broken.  From here
# an engine warning is a failure, and the [qa] reports opt out by name.
$script:ownReport = '^\[(debug|info|warning)\] qml: \[qa'
$script:noisePattern =
    'TypeError|ReferenceError|Cannot read|is not defined|does not exist|Local logs unavailable|did not capture|profile isolation is incomplete'

function Find-Noise {
    param([string[]]$Lines)
    return @($Lines | Where-Object {
        (($_ -match $script:noisePattern) -or ($_ -match '^\[qml-')) -and
        $_ -notmatch $script:ownReport
    })
}

function Note-Diagnostics {
    # Recorded, not asserted: warning lines worth reading once the hard failures
    # are gone, without letting a benign one turn the gate red.
    param([string]$Case, [string[]]$Lines)
    foreach ($line in $Lines) {
        if ($line -match '(?i)(warn|error|fail)' -and $line -notmatch $script:noisePattern -and $line -notmatch $script:ownReport) {
            $script:noise.Add($Case + ' | ' + $line.Trim())
        }
    }
}

function Read-Qa {
    # The app's own account of what it is showing, newest first.
    param([string[]]$Lines, [string]$Selector = '')
    $matches2 = @($Lines | Where-Object { $_ -match '\[qa\] ' })
    if ($Selector) { $matches2 = @($matches2 | Where-Object { $_ -match $Selector }) }
    if ($matches2.Count -eq 0) { return $null }
    $line = [string]$matches2[-1]
    $table = @{}
    foreach ($m in [regex]::Matches($line, '([a-zA-Z]+)=([^\s]+)')) {
        $table[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    $table['line'] = $line
    return $table
}

function Read-QaAll {
    # Every report the application made, in order.  Read-Qa answers "what did it
    # end up showing"; a case that drives a resize needs "did it pass through the
    # state that resize is supposed to cause", and that state only ever appears in
    # the middle of the log.
    param([string[]]$Lines)
    $table = @()
    foreach ($line in @($Lines | Where-Object { $_ -match '\[qa\] ' })) {
        $entry = @{}
        foreach ($m in [regex]::Matches([string]$line, '([a-zA-Z]+)=([^\s]+)')) {
            $entry[$m.Groups[1].Value] = $m.Groups[2].Value
        }
        $entry['line'] = [string]$line
        $table += $entry
    }
    return $table
}

function Invoke-Capture {
    param([string]$Hwnd, [string]$Png)
    $r = Invoke-Proc -File $winshot -Arguments @($Hwnd, $Png) -WorkDir $out -Timeout 60
    $parsed = @{}
    foreach ($m in [regex]::Matches($r.Out, '([a-z]+)=([^\s]+)')) {
        $parsed[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    $parsed['Code'] = $r.Code
    $parsed['Png'] = $Png
    $parsed['Out'] = $r.Out.Trim()
    return $parsed
}

function Format-KvDetail {
    # Keep a failure detail readable in a console whose code page is not UTF-8: the
    # python companions print the CJK fixture only on the escaped paths= and titles=
    # lines, which are exactly the lines worth leaving out of a one-line detail.
    param([string]$Out)
    return ((($Out -split "`r?`n") | Where-Object {
        $_ -match '^[a-z_]+=' -and $_ -notmatch '^(paths|titles)='
    }) -join ' ')
}

function Get-PaintVerdict {
    # A painted Zuuned page measures about 9,500 distinct colours with the most
    # common one near 61% of the frame; a window that never presented measures
    # one colour.  The thresholds stay far away from both.
    param($Shot)
    $distinct = 0
    $dominant = 0.0
    $mean = 0.0
    [void][int]::TryParse([string]$Shot['distinct'], [ref]$distinct)
    [void][double]::TryParse([string]$Shot['dominant'], [ref]$dominant)
    [void][double]::TryParse([string]$Shot['mean'], [ref]$mean)
    return [pscustomobject]@{
        Painted = ($Shot.Code -eq 0 -and $distinct -ge 300 -and $dominant -lt 0.97 -and $mean -ge 2)
        Distinct = $distinct
        Dominant = $dominant
        Mean = $mean
    }
}

function Invoke-Case {
    <#
      One instrumented launch: its own profile folder, its own instance lock, the
      null audio driver, a settle long enough to clear the two-second splash, a
      capture of what it actually painted, and a polite window close.  $Drive runs
      while the window is up for cases that have to change something first (a
      resize, a second capture).
    #>
    param(
        [string]$Name,
        [string[]]$Arguments = @(),
        [switch]$Seed,
        [string]$ProfileDir = '',
        [scriptblock]$Drive
    )
    $script:caseSeq++
    $tag = $script:caseSeq.ToString('00') + '-' + $Name
    # A case that has to re-open what an earlier launch left behind names its profile
    # folder exactly instead of taking a fresh one under profiles\.
    $profile = if ($ProfileDir) { $ProfileDir } else { Join-Path $profiles $tag }
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    if ($Seed) {
        # Deliberately no settings ini.  Both online questions ship off, so an
        # untouched profile is the honest fixture, and the music page case then
        # proves the defaults rather than a value written here.
        Copy-Item -LiteralPath (Join-Path $out 'seed-library.db') -Destination (Join-Path $profile 'library.db')
    }
    $handle = Start-Zuuned -Profile $profile -Suffix ('wus' + $script:caseSeq) `
        -Arguments (@($Arguments) + @('--qa-probe'))
    $script:pids.Add($handle.Proc.Id)
    $window = Wait-ZuunedWindow -Handle $handle
    $hwnd = $window.Handle
    $shot = $null
    $verdict = $null
    $extra = $null
    if ($hwnd -ne [IntPtr]::Zero) {
        Start-Sleep -Seconds $Settle
        $shot = Invoke-Capture -Hwnd (ConvertTo-HwndDecimal -Handle $hwnd) -Png (Join-Path $out ('shots\' + $tag + '.png'))
        $verdict = Get-PaintVerdict -Shot $shot
        if ($Drive) { $extra = & $Drive $handle $hwnd $tag }
    }
    $stop = Stop-Zuuned -Handle $handle
    if ($stop.Killed) { $script:killed++ }
    Note-Diagnostics -Case $tag -Lines $stop.Lines
    return [pscustomobject]@{
        Name = $Name
        Tag = $tag
        Profile = $profile
        WindowSeen = ($hwnd -ne [IntPtr]::Zero)
        Hwnd = $hwnd
        HwndSource = $window.Source
        Shot = $shot
        Verdict = $verdict
        Extra = $extra
        Exit = $stop.Code
        Killed = $stop.Killed
        CloseSeconds = $stop.CloseSeconds
        Lines = $stop.Lines
        Noise = @(Find-Noise -Lines $stop.Lines)
        QaSplash = Read-Qa -Lines $stop.Lines -Selector '\[qa\] splash '
        QaLast = Read-Qa -Lines $stop.Lines
    }
}

function Assert-Case {
    param($Case, [string]$Label, [string]$ExpectPage = '', [string]$ExpectOnboarding = '')
    $problems = @()
    if (-not $Case.WindowSeen) { $problems += 'no window appeared' }
    if ($Case.Killed) { $problems += 'did not close politely' }
    if ($Case.Exit -ne 0) { $problems += ('exit=' + $Case.Exit) }
    if ($Case.Noise.Count -gt 0) { $problems += ('stderr: ' + $Case.Noise[0].Trim()) }
    if (-not $Case.Verdict) { $problems += 'no capture' }
    elseif (-not $Case.Verdict.Painted) {
        $problems += ('blank-ish distinct=' + $Case.Verdict.Distinct + ' dominant=' + ('{0:N3}' -f $Case.Verdict.Dominant))
    }
    $qa = $Case.QaSplash
    if (-not $qa) { $problems += 'no [qa] splash report' }
    else {
        if ($ExpectPage -and $qa['page'] -ne $ExpectPage) { $problems += ('page=' + $qa['page']) }
        if ($ExpectOnboarding -and $qa['onboarding'] -ne $ExpectOnboarding) {
            $problems += ('onboarding=' + $qa['onboarding'])
        }
    }
    Check -Name $Label -Condition ($problems.Count -eq 0) -Detail ($problems -join '; ')
}

function Get-RealProfileFiles {
    # The profile the user actually uses, as files.  A byte-for-byte compare would
    # blame this gate for the daily Zuuned checkpointing its own library.db-wal, so
    # the gate asserts attribution instead: nothing it started may show up here.
    param([DateTime]$Since = [DateTime]::MinValue)
    $found = @()
    foreach ($target in @((Join-Path $env:APPDATA 'Zuuned'), (Join-Path $env:LOCALAPPDATA 'Zuuned'))) {
        if (-not (Test-Path -LiteralPath $target)) { continue }
        $found += Get-ChildItem -LiteralPath $target -Recurse -File -Force -ErrorAction SilentlyContinue
    }
    return @($found | Where-Object { $_.CreationTimeUtc -ge $Since })
}

function Get-RealProfileFingerprint {
    # The point of the whole run: an instrumented launch must not move a byte of
    # the profile the user actually uses.
    $count = 0
    $bytes = 0
    $newest = 0
    foreach ($target in @((Join-Path $env:APPDATA 'Zuuned'), (Join-Path $env:LOCALAPPDATA 'Zuuned'))) {
        if (-not (Test-Path -LiteralPath $target)) { continue }
        Get-ChildItem -LiteralPath $target -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
            $count++
            $bytes += $_.Length
            if ($_.LastWriteTimeUtc.Ticks -gt $newest) { $newest = $_.LastWriteTimeUtc.Ticks }
        }
    }
    return ($count.ToString() + ' files / ' + $bytes + ' bytes / newest ' + $newest)
}

$ffmpeg = Join-Path $FfmpegDir 'ffmpeg.exe'
$fixtureMusic = Join-Path $fixture 'music'
$seedDb = Join-Path $out 'seed-library.db'
$tone = Join-Path $fixtureMusic 'Tone A.mp3'
$pythonExe = ''
if ($python) { $pythonExe = $python.Source }
$script:runStart = (Get-Date).ToUniversalTime()
$beforeFp = Get-RealProfileFingerprint

Write-Host '== capture tool'
# The run wipes its own scratch directory, so the capture tool is rebuilt on
# every invocation, filtered or not: every later phase photographs through it.
$r = Invoke-Proc -File 'cl.exe' -Arguments @(
    '/nologo', '/W3', '/Od', '/Zi', '/MD', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    (Join-Path $root 'tests\win-usability\winshot.cpp'),
    ('/Fe:' + $winshot),
    ('/Fo:' + (Join-Path $out 'winshot.obj')),
    ('/Fd:' + (Join-Path $out 'winshot.pdb')),
    '/link', 'user32.lib', 'gdi32.lib', 'gdiplus.lib'
) -WorkDir $root -Timeout 240
Check 'winshot compiles' ($r.Code -eq 0 -and (Test-Path $winshot)) (($r.Out + $r.Err).Trim())
Check 'winshot build leaves no incremental pdb in the checkout' `
    (-not (Test-Path (Join-Path $root 'vc140.pdb')))

Write-Host ''
Write-Host '== QML type names: QtQuick exports a PathText of its own'
# Deliberately outside any phase: this reads source, launches nothing, and costs
# nothing, so every filtered run keeps it.  Qt6Quick.dll exports PathText (and
# PathSvg), so a QML file that imports QtQuick without Zuuned resolves the name to
# Qt's type and every binding on it throws at run time - which is exactly how the
# queue rows lost their artwork, silently, with the gate passing.  The app's
# singleton answers to ZPathText now, and no phase here loads a QML file that
# forgets the import, so the rule can only be read off the source.
$clash = @(Get-ChildItem (Join-Path $root 'qml') -Filter '*.qml' -Recurse |
           Where-Object {
               [regex]::Matches([string](Get-Content -LiteralPath $_.FullName -Raw),
                                '(?m)(?<![A-Za-z0-9_./])PathText\s*\.').Count -gt 0
           })
Check 'no QML file uses a type name QtQuick also exports' `
    ($clash.Count -eq 0) ('files=' + ($clash.Name -join ' '))
$pathHeader = [string](Get-Content -LiteralPath (Join-Path $root 'src\PathText.h') -Raw)
Check 'the path singleton is exposed to QML as ZPathText' `
    ($pathHeader -match 'QML_NAMED_ELEMENT\(ZPathText\)')

Write-Host ''
Write-Host '== scan fixture: CJK album folder, path past MAX_PATH, WMA named .mp3'
if (Select-Phase 'fixture') {
    if (-not $pythonExe) {
        Skip 'the scan fixture phase' 'python is not on PATH'
    }
    else {
        $fx = Invoke-Proc -File $pythonExe -Arguments @(
            (Join-Path $PSScriptRoot 'make_scan_fixture.py'), $fixtureMusic,
            '--ffmpeg', $ffmpeg, '--force',
            '--extra-roots', (Join-Path $fixture 'nonascii-roots'),
            '--photos', (Join-Path $fixture 'photos')) -WorkDir $out -Timeout 300
        Check 'the fixture put four tracks on disk' `
            ($fx.Code -eq 0 -and $fx.Out -match 'created=4') `
            (Format-KvDetail -Out ($fx.Out + [Environment]::NewLine + $fx.Err))
        # An empty-artwork assertion is only worth what its fixture is worth, so the
        # one picture the art checks depend on is named here as its own fact.
        Check 'the fixture put one album folder cover on disk' `
            ($fx.Out -match 'album_covers=1') ('out=' + ($fx.Out -split "`r?`n" |
                Where-Object { $_ -match 'album_covers' } | Select-Object -First 1))

        $scanDb = Join-Path $out 'scan-library.db'
        $scan = Invoke-Proc -File $libraryTool -Arguments @('scan', $fixtureMusic, $scanDb) -Timeout 900
        Check 'librarytool scan returns 0 over the CJK and deep-path tree' `
            ($scan.Code -eq 0) ('code=' + $scan.Code)
        Check 'librarytool scan counted four tracks' `
            ($scan.Out -match '4 tracks added') 'no added-track line in the captured output'

        $insp = Invoke-Proc -File $pythonExe -Arguments @(
            (Join-Path $PSScriptRoot 'inspect_scan_db.py'), $scanDb,
            '--expect-tracks', '4', '--expect-codepoints', '0x6F22,0x8A5E',
            '--min-longest-path', '260') -WorkDir $out -Timeout 180
        Check 'the scan stored the Chinese album folder and a path past 260 characters' `
            ($insp.Code -eq 0) (Format-KvDetail -Out $insp.Out)
        Check 'every scanned track was probed and left copy evidence' `
            ($insp.Out -match 'probe_failures=0' -and $insp.Out -match 'fingerprints=4' `
                -and $insp.Out -match 'nonzero_durations=yes') (Format-KvDetail -Out $insp.Out)
        # A non-ASCII scan root used to be the quiet failure: argv went over in
        # the local ANSI code page, the scan found nothing, and it still exited 0.
        # One scan per root so a root that works cannot mask one that does not.
        $rootsDir = Join-Path $fixture 'nonascii-roots'
        $rootDirs = @(Get-ChildItem -LiteralPath $rootsDir -Directory -ErrorAction SilentlyContinue |
                      Sort-Object Name)
        Check 'the fixture put three non-ASCII scan roots on disk' `
            ($fx.Out -match 'extra_roots=3' -and $rootDirs.Count -eq 3) `
            ('found=' + $rootDirs.Count)
        $ri = 0
        foreach ($rd in $rootDirs) {
            $ri++
            $named = $rd.Name -match '[^\x20-\x7E]'
            $rs = Invoke-Proc -File $libraryTool -Arguments @(
                'scan', $rd.FullName, (Join-Path $out ('nonascii-root-' + $ri + '.db'))) -Timeout 300
            Check ('librarytool scan counts its one track in non-ASCII root ' + $ri) `
                ($named -and $rs.Code -eq 0 -and $rs.Out -match ': 1 tracks added') `
                ('code=' + $rs.Code + ' nonAsciiName=' + $named + ' ' `
                 + ($rs.Out + ' ' + $rs.Err).Trim())
        }
        # The photo category is the one the ordinary scan reaches through a
        # different table and a different branch, and until now no Windows gate
        # had ever walked it.  The folder name here is the same kind of text a Zune
        # photo album is named from on a real send, so the CJK bytes that survive
        # this scan are the bytes a transfer depends on.
        Check 'the fixture put two photos in a Chinese-named album folder' `
            ($fx.Out -match 'photos=2') ('out=' + $fx.Out.Trim())
        $fixturePhotos = Join-Path $fixture 'photos'
        $photoScan = Invoke-Proc -File $libraryTool -Timeout 300 `
            -Arguments @('photoscan', $fixturePhotos, (Join-Path $out 'scan-photos.db'))
        Check 'librarytool photoscan counts both photos in the Chinese folder' `
            ($photoScan.Code -eq 0 -and $photoScan.Out -match 'total photos in db: 2') `
            ('code=' + $photoScan.Code + ' ' + ($photoScan.Out + ' ' + $photoScan.Err).Trim())
        $photoInsp = Invoke-Proc -File $pythonExe -Timeout 180 -Arguments @(
            (Join-Path $PSScriptRoot 'inspect_scan_db.py'), (Join-Path $out 'scan-photos.db'),
            '--expect-photos', '2', '--expect-codepoints', '0x7167,0x7247') -WorkDir $out
        Check 'the photo scan stored the Chinese album folder intact' `
            ($photoInsp.Code -eq 0) (Format-KvDetail -Out $photoInsp.Out)

        # A root stored as "/E:/Music" is not absolute on Windows: it resolves
        # against that drive's current directory, so the folder scanned nothing
        # and reported itself unreachable for ever.  LibraryDb repairs those
        # rows when it opens the library - prove it through a real librarytool
        # run rather than a unit test, because the scar came from the wiring
        # between the picker and the database, not from the database alone.
        $scarDb = Join-Path $out 'scarred-roots.db'
        $emptyRoot = Join-Path $out 'empty-scan-root'
        New-Item -ItemType Directory -Force -Path $emptyRoot | Out-Null
        $scarSeed = Invoke-Proc -File $pythonExe -Timeout 120 -Arguments @(
            (Join-Path $PSScriptRoot 'watch_folder_repair.py'), '--seed', $scarDb)
        Check 'the mangled watch-folder fixture seeded four scarred roots' `
            ($scarSeed.Code -eq 0 -and $scarSeed.Out -match 'seeded=4') `
            (Format-KvDetail -Out ($scarSeed.Out + ' ' + $scarSeed.Err))
        $scarScan = Invoke-Proc -File $libraryTool -Arguments @('scan', $emptyRoot, $scarDb) -Timeout 300
        Check 'librarytool opens a library full of mangled watch-folder roots' `
            ($scarScan.Code -eq 0) ('code=' + $scarScan.Code + ' ' `
             + ($scarScan.Out + ' ' + $scarScan.Err).Trim())
        $scarCheck = Invoke-Proc -File $pythonExe -Timeout 120 -WorkDir $out -Arguments @(
            (Join-Path $PSScriptRoot 'watch_folder_repair.py'), '--check', $scarDb,
            '--allow', $emptyRoot)
        Check 'opening the library repaired every mangled watch-folder root' `
            ($scarCheck.Code -eq 0 -and $scarCheck.Out -match 'OVERALL=ok') `
            (Format-KvDetail -Out ($scarCheck.Out + ' ' + $scarCheck.Err))
        if ($insp.Code -eq 0) {
            Copy-Item -LiteralPath $scanDb -Destination $seedDb -Force
        }
    }
}

$seeded = Test-Path -LiteralPath $seedDb

function Get-QaField {
    # A missing report has to read as a failed check, not as an index error in the
    # middle of a phase.
    param($Entry, [string]$Key)
    if ($null -eq $Entry) { return '' }
    return [string]$Entry[$Key]
}

function Get-QaSize {
    # 'size=1384x1181' as numbers.  The breakpoints the application uses are logical
    # pixels, so the gate compares what the window itself reported instead of what
    # winshot asked for: a minimum window size can move the second number.
    param($Entry)
    $w = 0
    $h = 0
    $m = [regex]::Match((Get-QaField $Entry 'size'), '^(\d+)x(\d+)$')
    if ($m.Success) {
        $w = [int]$m.Groups[1].Value
        $h = [int]$m.Groups[2].Value
    }
    return [pscustomobject]@{ W = $w; H = $h }
}

function Test-StatePath {
    # Subsequence match.  The QA reports are sampled on a timer rather than emitted
    # on every edge, so a resize shows up once or three times depending on how long
    # the drive dwelled in it; only the order of the states is meaningful.
    param([string[]]$States, [string[]]$Path)
    $at = 0
    foreach ($state in $States) {
        if ($state -eq $Path[$at]) {
            $at++
            if ($at -ge $Path.Count) { return $true }
        }
    }
    return $false
}

Write-Host ''
Write-Host '== pages'
if (Select-Phase 'pages') {
    if (-not $seeded) {
        Skip 'one launch case per --page' 'no seed library, so every case would only ever see onboarding'
    }
    else {
        foreach ($p in @('music', 'videos', 'photos', 'podcasts', 'audiobooks')) {
            $pageCase = Invoke-Case -Name ('page-' + $p) -Arguments @('--page', $p) -Seed
            Assert-Case -Case $pageCase -Label ('page ' + $p + ' paints and reports itself') `
                -ExpectPage $p -ExpectOnboarding 'false'
            if ($p -eq 'music') {
                Check 'the seeded library reaches the running application' `
                    ((Get-QaField $pageCase.QaSplash 'tracks') -eq '4') `
                    ('tracks=' + (Get-QaField $pageCase.QaSplash 'tracks'))
                # Path spelling, asserted rather than eyeballed.  The library holds one
                # canonical forward-slash identity per watch folder and the UI renders
                # the native spelling PathText produced from it; a screenshot check is
                # how /E:/Music once survived a release.
                $wfRows = Get-QaField $pageCase.QaSplash 'wfRows'
                $wfNative = Get-QaField $pageCase.QaSplash 'wfNative'
                $wfRaw = Get-QaField $pageCase.QaSplash 'wfRaw'
                $wfStored = Get-QaField $pageCase.QaSplash 'wfStored'
                $wfLabel = Get-QaField $pageCase.QaSplash 'wfLabel'
                $wfAny = $false
                if ($wfRows -match '^[0-9]+$') { $wfAny = [int]$wfRows -ge 1 }
                Check 'the seeded library reached the application as a watch folder' `
                    $wfAny ('wfRows=' + $wfRows)
                Check 'every watch-folder label the app reports is spelled natively' `
                    ($wfRaw -eq '0' -and $wfNative -eq $wfRows) `
                    ('wfNative=' + $wfNative + ' wfRaw=' + $wfRaw + ' wfRows=' + $wfRows)
                Check 'watch folders are still stored with forward slashes' `
                    ($wfStored -eq $wfRows) ('wfStored=' + $wfStored + ' wfRows=' + $wfRows)
                Check 'the QML display helper and the row it feeds agree' `
                    ($wfLabel -eq '1') ('wfLabel=' + $wfLabel)
                # Where a download would land, asserted through the running app.
                # The seed library holds two music roots and nobody marked
                # either, so exactly one row may read as the default: the label
                # is derived from where a pull would actually be written, per
                # kind, not echoed off a stored column the list happens to show.
                $wfDefaults = Get-QaField $pageCase.QaSplash 'wfDefaults'
                Check 'exactly one watch folder reports itself the default import folder' `
                    ($wfDefaults -eq '1') ('wfDefaults=' + $wfDefaults + ' wfRows=' + $wfRows)
                # And the words on those rows.  A default paints a status, a
                # non-default paints a button, so the status string has to exist
                # for exactly the rows the flag names -- one stale or blank label
                # and a row tells the user a download lands somewhere it will not.
                $wfDefaultLabels = Get-QaField $pageCase.QaSplash 'wfDefaultLabels'
                $wfDefaultAgree = Get-QaField $pageCase.QaSplash 'wfDefaultAgree'
                Check 'only a default row carries a default status string' `
                    ($wfDefaultLabels -eq $wfDefaults -and $wfDefaultAgree -eq $wfRows) `
                    ('wfDefaultLabels=' + $wfDefaultLabels + ' wfDefaultAgree=' `
                        + $wfDefaultAgree + ' wfDefaults=' + $wfDefaults `
                        + ' wfRows=' + $wfRows)
                # Artwork identity, asserted rather than read off a picture.  The
                # island, a queue row and the media-property report all ask the art
                # cache for the album's cover; a row that carries a track artist as
                # well asked with that name once, which is a different cache key, so
                # one view had art and the other did not and the same picture got
                # extracted twice.  The seed now carries one real folder cover, so
                # the same assertion also says the album that HAS art resolves it the
                # same way in both places, not just the two that have none.
                $artKeyOwner = Get-QaField $pageCase.QaLast 'artKeyOwner'
                $artKeyDecoy = Get-QaField $pageCase.QaLast 'artKeyDecoy'
                Check 'the player asks for album art as the album owner, never the track artist' `
                    ($artKeyOwner -eq '1' -and $artKeyDecoy -eq '1') `
                    ('artKeyOwner=' + $artKeyOwner + ' artKeyDecoy=' + $artKeyDecoy)
                # Whether any artwork reached the grid at all.  The shipping defaults answer
                # both online questions with "no" and the fixture puts one Cover.jpg in
                # one album folder, so a non-zero count can only be the user's own bytes
                # extracted into that profile's art cache - the half of album art no
                # Windows gate had ever reached, and the half a user with covers in
                # their files cares about.  Read from the last report rather than the
                # splash one because extraction is thread-pool work that lands after
                # the first frame paints.
                $artAlbums = Get-QaField $pageCase.QaLast 'albums'
                $artWith = Get-QaField $pageCase.QaLast 'artWith'
                $artAgree = Get-QaField $pageCase.QaLast 'artAgree'
                $artDecoy = Get-QaField $pageCase.QaLast 'artDecoy'
                $artWithCount = 0
                if ($artWith -match '^[0-9]+$') { $artWithCount = [int]$artWith }
                Check 'the fixture cover is extracted and reaches the running album grid' `
                    ($artWithCount -ge 1) ('albums=' + $artAlbums + ' artWith=' + $artWith)
                Check 'every album resolves the same picture in the grid and the player' `
                    ($artAgree -eq $artAlbums -and $artDecoy -eq $artAlbums) `
                    ('albums=' + $artAlbums + ' artAgree=' + $artAgree + ' artDecoy=' + $artDecoy)
                # The artist tile. A local library owns no portrait file, so an
                # artist wears the cover of one of their own albums, and every album
                # the cache holds a picture for has to land one. The Artists page is
                # the page the app opens on, and it refused any album fallback at
                # all: a library with real covers in its files painted nothing but
                # empty circles and "Needs a match" there, on every platform.
                $ownerWanted = Get-QaField $pageCase.QaLast 'artOwnerWanted'
                $ownerFound = Get-QaField $pageCase.QaLast 'artOwnerFound'
                Check 'every album with a cover hands one to its artist tile' `
                    ($ownerWanted -match '^[0-9]+$' -and $ownerWanted -eq $ownerFound `
                     -and [int]$ownerWanted -ge 1) `
                    ('artOwnerWanted=' + $ownerWanted + ' artOwnerFound=' + $ownerFound)
                # And the tie-break, proved on invented rows so it bites whether or
                # not this seed happens to hold two albums for one person.
                $rankOk = Get-QaField $pageCase.QaLast 'artRankOk'
                $preferArtOk = Get-QaField $pageCase.QaLast 'artPreferArtOk'
                Check 'an artist takes the album that has art and size breaks only a tie' `
                    ($rankOk -eq '1' -and $preferArtOk -eq '1') `
                    ('artRankOk=' + $rankOk + ' artPreferArtOk=' + $preferArtOk)
                # Opt-in lookup.  A launch used to reach musicbrainz.org once per
                # album without embedded art and once per artist on the grid, so a
                # user with covers in their files watched requests they never asked
                # for.  The app now says out loud that it asked nothing, and the one
                # line the provider layer writes about a request must not appear.
                $quiet = @($pageCase.Lines | Where-Object { $_ -match 'online lookup off' })
                $asked = @($pageCase.Lines | Where-Object { $_ -match 'customize-http' })
                Check 'a music launch reports that it looked nothing up online' `
                    ($quiet.Count -ge 1) ('lines=' + $quiet.Count)
                Check 'a music launch contacts no artwork provider' `
                    ($asked.Count -eq 0) ('' + ($asked | Select-Object -First 1))
            }
        }
        # A device page with nothing attached is the case a user hits every time the
        # cable is out; it must open and stay quiet even though no device claim here
        # is possible.
        $deviceCase = Invoke-Case -Name 'page-deviceMusic' -Arguments @('--page', 'deviceMusic') -Seed
        Assert-Case -Case $deviceCase -Label 'a device page opens with no device attached and stays clean' `
            -ExpectOnboarding 'false'
        Write-Host ('  note device page reported page=' + (Get-QaField $deviceCase.QaSplash 'page') +
            ' panel=' + (Get-QaField $deviceCase.QaSplash 'panel'))
    }
}

Write-Host ''
Write-Host '== settings tabs'
if (Select-Phase 'settings') {
    if (-not $seeded) {
        Skip 'the five settings tabs' 'no seed library'
    }
    else {
        foreach ($tab in @('Appearance', 'Library', 'Device', 'Mixtapes', 'Health')) {
            $tabCase = Invoke-Case -Name ('tab-' + $tab) -Seed `
                -Arguments @('--page', 'settings', '--settings-tab', $tab)
            Assert-Case -Case $tabCase -Label ('settings tab ' + $tab + ' paints on the settings page') `
                -ExpectPage 'settings' -ExpectOnboarding 'false'
        }
    }
}

Write-Host ''
Write-Host '== chrome collapse pin'
if (Select-Phase 'collapse') {
    if (-not $seeded) {
        Skip 'both collapse states' 'no seed library'
    }
    else {
        foreach ($pin in @('0', '1')) {
            $pinCase = Invoke-Case -Name ('collapse-' + $pin) -Seed `
                -Arguments @('--page', 'music', '--collapse', $pin)
            Assert-Case -Case $pinCase -Label ('the header at collapse pin ' + $pin + ' paints') `
                -ExpectPage 'music' -ExpectOnboarding 'false'
            $want = ([double]$pin).ToString('0.00')
            Check ('the window reports collapse=' + $want) `
                ((Get-QaField $pinCase.QaSplash 'collapse') -eq $want) `
                ('collapse=' + (Get-QaField $pinCase.QaSplash 'collapse'))
        }
    }
}

Write-Host ''
Write-Host '== compact playback'
if (Select-Phase 'compact') {
    if (-not ($seeded -and (Test-Path -LiteralPath $tone))) {
        Skip 'the compact Now-Playing island' 'needs the seed library and the fixture tone'
    }
    else {
        # A default 1280x800 launch is already compact: the ribbon needs 1280 logical
        # pixels of width AND Theme.headroomCompactBelow (1100) of height, so the
        # useful question is not "does shrinking retire the ribbon" but whether the
        # window travels both ways across those breakpoints with a queue loaded.
        # The floor is Main.qml's own minimumWidth/Height (1100x700), still narrow
        # enough to retire the ribbon, and the drive asks for less than that on
        # purpose so the floor is what answers.
        $compactCase = Invoke-Case -Name 'compact' -Seed `
            -Arguments @('--page', 'music', '--qa-play', $tone) -Drive {
                param($Handle, $Hwnd, $Tag)
                $dec = ConvertTo-HwndDecimal -Handle $Hwnd
                $scale = Get-WindowScale -Hwnd $dec
                # The trip has to cross 1280x1100 logical, and MoveWindow speaks
                # physical pixels, so the tall stages come from the window's own
                # scale plus room for its own frame: measured at 150%, 2000x1770
                # physical answers 1319x1143 logical, and the frame there is 22 px
                # wide and 56 px tall.  A window taller than the monitor is fine -
                # MoveWindow does not clamp and the app never sees the screen -
                # which matters, because 1100 logical does not fit on a 1440p
                # display at 150% at all.  The narrow stage stays below Main.qml's
                # 1100x700 floor at every scale at or above 100%, which is the
                # point: the floor, not the request, has to answer there.
                $tallW = [int][math]::Ceiling(1280 * $scale) + 80
                $tallH = [int][math]::Ceiling(1100 * $scale) + 120
                $stages = @(
                    @{ Name = 'tall1'; W = $tallW; H = $tallH },
                    @{ Name = 'narrow'; W = 900; H = 640 },
                    @{ Name = 'tall2'; W = $tallW; H = $tallH }
                )
                $moves = @()
                $paint = @{}
                foreach ($stage in $stages) {
                    $moved = Invoke-Proc -File $winshot -Timeout 60 -WorkDir $out `
                        -Arguments @('--move', $dec, [string]$stage.W, [string]$stage.H)
                    $client = [regex]::Match($moved.Out, 'client=(\d+)x(\d+)')
                    $clientSize = $client.Groups[1].Value + 'x' + $client.Groups[2].Value
                    $moves += [pscustomobject]@{
                        Stage = $stage.Name
                        Asked = ([string]$stage.W + 'x' + [string]$stage.H)
                        Client = $clientSize
                        Logical = ConvertTo-LogicalSize -Client $clientSize -Scale $scale
                    }
                    Start-Sleep -Seconds 5
                    $shot = Invoke-Capture -Hwnd $dec -Png (Join-Path $shots ($Tag + '-' + $stage.Name + '.png'))
                    $paint[$stage.Name] = Get-PaintVerdict -Shot $shot
                }
                return [pscustomobject]@{ Moves = $moves; Paint = $paint; Scale = $scale }
            }
        Assert-Case -Case $compactCase -Label 'a window with a queued track paints' `
            -ExpectPage 'music' -ExpectOnboarding 'false'
        $all = @(Read-QaAll -Lines $compactCase.Lines)
        $last = $null
        if ($all.Count) { $last = $all[$all.Count - 1] }
        Check 'the QA queue reached the player and the tone played' `
            (@($all | Where-Object {
                (Get-QaField $_ 'queue') -eq '1' -and (Get-QaField $_ 'playing') -eq 'true' `
                    -and (Get-QaField $_ 'dur') -eq '6000'
            }).Count -gt 0) ('reports=' + $all.Count)

        $states = @()
        foreach ($entry in $all) {
            $size = Get-QaSize -Entry $entry
            $roomy = ($size.W -ge 1280 -and $size.H -ge 1100)
            $retired = ((Get-QaField $entry 'ribbonRetired') -eq 'true')
            $tucked = ((Get-QaField $entry 'collapsed') -eq 'true')
            $name = 'compact'
            if ($roomy) { $name = 'roomy' }
            if ($retired) { $name += '-retired' } else { $name += '-ribbon' }
            $states += [pscustomobject]@{
                Name = $name
                Roomy = $roomy
                Retired = $retired
                Ribbon = (-not $retired)
                Tucked = $tucked
                Island = ((Get-QaField $entry 'nowPlaying') -eq 'true')
                Line = (Get-QaField $entry 'line')
            }
        }
        $sequence = @()
        foreach ($state in $states) {
            if ($sequence.Count -eq 0 -or $sequence[-1] -ne $state.Name) { $sequence += $state.Name }
        }
        Write-Host ('  note state sequence: ' + ($sequence -join ' -> '))
        Write-Host ('  note window scale ' + $compactCase.Extra.Scale)
        foreach ($move in $compactCase.Extra.Moves) {
            Write-Host ('  note ' + $move.Stage + ' asked ' + $move.Asked +
                ' client ' + $move.Client + ' logical ' + $move.Logical)
        }

        # The trip the compact redesign is about, in the order the drive performed it.
        Check 'a compact window with a queue plays through the retired ribbon' `
            (Test-StatePath -States $sequence -Path @('compact-retired')) `
            ('sequence=' + ($sequence -join '>'))
        Check 'growing past 1280x1100 brings the top ribbon back' `
            (Test-StatePath -States $sequence -Path @('compact-retired', 'roomy-ribbon')) `
            ('sequence=' + ($sequence -join '>'))
        Check 'shrinking again retires the ribbon and the island is up' `
            (Test-StatePath -States $sequence -Path @('compact-retired', 'roomy-ribbon', 'compact-retired')) `
            ('sequence=' + ($sequence -join '>'))
        Check 'the ribbon survives a second grow' `
            (Test-StatePath -States $sequence `
                -Path @('compact-retired', 'roomy-ribbon', 'compact-retired', 'roomy-ribbon')) `
            ('sequence=' + ($sequence -join '>'))
        Check 'the header untucks with the ribbon and tucks again without it' `
            ((@($states | Where-Object { $_.Ribbon -and -not $_.Tucked }).Count -gt 0) -and
                (@($states | Where-Object { $_.Retired -and $_.Tucked }).Count -gt 0)) `
            ('ribbonStates=' + (@($states | Where-Object { $_.Ribbon }).Count))
        # Main.qml's floor is written in logical pixels, so a physical rectangle
        # has to be converted before it can be compared with it - at 150% the raw
        # numbers clear 1100x700 without the floor having had anything to do
        # with them.  The tolerance is for a scale that is not a whole fraction.
        Check 'the window respected its own minimum size when squeezed' `
            (@($compactCase.Extra.Moves | Where-Object { $_.Stage -eq 'narrow' } | ForEach-Object {
                $parts = $_.Logical -split 'x'
                ([int]$parts[0] -ge 1100) -and ([int]$parts[0] -le 1108) -and
                    ([int]$parts[1] -ge 700) -and ([int]$parts[1] -le 708)
            }).Count -eq 1) (($compactCase.Extra.Moves | ForEach-Object {
                $_.Stage + '=' + $_.Logical }) -join ' ')
        Check 'the roomy window painted' ($compactCase.Extra.Paint['tall2'].Painted) `
            ('distinct=' + $compactCase.Extra.Paint['tall2'].Distinct)
        Check 'the narrow window painted' ($compactCase.Extra.Paint['narrow'].Painted) `
            ('distinct=' + $compactCase.Extra.Paint['narrow'].Distinct)
        # Not asserted: the island answers its own close button, not a resize, so a
        # gate with no mouse cannot certify that closing it sticks.
        Write-Host ('  note after growing back: nowPlaying=' + (Get-QaField $last 'nowPlaying') +
            ' size=' + (Get-QaField $last 'size'))
    }
}

Write-Host ''
Write-Host '== command-line tools'
if (Select-Phase 'tools') {
    if (Test-Path -LiteralPath $tone) {
        $probe = Invoke-Proc -File $transcodeTool -Arguments @('probe', $tone) -Timeout 120
        Check 'transcodetool probes the MP3 fixture at six seconds' `
            ($probe.Code -eq 0 -and $probe.Out -match 'duration=6\.') ('out=' + $probe.Out.Trim())
        $sneaky = Invoke-Proc -File $transcodeTool -Timeout 120 `
            -Arguments @('probe', (Join-Path $fixtureMusic 'Sneaky Wma.mp3'))
        Check 'transcodetool sniffs the WMA stream inside a file named .mp3' `
            ($sneaky.Code -eq 0 -and $sneaky.Out -match 'duration=6\.') ('out=' + $sneaky.Out.Trim())
        # The library is called with UTF-8 by the app itself (FrameThumbnailService,
        # LibraryService and AudioCopyFingerprint all pass toUtf8()), while the CRT
        # handed main() its argv in the ANSI code page: the tool probed a name the
        # local code page cannot spell as question marks and failed on a file the
        # application opened happily.  Its argv comes off the wide command line now.
        # Two probes, because one name inside this code page and one outside it are
        # different questions - ?? survives code page 936 untouched, Hebrew does not.
        $han = [string][char]0x6F22 + [string][char]0x8A5E
        $cjkTrack = Join-Path $fixtureMusic ($han + ' Album (2024)\Track One.mp3')
        $cjkProbe = Invoke-Proc -File $transcodeTool -Timeout 120 -Arguments @('probe', $cjkTrack)
        Check 'transcodetool probes a track under a Chinese album folder' `
            ($cjkProbe.Code -eq 0 -and $cjkProbe.Out -match 'duration=6\.') ('out=' + $cjkProbe.Out.Trim())
        $heb = (-join ([char]0x5DB, [char]0x5EA, [char]0x5E8, [char]0x5D1, [char]0x5EA)) + ' Root'
        $hebTrack = Join-Path (Join-Path $fixture 'nonascii-roots') (Join-Path $heb 'Track One.mp3')
        if (Test-Path -LiteralPath $hebTrack) {
            $hebProbe = Invoke-Proc -File $transcodeTool -Timeout 120 -Arguments @('probe', $hebTrack)
            Check 'transcodetool probes a track whose root the ANSI code page cannot spell' `
                ($hebProbe.Code -eq 0 -and $hebProbe.Out -match 'duration=6\.') ('out=' + $hebProbe.Out.Trim())
        }
        else {
            Skip 'transcodetool probe outside the ANSI code page' 'the non-ASCII roots are not on disk in this run'
        }
    }
    else {
        Skip 'transcodetool probe' 'no fixture tone on disk'
    }
    $names = Invoke-Proc -File $libraryTool -Arguments @('names') -Timeout 600
    Check 'librarytool names passes its 17-case naming self-test' `
        ($names.Code -eq 0 -and $names.Out -match 'ALL PASS \(0 failures\)') ('code=' + $names.Code)
    $cust = Invoke-Proc -File $libraryTool -Arguments @('customize') -Timeout 900
    Check 'librarytool customize runs the v0/v2 to v11 migration chain' `
        ($cust.Code -eq 0 -and $cust.Out -match 'ALL PASS \(0 failures\)') ('code=' + $cust.Code)
    # The filename, path-comparison and playlist rules are header-only, so no other
    # part of this gate can see them: they are reached through the tool instead.
    $paths = Invoke-Proc -File $libraryTool -Arguments @('paths') -Timeout 600
    Check 'librarytool paths pins the Windows filename and playlist rules' `
        ($paths.Code -eq 0 -and $paths.Out -match 'ALL PASS \(0 failures\)') ('code=' + $paths.Code)
    # Retirement is the one scan behaviour that can wreck a library on its own,
    # and the case that matters most - a folder that is not mounted, a row nobody
    # owns - is one no GUI launch can produce on purpose.  It is reached through
    # the tool for that reason.
    $vanish = Invoke-Proc -File $libraryTool -Arguments @('vanish') -Timeout 600
    Check 'librarytool retires only rows whose absence a completed walk certified' `
        ($vanish.Code -eq 0 -and $vanish.Out -match 'ALL PASS \(0 failures\)') ('code=' + $vanish.Code)
    # Which folder receives a download, and which rows a folder owns, are two rules
    # no GUI launch can be pointed at on purpose: the first is one click whose result
    # is a file on a disk nobody is allowed to write to during a gate, and the second
    # needs a folder named in a script this harness cannot pass through a file dialog.
    $folders = Invoke-Proc -File $libraryTool -Arguments @('folders') -Timeout 600
    Check 'librarytool folders pins the import default and the non-ASCII folder prefix' `
        ($folders.Code -eq 0 -and $folders.Out -match 'ALL PASS \(0 failures\)') ('code=' + $folders.Code)
    # A watch folder is allowed to discard a small video as trailer debris, and a
    # scan that does so quietly is how a download looks lost.  Which folder may
    # discard, which may not, and what the scan has to say either way is a fact
    # about a completed walk, so it needs the scanner and a database rather than
    # a screenshot - and a 2 KB fixture rather than a 50 MB one.
    $videofloor = Invoke-Proc -File $libraryTool -Arguments @('videofloor') -Timeout 600
    Check 'librarytool videofloor pins what a video folder may discard and what it must report' `
        ($videofloor.Code -eq 0 -and $videofloor.Out -match 'ALL PASS \(0 failures\)') ('code=' + $videofloor.Code)
    # A watch folder that skips a kind has to say which files it skipped, and a
    # folder typed "all" has to say nothing, because it skipped nothing.  One
    # wrong word here and a user adds a music folder, sees a complaint about the
    # picture inside it, and is told to set the type it already has.  This needs
    # the real scanner over a real tree with all three kinds in it, which no
    # screenshot of a page with one music folder can show.
    $census = Invoke-Proc -File $libraryTool -Arguments @('census') -Timeout 600
    Check 'librarytool census reports the kinds a folder skips and stays quiet about all' `
        ($census.Code -eq 0 -and $census.Out -match 'ALL PASS \(0 failures\)') ('code=' + $census.Code)
    # Album cover lookup ships off, so the rule that decides what a lookup sends
    # and what it is allowed to download is worth more than a screenshot.  The
    # self-test injects its transport and its cache directory and never opens a
    # socket: what it proves is the catalogue ladder, the exact-name rule and the
    # URL filter, from a canned answer rather than from whatever a store carries
    # today.  No phase here sends provider traffic.
    $cover = Invoke-Proc -File $libraryTool -Arguments @('cover') -Timeout 600
    Check 'librarytool cover pins the album cover lookup from canned answers' `
        ($cover.Code -eq 0 -and $cover.Out -match 'ALL PASS \(0 failures\)') ('code=' + $cover.Code)
    # ZUUNED_PROXY is the one network knob this application has, and this tool is the
    # only place on Windows its parsing rules can be reached: no phase here sends
    # provider traffic, and the situation the knob exists for - a network that drops
    # some providers while its own Windows proxy setting is switched off - is a fact
    # about one machine that no check is allowed to depend on.  The self-test installs
    # a route and never sends one byte through it.
    $net = Invoke-Proc -File $libraryTool -Arguments @('net') -Timeout 300
    Check 'librarytool net pins the proxy route rules without sending traffic' `
        ($net.Code -eq 0 -and $net.Out -match 'ALL PASS \(0 failures\)') ('code=' + $net.Code)
    # The GUID moniker rule exists in two languages: src/library/MusicIdentity.h,
    # which librarytool paths reaches, and qml/MusicIdentity.js, the last mile every
    # album and artist pivot groups through.  Nothing in this gate executes QML, so it
    # cannot paint the tile and must not claim to; what it can prove is that the two
    # copies are still the same pattern, which is exactly how a second implementation
    # drifts away from the first.
    $cppText = [IO.File]::ReadAllText((Join-Path $root 'src\library\MusicIdentity.h'))
    $jsText = [IO.File]::ReadAllText((Join-Path $root 'qml\MusicIdentity.js'))
    $cppBlock = [regex]::Match($cppText, 'moniker\(([\s\S]*?)\);').Groups[1].Value
    $cppPattern = ''
    foreach ($piece in [regex]::Matches($cppBlock, '"(.*?)"')) { $cppPattern += $piece.Groups[1].Value }
    $jsPattern = [regex]::Match($jsText, 'guidMoniker = /(.*?)/').Groups[1].Value
    Check 'the QML mirror and the C++ predicate are one GUID moniker pattern' `
        ($cppPattern.Replace('\\', '\') -ceq $jsPattern) `
        ('cpp=' + $cppPattern + ' js=' + $jsPattern)
    Check 'the Artists pivot groups through the mirror, not the raw name field' `
        ([IO.File]::ReadAllText((Join-Path $root 'qml\MusicPage.qml')).Contains('MusicIdentity.artistName(')) `
        'qml/MusicPage.qml computeArtists'
    # Exactly one recursive directory walk is allowed in this source tree, and it is
    # the one librarytool walk holds against a real `mklink /J`.  A second hand-rolled
    # QDirIterator(..., Subdirectories) would not ask who a folder is, and QFileInfo
    # does not call a junction a link, so it would reopen the hole the identity guard
    # closed for the library and open it for the audiobook import.  No phase here can
    # drive that folder chooser, so the check pins the structure rather than pretending
    # to have walked a tree: no second loop anywhere in src, and spoken import calls
    # through the guarded function.
    $walkers = @(Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -File |
        Where-Object {
            ($_.Extension -eq '.cpp' -or $_.Extension -eq '.h') -and
            [IO.File]::ReadAllText($_.FullName).Contains('QDirIterator::Subdirectories')
        } | ForEach-Object { $_.FullName.Substring($root.Length + 1) })
    Check 'src contains no second recursive directory walk' `
        ($walkers.Count -eq 0) ('found=' + ($walkers -join ','))
    $spokenText = [IO.File]::ReadAllText(
        (Join-Path $root 'src\spoken\SpokenMediaService.cpp'))
    Check 'the audiobook folder import walks through LibraryScan::walkFiles' `
        ($spokenText.Contains('LibraryScan::walkFiles(')) `
        'src/spoken/SpokenMediaService.cpp previewBookFolder'
    # A tool that runs under its own organisation and application name cannot ask
    # QStandardPaths where Zuuned keeps its files, because Qt answers for the
    # process that asks.  tools/artwork-preview worked around that with a literal
    # "/Zuuned/Zuuned" on the generic location, which is the application's folder
    # on Linux and one or two levels away from it on Windows, where the generic
    # data location is C:\ProgramData and the generic cache location sits above
    # the cache folder holding art and artistart.  Nothing here builds or launches
    # that tool, so the check pins the rule instead of pretending to have read an
    # artwork cache: the profile helpers own the application's folders, and no
    # source file spells a profile path out of a string.
    $profileText = [IO.File]::ReadAllText((Join-Path $root 'src\ProfilePaths.h'))
    Check 'ProfilePaths names the application data and cache folders' `
        ($profileText.Contains('applicationDataLocation()') -and
         $profileText.Contains('applicationCacheLocation()')) `
        'src/ProfilePaths.h'
    $previewText = [IO.File]::ReadAllText(
        (Join-Path $root 'tools\artwork-preview\main.cpp'))
    Check 'the artwork preview lab resolves the profile through ProfilePaths' `
        ($previewText.Contains('ProfilePaths::applicationCacheLocation()') -and
         $previewText.Contains('ProfilePaths::applicationDataLocation()')) `
        'tools/artwork-preview/main.cpp defaults'
    $spellers = @(Get-ChildItem -LiteralPath (Join-Path $root 'src'),
            (Join-Path $root 'tools') -Recurse -File |
        Where-Object {
            ($_.Extension -eq '.cpp' -or $_.Extension -eq '.h') -and
            $_.Name -ne 'ProfilePaths.h' -and
            [IO.File]::ReadAllText($_.FullName).Contains('/Zuuned/Zuuned')
        } | ForEach-Object { $_.FullName.Substring($root.Length + 1) })
    Check 'no source file builds a profile folder out of a string' `
        ($spellers.Count -eq 0) ('found=' + ($spellers -join ','))
    # Two invariants that no launch can see, pinned where they are written.
    # Both online questions have to ship off or a library of files that already
    # carry their own art spends every launch asking strangers about covers it
    # does not need, and every artless album then reads "Needs a match" instead
    # of the truth.  A seeded case that writes its own ini would hide a flipped
    # default completely, which is exactly how this one stayed on.
    $lookupText = [IO.File]::ReadAllText((Join-Path $root 'src\LibraryService.cpp'))
    Check 'album cover lookup ships off, so asking a store stays the user call' `
        ($lookupText.Contains('QSettings().value(QStringLiteral("albumCoverLookup"), false)') -and
         -not $lookupText.Contains('QSettings().value(QStringLiteral("albumCoverLookup"), true)')) `
        'src/LibraryService.cpp albumCoverLookupEnabled'
    Check 'artist portrait and tag lookup ships off' `
        ($lookupText.Contains('QSettings().value(QStringLiteral("autoMetadataLookup"), false)')) `
        'src/LibraryService.cpp onlineMetadataLookupEnabled'
    # The search field and the filter it drives are two states, and a reset that
    # touches only the property leaves the box showing a word the model stopped
    # honouring - observed live, "Watercolour" in the field over a wall of every
    # album.  Every reset has to go through clearSearch(), so the only place the
    # property is assigned the empty string is that function.
    $musicText = [IO.File]::ReadAllText((Join-Path $root 'qml\MusicPage.qml'))
    $blanks = @([regex]::Matches($musicText, 'searchText\s*=\s*""')).Count
    Check 'every music search reset clears the field and the filter together' `
        ($blanks -eq 1 -and $musicText.Contains('function clearSearch()')) `
        ('searchText = "" appears ' + $blanks + ' time(s) in qml/MusicPage.qml')
    # Three self-contained C gates that no GUI launch reaches on purpose.  The tag
    # writer only runs on a file the user edits or a FLAC about to become an mp3;
    # the copy policy only runs on a sync target; the framing gate only runs on a
    # SendObject.  None of them can be driven from a page without writing to a
    # real library or a real player, and the framing gate opens no device handle,
    # so it answers with no Zune attached.
    if (Test-Path -LiteralPath $transcodeTool) {
        $retag = Invoke-Proc -File $transcodeTool -Arguments @('retagcheck') -Timeout 600
        Check 'transcodetool retagcheck rewrites every text frame as legal ID3v2.3 and leaves the audio byte-exact' `
            (($retag.Code -eq 0) -and ($retag.Out -match 'retagcheck \d+ checks 0 failures')) `
            ('code=' + $retag.Code + ' out=' + $retag.Out.Trim().Replace([Environment]::NewLine, ' '))
        $copy = Invoke-Proc -File $transcodeTool -Arguments @('copycheck') -Timeout 600
        Check 'transcodetool copycheck refuses a native copy the probe cannot prove' `
            (($copy.Code -eq 0) -and ($copy.Out -match 'copycheck \d+ checks 0 failures')) `
            ('code=' + $copy.Code + ' out=' + $copy.Out.Trim().Replace([Environment]::NewLine, ' '))
    }
    else {
        Skip 'transcodetool retagcheck and copycheck' 'transcodetool.exe is missing'
    }
    # Streaming MTP framing is the send path after the whole-file malloc() went
    # away.  A chunk that drifts by one byte, or a zero-length packet that moves,
    # is a player that accepts the transfer and shows a truncated song, so the
    # gate holds the streaming path against the buffered one write for write.
    if (Test-Path -LiteralPath $mtpFraming) {
        $framing = Invoke-Proc -File $mtpFraming -Timeout 600
        Check 'mtpframingtool streams SendObject byte-for-byte like the buffered path' `
            (($framing.Code -eq 0) -and ($framing.Out -match 'framing \d+ checks 0 failures')) `
            ('code=' + $framing.Code + ' out=' + $framing.Out.Trim().Replace([Environment]::NewLine, ' '))
    }
    else {
        Skip 'mtpframingtool' 'mtpframingtool.exe is missing'
    }
}

Write-Host ''
Write-Host '== encode real media'
if (Select-Phase 'encode') {
    # Until now the Windows line had only ever asked the transcode layer to read: the
    # gate probed fixtures with it and never once made it produce a byte, while video
    # sync and FLAC sync are nothing but production.  Encoding walks the encoder
    # table, the temporary-path helper, LAME superframes and the ASF muxer, none of
    # which a probe touches, so it gets its own phase.  Containers are judged by
    # their magic numbers because a file name proves nothing - this repository
    # already learned that real libraries hold WMA streams inside .mp3 names.
    # transcodetool writes through make_temp_path into the temporary directory, which
    # is exactly where the application writes, so this is the real destination and
    # this phase cleans up after itself there.
    if (-not (Test-Path -LiteralPath $ffmpeg) -or -not (Test-Path -LiteralPath $transcodeTool)) {
        Skip 'the encode phase' 'ffmpeg.exe or transcodetool.exe is missing'
    }
    else {
        $clip = Join-Path $out 'encode-src.mp4'
        $made = Invoke-Proc -File $ffmpeg -WorkDir $out -Timeout 300 -Arguments @(
            '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=15:duration=3',
            '-f', 'lavfi', '-i', 'sine=frequency=440:duration=3',
            '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
            '-c:a', 'aac', '-shortest', $clip)
        Check 'a three-second H.264 source clip exists to encode from' `
            (($made.Code -eq 0) -and (Test-Path -LiteralPath $clip)) ('code=' + $made.Code)

        function Get-LeadBytes {
            param([string]$Path, [int]$Count)
            if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return '' }
            $bytes = [System.IO.File]::ReadAllBytes($Path)
            $take = [Math]::Min($Count, $bytes.Length)
            return (($bytes[0..($take - 1)] | ForEach-Object { $_.ToString('x2') }) -join ' ')
        }
        function Get-EncodeSeconds {
            param([string]$ProbeOut)
            if ($ProbeOut -match 'duration=(\d+\.\d+)') { return [double]$Matches[1] }
            return -1.0
        }

        # Profile 0 is wmv2 + wmav2 in an ASF container: what a Zune 30 plays, and the
        # profile the Classic video season was synced with.
        $enc0 = Invoke-Proc -File $transcodeTool -Timeout 900 -Arguments @('video', $clip, '0')
        $path0 = $enc0.Out.Trim()
        Check 'profile 0 encoded the clip and named the file it made' `
            (($enc0.Code -eq 0) -and $path0 -and (Test-Path -LiteralPath $path0)) `
            ('code=' + $enc0.Code + ' out=' + $path0)
        Check 'profile 0 wrote an ASF container' `
            ((Get-LeadBytes -Path $path0 -Count 4) -eq '30 26 b2 75') `
            ('magic=' + (Get-LeadBytes -Path $path0 -Count 4))
        $probe0 = Invoke-Proc -File $transcodeTool -Timeout 120 -Arguments @('probe', $path0)
        $sec0 = Get-EncodeSeconds -ProbeOut $probe0.Out
        Check 'profile 0 kept the three seconds and the source frame size' `
            (($sec0 -gt 2.5) -and ($sec0 -lt 3.5) -and ($probe0.Out -match 'video=320x240')) `
            ('seconds=' + $sec0 + ' out=' + $probe0.Out.Trim().Replace([Environment]::NewLine, ' '))

        # Profile 2 is the H.264 path, which on this platform means libx264 out of the
        # FFmpeg build the application links (src/transcode/libav_transcode.c picks it
        # for anything that is not Apple) at the 480x272 the HD screen has.
        $enc2 = Invoke-Proc -File $transcodeTool -Timeout 900 -Arguments @('video', $clip, '2')
        $path2 = $enc2.Out.Trim()
        Check 'profile 2 encoded the clip and named the file it made' `
            (($enc2.Code -eq 0) -and $path2 -and (Test-Path -LiteralPath $path2)) `
            ('code=' + $enc2.Code + ' out=' + $path2)
        $lead2 = Get-LeadBytes -Path $path2 -Count 12
        Check 'profile 2 wrote an ISO base-media container' `
            ($lead2 -match '66 74 79 70') ('magic=' + $lead2)
        $probe2 = Invoke-Proc -File $transcodeTool -Timeout 120 -Arguments @('probe', $path2)
        $sec2 = Get-EncodeSeconds -ProbeOut $probe2.Out
        Check 'profile 2 reached the 480x272 target with the same running time' `
            (($sec2 -gt 2.5) -and ($sec2 -lt 3.5) -and ($probe2.Out -match 'video=480x272')) `
            ('seconds=' + $sec2 + ' out=' + $probe2.Out.Trim().Replace([Environment]::NewLine, ' '))

        # The audio leg is what FLAC sync needs: decode, resample, LAME.  A truncated
        # superframe used to make LAME produce unusable files, and that is a Windows
        # and Linux bug at once, so the product is probed for its running time rather
        # than trusted for its extension.
        $audioSec = -1.0
        if (Test-Path -LiteralPath $tone) {
            $encA = Invoke-Proc -File $transcodeTool -Timeout 900 -Arguments @('audio', $tone)
            $pathA = $encA.Out.Trim()
            Check 'the audio encoder produced an mp3 and named it' `
                (($encA.Code -eq 0) -and $pathA -and (Test-Path -LiteralPath $pathA)) `
                ('code=' + $encA.Code + ' out=' + $pathA)
            $leadA = Get-LeadBytes -Path $pathA -Count 3
            Check 'the audio encoder wrote an ID3 tag or a frame sync header' `
                (($leadA -eq '49 44 33') -or ($leadA -match '^ff')) ('magic=' + $leadA)
            $probeA = Invoke-Proc -File $transcodeTool -Timeout 120 -Arguments @('probe', $pathA)
            $audioSec = Get-EncodeSeconds -ProbeOut $probeA.Out
            Check 'the encoded audio is as long as the tone it came from' `
                (($audioSec -gt 5.0) -and ($audioSec -lt 7.0)) ('seconds=' + $audioSec)
        }
        else {
            Skip 'the audio encode checks' 'no fixture tone on disk'
        }

        # The encode runs leave their products in the temporary directory.  Remove
        # them rather than teach the next run to tolerate strangers in %TEMP%.
        $leftovers = @($path0, $path2)
        if ($audioSec -gt 0) { $leftovers += $pathA }
        $kept = @()
        foreach ($tmp in $leftovers) {
            if (-not $tmp -or -not (Test-Path -LiteralPath $tmp)) { continue }
            if (-not $pythonExe) { $kept += $tmp; continue }
            Invoke-Proc -File $pythonExe -Arguments @(
                '-c', 'import os,sys;os.remove(sys.argv[1])', $tmp) -Timeout 60 | Out-Null
        }
        if ($kept.Count) {
            Write-Host ('  note encode products left in the temporary directory: ' +
                ($kept -join ' '))
        }
    }
}

Write-Host ''
Write-Host ''
Write-Host '== hardware decode'
if (Select-Phase 'decode') {
    # Settings offers a decode accelerator and SyncEngine passes it straight down, so
    # the Windows line ships a code path - CUDA device, hardware frame download - that
    # no earlier phase ever ran: encode used whatever the library default picked and
    # reported nothing about it.  A device context is not proof either, because
    # get_format answers with a software format whenever the decoder offers no hardware
    # one, so this reads back the pixel format of the first frame the decoder returned.
    if (-not (Test-Path -LiteralPath $ffmpeg) -or -not (Test-Path -LiteralPath $transcodeTool)) {
        Skip 'the decode phase' 'ffmpeg.exe or transcodetool.exe is missing'
    }
    else {
        $dclip = Join-Path $out 'decode-src.mp4'
        $dmade = Invoke-Proc -File $ffmpeg -WorkDir $out -Timeout 300 -Arguments @(
            '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', 'testsrc2=size=640x480:rate=15:duration=3',
            '-f', 'lavfi', '-i', 'sine=frequency=440:duration=3',
            '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p',
            '-c:a', 'aac', '-shortest', $dclip)
        Check 'a 640x480 H.264 clip exists to decode from' `
            (($dmade.Code -eq 0) -and (Test-Path -LiteralPath $dclip)) ('code=' + $dmade.Code)

        # Exactly one line, or the assertion below is reading a leftover from a
        # retry: the decoder opens twice when a hardware open fails.
        function Get-DecodeLine {
            param([string]$Text, [string]$Needle)
            $lines = @($Text -split "`n" | Where-Object { $_ -match $Needle })
            if ($lines.Count -ne 1) { return '' }
            return $lines[0].Trim()
        }

        $dtemps = @()

        # Mode 0 is the forced software path.  Every machine has it, so it is the only
        # path the gate may insist on: nothing requested, first frame plain yuv420p.
        $sw = Invoke-Proc -File $transcodeTool -Timeout 900 `
            -Arguments @('video', $dclip, '2') -Env @{ ZUUNED_HW_DECODE = '0' }
        $swOpen = Get-DecodeLine -Text $sw.Err -Needle 'video: decoder OK'
        $swPath = Get-DecodeLine -Text $sw.Err -Needle 'video: decode path'
        Check 'the forced software run opens one decoder and asks for no accelerator' `
            (($swOpen -like '*video: decoder OK (640x480)') -and
             ($sw.Err -notmatch 'unavailable')) ('line=' + $swOpen + ' code=' + $sw.Code)
        Check 'the forced software run decodes its first frame in software' `
            (($swPath -like '*decode path: software*fmt=yuv420p*') -and
             ($sw.Err -notmatch 'decode path: HARDWARE')) ('line=' + $swPath)
        $swOut = $sw.Out.Trim()
        if ($swOut -and (Test-Path -LiteralPath $swOut)) { $dtemps += $swOut }
        $swProbe = Invoke-Proc -File $transcodeTool -Timeout 120 -Arguments @('probe', $swOut)
        Check 'the software-decoded clip still reaches the 480x272 target intact' `
            (($sw.Code -eq 0) -and ($swProbe.Out -match 'video=480x272') -and
             ($swProbe.Out -match 'duration=(2\.[5-9]|3\.[0-4])')) `
            ('out=' + $swOut + ' probe=' + $swProbe.Out.Trim().Replace([Environment]::NewLine, ' '))

        # Mode 1 is what Settings leaves on for a user, and its answer belongs to the
        # machine: a box with no NVIDIA decoder falls back and is right to.  The gate
        # only insists that the run says once which path it took, and that a hardware
        # answer really produced a file through the download path.
        $auto = Invoke-Proc -File $transcodeTool -Timeout 900 `
            -Arguments @('video', $dclip, '2') -Env @{ ZUUNED_HW_DECODE = '1' }
        $autoPath = Get-DecodeLine -Text $auto.Err -Needle 'video: decode path'
        $autoOut = $auto.Out.Trim()
        if ($autoOut -and (Test-Path -LiteralPath $autoOut)) { $dtemps += $autoOut }
        Check 'the default run names the decode path it used exactly once' `
            (($auto.Code -eq 0) -and ($autoPath -like '*decode path: *')) `
            ('line=' + $autoPath + ' code=' + $auto.Code)
        if ($autoPath -like '*HARDWARE*') {
            $autoProbe = Invoke-Proc -File $transcodeTool -Timeout 120 `
                -Arguments @('probe', $autoOut)
            Check 'hardware-decoded frames download to a product the scaler and muxer accept' `
                (($auto.Err -match 'scaler OK') -and ($autoProbe.Out -match 'video=480x272') -and
                 ($autoProbe.Out -match 'duration=(2\.[5-9]|3\.[0-4])')) `
                ('line=' + $autoPath + ' probe=' + $autoProbe.Out.Trim().Replace([Environment]::NewLine, ' '))
        }
        else {
            Skip 'the hardware decode product check' 'this machine decoded the clip in software'
        }
        $autoDesc = 'none reported'
        if ($autoPath) { $autoDesc = $autoPath }
        Write-Host ('  note decode path on this machine: ' + $autoDesc)

        foreach ($tmp in $dtemps) {
            if (-not $tmp -or -not (Test-Path -LiteralPath $tmp)) { continue }
            if (-not $pythonExe) { continue }
            Invoke-Proc -File $pythonExe -Arguments @(
                '-c', 'import os,sys;os.remove(sys.argv[1])', $tmp) -Timeout 60 | Out-Null
        }
    }
}

Write-Host '== what is plugged in'
if (Select-Phase 'device') {
    # Whether the app is telling the truth about the device is the one question a
    # device-less gate usually waves away, and on Windows it is the question that
    # bites first: a Zune with no driver bound is invisible to libusb while Windows
    # still holds it on the bus.  devicetool asks libusb and the PnP manager, and
    # Win32_PnPEntity is a third, unrelated API to hold them against each other.
    # Nothing here asserts that a Zune IS attached, so the phase says the same true
    # thing on a machine with nothing plugged in, which is what makes it portable.
    if (-not (Test-Path -LiteralPath $deviceTool)) {
        Skip 'devicetool' 'devicetool.exe is not in the build directory'
    }
    else {
        $seen = Invoke-Proc -File $deviceTool -Timeout 120
        $dev = $seen.Out
        foreach ($line in ($dev -split '\r?\n')) {
            if ($line.Trim()) { Write-Host ('  note ' + $line.Trim()) }
        }
        Check 'devicetool reads the bus and reaches a verdict' `
            ($seen.Code -eq 0 -and $dev -match 'VERDICT=(agree|blocked|absent)') `
            ('code=' + $seen.Code + ' ' + $dev.Trim())
        Check 'libusb enumerated the bus' ($dev -match 'libusb_enumerated=1 libusb_devices=[1-9]') `
            $seen.Err.Trim()

        $osSees = [bool]($dev -match 'os_present=1')
        $usbSees = [bool]($dev -match 'libusb_sighted=1')
        $cim = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.DeviceID -match 'VID_045E&PID_(0710|063E|0711|0712|0713|0714)\\' -and
                           $_.Present })
        Check 'the devnode scan and Windows agree about whether a Zune is attached' `
            ($osSees -eq ($cim.Count -gt 0)) `
            ('scan_present=' + $osSees + ' wmi_present=' + ($cim.Count -gt 0))
        Check 'a Zune libusb can open is one the platform sees too' `
            (-not $usbSees -or $osSees) ('libusb_sighted=' + $usbSees + ' scan_present=' + $osSees)

        if ($osSees -and $cim.Count -gt 0) {
            $scanId = ''
            if ($dev -match 'os_instance=(.+)') { $scanId = $Matches[1].Trim() }
            Check 'the devnode scan names the device Windows names' `
                ($scanId -eq [string]$cim[0].DeviceID) `
                ('scan=' + $scanId + ' wmi=' + $cim[0].DeviceID)
            $scanProblem = -1
            if ($dev -match 'os_problem=(\d+)') { $scanProblem = [int]$Matches[1] }
            Check 'the devnode scan reports the problem code Windows reports' `
                ($scanProblem -eq [int]$cim[0].ConfigManagerErrorCode) `
                ('scan=' + $scanProblem + ' wmi=' + $cim[0].ConfigManagerErrorCode)
        }
        else {
            Skip 'the two checks that name a specific device' 'no Zune attached to compare'
        }
        # Records for hardware that left weeks ago are the trap here: a scan that
        # counted them would report a Zune that nobody can touch.  Win32_PnPEntity
        # hides precisely those records, so this count comes from Get-PnpDevice,
        # the one API here that still sees a player unplugged weeks ago.  Reported
        # rather than asserted, because a clean machine legitimately has none.
        $records = @(Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_045E&PID_(0710|063E|0711|0712|0713|0714)\\' })
        Write-Host ('  note devnode records Windows keeps: ' +
            $records.Count + ', the scan counted as attached: ' + [int]$osSees)
    }
}

Write-Host ''
Write-Host '== player core'
if (Select-Phase 'seek') {
    if (-not (Test-Path -LiteralPath $tone)) {
        Skip 'the silent playertool checks' 'no fixture tone on disk'
    }
    else {
        $mpvEnv = @{ ZUUNED_MPV_AO = 'null' }
        $seeked = Invoke-Proc -File $playerTool -Arguments @('seek', $tone, '4') -Env $mpvEnv -Timeout 180
        Check 'playertool lands a seek made while stopped' `
            ($seeked.Code -eq 0 -and $seeked.Out -match 'PASS' -and $seeked.Out -notmatch 'FAIL') `
            $seeked.Out.Trim()
        $played = Invoke-Proc -File $playerTool -Arguments @('play', $tone, '2') -Env $mpvEnv -Timeout 180
        Check 'playertool position advances on the null audio driver' `
            ($played.Out -match 'monotonic=yes') $played.Out.Trim()
    }
}

Write-Host ''
Write-Host '== audio backend'
if (Select-Phase 'audio') {
    # The null driver proves the player clocks playback; it proves nothing about
    # the platform backend.  mpv reports the output it really bound through the
    # "current-ao" property - there is no "audio-out" property - so an audible
    # run prints the driver it opened.  --mute keeps the samples silent, which is
    # what lets this run on the user's own desktop while WASAPI still opens.
    if (-not (Test-Path -LiteralPath $tone)) {
        Skip 'the audible playertool checks' 'no fixture tone on disk'
    }
    else {
        $audible = Invoke-Proc -File $playerTool -Arguments @('play', $tone, '3', '--audible', '--mute') `
            -Env @{ ZUUNED_MPV_AO = 'wasapi' } -Timeout 180
        Check 'playertool opens the Windows WASAPI output when asked' `
            (($audible.Code -eq 0) -and ($audible.Out -match 'audio_out=wasapi') -and
             ($audible.Out -match 'monotonic=yes')) $audible.Out.Trim()
        # Every other phase relies on the default being the null driver: a gate
        # that made sound on the user's desktop would be a worse gate.
        $defaulted = Invoke-Proc -File $playerTool -Arguments @('play', $tone, '2') `
            -Env @{ ZUUNED_MPV_AO = '' } -Timeout 180
        Check 'playertool still defaults to the silent null driver' `
            ($defaulted.Out -match 'audio_out=null') $defaulted.Out.Trim()
    }
}

Write-Host ''
Write-Host '== single instance'
if (Select-Phase 'single') {
    # The lock lives inside the profile, at <profile>/run/zuuned<SUFFIX>.lock, so the
    # only rival the app can refuse is one that names the same profile and the same
    # suffix.  Two profiles of the same build hold two locks and both are right to
    # run, which is what an earlier version of this phase asked for; it then waited
    # 120 s for a rival that was behaving correctly and called the app broken.
    $sharedDir = Join-Path $profiles 'single-instance'
    New-Item -ItemType Directory -Force -Path $sharedDir | Out-Null
    $holder = Start-Zuuned -Profile $sharedDir -Suffix 'wussingle' -Arguments @('--page', 'music')
    $script:pids.Add($holder.Proc.Id)
    $holderWindow = Wait-ZuunedWindow -Handle $holder
    Start-Sleep -Seconds 3
    $rival = Invoke-Proc -File $exe -WorkDir $build -Timeout 90 -Arguments @('--page', 'music') -Env @{
        ZUUNED_PROFILE_DIR = $sharedDir
        ZUUNED_LOCK_SUFFIX = 'wussingle'
        ZUUNED_MPV_AO = 'null'
        PATH = (Join-Path $QtDir 'bin') + ';' + $FfmpegDir + ';' + $env:PATH
    }
    Check 'a second instance on the same profile bows out in its own words' `
        ((($rival.Out + $rival.Err) -match 'another instance is already running') -and (-not $rival.TimedOut)) `
        ('code=' + $rival.Code + ' out=' + $rival.Out.Trim() + ' err=' + $rival.Err.Trim())
    # Bowing out happens before the QML engine exists, so a rival that reports what
    # page it is on has not bowed out at all - it has started a second app on a
    # library the holder is already using, which is the hazard C2 exists to stop.
    Check 'the refused instance never built a scene to show' `
        (($rival.Code -eq 0) -and (($rival.Out + $rival.Err) -notmatch '\[qa\] ')) `
        ('code=' + $rival.Code)
    Check 'the rival refused without touching the holder window' `
        ($holderWindow.Handle -ne [IntPtr]::Zero) ('source=' + $holderWindow.Source)
    $closed = Stop-Zuuned -Handle $holder
    if ($closed.Killed) { $script:killed++ }
    Check 'the instance that holds the lock still closes politely' `
        (-not $closed.Killed) ('code=' + $closed.Code)
    # The refusal comes after the session log opens, so the shared profile keeps a
    # second log file and the rival prints that it could not capture the first one.
    # Both are the app's existing order of operations: diagnostics before locks.
    $logs = @(Get-ChildItem -LiteralPath (Join-Path $sharedDir 'State\logs') -File -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'zuuned-*.log' })
    Write-Host ('  note the shared profile holds ' + $logs.Count + ' session log(s);' +
        ' the lock, not the exit code (' + $rival.Code + '), is what the check reads')
}

Write-Host ''
Write-Host '== profile isolation'
if (Select-Phase 'profile') {
    $fileCase = Invoke-Case -Name 'profile-files' -Arguments @('--page', 'music')
    Assert-Case -Case $fileCase -Label 'a fresh profile launch is healthy and shows onboarding' `
        -ExpectPage 'music' -ExpectOnboarding 'true'
    $logs = @(Get-ChildItem -LiteralPath (Join-Path $fileCase.Profile 'State\logs') -File -Force `
        -ErrorAction SilentlyContinue)
    $written = @($logs | Where-Object { $_.Name -like 'zuuned-*.log' -and $_.Length -gt 0 })
    Check 'the isolated profile owns a non-empty session log' `
        ($written.Count -gt 0) ('logs=' + $logs.Count)
    Check 'the isolated profile owns the settings ini' `
        (Test-Path -LiteralPath (Join-Path $fileCase.Profile 'Zuuned\Zuuned.ini')) $fileCase.Profile
    Check 'the isolated profile owns both databases' `
        ((Test-Path -LiteralPath (Join-Path $fileCase.Profile 'library.db')) -and
            (Test-Path -LiteralPath (Join-Path $fileCase.Profile 'spoken\spoken.db'))) $fileCase.Profile
    Check 'the isolated profile owns the single-instance lock' `
        (Test-Path -LiteralPath (Join-Path $fileCase.Profile 'State\logs\zuuned.lock')) $fileCase.Profile

    # The point of the whole run.  The user's own running Zuuned checkpoints its own
    # library.db-wal throughout, so a byte-for-byte compare would accuse this gate of
    # damage it did not do; instead nothing this run started may appear in the real
    # profile, under any of the process ids it launched.
    $byPid = @($script:pids | ForEach-Object { [regex]::Escape('-' + $_ + '-') })
    $mine = @()
    if ($byPid.Count) {
        $pattern = ($byPid -join '|')
        $mine = @(Get-RealProfileFiles | Where-Object { $_.Name -match $pattern })
    }
    Check 'no process this gate started wrote into the real profile' `
        ($mine.Count -eq 0) (($mine | ForEach-Object { $_.FullName }) -join ' ')
    $born = @(Get-RealProfileFiles -Since $script:runStart)
    Check 'the real profile gained no file while the gate ran' `
        ($born.Count -eq 0) (($born | ForEach-Object { $_.Name }) -join ' ')
    # Qt's automatic shader/pipeline cache is the one profile file this app does not
    # open itself - Qt6Quick asks QStandardPaths::CacheLocation directly - and a file
    # that already exists only changes mtime, so the creation-time check above cannot
    # see it.  Read the timestamp instead.  The app now names the cache path for Qt
    # inside the profile (src/main.cpp), so a gate launch must leave the real one
    # completely alone.
    $realCacheRoot = Join-Path $env:LOCALAPPDATA 'Zuuned\Zuuned\cache'
    $touchedRealCache = @(Get-ChildItem -LiteralPath $realCacheRoot -Recurse -File -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTimeUtc -ge $script:runStart })
    Check 'Qt pipeline cache stayed out of the real profile' `
        ($touchedRealCache.Count -eq 0) (($touchedRealCache | ForEach-Object { $_.Name }) -join ' ')
    $ownCache = @(Get-ChildItem -LiteralPath (Join-Path $fileCase.Profile 'cache') -Recurse -File -Force `
        -ErrorAction SilentlyContinue)
    Check 'the isolated profile owns the Qt pipeline cache' `
        ($ownCache.Count -gt 0) (($ownCache | ForEach-Object { $_.Name + '=' + $_.Length }) -join ' ')
    Write-Host ('  note real profile before the run: ' + $beforeFp)
    Write-Host ('  note real profile after the run:  ' + (Get-RealProfileFingerprint))
}

Write-Host ''
Write-Host '== profile folder the ANSI code page cannot spell'
if (Select-Phase 'cjkprofile') {
    # Every path the application builds hangs off the profile folder, and on a Chinese
    # Windows that folder usually sits under a Chinese user name, so a profile whose
    # name cannot be spelled in the ANSI code page is the ordinary case for a large
    # part of the people this port is for rather than an exotic one.
    #
    # SQLite is handed UTF-8 path bytes and decodes them itself on Windows.  The
    # spoken-media store used to hand it QFile::encodeName() instead, which is the
    # local code page, and a two-byte GBK sequence for a common Han character is very
    # often valid UTF-8 as well (U+4E00 encodes as D2 BB, which reads back as U+04BB),
    # so the store asked for a different file than the folder Qt had just made.
    # Measured here before the fix: spoken\ existed, spoken\spoken.db did not, and
    # every podcast and audiobook operation answered "Cannot open spoken-media
    # database", while library.db in the same folder opened fine because LibraryDb
    # passes toUtf8().
    #
    # The folder name is assembled from code points so that this script stays ASCII:
    # Windows PowerShell 5.1 reads a BOM-less script as ANSI, which is the very code
    # page under test.
    $cjk = [string]::Join('', @([char]0x4E2D, [char]0x6587, [char]0x97F3, [char]0x4E50))
    $cjkDir = Join-Path $out ($cjk + '-profile')
    New-Item -ItemType Directory -Force -Path $cjkDir | Out-Null
    $spokenDb = Join-Path $cjkDir 'spoken\spoken.db'

    $hanFirst = Invoke-Case -Name 'cjk-spoken-first-launch' -ProfileDir $cjkDir `
        -Arguments @('--page', 'music')
    Assert-Case -Case $hanFirst -Label 'a first launch into a Han profile folder shows a painted window'
    Check 'a Han profile folder gets a library database' `
        (Test-Path -LiteralPath (Join-Path $cjkDir 'library.db')) ('profile=' + $cjkDir)
    Check 'a Han profile folder gets the spoken-media database' `
        (Test-Path -LiteralPath $spokenDb) ('db=' + $spokenDb)

    # Re-opening is the half of the bug a single fresh profile cannot see: an importer,
    # a podcast download or an audiobook added later only ever meets the next launch,
    # and a store that cannot be found again is as dead as one that was never made.
    $hanSecond = Invoke-Case -Name 'cjk-spoken-second-launch' -ProfileDir $cjkDir `
        -Arguments @('--page', 'music')
    Assert-Case -Case $hanSecond -Label 'a second launch re-opens the same Han profile folder'
    $spokenBytes = 0
    if (Test-Path -LiteralPath $spokenDb) { $spokenBytes = (Get-Item -LiteralPath $spokenDb).Length }
    Check 'the second launch left a readable spoken-media database behind' `
        ($spokenBytes -gt 0) ('bytes=' + $spokenBytes)
    if (-not $pythonExe) {
        Skip 'the schema of the database under the Han folder' 'no python on this machine'
    }
    elseif (Test-Path -LiteralPath $spokenDb) {
        $insp = Invoke-Proc -File $pythonExe -Timeout 120 -Arguments @(
            (Join-Path $PSScriptRoot 'inspect_spoken_db.py'), $spokenDb)
        Check 'the database under the Han folder is a usable spoken-media store' `
            (($insp.Code -eq 0) -and ($insp.Out -match 'OVERALL=ok')) `
            ('code=' + $insp.Code + ' ' + $insp.Out.Trim().Replace([Environment]::NewLine, ' '))
    }
    else {
        Check 'the database under the Han folder is a usable spoken-media store' $false `
            'no database to inspect'
    }

    # Recorded, not asserted.  Qt writes its own shader/pipeline cache through a byte
    # path, so under a folder the local code page cannot spell the directory exists,
    # the open still fails, and Windows answers "the system cannot find the path
    # specified".  All sixteen ASCII profiles of the same build in the last full run
    # wrote their own qqpc_opengl, from 116,820 bytes upwards.  What a Han profile
    # folder costs is the warm shader cache on every start, never a byte of data.
    $cacheFile = @(Get-ChildItem -LiteralPath (Join-Path $cjkDir 'cache') -File -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq 'qqpc_opengl' })
    Write-Host ('  note the Qt shader cache wrote ' + $cacheFile.Count + ' file(s) under the' +
        ' Han folder, where an ASCII profile writes one: Qt opens it through a byte path,' +
        ' so a folder name the ANSI code page cannot spell costs the cache, not the data')
}

Write-Host ''
Write-Host '== spoken worker across a close'
if (Select-Phase 'spokenclose') {
    # Podcasts and audiobooks live on a worker thread of their own, and closing the
    # window takes that thread with it.  Qt makes destroying a running QThread fatal,
    # so the service has to wait the worker out - and a wait with no deadline is how a
    # close turns into a window that never goes away.  It now asks the last resume
    # point to land, gives it two seconds, and drains the thread inside a budget.
    # What this phase can honestly measure about that path is below; what it cannot
    # measure is written down in docs\WINDOWS_PORT.md rather than asserted here.
    if (-not $seeded) {
        Skip 'a spoken page opened and then closed' 'no seed library'
    }
    else {
        $spokenDir = Join-Path $out 'spoken-close-profile'
        New-Item -ItemType Directory -Force -Path $spokenDir | Out-Null
        $spokenDb = Join-Path $spokenDir 'spoken\spoken.db'
        $spokenCase = Invoke-Case -Name 'spoken-close' -ProfileDir $spokenDir `
            -Arguments @('--page', 'podcasts') -Seed
        Assert-Case -Case $spokenCase `
            -Label 'the podcasts page opens on the spoken worker and closes by itself' `
            -ExpectPage 'podcasts' -ExpectOnboarding 'false'
        $forced = @($spokenCase.Lines | Where-Object { $_ -match '\[spoken\] worker would not stop' })
        Check 'the spoken worker came down inside its own close budget' ($forced.Count -eq 0) `
            ($forced | Select-Object -First 1)
        $abandoned = @($spokenCase.Lines | Where-Object { $_ -match '\[spoken\] last position' })
        Check 'the close did not have to abandon a resume point' ($abandoned.Count -eq 0) `
            ($abandoned | Select-Object -First 1)
        # Recorded rather than asserted: how long a close takes is the worker's choice
        # as much as the application's, and the drain budget only ever pays out when a
        # job is genuinely still inside a fetch, a download or a probe.
        $drained = @($spokenCase.Lines | Where-Object { $_ -match 'still busy at shutdown' })
        $labels = @($drained | ForEach-Object {
            if ($_ -match '\[(\w+)\] worker still busy') { $matches[1] }
        } | Sort-Object -Unique)
        $who = 'none'
        if ($labels.Count) { $who = (@($labels | ForEach-Object { '[' + $_ + ']' }) -join ' ') }
        Write-Host ('  note this close took ' + $spokenCase.CloseSeconds +
            's from the window close to the exit code, carrying ' + $drained.Count +
            ' drain line(s) on stderr from ' + $who + ' - not every worker is spoken')
        if (-not $pythonExe) {
            Skip 'the spoken store the close left behind' 'no python on this machine'
        }
        elseif (Test-Path -LiteralPath $spokenDb) {
            $spokenInsp = Invoke-Proc -File $pythonExe -Timeout 120 -Arguments @(
                (Join-Path $PSScriptRoot 'inspect_spoken_db.py'), $spokenDb)
            Check 'the spoken store the close left behind is still a usable store' `
                (($spokenInsp.Code -eq 0) -and ($spokenInsp.Out -match 'OVERALL=ok')) `
                ('code=' + $spokenInsp.Code + ' ' + $spokenInsp.Out.Trim().Replace([Environment]::NewLine, ' '))
        }
        else {
            Check 'the spoken store the close left behind is still a usable store' $false `
                'no database to inspect'
        }
    }
}

Write-Host ''
Write-Host '== summary'
if ($script:killed -eq 0) {
    Check 'every launch this gate made closed on its own' $true
}
else {
    Check 'every launch this gate made closed on its own' $false ('killed=' + $script:killed)
}
Write-Host ('launches=' + $script:caseSeq + ' checks=' + $script:checks +
    ' failures=' + $script:failures + ' skips=' + $script:skips +
    ' killed=' + $script:killed + ' pid-scan-fallbacks=' + $script:pidScans)
if ($script:noise.Count) {
    Write-Host ('recorded diagnostics (' + $script:noise.Count + '), not asserted:')
    foreach ($n in @($script:noise | Select-Object -First 25)) { Write-Host ('  note ' + $n) }
}
if ($script:failures) {
    foreach ($f in $script:failed) { Write-Host ('  FAILED ' + $f) }
    Write-Host 'OVERALL=fail'
    exit 1
}
# Truthful scope: the audio phase does open the real Windows output, but
# only muted, so nothing in this gate is certified by anyone listening to it.
Write-Host ('scope: ' + $script:caseSeq + ' instrumented launches, no device, one real audio output opened muted and never heard')
Write-Host 'SCOPE: no device handle opened here; the Windows wire evidence is the hand test in docs\WINDOWS_PORT.md'
Write-Host 'OVERALL=ok'
exit 0
