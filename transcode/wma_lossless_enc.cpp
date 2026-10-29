/*
 * wma_lossless_enc.cpp - WMA Lossless through the encoder that is already in
 * Windows, so that a lossless destination needs no vendored codec.
 *
 * Why this file exists and what it deliberately is not.  The FFmpeg this
 * application links carries wmav1 and wmav2 as its only WMA encoders -
 * `ffmpeg -encoders` on the shipped build says so - and Windows offers no WMA
 * lossless ACM codec to reach instead.  That closed both doors a "just call an
 * encoder" answer would need, so until now a WMA Lossless track could only ever
 * survive a sync by being copied byte for byte.  There is a third door, and it
 * is not a fork of anything: the Windows Media Format SDK encoder that ships in
 * the OS (`wmvcore.dll`) carries "Windows Media Audio 9.2 Lossless" as a normal
 * codec, reachable in process through IWMCodecInfo3.  This file drives it.  The
 * only new dependency is an import library for a DLL that is on every Windows
 * install, which is the right shape for a lightweight client.
 *
 * What it takes to make the lossless formats appear at all, because the obvious
 * call does not find them: `GetCodecFormatCount()` answers 0 for the lossless
 * codec until `SetCodecEnumerationSetting()` has been given g_wszVBREnabled
 * (TRUE) and g_wszNumPasses (1).  With them, codec[1] "Windows Media Audio 9.2
 * Lossless" exposes eight formats and nothing else; without them it exposes
 * none, and the honest conclusion from a first probe was "no lossless encoder".
 * Measured on this machine (WMFSDK 12.0.19041, four audio codecs total - WMA
 * Voice 9 refuses the enumeration settings with 0xC00D0BCD):
 *
 *     2ch/44100/16   2ch/44100/24   2ch/48000/24   2ch/88200/24   2ch/96000/24
 *     6ch/48000/24   6ch/88200/24   6ch/96000/24
 *
 * Two facts fall out of that table and both are honoured below.  There is no
 * 16-bit format above 44100 Hz, so a 48 kHz/16-bit source has no format with
 * its exact depth: rather than refuse it, the encoder is handed the same samples
 * in 24-bit containers, which is a left shift and moves no sample value.  And
 * there is no mono format and no format past 96000 Hz, so those sources are
 * refused outright - resampling or upmixing to make a file fit would destroy the
 * one property the user asked for.  Nothing here ever changes a sample rate, a
 * channel count or a sample value.
 *
 * Evidence for the sequence below is a from-scratch probe (`IWMCodecInfo3` ->
 * `IWMStreamConfig` -> `IWMProfile` -> `IWMWriter`) measured against FFmpeg's
 * own decoder: 3 s of 44100/16/stereo came back as `wmalossless (0x0163)`,
 * 44100 Hz, stereo, VBR flagged, duration intact, and the PCM decoded from the
 * .wma was byte-for-byte the PCM that went in with zero delay.  Tags land
 * through IWMHeaderInfo3 before BeginWriting, including CJK.
 *
 * Traps measured while finding this, all still true:
 *   - `GetMediaType()` hands back ONE buffer and `pbFormat` points inside it, so
 *     `free(pbFormat)` is heap corruption (`0xc0000409`), not a leak.
 *   - The enumeration settings are per-call state on the codec-info object: they
 *     have to be re-applied before the final `GetCodecFormat()` or the format
 *     that was found is not the format that comes back.
 *   - `IWMWriter::GetInputFormat()` takes the input number and the index, not a
 *     single index; `GetCodecInfoCount()` takes its GUID by value.
 *
 * Kept free of the C++ standard library on purpose: `transcodetool` is a C
 * executable and links this library, so a std::string or an `operator new` here
 * would be an unresolved external on its link line.  RAII is used, because it
 * costs nothing and a `goto done` over a brace-initialised local is a compile
 * error in C++ but a silent bug in the C it replaced.  The file stays ASCII and
 * C++-safe under the forced `compat/win/posix_shim.h`, which is compiled into
 * this translation unit.  The target does compile C++ with /utf-8 now, and this
 * file stays pure ASCII either way: the shared libav_transcode.h it includes is
 * UTF-8, so leaving the code page to the console draws C4819 on every compile.
 *
 * POSIX has no such encoder, so the portable build compiles
 * `wma_lossless_stub.c` instead and every caller sees "not available".
 */

#include "libav_transcode.h"

#ifdef _WIN32

#include <windows.h>
#include <mmreg.h>
#include <wmsdk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* The libav* headers are C headers without an extern "C" of their own, which
 * every C++ caller in this repository already knows: without the brace here the
 * decoder functions are looked up as mangled C++ names and the link fails on
 * every av_* symbol.  See src/spoken/AudiobookProbe.cpp for the same brace. */
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#define WL_LOG "[transcode/wma-lossless]"

