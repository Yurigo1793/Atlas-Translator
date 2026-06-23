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

    const QFileInfoList directories = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
    for (const QFileInfo &directoryInfo : directories) {
        if (hasDatasetFiles(directoryInfo.absoluteFilePath())) {
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

    const QString datasetsFromCurrentPath = findDatasetsPathFrom(QDir::currentPath());
    if (!datasetsFromCurrentPath.isEmpty()) {
        return datasetsFromCurrentPath;
    }

    const QString datasetsFromApplicationPath = findDatasetsPathFrom(QCoreApplication::applicationDirPath());
    if (!datasetsFromApplicationPath.isEmpty()) {
        return datasetsFromApplicationPath;
    }

    const QString applicationDatasetsPath = makePath(QStringLiteral("datasets"));
    if (hasDatasetFiles(applicationDatasetsPath)) {
        return applicationDatasetsPath;
    }

    return applicationDatasetsPath;
}

QString AppPaths::databasePath()
{
    return makePath(QStringLiteral("database"));
}

QString AppPaths::preprocessProgressPath()
{
    return makePath(QStringLiteral("preprocess_progress"));
}

bool AppPaths::ensureRequiredDirectories(QString *errorMessage)
{
    const QStringList requiredPaths = {
        databasePath(),
        preprocessProgressPath()
    };

    for (const QString &path : requiredPaths) {
        if (!QDir().mkpath(path)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Nao foi possivel criar diretorio: %1").arg(path);
            }
            return false;
        }
    }

    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}
