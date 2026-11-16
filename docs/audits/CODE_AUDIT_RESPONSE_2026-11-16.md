# Code audit response - 2026-11-16

Source: `docs/audits/CODE_AUDIT_2026-11-16.md`, an independent read-only audit of this tree
carried out on 2026-11-16 (findings A-01 to A-15, no A-08 - the numbering skips it). This file
records what was done with each finding on 2026-11-16/17, and what evidence backs the claim.

Two words carry weight here. **Measured** means a gate or a real player produced it.
**Compile-verified** means the tree builds and links with the change and no gate on this machine
reaches it - which is not the same sentence as "works", and is never written as if it were.

## Disposition

| ID | Audit severity | Verdict | Where | Evidence class |
|---|---|---|---|---|
| A-01 | high P0 | Fixed, plus a second site the audit did not list | `libzune/src/mtp.c:419`, `libzune/src/mtp.c:1198` | measured (gate + real player) |
| A-02 | high P0 | Fixed in the shape this category owns | `src/sync/SyncEngine.cpp:2666` | compile-verified |
| A-03 | mid P1 | Fixed (join), partly mitigated before | `src/sync/SyncEngine.cpp:443` | compile-verified, shutdown phases measured |
| A-04 | mid P1 | Fixed, three sites | `src/sync/SyncEngine.cpp:1212` | compile-verified |
| A-05 | mid P1 | Bounded and made loud; full fix declared open | `src/DeviceWorker.cpp:1223` | compile-verified |
| A-06 | low P2 | Fixed | `src/library/TmdbClient.cpp:154` | compile-verified |
| A-07 | low P2 | Fixed | `src/library/TmdbClient.cpp:33` | compile-verified |
| A-09 | low P2 | Fixed at the root - no shell is started any more | `compat/win/wincompat.c:1680` | measured (real player) |
| A-10 | low P3 | Fixed, four sites | `transcode/libav_transcode.c:548` | measured (encode/decode gate) |
| A-11 | eng | Fixed | `transcode/wma_lossless_enc.cpp:550` | compile-verified |
| A-12 | low P3 | **Not a defect** - the code is commented out | `libzune/src/device.c:31` | read of the source |
| A-13 | eng | Offered, every switch off by design | `CMakeLists.txt:168` | measured (configure + build) |
| A-14 | eng | Declined for this round, with reasons | - | - |
| A-15 | eng | Fixed, POSIX byte-identical | `src/diagnostics/Redactor.cpp:141` | compile-verified, gate ran |

## Three things the audit got wrong, in its favour and against it

- **A-12 is not a defect.** All eight `system()` calls the finding names sit inside one block
  comment in `libzune/src/device.c`: the comment opens at `:24` with `/* COMMENTED OUT ... */`
  text and closes at `:50`, and the block is split by `#ifdef __APPLE__` / `#else`. The compiler
  never sees any of it, so there is no ignored return value and no silent no-op at run time - the
  only thing here is a dead comment that a grep matched. Left exactly as it is: deleting someone
  else's commented-out macOS/Linux plumbing is not part of a defect round.
- **A-01 was under-reported.** `mtp_get_object_references()` (`libzune/src/mtp.c:1198`) carried
  the same `len < 4 + n * 4` wrapping multiply. A malformed `GetObjectReferences` response is
  reachable from the album/playlist walk, not just from `GetStorageIDs`, so fixing only the
  reported site would have left the identical heap overflow one screen down the file. Both now
  use the division bound copied from the correct sibling at `libzune/src/mtp.c:554`.
- **A-05 over-stated the blast radius, and the audit said so itself.** The fallback it suggests
  - register by filepath when `itemId == 0` - is not available under the current contract, see
  the finding below for why, and for what was done instead.

## A-01 - the framing bound

`if (n > (len - 4) / 4)` replaces `if (len < 4 + n * 4)` at both sites, which is the shape
`mtp_get_object_handles()` already used. No extra "sane maximum number of storages" cap was
added: the division already rejects every count the payload cannot physically hold, and a cap
invented here would be a new way to refuse a legitimate device.

The audit asked for a mock-response test, and `libzune/tools/mtpframingtool.c` is where that
belongs, but the harness could not fabricate a response at all - it had no way to make the bulk-IN
queue answer on command. It can now. A scripted queue feeds hand-built `GetStorageIDs` and
`GetObjectReferences` payloads through the real `ptp_send_request` + parse path, so
`mtp_get_storage_ids` and `mtp_get_object_references` are executed rather than described: +40
checks, `framing 1285 checks 0 failures` (was 1245). Measured output for the two counts the audit
named:

    [mtp] get_storage_ids: truncated (count=1073741824, len=12)
    [mtp] get_object_references: truncated (count=4294967295, len=12)

