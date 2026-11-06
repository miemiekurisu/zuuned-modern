# macOS (Apple Silicon) port status

Dated 2026-11-06.  This is where the native Apple-silicon build of Zuuned actually
stands: what a Mac has to install, what has been done on a Mac against a real player,
and what has not.  It exists for the same reason `WINDOWS_PORT.md` exists, so that
nobody reads "it builds and the pages paint" as "it works with a Zune", and so that a
macOS fact is never quoted as Windows or Linux evidence - or the other way round, which
is the more tempting direction on this platform.

`BUILDING.md` owns the toolchain, the dependency locations, the packaging script and
every software measurement (launches, pages, settings tabs, CLI probes, CoreAudio,
VideoToolbox).  This page owns device facts only.  The Linux evidence lives in the
upstream repository and its gates; the Windows evidence lives in
[WINDOWS_PORT.md](WINDOWS_PORT.md).

## There is no driver to install, and no DriverKit extension either

The Mac build runs `libzune/src/usb_libusb.c` over Homebrew libusb 1.0.30 - the same
userspace path Linux uses.  Nothing is installed, nothing is bound, nothing is signed
for the kernel: no kext, no system extension, no udev rule, no Zadig equivalent.

That is a choice with a reason behind it, not an omission.  The only Apple-supported way
to own a USB interface on a modern macOS is an out-of-process DriverKit extension, and a
DEXT cannot load unless Apple's own notarisation signed it.  This project has no Apple
developer licence and no prospect of one, so `libzune/driver/` and
`libzune/src/driverkit_usb.c` stay out of the build entirely; the source is in the tree
and read by neither the macOS nor the other builds.  What that costs is one line of
honesty in the UI: `src/DevicePresence.cpp` answers from libusb alone here, the way it
does on Linux, so the "plugged in, but no driver zuuned can use" card - which is a
Windows-only `cfgmgr32` path - cannot fire on a Mac, and Settings -> Device says this
platform needs no driver rather than naming a tool that does not exist here.

## The interface question, measured

Windows had to be proven able to *see* a Zune before anything else, because an unbound
player is hidden from `libusb_get_device_list()` at PnP problem 28.  On macOS the same
question has a different shape and a better answer.  `packaging/macos/claimprobe.c` is
this platform's version of `packaging/windows/check-driver.ps1` - it opens the player,
asks who holds interface 0, prints the endpoints, and claims the interface:

```
libusb=1.0.30 hotplug=1
enumerated=4
  sighted bus=0 addr=5 045e:063e class=ff ncfg=1
open=0 (Success)
kernel_driver_active(0)=0 (Success)
active_config=0 (Success)
  config 1: interfaces=1 maxpower=250mA
  iface 0 alt0 class=ff/ff/ff eps=3
      ep addr=81 xfer=02 maxpkt=512 interval=0
      ep addr=01 xfer=02 maxpkt=512 interval=0
      ep addr=82 xfer=03 maxpkt=64 interval=4
claim_interface(0)=0 (Success)
VERDICT=claim_ok
release_interface(0)=0 (Success)
```

Four things to read out of that, because each one is a porting decision:

- The player is *visible* with nothing installed: it enumerates as `045e:063e`, class
  `ff` vendor-specific, and macOS binds no driver to it - `kernel_driver_active(0)`
  answers 0, so there is nothing to detach.  Linux can have a kernel PTP driver in the
  way; Windows has the opposite problem and hides the device outright.
- `claim_interface` succeeds on the first attempt.  No entitlement, no provisioning
  profile, no reboot, no user step.  This is the whole reason the port needs no Apple
  licence while a DEXT would have needed one.
- The endpoint set is the one `src/usb_libusb.c` reports when it opens the player -
  bulk `0x81`/`0x01` at 512 bytes and an interrupt endpoint `0x82` - so the two tools
  agree about what they are looking at.
- `libusb` reports `hotplug=1` here, and arrival is real: all three device sessions
  below log `[usbwatcher] Zune arrived (pid=0x063e)`, which is the hotplug callback,
  not `Zune arrived (poll)`, the poll fallback this watcher also carries.

