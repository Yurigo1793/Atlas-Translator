#ifndef ATLASIMPORTER_H
#define ATLASIMPORTER_H

#include "LanguageNormalizer.h"
#include "TranslationOrganizer.h"

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QVariantList>

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
        qint64 qualityWarningsLogged = 0;
        qint64 errorCount = 0;
        qint64 discardedLines = 0;
        qint64 parsingTimeNs = 0;
        qint64 sqliteTimeNs = 0;
        qint64 indexTimeNs = 0;
        qint64 cleanupTimeNs = 0;
        qint64 logTimeNs = 0;
        qint64 reportTimeNs = 0;
        qint64 totalTimeNs = 0;
        qint64 progressPrintCount = 0;
        qint64 cleanupRunCount = 0;
        qint64 punctuationSplitPairs = 0;
        qint64 punctuationSplitSegments = 0;
        qint64 punctuationSplitSkipped = 0;
        qint64 slashAlternativesExpanded = 0;
        qint64 slashAlternativeCandidates = 0;
        QHash<QString, qint64> discardReasons;
        QHash<QString, QStringList> discardSamples;
        QHash<QString, qint64> punctuationSplitSkipReasons;
        QHash<QString, QStringList> punctuationSplitSamples;
        QStringList slashAlternativeSamples;
    };

    struct CleanupStats {
        qint64 removedInvalidEntries = 0;
        qint64 removedDuplicates = 0;
        qint64 finalTranslationsCount = 0;
    };

    struct RejectedExample {
        QString fileName;
        QString fullPath;
        qint64 position = 0;
        QString sourceLang;
        QString targetLang;
        QString sourceText;
        QString targetText;
    };

    explicit AtlasImporter(const QString &databasePath = QString());
    ~AtlasImporter();

    static QString databasePathForLanguagePair(const QString &sourceLang, const QString &targetLang);

    bool importMosesDataset(const QString &sourceFilePath,
                            const QString &targetFilePath,
                            const QString &sourceLang = QStringLiteral("en"),
                            const QString &targetLang = QStringLiteral("pt_BR"));

    bool importMosesDatasetBidirectional(const QString &sourceFilePath,
                                         const QString &targetFilePath,
                                         const QString &sourceLang,
                                         const QString &targetLang);

    bool preprocessMosesDataset(const QString &sourceFilePath,
                                const QString &targetFilePath,
                                const QString &sourceLang,
                                const QString &targetLang,
                                const QString &corpusName,
                                const QString &outputFilePath,
                                QTextStream *progress = nullptr);

    bool importMosesPreprocessedDataset(const QString &preprocessedFilePath,
                                        const QString &sourceLang,
                                        const QString &targetLang);

    bool importFreeDictTeiDataset(const QString &teiFilePath,
                                  const QString &sourceLang,
                                  const QString &targetLang);

    bool importMediaWikiDataset(const QString &mediaWikiFilePath,
                                const QString &sourceLang,
                                const QString &targetLang);

    bool importMediaWikiPreprocessedDataset(const QString &preprocessedFilePath,
                                            const QString &sourceLang,
                                            const QString &targetLang);

    bool preprocessMediaWikiDataset(const QString &mediaWikiFilePath,
                                    const QString &sourceLang,
                                    const QString &targetLang,
                                    const QString &outputFilePath,
                                    QTextStream *progress = nullptr);
    bool preprocessMediaWikiDatasetAllLanguages(const QString &mediaWikiFilePath,
                                                const QString &sourceLang,
                                                const QString &outputDirectory,
                                                QTextStream *progress = nullptr);

    ImportStats stats() const;
    CleanupStats cleanupStats() const;
    QString lastError() const;
    QString databasePath() const;
    void setDebugRejectedDetailsEnabled(bool enabled);
    bool debugRejectedDetailsEnabled() const;

private:
    bool openDatabase();
    bool ensureSchema();
    bool ensureIndexes();
    bool ensureLookupIndex();
    bool ensureFrequencySchema();
    bool ensureTranslationMetadataSchema();
    bool removeLegacyUniqueConstraint();
    bool ensureProgressTable();
    bool ensureQualityLogTable();
    bool ensureImportBufferTable();
    bool mergeBufferedPairs();
    bool prepareInsertStatement(QSqlQuery &query);
    bool prepareUpdateFrequencyStatement(QSqlQuery &query);
    bool insertPreparedPair(QSqlQuery &insertQuery,
                            QSqlQuery &updateQuery,
                            const QString &sourceText,
                            const QString &targetText,
                            const QString &sourceLang,
                            const QString &targetLang,
                            const QString &senseGloss = QString(),
                            bool allowSameText = false);
    QString discardReasonForPair(const QString &sourceText, const QString &targetText) const;
    void recordDiscard(const QString &reason,
                       const QString &sourceText,
                       const QString &targetText,
                       const QString &sourceLang,
                       const QString &targetLang);
    void writeRejectedDetail(const QString &reason,
                             const QString &sourceText,
                             const QString &targetText,
                             const QString &sourceLang,
                             const QString &targetLang);
    void flushRejectedDetails();
    void writeRejectedDetailsSummary(const QString &sourceFilePath,
                                     const QString &sourceLang,
                                     const QString &targetLang,
                                     const QString &startedAt);
    void maybeWriteRejectedDetailsSummary(const QString &sourceFilePath,
                                          const QString &sourceLang,
                                          const QString &targetLang,
                                          const QString &startedAt,
                                          bool force = false);
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
    void writeImportReport(const QString &sourceType,
                           const QString &sourceFilePath,
                           const QString &sourceLang,
                           const QString &targetLang,
                           const QString &startedAt,
                           bool success);
    void writePreprocessReport(const QString &sourceType,
                               const QString &sourceFilePath,
                               const QString &targetFilePath,
                               const QString &sourceLang,
                               const QString &targetLang,
                               const QString &outputPath,
                               const QString &startedAt,
                               bool success,
                               const QString &details);
    bool skipLines(QTextStream &sourceStream, QTextStream &targetStream, qint64 linesToSkip);
    QString importKey(const QString &sourceFilePath,
                      const QString &targetFilePath,
                      const QString &sourceLang,
                      const QString &targetLang) const;
    QString cleanText(const QString &text) const;
    QString normalizedSourceText(const QString &text) const;
    bool logImportWarning(const QString &sourceName,
                          const QString &category,
                          const QString &detail,
                          qint64 position);
    void printProgress();

    QString m_databasePath;
    QString m_currentImportSourceFile;
    QString m_connectionName;
    QSqlDatabase m_database;
    QString m_lastError;
    QString m_rejectedDetailsBuffer;
    ImportStats m_stats;
    CleanupStats m_cleanupStats;
    LanguageNormalizer m_languageNormalizer;
    TranslationOrganizer m_translationOrganizer;
    bool m_debugRejectedDetails = false;
    qint64 m_bufferedPairs = 0;
    QVariantList m_bufferSourceTexts;
    QVariantList m_bufferTranslatedTexts;
    QVariantList m_bufferSourceLangs;
    QVariantList m_bufferTargetLangs;
    QVariantList m_bufferSenseGlosses;
    QHash<QString, RejectedExample> m_rejectedExamples;
    qint64 m_lastRejectedSummarySnapshotProcessed = -1;
};

#endif // ATLASIMPORTER_H
