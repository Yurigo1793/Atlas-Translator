#ifndef ATLASIMPORTER_H
#define ATLASIMPORTER_H

#include "LanguageNormalizer.h"

#include <QSqlDatabase>
#include <QString>
#include <QtGlobal>

class QSqlQuery;
class QTextStream;

class AtlasImporter
{
public:
    struct ImportStats {
        qint64 processedLines = 0;
        qint64 insertedLines = 0;
        qint64 updatedFrequencyLines = 0;
        qint64 ignoredLines = 0;
        qint64 duplicateLines = 0;
        qint64 resumedLines = 0;
    };

    struct CleanupStats {
        qint64 removedInvalidEntries = 0;
        qint64 removedDuplicates = 0;
        qint64 finalTranslationsCount = 0;
    };

    explicit AtlasImporter(const QString &databasePath = QString());
    ~AtlasImporter();

    bool importMosesDataset(const QString &sourceFilePath,
                            const QString &targetFilePath,
                            const QString &sourceLang = QStringLiteral("en"),
                            const QString &targetLang = QStringLiteral("pt_BR"));

    ImportStats stats() const;
    CleanupStats cleanupStats() const;
    QString lastError() const;
    QString databasePath() const;

private:
    bool openDatabase();
    bool ensureSchema();
    bool ensureIndexes();
    bool ensureLookupIndex();
    bool ensureFrequencySchema();
    bool removeLegacyUniqueConstraint();
    bool ensureProgressTable();
    bool prepareInsertStatement(QSqlQuery &query);
    bool preparePairExistsStatement(QSqlQuery &query);
    bool translationPairExists(QSqlQuery &query,
                               const QString &sourceText,
                               const QString &targetText,
                               const QString &sourceLang,
                               const QString &targetLang);
    bool isValidTranslationPair(const QString &sourceText, const QString &targetText) const;
    bool isInvalidStoredTranslationPair(const QString &sourceText, const QString &targetText) const;
    bool cleanupDatabase();
    bool optimizeDatabase(bool compactDatabase);
    qint64 translationCount() const;
    bool loadProgress(const QString &importKey, qint64 &processedLines, bool &completed);
    bool saveProgress(const QString &importKey,
                      const QString &sourceFilePath,
                      const QString &targetFilePath,
                      const QString &sourceLang,
                      const QString &targetLang,
                      qint64 processedLines,
                      bool completed);
    bool skipLines(QTextStream &sourceStream, QTextStream &targetStream, qint64 linesToSkip);
    QString importKey(const QString &sourceFilePath,
                      const QString &targetFilePath,
                      const QString &sourceLang,
                      const QString &targetLang) const;
    QString cleanText(const QString &text) const;
    QString normalizedSourceText(const QString &text) const;
    void printProgress() const;

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_database;
    QString m_lastError;
    ImportStats m_stats;
    CleanupStats m_cleanupStats;
    LanguageNormalizer m_languageNormalizer;
};

#endif // ATLASIMPORTER_H
