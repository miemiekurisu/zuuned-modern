# What Settings contains, and what it should contain

Dated 2026-10-24.  Settings grew one card per session for many sessions, and after the
screen audit ([UI_WIRING_AUDIT.md](UI_WIRING_AUDIT.md)) the blunt question came back
pointed at this one page: of the controls on it, which ones change what the app does,
which ones only report, which stored key has no control at all, and which control has
no reader.  And separately - what *should* be here, since this app is a port of a client
whose own Options dialog did things this one does not.

The first four sections are measured.  Section "What it should contain" is judgment, and
says so line by line, because nothing in this repository records the original client's
Options dialog and the public web was unreachable from the machine that wrote it.

## Where a setting lives

Two stores, both plain ini, no registry:

- `AppSettings` (`src/AppSettings.h`) wraps a default-constructed `QSettings`, so
  organization `Zuuned` / application `Zuuned` (`src/main.cpp:60-61`) and the file is
  `%APPDATA%\Zuuned\Zuuned.ini` on Windows, `$HOME/.config/Zuuned/Zuuned.conf` on Linux.
  On Windows this replaced an older `HKCU\Software\Zuuned\Zuuned` key, which is imported
  once.  `ZUUNED_PROFILE_DIR` moves the ini with the rest of the profile.
- `Prefs` (`qml/Prefs.qml`) is a QML `Settings` object holding three purely-visual keys:
  `playerStyle`, `backdrop`, `displayFont`.

Because both sides use the default `QSettings`, a C++ reader and the Settings page agree
by construction: `SyncEngine` asks for `syncAudioFormat` with a string literal
(`src/sync/SyncEngine.cpp:275`) and gets exactly what `AppSettings.syncAudioFormat`
wrote.  That is why the audio policy is read once, in `readAudioSyncPolicy()`, rather
than in three places.

## Every stored key, and what reads it

33 keys in `AppSettings`, and 3 in `Prefs`, two of which turn out to be unused aliases.  "Reader" names the place that consumes it, not
the setter.  A key is **wired** when a control writes it *and* something reads it.

| key | card | control | reader | status |
| --- | --- | --- | --- | --- |
| `onboardingComplete` | (first run) | Finish | `qml/OnboardingView.qml` | wired |
| `playerVolume` | - | transport | `qml/Main.qml` | wired, no Settings row by design |
| `headerFont` | Display font | Seg | `qml/Theme.qml` | wired |
| `backgroundImagePath` | Backdrop | picker / Clear | `qml/EnvironmentBackground.qml` | wired |
| `backdropPreset` | Backdrop | Seg | `qml/EnvironmentBackground.qml` | wired |
| `backgroundOpacity` | Backdrop | slider | `qml/EnvironmentBackground.qml` | wired |
| `backgroundBlurAmount` | Backdrop | slider | `qml/EnvironmentBackground.qml` | wired |
| `ambientSpray` | Ambient glow | switch | `qml/Main.qml` | wired |
| `letterBreaks` | Letter breaks | switch | `qml/GalleryView.qml` | wired |
| `artworkStyle` | Artwork print | ArtworkStyleSettings | `qml/ArtworkImage.qml` | wired |
| `artworkCleanDetail` | Artwork print | ditto | `qml/ArtworkImage.qml` | wired |
| `artworkHalftoneTexture` | Artwork print | ditto | `qml/ArtworkImage.qml` | wired |
| `artworkHalftoneDotSize` | Artwork print | ditto | `qml/ArtworkImage.qml` | wired |
| `artworkHalftoneMonochrome` | Artwork print | ditto | `qml/ArtworkImage.qml` | wired |
| `artworkWornTexture` | Artwork print | ditto | `qml/ArtworkImage.qml` | wired |
| `miniPlayerFrame` | - | none | none | **dead key** |
| `videoControlsStyle` | - | none | none | **dead key** |
| `videoProfile` | Default video profile | Seg + Auto | `src/sync/SyncEngine.cpp:2201` | wired |
| `videoMinSizeMB` | - | none | `src/library/LibraryScanner.cpp:426` | **live, no control** |
| `transcodeAccel` | Transcode acceleration | Seg | `src/sync/SyncEngine.cpp:1375` | wired |
| `preferredAudioLang` | Preferred audio language | list | `src/player/MpvController.cpp:52`, `src/sync/SyncEngine.cpp:2339` | wired |
| `syncAudioFormat` | Audio conversion | Seg MP3/WMA | `src/sync/SyncEngine.cpp:275` | wired today |
| `syncAudioBitrate` | Audio conversion | Seg 128/192/256/320 | `src/sync/SyncEngine.cpp:283` | wired today |
| `copyNativeAudio` | Audio conversion | switch | `src/sync/SyncEngine.cpp:287` | wired today |
| `mp3Quality` | Audio conversion | Seg Detail 0/2/4/6 | `src/sync/SyncEngine.cpp:292` | wired today |
| `ffmpegDir` | Audio conversion | folder picker | `src/main.cpp:254` | wired today |
| `duplicateFormats` | Duplicate formats | checkboxes | `src/LibraryService.cpp:169` | wired |
| `posterSource` | Artwork source | Seg | `src/LibraryService.cpp:1723` | wired |
| `albumCoverLookup` | Music artwork | switch | `src/LibraryService.cpp:348` | wired |
| `autoMetadataLookup` | Music artwork | switch | `src/LibraryService.cpp:338` | wired |
| `tmdbApiKey` | - | none | `src/library/TmdbClient.cpp:23` | **live, no control** |
| `fanarttvApiKey` | - | none | `src/library/MusicIdentityClient.cpp:580` | **live, no control** |
| `podcastAutoQueue` | Podcasts | switch | `src/main.cpp:325` | wired |
| `podcastAutoQueueCount` | Podcasts | picker | `qml/PodcastSyncPicker.qml` | wired |
| `podcastAutoCleanup` | Podcasts | switch | `src/sync/SyncEngine.cpp:1113` | wired |
| `Prefs.playerStyle` | Now-playing player | Seg | ribbon, island, mini-player, video | wired |
| `Prefs.backdrop` | - | none | none | **dead alias** of `backdropPreset` |
| `Prefs.displayFont` | - | none | none | **dead alias** of `headerFont` |
## The five tabs, card by card

