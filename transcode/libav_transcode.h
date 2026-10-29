/*
 * libav_transcode.h — In-process transcoding via MPVKit's libav* + LAME
 *
 * All transcoding, probing, and art extraction without subprocess overhead.
 * Links against MPVKit's libavformat/libavcodec/libswresample/libswscale
 * plus standalone libmp3lame for MP3 encoding.
 */

#ifndef LIBAV_TRANSCODE_H
#define LIBAV_TRANSCODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare ZuneMetadata from zune.h (avoid circular include) */
struct ZuneMetadata;

/* ---- Progress callback ---- */

typedef void (*zuuned_progress_cb)(float fraction, void *userdata);

/* ---- Cancellation ----
 * Thread-safe, process-global (one transcode at a time). abort() makes
 * the in-flight audio/video transcode bail within one packet — temp
 * file unlinked, NULL returned. The sync engine clears the flag at sync
 * start so a stale cancel can't kill the next run. */
void zuuned_transcode_abort(void);
void zuuned_transcode_clear_abort(void);

/* Reads the flag without changing it, for a stage that lives in another
 * translation unit and was never handed the flag directly. */
int zuuned_transcode_abort_requested(void);

/* ---- Hardware decode mode ----
 * 0 = software only
 * 1 = auto (default): NVDEC, then VAAPI, then software
 * 2 = NVDEC (CUDA) only, software fallback
 * 3 = VAAPI (AMD/Intel) only, software fallback
 * macOS ignores the mode beyond 0/non-0 (VideoToolbox). The sync
 * engine mirrors the Settings pick here before each sync. */
void zuuned_transcode_set_hw_decode(int mode);

/* ---- MP3 encoder quality ----
 * LAME's 0-9 scale (0 = most CPU for the best output, 9 = fastest).  Default 2,
 * which is what every build before the setting existed used.  Global by design,
 * like the hardware-decode mode: Settings stores the pick and the sync engine
 * mirrors it here before each pass.  Out-of-range values clamp. */
void zuuned_mp3_set_quality(int quality);
int zuuned_mp3_quality(void);

/* Release string of the linked LAME, e.g. "3.100".  Never NULL. */
const char *zuuned_mp3_encoder_version(void);

/* ---- Audio Transcoding ---- */

/*
 * Transcode any audio format to MP3 320kbps CBR with ID3v2.3 tags.
 * Uses MPVKit's libav* for decoding + libswresample for resampling
 * + standalone libmp3lame for MP3 encoding.
 * Metadata (title, artist, album, etc.) is copied from input.
 *
 * album_artist: if non-NULL and non-empty, overrides the output TPE1
 *               (artist) frame so Zune groups tracks under one artist
 *               instead of splitting on "feat. X" strings. Pass NULL
 *               to keep the input's artist tag.
 *
 * Returns: path to temp MP3 file (caller frees + unlinks). NULL on failure.
 */
char *zuuned_transcode_audio(const char *input_path,
                              const char *album_artist,
                              zuuned_progress_cb progress, void *userdata);

/* Positive disc_number explicitly overrides source disc metadata. Zero/negative
 * preserves a valid source disc or discnumber (including its total). */
char *zuuned_transcode_audio_with_disc(const char *input_path, const char *album_artist,
                                      int disc_number, zuuned_progress_cb progress, void *userdata);