`src/devicetool` answers the same sight on all three platforms and is the tool to run
first.  With the player attached on this machine:

```
libusb_enumerated=1 libusb_devices=4 libusb_hotplug=1 libusb_sighted=1
os_supported=0 os_present=0 os_usable=0 os_problem=0
VERDICT=agree platform scan declines to answer
```

`os_supported=0` is that Windows-only presence pass again, reported as *declines to
answer* rather than as *no device*: on a Mac the libusb sighting is the whole answer and
it agrees.  Run both tools outside any sandbox.  Inside a seatbelt/sandboxed shell libusb
enumerates **zero** devices, which is indistinguishable from an unplugged player; this
page's sibling sections already warn about the same class of false negative for
VideoToolbox and the audio devices.

## The first macOS device contact

**A Zune HD has been opened, authenticated, read, written to, read back, and had a track
deleted from it on Apple silicon.**  Measured 2026-11-06 on a Mac (macOS 26, Qt 6.12,
libusb 1.0.30) against the same Pavo Zune HD the Windows page describes, through the
`build-macos-release/zuuned.app` build-tree bundle launched with `open -n -g` - a
Launch Services launch, the shape a tester uses, not a terminal one.

```
[usbwatcher] Zune arrived (pid=0x063e)
[libusb] opened: bulk_in=0x81(512) bulk_out=0x01(512) int=0x82
[ptp] detected split header/data mode (Zune/WMP)
[libzune] GetDeviceInfo sent (885 bytes received)
[mtpz] ~/.mtpz-data not found, using embedded keys
[mtpz] handshake complete
[libzune] MTPZ authentication successful
[libzune] GetDeviceInfo: model='Zune' serial=[redacted]
[libzune] device family: Pavo (Zune HD) (0xD21A = 0x06040001)
[libzune] connected: [device name] (Zune), battery: 97%, storage: 10841047040/15543730176
[libzune-zmdb] totalSize=224940, payloadSize=224928, first chunk=12 bytes
[libzune-zmdb] received 224928/224928 bytes
[zunedb] scanned: 168 tracks, 0 videos, 5 photos, 10 albums, 10 artists, 0 playlists, 1 photo albums
```

So the driver question above was not the interesting one after all: the whole first read
comes from the same userspace path Linux uses, with the same ZMDB payload size and the
same Pavo property read.  The name fields carry the same device-side quirks as every
other platform and are counted rather than assumed: `GUID moniker fields on 168 track
rows: artist 25 ({09316835-...}), album 0 (-), genre 128`.  Those 25 artists display as
`Unknown Artist` and the 128 genres are dropped by `MusicIdentity::usableName()` - the
same predicate the Windows page records, doing the same job on the same player.

The player was unplugged and replugged between sessions, and each of the three sessions
below re-opened and re-read it from a fresh profile, so this is the connection path
working repeatedly rather than once.

## MTPZ, which the first Windows read never had

`[mtpz] ~/.mtpz-data not found, using embedded keys` then `handshake complete` then
`MTPZ authentication successful`.  The development machine's `~/.mtpz-data` was deleted
on purpose before the build so that the answer could only come from
`libzune/src/mtpz_keys.h`, which was created exactly as
[BUILD_WITH_MTPZ.md](BUILD_WITH_MTPZ.md) describes and is gitignored.  Authentication is
therefore built into the binary on this platform, which is what every write path below
needs and what the first Windows library read did without.

## One music send, and the re-index that followed

A 193,097-byte MP3 with no ID3v2 tag went through the ordinary controls with a QA spec
naming artist `Zuuned MAC QA`, album `MAC QA 2026-11-06`, title `MAC Roundtrip Test`:

