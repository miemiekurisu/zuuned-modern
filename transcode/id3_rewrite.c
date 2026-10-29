/*
 * id3_rewrite.c — Pure C ID3v2.3 tag rewriter
 *
 * Reads ID3v2.x (v2.2/v2.3/v2.4) tags from an MP3, rewrites as ID3v2.3 + ID3v1.1.
 * Audio data is byte-copied (no re-encoding). Zero external dependencies.
 *
 * The Zune ignores ID3v2.4 tags entirely. This is the fix.
 */

#include "id3_rewrite.h"
#include "disc_tag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* Scratch files live in /tmp on POSIX.  The Windows compatibility layer answers
 * with a writable directory that already ends in a separator, and the macro
 * expands to a function call, so the name is built through "%s". */
#ifndef ZUUNED_TMPDIR
#define ZUUNED_TMPDIR "/tmp/"
#endif

/* ---- Constants ---- */

#define ID3V2_HEADER_SIZE  10
#define ID3V1_TAG_SIZE     128
#define MAX_TAG_SIZE       (16 * 1024 * 1024)  /* 16 MB sanity limit */
#define MAX_FRAMES         256
#define PADDING_SIZE       1024  /* 1 KB padding after frames */

/* ---- ID3v2 frame storage ---- */

typedef struct {
    char     id[5];      /* Frame ID: "TIT2", "TPE1", etc. (NUL-terminated) */
    uint8_t *data;       /* Raw frame data (after frame header) */
    uint32_t size;       /* Size of data */
    uint16_t flags;      /* Frame flags */
} ID3Frame;

typedef struct {
    char     title[256];
    char     artist[256];
    char     album[256];
    char     genre[256];
    char     year[8];
    uint16_t track;
} ID3v1Info;

/* ---- Syncsafe integer decode/encode ---- */

static uint32_t decode_syncsafe(const uint8_t *b)
{
    return ((uint32_t)b[0] << 21) | ((uint32_t)b[1] << 14) |
           ((uint32_t)b[2] << 7)  | (uint32_t)b[3];
}

static void encode_syncsafe(uint32_t val, uint8_t *b)
{
    b[0] = (val >> 21) & 0x7F;
    b[1] = (val >> 14) & 0x7F;
    b[2] = (val >> 7)  & 0x7F;
    b[3] = val & 0x7F;
}

static uint32_t decode_be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static void encode_be32(uint32_t val, uint8_t *b)
{
    b[0] = (val >> 24) & 0xFF;
    b[1] = (val >> 16) & 0xFF;
    b[2] = (val >> 8)  & 0xFF;
    b[3] = val & 0xFF;
}

/* ---- Text frame decoding ---- */

/*
 * Extract a Latin-1/ASCII string from an ID3v2 text frame's data.
 * Handles encoding byte: 0x00=ISO-8859-1, 0x01=UTF-16, 0x02=UTF-16BE, 0x03=UTF-8.
 * For UTF-16, extracts ASCII chars (lossy but sufficient for ID3v1).
 * Returns a malloc'd string. Caller frees.
 */
