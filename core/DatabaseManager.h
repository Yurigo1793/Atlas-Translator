#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QSqlDatabase>
#include <QString>
#include <optional>

class DatabaseManager
{
public:
    explicit DatabaseManager(const QString &databasePath = QString());
    ~DatabaseManager();

    bool open();
    bool initialize();
    std::optional<QString> findTranslation(const QString &sourceText,
                                           const QString &sourceLang,
                                           const QString &targetLang) const;

    QString lastError() const;
    QString databasePath() const;

private:
    bool createTables();
    bool seedInitialTranslations();
    bool insertTranslation(const QString &sourceText,
                           const QString &translatedText,
                           const QString &sourceLang,
                           const QString &targetLang);

    QString normalizedText(const QString &text) const;

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_database;
    QString m_lastError;
};

#endif // DATABASEMANAGER_H
