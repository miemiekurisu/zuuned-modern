# The original Zune PC software, and what this app does instead

Purpose of this page: answer "what did the Zune software on Windows do that Zuuned
Modern does not do?" It is a product gap analysis, not a status page. Nothing here
adds or changes a hardware claim; [WINDOWS_PORT.md](WINDOWS_PORT.md) is the only page
that states what has been done to a player from a Windows machine, and
[FEATURE_COMPLETION.md](FEATURE_COMPLETION.md) is the implementation backlog. A third
axis lives in [UI_WIRING_AUDIT.md](UI_WIRING_AUDIT.md): whether a control exists on
screen at all and reaches a service. A capability counted as present here says nothing
about either of those, and the fourth column of the comparison table stays hardware
evidence.

Read it in this order: what the original software was, where that description comes
from and what each release added, then the capability comparison, then what cannot come
back, then the community work worth a pointer, then what is still worth doing here.

## What the original software was

The Zune software (also called the Zune client) was Microsoft's desktop companion for
the player line. First release 2006-11-14, final release 4.8.2345 on 2011-08-22.
It ran from Windows XP SP3 up to Windows 10, and it was built on Microsoft Iris (the
Metro-era UI framework), not on a toolkit this tree shares anything with.

It was really five products in one process:

- a desktop media player and local library over files on the PC,
- the sync and management tool for Zune 4/8/16/30, the 80 and 120 GB players, Zune HD,
  and later Windows Phone 7 and Kin devices,
- a media streaming server that shared the library to other PCs and to Xbox 360,
- the storefront client for Zune Marketplace, including the Zune Music Pass subscription,
- the Zune social client, including the files users swapped with each other.

Two of those five are gone in a way no client can repair. Microsoft replaced the
software in October 2012 (Xbox Music, then Movies and TV, plus a separate phone sync
app) and shut the online services down on 2015-11-15. Marketplace, Music Pass, social
presence, swaps, and the metadata and artwork metaservices that the library pulled from
`toc.music.metaservices.microsoft.com` and `images.metaservices.microsoft.com` all went
with them. The tag-enrichment half of what the original client did automatically was a
Microsoft-hosted database, not a property of the protocol, so "the old app filled in
album art by itself and this one does not" is a difference between two providers, not
between two protocol stacks.

Everything on this page is consulted, not remembered. The sources are named in *Where
this comes from* below, and the release timeline is dated from them line by line. Where
a consulted source does not answer, the page says so in the cell instead of filling it:
one row reads *no source*, one device-property row leaves the original client `n/a`
because nothing consulted records it, and one inherited cell states that the consulted
pages do not name the fields it claims. Nothing else here rests on memory.

Media formats the original client handled, as the reference for the transfer table
below: audio MP3, AAC-LC in .mp4/.m4b/.mov, and WMA in its standard, professional and
lossless flavours; video MPEG-4/H.264 in mp4/m4v/mov plus WMV, ASF and AVI; images JPEG.
The `.m4b` in that audio list is an audiobook container, and it means only that the
program could play one on a PC - see the audiobook note under the first table.

## Where this comes from

Three consulted sources, and nothing else:

- Wikipedia, *Zune software* - <https://en.wikipedia.org/wiki/Zune_software>. What the
  program was, the five jobs in one process, which devices it managed, the format list,
  the automatic album-art and metadata behaviour, the Marketplace and Music Pass terms,
  the Xbox 360 streaming, and the 2015-11-15 shutdown of the services.
- Wikipedia, *Zune Software version history* -
  <https://en.wikipedia.org/wiki/Zune_Software_version_history>. Every dated entry in the
  timeline below, and the absence of an audiobook entry from all of them.
