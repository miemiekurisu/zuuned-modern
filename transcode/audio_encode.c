/*
 * audio_encode.c - the audio-only encode that the LAME path cannot make.
 *
 * libav_transcode.c owns MP3: LAME plus a hand-built ID3v2.3 tag, which is what
 * a Zune reads.  This file owns WMA: libavcodec `wmav2` muxed into ASF, the
 * container the player reports as WMA.
 *
 * WMA Lossless is not here either, and for a reason that turned out to be about
 * where to look rather than what to write.  This file encodes through FFmpeg, and
 * measured on the FFmpeg build this application ships (`ffmpeg -encoders`) the
 * only WMA encoders are `wmav1` and `wmav2` - `wmalossless` and `wmapro` appear
 * under decoders only.  Windows has no WMA lossless ACM codec either.  It does
 * have the encoder, in the Windows Media Format runtime, reachable in process and
 * not through either of those doors, and `wma_lossless_enc.cpp` drives it instead
 * of anything here trying to follow.  A lossless WMA the user already owns is
 * still carried byte-for-byte by the sync engine, which needs no encoder at all.
 *
 * `wmav2` has no variable-bitrate mode, which is why the VBR control the sync
 * settings offer is MP3-only and the UI has to say so.
 */

#include "libav_transcode.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_PREFIX "[transcode/wma]"

/* Common WMA sample rates. Anything else in a source file is resampled to the
 * nearest of these rather than passed through, because the encoders reject the
 * exotic rates a FLAC rip can carry. */
static const int WMA_RATES[] = { 44100, 48000, 32000, 22050, 16000, 11025, 8000 };

static int pick_sample_rate(int wanted)
{
    if (wanted <= 0)
        return 44100;
    for (size_t i = 0; i < sizeof(WMA_RATES) / sizeof(WMA_RATES[0]); i++)
        if (WMA_RATES[i] == wanted)
            return wanted;
    return wanted > 48000 ? 48000 : 44100;
}

/* The first supported sample format, asked the way FFmpeg 8 wants it asked.
 * Mirrors the video path in libav_transcode.c: AVCodec.sample_fmts is gone from
 * 62 onwards, and reading it anyway is a null dereference, not a fallback.  The
 * fallback names fltp rather than s16 because that is all `wmav2` takes:
 * `ffmpeg -h encoder=wmav2` against the FFmpeg this application links answers
 * `Supported sample formats: fltp` and nothing else, so an s16 encoder context
 * opens and then refuses every frame. */
static enum AVSampleFormat first_sample_format(const AVCodec *codec)
{
#if LIBAVCODEC_VERSION_MAJOR >= 62
    const enum AVSampleFormat *fmts = NULL;
    int count = 0;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, (const void **)&fmts, &count) < 0 ||
        !fmts || count <= 0)
        return AV_SAMPLE_FMT_FLTP;
    return fmts[0];
#else
    return codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
}

/* Copy the tag pairs a Zune actually displays. The ASF muxer translates these
 * canonical keys into its own attribute set, so naming them here is enough;
 * anything the source carries beyond this stays behind, the same trade the MP3
 * retagger already makes. */
static void copy_tags(const AVFormatContext *in, AVFormatContext *out,
                      const char *album_artist, int disc_number)
{
    static const char *keys[] = { "title", "album", "genre", "date", "composer" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = av_dict_get(in->metadata, keys[i], NULL, 0)
                            ? av_dict_get(in->metadata, keys[i], NULL, 0)->value
                            : NULL;
        if (v && v[0])
            av_dict_set(&out->metadata, keys[i], v, 0);
    }
    /* Album artist wins over track artist, exactly as the MP3 path does: a
     * "feat. X" TPE1 splits one album across several artists on the player. */
    const AVDictionaryEntry *artist = av_dict_get(in->metadata, "artist", NULL, 0);
    if (album_artist && album_artist[0])
        av_dict_set(&out->metadata, "artist", album_artist, 0);
    else if (artist && artist->value[0])
        av_dict_set(&out->metadata, "artist", artist->value, 0);

    const AVDictionaryEntry *track = av_dict_get(in->metadata, "track", NULL, 0);
    if (track && track->value[0])
        av_dict_set(&out->metadata, "track", track->value, 0);
    if (disc_number > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", disc_number);
        av_dict_set(&out->metadata, "disc", buf, 0);
    }
}