static char *text_frame_to_latin1(const uint8_t *data, uint32_t size)
{
    if (size < 2) return NULL;

    uint8_t encoding = data[0];
    const uint8_t *text = data + 1;
    uint32_t text_len = size - 1;

    if (encoding == 0x00 || encoding == 0x03) {
        /* ISO-8859-1 or UTF-8 — copy as-is (ASCII subset) */
        uint32_t len = text_len;
        /* Trim trailing NULs */
        while (len > 0 && text[len - 1] == 0) len--;
        char *result = malloc(len + 1);
        if (!result) return NULL;
        memcpy(result, text, len);
        result[len] = '\0';
        return result;
    }

    if (encoding == 0x01 || encoding == 0x02) {
        /* UTF-16 (with or without BOM) — extract ASCII chars */
        const uint8_t *p = text;
        uint32_t remaining = text_len;
        int le = 1; /* default little-endian */

        /* Check for BOM */
        if (remaining >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { le = 1; p += 2; remaining -= 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { le = 0; p += 2; remaining -= 2; }
            else if (encoding == 0x02) { le = 0; } /* UTF-16BE, no BOM */
        }

        uint32_t chars = remaining / 2;
        char *result = malloc(chars + 1);
        if (!result) return NULL;

        uint32_t out = 0;
        for (uint32_t i = 0; i < chars; i++) {
            uint16_t ch = le ? (p[i*2] | (p[i*2+1] << 8)) : ((p[i*2] << 8) | p[i*2+1]);
            if (ch == 0) break;
            result[out++] = (ch < 128) ? (char)ch : '?';
        }
        result[out] = '\0';
        return result;
    }

    return NULL;
}

/* ---- Text frame character encoding ----
 *
 * The tag this file writes is ID3v2.3, and v2.3 defines exactly two text
 * encodings: 0x00 ISO-8859-1 and 0x01 UTF-16 with a byte-order mark.  0x02
 * (UTF-16BE) and 0x03 (UTF-8) exist only from v2.4 on, so any text frame that
 * carries one of them, and any frame holding bytes v2.3 cannot spell, has to
 * be re-expressed before it is written.
 *
 * The case that matters is not a frame marked 0x03, which says what it is.  It
 * is a frame marked 0x00 that holds UTF-8: that is what FFmpeg's id3v2 muxer
 * and most taggers write when they set the v2.3 encoding byte and hand over
 * the library's UTF-8 anyway.  Copying those bytes out as Latin-1 is how a
 * Chinese album title arrives on a Zune as mojibake, and the conversion done
 * here used to be gated on the *tag* being v2.4, so for one of those files it
 * never ran at all.  An 0x00 payload is therefore read as UTF-8 when it
 * decodes cleanly and actually uses more than one byte per character, and as
 * Latin-1 otherwise, which changes nothing for pure ASCII because there the
 * two encodings agree byte for byte.
 *
 * The codecs are written out here rather than pulled in so the transcode layer
 * keeps its zero dependencies, and so a decode failure stays a decision -
 * "leave that frame alone" - instead of becoming a guess.
 */
#define TEXT_CODE_POINTS_MAX (1u << 20)

/*
 * Decode one UTF-8 sequence.  Strict: an overlong form, an encoded surrogate
 * half or anything past U+10FFFF is rejected, because a tag that needs a
 * lenient decoder is a tag whose bytes cannot be trusted to be re-encoded.
 * *adv is one byte even on failure so a caller asking "is this UTF-8" cannot
 * stall on the byte that refused to decode.
 */
static int utf8_next(const uint8_t *p, size_t left, uint32_t *cp, size_t *adv)
{
    *cp = 0;
    *adv = 1;
    if (left == 0) return 0;

    const uint8_t c = p[0];
    if (c < 0x80) { *cp = c; return 1; }

    if ((c & 0xE0) == 0xC0) {
        if (left < 2 || (p[1] & 0xC0) != 0x80) return 0;
        const uint32_t v = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        if (v < 0x80) return 0;                     /* overlong */
        *cp = v; *adv = 2; return 1;
    }
    if ((c & 0xF0) == 0xE0) {
        if (left < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        const uint32_t v = ((uint32_t)(c & 0x0F) << 12)
                         | ((uint32_t)(p[1] & 0x3F) << 6)
                         | (uint32_t)(p[2] & 0x3F);
        if (v < 0x800) return 0;                    /* overlong */
        if (v >= 0xD800 && v <= 0xDFFF) return 0;   /* encoded surrogate */
        *cp = v; *adv = 3; return 1;
    }
    if ((c & 0xF8) == 0xF0) {
        if (left < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80
            || (p[3] & 0xC0) != 0x80) return 0;
        const uint32_t v = ((uint32_t)(c & 0x07) << 18)
                         | ((uint32_t)(p[1] & 0x3F) << 12)
                         | ((uint32_t)(p[2] & 0x3F) << 6)
                         | (uint32_t)(p[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return 0;  /* overlong or unrepresentable */
        *cp = v; *adv = 4; return 1;
    }
    return 0;   /* a byte that cannot start a sequence */
}

/*
 * Decode a whole run of UTF-8 into code points, handing back a malloc'd array.
 * Returns 0 when any byte is not valid UTF-8, so "is this UTF-8" and "what
 * does it say" are one walk over the bytes and cannot disagree.
 */
static int utf8_run_to_code_points(const uint8_t *p, size_t n,
                                   uint32_t **cps, size_t *count)
{
    if (n > TEXT_CODE_POINTS_MAX) return 0;
    uint32_t *out = malloc((n + 1) * sizeof(*out));
    if (!out) return 0;

    size_t produced = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t adv = 1;
        if (!utf8_next(p + i, n - i, &cp, &adv)) { free(out); return 0; }
        out[produced++] = cp;
        i += adv;
    }
    *cps = out;
    *count = produced;
    return 1;
}

/*
 * True when a payload marked 0x00 is better read as UTF-8 than as Latin-1: it
 * has to decode cleanly *and* use more than one byte somewhere, because a run
 * of single-byte characters means the same thing either way and there is
 * nothing to decide.
 */
static int utf8_run_is_distinctive(const uint8_t *p, size_t n)
{
    int multi = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t adv = 1;
        if (!utf8_next(p + i, n - i, &cp, &adv)) return 0;
        if (adv > 1) multi = 1;
        i += adv;
    }
    return multi;
}

/*
 * Decode UTF-16 into code points, either byte order, byte-order mark optional.
 * A surrogate pair becomes the one code point it stands for; an unpaired
 * surrogate becomes U+FFFD rather than the end of the string, because a name
 * that is mostly right still names the album it belongs to.
 */
static int utf16_run_to_code_points(const uint8_t *p, size_t n, int big_endian,
                                    uint32_t **cps, size_t *count)
{
    if (n % 2 != 0) n--;            /* a stray last byte is not a character */
    if (n > TEXT_CODE_POINTS_MAX * 2) return 0;

    int be = big_endian;
    if (n >= 2) {                   /* a mark outranks the declared order */
        if (p[0] == 0xFF && p[1] == 0xFE) { be = 0; p += 2; n -= 2; }
        else if (p[0] == 0xFE && p[1] == 0xFF) { be = 1; p += 2; n -= 2; }
    }

    uint32_t *out = malloc((n / 2 + 1) * sizeof(*out));
    if (!out) return 0;

    size_t produced = 0;
    size_t i = 0;
    while (i + 1 < n) {
        const uint32_t hi = be ? p[i] : p[i + 1];
        const uint32_t lo = be ? p[i + 1] : p[i];
        const uint32_t unit = (hi << 8) | lo;
        i += 2;

        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < n) {
            const uint32_t hi2 = be ? p[i] : p[i + 1];
            const uint32_t lo2 = be ? p[i + 1] : p[i];
            const uint32_t low = (hi2 << 8) | lo2;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                out[produced++] = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
                continue;
            }
        }
        out[produced++] = (unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFDu : unit;
    }
    *cps = out;
    *count = produced;
    return 1;
}

/*
 * Turn a text frame's payload into code points whatever encoding byte it
 * carries.  Returns 0 only when the frame should be left exactly as it is: an
 * encoding mark this reader does not know, or a payload that will not decode.
 */
static int text_payload_to_code_points(const uint8_t *data, uint32_t size,
                                       uint32_t **cps, size_t *count)
{
    if (!data || size < 1) return 0;

    const uint8_t encoding = data[0];
    const uint8_t *p = data + 1;
    size_t n = size - 1;
    uint32_t *out = NULL;
    size_t produced = 0;

    if (encoding == 0x00) {
        while (n > 0 && p[n - 1] == 0) n--;    /* a terminator, not a character */
        if (utf8_run_is_distinctive(p, n)) {
            if (!utf8_run_to_code_points(p, n, &out, &produced)) return 0;
        } else {
            out = malloc((n + 1) * sizeof(*out));
            if (!out) return 0;
            for (size_t i = 0; i < n; i++) out[i] = p[i];
            produced = n;
        }
    } else if (encoding == 0x03) {
        while (n > 0 && p[n - 1] == 0) n--;
        if (!utf8_run_to_code_points(p, n, &out, &produced)) return 0;
    } else if (encoding == 0x01 || encoding == 0x02) {
        if (!utf16_run_to_code_points(p, n, encoding == 0x02, &out, &produced))
            return 0;
    } else {
        return 0;   /* an unknown mark: keep the frame, change nothing */
    }

    /* UTF-16 text is terminated and UTF-8 text often is; a NUL that means
     * something in the middle of a value survives the walk above. */
    while (produced > 0 && out[produced - 1] == 0) produced--;

    *cps = out;
    *count = produced;
    return 1;
}

/*
 * Encode code points as a v2.3 text payload.  Text that fits in Latin-1 goes
 * out as 0x00 with no terminator, which keeps every ASCII tag exactly the
 * bytes it has always been here; anything else goes out as 0x01 UTF-16LE with
 * a mark, a terminator, and code points past U+FFFF as surrogate pairs.  Same
 * two forms and the same choice as id3_text_payload() in libav_transcode.c, so
 * a file written by an encode and a file retagged in place agree on the wire.
 */
static uint8_t *code_points_to_v23_text(const uint32_t *cps, size_t count,
                                        uint32_t *out_size)
{
    if (!cps || count == 0 || count > TEXT_CODE_POINTS_MAX) return NULL;

    int latin1 = 1;
    for (size_t i = 0; i < count; i++) {
        if (cps[i] > 0xFF) { latin1 = 0; break; }
    }

    if (latin1) {
        uint8_t *out = malloc(1 + count);
        if (!out) return NULL;
        out[0] = 0x00;
        for (size_t i = 0; i < count; i++) out[1 + i] = (uint8_t)cps[i];
        *out_size = (uint32_t)(1 + count);
        return out;
    }

    /* One unit a code point, one extra for a surrogate pair, plus the mark and
     * the terminator. */
    uint8_t *out = malloc(3 + count * 2 + 4);
    if (!out) return NULL;

    size_t pos = 0;
    out[pos++] = 0x01;
    out[pos++] = 0xFF;   /* little-endian mark */
    out[pos++] = 0xFE;
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = cps[i];
        if (cp < 0x10000) {
            out[pos++] = (uint8_t)(cp & 0xFF);
            out[pos++] = (uint8_t)((cp >> 8) & 0xFF);
        } else {
            cp -= 0x10000;
            const uint32_t hi = 0xD800 + (cp >> 10);
            const uint32_t lo = 0xDC00 + (cp & 0x3FF);
            out[pos++] = (uint8_t)(hi & 0xFF);
            out[pos++] = (uint8_t)((hi >> 8) & 0xFF);
            out[pos++] = (uint8_t)(lo & 0xFF);
            out[pos++] = (uint8_t)((lo >> 8) & 0xFF);
        }
    }
    out[pos++] = 0;
    out[pos++] = 0;
    *out_size = (uint32_t)pos;
    return out;
}

