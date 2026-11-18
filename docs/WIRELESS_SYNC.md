# Zune HD wireless sync

Dated 2026-11-18.  This is the protocol and status page for wireless (Wi-Fi) sync
between a Zune HD and this application: what the player is asked to do, in what
order, with which bytes, and - the reason a page like this exists rather than a
paragraph - which of that anybody in this repository has seen on a cable and which
is a careful reading of somebody else's source.

**Only the read half of this page has ever been asked of a player.**  The wireless
property reads and the Wi-Fi scan have been run over a cable on a Zune HD, and what
came back is recorded under Status in
"What has actually been asked of a player" - withheld in part, because the values name
a partnership and name the access points in this building.  **No pairing write has ever
been made.**  Neither `0xD401` nor `0xD220` has been written from this tree, no access
point has been enrolled, and wireless sync has never been switched on by this software.
One PTP/IP connection has since been opened - to a player whose Sync switch a person had
already turned on by hand, from a throwaway script that lives outside this repository, and
it drew no answer. "The first network sitting (2026-11-18)" is that whole record, and it is
also why this page no longer assumes a player announces itself.  Every wire *shape* here is
derived from the reference implementations named in [UPSTREAMS.md](../UPSTREAMS.md); where
this tree has its own observation it says so, and where it does not, the sentence is the
references' reading of somebody else's capture.  Read this paragraph before quoting any
byte offset below to someone who is about to plug a player in.


The licence position, the two pinned clones, the provenance gap in one of them, and
the three parts of them deliberately left where they are:
[UPSTREAMS.md](../UPSTREAMS.md).  Device evidence in general belongs to
[WINDOWS_PORT.md](WINDOWS_PORT.md) for Windows.  The one kind of device evidence this page
carries is the set of network observations of 2026-11-18, which are Windows observations
about one Pavo on one access point and travel no further than that sentence.

## What the arrangement actually is

A Zune HD will sync over Wi-Fi with no certificate, no pairing PIN and no shared
account.  What it needs instead is a partnership, and a partnership is two property
writes over the cable:

- the player stores the GUID of the **one** host it will talk to, in property
  `0xD401`;
- the player hands out a session GUID of its own, property `0xD221`;
- after that it speaks standard PTP/IP - ISO 15740, plain TCP port 15740, no TLS -
  to any host that presents that session GUID in its init packet.

The Wi-Fi access point passphrase is the only secret on the link, and the player
will not accept it in the clear: it is encrypted under the **device's own**
RSA-1024 public key, which falls out of the ordinary MTPZ handshake the library
already performs.  So the whole design is one USB session to teach the player who we
are and what its network password is, and then Ethernet frames.

No Microsoft key material is involved in the wireless step.  The MTPZ credentials
are needed only because the device public key is only visible inside the decrypted
MTPZ handshake payload, and they stay the external user-supplied
`$HOME`/`%USERPROFILE%/.mtpz-data` file that `libzune/src/mtpz.c` has always treated
them as.  See [BUILD_WITH_MTPZ.md](BUILD_WITH_MTPZ.md).

## The vocabulary this uses

None of these codes appears in a public Microsoft header.  They are what the Zune
desktop software sends, as recorded by the two references; the numbers are in
`libzune/src/ptp.h:99` (operations) and `libzune/src/ptp.h:183` (properties).

| code | name in this tree | what it is for |
| --- | --- | --- |
| `0xD21C` | `MTP_DPC_Zune_SyncFlags` | one-byte sync flags; the reference writes a bare `0x00` here during pairing |
| `0xD217` | `ZUNE_DPC_SyncProbe` (`libzune/src/wireless.c:46`) | read-only probe; a player that refuses it is a player with no Wi-Fi |
| `0xD220` | `MTP_DPC_Zune_PCSessionGUID` | which partner the player will accept over the network |
| `0xD221` | `MTP_DPC_Zune_WirelessSessionGUID` | the GUID a PTP/IP init must present |
| `0xD225` | `MTP_DPC_Zune_PartnerGUID` | stored partner; the reference clears it with a null GUID |
| `0xD229` `0xD22A` `0xD230` | `SyncCounterA/B/C` | sync counters the reference writes during its long pairing |
| `0xD401` | `MTP_DPC_Zune_SyncPartner` | the partnership record |
| `0xD402` | `MTP_DPC_Zune_DeviceFriendlyName` | the name the player shows |
| `0xD406` | `MTP_DPC_SessionInitiatorInfo` | who is driving; a version string in the desktop software |
| `0x9224` | `MTP_OC_Zune_Op9224` | no params, returns data; the reference runs it just before handing over a network profile |
| `0x9225` | `MTP_OC_Zune_Op9225` | the player's own Wi-Fi scan, returned as a blob |
| `0x9227` | `MTP_OC_Zune_Op9227` | carries a network profile; the 4-byte init form the reference sends inside its long pairing is answered `0x2002` on purpose |
| `0x9228` | `MTP_OC_Zune_Op9228` | one-param post-enrolment step |
| `0x922B` | `MTP_OC_Zune_Op922B` | the subsystem gate, in two forms - see the trap below |
| `0x922C` `0x922D` | `MTP_OC_Zune_Op922C/2D` | the PPP byte pipe the other route uses |
| `0x922F` | `MTP_OC_Zune_Op922F` | network subsystem reset; the reference reads 1,036 bytes back from it |
| `0x9230` | `MTP_OC_Zune_Op9230` | wireless mode: parameter 1 enables, 2 disables |
| `0x9231` | `MTP_OC_Zune_Op9231` | network subsystem init, a 258-byte all-zero request |
| `0x9217` `0x9218` `0x9215` | - | database-sync operations; `0x9217` is deliberately not part of pairing here |

## What is in this tree

Line numbers in this page are written against the revision that last edited it and drift
as `wireless.c` grows; the function and property names are the durable anchor, so check
the symbol before believing an offset.

| location | what it holds |
| --- | --- |
| `libzune/src/wireless.c` | 961 lines: `zune_wireless_pair_sync`, `zune_wireless_read_session_guid`, `zune_wireless_scan_wifi`, `zune_wireless_set_wifi_network`, `zune_wireless_pair_wifi`, `zune_wireless_disable`, `zune_wireless_status`, `zune_wireless_new_partner_guid`, and the GUID, property and vendor-operation helpers underneath them |
| `libzune/src/mtpz.c:58`, `libzune/src/mtpz.c:1765` | `mtpz_device_rsa_modulus()` and the scan that captures the device key out of the decrypted handshake |
| `libzune/src/ptp.h:99`, `libzune/src/ptp.h:183` | the vendor operation and property codes above |
| `libzune/include/zune.h:813` | the public API and the `ZuneWirelessStatus` struct |
| `src/wirelesstool.cpp` | a headless operator command - `status`, `pair`, `guid`, `scan`, `wifi`, `full`, `disable` - answering `VERDICT=hd-paired`, `hd-unpaired`, `no-wifi`, `absent` or `failed`, so the question "does this player do wireless at all" is one command rather than a GUI session |