/* ---- Output audio targets ----
 *
 * MP3 is the historical output and the default: LAME plus a hand-built ID3v2.3
 * tag, which is what a Zune reads.  WMA encodes through libavcodec `wmav2` into
 * ASF, the container the player reports as WMA.
 *
 * WMA Lossless is not FFmpeg's to give.  Measured against the FFmpeg this
 * application links, `ffmpeg -encoders` lists `wmav1` and `wmav2` as the only WMA
 * encoders - `wmalossless` and `wmapro` are decoders only - and Windows offers no
 * lossless WMA ACM codec to reach instead.  It does carry the encoder, just not
 * where an ACM or FFmpeg lookup thinks to look: the Windows Media Format runtime
 * that ships in every Windows install exposes "Windows Media Audio 9.2 Lossless"
 * in process through IWMCodecInfo3, and `wma_lossless_enc.cpp` drives that.  No
 * codec is vendored and no upstream project is forked to get it, which is the
 * right shape for a client this size.  The price is that the target is
 * Windows-only, and zuuned_audio_encoder_available() says so everywhere else.
 *
 * Two cautions that belong here rather than only beside the code.  The encoder
 * has no mono format and nothing past 96 kHz or 24 bits, and this layer refuses
 * such a source outright instead of resampling or upmixing it into something that
 * would no longer be lossless.  And a player playing WMA Lossless is not a given:
 * two-channel WMA Lossless is documented for the Zune HD, other models are not
 * documented either way, and the only player ever heard playing what this layer
 * writes is that one Zune HD, on 2026-10-29.  Lossless WMA the user already owns
 * continues to travel byte for byte, which needs no encoder at all.
 *
 * `wmav2` has no variable-bitrate mode, which is why the VBR option is MP3-only;
 * lossless has no bit rate to pick either.
 */
#define ZUUNED_AUDIO_MP3 0
#define ZUUNED_AUDIO_WMA 1
#define ZUUNED_AUDIO_WMA_LOSSLESS 2

/* The transcode with the target and its bit rate chosen by the caller.
 * bitrate_kbps is the nominal CBR rate for MP3, and the ceiling LAME may spend
 * when vbr is set; `wmav2` ignores vbr because it has no VBR mode.  Same
 * contract as zuuned_transcode_audio(): a new temporary path the caller frees
 * and unlinks, or NULL. */
char *zuuned_transcode_audio_ex(const char *input_path, const char *album_artist,
                                int disc_number, int target, int bitrate_kbps, int vbr,
                                zuuned_progress_cb progress, void *userdata);

/* 1 when this build can encode that target, 0 when it cannot, so a settings
 * page never offers what the linked libraries cannot do. */
int zuuned_audio_encoder_available(int target);

/* Release string of the FFmpeg whose libav* this process linked, e.g. "8.0".
 * Settings shows it so a report says what the app carries rather than what
 * happens to be on PATH. */
const char *zuuned_ffmpeg_version(void);

/* Encode to WMA in ASF at an explicit output path, which the caller owns.
 * Copies the tag pairs a Zune displays with the same album-artist override the
 * MP3 path applies; deletes the output on failure or cancellation.  Returns a
 * copy of output_path on success, NULL on failure. */
char *zuuned_audio_encode_wma(const char *input_path, const char *album_artist,
                              int disc_number, int bitrate_kbps,
                              const char *output_path,
                              zuuned_progress_cb progress, void *userdata);
/* Encode to WMA Lossless in ASF at an explicit output path, which the caller
 * owns, through the encoder in the Windows Media Format runtime rather than
 * through FFmpeg.  Same tag contract, same album-artist override and same
 * delete-the-output-on-failure rule as zuuned_audio_encode_wma(), and
 * deliberately no bit rate argument: the format has none.  Returns a copy of
 * output_path, or NULL when this Windows carries no lossless encoder or the
 * source does not fit one.  Never available off Windows. */
char *zuuned_audio_encode_wma_lossless(const char *input_path, const char *album_artist,
                                       int disc_number, const char *output_path,
                                       zuuned_progress_cb progress, void *userdata);

/* 1 when this build can encode WMA Lossless, 0 when it cannot.  Cached: the
 * codec list is asked once per process, not once per track, because a settings
 * page asks it on every repaint. */
int zuuned_wma_lossless_available(void);

/* The two WAVEFORMATEX tags the copy rule cares about, spelled once.  0x0161 is
 * WMA Standard (v1 and v2 share it) and 0x0163 is what the lossless encoder in
 * Windows writes; 0x0162, WMA Pro, is neither. */
#define ZUUNED_WMA_FORMAT_TAG_STD 0x0161
#define ZUUNED_WMA_FORMAT_TAG_PRO 0x0162
#define ZUUNED_WMA_FORMAT_TAG_LOSSLESS 0x0163