namespace {

/* ---- the small RAII helpers that replace the C cleanup ladder ---- */

/* COM is process-wide state we borrow.  S_FALSE means this thread already had
 * it and owes the matching call; RPC_E_CHANGED_MODE means somebody else owns
 * this thread's model, which is theirs to uninitialise, not ours. */
struct ComScope {
    bool owned;
    bool usable;
    ComScope() : owned(false), usable(false)
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE) {
            owned = true;
            usable = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            usable = true;
        } else {
            hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            if (hr == S_OK || hr == S_FALSE) {
                owned = true;
                usable = true;
            } else if (hr == RPC_E_CHANGED_MODE) {
                usable = true;
            }
        }
    }
    ~ComScope() { if (owned) CoUninitialize(); }
private:
    ComScope(const ComScope &);
    ComScope &operator=(const ComScope &);
};

/* Everything FFmpeg owns for one encode. */
struct Source {
    AVFormatContext *fmt;
    AVCodecContext *ctx;
    SwrContext *swr;
    AVFrame *frame;
    AVPacket *pkt;
    AVChannelLayout outLayout;
    int streamIndex;
    int rate;
    int channels;
    int depth;         /* integer bits per sample of the source content */
    int outBits;       /* bits the chosen lossless format carries */
    int blockAlign;
    unsigned char *scratch;
    int scratchBytes;

    Source() : fmt(NULL), ctx(NULL), swr(NULL), frame(NULL), pkt(NULL),
               scratch(NULL), scratchBytes(0), streamIndex(-1), rate(0),
               channels(0), depth(0), outBits(0), blockAlign(0)
    {
        memset(&outLayout, 0, sizeof(outLayout));
    }
    ~Source()
    {
        if (fmt) avformat_close_input(&fmt);
        if (ctx) avcodec_free_context(&ctx);
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        av_channel_layout_uninit(&outLayout);
        free(scratch);
    }
private:
    Source(const Source &);
    Source &operator=(const Source &);
};

/* Everything the Windows Media writer owns.  EndWriting runs before Release and
 * before this object's scope closes, which is what lets the caller delete a
 * rejected file at all - the writer holds it open until then. */
struct Writer {
    IWMProfileManager *profileManager;
    IWMCodecInfo3 *codecInfo;
    IWMStreamConfig *stream;
    IWMProfile *profile;
    IWMWriter *writer;
    IWMInputMediaProps *inputProps;
    BOOL began;
    /* The timestamp base belongs to one encode, so it lives here and not at
     * file scope: a settings self-test and a sync in the same process must not
     * start the second file at the end of the first one's timeline. */
    long long framesWritten;

    Writer() : profileManager(NULL), codecInfo(NULL), stream(NULL),
               profile(NULL), writer(NULL), inputProps(NULL), began(FALSE),
               framesWritten(0) {}
    ~Writer()
    {
        if (writer) {
            if (began) {
                const HRESULT hr = writer->EndWriting();
                if (FAILED(hr))
                    fprintf(stderr, WL_LOG " EndWriting failed hr=0x%08X\n", (unsigned)hr);
            }
            writer->Release();
        }
        if (inputProps) inputProps->Release();
        if (profile) profile->Release();
        if (stream) stream->Release();
        if (codecInfo) codecInfo->Release();
        if (profileManager) profileManager->Release();
    }
private:
    Writer(const Writer &);
    Writer &operator=(const Writer &);
};

/* ---- one enumerated format the machine can actually encode ---- */

struct LosslessFormat {
    int codec;      /* index in IWMCodecInfo3's audio codec list */
    int format;     /* index of this format inside that codec */
    int channels;
    int rate;
    int bits;
};

WAVEFORMATEX *waveOf(WM_MEDIA_TYPE *mt)
{
    if (!mt || mt->cbFormat < sizeof(WAVEFORMATEX))
        return NULL;
    return (WAVEFORMATEX *)mt->pbFormat;
}

/* The media type as one calloc'd block.  pbFormat points inside it, so freeing
 * the block is the whole cleanup and freeing pbFormat is heap corruption. */
WM_MEDIA_TYPE *readType(IWMMediaProps *props)
{
    DWORD cb = 0;
    if (FAILED(props->GetMediaType(NULL, &cb)) || cb == 0)
        return NULL;
    unsigned char *raw = (unsigned char *)calloc(cb + 8, 1);
    if (!raw)
        return NULL;
    DWORD got = cb;
    if (FAILED(props->GetMediaType((WM_MEDIA_TYPE *)raw, &got))) {
        free(raw);
        return NULL;
    }
    return (WM_MEDIA_TYPE *)raw;
}