No QML file and no application service reaches any of it yet.  There is no UI for
this feature, and `wirelesstool` is the only caller.

## A GUID as an MTP string

A GUID travels as an MTP string, and the player byte-compares what it is given, so
there is exactly one spelling that works.  `guid_to_mtp_string()`
(`libzune/src/wireless.c:72`) braces the text if it is unbraced, uppercases every
character, and then writes:

```
[character count including the terminator : 1 byte][UTF-16LE characters][00 00]
```

A braced GUID is 38 characters, so the length byte is **39** and the whole payload is
79 bytes: one length byte, 76 bytes of characters, two bytes of terminator.  Only
those three forms leave this function - braced, exactly 38 characters long, all
uppercase.  Anything else is refused before a byte is sent.

Read-back is the inverse minus the decoration: `mtp_string_to_ascii()`
(`libzune/src/wireless.c:115`) skips byte 0 (the length), takes the low byte of each
UTF-16LE character, stops at the first NUL, and strips `{` and `}`.  The unbraced
lowercase 36-character form is what the rest of the library and `wirelesstool` see.

`zune_wireless_new_partner_guid()` (`libzune/src/wireless.c:138`) mints the
partnership GUID: sixteen bytes from libgcrypt `GCRY_STRONG_RANDOM`, version nibble
set to 4 and variant bits set to RFC 4122, formatted braced and uppercase.  It is a
random name this PC chooses for itself, not anything the player issues.

## Phase 1 - claiming the partnership

`zune_wireless_pair_sync()` (`libzune/src/wireless.c:280`) is the cable half.  In
order, as the code stands:

| # | step | notes |
| --- | --- | --- |
| 1 | SetProp `0xD406` = driver tag | informational; default `Windows/10.0 Zuuned`, sent only when shorter than 60 characters.  This is what the desktop software fills with its own version string |
| 2 | SetProp `0xD402` = friendly name | only when the caller supplies one; truncated to 120 characters |
| 3 | GetPropDesc warm-up on `0xD22F`, `0xD402`, `0x5002`, `0xD231`, `0xD232`, `0xD21C`, `0xD225`, `0xD401`, `0xD22C` | the desktop software touches a property's descriptor before touching the property.  The player answers some and refuses others; the result is logged and dropped, and that is stated in the code because a silent failure here is otherwise indistinguishable from a skipped step |
| 4 | SetProp `0xD401` = the partnership GUID | the partnership |
| 5 | SetProp `0xD220` = the same GUID | the slot that says which partner may connect over the network |
| 6 | GetPropValue `0xD401` and `0xD220` | a read-back that must match, and the only proof this tree has - a refusal here fails the call |

### What phase 1 deliberately does not send

The reference pairing is much longer, and this tree does not follow it.  The
sequence in `XuneSyncLibrary` `lib/src/ZuneDevice.cpp:334-440`
(`EstablishSyncPairing`) is: SetProp `0xD406`; descriptors for `0xD22F`, `0xD402`,
`0x5002`; SetProp `0xD402`; descriptors for `0xD231`, `0xD232`, `0xD21C`, `0xD225`,
`0xD401`; descriptor for `0xD22C`; SetProp `0xD225` = a null GUID; SetProp `0xD21C` =
**a raw single byte `0x00`** - a bare byte, not an MTP string, which is the easy one to
get wrong; SetProp `0xD401` = a fresh UUIDv4; GetPropDesc then SetProp for `0xD230` =
`{7a 24 ec 12 00 00 00 00}`, `0xD229` = `{f4 00 00 00}` and `0xD22A` =
`{5c fe ff ff}`; `0x9224` GETDATA; read `0xD217` three times; `0x9217(1)`;
`0x9227` SENDDATA of four zero bytes; and `0x9218(0, 0, 5000)`.

Two details of that long form are worth recording even though this tree sends neither.

- The `0x9227` SENDDATA of four zero bytes is **answered `0x2002` (General Error) on
  purpose**.  `aftl/mtp/ptp/Session.cpp:762-773` sends it, records the `0x2002`, and
  labels the refusal expected behaviour, which is why that step in a reference
  sequence is allowed to fail and why a caller that treats it as fatal is wrong.  The
  same function in that reference is declared as receiving a response while its body
  sends a request payload; the reference contradicts itself there and this tree does
  not depend on either reading.
- `0x9217` is `SyncDeviceDB`.  In the reference it sits inside pairing; here it is not
  part of pairing at all, because telling the player its database changed and then
  asking it to sync is a different conversation from recording who its partner is, and
  `0x9217`/`0x9218` belong to the PPP-and-HTTP route this port does not take.

`zune_wireless_pair_sync()` stops at the two property writes and their read-backs.  The
removed steps are named in `libzune/src/wireless.c:357` rather than silently dropped:
clearing `0xD225`, the `0xD21C` byte, the three sync-counter writes and the
`0x9224` / `0xD217` x3 / `0x9217` / `0x9227` / `0x9218` close-out trace back to no
reference this tree can point at, and the counter values in the earlier draft matched
nothing.  **That is a reading of the references, not a measurement.**  The same comment
attributes a wedged player to the longer draft; see the section at the end of this page
about what the source comments claim.

## Phase 2 - wireless mode, the session GUID, and a network

`zune_wireless_pair_wifi()` (`libzune/src/wireless.c:733`) runs after phase 1 on the
same USB session:

| # | step | params | payload | fatal? |
| --- | --- | --- | --- | --- |
| 1 | `0x9231` SENDDATA | none | 258 zero bytes - network subsystem init | yes |
| 2 | `0x9230(1)` | 1 | none - enable wireless mode | yes |
| 3 | `0x922B` SENDDATA | `3, 1, 0` | 260 bytes, first four `02 52 c0 00`, rest zero - the subsystem gate | yes |
| 4 | SetProp `0xD220` = partner GUID | - | only when the caller passed a GUID | yes if attempted |
| 5 | GetPropValue `0xD221` = session GUID | - | only when the caller passed a buffer of at least 38 bytes | yes if attempted |
| 6 | enrol the access point | - | the 324-byte profile below | yes |
| 7 | `0x9230(1)` | 1 | none - enable again, on top of the enrolled network | yes |
| 8 | `0x922B(3, 1)` | two params | **no payload** - gate reopen | no, logged and continued |

Against the reference, steps 2 through 6 are its phase 2 verbatim in order
(`XuneSyncLibrary` `lib/src/ZuneDevice.cpp:443-478`), with one substitution: it writes
`0xD220` from a GUID it loaded from its own state file, and this tree writes the
partnership GUID from phase 1, which is the same value by construction rather than by
bookkeeping.