Both return -1 instead of allocating. The guard was also exercised by hardware: 15 instrumented
launches of the built application auto-connected to the attached Pavo Zune HD and each one read
its storage back (`storage: 10525163520/15543730176`), so the bound does not reject the answer a
real player gives.

## A-02 - the video marker survives its own failure

`onVideoSendDone()` now tests staleness before it touches the marker
(`src/sync/SyncEngine.cpp:2666`), and only then decides
(`src/sync/SyncEngine.cpp:2679`): `clearInflight()` when the send succeeded or when the
failure never reached an object id, and otherwise the in-flight pair is kept, a recovery status is
set, and the sync is cancelled.

The shape differs from audio and photo on purpose. A video marker is keyed by the wire filename
and carries no recovery record, so `retainFailedInflight()` has nothing to retain for this
category; holding `m_interruptedVideoFile` / `m_interruptedSend` is the same claim in the shape
video actually owns, and it reaches the same startup purge path. `m_currentVideoWireName`
(`src/sync/SyncEngine.h:463`, written at `src/sync/SyncEngine.cpp:2525`) exists so the failure
branch names exactly the object `markInflightVideo()` wrote rather than whatever the retry
renamed it to. Cancelling is not tidiness: the marker file has one slot, so letting the next send
proceed would overwrite the only evidence of the object this failure produced.

Compile-verified. No truncated video object was produced on hardware, deliberately - the
consequence the audit is protecting against is a firmware crash, and this repository does not
crash a user's player to collect a log line.

## A-03 - the destructor waits for its own work

`SyncEngine` now keeps the futures it starts (`src/sync/SyncEngine.h:468`) through
`trackBackgroundWork()` (`src/sync/SyncEngine.cpp:807`), called at both `QtConcurrent::run`
sites (`src/sync/SyncEngine.cpp:1975`, `src/sync/SyncEngine.cpp:2602`), and
`~SyncEngine()` (`src/sync/SyncEngine.cpp:443`) joins them after setting the cancel flag.

What was already true before: `src/main.cpp:388` drains the global pool through a scope guard
before the QML engine takes the singleton away, so at process exit the new join waits for nothing
that was not already being waited for. What the join buys is that the guarantee belongs to the
class that owns the work instead of to the order of statements in one function. The task bodies
still read a `QPointer<SyncEngine>` off-thread, which the audit correctly calls unsafe in
general; it is sound precisely while the owner is certainly alive, and that is what the join now
ensures. Rebuilding them as queued signals with no capture at all is a refactor, not a fix, and
is not in this round.

The shutdown-sensitive phases ran on the relinked build: `spoken worker across a close` (worker
came down inside its own budget, no abandoned resume point) and `every launch this gate made
closed on its own`, with `killed=0`.

## A-04 - three `return`s that meant `continue`

`refreshPodcastCleanupPreview()` at `src/sync/SyncEngine.cpp:1212`,
`src/sync/SyncEngine.cpp:1219` and `src/sync/SyncEngine.cpp:1232`. Every other filter in those
two loops already answered with `continue`; one repeated device object id, one feed-less row or
one already-matched candidate said nothing about the episodes beside them. Preview-only: the
audit is right that a real delete re-validates, so nothing was ever lost, but the preview was
under-reporting and the user could not tell that from an empty one.

## A-05 - bounded, and loudly bounded

`DeviceWorker::sendTrackWpd()` / `sendVideoWpd()` now say out loud when a send succeeded but the
WPD object id yielded no numeric handle (`src/DeviceWorker.cpp:1223`,
`src/DeviceWorker.cpp:1313`). No object id is printed - it is device content.

Why not the fallback registration the audit suggests: `m_sentTrackIds` is a
`QHash<QString, quint32>`, and a 0 inserted there behaves exactly like the key being absent
(`src/sync/SyncEngine.cpp:2829` reads it with a 0 default), so "register by filepath with an
unknown id" would add no information to the one map that needed it. The consumer already refuses
rather than guesses: an unresolved member sets `unresolved` and the playlist is not written
(`src/sync/SyncEngine.cpp:2840`), which is the safe outcome - a wrong member list on the player is
worse than a refused one. Whole-track dedup is genuinely unaffected, as the audit noticed, because
`noteSyncedTrack()` is called unconditionally at `src/sync/SyncEngine.cpp:2219`.

Open, and stated as open: the WPD backend forges playlists from object-id *strings*
(`o5000005`), which the quint32 contract between `DeviceWorker` and `SyncEngine` cannot carry when
the id is not a 32-bit handle. A WPD-native string-keyed member path is the real fix and is not a
defect-round change. No Zune took this path in this sitting: the player on the cable is bound to
WinUSB, not WPD.

