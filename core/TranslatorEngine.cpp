#include "TranslatorEngine.h"

#include <QElapsedTimer>
#include <QTextStream>
#include <QVector>

namespace {
constexpr qsizetype MaximumCachedEntries = 4096;
}

TranslatorEngine::TranslatorEngine(const QString &databasePath)
    : m_databaseManager(databasePath)
{
}

bool TranslatorEngine::initialize()
{
    return m_databaseManager.initialize();
}

QString TranslatorEngine::translate(const QString &text,
                                    const QString &sourceLang,
                                    const QString &targetLang) const
{
    QElapsedTimer timer;
    timer.start();

    const QStringList words = m_normalizer.words(text);
    if (words.isEmpty()) {
        recordTranslationTime(timer.nsecsElapsed());
        return QString();
    }

    const QString translatedText = translateNormalizedWords(words, sourceLang, targetLang);
    recordTranslationTime(timer.nsecsElapsed());
    return translatedText;
}

QString TranslatorEngine::translateNormalizedWords(const QStringList &words,
                                                   const QString &sourceLang,
                                                   const QString &targetLang) const
{
    struct CandidateGroup {
        qsizetype length = 0;
        QStringList candidates;
    };

    QStringList translatedParts;
    translatedParts.reserve(words.size());

    for (qsizetype position = 0; position < words.size();) {
        QVector<CandidateGroup> groups;
        QStringList missingCandidates;
        QHash<QString, QString> availableTranslations;

        for (qsizetype length = words.size() - position; length > 0; --length) {
            const QStringList candidates = m_normalizer.lookupCandidates(words.mid(position, length));
            if (candidates.isEmpty()) {
                continue;
            }

            groups.append({length, candidates});

            for (const QString &candidate : candidates) {
                const QString key = cacheKey(candidate, sourceLang, targetLang);
                if (m_translationCache.contains(key)) {
                    ++m_statistics.cacheHits;
                    availableTranslations.insert(candidate, m_translationCache.value(key));
                    continue;
                }

                ++m_statistics.cacheMisses;
                if (!missingCandidates.contains(candidate)) {
                    missingCandidates.append(candidate);
                }
            }
        }

        const QHash<QString, QString> databaseMatches = m_databaseManager.findTranslations(missingCandidates,
                                                                                            sourceLang,
                                                                                            targetLang);
        for (auto iterator = databaseMatches.constBegin(); iterator != databaseMatches.constEnd(); ++iterator) {
            availableTranslations.insert(iterator.key(), iterator.value());
            cacheTranslation(cacheKey(iterator.key(), sourceLang, targetLang), iterator.value());
        }

        bool matched = false;
        for (const CandidateGroup &group : groups) {
            for (const QString &candidate : group.candidates) {
                if (availableTranslations.contains(candidate)) {
                    translatedParts.append(availableTranslations.value(candidate));
                    position += group.length;
                    matched = true;
                    break;
                }
            }

            if (matched) {
                break;
            }
        }

        if (!matched) {
            translatedParts.append(words.at(position));
            ++position;
        }
    }

    return translatedParts.join(QStringLiteral(" "));
}

QString TranslatorEngine::lastError() const
{
    return m_databaseManager.lastError();
}

TranslatorEngine::Statistics TranslatorEngine::statistics() const
{
    return m_statistics;
}

double TranslatorEngine::averageTranslationTimeMs() const
{
    if (m_statistics.totalTranslations == 0) {
        return 0.0;
    }

    return static_cast<double>(m_statistics.totalTranslationTimeNs)
        / static_cast<double>(m_statistics.totalTranslations)
        / 1000000.0;
}

void TranslatorEngine::printStatistics(QTextStream &output) const
{
    output << Qt::endl;
    output << "Estatísticas do Atlas-Translator:" << Qt::endl;
    output << "Total de traduções: " << m_statistics.totalTranslations << Qt::endl;
    output << "Cache hits: " << m_statistics.cacheHits << Qt::endl;
    output << "Cache misses: " << m_statistics.cacheMisses << Qt::endl;
    output << "Tempo médio de tradução: " << QString::number(averageTranslationTimeMs(), 'f', 3) << " ms" << Qt::endl;
}

QString TranslatorEngine::cacheKey(const QString &sourceText,
                                   const QString &sourceLang,
                                   const QString &targetLang) const
{
    return QStringLiteral("%1|%2|%3").arg(sourceLang.toLower(), targetLang.toLower(), m_normalizer.normalizeForLookup(sourceText));
}

void TranslatorEngine::cacheTranslation(const QString &key, const QString &translation) const
{
    if (m_translationCache.size() >= MaximumCachedEntries) {
        m_translationCache.clear();
    }

    m_translationCache.insert(key, translation);
}

void TranslatorEngine::recordTranslationTime(qint64 elapsedNs) const
{
    ++m_statistics.totalTranslations;
    m_statistics.totalTranslationTimeNs += elapsedNs;
}