Steps 1 and 8 are this tree's additions and are marked as such.

- `0x9231` first.  The operation and its 258-byte zero request are documented from a
  capture (`aftl/mtp/ptp/Session.cpp:804-812`), so the shape is not invented, but the
  reference does not send it in its phase 2 - it sends it in `NetworkManager`, which is
  the PPP route.  `libzune/src/wireless.c:410` asserts that a pairing which skips it
  leaves a device that answers MTP but never announces itself on the network.  **Nobody
  here has observed that.**  It is a plausible reading of the capture's ordering and it
  is the kind of claim a first real run has to settle rather than argue about.
- `0x922B(3, 1)` last, without a payload.  That exact form is step 7 of the reference's
  disable sequence, so it is a documented call in a place the reference does not put it.
  It is non-fatal for exactly that reason.

## Enrol an access point

`zune_wireless_set_wifi_network()` (`libzune/src/wireless.c:624`) runs, in this exact
order, and with these exact consequences when the player disagrees:

| # | step | request payload | if the player refuses |
| --- | --- | --- | --- |
| 1 | `0x9225` GETDATA - the player's own Wi-Fi scan | none | enrol anyway, with the default security words |
| 2 | `0x9224` GETDATA | none | logged, continue (`libzune/src/wireless.c:697`) |
| 3 | `0x9227` SENDDATA - hand over the network profile | 324 bytes | the call fails |
| 4 | `0x9228(0)` | none | the call fails |
| 5 | `0x9228(2)`, three times | none | the call fails |
| 6 | GetPropValue `0xD217`, twice | none | stop reading, and **the call still returns success** |

The last row matters more than it looks. Those two reads are the reference's close-out
(`aftl/cli/Session.cpp:1508-1515`), and `libzune/src/wireless.c:721-727` breaks out of
the loop on the first refusal without recording an error, so a player that answered
steps 1-5 and refused the close-out is reported by this tree as `network enrolled`.
That is a deliberate choice inherited from the shape of the reference, not a
measurement of what the player needs.

### The 324-byte network profile

Written at `libzune/src/wireless.c:663-688`:

| offset | bytes | field | what this tree puts there |
| --- | --- | --- | --- |
| 0 | 4 | profile id | 1 |
| 4 | 4 | SSID length | byte length of the name, capped at 32 |
| 8 | 32 | SSID | the name, zero-padded |
| 40 | 12 | security words 0-2 | lifted from the scan entry, else `{1, 7, 4}` |
| 52 | 12 | words 3-5 | `{1, 0, 0}` |
| 64 | 4 | password length | 128, always, whatever the passphrase was |
| 68 | 128 | encrypted passphrase | the RSA ciphertext below |
| 196 | 128 | padding | zero, to 324 |

The reference describes the twenty-four bytes at offset 40 as one field, `flags[6]`:
three words of security information taken from the scan, then a word that is always 1,
then two words it fills with zero and comments as "varies, trying 0"
(`aftl/cli/Session.cpp:1441-1456`).  This tree writes the same twelve bytes at 40 and
the same twelve at 52, so the layouts agree; only the naming differs.  The total, the
196 bytes that are actually filled and the 324 that are sent, is the reference's too.

## The player's own RSA key

The passphrase is the only secret on the link, and the player will not accept it in
the clear.  The key it wants it encrypted under is the **player's own** RSA-1024
public key, and the only place that key is visible is inside the decrypted payload of
the ordinary MTPZ handshake.  That is the whole reason wireless pairing needs MTPZ
credentials, and the whole reason no Microsoft key takes part in the wireless step.

The capture, at `libzune/src/mtpz.c:1764`:

- scan the decrypted handshake message for the three marker bytes `01 00 80`;
- take the 128 bytes that follow them, and accept them only when the first has an odd
  low bit, because an RSA modulus is always odd;
- keep them **little-endian**, exactly as they sit in the payload.

The scan bound is `off + 3 + 128 <= 832`, the length of that message.  The reading of
the marker is that it is the length prefix of a key blob - `01 00` the exponent field,
`80` the 128-byte modulus size - and `aftl/mtp/mtpz/TrustedApp.cpp:323-345` scans for
the same three bytes with the same odd-low-byte test, calling them
`MARKER_EXPONENT_LO`, `MARKER_EXPONENT_HI` and `MARKER_SIZE`.  Two implementations
agreeing on a shape found by scanning rather than by offset is the strongest evidence
available here, and it is still not a capture of our own.

`mtpz_device_rsa_modulus()` (`libzune/src/mtpz.c:58`, declared
`libzune/src/mtpz.h:41`) hands the bytes out.  Its argument may be NULL, which asks
only whether a key is present without disturbing it, and that is exactly what the
read-only `zune_wireless_status()` does (`libzune/src/wireless.c:486`) to fill
`has_device_key`.  If the handshake produced no modulus the capture logs so at the
time, and `zune_wireless_set_wifi_network()` refuses with an instruction to connect
over USB with MTPZ credentials present (`libzune/src/wireless.c:646-651`).

To encrypt, `rsa1024_encrypt()` (`libzune/src/wireless.c:551`) does this:

| step | what this tree does |
| --- | --- |
| modulus | reverse the 128 little-endian bytes into a big-endian MPI |
| exponent | 65537, scanned as the hex text `10001` |
| plaintext | the passphrase as UTF-16LE with **no** null terminator |
| padding | PKCS#1 v1.5 block type 2: `00 02 <non-zero random PS> 00 <message>` |
| operation | `gcry_mpi_powm(c, m, e, n)` |
| output | 128 bytes big-endian, right-aligned when `gcry_mpi_print` returns fewer (`libzune/src/wireless.c:602-611`) |

`aftl/mtp/mtpz/TrustedApp.cpp:520-582` performs the same construction through
OpenSSL: reverse at `:538`, exponent `RSA_EXPONENT` = 65537
(`aftl/mtp/mtpz/MTPZConstants.h:12`, with `RSA_MODULUS_SIZE` = 128 at `:11`), one
`char,0` pair per input byte and no terminator at `:559-562`, `RSA_PKCS1_PADDING`, and
a 128-byte output check at `:580`.

Three limits are worth knowing before a user finds one of them.

- The padding check `msg_len + 11 > 128` (`libzune/src/wireless.c:596`) caps the
  plaintext at 117 bytes, and since every character costs two, the real ceiling is
  **58 characters**.  `zune_wireless_set_wifi_network()` now refuses anything longer
  (`libzune/src/wireless.c:718`) instead of capping at 60.  It used to cap: a 59- or
  60-character passphrase became 118 or 120 bytes and was refused by the padding check
  anyway, but a cap that low would have *enrolled* a shortened key, and that reaches the
  user as a wrong passphrase on a screen that says the network was saved.  A
  63-character WPA2 passphrase - the longest the standard spells in characters - cannot
  be enrolled by this code at all, which is a limit worth telling a user before they try.
