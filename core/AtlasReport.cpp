#include "AtlasReport.h"

#include "AppPaths.h"
#include "Utf8Streams.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace {
constexpr qint64 MaxReportSizeBytes = 5 * 1024 * 1024;

QString withSuffix(const QString &baseName, int suffix)
{
    if (suffix <= 0) {
        return baseName;
    }

    const qsizetype dotIndex = baseName.lastIndexOf(QLatin1Char('.'));
    if (dotIndex < 0) {
        return QStringLiteral("%1_%2").arg(baseName).arg(suffix, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1_%2%3")
        .arg(baseName.left(dotIndex))
        .arg(suffix, 2, 10, QLatin1Char('0'))
        .arg(baseName.mid(dotIndex));
}
}

void AtlasReport::append(File file, const QString &entry)
{
    QDir().mkpath(AppPaths::databasePath());

    int suffix = 0;
    QString path = reportPath(file, suffix);
    while (QFileInfo::exists(path) && QFileInfo(path).size() >= MaxReportSizeBytes) {
        ++suffix;
        path = reportPath(file, suffix);
    }

    QFile reportFile(path);
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&reportFile);
    configureUtf8Stream(stream);
    stream << entry;
    if (!entry.endsWith(QLatin1Char('\n'))) {
        stream << Qt::endl;
    }
    stream << Qt::endl;
}

void AtlasReport::appendFlux(const QString &level, const QString &message, const QString &details)
{
    QString entry;
    QTextStream stream(&entry);
    stream << "[" << level << "]" << Qt::endl;
    stream << "Data/hora: " << timestamp() << Qt::endl;
    stream << message << Qt::endl;
    if (!details.trimmed().isEmpty()) {
        stream << Qt::endl << details.trimmed() << Qt::endl;
    }

    append(File::Flux, entry);
}

QString AtlasReport::timestamp()
{
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

QString AtlasReport::formatDuration(qint64 elapsedNs)
{
    const qint64 totalMs = elapsedNs / 1000000;
    const qint64 hours = totalMs / 3600000;
    const qint64 minutes = (totalMs % 3600000) / 60000;
    const qint64 seconds = (totalMs % 60000) / 1000;
    const qint64 milliseconds = totalMs % 1000;

    return QStringLiteral("%1:%2:%3.%4 (%5 ms, %6 s)")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'))
        .arg(totalMs)
        .arg(static_cast<double>(elapsedNs) / 1000000000.0, 0, 'f', 3);
}

QString AtlasReport::formatMilliseconds(qint64 elapsedNs)
{
    return QStringLiteral("%1 ms")
        .arg(static_cast<double>(elapsedNs) / 1000000.0, 0, 'f', 3);
}

QString AtlasReport::reportPath(File file, int suffix)
{
    return QDir::cleanPath(QDir(AppPaths::databasePath()).filePath(withSuffix(reportBaseName(file), suffix)));
}

QString AtlasReport::reportBaseName(File file)
{
    switch (file) {
    case File::Import:
        return QStringLiteral("Atlas_Import_Report.txt");
    case File::ImportRejectedDetails:
        return QStringLiteral("Atlas_Import_Rejected_Details.txt");
    case File::Preprocess:
        return QStringLiteral("Atlas_Preprocess_Report.txt");
    case File::Translate:
        return QStringLiteral("Atlas_Translate_Report.txt");
    case File::Flux:
        return QStringLiteral("Atlas_Flux_Report.txt");
    }

    return QStringLiteral("Atlas_Flux_Report.txt");
}
