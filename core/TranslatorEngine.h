#ifndef TRANSLATORENGINE_H
#define TRANSLATORENGINE_H

#include "DatabaseManager.h"
#include "LanguageNormalizer.h"
#include "QueryCache.h"
#include "TextNormalizer.h"
#include "Tokenizer.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QTextStream;

class TranslatorEngine
{
public:
    struct Statistics {
        qint64 cacheHits = 0;
        qint64 cacheMisses = 0;
        qint64 totalTranslations = 0;
        qint64 totalTranslationTimeNs = 0;
    };

    struct TranslationResult {
        QString input;
        QString translation;
        QString matchedText;
        QString matchType = QStringLiteral("none");
        qsizetype consumedWords = 0;
        qint64 translationTimeNs = 0;
    };

    explicit TranslatorEngine(const QString &databasePath = QString());

    bool initialize();
    void shutdown();
    QString translate(const QString &text,
                      const QString &sourceLang = QStringLiteral("en"),
                      const QString &targetLang = QStringLiteral("pt_BR")) const;
    TranslationResult translateDetailed(const QString &text,
                                        const QString &sourceLang = QStringLiteral("en"),
                                        const QString &targetLang = QStringLiteral("pt_BR")) const;
    QString lastError() const;
    Statistics statistics() const;
    DatabaseManager::SqlStatistics sqlStatistics() const;
    DatabaseManager::DatabaseSummary databaseSummary() const;
    QStringList availableLanguages() const;
    QList<DatabaseManager::LanguagePair> availableLanguagePairs() const;
    bool hasLanguagePair(const QString &sourceLang, const QString &targetLang) const;
    double averageTranslationTimeMs() const;
    void printStatistics(QTextStream &output) const;
    void printDatabaseSummary(QTextStream &output) const;
    void setDebugEnabled(bool enabled);
    bool debugEnabled() const;

private:
    struct PhraseMatch {
        bool found = false;
        QString matchedText;
        QString translation;
        QString matchType = QStringLiteral("none");
        qsizetype consumedWords = 0;
    };

    TranslationResult translateSegment(const QString &text,
                                       const QString &sourceLang,
                                       const QString &targetLang) const;
    PhraseMatch findBestMatch(const QStringList &words,
                              qsizetype position,
                              const QString &sourceLang,
                              const QString &targetLang) const;
    QString cacheKey(const QString &sourceText,
                     const QString &sourceLang,
                     const QString &targetLang) const;
    void cacheTranslation(const QString &key, const QString &translation) const;
    void recordTranslationTime(qint64 elapsedNs) const;
    void logMatch(const QString &input,
                  const PhraseMatch &match,
                  const QString &sourceLang,
                  const QString &targetLang) const;
    QString matchType(qsizetype position, qsizetype consumedWords, qsizetype totalWords, bool exactCandidate) const;

    DatabaseManager m_databaseManager;
    Tokenizer m_tokenizer;
    TextNormalizer m_normalizer;
    LanguageNormalizer m_languageNormalizer;
    mutable QueryCache m_translationCache;
    mutable Statistics m_statistics;
    bool m_debugEnabled = false;
    bool m_ready = false;
};

#endif // TRANSLATORENGINE_H