- The high byte of every UTF-16 character is a literal zero
  (`libzune/src/wireless.c:723-725`), so a passphrase containing a non-ASCII character
  is not encoded as UTF-16, it is split into separate BMP characters and the player
  will decrypt something other than what was typed.  The reference has the identical
  flaw at `aftl/mtp/mtpz/TrustedApp.cpp:559-562`, which is an argument for using an
  ASCII passphrase, not an argument that the code is right.
- Nobody has watched a player accept or reject one of these ciphertexts.  What is
  recorded above is two independent implementations agreeing on the arithmetic.

## Turning wireless sync off

`zune_wireless_disable()` (`libzune/src/wireless.c:785`) is the reference's
`ClearWirelessPairing()` (`aftl/mtp/ptp/Session.cpp:698-716`) - a seven-step unwinding
rather than one call, in which **no step sends a payload**:

| # | operation | params | if the player refuses |
| --- | --- | --- | --- |
| 1 | `0x922F` | none | logged, continue |
| 2 | `0x9230(2)` | one | **the call fails** - the only fatal step |
| 3 | `0x922B(3, 2)` | two | logged, continue |
| 4 | `0x922D(3, 3)` | two | logged, continue |
| 5 | `0x9215` | none | logged, continue |
| 6 | `0x9230(1)` | one | logged, with a warning that the player may need pairing again |
| 7 | `0x922B(3, 1)` | two | logged, continue |

The order and the parameters match the reference step for step.  The shape of the last
two is the detail most likely to be broken by whoever edits this next, so it gets its
own paragraph.

**`0x922B` exists in two forms and mixing them up is the easiest way to get this
wrong.**  The enrolment form is three parameters **plus a 260-byte request**
(`aftl/mtp/ptp/Session.cpp:813-827`, whose capture note names a 260-byte data phase and
the `02 52 c0 00` opener).  The disable form is two parameters and **no payload at
all** (`aftl/mtp/ptp/Session.cpp:711` and `:715`).  Both forms live in the same
reference file, minutes of reading apart, and they are not interchangeable.  This tree
sends each in its own place: with the payload at `libzune/src/wireless.c:753`, and
without one at `libzune/src/wireless.c:778`, `:814` and `:823`.

Two honest differences from the reference this was transcribed from.

- Step 1 is issued as a **GETDATA** (`libzune/src/wireless.c:806`).  The reference
  issues `0x922F` with no data phase at `aftl/mtp/ptp/Session.cpp:709`, while its own
  wrapper at `aftl/mtp/ptp/Session.cpp:793-798` comments that the operation "RECEIVES
  data from the device, not sends it" - about a call that also declares a response
  without asking for a data phase.  `XuneSyncLibrary` reads a reply and expects 1,036
  bytes, logging when the size is anything else
  (`XuneSyncLibrary` `lib/src/ZuneDevice.cpp:1107-1114`).  So "seven commands with no
  payload anywhere" is exactly true of every **request** in this sequence; ours merely
  also asks for the answer.  Neither reading has been tried on a player.
- Only step 2 can fail the call (`libzune/src/wireless.c:810`).  A player that refused
  six of the seven steps is reported as `wireless sync off`.  That posture is inherited
  from the reference, which runs all seven and throws on none of them individually, and
  it is written here because a return code that means "I asked" rather than "it is off"
  is exactly the thing a user interface will otherwise build on.

The reference also carries the one-call version, `DisableWirelessSync()`, which is
`0x9230(2)` alone and is commented "verified from Windows capture"
(`aftl/mtp/ptp/Session.cpp:695-696`).  This tree implements the seven-step unwinding
and not that, so the shorter form is unrepresented here rather than disbelieved.

## What the player itself needs, before any of this matters

Everything above is the host side of the arrangement. The Zune HD has requirements of
its own, and a sync that fails one of them never reaches a byte of the protocol on this
page. **This section is documented behaviour of the original Microsoft client and
constants read out of the references; this repository has tested none of it against a
player.** It is written down anyway because these are the conditions a future interface
has to explain to a user, and because most of them are invisible from the PC.

- **Radios.** 802.11b/g on 2.4 GHz. There is no 5 GHz and no 802.11n, so a modern
  band-only access point is not a slow network, it is no network.
- **Security.** Open, WEP, or WPA/WPA2 with TKIP or AES; the player's own picker also
  offers a combined AES+TKIP automatic choice. There is no 802.1X and no enterprise
  profile, so WPA2-Enterprise is not a hard case - it is an impossible one.
- **Addressing.** DHCP. The client takes an address and offers no static one, so an
  access-point reservation is the closest a user gets to a fixed address for the player.
- **Direction.** Push only. The PC sends to the player. Nothing arrives back over the
  air, which is a real difference from the cable and has to be visible in any interface
  built on top of this.
- **When it fires.** The player looks for its partner while it is charging - on AC or in
  a dock - and after roughly a minute idle on battery, above roughly 10 % charge. A
  player near empty does not sync, and a player sitting on a desk unplugged may wait a
  long time before it tries.
- **The radio is down while the cable is in.** On USB the player turns Wi-Fi off, so a
  wireless attempt made while the device is plugged in is not evidence about this
  protocol one way or the other.
- **On the player.** Settings, then Wireless, then Sync. That is where the partner name
  appears once `0xD401` has been written, and where the user actually switches it on.
- **On the PC, in the original client.** Microsoft's software reports `C00D07C9`,
  `C00D07CA` or `C00D07CB` when the Windows Firewall blocks it, and it depends on the
  *SSDP Discovery* and *UPnP Device Host* services running. Both facts are about
  Microsoft's client and not about this one, but they are the two most likely reasons a
  historically correct setup does not sync. Any firewall rule this application ever needs
  has to be one explicit, elevated, opt-in action tied to pairing - never a silent
  change to the machine, and never a per-network-profile surprise.
- **Never identify the player by its router entry or its broadcast name.** The SSDP `USN`
  carries the device UUID, whose last 12 hex digits are the MAC address, and the
  friendly name behind `0xD402` is a string the user edits. The GUIDs are the identity.
  A MAC tail is a personal identifier and must not reach a log; that is the same reason
  no `scan` output is committed to this tree.

## PTP/IP and discovery - planned, not implemented

**There is no code for either of these in this tree.**  `libzune/src/wireless.c:14-16`
says so in the file where a reader would look first, and nothing else in the
repository opens a socket.  Everything below is what the references document, written
down so that whoever builds the transport does not have to re-derive it.  None of it
has been tested against a Zune by anybody here.