- Microsoft Q&A, *"Audiobooks on Zune HD"*, question dated 2011-02-01 and migrated from
  the old Microsoft Support Community -
  <https://learn.microsoft.com/en-us/answers/questions/2462655/audiobooks-on-zune-hd>.
  The answer is by an owner of a Zune Originals 120 GB and a Zune HD 64 GB, not by
  Microsoft: the Zune HD supports audiobooks, they cannot be synced through the Zune
  software, and Audible or OverDrive's own software was how one got there. It is a
  community answer on a Microsoft domain and it is quoted as one. It is the only source
  here for a negative about the client's media categories, and the version list's silence
  about audiobooks is what makes it safe to rely on.

No first-party manual was consulted, and the manual that did exist went away with the
service in 2015. That is a limit on this page, not a claim that nothing was written.

## What each release added on the client side

Firmware-only entries are left out: this is what the Windows program itself gained, which
is the half a client is measured against. Dates are those the version-history article
gives.

- 2006-11-14 - first release. Library, player, device sync, Marketplace.
- 2.2 - 2007-11-13. Wireless sync between players, podcast support, new interface on both
  ends of the cable.
- 2.4 - 2008-01-22. Fixes to Marketplace, podcasts, sync, Xbox streaming and the library.
  No new named feature.
- 2.5 - 2008-05-06. Television downloads, gapless playback, smart playlists, new sorting
  options, and syncing more than one device.
- 3.0 - 2008-09-16. Buying music from the radio, Zune Channels, wifi hotspot auto-refresh
  and favourites exchange, clock, screen lock and games, and the "quick list" renamed the
  "now playing" list.
- 3.1 - 2008-11-18. Like-Minded Listeners.
- 3.2 / 4.0.740 - 2009-09-15. Zune HD support.
- 4.0.740 - 2009-11-05. Unicode support inside the software - searching, and editing tags.
- 4.2.202 - 2010-01-26. Xvid (`.avi`) playback in the software. Smart DJ and Social
  removed for users outside the United States.
- 4.7.1404 - 2010-10-11. Windows Phone 7, Live Essentials 2011, content management
  separate from Windows Libraries, and streaming of purchased video.
- 4.8.2345 - 2011-08-22. Windows Phone 7.5, and a "Show Sort" entry added to the tagging
  system.
- 2012-08-31 - the store side ends. The apps section, users' media collections and Zune
  Social are disabled, and Mixview with them.
- 2015-11-15 - the services shut down entirely.

Three things to notice in that list. Podcasts were not in the product at launch, which is
worth remembering before anyone treats a podcast page as an invention. Audiobooks appear
in none of the dated releases above: no category, no view, no transfer, across nine
years of client releases. And the two entries that read as housekeeping - sorting options
in 2.5 and "Show Sort" in 4.8.2345 - are both about the same thing this application's
schema has no field for.

## Capability comparison

The third column answers one question only: has this line been done on a Windows
machine against a real player? It borrows the vocabulary `WINDOWS_PORT.md` argues in,
and every cell names the narrower thing that was actually measured.

- `read` - bytes came off the player to this machine and were looked at.
- `send` - the player accepted the object and re-listed it; no byte came back.
- `no` - implemented and software-gated, never done from Windows against a player.
- `n/a` - not a device question.
- `yes` - done from Windows against a player, and it is neither a media read nor a
  media write.

### Playing and holding a library

