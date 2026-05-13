#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>

class AppPaths
{
public:
    static QString basePath();
    static QString datasetsPath();
    static QString databasePath();
    static QString databaseFile();
    static bool ensureRequiredDirectories(QString *errorMessage = nullptr);
};

#endif // APPPATHS_H