/* What a container actually holds, as far as a native-copy decision needs to
 * know.
 *
 * This exists because "the first four bytes are the ASF GUID" says nothing about
 * the audio: WMA Standard, WMA Pro, WMA Lossless and a video file with music in
 * it all open the same way, and a lossless run that native-copies a 96 kbps
 * lossy file has replaced what the user asked for with something that fits.  The
 * answer therefore comes from the demuxer and the stream table.
 *
 * Two measurements decided the shape of these fields, both against files that
 * exist on a real Zune HD rather than against what a spec suggests.  One: a WMA
 * pulled off the player is an ASF carrying an MJPEG stream of the cover image
 * beside the audio, so an attached picture must not be counted as video, or the
 * rule would refuse to copy the player's own files.  Two: FFmpeg's lossless WMA
 * decoder is its own codec - `wmalossless` - and the container tag for it is
 * 0x0163, while the codec private data it hands that decoder starts with
 * something else entirely (measured 0x0010), so the tag has to come from the
 * demuxer's codec_tag and only fall back to the private data.
 *
 * A field this could not read stays zero and is never guessed, so a probe that
 * got nowhere leaves lossless at 0 - and a copy decision that cannot prove a
 * file is what the target asks for has to encode instead. */
typedef struct zuuned_audio_container {
    int probed;             /* 1 when the file opened and its streams were read */
    int is_asf;             /* the ASF container, which is the one WMA travels in */
    int audio_streams;      /* audio streams that a decoder exists for */
    int video_streams;      /* real video: an attached cover picture is not this */
    int art_streams;        /* image streams: the way ASF carries album art */
    int codec_id;           /* AV_CODEC_ID_* of the first audio stream, 0 unknown */
    char codec_name[32];    /* what this FFmpeg calls that codec, e.g. "wmalossless" */
    int format_tag;         /* WAVEFORMATEX wFormatTag, 0 when the file has none */
    int lossless;           /* 1 when the codec id or the container tag names
                             * WMA Lossless; either alone is enough, because a
                             * file can carry the codec without a tag */
    int has_decoder;        /* 1 when this FFmpeg can decode that stream */
    int channels;
    int sample_rate;
    int bits_per_sample;
    int64_t duration_ms;
} zuuned_audio_container;

/* 0 when the probe ran, negative when the file could not be read at all. */
int zuuned_probe_audio_container(const char *path, zuuned_audio_container *out);

/* Why a file may not travel to the player untouched, named in one place so the
 * sync engine, the session log and a command-line check cannot tell three
 * different stories about one refusal. */
#define ZUUNED_NATIVE_COPY_OK            0
#define ZUUNED_COPY_NOT_PROBED           1  /* the file would not open at all */
#define ZUUNED_COPY_NOT_ASF              2  /* a WMA lives in ASF and nowhere else */
#define ZUUNED_COPY_NO_DECODABLE_AUDIO   3  /* no audio stream this build can read */
#define ZUUNED_COPY_HAS_VIDEO            4  /* a picture stream that moves is video */
#define ZUUNED_COPY_NOT_LOSSLESS         5  /* lossless asked, something else offered */
#define ZUUNED_COPY_TARGET_NOT_WMA       6  /* an MP3 run retags, it does not copy */

/* Given what a probe found and the target the user asked for, say whether these
 * bytes may be handed to the player as they are.
 *
 * The rule is a proof, not a resemblance.  A lossless target copies only a file
 * the probe can name as WMA Lossless, because ASF is the container for WMA
 * Standard, Pro and Lossless alike and an ASF header cannot tell them apart;
 * 64 kbps of someone's MP3 history copied in place of 1,025 kbps of the same
 * song is the exact failure this layer exists to prevent.  A lossy target copies
 * any ASF audio it can decode, lossless included, because handing the player the
 * better file asks something of nobody.  Both refuse a stream that moves, and
 * both count an attached cover as art rather than as video, which is how a WMA
 * off a Zune carries its picture.
 *
 * Anything not proved here is encoded instead: a refusal is a detour through the
 * encoder, never a downgrade, and never silent. */
int zuuned_native_copy_verdict(const zuuned_audio_container *probe, int target);

