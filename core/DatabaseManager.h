#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include "LanguageNormalizer.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <optional>

class QTextStream;

class DatabaseManager
{
public:
    struct SqlStatistics {
        qint64 queryCount = 0;
        qint64 totalQueryTimeNs = 0;
    };

    struct DatabaseSummary {
        bool sqliteOpened = false;
        bool tableExists = false;
        bool indexesOk = false;
        qint64 translationCount = 0;
        qint64 totalFrequency = 0;
        QString databasePath;
        QStringList languagePairs;
        QStringList indexes;
    };

    struct LanguagePair {
        QString sourceLang;
        QString targetLang;
        qint64 translationCount = 0;
    };

    explicit DatabaseManager(const QString &databasePath = QString());
    ~DatabaseManager();

    bool open();
    void close();
    bool initialize();
    std::optional<QString> findTranslation(const QString &sourceText,
                                           const QString &sourceLang,
                                           const QString &targetLang) const;
    QHash<QString, QString> findTranslations(const QStringList &sourceTexts,
                                             const QString &sourceLang,
                                             const QString &targetLang) const;

    QString lastError() const;
    QString databasePath() const;
    SqlStatistics sqlStatistics() const;
    double averageSqlQueryTimeMs() const;
    DatabaseSummary databaseSummary() const;
    QStringList availableLanguages() const;
    QList<LanguagePair> availableLanguagePairs() const;
    bool hasLanguagePair(const QString &sourceLang, const QString &targetLang) const;
    void printDatabaseSummary(QTextStream &output) const;

private:
    bool createTables();
    bool createIndexes();
    bool ensureLookupIndex();
    bool ensureFrequencySchema();
    bool removeLegacyUniqueConstraint();
    bool normalizeStoredLanguages();
    bool tableExists(const QString &tableName) const;

    QString normalizedText(const QString &text) const;
    void recordSqlQueryTime(qint64 elapsedNs) const;

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_database;
    mutable QString m_lastError;
    LanguageNormalizer m_languageNormalizer;
    mutable SqlStatistics m_sqlStatistics;
};

#endif // DATABASEMANAGER_H
