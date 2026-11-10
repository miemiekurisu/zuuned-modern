# Builds and runs the Windows POSIX shim gate for compat/win/.
#
#   pwsh -NoProfile -File tests/win-compat/run.ps1
#
# Needs the MSVC x64 developer environment; it is loaded from msvc-env.ps1 when
# cl is not already on PATH.  No device, no Qt, no network.  The probe is run
# under a wall-clock guard: a shim bug that blocks must fail the gate with the
# last test marker visible rather than hang the caller.
param(
    [string]$Deps = 'D:\dev\zuuned-deps',
    [string]$Pcre2 = 'D:\dev\zuuned-deps\pcre2\include',
    [int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $root 'build\win-compat'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    . (Join-Path $Deps 'msvc-env.ps1')
}

Push-Location $out
$code = -1
try {
    # user32.lib belongs on this line for the same reason it sits in the
    # zuuned_wincompat interface in CMakeLists.txt: compat/win/wincompat.c owns
    # MessageBoxW() now, and this gate does not link that library at all - it
    # compiles the same source beside its own probe, which makes it a consumer of
    # the shim that CMake cannot see.  Without it the link dies as
    # "unresolved external symbol __imp_MessageBoxW" before one check runs.
    & cl /nologo /W3 /Od /Zi /MDd /FIposix_shim.h /I"$root\compat\win" /I"$Pcre2" "$root\compat\win\wincompat.c" "$PSScriptRoot\wincompat_probe.c" user32.lib /Fewincompat_probe.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = Join-Path $out 'wincompat_probe.exe'
    $psi.WorkingDirectory = $out
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill($true) } catch { $proc.Kill() }
        Write-Host "probe timed out after $TimeoutSeconds seconds"
        Write-Host $outTask.Result
        Write-Host $errTask.Result
        exit 124
    }
    $proc.WaitForExit()
    Write-Host $outTask.Result
    if ($errTask.Result) { Write-Host $errTask.Result }
    $code = $proc.ExitCode
} finally {
    Pop-Location
}
if ($code -eq 0) { Write-Host 'win-compat gate passed' } else { Write-Host "win-compat gate failed ($code)" }
exit $code