Routing is `qml/SettingsPage.qml`, one file, five tabs.  Nothing in it is a placeholder:
every card either writes a key above, calls a service, or is labelled as a readout.

**Appearance** - Now-playing player, Artwork print, Backdrop, Ambient glow, Letter
breaks, Display font.  All six write.

**Library** - Songs waiting for another read (readout of the retry queue), Podcasts
(three keys), Audiobook folders (opens the importer), Watch folders (add, set type,
set the import default, remove, plus the per-folder track count), Artwork source,
Duplicate formats, Re-lookup, Music artwork (two switches plus retry-missing), Rescan.

**Device** - USB access, Transcode acceleration, Default video profile, Preferred audio
language, and the three added today: This Zune, Previously connected, Audio conversion.

**Mixtapes** - Your mixtapes (art and dominant colour per tape), Import & export
(`.m3u` in, export out).

**Health** - Status (every `LibraryService.healthChecks()` row plus temp-space), Library
(watch-folder reachability), Backups & tour, About, Powered by.

## Two kinds of dishonesty found here

Both are worse than a missing feature because the page looks complete.

1. **A live key with no control.**  `videoMinSizeMB` is read while scanning
   (`src/library/LibraryScanner.cpp:426`) and the scanner *tells the user to change it in
   Settings*: "or lower videoMinSizeMB" (`src/library/LibraryScanner.cpp:876`).  There is
   no such control, on any tab, on either platform.  `tmdbApiKey` and `fanarttvApiKey` are
   the same shape - they gate real lookups (`src/library/TmdbClient.cpp:23`,
   `src/library/MusicIdentityClient.cpp:580`) and can only be set by hand in the ini.
2. **A control with no reader, or a key with neither end.**  `miniPlayerFrame` and
   `videoControlsStyle` are declared, defaulted, and never written by a control nor read
   by anyone - a previous session's scaffolding.  `Prefs.backdrop` and `Prefs.displayFont`
   are aliases whose real keys live in `AppSettings` instead.  These four are inert; the
   fix is to delete them or finish them, not to leave them in a Settings inventory.

## What was missing and is now wired

All three were named by the user on 2026-10-24 as "Settings has lots of problems", and
each is a real backend path, not a card:

- **This Zune** (`qml/SettingsPage.qml:1590`) - the device page answered "nothing
  connected" and no history.  Now name, model, serial, capacity, per-category storage and
  the connect-time battery snapshot come from `DeviceService`, which already had all of
  it.  There is deliberately no firmware row: `libzune/include/zune.h` has `zune_get_serial`
  (`:68`) and no firmware-version getter, so a firmware field would be an invention.
- **Previously connected** (`qml/SettingsPage.qml:1702`) - `src/DeviceHistory.{h,cpp}`,
  one row per serial, keyed by serial and falling back to model+name, stored beside
  `syncqueue.json` so an isolated profile gets its own history.  `connects` counts
  sessions, not probes, because a finished sync arms a reconnect ladder whose six rungs
  are each a full `OpenSession`.  Per-row forget, and clear all.
- **Audio conversion** (`qml/SettingsPage.qml:1781`) - MP3 or WMA, CBR 128/192/256/320,
  and a switch to send an existing WMA as its own bytes instead of re-encoding it.  The
  card answers its own capability question from the linked libavcodec through
  `DiagnosticsService.conversionTools()` rather than from a table, so a build without a
  WMA encoder cannot offer WMA.
