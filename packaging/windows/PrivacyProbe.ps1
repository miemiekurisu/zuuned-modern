# Zuuned Modern - the byte reader behind every privacy pass, on its own so a gate can aim it
# at a repository, a staged folder or a finished .zip without staging anything first.
#
#   . packaging\windows\PrivacyProbe.ps1
#   @(Find-PrivacyNeedles -Path some\file -Needles @('an account name'))
#   @(Find-PrivacyNeedles -Path some\file -BytesOnlyNeedles @('D:\some\root'))
#
# This is not two lines for four reasons, and every one of them is a way a name gets into a
# release while the check says it did not.
#
#   Encodings.  A name sits as plain bytes in a text file and as a wide string in the
#   .rdata of a DLL, so a needle is matched as itself and as itself with a zero byte after
#   every character.  Where the caller knows a needle can only ever be a path baked into a
#   text file it says so with -BytesOnlyNeedles, which is what lets a release demand that its
#   own executables are silent about where they were built while a third-party DLL, which
#   records the same kind of fact and names nobody, is only counted.
#   Encodings again.  Decoding is Latin-1, which maps every byte to exactly one character,
#   so no name can hide in a translation between code pages.  The machine this package gets
#   built on is locale 936, where the local 8-bit codec is not ASCII and not UTF-8.
#   Case.  Matching ignores case.  An object file keeps whatever case the shell happened
#   to have when it was compiled, a log writes the same account lower-case, and a path baked
#   by another tool spells its drive letter the other way round; a name that differs only in
#   its capitals is the same name.  This is the only safe direction for a privacy pass:
#   ignoring case can turn a clean into a finding, never a finding into a clean.
#   Size.  A package holds a 115 MB media runtime, so a file is read in chunks, and each
#   chunk carries the tail of the one before it - a string straddling a chunk edge is the
#   classic way a chunked scan produces a false clean.
#
# The reader works on a Stream, not a path, because the bytes that actually leave a machine
# are often the entries of an archive: a zip member has no path to open and must not need one.
# That is also why the loop counts bytes instead of asking the stream where it is.  A
# ZipArchive member is a DeflateStream, which is not seekable - Position answers nothing - and
# it hands back less than it was asked for and sometimes nothing at all between two reads, so
# a request for 4 MB measured against Position stopped at a quarter of avcodec-63.dll and this
# reader called that the end of the file.  Progress is therefore measured by what arrived, a
# silent read is retried, and a stream that stops short of the length it was declared to have
# is reported as unreadable rather than as clean.

$script:PrivacyLatin = [Text.Encoding]::GetEncoding(28591)

function Find-PrivacyNeedlesInStream {
    <#
    .SYNOPSIS
        Which needles appear anywhere in a stream, as bytes or as UTF-16LE.
    .DESCRIPTION
        Returns the distinct needles that were found, so a caller can tell a person's name
        from a build path without knowing how either was stored.  The caller keeps ownership
        of the stream; this reads from wherever it is positioned to the end and never closes
        it.  A stream it cannot read to the end is reported through -ReadError rather than as
        a clean stream: a privacy pass that shrugs at an unreadable file has measured nothing,
        and the caller decides what that is worth.
    .PARAMETER Length
        How many bytes to read.  -1 asks the stream, which is right for a file and wrong for a
        decompressing member, so a caller who opened a zip entry passes its declared size.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.IO.Stream]$Stream,
        [AllowEmptyCollection()][string[]]$Needles = @(),
        [AllowEmptyCollection()][string[]]$BytesOnlyNeedles = @(),
        [long]$Length = -1,
        [int]$ChunkBytes = 1048576,
        [ref]$ReadError
    )
    $probe = New-Object System.Collections.Generic.List[string]
    $kind = New-Object System.Collections.Generic.List[string]
    foreach ($n in @($Needles)) {
        if (-not $n) { continue }
        $probe.Add($n)
        $kind.Add($n)
        $w = ''
        foreach ($ch in $n.ToCharArray()) { $w = $w + $ch + [char]0 }
        $probe.Add($w)
        $kind.Add($n)
    }
    foreach ($n in @($BytesOnlyNeedles)) {
        if (-not $n) { continue }
        $probe.Add($n)
        $kind.Add($n)
    }
    $hits = New-Object System.Collections.Generic.List[string]
    if ($probe.Count -eq 0) { return $hits }
    if ($ChunkBytes -lt 1024) { $ChunkBytes = 1024 }
    $remaining = $Length
    if ($remaining -lt 0) { $remaining = $Stream.Length - $Stream.Position }
    $stalls = 0
    try {
        $tail = ''
        $buf = New-Object byte[] $ChunkBytes
        while ($remaining -gt 0) {
            $want = [int][math]::Min($ChunkBytes, $remaining)
            $got = $Stream.Read($buf, 0, $want)
            if ($got -le 0) {
                $stalls++
                if ($stalls -gt 200) {
                    throw ($remaining.ToString() + ' byte(s) of ' +
                        $Length.ToString() + ' never arrived')
                }
                continue
            }
            $stalls = 0
            $remaining -= $got
            $s = $tail + $script:PrivacyLatin.GetString($buf, 0, $got)
            for ($i = 0; $i -lt $probe.Count; $i++) {
                if ($s.IndexOf($probe[$i], [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    if (-not $hits.Contains($kind[$i])) { $hits.Add($kind[$i]) }
                }
            }
            $tail = $s.Substring([math]::Max(0, $s.Length - 128))
        }
    } catch {
        if ($ReadError) { $ReadError.Value = $_.Exception.Message }
    }
    return $hits
}

function Find-PrivacyNeedles {
    <#
    .SYNOPSIS
        Which needles appear anywhere in a file, as bytes or as UTF-16LE.
    .DESCRIPTION
        The file form of Find-PrivacyNeedlesInStream: same needles, same two encodings, same
        chunk tail.  Opens shared for read so a file another process still has open is scanned
        rather than excused, and reports a file it cannot open at all through -ReadError
        instead of as a clean file.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [AllowEmptyCollection()][string[]]$Needles = @(),
        [AllowEmptyCollection()][string[]]$BytesOnlyNeedles = @(),
        [long]$Length = -1,
        [int]$ChunkBytes = 1048576,
        [ref]$ReadError
    )
    $hits = New-Object System.Collections.Generic.List[string]
    $fs = $null
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                              [IO.FileShare]::ReadWrite)
        # A [ref] parameter handed a null throws while binding, and the catch a few lines
        # below folds that throw into an empty hit list - a caller being told a file is clean
        # because it forgot to ask why it was not.  This exact shape once made every call that
        # left -ReadError out answer "no needles found", including the wide-encoding half, which
        # is the half the whole probe exists for, so the ref handed down is always a real one.
        $inner = $null
        $hits = Find-PrivacyNeedlesInStream -Stream $fs -Needles $Needles `
            -BytesOnlyNeedles $BytesOnlyNeedles -Length $Length `
            -ChunkBytes $ChunkBytes -ReadError ([ref]$inner)
        if ($inner -and $ReadError) { $ReadError.Value = $inner }
    } catch {
        if ($ReadError) { $ReadError.Value = $_.Exception.Message }
    } finally {
        if ($fs) { $fs.Dispose() }
    }
    return $hits
}

