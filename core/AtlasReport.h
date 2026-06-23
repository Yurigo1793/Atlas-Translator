#ifndef ATLASREPORT_H
#define ATLASREPORT_H

#include <QString>

class AtlasReport
{
public:
    enum class File {
        Import,
        ImportRejectedDetails,
        Preprocess,
        Translate,
        Flux
    };

    static void append(File file, const QString &entry);
    static void appendFlux(const QString &level, const QString &message, const QString &details = QString());
    static QString timestamp();
    static QString formatDuration(qint64 elapsedNs);
    static QString formatMilliseconds(qint64 elapsedNs);

private:
    static QString reportPath(File file, int suffix = 0);
    static QString reportBaseName(File file);
};

#endif // ATLASREPORT_H