- **FFmpeg disclosure** - Health now reports a missing `ffmpeg`/`ffprobe` on PATH as its
  own row (`ffmpegcli`, `src/LibraryService.cpp`), and says out loud that conversion does
  *not* need them because it runs on the libav this executable links.  Before this, the
  only thing a user could see was a device video thumbnail that silently never appeared.

Named again the next day, 2026-10-25, with "is audio conversion really usable - you have no
LAME setting and no FFmpeg setting", which is a fair complaint about a card that reported a
capability without offering a knob or a way to prove it:

- **LAME effort** (`mp3Quality`, `src/sync/SyncEngine.cpp:292`) - Detail 0/2/4/6, read with
  the rest of the audio policy so a queue cannot change the effort halfway through an album,
  applied at both `lame_set_quality()` sites (`transcode/libav_transcode.c:255`, and the
  chapter-range path at `:773`), and echoed as `LAME <version>, constant bit rate, ID3v2.3,
  detail <n>` with the version read out of the linked encoder through `get_lame_version()`
  rather than assumed.  Default 2 is what the code already passed, so the control changes
  nothing until someone moves it.
- **FFmpeg folder** (`ffmpegDir`, `src/main.cpp:254`) - name the folder and it goes in front
  of `PATH` for this process; the card shows the resolved `ffmpeg`/`ffprobe` and says whether
  each came from that folder or from `PATH`, and the log says the same at startup.  A missing
  CLI is still not a conversion problem and the card keeps saying so.
- **Encoder self-test** (`DiagnosticsService::runAudioSelfTest()`,
  `src/diagnostics/DiagnosticsService.cpp:285`) - writes a two-second 440 Hz tone and sends it
  through every encoder this build can reach - MP3, WMA, and on Windows WMA Lossless - through
  the same `zuuned_transcode_audio_ex()` a sync uses at the bitrate and effort in Settings,
  checks the first four bytes for the container they claim, probes the result for length and
  size, and deletes what it made.  A lossless encode has no bit rate to be measured against, so
  that leg is judged on its container and its length and its size is only ever reported.
  Answers in codes; the sentence is QML's, so it goes through the catalogue.  It refuses while a
  transfer or a device operation holds the player, because one transcode at a time is a
  property of the encoder layer.

## Measured limits, so no option is offered that cannot be honoured

- The linked FFmpeg `N-127697-g3ebed8c087-20261019` carries **`wmav1`/`wmav2` only** - no
  `wmalossless`, no `wmapro` - and this machine's System32 has no WMA Lossless ACM codec
  either.  Nothing was forked or vendored to work around that.  On Windows the lossless encode
  asks **the encoder that is already inside Windows**: `transcode/wma_lossless_enc.cpp` drives
  the Windows Media Format runtime that ships as `wmvcore.dll` (`IWMCodecInfo3` to enumerate,
  `IWMWriter` to write), and `transcode/wma_lossless_stub.c` answers "not here" on every other
  platform.  The only new dependency is an import library for a DLL that is on every Windows
  install.  Making the formats show up at all takes `SetCodecEnumerationSetting()` with
  `g_wszVBREnabled` TRUE and `g_wszNumPasses` 1, re-applied before the final `GetCodecFormat()`,
  or the lossless codec enumerates zero formats and the honest first answer is "no lossless
  encoder".  Measured on this machine against the relinked binary: `transcodetool encoders`
  reports `wma-lossless=1`, and 44.1 kHz/16-bit stereo, 48 kHz/16-bit stereo, 44.1 kHz/24-bit
  stereo and a copy of the first under a Chinese file name each wrote an ASF that `ffprobe`
  names `wmalossless` and that decoded back to **bit-identical PCM** - equal sample counts, no
  encoder delay.  A 30 s slice of a real master cost 71.2 % of its PCM (1,006 kbps), which is
  why `estimateTrackBytes()` reserves 1,280 kbps for a lossless leg and why no size statement
  here rests on a tone - a sine costs about 795 kbps and understates the file.  The codec's own
  table is the limit and the app honours it exactly: stereo or 5.1 only, 44.1 to 96 kHz, 16-bit
  at 44.1 kHz and 24-bit above it, eight formats in total.  **Nothing is resampled, upmixed or
  otherwise changed to make a file fit** - a 48 kHz/16-bit source is handed over in 24-bit
  containers because the table has no 16-bit entry that high and a left shift moves no sample
  value, and a mono or 192 kHz source is refused with a message that says why.  Only the Zune HD
  is documented to play the result, and nothing this build writes has been heard on a player.
  A source that already *is* WMA still goes through byte for byte under `copyNativeAudio`, which
  is a different feature with a different name.