| Capability | Zune software | Zuuned Modern | Windows vs a player |
| --- | --- | --- | --- |
| Desktop player | Yes | Yes - mpv, queue, now-playing island, compact island below the headroom breakpoint, speed, sleep timer, bookmarks | n/a |
| Library over local files | Yes | Yes - watch folders plus scanner, audio-fingerprint duplicate detection, verified local copies | no |
| Album, artist, genre, track views | Yes | Yes - gallery grids, opt-in A-Z breaks, album and artist detail pages | n/a |
| Gapless playback | Yes, from 2.5 | No `gapless-audio` option is passed to mpv anywhere in this tree, so its default between tracks stands. Read off the option list, not heard | n/a |
| Sort order for names | New sorting options in 2.5, and a "Show Sort" tagging field in 4.8.2345 | No sort column in `tracks` at all. The A-Z breaks and the indexes order on the display name with `COLLATE NOCASE`, so an album called *The Unbearable Lightness of Being* files under T and there is no tag that could move it | n/a |
| Search and tags in a non-Latin script | Only from 4.0.740, 2009-11-05 - three years after the first release | Native from the first commit. The development fixture is a Chinese-named folder, album and artist set, and an album named in Chinese has been sent from Windows and re-listed off the player | send |
| Video library | Yes | Yes - TMDB matched titles, show/episode structure, posters and backdrops | n/a |
| Photo library | Yes, JPEG | Yes - folders plus custom nested albums, ordered membership | no |
| Podcasts | Yes, and only from 2.2 (2007-11-13) | Yes - RSS preview, subscribe, download, retention and cleanup, opt-in auto-queue on connect, played and played-on-Zune state | no |
| Audiobooks | No. No consulted source records an audiobook category, view or transfer in the client, in any dated release; see the note under this table | Yes - ordered imports, chapters, custom covers, resume and bookmarks | no |
| Battery and free space on the player | Yes | Yes - battery from MTP property `0x5001` at connect, capacity and free, attributed per category | read |
| CD ripping | Yes | No | n/a |
| Ratings, hearts, play and skip counts | Yes, and the smart playlists of 2.5 rank on them - though neither consulted page names the rating fields themselves, so treat this cell as the one inherited claim | No - `LibTrack` in `src/library/LibraryTypes.h` has no rating or play-count field at all, only `lastPositionMs` | n/a |

Two of those rows are about fields rather than behaviour, and that is the reason to write
them that way: `tracks` in `src/library/LibraryDb.cpp` carries no rating, no play count and
no sort column, so everything the original client sorted or ranked automatically is a
migration in this fork before it is a feature. The gapless row is the opposite kind of gap -
one option, no schema - and it is open for a procedural reason rather than a technical one,
because the only test for a join between two tracks is hearing it.

The audiobook row deserves its own sentence because it is the largest single difference
between the two products, and it is not a gap. Nothing in the dated releases above
adds an audiobook anything, and the community answer on Microsoft's own Q&A page is
explicit that a Zune's audiobooks came through Audible or OverDrive software rather than
through the Zune client, whose `.m4b` support meant playback on the PC. So the podcast and
audiobook pages in this application are not a feature this port failed to port - they are
a feature the original software never had. What is still open is on this side: the
firmware category at the end of an audiobook transfer has never been proven from Windows.

### Getting media into the library

| Capability | Zune software | Zuuned Modern | Windows vs a player |
| --- | --- | --- | --- |
| From folders on the PC | Yes | Yes, and a folder is the unit - there is no per-file add | no |
| From the player, into the library | Yes | Yes - `Save to Library` for music, photos and video; a pull keeps the device name and judges the payload | read |
| From the store | Yes | No, and it cannot (see below) | n/a |
| From a disc | Yes | No | n/a |
| From another user | Yes, swaps | No, and it cannot (see below) | n/a |
| Playlists as text | Import and export | Import of M3U, including the code-page and BOM rules; export keeps its own naming rule | n/a |

### Metadata and artwork

| Capability | Zune software | Zuuned Modern | Windows vs a player |
| --- | --- | --- | --- |
| Album art | Fetched automatically from Microsoft metaservices | Local-first: embedded picture, then folder image. A provider is reached only when the user asks | read |
| Artist imagery | Same hosted service | Deezer and Fanart.tv, per item, on request | no |
| Metadata and tag enrichment | Automatic | `AppSettings.autoMetadataLookup`, an ini key, defaults to false. When it is off a library read logs how much it resolved locally and that nothing was queried | n/a |
| Asking for one item | Automatic, no per-item control | Right-click: one album, one artist, one track row. Settings to Library carries a whole-library button for the times you want that | n/a |
| Tag editing | Yes | Yes - Customize matches a track against a release identity and writes tags back | n/a |
| Artwork rendering style | Fixed | Artwork style plus clean, halftone and worn parameters | n/a |