/*
 * Rewrite one text frame in place into a form v2.3 allows.  A frame that
 * cannot be decoded is left alone: a tag this file does not understand is
 * still likelier to be what the artist meant than anything invented for it.
 */
static void normalize_text_frame_for_v23(ID3Frame *frame)
{
    if (!frame || !frame->data || frame->size < 1) return;

    uint32_t *cps = NULL;
    size_t count = 0;
    if (!text_payload_to_code_points(frame->data, frame->size, &cps, &count))
        return;

    uint32_t new_size = 0;
    uint8_t *payload = code_points_to_v23_text(cps, count, &new_size);
    free(cps);
    if (!payload) return;

    if (new_size == frame->size && memcmp(payload, frame->data, new_size) == 0) {
        free(payload);   /* already exactly the bytes v2.3 wants */
        return;
    }
    free(frame->data);
    frame->data = payload;
    frame->size = new_size;
    frame->flags = 0;
}

/* ---- ID3v2 tag parsing ---- */

/*
 * Parse ID3v2 tag frames from a buffer.
 * Returns number of frames parsed. Frames stored in `frames` array.
 */
static int parse_id3v2_frames(const uint8_t *tag_data, uint32_t tag_size,
                               uint8_t version_major, ID3Frame *frames, int max_frames)
{
    int count = 0;
    uint32_t pos = 0;

    while (pos + 10 <= tag_size && count < max_frames) {
        /* Check for padding (all zeros) */
        if (tag_data[pos] == 0) break;

        /* Frame ID: 4 printable ASCII chars */
        if (tag_data[pos] < 0x20 || tag_data[pos] > 0x7E) break;

        char id[5];
        memcpy(id, tag_data + pos, 4);
        id[4] = '\0';
        pos += 4;

        /* Frame size */
        uint32_t frame_size;
        if (version_major == 4) {
            frame_size = decode_syncsafe(tag_data + pos);
        } else {
            frame_size = decode_be32(tag_data + pos);
        }
        pos += 4;

        /* Frame flags */
        uint16_t frame_flags = ((uint16_t)tag_data[pos] << 8) | tag_data[pos + 1];
        pos += 2;

        /* Sanity check */
        if (frame_size == 0 || frame_size > tag_size - pos) break;

        /* Store frame */
        frames[count].data = malloc(frame_size);
        if (!frames[count].data) break;
        memcpy(frames[count].data, tag_data + pos, frame_size);
        frames[count].size = frame_size;
        frames[count].flags = frame_flags;
        memcpy(frames[count].id, id, 5);

        /* Convert v2.4 frame IDs to v2.3 equivalents */
        if (version_major == 4) {
            if (strcmp(id, "TDRC") == 0) {
                memcpy(frames[count].id, "TYER", 5);
                /* TDRC format: "YYYY-MM-DD..." → truncate to "YYYY" for TYER */
                if (frames[count].size > 5) {
                    /* encoding byte + at least 4 chars */
                    uint8_t enc = frames[count].data[0];
                    if (enc == 0x00 || enc == 0x03) {
                        /* ASCII/UTF-8: truncate to first 4 chars after encoding byte */
                        if (frames[count].size > 5)
                            frames[count].size = 5; /* encoding + "YYYY" */
                    }
                }
            }
        }

        pos += frame_size;
        count++;
    }

    return count;
}