```
[sync] retagged to ID3v2.3: .../qa-macos/fixtures/mac-roundtrip.mp3
[sync] audio target=mp3 produced=mp3 tag=0x0000
[libzune] created album "MAC QA 2026-11-06" by "Zuuned MAC QA" (1 tracks, id=100663582)
[sync] forge done: 1 ok, 0 failed
[qt.debug] qml: [qa-device] sync-summary elapsedMs=2680 tracks=1 videos=0 photos=0 skipped=0 failed=0
[libzune-finalize] trying ReportAddedDeletedItems (0x9201)...
[libzune-finalize] 0x9201 result: -1 (0=ok)
[libzune-finalize] trying CleanDataStore (0x9108)...
[libzune-finalize] 0x9108 result: 0 (0=ok)
[libzune-finalize] trying ReportAcquiredItems (0x9202)...
[libzune-finalize] 0x9202 result: -1 (0=ok)
[device] re-index reconnect: attempt 1 of 6 in 6000ms
[zunedb] scanned: 169 tracks, 0 videos, 5 photos, 11 albums, 11 artists, 0 playlists, 1 photo albums
[device] storage: ... 10840850432/15543730176
```

`audio target=mp3 produced=mp3` is the no-conversion case: the source already matched the
selected profile, so the only thing the app wrote was an ID3v2.3 tag on the copy it
sends.  The player's own accounting agrees with the send - free space fell by 196,608
bytes for a 193,097-byte file, which is the file plus the allocation the filesystem
needed.  The library grew by exactly the one album and the one track.

Two finalize operations answered `-1` on this platform, and the re-index happened anyway,
which is why the bounded reconnect ladder exists: it exists precisely because
finalisation is not a reliable readiness probe.  Attempt 1 of 6 landed the player back at
169 tracks.  The two `-1`s are recorded, not diagnosed; `libzune/docs/` is where that
belongs and neither platform has chased them.

## The pull, and what came back byte for byte

The same track taken off the player in a second session, into a watch folder:

```
[qt.debug] qml: [qa-device] download-from itemId=16777487 readbackDurationMs=8046
[qt.debug] qml: [qa-device] download-into /tmp/macfx/music
[worker] extracting track 16777487 → /tmp/macfx/music/Zuuned MAC QA/MAC QA 2026-11-06/MAC Roundtrip Test.mp3
[qt.debug] qml: [qa-device] download-saved name='MAC Roundtrip Test.mp3' ok=true
```

It landed where the library declares music goes -
`<music folder>/<artist>/<album>/<file>` - rather than in a directory of the app's own
choosing.  What came back is the interesting part, and it was measured rather than
eyeballed:

```
sent    193,097 bytes, no ID3v2, frame sha256 be1ac731ab4e7bf5e912fd1a1101aa3a
pulled  194,283 bytes = ID3v2 1,058 + frames + ID3v1 128
        frames sha256    be1ac731ab4e7bf5e912fd1a1101aa3a   (identical)
decoded 709,632 bytes of s16le/44100/mono from each
        sha256 027d902a1c95edc05cb25cbe1cacbd90 from each    (identical)
```

So the container grew by 1,186 bytes of tags and not one audio frame changed.  This is the
music invariant stated the way the Windows page states it for WMA Lossless: judge a pull
by the payload and the decoded samples, never by the file bytes, because a Zune rewrites
the tags it holds device-side.  With an MP3 the payload happens to survive; that is not a
promise about the ASF formats, and it is not a promise about WMA Lossless on this
platform, which cannot be produced here at all - see the known issue in `README.md`.

## Deletion

A third session, `--qa-delete`, right-click purge on that same item, in Chinese UI:

```
[qt.debug] qml: [qa-delete] delete-before deviceTracks=169 itemId=16777487 durationMs=8046
[worker] purge: 1 item(s) requested
[worker] purged 16777487 ok
[qt.debug] qml: [qa-delete] delete-settled deviceTracks=168 rowGone=true
           freeGB=10.841044715 status="已删除 1 项 —— 释放 0 MB"
```

169 tracks became 168 in a *re-read* of the player, the row is gone from the device list,
and free space went from 10.840850432 GB to 10.841044715 GB - 194,283 bytes, which is
exactly the size of the file that had just been pulled off.  The agreement is the
player's, not a row dropped from a list.  `0 MB` is a floor on a display rounded to
megabytes, and it is honest about being one.

## Photos and video: read, not transferred

