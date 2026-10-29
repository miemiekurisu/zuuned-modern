/*
 * wma_lossless_stub.c - the POSIX half of the WMA Lossless target.
 *
 * The encoder this product uses lives in the Windows Media Format runtime, so on
 * Linux, macOS and anywhere else there is nothing to call.  Rather than hide the
 * target behind an #ifdef in every caller, the portable build links this file and
 * the rest of the application asks one question - zuuned_wma_lossless_available()
 * - and gets an honest 0.  A settings page built from that answer never offers a
 * format this build cannot make, and the sync engine never routes a track here.
 *
 * Compiled only where wma_lossless_enc.cpp is not.  Kept in C because the whole
 * POSIX link line is C: adding a single C++ translation unit to it for two stubs
 * would drag in the C++ runtime for no reason.
 */

#include "libav_transcode.h"

#include <stdio.h>

#ifndef _WIN32

#define STUB_LOG "[transcode/wma-lossless]"

int zuuned_wma_lossless_available(void)
{
    return 0;
}

char *zuuned_audio_encode_wma_lossless(const char *input_path, const char *album_artist,
                                       int disc_number, const char *output_path,
                                       zuuned_progress_cb progress, void *userdata)
{
    (void)album_artist;
    (void)disc_number;
    (void)progress;
    (void)userdata;
    /* Nothing is deleted here on purpose.  This path is one the caller chose, and
     * a build that never wrote to it has no business unlinking whatever was
     * already there. */
    fprintf(stderr, STUB_LOG " WMA Lossless is a Windows encoder; this build cannot"
            " produce it from %s\n", input_path ? input_path : "(no input)");
    return NULL;
}

#endif /* !_WIN32 */