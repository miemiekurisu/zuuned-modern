/*
 * libzune — thumbnail.c
 * Album art, video poster, and photo thumbnail operations.
 *
 * Uses native PTP/MTP stack (mtp.h) instead of vendored libmtp.
 */

#include "zune_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Method 2 parks the full download in the temporary directory.  POSIX uses /tmp
 * directly; the Windows compatibility layer hands back a writable directory with
 * a trailing separator and the macro is a function call, so it goes in via "%s". */
#ifndef ZUUNED_TMPDIR
#define ZUUNED_TMPDIR "/tmp/"
#endif

/* ---- Album art (set) ---- */

int zune_brand(ZuneDevice *dev, uint32_t item_id,
                        const uint8_t *jpeg_data, size_t jpeg_len) {
    if (!dev || !jpeg_data || jpeg_len == 0) return -1;

    int ret = mtp_send_representative_sample(&dev->ptp, item_id,
                                             MTP_OFC_JPEG,
                                             0, 0,   /* width/height: let device handle it */
                                             jpeg_data, (uint32_t)jpeg_len);
    if (ret != 0) {
        fprintf(stderr, "[libzune] set_album_art failed for item %u\n", item_id);
        return -1;
    }
    fprintf(stderr, "[libzune] album art set for item %u (%zu bytes)\n",
            item_id, jpeg_len);
    return 0;
}

/* ---- Thumbnail fetch (no full download) ---- */

int zune_grab_thumb(ZuneDevice *dev, uint32_t item_id,
                        const char *cache_path) {
    if (!dev || !cache_path) return -1;

    /* Already cached? */
    struct stat st;
    if (stat(cache_path, &st) == 0 && st.st_size > 0)
        return 0;

    /* Get representative sample (uses GetThumb 0x100A) */
    uint8_t *data = NULL;
    uint32_t size = 0;
    int ret = mtp_get_representative_sample(&dev->ptp, item_id, &data, &size);
    if (ret == 0 && data && size > 0) {
        FILE *fp = fopen(cache_path, "wb");
        if (fp) {
            fwrite(data, 1, size, fp);
            fclose(fp);
            fprintf(stderr, "[libzune] got thumbnail for %u (%u bytes)\n",
                    item_id, size);
            free(data);
            return 0;
        }
    }
    free(data);

    /* No full download fallback — too slow for album art / video posters */
    fprintf(stderr, "[libzune] all thumbnail methods failed for %u\n", item_id);
    return -1;
}

/* ---- Photo thumbnail with full fallback chain ---- */

int zune_grab_photo_thumb(ZuneDevice *dev, uint32_t item_id,
                              const char *cache_path) {
    if (!dev || !cache_path) return -1;

    /* Already cached? */
    struct stat st;
    if (stat(cache_path, &st) == 0 && st.st_size > 0)
        return 0;

    /* Method 1: Representative sample (GetThumb 0x100A) */
    uint8_t *data = NULL;
    uint32_t size = 0;
    int ret = mtp_get_representative_sample(&dev->ptp, item_id, &data, &size);
    if (ret == 0 && data && size > 0) {
        FILE *fp = fopen(cache_path, "wb");
        if (fp) {
            fwrite(data, 1, size, fp);
            fclose(fp);
            fprintf(stderr, "[libzune] got representative sample for %u (%u bytes)\n",
                    item_id, size);
            free(data);
            return 0;
        }
    }
    free(data);

    /* Method 2: Download full file + resize to 200x200 */
    /* ZUUNED_TMPDIR alone can run past the CRT limit, and Method 2 also has to
     * leave room for the resize command built on top of it. */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%szunelinux-photo-%u", ZUUNED_TMPDIR, item_id);
    ret = mtp_get_object_to_file(&dev->ptp, item_id, tmp);
    if (ret == 0) {
#ifdef _WIN32
        /* Same ffmpeg, started with argv and no shell in front of it, so
         * nothing a device object is named can be expanded into something
         * else - see zuuned_run_no_shell.  There is no command-line room check
         * here any more because there is no command line to overflow: the only
         * name that can fail is one the system code page cannot spell, and that
         * is what the NULL from zuuned_narrow_path_for_shell() already means.
         *
         * cache_path arrives as UTF-8 and the child decodes its arguments with
         * the ANSI code page, so the name ffmpeg writes has to be re-spelled or
         * it publishes a thumbnail no caller will ever find.  tmp needs nothing:
         * it is ZUUNED_TMPDIR plus a number, and that directory was built in the
         * code page already.  ffmpeg must be reachable on PATH - the app bundles
         * no copy - so on a machine without one Method 2 fails the same way it
         * always did and only the representative sample produces a thumbnail. */
        char *out_bytes = zuuned_narrow_path_for_shell(cache_path);
        if (!out_bytes) {
            fprintf(stderr, "[libzune] no code-page spelling for the thumbnail"
                            " cache path: %s\n", cache_path);
            remove(tmp);
            return -1;
        }
        {
            const char *thumb_argv[] = {
                "ffmpeg", "-y", "-i", tmp, "-vf",
                "scale=200:200:force_original_aspect_ratio=decrease",
                out_bytes, NULL
            };
            ret = zuuned_run_no_shell(thumb_argv);
        }
        free(out_bytes);
#else
        char cmd[1024];
#ifdef __APPLE__
        /* macOS: sips — preserve aspect ratio, fit within 200x200 */
        snprintf(cmd, sizeof(cmd),
                 "sips --resampleHeightWidthMax 200 --setProperty format jpeg '%s' --out '%s' 2>/dev/null",
                 tmp, cache_path);
#else
        /* Linux: use ffmpeg */
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -i '%s' -vf "
                 "scale=200:200:force_original_aspect_ratio=decrease "
                 "'%s' 2>/dev/null",
                 tmp, cache_path);
#endif
        ret = system(cmd);
#endif
        remove(tmp);

        if (ret == 0) {
            fprintf(stderr, "[libzune] resized full file for thumbnail %u\n",
                    item_id);
            return 0;
        }
    } else {
        remove(tmp);
    }

    fprintf(stderr, "[libzune] all photo thumbnail methods failed for %u\n",
            item_id);
    return -1;
}
