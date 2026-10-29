/*
 * transcodetool — CLI harness for the Zuuned transcode layer (Phase 2 gate)
 *
 * Exercises every exported function against real media without the app:
 *
 *   transcodetool audio <in>                 → MP3 320 CBR (prints path)
  transcodetool audio-cbr <in> <kbps>      → MP3 at a chosen CBR rate
  transcodetool audio-vbr <in> <kbps>      → MP3 VBR capped at <kbps>
  transcodetool audio-wma <in> [kbps]      → WMA (ASF) through libavcodec
  transcodetool audio-wma-lossless <in>    WMA Lossless through the encoder in the
                                           Windows Media Format runtime (Windows
                                           only; every other platform refuses)
  transcodetool encoders                   → what this build can encode
 *   transcodetool video <in> <profile 0-3>   → Zune video (prints path)
 *   transcodetool probe <in>                 → duration + resolution
 *   transcodetool art   <in> <out.jpg>       → embedded album art
 *   transcodetool frame <in> <sec> <out.jpg> → video frame grab
 *   transcodetool retag <in.mp3> <albumartist> → ID3v2.3 rewrite (prints path)
 *   transcodetool copycheck               → the native-copy rule as a table
 *   transcodetool retagcheck              → the ID3v2.3 rewrite, byte for byte
 *
 * ZUUNED_HW_DECODE=0|1|2|3 picks the decode path a transcode uses (0 sw,
 * 1 auto → NVDEC then VAAPI then sw, 2 NVDEC only, 3 VAAPI only), which is the
 * same knob Settings' transcodeAccel drives in the app.  A gate has to reach both
 * the hardware path and the software fallback on one machine; the library default
 * is auto and shows it only one of them.
 */

#include "libav_transcode.h"
#include "id3_rewrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

/* The CRT hands main() its argv in the ANSI code page, so on a machine whose
 * ACP cannot spell the name every argument arrives as question marks and probe,
 * art and frame all fail on a file the application opens happily.  The library
 * is called with UTF-8 by the app itself (FrameThumbnailService,
 * LibraryService, AudioCopyFingerprint all pass toUtf8()), so for this tool to
 * gate what the app does its arguments have to be the same bytes: rebuild argv
 * from the wide command line as UTF-8, which is what QCoreApplication does for
 * the Qt-side tools. */
static char **utf8_argv(int *count)
{
    int wide_argc = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide || wide_argc <= 0)
        return NULL;

    char **vec = calloc((size_t)wide_argc + 1, sizeof(char *));
    if (!vec) {
        LocalFree(wide);
        return NULL;
    }
    for (int i = 0; i < wide_argc; i++) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1,
                                             NULL, 0, NULL, NULL);
        if (need <= 0)
            goto fail;
        char *utf8 = malloc((size_t)need);
        if (!utf8)
            goto fail;
        if (WideCharToMultiByte(CP_UTF8, 0, wide[i], -1,
                                utf8, need, NULL, NULL) <= 0) {
            free(utf8);
            goto fail;
        }
        vec[i] = utf8;
    }
    vec[wide_argc] = NULL;
    LocalFree(wide);
    *count = wide_argc;
    return vec;

fail:
    for (int i = 0; i < wide_argc; i++)
        free(vec[i]);
    free(vec);
    LocalFree(wide);
    return NULL;
}
#endif

static void progress_cb(float fraction, void *userdata)
{
    (void)userdata;
    static int last_pct = -1;
    int pct = (int)(fraction * 100.0f);
    if (pct / 10 != last_pct / 10 || pct >= 100) {
        fprintf(stderr, "[progress] %d%%\n", pct);
        last_pct = pct;
    }
}