function Read-PrivacyTextPrefix {
    <#
    .SYNOPSIS
        The first bytes of a stream, as Latin-1 text, for a question a needle cannot ask.
    .DESCRIPTION
        Find-PrivacyNeedlesInStream answers "does this literal string appear anywhere".
        Some rules are patterns rather than strings - a USB devnode instance path is one, since
        the private half of it is whatever the owner's player printed - and this is the one
        place a caller can get bytes back as text to ask a pattern.  It reads at most -Limit
        bytes with the same counted-read discipline as the scanner, because a decompressing zip
        member is not seekable and says nothing about where it is, and a stream that stops short
        of the length it was declared to have is reported through -ReadError rather than as a
        short-but-fine read.  Latin-1 again, so no byte disappears into a code page on the way
        to the pattern.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.IO.Stream]$Stream,
        [long]$Length = -1,
        [long]$Limit = 4194304,
        [ref]$ReadError
    )
    $text = ''
    if ($Limit -lt 1) { return $text }
    $remaining = $Length
    if ($remaining -lt 0) { $remaining = $Stream.Length - $Stream.Position }
    if ($remaining -gt $Limit) { $remaining = $Limit }
    $stalls = 0
    try {
        $buf = New-Object byte[] 262144
        while ($remaining -gt 0) {
            $want = [int][math]::Min($buf.Length, $remaining)
            $got = $Stream.Read($buf, 0, $want)
            if ($got -le 0) {
                $stalls++
                if ($stalls -gt 200) {
                    throw ($remaining.ToString() + ' byte(s) never arrived')
                }
                continue
            }
            $stalls = 0
            $remaining -= $got
            $text += $script:PrivacyLatin.GetString($buf, 0, $got)
        }
    } catch {
        if ($ReadError) { $ReadError.Value = $_.Exception.Message }
    }
    return $text
}

function Get-PrivacySerialRule {
    <#
    .SYNOPSIS
        The pattern for the private half of a USB device instance path.
    .DESCRIPTION
        A Windows devnode is USB\VID_045E&PID_063E\ plus a serial number.  The identifiers
        name the model, which this project documents on purpose because a driver page has
        to say what to pick, and the tail names one physical player, which belongs to
        whoever had it plugged in while the page was written.  A document that quotes the
        path to teach somebody to bind WinUSB keeps the first half and drops the second,
        and this is the rule that notices when it did not.  Anchored on the VID and PID so
        a bare hex string in a fixture or an album GUID read off a device stays a clean, and
        case-insensitive for the same reason the needle matcher above is: Windows prints the
        path upper case and the Zune tooling prints the very same path lower. Ignoring case
        can turn a clean into a finding, never a finding into a clean.
    #>
    return '(?i)VID_[0-9A-F]{4}&PID_[0-9A-F]{4}\\[0-9A-F][0-9A-F\-]{5,}'
}