/* The two settings the lossless formats are hidden behind. */
void applyEnumerationSettings(IWMCodecInfo3 *info, DWORD codec)
{
    const BOOL vbr = TRUE;
    const DWORD passes = 1;
    info->SetCodecEnumerationSetting(WMMEDIATYPE_Audio, codec, g_wszVBREnabled,
                                     WMT_TYPE_BOOL, (const BYTE *)&vbr, sizeof(vbr));
    info->SetCodecEnumerationSetting(WMMEDIATYPE_Audio, codec, g_wszNumPasses,
                                     WMT_TYPE_DWORD, (const BYTE *)&passes,
                                     sizeof(passes));
}

int collectLosslessFormats(IWMCodecInfo3 *info, LosslessFormat *out, int maxOut)
{
    DWORD codecs = 0;
    if (FAILED(info->GetCodecInfoCount(WMMEDIATYPE_Audio, &codecs)))
        return 0;
    int found = 0;
    for (DWORD c = 0; c < codecs; c++) {
        applyEnumerationSettings(info, c);
        DWORD formats = 0;
        if (FAILED(info->GetCodecFormatCount(WMMEDIATYPE_Audio, c, &formats)) ||
            formats == 0)
            continue;
        for (DWORD f = 0; f < formats && found < maxOut; f++) {
            IWMStreamConfig *sc = NULL;
            if (FAILED(info->GetCodecFormat(WMMEDIATYPE_Audio, c, f, &sc)) || !sc)
                continue;
            IWMMediaProps *props = NULL;
            if (SUCCEEDED(sc->QueryInterface(IID_IWMMediaProps, (void **)&props)) && props) {
                WM_MEDIA_TYPE *mt = readType(props);
                WAVEFORMATEX *w = waveOf(mt);
                if (w && w->wFormatTag == WAVE_FORMAT_WMAUDIO_LOSSLESS && found < maxOut) {
                    LosslessFormat *slot = &out[found++];
                    slot->codec = (int)c;
                    slot->format = (int)f;
                    slot->channels = (int)w->nChannels;
                    slot->rate = (int)w->nSamplesPerSec;
                    slot->bits = (int)w->wBitsPerSample;
                }
                free(mt);
                props->Release();
            }
            sc->Release();
        }
    }
    return found;
}

/* Exact match first.  Failing that, the same rate and channels at a wider
 * container, which is a shift and not a change: a 16-bit sample placed in the
 * top half of a 24-bit sample is the same number, and a decoder that reads it
 * back at 16 bits gets the original bytes.  Nothing wider than 24 bits is ever
 * offered and nothing narrower is ever chosen. */
const LosslessFormat *chooseFormat(const LosslessFormat *formats, int count,
                                   int channels, int rate, int depth)
{
    for (int i = 0; i < count; i++)
        if (formats[i].channels == channels && formats[i].rate == rate &&
            formats[i].bits == depth)
            return &formats[i];
    if (depth <= 24) {
        for (int i = 0; i < count; i++)
            if (formats[i].channels == channels && formats[i].rate == rate &&
                formats[i].bits >= depth && formats[i].bits <= 24)
                return &formats[i];
    }
    return NULL;
}

wchar_t *toWide(const char *utf8)
{
    if (!utf8 || !utf8[0])
        return NULL;
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (chars <= 1)
        return NULL;
    wchar_t *wide = (wchar_t *)calloc((size_t)chars + 1, sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, chars) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

/* KSAUDIO_SPEAKER_* without dragging in ksmedia.h and its kernel headers. */
DWORD speakerMaskFor(int channels)
{
    switch (channels) {
    case 1:  return 0x4;    /* Speaker_C */
    case 2:  return 0x3;    /* Speaker_C | Speaker_RFC ... KSAUDIO_SPEAKER_STEREO */
    case 4:  return 0x33;   /* quadraphonic */
    case 6:  return 0x3F;   /* KSAUDIO_SPEAKER_5POINT1 */
    case 8:  return 0x63F;  /* 7.1 */
    default: return 0x3;
    }
}

/* ASF's own attribute set, not FFmpeg's short names.  Title/Author/AlbumTitle/
 * Genre/TrackNumber/Composer/PartOfSet are what a Windows Media reader looks
 * for, and the same album-artist override the MP3 and wmav2 paths apply keeps
 * one album under one artist on the player. */
void addStringTag(IWMHeaderInfo3 *header, const wchar_t *name, const char *utf8)
{
    if (!utf8 || !utf8[0])
        return;
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (chars <= 1)
        return;
    wchar_t *wide = (wchar_t *)calloc((size_t)chars + 1, sizeof(wchar_t));
    if (!wide)
        return;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, chars) > 0) {
        WORD index = 0;
        const HRESULT hr = header->AddAttribute(
            0, name, &index, WMT_TYPE_STRING, 0, (const BYTE *)wide,
            (DWORD)((DWORD)chars * (DWORD)sizeof(wchar_t)));
        if (FAILED(hr))
            fprintf(stderr, WL_LOG " tag %ls hr=0x%08X\n", name, (unsigned)hr);
    }
    free(wide);
}