The scan tallies 5 photo objects and 1 photo album, and three of those photos produced a
picture on this platform by `resized full file` - libzune fetching the original and
shrinking it because every thumbnail method the player offers failed.  The ZMDB scan
reports **0 videos** while the HD MTP fallback pass finds **1** video object and names it,
and that object's thumbnail fails every method.  None of it was written, taken back, or
deleted: photo, video, podcast and audiobook *transfer* remain unproven here, and the read
above is a device-page read, which is a different gate with a different name.

## Not clean

Recorded so the next reader does not spend a sitting rediscovering it, and not diagnosed:

- `get_object_info 0x07000002` answers `0x2002` three times per scan, against the atom the
  scanner itself names a playlist record.  Same atom, same response, same platform-blind
  behaviour the Windows page records.
- `get_object_prop_value` on three `0x0300xxxx` objects for property `0xDC86` answers
  `0xA801`.
- `all thumbnail methods failed` for `33554718` (the one video), `100663393`, `100663578`
  and `100663582` - the last being the album this very run created, which is the expected
  shape of that failure and still a missing picture in the UI.
- A `[tmdb]` lookup timed out during one session.  Online artwork is off by default and
  was off; this is a transient network fact, not a device fact.

## Not proven on macOS

Not inherited from Linux or Windows either, in either direction:

- Deletion of anything that is not that one music track.
- The podcast, audiobook, photo and video transfer categories, in either direction.
- Every Zune model but this one Pavo.  Nothing here has been tried on a 30/80/120 GB
  player, and this page makes no claim that the Pavo's numbers generalise.
- Anyone hearing anything from this Mac.  `BUILDING.md` measures that CoreAudio binds and
  the playhead advances; nobody has listened, and the track that was heard on the HD in
  the Windows record was heard *there*, on a file encoded *there*.
- The *removal* half of hotplug.  Arrival is measured three times over; `Zune left` - the
  watcher's own departure line - has never once appeared in a macOS log here.
- The packaged bundle with a player attached.  The evidence above is the build-tree
  bundle resolving Qt and libusb through `/opt/homebrew`; the self-contained bundle below
  has been verified structurally and launched with no device.  Nobody has plugged a Zune
  into the `.dmg` copy yet.
- Anything at all through the `.dmg` on a second Mac.  The archives were opened by this
  machine, and `spctl` answers `rejected` for them, which is what ad-hoc signing means.

## Packaging, which is closed here and open on Windows

`packaging/macos/deploy.sh` produces the self-contained bundle and both archives, and
`BUILDING.md` keeps that section current with the measured counters:
`install_names_rewritten=42`, `load_commands_checked=1936 self-contained=yes`,
`signed_macho=173`, `zip_appledouble_entries=0`, `smoke=rendered`.  The dmg was mounted
read-only and its bundle verified `--deep --strict` and launched from inside the volume;
the zip was unpacked by `unzip` and left no AppleDouble file.  That is structure: the
bundle carries its own libraries, and every device sentence above belongs to this page and
not to that script.  Because there is no Apple developer licence, there is nothing to
notarise, so a tester answers Gatekeeper by right-click -> Open, and a bundle built inside
`~/Documents` needs the Documents-folder consent - both measured, both in `BUILDING.md`.

## How these runs were made

Three launches of the Release bundle, each in its own `ZUUNED_PROFILE_DIR` seeded with the
same two-tone library, each through `open -n -g --args --page deviceMusic`:

```bash
--qa-device "<file>|<artist>|<album>|<title>|<duration-ms>"   # send, then re-read
--qa-device "|<artist>|<album>|<title>"                      # pull into a watch folder
--qa-delete     "<artist>|<album>|<title>"                   # purge, then re-read
```

The read-back judgement is the duration the spec asks for, and the byte comparison above
is `ffmpeg -f s16le` over the sent and pulled files plus an ID3v2 synchsafe-size split of
the pulled one.  Those three scripts and their captured logs live in `qa-macos/`, which is
gitignored run artefact - this page is the record; the `--qa-device` and `--qa-delete`
hooks it quotes are in `qml/` and `src/` and are the same hooks the Windows gates drive.
