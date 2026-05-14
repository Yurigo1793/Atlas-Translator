#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QStringList>

namespace {
QString makePath(const QString &directoryName)
{
    return QDir::cleanPath(QDir(AppPaths::basePath()).filePath(directoryName));
}

bool hasDatasetFiles(const QString &path)
{
    const QDir directory(path);
    if (!directory.exists()) {
        return false;
    }

    const QFileInfoList files = directory.entryInfoList(QDir::Files | QDir::Readable);
    for (const QFileInfo &fileInfo : files) {
        if (fileInfo.size() >= 8 && !fileInfo.suffix().isEmpty()) {
            return true;
        }
    }

    return false;
}

QString findDatasetsPathFrom(const QString &startPath)
{
    QDir directory(startPath);
    for (int depth = 0; depth < 6; ++depth) {
        const QString candidate = QDir::cleanPath(directory.filePath(QStringLiteral("datasets")));
        if (hasDatasetFiles(candidate)) {
            return candidate;
        }

        if (!directory.cdUp()) {
            break;
        }
    }

    return QString();
}
}

QString AppPaths::basePath()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath());
}

QString AppPaths::datasetsPath()
{
    const QString environmentPath = qEnvironmentVariable("ATLAS_DATASETS_PATH");
    if (!environmentPath.trimmed().isEmpty()) {
        return QDir::cleanPath(environmentPath);
    }

    const QString applicationDatasetsPath = makePath(QStringLiteral("datasets"));
    if (hasDatasetFiles(applicationDatasetsPath)) {
        return applicationDatasetsPath;
    }

    const QString datasetsFromApplicationPath = findDatasetsPathFrom(QCoreApplication::applicationDirPath());
    if (!datasetsFromApplicationPath.isEmpty()) {
        return datasetsFromApplicationPath;
    }

    const QString datasetsFromCurrentPath = findDatasetsPathFrom(QDir::currentPath());
    if (!datasetsFromCurrentPath.isEmpty()) {
        return datasetsFromCurrentPath;
    }

    return applicationDatasetsPath;
}

QString AppPaths::databasePath()
{
    return makePath(QStringLiteral("database"));
}

QString AppPaths::databaseFile()
{
    return QDir::cleanPath(QDir(databasePath()).filePath(QStringLiteral("atlas.db")));
}

bool AppPaths::ensureRequiredDirectories(QString *errorMessage)
{
    const QStringList requiredPaths = {
        datasetsPath(),
        databasePath()
    };

    for (const QString &path : requiredPaths) {
        if (!QDir().mkpath(path)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Não foi possível criar o diretório: %1").arg(path);
            }
            return false;
        }
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}
