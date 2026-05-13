#ifndef UTF8STREAMS_H
#define UTF8STREAMS_H

#include <QStringConverter>
#include <QTextStream>

inline void configureUtf8Stream(QTextStream &stream)
{
    stream.setEncoding(QStringConverter::Utf8);
}

#endif // UTF8STREAMS_H