The transport is standard PTP/IP, ISO 15740: two plain TCP connections to port 15740 -
the port number is the ISO standard number - one carrying commands and data and driven
by the host, the other carrying events and driven by the device.  No TLS.  The only
credential is the session GUID from `0xD221`.

Every packet begins the same way, and the header is **eight bytes**:

```
[length : 4][type : 4]  then length - 8 bytes of type-specific payload
```

The transaction id lives **inside** that payload, not in the header.  The types, as
`XuneSyncLibrary` names them at `lib/src/ptpip_client.h:18-33`, with the names the
gPhoto PTP/IP write-up uses for the same numbers:

| value | `XuneSyncLibrary` | gPhoto |
| --- | --- | --- |
| 1 | `INIT_COMMAND_REQUEST` | Init_Command_Request |
| 2 | `INIT_COMMAND_ACK` | Init_Command_Ack |
| 3 | `INIT_EVENT_REQUEST` | Init_Event_Request |
| 4 | `INIT_EVENT_ACK` | Init_Event_Ack |
| 5 | `INIT_FAIL` | Init_Fail |
| 6 | `OPERATION_REQUEST` | Cmd_Request |
| 7 | `OPERATION_RESPONSE` | Cmd_Response |
| 8 | `EVENT` | Event |
| 9 | `START_DATA` | Start_Data_Packet |
| 10 | `DATA` | Data_Packet |
| 11 | `CANCEL_TRANSACTION` | Cancel_Transaction |
| 12 | `END_DATA` | End_Data_Packet |
| 13 | `PROBE_REQUEST` | Ping |
| 14 | `PROBE_RESPONSE` | Pong |

`PTPIP_PORT` is 15740 at `lib/src/ptpip_client.h:80`.

### The init packet

`INIT_COMMAND_REQUEST` (`XuneSyncLibrary` `lib/src/ptpip_client.cpp:346-383`):

```
[length = 8 + 16 + name bytes + 4][type = 1]
[16-byte session GUID][host name UTF-16LE with terminator][0x00010000]
```

The 16 GUID bytes are the hex digits of the 36-character `0xD221` text taken two at a
time in textual order, after stripping `-`, `{` and `}`
(`lib/src/ptpip_client.cpp:349-358`).  **There is no mixed-endian byte swapping.**
This deserves the emphasis because GUIDs elsewhere in MTP, and in the Windows registry
generally, store their first three groups byte-swapped; a first implementation that
applies the usual rule here will be refused by a device that is behaving perfectly
correctly, and the failure will look like a wrong passphrase.

gPhoto's write-up lists no trailing version word in this payload, where the reference
appends `0x00010000` (`lib/src/ptpip_client.cpp:371`).  Unresolved; a first capture
settles it.

### The operation request

`OPERATION_REQUEST` (`XuneSyncLibrary` `lib/src/ptpip_client.cpp:397-418`) writes
`length = 18 + 4 * number of parameters`, then:

```
[length : 4][type = 6][4-byte field : 0][opcode : 2][transaction id : 4][parameters]
```

Two things a draft of this page asserted that neither reference supports, corrected
here because the wrong version of both would be load-bearing.  The generic header is
**eight** bytes - `[length][type]` - and not `[length][type][transaction id]`; and the
reserved field before the opcode is **four bytes**, not five.  gPhoto agrees on the
shape, describing the type 6 payload as "4 byte unknown. set to 1 currently. / 2 byte
PTP command code / 4 byte Transaction ID / n x 4 byte PTP command arguments", and
disagrees on the value: it sends 1 where the reference hardcodes 0
(`lib/src/ptpip_client.cpp:400`).  That disagreement is the first thing a capture has
to settle, and a zero down a path the reference has never carried traffic on is
consistent with what its own README says about that code.

The response (`type 7`) is `[response code : 2][transaction id : 4][arguments]`, and a
transfer with a body runs `START_DATA` (transaction id, total size), then `DATA`, then
`END_DATA`.

### Finding the player without knowing its address

The reference claims the player announces itself, so a host does not need to be told where
it is.  **One real Zune HD refused to support that, and the design has to live with it**:
see "The first network sitting (2026-11-18)".  An implementation that waits for an
announcement waits forever on this player, so the address comes from the user.  What the
reference recorded, in `XuneSyncLibrary` `lib/src/ssdp_discovery.h:61-64` and
`lib/src/ssdp_discovery.cpp`:

- a `NOTIFY` to `239.255.255.250:1900`, accepted only when it contains `NOTIFY *`
  (`lib/src/ssdp_discovery.cpp:109-113`) and the server token `Zune/2.0`
  (`lib/src/ssdp_discovery.cpp:116-118`);
- the UUID is read from the `NT` or `USN` header after the `uuid:` prefix
  (`lib/src/ssdp_discovery.cpp:149-154`), and the **last 12 hex digits of that UUID are
  the player's MAC address** (`lib/src/ssdp_discovery.cpp:80-102`) - the device's
  identity and its network address in one field, which is how a host can tell two
  players apart;
- a `LOCATION` URL for the device description document, from whose port component the
  reference reads an `http_port` (`lib/src/ssdp_discovery.cpp:155-157`, `:172-183`);
- a device is dropped after `DEVICE_TIMEOUT = 600` seconds, whose own comment reads
  "10 minutes (2x cache-control max-age)" (`lib/src/ssdp_discovery.h:64`).  **The
  roughly 300-second cache age is an inference from that comment, not a value either
  reference captured**, so treat it as the reason someone chose 600 and not as a
  documented number.

The references' own candour is why this whole section is headed "planned".
`XuneSyncLibrary` logs "Wireless connection is not yet implemented." from
`ConnectWireless()` and returns false (`lib/src/ZuneDevice.cpp:183-186`), and its
README lists "Wireless sync (PTP/IP) is implemented but not actively tested" and
"Two-way wireless sync is not implemented" (`README.md:353`, `:357`).  The one project
that has a PTP/IP client has never carried a sync over it, and the other never wrote
one.

## The other route: PPP carried inside `0x922C` and `0x922D`

`XuneSyncLibrary` has a second way to put a player on a network, and it is not the
network sync this feature is about.  It brings up a PPP link - LCP and IPCP handshake
bytes - carried inside the vendor operations `0x922C` (send) and `0x922D` (poll), which
makes the host a link endpoint instead of a peer on the access point.  The player then
dials an HTTP server the host runs over that link and asks it for artist images and
metadata; the README describes that arrangement at `README.md:15`, `:18` and
`:310-313`.  Its init sequence (`lib/src/NetworkManager.cpp:48-89`) is `0x9231` ->
`0x9217(1)` -> `0x9218(0,0,5000)` -> `0x9217(1)` -> `0x9219(0,0,5000)` -> `0x922F` ->
`0x922B(3,1,0)` -> `0x9230(1)`, after which the device sends `CLIENT` through `0x922D`
and the host answers `CLIENTSERVER` (`lib/src/NetworkManager.cpp:91-92`, `:216`).