/* ---- retagcheck: the ID3v2.3 rewrite, gated on bytes ----
 *
 * A tag rewrite is a promise about bytes: what a Zune can read, what a Chinese
 * album name turns into, and that the audio on either side of the tag is the
 * same audio.  None of that needs a player or an encoder, so it is gated here
 * against a fixture built one byte at a time and read back one byte at a time.
 *
 * The expected payloads are written out as bytes rather than computed, because
 * a test that re-runs the code under test to predict it proves nothing.
 */
static int rc_checks;
static int rc_failures;

static void rc_check(int ok, const char *what)
{
    ++rc_checks;
    if (!ok) {
        printf("retagcheck FAIL: %s\n", what);
        ++rc_failures;
    }
}

typedef struct { unsigned char *b; size_t n; size_t cap; } rc_buf;

static void rc_put(rc_buf *f, const void *p, size_t n)
{
    if (f->n + n > f->cap) return;   /* the fixture is fixed size; overflow is a bug */
    memcpy(f->b + f->n, p, n);
    f->n += n;
}

static void rc_be32(rc_buf *f, unsigned v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
    rc_put(f, b, 4);
}

static void rc_syncsafe(rc_buf *f, unsigned v)
{
    unsigned char b[4];
    b[0] = (unsigned char)((v >> 21) & 0x7F); b[1] = (unsigned char)((v >> 14) & 0x7F);
    b[2] = (unsigned char)((v >> 7) & 0x7F);  b[3] = (unsigned char)(v & 0x7F);
    rc_put(f, b, 4);
}

static void rc_frame(rc_buf *f, const char *id, const unsigned char *payload, size_t n)
{
    rc_put(f, id, 4);
    rc_be32(f, (unsigned)n);
    rc_put(f, "\0\0", 2);
    rc_put(f, payload, n);
}

static char *rc_scratch(const char *tag)
{
    char path[512];
#ifdef _WIN32
    const char *dir = getenv("TEMP");
    if (!dir || !*dir) dir = ".";
    const size_t dl = strlen(dir);
    const char sep = (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) ? '\0' : '\\';
    snprintf(path, sizeof(path), "%s%c%s_XXXXXX.mp3", dir, sep, tag);
#else
    snprintf(path, sizeof(path), "/tmp/%s_XXXXXX.mp3", tag);
#endif
    const int fd = mkstemps(path, 4);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(path);
}

static int rc_write(const char *path, const unsigned char *b, size_t n)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    const size_t wrote = fwrite(b, 1, n, fp);
    if (fclose(fp) != 0) return 0;
    return wrote == n;
}

static unsigned char *rc_read(const char *path, size_t *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    const long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    unsigned char *b = malloc((size_t)sz);
    if (!b) { fclose(fp); return NULL; }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) { free(b); fclose(fp); return NULL; }
    fclose(fp);
    *n = (size_t)sz;
    return b;
}