The order matters and was a deliberate decision: on a library whose files carry no
embedded picture and no folder image, tiles stay empty and the artist grid says
`Needs a match` until someone asks. The network is something a user does to one item,
not something that happens to a library. The measured reason is the eighteenth pass of
`WINDOWS_PORT.md`, where an artist portrait with no local source at all turned fourteen
songs into six provider requests and a `HTTP 503`.

### Organizing

| Capability | Zune software | Zuuned Modern | Windows vs a player |
| --- | --- | --- | --- |
| Manual playlists | Yes | Yes - create, rename, delete, reorder, Add to Playlist with dedup, library-first so a playlist survives having no player attached | n/a |
| Device playlists | Yes | Yes - forge a playlist on the player, and import one off it | read |
| Automatic playlists from rules | Yes - top rated, most played and similar built themselves | No - there is no rule engine, and the ratings and play counts it would rank on do not exist in the schema yet | n/a |
| Favourites and starred items | Loved flag | Podcast and audiobook favourites, independent per show, episode and book | n/a |
| Virtual grouping | Genre and mood views | Podcast series, audiobook Libraries and starred collections, many-to-many and never folders | n/a |
| Mood and genre artwork | No source. Neither consulted page records artwork or browsing keyed to a mood; what they do record is a genre-filtered store, smart playlists from 2.5, and Mixview until 2012-08-31 | Genre and artist identity moments through the artwork style pipeline - a fork idea, not a port of a Zune feature | n/a |

### Managing the player

| Capability | Zune software | Zuuned Modern | Windows vs a player |
| --- | --- | --- | --- |
| Detect it plugged in | Yes | Yes - libusb, and on Windows `cfgmgr32` as well, because a driver-less Zune is at PnP problem 28 and libusb never sees it | yes |
| Tell the user how to fix the driver | n/a on its own platform | Yes - the fix-it card and Settings to Device name WinUSB and print the exact device id to pick in Zadig | read |
| Authenticate | Yes | Yes - MTPZ, with credentials supplied at build time | yes |
| Browse what is on it | Yes | Yes - music, video, photos, playlists, podcasts and audiobooks, each lazily and serialized | read |
| Re-index after a write | Yes | Yes - finalize plus a bounded reconnect ladder, because WinUSB keeps the interface and Windows sends no hotplug arrival | read |
| Rename the player | Yes | Yes - `renameDevice` through MTP property `0xD402` | no |
| Rename an object | Yes | Yes - `renameItem` | no |
| Create a playlist on it | Yes | Yes - `createPlaylist` | no |
| Delete from it | Yes | Yes - `purgeItems` and `purgeSpokenItems`, user-confirmed, live selection revalidated first | no |
| Eject safely | Yes | Yes - `eject` | no |
| Duplicate detection before sending | Yes | Yes - shared identity with disc/track distinction and an explicit ambiguous answer | no |
| Storage accounting per category | Yes | Yes, with honest counts where per-media bytes are unknown on classic players | read |
| Sync on connect, driven by settings | Yes | No equivalent setting on the music side; the queue is explicit and only the reconnect ladder is automatic | no |
| Device clock write | n/a | No - the only device-property write in this tree is the `0xD402` name | n/a |
| Battery charge state shown while connected | Yes | Yes, as a connect-time snapshot | read |
| Manage more than one player | Yes - multiple device syncing from 2.5 | One at a time by design: `DeviceService::breach()`, the connect path, returns at once when a connection is already open, and every device operation is serialised on one worker | n/a |

### Moving media

