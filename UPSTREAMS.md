# Upstream references

Dated 2026-11-18.  This page records every external source that stands behind the
Zune HD wireless-sync work, what came across from each one, and what was
deliberately left where it was.  It exists so that a licensing question about
`libzune/src/wireless.c` can be answered from this repository rather than from
somebody's memory of a clone.

The protocol itself, and what has and has not been measured, is on
[docs/WIRELESS_SYNC.md](docs/WIRELESS_SYNC.md).

## The two reference clones

| reference | pin | licence, as measured | what it was for |
| --- | --- | --- | --- |
| [XuneSyncLibrary](https://github.com/magicisinthehole/XuneSyncLibrary) | `c2a6eebeac34d1a9e4c805b1ad51ec25895dfdc4` | `LICENSE` is the GNU LGPL-2.1 text, 501 lines | wire shapes for pairing, Wi-Fi enrolment, the device RSA key, PTP/IP framing and SSDP |
| [android-file-transfer-linux](https://github.com/magicisinthehole/android-file-transfer-linux), the `magicisinthehole` fork of `whoozle/android-file-transfer-linux` | `eb36f23ee213475c77bcecaa0898732f251e80b1` | `LICENSE` is LGPL-2.1, and 140 per-file headers read `Copyright (C) 2015-2020 Vladimir Menshakov` with `either version 2.1 of the License, or (at your option) any later version` - so **LGPL-2.1-or-later** | the MTPZ handshake this project already speaks, and the vendor-operation vocabulary around it |

`XuneSyncLibrary` vendors that AFTL fork as a submodule, which is why the two pins
appear together; the fork, not upstream `whoozle`, is what the submodule resolves
to at the commit above.

**LGPL-2.1-or-later is compatible with this project's licence by either of two
independent routes.**  The per-file header grants "or at your option any later
version", so that code may be taken under LGPL-3.0, which a GPL-3.0 project may
link.  And even taken strictly as LGPL-2.1, section 3 of that licence permits
converting the library to GPL-3.0 once it is combined with a GPL-3.0 program.
Nothing here depends on which of the two arguments one prefers.

This project is GPL-3.0: the root [`LICENSE`](LICENSE) is the GNU GPL version 3
text and [`COPYING.md`](COPYING.md) declares `GPL-3.0-or-later`;
[`libzune/LICENSE`](libzune/LICENSE) and [`libzune/COPYING.md`](libzune/COPYING.md)
say the same for the library whose tree this feature lives in.

## What actually crossed the boundary

**Protocol shapes only.**  Operation codes, property codes, payload layouts, byte
offsets and the order in which calls are issued - the facts about what goes on the
wire, which anybody with a bus trace would independently record the same way.  No
source line was copied, translated or adapted from either reference into this tree.

`libzune/src/wireless.c` is written against this project's own PTP layer: every step
goes through this library's `ptp_transaction()`, with this library's error
convention and this library's message wording, and the file carries no identifier,
comment or structure lifted from the references.  The same is true of
`mtpz_device_rsa_modulus()` in `libzune/src/mtpz.c` and of the new vendor operation
and property codes in `libzune/src/ptp.h`.

## A provenance gap in the reference, named rather than glossed

`XuneSyncLibrary` has a licensing hole a reader is entitled to know about before
trusting anything in it.  Measured at the pin above: of the tracked source files
under `include/`, `lib/`, `tests/` and `tools/` - 83 files, `.c`, `.h` and `.cpp` -
**not one carries a copyright header or names a licensor**.  The only notices in
that repository are its top-level `LICENSE` and the two vendored `libwdi` headers,
which is also the only reason the count inside `vendor/` differs: 85 tracked source
files, 2 of them noticed.  The repository has a single committer
(`magicisinthehole`), so there is no contributor history to infer a licensor from
either, and a top-level licence file does not by itself say who owns each file.

That is a reason for care, and it is why the boundary above is drawn where it is.
**Those files are relied on for the factual wire behaviour they describe, and not
as sources of code.**  Anyone tempted to port a function out of `XuneSyncLibrary`
should stop: with no per-file notice there is no clean answer to "who granted you
the right to redistribute this", and this tree does not need one.

The count itself depends on whether `vendor/` and its submodule are in the set, so
it is stated above as measured at the pin rather than as a figure carried over from
an earlier reading of the clone.

## Deliberately not used

Three parts of the two references were read and left alone on purpose.  They are
listed because the honest question is not "did you copy it" but "why is it not
here".

- **`vendor/libwdi` inside `XuneSyncLibrary`** - the Windows driver-installation
  library.  Its two headers are `Copyright (c) 2010-2014/-2021 Pete Batard` under
  **LGPL-3.0-or-later**, a second copyleft to reason about for no benefit, and
  `vendor/libwdi/lib/x64/libwdi.lib` is a 4,571,282-byte prebuilt static library
  whose own `VENDORING.md` describes it only as "the existing Windows x64 static
  library", with a regeneration recipe documented for the ARM64 half and none for
  the x64 half.  libwdi's design embeds a driver package - including a
  Microsoft-signed driver coinstaller - inside whatever binary links it, which is
  precisely the kind of payload this project will not ship unexamined.  Zuuned
  installs no driver and ships no driver installer: the documented route is WinUSB
  through Zadig, in `packaging/windows/DRIVER_INSTALL.md`.
- **`lib/src/ZuneDeviceIdentification.cpp` lines 6-40** - the file's own comment
  calls the tables "Color lookup tables extracted from ZuneNativeLib.dll via Ghidra
  decompilation".  That is a self-declared derivative of a Microsoft binary, and
  decompilation output is expression rather than fact, so it is the least portable
  material in the reference.  This project does not need it: model identity already
  comes from the `0xD21A` family property, and colour names are a cosmetic detail
  nobody has asked for.
- **`aftl/mtp/backend/linux/usb/linux/usbdevice_fs.h`** - `Copyright (C) 2000
  Thomas Sailer`, "either version 2 of the License, or (at your option) any later
  version", i.e. **GPL-2.0-or-later**.  A GPL-2.0-only component would be the one
  genuine licence friction in a GPL-3.0-or-later tree; this is the Linux usbdevfs
  header of a C++ MTP stack this project never compiles, so the question does not
  arise.

## No Microsoft secret is in either reference, and none is in this feature

Neither clone contains Microsoft private key material, and this feature introduces
none.

The MTPZ credentials stay exactly where they already were: an external,
user-supplied `.mtpz-data` file in the user's home directory - `$HOME` on Linux and
macOS, `%USERPROFILE%` on Windows, which is the fallback
`mtpz_load_keys_from_file()` uses because Windows never sets `HOME`.
`libzune/src/mtpz.c` tries that file first and the public source embeds no default;
a private build may opt in by supplying its own git-ignored
`libzune/src/mtpz_keys.h`, and `.gitignore` already refuses that path along with
`.mtpz-data`, `*.pem` and `*.key`.  See [docs/BUILD_WITH_MTPZ.md](docs/BUILD_WITH_MTPZ.md).

**The wireless step uses no Microsoft key at all.**  The Wi-Fi passphrase is
encrypted under the player's *own* RSA-1024 public key, which the device hands over
inside the ordinary authenticated MTPZ handshake it already performs - see
`mtpz_device_rsa_modulus()` and the device-RSA section of
[docs/WIRELESS_SYNC.md](docs/WIRELESS_SYNC.md).  Nothing about the wireless feature
widens what the MTPZ credentials are for, and nothing about it asks for them to
live anywhere other than where they do today.

## The LGPL text this repository ships

[`LICENSES/LGPL-2.1.txt`](LICENSES/LGPL-2.1.txt) is the GNU Lesser General Public
License, version 2.1, verbatim - 502 lines, byte-identical to `aftl/LICENSE` apart from
line endings, which are LF here.  `XuneSyncLibrary/LICENSE` is the same licence in the
Foundation's newer spelling, 501 lines: it prints a URL where this text prints the
Foundation's postal address, and its how-to-apply paragraph carries a later signature
block.  Those are the only differences, so either clone's copy is the same licence, and
the older spelling is the one shipped here.  It ships for completeness.
No line of LGPL code is in this tree; the notice is on the shelf so that the terms
this project's protocol knowledge was checked against can be read without leaving
the repository, and so that a future change which *does* take LGPL code finds the
text present instead of inventing a licence for it.

## Two follow-ups this page cannot perform

- Three source comments state the licence of these references loosely, and editing
  source comments is outside this change's scope.  Measured at this revision: the
  header of `libzune/src/wireless.c` and the vendor-operation block in
  `libzune/src/ptp.h` both spell the reference correctly as **XuneSyncLibrary** but
  describe it and the AFTL fork as "both under the GNU LGPL-2.1", which under-states
  the fork - its 140 per-file headers carry the "or at your option any later
  version" grant.  `libzune/include/zune.h` is the one that is wrong in both
  directions at once: it says "the LGPL-2.1-or-later Xune Sync Library", a name with
  a space that is not that project's name, with the "or later" grant attached to the
  one reference that has no per-file notices capable of granting it.  The wording on
  this page is the one to trust.
- Both clones live outside this repository, in a local read-only audit area.  They
  are not vendored, not submodules, and not committable here, and nothing in this
  tree takes a build-time dependency on either.