- VBR MP3 is broken upstream of this app and deliberately not offered:
  `lame_mp3_tags_fid()` never writes the header, measured as no `Xing`/`Info` frame and a
  probe duration of 388.58 s against 272.17 s of real audio.  A VBR option would produce
  files whose length a Zune reads wrong, so the bitrate control is CBR only.
- `format_from_ext()` (`libzune/src/track.c:230`) chooses the MTP object format from the
  **file extension**, so a native WMA copy has to be staged under a `.wma` name or it is
  announced to the player as MP3.
- Nothing in this repository has ever heard a file come out of a player.  WMA device
  playback is unproven, so **WMA stays opt-in and MP3 320 CBR stays the default.**

## What it should contain

Judgment, not evidence, from here down.  The original client's own Options dialog is not
recorded anywhere in this tree; the two lines this section leans on are
[ZUNE_CLIENT_FEATURES.md](ZUNE_CLIENT_FEATURES.md) `:230` ("Convert for the target model -
Yes, quality presets") and `:219` ("Sync on connect, driven by settings - ... No
equivalent setting on the music side").

Worth adding, in the order I would do it:

- **Finish what is half-present.**  Give `videoMinSizeMB` the slider the scanner already
  tells the user to find, and give the two API keys a row each under Library, since a key
  in an ini is not a feature.  Delete `miniPlayerFrame`, `videoControlsStyle`,
  `Prefs.backdrop` and `Prefs.displayFont`.
- **Quality presets, not a bitrate matrix.**  The original client shipped presets.  A
  single "Quality" control - Lossless passthrough / High 320 / Standard 192 / Space
  saving 128 - covers what the three audio keys do today and keeps "convert for the target
  model" as one decision.  WMA Lossless belongs beside that control rather than inside it: it
  exists on Windows only, it takes no bit rate, and a preset that is a fourth format on one
  platform and an error on another is worse than the three keys it replaced.  The three keys
  stay as the storage underneath.
- **Per-device overrides.**  Everything audio is global today, and one user with a Zune HD
  and a Classic has two different decoders and two different disks.  The history rows now
  exist to hang that on.  Do not do it before a second model has been connected once.
- **A "what will this cost me" summary** next to the queue: the capacity gate already
  computes it through `estimateTrackBytes()`, which now accounts for a native copy
  spending the source size rather than the nominal rate.
- **Auto-sync on connect**, which is the honest name for the gap at `:219`.  It is the
  original client's behaviour and it is the one setting here that would write to a
  player without a click, so it has to arrive with a visible plan and a cancel, not on
  its own.

Deliberately **not** options, each for a reason already in this repository:

- VBR - measured above.  WMA Lossless left this list when the encode stopped being a fork
  question: on Windows the operating system writes it and it is measured above, and where there
  is no such encoder the option is absent from the card rather than offered and broken.
- DRM or playsync conversion - the licence servers are gone
  ([ZUNE_CLIENT_FEATURES.md](ZUNE_CLIENT_FEATURES.md), "What cannot come back").
- Automatic playlists from ratings and play counts - no rule engine, and no ratings or
  play counts in the schema.
- Multiple simultaneous devices - one connection at a time is a design decision
  (`DeviceService::breach()`), not an unfinished feature.
- A device clock write - the only device-property write in this tree is the `0xD402` name.

## Wording, and one layout trap that hid a working card (2026-10-25)

Every label on the five tabs is catalogue wording - `qsTr()` in QML,
`UiText::tr()` in C++, Chinese from `i18n/zh_CN.tsv`. The five tab names are the
one exception in the visible page: `qml/SettingsPage.qml` keeps `外观 资料库 设备
混音带 健康` as identity tokens in `pivots.tabs`, because
`--settings-tab Device` names one byte for byte and the body picks its tab with
`indexOf(current)`; the pivot paints the catalogue translation and the token never
changes. See [I18N.md](I18N.md), "When one English word is two words in Chinese".

The trap: `This Zune` printed its six labels and none of its six values, so the
card looked unimplemented while `DeviceService` was answering. The Device tab was
**2263 logical pixels wide** - a `StackLayout` sizes every child to the implicit
width of the widest one and lets it overflow, so the widest row in any of the five
tabs widened all five, and the card's right-aligned values landed outside the
window. Measured headless with `qml.exe` on a 300 px stack: a Layout child cannot
opt out (`width: stack.width` is clobbered, `implicitWidth` is ignored, a filler
item cannot cap a `max()`), but `anchors.left/right` inside a plain `Item` take the
parent's width and are immune to content. The tab bodies are anchored in a plain
`Item` for exactly that reason, the same reasoning `qml/Main.qml:1078` records for
the page stack. `PhaseNote` wraps and fills instead of setting its own width, which
is what let one long note widen a whole tab in the first place.