/* Pull every frame the decoder currently holds, convert it, and queue the
 * result.  EAGAIN/EOF from receive_frame is the normal idle answer, not an
 * error.  Returns 0 when idle, -1 on a real failure, -2 when cancelled. */
static int pump_decoder(AVCodecContext *dec_ctx, AVFrame *frame, SwrContext *swr,
                        AVAudioFifo *fifo, AVCodecContext *enc_ctx,
                        int64_t *produced)
{
    int ret;
    while ((ret = avcodec_receive_frame(dec_ctx, frame)) == 0) {
        const int need = swr_get_out_samples(swr, frame->nb_samples);
        if (need > 0) {
            uint8_t *converted[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
            int linesize = 0;
            if (av_samples_alloc(converted, &linesize, enc_ctx->ch_layout.nb_channels,
                                 need, enc_ctx->sample_fmt, 0) < 0) {
                av_frame_unref(frame);
                return -1;
            }
            const int got = swr_convert(swr, converted, need,
                                        (const uint8_t **)frame->extended_data,
                                        frame->nb_samples);
            if (got >= 0 && av_audio_fifo_write(fifo, (void **)converted, got) < 0) {
                av_freep(&converted[0]);
                av_frame_unref(frame);
                return -1;
            }
            if (got >= 0)
                *produced += got;
            av_freep(&converted[0]);
        }
        av_frame_unref(frame);
        if (zuuned_transcode_abort_requested())
            return -2;
    }
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : -1;
}

/* Hand back every packet the encoder has, with the timestamps the muxer wants. */
static int write_encoder_packets(AVCodecContext *enc_ctx, AVFormatContext *ofmt,
                                int out_idx, AVPacket *out_pkt)
{
    int ret;
    while ((ret = avcodec_receive_packet(enc_ctx, out_pkt)) == 0) {
        out_pkt->stream_index = out_idx;
        av_packet_rescale_ts(out_pkt, enc_ctx->time_base,
                             ofmt->streams[out_idx]->time_base);
        const int wrote = av_interleaved_write_frame(ofmt, out_pkt);
        av_packet_unref(out_pkt);
        if (wrote < 0)
            return -1;
    }
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : -1;
}

/* Feed the encoder in frame_size pieces.  With `tail` set the last short piece
 * goes too, because after the decoder is done there is nothing left to pad it. */
static int pump_fifo_into_encoder(AVAudioFifo *fifo, AVCodecContext *enc_ctx,
                                  AVFrame *scratch, AVFormatContext *ofmt,
                                  int out_idx, AVPacket *out_pkt, int tail)
{
    while (av_audio_fifo_size(fifo) > (tail ? 0 : enc_ctx->frame_size)) {
        const int have = av_audio_fifo_size(fifo);
        const int chunk = have < enc_ctx->frame_size ? have : enc_ctx->frame_size;
        av_frame_unref(scratch);
        scratch->nb_samples = chunk;
        scratch->format = enc_ctx->sample_fmt;
        scratch->sample_rate = enc_ctx->sample_rate;
        av_channel_layout_copy(&scratch->ch_layout, &enc_ctx->ch_layout);
        if (av_frame_get_buffer(scratch, 0) < 0 ||
            av_audio_fifo_read(fifo, (void **)scratch->data, chunk) < 0)
            return -1;
        const int sent = avcodec_send_frame(enc_ctx, scratch);
        av_frame_unref(scratch);
        if (sent < 0)
            return sent == AVERROR(EAGAIN) ? 0 : -1;
        if (write_encoder_packets(enc_ctx, ofmt, out_idx, out_pkt) < 0)
            return -1;
    }
    return 0;
}
char *zuuned_audio_encode_wma(const char *input_path, const char *album_artist,
                              int disc_number, int bitrate_kbps,
                              const char *output_path,
                              zuuned_progress_cb progress, void *userdata)
{
    if (!input_path || !output_path)
        return NULL;

    AVFormatContext *ifmt = NULL;
    AVFormatContext *ofmt = NULL;
    AVCodecContext *dec_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;
    SwrContext *swr_ctx = NULL;
    AVAudioFifo *fifo = NULL;
    AVFrame *frame = NULL;
    AVFrame *fifo_frame = NULL;
    AVPacket *pkt = NULL;
    AVPacket *out_pkt = NULL;
    char *result = NULL;
    int wrote_header = 0;
    int out_idx = -1;
    int64_t produced = 0;   /* samples handed the encoder, for progress */
    int last_pct = -1;

    const AVCodec *enc = avcodec_find_encoder_by_name("wmav2");
    if (!enc)
        enc = avcodec_find_encoder(AV_CODEC_ID_WMAV2);
    if (!enc) {
        fprintf(stderr, LOG_PREFIX " no WMA encoder in this build\n");
        return NULL;
    }

    if (avformat_open_input(&ifmt, input_path, NULL, NULL) < 0) {
        fprintf(stderr, LOG_PREFIX " can't open input: %s\n", input_path);
        goto done;
    }
    if (avformat_find_stream_info(ifmt, NULL) < 0)
        goto done;
    const int in_idx = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (in_idx < 0)
        goto done;
    AVStream *in_stream = ifmt->streams[in_idx];

    const AVCodec *dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec)
        goto done;
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx ||
        avcodec_parameters_to_context(dec_ctx, in_stream->codecpar) < 0 ||
        avcodec_open2(dec_ctx, dec, NULL) < 0)
        goto done;

    if (avformat_alloc_output_context2(&ofmt, NULL, "asf", output_path) < 0 || !ofmt)
        goto done;

    const int rate = pick_sample_rate(dec_ctx->sample_rate);
    const int bitrate = (bitrate_kbps >= 32 && bitrate_kbps <= 320) ? bitrate_kbps : 128;

    enc_ctx = avcodec_alloc_context3(enc);
    if (!enc_ctx)
        goto done;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&enc_ctx->ch_layout, &stereo);
    enc_ctx->sample_rate = rate;
    enc_ctx->bit_rate = (int64_t)bitrate * 1000;
    enc_ctx->sample_fmt = first_sample_format(enc);
    enc_ctx->time_base = (AVRational){ 1, rate };
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc_ctx, enc, NULL) < 0) {
        fprintf(stderr, LOG_PREFIX " encoder '%s' failed to open\n", enc->name);
        goto done;
    }

    {
        AVStream *out_stream = avformat_new_stream(ofmt, NULL);
        if (!out_stream)
            goto done;
        if (avcodec_parameters_from_context(out_stream->codecpar, enc_ctx) < 0)
            goto done;
        out_stream->time_base = enc_ctx->time_base;
        out_idx = out_stream->index;
    }

    copy_tags(ifmt, ofmt, album_artist, disc_number);

    if (swr_alloc_set_opts2(&swr_ctx, &enc_ctx->ch_layout, enc_ctx->sample_fmt,
                            enc_ctx->sample_rate, &dec_ctx->ch_layout,
                            dec_ctx->sample_fmt, dec_ctx->sample_rate, 0,
                            NULL) < 0 ||
        swr_init(swr_ctx) < 0)
        goto done;

    fifo = av_audio_fifo_alloc(enc_ctx->sample_fmt, enc_ctx->ch_layout.nb_channels,
                               enc_ctx->frame_size * 4);
    frame = av_frame_alloc();
    fifo_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    out_pkt = av_packet_alloc();
    if (!fifo || !frame || !fifo_frame || !pkt || !out_pkt)
        goto done;

    if (!(ofmt->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&ofmt->pb, output_path, AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, LOG_PREFIX " can't create: %s\n", output_path);
        goto done;
    }
    if (avformat_write_header(ofmt, NULL) < 0) {
        fprintf(stderr, LOG_PREFIX " write_header failed\n");
        goto done;
    }
    wrote_header = 1;

    const int64_t total = ifmt->duration > 0 ? ifmt->duration : 0;
    long dropped = 0;

    while (av_read_frame(ifmt, pkt) >= 0) {
        if (zuuned_transcode_abort_requested()) {
            fprintf(stderr, LOG_PREFIX " cancelled\n");
            goto done;
        }
        if (pkt->stream_index != in_idx) {
            av_packet_unref(pkt);
            continue;
        }

        /* EAGAIN means the decoder is merely full, not that this packet is
         * unusable, so the answer is to drain and retry the SAME packet.
         * Dropping it instead deletes real audio in silence: measured on a
         * WMA Lossless source taken off a player, that cost 1.58 seconds of
         * the end of the track and still reported a successful encode. */
        int sent = avcodec_send_packet(dec_ctx, pkt);
        if (sent == AVERROR(EAGAIN)) {
            const int pr = pump_decoder(dec_ctx, frame, swr_ctx, fifo, enc_ctx, &produced);
            if (pr < 0) {
                av_packet_unref(pkt);
                if (pr == -2)
                    fprintf(stderr, LOG_PREFIX " cancelled\n");
                goto done;
            }
            if (pump_fifo_into_encoder(fifo, enc_ctx, fifo_frame, ofmt, out_idx,
                                       out_pkt, 0) < 0) {
                av_packet_unref(pkt);
                goto done;
            }
            sent = avcodec_send_packet(dec_ctx, pkt);
        }
        av_packet_unref(pkt);
        if (sent < 0) {
            /* Only now is dropping honest: the decoder refused this packet
             * outright, which is corruption, not backpressure. It is counted
             * and reported rather than swallowed. */
            dropped++;
            continue;
        }

        const int pr = pump_decoder(dec_ctx, frame, swr_ctx, fifo, enc_ctx, &produced);
        if (pr < 0) {
            if (pr == -2)
                fprintf(stderr, LOG_PREFIX " cancelled\n");
            goto done;
        }
        if (pump_fifo_into_encoder(fifo, enc_ctx, fifo_frame, ofmt, out_idx,
                                   out_pkt, 0) < 0)
            goto done;

        /* Samples handed the encoder against the declared duration, throttled
         * to whole percents, so a callback that repaints a queue row is not
         * called once per decoded frame. */
        if (progress && total > 0) {
            const double end = (double)total / AV_TIME_BASE * (double)rate;
            const int pct = end > 0.0 ? (int)(produced * 100.0 / end) : 0;
            if (pct != last_pct && pct <= 100) {
                last_pct = pct;
                progress((float)pct / 100.0f, userdata);
            }
        }
    }

    /* Drain the decoder, then the resampler, then the encoder. */
    avcodec_send_packet(dec_ctx, NULL);
    if (pump_decoder(dec_ctx, frame, swr_ctx, fifo, enc_ctx, &produced) < 0)
        goto done;

    /* swr_convert(swr, NULL, 0, NULL, 0) only reports how much the resampler is
     * still holding; it delivers nothing. The samples come out of calls that
     * pass a real output buffer and no input. */
    for (;;) {
        const int delayed = swr_get_delay(swr_ctx, enc_ctx->sample_rate);
        if (delayed <= 0)
            break;
        const int chunk = delayed < 1024 ? delayed : 1024;
        uint8_t *converted[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        int linesize = 0;
        if (av_samples_alloc(converted, &linesize, enc_ctx->ch_layout.nb_channels,
                             chunk, enc_ctx->sample_fmt, 0) < 0)
            goto done;
        const int got = swr_convert(swr_ctx, converted, chunk, NULL, 0);
        if (got < 0) {
            av_freep(&converted[0]);
            break;
        }
        if (got > 0 && av_audio_fifo_write(fifo, (void **)converted, got) < 0) {
            av_freep(&converted[0]);
            goto done;
        }
        if (got > 0)
            produced += got;
        av_freep(&converted[0]);
        if (got == 0)
            break;
    }

    if (pump_fifo_into_encoder(fifo, enc_ctx, fifo_frame, ofmt, out_idx,
                               out_pkt, 1) < 0)
        goto done;
    avcodec_send_frame(enc_ctx, NULL);
    if (write_encoder_packets(enc_ctx, ofmt, out_idx, out_pkt) < 0)
        goto done;

    if (dropped > 0)
        fprintf(stderr, LOG_PREFIX " warning: %ld packet(s) the decoder refused\n",
                dropped);
    if (wrote_header)
        av_write_trailer(ofmt);

    if (!zuuned_transcode_abort_requested()) {
        if (progress)
            progress(1.0f, userdata);
        result = strdup(output_path);
        fprintf(stderr, LOG_PREFIX " wrote %s (%s %d kbps)\n", output_path,
                enc->name, bitrate);
    }

done:
    /* Close exactly once, and before judging the file: avio_closep is what
     * flushes the ASF index, and a half-written one is deleted rather than left
     * for the player or for a retry to mistake for a completed encode. */
    if (ofmt && (ofmt->oformat->flags & AVFMT_NOFILE) == 0 && ofmt->pb)
        avio_closep(&ofmt->pb);
    if (!result)
        unlink(output_path);
    if (ifmt)
        avformat_close_input(&ifmt);
    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    swr_free(&swr_ctx);
    if (fifo)
        av_audio_fifo_free(fifo);
    av_frame_free(&frame);
    av_frame_free(&fifo_frame);
    av_packet_free(&pkt);
    av_packet_free(&out_pkt);
    return result;
}
