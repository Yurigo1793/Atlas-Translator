#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStringList>

namespace {
QString makePath(const QString &directoryName)
{
    return QDir::cleanPath(QDir(AppPaths::basePath()).filePath(directoryName));
}
}

QString AppPaths::basePath()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath());
}

QString AppPaths::datasetsPath()
{
    return makePath(QStringLiteral("datasets"));
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