/* Transcode decoded audio in the half-open presentation interval [start_ms,
 * end_ms) into a unique temporary MP3. Milliseconds are relative to the audio
 * stream start. Cuts are made on PCM samples, never packet boundaries; the
 * Info/LAME header preserves encoder delay/padding for gapless-aware playback.
 * Local regular input only; matching AudiobookProbe, external container
 * references and network opens are refused and probing has a 15-second bound.
 * Refuses invalid/missing timestamps, unavailable intervals and ranges whose
 * 320 kbit/s output would exceed 512 MiB. Source endpoint rounding is limited
 * to one millisecond. No source tags or embedded chapters are copied: the
 * caller supplies chapter identity through its typed device metadata.
 * Polls the existing cancellation flag; failure/cancel removes partial output.
 * On success caller owns both the malloc path and file (free + unlink).
 */
char *zuuned_transcode_audio_range(const char *input_path, int64_t start_ms, int64_t end_ms,
                                  zuuned_progress_cb progress, void *userdata);

/* ---- Video Transcoding ---- */

/*
 * Video transcode profiles (matches VideoProfile Swift enum):
 *   0 = Zune 30:      WMV2 320x240, 800k video, WMA 128k (requires forked MPVKit)
 *   1 = Zune 4-120:   H.264 Baseline 2.1, 320x240, 768k, AAC 128k
 *   2 = Zune HD:       H.264 Baseline 3.1, 480x272, 2500k, AAC 192k
 *   3 = Zune HD 720p: H.264 Baseline 3.1, 1280x720, 8000k, AAC 192k
 */
#define ZUUNED_PROFILE_ZUNE30       0
#define ZUUNED_PROFILE_ZUNE4_120    1
#define ZUUNED_PROFILE_ZUNEHD       2
#define ZUUNED_PROFILE_ZUNEHD_720P  3

/*
 * Transcode video to a Zune-compatible format.
 * Profiles 1-3: h264_videotoolbox (HW!) + AAC → MP4
 * Profile 0:    wmv2 + wmav2 → ASF (requires forked MPVKit with wmv2 encoder)
 *
 * series/season/episode: optional TV metadata (pass NULL/0 to skip).
 * audio_lang: preferred audio language (e.g., "eng"). NULL for default.
 *
 * Returns: path to temp video file (caller frees + unlinks). NULL on failure.
 */
char *zuuned_transcode_video(const char *input_path, int profile,
                              const char *series, int season, int episode,
                              const char *audio_lang,
                              zuuned_progress_cb progress, void *userdata);

/* ---- Probing ---- */

/*
 * Probe file duration in seconds. Returns 0 on success, -1 on failure.
 */
int zuuned_probe_duration(const char *filepath, double *duration_out);

/*
 * Probe duration (seconds) AND video resolution in one header read.
 * Returns 0 when at least one of duration/resolution was found, -1 otherwise.
 */
int zuuned_probe_video_info(const char *filepath, double *duration_out,
                            int *width_out, int *height_out);

/*
 * Find audio stream index matching a language tag (e.g., "eng", "jpn").
 * Returns 0 on success (stream_idx_out set), -1 if not found.
 */
int zuuned_probe_audio_lang(const char *filepath, const char *lang, int *stream_idx_out);

/*
 * Probe full metadata from any media file.
 * Populates a ZuneMetadata struct (title, artist, album, genre, track#, duration).
 * Returns 0 on success, -1 on failure.
 */
int zuuned_probe_metadata(const char *filepath, struct ZuneMetadata *out);

/* ---- Album Art Extraction ---- */

/*
 * Extract embedded album art from a media file, save as JPEG.
 * Scales to max_dimension x max_dimension (preserving aspect ratio).
 * Returns 0 on success, -1 if no art found or extraction failed.
 */
int zuuned_extract_art(const char *filepath, const char *output_jpeg_path,
                        int max_dimension);

/*
 * Decode one video frame at at_seconds (clamped to the file's duration),
 * scale to max_width (preserving aspect ratio), save as JPEG.
 * Used for episode-thumbnail fallbacks when no TMDB still exists.
 * Returns 0 on success, -1 on failure.
 */
int zuuned_extract_frame(const char *filepath, double at_seconds,
                         const char *output_jpeg_path, int max_width);

#ifdef __cplusplus
}
#endif

#endif /* LIBAV_TRANSCODE_H */
