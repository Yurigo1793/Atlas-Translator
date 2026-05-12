#ifndef TRANSLATORENGINE_H
#define TRANSLATORENGINE_H

#include "DatabaseManager.h"
#include "TextNormalizer.h"
#include "Tokenizer.h"

#include <QHash>
#include <QString>
#include <QStringList>

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

    explicit TranslatorEngine(const QString &databasePath = QString());

    bool initialize();
    QString translate(const QString &text,
                      const QString &sourceLang = QStringLiteral("en"),
                      const QString &targetLang = QStringLiteral("pt")) const;
    QString lastError() const;
    Statistics statistics() const;
    double averageTranslationTimeMs() const;
    void printStatistics(QTextStream &output) const;

private:
    QString translateNormalizedWords(const QStringList &words,
                                     const QString &sourceLang,
                                     const QString &targetLang) const;
    QString cacheKey(const QString &sourceText,
                     const QString &sourceLang,
                     const QString &targetLang) const;
    void cacheTranslation(const QString &key, const QString &translation) const;
    void recordTranslationTime(qint64 elapsedNs) const;

    DatabaseManager m_databaseManager;
    Tokenizer m_tokenizer;
    TextNormalizer m_normalizer;
    mutable QHash<QString, QString> m_translationCache;
    mutable Statistics m_statistics;
};

#endif // TRANSLATORENGINE_H