This project is not taking that route, for two reasons.

- It is an **alternative transport for a USB-served HTTP metadata API**, not a sync
  over an access point.  It answers "how does a docked player fetch artwork", which is a
  different question from "how does a player sync without a cable", and the second one
  is what this feature exists for.  It is also the reason `0x9217`, `0x9218` and
  `0x9219` are in the vocabulary at all - they belong to this route, which is a further
  reason `zune_wireless_pair_sync()` refuses to send the first two.
- It costs a PPP, IP and TCP stack plus a host-side HTTP server, none of which the
  Wi-Fi route needs.

The one place this tree touches that machinery is `0x9231`, whose 258-byte zero request
is documented from the capture behind this route.  Its placement at the head of phase 2
here is this tree's own inference, as marked in the phase 2 section above.

## Status

**Implemented in this tree**: `libzune/src/wireless.c` - pairing, session-GUID read,
Wi-Fi scan, network enrolment, disable; plus the `mtpz_device_rsa_modulus()` extraction
in `libzune/src/mtpz.c` and the new vendor op/property codes in `libzune/src/ptp.h`.

| claim | state at this revision |
| --- | --- |
| source exists | yes - `libzune/src/wireless.c`, 961 lines, with `libzune/src/mtpz.c:58`, `libzune/src/ptp.h:99`, `libzune/src/ptp.h:183`, `libzune/include/zune.h:836` and `src/wirelesstool.cpp`, 220 lines |
| named by the build files | yes - `CMakeLists.txt:301`, the `wirelesstool` target at `CMakeLists.txt:648-653`, `libzune/Makefile:70` |
| compiles | yes, measured. A full `cmake --build build --clean-first` under MSVC finished `EXITCODE=0` on 2026-11-18 with the wireless source and its tool in the tree. The warnings it printed belong to `libzune/src/transcode.c` and `transcode/transcodetool.c`, which this feature does not touch |
| runs | the read half has been run on a Zune HD; the write half has never been run. See below |
| reachable from the interface | no. There is no QML file and no application service for this feature; `wirelesstool` is the only caller |

### What has actually been asked of a player

One Zune HD, on the cable, through `wirelesstool`, on Windows, during development.
What it answered:

| command | what it puts on the wire | outcome |
| --- | --- | --- |
| `status` | `GETPROP` on `0xD401`, `0xD220`, `0xD221`, `0xD225`, `0xD21C`, `0xD217` | all six answered. A partner GUID and a session GUID came back and are withheld here: they identify this partnership and this machine. The MTPZ authentication ahead of those reads also captured the player's RSA public key, which is the first real Pavo to confirm the shape `mtpz_device_rsa_modulus()` scans for, and the tool's verdict was `hd-paired` |
| `guid` | `GETPROP` on `0xD221` | answered, with the same value `status` reported |
| `scan` | vendor `0x9225`, no params | answered with a blob that lists the access points in range. It is not repeated here, because an SSID list is a map of where the user lives and works |
| `pair` | `SETPROP` on `0xD401` and `0xD220` | **never run.** The player used for the reads went into a state where every bulk OUT timed out before `pair` was attempted, and it has not been available since |
| `wifi`, `full`, `disable` | `0x922B`, `0x9227`, `0x9228`, `0x9231`; for `disable`, the seven-step `0x922F` / `0x9230` / `0x922B` / `0x922D` / `0x9215` unwinding | **never run** |

Three things that table does not license anyone to say.

- The reads prove those properties **exist** on this model. They do not prove a **write**
  is accepted, which is the single thing phase 1 rests on.
- The `scan` reply proves the operation answers. It does not prove `scan_security_flags()`
  parses it: the three security words are located by an inference about where a name sits
  inside a record, so a wrong offset is quite capable of reporting a network as WPA.
- Nothing here has ever been switched **on**. This software has not enabled the player's
  wireless sync. When the radio and the 15740 listener did come up, a person pressed Sync
  on the player and a script outside this repository watched the result, so the PTP/IP half
  of the design now has evidence about **a listener** and still no evidence at all about
  **a protocol**.

No log, pcap, or capture file of any of it lives in the repository: `wirelesstool`
prints to stdout, and the values it prints are exactly the kind that must not be
committed. Whoever runs it next should treat the table above as a question and not as
an answer.

Four consequences, stated as rules rather than as apologies.

- **No hardware evidence crosses between platforms.** That is this project's standing
  policy and it applies here as much as anywhere: a result belongs on this page and, for
  a Windows result, in [WINDOWS_PORT.md](WINDOWS_PORT.md), and may never be inherited
  from a Linux run or carried the other way. The reads above are Windows observations
  with no Linux counterpart, and no Linux behaviour may be inferred from them.
- **Wireless sync is a Zune HD capability and nothing else.** No other Zune model has
  Wi-Fi, so every property and operation on this page is addressed at that one player.
  A model without Wi-Fi is expected to refuse `0xD217`, which is the distinction
  `zune_wireless_status()` exists to make - and even that expectation is unmeasured.
  `src/wirelesstool.cpp` answers `no-wifi` for it, and that verdict has still not been
  printed: the one player used so far answered every probe.
- **This page supersedes the out-of-scope entry in
  [ZUNE_CLIENT_FEATURES.md](ZUNE_CLIENT_FEATURES.md).** That list ruled out
  "wireless sync between two players (2.2)", on the ground that it was one client talking
  to a Microsoft endpoint or to another client inside the same social system. What is
  described here is host-to-player sync over the user's own access point, which needs
  neither, so the earlier ruling does not reach it. The bullet has been amended to say so
  in that file rather than left to be reconciled by memory.
- **The reference clones stay outside the repository.** They are read-only local audit
  material, not vendored and not submodules, and no licence-compliance figure from
  [UPSTREAMS.md](../UPSTREAMS.md) should be re-derived from memory.
## What the source used to claim, and what it claims now

This feature arrived with comments that said more than the evidence allows. They were
edited in the same sitting that wrote the Status table above, so the table below is
**history, not a to-do list** - it exists because an older checkout really does contain
the wrong sentences, and because the pattern is worth naming: every one of them took
something a reference *recorded* and stated it as something *this tree had seen*.