| Capability | Zune software | Zuuned Modern | Windows vs a player |
| --- | --- | --- | --- |
| Music to the player | Yes | Yes | read |
| Music off the player | Yes | Yes, into the folder the library declares for music - the marked destination, else the earliest added music folder, and only else `Music\Zuuned Imports` - indexed there and with its embedded art read out | read |
| Convert for the target model | Yes, quality presets | Yes - video profile auto from the connected model, Zune 30 WMV and 480x272 profiles, `transcodeAccel` auto with NVDEC on this machine; audio from Settings to Audio conversion - MP3 through LAME or WMA, CBR 128 to 320, with a source that already is WMA copied rather than re-encoded. WMA is opt-in and MP3 320 stays the default because no player attached here has ever played a WMA, WMA Lossless is a third target and it is Windows-only: the operating system's own encoder writes it, not libavcodec, which still carries `wmav1`/`wmav2` and no `wmalossless`. The encode is measured - PCM comes back bit-identical - but it is stereo or 5.1 at 44.1 to 96 kHz and nothing else, it never resamples to fit, and only the Zune HD is documented to play the result. Presets are still one control short of the original's - see [Settings inventory](SETTINGS_INVENTORY.md) | send |
| Photos to the player | Yes | Yes | send |
| Photos off the player | Yes | Implemented; the thumbnail fallback path has never returned a picture from a player | no |
| Video to the player | Yes | Yes, through the real transcoder | send |
| Video off the player | Yes | Yes - `saveVideoToLibrary` | no |
| Podcasts and audiobooks to the player | Yes | Yes in source, with firmware listening state and typed admission | no |
| Size and space guard before a batch | Yes | Yes - capacity check plus typed admission with recovery | no |
| Client-side content protection | Yes, WMA playsync | No, and it cannot (see below) | n/a |

## What cannot come back

These are not gaps in this port. They depended on a Microsoft service that has been
shut down, or on material Microsoft never published, and no client can restore them.

- Zune Marketplace: the storefront, the 14 million track catalogue, TV and movie
  purchase and rental, and the Zune HD apps and games catalogue.
- Zune Music Pass and every WMDRM-licensed file. A `.wmz`/`.mswmp` purchase from 2009
  has no license server to answer for it, which is also why there is no
  DRM-conversion path here.
- Zune social: presence, what-a-friend-is-listening, and the swapped files, which were
  themselves DRM-limited exchanges between two clients. Smart DJ and Like-Minded
  Listeners (3.1, 2008-11-18) sorted a graph of those listens, were withdrawn from users
  outside the United States in 4.2.202, and went with the rest.
- Mixview, the auto-mixed visual set, and Zune Channels. Mixview was switched off on
  2012-08-31 along with the apps section and users' media collections; Channels was a
  feed from the same servers.
- Microsoft's metadata and artwork metaservices. Replaced here by open providers
  (MusicBrainz and Cover Art Archive for releases, Deezer and Fanart.tv for artist
  images, TMDB for video) reached per item and only on request.
- Firmware updates, Windows Phone 7 and Kin management, and deploying apps or games to
  a Zune HD.
- Sharing the library to other PCs and to Xbox 360 through Windows Media Player
  Sharing - a different dead ecosystem, and out of scope for a Zune manager.
- Wireless sync between two players (2.2), buying music from the radio (3.0), and the
  Messenger now-playing broadcast. Each of those was this client talking to a Microsoft
  endpoint or to another client inside the same social system.

One line in that list is a real judgement call rather than a dead service: CD ripping.
It was never a Zune protocol feature, so it is missing here because nobody has built it,
not because anything is missing. It sits in the open list below for that reason.

## Pointers to the surviving community work

Found while writing this page. None of it is a dependency of this repository, none of it
is wired in, and it is named as a pointer rather than as something reviewed line by
line. Two of them are useful for exactly the question this page is about.

