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
 * WMA Lossless is not one of these targets, and it is not an omission that more
 * code would fix.  Measured against the FFmpeg this application links:
 * `ffmpeg -encoders` lists `wmav1` and `wmav2` as the only WMA encoders, with
 * `wmalossless` and `wmapro` under decoders only, and Windows itself offers no
 * WMA lossless ACM codec to reach instead (System32 carries atrac3, the two
 * ADPCMs, l3codeca/l3codecp and two telephony codecs).  So a lossless WMA the
 * user already owns is carried byte-for-byte by the sync engine rather than
 * converted: every encoder that exists here would be a downgrade.  `wmav2` also
 * has no variable-bitrate mode, which is why the VBR option is MP3-only.
 */
#define ZUUNED_AUDIO_MP3 0
#define ZUUNED_AUDIO_WMA 1

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