/* WM/Year is a DWORD attribute, so a date tag gets its leading digits and
 * nothing else.  "2007-05-04" is the year 2007, and writing the string into a
 * numeric attribute is how a tag ends up unreadable on the player. */
void addYearTag(IWMHeaderInfo3 *header, const char *date)
{
    if (!date)
        return;
    int year = 0;
    int i = 0;
    while (date[i] >= '0' && date[i] <= '9' && i < 4) {
        year = year * 10 + (date[i] - '0');
        i++;
    }
    if (i < 4 || year <= 0 || year > 9999)
        return;
    WORD index = 0;
    const DWORD value = (DWORD)year;
    header->AddAttribute(0, L"Year", &index, WMT_TYPE_DWORD, 0,
                         (const BYTE *)&value, sizeof(value));
}

const char *tagOf(AVFormatContext *fmt, AVStream *stream, const char *key)
{
    const AVDictionaryEntry *entry = av_dict_get(fmt->metadata, key, NULL, 0);
    if (entry && entry->value && entry->value[0])
        return entry->value;
    if (stream) {
        entry = av_dict_get(stream->metadata, key, NULL, 0);
        if (entry && entry->value && entry->value[0])
            return entry->value;
    }
    return NULL;
}

void writeTags(Writer &writer, AVFormatContext *fmt, AVStream *stream,
               const char *albumArtist, int discNumber)
{
    IWMHeaderInfo3 *header = NULL;
    if (FAILED(writer.writer->QueryInterface(IID_IWMHeaderInfo3, (void **)&header)) ||
        !header) {
        fprintf(stderr, WL_LOG " no IWMHeaderInfo3, tags stay behind\n");
        return;
    }
    addStringTag(header, L"Title", tagOf(fmt, stream, "title"));
    const char *artist = tagOf(fmt, stream, "artist");
    if (albumArtist && albumArtist[0])
        addStringTag(header, L"Author", albumArtist);
    else
        addStringTag(header, L"Author", artist);
    addStringTag(header, L"AlbumTitle", tagOf(fmt, stream, "album"));
    addStringTag(header, L"Genre", tagOf(fmt, stream, "genre"));
    addStringTag(header, L"Composer", tagOf(fmt, stream, "composer"));
    addStringTag(header, L"TrackNumber", tagOf(fmt, stream, "track"));
    addYearTag(header, tagOf(fmt, stream, "date"));
    if (discNumber > 0) {
        char disc[16];
        sprintf(disc, "%d", discNumber);
        addStringTag(header, L"PartOfSet", disc);
    } else {
        addStringTag(header, L"PartOfSet", tagOf(fmt, stream, "disc"));
    }
    header->Release();
}

/* ---- decode side ---- */

/* The source's own integer depth.  bits_per_raw_sample is what a FLAC or WMA
 * Lossless stream carries; a packed PCM codec states its own width; everything
 * else (MP3, AAC, WMA) is 16-bit content whatever the decoder happens to hold
 * in memory, and the decoder's sample format is not evidence about the source. */
int sourceDepth(AVCodecContext *ctx)
{
    const int raw = ctx->bits_per_raw_sample;
    if (raw >= 8 && raw <= 32)
        return raw;
    const int coded = av_get_bits_per_sample(ctx->codec_id);
    if (coded >= 8 && coded <= 32)
        return coded;
    return 16;
}

int openSource(Source &src, const char *inputPath)
{
    if (avformat_open_input(&src.fmt, inputPath, NULL, NULL) < 0) {
        fprintf(stderr, WL_LOG " can't open input: %s\n", inputPath);
        return 0;
    }
    if (avformat_find_stream_info(src.fmt, NULL) < 0)
        return 0;
    src.streamIndex = av_find_best_stream(src.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (src.streamIndex < 0) {
        fprintf(stderr, WL_LOG " no audio stream\n");
        return 0;
    }
    AVStream *stream = src.fmt->streams[src.streamIndex];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, WL_LOG " no decoder for this stream\n");
        return 0;
    }
    src.ctx = avcodec_alloc_context3(decoder);
    if (!src.ctx ||
        avcodec_parameters_to_context(src.ctx, stream->codecpar) < 0 ||
        avcodec_open2(src.ctx, decoder, NULL) < 0) {
        fprintf(stderr, WL_LOG " decoder would not open\n");
        return 0;
    }
    src.rate = src.ctx->sample_rate;
    src.channels = src.ctx->ch_layout.nb_channels;
    src.depth = sourceDepth(src.ctx);
    src.frame = av_frame_alloc();
    src.pkt = av_packet_alloc();
    if (!src.frame || !src.pkt)
        return 0;
    if (src.rate <= 0 || src.channels <= 0) {
        fprintf(stderr, WL_LOG " stream has no rate or channel count\n");
        return 0;
    }
    return 1;
}