- `ZuneDev/ZuneNetApi` and `JarHead4/PyZuneMetadataServer` - recreations of the original
  Zune web and metadata APIs. Between them they are the best surviving record of which
  fields the original client asked a server for, which is what a metadata feature has to
  be compared against.
- `ZuneDev/ZuneShell.dll`, `ZuneDev/MicrosoftIris`, `ZuneDev/Xune` - decompiled ZuneShell
  and UIX material and the Iris UI framework the original client was built on, plus a
  cross-platform Iris port. Reference for behaviour and layout, not for this UI.
- `kbhomes/libmtp-zune`, `nicebeard/zune-explorer` - other independent clients, useful as
  a second opinion on protocol behaviour alongside libzune.
- `project-lyra-zune/project-lyra`, `simulacra10/zunefirmware-upgrade`,
  `ZuneDev/ZuneModdingHelper` - the jailbreak, firmware and modding side. This is where
  anything about firmware or on-device app deployment would have to start from, and this
  repository does not go there.

## Still worth doing here, in the order worth doing it

Item 1 is not a feature decision, it is the precondition for the rest: the Windows
hardware gates in `WINDOWS_PORT.md` are open, and every added capability would be
measured against a player that this port has never deleted from.

Items 4, 6 and 7 are the ones a dated release names, which is the useful kind of gap:
the original client shipped them, they were not a hosted service, and nothing about the
protocol prevents them here.

1. Close the open Windows device gates: one deletion, audible playback, the photo and
   video read halves, the podcast and audiobook categories with firmware resume, then a
   Classic, a Draco and an 80/120.
2. Packaging and an install path, so a tester is not choosing a driver in Zadig by hand.
3. Ratings or hearts, plus play and skip counts, in the music schema. Everything
   rule-driven in the original client ranked on these, and the schema has no field for
   them, so this is the one gap that blocks three others. The video side already carries
   TMDB and episode ratings, which is the precedent for the migration shape.
4. Sort-order tags - `artistsort`, `albumartistsort`, `albumsort`, and the "Show Sort"
   field of 4.8.2345 behind them. The A-Z breaks order on display names today, so an
   album called *The Unbearable Lightness of Being* files under T. Same migration shape
   as item 3, and the original client cared twice: sorting options in 2.5, Show Sort in
   4.8.2345.
5. Automatic playlists from rules, once there is something to rank on.
6. Gapless playback, which is one mpv option (`gapless-audio`) and nothing else. The
   client had it from 2.5. It is not switched on here because the only test for a join
   between two tracks is hearing two tracks join, and every audio gate in this repository
   runs muted: item 1 is the precondition, not the implementation.
7. Two players managed at once. The client synced more than one device from 2.5; here a
   second `breach()` is refused while one is open and every operation is serialised on
   one worker. That shape is deliberate and has a reason - device ownership - so this
   item exists to keep it a decision rather than a drift.
8. A real "which folder receives each kind" surface, one step past what the per-kind
   default now does. The destination rule is right as of the twenty-second pass of
   `WINDOWS_PORT.md`; what is still missing is any way to see, per folder, that a music
   download will land there without reading the label.
9. A per-device "sync this on connect" preference, which is what made the original
   client feel automatic. The reconnect ladder today is not that and should not be
   described as it.
10. A device clock write, if a Zune property for it exists. Nothing in this tree writes
   time, and no claim is made here about what the original client did.
11. CD ripping as an add-media path, for people whose source is discs rather than folders.
12. Per-media exact sizes on classic players, and resume as a protocol question, both
   already deferred in `FEATURE_COMPLETION.md`.

## What this page is not

It is not evidence. Every `read` and `send` cell above is a summary of a capture that
lives in [WINDOWS_PORT.md](WINDOWS_PORT.md), and the dated checkpoints there are the
record; if this page and that one ever disagree, that one is right. Software gates say a
rule is in the running binary, nothing more, and no Linux hardware result carried into
any cell here.