static int cmd_retagcheck(void)
{
    /* The four frames a source tag actually arrives in: a v2.4 UTF-8 frame in a
     * v2.3 tag, an honest ASCII frame, an 0x00 frame that holds UTF-8 (what
     * FFmpeg writes), and an UTF-16 frame that is already right. */
    static const unsigned char IN_ALBUM[]  = { 0x03,
        0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87, 0xE4, 0xB8, 0x93, 0xE8, 0xBE, 0x91 };
    static const unsigned char IN_TITLE[]  = { 0x00,
        'P', 'l', 'a', 'i', 'n', ' ', 'T', 'i', 't', 'l', 'e' };
    static const unsigned char IN_ARTIST[] = { 0x00,
        0xE5, 0x8D, 0x8E, 0xE8, 0xAF, 0xAD, 0xE6, 0xAD, 0x8C, 0xE6, 0x89, 0x8B };
    static const unsigned char IN_GENRE[]  = { 0x01, 0xFF, 0xFE,
        0x47, 0x64, 0xDA, 0x6E, 0x00, 0x00 };
    /* And what v2.3 has to say about each: UTF-16LE with a mark for anything
     * outside Latin-1, and the ASCII frame untouched down to its last byte. */
    static const unsigned char OUT_ALBUM[] = { 0x01, 0xFF, 0xFE,
        0x2D, 0x4E, 0x87, 0x65, 0x13, 0x4E, 0x91, 0x8F, 0x00, 0x00 };
    static const unsigned char OUT_ARTIST[] = { 0x01, 0xFF, 0xFE,
        0x4E, 0x53, 0xED, 0x8B, 0x4C, 0x6B, 0x4B, 0x62, 0x00, 0x00 };
    static const unsigned char OUT_TITLE[] = { 0x00,
        'P', 'l', 'a', 'i', 'n', ' ', 'T', 'i', 't', 'l', 'e' };
    static const unsigned char OUT_GENRE[] = { 0x01, 0xFF, 0xFE,
        0x47, 0x64, 0xDA, 0x6E, 0x00, 0x00 };
    /* "new album", handed down as UTF-8 by the Qt side through the edit path. */
    static const char NEW_ALBUM[] = "\xE6\x96\xB0\xE4\xB8\x93\xE8\xBE\x91";
    static const unsigned char OUT_ALBUM2[] = { 0x01, 0xFF, 0xFE,
        0xB0, 0x65, 0x13, 0x4E, 0x91, 0x8F, 0x00, 0x00 };
    static const unsigned char OUT_DISC[] = { 0x00, '3' };

    /* Fake audio, deliberately past the copy chunk so the streaming loop runs
     * more than once and the byte-for-byte comparison below is about the loop. */
    const size_t audio_len = 600u * 1024u;
    unsigned char *audio = malloc(audio_len);
    unsigned char body[4096];
    rc_buf tag = { body, 0, sizeof(body) };
    size_t n = 0, out_n = 0;
    unsigned char *out = NULL;
    char *src = rc_scratch("zuuned_retag_fixture");
    char *made = NULL;
    int i = 0;

    if (!audio || !src) {
        printf("retagcheck 1 checks 1 failures\n");
        free(audio);
        free(src);
        return 1;
    }
    for (i = 0; (size_t)i < audio_len; i++) audio[i] = (unsigned char)((i * 31 + 7) & 0xFF);
    audio[0] = 0xFF; audio[1] = 0xFB; audio[2] = 0x90; audio[3] = 0x64;

    rc_frame(&tag, "TALB", IN_ALBUM, sizeof(IN_ALBUM));
    rc_frame(&tag, "TIT2", IN_TITLE, sizeof(IN_TITLE));
    rc_frame(&tag, "TPE1", IN_ARTIST, sizeof(IN_ARTIST));
    rc_frame(&tag, "TCON", IN_GENRE, sizeof(IN_GENRE));
    {
        unsigned char pad[64];
        memset(pad, 0, sizeof(pad));
        rc_put(&tag, pad, sizeof(pad));   /* the padding a tagger leaves behind */
    }

    {
        rc_buf file = { malloc(10 + tag.n + audio_len + 128), 0,
                        10 + tag.n + audio_len + 128 };
        unsigned char v1[128];
        rc_check(file.b != NULL, "fixture buffer allocated");
        if (!file.b) goto done;
        rc_put(&file, "ID3", 3);
        rc_put(&file, "\x03\x00\x00", 3);
        rc_syncsafe(&file, (unsigned)tag.n);
        rc_put(&file, body, tag.n);
        rc_put(&file, audio, audio_len);
        memset(v1, 0x20, sizeof(v1));
        memcpy(v1, "TAG", 3);
        rc_put(&file, v1, sizeof(v1));    /* an ID3v1 to drop and rewrite */
        rc_check(file.n == 10 + tag.n + audio_len + 128, "fixture assembled");
        rc_check(rc_write(src, file.b, file.n), "fixture written");
        free(file.b);
    }

    made = zuuned_retag_mp3_with_disc(src, NULL, 3);
    rc_check(made != NULL, "retag of a mis-encoded tag produced a file");
    out = made ? rc_read(made, &out_n) : NULL;
    rc_check(out != NULL, "the retagged file reads back");
    if (!out) goto done;

    rc_check(out_n > 10 && memcmp(out, "ID3", 3) == 0 && out[3] == 3,
             "the output tag is ID3v2.3");
    {
        const size_t tag_len = ((size_t)out[6] << 21) | ((size_t)out[7] << 14)
                             | ((size_t)out[8] << 7) | (size_t)out[9];
        size_t pos = 10;
        int seen = 0, bad_encoding = 0;
        const unsigned char *f[8];
        size_t fs[8];
        memset(f, 0, sizeof(f));
        memset(fs, 0, sizeof(fs));
        while (pos + 10 <= 10 + tag_len && out[pos] != 0) {
            char id[5];
            memcpy(id, out + pos, 4);
            id[4] = '\0';
            const size_t sz = ((size_t)out[pos + 4] << 24) | ((size_t)out[pos + 5] << 16)
                            | ((size_t)out[pos + 6] << 8) | (size_t)out[pos + 7];
            if (sz == 0 || pos + 10 + sz > 10 + tag_len) break;
            ++seen;
            if (id[0] == 'T' && out[pos + 10] != 0x00 && out[pos + 10] != 0x01)
                bad_encoding = 1;
            if (strcmp(id, "TALB") == 0) { f[0] = out + pos + 10; fs[0] = sz; }
            else if (strcmp(id, "TIT2") == 0) { f[1] = out + pos + 10; fs[1] = sz; }
            else if (strcmp(id, "TPE1") == 0) { f[2] = out + pos + 10; fs[2] = sz; }
            else if (strcmp(id, "TCON") == 0) { f[3] = out + pos + 10; fs[3] = sz; }
            else if (strcmp(id, "TPOS") == 0) { f[4] = out + pos + 10; fs[4] = sz; }
            pos += 10 + sz;
        }
        rc_check(seen == 5, "five text frames came out");
        rc_check(!bad_encoding, "every text frame declares 0x00 or 0x01");
        rc_check(fs[0] == sizeof(OUT_ALBUM)
                 && memcmp(f[0], OUT_ALBUM, sizeof(OUT_ALBUM)) == 0,
                 "a UTF-8-marked frame becomes marked UTF-16, not mojibake");
        rc_check(fs[1] == sizeof(OUT_TITLE)
                 && memcmp(f[1], OUT_TITLE, sizeof(OUT_TITLE)) == 0,
                 "an ASCII frame keeps its exact bytes");
        rc_check(fs[2] == sizeof(OUT_ARTIST)
                 && memcmp(f[2], OUT_ARTIST, sizeof(OUT_ARTIST)) == 0,
                 "an 0x00 frame holding UTF-8 is re-read as UTF-8");
        rc_check(fs[3] == sizeof(OUT_GENRE)
                 && memcmp(f[3], OUT_GENRE, sizeof(OUT_GENRE)) == 0,
                 "a correct UTF-16 frame is left as it was");
        rc_check(fs[4] == sizeof(OUT_DISC)
                 && memcmp(f[4], OUT_DISC, sizeof(OUT_DISC)) == 0,
                 "the queued disc number is published as one ASCII frame");

        /* Streaming, proved the only way it can be: nothing changed. */
        {
            const size_t head = 10 + tag_len;
            rc_check(out_n > head + 128
                     && out_n - head - 128 == audio_len
                     && memcmp(out + head, audio, audio_len) == 0,
                     "600 KB of audio copied byte for byte through the chunk loop");
            rc_check(memcmp(out + out_n - 128, "TAG", 3) == 0,
                     "an ID3v1.1 shadow was written at the end");
            rc_check(memcmp(out + out_n - 125, "Plain Title", 11) == 0,
                     "the ID3v1 title still came out of the frames");
        }
    }

    /* The in-place edit path, with the album name a Chinese user typed. */
    {
        ZuunedId3Edits e;
        memset(&e, 0, sizeof(e));
        e.album = NEW_ALBUM;
        rc_check(zuuned_edit_mp3_tags(made, &e) == 0,
                 "an in-place edit of the retagged file returns 0");
        free(out);
        out = rc_read(made, &out_n);
        rc_check(out != NULL, "the edited file reads back");
        if (out) {
            const size_t tag_len = ((size_t)out[6] << 21) | ((size_t)out[7] << 14)
                                 | ((size_t)out[8] << 7) | (size_t)out[9];
            int found = 0;
            size_t pos = 10;
            while (pos + 10 <= 10 + tag_len && out[pos] != 0) {
                char id[5];
                memcpy(id, out + pos, 4);
                id[4] = '\0';
                const size_t sz = ((size_t)out[pos + 4] << 24) | ((size_t)out[pos + 5] << 16)
                                | ((size_t)out[pos + 6] << 8) | (size_t)out[pos + 7];
                if (sz == 0 || pos + 10 + sz > 10 + tag_len) break;
                if (strcmp(id, "TALB") == 0
                    && sz == sizeof(OUT_ALBUM2)
                    && memcmp(out + pos + 10, OUT_ALBUM2, sz) == 0)
                    found = 1;
                pos += 10 + sz;
            }
            rc_check(found, "a typed Chinese album name is stored as UTF-16LE");
            rc_check(out_n > 10 + tag_len + 128,
                     "the edited file still carries its audio");
        }
    }

    /* A file with no tag at all still gains one when the sync supplies an
     * artist, which is the case the ordinary send path runs. */
    {
        char *bare = rc_scratch("zuuned_retag_bare");
        if (bare) {
            rc_check(rc_write(bare, audio, audio_len), "a bare-audio fixture written");
            char *made2 = zuuned_retag_mp3(bare, "Someone");
            unsigned char *o2 = NULL;
            size_t n2 = 0;
            rc_check(made2 != NULL, "a bare audio file gains an ID3v2.3 tag");
            if (made2) {
                o2 = rc_read(made2, &n2);
                rc_check(o2 != NULL && n2 > 10 && memcmp(o2, "ID3", 3) == 0
                         && o2[3] == 3, "the new tag is v2.3");
                /* header(10) + frame header(10) puts the encoding byte at 20 */
                rc_check(o2 != NULL && n2 > 28 && memcmp(o2 + 10, "TPE1", 4) == 0
                         && o2[20] == 0x00
                         && memcmp(o2 + 21, "Someone", 7) == 0,
                         "the supplied artist is the first frame, in Latin-1");
                rc_check(o2 != NULL
                         && n2 == 10
                            + ((((size_t)o2[6] << 21) | ((size_t)o2[7] << 14)
                                | ((size_t)o2[8] << 7) | (size_t)o2[9]))
                            + audio_len + 128,
                         "a tagless file keeps its audio and gains only the tag");
                free(o2);
                remove(made2);
                free(made2);
            }
            remove(bare);
            free(bare);
        }
    }

    /* And a file that is not an MP3 is refused rather than half-written. */
    {
        char *junk_name = rc_scratch("zuuned_retag_junk");
        if (junk_name) {
            unsigned char junk[4096];
            memset(junk, 0x11, sizeof(junk));
            rc_check(rc_write(junk_name, junk, sizeof(junk)), "a non-MP3 written");
            char *none = zuuned_retag_mp3(junk_name, NULL);
            rc_check(none == NULL, "a file with no MPEG sync is refused");
            free(none);
            remove(junk_name);
            free(junk_name);
        }
    }

done:
    free(out);
    if (made) { remove(made); free(made); }
    remove(src);
    free(src);
    free(audio);
    printf("retagcheck %d checks %d failures\n", rc_checks, rc_failures);
    return rc_failures ? 1 : 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  transcodetool audio <in>\n"
        "  transcodetool audio-cbr <in> <kbps>\n"
        "  transcodetool audio-vbr <in> <kbps>\n"
        "  transcodetool audio-wma <in> [kbps]\n"
        "  transcodetool audio-wma-lossless <in>\n"
        "  transcodetool encoders\n"
        "  transcodetool video <in> <profile 0-3>\n"
        "  transcodetool probe <in>\n"
        "  transcodetool container <in>\n"
        "  transcodetool art <in> <out.jpg>\n"
        "  transcodetool frame <in> <seconds> <out.jpg>\n"
        "  transcodetool retag <in.mp3> <albumartist>\n"
        "  transcodetool copycheck\n"
        "  transcodetool retagcheck\n");
    return 2;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    {
        int utf8_argc = 0;
        char **wide_argv = utf8_argv(&utf8_argc);
        if (wide_argv) {
            argv = wide_argv;
            argc = utf8_argc;
        }
    }