/* ---- Find MPEG sync word ---- */

/*
 * Find the first MPEG audio sync word in the buffer.
 * MPEG sync: 0xFF followed by 0xE0-0xFF (11 sync bits set).
 * Returns offset from start, or -1 if not found.
 */
static long find_mpeg_sync(const uint8_t *buf, long size)
{
    for (long i = 0; i + 1 < size; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            /* Verify it's a valid MPEG frame header */
            uint8_t version = (buf[i + 1] >> 3) & 0x03;
            uint8_t layer   = (buf[i + 1] >> 1) & 0x03;
            if (version != 0x01 && layer != 0x00) {
                return i;
            }
        }
    }
    return -1;
}

/* ---- Write ID3v2.3 tag ---- */

/*
 * Every write is checked.  A rewrite that quietly lost the second half of the
 * audio is worse than one that failed outright, because by then the caller has
 * been handed a path and a size to trust.
 */
static int write_all(FILE *fp, const void *data, size_t n)
{
    if (n == 0) return 1;
    return fwrite(data, 1, n, fp) == n;
}

/* Read n bytes at an absolute offset and report how many arrived - a short read
 * at the end of the file is itself the answer the caller is asking for. */
static size_t read_at(FILE *fp, long offset, uint8_t *buf, size_t n)
{
    if (fseek(fp, offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, fp);
}

/*
 * Write ID3v2.3 frames to a file.  Returns 0 as soon as a byte fails to land:
 * a tag that is only partly there is not a tag.
 */
static int write_id3v2_3_frames(FILE *fp, const ID3Frame *frames, int count)
{
    for (int i = 0; i < count; i++) {
        /* Skip empty frames */
        if (frames[i].size == 0) continue;

        /* Frame header: ID(4) + Size(4, big-endian) + Flags(2) */
        uint8_t hdr[10];
        memcpy(hdr, frames[i].id, 4);
        encode_be32(frames[i].size, hdr + 4);
        hdr[8] = 0;  /* No flags in v2.3 output */
        hdr[9] = 0;
        if (!write_all(fp, hdr, 10)) return 0;
        if (!write_all(fp, frames[i].data, frames[i].size)) return 0;
    }

    return 1;
}

/* ---- Write ID3v1 tag ---- */

static int write_id3v1(FILE *fp, const ID3Frame *frames, int count)
{
    uint8_t tag[ID3V1_TAG_SIZE];
    memset(tag, 0, sizeof(tag));
    memcpy(tag, "TAG", 3);

    /* Extract metadata from frames */
    for (int i = 0; i < count; i++) {
        char *text = NULL;
        int is_text = (frames[i].id[0] == 'T' && frames[i].size > 0);
        if (is_text) text = text_frame_to_latin1(frames[i].data, frames[i].size);
        if (!text) continue;

        if (strcmp(frames[i].id, "TIT2") == 0) {
            strncpy((char *)tag + 3, text, 30);
        } else if (strcmp(frames[i].id, "TPE1") == 0) {
            strncpy((char *)tag + 33, text, 30);
        } else if (strcmp(frames[i].id, "TALB") == 0) {
            strncpy((char *)tag + 63, text, 30);
        } else if (strcmp(frames[i].id, "TYER") == 0) {
            strncpy((char *)tag + 93, text, 4);
        } else if (strcmp(frames[i].id, "TRCK") == 0) {
            int trk = atoi(text);
            if (trk > 0 && trk < 256) {
                tag[125] = 0;              /* ID3v1.1: byte before track must be 0 */
                tag[126] = (uint8_t)trk;   /* Track number */
            }
        } else if (strcmp(frames[i].id, "TCON") == 0) {
            /* Genre: try numeric first, then store text */
            strncpy((char *)tag + 97, text, 30);
            tag[127] = 0xFF; /* "Unknown" genre ID */
        }

        free(text);
    }

    return write_all(fp, tag, ID3V1_TAG_SIZE);
}

/* ---- Public API ---- */

/*
 * Build a text frame from the UTF-8 the Qt side hands down.  It used to be
 * [0x03][UTF-8], which is a v2.4 encoding written into a v2.3 tag - undefined
 * there, and ignored by the reader a Zune has.  The bytes now come from the
 * shared encoder, so an override and a frame normalised from the source tag
 * are the same shape: Latin-1 when the text genuinely is Latin-1, which leaves
 * every ASCII value byte-for-byte what it was, and UTF-16LE with a mark for
 * anything else.
 */
static uint8_t *build_text_frame_v23(const char *text, uint32_t *out_size)
{
    const uint8_t *bytes = (const uint8_t *)text;
    const size_t len = strlen(text);
    if (len == 0) return NULL;

    uint32_t *cps = NULL;
    size_t count = 0;
    if (!utf8_run_to_code_points(bytes, len, &cps, &count)) {
        /* Not UTF-8.  Whoever handed over local 8-bit bytes still means those
         * bytes, so read them as Latin-1 rather than drop the edit. */
        cps = malloc(len * sizeof(*cps));
        if (!cps) return NULL;
        for (size_t i = 0; i < len; i++) cps[i] = bytes[i];
        count = len;
    }

    uint8_t *payload = code_points_to_v23_text(cps, count, out_size);
    free(cps);
    return payload;
}

/* Replace (or append) a text frame in the frames array with the given text. */
static void override_text_frame(ID3Frame *frames, int *frame_count,
                                 int max_frames, const char *frame_id,
                                 const char *text)
{
    uint32_t new_size = 0;
    uint8_t *new_data = build_text_frame_v23(text, &new_size);
    if (!new_data) return;

    for (int i = 0; i < *frame_count; i++) {
        if (strcmp(frames[i].id, frame_id) == 0) {
            free(frames[i].data);
            frames[i].data = new_data;
            frames[i].size = new_size;
            frames[i].flags = 0;
            return;
        }
    }
    if (*frame_count >= max_frames) { free(new_data); return; }
    ID3Frame *f = &frames[*frame_count];
    memset(f->id, 0, sizeof(f->id));
    strncpy(f->id, frame_id, 4);
    f->data  = new_data;
    f->size  = new_size;
    f->flags = 0;
    (*frame_count)++;
}

/* ---- Output-path helpers ----
 *
 * A retag writes to a scratch file and never touches the input, and that name
 * has to belong to this one rewrite.  It used to be "basename plus process id",
 * which is the same path twice for two files of the same name in one process,
 * so it now takes its uniqueness from mkstemps(), which creates with O_EXCL.
 * The descriptor is closed straight away: every caller reopens the path with
 * fopen() under its own error handling, and a name that exists is still a name
 * that nobody else can be holding.  The basename is kept because a log that
 * names the song is worth its characters, and it is the part that gives way to
 * a name that will not fit.
 */
static int build_scratch_path(char *out, size_t out_size, const char *mp3_path)
{
    const char *tmpdir = ZUUNED_TMPDIR;   /* a function call on Windows */
    const char *sep = strrchr(mp3_path, '/');
    /* Windows reaches here with native separators, and Qt also hands this layer
     * forward slashes, so the basename has to stop at whichever separator comes
     * later - otherwise the drive letter colon lands inside the temp file name.
     * A POSIX name may legitimately contain a backslash, so that extra scan
     * stays out of the POSIX build. */
#ifdef _WIN32
    const char *backslash = strrchr(mp3_path, '\\');
    if (backslash && (!sep || backslash > sep)) sep = backslash;
#endif
    const char *base = sep ? sep + 1 : mp3_path;
    const char *dot = strrchr(base, '.');
    size_t base_len = dot ? (size_t)(dot - base) : strlen(base);
    if (base_len > 64) base_len = 64;

    const int needed = snprintf(out, out_size, "%szuuned_retag_%.*s_XXXXXX.mp3",
                                tmpdir, (int)base_len, base);
    if (needed < 0 || (size_t)needed >= out_size) return 0;

    const int fd = mkstemps(out, 4);
    if (fd < 0) { out[0] = '\0'; return 0; }
    close(fd);
    return 1;
}

/*
 * Shared rewrite core. album_artist: TPE1 override (sync path).
 * edits: full-field overrides (metadata editor). out_path_override:
 * exact output path (in-place edit flow); NULL = scratch file.
 *
 * The audio is streamed.  A rewrite used to malloc the whole input - up to the
 * 500 MB cap - and hand it to a single fwrite(), so retagging a lossless album
 * asked for every file twice over in the heap and paid for it in page faults,
 * and a full disk surfaced only once the whole file had been buffered.  What a
 * tag rewrite needs from the audio is the head (to settle where the audio
 * begins) and the last 128 bytes (to know whether an ID3v1 trailer is there to
 * drop); everything in between is copied through one fixed buffer and never
 * looked at.
 *
 * Every write is checked and any failure removes the output, so a half-written
 * file is never the path a caller is handed.
 */
#define RETAG_PROBE_WINDOW (1024u * 1024u)
#define RETAG_COPY_CHUNK   (256u * 1024u)

static char *retag_core(const char *mp3_path, const char *album_artist,
                        const ZuunedId3Edits *edits,
                        const char *out_path_override, int disc_number)
{
    ID3Frame frames[MAX_FRAMES];
    uint8_t header[ID3V2_HEADER_SIZE];
    uint8_t tail[ID3V1_TAG_SIZE];
    uint8_t *tag_data = NULL;
    uint8_t *probe = NULL;
    uint8_t *copy = NULL;
    FILE *fin = NULL;
    FILE *fout = NULL;
    char output_path[1024];
    long file_size = 0;
    long audio_start = 0;
    long audio_end = 0;
    long remaining = 0;
    int frame_count = 0;
    int i = 0;

    memset(frames, 0, sizeof(frames));
    memset(tail, 0, sizeof(tail));
    output_path[0] = '\0';

    if (!mp3_path) return NULL;
    fin = fopen(mp3_path, "rb");
    if (!fin) return NULL;

    if (fseek(fin, 0, SEEK_END) != 0) goto fail;
    file_size = ftell(fin);
    if (file_size < 128 || file_size > 500L * 1024 * 1024) goto fail;

    /* ---- The ID3v2 tag, if the file carries one ---- */
    if (read_at(fin, 0, header, sizeof(header)) == sizeof(header)
        && header[0] == 'I' && header[1] == 'D' && header[2] == '3') {

        const int version_major = header[3];
        const uint8_t flags = header[5];
        const uint32_t declared = decode_syncsafe(header + 6);
        size_t want = declared > MAX_TAG_SIZE ? (size_t)MAX_TAG_SIZE : (size_t)declared;
        if ((size_t)ID3V2_HEADER_SIZE + want > (size_t)file_size)
            want = (size_t)file_size - ID3V2_HEADER_SIZE;

        /* A tag that claims more than the file holds is read as far as the file
         * goes, because the sync probe below settles the truth either way. */
        tag_data = malloc(want + 1);
        if (!tag_data) goto fail;
        if (read_at(fin, ID3V2_HEADER_SIZE, tag_data, want) != want) goto fail;

        size_t body = 0;
        if ((flags & 0x40) && want >= 4) {
            /* Extended header.  v2.3 leaves its own size field out of that
             * figure and v2.4 counts it in, and four bytes off turns the first
             * frame's ID into the tail of a length. */
            const uint32_t ext = (version_major == 4) ? decode_syncsafe(tag_data)
                                                      : decode_be32(tag_data);
            body = (version_major == 4) ? (size_t)ext : (size_t)ext + 4;
            if (body > want) body = want;
        }

        size_t parse_len = want - body;
        if ((flags & 0x10) && version_major == 4 && parse_len >= ID3V2_HEADER_SIZE)
            parse_len -= ID3V2_HEADER_SIZE;   /* the v2.4 footer is not a frame */

        frame_count = parse_id3v2_frames(tag_data + body, (uint32_t)parse_len,
                                         (uint8_t)version_major, frames, MAX_FRAMES);

        /* The hint for where the audio begins is the END of the tag, not the
         * start of its frames: a tag body is full of 0xFF bytes (every UTF-16
         * byte-order mark is one, and 0xFF 0xFE reads as a perfectly good MPEG
         * sync word to a scanner), so probing from the frames would stop on the
         * artist's name and copy half the tag as audio. */
        audio_start = (long)ID3V2_HEADER_SIZE + (long)want;
        if ((flags & 0x10) && version_major == 4)
            audio_start += ID3V2_HEADER_SIZE;   /* the v2.4 footer trails the body */
    }

    /* ---- Where the audio really starts ----
     * The tag's own size is a hint; an MPEG sync word is the fact.  Trusting
     * the figure alone is how a tagger that wrote the wrong one left a "file"
     * whose first audio byte was the tail of a frame header. */
    probe = malloc(RETAG_PROBE_WINDOW);
    if (!probe) goto fail;

    const size_t window = RETAG_PROBE_WINDOW < (size_t)file_size
                        ? RETAG_PROBE_WINDOW : (size_t)file_size;
    long from = audio_start;
    size_t got = read_at(fin, from, probe, window);
    long sync = got ? find_mpeg_sync(probe, (long)got) : -1;
    if (sync < 0 && from != 0) {
        from = 0;
        got = read_at(fin, 0, probe, window);
        sync = got ? find_mpeg_sync(probe, (long)got) : -1;
    }
    if (sync < 0) goto fail;      /* no MPEG sync anywhere: not an MP3 */
    audio_start = from + sync;

    /* ---- An ID3v1 trailer is rewritten from the frames, never copied ---- */
    audio_end = file_size;
    if (read_at(fin, file_size - ID3V1_TAG_SIZE, tail, ID3V1_TAG_SIZE) == ID3V1_TAG_SIZE
        && tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G')
        audio_end = file_size - ID3V1_TAG_SIZE;
    if (audio_end <= audio_start) goto fail;

    /* Override TPE1 (artist) with album_artist if provided. Keeps the device
     * grouping tracks under one artist object instead of splitting on
     * per-track "feat. X" strings. */
    if (album_artist && album_artist[0]) {
        override_text_frame(frames, &frame_count, MAX_FRAMES, "TPE1", album_artist);
    }

    /* Metadata-editor overrides (UX-2 Edit Info). Empty strings keep
     * the existing frame; track/year 0 likewise. */
    if (edits) {
        if (edits->title && edits->title[0])
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TIT2", edits->title);
        if (edits->artist && edits->artist[0])
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TPE1", edits->artist);
        if (edits->album_artist && edits->album_artist[0])
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TPE2", edits->album_artist);
        if (edits->album && edits->album[0])
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TALB", edits->album);
        if (edits->genre && edits->genre[0])
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TCON", edits->genre);
        if (edits->track > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", edits->track);
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TRCK", buf);
        }
        if (edits->year > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", edits->year);
            override_text_frame(frames, &frame_count, MAX_FRAMES, "TYER", buf);
        }
    }

    /* Normalize a source TPOS, omit malformed values, or use the queued disc.
     * Remove duplicate TPOS frames before publishing one canonical value. */
    char disc[48] = {0};
    if (disc_number > 0) snprintf(disc, sizeof(disc), "%d", disc_number);
    for (int i = 0; i < frame_count;) {
        if (strcmp(frames[i].id, "TPOS") != 0) { ++i; continue; }
        if (!disc[0]) {
            char *text = text_frame_to_latin1(frames[i].data, frames[i].size);
            normalize_disc_tag(text, disc);
            free(text);
        }
        free(frames[i].data);
        memmove(&frames[i], &frames[i + 1], (size_t)(frame_count - i - 1) * sizeof(frames[0]));
        --frame_count;
    }
    if (disc[0]) override_text_frame(frames, &frame_count, MAX_FRAMES, "TPOS", disc);

    /* One encoding for every text frame, whatever the source claimed.  This
     * runs after the overrides so an inherited value and a value written here
     * cannot leave in two different encodings, and it skips TXXX, whose payload
     * is a description plus a value rather than one string. */
    for (i = 0; i < frame_count; i++) {
        if (frames[i].id[0] == 'T' && strcmp(frames[i].id, "TXXX") != 0)
            normalize_text_frame_for_v23(&frames[i]);
    }

    /* ---- Output path ---- */
    if (out_path_override) {
        if (snprintf(output_path, sizeof(output_path), "%s",
                     out_path_override) >= (int)sizeof(output_path))
            goto fail;   /* a truncated path would write somewhere else */
    } else if (!build_scratch_path(output_path, sizeof(output_path), mp3_path)) {
        goto fail;
    }

    fout = fopen(output_path, "wb");
    if (!fout) goto fail;

    /* ---- The new tag ---- */
    if (frame_count > 0) {
        uint32_t frames_size = 0;
        for (i = 0; i < frame_count; i++) {
            if (frames[i].size > 0) frames_size += 10 + frames[i].size;
        }

        uint8_t out_header[ID3V2_HEADER_SIZE];
        out_header[0] = 'I'; out_header[1] = 'D'; out_header[2] = '3';
        out_header[3] = 3;   /* Version 2.3, which is the whole point */
        out_header[4] = 0;
        out_header[5] = 0;   /* No flags */
        encode_syncsafe(frames_size + PADDING_SIZE, out_header + 6);

        if (!write_all(fout, out_header, sizeof(out_header))) goto fail;
        if (!write_id3v2_3_frames(fout, frames, frame_count)) goto fail;

        uint8_t zeros[PADDING_SIZE];
        memset(zeros, 0, sizeof(zeros));
        if (!write_all(fout, zeros, sizeof(zeros))) goto fail;
    }

    /* ---- The audio, in chunks ---- */
    copy = malloc(RETAG_COPY_CHUNK);
    if (!copy) goto fail;

    remaining = audio_end - audio_start;
    if (fseek(fin, audio_start, SEEK_SET) != 0) goto fail;
    while (remaining > 0) {
        const size_t want = remaining > (long)RETAG_COPY_CHUNK
                          ? (size_t)RETAG_COPY_CHUNK : (size_t)remaining;
        if (fread(copy, 1, want, fin) != want) goto fail;   /* truncated source */
        if (!write_all(fout, copy, want)) goto fail;
        remaining -= (long)want;
    }

    /* ---- The ID3v1 shadow of the same frames ---- */
    if (frame_count > 0 && !write_id3v1(fout, frames, frame_count)) goto fail;

    if (fflush(fout) != 0) goto fail;
    if (fclose(fout) != 0) { fout = NULL; goto fail; }
    fout = NULL;

    fclose(fin);
    fin = NULL;
    free(tag_data);
    free(probe);
    free(copy);
    for (i = 0; i < MAX_FRAMES; i++) free(frames[i].data);

    fprintf(stderr, "[zuuned] retagged to ID3v2.3: %s\n", output_path);
    return strdup(output_path);

fail:
    if (fout) fclose(fout);
    if (output_path[0]) remove(output_path);
    if (fin) fclose(fin);
    free(tag_data);
    free(probe);
    free(copy);
    for (i = 0; i < MAX_FRAMES; i++) free(frames[i].data);
    return NULL;
}

char *zuuned_retag_mp3(const char *mp3_path, const char *album_artist)
{
    return zuuned_retag_mp3_with_disc(mp3_path, album_artist, 0);
}

char *zuuned_retag_mp3_with_disc(const char *mp3_path, const char *album_artist, int disc_number)
{
    return retag_core(mp3_path, album_artist, NULL, NULL, disc_number);
}

int zuuned_edit_mp3_tags(const char *mp3_path, const ZuunedId3Edits *edits)
{
    if (!mp3_path || !edits) return -1;

    /* Rewrite into a sibling temp file (same filesystem), then rename
     * over the original — atomic, no cross-device copy. */
    char tmp_path[1024];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.zuuned_edit.tmp",
                 mp3_path) >= (int)sizeof(tmp_path))
        return -1;

    char *out = retag_core(mp3_path, NULL, edits, tmp_path, 0);
    if (!out) return -1;
    free(out);

    if (rename(tmp_path, mp3_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    fprintf(stderr, "[zuuned] tags edited in place: %s\n", mp3_path);
    return 0;
}