## A-06 / A-07 - TMDB

The failure log line no longer prints the URL: `src/library/TmdbClient.cpp:154` reports
`QUrl::RemoveUserInfo | QUrl::RemoveQuery`, which keeps the endpoint and drops the `api_key` the
query carries. It no longer depends on the session-log Redactor having run first, which is what
the audit objected to: the raw stderr line preceded sanitisation.

`fetch()` now refuses a body over 32 MiB by aborting on `downloadProgress`
(`src/library/TmdbClient.cpp:33`, `src/library/TmdbClient.cpp:133`), the same ceiling
`src/library/ArtworkHttp.cpp` has always had.

## A-09 - no shell is started on Windows any more

The finding was right about the mechanism and understated it. `cmd.exe` expands `%VAR%` inside
double quotes, and there is no `%` escape in `cmd.exe` at all, so doubling the quotes - which is
what `shell_quote_dup()` did - cannot make a device-supplied file name inert. The fix is not a
better quote. It is a primitive that starts the same program with argv and never starts a shell:

- `zuuned_run_no_shell(const char *const argv[])`, declared in `compat/win/posix_shim.h:505` and
  implemented at `compat/win/wincompat.c:1680`. It builds the command line with the
  `CommandLineToArgvW` quoting rules (every argument quoted, backslashes before a quote doubled),
  converts with the ANSI code page - provably the same bytes `cmd.exe` used to decode, which is
  why this is not the moment to upgrade to UTF-8 - and runs `CreateProcessW` with
  `CREATE_NO_WINDOW`, one inheritable `NUL` handle wired to all three child std handles, and an
  infinite wait, matching the old `system()` contract. The return value is the child exit code, or
  -1. The child's output is discarded, which is what the Windows command line it replaced asked
  for with `>nul 2>&1`; a failing ffmpeg stays silent by design and its exit status is still
  checked.
- `zune_arm_photo()` (`libzune/src/photo.c:432`) and `zune_grab_photo_thumb()`
  (`libzune/src/thumbnail.c:136`) use it on Windows. The avfilter string keeps its single quotes
  because the *avfilter* parser consumes them, and `cmd.exe` never treated them as quotes either,
  so the string reaching ffmpeg is byte-identical. `shell_quote_path()` is deleted,
  `shell_quote_dup()` is now `#ifndef _WIN32`, and `SHELL_QUIET` is back to being the POSIX
  `2>/dev/null`. `zune_snap_thumb()` (`libzune/src/util.c:391`) gained the same branch; it is
  public API with no callers in this tree.
- Linux and macOS keep `char cmd[]` + `system()` byte for byte. There is no command-line room
  check on the Windows branch any more because there is no command line to overflow; the only name
  that can still fail is one the ANSI code page cannot spell, and `NULL` from
  `zuuned_narrow_path_for_shell()` already means that, and is refused rather than guessed.

Measured on hardware, and this is the part worth reading twice: the 19 instrumented launches of the
2026-11-17 `tests\win-usability` run each auto-connected to the Zune HD, and together they logged
`resized full file for thumbnail` 51 times, from 17 of those launches, while
`all photo thumbnail methods failed` appears zero. That message is printed only at
`libzune/src/thumbnail.c:159`, which on `_WIN32` is reachable only when the download succeeded
*and* `zuuned_run_no_shell()` returned 0. So the ffmpeg resize branch - which
`docs/WINDOWS_COMPAT_AUDIT.md` records as having "failed on every single call whatever it was
pointed at" - produced photo thumbnails from real device objects on real hardware 51 times in one
sitting. Prerequisite: ffmpeg reachable on PATH, which this app does not and will not bundle.
The `all thumbnail methods failed` lines still in those logs come from `zune_grab_thumbnail()` at
`libzune/src/thumbnail.c:69`, a different function with no shell-out in it at all, and they are
the pre-existing `get_representative_sample` defect recorded on 2026-10-20.

Left alone and named: `libzune/src/transcode.c:99`, `:161`, `:286` still `popen()` a hardcoded
`/opt/homebrew/bin/ffmpeg`. Zero callers, and on MSVC `popen` is undeclared, which is the source
of the three pre-existing `warning C4047` lines in a Windows build. Dead macOS plumbing that
compiles into the Windows target; deleting it is a separate decision.

## A-10 - the encoder answers its own failures

`avcodec_send_packet()` / `avcodec_receive_frame()` handling in `transcode/libav_transcode.c`
follows the contract F-01 established: `EAGAIN` drains and retries once
(`transcode/libav_transcode.c:548` flush, `transcode/libav_transcode.c:1895` audio), any other
negative is a failure and takes the failure path (`transcode/libav_transcode.c:1670` video,
`transcode/libav_transcode.c:2312` art) instead of continuing to write a short file that reports
success.