#endif
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    const char *in = argc >= 3 ? argv[2] : NULL;

    /* Every command but the three that ask about this build names a file.  Ask
     * for one and pass nothing and the answer is a usage error, not an access
     * violation: this binary is a gate, and a gate that dies on a null path
     * reports nothing about the thing it was asked to check. */
    if (!in && strcmp(cmd, "encoders") != 0 && strcmp(cmd, "copycheck") != 0
        && strcmp(cmd, "retagcheck") != 0)
        return usage();

    const char *hw = getenv("ZUUNED_HW_DECODE");
    if (hw && *hw)
        zuuned_transcode_set_hw_decode(atoi(hw));

    if (strcmp(cmd, "audio") == 0) {
        char *out = zuuned_transcode_audio(in, NULL, progress_cb, NULL);
        if (!out) { fprintf(stderr, "FAIL: audio transcode\n"); return 1; }
        printf("%s\n", out);
        free(out);
        return 0;
    }

    if (strcmp(cmd, "encoders") == 0) {
        /* What the linked libraries can actually do.  A settings page must not
         * offer a format this build cannot make, and only the linked libav*
         * knows: the ffmpeg on PATH is a different binary. */
        printf("ffmpeg=%s\n", zuuned_ffmpeg_version());
        printf("mp3=%d\n", zuuned_audio_encoder_available(ZUUNED_AUDIO_MP3));
        printf("wma=%d\n", zuuned_audio_encoder_available(ZUUNED_AUDIO_WMA));
        /* Not an FFmpeg answer.  The lossless encoder belongs to the Windows Media
         * Format runtime, so this line reports the operating system rather than
         * the linked libraries, and off Windows it can only ever say 0. */
        printf("wma-lossless=%d\n",
               zuuned_audio_encoder_available(ZUUNED_AUDIO_WMA_LOSSLESS));
        return 0;
    }

    if (strcmp(cmd, "audio-cbr") == 0 || strcmp(cmd, "audio-vbr") == 0) {
        if (argc < 4) return usage();
        const int vbr = strcmp(cmd, "audio-vbr") == 0 ? 1 : 0;
        char *out = zuuned_transcode_audio_ex(in, NULL, 0, ZUUNED_AUDIO_MP3,
                                              atoi(argv[3]), vbr, progress_cb, NULL);
        if (!out) { fprintf(stderr, "FAIL: audio transcode\n"); return 1; }
        printf("%s\n", out);
        free(out);
        return 0;
    }

    if (strcmp(cmd, "audio-wma") == 0) {
        const int kbps = argc >= 4 ? atoi(argv[3]) : 128;
        char *out = zuuned_transcode_audio_ex(in, NULL, 0, ZUUNED_AUDIO_WMA,
                                              kbps, 0, progress_cb, NULL);
        if (!out) { fprintf(stderr, "FAIL: wma encode\n"); return 1; }
        printf("%s\n", out);
        free(out);
        return 0;
    }

    if (strcmp(cmd, "audio-wma-lossless") == 0) {
        /* No bitrate on purpose - the format has none, and a knob here would
         * invite a gate that believed it had configured a lossless encode. */
        char *out = zuuned_transcode_audio_ex(in, NULL, 0,
                                              ZUUNED_AUDIO_WMA_LOSSLESS, 0, 0,
                                              progress_cb, NULL);
        if (!out) { fprintf(stderr, "FAIL: wma-lossless encode\n"); return 1; }
        printf("%s\n", out);
        free(out);
        return 0;
    }

    if (strcmp(cmd, "video") == 0) {
        if (argc < 4) return usage();
        int profile = atoi(argv[3]);
        char *out = zuuned_transcode_video(in, profile, NULL, 0, 0, NULL,
                                           progress_cb, NULL);
        if (!out) { fprintf(stderr, "FAIL: video transcode (profile %d)\n", profile); return 1; }
        printf("%s\n", out);
        free(out);
        return 0;
    }

    /* A WAVEFORMATEX tag alone does not make a file copyable, and neither does a
     * file that exists: the rule below is a proof about the numbers a probe
     * returns, so it gets gated as a table of numbers rather than as a file that
     * happens to be on this machine.  No demuxer runs here - that half is the
     * `container` command, and the two halves together are the copy rule. */
    if (strcmp(cmd, "copycheck") == 0) {
        static const int rows[][9] = {
            /* target, probed, asf, audio, video, art, lossless, decoder, want */
            { ZUUNED_AUDIO_WMA_LOSSLESS, 1, 1, 1, 0, 0, 1, 1, ZUUNED_NATIVE_COPY_OK },
            { ZUUNED_AUDIO_WMA_LOSSLESS, 1, 1, 1, 0, 1, 1, 1, ZUUNED_NATIVE_COPY_OK },
            { ZUUNED_AUDIO_WMA_LOSSLESS, 1, 1, 1, 0, 1, 0, 1, ZUUNED_COPY_NOT_LOSSLESS },
            { ZUUNED_AUDIO_WMA_LOSSLESS, 1, 1, 1, 1, 0, 1, 1, ZUUNED_COPY_HAS_VIDEO },
            { ZUUNED_AUDIO_WMA_LOSSLESS, 1, 1, 1, 0, 0, 0, 1, ZUUNED_COPY_NOT_LOSSLESS },
            { ZUUNED_AUDIO_WMA_LOSSLESS, 1, 0, 1, 0, 0, 1, 1, ZUUNED_COPY_NOT_ASF },
            { ZUUNED_AUDIO_WMA_LOSSLESS, 0, 1, 1, 0, 0, 1, 1, ZUUNED_COPY_NOT_PROBED },
            { ZUUNED_AUDIO_WMA, 1, 1, 1, 0, 1, 1, 1, ZUUNED_NATIVE_COPY_OK },
            { ZUUNED_AUDIO_WMA, 1, 1, 1, 0, 0, 0, 1, ZUUNED_NATIVE_COPY_OK },
            { ZUUNED_AUDIO_WMA, 1, 1, 1, 1, 0, 0, 1, ZUUNED_COPY_HAS_VIDEO },
            { ZUUNED_AUDIO_WMA, 1, 1, 0, 0, 0, 0, 1, ZUUNED_COPY_NO_DECODABLE_AUDIO },
            { ZUUNED_AUDIO_WMA, 1, 1, 1, 0, 0, 0, 0, ZUUNED_COPY_NO_DECODABLE_AUDIO },
            { ZUUNED_AUDIO_MP3, 1, 1, 1, 0, 1, 1, 1, ZUUNED_COPY_TARGET_NOT_WMA }
        };
        int failures = 0;
        for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
            zuuned_audio_container c;
            c.probed = rows[i][1]; c.is_asf = rows[i][2];
            c.audio_streams = rows[i][3]; c.video_streams = rows[i][4];
            c.art_streams = rows[i][5]; c.lossless = rows[i][6];
            c.has_decoder = rows[i][7];
            int want = rows[i][8];
            int got = zuuned_native_copy_verdict(&c, rows[i][0]);
            if (got != want) {
                printf("row %u target=%d got=%d want=%d FAIL\n",
                       i, rows[i][0], got, want);
                ++failures;
            }
        }
        {
            zuuned_audio_container c;
            c.probed = 1; c.is_asf = 1; c.audio_streams = 1;
            c.video_streams = 0; c.art_streams = 0; c.lossless = 1;
            c.has_decoder = 1;
            int got = zuuned_native_copy_verdict(NULL, ZUUNED_AUDIO_WMA_LOSSLESS);
            if (got != ZUUNED_COPY_NOT_PROBED) {
                printf("null probe got=%d want=%d FAIL\n", got, ZUUNED_COPY_NOT_PROBED);
                ++failures;
            }
        }
        printf("copycheck %d checks %d failures\n",
               (int)(sizeof(rows) / sizeof(rows[0])) + 1, failures);
        return failures ? 1 : 0;
    }

    if (strcmp(cmd, "retagcheck") == 0)
        return cmd_retagcheck();

    if (strcmp(cmd, "probe") == 0) {
        double dur = 0;
        int w = 0, h = 0;
        int vi = zuuned_probe_video_info(in, &dur, &w, &h);
        double d2 = 0;
        int di = zuuned_probe_duration(in, &d2);
        if (vi != 0 && di != 0) { fprintf(stderr, "FAIL: probe\n"); return 1; }
        printf("duration=%.3f\n", dur > 0 ? dur : d2);
        if (w > 0) printf("video=%dx%d\n", w, h);
        return 0;
    }

    /* Name what the container holds, which is the question a native copy has to
     * answer before it decides a file may travel byte for byte.  Everything is
     * printed as a number so a gate can compare it without parsing prose. */
    if (strcmp(cmd, "container") == 0) {
        zuuned_audio_container c;
        if (zuuned_probe_audio_container(in, &c) != 0) {
            fprintf(stderr, "FAIL: container probe\n");
            return 1;
        }
        printf("probed=%d asf=%d audio=%d video=%d art=%d codec=%d name=%s "
               "tag=0x%04x lossless=%d decoder=%d channels=%d rate=%d bits=%d duration=%lld "
               "copy_wma=%d copy_wma_lossless=%d\n",
               c.probed, c.is_asf, c.audio_streams, c.video_streams, c.art_streams,
               c.codec_id, c.codec_name, c.format_tag, c.lossless, c.has_decoder,
               c.channels, c.sample_rate, c.bits_per_sample, (long long)c.duration_ms,
               zuuned_native_copy_verdict(&c, ZUUNED_AUDIO_WMA),
               zuuned_native_copy_verdict(&c, ZUUNED_AUDIO_WMA_LOSSLESS));
        return 0;
    }

    if (strcmp(cmd, "art") == 0) {
        if (argc < 4) return usage();
        if (zuuned_extract_art(in, argv[3], 400) != 0) {
            fprintf(stderr, "FAIL: no embedded art\n");
            return 1;
        }
        printf("%s\n", argv[3]);
        return 0;
    }

    if (strcmp(cmd, "frame") == 0) {
        if (argc < 5) return usage();
        if (zuuned_extract_frame(in, atof(argv[3]), argv[4], 400) != 0) {
            fprintf(stderr, "FAIL: frame extract\n");
            return 1;
        }
        printf("%s\n", argv[4]);
        return 0;
    }

    if (strcmp(cmd, "retag") == 0) {
        if (argc < 4) return usage();
        char *out = zuuned_retag_mp3(in, argv[3]);
        if (!out) { fprintf(stderr, "FAIL: retag\n"); return 1; }
        printf("%s\n", out);
        free(out);
        return 0;
    }

    return usage();
}
