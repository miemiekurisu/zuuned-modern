#pragma once

// The file facts a listener asks about: what container, what codec, at what
// rate.  Nothing here stores them; both answers are read out of the media
// itself, by the transcode layer, and this header is the one place that turns
// that read into the keys QML shows.
//
// It is a header rather than a translation unit because the two callers cannot
// share anything heavier: src/LibraryService.cpp reads a file on this machine
// and src/DeviceWorker.cpp reads the front of an object off a player, and only
// the second is barred from transcode/libav_transcode.h (see the C2371 note at
// the top of that file's use in DeviceWorker.cpp).  Both roads are handed the
// same key=value text, so both parse it here and cannot drift into two
// vocabularies for one question.

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace MediaDescription {

// The lines zuuned_describe_audio_file() writes: `key=value`, one per line, a
// value running to the end of the line and never holding a newline.  Names are
// kept as the text FFmpeg answered - avcodec_get_name() says "MP3 (MPEG audio
// layer 3)" and that is what the user should read - and numbers stay numbers.
// A key that is absent stays absent, which the sheet shows as unknown; filling
// it with 0 would be a claim the file never made.
inline QVariantMap parse(const QString &lines) {
    QVariantMap out;
    const QStringList rows = lines.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &row : rows) {
        const int equals = row.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = row.left(equals);
        const QString value = row.mid(equals + 1);
        if (key == QLatin1String("probed")) {
            out[QStringLiteral("probed")] = value.toInt() != 0;
            continue;
        }
        if (key == QLatin1String("lossless")) {
            out[QStringLiteral("lossless")] = value.toInt() != 0;
            continue;
        }
        if (key == QLatin1String("format") || key == QLatin1String("codec")) {
            out[key] = value;
            continue;
        }
        if (key == QLatin1String("bit_rate"))
            out[QStringLiteral("bitRate")] = value.toLongLong();
        else if (key == QLatin1String("sample_rate"))
            out[QStringLiteral("sampleRate")] = value.toLongLong();
        else if (key == QLatin1String("channels"))
            out[QStringLiteral("channels")] = value.toInt();
        else if (key == QLatin1String("bits_per_sample"))
            out[QStringLiteral("bitsPerSample")] = value.toInt();
        else if (key == QLatin1String("duration_ms"))
            out[QStringLiteral("durationMs")] = value.toLongLong();
        // Anything else is a key this build does not know: not an error, and
        // not a reason to drop the lines that came after it.
    }
    return out;
}

} // namespace MediaDescription