Measured by the encode and decode phases of the Windows usability gate on this build: profile 0
produces an ASF container with the source duration and frame size, profile 2 an ISO base-media
file at 480x272, the LAME leg an mp3 whose length matches the tone, and the lossless encoder a
WMA file whose `WM/Picture` is the picture that went in - including a cover past the head cap. The
hardware decode path still downloads frames the scaler and muxer accept.

## A-11 - `sprintf` on a stack buffer

`transcode/wma_lossless_enc.cpp:550` uses `snprintf`. The audit called it bounded-but-latent and
it was exactly that: no overflow was reachable, and now none is expressible.

## A-13 - hardening switches that ship off

`CMakeLists.txt:186` adds three MSVC-only switches: `ZUUNED_WARNINGS_AS_ERRORS` (OFF),
`ZUUNED_CONTROL_GUARD` for `/guard:cf` (OFF), and `ZUUNED_MSVC_WARNING_LEVEL` (`default`, which
appends nothing). With the defaults no compile or link command in the tree changes at all, which
is the point: the released Windows artifact is built from the bare configure line, so a hardening
switch must not be able to change what somebody who downloaded it runs. Measured: configure prints

    -- Zuuned hardening: warning level default, warnings as errors OFF, control guard OFF

and the 352-target build that produced every number in this file ran with those defaults.

`/WX` ships off for a reason that is now written down instead of argued: this tree does not build
clean today. Three warning sites are pre-existing and unrelated to this round - the dead
`popen()` lines above (`C4047` x3), a `%d` against a `qsizetype` at `src/DeviceWorker.cpp:513`
(`C4477`), and a const qualifier at `transcode/transcodetool.c:293` (`C4090`). Enabling `/WX`
means fixing those first, and `/W4` before that.

AddressSanitizer is deliberately not offered. It has never been run on this tree on this machine,
and a CMake preset that nobody has executed is a promise, not an option. That leaves the audit's
strongest suggestion open, and it stays open honestly: `/WX` plus ASan in CI would have said A-01
and A-04 out loud before a human reading pass did, and still would.

## A-14 - the big files

Declined for this round. The four the audit means are `src/LibraryService.cpp` (166.6 KB),
`qml/SettingsPage.qml` (146.9 KB), `src/DeviceWorker.cpp` (144.0 KB) and
`src/sync/SyncEngine.cpp` (143.6 KB). Splitting them is a refactor with no functional benefit and
a real regression risk against a beta that a user has just verified by hand on hardware, in a
sitting whose purpose is to close findings. The audit's own diagnosis is accepted -
A-02 and A-04 are both "one branch of a state machine disagreed with its neighbours", and that is
what a large file hides - so the offer stands as separate work with one condition: a state-machine
test per phase before any file moves, not after.

## A-15 - the home directory in a log line

`src/diagnostics/Redactor.cpp:141` also folds `QDir::toNativeSeparators(home)`, guarded by
`nativeHome != home` so on POSIX the string is unchanged and the fold cannot fire, and
`src/diagnostics/Redactor.cpp:160` adds a case-insensitive `X:\Users\...` / `X:\home\...` form for
paths whose user segment is not this user's. Windows-only by construction, and the privacy gate
ran on the build that carries it.

## Counters for the relinked build (2026-11-17)

- `cmake --build build --clean-first`: 352/352, exit 0. Three warning sites, all pre-existing and
  named under A-13; nothing new was introduced by this round.
- `mtpframingtool`: 1285 checks / 0 failures (was 1245).
- `librarytool paths`: ALL PASS, 73 checks.
- `tests\win-compat`: 204 checks / 0 failures / 0 skips.
- `tests\win-usability`: `launches=19 checks=157 failures=0 skips=0 killed=0`, `OVERALL=ok`. The
  harness as committed counts more launches and checks than the 17/95 quoted in `AGENTS.md`; that
  pair is a stale counter from an earlier harness, and this round changed no test file.
- `tests\privacy`: 19 checks / 0 failures. `python i18n\audit.py`: `AUDIT=ok`.
- `devicetool`: `VERDICT=agree` with the player bound to WinUSB and problem code 0.

## What this round did not earn

No audio was listened to, on the PC or on the player. No write, delete or read-back of any kind
was performed in this sitting: the 15 device sessions are reads - enumeration, storage, the ZMDB
scan, photo thumbnails. Deletion, the podcast and audiobook categories on Windows, video transfer,
every player model but this Pavo, and the WPD backend (the player here is on WinUSB) remain
exactly as open as they were. The A-02 and A-03 fixes are compile-verified with measured
neighbours, not measured themselves. The Linux and macOS lines are unchanged by construction,
which on this machine means unverified rather than verified.