/* Reserve the scratch buffer that every converted frame lands in, sized for the
 * widest frame a decoder may hand over. */
int prepareResampler(Source &src, const LosslessFormat &target)
{
    src.outBits = target.bits;
    src.blockAlign = target.channels * (target.bits / 8);
    const enum AVSampleFormat outFmt =
        target.bits == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;

    if (src.ctx->ch_layout.nb_channels > 0 &&
        src.ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_copy(&src.outLayout, &src.ctx->ch_layout);
    else
        av_channel_layout_default(&src.outLayout, src.channels);

    /* Identical layout and identical rate, so this resampler only converts the
     * sample format and interleaves - which is the whole point: a lossless
     * encode that resamples is not lossless. */
    if (swr_alloc_set_opts2(&src.swr, &src.outLayout, outFmt, src.rate,
                            &src.ctx->ch_layout, src.ctx->sample_fmt, src.rate,
                            0, NULL) < 0 ||
        swr_init(src.swr) < 0) {
        fprintf(stderr, WL_LOG " could not set up the sample format conversion\n");
        return 0;
    }
    src.scratchBytes = (16384 + 1024) * src.blockAlign;
    src.scratch = (unsigned char *)malloc((size_t)src.scratchBytes);
    if (!src.scratch) {
        fprintf(stderr, WL_LOG " out of memory\n");
        return 0;
    }
    return 1;
}

/* One contiguous run of target-format PCM into the writer, in chunks a muxer
 * queue is happy with.  Timestamps are recomputed from the total frame count so
 * rounding cannot drift away from the declared sample rate. */
int emitPcm(Writer &writer, const unsigned char *data, int bytes, int rate,
            int blockAlign)
{
    static const int kChunkBytes = 65536;
    long long framesWritten = writer.framesWritten;
    int offset = 0;
    while (bytes - offset > 0) {
        const int remaining = bytes - offset;
        const unsigned long n =
            (unsigned long)(remaining > kChunkBytes ? kChunkBytes : remaining);
        INSSBuffer *sample = NULL;
        if (FAILED(writer.writer->AllocateSample(n, &sample)) || !sample) {
            fprintf(stderr, WL_LOG " AllocateSample failed at %d bytes\n", offset);
            return 0;
        }
        BYTE *buffer = NULL;
        if (FAILED(sample->GetBuffer(&buffer)) || !buffer) {
            sample->Release();
            fprintf(stderr, WL_LOG " no sample buffer\n");
            return 0;
        }
        memcpy(buffer, data + offset, n);
        sample->SetLength((DWORD)n);
        const long long stamps = (long long)((double)framesWritten * 10000000.0 /
                                             (double)rate);
        const HRESULT hr = writer.writer->WriteSample(0, (QWORD)stamps, 0, sample);
        sample->Release();
        if (FAILED(hr)) {
            fprintf(stderr, WL_LOG " WriteSample failed hr=0x%08X\n", (unsigned)hr);
            return 0;
        }
        framesWritten += (long long)(n / (unsigned long)blockAlign);
        offset += (int)n;
    }
    writer.framesWritten = framesWritten;
    return 1;
}

int growScratch(Source &src, int needed)
{
    if (needed <= src.scratchBytes)
        return 1;
    const int bigger = needed + src.blockAlign;
    unsigned char *grown = (unsigned char *)realloc(src.scratch, (size_t)bigger);
    if (!grown) {
        fprintf(stderr, WL_LOG " out of memory\n");
        return 0;
    }
    src.scratch = grown;
    src.scratchBytes = bigger;
    return 1;
}

/* Byte count the last rames converted frames occupy in the scratch buffer,
 * compacting in place when the target container is narrower than the one FFmpeg
 * decodes into. */
int packedBytes(Source &src, int frames)
{
    if (src.outBits != 24)
        return frames * src.blockAlign;
    /* S32 is the full-range 32-bit container FFmpeg uses; a 24-bit sample is its
     * top three bytes.  Compacting forward is safe because 3 < 4. */
    const int samples = frames * src.channels;
    unsigned char *base = src.scratch;
    for (int i = 0; i < samples; i++) {
        base[i * 3 + 0] = base[i * 4 + 1];
        base[i * 3 + 1] = base[i * 4 + 2];
        base[i * 3 + 2] = base[i * 4 + 3];
    }
    return samples * 3;
}

int sendFrame(Writer &writer, Source &src, AVFrame *frame)
{
    const int need = swr_get_out_samples(src.swr, frame->nb_samples);
    if (need <= 0)
        return 1;
    if (!growScratch(src, need * src.blockAlign))
        return 0;
    const int got = swr_convert(src.swr, &src.scratch, need,
                                (const uint8_t **)frame->extended_data,
                                frame->nb_samples);
    if (got < 0) {
        fprintf(stderr, WL_LOG " sample format conversion failed\n");
        return 0;
    }
    if (got == 0)
        return 1;
    return emitPcm(writer, src.scratch, packedBytes(src, got), src.rate, src.blockAlign);
}

int drainDecoder(Writer &writer, Source &src)
{
    int ret;
    while ((ret = avcodec_receive_frame(src.ctx, src.frame)) == 0) {
        const int ok = sendFrame(writer, src, src.frame);
        av_frame_unref(src.frame);
        if (!ok)
            return 0;
        if (zuuned_transcode_abort_requested())
            return -1;
    }
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 1 : 0;
}

/* ---- the writer side of one encode ---- */

/* The profile manager and the codec-info object underneath it.  Lossless formats
 * only answer IWMCodecInfo3; the older interfaces on the same object do not list
 * them, which is why a first probe of this machine said "no lossless encoder". */
int openCodecInfo(Writer &writer)
{
    HRESULT hr = WMCreateProfileManager(&writer.profileManager);
    if (FAILED(hr) || !writer.profileManager) {
        fprintf(stderr, WL_LOG " WMCreateProfileManager hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    hr = writer.profileManager->QueryInterface(IID_IWMCodecInfo3,
                                              (void **)&writer.codecInfo);
    if (FAILED(hr) || !writer.codecInfo) {
        fprintf(stderr, WL_LOG " QI(IWMCodecInfo3) hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    return 1;
}

/* Hand the chosen format to a fresh 9.0 profile and open a writer over it.
 *
 * The enumeration settings are re-applied here on purpose.  They are per-call
 * state on the codec-info object rather than a property of it, so by the time the
 * chosen index is looked up again the settings that made it visible are gone, and
 * GetCodecFormat() then returns a different stream config than the one that was
 * matched - measured, not assumed. */
int attachStream(Writer &writer, const LosslessFormat &fmt)
{
    applyEnumerationSettings(writer.codecInfo, (DWORD)fmt.codec);
    HRESULT hr = writer.codecInfo->GetCodecFormat(WMMEDIATYPE_Audio, (DWORD)fmt.codec,
                                                  (DWORD)fmt.format, &writer.stream);
    if (FAILED(hr) || !writer.stream) {
        fprintf(stderr, WL_LOG " GetCodecFormat(codec=%d format=%d) hr=0x%08X\n",
                fmt.codec, fmt.format, (unsigned)hr);
        return 0;
    }
    hr = writer.stream->SetStreamNumber(1);
    if (FAILED(hr)) {
        fprintf(stderr, WL_LOG " SetStreamNumber hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    hr = writer.profileManager->CreateEmptyProfile(WMT_VER_9_0, &writer.profile);
    if (FAILED(hr) || !writer.profile) {
        fprintf(stderr, WL_LOG " CreateEmptyProfile hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    hr = writer.profile->AddStream(writer.stream);
    if (FAILED(hr)) {
        fprintf(stderr, WL_LOG " AddStream hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    hr = WMCreateWriter(NULL, &writer.writer);
    if (FAILED(hr) || !writer.writer) {
        fprintf(stderr, WL_LOG " WMCreateWriter hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    hr = writer.writer->SetProfile(writer.profile);
    if (FAILED(hr)) {
        fprintf(stderr, WL_LOG " SetProfile hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    DWORD inputs = 0;
    writer.writer->GetInputCount(&inputs);
    if (inputs == 0) {
        fprintf(stderr, WL_LOG " the writer took the profile and offered no input\n");
        return 0;
    }
    return 1;
}

/* Declare the PCM the caller is about to feed in.  The writer starts out asking
 * for the format its codec chose, which is right for rate and depth but not for a
 * 5.1 channel mask, so the block is rewritten rather than replaced - the format
 * tag and everything the codec itself decided stay as they came. */
int declareInputFormat(Writer &writer, const Source &src)
{
    HRESULT hr = writer.writer->GetInputProps(0, &writer.inputProps);
    if (FAILED(hr) || !writer.inputProps) {
        fprintf(stderr, WL_LOG " GetInputProps hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    WM_MEDIA_TYPE *mt = readType(writer.inputProps);
    WAVEFORMATEX *w = waveOf(mt);
    if (!w) {
        fprintf(stderr, WL_LOG " the writer's input format is not a WAVEFORMATEX\n");
        free(mt);
        return 0;
    }
    w->nChannels = (WORD)src.channels;
    w->nSamplesPerSec = (DWORD)src.rate;
    w->wBitsPerSample = (WORD)src.outBits;
    w->nBlockAlign = (WORD)src.blockAlign;
    w->nAvgBytesPerSec = (DWORD)((unsigned long)src.blockAlign * (unsigned long)src.rate);
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE *we = (WAVEFORMATEXTENSIBLE *)w;
        we->dwChannelMask = speakerMaskFor(src.channels);
        /* The container width, not the source depth.  A 16-bit source in a 24-bit
         * lossless format IS a 24-bit stream whose low byte is zero; claiming 16
         * valid bits would tell the encoder it may discard the container. */
        we->Samples.wValidBitsPerSample = (WORD)src.outBits;
    }
    hr = writer.inputProps->SetMediaType(mt);
    free(mt); /* one buffer; pbFormat points inside it */
    if (FAILED(hr)) {
        fprintf(stderr, WL_LOG " SetMediaType hr=0x%08X - this machine will not take "
                "%d ch / %d Hz / %d-bit as lossless input\n", (unsigned)hr,
                src.channels, src.rate, src.outBits);
        return 0;
    }
    hr = writer.writer->SetInputProps(0, writer.inputProps);
    if (FAILED(hr)) {
        fprintf(stderr, WL_LOG " SetInputProps hr=0x%08X\n", (unsigned)hr);
        return 0;
    }
    return 1;
}

/* Everything the resampler is still holding, out through the writer.  Passing a
 * real output buffer and no input is what delivers them;
 * swr_convert(swr, NULL, 0, NULL, 0) only reports a count. */
int flushResampler(Writer &writer, Source &src)
{
    for (;;) {
        const int64_t delayed = swr_get_delay(src.swr, (int64_t)src.rate);
        if (delayed <= 0)
            return 1;
        const int chunk = delayed > 4096 ? 4096 : (int)delayed;
        if (!growScratch(src, chunk * src.blockAlign))
            return 0;
        const int got = swr_convert(src.swr, &src.scratch, chunk, NULL, 0);
        if (got <= 0)
            return got < 0 ? 0 : 1;
        if (!emitPcm(writer, src.scratch, packedBytes(src, got), src.rate,
                     src.blockAlign))
            return 0;
    }
}
} // namespace

/* ---- public entry points ---- */

/* Does this machine carry a WMA Lossless encoder at all?  Every Windows install
 * with the Media Format runtime does, and every one answers 0 until the two
 * enumeration settings are applied - so this asks the real question rather than
 * testing for a file.  Cached because the answer cannot change during a run and
 * the settings page asks it on every repaint. */
static int probeLosslessAvailability(void)
{
    ComScope com;
    if (!com.usable)
        return 0;
    Writer writer;
    if (!openCodecInfo(writer))
        return 0;
    LosslessFormat formats[64];
    return collectLosslessFormats(writer.codecInfo, formats, 64) > 0 ? 1 : 0;
}

extern "C" int zuuned_wma_lossless_available(void)
{
    /* Constant initialiser, deliberately: a lazily guarded static would need
     * __cxa_guard_acquire, and this library is linked by a C executable that has
     * no reason to expect the C++ runtime to be part of its link line. */
    static int cached = -1;
    if (cached < 0)
        cached = probeLosslessAvailability();
    return cached;
}

extern "C" char *zuuned_audio_encode_wma_lossless(const char *input_path,
                                                 const char *album_artist,
                                                 int disc_number,
                                                 const char *output_path,
                                                 zuuned_progress_cb progress,
                                                 void *userdata)
{
    if (!input_path || !output_path)
        return NULL;
    ComScope com;
    if (!com.usable) {
        fprintf(stderr, WL_LOG " this thread has no COM apartment to build a profile in\n");
        return NULL;
    }
    wchar_t *outWide = toWide(output_path);
    if (!outWide)
        return NULL;

    char *result = NULL;
    int wrote = 0;
    {
        /* Two scopes, one per library, and neither needs a cleanup ladder: what
         * FFmpeg owns and what the writer owns are each released when the brace
         * closes, and the writer's EndWriting() runs before anything tries to
         * delete a rejected file. */
        Source src;
        Writer writer;

        do {
            if (!openCodecInfo(writer))
                break;
            if (!openSource(src, input_path))
                break;

            LosslessFormat formats[64];
            const int count = collectLosslessFormats(writer.codecInfo, formats, 64);
            if (count == 0) {
                fprintf(stderr, WL_LOG " this Windows has no WMA Lossless encoder\n");
                break;
            }
            const LosslessFormat *chosen = chooseFormat(formats, count, src.channels,
                                                        src.rate, src.depth);
            if (!chosen) {
                /* The honest answer.  There is no mono format and nothing above
                 * 96 kHz or 24 bits, and upmixing, resampling or narrowing a
                 * sample to fit would throw away the one property the user chose
                 * this target for, so a refusal beats a file that is not what it
                 * claims to be. */
                fprintf(stderr, WL_LOG " no lossless format for %d ch / %d Hz / %d-bit"
                        " (%d formats offered); refusing instead of altering the"
                        " audio\n", src.channels, src.rate, src.depth, count);
                break;
            }
            fprintf(stderr, WL_LOG " target %d ch / %d Hz / %d-bit for a %d ch /"
                    " %d Hz / %d-bit source\n", chosen->channels, chosen->rate,
                    chosen->bits, src.channels, src.rate, src.depth);

            if (!prepareResampler(src, *chosen))
                break;
            if (!attachStream(writer, *chosen))
                break;
            if (!declareInputFormat(writer, src))
                break;
            writeTags(writer, src.fmt, src.fmt->streams[src.streamIndex],
                      album_artist, disc_number);

            /* The writer keeps the file open until EndWriting, so an existing file
             * goes before BeginWriting and not after. */
            DeleteFileW(outWide);
            HRESULT hr = writer.writer->SetOutputFilename(outWide);
            if (FAILED(hr)) {
                fprintf(stderr, WL_LOG " SetOutputFilename hr=0x%08X\n", (unsigned)hr);
                break;
            }
            hr = writer.writer->BeginWriting();
            if (FAILED(hr)) {
                fprintf(stderr, WL_LOG " BeginWriting hr=0x%08X\n", (unsigned)hr);
                break;
            }
            writer.began = TRUE;

            const long long totalUs = src.fmt->duration > 0 ? src.fmt->duration : 0;
            long long dropped = 0;
            int lastPct = -1;
            int failed = 0;
            int cancelled = 0;

            for (;;) {
                if (zuuned_transcode_abort_requested()) {
                    fprintf(stderr, WL_LOG " cancelled\n");
                    cancelled = 1;
                    break;
                }
                const int ret = av_read_frame(src.fmt, src.pkt);
                if (ret < 0)
                    break; /* EOF; a real read error shows up as a length mismatch */
                if (src.pkt->stream_index != src.streamIndex) {
                    av_packet_unref(src.pkt);
                    continue;
                }
                /* EAGAIN means the decoder is full, not that this packet is
                 * unusable: drain and retry the SAME packet.  Dropping it deletes
                 * audio in silence - measured at 1.58 s off the end of a track on
                 * the other encoder, with the encode still reporting success. */
                int sent = avcodec_send_packet(src.ctx, src.pkt);
                if (sent == AVERROR(EAGAIN)) {
                    const int drained = drainDecoder(writer, src);
                    av_packet_unref(src.pkt);
                    if (drained < 0) { cancelled = 1; break; }
                    if (drained == 0) { failed = 1; break; }
                    sent = avcodec_send_packet(src.ctx, src.pkt);
                }
                av_packet_unref(src.pkt);
                if (sent < 0) {
                    /* Only now is dropping honest: the decoder refused this packet
                     * outright, which is corruption, not backpressure. */
                    dropped++;
                    continue;
                }
                const int drained = drainDecoder(writer, src);
                if (drained < 0) { cancelled = 1; break; }
                if (drained == 0) { failed = 1; break; }

                if (progress && totalUs > 0 && src.rate > 0) {
                    const double end = (double)totalUs / 1000000.0 * (double)src.rate;
                    const int pct = end > 0.0
                        ? (int)((double)writer.framesWritten * 100.0 / end) : 0;
                    if (pct != lastPct && pct <= 100) {
                        lastPct = pct;
                        progress((float)pct / 100.0f, userdata);
                    }
                }
            }

            if (!failed && !cancelled) {
                avcodec_send_packet(src.ctx, NULL);
                const int drained = drainDecoder(writer, src);
                if (drained < 0) cancelled = 1;
                else if (drained == 0) failed = 1;
            }
            if (!failed && !cancelled && !flushResampler(writer, src))
                failed = 1;

            if (dropped > 0)
                fprintf(stderr, WL_LOG " %lld packet(s) the decoder refused were"
                        " dropped\n", dropped);
            if (cancelled)
                break;
            if (failed) {
                fprintf(stderr, WL_LOG " encode did not finish\n");
                break;
            }
            if (writer.framesWritten <= 0) {
                fprintf(stderr, WL_LOG " produced no audio\n");
                break;
            }
            if (progress)
                progress(1.0f, userdata);
            fprintf(stderr, WL_LOG " wrote %lld samples as %d ch / %d Hz / %d-bit"
                    " lossless\n", writer.framesWritten, chosen->channels,
                    chosen->rate, chosen->bits);
            wrote = 1;
        } while (0);
    }

    if (wrote) {
        result = strdup(output_path);
    } else {
        /* Nothing partial is left behind: a half-written ASF is not a file worth
         * retrying, and the caller reports a failure it can name. */
        DeleteFileW(outWide);
    }
    free(outWide);
    return result;
}

#endif /* _WIN32 */