| where | it used to say | what it says now |
| --- | --- | --- |
| `libzune/src/wireless.c` header | the vendor operations were "decoded from the Zune desktop software's own traffic" | attributed to the two references, which describe a capture nobody here has. The same comment now names the one thing this tree has observed on a cable, so the two kinds of statement cannot be confused |
| `libzune/src/wireless.c`, `0x922B` | the 260-byte payload was "Verified against the desktop software's traffic", and a shorter one makes the device refuse | both halves named as `aftl`'s observation, with `0x922B` never having been sent from this file |
| `libzune/src/wireless.c`, phase 1 | "Both writes are accepted by a real Zune HD" | deleted. The writes have never been attempted, which is why the `pair` row above reads *never run* |
| `libzune/src/wireless.c`, and the `0x9217` note in `libzune/src/ptp.h` | an earlier draft of this file stopped a Zune HD answering its bulk OUT endpoint | correlation. The identical symptom was measured on a build containing no wireless code at all, where a power cycle was the cure - see "The seventh pass: a reset that reported success (2026-10-21)" in [WINDOWS_PORT.md](WINDOWS_PORT.md) |
| `libzune/src/wireless.c`, `0x9231` | skipping it leaves a device that answers MTP and never announces itself | labelled an inference from the reference's ordering. Neither half has been observed, in either direction |
| `libzune/include/zune.h`, `zune_wireless_scan_wifi()` | blocks until the device answers or "the poll budget runs out" | there is no poll budget. One `GETDATA`, then return whatever the PTP layer's timeout allowed |
| `libzune/include/zune.h`, `zune_wireless_pair_wifi()` | "One-call pairing: sync partnership, then network enrolment" | it never wrote the partnership, and now no longer pretends to: `zune_wireless_pair_wifi()` reads `0xD401` first and refuses phase 2 outright when the player names no matching partner, before it touches the network subsystem |
| `libzune/include/zune.h`, `session_guid_out` | the GUID "the player will announce over SSDP" | the wrong value. SSDP advertises the player's device UUID, whose last 12 hex digits are its MAC address (`XuneSyncLibrary` `lib/src/ssdp_discovery.cpp:80-102`). `0xD221` is presented in the PTP/IP INIT packet and is never advertised; the two GUIDs have different jobs |
| the licence lines in `wireless.c`, `libzune/src/ptp.h`, `zune.h` | three different paraphrases of the upstream licences | one wording, matching [UPSTREAMS.md](../UPSTREAMS.md): XuneSyncLibrary is LGPL-2.1, the vendored `aftl` fork is LGPL-2.1-or-later |

Three of those corrections changed behaviour rather than prose, and they are the part a
reviewer should read.

- `zune_wireless_set_wifi_network()` **refuses** a passphrase longer than 58 characters
  instead of truncating it at 60. The envelope is PKCS#1 v1.5 over UTF-16, so
  `msg + 11 <= 128`. Truncation was the worse of the two options: it enrols a network the
  player can never join, and reports success doing it.
- The two closing `0xD217` reads in enrolment are **fatal** and say that enrolment is not
  confirmed. They used to `break` and return success, which is exactly how a player that
  rejected the profile would have been reported as enrolled.
- `zune_wireless_disable()` sends `0x922F` **bare**, which is how `ClearWirelessPairing()`
  in the reference issues it, and counts the refusals among the six steps beside the mode
  change instead of printing a clean `off` over the top of them. It does **not** clear
  `0xD401` or `0xD220`: after `disable` the player still holds the partnership, which
  matches the reference and is worth knowing before anyone calls it a factory reset.

## The first network sitting (2026-11-18)

One Pavo Zune HD, one access point, Windows as the host, and the player's own Sync switch
worked by hand.  **Nothing in this repository took part in any of it**: this tree opens no
socket, so every packet below was written by a throwaway script that lives outside the
repository - which is also where its output stays, because what it prints is exactly the
material that must not be committed.  No address, GUID, serial, MAC, network name or
friendly name appears in this section.

Five measurements, and the order is the argument.

1. **The player's RSA key came off the cable.** The `status` run that read the six vendor
   properties also reported that `mtpz_device_rsa_modulus()` had captured the player's
   public key.  It is the first time that extraction has touched a real Pavo, and it
   confirms the shape it scans for - a `01 00 80` marker, a public exponent, an odd low
   byte - which until this sitting was an inference from the reference and nothing more.
2. **A partnership already existed.** `0xD401` named a partner and `0xD220` named a
   session, and the verdict was `hd-paired`, on a player to which `pair`, `wifi`, `full`
   and `disable` have never been sent.  A Zune that once sat on a machine running the
   Microsoft software therefore arrives already holding the credential this design wants,
   and a partner GUID on screen is not evidence that anything here wrote it.
3. **With the cable gone the player was a host.** Associated, on power, screen awake: it
   answered ARP, it answered ICMP with TTL 128, and TCP 15740 completed a handshake.  That
   is the listener this design assumes, seen for the first time on this platform.
4. **One init, no answer.** An `INIT_COMMAND_REQUEST` was written to that connection in
   exactly the spelling this page documents - sixteen GUID bytes in textual order, host
   name UTF-16LE with its terminator, trailing `0x00010000`.  The connection was accepted
   and nothing came back within six seconds.  That is **one** attempt.  It does not show
   the version word wrong, or the GUID order wrong, or the player refusing third-party
   hosts: the variants that would separate those answers were ready but never sent, because
   by then the port had closed.
5. **The player never announced itself.** A listener joined `239.255.255.250:1900` for 900
   seconds while the player was associated, on the same LAN and in the same window as SSDP
   traffic from the access points - which is the control that makes the silence mean
   something.  Not one `NOTIFY` from a `Zune/2.0` server arrived, and an active `M-SEARCH`
   never saw one either.  Discovery as the references describe it is unusable against this
   player in this state, so **the address comes from the user**.

Then the control that explains the rest.

**The player's Sync switch is the listener's power button.**  Turning Sync off closed 15740
within seconds and took the player out of the ARP table in the same moment.  Pressing Sync
again ran `connecting to network` and, once, reached `connected` - and that `connected` was
an association with no listener behind it: 15740 refused, ICMP silent.  Letting the screen
go dark closed the port again.  In the most recent sitting the same gesture got worse in a
way the user can reproduce at will: tapping Wireless connects and holds, and tapping Sync
drops the association, fails to rejoin, and ends with the player unable to obtain a lease.

Three consequences, in the order they change the design.

- **Discovery is out, an address is in.**  Settings takes an IP, because a host that waits
  for an announcement waits forever.
- **The window is short and it is not free.**  The listener is open only while the player is
  associated, awake and syncing, and pressing Sync is the very moment a legacy 802.11b/g
  client is likeliest to be thrown off a modern access point.  A client has to be waiting
  already or arrive inside about a second.  Every throwaway probe that opens a connection,
  waits six seconds and gives up loses the race it was entered to win, which is the
  strongest argument on this page for a real transport in `libzune/src/` instead of scripts.
