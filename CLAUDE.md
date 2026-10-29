# CLAUDE.md - AI Assistant Guide for Zuuned Modern

**This file is a pointer, not the guide.** `AGENTS.md` is the authoritative,
current log of this tree and every rule that binds a change in it. This file used
to be a frozen copy of that guide from the upstream Linux line, and a copy is how
a stale instruction survives: it said `usbreset 045e:0710` was the only way back
from a wedged OpenSession, which stopped being true the day Windows grew a
measured answer. It is a pointer now. Read `AGENTS.md`.

## What this tree is

Zuuned Modern: a **Windows client port** of upstream
[badgeromen/zuuned-linux](https://github.com/badgeromen/zuuned-linux). One source
tree, built natively with MSVC (Qt 6.11 `msvc2022_64`, vcpkg plus a
`ZUUNED_WIN_DEPS` tree). No WSL, no MinGW, no Cygwin - which is why the Linux line
in the same tree is unchanged by construction rather than re-verified on every
Windows change. The fork publishes no Linux binaries.

- `docs/WINDOWS_PORT.md` - the single status page for the port. Closed and open
  Windows gates live there and nowhere else.
- `docs/WINDOWS_COMPAT_AUDIT.md` - the Windows compatibility sweeps, defect by
  defect, including what each sweep deliberately left alone.
- `BUILDING.md` - toolchain, and the byte-level account of the `/showIncludes`
  dependency defect.
- `docs/PORT_PLAN.md` - the plan for the port.
- `README.md` - what a user sees.

## The rules that outlive any one session

- **No Windows hardware claim that `docs/WINDOWS_PORT.md` has no capture of.** No
  Linux hardware evidence crosses platforms in either direction, in a commit
  message, a document or a final answer.
- Nothing has been listened to on the PC under Windows - the audio gate opens a real
  output muted and never hears it - so never write that playback works there. What has
  been heard came out of a player: on 2026-10-29 a Zune HD played a WMA Lossless file
  this tree encoded. That is the device playing, not Windows playing, and it licenses
  no claim about PC playback.
- After a header-only edit, build with `cmake --build build --clean-first`.
  Ninja's `deps = msvc` never matches this machine's localized `/showIncludes`
  output, so every object records `#deps 0` and an incremental build silently
  recompiles nothing while still relinking and looking busy. Check with
  `ninja -t deps <obj>`.
- A build started without the MSVC environment fails as about a hundred bogus
  `std::thread` diagnostics from the Windows SDK, not as a linker error. Load
  `vcvars64.bat` before believing a compiler.
- Gates: `tests\win-compat\run.ps1` and
  `tests\win-usability\run.ps1`. The second launches the real app once per page
  into isolated profiles and captures what the window actually paints with
  `build\win-usability\winshot.exe`. It opens no USB handle itself; with a
  player attached the application still auto-connects inside those profiles.
- With a player plugged in, software work is read-only: no transfers, no
  deletion, and never open the device from a second instance while the running
  application holds it.
- After a completed user-facing local build, restart only Zuuned with the fresh
  executable and the same profile. Defer it past an active sync. Never close the
  terminal or the desktop session.

## History

The upstream log this file carried is in `git log -- CLAUDE.md`. Do not restate it
here: `AGENTS.md` is where the current log is kept, and this pointer is what keeps
the two from disagreeing.