- **The dropped association is not this software's doing.**  No process here holds a socket,
  sends a packet, or can deauthenticate a station, so whatever ended that association is
  between the player's radio and the access point.  Microsoft's own guidance for wireless
  sync assumes an access point the user configures: the access point must be the DHCP server
  with DHCP enabled, an address reserved for the player is recommended, and where MAC
  filtering is in use the player's MAC has to be on the list - which the player itself shows
  at Settings, Wireless, Information.  **It associated and answered on this network earlier
  the same evening**, so the access point is capable of it; what is left is association
  reliability, not a closed door.

What this sitting does not license.  No sync has been negotiated, not one object has moved
over the network, the write half of the vendor API is still unrun, and one INIT with no
reply is a closed loop and an empty read, not a protocol result.  It is a Windows
observation about one Pavo on one access point: it may not be carried to Linux, to another
model, or to every network.

### What the access point has to do

The association failure recorded above is outside this software, and the fix belongs to the
network.  It is written down here because whoever sits down with a Zune HD next will meet it
long before they meet any byte of this protocol.  The first four bullets are Microsoft's
published requirements for wireless sync between the Zune software and a player; the rest is
what an 802.11b/g station from 2006 needs from an access point of this decade, and none of
that has been measured by this tree - it is guidance, not evidence.

- The access point must be the DHCP server, with DHCP enabled.  A player that cannot get a
  lease cannot be dialled, which is precisely where the user now stands.
- Reserve an address for the player.  The player shows its own MAC at Settings, Wireless,
  Information, and where MAC filtering is in use that address has to be permitted.
- Keep it on 2.4 GHz.  A Zune HD is 802.11b/g, and 802.11b/g is the first thing a modern
  access point turns off or quietly de-prioritises.
- Give the old client nothing modern: WPA2-PSK with AES and **not** WPA3 or a mixed
  WPA2/WPA3 mode; protected management frames (802.11w, PMF) off or optional, never
  required; mode set to 802.11b/g/n mixed rather than n-only or ax-only; 20 MHz channel
  width; a fixed channel; band steering and merged dual-band single-SSID behaviour off; a
  separate SSID for the player if the access point will carve one out.
- Leave the two ends in sight of each other: no AP or client isolation, and nothing on the
  router blocking TCP 15740.

The honest frame for all of it is that this player associated, held a lease, answered ARP and
ICMP and opened its listener on this access point earlier the same evening.  The access point
is therefore capable, and what fails is the re-association that pressing Sync performs - the
exact moment a legacy station tends to get deauthenticated.  A dedicated b/g/n WPA2-only
SSID is the usual cure, and until the association holds there is nothing here worth
connecting to.

## What a first run has to settle

Ordered by how much of the design each answer moves.  The `status`, `guid` and `scan` rows
have been asked and answered - twice on the cable as of 2026-11-18 - and the network sitting
of the same date moved items 1, 8 and 10 without closing any of them.  Everything below that
still needs a player on the end of a cable needs the power cycle that precedes every cable
session.

1. Do `0xD401` and `0xD220` accept a write and give the same value back, in the spelling
   `guid_to_mtp_string()` produces? Phase 1 rests entirely on that read-back, and it is
   the one thing `status` has not yet been able to answer, because nothing has written
   them. The network sitting of 2026-11-18 sharpened this rather than answering it: the
   player named a partner and a session before any write had ever been attempted, so
   *presence* proves nothing - the Microsoft software may have written it years ago. The
   test is a **change**: `pair` generates a GUID, so the partner value `status` prints
   afterwards must differ from the one it printed before and must equal the new one.
2. Whether the short phase 1 is sufficient. The reference also clears `0xD225`, writes
   the bare `0xD21C` byte, writes three sync counters and closes with `0x9224`,
   `0xD217` x3, `0x9217`, `0x9227` and `0x9218`; this tree sends none of that. A player
   that accepts the two writes and then never announces itself is the evidence that the
   long form was doing something real.
3. Whether `0x9231` and the closing `0x922B(3, 1)` are necessary, harmless, or harmful.
   Both are this tree's additions to the reference's phase 2, and the claim that a
   pairing without `0x9231` leaves a silent device has never been tested in either
   direction.
4. The shape of a scan-blob entry. `scan_security_flags()`
   (`libzune/src/wireless.c:556`) assumes the three security words sit 24 bytes before
   the SSID, which is an inference from where a name lands inside a record, and the
   reference infers the same rather than knowing. A wrong guess does not fail loudly: it
   enrols with the `{1, 7, 4}` default, so an open or WEP network will be refused in a way
   that looks exactly like a wrong passphrase. `scan` has answered once; the blob has
   never been laid out against a known network list.
5. Whether the closing `0xD217` reads matter now that they are fatal. They are the last
   thing standing between a rejected profile and an "enrolled" verdict, so the first
   `wifi` run either confirms them or produces a false negative that looks like a wrong
   passphrase.
6. Passphrases in practice: ASCII only, 58 characters maximum enforced here, and what a
   player would have done with the 63-character one this code now refuses to send.
7. Whether the player's wireless mode ever comes up at all from this software - which is
   the question every item above is circling, and which cannot be answered from the PC
   alone: it needs someone to look at Settings, Wireless, Sync on the player.
8. Whether the session GUID really is laid into a PTP/IP init packet in plain textual
   order. One attempt in the spelling this page documents drew no answer inside six
   seconds, which is one accepted connection and an empty read, not a result: the variant
   battery - no trailing version word, empty host name, no name field at all, mixed endian
   as a control, `PROBE_REQUEST` for liveness - was written but never sent, because the
   port closed first. Only a transport that can sit and wait on an address can ask this.
9. Whether the trailing `0x00010000` belongs in the init payload at all, and whether the
   reserved field before the opcode is `0` or `1`. Both are disagreements between the two
   references, both are unmeasured, and the first is now the leading suspect for a listener
   that accepts a connection and says nothing - which is precisely why it has to be settled
   by A/B against a live player rather than argued from a document again.
10. Whether 15740 is open while the player is *not* syncing. Every sighting this tree has
    sits inside a syncing window, and turning Sync off closed the port within seconds. If
    the port really is Sync-gated then a wireless sync this application starts on its own is
    impossible by design and the user has to press a button on the player - a product
    decision, so it needs a measurement and not an assumption.
11. Whether the player will hold an association on a network configured the way Microsoft's
    own wireless-sync guidance requires: the access point as DHCP server with DHCP enabled,
    an address reserved for the player, and the player's MAC permitted wherever filtering is
    on. The most recent sitting ended with the player unable to obtain a lease after pressing
    Sync, and until it holds an association there is nothing to connect to.

Until those have answers the summary stays the one at the top of this page. The read half
has been asked twice and answered twice; the write half exists as careful transcription and
the player has never been asked; and the network half has been touched once, by a script,
with the port shutting before the interesting half of the question could be put.

